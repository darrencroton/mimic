/**
 * @file    sage_apply_star_formation_supernova.c
 * @brief   SAGE SF/SN apply step - commits SF and SN transport fields to galaxy reservoirs
 * (infrastructure)
 *
 * Applies calculated star formation and supernova feedback to galaxy properties.
 * Handles stellar recycling, metal enrichment, gas transfers (cold→hot→ejected),
 * and resets temporary calculation properties (NewStellarMass, SupernovaReheatedMass,
 * SupernovaEjectedMass) to zero after processing.
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
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RECYCLE_FRACTION;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_apply_star_formation_supernova_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                    "recycle fraction");

  /* Dependency checks: this is an infrastructure apply step that must follow
   * its SF/SN prescription modules. */

  const bool sf_present = module_configured_anywhere("sage_calculate_star_formation");
  const bool sn_present = module_configured_anywhere("sage_calculate_supernova_feedback");

  /* WARNING: apply step configured with no prescriptions — all fields will be zero */
  if (!sf_present && !sn_present) {
    WARNING_LOG("sage_apply_star_formation_supernova: neither "
                "sage_calculate_star_formation nor sage_calculate_supernova_feedback is "
                "configured — all SF/SN transport fields will be zero; "
                "likely a configuration mistake");
  }

  /* ERROR: any SF/SN module must precede this apply step in the same phase */
  if (sf_present && !module_precedes_in_substep_phase(
                        "sage_calculate_star_formation", PROCESSING_MODE_BY_GALAXY,
                        "sage_apply_star_formation_supernova", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage_apply_star_formation_supernova requires "
              "sage_calculate_star_formation to precede it in the same substep phase — apply step "
              "would commit stale values from the previous substep");
    return -1;
  }
  if (sn_present && !module_precedes_in_substep_phase(
                        "sage_calculate_supernova_feedback", PROCESSING_MODE_BY_GALAXY,
                        "sage_apply_star_formation_supernova", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage_apply_star_formation_supernova requires "
              "sage_calculate_supernova_feedback to precede it in the same substep phase — apply "
              "step would commit stale values from the previous substep");
    return -1;
  }

  /* WARNING: without the enrichment module, NewStellarMass is never consumed
   * and SAGE's instantaneous-recycling disk yield is silently skipped */
  if (sf_present && !module_configured_anywhere("sage_apply_metal_enrichment")) {
    WARNING_LOG("sage_apply_star_formation_supernova: sage_apply_metal_enrichment is not "
                "configured — the disk-SF metal yield will not be applied "
                "(SAGE parity loss; metals will be under-produced)");
  }

  VERBOSE_LOG("SAGE SF/SN apply step module initialized");
  VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);

  return 0;
}

int sage_apply_star_formation_supernova_process(struct ModuleContext *ctx, struct Halo *halos,
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
  const double reheated_mass = gal->SupernovaReheatedMass;
  double ejected_mass = gal->SupernovaEjectedMass;

  // Skip if no star formation occurred (SAGE runs unconditionally, but with
  // stars == 0 every update below is a no-op, so skipping only at exactly 0
  // is equivalent; a larger epsilon here would drop real tiny SF events).
  // NewStellarMass is left for sage_apply_metal_enrichment to consume.
  if (stars <= 0.0) {
    gal->SupernovaReheatedMass = 0.0;
    gal->SupernovaEjectedMass = 0.0;
    return 0;
  }

  // ========================================================================
  // STAR FORMATION: Update gas and stars with recycling
  // ========================================================================

  double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

  // Remove gas consumed by star formation (accounting for recycling)
  gal->ColdGas -= (1.0 - RECYCLE_FRACTION) * stars;
  gal->MetalsColdGas -= metallicity * (1.0 - RECYCLE_FRACTION) * stars;

  // Add to stellar mass (accounting for recycling)
  gal->StellarMass += (1.0 - RECYCLE_FRACTION) * stars;
  gal->MetalsStellarMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

  // Accumulate star formation rate
  if (halo->dT > 0.0) {
    gal->StarFormationRate += stars / halo->dT;
  }

  // ========================================================================
  // SUPERNOVA FEEDBACK: Reheating (cold → hot)
  // ========================================================================

  // Recompute metallicity after star formation
  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

  // Remove reheated gas from this galaxy's cold phase
  gal->ColdGas -= reheated_mass;
  gal->MetalsColdGas -= metallicity * reheated_mass;

  // Add reheated gas to central's hot halo (satellites are stripped)
  central_gal->HotGas += reheated_mass;
  central_gal->MetalsHotGas += metallicity * reheated_mass;

  // ========================================================================
  // SUPERNOVA FEEDBACK: Ejection (hot → ejected)
  // ========================================================================

  // Limit ejected mass to available hot gas in central
  if (ejected_mass > central_gal->HotGas) {
    ejected_mass = central_gal->HotGas;
  }

  // Calculate central's hot gas metallicity
  const double metallicity_hot =
      mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);

  // Remove ejected gas from central's hot phase
  central_gal->HotGas -= ejected_mass;
  central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;

  // Add ejected gas to central's ejected reservoir
  central_gal->EjectedGas += ejected_mass;
  central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

  // Accumulate outflow rate
  if (halo->dT > 0.0) {
    gal->SupernovaOutflowRate += reheated_mass / halo->dT;
  }

  // ========================================================================
  // CLEANUP: Zero out consumed SN transport properties
  // ========================================================================
  // NewStellarMass is intentionally NOT zeroed here: sage_apply_metal_enrichment
  // consumes it after the disk-instability chain (SAGE ordering parity — the
  // disk-SF metal yield is added after check_disk_instability in SAGE).

  gal->SupernovaReheatedMass = 0.0;
  gal->SupernovaEjectedMass = 0.0;

  DEBUG_LOG("Type=%d: Applied SF=%.3e, Reheat=%.3e, Eject=%.3e → ColdGas=%.3e, StellarMass=%.3e",
            halo->Type, stars, reheated_mass, ejected_mass, gal->ColdGas, gal->StellarMass);

  return 0;
}

int sage_apply_star_formation_supernova_cleanup(void) {
  VERBOSE_LOG("SAGE apply star formation supernova module cleaned up");
  return 0;
}
