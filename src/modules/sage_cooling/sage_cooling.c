/**
 * @file    sage_cooling.c
 * @brief   SAGE cooling and AGN heating module implementation
 *
 * This module implements gas cooling from hot halos to cold disks with
 * AGN feedback from the SAGE model. Key processes:
 * - Gas cooling based on cooling radius and metallicity-dependent rates
 * - Two cooling regimes: cold accretion vs hot halo cooling
 * - AGN radio-mode feedback suppression of cooling
 * - Black hole accretion and growth
 * - Tracking of heating radius and energy budgets
 *
 * Physics Summary:
 *   T_vir = 35.9 * Vvir^2 (temperature in K, velocity in km/s)
 *   Lambda(T, Z) = cooling function from Sutherland & Dopita (1993) tables
 *   rcool = cooling radius where tcool = tdyn
 *   Cold accretion (rcool > Rvir): rapid cooling throughout halo
 *   Hot halo (rcool < Rvir): cooling only within rcool
 *
 * Implementation Notes:
 * - Only central galaxies cool (satellites have no accretion)
 * - Metal content preserved during gas transfers
 * - AGN feedback can completely suppress cooling
 * - Energy budgets tracked for cooling and heating
 *
 * References:
 *   - SAGE: sage-code/model_cooling_heating.c, core_cool_func.c
 *   - White & Frenk (1991) - Cooling model framework
 *   - Croton et al. (2006) - AGN feedback implementation
 *   - Sutherland & Dopita (1993) - Cooling function tables
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Updates GalaxyData properties only
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "../shared/metallicity.h"  // Shared utility for metallicity calculations
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_cooling.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/**
 * @brief   Radio-mode AGN feedback efficiency
 *
 * Controls the strength of AGN heating that suppresses gas cooling.
 * Higher values = stronger feedback = more cooling suppression.
 * Typical value: 0.01 (1% of accreted rest mass energy goes into heating)
 *
 * Configuration: SageCooling_RadioModeEfficiency
 */
static double RADIO_MODE_EFFICIENCY = 0.01;

/**
 * @brief   AGN accretion recipe selector
 *
 * Selects which black hole accretion model to use:
 *   0 = No AGN (cooling only, no feedback)
 *   1 = Empirical scaling (default) - scales with BH mass, Vvir, gas fraction
 *   2 = Bondi-Hoyle accretion - based on gas density and BH mass
 *   3 = Cold cloud accretion - triggered when BH mass exceeds threshold
 *
 * Configuration: SageCooling_AGNrecipeOn
 */
static int AGN_RECIPE_ON = 1;

/**
 * @brief   Path to cooling function tables directory
 *
 * Directory containing the Sutherland & Dopita (1993) cooling tables.
 * Expected files: stripped_mzero.cie, stripped_m-30.cie, ..., stripped_m+05.cie
 * Tables are stored with the module as they are model-specific.
 *
 * Configuration: SageCooling_CoolFunctionsDir
 */
static char COOL_FUNCTIONS_DIR[512] = "src/modules/sage_cooling/CoolFunctions";

// ============================================================================
// HELPER FUNCTIONS (Physics Calculations)
// ============================================================================

// Metallicity calculation now provided by shared utility: mimic_get_metallicity()
// See: src/modules/shared/metallicity.h

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

    /* Calculate dynamical time of the halo (approximation for cooling time) */
    tcool = safe_div(rvir, vvir, 0.0);

    /* Calculate virial temperature from virial velocity
     * T = 35.9 * V^2 where V is in km/s and T is in Kelvin
     * This comes from T = (μ*m_p*V^2)/(2*k_B) with μ=0.59 for ionized gas */
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
    *x = PROTONMASS * BOLTZMANN * temp / lambda;

    /* Convert to simulation units */
    *x /= (ctx->params->UnitDensity_in_cgs * ctx->params->UnitTime_in_s);

    /* Calculate density at cooling radius
     * Factor 0.885 = 3/2 * mu, where mu=0.59 for fully ionized gas
     * This is the density where cooling time equals dynamical time */
    rho_rcool = safe_div(*x, tcool, 0.0) * 0.885;

    /* Calculate central density assuming isothermal profile for hot gas */
    rho0 = safe_div(hot_gas, 4.0 * M_PI * rvir, 0.0);

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
        coolingGas = safe_div(hot_gas, rvir, 0.0) * safe_div(*rcool, 2.0 * tcool, 0.0) * dt;
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

    /* Calculate the new heating rate from black hole accretion */
    if (hot_gas > EPSILON_SMALL) {
        /* Choose accretion model based on configuration */
        if (AGN_RECIPE_ON == 2) {
            /* Bondi-Hoyle accretion recipe
             * Based on BH mass and local gas properties
             * Formula: AGNrate ~ G * rho * M_BH^2 / c_s^3 */
            AGNrate = (2.5 * M_PI * ctx->params->G) * (0.375 * 0.6 * x) *
                     black_hole_mass * RADIO_MODE_EFFICIENCY;

        } else if (AGN_RECIPE_ON == 3) {
            /* Cold cloud accretion model
             * Triggered when BH mass exceeds threshold related to cooling properties
             * Accretion rate = 0.01% of cooling rate when triggered */
            if (black_hole_mass > 0.0001 * mvir * pow(safe_div(rcool, rvir, 0.0), 3.0))
                AGNrate = 0.0001 * safe_div(coolingGas, dt, 0.0);
            else
                AGNrate = 0.0;

        } else {
            /* Empirical (standard) accretion recipe
             * Scales with black hole mass, virial velocity, and hot gas fraction
             * Formula based on simulation fits */
            double unit_conv = ctx->params->UnitMass_in_g / ctx->params->UnitTime_in_s *
                             SEC_PER_YEAR / SOLAR_MASS;

            if (mvir > EPSILON_SMALL)
                AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
                         (black_hole_mass / 0.01) *
                         pow(vvir / 200.0, 3.0) *
                         (safe_div(hot_gas, mvir, 0.0) / 0.1);
            else
                AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
                         (black_hole_mass / 0.01) *
                         pow(vvir / 200.0, 3.0);
        }

        /* Calculate Eddington accretion rate limit
         * L_Edd = 1.3e38 * (M_BH/M_sun) erg/s
         * Convert to mass accretion rate using E = 0.1 * m * c^2 efficiency */
        EDDrate = (1.3e38 * black_hole_mass * 1e10 / ctx->params->Hubble_h) /
                 (ctx->params->UnitEnergy_in_cgs / ctx->params->UnitTime_in_s) /
                 (0.1 * 9e10);

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
        AGNcoeff = pow(safe_div(1.34e5, vvir, 1.0), 2.0);

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
            halo->galaxy->Heating += 0.5 * AGNheating * vvir * vvir;
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
    halo->galaxy->Cooling += 0.5 * coolingGas * vvir * vvir;
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
    /* Read module parameters from parameter file */
    module_get_double("SageCooling", "RadioModeEfficiency", &RADIO_MODE_EFFICIENCY, 0.01);
    module_get_int("SageCooling", "AGNrecipeOn", &AGN_RECIPE_ON, 1);

    /* Note: CoolFunctionsDir is set to default value "input/CoolFunctions"
     * String parameters not yet supported by module_get_* interface.
     * TODO: Add module_get_string() support for string parameters */

    /* Validate parameters */
    if (RADIO_MODE_EFFICIENCY < 0.0) {
        ERROR_LOG("SageCooling_RadioModeEfficiency must be non-negative (got %.4f)",
                 RADIO_MODE_EFFICIENCY);
        return -1;
    }

    if (AGN_RECIPE_ON < 0 || AGN_RECIPE_ON > 3) {
        ERROR_LOG("SageCooling_AGNrecipeOn must be 0, 1, 2, or 3 (got %d)", AGN_RECIPE_ON);
        return -1;
    }

    /* Initialize cooling function tables */
    if (cooling_tables_init(COOL_FUNCTIONS_DIR) != 0) {
        ERROR_LOG("Failed to initialize cooling function tables");
        return -1;
    }

    /* Log module configuration */
    INFO_LOG("SAGE cooling & AGN heating module initialized");
    INFO_LOG("  RadioModeEfficiency = %.4f", RADIO_MODE_EFFICIENCY);
    INFO_LOG("  AGNrecipeOn = %d (0=off, 1=empirical, 2=Bondi, 3=cold cloud)", AGN_RECIPE_ON);
    INFO_LOG("  CoolFunctionsDir = %s", COOL_FUNCTIONS_DIR);

    return 0;
}

/**
 * @brief   Process halos for cooling and AGN heating
 *
 * Main processing function called once per timestep for each forest.
 * Calculates cooling rates and AGN feedback for all central galaxies.
 *
 * @param   ctx    Module context with simulation parameters and timestep info
 * @param   halos  Array of halos to process
 * @param   ngal   Number of halos in the array
 * @return  0 on success, -1 on failure
 */
static int sage_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    double dt, coolingGas, x, rcool;

    /* Process each halo */
    for (int i = 0; i < ngal; i++) {
        /* Only central galaxies cool (Type == 0)
         * Satellites don't accrete fresh gas (handled by sage_infall module) */
        if (halos[i].Type != 0)
            continue;

        /* Get time step from halo property dT (time since progenitor in Myr)
         * Convert from Myr to code units using UnitTime_in_Megayears */
        if (halos[i].dT > EPSILON_SMALL)
            dt = halos[i].dT / ctx->params->UnitTime_in_Megayears;
        else
            dt = 0.0;  /* No cooling if no time has elapsed */

        /* Calculate cooling rate */
        coolingGas = cooling_recipe(&halos[i], ctx, dt, &x, &rcool);

        /* Apply AGN heating if enabled and cooling is occurring */
        if (AGN_RECIPE_ON > 0 && coolingGas > EPSILON_SMALL) {
            coolingGas = do_AGN_heating(&halos[i], coolingGas, ctx, dt, x, rcool);
        }

        /* Transfer cooled gas from hot to cold reservoir */
        if (coolingGas > EPSILON_SMALL) {
            cool_gas_onto_galaxy(&halos[i], coolingGas, halos[i].Vvir);

            DEBUG_LOG("Central galaxy cooled: Mvir=%.3e, coolingGas=%.3e, z=%.3f",
                     halos[i].Mvir, coolingGas, ctx->redshift);
        }
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
    DEBUG_LOG("SAGE cooling module cleaned up");
    return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module structure for sage_cooling
 *
 * Defines the module interface functions and metadata.
 */
static struct Module sage_cooling_module = {
    .name = "sage_cooling",
    .init = sage_cooling_init,
    .process_halos = sage_cooling_process,
    .cleanup = sage_cooling_cleanup
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
