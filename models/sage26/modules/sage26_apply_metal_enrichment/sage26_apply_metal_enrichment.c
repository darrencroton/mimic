/**
 * @file    sage26_apply_metal_enrichment.c
 * @brief   SAGE26 disk-SF metal yield - instantaneous recycling enrichment after instability chain
 *
 * Applies the instantaneous-recycling metal yield for stars formed in the disk this substep
 * (NewStellarMass), then consumes (zeroes) NewStellarMass; a row whose FFBRegime is 1 is left to
 * sage26_ffb_star_formation. Implements the end of SAGE26's starformation_and_feedback:
 *
 *   if ColdGas > 1.0e-8:
 *     FracZleaveDiskVal = FracZleaveDisk * exp(-1.0 * Mvir_central / 30.0)
 *     MetalsColdGas            += Yield * (1.0 - FracZleaveDiskVal) * stars
 *     selected metals (central) += Yield * FracZleaveDiskVal * stars
 *   else:
 *     selected metals (central) += Yield * stars
 *
 * The arithmetic is shared/sage_metal_yield_physics.h. The escaping metals go to the FoF
 * central's MetalsCGMgas with CGMrecipeOn 1 and the central's Regime 0, MetalsHotGas otherwise.
 *
 * SAGE parity: the yield is added after the disk-instability chain, so the instability starburst
 * sees the cold gas metallicity without this substep's disk-SF enrichment; init() enforces that
 * this module runs after sage26_starburst_feedback in the same substep phase.
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
 * init, which with the FoF central's Regime selects where the escaping metals go */
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
   * disk yield is added (SAGE26 adds the yield after check_disk_instability). */
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

  /* SAGE parity: SAGE26 hands an FFBRegime 1 row to starformation_ffb before its ordinary
   * yield, so this module writes nothing on that row (NewStellarMass is not consumed) and
   * sage26_ffb_star_formation applies its yield. FFBRegime is 1 only with
   * EnhancedStarFormationOn 1. */
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

  /* SAGE parity: the yield is applied on every ordinary (non-FFB) call, also when no stars
   * formed; adding a +0.0 yield turns a -0.0 metal reservoir into +0.0. */

  // Formation of new metals - instantaneous recycling approximation - only SNII: the retained
  // metals into the cold gas above the 1e-8 floor, the escaping metals to the FoF central's
  // selected metals by its Regime. SAGE parity: the Krumholz & Dekel (2011) scale reads the FoF
  // central's float Mvir.
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
