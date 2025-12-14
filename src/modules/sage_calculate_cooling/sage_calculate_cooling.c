/**
 * @file    sage_calculate_cooling.c
 * @brief   SAGE calculate cooling module - computes cooling budget for substep
 *
 * Calculates gas cooling from hot halos based on cooling radius and regime.
 * Two cooling regimes: cold accretion (Rcool > Rvir) throughout halo, or
 * hot halo cooling (Rcool < Rvir) only within Rcool. Stores result in
 * CoolingGas property for subsequent processing by sage_radio_mode_heating
 * and sage_add_cooling modules.
 *
 * Physics: Lambda(T, Z) from Sutherland & Dopita (1993) cooling tables
 *          T_vir = 35.9 × Vvir^2 (K, km/s)
 *          Two regimes based on cooling radius vs virial radius
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016)
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"
#include "../_system/parameter_helpers.h"
#include "../_system/physical_constants.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Calculates gas cooling based on halo properties and cooling functions
 *
 * @param   halo     Pointer to the halo structure
 * @param   ctx      Module context (for unit conversions and parameters)
 * @param   dt       Time step size (substep_dt)
 * @param   rcool_out Pointer to store calculated cooling radius (Mpc/h)
 * @return  Mass of gas that cools from hot to cold phase in this time step
 *
 * This function implements the standard cooling model where hot gas cools
 * from an isothermal density profile based on a cooling radius. The cooling
 * rate depends on gas temperature (from virial velocity), gas metallicity,
 * and the corresponding cooling function.
 *
 * Two cooling regimes:
 * 1. "Cold accretion" when Rcool > Rvir: rapid cooling throughout the halo
 * 2. "Hot halo cooling" when Rcool < Rvir: cooling only within cooling radius
 */
static double cooling_recipe(struct Halo *halo, struct ModuleContext *ctx, double dt, double *rcool_out)
{
    double tcool, logZ, lambda, rho_rcool, rho0, temp, coolingGas;
    double rcool;
    float hot_gas, metals_hot_gas, vvir, rvir;

    /* Get galaxy properties */
    hot_gas = halo->galaxy->HotGas;
    metals_hot_gas = halo->galaxy->MetalsHotGas;
    vvir = halo->Vvir;
    rvir = halo->Rvir;

    /* Only proceed if galaxy has hot gas and non-zero virial velocity */
    if (hot_gas <= EPSILON_SMALL || vvir <= EPSILON_SMALL) {
        *rcool_out = 0.0;
        return 0.0;
    }

    /* Dynamical time: tcool = R_vir / V_vir (approximation for cooling time) */
    tcool = safe_div(rvir, vvir, 0.0);

    /* Calculate virial temperature from virial velocity */
    temp = 35.9 * vvir * vvir;

    /* Calculate log of metallicity (Z/Z_sun) for cooling function lookup */
    if (metals_hot_gas > EPSILON_SMALL) {
        double Z = safe_div(metals_hot_gas, hot_gas, 0.0);
        logZ = (Z > 0.0) ? log10(Z) : -10.0;
    } else {
        logZ = -10.0;  /* Very low metallicity if no metals */
    }

    /* Get cooling rate (lambda) from interpolation tables
     * Returns Lambda in units of erg cm^3 s^-1 */
    lambda = get_metaldependent_cooling_rate(log10(temp), logZ);

    /* Calculate coefficient for cooling density threshold
     * x = (m_p * k_B * T) / lambda in physical units (sec * g/cm^3) */
    double x = PROTONMASS * BOLTZMANN * temp / lambda;

    /* Convert to simulation units */
    x /= (ctx->params->UnitDensity_in_cgs * ctx->params->UnitTime_in_s);

    /* Calculate density at cooling radius
     * Factor 0.885 = 3/2 * mu, where mu=0.59 for fully ionized gas
     * This is the density where cooling time equals dynamical time */
    rho_rcool = safe_div(x, tcool, 0.0) * 0.885;

    /* Calculate central density assuming isothermal profile for hot gas */
    rho0 = safe_div(hot_gas, 4.0 * M_PI * rvir, 0.0);

    /* Calculate cooling radius where tcool = tdyn */
    rcool = sqrt(safe_div(rho0, rho_rcool, 0.0));

    /* Store rcool for AGN module */
    *rcool_out = rcool;

    /* Determine cooling regime and calculate cooled gas mass */
    if (rcool > rvir) {
        /* "Cold accretion" regime - rapid cooling throughout the halo
         * All hot gas cools on the dynamical timescale */
        coolingGas = hot_gas * safe_div(vvir, rvir, 0.0) * dt;
    } else {
        /* "Hot halo cooling" regime - cooling only within cooling radius
         * This follows from integrating the isothermal density profile
         * within rcool and dividing by the cooling time */
        coolingGas = safe_div(hot_gas, rvir, 0.0) * safe_div(rcool, 2.0 * tcool, 0.0) * dt;
    }

    /* Apply limits to ensure physically sensible cooling */
    if (coolingGas > hot_gas)
        coolingGas = hot_gas;  /* Cannot cool more gas than is available */
    else if (coolingGas <= 0.0)
        coolingGas = 0.0;  /* Prevent negative cooling */

    return coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the sage_calculate_cooling module
 *
 * Loads cooling function tables, and prepares the module for execution.
 *
 * @return  0 on success, -1 on failure
 */
int sage_calculate_cooling_init(void)
{
    /* Initialize cooling function tables */
    if (cooling_tables_init("src/modules/sage_calculate_cooling/CoolFunctions") != 0) {
        ERROR_LOG("Failed to initialize cooling function tables");
        return -1;
    }

    /* Log module configuration */
    INFO_LOG("SAGE calculate cooling module initialized");
    VERBOSE_LOG("  Physics: Calculates CoolingGas for this substep");

    return 0;
}

/**
 * @brief   Process individual galaxy for cooling calculation
 *
 * Calculates cooling rates for galaxies with hot gas (types 0 and 1 only).
 * Uses ctx->substep_dt for time evolution. Stores cooling mass and cooling
 * radius for use by subsequent modules (radio_mode_heating, add_cooling).
 *
 * @param   ctx    Module context (substep_dt, redshift, etc.)
 * @param   halos  Pointer to single halo (ngal=1 for process_by_galaxy)
 * @param   ngal   Number of halos (always 1 for process_by_galaxy)
 * @return  0 on success, -1 on failure
 */
int sage_calculate_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    /* Validate process_by_galaxy contract */
    if (ngal != 1) {
        ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];

    /* Skip orphan galaxies (type 2) */
    if (halo->Type == 2) {
        return 0;
    }

    /* Validate galaxy data */
    if (halo->galaxy == NULL) {
        return 0;
    }

    /* Skip if no hot gas */
    if (halo->galaxy->HotGas <= EPSILON_SMALL) {
        halo->galaxy->CoolingGas = 0.0f;
        halo->galaxy->Rcool = 0.0f;
        return 0;
    }

    /* Calculate cooling using substep timestep */
    double rcool;
    double coolingGas = cooling_recipe(halo, ctx, ctx->substep_dt, &rcool);

    /* Store in properties for subsequent modules */
    halo->galaxy->CoolingGas = (float)coolingGas;
    halo->galaxy->Rcool = (float)rcool;

    DEBUG_LOG("Calculated cooling: Type=%d, Mvir=%.3e, CoolingGas=%.3e, rcool=%.3e, z=%.3f",
              halo->Type, halo->Mvir, coolingGas, rcool, ctx->redshift);

    return 0;
}

/**
 * @brief   Cleanup the sage_calculate_cooling module
 *
 * Frees any resources allocated during initialization.
 *
 * @return  0 on success
 */
int sage_calculate_cooling_cleanup(void)
{
    cooling_tables_cleanup();
    VERBOSE_LOG("SAGE calculate cooling module cleaned up");
    return 0;
}
