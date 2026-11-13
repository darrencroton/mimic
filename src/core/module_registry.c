/**
 * @file    module_registry.c
 * @brief   Implementation of module registration and execution pipeline
 *
 * This file implements the module registry system that allows galaxy physics
 * modules to register themselves and be executed in a coordinated pipeline.
 *
 * The registry maintains two arrays:
 * - registered_modules[]: All modules available (compile-time)
 * - execution_pipeline[]: Enabled modules in configured order (runtime)
 *
 * Physics-Agnostic Core (VISION.md §1): The registry treats all modules
 * identically through the Module interface, with zero knowledge of specific
 * physics implementations.
 *
 * Runtime Modularity (VISION.md §2): Module selection is configurable at
 * runtime without recompilation.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "fof_workspace.h"
#include "galaxy_id.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "inheritance.h"
#include "memory.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "rate_assembly.h"
#include "task_layout.h"
#include "generated/parameter_unit_conversions.h"

/** Array of all registered modules (available for use) */
static struct Module *registered_modules[MAX_MODULES];

/** Number of currently registered modules */
static int num_registered_modules = 0;

/** Array of configured modules in execution order (runtime configuration) */
static struct Module *execution_pipeline[MAX_MODULES];

/** Number of modules in execution pipeline */
static int num_pipeline_modules = 0;

/**
 * @brief Phase-local event dispatch state
 *
 * Active only during execute_phase(). Enables module_emit_event() to append
 * events and dispatch them to subscribed PROCESSING_MODE_PER_EVENT consumers.
 *
 * `events` is a run-persistent grow-to-high-water buffer, not a per-phase
 * allocation: `event_count` resets at the top of every execute_phase() call
 * while `event_capacity` only ever grows, so the dispatch hot path never
 * allocates once the busiest FoF group has been seen. Ownership is the module
 * system's; module_system_cleanup() releases it (VISION.md Principle 5).
 */
struct PhaseEventDispatchState {
  struct ModuleEvent *events; /**< Heap buffer of `event_capacity` slots (may be NULL) */
  int event_capacity;         /**< Slots allocated in `events`; high-water, never shrinks */
  int event_count;
  int last_dispatched_event;
  struct Module *const *per_event; /**< The phase plan's per-event list (NULL when inactive) */
  int num_per_event;
  struct ModuleContext *ctx;
  struct Halo *halos;
  int ngal;
  bool active;
  bool emission_allowed;
  int current_producer_module_id; /**< module_id of the currently executing producer */
};

/**
 * @brief   Whether execute_post_snapshot() is currently running a callback
 *
 * Checked by module_emit_event() before its no-active-phase shortcut, so a
 * snapshot callback that tries to emit a FoF event is rejected rather than
 * silently accepted as if it were a direct module unit test. Snapshot dispatch
 * and FoF phase dispatch are never active together.
 */
static bool snapshot_dispatch_active = false;

static struct PhaseEventDispatchState phase_event_state = {.events = NULL,
                                                           .event_capacity = 0,
                                                           .event_count = 0,
                                                           .last_dispatched_event = 0,
                                                           .per_event = NULL,
                                                           .num_per_event = 0,
                                                           .ctx = NULL,
                                                           .halos = NULL,
                                                           .ngal = 0,
                                                           .active = false,
                                                           .emission_allowed = false,
                                                           .current_producer_module_id = 0};

/**
 * @brief   The rate phase: its startup plan and its segment scratch
 *
 * A rate phase is a phase under modules.phases whose entries are all
 * process_rates; a run has at most one. module_system_init() builds its plan
 * (rate_assembly.h) before any module's init() and keys it by the phase's
 * configuration array; execute_phase() runs it. The scratch is one
 * run-persistent, grow-to-high-water MEM_UTILITY allocation holding every
 * table of a segment and the integrator's workspace; module_system_cleanup()
 * releases it with the plan, following the phase event buffer.
 */
static struct RateAssemblyPlan *rate_plan = NULL;
static const struct PhaseModuleConfig *rate_phase_config = NULL;
static struct RateAssemblyScratch rate_scratch = {NULL, 0};

/**
 * @brief   The rate phase's step statistics, and the discrete-phase ledger's unit count
 *
 * The statistics are zeroed when module_system_init() builds the rate plan; the
 * rate segments and the discrete-phase ledger (module_registry.h) add to them,
 * and module_rate_statistics() exposes them only while a rate plan exists. The
 * unit count is zeroed by every module_system_init() and counts the dispatch
 * units the ledger checks, so it stays zero in a run with no rate phase.
 */
static struct RateStatistics rate_statistics;
static int64_t discrete_ledger_units = 0;

/**
 * @brief   The module callback the registry is running, if any
 *
 * Read by module_create_record() to name the caller in a refusal, and by the
 * snapshot collectives through module_registry_running_callback(); set and
 * restored by enter_callback()/leave_callback() around every module call. The
 * kinds (enum RunningCallbackKind) are declared in module_registry.h.
 */
struct RunningCallback {
  enum RunningCallbackKind kind;
  const char *module_name; /**< Registered module name; NULL outside any callback */
};

static struct RunningCallback running_callback = {RUNNING_CALLBACK_NONE, NULL};

/** @brief Mark a module callback as running; returns the state to restore afterwards */
static struct RunningCallback enter_callback(enum RunningCallbackKind kind,
                                             const char *module_name) {
  const struct RunningCallback prior = running_callback;
  running_callback.kind = kind;
  running_callback.module_name = module_name;
  return prior;
}

/** @brief Restore the running-callback state enter_callback() returned */
static void leave_callback(struct RunningCallback prior) { running_callback = prior; }

enum RunningCallbackKind module_registry_running_callback(const char **module_name) {
  if (module_name != NULL) {
    *module_name = running_callback.module_name;
  }
  return running_callback.kind;
}

/** Rows in the first staging block; block b holds RECORD_STAGING_BLOCK_ROWS << b rows */
#define RECORD_STAGING_BLOCK_ROWS 256

/**
 * Staging blocks a run can hold. The blocks double, so block b ends at row
 * RECORD_STAGING_BLOCK_ROWS * (2^(b+1) - 1): 22 blocks already exceed
 * MAX_HALO_ARRAY_SIZE staged rows, which reserve_staging_slot() refuses first.
 */
#define RECORD_STAGING_MAX_BLOCKS 32

/**
 * @brief   Record-creation state (module_create_record())
 *
 * Creation is legal exactly while the running callback is a process_full_halo
 * one (running_callback.kind == RUNNING_CALLBACK_FULL_HALO): a per-event
 * consumer its events reach runs as RUNNING_CALLBACK_PER_EVENT, so it cannot
 * create. `ctx` and `ws` are the context and workspace execute_phase() handed
 * that callback. Records a callback creates are staged here and appended to the
 * workspace by commit_created_records() when the callback returns.
 *
 * Staged rows live in blocks that are never reallocated, so every row pointer
 * module_create_record() hands out stays valid until its callback returns,
 * however many records it creates after it. Block b holds
 * RECORD_STAGING_BLOCK_ROWS << b rows, so the number of blocks (each one
 * tracked allocation) grows only logarithmically with the staged-row
 * high-water. `staged_host` holds each staged row's host index. `ordinals` holds
 * each host row's created-record count for the current FoF step, indexed by
 * workspace row in [0, base_count); it is cleared lazily by the step's first
 * creation, so a FoF step that creates nothing never touches it.
 *
 * All of it is run-persistent, grow-to-high-water scratch in MEM_HALOS that a
 * run allocates only once a module creates a record, released by
 * module_release_record_creation_scratch() at driver teardown.
 */
struct RecordCreationState {
  struct ModuleContext *ctx;
  struct FoFWorkspace *ws;

  struct Halo *blocks[RECORD_STAGING_MAX_BLOCKS]; /**< The first block_count are allocated */
  int64_t block_count;
  int64_t *staged_host; /**< [staged_host_capacity] */
  int64_t staged_host_capacity;
  int64_t staged_count; /**< Rows staged by the running callback */

  int *ordinals; /**< [ordinal_capacity] */
  int64_t ordinal_capacity;
  bool ordinals_ready; /**< ordinals[0, base_count) were cleared for this FoF step */
};

static struct RecordCreationState record_creation = {.ctx = NULL,
                                                     .ws = NULL,
                                                     .blocks = {NULL},
                                                     .block_count = 0,
                                                     .staged_host = NULL,
                                                     .staged_host_capacity = 0,
                                                     .staged_count = 0,
                                                     .ordinals = NULL,
                                                     .ordinal_capacity = 0,
                                                     .ordinals_ready = false};

/* The processing-mode table and its lookups live in processing_modes.c. */

/**
 * @brief   Find a registered module by name
 *
 * @param   name    Module name to search for
 * @return  Pointer to module if found, NULL otherwise
 */
static struct Module *find_module_by_name(const char *name) {
  for (int i = 0; i < num_registered_modules; i++) {
    if (strcmp(registered_modules[i]->name, name) == 0) {
      return registered_modules[i];
    }
  }
  return NULL;
}

/**
 * @brief   Find a registered module by its generated module_id
 *
 * @param   module_id   Generated producer ID (from MODULE_ID_* macros)
 * @return  Pointer to module if found, NULL otherwise
 */
static struct Module *find_module_by_id(int module_id) {
  if (module_id <= 0) {
    return NULL;
  }
  for (int i = 0; i < num_registered_modules; i++) {
    if (registered_modules[i]->module_id == module_id) {
      return registered_modules[i];
    }
  }
  return NULL;
}

void for_each_phase(PhaseVisitor visit, void *userdata) {
  visit("pre_timestep", MimicConfig.pre_timestep, MimicConfig.num_pre_timestep, userdata);
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    visit(phase->name, phase->modules, phase->num_modules, userdata);
  }
  visit("post_timestep", MimicConfig.post_timestep, MimicConfig.num_post_timestep, userdata);
  /* Only when configured, so a run without snapshot modules visits exactly the
   * phases it did before post_snapshot existed. */
  if (MimicConfig.num_post_snapshot > 0) {
    visit(POST_SNAPSHOT_PHASE_NAME, MimicConfig.post_snapshot, MimicConfig.num_post_snapshot,
          userdata);
  }
}

/**
 * @brief   Require the typed callback of every callback family a module advertises
 *
 * Fails closed: an empty mode list, a mode outside the descriptor table, or an
 * advertised family without its callback is fatal. A family the module does
 * not advertise may leave its callback NULL.
 *
 * @param   module  Module being registered (name already checked non-NULL)
 */
static void check_module_callback_families(const struct Module *module) {
  if (module->supported_processing_modes == NULL || module->num_supported_modes <= 0) {
    FATAL_ERROR("Module '%s' advertises no processing modes", module->name);
  }

  for (int i = 0; i < module->num_supported_modes; i++) {
    const enum ProcessingMode mode = module->supported_processing_modes[i];
    enum ModuleCallbackFamily family;
    if (processing_mode_family(mode, &family) != 0) {
      FATAL_ERROR("Module '%s' advertises unknown processing mode value %d", module->name,
                  (int)mode);
    }

    switch (family) {
    case MODULE_CALLBACK_FAMILY_FOF:
      if (module->process == NULL) {
        FATAL_ERROR("Module '%s' advertises FoF mode '%s' but has a NULL process callback",
                    module->name, processing_mode_to_string(mode));
      }
      break;
    case MODULE_CALLBACK_FAMILY_SNAPSHOT:
      if (module->process_snapshot == NULL) {
        FATAL_ERROR("Module '%s' advertises snapshot mode '%s' but has a NULL "
                    "process_snapshot callback",
                    module->name, processing_mode_to_string(mode));
      }
      break;
    case MODULE_CALLBACK_FAMILY_RATE:
      if (module->rates == NULL) {
        FATAL_ERROR("Module '%s' advertises rate mode '%s' but has a NULL rates callback",
                    module->name, processing_mode_to_string(mode));
      }
      break;
    default:
      FATAL_ERROR("Module '%s' mode '%s' maps to unhandled callback family %d", module->name,
                  processing_mode_to_string(mode), (int)family);
    }
  }
}

/**
 * @brief   Register a galaxy physics module
 *
 * Adds the module to the registry of available modules. Modules must be
 * registered before module_system_init() is called.
 *
 * @param   module  Pointer to module struct (must remain valid for program
 * lifetime)
 */
void module_registry_add(struct Module *module) {
  if (module == NULL) {
    FATAL_ERROR("Attempted to register NULL module");
  }

  if (num_registered_modules >= MAX_MODULES) {
    FATAL_ERROR("Maximum number of modules (%d) exceeded", MAX_MODULES);
  }

  if (module->name == NULL) {
    FATAL_ERROR("Module has NULL name");
  }

  if (module->init == NULL || module->cleanup == NULL) {
    FATAL_ERROR("Module '%s' has a NULL init or cleanup function", module->name);
  }

  check_module_callback_families(module);

  if (find_module_by_name(module->name) != NULL) {
    FATAL_ERROR("Module '%s' is already registered", module->name);
  }

  registered_modules[num_registered_modules] = module;
  num_registered_modules++;

  DEBUG_LOG("Registered module: %s", module->name);
}

/**
 * @brief   Resolve a configured module and add it to the pipeline if new
 *
 * @param   phase_name   Phase the entry is configured in (for diagnostics)
 * @param   module_name  Name of module to add
 * @return  The registered module (never NULL; fatal if unregistered)
 */
static struct Module *add_module_to_pipeline(const char *phase_name, const char *module_name) {
  /* Check if already in pipeline */
  for (int i = 0; i < num_pipeline_modules; i++) {
    if (strcmp(execution_pipeline[i]->name, module_name) == 0) {
      return execution_pipeline[i]; /* Already added */
    }
  }

  /* Find module in registry */
  struct Module *mod = find_module_by_name(module_name);
  if (mod == NULL) {
    ERROR_LOG("Module '%s' configured in phase '%s' but not registered", module_name, phase_name);
    ERROR_LOG("Available modules:");
    for (int j = 0; j < num_registered_modules; j++) {
      ERROR_LOG("  - %s", registered_modules[j]->name);
    }
    FATAL_ERROR("Unknown module '%s' in phase '%s' of the pipeline configuration", module_name,
                phase_name);
  }

  /* Add to execution pipeline */
  if (num_pipeline_modules >= MAX_MODULES) {
    FATAL_ERROR("Too many unique modules in pipeline (max %d)", MAX_MODULES);
  }
  execution_pipeline[num_pipeline_modules++] = mod;
  DEBUG_LOG("Added module to pipeline: %s", module_name);
  return mod;
}

/**
 * @brief   Check if module supports the configured processing mode
 *
 * @param   mod             Module to check
 * @param   configured_mode Processing mode from input YAML
 * @return  true if supported, false otherwise
 */
static bool module_supports_processing_mode(const struct Module *mod,
                                            enum ProcessingMode configured_mode) {
  for (int i = 0; i < mod->num_supported_modes; i++) {
    if (mod->supported_processing_modes[i] == configured_mode) {
      return true;
    }
  }
  return false;
}

/**
 * @brief   Format supported modes for error messages
 *
 * @param   mod  Module whose supported modes to format
 * @return  Static string with mode names (e.g., "process_full_halo, process_per_event,
 * process_by_galaxy")
 */
static const char *format_supported_modes(const struct Module *mod) {
  static char buffer[128];
  size_t used = 0;

  buffer[0] = '\0';
  for (int i = 0; i < mod->num_supported_modes && used < sizeof(buffer); i++) {
    int written = snprintf(buffer + used, sizeof(buffer) - used, "%s%s", (i > 0) ? ", " : "",
                           processing_mode_to_string(mod->supported_processing_modes[i]));
    if (written < 0) {
      break;
    }
    used += (size_t)written;
  }
  return buffer;
}

/* ==============================================================================
 * DEPENDENCY ENFORCEMENT API (public — see module_registry.h)
 * ============================================================================== */

bool module_configured_in_phase(const char *name, const struct PhaseModuleConfig *phase,
                                int num_modules, enum ProcessingMode mode) {
  if (name == NULL || phase == NULL || num_modules <= 0) {
    return false;
  }
  for (int i = 0; i < num_modules; i++) {
    if (phase[i].module_name != NULL && phase[i].processing_mode == mode &&
        strcmp(phase[i].module_name, name) == 0) {
      return true;
    }
  }
  return false;
}

int module_count_phase_entries(const char *module_name, const struct PhaseModuleConfig *phase,
                               int num_modules, enum ProcessingMode mode, int *matching_mode_out) {
  int entries = 0;
  int matching_mode = 0;
  if (module_name != NULL && phase != NULL) {
    for (int i = 0; i < num_modules; i++) {
      if (phase[i].module_name != NULL && strcmp(phase[i].module_name, module_name) == 0) {
        entries++;
        matching_mode += (phase[i].processing_mode == mode);
      }
    }
  }
  if (matching_mode_out != NULL) {
    *matching_mode_out = matching_mode;
  }
  return entries;
}

bool mimic_is_output_snapshot(int snapshot) {
  for (int n = 0; n < MimicConfig.NOUT; n++) {
    if (MimicConfig.ListOutputSnaps[n] == snapshot) {
      return true;
    }
  }
  return false;
}

/**
 * @brief   Does a phase array contain a module by name (any mode)?
 */
static bool phase_contains_module(const struct PhaseModuleConfig *phase, int num_modules,
                                  const char *name) {
  if (phase == NULL) {
    return false;
  }
  for (int i = 0; i < num_modules; i++) {
    if (phase[i].module_name != NULL && strcmp(phase[i].module_name, name) == 0) {
      return true;
    }
  }
  return false;
}

bool module_configured_anywhere(const char *name) {
  if (name == NULL) {
    return false;
  }
  if (phase_contains_module(MimicConfig.pre_timestep, MimicConfig.num_pre_timestep, name)) {
    return true;
  }
  if (phase_contains_module(MimicConfig.post_timestep, MimicConfig.num_post_timestep, name)) {
    return true;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    if (phase_contains_module(MimicConfig.substep_phases[p].modules,
                              MimicConfig.substep_phases[p].num_modules, name)) {
      return true;
    }
  }
  if (phase_contains_module(MimicConfig.post_snapshot, MimicConfig.num_post_snapshot, name)) {
    return true;
  }
  return false;
}

bool module_in_substep_phase(const char *name, enum ProcessingMode mode) {
  if (name == NULL) {
    return false;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_configured_in_phase(name, phase->modules, phase->num_modules, mode)) {
      return true;
    }
  }
  return false;
}

bool modules_in_same_substep_phase(const char *a, enum ProcessingMode mode_a, const char *b,
                                   enum ProcessingMode mode_b) {
  if (a == NULL || b == NULL) {
    return false;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_configured_in_phase(a, phase->modules, phase->num_modules, mode_a) &&
        module_configured_in_phase(b, phase->modules, phase->num_modules, mode_b)) {
      return true;
    }
  }
  return false;
}

static bool module_mode_precedes_in_phase(const char *first, enum ProcessingMode first_mode,
                                          const char *second, enum ProcessingMode second_mode,
                                          const struct PhaseModuleConfig *phase, int num_modules) {
  if (first == NULL || second == NULL || phase == NULL || num_modules <= 0) {
    return false;
  }
  int first_idx = -1;
  int second_idx = -1;
  for (int i = 0; i < num_modules; i++) {
    if (phase[i].module_name == NULL) {
      continue;
    }
    if (first_idx < 0 && phase[i].processing_mode == first_mode &&
        strcmp(phase[i].module_name, first) == 0) {
      first_idx = i;
    }
    if (second_idx < 0 && phase[i].processing_mode == second_mode &&
        strcmp(phase[i].module_name, second) == 0) {
      second_idx = i;
    }
  }
  if (first_idx < 0 || second_idx < 0) {
    return false;
  }
  return first_idx < second_idx;
}

bool module_precedes_in_substep_phase(const char *first, enum ProcessingMode first_mode,
                                      const char *second, enum ProcessingMode second_mode) {
  if (first == NULL || second == NULL) {
    return false;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_mode_precedes_in_phase(first, first_mode, second, second_mode, phase->modules,
                                      phase->num_modules)) {
      return true;
    }
  }
  return false;
}

bool module_precedes_in_phase(const char *first, const char *second,
                              const struct PhaseModuleConfig *phase, int num_modules) {
  if (first == NULL || second == NULL || phase == NULL || num_modules <= 0) {
    return false;
  }
  int first_idx = -1;
  int second_idx = -1;
  for (int i = 0; i < num_modules; i++) {
    if (phase[i].module_name == NULL) {
      continue;
    }
    if (first_idx < 0 && strcmp(phase[i].module_name, first) == 0) {
      first_idx = i;
    }
    if (second_idx < 0 && strcmp(phase[i].module_name, second) == 0) {
      second_idx = i;
    }
  }
  if (first_idx < 0 || second_idx < 0) {
    return false;
  }
  return first_idx < second_idx;
}

/** @brief Whether a phase is one of the fixed lifecycle phases pre_timestep and post_timestep */
static bool phase_is_lifecycle(const char *phase_name) {
  return strcmp(phase_name, "pre_timestep") == 0 || strcmp(phase_name, "post_timestep") == 0;
}

/**
 * @brief   Validate a phase that holds a process_rates entry
 *
 * Such a phase is the rate phase: a named substep phase under modules.phases
 * whose entries are all process_rates, each naming a different module that
 * supports the mode. process_rates in pre_timestep or post_timestep, a phase
 * mixing it with another mode, and a module listed twice (its law would be
 * counted twice) are refused, naming the phase and the module. post_snapshot
 * refuses every mode but process_snapshot itself (validate_post_snapshot_entries()).
 *
 * @return  0 on success, -1 after an ERROR_LOG
 */
static int validate_rate_phase_entries(const struct PhaseModuleConfig *config, int num_modules,
                                       const char *phase_name) {
  int first_rate = 0;
  while (config[first_rate].processing_mode != PROCESSING_MODE_RATES) {
    first_rate++;
  }
  if (phase_is_lifecycle(phase_name)) {
    ERROR_LOG("Configuration error in phase '%s':", phase_name);
    ERROR_LOG("  Module '%s' is configured with processing mode 'process_rates', which only a "
              "rate phase under modules.phases can run (a phase whose entries are all "
              "process_rates)",
              config[first_rate].module_name);
    return -1;
  }

  for (int i = 0; i < num_modules; i++) {
    if (config[i].processing_mode != PROCESSING_MODE_RATES) {
      ERROR_LOG("Configuration error in phase '%s':", phase_name);
      ERROR_LOG("  The phase mixes process_rates (module '%s') with %s (module '%s'); a rate "
                "phase holds only process_rates entries",
                config[first_rate].module_name,
                processing_mode_to_string(config[i].processing_mode), config[i].module_name);
      ERROR_LOG("  Fix: move the other modules to a phase of their own");
      return -1;
    }
    for (int j = 0; j < i; j++) {
      if (strcmp(config[j].module_name, config[i].module_name) == 0) {
        ERROR_LOG("Configuration error in phase '%s':", phase_name);
        ERROR_LOG("  Module '%s' is listed more than once; a rate module's transfers enter the "
                  "phase's one system once",
                  config[i].module_name);
        return -1;
      }
    }

    const struct Module *mod = find_module_by_name(config[i].module_name);
    if (mod != NULL && !module_supports_processing_mode(mod, PROCESSING_MODE_RATES)) {
      ERROR_LOG("Configuration error in phase '%s':", phase_name);
      ERROR_LOG("  Module '%s' does not support processing mode 'process_rates'", mod->name);
      ERROR_LOG("  Supported modes: %s", format_supported_modes(mod));
      return -1;
    }
  }
  return 0;
}

/**
 * @brief   Validate phase configuration against module constraints
 *
 * Ensures that each module in the phase is configured with a FoF-family
 * processing mode it actually supports, or, for a phase holding a
 * process_rates entry, that the phase is a valid rate phase
 * (validate_rate_phase_entries()). Fails hard with clear error messages if a
 * mismatch is detected.
 *
 * @param   config       Phase module configuration array
 * @param   num_modules  Number of modules in phase
 * @param   phase_name   Phase name for error messages
 * @return  0 on success, -1 on validation failure
 */
static int validate_phase_processing_modes(struct PhaseModuleConfig *config, int num_modules,
                                           const char *phase_name) {
  for (int i = 0; i < num_modules; i++) {
    if (config[i].processing_mode == PROCESSING_MODE_RATES) {
      return validate_rate_phase_entries(config, num_modules, phase_name);
    }
  }

  for (int i = 0; i < num_modules; i++) {
    /* Find registered module */
    struct Module *mod = find_module_by_name(config[i].module_name);
    if (mod == NULL) {
      /* Module not found - handled elsewhere in add_module_to_pipeline */
      continue;
    }

    /* A phase without a process_rates entry is dispatched by execute_phase()'s FoF
     * passes, which call only FoF callbacks: any other family would be skipped silently.
     * A mode outside the table leaves the family at this FoF default and is reported as
     * not a FoF mode. */
    enum ModuleCallbackFamily family = MODULE_CALLBACK_FAMILY_FOF;
    if (processing_mode_family(config[i].processing_mode, &family) != 0 ||
        family != MODULE_CALLBACK_FAMILY_FOF) {
      ERROR_LOG("Configuration error in phase '%s':", phase_name);
      ERROR_LOG("  Module '%s' is configured with processing mode '%s', which is not a "
                "FoF mode (process_full_halo, process_per_event, process_by_galaxy)",
                mod->name, processing_mode_to_string(config[i].processing_mode));
      return -1;
    }

    /* Check if configured mode is supported */
    if (!module_supports_processing_mode(mod, config[i].processing_mode)) {
      const char *mode_str = processing_mode_to_string(config[i].processing_mode);

      ERROR_LOG("Configuration error in phase '%s':", phase_name);
      ERROR_LOG("  Module '%s' does not support processing mode '%s'", mod->name, mode_str);
      ERROR_LOG("  Supported modes: %s", format_supported_modes(mod));
      ERROR_LOG("  Fix: Change processing mode in input YAML to one of the "
                "supported modes");
      return -1;
    }
  }
  return 0;
}

/**
 * @brief   Validate the post_snapshot phase against module constraints
 *
 * Every entry must use process_snapshot, name a module that advertises it
 * (registration guarantees its typed process_snapshot() callback), and appear
 * only once: the phase runs each entry once per snapshot, so a repeat would
 * silently run a module twice. When NTask > 1 every entry must also declare
 * snapshot_distribution: collective; a serial_only module assumes the whole
 * population is resident and is refused, naming the module and NTask.
 * Unregistered modules are reported by add_module_to_pipeline().
 *
 * @param   config       post_snapshot configuration array
 * @param   num_modules  Number of entries
 * @return  0 on success, -1 on validation failure
 */
static int validate_post_snapshot_entries(const struct PhaseModuleConfig *config, int num_modules) {
  for (int i = 0; i < num_modules; i++) {
    /* The single home of the duplicate check: it covers run files and C-built configurations. */
    for (int j = 0; j < i; j++) {
      if (strcmp(config[j].module_name, config[i].module_name) == 0) {
        ERROR_LOG("Configuration error in phase '%s':", POST_SNAPSHOT_PHASE_NAME);
        ERROR_LOG("  Module '%s' is listed more than once; each snapshot module runs once per "
                  "snapshot",
                  config[i].module_name);
        return -1;
      }
    }

    const struct Module *mod = find_module_by_name(config[i].module_name);
    if (mod == NULL) {
      continue; /* Missing module handled by add_module_to_pipeline */
    }

    enum ModuleCallbackFamily family;
    if (processing_mode_family(config[i].processing_mode, &family) != 0 ||
        family != MODULE_CALLBACK_FAMILY_SNAPSHOT) {
      ERROR_LOG("Configuration error in phase '%s':", POST_SNAPSHOT_PHASE_NAME);
      ERROR_LOG("  Module '%s' is configured with processing mode '%s'; only process_snapshot "
                "is allowed in this phase",
                mod->name, processing_mode_to_string(config[i].processing_mode));
      return -1;
    }

    if (!module_supports_processing_mode(mod, config[i].processing_mode)) {
      ERROR_LOG("Configuration error in phase '%s':", POST_SNAPSHOT_PHASE_NAME);
      ERROR_LOG("  Module '%s' does not support processing mode '%s'", mod->name,
                processing_mode_to_string(config[i].processing_mode));
      ERROR_LOG("  Supported modes: %s", format_supported_modes(mod));
      return -1;
    }
    /* No process_snapshot != NULL check: registration requires the callback of
     * every advertised family, and execute_post_snapshot() still FATALs on NULL. */

    /* Across tasks each process_snapshot call sees only its task's part of the
     * population, so only a module that reaches the whole population through the
     * snapshot collectives may run. Fails closed on any value but collective. */
    if (effective_task_count() > 1 &&
        mod->snapshot_distribution != SNAPSHOT_DISTRIBUTION_COLLECTIVE) {
      ERROR_LOG("Configuration error in phase '%s':", POST_SNAPSHOT_PHASE_NAME);
      ERROR_LOG("  Module '%s' is snapshot_distribution: serial_only (it needs the whole "
                "snapshot population on one task), but this run has NTask = %d",
                mod->name, effective_task_count());
      ERROR_LOG("  Fix: run with a single task, or port the module to the snapshot collectives "
                "and declare 'snapshot_distribution: collective' in its module_info.yaml");
      return -1;
    }
  }
  return 0;
}

/**
 * @brief   Validate event subscription contracts for a phase
 *
 * Ensures every process_per_event module in the phase:
 * 1. Declares at least one subscription (process_per_event with no subscriptions
 *    is a configuration error that would silently receive no events).
 * 2. Each referenced producer is present in the same phase as process_full_halo.
 *
 * @param   config       Phase module configuration array
 * @param   num_modules  Number of modules in phase
 * @param   phase_name   Phase name for error messages
 * @return  0 on success, -1 on validation failure
 */
static int validate_event_subscriptions(struct PhaseModuleConfig *config, int num_modules,
                                        const char *phase_name) {
  for (int i = 0; i < num_modules; i++) {
    if (config[i].processing_mode != PROCESSING_MODE_PER_EVENT) {
      continue;
    }

    struct Module *consumer = find_module_by_name(config[i].module_name);
    if (consumer == NULL) {
      continue; /* Missing module handled by add_module_to_pipeline */
    }

    /* Every process_per_event module must declare subscriptions */
    if (consumer->num_subscriptions == 0) {
      ERROR_LOG("Configuration error in phase '%s':", phase_name);
      ERROR_LOG("  Module '%s' is configured as process_per_event but declares "
                "no event subscriptions in module_info.yaml",
                consumer->name);
      ERROR_LOG("  Fix: Add an events.consumes section to %s/module_info.yaml", consumer->name);
      return -1;
    }

    /* Each subscription's producer must be in the same phase as process_full_halo */
    for (int s = 0; s < consumer->num_subscriptions; s++) {
      int producer_id = consumer->subscriptions[s].producer_module_id;
      const char *producer_name = consumer->subscriptions[s].producer_name;
      bool producer_found = false;

      for (int j = 0; j < num_modules; j++) {
        if (config[j].processing_mode != PROCESSING_MODE_FULL_HALO) {
          continue;
        }
        struct Module *candidate = find_module_by_name(config[j].module_name);
        if (candidate != NULL && candidate->module_id == producer_id) {
          producer_found = true;
          break;
        }
      }

      if (!producer_found) {
        ERROR_LOG("Configuration error in phase '%s':", phase_name);
        ERROR_LOG("  Module '%s' (process_per_event) subscribes to producer "
                  "'%s' (id=%d, event='%s'), but that producer is not "
                  "configured as process_full_halo in the same phase",
                  consumer->name, producer_name, producer_id,
                  consumer->subscriptions[s].event_name);
        ERROR_LOG("  Fix: Add '%s: process_full_halo' to the %s phase in "
                  "the input YAML, or move both modules to the same phase",
                  producer_name, phase_name);
        return -1;
      }
    }
  }
  return 0;
}

static void free_phase_event_buffer(void);

/* ── Phase plans: each FoF phase's dispatch lists, built once at startup ── */

/**
 * @brief   The resolved modules of one FoF phase, split by processing mode
 *
 * Built by module_system_init() after every module's init() has succeeded and
 * released by release_phase_plans(). Between the two its contents are
 * read-only, so execute_phase() walks only the entries of the mode it is
 * dispatching instead of re-testing every entry's mode on every call. The
 * four lists share one allocation of `num_entries` slots, in YAML order
 * within each mode; a module listed twice in the phase appears twice. The rate
 * list is non-empty only in the rate phase, which holds nothing else and whose
 * dispatch the rate plan (rate_assembly.h) describes.
 */
struct PhasePlan {
  const struct PhaseModuleConfig *phase_config; /**< Key: the phase's configuration array */
  const char *phase_name;                       /**< For diagnostics; outlives the plan */
  int num_entries;                              /**< The phase's entry count at build time */
  struct Module **modules;                      /**< Owns the three lists below */
  struct Module *const *full_halo;
  int num_full_halo;
  struct Module *const *per_event;
  int num_per_event;
  struct Module *const *by_galaxy;
  int num_by_galaxy;
  struct Module *const *rates;
  int num_rates;
  /**
   * The discrete-phase ledger's declarations, or NULL when the ledger does not
   * run in this phase (no rate phase is configured, or this is the rate phase).
   * Row u < num_full_halo is full-halo unit u, row num_full_halo the by-galaxy
   * pass; entry [u * MIMIC_RESERVOIR_TOTAL_COUNT + t] is true when the unit's
   * declarations cover a change to total t.
   */
  bool *ledger_declares;
};

/** pre_timestep, every named substep phase and post_timestep; post_snapshot has no plan */
#define MAX_PHASE_PLANS (MAX_SUBSTEP_PHASES + 2)

static struct PhasePlan phase_plans[MAX_PHASE_PLANS];
static int num_phase_plans = 0;

/**
 * @brief   Release every phase plan
 *
 * Called by free_phase_configuration() before the phase arrays the plans are
 * keyed by are freed, and by module_system_init() for a table a previous
 * successful initialisation left populated.
 */
static void release_phase_plans(void) {
  int abandoned = 0;
  for (int p = 0; p < num_phase_plans; p++) {
    /* A test-tier memory reset between init and cleanup must not abort the
     * process on a stale pointer; ./mimic never resets the memory system. */
    if (is_tracked_memory_block(phase_plans[p].modules)) {
      myfree(phase_plans[p].modules);
    } else {
      abandoned++;
    }
    if (phase_plans[p].ledger_declares != NULL) {
      if (is_tracked_memory_block(phase_plans[p].ledger_declares)) {
        myfree(phase_plans[p].ledger_declares);
      } else {
        abandoned++;
      }
    }
    phase_plans[p] = (struct PhasePlan){0};
  }
  if (abandoned > 0) {
    WARNING_LOG("Phase plan table held %d pointer(s) the memory system no longer tracks (memory "
                "system reset without module_system_cleanup()); dropping them",
                abandoned);
  }
  num_phase_plans = 0;
}

/**
 * @brief   Append the given phase's entries of one processing mode to a plan list
 *
 * @return  Number of entries appended
 */
static int collect_phase_mode(const struct PhaseModuleConfig *modules, int num_modules,
                              enum ProcessingMode mode, struct Module **out) {
  int count = 0;
  for (int i = 0; i < num_modules; i++) {
    if (modules[i].processing_mode == mode) {
      out[count++] = modules[i].resolved;
    }
  }
  return count;
}

/** @brief The index of a conserved total named @p name, or -1 when the build has none */
static int conserved_total_index(const char *name) {
  const int count = MIMIC_RESERVOIR_TOTAL_COUNT;
  for (int t = 0; t < count; t++) {
    if (strcmp(mimic_reservoir_total_names()[t], name) == 0) {
      return t;
    }
  }
  return -1;
}

/** @brief Mark the totals @p mod lists in changes_totals in one ledger row (names checked) */
static void declare_module_totals(const struct Module *mod, bool *row) {
  for (int i = 0; i < mod->num_changes_totals; i++) {
    const int t = conserved_total_index(mod->changes_totals[i]);
    if (t < 0) {
      FATAL_ERROR("Module '%s' declares changes_totals '%s', which is not a conserved total of "
                  "this build — build_rate_phase() must refuse it first",
                  mod->name, mod->changes_totals[i]);
    }
    row[t] = true;
  }
}

/** @brief Whether per-event module @p consumer subscribes to an event @p producer emits */
static bool subscribes_to(const struct Module *consumer, const struct Module *producer) {
  if (producer->module_id <= 0) {
    return false; /* not a producer */
  }
  for (int s = 0; s < consumer->num_subscriptions; s++) {
    if (consumer->subscriptions[s].producer_module_id == producer->module_id) {
      return true;
    }
  }
  return false;
}

/**
 * @brief   The discrete-phase ledger's declarations of one discrete phase (struct PhasePlan)
 *
 * A full-halo unit is covered by its module's changes_totals and by those of
 * every per-event module in the phase that subscribes to one of its events; the
 * by-galaxy pass by every by-galaxy module's in the phase.
 *
 * @return  A tracked MEM_UTILITY allocation, or NULL when the ledger does not run
 */
static bool *build_ledger_declarations(const struct PhasePlan *plan) {
  const int num_totals = MIMIC_RESERVOIR_TOTAL_COUNT;
  if (rate_plan == NULL || plan->num_rates > 0 || num_totals == 0) {
    return NULL;
  }
  const size_t rows = (size_t)plan->num_full_halo + 1;
  bool *declares = mymalloc_cat(rows * (size_t)num_totals * sizeof(bool), MEM_UTILITY);
  memset(declares, 0, rows * (size_t)num_totals * sizeof(bool));
  for (int u = 0; u < plan->num_full_halo; u++) {
    bool *row = declares + (size_t)u * (size_t)num_totals;
    declare_module_totals(plan->full_halo[u], row);
    for (int c = 0; c < plan->num_per_event; c++) {
      if (subscribes_to(plan->per_event[c], plan->full_halo[u])) {
        declare_module_totals(plan->per_event[c], row);
      }
    }
  }
  bool *by_galaxy_row = declares + (size_t)plan->num_full_halo * (size_t)num_totals;
  for (int b = 0; b < plan->num_by_galaxy; b++) {
    declare_module_totals(plan->by_galaxy[b], by_galaxy_row);
  }
  return declares;
}

/**
 * @brief   for_each_phase visitor: build the dispatch plan of one FoF phase
 *
 * Fatal when an entry is unresolved, when the plan table is full, or when the
 * four mode lists do not together cover every entry of the phase.
 */
static void build_phase_plan_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                     int num_modules, void *userdata) {
  (void)userdata;
  if (num_modules == 0 || strcmp(phase_name, POST_SNAPSHOT_PHASE_NAME) == 0) {
    return; /* Empty phases need no plan; post_snapshot runs through execute_post_snapshot() */
  }
  if (num_phase_plans >= MAX_PHASE_PLANS) {
    FATAL_ERROR("Too many FoF phases to plan (max %d)", MAX_PHASE_PLANS);
  }
  for (int i = 0; i < num_modules; i++) {
    if (modules[i].resolved == NULL) {
      FATAL_ERROR("Module '%s' in phase '%s' was not resolved — cannot build the phase's "
                  "dispatch plan",
                  modules[i].module_name, phase_name);
    }
  }

  struct Module **slots = mymalloc_cat((size_t)num_modules * sizeof(struct Module *), MEM_UTILITY);
  const int num_full_halo =
      collect_phase_mode(modules, num_modules, PROCESSING_MODE_FULL_HALO, slots);
  const int num_per_event =
      collect_phase_mode(modules, num_modules, PROCESSING_MODE_PER_EVENT, slots + num_full_halo);
  const int num_by_galaxy = collect_phase_mode(modules, num_modules, PROCESSING_MODE_BY_GALAXY,
                                               slots + num_full_halo + num_per_event);
  const int num_rates = collect_phase_mode(modules, num_modules, PROCESSING_MODE_RATES,
                                           slots + num_full_halo + num_per_event + num_by_galaxy);

  /* Internal invariant: validate_phase_processing_modes() has already refused every mode that
   * is neither a FoF mode nor process_rates in the rate phase, so the four lists cover the
   * phase. An entry in none of them would be dropped silently from dispatch (a mode added
   * without its list), so fail rather than plan. */
  const int num_planned = num_full_halo + num_per_event + num_by_galaxy + num_rates;
  if (num_planned != num_modules) {
    FATAL_ERROR("Phase '%s' has %d entries but its dispatch plan covers only %d: a configured "
                "processing mode has no plan list",
                phase_name, num_modules, num_planned);
  }

  struct PhasePlan *plan = &phase_plans[num_phase_plans++];
  *plan = (struct PhasePlan){
      .phase_config = modules,
      .phase_name = phase_name,
      .num_entries = num_modules,
      .modules = slots,
      .full_halo = slots,
      .num_full_halo = num_full_halo,
      .per_event = slots + num_full_halo,
      .num_per_event = num_per_event,
      .by_galaxy = slots + num_full_halo + num_per_event,
      .num_by_galaxy = num_by_galaxy,
      .rates = slots + num_full_halo + num_per_event + num_by_galaxy,
      .num_rates = num_rates,
  };
  plan->ledger_declares = build_ledger_declarations(plan);
}

/**
 * @brief   Find the plan of a phase configuration array
 *
 * @return  The plan, or NULL when the array has none or its entry count has
 *          changed since the plan was built
 */
static const struct PhasePlan *find_phase_plan(const struct PhaseModuleConfig *phase_config,
                                               int num_modules) {
  for (int p = 0; p < num_phase_plans; p++) {
    if (phase_plans[p].phase_config == phase_config) {
      return phase_plans[p].num_entries == num_modules ? &phase_plans[p] : NULL;
    }
  }
  return NULL;
}

/* ── for_each_phase visitors used by module_system_init ─────────────────── */

static void build_pipeline_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                   int num_modules, void *userdata) {
  (void)userdata;
  for (int i = 0; i < num_modules; i++) {
    modules[i].resolved = add_module_to_pipeline(phase_name, modules[i].module_name);
  }
}

static void log_phase_size_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                   int num_modules, void *userdata) {
  (void)modules;
  (void)userdata;
  VERBOSE_LOG("  %s: %d module(s)", phase_name, num_modules);
}

static void validate_modes_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                   int num_modules, void *userdata) {
  /* post_snapshot is a reserved name, so no substep phase can match it here. */
  const int failed = (strcmp(phase_name, POST_SNAPSHOT_PHASE_NAME) == 0)
                         ? validate_post_snapshot_entries(modules, num_modules)
                         : validate_phase_processing_modes(modules, num_modules, phase_name);
  if (failed != 0) {
    *(int *)userdata = 1;
  }
}

static void validate_events_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                    int num_modules, void *userdata) {
  if (validate_event_subscriptions(modules, num_modules, phase_name) != 0) {
    *(int *)userdata = 1;
  }
}

/** The rate phase found by find_rate_phase_visitor(), and whether a second one was refused */
struct RatePhaseSearch {
  const char *name;
  struct PhaseModuleConfig *entries;
  int num_entries;
  int failed;
};

/**
 * @brief   for_each_phase visitor: find the rate phase, refusing a second one
 *
 * Runs after validate_modes_visitor(), so a phase holding a process_rates entry
 * is a valid rate phase (a named substep phase of process_rates entries only).
 */
static void find_rate_phase_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                    int num_modules, void *userdata) {
  struct RatePhaseSearch *search = userdata;
  if (num_modules == 0 || modules[0].processing_mode != PROCESSING_MODE_RATES) {
    return;
  }
  if (search->name != NULL) {
    ERROR_LOG("Configuration error in phase '%s':", phase_name);
    ERROR_LOG("  A second rate phase: phase '%s' already runs process_rates, and a run has at "
              "most one rate phase",
              search->name);
    ERROR_LOG("  Fix: put every process_rates module in one phase");
    search->failed = 1;
    return;
  }
  search->name = phase_name;
  search->entries = modules;
  search->num_entries = num_modules;
}

/**
 * @brief   Release the rate phase's plan and scratch
 *
 * Called by module_system_cleanup(), by module_system_init() for a plan an
 * earlier initialisation left, and when initialisation fails after the plan
 * was built.
 */
static void release_rate_phase(void) {
  if (rate_plan != NULL) {
    /* A test-tier memory reset between init and cleanup must not abort the
     * process on a stale pointer; ./mimic never resets the memory system. */
    if (is_tracked_memory_block(rate_plan)) {
      rate_assembly_plan_free(rate_plan);
    } else {
      WARNING_LOG("Rate plan held a pointer the memory system no longer tracks (memory system "
                  "reset without module_system_cleanup()); dropping it");
    }
  }
  rate_plan = NULL;
  rate_phase_config = NULL;
  rate_assembly_scratch_release(&rate_scratch);
}

/**
 * @brief   for_each_phase visitor: refuse a changes_totals name the build has no total for
 *
 * Generation validates the names against the same property metadata the
 * reservoir table is generated from, so a refusal here means stale generated
 * code; it is still refused before any init() rather than trusted.
 */
static void check_declared_totals_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                          int num_modules, void *userdata) {
  for (int i = 0; i < num_modules; i++) {
    const struct Module *mod = modules[i].resolved;
    for (int k = 0; mod != NULL && k < mod->num_changes_totals; k++) {
      if (conserved_total_index(mod->changes_totals[k]) < 0) {
        ERROR_LOG("Configuration error in phase '%s':", phase_name);
        ERROR_LOG("  Module '%s' declares changes_totals '%s', which is not a conserved total "
                  "of this build's reservoir table",
                  mod->name, mod->changes_totals[k]);
        ERROR_LOG("  Fix: regenerate the build (make generate), or correct the module's "
                  "changes_totals");
        *(int *)userdata = 1;
      }
    }
  }
}

/**
 * @brief   Find the rate phase, if any, and build its plan
 *
 * With a rate phase, also checks every configured module's changes_totals
 * names against the reservoir table and zeroes the run's rate statistics.
 *
 * @return  0 on success (with or without a rate phase), -1 after an ERROR_LOG
 */
static int build_rate_phase(void) {
  struct RatePhaseSearch search = {NULL, NULL, 0, 0};
  for_each_phase(find_rate_phase_visitor, &search);
  if (search.failed) {
    return -1;
  }
  if (search.name == NULL) {
    return 0;
  }
  int stale = 0;
  for_each_phase(check_declared_totals_visitor, &stale);
  if (stale) {
    return -1;
  }
  if (rate_assembly_plan_build(search.name, search.entries, search.num_entries,
                               &MimicConfig.RateIntegration, &rate_plan) != 0) {
    return -1;
  }
  rate_phase_config = search.entries;
  memset(&rate_statistics, 0, sizeof(rate_statistics));
  return 0;
}

size_t module_rate_scratch_bytes(void) { return rate_scratch.capacity; }

const struct RateAssemblyPlan *module_rate_plan(void) { return rate_plan; }

struct RateStatistics *module_rate_statistics(void) {
  return rate_plan != NULL ? &rate_statistics : NULL;
}

int64_t module_discrete_ledger_units(void) { return discrete_ledger_units; }

/**
 * @brief   Initialize the module system
 *
 * Validates multi-phase pipeline configuration and initializes all referenced
 * modules. Modules are initialized in the order they appear across all phases
 * (pre_timestep, named substep phases, post_timestep, post_snapshot), with
 * duplicates initialized only once. Once every init() has returned 0, it
 * builds each non-empty FoF phase's dispatch plan, which execute_phase()
 * requires; the phase configuration must not change until cleanup.
 *
 * @return  0 on success, non-zero if initialization fails
 */
int module_system_init(void) {
  int validation_failed = 0;

  VERBOSE_LOG("Initializing multi-phase module system");

  /* Plans and an event buffer left by an earlier initialisation that was not
   * cleaned up; both releases tolerate a memory-system reset in between. */
  release_phase_plans();
  free_phase_event_buffer();
  release_rate_phase();
  discrete_ledger_units = 0;

  /* Build execution pipeline by collecting all unique modules across phases */
  num_pipeline_modules = 0;
  for_each_phase(build_pipeline_visitor, NULL);

  if (num_pipeline_modules == 0) {
    INFO_LOG("No modules configured (physics-free mode)");
    VERBOSE_LOG("SubSteps = %d", MimicConfig.SubSteps);
    return 0;
  }

  VERBOSE_LOG("Pipeline configuration:");
  VERBOSE_LOG("  SubSteps: %d", MimicConfig.SubSteps);
  for_each_phase(log_phase_size_visitor, NULL);
  VERBOSE_LOG("  Total unique modules: %d", num_pipeline_modules);

  /* Validate processing mode configurations */
  VERBOSE_LOG("Validating module processing mode configurations...");
  for_each_phase(validate_modes_visitor, &validation_failed);
  if (validation_failed) {
    return -1;
  }
  VERBOSE_LOG("Processing mode validation passed");

  /* Validate event subscription contracts for each phase */
  VERBOSE_LOG("Validating event subscription contracts...");
  for_each_phase(validate_events_visitor, &validation_failed);
  if (validation_failed) {
    return -1;
  }
  VERBOSE_LOG("Event subscription validation passed");

  /* The rate phase: at most one, its transfers checked against the reservoir
   * table and each other, and its integration domain fixed, before any init() */
  if (build_rate_phase() != 0) {
    return -1;
  }

  /* Initialize all modules in pipeline order */
  for (int i = 0; i < num_pipeline_modules; i++) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Initializing module: %s", mod->name);

    const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_INIT, mod->name);
    int result = mod->init();
    leave_callback(prior);
    if (result != 0) {
      ERROR_LOG("Module '%s' initialization failed with code %d", mod->name, result);
      release_rate_phase(); /* a failed initialisation leaves no plan */
      return result;
    }
  }

  /* Last, so a failed initialisation leaves no plan */
  for_each_phase(build_phase_plan_visitor, NULL);

  VERBOSE_LOG("Module system initialized successfully");
  return 0;
}

/**
 * @brief   Ensure the phase event buffer can hold at least `required` events
 *
 * Growth arithmetic mirrors fof_workspace_reserve() in fof_workspace.c
 * (same factor, same minimum increment, same ceiling, same fatal), which is the
 * established pattern for buffers whose size follows the FoF workspace being
 * processed. Nothing in the module contract limits a producer to one event per
 * halo, so capacity is grown lazily on demand rather than presized from ngal.
 *
 * MAX_HALO_ARRAY_SIZE stays as a fail-fast backstop against runaway or
 * malformed emission (VISION.md Principle 7). It is a structural safety limit,
 * not a scale-dependent cap: no simulation's legitimate event count should
 * approach it.
 */
static void ensure_phase_event_capacity(int required) {
  while (required > phase_event_state.event_capacity) {
    int new_capacity = (int)(phase_event_state.event_capacity * HALO_ARRAY_GROWTH_FACTOR);

    if (new_capacity - phase_event_state.event_capacity < MIN_HALO_ARRAY_GROWTH)
      new_capacity = phase_event_state.event_capacity + MIN_HALO_ARRAY_GROWTH;

    if (new_capacity > MAX_HALO_ARRAY_SIZE)
      new_capacity = MAX_HALO_ARRAY_SIZE;

    if (new_capacity <= phase_event_state.event_capacity) {
      FATAL_ERROR("Phase event buffer requires %d events but maximum allowed size is %d", required,
                  MAX_HALO_ARRAY_SIZE);
    }

    INFO_LOG("Growing phase event buffer from %d to %d elements", phase_event_state.event_capacity,
             new_capacity);

    const size_t bytes = (size_t)new_capacity * sizeof(struct ModuleEvent);
    phase_event_state.events = myrealloc_cat(phase_event_state.events, bytes, MEM_UTILITY);
    phase_event_state.event_capacity = new_capacity;
  }
}

/**
 * @brief   Release the run-persistent phase event buffer
 *
 * Called by module_system_cleanup(), and by module_system_init() for a buffer
 * a previous initialisation left without a cleanup.
 */
static void free_phase_event_buffer(void) {
  if (phase_event_state.events == NULL) {
    return;
  }

  /* A test-tier memory reset between init and cleanup must not abort the
   * process on a stale pointer; ./mimic never resets the memory system. */
  if (is_tracked_memory_block(phase_event_state.events)) {
    myfree(phase_event_state.events);
  } else {
    WARNING_LOG("Phase event buffer held a pointer the memory system no longer tracks (memory "
                "system reset without module_system_cleanup()); dropping it");
  }
  phase_event_state.events = NULL;
  phase_event_state.event_capacity = 0;
}

/**
 * @brief   Initialize per-phase event dispatch state
 */
static void begin_phase_event_dispatch(const struct PhasePlan *plan, struct ModuleContext *ctx,
                                       struct Halo *halos, int ngal) {
  phase_event_state.event_count = 0;
  phase_event_state.last_dispatched_event = 0;
  phase_event_state.per_event = plan->per_event;
  phase_event_state.num_per_event = plan->num_per_event;
  phase_event_state.ctx = ctx;
  phase_event_state.halos = halos;
  phase_event_state.ngal = ngal;
  phase_event_state.active = true;
  phase_event_state.emission_allowed = false;
  phase_event_state.current_producer_module_id = 0;

  if (ctx != NULL) {
    ctx->active_event = NULL;
  }
}

/**
 * @brief   Clear per-phase event dispatch state
 */
static void end_phase_event_dispatch(void) {
  if (phase_event_state.ctx != NULL) {
    phase_event_state.ctx->active_event = NULL;
  }

  phase_event_state.event_count = 0;
  phase_event_state.last_dispatched_event = 0;
  phase_event_state.per_event = NULL;
  phase_event_state.num_per_event = 0;
  phase_event_state.ctx = NULL;
  phase_event_state.halos = NULL;
  phase_event_state.ngal = 0;
  phase_event_state.active = false;
  phase_event_state.emission_allowed = false;
  phase_event_state.current_producer_module_id = 0;
}

/**
 * @brief   Dispatch a contiguous range of queued events to subscribed per-event modules
 *
 * Only delivers each event to consumers whose EventSubscription records match
 * the event's (producer_module_id, event_id) pair. This is the primary routing
 * filter; consumer modules do not need to perform defensive event-code checks.
 */
static void dispatch_events_range(int start_index, int end_index) {
  if (!phase_event_state.active || start_index >= end_index) {
    return;
  }

  /*
   * Event emission is producer-only for v1.
   * Disable emission while running per-event consumers to prevent recursive
   * re-emit loops from consumer modules.
   */
  bool prior_emission_allowed = phase_event_state.emission_allowed;
  phase_event_state.emission_allowed = false;

  for (int event_index = start_index; event_index < end_index; event_index++) {
    const struct ModuleEvent *event = &phase_event_state.events[event_index];

    if (event->target_index < 0 || event->target_index >= phase_event_state.ngal) {
      FATAL_ERROR("Event dispatch failed: target_index=%d out of bounds [0, %d)",
                  event->target_index, phase_event_state.ngal);
    }

    struct Halo *target_halo = &phase_event_state.halos[event->target_index];

    for (int i = 0; i < phase_event_state.num_per_event; i++) {
      struct Module *mod = phase_event_state.per_event[i];

      /* Subscription routing: only dispatch to consumers subscribed to this event */
      bool subscribed = false;
      for (int s = 0; s < mod->num_subscriptions; s++) {
        if (mod->subscriptions[s].producer_module_id == event->producer_module_id &&
            mod->subscriptions[s].event_id == event->event_id) {
          subscribed = true;
          break;
        }
      }
      if (!subscribed) {
        continue;
      }

      phase_event_state.ctx->active_event = event;

      DEBUG_LOG("Executing module: %s (event_id=%d, producer_module_id=%d, "
                "source=%d, target=%d, substep %d/%d, z=%.3f)",
                mod->name, event->event_id, event->producer_module_id, event->source_index,
                event->target_index, phase_event_state.ctx->substep_number + 1,
                phase_event_state.ctx->num_substeps, phase_event_state.ctx->redshift);

      const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_PER_EVENT, mod->name);
      int result = mod->process(phase_event_state.ctx, target_halo, 1);
      leave_callback(prior);
      if (result != 0) {
        FATAL_ERROR("Module '%s' failed on event %d (event_id=%d, substep %d)", mod->name,
                    event_index, event->event_id, phase_event_state.ctx->substep_number);
      }
    }

    phase_event_state.ctx->active_event = NULL;
  }

  phase_event_state.emission_allowed = prior_emission_allowed;
  phase_event_state.last_dispatched_event = end_index;
}

int module_emit_event(struct ModuleContext *ctx, int event_id, int source_index, int target_index,
                      double value0, double value1) {
  /* Checked first, and before the shortcut below: a rate law runs inside its
   * phase's event-dispatch window, so neither a context it kept from an earlier
   * discrete callback nor that shortcut may let it emit. */
  if (running_callback.kind == RUNNING_CALLBACK_RATE) {
    ERROR_LOG("module_emit_event refused for module '%s': called from a process_rates callback "
              "(event_id=%d); a rate law returns coefficients and cannot emit events",
              running_callback.module_name != NULL ? running_callback.module_name : "(unnamed)",
              event_id);
    return -1;
  }

  if (ctx == NULL) {
    ERROR_LOG("module_emit_event called with NULL context");
    return -1;
  }

  /* Checked before the shortcut below: a snapshot callback has no FoF event
   * buffer to emit into, and must not be mistaken for a direct unit test. */
  if (snapshot_dispatch_active) {
    ERROR_LOG("module_emit_event called during post_snapshot dispatch (event_id=%d); snapshot "
              "callbacks cannot emit or consume FoF events",
              event_id);
    return -1;
  }

  /* Allow direct module unit tests to call producers without active dispatch. */
  if (!phase_event_state.active) {
    DEBUG_LOG("Dropping event_id=%d because no phase dispatch context is active", event_id);
    return 0;
  }

  if (ctx != phase_event_state.ctx) {
    ERROR_LOG("module_emit_event called with mismatched ModuleContext");
    return -1;
  }

  if (!phase_event_state.emission_allowed) {
    ERROR_LOG("module_emit_event is only allowed during PROCESSING_MODE_FULL_HALO execution");
    return -1;
  }

  /* Validate the emitting module declared events.emits (module_id == 0 means
   * the generator assigned no producer ID — the module has no emits declaration) */
  if (phase_event_state.current_producer_module_id == 0) {
    ERROR_LOG("module_emit_event called from a module with no events.emits declaration "
              "(module_id == 0); add an events.emits section to module_info.yaml");
    return -1;
  }

  /* Validate event_id is positive (0 is reserved for unset) */
  if (event_id <= 0) {
    ERROR_LOG("module_emit_event: invalid event_id=%d (must be > 0; use generated "
              "constants from _system/generated/event_contracts.h)",
              event_id);
    return -1;
  }

  /* Validate the (producer, event_id) pair was declared in module_info.yaml */
  const struct Module *producer = find_module_by_id(phase_event_state.current_producer_module_id);
  if (producer == NULL) {
    FATAL_ERROR("module_emit_event internal error: producer module_id=%d is not registered",
                phase_event_state.current_producer_module_id);
  }
  if (producer->num_emitted_events <= 0) {
    FATAL_ERROR("module_emit_event internal error: module '%s' has producer module_id=%d "
                "but no declared events.emits entries",
                producer->name, phase_event_state.current_producer_module_id);
  }

  bool event_declared = false;
  for (int i = 0; i < producer->num_emitted_events; i++) {
    if (producer->emitted_event_ids[i] == event_id) {
      event_declared = true;
      break;
    }
  }
  if (!event_declared) {
    ERROR_LOG("module_emit_event: module '%s' emitted event_id=%d which is not "
              "declared in its events.emits (module_info.yaml)",
              producer->name, event_id);
    return -1;
  }

  /* ngal is the committed row count: records created by the running callback
   * are appended only when it returns, so an event cannot name one of them. */
  if (source_index < 0 || source_index >= phase_event_state.ngal) {
    ERROR_LOG("module_emit_event invalid source_index=%d: outside the %d committed rows (a "
              "record created in the running callback cannot be named until it returns)",
              source_index, phase_event_state.ngal);
    return -1;
  }

  if (target_index < 0 || target_index >= phase_event_state.ngal) {
    ERROR_LOG("module_emit_event invalid target_index=%d: outside the %d committed rows (a "
              "record created in the running callback cannot be named until it returns)",
              target_index, phase_event_state.ngal);
    return -1;
  }

  /* Grow before taking a slot pointer: a realloc here can move the buffer, and
   * only this path may grow it (emission is disabled while consumers run), so
   * event pointers handed to consumers below stay valid for their dispatch. */
  ensure_phase_event_capacity(phase_event_state.event_count + 1);

  int event_index = phase_event_state.event_count;
  struct ModuleEvent *event = &phase_event_state.events[event_index];
  event->producer_module_id = phase_event_state.current_producer_module_id;
  event->event_id = event_id;
  event->source_index = source_index;
  event->target_index = target_index;
  event->value0 = value0;
  event->value1 = value1;

  phase_event_state.event_count++;

  /* Dispatch immediately to preserve producer-side event ordering semantics. */
  dispatch_events_range(event_index, event_index + 1);
  return 0;
}

/*
 * The FoF workspace row count as a module callback receives it. The process()
 * ABI counts a workspace in int while the descriptor counts in int64_t; the
 * workspace is bounded by MAX_HALO_ARRAY_SIZE, below INT_MAX, so this never
 * fires on a valid run.
 */
static int workspace_callback_count(const struct FoFWorkspace *ws) {
  return narrow_int64_to_int_checked(ws->count, "FoF workspace galaxy count");
}

/* ==============================================================================
 * RECORD CREATION
 * ============================================================================== */

/** @brief Staging block holding staged row @p k, and the row's offset within it */
static int64_t staging_block_of(int64_t k, int64_t *offset) {
  /* Block b covers rows [R (2^b - 1), R (2^(b+1) - 1)), so b = floor(log2(k / R + 1)). */
  int64_t block = 0;
  for (int64_t units = k / RECORD_STAGING_BLOCK_ROWS + 1; units > 1; units >>= 1) {
    block++;
  }
  *offset = k - RECORD_STAGING_BLOCK_ROWS * ((INT64_C(1) << block) - 1);
  return block;
}

/** @brief Staged row @p k of the running callback (k < staged_count or the next slot) */
static struct Halo *staged_row(int64_t k) {
  int64_t offset = 0;
  const int64_t block = staging_block_of(k, &offset);
  return &record_creation.blocks[block][offset];
}

/** @brief Make staging slot @p k addressable, allocating its block when needed */
static void reserve_staging_slot(int64_t k) {
  if (k >= MAX_HALO_ARRAY_SIZE) {
    FATAL_ERROR("Record creation: one callback staged more than %d records", MAX_HALO_ARRAY_SIZE);
  }

  int64_t offset = 0;
  const int64_t block = staging_block_of(k, &offset);
  /* The row ceiling above keeps block below the table size; guard it explicitly. */
  if (block >= RECORD_STAGING_MAX_BLOCKS) {
    FATAL_ERROR("Record creation: staged row %" PRId64 " needs staging block %" PRId64
                " but only %d exist",
                k, block, RECORD_STAGING_MAX_BLOCKS);
  }
  while (record_creation.block_count <= block) {
    const int64_t rows = (int64_t)RECORD_STAGING_BLOCK_ROWS << record_creation.block_count;
    record_creation.blocks[record_creation.block_count++] =
        mymalloc_cat((size_t)rows * sizeof(struct Halo), MEM_HALOS);
  }

  if (k >= record_creation.staged_host_capacity) {
    const int64_t capacity = fof_workspace_grown_capacity(record_creation.staged_host_capacity,
                                                          k + 1, "Record creation staged-host map");
    record_creation.staged_host =
        myrealloc_cat(record_creation.staged_host,
                      (size_t)capacity * sizeof(*record_creation.staged_host), MEM_HALOS);
    record_creation.staged_host_capacity = capacity;
  }
}

/** @brief Clear the per-host ordinals for this FoF step on its first creation */
static void ready_creation_ordinals(const struct FoFWorkspace *ws) {
  if (record_creation.ordinals_ready) {
    return;
  }
  if (ws->base_count > record_creation.ordinal_capacity) {
    const int64_t capacity = fof_workspace_grown_capacity(
        record_creation.ordinal_capacity, ws->base_count, "Record creation per-host ordinals");
    record_creation.ordinals = myrealloc_cat(
        record_creation.ordinals, (size_t)capacity * sizeof(*record_creation.ordinals), MEM_HALOS);
    record_creation.ordinal_capacity = capacity;
  }
  if (ws->base_count > 0) {
    memset(record_creation.ordinals, 0, (size_t)ws->base_count * sizeof(*record_creation.ordinals));
  }
  record_creation.ordinals_ready = true;
}

/** @brief "from init()", "from a process_by_galaxy callback", ... for refusals */
static const char *running_callback_description(void) {
  switch (running_callback.kind) {
  case RUNNING_CALLBACK_INIT:
    return "from init()";
  case RUNNING_CALLBACK_PER_EVENT:
    return "from a process_per_event callback";
  case RUNNING_CALLBACK_BY_GALAXY:
    return "from a process_by_galaxy callback";
  case RUNNING_CALLBACK_SNAPSHOT:
    return "from a process_snapshot callback";
  case RUNNING_CALLBACK_RATE:
    return "from a process_rates callback";
  case RUNNING_CALLBACK_CLEANUP:
    return "from cleanup()";
  case RUNNING_CALLBACK_FULL_HALO: /* creation is legal there, so it is never refused */
  case RUNNING_CALLBACK_NONE:
    break;
  }
  return "outside any module callback";
}

/**
 * @brief   Caller check: a running process_full_halo callback, a row pointer, its own context
 * @return  true when @p module may create; false after an ERROR_LOG naming the reason
 */
static bool creation_caller_allowed(const char *module, const struct ModuleContext *ctx,
                                    struct Halo **row) {
  if (running_callback.kind != RUNNING_CALLBACK_FULL_HALO) {
    ERROR_LOG("module_create_record refused for module '%s': called %s; records can only be "
              "created from a running process_full_halo callback",
              module, running_callback_description());
    return false;
  }

  if (row == NULL) {
    ERROR_LOG("module_create_record refused for module '%s': row pointer is NULL", module);
    return false;
  }

  if (ctx != record_creation.ctx) {
    ERROR_LOG("module_create_record refused for module '%s': mismatched ModuleContext", module);
    return false;
  }
  return true;
}

/**
 * @brief   Host check: @p host_index names a row that may host a created record
 *
 * The host must be a committed row present when the pipeline started, of Type 0
 * or 1, with a galaxy, and its subhalo slice's Type 0/1 row as inheritance
 * assembled it: set_local_centrals() (inheritance.c) stamps that row's index
 * into every row of the slice as CentralHalo, so CentralHalo == host_index
 * holds for one row per slice. A row a module promoted to Type 1 fails it,
 * which keeps two rows of one HaloNr from drawing the same created IDs from
 * their separate per-row ordinals.
 *
 * @return  The host row, or NULL after an ERROR_LOG naming the reason. A
 *          workspace the core did not set up for creation is fatal.
 */
static const struct Halo *creation_host(const char *module, const struct FoFWorkspace *ws,
                                        int host_index) {
  const int64_t committed = ws->count;

  /* Core invariants, not module errors: base_count comes from
   * process_halo_evolution() and every row past it has a recorded host. */
  if (ws->base_count < 0 || ws->base_count > committed ||
      committed - ws->base_count > ws->created_capacity || ws->pool == NULL) {
    FATAL_ERROR("module_create_record: FoF workspace is not set up for creation (count=%" PRId64
                ", base_count=%" PRId64 ", created_capacity=%" PRId64 ", pool=%p); "
                "process_halo_evolution() sets base_count before the pipeline",
                committed, ws->base_count, ws->created_capacity, (void *)ws->pool);
  }

  if (host_index < 0 || host_index >= committed) {
    ERROR_LOG("module_create_record refused for module '%s': host_index=%d is outside the %" PRId64
              " committed rows",
              module, host_index, committed);
    return NULL;
  }

  if (host_index >= ws->base_count) {
    ERROR_LOG("module_create_record refused for module '%s': host_index=%d is a record created "
              "in this FoF step (created rows start at %" PRId64 "); created rows cannot host",
              module, host_index, ws->base_count);
    return NULL;
  }

  const struct Halo *host = &ws->halos[host_index];
  if (host->Type != 0 && host->Type != 1) {
    ERROR_LOG("module_create_record refused for module '%s': host row %d (UniqueGalaxyID %lld) "
              "is Type %d; only a Type 0 or 1 row can host",
              module, host_index, host->UniqueGalaxyID, host->Type);
    return NULL;
  }

  if (host->galaxy == NULL) {
    ERROR_LOG("module_create_record refused for module '%s': host row %d (UniqueGalaxyID %lld) "
              "has no galaxy",
              module, host_index, host->UniqueGalaxyID);
    return NULL;
  }

  if (host->CentralHalo != host_index) {
    ERROR_LOG("module_create_record refused for module '%s': host row %d (UniqueGalaxyID %lld) "
              "has CentralHalo %d; only the Type 0/1 row inheritance assembled for a subhalo "
              "slice (CentralHalo equal to its own row) can host",
              module, host_index, host->UniqueGalaxyID, host->CentralHalo);
    return NULL;
  }
  return host;
}

/**
 * @brief   Identity check: the host's next ordinal, in a space that can encode it
 *
 * @return  The ordinal (>= 0), or -1 after an ERROR_LOG when the run's space does
 *          not fit int64 or the host has used up the radix. A host or unit
 *          outside the published space, or a space published as fitting that
 *          does not, is fatal: the encoder's preconditions are live assert()s,
 *          and this guard keeps them unreachable.
 */
static int creation_ordinal(const char *module, const struct FoFWorkspace *ws, int host_index,
                            const struct Halo *host) {
  const char *driver = ws->identity.driver != NULL ? ws->identity.driver : "(unnamed)";

  if (!ws->identity.fits) {
    ERROR_LOG("module_create_record refused for module '%s': the %s driver's created-record "
              "identity space does not fit int64 (units=%" PRId64 ", rows_per_unit=%" PRId64
              ", radix=%d), so this run cannot create records",
              module, driver, ws->identity.units, ws->identity.rows_per_unit,
              MAX_CREATED_RECORDS_PER_HOST);
    return -1;
  }

  ready_creation_ordinals(ws);
  const int ordinal = record_creation.ordinals[host_index];
  if (ordinal >= MAX_CREATED_RECORDS_PER_HOST) {
    ERROR_LOG("module_create_record refused for module '%s': host row %d (UniqueGalaxyID %lld) "
              "already has %d created records in this FoF step, the identity radix",
              module, host_index, host->UniqueGalaxyID, MAX_CREATED_RECORDS_PER_HOST);
    return -1;
  }

  /* The space is re-derived here rather than trusted from `fits`. A driver
   * that measures its space covers its own rows; where the vertical driver
   * falls back to the forest multiplier, the bound on HaloNr is the reader's
   * own forest-size guard, and a reader without one stops here too. The row
   * bounded is the host's global row in its unit, HaloNr + row_offset (the
   * comparison is written without the sum, so it cannot overflow). */
  const bool space_fits =
      mimic_created_record_space_fits(ws->identity.units, ws->identity.rows_per_unit);
  if (ws->identity.unit < 0 || ws->identity.unit >= ws->identity.units || host->HaloNr < 0 ||
      ws->identity.row_offset < 0 ||
      host->HaloNr >= ws->identity.rows_per_unit - ws->identity.row_offset || !space_fits) {
    FATAL_ERROR("module_create_record: host HaloNr=%lld (unit row offset %" PRId64
                ") at unit %" PRId64
                " cannot be encoded in the %s driver's published identity space (units=%" PRId64
                ", rows_per_unit=%" PRId64 ", fits int64: %s); the space must fit and cover "
                "every unit and global row (HaloNr + row offset) handed to modules "
                "(rows_per_unit is the largest unit, or the unique_galaxy_id_multiplier when a "
                "vertical reader cannot report it)",
                host->HaloNr, ws->identity.row_offset, ws->identity.unit, driver,
                ws->identity.units, ws->identity.rows_per_unit, space_fits ? "yes" : "no");
  }
  return ordinal;
}

/**
 * @brief   Staging step: copy the host into the next staging slot as its created record
 * @return  The record's future logical index; *@p row receives the staged row
 */
static int stage_created_record(const struct FoFWorkspace *ws, int host_index,
                                const struct Halo *host, int ordinal, struct Halo **row) {
  const int64_t k = record_creation.staged_count;
  const int index = narrow_int64_to_int_checked(ws->count + k, "created record index");
  reserve_staging_slot(k);

  struct Halo *staged = staged_row(k);
  *staged = *host;
  make_orphan(staged);
  staged->UniqueGalaxyID =
      mimic_encode_created_galaxy_id(ws->identity.unit, host->HaloNr + ws->identity.row_offset,
                                     ws->identity.rows_per_unit, ordinal);
  staged->galaxy = galaxy_pool_alloc(ws->pool);
  init_galaxy_defaults(staged->galaxy);

  record_creation.staged_host[k] = host_index;
  record_creation.ordinals[host_index] = ordinal + 1;
  record_creation.staged_count = k + 1;

  *row = staged;
  return index;
}

int module_create_record(struct ModuleContext *ctx, int host_index, struct Halo **row) {
  const char *module =
      running_callback.module_name != NULL ? running_callback.module_name : "(none running)";

  if (row != NULL) {
    *row = NULL;
  }
  if (!creation_caller_allowed(module, ctx, row)) {
    return -1;
  }

  const struct FoFWorkspace *ws = record_creation.ws;
  const struct Halo *host = creation_host(module, ws, host_index);
  if (host == NULL) {
    return -1;
  }

  const int ordinal = creation_ordinal(module, ws, host_index, host);
  if (ordinal < 0) {
    return -1;
  }
  return stage_created_record(ws, host_index, host, ordinal, row);
}

/**
 * @brief   Append the rows the returning full-halo callback staged
 *
 * Runs after the callback returns, before the next module and the phase's
 * safety dispatch of any undelivered event (events emitted during the callback
 * were delivered as they were emitted, against the committed rows): the rows
 * join the workspace tail in
 * creation order, their hosts are recorded in ws->created_host, and every
 * cache of the workspace's address or count the phase holds is refreshed
 * (ctx->central_galaxy and the event-dispatch view), since growing the rows
 * may move them.
 *
 * @return  true when rows were appended (ws->halos and ws->count may have
 *          changed), false when the callback staged none
 */
static bool commit_created_records(struct ModuleContext *ctx, struct FoFWorkspace *ws) {
  const int64_t staged = record_creation.staged_count;
  if (staged == 0) {
    return false;
  }

  const int64_t first = ws->count;
  fof_workspace_reserve(ws, first + staged);
  fof_workspace_reserve_created(ws, first + staged - ws->base_count);

  for (int64_t k = 0; k < staged; k++) {
    ws->halos[first + k] = *staged_row(k);
    ws->created_host[first + k - ws->base_count] = record_creation.staged_host[k];
  }
  ws->count = first + staged;
  record_creation.staged_count = 0;

  ctx->central_galaxy = &ws->halos[ctx->central_index];
  phase_event_state.halos = ws->halos;
  phase_event_state.ngal = workspace_callback_count(ws);
  return true;
}

/** @brief Start a FoF step: per-host ordinals count from zero again */
static void begin_record_creation_step(void) {
  if (record_creation.staged_count != 0) {
    FATAL_ERROR("Record creation: %" PRId64 " staged rows were never committed",
                record_creation.staged_count);
  }
  record_creation.ordinals_ready = false;
}

int64_t module_record_creation_staging_blocks(void) { return record_creation.block_count; }

void module_release_record_creation_scratch(void) {
  for (int64_t b = 0; b < record_creation.block_count; b++) {
    myfree(record_creation.blocks[b]);
    record_creation.blocks[b] = NULL;
  }
  myfree(record_creation.staged_host);
  myfree(record_creation.ordinals);

  record_creation.block_count = 0;
  record_creation.staged_host = NULL;
  record_creation.staged_host_capacity = 0;
  record_creation.staged_count = 0;
  record_creation.ordinals = NULL;
  record_creation.ordinal_capacity = 0;
  record_creation.ordinals_ready = false;
}

/* ── Discrete-phase ledger (module_registry.h) ─────────────────────────── */

/** Each conserved total over a workspace's active rows: its sum, magnitude and entry count */
struct TotalSums {
  double sum[RATE_STATS_TOTAL_SLOTS];
  double magnitude[RATE_STATS_TOTAL_SLOTS]; /* sum of |x| */
  int64_t entries[RATE_STATS_TOTAL_SLOTS];
};

/** @brief Sum every conserved total over the active rows, in workspace then table order */
static void sum_conserved_totals(const struct FoFWorkspace *ws, struct TotalSums *out) {
  memset(out, 0, sizeof(*out));
  const int num_entries = MIMIC_RESERVOIR_COUNT;
  const struct ReservoirEntry *table = mimic_reservoir_table();
  for (int64_t w = 0; w < ws->count; w++) {
    const struct GalaxyData *galaxy = ws->halos[w].galaxy;
    if (galaxy == NULL || ws->halos[w].Type == 3) {
      continue;
    }
    for (int e = 0; e < num_entries; e++) {
      const int t = table[e].total;
      if (t < 0) {
        continue; /* an observer belongs to no total */
      }
      const double value = *(const double *)(const void *)((const char *)galaxy + table[e].offset);
      out->sum[t] += value;
      out->magnitude[t] += fabs(value);
      out->entries[t]++;
    }
  }
}

/** @brief Comma-separated names of a plan list's modules, for a diagnostic */
static const char *module_list_names(struct Module *const *list, int count, char *buffer,
                                     size_t size) {
  size_t used = 0;
  buffer[0] = '\0';
  for (int i = 0; i < count && used < size; i++) {
    const int written =
        snprintf(buffer + used, size - used, "%s'%s'", i > 0 ? ", " : "", list[i]->name);
    if (written < 0) {
      break;
    }
    used += (size_t)written;
  }
  return buffer;
}

/**
 * @brief   Check one dispatch unit's change to every conserved total
 *
 * @param   unit     A full-halo unit's index in the plan, or plan->num_full_halo for the
 *                   by-galaxy pass
 *
 * Fatal on an undeclared change; counts a declared one in the run's statistics.
 */
static void check_discrete_unit(const struct PhasePlan *plan, int unit,
                                const struct TotalSums *before, const struct TotalSums *after,
                                const struct ModuleContext *ctx, const struct FoFWorkspace *ws) {
  const int num_totals = MIMIC_RESERVOIR_TOTAL_COUNT;
  const double unit_roundoff = 0x1p-53;
  discrete_ledger_units++;
  for (int t = 0; t < num_totals; t++) {
    const int64_t entries =
        before->entries[t] > after->entries[t] ? before->entries[t] : after->entries[t];
    const double magnitude = fmax(before->magnitude[t], after->magnitude[t]);
    const double tolerance = 8.0 * unit_roundoff * ((double)entries + 2.0) * magnitude;
    const double change = after->sum[t] - before->sum[t];
    if (fabs(change) <= tolerance) {
      continue; /* no change; a NaN fails this test and is a change */
    }
    if (plan->ledger_declares[(size_t)unit * (size_t)num_totals + (size_t)t]) {
      rate_statistics.counts.declared_changes[t]++;
      continue;
    }

    const char *total = mimic_reservoir_total_names()[t];
    const long long central_id = (ctx->central_index >= 0 && ctx->central_index < ws->count)
                                     ? ws->halos[ctx->central_index].UniqueGalaxyID
                                     : -1;
    ERROR_LOG("Discrete-phase ledger: phase '%s' changed conserved total '%s' from %.17g to "
              "%.17g (by %.17g; rounding tolerance %.3g) at snapshot %d, substep %d, in the FoF "
              "group of central UniqueGalaxyID %lld",
              plan->phase_name, total, before->sum[t], after->sum[t], change, tolerance,
              ctx->snapshot_number, ctx->substep_number, central_id);
    if (unit < plan->num_full_halo) {
      const struct Module *mod = plan->full_halo[unit];
      FATAL_ERROR("Undeclared change to conserved total '%s' in phase '%s' by module '%s' "
                  "(process_full_halo, with the per-event modules its events trigger): neither "
                  "'%s' nor a per-event module in the phase subscribed to its events lists '%s' "
                  "in changes_totals",
                  total, plan->phase_name, mod->name, mod->name, total);
    }
    char names[512];
    FATAL_ERROR("Undeclared change to conserved total '%s' in phase '%s' by its "
                "process_by_galaxy pass (modules %s): no by-galaxy module in the phase lists "
                "'%s' in changes_totals",
                total, plan->phase_name,
                module_list_names(plan->by_galaxy, plan->num_by_galaxy, names, sizeof(names)),
                total);
  }
}

/** @brief RateLawInvoker for the rate phase: one module's law, marked as the running callback */
static int invoke_rate_law(const struct Module *mod, const struct RateContext *ctx,
                           const struct Halo *halo, double *coefficients) {
  const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_RATE, mod->name);
  const int result = mod->rates(ctx, halo, coefficients);
  leave_callback(prior);
  return result;
}

/**
 * @brief   Execute modules in a specific phase
 *
 * Core execution engine for multi-phase pipeline:
 * 0. In the rate phase (its entries all PROCESSING_MODE_RATES, so it runs no
 *    other pass), the substep's rate segment: every component of the
 *    workspace integrated and written back (rate_assembly_run())
 * 1. PROCESSING_MODE_FULL_HALO modules, each followed by the commit of the
 *    records it created (commit_created_records())
 * 2. Event dispatch to PROCESSING_MODE_PER_EVENT modules
 * 3. PROCESSING_MODE_BY_GALAXY modules (galaxy-major loop)
 *
 * Each pass walks the phase's plan (built by module_system_init()), so no
 * entry's mode is tested here; the galaxy loop is entered only when the plan
 * has a by-galaxy entry, and the safety dispatch runs only for pending events.
 *
 * When a rate phase is configured, every other phase also runs the
 * discrete-phase ledger (module_registry.h): each conserved total is summed
 * before and after each full-halo unit (the call, the per-event consumers its
 * events trigger, its commit and the safety dispatch) and around the by-galaxy
 * pass as a whole, and a change its declarations do not cover is fatal. With no
 * rate phase the plan carries no declarations and the hook is not entered.
 *
 * @param   phase_config   Array of module configurations for this phase
 * @param   num_modules    Number of modules in this phase (0 = skip)
 * @param   ctx            Module execution context
 * @param   ws             FoF workspace; the full-halo pass reads its halos and
 *                         count before its first callback and again after every
 *                         commit of created records, the only point at which the
 *                         workspace changes during a phase; the by-galaxy pass
 *                         reads them at every callback
 */
void execute_phase(struct PhaseModuleConfig *phase_config, int num_modules,
                   struct ModuleContext *ctx, struct FoFWorkspace *ws) {
  if (num_modules == 0) {
    return; /* Empty phase */
  }

  const struct PhasePlan *plan = find_phase_plan(phase_config, num_modules);
  if (plan == NULL) {
    FATAL_ERROR("Phase whose first entry is module '%s' (%d module(s)) has no dispatch plan — "
                "module_system_init() must run before execute_phase(), and the phase must not "
                "change after it",
                phase_config[0].module_name, num_modules);
  }

  if (ctx == NULL || ws == NULL || ws->halos == NULL || ws->count <= 0) {
    return; /* Nothing to process */
  }

  begin_phase_event_dispatch(plan, ctx, ws->halos, workspace_callback_count(ws));

  /* PASS 0: the rate segment, inside the dispatch window; emission stays refused
   * (module_emit_event() rejects a running rate callback), and creation too
   * (module_create_record() accepts only a running full-halo callback). */
  if (plan->num_rates > 0) {
    if (rate_plan == NULL || rate_phase_config != phase_config) {
      FATAL_ERROR("Rate phase whose first entry is module '%s' has no rate plan — "
                  "module_system_init() must build it before execute_phase()",
                  phase_config[0].module_name);
    }
    rate_assembly_run(rate_plan, ctx, ws, &rate_scratch, invoke_rate_law, &rate_statistics);
  }

  /* The discrete-phase ledger runs only when a rate phase is configured, and never in it */
  const bool ledger = plan->ledger_declares != NULL;
  struct TotalSums before;
  struct TotalSums after;

  /* PASS 1: PROCESSING_MODE_FULL_HALO modules (always first) */
  struct Halo *halos = ws->halos;
  int ngal = workspace_callback_count(ws);
  for (int i = 0; i < plan->num_full_halo; i++) {
    struct Module *mod = plan->full_halo[i];
    if (ledger) {
      sum_conserved_totals(ws, &before);
    }

    DEBUG_LOG("Executing module: %s (full array, ngal=%d, substep %d/%d, z=%.3f)", mod->name, ngal,
              ctx->substep_number + 1, ctx->num_substeps, ctx->redshift);

    ctx->active_event = NULL;
    phase_event_state.current_producer_module_id = mod->module_id;
    phase_event_state.emission_allowed = true;
    record_creation.ctx = ctx;
    record_creation.ws = ws;
    const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_FULL_HALO, mod->name);
    int result = mod->process(ctx, halos, ngal);
    leave_callback(prior);
    phase_event_state.emission_allowed = false;
    phase_event_state.current_producer_module_id = 0;

    if (result != 0) {
      FATAL_ERROR("Module '%s' failed (substep %d)", mod->name, ctx->substep_number);
    }

    /* Records the callback created join the workspace now, before the safety
     * dispatch below and before the next module runs. */
    if (commit_created_records(ctx, ws)) {
      /* A commit is the only mid-phase change to ws->halos or ws->count, so
       * re-reading here keeps every later callback on the current rows. */
      halos = ws->halos;
      ngal = workspace_callback_count(ws);
    }

    /* Safety dispatch for any events not yet dispatched during emission. */
    if (phase_event_state.last_dispatched_event < phase_event_state.event_count) {
      dispatch_events_range(phase_event_state.last_dispatched_event, phase_event_state.event_count);
    }

    /* The unit ends here: the call, its consumers, its commit and the safety dispatch. */
    if (ledger) {
      sum_conserved_totals(ws, &after);
      check_discrete_unit(plan, i, &before, &after, ctx, ws);
    }
  }

  /* PASS 2: PROCESSING_MODE_BY_GALAXY modules (after full-halo/event work); entered only
   * when the phase has a by-galaxy entry, and checked by the ledger as one unit */
  const bool by_galaxy_ledger = ledger && plan->num_by_galaxy > 0;
  if (by_galaxy_ledger) {
    sum_conserved_totals(ws, &before);
  }
  for (int64_t g = 0; plan->num_by_galaxy > 0 && g < ws->count; g++) {
    if (ws->halos[g].galaxy == NULL || ws->halos[g].Type == 3) {
      continue;
    }

    for (int i = 0; i < plan->num_by_galaxy; i++) {
      struct Module *mod = plan->by_galaxy[i];

      DEBUG_LOG("Executing module: %s (galaxy %" PRId64 "/%" PRId64 ", substep %d/%d, z=%.3f)",
                mod->name, g, ws->count, ctx->substep_number + 1, ctx->num_substeps, ctx->redshift);

      ctx->active_event = NULL;
      const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_BY_GALAXY, mod->name);
      int result = mod->process(ctx, &ws->halos[g], 1);
      leave_callback(prior);
      if (result != 0) {
        FATAL_ERROR("Module '%s' failed on galaxy %" PRId64 " (substep %d)", mod->name, g,
                    ctx->substep_number);
      }
    }
  }
  if (by_galaxy_ledger) {
    sum_conserved_totals(ws, &after);
    check_discrete_unit(plan, plan->num_full_halo, &before, &after, ctx, ws);
  }

  end_phase_event_dispatch();
}

void execute_post_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                           int64_t count) {
  if (ctx == NULL || ctx->params == NULL) {
    FATAL_ERROR("execute_post_snapshot called without a snapshot context");
  }
  if (count < 0 || (count > 0 && halos == NULL)) {
    FATAL_ERROR("execute_post_snapshot called with an invalid population at snapshot %d "
                "(count=%" PRId64 ", halos=%p)",
                ctx->snapshot_number, count, (const void *)halos);
  }
  if (snapshot_dispatch_active || phase_event_state.active) {
    FATAL_ERROR("execute_post_snapshot called at snapshot %d while another phase dispatch is "
                "active",
                ctx->snapshot_number);
  }

  const struct PhaseModuleConfig *phase = ctx->params->post_snapshot;
  const int num_modules = ctx->params->num_post_snapshot;

  /* No count shortcut: an empty snapshot is a real call of every entry. */
  for (int i = 0; i < num_modules; i++) {
    const struct Module *mod = phase[i].resolved;
    if (mod == NULL || mod->process_snapshot == NULL) {
      FATAL_ERROR("Module '%s' in phase '%s' was not resolved to a process_snapshot callback "
                  "— module_system_init() must run before execute_post_snapshot()",
                  phase[i].module_name, POST_SNAPSHOT_PHASE_NAME);
    }

    DEBUG_LOG("Executing module: %s (snapshot %d, count=%" PRId64 ", z=%.3f)", mod->name,
              ctx->snapshot_number, count, ctx->redshift);

    snapshot_dispatch_active = true;
    const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_SNAPSHOT, mod->name);
    const int result = mod->process_snapshot(ctx, halos, count);
    leave_callback(prior);
    snapshot_dispatch_active = false;

    if (result != 0) {
      FATAL_ERROR("Module '%s' failed in phase '%s' at snapshot %d with return code %d", mod->name,
                  POST_SNAPSHOT_PHASE_NAME, ctx->snapshot_number, result);
    }
  }
}

/**
 * @brief   Update context for a specific substep
 *
 * @param   ctx     Module context to update
 * @param   step    Current substep number (0-indexed)
 */
static void update_context_for_substep(struct ModuleContext *ctx, int step) {
  ctx->substep_number = step;
  /* Interpolate from progenitor snapshot age toward current snapshot age. */
  const double progenitor_age = ctx->time + ctx->time_interval;
  ctx->substep_time = progenitor_age - (step + 0.5) * ctx->substep_dt;
}

/**
 * @brief   Run the full configured module lifecycle over a halo workspace
 *
 * Implementation of the format-neutral physics-execution engine; see
 * module_registry.h for the full driver-neutral contract. Executes, in order:
 * the pre-timestep phase once, then each user-named substep phase per substep,
 * then the post-timestep phase once. Phase configuration is read from the
 * caller-supplied context (@p ctx->params), not from a global, so the engine
 * makes no assumptions about which driver populated it.
 *
 * @param   ctx     Module execution context (already populated by the caller)
 * @param   ws      FoF workspace to evolve; every phase reads it afresh
 */
void execute_module_pipeline(struct ModuleContext *ctx, struct FoFWorkspace *ws) {
  const struct MimicConfig *config = ctx->params;

  /* One pipeline run is one FoF step for created-record ordinals. */
  begin_record_creation_step();

  /* Pre-timestep phase (runs once before substeps) */
  execute_phase(config->pre_timestep, config->num_pre_timestep, ctx, ws);

  /* Substep loop: each user-named phase runs once per substep, in order */
  for (int step = 0; step < ctx->num_substeps; step++) {
    update_context_for_substep(ctx, step);

    for (int p = 0; p < config->num_substep_phases; p++) {
      execute_phase(config->substep_phases[p].modules, config->substep_phases[p].num_modules, ctx,
                    ws);
    }
  }

  /* Post-timestep phase (runs once after substeps) */
  execute_phase(config->post_timestep, config->num_post_timestep, ctx, ws);
}

/**
 * @brief   Cleanup the module system
 *
 * Calls cleanup() on all initialized modules in reverse order.
 *
 * @return  0 on success, non-zero if any module cleanup fails
 */
static void free_phase_configuration(void) {
  /* Before the phase arrays the plans are keyed by */
  release_phase_plans();

  if (MimicConfig.pre_timestep) {
    for (int i = 0; i < MimicConfig.num_pre_timestep; i++) {
      if (MimicConfig.pre_timestep[i].module_name) {
        free((void *)MimicConfig.pre_timestep[i].module_name);
      }
    }
    myfree(MimicConfig.pre_timestep);
    MimicConfig.pre_timestep = NULL;
  }
  MimicConfig.num_pre_timestep = 0;

  if (MimicConfig.substep_phases) {
    for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
      struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
      if (phase->modules) {
        for (int i = 0; i < phase->num_modules; i++) {
          if (phase->modules[i].module_name) {
            free((void *)phase->modules[i].module_name);
          }
        }
        myfree(phase->modules);
      }
      if (phase->name) {
        free(phase->name);
      }
    }
    myfree(MimicConfig.substep_phases);
    MimicConfig.substep_phases = NULL;
  }
  MimicConfig.num_substep_phases = 0;

  if (MimicConfig.post_timestep) {
    for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
      if (MimicConfig.post_timestep[i].module_name) {
        free((void *)MimicConfig.post_timestep[i].module_name);
      }
    }
    myfree(MimicConfig.post_timestep);
    MimicConfig.post_timestep = NULL;
  }
  MimicConfig.num_post_timestep = 0;

  if (MimicConfig.post_snapshot) {
    for (int i = 0; i < MimicConfig.num_post_snapshot; i++) {
      if (MimicConfig.post_snapshot[i].module_name) {
        free((void *)MimicConfig.post_snapshot[i].module_name);
      }
    }
    myfree(MimicConfig.post_snapshot);
    MimicConfig.post_snapshot = NULL;
  }
  MimicConfig.num_post_snapshot = 0;
}

int module_system_pipeline_count(void) { return num_pipeline_modules; }

int module_system_cleanup(void) {
  int result = 0;

  /* Released here, not at phase end: the event buffer is shared by every
   * execute_phase() call, so freeing it per phase would reintroduce the
   * hot-path allocation churn the grow-to-high-water design exists to avoid.
   * The rate scratch follows the same rule, and its plan goes with it. */
  free_phase_event_buffer();
  release_rate_phase();

  if (num_pipeline_modules == 0) {
    free_phase_configuration();
    VERBOSE_LOG("Module system cleanup complete (no modules were enabled)");
    return 0;
  }

  VERBOSE_LOG("Cleaning up %d module(s)", num_pipeline_modules);

  for (int i = num_pipeline_modules - 1; i >= 0; i--) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Cleaning up module: %s", mod->name);

    const struct RunningCallback prior = enter_callback(RUNNING_CALLBACK_CLEANUP, mod->name);
    int cleanup_result = mod->cleanup();
    leave_callback(prior);
    if (cleanup_result != 0) {
      ERROR_LOG("Module '%s' cleanup failed with code %d", mod->name, cleanup_result);
      result = cleanup_result;
    }
    execution_pipeline[i] = NULL;
  }

  free_phase_configuration();
  num_pipeline_modules = 0;

  VERBOSE_LOG("Module system cleanup complete");
  return result;
}

/* ==============================================================================
 * EVENT CONTRACT ENUMERATION
 * ============================================================================== */

/**
 * @brief   Emit event contracts for one phase's per-event consumers.
 */
static void enumerate_phase_event_contracts(const char *phase_name,
                                            const struct PhaseModuleConfig *phase, int num_modules,
                                            EventContractCallback cb, void *userdata) {
  if (phase == NULL) {
    return;
  }
  for (int i = 0; i < num_modules; i++) {
    if (phase[i].processing_mode != PROCESSING_MODE_PER_EVENT) {
      continue;
    }
    const char *consumer_name = phase[i].module_name;
    struct Module *consumer = find_module_by_name(consumer_name);
    if (consumer == NULL || consumer->num_subscriptions == 0) {
      continue;
    }
    for (int s = 0; s < consumer->num_subscriptions; s++) {
      const struct EventSubscription *sub = &consumer->subscriptions[s];
      cb(phase_name, consumer_name, sub->producer_name, sub->event_name, sub->event_id, userdata);
    }
  }
}

struct ContractEnumState {
  EventContractCallback cb;
  void *userdata;
};

static void enumerate_contracts_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                        int num_modules, void *userdata) {
  struct ContractEnumState *state = userdata;
  enumerate_phase_event_contracts(phase_name, modules, num_modules, state->cb, state->userdata);
}

void module_system_enumerate_event_contracts(EventContractCallback cb, void *userdata) {
  if (cb == NULL) {
    return;
  }

  struct ContractEnumState state = {cb, userdata};
  for_each_phase(enumerate_contracts_visitor, &state);
}

/* ==============================================================================
 * MODEL PARAMETER ACCESS
 * ============================================================================== */

/**
 * @brief   Helper: Parse double value with strict validation
 *
 * @param   str          String to parse
 * @param   out          Output double value
 * @param   param_name   Parameter name (for error messages)
 * @return  0 on success, -1 on parse error
 */
static int parse_double_strict(const char *str, double *out, const char *param_name) {
  char *endptr;
  errno = 0;
  *out = strtod(str, &endptr);

  if (errno != 0 || endptr == str || *endptr != '\0') {
    ERROR_LOG("Parameter '%s': invalid double value '%s'", param_name, str);
    return -1;
  }

  /* Same refusal as the run-file top-level doubles: a nan or inf parameter poisons the physics */
  if (!isfinite(*out)) {
    ERROR_LOG("Parameter '%s': non-finite double value '%s'", param_name, str);
    return -1;
  }
  return 0;
}

/**
 * @brief   Helper: Parse int value with strict validation
 *
 * @param   str          String to parse
 * @param   out          Output int value
 * @param   param_name   Parameter name (for error messages)
 * @return  0 on success, -1 on parse error
 */
static int parse_int_strict(const char *str, int *out, const char *param_name) {
  char *endptr;
  errno = 0;
  long val = strtol(str, &endptr, 10);

  if (errno != 0 || endptr == str || *endptr != '\0') {
    ERROR_LOG("Parameter '%s': invalid int value '%s'", param_name, str);
    return -1;
  }

  if (val < INT_MIN || val > INT_MAX) {
    ERROR_LOG("Parameter '%s': value %ld out of int range", param_name, val);
    return -1;
  }

  *out = (int)val;
  return 0;
}

/**
 * @brief   Get required model parameter as double
 *
 * Model parameters MUST be specified in the input YAML file.
 * NO defaults are used - parameters are REQUIRED.
 *
 * This enforces explicit model specification for reproducible science.
 *
 * Single Source of Truth (VISION.md §3): Input file is the truth for values.
 *
 * @param   param_name   Parameter name (e.g., "BaryonFrac")
 * @param   out_value    Output pointer for double value
 * @return  0 on success, -1 if parameter missing or invalid
 */
int model_get_double(const char *param_name, double *out_value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      return parse_double_strict(MimicConfig.ModelParams[i].value, out_value, param_name);
    }
  }

  ERROR_LOG("Required model parameter '%s' not found in input file", param_name);
  return -1;
}

int model_get_double_internal(const char *param_name, double *out_value) {
  if (model_get_double(param_name, out_value) != 0) {
    return -1;
  }
  *out_value *= mimic_parameter_unit_factor(param_name);
  return 0;
}

/**
 * @brief   Get required model parameter as integer
 *
 * Model parameters MUST be specified in the input YAML file.
 * NO defaults are used - parameters are REQUIRED.
 *
 * Single Source of Truth (VISION.md §3): Input file is the truth for values.
 *
 * @param   param_name   Parameter name (e.g., "AGNrecipe")
 * @param   out_value    Output pointer for integer value
 * @return  0 on success, -1 if parameter missing or invalid
 */
int model_get_int(const char *param_name, int *out_value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      return parse_int_strict(MimicConfig.ModelParams[i].value, out_value, param_name);
    }
  }

  ERROR_LOG("Required model parameter '%s' not found in input file", param_name);
  return -1;
}

bool model_has_param(const char *param_name) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief   Get required model parameter as string
 *
 * Model parameters MUST be specified in the input YAML file.
 * NO defaults are used - parameters are REQUIRED.
 *
 * Single Source of Truth (VISION.md §3): Input file is the truth for values.
 *
 * @param   param_name   Parameter name
 * @param   out_value    Output buffer for string value
 * @param   max_len      Maximum length of output buffer
 * @return  0 on success, -1 if parameter missing
 */
int model_get_string(const char *param_name, char *out_value, size_t max_len) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      strncpy(out_value, MimicConfig.ModelParams[i].value, max_len - 1);
      out_value[max_len - 1] = '\0';
      return 0;
    }
  }

  ERROR_LOG("Required model parameter '%s' not found in input file", param_name);
  return -1;
}
