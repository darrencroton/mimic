/**
 * @file    sage26_apply_metal_enrichment.c
 * @brief   SAGE26 disk-SF metal yield - instantaneous recycling enrichment after instability chain
 *
 * Applies the instantaneous-recycling metal yield for stars formed in the disk
 * this substep (NewStellarMass), then consumes (zeroes) NewStellarMass, on every galaxy that
 * takes SAGE26's ordinary chain; a row whose FFBRegime is 1 returns at once, writing nothing
 * (:1004-1008), and sage26_ffb_star_formation applies its yield. Ports the
 * end of SAGE26's starformation_and_feedback()
 * (R/src/model_starformation_and_feedback.c:1092-1109, R = the staged SAGE26
 * source):
 *
 *   if ColdGas > 1.0e-8:
 *     FracZleaveDiskVal = FracZleaveDisk * exp(-1.0 * Mvir_central / 30.0)
 *     MetalsColdGas            += Yield * (1.0 - FracZleaveDiskVal) * stars
 *     selected metals (central) += Yield * FracZleaveDiskVal * stars
 *   else:
 *     selected metals (central) += Yield * stars
 *
 * The arithmetic is the package's one disk yield, shared/sage_metal_yield_physics.h, which
 * sage26_ffb_star_formation also calls. The escaping metals go through SAGE26's
 * add_metals_to_hot_reservoir
 * (R/src/model_misc.h:67-74, shared/sage_cgm_routing.h) to the FoF central's selected metals:
 * MetalsCGMgas with the required CGMrecipeOn 1 and the CENTRAL's Regime 0,
 * MetalsHotGas otherwise (:1102, :1108).
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
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"
#include "shared/sage_metal_yield_physics.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double YIELD;
static double FRAC_Z_LEAVE_DISK;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and checked at
 * init, which with the FoF central's Regime selects where the escaping metals go
 * (R/src/model_misc.h:69) */
static int CGM_RECIPE_ON;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_apply_metal_enrichment_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                    "metal outflow fraction");
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on("sage26_apply_metal_enrichment", CGM_RECIPE_ON) != 0) {
    return -1;
  }

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
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);

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

  /* SAGE parity: SAGE26's starformation_and_feedback hands an FFBRegime 1 row to
   * starformation_ffb and returns before its ordinary yield
   * (R/src/model_starformation_and_feedback.c:1004-1008), so this module returns at once and
   * writes nothing: no yield, and NewStellarMass, which no module writes on that row, is not
   * consumed. sage26_ffb_star_formation applies that row's yield from its renormalised stars
   * (:1353-1362). FFBRegime is 1 only where sage26_determine_ffb_regime wrote it, which it does
   * only with EnhancedStarFormationOn 1, so this is SAGE26's test EnhancedStarFormationOn >= 1 &&
   * FFBRegime == 1 (:1004). */
  if (halo->galaxy->FFBRegime == 1) {
    return 0;
  }

  if (ctx->central_galaxy == NULL || ctx->central_galaxy->galaxy == NULL) {
    DEBUG_LOG("No FOF central available for metal ejection destination");
    return 0;
  }

  struct GalaxyData *gal = halo->galaxy;
  struct GalaxyData *central_gal = ctx->central_galaxy->galaxy;

  // Consume the disk star formation transport field for this substep (the ordinary chain's;
  // an FFB row returned above and its yield is sage26_ffb_star_formation's)
  const double stars = gal->NewStellarMass;
  gal->NewStellarMass = 0.0;

  /* SAGE parity: SAGE26 applies the yield on every ordinary (non-FFB) call, also when no stars
   * formed (:1092-1109); adding a +0.0 yield turns a -0.0 metal reservoir into +0.0. The FFB
   * transaction applies the same yield (shared/sage_metal_yield_physics.h) from its renormalised
   * stars, after its own feedback and with no instability chain before it (:1353-1362). */

  // Formation of new metals - instantaneous recycling approximation - only SNII (:1092-1109),
  // the package's one implementation of SAGE26's disk yield (shared/sage_metal_yield_physics.h):
  // the retained metals into the cold gas above the 1e-8 floor, the escaping metals to the FoF
  // CENTRAL's selected metals by its Regime. SAGE parity: the Krumholz & Dekel (2011) scale reads
  // the FoF central's float Mvir (:1094).
  mimic_sage_apply_disk_yield(gal, central_gal, (float)ctx->central_galaxy->Mvir, stars, YIELD,
                              FRAC_Z_LEAVE_DISK, CGM_RECIPE_ON, NULL);

  DEBUG_LOG("Type=%d: Applied metal yield for stars=%.3e (ColdGas=%.3e)", halo->Type, stars,
            gal->ColdGas);

  return 0;
}

int sage26_apply_metal_enrichment_cleanup(void) {
  VERBOSE_LOG("SAGE metal enrichment module cleaned up");
  return 0;
}
