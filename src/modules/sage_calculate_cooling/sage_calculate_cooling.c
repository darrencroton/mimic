/**
 * @file    sage_calculate_cooling.c
 * @brief   SAGE calculate cooling module - computes cooling budget for substep
 *
 * Calculates gas cooling from hot halos based on cooling radius and regime.
 * Two cooling regimes: cold accretion (Rcool > Rvir) or hot halo cooling (Rcool < Rvir).
 * Uses metallicity-dependent cooling functions from Sutherland & Dopita (1993).
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"
#include "../_system/physical_constants.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Calculate gas cooling based on halo properties and cooling functions
 *
 * Two regimes: (1) cold accretion when rcool > Rvir, (2) hot halo cooling when rcool < Rvir
 */
static double cooling_recipe(struct Halo *halo, struct ModuleContext *ctx, const double dt, double *rcool_out, double *lambda_out)
{
    double coolingGas;

    if (halo->galaxy->HotGas > 0.0 && halo->Vvir > 0.0) {
        const double tcool = halo->Rvir / halo->Vvir;
        const double temp = 35.9 * halo->Vvir * halo->Vvir;  // T_vir in Kelvin

        double logZ = -10.0;
        if (halo->galaxy->MetalsHotGas > 0.0) {
            logZ = log10(halo->galaxy->MetalsHotGas / halo->galaxy->HotGas);
        }

        double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
        double x = PROTONMASS * BOLTZMANN * temp / lambda;  // sec * g/cm^3
        x /= (ctx->params->UnitDensity_in_cgs * ctx->params->UnitTime_in_s);  // convert to code units

        const double rho_rcool = x / tcool * 0.885;  // 0.885 = 3/2 * mu, mu=0.59 for fully ionized gas

        // Isothermal density profile for hot gas
        const double rho0 = halo->galaxy->HotGas / (4.0 * M_PI * halo->Rvir);
        const double rcool = sqrt(rho0 / rho_rcool);

        coolingGas = 0.0;
        if (rcool > halo->Rvir) {
            // Cold accretion regime
            coolingGas = halo->galaxy->HotGas / (halo->Rvir / halo->Vvir) * dt;
        } else {
            // Hot halo cooling regime
            coolingGas = (halo->galaxy->HotGas / halo->Rvir) * (rcool / (2.0 * tcool)) * dt;
        }

        if (coolingGas > halo->galaxy->HotGas) {
            coolingGas = halo->galaxy->HotGas;
        } else {
            if (coolingGas < 0.0) coolingGas = 0.0;
        }

        *rcool_out = rcool;
        *lambda_out = lambda;
    } else {
        coolingGas = 0.0;
        *rcool_out = 0.0;
        *lambda_out = 0.0;
    }

    return coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_calculate_cooling_init(void)
{
    // Initialize cooling function tables (path relative to module directory)
    if (cooling_tables_init("src/modules/sage_calculate_cooling/CoolFunctions") != 0) {
        ERROR_LOG("Failed to initialize cooling function tables");
        return -1;
    }

    INFO_LOG("SAGE calculate cooling module initialized");
    return 0;
}

int sage_calculate_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];

    // Skip orphan galaxies (type 2)
    if (halo->Type == 2 || halo->galaxy == NULL) {
        return 0;
    }

    // Calculate cooling using substep timestep
    double rcool, lambda;
    double coolingGas = cooling_recipe(halo, ctx, ctx->substep_dt, &rcool, &lambda);

    // Store in properties for subsequent modules
    halo->galaxy->CoolingGas = (float)coolingGas;
    halo->galaxy->Rcool = (float)rcool;
    halo->galaxy->CoolingLambda = (float)lambda;

    return 0;
}

int sage_calculate_cooling_cleanup(void)
{
    cooling_tables_cleanup();
    VERBOSE_LOG("SAGE calculate cooling module cleaned up");
    return 0;
}
