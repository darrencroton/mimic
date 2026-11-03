/**
 * @file    sage26_radio_mode_heating.c
 * @brief   SAGE26 radio-mode AGN heating: do_AGN_heating on the hot-cooling path
 *
 * Ports SAGE26's do_AGN_heating and its private agn_accretion_compute
 * (R/src/model_cooling_heating.c:526-626) as cooling_recipe_hot calls them on R0
 * (:297-299, CGMrecipeOn 0). It reads the post-cap cooling mass and SAGE26's x and
 * rcool locals that sage26_calculate_cooling_budget carries (CoolingGas, CoolingX,
 * Rcool), suppresses the mass by the earlier heating radius, grows the black hole
 * from the hot gas, ratchets Rheat, accumulates Heating undivided, and writes the
 * suppressed mass back into CoolingGas for sage26_apply_cooling.
 *
 * AGNrecipe 2 (Bondi-Hoyle) is SAGE26's R0 recipe and is ported exactly. The
 * empirical (1) and cold-cloud (3) rates are the earlier package's, not ported.
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
 * @brief AGN Mode 2: Bondi-Hoyle accretion recipe (Bondi 1952)
 *
 * SAGE26's Bondi branch of agn_accretion_compute (R/src/model_cooling_heating.c:538-540),
 * with x carried from the cooling budget rather than recomputed from the cooling function.
 */
static double calculate_agn_rate_bondi(const double black_hole_mass, const double x,
                                       const struct MimicConfig *run_params) {
  return (2.5 * M_PI * run_params->G) * (0.375 * 0.6 * x) * black_hole_mass * RADIO_MODE_EFFICIENCY;
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
 * @brief SAGE26's agn_accretion_compute: accreted mass and heating mass
 *
 * Accretion rate by recipe, the Eddington cap, the reservoir cap, the heating
 * coefficient and the cap of heating to the suppressed cooling mass, in SAGE26's
 * order and with its Vvir > 0 and AGNcoeff > 0 guards
 * (R/src/model_cooling_heating.c:526-575). Reads no field it writes. The heating
 * coefficient uses SAGE26's float Vvir; the empirical and cold-cloud rates, which
 * are not ported, keep the core's double halo values they always used.
 */
static void agn_accretion_compute(const struct Halo *halo, const double dt, const double x,
                                  const double coolingGas, const double rcool,
                                  const double reservoir_mass, const struct MimicConfig *params,
                                  double *accreted_out, double *heating_out) {
  const double black_hole_mass = halo->galaxy->BlackHoleMass;
  const double vvir = (float)halo->Vvir;

  double AGNrate;
  if (AGN_RECIPE_ON == 2) {
    AGNrate = calculate_agn_rate_bondi(black_hole_mass, x, params);
  } else if (AGN_RECIPE_ON == 3) {
    AGNrate = calculate_agn_rate_cold_cloud(black_hole_mass, halo->Mvir, rcool, halo->Rvir,
                                            coolingGas, dt);
  } else {
    AGNrate = calculate_agn_rate_empirical(black_hole_mass, halo->Mvir, halo->Vvir, reservoir_mass,
                                           params);
  }

  // Eddington-limited accretion: L_edd = 1.3e38 erg/s per Msun, radiative
  // efficiency 0.1, c^2 = 9e10 (km/s)^2 (R/src/model_cooling_heating.c:65-67, :553-557)
  const double EDDrate = (1.3e38 * black_hole_mass * 1e10 / params->Hubble_h) /
                         (params->UnitEnergy_in_cgs / params->UnitTime_in_s) / (0.1 * 9.0e10);
  if (AGNrate > EDDrate)
    AGNrate = EDDrate;

  double AGNaccreted = AGNrate * dt;
  if (AGNaccreted > reservoir_mass)
    AGNaccreted = reservoir_mass;

  // Heating coefficient 1.34e5 km/s = sqrt(2 eta c^2) (:72, :562-572)
  double AGNheating = 0.0;
  if (vvir > 0.0) {
    const double AGNcoeff = (1.34e5 / vvir) * (1.34e5 / vvir);
    AGNheating = AGNcoeff * AGNaccreted;
    // Limit heating to the available cooling mass
    if (AGNheating > coolingGas && AGNcoeff > 0.0) {
      AGNaccreted = coolingGas / AGNcoeff;
      AGNheating = coolingGas;
    }
  }

  *accreted_out = AGNaccreted;
  *heating_out = AGNheating;
}

/**
 * @brief SAGE26's do_AGN_heating on R0: suppress the post-cap cooling mass, grow the BH
 *
 * Reproduces R/src/model_cooling_heating.c:587-626 with CGMrecipeOn 0. Vvir is a
 * SAGE26 float (R/src/core_allvars.h:106-221); the core's double is read as that
 * float.
 *
 * @param halo        Halo whose galaxy is heated (galaxy not NULL)
 * @param params      Run configuration
 * @param dt          Substep interval, SAGE26's deltaT / effective_steps
 * @param coolingGas  Post-cap cooling mass from the cooling budget (positive)
 * @param x           SAGE26's x, carried from the cooling budget
 * @param rcool       SAGE26's rcool, carried from the cooling budget
 * @return The AGN-suppressed cooling mass
 */
static double do_AGN_heating(struct Halo *halo, const struct MimicConfig *params, const double dt,
                             double coolingGas, const double x, const double rcool) {
  struct GalaxyData *galaxy = halo->galaxy;
  const float vvir = (float)halo->Vvir;

  // SAGE parity: the cooling mass is suppressed only by the heating radius from
  // earlier events, and Rheat >= rcool suppresses it completely; the CGM-only
  // 0.99 * rcool saturation does not apply on R0
  // (R/src/model_cooling_heating.c:590-595).
  if (galaxy->Rheat < rcool) {
    coolingGas = (1.0 - galaxy->Rheat / rcool) * coolingGas;
  } else {
    coolingGas = 0.0;
  }

  if (galaxy->HotGas > 0.0) {
    double AGNaccreted, AGNheating;
    agn_accretion_compute(halo, dt, x, coolingGas, rcool, galaxy->HotGas, params, &AGNaccreted,
                          &AGNheating);

    // Metallicity of the hot gas before the accretion is drawn from it (:605-608)
    const double metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
    galaxy->BlackHoleMass += AGNaccreted;
    galaxy->HotGas -= AGNaccreted;
    galaxy->MetalsHotGas -= metallicity * AGNaccreted;

    // The heating radius ratchet: it only grows (:612-616)
    if (galaxy->Rheat < rcool && coolingGas > 0.0) {
      const double rheat_new = (AGNheating / coolingGas) * rcool;
      if (rheat_new > galaxy->Rheat)
        galaxy->Rheat = rheat_new;
    }

    // Heating energy, accumulated undivided; sage26_finalize_cooling_heating
    // normalises it once per snapshot (:618-620, R/src/core_build_model.c:590-603)
    if (AGNheating > 0.0)
      galaxy->Heating += 0.5 * AGNheating * vvir * vvir;
  }

  // SAGE parity: the AGN heating is not subtracted from the cooling mass a second
  // time; only the earlier heating radius suppresses it, and the returned mass is
  // the radius-suppressed one (R/src/model_cooling_heating.c:590-626). The
  // CGM-only cap of Rheat to Rvir (:623-624) does not apply on R0.
  return coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_radio_mode_heating_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                    "AGN radio mode heating efficiency");
  LOAD_AND_VALIDATE_OPTION("AGNrecipe", AGN_RECIPE_ON, 3,
                           "0=off, 1=empirical, 2=Bondi, 3=cold cloud");

  VERBOSE_LOG("SAGE radio-mode AGN heating module initialized");
  VERBOSE_LOG("  RadioModeEfficiency = %.4f", RADIO_MODE_EFFICIENCY);
  VERBOSE_LOG("  AGNrecipe = %d (0=off, 1=empirical, 2=Bondi, 3=cold cloud)", AGN_RECIPE_ON);
  return 0;
}

int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: radio-mode heating suppresses a galaxy's own cooling with its
  // own black hole for every non-merged galaxy, Type 2 orphans included: SAGE26
  // calls do_AGN_heating from cooling_recipe_hot for every galaxy it cools
  // (R/src/core_build_model.c:473-516, R/src/model_cooling_heating.c:297-299).
  if (halo->galaxy == NULL) {
    return 0;
  }

  struct GalaxyData *galaxy = halo->galaxy;

  // SAGE26 calls do_AGN_heating only with AGNrecipeOn > 0 and a positive
  // post-cap cooling mass (R/src/model_cooling_heating.c:297); otherwise every
  // field, the carried mass included, is left as it is.
  if (!(AGN_RECIPE_ON > 0 && galaxy->CoolingGas > 0.0)) {
    return 0;
  }

  dt_status = mimic_object_substep_dt(halo, ctx, &dt_obj);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG(
        "Invalid radio-mode dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
        halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
        mimic_object_time_status_str(dt_status));
    return -1;
  }

  // The suppressed mass replaces the post-cap mass for sage26_apply_cooling
  galaxy->CoolingGas = do_AGN_heating(halo, ctx->params, dt_obj, galaxy->CoolingGas,
                                      galaxy->CoolingX, galaxy->Rcool);

  return 0;
}

int sage26_radio_mode_heating_cleanup(void) {
  VERBOSE_LOG("SAGE radio-mode heating module cleaned up");
  return 0;
}
