/**
 * @file    sage26_apply_metal_enrichment.c
 * @brief   SAGE26 disk-SF metal yield - instantaneous recycling enrichment after instability chain
 *
 * Applies the instantaneous-recycling metal yield for stars formed in the disk
 * this substep (NewStellarMass), then consumes (zeroes) NewStellarMass. Ports the
 * end of SAGE26's starformation_and_feedback()
 * (R/src/model_starformation_and_feedback.c:1092-1109, R = the staged SAGE26
 * source):
 *
 *   if ColdGas > 1.0e-8:
 *     FracZleaveDiskVal = FracZleaveDisk * exp(-1.0 * Mvir_central / 30.0)
 *     MetalsColdGas            += Yield * (1.0 - FracZleaveDiskVal) * stars
 *     MetalsHotGas (central)   += Yield * FracZleaveDiskVal * stars
 *   else:
 *     MetalsHotGas (central)   += Yield * stars
 *
 * SAGE parity: SAGE26 adds the yield AFTER check_disk_instability() (:1088-1090),
 * so the disk-instability starburst sees the cold gas metallicity WITHOUT this
 * substep's disk-SF enrichment. This module must therefore run after
 * sage26_disk_instability / sage26_quasar_mode / sage26_starburst_feedback in the
 * same substep phase (enforced in init). The starburst channel applies its own
 * yield inside the burst kernel, as SAGE26's collisional_starburst_recipe() does
 * (R/src/model_mergers.c:814-831).
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

int sage26_apply_metal_enrichment_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                    "metal outflow fraction");

  /* ERROR: the SF/SN apply step must precede this module in the same phase —
   * the yield is keyed to the renormalised NewStellarMass it commits. */
  if (module_configured_anywhere("sage26_apply_star_formation_supernova") &&
      !module_precedes_in_substep_phase("sage26_apply_star_formation_supernova",
                                        PROCESSING_MODE_BY_GALAXY, "sage26_apply_metal_enrichment",
                                        PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_apply_metal_enrichment requires sage26_apply_star_formation_supernova to "
              "precede it in the same substep phase — the yield applies to the stellar "
              "mass committed by the apply step");
    return -1;
  }

  /* ERROR: SAGE parity requires the disk-instability chain to run BEFORE the
   * disk yield is added (SAGE26 adds the yield after check_disk_instability,
   * R/src/model_starformation_and_feedback.c:1088-1109). */
  if (module_in_substep_phase("sage26_starburst_feedback", PROCESSING_MODE_BY_GALAXY) &&
      !module_precedes_in_substep_phase("sage26_starburst_feedback", PROCESSING_MODE_BY_GALAXY,
                                        "sage26_apply_metal_enrichment",
                                        PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage26_apply_metal_enrichment must run after sage26_starburst_feedback in the same "
              "substep phase — SAGE adds the disk-SF yield after the disk-instability "
              "starburst, so the burst must see pre-enrichment metallicity");
    return -1;
  }

  VERBOSE_LOG("SAGE metal enrichment module initialized");
  VERBOSE_LOG("  Yield = %.4f", YIELD);
  VERBOSE_LOG("  FracZleaveDisk = %.3f", FRAC_Z_LEAVE_DISK);

  return 0;
}

int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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

  /* SAGE parity: SAGE26 applies the yield on every call, also when no stars formed
   * (:1092-1109); adding a +0.0 yield turns a -0.0 metal reservoir into +0.0. */

  // Formation of new metals - instantaneous recycling approximation - only SNII
  if (gal->ColdGas > SAGE_COLD_GAS_YIELD_THRESHOLD) {
    /* SAGE parity: the Krumholz & Dekel (2011) ejection scale uses the FoF central's
     * float Mvir (:1094) */
    const float central_mvir = (float)ctx->central_galaxy->Mvir;
    const double FracZleaveDiskVal =
        FRAC_Z_LEAVE_DISK * exp(-1.0 * central_mvir / SAGE_METAL_EJECTION_MVIR_SCALE);

    /* SAGE parity: metals that stay in the disk, Yield * (1.0 - FracZleaveDiskVal)
     * left to right, then one fused multiply-add with stars in SAGE26's build (:1097) */
    const double yield_retained = YIELD * (1.0 - FracZleaveDiskVal);
    gal->MetalsColdGas = (float)fma(yield_retained, stars, (double)gal->MetalsColdGas);

    // Metals that leave the disk go to the central's hot gas, formed in its own
    // statement as SAGE26 does (:1100-1102)
    const double metals_leaving_disk = YIELD * FracZleaveDiskVal * stars;
    central_gal->MetalsHotGas += metals_leaving_disk;
  } else {
    // All metals leave the disk when ColdGas is very low (:1105-1108)
    const double all_metals = YIELD * stars;
    central_gal->MetalsHotGas += all_metals;
  }

  DEBUG_LOG("Type=%d: Applied metal yield for stars=%.3e (ColdGas=%.3e)", halo->Type, stars,
            gal->ColdGas);

  return 0;
}

int sage26_apply_metal_enrichment_cleanup(void) {
  VERBOSE_LOG("SAGE metal enrichment module cleaned up");
  return 0;
}
