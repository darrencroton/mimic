/**
 * @file    module_interface.h
 * @brief   Galaxy physics module interface
 *
 * This file defines the standard interface that all galaxy physics modules
 * must implement. Modules register themselves with the module registry and
 * are executed through a multi-phase pipeline with optional time sub-stepping.
 *
 * Vision Principle 1 (Physics-Agnostic Core): The core has zero knowledge of
 * specific physics implementations. Modules interact with core only through
 * this well-defined interface.
 *
 * Vision Principle 2 (Runtime Modularity): Module combinations and execution
 * structure are configurable at runtime without recompilation.
 *
 * Multi-Phase Pipeline Architecture:
 * - pre_timestep: Setup phase (once before substeps)
 * - modules.phases: Ordered user-named substep phases
 * - post_timestep: Finalization phase (once after substeps)
 *
 * Phase assignments and processing modes are specified in the input YAML configuration,
 * not in module metadata. This provides maximum flexibility - the same module
 * can be used in different phases in different configurations.
 *
 * Module Lifecycle:
 * 1. Module registers itself via module_registry_add() during program start
 * 2. Core calls init() during program initialization
 * 3. Core calls process() for each FOF group during tree processing
 *    - May be called once per timestep or multiple times per substep
 *    - May receive full halo array (process_full_halo), event target halo
 *      (process_per_event), or single galaxy (process_by_galaxy)
 * 4. Core calls cleanup() during program shutdown
 *
 * Example module implementation:
 * @code
 * static int my_module_init(void) {
 *     INFO_LOG("My module initialized");
 *     return 0;
 * }
 *
 * static int my_module_process(struct ModuleContext *ctx, struct Halo *halos,
 *                               int ngal) {
 *     // Access substep information
 *     int substep = ctx->substep_number;
 *     double dt = ctx->substep_dt;
 *
 *     // Process halos:
 *     //   - ngal=1 for process_by_galaxy and process_per_event
 *     //   - ngal>1 for process_full_halo
 *     for (int i = 0; i < ngal; i++) {
 *         if (halos[i].galaxy == NULL) continue;
 *
 *         // Access simulation context
 *         double z = ctx->redshift;
 *         double hubble_h = ctx->params->Hubble_h;
 *
 *         // Update galaxy properties
 *         halos[i].galaxy->SomeProperty += compute_physics(halos[i], z, dt);
 *     }
 *     return 0;
 * }
 *
 * static int my_module_cleanup(void) {
 *     INFO_LOG("My module cleaned up");
 *     return 0;
 * }
 *
 * static struct Module my_module = {
 *     .name = "my_module",
 *     .init = my_module_init,
 *     .process = my_module_process,
 *     .cleanup = my_module_cleanup
 * };
 *
 * void my_module_register(void) {
 *     module_registry_add(&my_module);
 * }
 * @endcode
 */

#ifndef MODULE_INTERFACE_H
#define MODULE_INTERFACE_H

#include "types.h"

/*
 * Pipeline shape (configured in the input YAML, not in module metadata):
 *
 *   pre_timestep (once)
 *     -> [ user-named substep phases, in input order ] x SubSteps
 *     -> post_timestep (once)
 *
 * pre_timestep and post_timestep are fixed optional lifecycle phases. The
 * middle phases are arbitrary in number and named by the user for their
 * physical meaning (e.g. "galaxy_physics", "satellite_mergers"); see
 * struct ModulePhaseConfig in module_registry.h. Within each phase, full-halo
 * and event work precedes galaxy-local work. Adding a phase is purely a YAML
 * change — no enum or struct edits are required.
 */

/**
 * @brief   Processing modes for module execution
 *
 * Controls how the core calls modules within a phase:
 * - PROCESSING_MODE_FULL_HALO: Module processes entire halo array at once (ngal = full array size)
 * - PROCESSING_MODE_PER_EVENT: Module processes one emitted event target at a time (ngal = 1)
 * - PROCESSING_MODE_BY_GALAXY: Core loops over galaxies, module processes one at a time (ngal = 1)
 *
 * When multiple PROCESSING_MODE_BY_GALAXY modules exist in a phase, they execute in
 * galaxy-major order:
 *   for each galaxy g:
 *     module1(galaxy g)
 *     module2(galaxy g)
 *     module3(galaxy g)
 *
 * This provides better cache locality and matches SAGE behavior.
 */
enum ProcessingMode {
  PROCESSING_MODE_FULL_HALO, /**< Module called once with full halo array */
  PROCESSING_MODE_PER_EVENT, /**< Module called for each emitted event target */
  PROCESSING_MODE_BY_GALAXY, /**< Module called per-galaxy (galaxy-major loop) */
  PROCESSING_MODE_COUNT      /**< Number of processing modes */
};

/**
 * @brief   Phase-local event payload with producer-scoped identity
 *
 * Events are emitted by process_full_halo producer modules and dispatched
 * only to process_per_event consumers that declare a matching subscription
 * in their module_info.yaml. Routing is resolved at startup; no consumer-side
 * event-code filtering is required in module C code.
 *
 * producer_module_id and event_id are generated by the metadata system and
 * injected by the core at dispatch time — module authors work with the
 * generated constants from _system/generated/event_contracts.h.
 */
struct ModuleEvent {
  int producer_module_id; /**< Generated ID of the emitting module */
  int event_id;           /**< Generated per-producer event ID */
  int source_index;       /**< Source halo index in FoFWorkspace */
  int target_index;       /**< Target halo index in FoFWorkspace */
  double value0;          /**< Primary scalar payload */
  double value1;          /**< Secondary scalar payload */
};

/**
 * @brief   Consumer subscription record
 *
 * Describes one (producer, event) pair that a consumer module subscribes to.
 * Generated from events.consumes declarations in module_info.yaml and stored
 * in the Module struct so the registry can perform subscription-based routing
 * without string comparisons on the hot path.
 *
 * event_name and producer_name are human-readable strings used for logging
 * and HDF5 run metadata; they are not used for dispatch matching.
 */
struct EventSubscription {
  int producer_module_id;    /**< Expected producer module ID */
  int event_id;              /**< Expected event ID within the producer */
  const char *event_name;    /**< Event name (e.g., "merger"); for metadata */
  const char *producer_name; /**< Producer module name; for metadata */
};

/**
 * @brief   Module execution context
 *
 * Provides modules with access to simulation state, substep information,
 * and core infrastructure. This context is passed to modules during execution.
 *
 * Modules should treat all context fields as read-only - they provide
 * information about the current execution state but should not be modified.
 */
struct ModuleContext {
  /* ===== Snapshot Information ===== */

  /**
   * @brief Current snapshot redshift
   *
   * Redshift of the current snapshot being processed. Use for
   * redshift-dependent physics (e.g., UV background, cooling rates).
   */
  double redshift;

  /**
   * @brief Current cosmic time (lookback time from z=0)
   *
   * Time at current snapshot in internal units.
   * Use for time-dependent physics.
   */
  double time;

  /**
   * @brief Current snapshot number
   *
   * Index of current snapshot (0 = z=127, increasing towards z=0).
   */
  int snapshot_number;

  /* ===== Sub-stepping Information ===== */

  /**
   * @brief Current substep number (0-indexed)
   *
   * Which substep we're in (0 to num_substeps-1).
   * Modules in pre_timestep and post_timestep phases will see substep_number=0.
   */
  int substep_number;

  /**
   * @brief Total number of substeps
   *
   * Total substeps for this timestep (from SubSteps parameter in config).
   * If SubSteps=0 or not specified, num_substeps=1 (no sub-stepping).
   */
  int num_substeps;

  /**
   * @brief Total time interval for this timestep
   *
   * Time between previous snapshot and current snapshot.
   * time_interval = Age[prev] - Age[current]
   */
  double time_interval;

  /**
   * @brief Cosmic time at current substep midpoint
   *
   * Interpolated midpoint time for this substep.
   * substep_time = (time + time_interval) - (substep_number + 0.5) * substep_dt
   */
  double substep_time;

  /**
   * @brief Time interval for this substep
   *
   * Time step for this substep.
   * substep_dt = time_interval / num_substeps
   * Use this for time integration within modules.
   */
  double substep_dt;

  /* ===== Halo Information ===== */

  /**
   * @brief Index of central halo in FoFWorkspace array
   *
   * Index of the Type 0 central galaxy for this FOF group.
   * All galaxies in the array belong to the same FOF group.
   */
  int central_index;

  /**
   * @brief Pointer to central galaxy for this FOF group
   *
   * Direct access to the Type 0 central galaxy (FoFWorkspace[central_index]).
   * This allows process_by_galaxy modules to access central galaxy properties
   * when processing satellites.
   *
   * Use cases:
   * - Access central's Vvir for ejection calculations
   * - Modify central's HotGas/EjectedGas from satellite modules
   * - Read central's properties for satellite physics
   *
   * Always non-NULL during module execution. Safe to dereference.
   */
  struct Halo *central_galaxy;

  /* ===== Event Information ===== */

  /**
   * @brief Active event payload for PROCESSING_MODE_PER_EVENT invocations
   *
   * NULL for PROCESSING_MODE_FULL_HALO and PROCESSING_MODE_BY_GALAXY calls.
   * Non-NULL only when core is dispatching one emitted event.
   */
  const struct ModuleEvent *active_event;

  /* ===== Configuration Access ===== */

  /**
   * @brief Read-only access to configuration parameters
   *
   * Provides modules with access to simulation parameters (cosmology, units,
   * etc.). Modules should NOT modify these parameters.
   */
  const struct MimicConfig *params;
};

/**
 * @brief Emit a phase-local event for subscribed PROCESSING_MODE_PER_EVENT consumers
 *
 * Intended for producer modules running in PROCESSING_MODE_FULL_HALO.
 * Events are dispatched immediately to subscribed consumers and cleared at
 * phase end. The core injects the caller's producer_module_id automatically;
 * the caller supplies only the per-producer event_id (a generated constant
 * from _system/generated/event_contracts.h).
 *
 * Only consumers that declared a matching subscription in their module_info.yaml
 * receive the event — no consumer-side event-code checks are needed.
 *
 * @param ctx           Module context from current process() call
 * @param event_id      Generated per-producer event ID (e.g.,
 *                      SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER)
 * @param source_index  Source halo index in FoFWorkspace
 * @param target_index  Target halo index in FoFWorkspace
 * @param value0        Primary scalar payload
 * @param value1        Secondary scalar payload
 * @return 0 on success, non-zero on failure
 *
 * Failure cases: invalid context, invalid halo indices, buffer overflow,
 * or calling outside PROCESSING_MODE_FULL_HALO dispatch.
 *
 * Special case for direct module unit tests: when no phase dispatch context is
 * active, the event is dropped and 0 is returned.
 */
int module_emit_event(struct ModuleContext *ctx, int event_id, int source_index, int target_index,
                      double value0, double value1);

/**
 * @brief   Galaxy physics module interface
 *
 * All galaxy physics modules must implement this interface. The core calls
 * these functions at appropriate points in the execution pipeline.
 *
 * KEY DESIGN: Modules are simple - they just implement physics via a single
 * process() function. The module doesn't specify its execution phase or processing
 * mode - those are configuration details specified in the input YAML file.
 * This makes modules maximally reusable.
 */
struct Module {
  /**
   * @brief Module name (must be unique)
   *
   * Used for logging, diagnostics, and runtime configuration. Should be
   * lowercase with underscores, e.g., "stellar_mass", "cooling_sd93",
   * "star_formation_ks98".
   */
  const char *name;

  /**
   * @brief Initialize module
   *
   * Called once during program startup. Use for:
   * - Loading module parameters from configuration
   * - Allocating persistent memory
   * - Initializing lookup tables
   * - Logging module configuration
   *
   * @return 0 on success, non-zero on failure
   */
  int (*init)(void);

  /**
   * @brief Process halos in a FOF group
   *
   * This is the single processing function called by the pipeline.
   * May be called:
   * - Once or multiple times per timestep (depends on phase and substeps)
   * - With full halo array (ngal > 1, process_full_halo)
   * - With one event target halo (ngal = 1, process_per_event)
   * - With single galaxy (ngal = 1, process_by_galaxy)
   *
   * The halos array is in FoFWorkspace (temporary processing space). All
   * halos in the array belong to the same FOF group at the same snapshot.
   *
   * Modules should:
   * - Update galaxy properties (halos[i].galaxy->SomeProperty)
   * - Preserve halo properties (read-only)
   * - Handle all halo types (central, satellite, orphan)
   * - Access simulation/substep context via ctx
   * - Use ctx->substep_dt for time integration
   *
   * @param ctx   Module execution context (redshift, time, substep info, params)
   * @param halos Array of halos in the FOF group (FoFWorkspace)
   * @param ngal  Number of halos in the array (1 if process_by_galaxy or
   *              process_per_event, >1 if process_full_halo)
   * @return 0 on success, non-zero on failure
   */
  int (*process)(struct ModuleContext *ctx, struct Halo *halos, int ngal);

  /**
   * @brief Cleanup module
   *
   * Called once during program shutdown. Use for:
   * - Freeing persistent memory
   * - Closing files
   * - Logging final statistics
   *
   * @return 0 on success, non-zero on failure
   */
  int (*cleanup)(void);

  /**
   * @brief Supported processing modes for this module
   *
   * Declares which processing modes this module can execute in:
   * - PROCESSING_MODE_FULL_HALO: Module processes full halo array (array-based operations)
   * - PROCESSING_MODE_PER_EVENT: Module processes one event target at a time
   * - PROCESSING_MODE_BY_GALAXY: Module processes one galaxy at a time (per-galaxy operations)
   *
   * Set via module_info.yaml (supported_processing_modes field).
   * Runtime directory modules must declare this explicitly. Legacy standalone
   * fallback metadata, if still present, may advertise broader support.
   *
   * Runtime validation ensures modules are only configured with supported modes.
   *
   * Example array (generated from metadata):
   *   static const enum ProcessingMode my_module_modes[] = {PROCESSING_MODE_BY_GALAXY};
   *
   * Example usage in Module struct:
   *   .supported_processing_modes = my_module_modes,
   *   .num_supported_modes = 1
   */
  const enum ProcessingMode *supported_processing_modes;

  /**
   * @brief Number of supported processing modes
   *
   * Length of the supported_processing_modes array. Must be > 0.
   * Typically 1 (module only works in one mode) or 2 (module supports both modes).
   */
  int num_supported_modes;

  /* ===== Event System (generated from module_info.yaml events section) ===== */

  /**
   * @brief Producer module ID for event dispatch (0 if not a producer)
   *
   * Assigned at code-generation time from the sorted list of producer modules.
   * Injected into ModuleEvent.producer_module_id before calling process() so
   * consumers can verify event origin via subscription tables.
   * Generated constant: MODULE_ID_<UPPERCASE_MODULE_NAME> in event_contracts.h.
   */
  int module_id;

  /**
   * @brief Consumer event subscriptions (NULL if not a consumer)
   *
   * Points to a generated array of EventSubscription records describing which
   * (producer, event) pairs this module subscribes to. The dispatch loop checks
   * these before calling process_per_event consumers — no C-level event-code
   * filtering is needed in module code.
   * Populated in module_init.c from events.consumes in module_info.yaml.
   */
  const struct EventSubscription *subscriptions;

  /**
   * @brief Length of subscriptions array (0 if not a consumer)
   *
   * Must be > 0 for any module configured as process_per_event; validated
   * during module_system_init().
   */
  int num_subscriptions;

  /**
   * @brief Valid emitted event IDs for producer modules (NULL if not a producer)
   *
   * Points to a generated array of event_id values this module declared in
   * events.emits. module_emit_event() validates against this array at emit time
   * so undeclared or wrong event IDs fail immediately rather than silently
   * routing to no one.
   * Populated in module_init.c from events.emits in module_info.yaml.
   */
  const int *emitted_event_ids;

  /**
   * @brief Length of emitted_event_ids array (0 if not a producer)
   */
  int num_emitted_events;
};

#endif // MODULE_INTERFACE_H
