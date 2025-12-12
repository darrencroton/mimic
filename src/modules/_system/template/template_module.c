/**
 * @file    template_module.c
 * @brief   Template physics module implementation (v3.0 Multi-Phase Pipeline API)
 *
 * [Brief description of physics process - 1-2 sentences]
 *
 * Physics: [Key equation]
 *
 * Key functions:
 * - compute_physics(): [Brief description]
 *
 * Reference: [Citation and/or SAGE source]
 *
 * ============================================================================
 * MULTI-PHASE PIPELINE API v3.0 KEY POINTS:
 * ============================================================================
 * - Use ctx->substep_dt for time integration (NOT halos[i].dT)
 * - Access substep info: ctx->substep_number, ctx->num_substeps
 * - Processing modes (choose based on physics):
 *   * PROCESSING_MODE_FULL_HALO: Process full array (ngal > 1) for group-level physics
 *   * PROCESSING_MODE_BY_GALAXY: Process one galaxy (ngal = 1) for per-galaxy physics
 * - Declare supported modes via module_info.yaml: supported_processing_modes: [process_full_halo, process_by_galaxy]
 * - Module struct must include .supported_processing_modes and .num_supported_modes
 * - See examples below in template_module_process() for both processing mode patterns
 */

#include <math.h>
#include <stdio.h>   /* Required for error.h logging macros */
#include <stdlib.h>  /* Required for error.h logging macros */

#include "constants.h"
#include "error.h"
#include "memory.h"
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
  lookup_table = mymalloc_cat(table_size * sizeof(double), MEM_UTILITY);
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
  VERBOSE_LOG("  Physics: [DESCRIBE YOUR EQUATION]");
  VERBOSE_LOG("  Parameter1 = %.3f", example_param1);
  VERBOSE_LOG("  Parameter2 = %.3f", example_param2);
  VERBOSE_LOG("  Lookup table: %d entries", table_size);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Compute galaxy physics for each halo. This function is called by the
 * multi-phase pipeline in the configured phase and loop mode.
 *
 * LOOP MODE USAGE:
 * - PROCESSING_MODE_FULL_HALO: Module processes entire halo array (ngal > 1)
 *   Use when physics requires global information (e.g., summing over all galaxies,
 *   central-satellite interactions, group-level properties).
 *   Example: Calculating total infall budget for FOF group
 *
 * - PROCESSING_MODE_BY_GALAXY: Module processes one galaxy at a time (ngal = 1)
 *   Use when physics is per-galaxy and independent (e.g., cooling, star formation).
 *   Provides better cache locality in galaxy-major loop execution.
 *   Example: Cooling rates that depend only on individual galaxy properties
 *
 * Choose processing mode in module_info.yaml based on your physics requirements.
 * If your module supports both, declare: supported_processing_modes: [process_full_halo, process_by_galaxy]
 *
 * @param   ctx     Module execution context (redshift, time, substep info, params)
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos (1 if processing_mode=BY_GALAXY, >1 if processing_mode=FULL_HALO)
 * @return  0 on success, non-zero on failure
 */
static int template_module_process(struct ModuleContext *ctx,
                                    struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // Extract simulation context (if needed)
  double z = ctx->redshift;
  double time = ctx->time;
  double hubble_h = ctx->params->Hubble_h;

  // Access substep information for time integration
  // Use ctx->substep_dt instead of halos[i].dT for multi-phase pipeline
  double dt = ctx->substep_dt;
  int substep = ctx->substep_number;
  int num_substeps = ctx->num_substeps;

  (void)time;
  (void)hubble_h;
  (void)substep;
  (void)num_substeps;

  // ========== EXAMPLE 1: PROCESSING_MODE_FULL_HALO ==========
  // Process entire array when global information is needed
  // Uncomment this block if your module uses PROCESSING_MODE_FULL_HALO
  /*
  // Find central galaxy index
  int central_idx = ctx->central_index;

  // Compute group-level properties
  double total_mass = 0.0;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy != NULL) {
      total_mass += halos[i].Mvir;
    }
  }

  // Update central galaxy based on group properties
  if (halos[central_idx].galaxy != NULL) {
    float delta = compute_physics(total_mass, z) * dt;
    halos[central_idx].galaxy->SomeProperty += delta;

    DEBUG_LOG("Central: total_mass=%.3e, delta=%.3e, z=%.3f, substep=%d/%d",
             total_mass, delta, z, substep + 1, num_substeps);
  }
  */

  // ========== EXAMPLE 2: PROCESSING_MODE_BY_GALAXY ==========
  // Process each halo independently (ngal will be 1 in PROCESSING_MODE_BY_GALAXY)
  // This is the standard pattern for per-galaxy physics
  for (int i = 0; i < ngal; i++) {

    // Filter by halo type if needed (Type 0=central, 1=satellite, 2=orphan)
    // Many modules only operate on central galaxies
    if (halos[i].Type != 0) {
      continue;
    }

    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d (Type=%d) has NULL galaxy data", i, halos[i].Type);
      return -1;
    }

    // Read halo properties (read-only)
    float mvir = halos[i].Mvir;
    float rvir = halos[i].Rvir;
    float vvir = halos[i].Vvir;

    // Validate inputs
    if (dt <= 0.0) {
      DEBUG_LOG("Halo %d: Invalid substep_dt=%.3f, skipping", i, dt);
      continue;
    }

    // TODO: Compute physics using helper functions
    float result = compute_physics(mvir, z);
    float delta = result * dt;  // Scale by timestep for integration

    (void)rvir;
    (void)vvir;
    (void)delta;

    // TODO: Update galaxy properties (galaxy struct is mutable, halo struct is not)
    // halos[i].galaxy->SomeProperty += delta;

    DEBUG_LOG("Halo %d: Mvir=%.3e, result=%.3e, delta=%.3e, z=%.3f, substep=%d/%d",
             i, mvir, result, delta, z, substep + 1, num_substeps);
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
    myfree(lookup_table);
    lookup_table = NULL;
  }

  // TODO: Close files if any were opened

  INFO_LOG("Template module cleaned up");

  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief Supported processing modes for this module
 *
 * This array is auto-generated by the module system based on the
 * supported_processing_modes field in module_info.yaml. If not specified,
 * defaults to supporting both modes: [PROCESSING_MODE_FULL_HALO, PROCESSING_MODE_BY_GALAXY]
 *
 * To restrict your module to a specific processing mode, add to module_info.yaml:
 *   supported_processing_modes: [process_full_halo]   # Only PROCESSING_MODE_FULL_HALO
 *   supported_processing_modes: [process_by_galaxy]   # Only PROCESSING_MODE_BY_GALAXY
 *   supported_processing_modes: [process_full_halo, process_by_galaxy]  # Both modes (default)
 *
 * The core validates at runtime that configured processing modes match supported modes.
 */
extern const enum ProcessingMode template_module_supported_modes[];

static struct Module template_module = {
    .name = "template_module",
    .init = template_module_init,
    .process = template_module_process,
    .cleanup = template_module_cleanup,
    .supported_processing_modes = template_module_supported_modes,
    .num_supported_modes = 2  /* Must match count in module_info.yaml supported_processing_modes */
};

void template_module_register(void) {
  module_registry_add(&template_module);
}
