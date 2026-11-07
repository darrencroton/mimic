/**
 * @file    sage26_apply_cooling.c
 * @brief   SAGE26 cooling application: the tail of cooling_recipe_hot, then cool_gas_onto_galaxy
 *
 * The last of the three modules SAGE26's hot cooling is split across (see
 * sage26_calculate_cooling_budget). CoolingGas holds the cooled mass after AGN
 * suppression, the mass SAGE26's cooling_recipe_hot returns. This module first runs
 * that routine's tail (R/src/model_cooling_heating.c:301-313): Cooling accumulates
 * 0.5 * m * Vvir^2 undivided, and CoolingRate records the mass over the substep.
 * It then moves the mass from the hot to the cold reservoir with SAGE26's
 * cool_gas_onto_galaxy (:689-705), as R/src/core_build_model.c:508-509 calls it.
 *
 * The tail reads only the mass, Vvir and dt, none of which the transfer changes,
 * so running it here gives SAGE26's values. It is the first module that always
 * holds the final (suppressed) mass. sage26_finalize_cooling_heating normalises
 * Cooling once per snapshot (R/src/core_build_model.c:590-603).
 *
 * This is the CGMrecipeOn 0 (R0-R3) path. With CGMrecipeOn 1 SAGE26 never calls
 * cool_gas_onto_galaxy: its regime-aware wrapper applies the CGM and hot masses and writes
 * CoolingRate itself (R/src/model_cooling_heating.c:475-502), which sage26_regime_aware_cooling,
 * the next module, ports; sage26_calculate_cooling_budget then carries path -1 and zero mass, so
 * this module writes nothing.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "shared/sage_constants.h"
#include "shared/sage_cooling_physics.h"
#include "shared/time_parity.h"
#include "module_interface.h"
#include "module_registry.h"
#include "globals.h"
#include "types.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief The tail of SAGE26's cooling_recipe_hot: Cooling and CoolingRate
 *
 * Reproduces R/src/model_cooling_heating.c:301-313 with SAGE26's float Vvir
 * (R/src/core_allvars.h:106-221), through the shared mimic_sage_cooling_energy and
 * mimic_sage_cooling_rate (shared/sage_cooling_physics.h), each one C expression with
 * SAGE26's operand order, so clang contracts it as SAGE26's build does.
 *
 * @param galaxy      Galaxy that cooled
 * @param vvir        SAGE26's float Vvir
 * @param dt          Substep interval, SAGE26's deltaT / effective_steps
 * @param coolingGas  The AGN-suppressed cooling mass
 * @param params      Run configuration (Hubble_h, UnitTime_in_s)
 */
static void cooling_recipe_tail(struct GalaxyData *galaxy, const float vvir, const double dt,
                                const double coolingGas, const struct MimicConfig *params) {
  // Accumulated undivided; the per-snapshot normalisation is applied once by
  // sage26_finalize_cooling_heating (R/src/core_build_model.c:590-603)
  mimic_sage_cooling_energy(galaxy, vvir, coolingGas);

  // SAGE parity: CoolingRate is (mass / dt) * 1.0e10 / Hubble_h * SEC_PER_GIGAYEAR /
  // UnitTime_in_s narrowed to float, the true Msun/Gyr rate over little h
  // (R/src/model_cooling_heating.c:310-313).
  galaxy->CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, params);
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_apply_cooling_init(void) {
  /* Dependency check: sage26_calculate_cooling_budget must precede this module
   * in the same substep phase */
  if (!module_precedes_in_substep_phase("sage26_calculate_cooling_budget",
                                        PROCESSING_MODE_BY_GALAXY, "sage26_apply_cooling",
                                        PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_apply_cooling requires sage26_calculate_cooling_budget to "
              "precede it in the same substep phase — CoolingGas will be 0 without it");
    return -1;
  }

  VERBOSE_LOG("SAGE apply cooling module initialized");
  return 0;
}

int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  double dt_obj = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: cooling is applied for every non-merged galaxy, Type 2 orphans
  // included: with CGMrecipeOn 0 SAGE26 calls cool_gas_onto_galaxy for every galaxy
  // with mergeType 0 before the merger pass (R/src/core_build_model.c:473-516). CoolingGas is 0 for
  // a galaxy the budget module found could not cool.
  if (halo->galaxy == NULL) {
    return 0;
  }

  struct GalaxyData *galaxy = halo->galaxy;

  dt_status = mimic_object_substep_dt(halo, ctx, &dt_obj);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    // No positive interval: the budget module did not run cooling either
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid apply-cooling dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
              "status=%s)",
              halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
              mimic_object_time_status_str(dt_status));
    return -1;
  }

  const double coolingGas = galaxy->CoolingGas;
  const int path = galaxy->CoolingReturnPath;

  // SAGE parity: the lambda <= 0 and rho_rcool <= 0 early returns skip the tail,
  // leaving Cooling and CoolingRate as they were; an outer-guard failure reaches
  // it with zero mass and writes CoolingRate 0 (R/src/model_cooling_heating.c:141,
  // :157-169, :301-313). This is the CGMrecipeOn 0 tail; with CGMrecipeOn 1 the budget
  // carries path -1, so it does not run here, and sage26_regime_aware_cooling runs the
  // nested recipes' tails and the regime-aware wrapper's own, which always overwrites
  // CoolingRate (:498-502).
  if (path == SAGE_COOLING_PATH_TAIL || path == SAGE_COOLING_PATH_OUTER_GUARD) {
    cooling_recipe_tail(galaxy, (float)halo->Vvir, dt_obj, coolingGas, ctx->params);
  }

  // SAGE parity: cool_gas_onto_galaxy applies for any coolingGas > 0.0
  // (R/src/model_cooling_heating.c:693); it is the CGMrecipeOn 0 transfer: with
  // CGMrecipeOn 1 SAGE26 does not call it, and its regime-aware wrapper applies the CGM and
  // hot masses itself, without the classic "move all metals on equality" branch (:475-496),
  // in sage26_regime_aware_cooling; the budget then carries zero mass, so nothing moves here
  mimic_sage_cool_gas_onto_galaxy(galaxy, coolingGas);

  return 0;
}

int sage26_apply_cooling_cleanup(void) {
  VERBOSE_LOG("SAGE apply cooling module cleaned up");
  return 0;
}
