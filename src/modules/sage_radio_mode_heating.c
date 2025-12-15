/**
 * @file    sage_radio_mode_heating.c
 * @brief   SAGE radio-mode AGN heating module
 *
 * Implements AGN radio-mode feedback that suppresses cooling via black hole
 * accretion and heating. Three accretion modes: empirical, Bondi-Hoyle, cold cloud.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RADIO_MODE_EFFICIENCY;
static int AGN_RECIPE_ON;

// ============================================================================
// AGN ACCRETION MODE FUNCTIONS
// ============================================================================

/**
 * @brief AGN Mode 1: Empirical accretion recipe (default)
 *
 * Scales with black hole mass, virial velocity, and hot gas fraction.
 * Normalized to typical massive galaxy: M_BH = 10^8 M_sun, V_vir = 200 km/s, f_hot = 0.1
 */
static double calculate_agn_rate_empirical(const double black_hole_mass, const double mvir,
                                           const double vvir, const double hot_gas,
                                           const struct MimicConfig *run_params)
{
    const double unit_conv = run_params->UnitMass_in_g / run_params->UnitTime_in_s *
                            SEC_PER_YEAR / SOLAR_MASS;

    double AGNrate;
    if (mvir > 0.0) {
        AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
                 (black_hole_mass / 0.01) *            // Normalized to M_BH = 10^8 M_sun
                 (vvir / 200.0) * (vvir / 200.0) * (vvir / 200.0) *  // Normalized to V_vir = 200 km/s
                 ((hot_gas / mvir) / 0.1);            // Normalized to f_hot = 0.1
    } else {
        AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
                 (black_hole_mass / 0.01) *
                 (vvir / 200.0) * (vvir / 200.0) * (vvir / 200.0);
    }

    return AGNrate;
}

/**
 * @brief AGN Mode 2: Bondi-Hoyle accretion recipe
 *
 * Based on BH mass and local gas properties: AGNrate ~ G * rho * M_BH^2 / c_s^3
 * NOTE: Not implemented - requires 'x' variable from cooling_recipe.
 *       To enable: add 'x' as a galaxy property or recalculate here.
 */
/* COMMENTED OUT - NOT IMPLEMENTED
static double calculate_agn_rate_bondi(const double black_hole_mass, const double x,
                                       const struct params *run_params)
{
    // AGNrate = (2.5 * M_PI * run_params->G) * (0.375 * 0.6 * x) *
    //          black_hole_mass * RADIO_MODE_EFFICIENCY;
    // return AGNrate;
    return 0.0;
}
*/

/**
 * @brief AGN Mode 3: Cold cloud accretion recipe
 *
 * Triggered when BH mass exceeds threshold: M_BH > 10^-4 * M_vir * (R_cool/R_vir)^3
 * Accretion rate = 0.01% of cooling rate when triggered.
 */
static double calculate_agn_rate_cold_cloud(const double black_hole_mass, const double mvir,
                                             const double rcool, const double rvir,
                                             const double coolingGas, const double dt)
{
    if (black_hole_mass > 0.0001 * mvir * (rcool / rvir) * (rcool / rvir) * (rcool / rvir)) {
        return 0.0001 * coolingGas / dt;
    } else {
        return 0.0;
    }
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Implements AGN heating and black hole accretion
 *
 * Reduces CoolingGas based on AGN feedback. Accretion mode selected by AGN_RECIPE_ON,
 * all limited by Eddington rate. Updates BlackHoleMass, HotGas, MetalsHotGas, Rheat, Heating.
 */
static void do_AGN_heating(struct Halo *halo, struct ModuleContext *ctx, const double dt)
{
    double AGNrate, EDDrate, AGNaccreted, AGNcoeff, AGNheating, metallicity;

    // Get properties
    const double hot_gas = halo->galaxy->HotGas;
    const double metals_hot_gas = halo->galaxy->MetalsHotGas;
    const double black_hole_mass = halo->galaxy->BlackHoleMass;
    const double r_heat = halo->galaxy->Rheat;
    const double mvir = halo->Mvir;
    const double vvir = halo->Vvir;
    const double rvir = halo->Rvir;

    double coolingGas = (double)halo->galaxy->CoolingGas;
    const double rcool = halo->galaxy->Rcool;

    // First update cooling rate based on past AGN heating
    if (rcool <= EPSILON_SMALL) {
        // No cooling if cooling radius is zero
        coolingGas = 0.0;
    } else if (r_heat < rcool) {
        // Partial suppression based on heating radius fraction
        coolingGas = (1.0 - r_heat / rcool) * coolingGas;
    } else {
        // Complete suppression if heating radius exceeds cooling radius
        coolingGas = 0.0;
    }

    // Now calculate the new heating rate from black hole accretion
    if (hot_gas > 0.0) {
        // Select AGN accretion mode
        if (AGN_RECIPE_ON == 2) {
            // Bondi-Hoyle mode not implemented
            ERROR_LOG("AGN Mode 2 (Bondi-Hoyle) not implemented - requires additional properties");
            AGNrate = 0.0;
            // AGNrate = calculate_agn_rate_bondi(black_hole_mass, x, ctx->params);
        } else if (AGN_RECIPE_ON == 3) {
            // Cold cloud accretion
            AGNrate = calculate_agn_rate_cold_cloud(black_hole_mass, mvir, rcool, rvir, coolingGas, dt);
        } else {
            // Empirical (default) accretion recipe
            AGNrate = calculate_agn_rate_empirical(black_hole_mass, mvir, vvir, hot_gas, ctx->params);
        }

        // Eddington limit: L_edd = 1.3e38 * M_BH erg/s
        EDDrate = (1.3e38 * black_hole_mass * 1e10 / ctx->params->Hubble_h) /
                 (ctx->params->UnitEnergy_in_cgs / ctx->params->UnitTime_in_s) /
                 (0.1 * 9e10);  // 0.1 = radiative efficiency, 9e10 = c^2 in (km/s)^2

        // Accretion limited by Eddington rate
        if (AGNrate > EDDrate) {
            AGNrate = EDDrate;
        }

        AGNaccreted = AGNrate * dt;

        // Cannot accrete more mass than is available
        if (AGNaccreted > hot_gas) {
            AGNaccreted = hot_gas;
        }

        // Coefficient to heat cooling gas back to virial temperature
        // 1.34e5 = sqrt(2*eta*c^2), eta=0.1, c in km/s
        AGNcoeff = (1.34e5 / vvir) * (1.34e5 / vvir);

        // Cooling mass that can be suppressed from AGN heating
        AGNheating = AGNcoeff * AGNaccreted;

        // Limit heating to current cooling rate
        if (AGNheating > coolingGas) {
            AGNaccreted = coolingGas / AGNcoeff;
            AGNheating = coolingGas;
        }

        // Update galaxy properties
        metallicity = mimic_get_metallicity(hot_gas, metals_hot_gas);
        halo->galaxy->BlackHoleMass += AGNaccreted;
        halo->galaxy->HotGas -= AGNaccreted;
        halo->galaxy->MetalsHotGas -= metallicity * AGNaccreted;

        // Update heating radius
        if (r_heat < rcool && coolingGas > 0.0) {
            double r_heat_new = (AGNheating / coolingGas) * rcool;
            if (r_heat_new > r_heat) {
                halo->galaxy->Rheat = r_heat_new;
            }
        }

        // Track heating energy
        if (AGNheating > 0.0) {
            halo->galaxy->Heating += 0.5 * AGNheating * vvir * vvir;
        }
    }

    // Update CoolingGas property to reflect AGN suppression
    halo->galaxy->CoolingGas = (float)coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_radio_mode_heating_init(void)
{
    // Load and validate parameters
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                      "AGN radio mode heating efficiency");
    LOAD_AND_VALIDATE_OPTION("AGNrecipeOn", AGN_RECIPE_ON, 3,
                             "0=off, 1=empirical, 2=Bondi, 3=cold cloud");

    INFO_LOG("SAGE radio-mode AGN heating module initialized");
    VERBOSE_LOG("  RadioModeEfficiency = %.4f", RADIO_MODE_EFFICIENCY);
    VERBOSE_LOG("  AGNrecipeOn = %d (0=off, 1=empirical, 2=Bondi, 3=cold cloud)", AGN_RECIPE_ON);
    return 0;
}

int sage_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
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

    // Only apply AGN heating if cooling is occurring and AGN is enabled
    if (halo->galaxy->CoolingGas > EPSILON_SMALL && AGN_RECIPE_ON > 0) {
        do_AGN_heating(halo, ctx, ctx->substep_dt);
    }

    return 0;
}

int sage_radio_mode_heating_cleanup(void)
{
    VERBOSE_LOG("SAGE radio-mode heating module cleaned up");
    return 0;
}
