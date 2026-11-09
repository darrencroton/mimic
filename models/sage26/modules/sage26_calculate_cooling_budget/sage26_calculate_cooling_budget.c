/**
 * @file    sage26_calculate_cooling_budget.c
 * @brief   SAGE26 hot-halo cooling budget: the cooling_recipe_hot calculation up to the AGN step
 *
 * Computes the hot-halo cooling diagnostics (tcool, tff, RcoolToRvir), the cooling radius and the
 * capped rapid or static cooling mass, and carries the mass, x, rcool and the return path to
 * sage26_radio_mode_heating (AGN suppression) and sage26_apply_cooling (the transfer). Implements
 * SAGE26's cooling_recipe_hot up to its do_AGN_heating call (shared/sage_cooling_physics.h).
 *
 * With CGMrecipeOn 1 SAGE26 calls cooling_recipe_regime_aware instead, which
 * sage26_regime_aware_cooling owns: this module then writes only its no-cooling transport
 * (CoolingGas = CoolingX = Rcool = 0, CoolingReturnPath = -1), and init() refuses unless
 * sage26_regime_aware_cooling is in the same substep phase and sage26_determine_regime in
 * pre_timestep. The transport is written on every call, as init_repeat resets it only once per
 * snapshot.
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "shared/sage_cgm_routing.h"
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
 * @brief With CGMrecipeOn 1, require the cooling transaction and the regime classifier
 *
 * With CGMrecipeOn 1 this module writes no cooling: without sage26_regime_aware_cooling in the
 * substep phase that holds this module no galaxy would cool, and without sage26_determine_regime in
 * pre_timestep every Regime would stay -1 and route every galaxy through the hot branch.
 * With CGMrecipeOn 0 nothing is required.
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
  if (mimic_sage_validate_cgm_recipe_on(MODULE_NAME, CGM_RECIPE_ON) != 0) {
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
  // included, before the merger pass. An orphan keeps its last Rvir and Vvir, and
  // the outer guard needs only HotGas > 0 and Vvir > 0.
  if (halo->galaxy == NULL) {
    return 0;
  }

  struct GalaxyData *galaxy = halo->galaxy;

  // With CGMrecipeOn 1 SAGE26 calls cooling_recipe_regime_aware instead of cooling_recipe_hot,
  // which sage26_regime_aware_cooling owns whole. The carried state says no cooling, so
  // sage26_radio_mode_heating (CoolingGas > 0 gate) and sage26_apply_cooling (path -1, zero
  // mass) write nothing, and no galaxy property is written here.
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

  // SAGE26's cooling_recipe_hot up to the AGN step, with CGMrecipeOn 0: the shared
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
