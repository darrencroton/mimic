/**
 * @file    sage26_calculate_cooling_budget.c
 * @brief   SAGE26 hot-halo cooling budget: the cooling_recipe_hot calculation up to the AGN step
 *
 * Ports SAGE26's cooling_recipe_hot (R/src/model_cooling_heating.c:135-314, R0:
 * CGMrecipeOn 0) up to the point where it calls do_AGN_heating. The routine is
 * split across three modules that run consecutively in galaxy_physics:
 *
 *   1. this module: the diagnostics tcool, tff and RcoolToRvir, the cooling
 *      function, the cooling radius, the rapid or static cooling mass, and its
 *      cap and floor (:139-292);
 *   2. sage26_radio_mode_heating: do_AGN_heating on the post-cap mass (:297-299);
 *   3. sage26_apply_cooling: the routine's tail, Cooling and CoolingRate
 *      (:301-313), then cool_gas_onto_galaxy (R/src/core_build_model.c:508-509).
 *
 * It carries SAGE26's locals between them in package-private transport
 * properties, written on every call because init_repeat resets them only once per
 * snapshot: the post-cap mass (CoolingGas), x (CoolingX), rcool (Rcool) and the
 * return path (CoolingReturnPath), which tells sage26_apply_cooling whether SAGE26
 * reached the tail.
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "shared/sage_constants.h"
#include "shared/time_parity.h"
#include "module_system/physical_constants.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// MODULE-LOCAL CONSTANTS
// ============================================================================

/* Mean molecular weight of a fully ionised gas (R/src/model_cooling_heating.c:52) */
static const double MU_IONISED = 0.59;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/** @brief SAGE26's locals of cooling_recipe_hot that later modules need */
struct cooling_locals {
  double x;     /* PROTONMASS * BOLTZMANN * temp / lambda in code units (:163-164) */
  double rcool; /* cooling radius (:173) */
  int path;     /* SAGE_COOLING_PATH_* */
};

/**
 * @brief SAGE26's cooling_recipe_hot up to the AGN step: the post-cap cooling mass
 *
 * Writes tcool, tff and RcoolToRvir as SAGE26 does, including its early returns,
 * which leave RcoolToRvir (and, through sage26_apply_cooling, Cooling and
 * CoolingRate) untouched. Mvir, Rvir and Vvir are SAGE26 floats
 * (R/src/core_allvars.h:106-221); the core's doubles are read as those floats.
 *
 * @param halo    Halo whose galaxy cools (galaxy not NULL)
 * @param params  Run configuration (unit factors, Hubble_h)
 * @param dt      Substep interval, SAGE26's deltaT / effective_steps
 * @param locals  Output: x, rcool and the return path
 * @return The post-cap, pre-suppression cooling mass (0 on every early return)
 */
static double cooling_recipe_hot(struct Halo *halo, const struct MimicConfig *params,
                                 const double dt, struct cooling_locals *locals) {
  struct GalaxyData *galaxy = halo->galaxy;
  const float rvir = (float)halo->Rvir;
  const float vvir = (float)halo->Vvir;
  double coolingGas;

  locals->x = 0.0;
  locals->rcool = 0.0;

  galaxy->tcool = 0.0f;

  if (!(galaxy->HotGas > 0.0 && vvir > 0.0)) {
    locals->path = SAGE_COOLING_PATH_OUTER_GUARD;
    return 0.0;
  }

  // SAGE parity: SAGE26 divides its float Rvir by its float Vvir in float and
  // stores the result in a double (R/src/model_cooling_heating.c:142); both
  // cooling branches then use that tcool (:190-200).
  const double tcool = rvir / vvir;
  const double tff = M_SQRT2 * tcool;

  // SAGE parity: tcool and tff are written with SAGE26's conversion, a mass-rate
  // factor applied to a time, so neither is a time in Gyr
  // (R/src/model_cooling_heating.c:142-148).
  galaxy->tcool =
      (float)((tcool * 1.0e10 / params->Hubble_h * SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s));
  galaxy->tff =
      (float)((tff * 1.0e10 / params->Hubble_h * SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s));

  const double temp = SAGE_TVIR_K_PER_SQKMS * vvir * vvir; // T_vir in Kelvin

  // The float MetalsHotGas / HotGas is a float division, as in SAGE26 (:152-155)
  double logZ = -10.0;
  if (galaxy->MetalsHotGas > 0) {
    logZ = log10(galaxy->MetalsHotGas / galaxy->HotGas);
  }

  const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);

  // SAGE parity: the lambda <= 0 and rho_rcool <= 0 early returns come after
  // tcool and tff are written and skip the rest of the routine, so RcoolToRvir,
  // Cooling and CoolingRate keep their previous values
  // (R/src/model_cooling_heating.c:157-169, :175, :302, :310).
  if (lambda <= 0.0) {
    locals->path = SAGE_COOLING_PATH_LAMBDA;
    return 0.0;
  }

  double x = PROTONMASS * BOLTZMANN * temp / lambda;         // sec g/cm^3
  x /= (params->UnitDensity_in_cgs * params->UnitTime_in_s); // internal units
  const double rho_rcool = x / tcool * (1.5 * MU_IONISED);   // 3/2 mu, ionised gas
  locals->x = x;

  if (rho_rcool <= 0.0) {
    locals->path = SAGE_COOLING_PATH_RHO_RCOOL;
    return 0.0;
  }

  // An isothermal density profile for the hot gas
  const double rho0 = galaxy->HotGas / (4 * M_PI * rvir);
  const double rcool = sqrt(rho0 / rho_rcool);
  locals->rcool = rcool;

  galaxy->RcoolToRvir = rcool / rvir;

  // SAGE parity: rcool is left uncapped, and the rapid branch cools the whole
  // corona in one tcool while the static branch cools at half that rate at
  // rcool == Rvir: the branches are discontinuous by a factor 2, as SAGE26
  // keeps them (R/src/model_cooling_heating.c:177-200).
  if (rcool > rvir) {
    coolingGas = galaxy->HotGas / tcool * dt;
  } else {
    // HotGas / Rvir is a float division, as in SAGE26 (:199)
    coolingGas = (galaxy->HotGas / rvir) * (rcool / (2.0 * tcool)) * dt;
  }

  if (coolingGas > galaxy->HotGas) {
    coolingGas = galaxy->HotGas;
  } else {
    if (coolingGas < 0.0)
      coolingGas = 0.0;
  }

  locals->path = SAGE_COOLING_PATH_TAIL;
  return coolingGas;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_calculate_cooling_budget_init(void) {
  if (cooling_tables_init(MIMIC_COMPILED_MODEL_PATH
                          "/modules/sage26_calculate_cooling_budget/CoolFunctions") != 0) {
    ERROR_LOG("Failed to initialize cooling function tables");
    return -1;
  }

  VERBOSE_LOG("SAGE calculate cooling budget module initialized");
  return 0;
}

int sage26_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                            int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: cooling runs for every non-merged galaxy, Type 2 orphans
  // included: SAGE26 calls cooling_recipe for every galaxy with mergeType 0
  // before the merger pass (R/src/core_build_model.c:473-516). An orphan keeps
  // its last Rvir and Vvir, and the outer guard needs only HotGas > 0 and
  // Vvir > 0 (R/src/model_cooling_heating.c:141).
  if (halo->galaxy == NULL) {
    return 0;
  }

  struct GalaxyData *galaxy = halo->galaxy;

  dt_status = mimic_object_substep_dt(halo, ctx, &dt_obj);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    // No positive interval (the core's snapshot-zero birth): cooling does not
    // run, and the carried state says so to the later modules.
    galaxy->CoolingGas = 0.0;
    galaxy->CoolingX = 0.0;
    galaxy->Rcool = 0.0;
    galaxy->CoolingReturnPath = SAGE_COOLING_PATH_NOT_EVALUATED;
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid cooling dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
              halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
              mimic_object_time_status_str(dt_status));
    return -1;
  }

  struct cooling_locals locals;
  const double coolingGas = cooling_recipe_hot(halo, ctx->params, dt_obj, &locals);

  // Carried to the radio and apply modules as doubles, as SAGE26 keeps these
  // locals; every entry is written on every call.
  galaxy->CoolingGas = coolingGas;
  galaxy->CoolingX = locals.x;
  galaxy->Rcool = locals.rcool;
  galaxy->CoolingReturnPath = locals.path;

  return 0;
}

int sage26_calculate_cooling_budget_cleanup(void) {
  cooling_tables_cleanup();
  VERBOSE_LOG("SAGE calculate cooling budget module cleaned up");
  return 0;
}
