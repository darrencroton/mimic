/**
 * @file    sage26_determine_ffb_regime.c
 * @brief   SAGE26 feedback-free-burst classifier - each unmerged row's FFBRegime from the
 *          Li et al. (2024) FFB fraction of its Mvir at the current redshift, realised against
 *          SAGE26's own SplitMix64 draw of its MostBoundID and the current snapshot
 *
 * Sets each unmerged row's FFBRegime to 1 when SAGE26's SplitMix64 draw of (MostBoundID, current
 * snapshot) falls below the Li et al. (2024) FFB fraction of its float Mvir at the current
 * redshift, else 0. Implements SAGE26's determine_and_store_ffb_regime; runs once per FoF in
 * pre_timestep, after sage26_determine_regime and sage26_prepare_infall_budget, and only with
 * EnhancedStarFormationOn 1 (with 0, FFBRegime keeps its creation value 0).
 *
 * The arithmetic is shared/sage_ffb_regime.h's. On a Type 2 row the core's MostBoundID can differ
 * from SAGE26's, but Mvir is 0 there, so the row is 0 whatever its draw. A NaN fraction compares
 * false, giving 0.
 *
 * Reference: Li et al. (2024)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "module_system/parameter_helpers.h"
#include "shared/sage_ffb_regime.h"
#include "types.h"

#include "sage26_determine_ffb_regime.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_determine_ffb_regime"
#define BUDGET_NAME "sage26_prepare_infall_budget"
#define CGM_CLASSIFIER_NAME "sage26_determine_regime"
#define TRANSACTION_NAME "sage26_ffb_star_formation"

/* SAGE26's EnhancedStarFormationOn (0 off, 1 the Li et al. 2024 classifier), required and checked
 * at init: a missing key fails there */
static int ENHANCED_STAR_FORMATION_ON = MIMIC_SAGE_FFB_OFF;

/* The test seam (sage26_determine_ffb_regime.h); inactive in a run */
static struct Sage26FfbSeam test_seam;
static int test_seam_active = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void sage26_determine_ffb_regime_set_test_seam(const struct Sage26FfbSeam *seam) {
  if (seam == NULL || (seam->deviate == NULL && seam->observe == NULL)) {
    test_seam_active = 0;
    return;
  }
  test_seam = *seam;
  test_seam_active = 1;
}

/**
 * @brief Refuse a placement other than once in pre_timestep as process_full_halo, after the
 *        CGM classifier and the infall budget where those are configured
 *
 * SAGE26 classifies once per FoF, after determine_and_store_regime and infall_recipe and before
 * the substeps. The package keeps that order: an entry in a
 * substep phase or post_timestep, a second pre_timestep entry, another processing mode, or a
 * pre_timestep entry before sage26_determine_regime or sage26_prepare_infall_budget is refused,
 * naming the module it must follow. The check names modules and their order, never a substep
 * phase's name (phase names are run configuration).
 *
 * @return 0 when placed correctly; -1 otherwise
 */
static int check_placement(void) {
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_count_phase_entries(MODULE_NAME, phase->modules, phase->num_modules,
                                   PROCESSING_MODE_FULL_HALO, NULL) > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; it must run only in "
                "modules.pre_timestep, once per snapshot: SAGE26 classifies once per FoF before "
                "the substeps",
                MODULE_NAME, phase->name != NULL ? phase->name : "(unnamed)");
      return -1;
    }
  }
  if (module_count_phase_entries(MODULE_NAME, MimicConfig.post_timestep,
                                 MimicConfig.num_post_timestep, PROCESSING_MODE_FULL_HALO,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.post_timestep; it must run only in "
              "modules.pre_timestep, once per snapshot: SAGE26 classifies once per FoF before the "
              "substeps",
              MODULE_NAME);
    return -1;
  }
  int full_halo_entries = 0;
  const int entries = module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                                 MimicConfig.num_pre_timestep,
                                                 PROCESSING_MODE_FULL_HALO, &full_halo_entries);
  if (entries != 1 || full_halo_entries != 1) {
    ERROR_LOG("%s must be configured exactly once in modules.pre_timestep as process_full_halo "
              "(found %d entries, %d as process_full_halo)",
              MODULE_NAME, entries, full_halo_entries);
    return -1;
  }
  const char *const predecessors[2] = {CGM_CLASSIFIER_NAME, BUDGET_NAME};
  for (int k = 0; k < 2; k++) {
    if (module_count_phase_entries(predecessors[k], MimicConfig.pre_timestep,
                                   MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO,
                                   NULL) > 0 &&
        !module_precedes_in_phase(predecessors[k], MODULE_NAME, MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep)) {
      ERROR_LOG("%s must run after %s in modules.pre_timestep: SAGE26 classifies a FoF's FFB "
                "regime after determine_and_store_regime "
                "and infall_recipe",
                MODULE_NAME, predecessors[k]);
      return -1;
    }
  }
  return 0;
}

/**
 * @brief SAGE26's determine_and_store_ffb_regime for one unmerged row at
 *        EnhancedStarFormationOn 1
 *
 * @param mvir      The row's core Mvir, narrowed here to the float SAGE26 holds
 * @param hubble_h  The run's Hubble_h
 * @param t         Receives the row's values; t->row, t->most_bound_id, t->snapshot and
 *                  t->redshift are set by the caller
 * @return FFBRegime, 1 (FFB) or 0 (normal)
 */
static int classify_row(const double mvir, const double hubble_h, struct Sage26FfbRowTrace *t) {
  // SAGE26 also calls rand() here and discards the value, to keep its libc stream in step; the
  // package has no rand() stream and makes no call.

  // SAGE26's own draw, keyed on the halo's MostBoundID and the current snapshot
  t->draw_bits = mimic_sage_ffb_halo_uniform_bits(t->most_bound_id, t->snapshot);
  const double draw = mimic_sage_ffb_uniform_from_bits(t->draw_bits);
  t->draw = draw;

  // Mvir is read as the float SAGE26 holds, widened
  const double Mvir = (float)mvir;
  t->mvir = Mvir;

  // Li et al. (2024) eq. 3 at the current redshift
  const double f_ffb = mimic_sage_ffb_fraction(Mvir, t->redshift, hubble_h, &t->fraction);
  t->f_ffb = f_ffb;

  // The deviate compared: SAGE26's draw, or a test seam's
  const double random_uniform =
      (test_seam_active && test_seam.deviate != NULL) ? test_seam.deviate(test_seam.user, t) : draw;
  t->deviate = random_uniform;

  // SAGE parity: the strict comparison, so a deviate equal to f_ffb is not FFB
  const int ffb_regime = (random_uniform < f_ffb) ? 1 : 0;
  t->ffb_regime = ffb_regime;
  return ffb_regime;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_determine_ffb_regime_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("EnhancedStarFormationOn", ENHANCED_STAR_FORMATION_ON);
  if (mimic_sage_validate_enhanced_star_formation_on(MODULE_NAME, ENHANCED_STAR_FORMATION_ON) !=
      0) {
    return -1;
  }
  if (check_placement() != 0) {
    return -1;
  }
  /* At EnhancedStarFormationOn 1 the classifier's FFBRegime 1 rows bypass the ordinary
   * star-formation chain, so without the FFB
   * transaction every FFB row would form no stars at all: refused, naming the missing module (the
   * transaction checks its own placement). At 0 nothing is classified and no check applies. */
  if (ENHANCED_STAR_FORMATION_ON != MIMIC_SAGE_FFB_OFF &&
      !module_in_substep_phase(TRANSACTION_NAME, PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("%s requires %s in a substep phase (process_by_galaxy) with "
              "EnhancedStarFormationOn 1: an FFBRegime 1 row skips the ordinary star-formation "
              "chain, so without the FFB transaction "
              "it would form no stars",
              MODULE_NAME, TRANSACTION_NAME);
    return -1;
  }

  VERBOSE_LOG("SAGE determine FFB regime module initialized");
  VERBOSE_LOG("  EnhancedStarFormationOn = %d%s", ENHANCED_STAR_FORMATION_ON,
              ENHANCED_STAR_FORMATION_ON == MIMIC_SAGE_FFB_OFF
                  ? " (no classification: FFBRegime keeps its creation value 0)"
                  : " (the Li et al. 2024 classifier)");
  return 0;
}

int sage26_determine_ffb_regime_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // SAGE parity: the classifier runs only with
  // EnhancedStarFormationOn >= 1; at 0 nothing is written
  if (ENHANCED_STAR_FORMATION_ON == MIMIC_SAGE_FFB_OFF) {
    return 0;
  }
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int p = 0; p < ngal; p++) {
    // SAGE parity: SAGE26 skips only mergeType > 0 rows, which the package marks Type 3
    // (removed rows, never output); no other Type and no CGM regime is excluded
    if (halos[p].galaxy == NULL || halos[p].Type == 3) {
      continue;
    }

    struct Sage26FfbRowTrace trace;
    trace.row = p;
    trace.most_bound_id = halos[p].MostBoundID;
    trace.snapshot = ctx->snapshot_number;
    trace.redshift = ctx->redshift;
    halos[p].galaxy->FFBRegime = classify_row(halos[p].Mvir, ctx->params->Hubble_h, &trace);

    if (test_seam_active && test_seam.observe != NULL) {
      test_seam.observe(test_seam.user, &trace);
    }
  }

  return 0;
}

int sage26_determine_ffb_regime_cleanup(void) {
  VERBOSE_LOG("SAGE determine FFB regime module cleaned up");
  return 0;
}
