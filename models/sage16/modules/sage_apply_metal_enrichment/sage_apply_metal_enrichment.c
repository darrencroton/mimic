/**
 * @file    sage_apply_metal_enrichment.c
 * @brief   SAGE disk-SF metal yield - instantaneous recycling enrichment after instability chain
 *
 * Applies the instantaneous-recycling metal yield for stars formed in the disk
 * this substep (NewStellarMass), then consumes (zeroes) NewStellarMass.
 *
 * SAGE parity: in SAGE's starformation_and_feedback() the yield is added AFTER
 * check_disk_instability() runs, so the disk-instability starburst sees the cold
 * gas metallicity WITHOUT this substep's disk-SF enrichment. This module must
 * therefore run after sage_disk_instability / sage_quasar_mode /
 * sage_starburst_feedback in the same substep phase (enforced in init).
 * The starburst channel applies its own yield inside the burst kernel, exactly
 * as SAGE's collisional_starburst_recipe() does.
 *
 * Reference: Croton et al. (2006, 2016), SAGE model_starformation_and_feedback.c
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/sage_constants.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double YIELD;
static double FRAC_Z_LEAVE_DISK;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_apply_metal_enrichment_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                    "metal outflow fraction");

  /* ERROR: the SF/SN apply step must precede this module in the same phase —
   * the yield is keyed to the renormalised NewStellarMass it commits. */
  if (module_configured_anywhere("sage_apply_star_formation_supernova") &&
      !module_precedes_in_substep_phase("sage_apply_star_formation_supernova",
                                        PROCESSING_MODE_BY_GALAXY, "sage_apply_metal_enrichment",
                                        PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage_apply_metal_enrichment requires sage_apply_star_formation_supernova to "
              "precede it in the same substep phase — the yield applies to the stellar "
              "mass committed by the apply step");
    return -1;
  }

  /* ERROR: SAGE parity requires the disk-instability chain to run BEFORE the
   * disk yield is added (SAGE adds the yield after check_disk_instability). */
  if (module_in_substep_phase("sage_starburst_feedback", PROCESSING_MODE_BY_GALAXY) &&
      !module_precedes_in_substep_phase("sage_starburst_feedback", PROCESSING_MODE_BY_GALAXY,
                                        "sage_apply_metal_enrichment", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage_apply_metal_enrichment must run after sage_starburst_feedback in the same "
              "substep phase — SAGE adds the disk-SF yield after the disk-instability "
              "starburst, so the burst must see pre-enrichment metallicity");
    return -1;
  }

  INFO_LOG("SAGE metal enrichment module initialized");
  VERBOSE_LOG("  Yield = %.4f", YIELD);
  VERBOSE_LOG("  FracZleaveDisk = %.3f", FRAC_Z_LEAVE_DISK);

  return 0;
}

int sage_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  if (halo->galaxy == NULL) {
    return 0;
  }

  if (ctx->central_galaxy == NULL || ctx->central_galaxy->galaxy == NULL) {
    DEBUG_LOG("No FOF central available for metal ejection destination");
    return 0;
  }

  struct GalaxyData *gal = halo->galaxy;
  struct GalaxyData *central_gal = ctx->central_galaxy->galaxy;

  // Consume the disk star formation transport field for this substep
  const double stars = gal->NewStellarMass;
  gal->NewStellarMass = 0.0;

  if (stars <= 0.0) {
    return 0;
  }

  // Formation of new metals - instantaneous recycling approximation - only SNII
  // SAGE parity: the Krumholz & Dekel (2011) ejection scale uses the FOF
  // central's Mvir
  if (gal->ColdGas > SAGE_COLD_GAS_YIELD_THRESHOLD) {
    const double FracZleaveDiskVal =
        FRAC_Z_LEAVE_DISK * exp(-1.0 * ctx->central_galaxy->Mvir / SAGE_METAL_EJECTION_MVIR_SCALE);
    gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
    central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
  } else {
    central_gal->MetalsHotGas += YIELD * stars;
  }

  DEBUG_LOG("Type=%d: Applied metal yield for stars=%.3e (ColdGas=%.3e)", halo->Type, stars,
            gal->ColdGas);

  return 0;
}

int sage_apply_metal_enrichment_cleanup(void) {
  VERBOSE_LOG("SAGE metal enrichment module cleaned up");
  return 0;
}
