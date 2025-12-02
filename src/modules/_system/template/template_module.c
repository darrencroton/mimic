/**
 * @file    template_module.c
 * @brief   Template physics module implementation
 *
 * [REPLACE WITH YOUR MODULE DESCRIPTION]
 *
 * This module implements [DESCRIBE PHYSICS PROCESS IN DETAIL].
 *
 * Physics:
 *   [WRITE EQUATIONS]
 *   Example:
 *     ΔColdGas = f_cool * Mvir * Δt
 *   where:
 *     - f_cool = cooling efficiency (configurable parameter)
 *     - Mvir = virial mass of halo
 *     - Δt = timestep
 *
 * Implementation Notes:
 *   - [EXPLAIN KEY ALGORITHMS OR DESIGN CHOICES]
 *   - [NOTE ANY ASSUMPTIONS OR SIMPLIFICATIONS]
 *   - [REFERENCE SAGE SOURCE IF PORTING]
 *
 * Reference:
 *   - [CITE PAPERS]
 *   - SAGE: sage-code/model_*.c
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Updates GalaxyData properties only
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "template_module.h"
#include "types.h"

#include "../_system/parameter_helpers.h" // For LOAD_PARAM_*, VALIDATE_RANGE_*, etc.

// ============================================================================
// MODEL PARAMETERS
// ============================================================================

/**
 * Model parameters read from input YAML file via model_get_*() functions
 *
 * Example: If your module uses BaryonFrac and SfrEfficiency:
 *   static double baryon_frac;     // Read via model_get_double("BaryonFrac", ...)
 *   static double sfr_efficiency;  // Read via model_get_double("SfrEfficiency", ...)
 *
 * All parameters must be explicitly specified in the input YAML file (no defaults).
 * Declare parameters used by your module in module_info.yaml under dependencies.parameters
 */
static double example_param1;
static double example_param2;

/* ============================================================================
 * PHYSICS CONSTANTS (if needed)
 * ============================================================================
 * Module-specific physics constants should be inlined here with compact
 * documentation. Use shared/physics_constants.h for constants used by
 * multiple modules (e.g., RADIATIVE_EFFICIENCY, KINETIC_ENERGY_FACTOR).
 *
 * Example:
 *   static const double SOME_COEFF = 2.5;  / * Brief explanation * /
 *   static const double THRESHOLD_VALUE = 1.0e-3;  / * Units and reference * /
 */

/* ============================================================================
 * MODULE STATE (if needed)
 * ============================================================================ */

/**
 * @brief   [DESCRIBE ANY PERSISTENT MODULE DATA]
 *
 * Example: Lookup tables, cached calculations, statistics, etc.
 * Allocate in init(), free in cleanup().
 */
static double *lookup_table = NULL;
static int table_size = 0;

// ============================================================================
// HELPER FUNCTIONS (Physics Calculations)
// ============================================================================

/**
 * @brief   [HELPER FUNCTION DESCRIPTION]
 *
 * [EXPLAIN WHAT THIS FUNCTION COMPUTES]
 *
 * @param   input1  [DESCRIPTION]
 * @param   input2  [DESCRIPTION]
 * @return  [DESCRIPTION OF RETURN VALUE]
 *
 * Example usage:
 * @code
 * float result = compute_physics(mass, redshift);
 * @endcode
 */
static float compute_physics(float input1, double input2) {
  // TODO: Implement your physics calculation
  // This is a placeholder - replace with actual physics

  // Example: Simple linear relationship
  float result = example_param1 * input1 * input2;

  return result;
}

/**
 * @brief   [ANOTHER HELPER FUNCTION IF NEEDED]
 *
 * Keep physics logic in helper functions for testability.
 * These can be unit tested independently of the module system.
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
 * Called once during program startup. Responsibilities:
 * - Read model parameters (validation automatic from model_parameters.yaml)
 * - Allocate persistent memory structures
 * - Load external data files (if needed)
 * - Initialize lookup tables (if needed)
 * - Log module configuration
 *
 * @return  0 on success, non-zero on failure
 */
static int template_module_init(void) {
  // -------------------------------------------------------------------------
  // 1. Read and validate model parameters
  // -------------------------------------------------------------------------
  // All parameters are REQUIRED in the input YAML file (no defaults).
  // Declare parameters in module_info.yaml under dependencies.parameters
  //
  // Option A: Simple parameter loading (manual validation)
  //   LOAD_PARAM_DOUBLE("BaryonFrac", baryon_frac);
  //   if (baryon_frac <= 0.0 || baryon_frac > 1.0) {
  //     ERROR_LOG("BaryonFrac must be in (0, 1]");
  //     return -1;
  //   }
  //
  // Option B: Load and validate in one call (recommended for simple ranges)
  //   LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BaryonFrac", baryon_frac, 0.0, 1.0,
  //                                     "cosmic baryon fraction must be physical");
  //
  // Option C: Mode/option validation
  //   LOAD_AND_VALIDATE_OPTION("AGNrecipeOn", agn_recipe, 3,
  //                            "0=off, 1=radio, 2=quasar, 3=both");
  //
  // See src/modules/_system/parameter_helpers.h for all available macros.

  // TODO: Replace with actual parameter reads for your module
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ExampleParam1", example_param1, 0.0, 10.0,
                                    "example physics parameter");

  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ExampleParam2", example_param2, 0.0, 1.0,
                                    "example efficiency factor");

  // -------------------------------------------------------------------------
  // 2. Allocate persistent memory (if needed)
  // -------------------------------------------------------------------------

  // Example: Allocate lookup table
  table_size = 1000;
  lookup_table =
      malloc_tracked(table_size * sizeof(double), MEM_UTILITY);
  if (lookup_table == NULL) {
    ERROR_LOG("Failed to allocate lookup table");
    return -1;
  }

  // TODO: Initialize table contents
  for (int i = 0; i < table_size; i++) {
    lookup_table[i] = 0.0; // Replace with actual initialization
  }

  // -------------------------------------------------------------------------
  // 3. Load external data (if needed)
  // -------------------------------------------------------------------------

  // TODO: Load data files (cooling tables, yields, etc.)
  // if (load_data_file("data/my_table.dat", lookup_table) != 0) {
  //     ERROR_LOG("Failed to load data file");
  //     return -1;
  // }

  // -------------------------------------------------------------------------
  // 4. Log module configuration
  // -------------------------------------------------------------------------

  INFO_LOG("Template module initialized");
  INFO_LOG("  Physics: [DESCRIBE YOUR EQUATION]");
  INFO_LOG("  Parameter1 = %.3f (from config)", PARAM1);
  INFO_LOG("  Parameter2 = %.3f (from config)", PARAM2);
  INFO_LOG("  Lookup table: %d entries allocated", table_size);

  // TODO: Update physics description in log

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Called for each FOF group during tree processing. This is where galaxy
 * physics is computed. Responsibilities: - Iterate through halos in FOF group
 * - Access galaxy properties via halos[i].galaxy->PropertyName
 * - Compute physics updates using helper functions
 * - Modify galaxy properties
 * - Handle all halo types appropriately
 * - Use context for redshift-dependent physics
 *
 * @param   ctx     Module execution context (redshift, time, params)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, non-zero on failure
 */
static int template_module_process(struct ModuleContext *ctx,
                                    struct Halo *halos, int ngal) {
  // -------------------------------------------------------------------------
  // 1. Validate inputs
  // -------------------------------------------------------------------------

  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process (not an error)
  }

  // -------------------------------------------------------------------------
  // 2. Extract context information (if needed for physics)
  // -------------------------------------------------------------------------

  double z = ctx->redshift;
  double time = ctx->time;
  double hubble_h = ctx->params->Hubble_h;

  // TODO: Compute any redshift-dependent quantities
  // Example: Hubble parameter at redshift z
  // double hubble_z = hubble_h * sqrt(ctx->params->Omega * pow(1+z, 3)
  //                                   + ctx->params->OmegaLambda);

  (void)time;      // Suppress unused warning if not needed
  (void)hubble_h;  // Suppress unused warning if not needed

  // -------------------------------------------------------------------------
  // 3. Process each halo in the FOF group
  // -------------------------------------------------------------------------

  for (int i = 0; i < ngal; i++) {

    // -----------------------------------------------------------------------
    // 3a. Filter by halo type (if physics is type-specific)
    // -----------------------------------------------------------------------

    // Example: Only process central galaxies
    if (halos[i].Type != 0) {
      continue; // Skip satellites and orphans
    }

    // TODO: Adjust type filtering for your physics
    // Type 0 = Central galaxy
    // Type 1 = Satellite galaxy
    // Type 2 = Orphan galaxy (host subhalo disappeared)

    // -----------------------------------------------------------------------
    // 3b. Validate galaxy data
    // -----------------------------------------------------------------------

    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d (Type=%d) has NULL galaxy data", i, halos[i].Type);
      return -1;
    }

    // -----------------------------------------------------------------------
    // 3c. Read input properties
    // -----------------------------------------------------------------------

    // TODO: Replace with properties your physics needs
    // float input_property = halos[i].galaxy->SomeProperty;

    // Example: Read halo properties (read-only!)
    float mvir = halos[i].Mvir;
    float rvir = halos[i].Rvir;
    float vvir = halos[i].Vvir;

    // Example: Read timestep (if time evolution needed)
    float dt = halos[i].dT;

    // Validate timestep
    if (dt <= 0.0f) {
      DEBUG_LOG("Halo %d: Invalid dT=%.3f, skipping", i, dt);
      continue;
    }

    // -----------------------------------------------------------------------
    // 3d. Compute physics
    // -----------------------------------------------------------------------

    // TODO: Replace with your physics calculations
    // Use helper functions for complex logic (easier to test)

    // Example: Compute some physics quantity
    float result = compute_physics(mvir, z);

    // Example: Apply time evolution
    float delta = result * dt;

    // Suppress unused variable warnings (remove when implementing)
    (void)rvir;
    (void)vvir;
    (void)delta;

    // -----------------------------------------------------------------------
    // 3e. Update galaxy properties
    // -----------------------------------------------------------------------

    // TODO: Replace with actual property updates

    // Example: Increment a property
    // halos[i].galaxy->SomeProperty += delta;

    // Example: Deplete one property, increase another
    // halos[i].galaxy->ColdGas -= delta;
    // halos[i].galaxy->StellarMass += delta;

    // IMPORTANT: Only modify galaxy properties, NOT halo properties
    // DON'T: halos[i].Mvir = ...;  (read-only!)
    // DO:    halos[i].galaxy->Mass = ...;  (OK to modify)

    // -----------------------------------------------------------------------
    // 3f. Debug logging (use DEBUG_LOG for per-halo details)
    // -----------------------------------------------------------------------

    DEBUG_LOG("Halo %d: Mvir=%.3e, result=%.3e, z=%.3f", i, mvir, result, z);

    // TODO: Update debug log with your property names and values
  }

  return 0;
}

/**
 * @brief   Cleanup template module
 *
 * Called once during program shutdown. Responsibilities:
 * - Free all allocated memory
 * - Close open files
 * - Log final statistics (optional)
 * - Clean up module state
 *
 * @return  0 on success, non-zero on failure
 */
static int template_module_cleanup(void) {
  // -------------------------------------------------------------------------
  // 1. Free persistent memory
  // -------------------------------------------------------------------------

  if (lookup_table != NULL) {
    free_tracked(lookup_table, MEM_UTILITY);
    lookup_table = NULL;
  }

  // TODO: Free any other allocated memory

  // -------------------------------------------------------------------------
  // 2. Close files (if any were opened)
  // -------------------------------------------------------------------------

  // TODO: Close any open file handles

  // -------------------------------------------------------------------------
  // 3. Log cleanup completion
  // -------------------------------------------------------------------------

  INFO_LOG("Template module cleaned up");

  // TODO: Optionally log final statistics
  // INFO_LOG("  Total halos processed: %d", total_halos);

  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module structure for template module
 *
 * Defines this module's interface to the module system.
 * The core calls these functions at appropriate times.
 */
static struct Module template_module = {
    .name = "template_module",          // Must match parameter prefix
    .init = template_module_init,       // Called once at startup
    .process_halos = template_module_process, // Called per FOF group
    .cleanup = template_module_cleanup  // Called once at shutdown
};

/**
 * @brief   Register the template module
 *
 * Registers this module with the module registry. This function should be
 * called from src/modules/module_init.c :: register_all_modules().
 */
void template_module_register(void) {
  module_registry_add(&template_module);
}
