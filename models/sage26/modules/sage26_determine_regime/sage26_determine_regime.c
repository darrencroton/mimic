/**
 * @file    sage26_determine_regime.c
 * @brief   SAGE26 CGM regime classifier - each row's CGM (0) or hot-halo (1) regime from the
 *          Dekel & Birnboim (2006) shock mass, with the package's keyed draw
 *
 * Ports SAGE26's determine_and_store_regime (R = the staged SAGE26 source,
 * R/src/model_regimes.c:63-100), which SAGE26 calls once per FoF group after progenitor joining
 * and before infall_recipe, only when CGMrecipeOn is 1 (R/src/core_build_model.c:407-415). The
 * module runs once per FoF in pre_timestep (process_full_halo), immediately before
 * sage26_prepare_infall_budget. With CGMrecipeOn 0 it writes nothing, as SAGE26 skips the call,
 * so Regime keeps its creation value -1. With CGMrecipeOn 1 every row SAGE26 does not skip is
 * classified:
 *
 *   Mshock       = MShockMsun * Hubble_h / 1.0e10                     (double, R/src/macros.h:47)
 *   mass_ratio   = (double)(float)Mvir / Mshock                        (double, :74)
 *   mass_ratio <= 0.0:  Regime = 0, no draw                            (:77-78)
 *   otherwise:   x = log10(mass_ratio) / 0.1                           (double, :82-85)
 *                hot_fraction = 1.0 / (1.0 + exp(-x))                  (double, :89)
 *                Regime = (u < hot_fraction) ? 1 : 0                   (:94-95)
 *
 * with the deviate u from the package's keyed draw (below). A NaN mass_ratio is not <= 0.0, so it
 * takes the draw branch, as SAGE26's test implies, and is classified 0 (every ordered comparison
 * with the NaN hot_fraction is false). There is no Type restriction, hysteresis, redshift term or
 * reuse of a previous regime, and classification moves no gas.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand order and types. None
 * of them has a multiply feeding an add or subtract, so none can contract to a fused multiply-add,
 * and SAGE26's object has none in this routine (the oracle README's "The c10 Group"
 * shows the disassembly).
 *
 * Reference: Dekel & Birnboim (2006); Voit (2015)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "module_system/parameter_helpers.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_keyed_draw.h"
#include "types.h"

#include "sage26_determine_regime.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_determine_regime"

/* SAGE26's CGMrecipeOn (0 off, 1 on), required and checked at init */
static int CGM_RECIPE_ON;

/* SAGE26's MShockMsun, the Dekel & Birnboim shock mass in physical Msun (no h), required, positive
 * and finite; SAGE26's default is 6.0e11 (R/src/core_read_parameter_file.c:114) */
static double MSHOCK_MSUN;

/* The test seam (sage26_determine_regime.h); inactive in a run */
static struct Sage26RegimeSeam test_seam;
static int test_seam_active = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void sage26_determine_regime_set_test_seam(const struct Sage26RegimeSeam *seam) {
  if (seam == NULL || seam->deviate == NULL) {
    test_seam_active = 0;
    return;
  }
  test_seam = *seam;
  test_seam_active = 1;
}

/**
 * @brief Refuse a placement other than once in pre_timestep, before the infall budget
 *
 * SAGE26 classifies once per FoF, between progenitor joining and infall_recipe
 * (R/src/core_build_model.c:407-415): the module must have exactly one pre_timestep entry, none
 * in a substep phase or post_timestep, and, when sage26_prepare_infall_budget runs in
 * pre_timestep, precede it, so the budget reads the regime of the current snapshot.
 *
 * @return 0 when placed correctly; -1 otherwise
 */
static int validate_placement(void) {
  const int pre_entries =
      module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO, NULL);
  const int post_entries =
      module_count_phase_entries(MODULE_NAME, MimicConfig.post_timestep,
                                 MimicConfig.num_post_timestep, PROCESSING_MODE_FULL_HALO, NULL);
  if (pre_entries != 1 || post_entries != 0 ||
      module_in_substep_phase(MODULE_NAME, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("%s must be configured exactly once, in pre_timestep (found %d pre_timestep and %d "
              "post_timestep entries%s): SAGE26 classifies once per FoF, after progenitor joining "
              "and before the infall budget",
              MODULE_NAME, pre_entries, post_entries,
              module_in_substep_phase(MODULE_NAME, PROCESSING_MODE_FULL_HALO)
                  ? ", and a substep-phase entry"
                  : "");
    return -1;
  }
  if (module_configured_in_phase("sage26_prepare_infall_budget", MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO) &&
      !module_precedes_in_phase(MODULE_NAME, "sage26_prepare_infall_budget",
                                MimicConfig.pre_timestep, MimicConfig.num_pre_timestep)) {
    ERROR_LOG("%s requires to run before sage26_prepare_infall_budget in pre_timestep — SAGE26 "
              "classifies before infall_recipe (core_build_model.c:410-415)",
              MODULE_NAME);
    return -1;
  }
  return 0;
}

/**
 * @brief SAGE26 determine_and_store_regime's loop body for one row (R/src/model_regimes.c:66-98)
 *
 * @param mvir     The row's core Mvir, narrowed here to the float SAGE26 holds
 * @param mshock   The shock mass in 1e10 Msun/h, formed by the caller as SAGE26 forms it
 * @param t        Receives the row's values; t->row, t->unique_galaxy_id and t->snapshot are
 *                 set by the caller
 * @return The regime, 0 (CGM) or 1 (hot)
 */
static int classify_row(const double mvir, const double mshock, struct Sage26RegimeRowTrace *t) {
  // Mvir is read as the float SAGE26 holds (core_allvars.h:129), widened in the division (:74)
  const double mass_ratio = (float)mvir / mshock;

  t->mshock = mshock;
  t->mass_ratio = mass_ratio;
  t->x = NAN;
  t->hot_fraction = NAN;
  t->deviate = NAN;
  t->drew = 0;

  int new_regime;
  // SAGE parity (R/src/model_regimes.c:77-78): only a ratio <= 0.0 takes the no-draw CGM
  // branch, so a NaN ratio (from a NaN Mvir) takes the draw branch below
  if (mass_ratio <= 0.0) {
    new_regime = 0;
  } else {
    // Sigmoid of width delta_log_M dex about the shock mass (:82-89)
    const double delta_log_M = SAGE26_REGIME_DELTA_LOG_M;
    const double x = log10(mass_ratio) / delta_log_M;
    const double hot_fraction = 1.0 / (1.0 + exp(-x));
    t->x = x;
    t->hot_fraction = hot_fraction;

    // SAGE parity, the one deliberate departure: SAGE26 draws
    // (double)rand() / (double)RAND_MAX from the process-global libc stream
    // (R/src/model_regimes.c:94); the package draws the keyed deviate of the row's
    // UniqueGalaxyID and the current snapshot (shared/sage_keyed_draw.h), compared exactly as
    // SAGE26 compares its draw (:95)
    const double random_uniform =
        test_seam_active ? test_seam.deviate(test_seam.user, t)
                         : mimic_sage_cgm_regime_deviate((int64_t)t->unique_galaxy_id, t->snapshot);
    t->deviate = random_uniform;
    t->drew = 1;
    new_regime = (random_uniform < hot_fraction) ? 1 : 0;
  }

  t->regime = new_regime;
  return new_regime;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_determine_regime_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on(MODULE_NAME, CGM_RECIPE_ON) != 0) {
    return -1;
  }
  LOAD_PARAM_DOUBLE("MShockMsun", MSHOCK_MSUN);
  if (mimic_sage_validate_mshock_msun(MODULE_NAME, MSHOCK_MSUN) != 0) {
    return -1;
  }
  if (validate_placement() != 0) {
    return -1;
  }

  VERBOSE_LOG("SAGE determine regime module initialized");
  VERBOSE_LOG("  CGMrecipeOn = %d%s", CGM_RECIPE_ON,
              CGM_RECIPE_ON == 0 ? " (no classification: Regime keeps its creation value)" : "");
  VERBOSE_LOG("  MShockMsun = %.6e Msun", MSHOCK_MSUN);

  return 0;
}

int sage26_determine_regime_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // SAGE parity (R/src/core_build_model.c:410): the classifier runs only with CGMrecipeOn 1
  if (CGM_RECIPE_ON == 0) {
    return 0;
  }
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // SAGE26's MSUN_TO_CODE_MASS(MShockMsun, Hubble_h), ((x) * (h) / 1.0e10) (R/src/macros.h:47,
  // R/src/model_regimes.c:71), in 1e10 Msun/h; the same value for every row
  const double Mshock = MSHOCK_MSUN * ctx->params->Hubble_h / 1.0e10;

  for (int p = 0; p < ngal; p++) {
    // SAGE parity (R/src/model_regimes.c:67): SAGE26 skips only mergeType > 0 rows, which the
    // package marks Type 3 (removed rows, never output); no other Type is excluded
    if (halos[p].galaxy == NULL || halos[p].Type == 3) {
      continue;
    }

    struct Sage26RegimeRowTrace trace;
    trace.row = p;
    trace.unique_galaxy_id = halos[p].UniqueGalaxyID;
    trace.snapshot = ctx->snapshot_number;
    halos[p].galaxy->Regime = classify_row(halos[p].Mvir, Mshock, &trace);

    if (test_seam_active && test_seam.observe != NULL) {
      test_seam.observe(test_seam.user, &trace);
    }
  }

  return 0;
}

int sage26_determine_regime_cleanup(void) {
  VERBOSE_LOG("SAGE determine regime module cleaned up");
  return 0;
}
