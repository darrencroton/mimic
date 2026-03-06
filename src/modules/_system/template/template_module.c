/**
 * @file    template_module.c
 * @brief   TODO: One-line description of your module's physics
 *
 * TODO: 2-3 sentence description of the physics this module implements
 *
 * Physics: TODO: Key equation or physical process
 *
 * Reference: TODO: Citation if applicable
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "memory.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

// NOTE: For directory modules (src/modules/my_module/my_module.c):
//       Use: #include "_system/parameter_helpers.h"
// NOTE: For standalone modules (src/modules/my_module.c):
//       Use: #include "_system/parameter_helpers.h"
#include "_system/parameter_helpers.h"

// ============================================================================
// MODEL PARAMETERS
// ============================================================================

// TODO: Declare static variables for parameters loaded from YAML
// Example:
// static double my_efficiency;
// static int my_option;

// ============================================================================
// MODULE STATE (if needed)
// ============================================================================

// TODO: Declare persistent module data (lookup tables, cached values, etc.)
// Example:
// static double *lookup_table = NULL;
// static int table_size = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// TODO: Implement physics calculations as helper functions
// Example:
// static float compute_my_physics(float input, double dt) {
//     return my_efficiency * input * dt;
// }

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize template module
 *
 * TODO: Load parameters, allocate memory, initialize lookup tables
 *
 * @return  0 on success, non-zero on failure
 */
int template_module_init(void) {
  // TODO: Load and validate parameters
  // LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("MyParam", my_param, 0.0, 1.0, "description");
  // LOAD_PARAM_INT("MyOption", my_option);

  // TODO: Allocate persistent memory if needed
  // table_size = 1000;
  // lookup_table = mymalloc_cat(table_size * sizeof(double), MEM_UTILITY);

  // TODO: Log initialization
  // INFO_LOG("My Module initialized");
  // VERBOSE_LOG("  MyParam = %.3f", my_param);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * TODO: Implement your physics here
 *
 * @param   ctx     Module execution context (redshift, time, substep info, params)
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos (1 if process_by_galaxy, >1 if process_full_halo)
 * @return  0 on success, non-zero on failure
 */
int template_module_process(struct ModuleContext *ctx,
                                    struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // TODO: Extract context if needed
  // double z = ctx->redshift;
  // double dt = ctx->substep_dt;  // Use this for time integration!

  // TODO: Implement your physics
  // For process_by_galaxy mode (ngal=1):
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL) {
      continue;
    }

    struct GalaxyData *gal = halos[i].galaxy;

    // TODO: Read input properties
    // float cold_gas = gal->ColdGas;

    // TODO: Compute physics
    // float result = compute_my_physics(cold_gas, dt);

    // TODO: Update output properties
    // gal->StellarMass += result;

    // TODO: Add debug logging if needed
    // DEBUG_LOG("Halo %d: result=%.3e", i, result);
  }

  return 0;
}

/**
 * @brief   Cleanup template module
 *
 * TODO: Free memory, close files, log final statistics
 *
 * @return  0 on success, non-zero on failure
 */
int template_module_cleanup(void) {
  // TODO: Free persistent memory
  // if (lookup_table != NULL) {
  //   myfree(lookup_table);
  //   lookup_table = NULL;
  // }

  // VERBOSE_LOG("My Module cleaned up");

  return 0;
}
