/**
 * @file    template_module.c
 * @brief   Template physics module implementation
 *
 * [Brief description of physics process - 1-2 sentences]
 *
 * Physics: [Key equation]
 *
 * Key functions:
 * - compute_physics(): [Brief description]
 *
 * Reference: [Citation and/or SAGE source]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "template_module.h"
#include "types.h"

#include "../_system/parameter_helpers.h"

// ============================================================================
// MODEL PARAMETERS
// ============================================================================

// Parameters read from YAML via model_get_*() functions
// Declare in module_info.yaml under dependencies.parameters
static double example_param1;
static double example_param2;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

// Module-specific constants (use _shared/physics_constants.h for shared values)
// Example: static const double SOME_COEFF = 2.5;  // Brief explanation

// ============================================================================
// MODULE STATE
// ============================================================================

// Persistent module data (lookup tables, cached calculations, etc.)
// Allocate in init(), free in cleanup()
static double *lookup_table = NULL;
static int table_size = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   [Brief description of physics calculation]
 *
 * @param   input1  [Description]
 * @param   input2  [Description]
 * @return  [Description]
 */
static float compute_physics(float input1, double input2) {
  // TODO: Implement physics calculation
  float result = example_param1 * input1 * input2;
  return result;
}

/**
 * @brief   [Another helper if needed]
 *
 * Keep physics logic in helpers for testability.
 */
static float another_helper(float x) {
  // TODO: Implement
  return x * example_param2;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize template module
 *
 * Load parameters, allocate memory, initialize lookup tables, log configuration.
 *
 * @return  0 on success, non-zero on failure
 */
static int template_module_init(void) {
  // Load and validate parameters (declare in module_info.yaml)
  // See parameter_helpers.h for LOAD_PARAM_*, VALIDATE_*, LOAD_AND_VALIDATE_* macros

  // TODO: Replace with actual parameters
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ExampleParam1", example_param1, 0.0, 10.0,
                                    "example physics parameter");
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ExampleParam2", example_param2, 0.0, 1.0,
                                    "example efficiency factor");

  // Allocate persistent memory (if needed)
  table_size = 1000;
  lookup_table = malloc_tracked(table_size * sizeof(double), MEM_UTILITY);
  if (lookup_table == NULL) {
    ERROR_LOG("Failed to allocate lookup table");
    return -1;
  }

  // TODO: Initialize table contents
  for (int i = 0; i < table_size; i++) {
    lookup_table[i] = 0.0;
  }

  // TODO: Load external data files if needed

  // Log configuration
  INFO_LOG("Template module initialized");
  INFO_LOG("  Physics: [DESCRIBE YOUR EQUATION]");
  INFO_LOG("  Parameter1 = %.3f", example_param1);
  INFO_LOG("  Parameter2 = %.3f", example_param2);
  INFO_LOG("  Lookup table: %d entries", table_size);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Compute galaxy physics for each halo.
 *
 * @param   ctx     Module execution context (redshift, time, params)
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int template_module_process(struct ModuleContext *ctx,
                                    struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // Extract context (if needed)
  double z = ctx->redshift;
  double time = ctx->time;
  double hubble_h = ctx->params->Hubble_h;

  (void)time;
  (void)hubble_h;

  // Process each halo
  for (int i = 0; i < ngal; i++) {

    // Filter by halo type if needed (Type 0=central, 1=satellite, 2=orphan)
    if (halos[i].Type != 0) {
      continue;
    }

    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d (Type=%d) has NULL galaxy data", i, halos[i].Type);
      return -1;
    }

    // Read properties (halo properties are read-only)
    float mvir = halos[i].Mvir;
    float rvir = halos[i].Rvir;
    float vvir = halos[i].Vvir;
    float dt = halos[i].dT;

    if (dt <= 0.0f) {
      DEBUG_LOG("Halo %d: Invalid dT=%.3f, skipping", i, dt);
      continue;
    }

    // TODO: Compute physics using helper functions
    float result = compute_physics(mvir, z);
    float delta = result * dt;

    (void)rvir;
    (void)vvir;
    (void)delta;

    // TODO: Update galaxy properties (not halo properties)
    // halos[i].galaxy->SomeProperty += delta;

    DEBUG_LOG("Halo %d: Mvir=%.3e, result=%.3e, z=%.3f", i, mvir, result, z);
  }

  return 0;
}

/**
 * @brief   Cleanup template module
 *
 * Free memory, close files, log final statistics.
 *
 * @return  0 on success, non-zero on failure
 */
static int template_module_cleanup(void) {
  // Free persistent memory
  if (lookup_table != NULL) {
    free_tracked(lookup_table, MEM_UTILITY);
    lookup_table = NULL;
  }

  // TODO: Close files if any were opened

  INFO_LOG("Template module cleaned up");

  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

static struct Module template_module = {
    .name = "template_module",
    .init = template_module_init,
    .process = template_module_process,
    .cleanup = template_module_cleanup
};

void template_module_register(void) {
  module_registry_add(&template_module);
}
