/**
 * @file    sage26_apply_star_formation_supernova.c
 * @brief   SAGE SF/SN apply step - commits SF and SN transport fields to galaxy reservoirs
 * (infrastructure)
 *
 * Samples the disk star-formation history, then commits the substep's disk star formation
 * (NewStellarMass, with recycling) and supernova feedback (reheated and ejected mass, to the FoF
 * central's CGM pair with CGMrecipeOn 1 and the central's Regime 0, its hot pair otherwise).
 * Implements the commit stage of SAGE26's starformation_and_feedback, through the shared
 * update_from_star_formation and update_from_feedback (shared/sage_sf_feedback_physics.h).
 *
 * With SFprescription 1 (BR06) each commit caps H2gas and H1gas at the hydrogen of the remaining
 * cold gas. NewStellarMass is left for sage26_apply_metal_enrichment, which applies the disk yield
 * after the disk-instability chain. A row whose FFBRegime is 1 is left to
 * sage26_ffb_star_formation: this step writes nothing on it.
 *
 * With no FoF central galaxy the history is sampled but nothing committed; on the
 * initial-snapshot boundary (no positive interval) nothing is sampled.
 *
 * Reference: Croton et al. (2006, 2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
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

/* SAGE26's SFprescription: 0 (Kauffmann) or 1 (BR06), which the commits test for their hydrogen
 * caps */
static int SF_PRESCRIPTION;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and checked at
 * init, which with the FoF central's Regime selects the feedback's pair */
static int CGM_RECIPE_ON;

/* True when sage26_calculate_supernova_feedback is configured; it then owns the cold-gas cap */
static bool SUPERNOVA_CALCULATOR_PRESENT;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Sample the disk star-formation history of this substep
 *
 * SAGE parity: sampled on every ordinary (non-FFB) call, zero stars included, after the
 * supernova renormalisation of stars and before any reservoir update.
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
  SUPERNOVA_CALCULATOR_PRESENT = sn_present;

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

  /* SAGE parity: SAGE26 hands an FFBRegime 1 row to starformation_ffb before its ordinary
   * history sample and commits, so this module writes nothing on that row;
   * sage26_ffb_star_formation samples and commits it. FFBRegime is 1 only with
   * EnhancedStarFormationOn 1. */
  if (halo->galaxy->FFBRegime == 1) {
    return 0;
  }

  // Cold-gas cap. With sage26_calculate_supernova_feedback present it renormalises stars and
  // reheated mass together, so nothing changes here. Without it nothing else caps star formation
  // and the feedback commit would ship a negative reheat to the central, so this step applies the
  // same renormalisation with no reheated mass (stars = ColdGas) before the history sample and the
  // commit. A negative ColdGas caps at zero so no negative stars are written back.
  if (!SUPERNOVA_CALCULATOR_PRESENT && halo->galaxy->NewStellarMass > halo->galaxy->ColdGas) {
    halo->galaxy->NewStellarMass = (halo->galaxy->ColdGas > 0.0) ? halo->galaxy->ColdGas : 0.0;
  }

  // ========================================================================
  // DISK SF HISTORY: sampled before any reservoir update
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

  /* SAGE parity: star formation and feedback are committed on every ordinary (non-FFB) call,
   * also when no stars formed; a zero event still runs the clamps and caps, which can move a
   * negative or -0.0 metal reservoir. */

  // ========================================================================
  // STAR FORMATION: Update gas and stars with recycling
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
  // SUPERNOVA FEEDBACK: cold -> central's selected pair -> central ejected
  // ========================================================================

  // Recompute the metallicity of the cold phase after star formation
  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

  /* SAGE parity: the reheated mass is clamped to the cold gas left after star
   * formation before update_from_feedback, which clamps it again */
  if (reheated_mass > gal->ColdGas) {
    reheated_mass = gal->ColdGas;
  }

  /* SAGE parity: the FoF central (SAGE26's centralgal) receives the feedback, and ITS Regime
   * selects the pair with CGMrecipeOn 1 */
  mimic_sage_update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity,
                                  SF_PRESCRIPTION, CGM_RECIPE_ON, NULL);

  // ========================================================================
  // CLEANUP: Zero out consumed SN transport properties
  // ========================================================================
  // NewStellarMass is intentionally NOT zeroed here: sage26_apply_metal_enrichment
  // consumes it after the disk-instability chain (SAGE26 adds the disk yield after
  // check_disk_instability). On an FFB row no module writes or consumes NewStellarMass.

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
