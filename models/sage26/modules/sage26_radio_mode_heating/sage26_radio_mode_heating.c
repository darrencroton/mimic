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
 * The arithmetic is shared/sage_cooling_physics.h's mimic_sage_do_agn_heating_hot and
 * mimic_sage_agn_accretion_compute, called with CGMrecipeOn 0 (the R0-R3 behaviour). With
 * CGMrecipeOn 1 SAGE26's do_AGN_heating adds the 0.99 * rcool saturation and the cap of Rheat to
 * Rvir (:590-595, :623-624), which apply there: sage26_regime_aware_cooling calls those helpers
 * with CGMrecipeOn 1 inside SAGE26's regime-aware cooling, and this module then writes nothing, as
 * sage26_calculate_cooling_budget carries CoolingGas 0 and the gate below returns.
 *
 * AGNrecipe 2 (Bondi-Hoyle) is SAGE26's R0 recipe and is ported exactly. The
 * empirical (1) and cold-cloud (3) rates are the earlier package's, not ported.
 *
 * Reference: Croton et al. (2006, 2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "shared/sage_constants.h"
#include "shared/sage_cooling_physics.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RADIO_MODE_EFFICIENCY;
static int AGN_RECIPE_ON;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_radio_mode_heating_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", RADIO_MODE_EFFICIENCY, 0.0, 1.0,
                                    "AGN radio mode heating efficiency");
  LOAD_AND_VALIDATE_OPTION("AGNrecipe", AGN_RECIPE_ON, 3,
                           "0=off, 1=empirical, 2=Bondi, 3=cold cloud");

  /* Placement, checked when the partner is configured: after the budget, whose CoolingGas,
   * CoolingX and Rcool this module reads, and before the apply step, which commits the
   * suppressed mass, in the same substep phase */
  if (module_configured_anywhere("sage26_calculate_cooling_budget") &&
      !module_precedes_in_substep_phase("sage26_calculate_cooling_budget",
                                        PROCESSING_MODE_BY_GALAXY, "sage26_radio_mode_heating",
                                        PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_radio_mode_heating requires sage26_calculate_cooling_budget to precede it "
              "in the same substep phase — otherwise it reads the previous substep's cooling");
    return -1;
  }
  if (module_configured_anywhere("sage26_apply_cooling") &&
      !module_precedes_in_substep_phase("sage26_radio_mode_heating", PROCESSING_MODE_BY_GALAXY,
                                        "sage26_apply_cooling", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_radio_mode_heating must precede sage26_apply_cooling in the same substep "
              "phase — otherwise the apply step commits the unsuppressed cooling");
    return -1;
  }

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
  // field, the carried mass included, is left as it is. With CGMrecipeOn 1 the budget
  // carries CoolingGas 0, so this module writes nothing (sage26_regime_aware_cooling runs
  // the hot and CGM AGN calls itself).
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
  // (SAGE26's do_AGN_heating with CGMrecipeOn 0, the R0-R3 behaviour: no 0.99 * rcool
  // saturation and no cap of Rheat to Rvir, R/src/model_cooling_heating.c:590-595, :623-624;
  // with CGMrecipeOn 1 both apply, in sage26_regime_aware_cooling's call of the same helper)
  galaxy->CoolingGas = mimic_sage_do_agn_heating_hot(
      halo, ctx->params, AGN_RECIPE_ON, RADIO_MODE_EFFICIENCY, 0, dt_obj, galaxy->CoolingGas,
      galaxy->CoolingX, galaxy->Rcool, NULL);

  return 0;
}

int sage26_radio_mode_heating_cleanup(void) {
  VERBOSE_LOG("SAGE radio-mode heating module cleaned up");
  return 0;
}
