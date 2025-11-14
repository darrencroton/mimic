/**
 * @file    simple_sfr.c
 * @brief   Simple star formation rate module implementation
 *
 * This module implements star formation from cold gas using a
 * Kennicutt-Schmidt-like law:
 *   ΔStellarMass = ε_SF * ColdGas * (Vvir/Rvir) * Δt
 *
 * This is a placeholder module for testing module system infrastructure:
 * - Phase 3: Runtime configuration and parameter reading
 * - Module interaction (reads ColdGas from simple_cooling module)
 * - Time evolution (uses Δt calculated per timestep)
 * - Property dependencies (must run after simple_cooling)
 *
 * Vision Principle 1 (Physics-Agnostic Core): This module interacts with core
 * only through well-defined interfaces. Core has zero knowledge of star
 * formation physics.
 *
 * A realistic star formation module will be implemented in a later phase.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "simple_sfr.h"
#include "types.h"

/**
 * @brief   Star formation efficiency parameter
 *
 * Efficiency of converting cold gas to stars.
 * Physics: ΔStellarMass = SF_EFFICIENCY * ColdGas * (Vvir/Rvir) * Δt
 *
 * Read from configuration file via SimpleSFR_Efficiency parameter.
 */
static double SF_EFFICIENCY = 0.02; /* Default value */

/**
 * @brief   Initialize simple star formation rate module
 *
 * Called once during program startup. Reads module parameters from
 * configuration and logs module configuration.
 *
 * @return  0 on success
 */
static int simple_sfr_init(void) {
  // Read module parameters from configuration
  module_get_double("SimpleSFR", "Efficiency", &SF_EFFICIENCY, 0.02);

  INFO_LOG("Simple star formation rate module initialized");
  INFO_LOG("  Physics: ΔStellarMass = %.3f * ColdGas * (Vvir/Rvir) * Δt",
           SF_EFFICIENCY);
  INFO_LOG("  Efficiency = %.3f (from config)", SF_EFFICIENCY);
  INFO_LOG("  Dependencies: Requires ColdGas from simple_cooling module");
  INFO_LOG("  Note: This is a placeholder module for infrastructure testing");
  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Forms stars from cold gas using Kennicutt-Schmidt-like law:
 *   ΔStellarMass = ε_SF * ColdGas * (Vvir/Rvir) * Δt
 *
 * Updates:
 *   ColdGas -= ΔStellarMass  (gas depletion)
 *   StellarMass += ΔStellarMass  (star formation)
 *
 * @param   ctx     Module execution context (provides redshift, time, params)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, -1 on error
 */
static int simple_sfr_process(struct ModuleContext *ctx, struct Halo *halos,
                               int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0; // Nothing to process
  }

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

    // Get current cold gas mass (from cooling module)
    float cold_gas = halos[i].galaxy->ColdGas;

    // Skip if no gas available
    if (cold_gas <= 0.0f) {
      continue;
    }

    // Get timestep (calculated in build_model.c)
    float dt = halos[i].dT;

    // Skip if invalid timestep
    if (dt <= 0.0f) {
      DEBUG_LOG("Halo %d: Invalid dT=%.3f, skipping star formation", i, dt);
      continue;
    }

    // Get virial properties (needed for dynamical time)
    float Vvir = halos[i].Vvir;
    float Rvir = halos[i].Rvir;

    // Calculate dynamical time^(-1): Vvir / Rvir
    // This gives units of 1/time (star formation rate per unit gas mass)
    float inv_tdyn = (Rvir > 0.0f) ? (Vvir / Rvir) : 0.0f;

    // Calculate star formation:
    // ΔStellarMass = ε_SF * ColdGas * (Vvir/Rvir) * Δt
    float delta_stellar_mass = SF_EFFICIENCY * cold_gas * inv_tdyn * dt;

    // Clamp to available gas (prevent negative gas)
    if (delta_stellar_mass > cold_gas) {
      delta_stellar_mass = cold_gas;
    }

    // Update galaxy properties
    halos[i].galaxy->ColdGas -= delta_stellar_mass;     // Deplete gas
    halos[i].galaxy->StellarMass += delta_stellar_mass; // Form stars

    DEBUG_LOG("Halo %d: ColdGas=%.3e, Δt=%.3f -> ΔStellarMass=%.3e, "
              "StellarMass=%.3e, remaining ColdGas=%.3e (z=%.3f)",
              i, cold_gas, dt, delta_stellar_mass, halos[i].galaxy->StellarMass,
              halos[i].galaxy->ColdGas, ctx->redshift);
  }

  return 0;
}

/**
 * @brief   Cleanup simple star formation rate module
 *
 * Called once during program shutdown. No resources to free in this simple
 * module.
 *
 * @return  0 on success
 */
static int simple_sfr_cleanup(void) {
  INFO_LOG("Simple star formation rate module cleaned up");
  return 0;
}

/**
 * @brief   Module structure for simple star formation rate module
 *
 * Defines this module's interface to the module system.
 */
static struct Module simple_sfr_module = {
    .name = "simple_sfr",
    .init = simple_sfr_init,
    .process_halos = simple_sfr_process,
    .cleanup = simple_sfr_cleanup};

/**
 * @brief   Register the simple star formation rate module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 */
void simple_sfr_register(void) {
  module_registry_add(&simple_sfr_module);
}
