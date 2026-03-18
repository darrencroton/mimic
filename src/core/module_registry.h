/**
 * @file    module_registry.h
 * @brief   Module registration and multi-phase execution pipeline
 *
 * This file provides the interface for registering galaxy physics modules
 * and executing them in a multi-phase pipeline with optional time sub-stepping.
 *
 * Vision Principle 1 (Physics-Agnostic Core): The registry manages modules
 * without knowing anything about their specific physics implementations.
 *
 * Vision Principle 2 (Runtime Modularity): Pipeline structure is configured
 * at runtime via input YAML file, not hardcoded in module metadata.
 *
 * Multi-Phase Pipeline:
 * - Pre-timestep phase (once before substeps)
 * - Phase 1 (each substep, configurable processing mode)
 * - Phase 2 (each substep, configurable processing mode)
 * - Post-timestep phase (once after substeps)
 *
 * Usage:
 * 1. Modules call module_registry_add() to register themselves (at startup)
 * 2. Main program calls module_system_init() after parameter reading
 * 3. Tree processing calls execute_phase() for each phase
 * 4. Main program calls module_system_cleanup() before exit
 */

#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h> /* for size_t */

#include "module_interface.h"

/**
 * @brief   Phase module configuration entry
 *
 * Specifies one module's role in one phase. The input YAML file contains
 * lists of these for each phase, defining the complete execution pipeline.
 *
 * Example YAML:
 *   phase_1:
 *     - sage_calculate_cooling: process_by_galaxy
 *     - sage_radio_mode_heating: process_by_galaxy
 *     - sage_add_cooling: process_by_galaxy
 *
 * This creates three PhaseModuleConfig entries for phase_1.
 */
struct PhaseModuleConfig {
  char *module_name;                  /**< Module name (must match registered module) */
  enum ProcessingMode processing_mode; /**< How to call module */
};

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
 * 2. Events emitted by full-halo modules are dispatched to all
 *    PROCESSING_MODE_PER_EVENT modules in YAML order
 * 3. All PROCESSING_MODE_BY_GALAXY modules execute in galaxy-major order:
 *    for each galaxy g:
 *      module1(galaxy g)
 *      module2(galaxy g)
 *
 * Called from process_halo_evolution() for each phase.
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
 * Implementation: Auto-generated from module metadata in module_info.yaml files
 */
void register_all_modules(void);

/* ==============================================================================
 * MODEL PARAMETER ACCESS
 * ==============================================================================
 *
 * Model parameters MUST be explicitly specified in the input YAML file.
 * NO defaults are used.
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
 * Intended for use in module init() functions to enforce dependency contracts.
 *
 * @param   name         Module name to search for
 * @param   phase        Phase config array (e.g., MimicConfig.phase_1)
 * @param   num_modules  Number of entries in the phase array
 * @param   mode         Processing mode to match
 * @return  true if the module is present in the phase with the given mode
 */
bool module_configured_in_phase(const char *name,
                                const struct PhaseModuleConfig *phase,
                                int num_modules, enum ProcessingMode mode);

/**
 * @brief   Check if a module is configured in any phase with any mode
 *
 * @param   name  Module name to search for
 * @return  true if the module appears in any phase
 */
bool module_configured_anywhere(const char *name);

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
                              const struct PhaseModuleConfig *phase,
                              int num_modules);

#endif // MODULE_REGISTRY_H
