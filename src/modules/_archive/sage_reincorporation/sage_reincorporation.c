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

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"  // Shared utility for metallicity calculations
#include "../_system/parameter_helpers.h"  // Parameter loading and validation macros
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_reincorporation.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================
// Parameters loaded from input YAML file (required, no defaults).
// Validated in module init function.

static double REINCORPORATION_FACTOR;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

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
    if (get_verbose_format()) {
        INFO_LOG("Initializing SAGE reincorporation module...");
    }

    /* Load and validate parameters from input YAML file */
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ReIncorporationFactor", REINCORPORATION_FACTOR, 0.0, 10.0,
                                      "reincorporation efficiency factor");

    // Calculate effective critical velocity
    double Vcrit = VCRIT_BASE * REINCORPORATION_FACTOR;

    // Log configuration
    if (get_verbose_format()) {
        INFO_LOG("  ReIncorporationFactor = %.3f", REINCORPORATION_FACTOR);
        INFO_LOG("  Critical velocity (Vcrit) = %.2f km/s", Vcrit);
        INFO_LOG("SAGE reincorporation module initialized successfully");
    }

    return 0;
}

/**
 * @brief   Process gas reincorporation for a galaxy
 *
 * Processes gas reincorporation from the ejected reservoir back to hot gas.
 * Only central galaxies (Type == 0) can reincorporate gas.
 *
 * Physics:
 *   - Reincorporation only occurs when Vvir > Vcrit
 *   - Rate = (Vvir/Vcrit - 1) * M_ejected * (Vvir/Rvir) * dt
 *   - Metallicity preserved during transfer
 *
 * @param ctx    Module context (substep_dt, redshift, etc.)
 * @param halos  Array of halos (ngal=1 for LOOP_MODE_ALL)
 * @param ngal   Number of halos (always 1)
 *
 * @return       0 on success, -1 on failure
 */
static int sage_reincorporation_process(struct ModuleContext *ctx,
                                        struct Halo *halos,
                                        int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    /* Only central galaxies can reincorporate gas */
    if (halos[0].Type != 0) {
        return 0;  /* Skip satellites and orphans */
    }

    /* Validate galaxy data exists */
    if (halos[0].galaxy == NULL) {
        ERROR_LOG("Central halo has NULL galaxy data");
        return -1;
    }

    struct GalaxyData *galaxy = halos[0].galaxy;

    /* Get current gas masses */
    float ejected_mass = galaxy->EjectedMass;
    float metals_ejected = galaxy->MetalsEjectedMass;

    /* Skip if no ejected gas to reincorporate */
    if (ejected_mass <= EPSILON_SMALL) {
        return 0;
    }

    /* Get halo properties */
    float Vvir = halos[0].Vvir;
    float Rvir = halos[0].Rvir;

    /* Calculate effective critical velocity */
    double Vcrit = VCRIT_BASE * REINCORPORATION_FACTOR;

    /* Check if virial velocity exceeds critical velocity */
    if (Vvir <= Vcrit) {
        return 0;  /* Virial velocity too low for reincorporation */
    }

    /* Calculate reincorporation rate
     * Rate = (Vvir/Vcrit - 1) * M_ejected * (Vvir/Rvir) * dt */
    double velocity_factor = safe_div(Vvir, Vcrit, 0.0) - 1.0;
    double dynamical_rate = safe_div(Vvir, Rvir, 0.0);  /* 1/t_dyn = Vvir/Rvir */
    float reincorporated = velocity_factor * ejected_mass * dynamical_rate * ctx->substep_dt;

    /* Limit to available ejected mass (cannot exceed reservoir) */
    if (reincorporated > ejected_mass) {
        reincorporated = ejected_mass;
    }

    /* Calculate metallicity of ejected gas (preserved during transfer) */
    float metallicity = mimic_get_metallicity(ejected_mass, metals_ejected);

    /* Calculate metal mass being reincorporated */
    float reincorporated_metals = metallicity * reincorporated;

    /* Update galaxy properties: remove from ejected reservoir */
    galaxy->EjectedMass -= reincorporated;
    galaxy->MetalsEjectedMass -= reincorporated_metals;

    /* Update galaxy properties: add to hot gas reservoir */
    galaxy->HotGas += reincorporated;
    galaxy->MetalsHotGas += reincorporated_metals;

    /* Debug logging */
    DEBUG_LOG("Reincorporated %.3e Msun/h (Vvir=%.1f km/s > Vcrit=%.1f km/s, substep=%d/%d)",
             reincorporated * 1e10, Vvir, Vcrit, ctx->substep_number + 1, ctx->num_substeps);
    DEBUG_LOG("  EjectedMass: %.3e → %.3e Msun/h",
             ejected_mass * 1e10, galaxy->EjectedMass * 1e10);
    DEBUG_LOG("  HotGas: %.3e Msun/h (added %.3e)",
             galaxy->HotGas * 1e10, reincorporated * 1e10);

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

/* Extern reference to generated loop mode array */
extern const enum LoopMode sage_reincorporation_supported_modes[];

/**
 * @brief   Module structure for sage_reincorporation module
 */
static struct Module sage_reincorporation_module = {
    .name = "sage_reincorporation",
    .init = sage_reincorporation_init,
    .process = sage_reincorporation_process,
    .cleanup = sage_reincorporation_cleanup,
    .supported_loop_modes = sage_reincorporation_supported_modes,
    .num_supported_modes = 1  /* Only supports LOOP_MODE_ALL */
};

/**
 * @brief   Register the SAGE reincorporation module
 *
 * Called by the module system to register this module's lifecycle functions.
 */
void sage_reincorporation_register(void)
{
    module_registry_add(&sage_reincorporation_module);
}
