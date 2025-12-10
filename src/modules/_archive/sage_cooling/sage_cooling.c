/**
 * @file    sage_cooling.c
 * @brief   SAGE cooling and AGN heating module implementation
 *
 * Implements gas cooling from hot halos to cold disks with AGN feedback. Two cooling
 * regimes based on cooling radius: cold accretion (rcool > Rvir) throughout halo, or
 * hot halo cooling (rcool < Rvir) only within rcool. AGN radio-mode feedback can
 * suppress cooling. Black hole growth via empirical, Bondi-Hoyle, or cold cloud modes.
 *
 * Physics: Lambda(T, Z) from Sutherland & Dopita (1993) cooling tables
 *          T_vir = 35.9 × Vvir^2 (K, km/s)
 *
 * Key functions:
 * - compute_cooling_radius(): Calculate rcool where tcool = tdyn
 * - compute_cooling(): Apply cooling based on regime
 * - add_cooling_to_cold(): Transfer gas with metallicity tracking
 * - compute_black_hole_accretion(): BH growth and AGN feedback
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016), based on SAGE model_cooling_heating.c
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"  /* Shared utility for metallicity calculations */
#include "../_system/parameter_helpers.h"  /* Parameter loading and validation macros */
#include "../_system/physical_constants.h"  /* Shared physics constants */
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_cooling.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RADIO_MODE_EFFICIENCY;
static int AGN_RECIPE_ON;
static char COOL_FUNCTIONS_DIR[512];

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

static const double VIRIAL_TEMP_COEFF = 35.9;  /* T_vir coefficient (K/(km/s)^2) */

static const double EDDINGTON_LUM_COEFF = 1.3e38;  /* erg/s per Msun */

static const double COOLING_MU_FACTOR = 0.885;      /* 3/2 × μ (μ=0.59) */
static const double SPHERE_VOLUME_COEFF = 4.0;      /* 4π approximation */
static const double COOLING_TIME_DIVISOR = 2.0;

/* Bondi-Hoyle accretion (AGN Mode 2) */
static const double BONDI_HOYLE_COEFF = 2.5;
static const double BONDI_DENSITY_FACTOR = 0.375;
static const double BONDI_SOUND_SPEED_FACTOR = 0.6;

/* Cold cloud accretion (AGN Mode 3) */
static const double BH_MASS_THRESHOLD_FRAC = 0.0001;
static const double COLD_CLOUD_ACCRETION_FRAC = 0.0001;

/* Empirical accretion (AGN Mode 1) */
static const double BH_MASS_NORM = 0.01;
static const double VVIR_AGN_NORM = 200.0;
static const double HOT_GAS_FRAC_NORM = 0.1;
static const double EDDINGTON_VELOCITY_SCALE = 1.34e5;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Calculates gas cooling based on halo properties and cooling functions
 *
 * @param   halo     Pointer to the halo structure
 * @param   ctx      Module context (for unit conversions and parameters)
 * @param   dt       Time step size
 * @param   x        Cooling coefficient (output, used by AGN heating)
 * @param   rcool    Cooling radius (output, used by AGN heating)
 * @return  Mass of gas that cools from hot to cold phase in this time step
 *
 * This function implements the standard cooling model where hot gas cools
 * from an isothermal density profile based on a cooling radius. The cooling
 * rate depends on gas temperature (from virial velocity), gas metallicity,
 * and the corresponding cooling function.
 *
 * Two cooling regimes:
 * 1. "Cold accretion" when rcool > Rvir: rapid cooling throughout the halo
 * 2. "Hot halo cooling" when rcool < Rvir: cooling only within cooling radius
 */
static double cooling_recipe(struct Halo *halo, struct ModuleContext *ctx,
                            double dt, double *x, double *rcool)
{
    double tcool, logZ, lambda, rho_rcool, rho0, temp, coolingGas;
    float hot_gas, metals_hot_gas, vvir, rvir;

    /* Get galaxy properties */
    hot_gas = halo->galaxy->HotGas;
    metals_hot_gas = halo->galaxy->MetalsHotGas;
    vvir = halo->Vvir;
    rvir = halo->Rvir;

    /* Only proceed if galaxy has hot gas and non-zero virial velocity */
    if (hot_gas <= EPSILON_SMALL || vvir <= EPSILON_SMALL) {
        *x = 0.0;
        *rcool = 0.0;
        return 0.0;
    }

    /* Dynamical time: tcool = R_vir / V_vir (approximation for cooling time) */
    tcool = safe_div(rvir, vvir, 0.0);

    /* Calculate virial temperature from virial velocity */
    temp = VIRIAL_TEMP_COEFF * vvir * vvir;

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
    *x = PROTONMASS * BOLTZMANN * temp / lambda;

    /* Convert to simulation units */
    *x /= (ctx->params->UnitDensity_in_cgs * ctx->params->UnitTime_in_s);

    /* Calculate density at cooling radius
     * Factor 0.885 = 3/2 * mu, where mu=0.59 for fully ionized gas
     * This is the density where cooling time equals dynamical time */
    rho_rcool = safe_div(*x, tcool, 0.0) * COOLING_MU_FACTOR;

    /* Calculate central density assuming isothermal profile for hot gas */
    rho0 = safe_div(hot_gas, SPHERE_VOLUME_COEFF * M_PI * rvir, 0.0);

    /* Calculate cooling radius where tcool = tdyn */
    *rcool = sqrt(safe_div(rho0, rho_rcool, 0.0));

    /* Determine cooling regime and calculate cooled gas mass */
    if (*rcool > rvir) {
        /* "Cold accretion" regime - rapid cooling throughout the halo
         * All hot gas cools on the dynamical timescale */
        coolingGas = hot_gas * safe_div(vvir, rvir, 0.0) * dt;
    } else {
        /* "Hot halo cooling" regime - cooling only within cooling radius
         * This follows from integrating the isothermal density profile
         * within rcool and dividing by the cooling time */
        coolingGas = safe_div(hot_gas, rvir, 0.0) * safe_div(*rcool, COOLING_TIME_DIVISOR * tcool, 0.0) * dt;
    }

    /* Apply limits to ensure physically sensible cooling */
    if (coolingGas > hot_gas)
        coolingGas = hot_gas;  /* Cannot cool more gas than is available */
    else if (coolingGas <= 0.0)
        coolingGas = 0.0;  /* Prevent negative cooling */

    return coolingGas;
}

/**
 * @brief   Implements AGN heating and black hole accretion process
 *
 * @param   halo          Pointer to the halo structure
 * @param   coolingGas    Current calculated cooling gas mass (will be modified)
 * @param   ctx           Module context (for unit conversions)
 * @param   dt            Time step size
 * @param   x             Cooling coefficient (from cooling_recipe)
 * @param   rcool         Cooling radius (from cooling_recipe)
 * @return  Updated cooling gas mass after accounting for AGN heating
 *
 * This function models the suppression of cooling by AGN feedback and
 * the growth of the central supermassive black hole. It implements:
 *
 * 1. Reduction of cooling based on past heating events (heating radius)
 * 2. Black hole accretion through one of three selectable methods
 * 3. Limiting of accretion by the Eddington rate
 * 4. Calculation of heating that suppresses cooling
 * 5. Tracking of heating radius and energy
 */
static double do_AGN_heating(struct Halo *halo, double coolingGas,
                            struct ModuleContext *ctx, double dt,
                            double x, double rcool)
{
    double AGNrate, EDDrate, AGNaccreted, AGNcoeff, AGNheating;
    double metallicity, r_heat_new;
    float hot_gas, metals_hot_gas, mvir, vvir, rvir;
    float black_hole_mass, r_heat;

    /* Get galaxy and halo properties */
    hot_gas = halo->galaxy->HotGas;
    metals_hot_gas = halo->galaxy->MetalsHotGas;
    black_hole_mass = halo->galaxy->BlackHoleMass;
    r_heat = halo->galaxy->r_heat;
    mvir = halo->Mvir;
    vvir = halo->Vvir;
    rvir = halo->Rvir;

    /* First, reduce cooling rate based on past AGN heating events
     * This models the cumulative effect of multiple AGN outbursts */
    if (r_heat < rcool)
        coolingGas = (1.0 - safe_div(r_heat, rcool, 0.0)) * coolingGas;
    else
        coolingGas = 0.0;  /* Complete suppression if heating radius exceeds cooling radius */

    assert(coolingGas >= 0.0);

    /* Calculate the new heating rate from black hole accretion
     * Four different accretion modes available (set by AGNrecipeOn parameter):
     *   Mode 0: AGN heating disabled
     *   Mode 1: Empirical scaling (default)
     *   Mode 2: Bondi-Hoyle accretion
     *   Mode 3: Cold cloud accretion */
    if (hot_gas > EPSILON_SMALL) {
        if (AGN_RECIPE_ON == 0) {
            /* ============================================================
             * AGN ACCRETION MODE 0: Disabled
             * ============================================================
             * No AGN heating - for testing or alternative physics models */
            AGNrate = 0.0;

        } else if (AGN_RECIPE_ON == 2) {
            /* ============================================================
             * AGN ACCRETION MODE 2: Bondi-Hoyle
             * ============================================================
             * Physics: Spherical accretion onto black hole
             * Formula: dM/dt ~ G * rho * M_BH^2 / c_s^3
             * Depends on: Black hole mass, gas density, sound speed */
            AGNrate = (BONDI_HOYLE_COEFF * M_PI * ctx->params->G) * (BONDI_DENSITY_FACTOR * BONDI_SOUND_SPEED_FACTOR * x) *
                     black_hole_mass * RADIO_MODE_EFFICIENCY;

        } else if (AGN_RECIPE_ON == 3) {
            /* ============================================================
             * AGN ACCRETION MODE 3: Cold Cloud
             * ============================================================
             * Physics: Accretion triggered when BH exceeds mass threshold
             * Condition: M_BH > 0.0001 * M_vir * (r_cool / R_vir)^3
             * Rate: 0.01% of current cooling rate when triggered */
            double bh_mass_threshold = BH_MASS_THRESHOLD_FRAC * mvir * pow(safe_div(rcool, rvir, 0.0), 3.0);
            if (black_hole_mass > bh_mass_threshold)
                AGNrate = COLD_CLOUD_ACCRETION_FRAC * safe_div(coolingGas, dt, 0.0);
            else
                AGNrate = 0.0;

        } else if (AGN_RECIPE_ON == 1) {
            /* ============================================================
             * AGN ACCRETION MODE 1: Empirical (Default)
             * ============================================================
             * Physics: Empirical scaling from simulations
             * Formula: dM/dt ~ η * (M_BH/0.01) * (V_vir/200)^3 * (f_hot/0.1)
             * Depends on: Black hole mass, virial velocity, hot gas fraction */
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
            return -1;
        }

        /* Calculate Eddington accretion rate limit
         * L_Edd = EDDINGTON_LUM_COEFF * M_BH (in erg/s)
         * Convert luminosity to mass accretion rate: L = η * dM/dt * c^2 */
        EDDrate = (EDDINGTON_LUM_COEFF * black_hole_mass * 1e10 / ctx->params->Hubble_h) /
                 (ctx->params->UnitEnergy_in_cgs / ctx->params->UnitTime_in_s) /
                 (RADIATIVE_EFFICIENCY * C_SQUARED_CGS);

        /* Limit accretion to Eddington rate */
        if (AGNrate > EDDrate)
            AGNrate = EDDrate;

        /* Calculate total mass accreted onto black hole in this time step */
        AGNaccreted = AGNrate * dt;

        /* Ensure we don't accrete more hot gas than is available */
        if (AGNaccreted > hot_gas)
            AGNaccreted = hot_gas;

        /* Calculate heating efficiency coefficient
         * 1.34e5 = sqrt(2*eta*c^2), where eta=0.1 is standard efficiency
         * and c is speed of light in km/s */
        AGNcoeff = pow(safe_div(EDDINGTON_VELOCITY_SCALE, vvir, 1.0), 2.0);

        /* Calculate mass of cooling gas that can be suppressed by this heating */
        AGNheating = AGNcoeff * AGNaccreted;

        /* Limit heating to current cooling rate for energy conservation
         * If heating would exceed cooling, reduce accretion accordingly */
        if (AGNheating > coolingGas) {
            AGNaccreted = safe_div(coolingGas, AGNcoeff, 0.0);
            AGNheating = coolingGas;
        }

        /* Update galaxy properties based on black hole accretion */
        metallicity = mimic_get_metallicity(hot_gas, metals_hot_gas);

        halo->galaxy->BlackHoleMass += AGNaccreted;  /* Grow the black hole */
        halo->galaxy->HotGas -= AGNaccreted;         /* Remove accreted gas from hot phase */
        halo->galaxy->MetalsHotGas -= metallicity * AGNaccreted;  /* Remove corresponding metals */

        /* Update the heating radius - this affects future cooling suppression
         * The heating radius grows when effective heating occurs */
        if (r_heat < rcool && coolingGas > EPSILON_SMALL) {
            r_heat_new = safe_div(AGNheating, coolingGas, 0.0) * rcool;
            if (r_heat_new > r_heat)
                halo->galaxy->r_heat = r_heat_new;
        }

        /* Track heating energy for energy budget calculations
         * E_heat = 0.5 * m * V_vir^2 = energy needed to heat gas to virial temperature */
        if (AGNheating > EPSILON_SMALL)
            halo->galaxy->Heating += KINETIC_ENERGY_FACTOR * AGNheating * vvir * vvir;
    }

    return coolingGas;  /* Return updated cooling gas mass after heating effects */
}

/**
 * @brief   Transfers cooled gas from the hot halo to the cold disk
 *
 * @param   halo         Pointer to the halo structure
 * @param   coolingGas   Mass of gas to be transferred from hot to cold phase
 * @param   vvir         Virial velocity (for energy tracking)
 *
 * This function moves the calculated amount of cooling gas from the hot
 * halo to the cold disk of the galaxy, along with its associated metals.
 * It ensures mass conservation and tracks cooling energy.
 */
static void cool_gas_onto_galaxy(struct Halo *halo, double coolingGas, float vvir)
{
    float metallicity;

    /* Only proceed if there is gas to cool */
    if (coolingGas <= EPSILON_SMALL)
        return;

    /* Check if we're trying to cool more gas than is available in the hot halo */
    if (coolingGas < halo->galaxy->HotGas) {
        /* Normal case: cooling doesn't deplete hot gas completely */
        metallicity = mimic_get_metallicity(halo->galaxy->HotGas, halo->galaxy->MetalsHotGas);
    } else {
        /* Edge case: cooling all remaining hot gas */
        coolingGas = halo->galaxy->HotGas;
        metallicity = mimic_get_metallicity(halo->galaxy->HotGas, halo->galaxy->MetalsHotGas);
    }

    /* Transfer gas from hot to cold reservoir */
    halo->galaxy->HotGas -= coolingGas;
    halo->galaxy->ColdGas += coolingGas;

    /* Transfer metals from hot to cold reservoir */
    halo->galaxy->MetalsHotGas -= metallicity * coolingGas;
    halo->galaxy->MetalsColdGas += metallicity * coolingGas;

    /* Track cooling energy for energy budget calculations
     * E_cool = 0.5 * m * V_vir^2 = change in potential energy */
    halo->galaxy->Cooling += KINETIC_ENERGY_FACTOR * coolingGas * vvir * vvir;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the sage_cooling module
 *
 * Reads module parameters from the parameter file, loads cooling function
 * tables, and prepares the module for execution.
 *
 * @return  0 on success, -1 on failure
 */
static int sage_cooling_init(void)
{
    /* Load and validate parameters from input YAML file */
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                      "AGN radio mode heating efficiency");
    LOAD_AND_VALIDATE_OPTION("AGNrecipeOn", AGN_RECIPE_ON, 3,
                             "0=off, 1=empirical, 2=Bondi, 3=cold cloud");
    LOAD_PARAM_STRING("CoolFunctionsDir", COOL_FUNCTIONS_DIR, sizeof(COOL_FUNCTIONS_DIR));

    /* Initialize cooling function tables */
    if (cooling_tables_init(COOL_FUNCTIONS_DIR) != 0) {
        ERROR_LOG("Failed to initialize cooling function tables");
        return -1;
    }

    /* Log module configuration only when verbose logging is enabled */
    if (get_verbose_format()) {
        INFO_LOG("SAGE cooling & AGN heating module initialized");
        INFO_LOG("  RadioModeEfficiency = %.4f", RADIO_MODE_EFFICIENCY);
        INFO_LOG("  AGNrecipeOn = %d (0=off, 1=empirical, 2=Bondi, 3=cold cloud)", AGN_RECIPE_ON);
        INFO_LOG("  CoolFunctionsDir = %s", COOL_FUNCTIONS_DIR);
    }

    return 0;
}

/**
 * @brief   Process galaxy for cooling and AGN heating
 *
 * Calculates cooling rates and AGN feedback for central galaxies only.
 * Uses ctx->substep_dt for time evolution.
 *
 * @param   ctx    Module context (substep_dt, redshift, etc.)
 * @param   halos  Array of halos (ngal=1 for LOOP_MODE_ALL)
 * @param   ngal   Number of halos (always 1)
 * @return  0 on success, -1 on failure
 */
static int sage_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    /* Only central galaxies cool (Type == 0)
     * Satellites don't accrete fresh gas (handled by sage_infall module) */
    if (halos[0].Type != 0) {
        return 0;
    }

    double coolingGas, x, rcool;

    /* Calculate cooling rate using substep timestep */
    coolingGas = cooling_recipe(&halos[0], ctx, ctx->substep_dt, &x, &rcool);

    /* Apply AGN heating if enabled and cooling is occurring */
    if (AGN_RECIPE_ON > 0 && coolingGas > EPSILON_SMALL) {
        coolingGas = do_AGN_heating(&halos[0], coolingGas, ctx, ctx->substep_dt, x, rcool);
    }

    /* Transfer cooled gas from hot to cold reservoir */
    if (coolingGas > EPSILON_SMALL) {
        cool_gas_onto_galaxy(&halos[0], coolingGas, halos[0].Vvir);

        DEBUG_LOG("Cooled %.3e Msun/h (Mvir=%.3e, z=%.3f, substep=%d/%d)",
                 coolingGas * 1e10, halos[0].Mvir, ctx->redshift,
                 ctx->substep_number + 1, ctx->num_substeps);
    }

    return 0;
}

/**
 * @brief   Cleanup the sage_cooling module
 *
 * Frees any resources allocated during initialization.
 *
 * @return  0 on success
 */
static int sage_cooling_cleanup(void)
{
    cooling_tables_cleanup();
    INFO_LOG("SAGE cooling module cleaned up");
    return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/* Extern reference to generated loop mode array */
extern const enum LoopMode sage_cooling_supported_modes[];

/**
 * @brief   Module structure for sage_cooling
 *
 * Defines the module interface functions and metadata.
 */
static struct Module sage_cooling_module = {
    .name = "sage_cooling",
    .init = sage_cooling_init,
    .process = sage_cooling_process,
    .cleanup = sage_cooling_cleanup,
    .supported_loop_modes = sage_cooling_supported_modes,
    .num_supported_modes = 1  /* Only supports LOOP_MODE_ALL */
};

/**
 * @brief   Register the sage_cooling module
 *
 * This function is called during program initialization to register
 * the module with the module registry.
 */
void sage_cooling_register(void)
{
    module_registry_add(&sage_cooling_module);
}
