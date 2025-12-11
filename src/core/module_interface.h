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
 * - phase_1: First substep phase (configurable processing mode)
 * - phase_2: Second substep phase (configurable processing mode)
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
 *    - May receive full halo array (process_full_halo) or single galaxy (process_by_galaxy)
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
 *     // Process halos (ngal=1 if process_by_galaxy, ngal>1 if process_full_halo)
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

/**
 * @brief   Execution phases for module pipeline
 *
 * Generic phase names allow flexible configuration. Users decide which physics
 * goes in which phase via input YAML configuration.
 *
 * Typical usage (but not enforced):
 * - pre_timestep: Setup calculations that run once before substeps
 *   (e.g., reionization, infall budget calculation)
 * - phase_1: Baryonic physics during substeps
 *   (e.g., cooling, star formation, feedback)
 * - phase_2: Merger/disruption physics during substeps
 *   (e.g., mergers, satellite tracking)
 * - post_timestep: Finalization that runs once after substeps
 *   (e.g., converting accumulators to rates, summary statistics)
 *
 * Extensibility: Adding phase_3, phase_4, etc. requires:
 * 1. Add enum value here
 * 2. Add fields to MimicConfig struct
 * 3. Add execution call in process_halo_evolution()
 * 4. Update YAML parsing
 */
enum ModulePhase {
  MODULE_PHASE_PRE_TIMESTEP,   /**< Before substeps (once) */
  MODULE_PHASE_1,               /**< First phase within substep loop */
  MODULE_PHASE_2,               /**< Second phase within substep loop */
  MODULE_PHASE_POST_TIMESTEP,  /**< After substeps (once) */
  MODULE_PHASE_COUNT           /**< Number of phases */
};

/**
 * @brief   Processing modes for module execution
 *
 * Controls how the core calls modules within a phase:
 * - PROCESSING_MODE_FULL_HALO: Module processes entire halo array at once (ngal = full array size)
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
  PROCESSING_MODE_FULL_HALO,  /**< Module called once with full halo array */
  PROCESSING_MODE_BY_GALAXY,  /**< Module called per-galaxy (galaxy-major loop) */
  PROCESSING_MODE_COUNT       /**< Number of processing modes */
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
   * Time at current snapshot in internal units (Gyr/h).
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
   * Time between previous snapshot and current snapshot (Gyr/h).
   * time_interval = Age[prev] - Age[current]
   */
  double time_interval;

  /**
   * @brief Cosmic time at current substep midpoint
   *
   * Interpolated time for this substep (Gyr/h).
   * substep_time = time - (substep_number + 0.5) * substep_dt
   */
  double substep_time;

  /**
   * @brief Time interval for this substep
   *
   * Time step for this substep (Gyr/h).
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
   * - With full halo array (ngal > 1, process_full_halo) or single galaxy (ngal = 1, process_by_galaxy)
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
   * @param ngal  Number of halos in the array (1 if process_by_galaxy, >1 if process_full_halo)
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
   * - PROCESSING_MODE_BY_GALAXY: Module processes one galaxy at a time (per-galaxy operations)
   *
   * Set via module_info.yaml (supported_processing_modes field). If omitted from
   * metadata, defaults to supporting both modes.
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
};

#endif // MODULE_INTERFACE_H
