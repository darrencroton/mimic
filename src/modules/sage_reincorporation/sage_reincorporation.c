/**
 * @file    sage_reincorporation.c
 * @brief   SAGE gas reincorporation module implementation
 *
 * This module implements gas reincorporation from the ejected reservoir back
 * to the hot halo gas. Ejected gas can be recaptured by halos with virial
 * velocities exceeding a critical velocity (~445 km/s), which is related to
 * the characteristic velocity of supernova-driven winds.
 *
 * Physics Process:
 * ----------------
 * 1. Only central galaxies can reincorporate gas (Type == 0)
 * 2. Reincorporation requires Vvir > Vcrit
 * 3. Rate scales with (Vvir/Vcrit - 1) * M_ejected / t_dyn
 * 4. Metallicity is preserved during gas transfer
 *
 * Implementation Notes:
 * ---------------------
 * - Uses core halo properties: Vvir, Rvir, dT (timestep)
 * - Accesses ejected and hot gas reservoirs via GalaxyData properties
 * - Metallicity calculation uses shared utility: mimic_get_metallicity()
 * - All mass transfers are bounded (cannot exceed available ejected mass)
 *
 * Reference:
 *   - Croton et al. (2016) - SAGE model description
 *   - SAGE source: sage-code/model_reincorporation.c
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Updates GalaxyData properties only
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../shared/metallicity.h"  // Shared utility for metallicity calculations
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_reincorporation.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/**
 * @brief   Tunable parameter for critical velocity threshold
 *
 * Multiplies the critical virial velocity (445.48 km/s) for reincorporation.
 * Lower values allow reincorporation in lower-mass halos.
 *
 * Physics:
 *   Vcrit = 445.48 km/s * ReIncorporationFactor
 *
 * The critical velocity is derived from:
 *   - Supernova wind velocity: V_SN ≈ 630 km/s
 *   - Escape velocity: V_esc = sqrt(2) * V_vir
 *   - Critical V_vir = V_SN / sqrt(2) ≈ 445.48 km/s
 *
 * Configuration: SageReincorporation_ReIncorporationFactor
 * Default: 1.0
 * Range: [0.0, 10.0]
 */
static double REINCORPORATION_FACTOR = 1.0;

/**
 * @brief   Base critical virial velocity for reincorporation (km/s)
 *
 * Physical constant derived from supernova wind velocity (630 km/s)
 * and escape velocity relation: V_crit = V_SN / sqrt(2) ≈ 445.48 km/s
 */
static const double VCRIT_BASE = 445.48;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the SAGE reincorporation module
 *
 * Reads module parameters from configuration and logs initialization.
 *
 * @return  0 on success, -1 on failure
 */
static int sage_reincorporation_init(void)
{
    INFO_LOG("Initializing SAGE reincorporation module...");

    // Read parameters from configuration
    module_get_double("SageReincorporation", "ReIncorporationFactor",
                     &REINCORPORATION_FACTOR, 1.0);

    // Validate parameters
    if (REINCORPORATION_FACTOR < 0.0 || REINCORPORATION_FACTOR > 10.0) {
        ERROR_LOG("SageReincorporation_ReIncorporationFactor = %.3f is outside "
                 "valid range [0.0, 10.0]",
                 REINCORPORATION_FACTOR);
        return -1;
    }

    // Calculate effective critical velocity
    double Vcrit = VCRIT_BASE * REINCORPORATION_FACTOR;

    // Log configuration
    INFO_LOG("  ReIncorporationFactor = %.3f", REINCORPORATION_FACTOR);
    INFO_LOG("  Critical velocity (Vcrit) = %.2f km/s", Vcrit);
    INFO_LOG("SAGE reincorporation module initialized successfully");

    return 0;
}

/**
 * @brief   Process gas reincorporation for a FOF group
 *
 * Processes gas reincorporation from the ejected reservoir back to hot gas
 * for all halos in the group. Only central galaxies (Type == 0) can
 * reincorporate gas.
 *
 * Physics:
 *   - Reincorporation only occurs when Vvir > Vcrit
 *   - Rate = (Vvir/Vcrit - 1) * M_ejected * (Vvir/Rvir) * dt
 *   - Metallicity preserved during transfer
 *
 * @param ctx    Module context (redshift, time, parameters)
 * @param halos  Array of halos in the FOF group
 * @param ngal   Number of halos in the group
 *
 * @return       0 on success, -1 on failure
 */
static int sage_reincorporation_process_halos(struct ModuleContext *ctx,
                                              struct Halo *halos,
                                              int ngal)
{
    // Validate inputs
    if (halos == NULL || ngal <= 0) {
        return 0;  // Nothing to process
    }

    // Calculate effective critical velocity
    double Vcrit = VCRIT_BASE * REINCORPORATION_FACTOR;

    // Process each halo
    for (int i = 0; i < ngal; i++) {
        // Only central galaxies can reincorporate gas
        if (halos[i].Type != 0) {
            continue;  // Skip satellites and orphans
        }

        // Validate galaxy data exists
        if (halos[i].galaxy == NULL) {
            ERROR_LOG("Central halo %d has NULL galaxy data", i);
            return -1;
        }

        // Get current gas masses
        float ejected_mass = halos[i].galaxy->EjectedMass;
        float metals_ejected = halos[i].galaxy->MetalsEjectedMass;

        // Skip if no ejected gas to reincorporate
        if (ejected_mass <= EPSILON_SMALL) {
            continue;
        }

        // Get halo properties
        float Vvir = halos[i].Vvir;
        float Rvir = halos[i].Rvir;
        float dt = halos[i].dT;

        // Validate timestep
        if (dt <= 0.0f) {
            DEBUG_LOG("Halo %d: Invalid timestep dT=%.3e, skipping reincorporation",
                     i, dt);
            continue;
        }

        // Check if virial velocity exceeds critical velocity
        if (Vvir <= Vcrit) {
            // Virial velocity too low for reincorporation
            continue;
        }

        // Calculate reincorporation rate
        // Rate = (Vvir/Vcrit - 1) * M_ejected * (Vvir/Rvir) * dt
        double velocity_factor = safe_div(Vvir, Vcrit, 0.0) - 1.0;
        double dynamical_rate = safe_div(Vvir, Rvir, 0.0);  // 1/t_dyn = Vvir/Rvir
        float reincorporated = velocity_factor * ejected_mass * dynamical_rate * dt;

        // Limit to available ejected mass (cannot exceed reservoir)
        if (reincorporated > ejected_mass) {
            reincorporated = ejected_mass;
        }

        // Calculate metallicity of ejected gas (preserved during transfer)
        float metallicity = mimic_get_metallicity(ejected_mass, metals_ejected);

        // Calculate metal mass being reincorporated
        float reincorporated_metals = metallicity * reincorporated;

        // Update galaxy properties: remove from ejected reservoir
        halos[i].galaxy->EjectedMass -= reincorporated;
        halos[i].galaxy->MetalsEjectedMass -= reincorporated_metals;

        // Update galaxy properties: add to hot gas reservoir
        halos[i].galaxy->HotGas += reincorporated;
        halos[i].galaxy->MetalsHotGas += reincorporated_metals;

        // Debug logging
        DEBUG_LOG("Halo %d: Reincorporated %.3e Msun/h (Vvir=%.1f km/s > Vcrit=%.1f km/s)",
                 i, reincorporated * 1e10, Vvir, Vcrit);
        DEBUG_LOG("  EjectedMass: %.3e → %.3e Msun/h",
                 (ejected_mass) * 1e10,
                 (halos[i].galaxy->EjectedMass) * 1e10);
        DEBUG_LOG("  HotGas: %.3e → %.3e Msun/h (added %.3e)",
                 (halos[i].galaxy->HotGas - reincorporated) * 1e10,
                 (halos[i].galaxy->HotGas) * 1e10,
                 reincorporated * 1e10);
    }

    return 0;
}

/**
 * @brief   Cleanup the SAGE reincorporation module
 *
 * Frees any resources allocated by the module. This module has no persistent
 * state, so cleanup is minimal.
 *
 * @return  0 on success
 */
static int sage_reincorporation_cleanup(void)
{
    INFO_LOG("SAGE reincorporation module cleaned up");
    return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module structure for sage_reincorporation module
 */
static struct Module sage_reincorporation_module = {
    .name = "sage_reincorporation",
    .init = sage_reincorporation_init,
    .process_halos = sage_reincorporation_process_halos,
    .cleanup = sage_reincorporation_cleanup};

/**
 * @brief   Register the SAGE reincorporation module
 *
 * Called by the module system to register this module's lifecycle functions.
 */
void sage_reincorporation_register(void)
{
    module_registry_add(&sage_reincorporation_module);
}
