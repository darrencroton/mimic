/**
 * @file module_registry.h
 * @brief Module registration and execution pipeline
 *
 * This file provides the interface for registering galaxy physics modules
 * and executing them in a coordinated pipeline.
 *
 * Vision Principle 1 (Physics-Agnostic Core): The registry manages modules
 * without knowing anything about their specific physics implementations.
 *
 * Usage:
 * 1. Modules call register_module() to register themselves (typically at startup)
 * 2. Main program calls module_system_init() after parameter reading
 * 3. Tree processing calls module_execute_pipeline() for each FOF group
 * 4. Main program calls module_system_cleanup() before exit
 */

#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include "module_interface.h"

/**
 * @brief Register a galaxy physics module
 *
 * Modules call this function to register themselves with the core.
 * Must be called before module_system_init().
 *
 * @param module Pointer to module struct (must remain valid for program lifetime)
 */
void register_module(struct Module *module);

/**
 * @brief Initialize all registered modules
 *
 * Calls init() on all registered modules in registration order.
 * Should be called once during program initialization, after parameter
 * reading but before tree processing begins.
 *
 * @return 0 on success, non-zero if any module init fails
 */
int module_system_init(void);

/**
 * @brief Execute all modules on a FOF group
 *
 * Calls process_halos() on all registered modules in registration order.
 * This is where galaxy physics is computed.
 *
 * Called from process_halo_evolution() after halo tracking is complete
 * but before halos are moved to output storage.
 *
 * @param halos Array of halos in the FOF group (FoFWorkspace)
 * @param ngal Number of halos in the array
 * @return 0 on success, non-zero if any module processing fails
 */
int module_execute_pipeline(struct Halo *halos, int ngal);

/**
 * @brief Cleanup all registered modules
 *
 * Calls cleanup() on all registered modules in reverse registration order.
 * Should be called once during program shutdown.
 *
 * @return 0 on success, non-zero if any module cleanup fails
 */
int module_system_cleanup(void);

#endif // MODULE_REGISTRY_H
