/**
 * @file    sage26_apply_star_formation_supernova.c
 * @brief   SAGE SF/SN apply step - commits SF and SN transport fields to galaxy reservoirs
 * (infrastructure)
 *
 * Commits the disk star formation and supernova feedback of one substep as SAGE26's
 * starformation_and_feedback() does (R/src/model_starformation_and_feedback.c:1063-1085,
 * R = the staged SAGE26 source), through the package's shared implementation of
 * SAGE26's update_from_star_formation and update_from_feedback
 * (shared/sage_sf_feedback_physics.h), which the starburst channels also use:
 *
 *   metallicity = get_metallicity(ColdGas, MetalsColdGas)
 *   update_from_star_formation(stars, metallicity)            (locks (1 - R) * stars)
 *   metallicity = get_metallicity(ColdGas, MetalsColdGas)
 *   reheated mass clamped to the remaining ColdGas             (:1079-1082)
 *   update_from_feedback(reheated, ejected, metallicity)       (OutflowRate += reheated)
 *
 * SAGE26 makes both calls on every substep, also when no stars formed, so this
 * step always runs them, except on the Mimic-only degenerate path with no FoF
 * central galaxy, where it returns before committing anything (SAGE26 always has a
 * central). It then zeroes the reheated and ejected transport fields;
 * NewStellarMass is left for sage26_apply_metal_enrichment, which applies the disk
 * yield after the disk-instability chain.
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
#include "shared/sage_sf_feedback_physics.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RECYCLE_FRACTION;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_apply_star_formation_supernova_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                    "recycle fraction");

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

  /* SAGE parity: SAGE26 commits star formation and feedback on every call, also when
   * no stars formed (:1063-1085); a zero event still runs the clamps and caps, which
   * can move a negative or -0.0 metal reservoir. */

  // ========================================================================
  // STAR FORMATION: Update gas and stars with recycling (:1063-1064)
  // ========================================================================

  double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  mimic_sage_update_from_star_formation(gal, stars, metallicity, RECYCLE_FRACTION);

  // Accumulate the package's snapshot-averaged StarFormationRate diagnostic (not a
  // SAGE26 output; SAGE26's SFR outputs come from its per-substep histories)
  if (halo->dT > 0.0) {
    gal->StarFormationRate += stars / halo->dT;
  }

  // ========================================================================
  // SUPERNOVA FEEDBACK: cold -> central hot -> central ejected (:1075-1085)
  // ========================================================================

  // Recompute the metallicity of the cold phase after star formation
  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

  /* SAGE parity: the reheated mass is clamped to the cold gas left after star
   * formation before update_from_feedback, which clamps it again (:1079-1082) */
  if (reheated_mass > gal->ColdGas) {
    reheated_mass = gal->ColdGas;
  }

  mimic_sage_update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity, NULL);

  // ========================================================================
  // CLEANUP: Zero out consumed SN transport properties
  // ========================================================================
  // NewStellarMass is intentionally NOT zeroed here: sage26_apply_metal_enrichment
  // consumes it after the disk-instability chain (SAGE26 adds the disk yield after
  // check_disk_instability, :1092-1109).

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
