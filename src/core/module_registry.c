/**
 * @file module_registry.c
 * @brief Implementation of module registration and execution pipeline
 *
 * This file implements the module registry system that allows galaxy physics
 * modules to register themselves and be executed in a coordinated pipeline.
 *
 * The registry maintains a simple array of module pointers. Modules are
 * executed in registration order during the processing pipeline.
 *
 * Vision Principle 1 (Physics-Agnostic Core): The registry treats all modules
 * identically through the Module interface, with zero knowledge of specific
 * physics implementations.
 */

#include <stdio.h>
#include <stdlib.h>
#include "module_registry.h"
#include "module_interface.h"
#include "globals.h"
#include "error.h"

/** Maximum number of modules that can be registered */
#define MAX_MODULES 32

/** Array of registered modules */
static struct Module *registered_modules[MAX_MODULES];

/** Number of currently registered modules */
static int num_registered_modules = 0;

/**
 * @brief Register a galaxy physics module
 *
 * Adds the module to the registry. Modules are executed in registration order.
 *
 * @param module Pointer to module struct (must remain valid for program lifetime)
 */
void register_module(struct Module *module)
{
    if (module == NULL) {
        ERROR_LOG("Attempted to register NULL module");
        exit(EXIT_FAILURE);
    }

    if (num_registered_modules >= MAX_MODULES) {
        ERROR_LOG("Maximum number of modules (%d) exceeded", MAX_MODULES);
        exit(EXIT_FAILURE);
    }

    if (module->name == NULL) {
        ERROR_LOG("Module has NULL name");
        exit(EXIT_FAILURE);
    }

    if (module->init == NULL || module->process_halos == NULL ||
        module->cleanup == NULL) {
        ERROR_LOG("Module '%s' has NULL function pointers", module->name);
        exit(EXIT_FAILURE);
    }

    registered_modules[num_registered_modules] = module;
    num_registered_modules++;

    INFO_LOG("Registered module: %s", module->name);
}

/**
 * @brief Initialize all registered modules
 *
 * Calls init() on all registered modules in registration order.
 *
 * @return 0 on success, non-zero if any module init fails
 */
int module_system_init(void)
{
    INFO_LOG("Initializing %d module(s)", num_registered_modules);

    for (int i = 0; i < num_registered_modules; i++) {
        struct Module *mod = registered_modules[i];
        DEBUG_LOG("Initializing module: %s", mod->name);

        int result = mod->init();
        if (result != 0) {
            ERROR_LOG("Module '%s' initialization failed with code %d",
                      mod->name, result);
            return result;
        }
    }

    INFO_LOG("Module system initialized successfully");
    return 0;
}

/**
 * @brief Execute all modules on a FOF group
 *
 * Calls process_halos() on all registered modules in registration order.
 *
 * @param halonr Index of the main halo in InputTreeHalos (for context information)
 * @param halos Array of halos in the FOF group (FoFWorkspace)
 * @param ngal Number of halos in the array
 * @return 0 on success, non-zero if any module processing fails
 */
int module_execute_pipeline(int halonr, struct Halo *halos, int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0; // Nothing to process
    }

    // Populate module execution context
    struct ModuleContext ctx;
    int snap = InputTreeHalos[halonr].SnapNum;
    ctx.redshift = ZZ[snap];
    ctx.time = Age[snap];
    ctx.params = &MimicConfig;

    for (int i = 0; i < num_registered_modules; i++) {
        struct Module *mod = registered_modules[i];
        DEBUG_LOG("Executing module: %s (ngal=%d, z=%.3f)", mod->name, ngal, ctx.redshift);

        int result = mod->process_halos(&ctx, halos, ngal);
        if (result != 0) {
            ERROR_LOG("Module '%s' processing failed with code %d",
                      mod->name, result);
            return result;
        }
    }

    return 0;
}

/**
 * @brief Cleanup all registered modules
 *
 * Calls cleanup() on all registered modules in reverse registration order.
 *
 * @return 0 on success, non-zero if any module cleanup fails
 */
int module_system_cleanup(void)
{
    INFO_LOG("Cleaning up %d module(s)", num_registered_modules);

    int result = 0;

    // Cleanup in reverse order
    for (int i = num_registered_modules - 1; i >= 0; i--) {
        struct Module *mod = registered_modules[i];
        DEBUG_LOG("Cleaning up module: %s", mod->name);

        int cleanup_result = mod->cleanup();
        if (cleanup_result != 0) {
            ERROR_LOG("Module '%s' cleanup failed with code %d",
                      mod->name, cleanup_result);
            result = cleanup_result; // Continue cleanup but record failure
        }
    }

    INFO_LOG("Module system cleanup complete");
    return result;
}
