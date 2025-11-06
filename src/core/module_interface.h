/**
 * @file module_interface.h
 * @brief Galaxy physics module interface
 *
 * This file defines the standard interface that all galaxy physics modules
 * must implement. Modules register themselves with the module registry and
 * are executed through a unified pipeline.
 *
 * Vision Principle 1 (Physics-Agnostic Core): The core has zero knowledge of
 * specific physics implementations. Modules interact with core only through
 * this well-defined interface.
 *
 * Vision Principle 2 (Runtime Modularity): Module combinations are configurable
 * without recompilation (future: currently all compiled modules always run).
 *
 * Module Lifecycle:
 * 1. Module registers itself via register_module() (typically at program start)
 * 2. Core calls init() during program initialization
 * 3. Core calls process_halos() for each FOF group during tree processing
 * 4. Core calls cleanup() during program shutdown
 *
 * Example module implementation:
 * @code
 * static int my_module_init(void) {
 *     INFO_LOG("My module initialized");
 *     return 0;
 * }
 *
 * static int my_module_process(struct Halo *halos, int ngal) {
 *     for (int i = 0; i < ngal; i++) {
 *         // Update galaxy properties
 *         halos[i].galaxy->SomeProperty = compute_physics(halos[i]);
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
 *     .process_halos = my_module_process,
 *     .cleanup = my_module_cleanup
 * };
 *
 * void my_module_register(void) {
 *     register_module(&my_module);
 * }
 * @endcode
 */

#ifndef MODULE_INTERFACE_H
#define MODULE_INTERFACE_H

#include "types.h"

/**
 * @brief Galaxy physics module interface
 *
 * All galaxy physics modules must implement this interface. The core calls
 * these functions at appropriate points in the execution pipeline.
 */
struct Module {
    /**
     * @brief Module name (must be unique)
     *
     * Used for logging and diagnostics. Should be lowercase with underscores,
     * e.g., "stellar_mass", "cooling_sd93", "star_formation_ks98".
     */
    const char *name;

    /**
     * @brief Initialize module
     *
     * Called once during program startup. Use for:
     * - Loading module parameters
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
     * Called for each FOF group after halo tracking is complete but before
     * output is written. This is where galaxy physics is computed.
     *
     * The halos array is in FoFWorkspace (temporary processing space). All
     * halos in the array belong to the same FOF group at the same snapshot.
     *
     * Modules should:
     * - Update galaxy properties (halos[i].galaxy->SomeProperty)
     * - Preserve halo properties (read-only)
     * - Handle all halo types (central, satellite, orphan)
     *
     * @param halos Array of halos in the FOF group (FoFWorkspace)
     * @param ngal Number of halos in the array
     * @return 0 on success, non-zero on failure
     */
    int (*process_halos)(struct Halo *halos, int ngal);

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
};

#endif // MODULE_INTERFACE_H
