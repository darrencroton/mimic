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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"

/** Maximum number of modules that can be registered */
#define MAX_MODULES 32

/** Array of all registered modules (available for use) */
static struct Module *registered_modules[MAX_MODULES];

/** Number of currently registered modules */
static int num_registered_modules = 0;

/** Array of enabled modules in execution order (runtime configuration) */
static struct Module *execution_pipeline[MAX_MODULES];

/** Number of modules in execution pipeline */
static int num_pipeline_modules = 0;

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

  // Check for duplicate names
  if (find_module_by_name(module->name) != NULL) {
    ERROR_LOG("Module '%s' is already registered", module->name);
    exit(EXIT_FAILURE);
  }

  registered_modules[num_registered_modules] = module;
  num_registered_modules++;

  DEBUG_LOG("Registered module: %s", module->name);
}

/**
 * @brief   Initialize the module system
 *
 * Builds the execution pipeline based on runtime configuration from
 * MimicConfig.EnabledModules. Then calls init() on all enabled modules in
 * configured order.
 *
 * @return  0 on success, non-zero if initialization fails
 */
int module_system_init(void) {
  INFO_LOG("Initializing module system");

  // Phase 3.4: Build execution pipeline from MimicConfig.EnabledModules
  num_pipeline_modules = 0;

  if (MimicConfig.NumEnabledModules == 0) {
    INFO_LOG("No modules enabled (physics-free mode)");
    return 0;
  }

  // Build execution pipeline from configured module list
  for (int i = 0; i < MimicConfig.NumEnabledModules; i++) {
    const char *module_name = MimicConfig.EnabledModules[i];
    struct Module *mod = find_module_by_name(module_name);

    if (mod == NULL) {
      ERROR_LOG("Module '%s' listed in EnabledModules but not registered",
                module_name);
      ERROR_LOG("Available modules:");
      for (int j = 0; j < num_registered_modules; j++) {
        ERROR_LOG("  - %s", registered_modules[j]->name);
      }
      return -1;
    }

    execution_pipeline[num_pipeline_modules++] = mod;
    DEBUG_LOG("Added module to pipeline: %s", module_name);
  }

  INFO_LOG("Enabling %d module(s)", num_pipeline_modules);

  // Initialize all enabled modules in configured order
  for (int i = 0; i < num_pipeline_modules; i++) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Initializing module: %s", mod->name);

    int result = mod->init();
    if (result != 0) {
      ERROR_LOG("Module '%s' initialization failed with code %d", mod->name,
                result);
      return result;
    }
  }

  INFO_LOG("Module system initialized successfully");
  return 0;
}

/**
 * @brief   Execute all enabled modules on a FOF group
 *
 * Calls process_halos() on all enabled modules in configured order.
 *
 * @param   halonr  Index of the main halo in InputTreeHalos (for context)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, non-zero if any module processing fails
 */
int module_execute_pipeline(int halonr, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process
  }

  if (num_pipeline_modules == 0) {
    return 0; // No modules enabled
  }

  // Populate module execution context
  struct ModuleContext ctx;
  int snap = InputTreeHalos[halonr].SnapNum;
  ctx.redshift = ZZ[snap];
  ctx.time = Age[snap];
  ctx.params = &MimicConfig;

  // Execute each enabled module in order
  for (int i = 0; i < num_pipeline_modules; i++) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Executing module: %s (ngal=%d, z=%.3f)", mod->name, ngal,
              ctx.redshift);

    int result = mod->process_halos(&ctx, halos, ngal);
    if (result != 0) {
      ERROR_LOG("Module '%s' processing failed with code %d", mod->name,
                result);
      return result;
    }
  }

  return 0;
}

/**
 * @brief   Cleanup the module system
 *
 * Calls cleanup() on all initialized modules in reverse order.
 *
 * @return  0 on success, non-zero if any module cleanup fails
 */
int module_system_cleanup(void) {
  if (num_pipeline_modules == 0) {
    INFO_LOG("Module system cleanup complete (no modules were enabled)");
    return 0;
  }

  INFO_LOG("Cleaning up %d module(s)", num_pipeline_modules);

  int result = 0;

  // Cleanup in reverse order
  for (int i = num_pipeline_modules - 1; i >= 0; i--) {
    struct Module *mod = execution_pipeline[i];
    DEBUG_LOG("Cleaning up module: %s", mod->name);

    int cleanup_result = mod->cleanup();
    if (cleanup_result != 0) {
      ERROR_LOG("Module '%s' cleanup failed with code %d", mod->name,
                cleanup_result);
      result = cleanup_result; // Continue cleanup but record failure
    }
  }

  INFO_LOG("Module system cleanup complete");
  return result;
}

/* ==============================================================================
 * MODEL PARAMETER ACCESS (Phase 4.4)
 * ============================================================================== */

/**
 * @brief   Get required model parameter as double
 *
 * Model parameters are defined in model_parameters.yaml and MUST be specified
 * in the input YAML file. NO defaults are used - parameters are REQUIRED.
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
  // Search for parameter in MimicConfig.ModelParams[]
  // Note: MimicConfig.ModelParams[] stores all model_parameters from input file
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      // Found - parse to double with error checking
      char *endptr;
      errno = 0;
      *out_value = strtod(MimicConfig.ModelParams[i].value, &endptr);

      // Check for conversion errors
      if (errno != 0) {
        ERROR_LOG("Model parameter '%s': conversion error for value '%s' (%s)",
                 param_name, MimicConfig.ModelParams[i].value, strerror(errno));
        return -1;
      }
      if (endptr == MimicConfig.ModelParams[i].value) {
        ERROR_LOG("Model parameter '%s': no digits found in value '%s'",
                 param_name, MimicConfig.ModelParams[i].value);
        return -1;
      }
      if (*endptr != '\0') {
        ERROR_LOG("Model parameter '%s': invalid characters after number in '%s'",
                 param_name, MimicConfig.ModelParams[i].value);
        return -1;
      }

      return 0;  // Success
    }
  }

  // NOT FOUND - this is a fatal error (all model parameters are required)
  ERROR_LOG("Required model parameter '%s' not found in input file", param_name);
  ERROR_LOG("All model parameters must be specified in the 'model_parameters:' section");
  return -1;
}

/**
 * @brief   Get required model parameter as integer
 *
 * Model parameters are defined in model_parameters.yaml and MUST be specified
 * in the input YAML file. NO defaults are used - parameters are REQUIRED.
 *
 * Vision Principle 4 (Single Source of Truth): Input file is the truth for values.
 *
 * @param   param_name   Parameter name (e.g., "AGNrecipeOn")
 * @param   out_value    Output pointer for integer value
 * @return  0 on success, -1 if parameter missing or invalid
 */
int model_get_int(const char *param_name, int *out_value) {
  // Search for parameter in MimicConfig.ModelParams[]
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      // Found - parse to int with error checking
      char *endptr;
      errno = 0;
      long val = strtol(MimicConfig.ModelParams[i].value, &endptr, 10);

      // Check for conversion errors
      if (errno != 0) {
        ERROR_LOG("Model parameter '%s': conversion error for value '%s' (%s)",
                 param_name, MimicConfig.ModelParams[i].value, strerror(errno));
        return -1;
      }
      if (endptr == MimicConfig.ModelParams[i].value) {
        ERROR_LOG("Model parameter '%s': no digits found in value '%s'",
                 param_name, MimicConfig.ModelParams[i].value);
        return -1;
      }
      if (*endptr != '\0') {
        ERROR_LOG("Model parameter '%s': invalid characters after number in '%s'",
                 param_name, MimicConfig.ModelParams[i].value);
        return -1;
      }

      // Check for int overflow
      if (val < INT_MIN || val > INT_MAX) {
        ERROR_LOG("Model parameter '%s': value %ld out of int range",
                 param_name, val);
        return -1;
      }

      *out_value = (int)val;

      return 0;  // Success
    }
  }

  // NOT FOUND - this is a fatal error (all model parameters are required)
  ERROR_LOG("Required model parameter '%s' not found in input file", param_name);
  ERROR_LOG("All model parameters must be specified in the 'model_parameters:' section");
  return -1;
}

/**
 * @brief   Get required model parameter as string
 *
 * Model parameters are defined in model_parameters.yaml and MUST be specified
 * in the input YAML file. NO defaults are used - parameters are REQUIRED.
 *
 * Vision Principle 4 (Single Source of Truth): Input file is the truth for values.
 *
 * @param   param_name   Parameter name (e.g., "CoolFunctionsDir")
 * @param   out_value    Output buffer for string value
 * @param   max_len      Maximum length of output buffer
 * @return  0 on success, -1 if parameter missing
 */
int model_get_string(const char *param_name, char *out_value, size_t max_len) {
  // Search for parameter in MimicConfig.ModelParams[]
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, param_name) == 0) {
      // Found - copy string value
      strncpy(out_value, MimicConfig.ModelParams[i].value, max_len - 1);
      out_value[max_len - 1] = '\0';
      return 0;  // Success
    }
  }

  // NOT FOUND - this is a fatal error (all model parameters are required)
  ERROR_LOG("Required model parameter '%s' not found in input file", param_name);
  ERROR_LOG("All model parameters must be specified in the 'model_parameters:' section");
  return -1;
}
