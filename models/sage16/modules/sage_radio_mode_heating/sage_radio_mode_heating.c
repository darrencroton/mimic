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

#include "constants.h"
#include "error.h"
#include "shared/metallicity.h"
#include "shared/sage_constants.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"
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
                                           const struct MimicConfig *run_params) {
  const double unit_conv =
      run_params->UnitMass_in_g / run_params->UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS;

  double AGNrate;
  if (mvir > 0.0) {
    AGNrate = RADIO_MODE_EFFICIENCY / unit_conv *
              (black_hole_mass / 0.01) *                         // Normalized to M_BH = 10^8 M_sun
              (vvir / 200.0) * (vvir / 200.0) * (vvir / 200.0) * // Normalized to V_vir = 200 km/s
              ((hot_gas / mvir) / 0.1);                          // Normalized to f_hot = 0.1
  } else {
    AGNrate = RADIO_MODE_EFFICIENCY / unit_conv * (black_hole_mass / 0.01) * (vvir / 200.0) *
              (vvir / 200.0) * (vvir / 200.0);
  }

  return AGNrate;
}

/**
 * @brief AGN Mode 2: Bondi-Hoyle accretion recipe
 *
 * Based on spherical accretion: M_dot = 4π G² M_BH² ρ / c_s³
 */
static double calculate_agn_rate_bondi(const double black_hole_mass, const double vvir,
                                       const double lambda, const struct MimicConfig *run_params) {
  const double temp = SAGE_TVIR_K_PER_SQKMS * vvir * vvir; // T_vir in Kelvin

  double x = PROTONMASS * BOLTZMANN * temp / lambda;                 // sec * g/cm^3
  x /= (run_params->UnitDensity_in_cgs * run_params->UnitTime_in_s); // convert to code units

  const double AGNrate =
      (2.5 * M_PI * run_params->G) * (0.375 * 0.6 * x) * black_hole_mass * RADIO_MODE_EFFICIENCY;

  return AGNrate;
}

/**
 * @brief AGN Mode 3: Cold cloud accretion recipe
 *
 * Triggered when BH mass exceeds threshold: M_BH > 10^-4 * M_vir * (R_cool/R_vir)^3
 * Accretion rate = 0.01% of cooling rate when triggered.
 */
static double calculate_agn_rate_cold_cloud(const double black_hole_mass, const double mvir,
                                            const double rcool, const double rvir,
                                            const double coolingGas, const double dt) {
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
static void do_AGN_heating(struct Halo *halo, struct ModuleContext *ctx, const double dt) {
  double AGNrate, EDDrate, AGNaccreted, AGNcoeff, AGNheating, metallicity;

  // Get properties
  const double hot_gas = halo->galaxy->HotGas;
  const double metals_hot_gas = halo->galaxy->MetalsHotGas;
  const double black_hole_mass = halo->galaxy->BlackHoleMass;
  const double lambda = halo->galaxy->CoolingLambda;
  const double rheat = halo->galaxy->Rheat;
  const double mvir = halo->Mvir;
  const double vvir = halo->Vvir;
  const double rvir = halo->Rvir;

  double coolingGas = (double)halo->galaxy->CoolingGas;
  const double rcool = halo->galaxy->Rcool;

  // First update cooling rate based on past AGN heating (SAGE parity:
  // rheat < rcool gives partial suppression, otherwise complete suppression;
  // rcool == 0 with rheat == 0 falls into the complete-suppression branch)
  if (rheat < rcool) {
    // Partial suppression based on heating radius fraction
    coolingGas = (1.0 - rheat / rcool) * coolingGas;
  } else {
    // Complete suppression if heating radius exceeds cooling radius
    coolingGas = 0.0;
  }

  // Now calculate the new heating rate from black hole accretion
  if (hot_gas > 0.0) {
    // Select AGN accretion mode
    if (AGN_RECIPE_ON == 2) {
      // Bondi-Hoyle accretion recipe
      AGNrate = calculate_agn_rate_bondi(black_hole_mass, vvir, lambda, ctx->params);
    } else if (AGN_RECIPE_ON == 3) {
      // Cold cloud accretion recipe
      AGNrate = calculate_agn_rate_cold_cloud(black_hole_mass, mvir, rcool, rvir, coolingGas, dt);
    } else {
      // Empirical (default) accretion recipe
      AGNrate = calculate_agn_rate_empirical(black_hole_mass, mvir, vvir, hot_gas, ctx->params);
    }

    // Eddington limit: L_edd = 1.3e38 * M_BH erg/s
    EDDrate = (1.3e38 * black_hole_mass * 1e10 / ctx->params->Hubble_h) /
              (ctx->params->UnitEnergy_in_cgs / ctx->params->UnitTime_in_s) /
              (0.1 * 9e10); // 0.1 = radiative efficiency, 9e10 = c^2 in (km/s)^2

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
    if (rheat < rcool && coolingGas > 0.0) {
      double rheat_new = (AGNheating / coolingGas) * rcool;
      if (rheat_new > rheat) {
        halo->galaxy->Rheat = rheat_new;
      }
    }

    // Track heating energy
    if (AGNheating > 0.0 && halo->dT > 0.0) {
      halo->galaxy->Heating += (0.5 * AGNheating * vvir * vvir) / halo->dT;
    }
  }

  // Update CoolingGas property to reflect AGN suppression
  halo->galaxy->CoolingGas = coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_radio_mode_heating_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                    "AGN radio mode heating efficiency");
  LOAD_AND_VALIDATE_OPTION("AGNrecipe", AGN_RECIPE_ON, 3,
                           "0=off, 1=empirical, 2=Bondi, 3=cold cloud");

  VERBOSE_LOG("SAGE radio-mode AGN heating module initialized");
  VERBOSE_LOG("  RadioModeEfficiency = %.4f", RADIO_MODE_EFFICIENCY);
  VERBOSE_LOG("  AGNrecipe = %d (0=off, 1=empirical, 2=Bondi, 3=cold cloud)", AGN_RECIPE_ON);
  return 0;
}

int sage_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: radio-mode AGN heating suppresses a galaxy's own cooling via
  // its own black hole, for every non-merged galaxy including Type 2 orphans
  // (SAGE calls do_AGN_heating from cooling_recipe for all galaxies).
  if (halo->galaxy == NULL) {
    return 0;
  }

  // Only apply AGN heating if cooling is occurring and AGN is enabled
  // (SAGE parity: cooling_recipe gates do_AGN_heating on coolingGas > 0.0)
  if (halo->galaxy->CoolingGas > 0.0 && AGN_RECIPE_ON > 0) {
    dt_status = mimic_object_substep_dt(halo, ctx, &dt_obj);
    if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
      return 0;
    }
    if (dt_status != MIMIC_OBJECT_TIME_OK) {
      ERROR_LOG(
          "Invalid radio-mode dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
          halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
          mimic_object_time_status_str(dt_status));
      return -1;
    }
    do_AGN_heating(halo, ctx, dt_obj);
  }

  return 0;
}

int sage_radio_mode_heating_cleanup(void) {
  VERBOSE_LOG("SAGE radio-mode heating module cleaned up");
  return 0;
}
