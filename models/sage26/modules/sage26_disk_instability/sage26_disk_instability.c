/**
 * @file    sage26_disk_instability.c
 * @brief   SAGE26 disk instability: stellar transfer to the bulge and the unstable-gas trigger
 *
 * Ports SAGE26's check_disk_instability (R/src/model_disk_instability.c:47-108) up to
 * its black-hole and burst calls, for BulgeSizeOn 0 (R0) and 3 (R1), through the shared
 * helper in shared/sage_disk_instability_physics.h:
 *
 *     diskmass       = ColdGas + (StellarMass - BulgeMass)              (float sums, :54)
 *     Mcrit          = Vmax * Vmax * (3.0 * DiskScaleRadius) / G,       (:57)
 *                      capped at diskmass                                (:58-60)
 *     unstable_stars = (1 - ColdGas / diskmass) * (diskmass - Mcrit)     (:63-66)
 *     BulgeMass            += unstable_stars                             (:77)
 *     InstabilityBulgeMass += unstable_stars                             (:78)
 *     MetalsBulgeMass      += Z_disk * unstable_stars                    (:79)
 *
 * with no cap on the transfer and no clamp of the bulge to the stellar totals, then the
 * incremental instability-bulge radius from the disk radius saved before the transfer
 * (:68-70, :81-83; update_instability_bulge_radius, R/src/model_misc.c:385-424,
 * shared/sage_bulge_size_physics.h). With BulgeSizeOn 0 that update returns at once and the
 * bulge radii are left as they were (R/src/model_misc.c:395).
 *
 * Trigger: where unstable_gas > 0 and ColdGas > 0 (:90), UnstableDiskGasFraction is set to
 * unstable_gas / ColdGas (:101), the fraction SAGE26 hands to grow_black_hole and
 * collisional_starburst_recipe (:102-108). In the package those are
 * sage26_quasar_mode and sage26_starburst_feedback, which run after this module.
 *
 * A row whose FFBRegime is 1 (written by sage26_determine_ffb_regime only with
 * EnhancedStarFormationOn 1) is not checked: SAGE26's FFB call returns with no instability
 * check (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367), so this module writes
 * nothing on it. The post-merger recheck inside the burst transaction does not test FFBRegime
 * (R/src/model_mergers.c:806-811) and runs on an FFB target too.
 *
 * References:
 *   - SAGE26 R/src/model_disk_instability.c:35, :47-108; R/src/model_misc.c:239-424
 *   - Mo, Mao & White (1998): disk stability criterion
 */

#include "error.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_disk_instability_physics.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/* SAGE parity: SAGE26's TOOMRE_DISK_FACTOR is the double literal 3.0
 * (R/src/model_disk_instability.c:35). The parameter enters Mcrit exactly where that
 * constant does; the run files hold 3.0. */
static double STAR_FORMING_DISK_FACTOR;

/* SAGE26's BulgeSizeOn: 0 (R0) or 3 (Tonini sizes), the mode of the radius update that
 * follows the stellar transfer (R/src/model_misc.c:395) */
static int BULGE_SIZE_ON;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_disk_instability_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");
  LOAD_PARAM_INT("BulgeSizeOn", BULGE_SIZE_ON);
  if (mimic_sage_validate_bulge_size_on("sage26_disk_instability", BULGE_SIZE_ON) != 0) {
    return -1;
  }

  VERBOSE_LOG("SAGE disk instability module initialized");
  VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
  VERBOSE_LOG("  BulgeSizeOn = %d", BULGE_SIZE_ON);
  return 0;
}

int sage26_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ngal != 1) {
    ERROR_LOG("sage26_disk_instability expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];
  struct GalaxyData *gal = halo->galaxy;

  if (gal == NULL) {
    return 0;
  }
  /* SAGE parity: SAGE26's starformation_and_feedback hands an FFBRegime 1 row to
   * starformation_ffb, which returns with no disk-instability check
   * (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367), so this module returns at
   * once and writes nothing: no stellar transfer, no radius update and no UnstableDiskGasFraction,
   * which keeps its per-snapshot reset value 0, so the by-galaxy entries of sage26_quasar_mode and
   * sage26_starburst_feedback do not act on the row. Only this by-galaxy call is skipped: the
   * post-merger recheck inside the burst transaction (R/src/model_mergers.c:806-811,
   * shared/sage_starburst_physics.h) runs on an FFB target too, as SAGE26's merger routines do not
   * read FFBRegime. FFBRegime is 1 only where sage26_determine_ffb_regime wrote it, which it does
   * only with EnhancedStarFormationOn 1, so this is SAGE26's test EnhancedStarFormationOn >= 1 &&
   * FFBRegime == 1 (:1004). */
  if (gal->FFBRegime == 1) {
    return 0;
  }
  /* SAGE26's run_params->BulgeSizeOn and Hubble_h (R/src/model_misc.c:395, :398); the helper
   * returns 0 without a transfer when the context or its configuration is missing */
  const double hubble_h = (ctx != NULL && ctx->params != NULL) ? ctx->params->Hubble_h : 0.0;
  const struct MimicSageBulgeSize sizes = {BULGE_SIZE_ON, hubble_h};
  gal->UnstableDiskGasFraction =
      mimic_sage_apply_disk_instability(halo, ctx, STAR_FORMING_DISK_FACTOR, &sizes);

  return 0;
}

int sage26_disk_instability_cleanup(void) {
  BULGE_SIZE_ON = MIMIC_SAGE_BULGE_SIZE_OFF;
  VERBOSE_LOG("SAGE disk instability module cleaned up");
  return 0;
}
