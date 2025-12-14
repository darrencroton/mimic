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

  if (module->init == NULL || module->process == NULL ||
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
 * Validates multi-phase pipeline configuration and initializes all referenced
 * modules. Modules are initialized in the order they appear across all phases
 * (pre_timestep → phase_1 → phase_2 → post_timestep), with duplicates
 * initialized only once.
 *
 * @return  0 on success, non-zero if initialization fails
 */
/**
 * @brief   Helper to add module to execution pipeline if not already present
 *
 * @param   module_name  Name of module to add
 */
static void add_module_to_pipeline(const char *module_name) {
  /* Check if already in pipeline */
  for (int i = 0; i < num_pipeline_modules; i++) {
    if (strcmp(execution_pipeline[i]->name, module_name) == 0) {
      return; /* Already added */
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
    exit(EXIT_FAILURE);
  }

  /* Add to execution pipeline */
  if (num_pipeline_modules >= MAX_MODULES) {
    ERROR_LOG("Too many unique modules in pipeline (max %d)", MAX_MODULES);
    exit(EXIT_FAILURE);
  }
  execution_pipeline[num_pipeline_modules++] = mod;
  DEBUG_LOG("Added module to pipeline: %s", module_name);
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

/**
 * @brief   Format supported modes for error messages
 *
 * @param   mod  Module whose supported modes to format
 * @return  Static string with mode names (e.g., "process_full_halo, process_by_galaxy")
 */
static const char *format_supported_modes(const struct Module *mod) {
  static char buffer[128];
  buffer[0] = '\0';

  for (int i = 0; i < mod->num_supported_modes; i++) {
    if (i > 0)
      strcat(buffer, ", ");
    strcat(buffer,
           mod->supported_processing_modes[i] == PROCESSING_MODE_FULL_HALO ? "process_full_halo" : "process_by_galaxy");
  }
  return buffer;
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
static int validate_phase_processing_modes(struct PhaseModuleConfig *config,
                                           int num_modules,
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
      const char *mode_str =
          (config[i].processing_mode == PROCESSING_MODE_FULL_HALO) ? "process_full_halo" : "process_by_galaxy";

      ERROR_LOG("Configuration error in phase '%s':", phase_name);
      ERROR_LOG("  Module '%s' does not support processing mode '%s'", mod->name,
                mode_str);
      ERROR_LOG("  Supported modes: %s", format_supported_modes(mod));
      ERROR_LOG("  Fix: Change processing mode in input YAML to one of the "
                "supported modes");
      return -1;
    }
  }
  return 0;
}

int module_system_init(void) {
  INFO_LOG("Initializing multi-phase module system");

  /* Build execution pipeline by collecting all unique modules across phases */
  num_pipeline_modules = 0;

  /* Collect modules from all phases in execution order */
  for (int i = 0; i < MimicConfig.num_pre_timestep; i++) {
    add_module_to_pipeline(MimicConfig.pre_timestep[i].module_name);
  }
  for (int i = 0; i < MimicConfig.num_phase_1; i++) {
    add_module_to_pipeline(MimicConfig.phase_1[i].module_name);
  }
  for (int i = 0; i < MimicConfig.num_phase_2; i++) {
    add_module_to_pipeline(MimicConfig.phase_2[i].module_name);
  }
  for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
    add_module_to_pipeline(MimicConfig.post_timestep[i].module_name);
  }

  if (num_pipeline_modules == 0) {
    INFO_LOG("No modules configured (physics-free mode)");
    INFO_LOG("SubSteps = %d", MimicConfig.SubSteps);
    return 0;
  }

  INFO_LOG("Pipeline configuration:");
  INFO_LOG("  SubSteps: %d", MimicConfig.SubSteps);
  INFO_LOG("  Pre-timestep: %d module(s)", MimicConfig.num_pre_timestep);
  INFO_LOG("  Phase 1: %d module(s)", MimicConfig.num_phase_1);
  INFO_LOG("  Phase 2: %d module(s)", MimicConfig.num_phase_2);
  INFO_LOG("  Post-timestep: %d module(s)", MimicConfig.num_post_timestep);
  INFO_LOG("  Total unique modules: %d", num_pipeline_modules);

  /* Validate processing mode configurations */
  INFO_LOG("Validating module processing mode configurations...");

  if (validate_phase_processing_modes(MimicConfig.pre_timestep,
                                      MimicConfig.num_pre_timestep,
                                      "pre_timestep") != 0) {
    return -1;
  }

  if (validate_phase_processing_modes(MimicConfig.phase_1, MimicConfig.num_phase_1,
                                      "phase_1") != 0) {
    return -1;
  }

  if (validate_phase_processing_modes(MimicConfig.phase_2, MimicConfig.num_phase_2,
                                      "phase_2") != 0) {
    return -1;
  }

  if (validate_phase_processing_modes(MimicConfig.post_timestep,
                                      MimicConfig.num_post_timestep,
                                      "post_timestep") != 0) {
    return -1;
  }

  INFO_LOG("Processing mode validation passed");

  /* Initialize all modules in pipeline order */
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
 * @brief   Execute modules in a specific phase
 *
 * Core execution engine for multi-phase pipeline. Implements galaxy-major
 * loop for PROCESSING_MODE_BY_GALAXY modules (better cache locality, matches SAGE).
 *
 * Execution order within phase:
 * 1. PROCESSING_MODE_FULL_HALO modules: called with full halo array
 * 2. PROCESSING_MODE_BY_GALAXY modules: galaxy-major order
 *    for each galaxy g:
 *      module1(galaxy g)
 *      module2(galaxy g)
 *
 * @param   phase_config   Array of module configurations for this phase
 * @param   num_modules    Number of modules in this phase (0 = skip)
 * @param   ctx            Module execution context
 * @param   halos          Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal           Number of halos in the array
 */
void execute_phase(struct PhaseModuleConfig *phase_config, int num_modules,
                   struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (num_modules == 0 || halos == NULL || ngal <= 0) {
    return; // Empty phase or nothing to process
  }

  /* PASS 1: PROCESSING_MODE_FULL_HALO modules (full halo array - executes first) */
  for (int i = 0; i < num_modules; i++) {
    if (phase_config[i].processing_mode != PROCESSING_MODE_FULL_HALO) {
      continue; // Skip PROCESSING_MODE_BY_GALAXY modules in this pass
    }

    /* Find module by name */
    struct Module *mod = find_module_by_name(phase_config[i].module_name);
    if (mod == NULL) {
      ERROR_LOG("Module '%s' configured but not registered",
                  phase_config[i].module_name);
      exit(EXIT_FAILURE);
    }

    /* Call module with full halo array */
    DEBUG_LOG("Executing module: %s (full array, ngal=%d, substep %d/%d, "
              "z=%.3f)",
              mod->name, ngal, ctx->substep_number + 1, ctx->num_substeps,
              ctx->redshift);

    int result = mod->process(ctx, halos, ngal);
    if (result != 0) {
      ERROR_LOG("Module '%s' failed (substep %d)", mod->name,
                ctx->substep_number);
      exit(EXIT_FAILURE);
    }
  }

  /* PASS 2: PROCESSING_MODE_BY_GALAXY modules (galaxy-major loop - executes after FULL_HALO) */
  for (int g = 0; g < ngal; g++) {
    /* Skip halos without galaxies or already merged */
    if (halos[g].galaxy == NULL || halos[g].Type == 3) {
      continue;
    }

    /* Execute all PROCESSING_MODE_BY_GALAXY modules for this galaxy */
    for (int i = 0; i < num_modules; i++) {
      if (phase_config[i].processing_mode != PROCESSING_MODE_BY_GALAXY) {
        continue; // Skip PROCESSING_MODE_FULL_HALO modules (already done)
      }

      /* Find module by name */
      struct Module *mod = find_module_by_name(phase_config[i].module_name);
      if (mod == NULL) {
        ERROR_LOG("Module '%s' configured but not registered",
                  phase_config[i].module_name);
        exit(EXIT_FAILURE);
      }

      /* Call module with single galaxy (ngal=1) */
      DEBUG_LOG("Executing module: %s (galaxy %d/%d, substep %d/%d, z=%.3f)",
                mod->name, g, ngal, ctx->substep_number + 1, ctx->num_substeps,
                ctx->redshift);

      int result = mod->process(ctx, &halos[g], 1);
      if (result != 0) {
        ERROR_LOG("Module '%s' failed on galaxy %d (substep %d)", mod->name, g,
                  ctx->substep_number);
        exit(EXIT_FAILURE);
      }
    }
  }
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

  // Free phase configuration arrays and their module name strings
  if (MimicConfig.pre_timestep) {
    for (int i = 0; i < MimicConfig.num_pre_timestep; i++) {
      if (MimicConfig.pre_timestep[i].module_name) {
        free((void *)MimicConfig.pre_timestep[i].module_name);
      }
    }
    myfree(MimicConfig.pre_timestep);
    MimicConfig.pre_timestep = NULL;
  }

  if (MimicConfig.phase_1) {
    for (int i = 0; i < MimicConfig.num_phase_1; i++) {
      if (MimicConfig.phase_1[i].module_name) {
        free((void *)MimicConfig.phase_1[i].module_name);
      }
    }
    myfree(MimicConfig.phase_1);
    MimicConfig.phase_1 = NULL;
  }

  if (MimicConfig.phase_2) {
    for (int i = 0; i < MimicConfig.num_phase_2; i++) {
      if (MimicConfig.phase_2[i].module_name) {
        free((void *)MimicConfig.phase_2[i].module_name);
      }
    }
    myfree(MimicConfig.phase_2);
    MimicConfig.phase_2 = NULL;
  }

  if (MimicConfig.post_timestep) {
    for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
      if (MimicConfig.post_timestep[i].module_name) {
        free((void *)MimicConfig.post_timestep[i].module_name);
      }
    }
    myfree(MimicConfig.post_timestep);
    MimicConfig.post_timestep = NULL;
  }

  INFO_LOG("Module system cleanup complete");
  return result;
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
 * @param   param_name   Parameter name (e.g., "AGNrecipeOn")
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
