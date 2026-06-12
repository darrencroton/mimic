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
 * Vision Principle 1 (Physics-Agnostic Core): The registry treats all modules
 * identically through the Module interface, with zero knowledge of specific
 * physics implementations.
 *
 * Vision Principle 2 (Runtime Modularity): Module selection is configurable
 * at runtime without recompilation.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "globals.h"
#include "memory.h"
#include "module_interface.h"
#include "module_registry.h"

/** Maximum number of modules that can be registered */
#define MAX_MODULES 32

/** Maximum number of phase-local events buffered before hard failure */
#define MAX_PHASE_EVENTS 4096

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
 */
struct PhaseEventDispatchState {
  struct ModuleEvent events[MAX_PHASE_EVENTS];
  int event_count;
  int last_dispatched_event;
  struct PhaseModuleConfig *phase_config;
  int num_modules;
  struct ModuleContext *ctx;
  struct Halo *halos;
  int ngal;
  bool active;
  bool emission_allowed;
  int current_producer_module_id; /**< module_id of the currently executing producer */
};

static struct PhaseEventDispatchState phase_event_state = {.event_count = 0,
                                                           .last_dispatched_event = 0,
                                                           .phase_config = NULL,
                                                           .num_modules = 0,
                                                           .ctx = NULL,
                                                           .halos = NULL,
                                                           .ngal = 0,
                                                           .active = false,
                                                           .emission_allowed = false,
                                                           .current_producer_module_id = 0};

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

  if (module->init == NULL || module->process == NULL || module->cleanup == NULL) {
    FATAL_ERROR("Module '%s' has NULL function pointers", module->name);
  }

  // Check for duplicate names
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
 * @param   module_name  Name of module to add
 * @return  The registered module (never NULL; fatal if unregistered)
 */
static struct Module *add_module_to_pipeline(const char *module_name) {
  /* Check if already in pipeline */
  for (int i = 0; i < num_pipeline_modules; i++) {
    if (strcmp(execution_pipeline[i]->name, module_name) == 0) {
      return execution_pipeline[i]; /* Already added */
    }
  }

  /* Find module in registry */
  struct Module *mod = find_module_by_name(module_name);
  if (mod == NULL) {
    ERROR_LOG("Module '%s' configured but not registered", module_name);
    ERROR_LOG("Available modules:");
    for (int j = 0; j < num_registered_modules; j++) {
      ERROR_LOG("  - %s", registered_modules[j]->name);
    }
    FATAL_ERROR("Unknown module '%s' in pipeline configuration", module_name);
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

const char *processing_mode_to_string(enum ProcessingMode mode) {
  switch (mode) {
  case PROCESSING_MODE_FULL_HALO:
    return "process_full_halo";
  case PROCESSING_MODE_PER_EVENT:
    return "process_per_event";
  case PROCESSING_MODE_BY_GALAXY:
    return "process_by_galaxy";
  default:
    return "unknown";
  }
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

/**
 * @brief   Validate phase configuration against module constraints
 *
 * Ensures that each module in the phase is configured with a processing mode it
 * actually supports. Fails hard with clear error messages if mismatch detected.
 *
 * @param   config       Phase module configuration array
 * @param   num_modules  Number of modules in phase
 * @param   phase_name   Phase name for error messages
 * @return  0 on success, -1 on validation failure
 */
static int validate_phase_processing_modes(struct PhaseModuleConfig *config, int num_modules,
                                           const char *phase_name) {
  for (int i = 0; i < num_modules; i++) {
    /* Find registered module */
    struct Module *mod = find_module_by_name(config[i].module_name);
    if (mod == NULL) {
      /* Module not found - handled elsewhere in add_module_to_pipeline */
      continue;
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

/* ── for_each_phase visitors used by module_system_init ─────────────────── */

static void build_pipeline_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                   int num_modules, void *userdata) {
  (void)phase_name;
  (void)userdata;
  for (int i = 0; i < num_modules; i++) {
    modules[i].resolved = add_module_to_pipeline(modules[i].module_name);
  }
}

static void log_phase_size_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                   int num_modules, void *userdata) {
  (void)modules;
  (void)userdata;
  INFO_LOG("  %s: %d module(s)", phase_name, num_modules);
}

static void validate_modes_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                   int num_modules, void *userdata) {
  if (validate_phase_processing_modes(modules, num_modules, phase_name) != 0) {
    *(int *)userdata = 1;
  }
}

static void validate_events_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                    int num_modules, void *userdata) {
  if (validate_event_subscriptions(modules, num_modules, phase_name) != 0) {
    *(int *)userdata = 1;
  }
}

/**
 * @brief   Initialize the module system
 *
 * Validates multi-phase pipeline configuration and initializes all referenced
 * modules. Modules are initialized in the order they appear across all phases
 * (pre_timestep, named substep phases, post_timestep), with duplicates
 * initialized only once.
 *
 * @return  0 on success, non-zero if initialization fails
 */
int module_system_init(void) {
  int validation_failed = 0;

  INFO_LOG("Initializing multi-phase module system");

  /* Build execution pipeline by collecting all unique modules across phases */
  num_pipeline_modules = 0;
  for_each_phase(build_pipeline_visitor, NULL);

  if (num_pipeline_modules == 0) {
    INFO_LOG("No modules configured (physics-free mode)");
    INFO_LOG("SubSteps = %d", MimicConfig.SubSteps);
    return 0;
  }

  INFO_LOG("Pipeline configuration:");
  INFO_LOG("  SubSteps: %d", MimicConfig.SubSteps);
  for_each_phase(log_phase_size_visitor, NULL);
  INFO_LOG("  Total unique modules: %d", num_pipeline_modules);

  /* Validate processing mode configurations */
  INFO_LOG("Validating module processing mode configurations...");
  for_each_phase(validate_modes_visitor, &validation_failed);
  if (validation_failed) {
    return -1;
  }
  INFO_LOG("Processing mode validation passed");

  /* Validate event subscription contracts for each phase */
  INFO_LOG("Validating event subscription contracts...");
  for_each_phase(validate_events_visitor, &validation_failed);
  if (validation_failed) {
    return -1;
  }
  INFO_LOG("Event subscription validation passed");

  /* Initialize all modules in pipeline order */
  for (int i = 0; i < num_pipeline_modules; i++) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Initializing module: %s", mod->name);

    int result = mod->init();
    if (result != 0) {
      ERROR_LOG("Module '%s' initialization failed with code %d", mod->name, result);
      return result;
    }
  }

  INFO_LOG("Module system initialized successfully");
  return 0;
}

/**
 * @brief   Initialize per-phase event dispatch state
 */
static void begin_phase_event_dispatch(struct PhaseModuleConfig *phase_config, int num_modules,
                                       struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  phase_event_state.event_count = 0;
  phase_event_state.last_dispatched_event = 0;
  phase_event_state.phase_config = phase_config;
  phase_event_state.num_modules = num_modules;
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
  phase_event_state.phase_config = NULL;
  phase_event_state.num_modules = 0;
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

    for (int i = 0; i < phase_event_state.num_modules; i++) {
      if (phase_event_state.phase_config[i].processing_mode != PROCESSING_MODE_PER_EVENT) {
        continue;
      }

      struct Module *mod = phase_event_state.phase_config[i].resolved;
      if (mod == NULL) {
        FATAL_ERROR("Module '%s' was not resolved — module_system_init() must run "
                    "before event dispatch",
                    phase_event_state.phase_config[i].module_name);
      }

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

      int result = mod->process(phase_event_state.ctx, target_halo, 1);
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
  if (ctx == NULL) {
    ERROR_LOG("module_emit_event called with NULL context");
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

  if (source_index < 0 || source_index >= phase_event_state.ngal) {
    ERROR_LOG("module_emit_event invalid source_index=%d (ngal=%d)", source_index,
              phase_event_state.ngal);
    return -1;
  }

  if (target_index < 0 || target_index >= phase_event_state.ngal) {
    ERROR_LOG("module_emit_event invalid target_index=%d (ngal=%d)", target_index,
              phase_event_state.ngal);
    return -1;
  }

  if (phase_event_state.event_count >= MAX_PHASE_EVENTS) {
    ERROR_LOG("Phase event buffer overflow: emitted %d events (max %d)",
              phase_event_state.event_count, MAX_PHASE_EVENTS);
    return -1;
  }

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

/**
 * @brief   Execute modules in a specific phase
 *
 * Core execution engine for multi-phase pipeline:
 * 1. PROCESSING_MODE_FULL_HALO modules
 * 2. Event dispatch to PROCESSING_MODE_PER_EVENT modules
 * 3. PROCESSING_MODE_BY_GALAXY modules (galaxy-major loop)
 *
 * @param   phase_config   Array of module configurations for this phase
 * @param   num_modules    Number of modules in this phase (0 = skip)
 * @param   ctx            Module execution context
 * @param   halos          Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal           Number of halos in the array
 */
void execute_phase(struct PhaseModuleConfig *phase_config, int num_modules,
                   struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (num_modules == 0 || ctx == NULL || halos == NULL || ngal <= 0) {
    return; /* Empty phase or nothing to process */
  }

  begin_phase_event_dispatch(phase_config, num_modules, ctx, halos, ngal);

  /* PASS 1: PROCESSING_MODE_FULL_HALO modules (always first) */
  for (int i = 0; i < num_modules; i++) {
    if (phase_config[i].processing_mode != PROCESSING_MODE_FULL_HALO) {
      continue;
    }

    struct Module *mod = phase_config[i].resolved;
    if (mod == NULL) {
      FATAL_ERROR("Module '%s' was not resolved — module_system_init() must run "
                  "before execute_phase()",
                  phase_config[i].module_name);
    }

    DEBUG_LOG("Executing module: %s (full array, ngal=%d, substep %d/%d, z=%.3f)", mod->name, ngal,
              ctx->substep_number + 1, ctx->num_substeps, ctx->redshift);

    ctx->active_event = NULL;
    phase_event_state.current_producer_module_id = mod->module_id;
    phase_event_state.emission_allowed = true;
    int result = mod->process(ctx, halos, ngal);
    phase_event_state.emission_allowed = false;
    phase_event_state.current_producer_module_id = 0;

    if (result != 0) {
      FATAL_ERROR("Module '%s' failed (substep %d)", mod->name, ctx->substep_number);
    }

    /* Safety dispatch for any events not yet dispatched during emission. */
    dispatch_events_range(phase_event_state.last_dispatched_event, phase_event_state.event_count);
  }

  /* PASS 2: PROCESSING_MODE_BY_GALAXY modules (after full-halo/event work) */
  for (int g = 0; g < ngal; g++) {
    if (halos[g].galaxy == NULL || halos[g].Type == 3) {
      continue;
    }

    for (int i = 0; i < num_modules; i++) {
      if (phase_config[i].processing_mode != PROCESSING_MODE_BY_GALAXY) {
        continue;
      }

      struct Module *mod = phase_config[i].resolved;
      if (mod == NULL) {
        FATAL_ERROR("Module '%s' was not resolved — module_system_init() must run "
                    "before execute_phase()",
                    phase_config[i].module_name);
      }

      DEBUG_LOG("Executing module: %s (galaxy %d/%d, substep %d/%d, z=%.3f)", mod->name, g, ngal,
                ctx->substep_number + 1, ctx->num_substeps, ctx->redshift);

      ctx->active_event = NULL;
      int result = mod->process(ctx, &halos[g], 1);
      if (result != 0) {
        FATAL_ERROR("Module '%s' failed on galaxy %d (substep %d)", mod->name, g,
                    ctx->substep_number);
      }
    }
  }

  end_phase_event_dispatch();
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
 * @param   halos   Array of halos to evolve (e.g. FoFWorkspace)
 * @param   ngal    Number of halos in the array
 */
void execute_module_pipeline(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  const struct MimicConfig *config = ctx->params;

  /* Pre-timestep phase (runs once before substeps) */
  execute_phase(config->pre_timestep, config->num_pre_timestep, ctx, halos, ngal);

  /* Substep loop: each user-named phase runs once per substep, in order */
  for (int step = 0; step < ctx->num_substeps; step++) {
    update_context_for_substep(ctx, step);

    for (int p = 0; p < config->num_substep_phases; p++) {
      execute_phase(config->substep_phases[p].modules, config->substep_phases[p].num_modules, ctx,
                    halos, ngal);
    }
  }

  /* Post-timestep phase (runs once after substeps) */
  execute_phase(config->post_timestep, config->num_post_timestep, ctx, halos, ngal);
}

/**
 * @brief   Cleanup the module system
 *
 * Calls cleanup() on all initialized modules in reverse order.
 *
 * @return  0 on success, non-zero if any module cleanup fails
 */
static void free_phase_configuration(void) {
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
}

int module_system_cleanup(void) {
  int result = 0;

  if (num_pipeline_modules == 0) {
    free_phase_configuration();
    INFO_LOG("Module system cleanup complete (no modules were enabled)");
    return 0;
  }

  INFO_LOG("Cleaning up %d module(s)", num_pipeline_modules);

  // Cleanup in reverse order
  for (int i = num_pipeline_modules - 1; i >= 0; i--) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Cleaning up module: %s", mod->name);

    int cleanup_result = mod->cleanup();
    if (cleanup_result != 0) {
      ERROR_LOG("Module '%s' cleanup failed with code %d", mod->name, cleanup_result);
      result = cleanup_result; // Continue cleanup but record failure
    }
    execution_pipeline[i] = NULL;
  }

  free_phase_configuration();
  num_pipeline_modules = 0;

  INFO_LOG("Module system cleanup complete");
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
 * Vision Principle 4 (Single Source of Truth): Input file is the truth for values.
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

/**
 * @brief   Get required model parameter as integer
 *
 * Model parameters MUST be specified in the input YAML file.
 * NO defaults are used - parameters are REQUIRED.
 *
 * Vision Principle 4 (Single Source of Truth): Input file is the truth for values.
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

/**
 * @brief   Get required model parameter as string
 *
 * Model parameters MUST be specified in the input YAML file.
 * NO defaults are used - parameters are REQUIRED.
 *
 * Vision Principle 4 (Single Source of Truth): Input file is the truth for values.
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
