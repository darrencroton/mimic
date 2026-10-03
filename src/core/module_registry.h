/**
 * @file    module_registry.h
 * @brief   Module registration and multi-phase execution pipeline
 *
 * This file provides the interface for registering galaxy physics modules
 * and executing them in a multi-phase pipeline with optional time sub-stepping.
 *
 * Physics-Agnostic Core (VISION.md §1): The registry manages modules without
 * knowing anything about their specific physics implementations.
 *
 * Runtime Modularity (VISION.md §2): Pipeline structure is configured at
 * runtime via input YAML file, not hardcoded in module metadata.
 *
 * Multi-Phase Pipeline:
 * - Pre-timestep phase (once before substeps)
 * - User-named substep phases (each substep, in input-YAML order)
 * - Post-timestep phase (once after substeps)
 * - Post-snapshot phase (horizontal driver only: once per snapshot, after
 *   every FoF group of the snapshot, over its whole processed population)
 *
 * Usage:
 * 1. Modules call module_registry_add() to register themselves (at startup)
 * 2. Main program calls module_system_init() after parameter reading
 * 3. Tree processing calls execute_phase() for each FoF phase, and the
 *    horizontal driver calls execute_post_snapshot() once per snapshot
 * 4. Main program calls module_system_cleanup() before exit
 */

#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h> /* for size_t */
#include <stdint.h> /* for int64_t */

#include "module_interface.h"

/**
 * @brief   Phase module configuration entry
 *
 * Specifies one module's role in one phase. The input YAML file contains
 * lists of these for each phase, defining the complete execution pipeline.
 *
 * Example YAML:
 *   modules:
 *     phases:
 *       galaxy_physics:
 *         - sage_calculate_cooling: process_by_galaxy
 *         - sage_radio_mode_heating: process_by_galaxy
 *         - sage_add_cooling: process_by_galaxy
 *
 * This creates three PhaseModuleConfig entries for the named substep phase.
 */
struct PhaseModuleConfig {
  char *module_name;                   /**< Module name (must match registered module) */
  enum ProcessingMode processing_mode; /**< How to call module */
  struct Module *resolved;             /**< Registered module, resolved once by
                                            module_system_init() so execution never
                                            looks modules up by name on the hot path */
};

/** Configuration and provenance name of the snapshot-wide phase (modules.post_snapshot) */
#define POST_SNAPSHOT_PHASE_NAME "post_snapshot"

/** Maximum number of user-named substep middle phases per run */
#define MAX_SUBSTEP_PHASES 32

/**
 * @brief   One user-named substep middle phase
 *
 * The fixed lifecycle phases (pre_timestep, post_timestep) bracket an ordered
 * list of these middle phases. Each runs once per substep, in input order. The
 * name is user-facing configuration (e.g. "galaxy_physics", "satellite_mergers")
 * and is recorded in run provenance so the physical pipeline is recoverable
 * from outputs. Legacy top-level phase_1/phase_2 inputs are rejected by the
 * parser rather than translated.
 */
struct ModulePhaseConfig {
  char *name;                        /**< User-facing phase name (heap-owned) */
  struct PhaseModuleConfig *modules; /**< Modules configured in this phase */
  int num_modules;                   /**< Number of modules in this phase */
};

/**
 * @brief   Visitor callback for iterating all configured phases in execution order
 *
 * @param   phase_name   Phase name ("pre_timestep", user-named, "post_timestep",
 *                       "post_snapshot")
 * @param   modules      Phase module configuration array (may be NULL when empty)
 * @param   num_modules  Number of entries in the array
 * @param   userdata     Caller-supplied context pointer
 */
typedef void (*PhaseVisitor)(const char *phase_name, struct PhaseModuleConfig *modules,
                             int num_modules, void *userdata);

/**
 * @brief   Visit every configured phase (pre_timestep, each substep phase in
 *          input order, post_timestep, then post_snapshot) exactly once
 *
 * Single home for the phase-iteration pattern shared by pipeline build,
 * validation, contract enumeration, and output metadata. pre_timestep and
 * post_timestep are visited even when empty; post_snapshot is visited only
 * when it has entries, so a run without snapshot modules visits exactly the
 * phases (and produces exactly the pipeline and provenance) it did before
 * the phase existed.
 */
void for_each_phase(PhaseVisitor visit, void *userdata);

/* ==============================================================================
 * PROCESSING MODE LOOKUP
 * ==============================================================================
 *
 * processing_modes.c holds the single C table of processing modes: each entry
 * names a mode's configuration string and its callback family. Lookups fail
 * closed on any value outside that table rather than defaulting to a family.
 * processing_mode_to_string() (module_interface.h) uses the same table.
 */

/**
 * @brief   Parse a configuration-string processing mode name
 *
 * @param   name      Mode name, e.g. "process_by_galaxy" or "process_snapshot"
 * @param   out_mode  Receives the mode on success; untouched on failure
 * @return  0 on success, -1 if @p name is NULL or names no known mode
 */
int processing_mode_from_string(const char *name, enum ProcessingMode *out_mode);

/**
 * @brief   Look up the callback family a processing mode dispatches to
 *
 * @param   mode        Processing mode
 * @param   out_family  Receives the family on success; untouched on failure
 * @return  0 on success, -1 if @p mode is not a known mode
 */
int processing_mode_family(enum ProcessingMode mode, enum ModuleCallbackFamily *out_family);

/**
 * @brief   Register a galaxy physics module
 *
 * Modules call this function to register themselves with the core.
 * Must be called before module_system_init().
 *
 * Registration is family-aware: init() and cleanup() are always required, the
 * module must advertise at least one known processing mode, and each callback
 * family it advertises must have its typed callback (process() for FoF modes,
 * process_snapshot() for the snapshot mode). The callback of a family the
 * module does not advertise may be NULL. Any violation is fatal.
 *
 * @param   module  Pointer to module struct (must remain valid for program
 * lifetime)
 */
void module_registry_add(struct Module *module);

/**
 * @brief   Initialize the module system
 *
 * Validates pipeline configuration and calls init() on all referenced
 * modules. Modules are initialized in the order they appear across all phases.
 *
 * Should be called once during program initialization, after parameter
 * reading but before tree processing begins.
 *
 * @return  0 on success, non-zero if initialization fails
 */
int module_system_init(void);

/**
 * @brief   Execute modules in a specific phase
 *
 * Core execution engine for multi-phase pipeline. Implements galaxy-major
 * loop for PROCESSING_MODE_BY_GALAXY modules and event dispatch for
 * PROCESSING_MODE_PER_EVENT modules.
 *
 * Execution order within phase:
 * 1. All PROCESSING_MODE_FULL_HALO modules execute with full halo array
 * 2. Events emitted by full-halo modules are dispatched only to subscribed
 *    PROCESSING_MODE_PER_EVENT modules (resolved from module_info.yaml events.consumes)
 * 3. All PROCESSING_MODE_BY_GALAXY modules execute in galaxy-major order:
 *    for each galaxy g:
 *      module1(galaxy g)
 *      module2(galaxy g)
 *
 * Called from execute_module_pipeline() for each phase.
 *
 * @param   phase_config   Array of module configurations for this phase
 * @param   num_modules    Number of modules in this phase (0 = skip phase)
 * @param   ctx            Module execution context (redshift, time, substep info)
 * @param   halos          Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal           Number of halos in the array
 */
void execute_phase(struct PhaseModuleConfig *phase_config, int num_modules,
                   struct ModuleContext *ctx, struct Halo *halos, int ngal);

/**
 * @brief   Run the post_snapshot phase over one snapshot's processed population
 *
 * Calls each configured post_snapshot entry's typed process_snapshot()
 * callback once, in YAML order, with @p ctx, @p halos and @p count. The
 * phase is read from ctx->params. A zero count is a real call (an empty
 * snapshot still runs the phase), and @p halos may then be NULL or not.
 *
 * Snapshot dispatch is not a FoF phase: while it is active,
 * module_emit_event() rejects every call with a non-NULL context, so a
 * snapshot callback can neither emit nor consume FoF events.
 *
 * A non-zero callback return is fatal, naming the module, the snapshot and
 * the return code; the caller's exit-time failure cleanup then runs.
 *
 * @param   ctx     Snapshot context (built by the caller for this snapshot)
 * @param   halos   The snapshot's current processed population (borrowed)
 * @param   count   Number of entries in @p halos (>= 0)
 */
void execute_post_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                           int64_t count);

/**
 * @brief   Run the full configured module lifecycle over a halo workspace
 *
 * Format-neutral physics-execution engine: the shared entry point that runs the
 * configured module lifecycle (pre-timestep phase, the substep loop with its
 * user-named phases, then the post-timestep phase) over a halo workspace. It
 * operates purely on (ctx, halos, ngal), reading its phase configuration from
 * ctx->params rather than any global; it carries no tree-index, output-array,
 * or traversal-order assumptions, so any driver can call it once its
 * ModuleContext is populated.
 *
 * The caller is responsible for populating @p ctx (snapshot/substep timing,
 * central selection) and for marshalling the evolved workspace to output
 * afterwards; this engine only executes physics.
 *
 * @param   ctx     Module execution context (already populated by the caller)
 * @param   halos   Array of halos to evolve (e.g. FoFWorkspace)
 * @param   ngal    Number of halos in the array
 */
void execute_module_pipeline(struct ModuleContext *ctx, struct Halo *halos, int ngal);

/**
 * @brief   Cleanup the module system
 *
 * Calls cleanup() on all initialized modules in reverse order.
 * Should be called once during program shutdown.
 *
 * @return  0 on success, non-zero if any module cleanup fails
 */
int module_system_cleanup(void);

/**
 * @brief   Return the number of unique modules in the active pipeline
 *
 * Valid after module_system_init() has been called. Returns 0 in physics-free mode.
 */
int module_system_pipeline_count(void);

/**
 * @brief   Register all available physics modules
 *
 * Registers all physics modules that are compiled into the current build.
 * This function isolates physics-specific knowledge from the core.
 *
 * The core calls this function without knowing which specific modules exist,
 * maintaining physics-agnostic architecture (Vision Principle 1).
 *
 * Must be called before module_system_init().
 *
 * Implemented by the generated module-init source for the selected build.
 */
void register_all_modules(void);

/* ==============================================================================
 * MODEL PARAMETER ACCESS
 * ==============================================================================
 *
 * Model parameters must be explicitly specified in the input YAML file; no
 * defaults are used. This enforces reproducible science.
 */

/**
 * @brief   Get required model parameter as double
 *
 * @param   param_name      Parameter name (e.g., "BaryonFrac")
 * @param   out_value       Output pointer for double value
 * @return  0 on success, -1 if parameter missing or validation fails
 */
int model_get_double(const char *param_name, double *out_value);

/**
 * @brief   Get required model parameter as double converted to Mimic internal units
 *
 * Only parameters declared in the selected model package's parameter_units.yaml
 * receive a conversion factor. All other parameters are returned unchanged.
 */
int model_get_double_internal(const char *param_name, double *out_value);

/**
 * @brief   Get required model parameter as integer
 *
 * @param   param_name      Parameter name (e.g., "AGNrecipe")
 * @param   out_value       Output pointer for integer value
 * @return  0 on success, -1 if parameter missing or validation fails
 */
int model_get_int(const char *param_name, int *out_value);

/**
 * @brief   Get required model parameter as string
 *
 * @param   param_name      Parameter name
 * @param   out_value       Output buffer for string value
 * @param   max_len         Maximum length of output buffer
 * @return  0 on success, -1 if parameter missing
 */
int model_get_string(const char *param_name, char *out_value, size_t max_len);

/* ==============================================================================
 * DEPENDENCY ENFORCEMENT API
 * ==============================================================================
 *
 * Module init() functions use these utilities to enforce inter-module dependency
 * contracts at startup. Violations either exit (ERROR) or warn and continue
 * (WARNING), depending on severity.
 *
 * All functions are safe to call from any module init() — MimicConfig phase
 * arrays are fully populated before any init() is called.
 */

/**
 * @brief   Check if a module is configured in a given phase with a specific mode
 *
 * Intended for use in module init() functions to enforce dependency contracts
 * against the fixed lifecycle phases (including MimicConfig.post_snapshot with
 * PROCESSING_MODE_SNAPSHOT). For the substep middle phases use the
 * phase-name-agnostic helpers below (module_in_substep_phase, etc.).
 *
 * @param   name         Module name to search for
 * @param   phase        Phase config array (e.g., MimicConfig.pre_timestep)
 * @param   num_modules  Number of entries in the phase array (e.g. num_pre_timestep)
 * @param   mode         Processing mode to match
 * @return  true if the module is present in the phase with the given mode
 */
bool module_configured_in_phase(const char *name, const struct PhaseModuleConfig *phase,
                                int num_modules, enum ProcessingMode mode);

/**
 * @brief   Check if a module is configured in any phase with any mode
 *
 * Covers the FoF phases and post_snapshot.
 *
 * @param   name  Module name to search for
 * @return  true if the module appears in any phase
 */
bool module_configured_anywhere(const char *name);

/* ==============================================================================
 * EVENT CONTRACT ENUMERATION
 * ==============================================================================
 *
 * Provides a read-only view of the resolved event contracts for use by
 * output subsystems (e.g., HDF5 metadata writer). Only valid after
 * module_system_init() has been called.
 */

/**
 * @brief   Callback invoked for each resolved event subscription contract
 *
 * @param   phase             Phase name (e.g., "satellite_mergers")
 * @param   consumer_module   Consumer module name
 * @param   producer_module   Producer module name
 * @param   event_name        Event name (e.g., "merger")
 * @param   event_id          Generated numeric event ID
 * @param   userdata          Caller-supplied context pointer
 */
typedef void (*EventContractCallback)(const char *phase, const char *consumer_module,
                                      const char *producer_module, const char *event_name,
                                      int event_id, void *userdata);

/**
 * @brief   Enumerate all resolved event contracts
 *
 * Iterates all configured process_per_event consumers and their subscriptions,
 * calling cb once for each (consumer, subscription) pair.
 *
 * Intended for use by output subsystems (e.g., HDF5 metadata) to record
 * event wiring for reproducibility. Only valid after module_system_init().
 *
 * @param   cb        Callback function called for each contract (may be NULL)
 * @param   userdata  Caller-supplied pointer passed to each callback invocation
 */
void module_system_enumerate_event_contracts(EventContractCallback cb, void *userdata);

/**
 * @brief   Check if 'first' appears before 'second' in a phase array
 *
 * Returns false if either module is absent from the phase.
 * Used to enforce ordering constraints between dependent modules.
 *
 * @param   first        Module that must appear earlier
 * @param   second       Module that must appear later
 * @param   phase        Phase config array to search
 * @param   num_modules  Number of entries in the phase array
 * @return  true if first precedes second; false if either is absent or order is wrong
 */
bool module_precedes_in_phase(const char *first, const char *second,
                              const struct PhaseModuleConfig *phase, int num_modules);

/* ==============================================================================
 * SUBSTEP-PHASE DEPENDENCY API (phase-name-agnostic)
 * ==============================================================================
 *
 * The substep middle phases are user-named and arbitrary in number, so module
 * init() code must not reference a fixed numbered phase array. These helpers ask
 * dependency questions in terms of "the substep phase containing this module"
 * without naming it. Checks against the fixed pre_timestep/post_timestep phases
 * still use module_configured_in_phase() with MimicConfig.pre_timestep/post_timestep.
 */

/**
 * @brief   Is (name, mode) configured in any substep middle phase?
 *
 * @param   name  Module name to search for
 * @param   mode  Processing mode to match
 * @return  true if the (module, mode) pair appears in some substep phase
 */
bool module_in_substep_phase(const char *name, enum ProcessingMode mode);

/**
 * @brief   Do (a, mode_a) and (b, mode_b) appear together in one substep phase?
 *
 * Used to enforce same-phase co-occurrence contracts (e.g. a per-event consumer
 * needs its full-halo producer in the same phase).
 *
 * @return  true if some single substep phase contains both pairings
 */
bool modules_in_same_substep_phase(const char *a, enum ProcessingMode mode_a, const char *b,
                                   enum ProcessingMode mode_b);

/**
 * @brief   Does (first, first_mode) precede (second, second_mode) within the
 *          same substep phase?
 *
 * Returns true only if some substep phase contains both module/mode pairs with
 * 'first' earlier in YAML order. Returns false if they never share a phase.
 *
 * @return  true if first precedes second within a shared substep phase
 */
bool module_precedes_in_substep_phase(const char *first, enum ProcessingMode first_mode,
                                      const char *second, enum ProcessingMode second_mode);

#endif // MODULE_REGISTRY_H
