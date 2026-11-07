/**
 * @file    sage26_apply_star_formation_supernova.c
 * @brief   SAGE SF/SN apply step - commits SF and SN transport fields to galaxy reservoirs
 * (infrastructure)
 *
 * Samples the disk star-formation history and commits the disk star formation and
 * supernova feedback of one substep as SAGE26's starformation_and_feedback() does
 * (R/src/model_starformation_and_feedback.c:1058-1085, R = the staged SAGE26 source),
 * through the package's shared history helper (shared/sage_sfr_history.h) and its
 * shared implementation of SAGE26's update_from_star_formation and
 * update_from_feedback (shared/sage_sf_feedback_physics.h), which the starburst
 * channels also use:
 *
 *   SfrDisk[bin] += stars / dt; SfrDiskColdGas[bin] = ColdGas;
 *   SfrDiskColdGasMetals[bin] = MetalsColdGas                 (:1058-1060)
 *   metallicity = get_metallicity(ColdGas, MetalsColdGas)
 *   update_from_star_formation(stars, metallicity)            (locks (1 - R) * stars)
 *   metallicity = get_metallicity(ColdGas, MetalsColdGas)
 *   reheated mass clamped to the remaining ColdGas             (:1079-1082)
 *   update_from_feedback(reheated, ejected, metallicity)       (OutflowRate += reheated)
 *
 * With SFprescription 1 (BR06) each commit then caps H2gas and H1gas at the hydrogen of the
 * remaining cold gas (:1128-1135, :1169-1173); with 0 (R0, R1) neither does.
 *
 * The required parameter CGMrecipeOn (0 or 1) selects the feedback's destination pair: the FoF
 * central's CGM pair with CGMrecipeOn 1 and the CENTRAL's Regime 0, its hot pair otherwise,
 * whatever the forming galaxy's own Regime (:1175-1247). The reheated gas is credited to that pair
 * and the ejection capped against it after the reheating.
 *
 * The sample is taken at SAGE26's point: stars is the renormalised mass
 * sage26_calculate_supernova_feedback leaves in NewStellarMass, and the cold gas is
 * the reservoir before anything is committed. bin is the substep's history bin,
 * (substep * STEPS) / num_substeps capped at STEPS - 1 (R/src/core_build_model.c:514-515),
 * and dt the substep interval dT / num_substeps.
 *
 * SAGE26 samples and makes both calls on every substep of a galaxy that takes its ordinary
 * chain, also when no stars formed, so this step always does on such a galaxy, except on
 * Mimic-only degenerate paths: with no FoF
 * central galaxy it samples but returns before committing anything (SAGE26 always
 * has a central), and on the initial-snapshot boundary (no positive interval, where
 * sage26_calculate_star_formation forms nothing) it does not sample, as there is
 * no dt (SAGE26 always has a positive interval). It then zeroes the reheated and
 * ejected transport fields;
 * NewStellarMass is left for sage26_apply_metal_enrichment, which applies the disk
 * yield after the disk-instability chain.
 *
 * None of this describes a feedback-free-burst row: SAGE26 hands a row whose FFBRegime is 1
 * (with EnhancedStarFormationOn 1) to starformation_ffb and returns before the ordinary chain
 * (:1004-1008), so this step returns at once on such a row and writes nothing, its history sample
 * included. sage26_ffb_star_formation samples and commits that row in its own order (the commit
 * before the feedback budget, the feedback with the pre-commit metallicity), and no transport
 * field carries its stars.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_h2_physics.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RECYCLE_FRACTION;

/* SAGE26's SFprescription: 0 (R0, R1) or 1 (BR06), which the commits test for their hydrogen
 * caps (R/src/model_starformation_and_feedback.c:1131, :1169) */
static int SF_PRESCRIPTION;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and checked at
 * init, which with the FoF central's Regime selects the feedback's pair
 * (R/src/model_starformation_and_feedback.c:1175) */
static int CGM_RECIPE_ON;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Sample the disk star-formation history of this substep (S:1058-1060)
 *
 * SAGE parity: SAGE26 samples on every ordinary (non-FFB) starformation_and_feedback call, zero
 * stars included, after compute_sn_feedback renormalised stars and before any reservoir update
 * (R/src/model_starformation_and_feedback.c:1053-1060).
 *
 * @return 0, or -1 on an invalid interval or substep
 */
static int sample_disk_history(const struct ModuleContext *ctx, const struct Halo *halo,
                               struct GalaxyData *gal) {
  double dt = 0.0;
  const enum MimicObjectTimeStatus dt_status = mimic_object_substep_dt(halo, ctx, &dt);
  if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
    return 0;
  }
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    ERROR_LOG("Invalid disk SF history dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
              "status=%s)",
              halo->HaloNr, halo->SnapNum, halo->dT, (ctx != NULL) ? ctx->num_substeps : -1,
              mimic_object_time_status_str(dt_status));
    return -1;
  }
  const int bin = mimic_sage_sfr_step_bin(ctx->substep_number, ctx->num_substeps);
  if (mimic_sage_sample_disk_sfr(gal, bin, gal->NewStellarMass, dt) != 0) {
    ERROR_LOG("Invalid disk SF history bin for halo %lld (substep %d of %d)", halo->HaloNr,
              ctx->substep_number, ctx->num_substeps);
    return -1;
  }
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_apply_star_formation_supernova_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                    "recycle fraction");
  LOAD_PARAM_INT("SFprescription", SF_PRESCRIPTION);
  if (mimic_sage_validate_sf_prescription("sage26_apply_star_formation_supernova",
                                          SF_PRESCRIPTION) != 0) {
    return -1;
  }
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on("sage26_apply_star_formation_supernova", CGM_RECIPE_ON) !=
      0) {
    return -1;
  }

  /* Dependency checks: this is an infrastructure apply step that must follow
   * its SF/SN prescription modules. */

  const bool sf_present = module_configured_anywhere("sage26_calculate_star_formation");
  const bool sn_present = module_configured_anywhere("sage26_calculate_supernova_feedback");

  /* WARNING: apply step configured with no prescriptions — all fields will be zero */
  if (!sf_present && !sn_present) {
    WARNING_LOG("sage26_apply_star_formation_supernova: neither "
                "sage26_calculate_star_formation nor sage26_calculate_supernova_feedback is "
                "configured — all SF/SN transport fields will be zero; "
                "likely a configuration mistake");
  }

  /* ERROR: any SF/SN module must precede this apply step in the same phase */
  if (sf_present && !module_precedes_in_substep_phase(
                        "sage26_calculate_star_formation", PROCESSING_MODE_BY_GALAXY,
                        "sage26_apply_star_formation_supernova", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG(
        "sage26_apply_star_formation_supernova requires "
        "sage26_calculate_star_formation to precede it in the same substep phase — apply step "
        "would commit stale values from the previous substep");
    return -1;
  }
  if (sn_present && !module_precedes_in_substep_phase(
                        "sage26_calculate_supernova_feedback", PROCESSING_MODE_BY_GALAXY,
                        "sage26_apply_star_formation_supernova", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_apply_star_formation_supernova requires "
              "sage26_calculate_supernova_feedback to precede it in the same substep phase — apply "
              "step would commit stale values from the previous substep");
    return -1;
  }

  /* WARNING: without the enrichment module, NewStellarMass is never consumed
   * and SAGE's instantaneous-recycling disk yield is silently skipped */
  if (sf_present && !module_configured_anywhere("sage26_apply_metal_enrichment")) {
    WARNING_LOG("sage26_apply_star_formation_supernova: sage26_apply_metal_enrichment is not "
                "configured — the disk-SF metal yield will not be applied "
                "(SAGE parity loss; metals will be under-produced)");
  }

  VERBOSE_LOG("SAGE SF/SN apply step module initialized");
  VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);
  VERBOSE_LOG("  SFprescription = %d", SF_PRESCRIPTION);
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);

  return 0;
}

int sage26_apply_star_formation_supernova_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal) {
  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  if (halo->galaxy == NULL) {
    return 0;
  }

  /* SAGE parity: SAGE26's starformation_and_feedback hands an FFBRegime 1 row to
   * starformation_ffb and returns before its ordinary history sample and commits
   * (R/src/model_starformation_and_feedback.c:1004-1008), so this module returns at once and
   * writes nothing, before its history sample and time guard. sage26_ffb_star_formation samples
   * and commits that row itself. FFBRegime is 1 only where sage26_determine_ffb_regime wrote it,
   * which it does only with EnhancedStarFormationOn 1, so this is SAGE26's test
   * EnhancedStarFormationOn >= 1 && FFBRegime == 1 (:1004). */
  if (halo->galaxy->FFBRegime == 1) {
    return 0;
  }

  // ========================================================================
  // DISK SF HISTORY: sampled before any reservoir update (:1058-1060)
  // ========================================================================

  if (sample_disk_history(ctx, halo, halo->galaxy) != 0) {
    return -1;
  }

  if (ctx->central_galaxy == NULL || ctx->central_galaxy->galaxy == NULL) {
    DEBUG_LOG("No FOF central available for feedback destination");
    return 0;
  }

  struct GalaxyData *gal = halo->galaxy;
  struct GalaxyData *central_gal = ctx->central_galaxy->galaxy;

  // Read calculated values from previous modules
  const double stars = gal->NewStellarMass;
  double reheated_mass = gal->SupernovaReheatedMass;
  const double ejected_mass = gal->SupernovaEjectedMass;

  /* SAGE parity: SAGE26 commits star formation and feedback on every ordinary (non-FFB) call,
   * also when no stars formed (:1063-1085); a zero event still runs the clamps and caps, which
   * can move a negative or -0.0 metal reservoir. An FFB row returned above (:1004-1008). */

  // ========================================================================
  // STAR FORMATION: Update gas and stars with recycling (:1063-1064)
  // ========================================================================

  double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  mimic_sage_update_from_star_formation(gal, stars, metallicity, RECYCLE_FRACTION, SF_PRESCRIPTION,
                                        NULL);

  // Accumulate the package's snapshot-averaged StarFormationRate diagnostic (not a
  // SAGE26 output; SAGE26's SFR outputs come from its per-substep histories)
  if (halo->dT > 0.0) {
    gal->StarFormationRate += stars / halo->dT;
  }

  // ========================================================================
  // SUPERNOVA FEEDBACK: cold -> central's selected pair -> central ejected (:1075-1085)
  // ========================================================================

  // Recompute the metallicity of the cold phase after star formation
  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

  /* SAGE parity: the reheated mass is clamped to the cold gas left after star
   * formation before update_from_feedback, which clamps it again (:1079-1082) */
  if (reheated_mass > gal->ColdGas) {
    reheated_mass = gal->ColdGas;
  }

  /* SAGE parity: the FoF central (SAGE26's centralgal) receives the feedback, and ITS Regime
   * selects the pair with CGMrecipeOn 1 (:1085, :1175-1176) */
  mimic_sage_update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity,
                                  SF_PRESCRIPTION, CGM_RECIPE_ON, NULL);

  // ========================================================================
  // CLEANUP: Zero out consumed SN transport properties
  // ========================================================================
  // NewStellarMass is intentionally NOT zeroed here: sage26_apply_metal_enrichment
  // consumes it after the disk-instability chain (SAGE26 adds the disk yield after
  // check_disk_instability, :1092-1109). This lifetime is the ordinary chain's only: on an
  // FFB row no module writes or consumes NewStellarMass, which keeps its per-snapshot reset
  // value, and sage26_ffb_star_formation carries its stars in locals of its one call.

  gal->SupernovaReheatedMass = 0.0;
  gal->SupernovaEjectedMass = 0.0;

  DEBUG_LOG("Type=%d: Applied SF=%.3e, Reheat=%.3e, Eject=%.3e → ColdGas=%.3e, StellarMass=%.3e",
            halo->Type, stars, reheated_mass, ejected_mass, gal->ColdGas, gal->StellarMass);

  return 0;
}

int sage26_apply_star_formation_supernova_cleanup(void) {
  VERBOSE_LOG("SAGE apply star formation supernova module cleaned up");
  return 0;
}
