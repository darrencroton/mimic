/**
 * @file    simple_cooling.c
 * @brief   Simple cooling module implementation
 *
 * This module implements a simple cooling prescription: cold gas accumulates
 * from newly accreted dark matter at a rate proportional to the baryon
 * fraction.
 *
 * Physics:
 *   ΔColdGas = f_baryon * ΔMvir
 *
 * where:
 *   - f_baryon = 0.15 (cosmic baryon fraction, configurable in Phase 3.6)
 *   - ΔMvir = Mvir(current) - Mvir(progenitor)  [from struct Halo.deltaMvir]
 *   - ColdGas accumulates over time via progenitor inheritance
 *
 * This is a placeholder module for testing module system infrastructure:
 * - Phase 3: Runtime configuration and parameter reading
 * - Module interaction (cooling provides gas for star formation)
 * - Property communication (writes ColdGas for stellar_mass to read)
 *
 * A realistic cooling module with physics tables will be implemented in Phase
 * 6.
 */

#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "simple_cooling.h"
#include "types.h"

/**
 * @brief   Baryon fraction parameter
 *
 * Fraction of accreted halo mass that becomes cold gas.
 * Cosmic value: Ω_b / Ω_m ≈ 0.15
 *
 * Read from configuration file via SimpleCooling_BaryonFraction parameter.
 */
static double BARYON_FRACTION = 0.15; /* Default value */

/**
 * @brief   Initialize simple cooling module
 *
 * Called once during program startup. Reads module parameters from
 * configuration and logs module configuration.
 *
 * @return  0 on success
 */
static int simple_cooling_init(void) {
  // Read module parameters from configuration
  module_get_double("SimpleCooling", "BaryonFraction", &BARYON_FRACTION, 0.15);

  INFO_LOG("Simple cooling module initialized");
  INFO_LOG("  Physics: ΔColdGas = %.3f * ΔMvir", BARYON_FRACTION);
  INFO_LOG("  BaryonFraction = %.3f (from config)", BARYON_FRACTION);
  INFO_LOG("  Note: This is a placeholder module for infrastructure testing");
  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Computes cold gas accumulation for all halos in the group using:
 *   ΔColdGas = BARYON_FRACTION * deltaMvir
 *   ColdGas += ΔColdGas
 *
 * Only processes central galaxies (Type == 0). For halos that have grown
 * (deltaMvir > 0), converts a fraction of the accreted mass to cold gas.
 *
 * @param   ctx     Module execution context (provides redshift, time, params)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, -1 on error
 */
static int simple_cooling_process(struct ModuleContext *ctx, struct Halo *halos,
                                   int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process
  }

  // Context available for redshift-dependent physics (not used in this simple
  // model)
  (void)ctx;

  // Process each halo in the FOF group
  for (int i = 0; i < ngal; i++) {
    // Only process central galaxies
    if (halos[i].Type != 0) {
      continue;
    }

    // Validate galaxy data is allocated
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d (Type=0) has NULL galaxy data", i);
      return -1;
    }

    // Get mass accreted since last timestep
    // deltaMvir is calculated in build_model.c:
    //   deltaMvir = Mvir(current) - Mvir(progenitor)
    // Can be negative if halo loses mass
    float delta_mvir = halos[i].deltaMvir;

    // Only cool gas if halo is growing
    if (delta_mvir > 0.0f) {
      // Calculate newly cooled gas
      float delta_cold_gas = BARYON_FRACTION * delta_mvir;

      // Accumulate cold gas
      // (ColdGas from progenitor was already inherited via deep copy)
      halos[i].galaxy->ColdGas += delta_cold_gas;

      DEBUG_LOG("Halo %d: ΔMvir=%.3e -> ΔColdGas=%.3e, total ColdGas=%.3e "
                "(z=%.3f)",
                i, delta_mvir, delta_cold_gas, halos[i].galaxy->ColdGas,
                ctx->redshift);
    }
  }

  return 0;
}

/**
 * @brief   Cleanup simple cooling module
 *
 * Called once during program shutdown. No resources to free in this simple
 * module.
 *
 * @return  0 on success
 */
static int simple_cooling_cleanup(void) {
  INFO_LOG("Simple cooling module cleaned up");
  return 0;
}

/**
 * @brief   Module structure for simple cooling module
 *
 * Defines this module's interface to the module system.
 */
static struct Module simple_cooling_module = {
    .name = "simple_cooling",
    .init = simple_cooling_init,
    .process_halos = simple_cooling_process,
    .cleanup = simple_cooling_cleanup};

/**
 * @brief   Register the simple cooling module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 */
void simple_cooling_register(void) {
  module_registry_add(&simple_cooling_module);
}
