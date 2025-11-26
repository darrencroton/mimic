/**
 * @file    module_registry.h
 * @brief   Module registration and execution pipeline
 *
 * This file provides the interface for registering galaxy physics modules
 * and executing them in a coordinated pipeline.
 *
 * Vision Principle 1 (Physics-Agnostic Core): The registry manages modules
 * without knowing anything about their specific physics implementations.
 *
 * Usage:
 * 1. Modules call module_registry_add() to register themselves (at startup)
 * 2. Main program calls module_system_init() after parameter reading
 * 3. Tree processing calls module_execute_pipeline() for each FOF group
 * 4. Main program calls module_system_cleanup() before exit
 */

#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include <stddef.h> /* for size_t */

#include "module_interface.h"

/**
 * @brief   Register a galaxy physics module
 *
 * Modules call this function to register themselves with the core.
 * Must be called before module_system_init().
 *
 * @param   module  Pointer to module struct (must remain valid for program
 * lifetime)
 */
void module_registry_add(struct Module *module);

/**
 * @brief   Initialize the module system
 *
 * Builds the execution pipeline based on runtime configuration and calls
 * init() on all enabled modules in configured order.
 *
 * Should be called once during program initialization, after parameter
 * reading but before tree processing begins.
 *
 * @return  0 on success, non-zero if initialization fails
 */
int module_system_init(void);

/**
 * @brief   Execute all enabled modules on a FOF group
 *
 * Calls process_halos() on all enabled modules in configured order.
 * This is where galaxy physics is computed.
 *
 * Called from the tree processing loop after halo tracking is complete
 * but before halos are moved to output storage.
 *
 * @param   halonr  Index of the main halo in InputTreeHalos (for context
 * information)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, non-zero if any module processing fails
 */
int module_execute_pipeline(int halonr, struct Halo *halos, int ngal);

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
 * Implementation: src/modules/module_init.c
 * Phase 5: Will be auto-generated from module metadata
 */
void register_all_modules(void);

/**
 * @brief   Read a module parameter as string (optional with default)
 *
 * Reads a parameter value for the specified module. If the parameter is not
 * found, uses the provided default value. Always succeeds.
 *
 * @param   module_name     Module name (e.g., "SimpleCooling")
 * @param   param_name      Parameter name (e.g., "BaryonFraction")
 * @param   out_value       Output buffer for value string
 * @param   max_len         Maximum length of output buffer
 * @param   default_value   Default value if parameter not found
 * @return  0 on success (always succeeds)
 */
int module_get_parameter(const char *module_name, const char *param_name,
                         char *out_value, size_t max_len,
                         const char *default_value);

/**
 * @brief   Read a module parameter as double
 *
 * Reads a numeric parameter value for the specified module.
 * Default value and validation range come from module_info.yaml.
 *
 * @param   module_name     Module name (e.g., "sage_infall")
 * @param   param_name      Parameter name (e.g., "BaryonFrac")
 * @param   out_value       Output pointer for double value
 * @return  0 on success, -1 on validation error
 */
int module_get_double(const char *module_name, const char *param_name,
                      double *out_value);

/**
 * @brief   Read a module parameter as integer
 *
 * Reads an integer parameter value for the specified module.
 * Default value and validation range come from module_info.yaml.
 *
 * @param   module_name     Module name (e.g., "sage_infall")
 * @param   param_name      Parameter name (e.g., "ReionizationOn")
 * @param   out_value       Output pointer for integer value
 * @return  0 on success, -1 on validation error
 */
int module_get_int(const char *module_name, const char *param_name,
                   int *out_value);

/* ==============================================================================
 * MODEL PARAMETER ACCESS (Phase 4.4)
 * ==============================================================================
 *
 * Model parameters are defined in model_parameters.yaml and MUST be explicitly
 * specified in the input YAML file. NO defaults are used.
 *
 * This enforces reproducible science: input file is complete model specification.
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
 * @brief   Get required model parameter as integer
 *
 * @param   param_name      Parameter name (e.g., "AGNrecipeOn")
 * @param   out_value       Output pointer for integer value
 * @return  0 on success, -1 if parameter missing or validation fails
 */
int model_get_int(const char *param_name, int *out_value);

/**
 * @brief   Get required model parameter as string
 *
 * @param   param_name      Parameter name (e.g., "CoolFunctionsDir")
 * @param   out_value       Output buffer for string value
 * @param   max_len         Maximum length of output buffer
 * @return  0 on success, -1 if parameter missing
 */
int model_get_string(const char *param_name, char *out_value, size_t max_len);

#endif // MODULE_REGISTRY_H
