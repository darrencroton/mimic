/**
 * @file simple_cooling.c
 * @brief Simple cooling module implementation
 *
 * This module implements a simple cooling prescription: cold gas accumulates
 * from newly accreted dark matter at a rate proportional to the baryon fraction.
 *
 * Physics:
 *   ΔColdGas = f_baryon * ΔMvir
 *
 * where:
 *   - f_baryon = 0.15 (cosmic baryon fraction)
 *   - ΔMvir = Mvir(current) - Mvir(progenitor)  [from struct Halo]
 *   - ColdGas accumulates over time via progenitor inheritance
 *
 * This is part of PoC Round 2 testing:
 * - Module interaction (cooling provides gas for star formation)
 * - Time evolution (uses deltaMvir calculated per timestep)
 * - Property communication (writes ColdGas for stellar_mass to read)
 */

#include <stdio.h>
#include <stdlib.h>
#include "simple_cooling.h"
#include "module_registry.h"
#include "module_interface.h"
#include "types.h"
#include "error.h"

/**
 * @brief Baryon fraction parameter
 *
 * Fraction of accreted halo mass that becomes cold gas.
 * Cosmic value: Ω_b / Ω_m ≈ 0.15
 */
static const float BARYON_FRACTION = 0.15;

/**
 * @brief Initialize simple cooling module
 *
 * Called once during program startup. Logs module configuration.
 *
 * @return 0 on success
 */
static int simple_cooling_init(void)
{
    INFO_LOG("Simple cooling module initialized");
    INFO_LOG("  Physics: ΔColdGas = %.2f * ΔMvir", BARYON_FRACTION);
    return 0;
}

/**
 * @brief Process halos in a FOF group
 *
 * Computes cold gas accumulation for all halos in the group using:
 *   ΔColdGas = BARYON_FRACTION * deltaMvir
 *   ColdGas += ΔColdGas
 *
 * Only processes central galaxies (Type == 0). For halos that have grown
 * (deltaMvir > 0), converts a fraction of the accreted mass to cold gas.
 *
 * @param ctx Module execution context (provides redshift, time, params)
 * @param halos Array of halos in the FOF group (FoFWorkspace)
 * @param ngal Number of halos in the array
 * @return 0 on success
 */
static int simple_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0; // Nothing to process
    }

    // Access context information (could be used for redshift-dependent cooling)
    (void)ctx; // Currently not used in this simple model

    // Process each halo in the FOF group
    for (int i = 0; i < ngal; i++) {
        // Only process central galaxies
        if (halos[i].Type != 0) {
            continue;
        }

        // Check that galaxy data is allocated
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
        // (Mass loss doesn't add cold gas)
        if (delta_mvir > 0.0) {
            // Calculate newly cooled gas
            float delta_cold_gas = BARYON_FRACTION * delta_mvir;

            // Accumulate cold gas
            // (ColdGas from progenitor was already inherited via deep copy)
            halos[i].galaxy->ColdGas += delta_cold_gas;

            DEBUG_LOG("Halo %d: ΔMvir=%.3e -> ΔColdGas=%.3e, total ColdGas=%.3e (z=%.3f)",
                      i, delta_mvir, delta_cold_gas, halos[i].galaxy->ColdGas, ctx->redshift);
        } else {
            // No cooling for halos losing mass
            DEBUG_LOG("Halo %d: ΔMvir=%.3e (negative), no cooling (z=%.3f)",
                      i, delta_mvir, ctx->redshift);
        }
    }

    return 0;
}

/**
 * @brief Cleanup simple cooling module
 *
 * Called once during program shutdown. Logs cleanup.
 *
 * @return 0 on success
 */
static int simple_cooling_cleanup(void)
{
    INFO_LOG("Simple cooling module cleaned up");
    return 0;
}

/**
 * @brief Module structure for simple cooling module
 *
 * Defines this module's interface to the module system.
 */
static struct Module simple_cooling_module = {
    .name = "simple_cooling",
    .init = simple_cooling_init,
    .process_halos = simple_cooling_process,
    .cleanup = simple_cooling_cleanup
};

/**
 * @brief Register the simple cooling module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 */
void simple_cooling_register(void)
{
    register_module(&simple_cooling_module);
}
