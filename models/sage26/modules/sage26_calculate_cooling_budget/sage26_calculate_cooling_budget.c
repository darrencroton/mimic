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
 * The arithmetic is shared/sage_cooling_physics.h's mimic_sage_hot_cooling_budget, called
 * with CGMrecipeOn 0 (the R0-R3 behaviour).
 *
 * CGMrecipeOn (required, 0 or 1) selects the path. With CGMrecipeOn 1 SAGE26 calls
 * cooling_recipe_regime_aware instead (R/src/core_build_model.c:503-509,
 * R/src/model_cooling_heating.c:125-132), which sage26_regime_aware_cooling owns whole: this module
 * then writes only its no-cooling transport (CoolingGas = CoolingX = Rcool = 0,
 * CoolingReturnPath = -1) and no galaxy property, so sage26_radio_mode_heating and
 * sage26_apply_cooling skip by their own gates. Because a CGMrecipeOn 1 run without the transaction
 * would cool nothing, and one without the classifier would route every galaxy through the hot
 * branch, init() then refuses unless sage26_regime_aware_cooling is configured in the substep phase
 * that holds this module and sage26_determine_regime in pre_timestep.
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
#include "globals.h"
#include "shared/sage_constants.h"
#include "shared/sage_cooling_physics.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "cooling_tables.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_calculate_cooling_budget"

/* SAGE26's CGMrecipeOn (0 off, 1 on), required and checked at init */
static int CGM_RECIPE_ON;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Refuse a CGMrecipeOn other than 0 or 1, naming the parameter
 * @return 0 when supported; -1 otherwise
 */
static int validate_cgm_recipe_on(const int value) {
  if (value != 0 && value != 1) {
    ERROR_LOG("%s: CGMrecipeOn = %d is not supported: the package ports only 0 (classic hot "
              "cooling) and 1 (SAGE26's regime-aware cooling, sage26_regime_aware_cooling)",
              MODULE_NAME, value);
    return -1;
  }
  return 0;
}

/**
 * @brief With CGMrecipeOn 1, require the cooling transaction and the regime classifier
 *
 * With CGMrecipeOn 1 this module writes no cooling: without sage26_regime_aware_cooling in the
 * substep phase that holds this module no galaxy would cool, and without sage26_determine_regime in
 * pre_timestep every Regime would stay -1 and route every galaxy through the hot branch
 * (R/src/model_cooling_heating.c:446). With CGMrecipeOn 0 nothing is required.
 *
 * @return 0 when the configuration is complete (or CGMrecipeOn is 0); -1 otherwise
 */
static int validate_cgm_configuration(void) {
  if (CGM_RECIPE_ON != 1) {
    return 0;
  }
  if (!modules_in_same_substep_phase(MODULE_NAME, PROCESSING_MODE_BY_GALAXY,
                                     "sage26_regime_aware_cooling", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("%s: CGMrecipeOn = 1 requires sage26_regime_aware_cooling (process_by_galaxy) in "
              "the same substep phase (galaxy_physics in the run files): with CGMrecipeOn 1 this "
              "module writes no cooling and that module owns SAGE26's regime-aware cooling",
              MODULE_NAME);
    return -1;
  }
  if (!module_configured_in_phase("sage26_determine_regime", MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("%s: CGMrecipeOn = 1 requires sage26_determine_regime (process_full_halo) in "
              "pre_timestep: without the classifier every Regime stays -1 and the regime-aware "
              "cooling routes every galaxy through the hot branch",
              MODULE_NAME);
    return -1;
  }
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_calculate_cooling_budget_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (validate_cgm_recipe_on(CGM_RECIPE_ON) != 0) {
    return -1;
  }
  if (validate_cgm_configuration() != 0) {
    return -1;
  }

  if (cooling_tables_init(MIMIC_COMPILED_MODEL_PATH
                          "/modules/sage26_calculate_cooling_budget/CoolFunctions") != 0) {
    ERROR_LOG("Failed to initialize cooling function tables");
    return -1;
  }

  VERBOSE_LOG("SAGE calculate cooling budget module initialized");
  VERBOSE_LOG("  CGMrecipeOn = %d%s", CGM_RECIPE_ON,
              CGM_RECIPE_ON == 1 ? " (no-cooling transport only: sage26_regime_aware_cooling cools)"
                                 : "");
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

  // With CGMrecipeOn 1 SAGE26 calls cooling_recipe_regime_aware instead of cooling_recipe_hot
  // (R/src/model_cooling_heating.c:128-129), which sage26_regime_aware_cooling owns whole. The
  // carried state says no cooling, so sage26_radio_mode_heating (CoolingGas > 0 gate) and
  // sage26_apply_cooling (path -1, zero mass) write nothing, and no galaxy property
  // is written here.
  if (CGM_RECIPE_ON == 1) {
    galaxy->CoolingGas = 0.0;
    galaxy->CoolingX = 0.0;
    galaxy->Rcool = 0.0;
    galaxy->CoolingReturnPath = SAGE_COOLING_PATH_NOT_EVALUATED;
    return 0;
  }

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

  // SAGE26's cooling_recipe_hot up to the AGN step, with CGMrecipeOn 0 (R0-R3): the shared
  // implementation writes tcool, tff and RcoolToRvir as SAGE26 does, including its early
  // returns, and never refuses with CGMrecipeOn 0 (only the CGMrecipeOn 1 streams read the
  // stored snapshot's redshift).
  struct MimicSageHotCoolingLocals locals;
  double coolingGas = 0.0;
  if (mimic_sage_hot_cooling_budget(halo, ctx->params, dt_obj, 0, 0.0, &locals, &coolingGas) != 0) {
    return -1;
  }

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
