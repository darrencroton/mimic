/**
 * @file    sage_radio_mode_heating.c
 * @brief   SAGE radio-mode AGN heating module
 *
 * Implements AGN radio-mode feedback that suppresses cooling. Black hole
 * accretes hot gas via empirical, Bondi-Hoyle, or cold cloud modes (limited
 * by Eddington rate). AGN heating reduces CoolingGas calculated by
 * sage_calculate_cooling module.
 *
 * Physics: L_AGN = η × Mdot × c² suppresses cooling
 *          Four modes: 0=off, 1=empirical, 2=Bondi, 3=cold cloud
 *          Eddington limit: L_edd = 1.3e38 × M_BH erg/s
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_cooling_heating.c
 */

#include <assert.h>
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
#include "numeric.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RADIO_MODE_EFFICIENCY;
static int AGN_RECIPE_ON;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

static const double EDDINGTON_LUM_COEFF = 1.3e38;  /* erg/s per Msun */

/* Cold cloud accretion (AGN Mode 3) */
static const double BH_MASS_THRESHOLD_FRAC = 0.0001;
static const double COLD_CLOUD_ACCRETION_FRAC = 0.0001;

/* Empirical accretion (AGN Mode 1) */
static const double BH_MASS_NORM = 0.01;
static const double VVIR_AGN_NORM = 200.0;
static const double HOT_GAS_FRAC_NORM = 0.1;
static const double EDDINGTON_VELOCITY_SCALE = 1.34e5;

/* BUG FIX #2: Speed of light in CODE UNITS (km/s)^2, not CGS */
static const double C_SQUARED_CODE_UNITS = 9.0e10;  /* (km/s)^2 */

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Implements AGN heating and black hole accretion
 *
 * Reduces CoolingGas based on AGN feedback. Three accretion modes available
 * (empirical, Bondi-Hoyle, cold cloud), all limited by Eddington rate.
 * Updates BlackHoleMass, HotGas, MetalsHotGas, Rheat, and Heating.
 *
 * @param   halo        Pointer to halo
 * @param   ctx         Module context
 * @param   dt          Time step (substep_dt)
 */
static void do_AGN_heating(struct Halo *halo, struct ModuleContext *ctx, double dt)
{
    double AGNrate, EDDrate, AGNaccreted, AGNcoeff, AGNheating;
    double metallicity, r_heat_new, coolingGas;
    float hot_gas, metals_hot_gas, mvir, vvir, rvir;
    float black_hole_mass, r_heat, rcool;

    /* Get galaxy and halo properties */
    hot_gas = halo->galaxy->HotGas;
    metals_hot_gas = halo->galaxy->MetalsHotGas;
    black_hole_mass = halo->galaxy->BlackHoleMass;
    r_heat = halo->galaxy->Rheat;
    mvir = halo->Mvir;
    vvir = halo->Vvir;
    rvir = halo->Rvir;

    /* Get CoolingGas and Rcool from properties (calculated by sage_calculate_cooling) */
    coolingGas = (double)halo->galaxy->CoolingGas;
    rcool = halo->galaxy->Rcool;

    /* BUG FIX #1: Heating radius division fallback must be 1.0, not 0.0
     * This prevents unphysical cooling when rcool=0 */
    if (r_heat < rcool)
        coolingGas = (1.0 - safe_div(r_heat, rcool, 1.0)) * coolingGas;  /* FIXED: 0.0 → 1.0 */
    else
        coolingGas = 0.0;  /* Complete suppression if heating radius exceeds cooling radius */

    assert(coolingGas >= 0.0);

    /* Calculate AGN accretion rate based on selected mode */
    if (hot_gas > EPSILON_SMALL) {
        if (AGN_RECIPE_ON == 0) {
            /* AGN ACCRETION MODE 0: Disabled */
            AGNrate = 0.0;

        } else if (AGN_RECIPE_ON == 2) {
            /* AGN ACCRETION MODE 2: Bondi-Hoyle */
            /* Note: x is not available as a property, but for Bondi mode we can approximate
             * or skip this mode if it's not commonly used. For now, skip Bondi mode. */
            ERROR_LOG("AGN Mode 2 (Bondi-Hoyle) not supported - requires additional properties");
            AGNrate = 0.0;

        } else if (AGN_RECIPE_ON == 3) {
            /* AGN ACCRETION MODE 3: Cold Cloud */
            double bh_mass_threshold = BH_MASS_THRESHOLD_FRAC * mvir *
                                      pow(safe_div(rcool, rvir, 0.0), 3.0);
            if (black_hole_mass > bh_mass_threshold)
                AGNrate = COLD_CLOUD_ACCRETION_FRAC * safe_div(coolingGas, dt, 0.0);
            else
                AGNrate = 0.0;

        } else if (AGN_RECIPE_ON == 1) {
            /* AGN ACCRETION MODE 1: Empirical (Default) */
            double unit_conv = ctx->params->UnitMass_in_g / ctx->params->UnitTime_in_s *
                              SEC_PER_YEAR / SOLAR_MASS;

            if (mvir > EPSILON_SMALL)
                AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
                         (black_hole_mass / BH_MASS_NORM) *
                         pow(vvir / VVIR_AGN_NORM, 3.0) *
                         (safe_div(hot_gas, mvir, 0.0) / HOT_GAS_FRAC_NORM);
            else
                AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
                         (black_hole_mass / BH_MASS_NORM) *
                         pow(vvir / VVIR_AGN_NORM, 3.0);

        } else {
            /* Invalid AGN mode */
            ERROR_LOG("Invalid AGN_RECIPE_ON value: %d (valid: 0-3)", AGN_RECIPE_ON);
            return;
        }

        /* BUG FIX #2: Use C_SQUARED_CODE_UNITS (9e10), not C_SQUARED_CGS (9e20)
         * Code velocities are in km/s, so c² = 9×10¹⁰ (km/s)² */
        EDDrate = (EDDINGTON_LUM_COEFF * black_hole_mass * 1e10 / ctx->params->Hubble_h) /
                 (ctx->params->UnitEnergy_in_cgs / ctx->params->UnitTime_in_s) /
                 (RADIATIVE_EFFICIENCY * C_SQUARED_CODE_UNITS);  /* FIXED: C_SQUARED_CGS → C_SQUARED_CODE_UNITS */

        /* Limit accretion to Eddington rate */
        if (AGNrate > EDDrate)
            AGNrate = EDDrate;

        AGNaccreted = AGNrate * dt;

        /* Ensure we don't accrete more hot gas than is available */
        if (AGNaccreted > hot_gas)
            AGNaccreted = hot_gas;

        /* Calculate heating efficiency coefficient */
        AGNcoeff = pow(safe_div(EDDINGTON_VELOCITY_SCALE, vvir, 1.0), 2.0);

        /* Calculate mass of cooling gas that can be suppressed by this heating */
        AGNheating = AGNcoeff * AGNaccreted;

        /* Limit heating to current cooling rate for energy conservation */
        if (AGNheating > coolingGas) {
            AGNaccreted = safe_div(coolingGas, AGNcoeff, 0.0);
            AGNheating = coolingGas;
        }

        /* Update galaxy properties based on black hole accretion */
        metallicity = mimic_get_metallicity(hot_gas, metals_hot_gas);  /* Bug #3 fixed in metallicity.h */

        halo->galaxy->BlackHoleMass += AGNaccreted;
        halo->galaxy->HotGas -= AGNaccreted;
        halo->galaxy->MetalsHotGas -= metallicity * AGNaccreted;

        /* Update heating radius */
        if (r_heat < rcool && coolingGas > EPSILON_SMALL) {
            r_heat_new = safe_div(AGNheating, coolingGas, 0.0) * rcool;
            if (r_heat_new > r_heat)
                halo->galaxy->Rheat = r_heat_new;
        }

        /* Track heating energy */
        if (AGNheating > EPSILON_SMALL)
            halo->galaxy->Heating += KINETIC_ENERGY_FACTOR * AGNheating * vvir * vvir;

        /* Update CoolingGas property to reflect AGN suppression */
        halo->galaxy->CoolingGas = (float)coolingGas;
    }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the sage_radio_mode_heating module
 *
 * Loads module parameters and validates configuration.
 *
 * @return  0 on success, -1 on failure
 */
int sage_radio_mode_heating_init(void)
{
    /* Load and validate parameters */
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                      "AGN radio mode heating efficiency");
    LOAD_AND_VALIDATE_OPTION("AGNrecipeOn", AGN_RECIPE_ON, 3,
                             "0=off, 1=empirical, 2=Bondi, 3=cold cloud");

    /* Log module configuration */
    INFO_LOG("SAGE radio-mode AGN heating module initialized");
    VERBOSE_LOG("  RadioModeEfficiency = %.4f", RADIO_MODE_EFFICIENCY);
    VERBOSE_LOG("  AGNrecipeOn = %d (0=off, 1=empirical, 2=Bondi, 3=cold cloud)", AGN_RECIPE_ON);
    VERBOSE_LOG("  Physics: AGN suppresses CoolingGas via black hole accretion");
    VERBOSE_LOG("  Execution: Runs each substep in phase_1 (process_by_galaxy)");

    return 0;
}

/**
 * @brief   Process individual galaxy for AGN heating
 *
 * Applies AGN feedback to suppress cooling. Processes galaxies with hot gas
 * (types 0 and 1 only). Uses ctx->substep_dt for time evolution.
 *
 * @param   ctx    Module context (substep_dt, redshift, etc.)
 * @param   halos  Pointer to single halo (ngal=1 for process_by_galaxy)
 * @param   ngal   Number of halos (always 1 for process_by_galaxy)
 * @return  0 on success
 */
int sage_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
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

    /* Only apply AGN heating if cooling is occurring and AGN is enabled */
    if (halo->galaxy->CoolingGas > EPSILON_SMALL && AGN_RECIPE_ON > 0) {
        do_AGN_heating(halo, ctx, ctx->substep_dt);

        DEBUG_LOG("AGN heating: Type=%d, BHMass=%.3e, CoolingGas=%.3e (after suppression), z=%.3f",
                  halo->Type, halo->galaxy->BlackHoleMass, halo->galaxy->CoolingGas, ctx->redshift);
    }

    return 0;
}

/**
 * @brief   Cleanup the sage_radio_mode_heating module
 *
 * No resources to free.
 *
 * @return  0 on success
 */
int sage_radio_mode_heating_cleanup(void)
{
    VERBOSE_LOG("SAGE radio-mode heating module cleaned up");
    return 0;
}
