/**
 * @file    sage26_disk_instability.c
 * @brief   SAGE26 disk instability: stellar transfer to the bulge and the unstable-gas trigger
 *
 * Ports SAGE26's check_disk_instability (R/src/model_disk_instability.c:47-108) up to
 * its black-hole and burst calls, for R0, through the shared helper in
 * shared/sage_disk_instability_physics.h:
 *
 *     diskmass       = ColdGas + (StellarMass - BulgeMass)              (float sums, :54)
 *     Mcrit          = Vmax * Vmax * (3.0 * DiskScaleRadius) / G,       (:57)
 *                      capped at diskmass                                (:58-60)
 *     unstable_stars = (1 - ColdGas / diskmass) * (diskmass - Mcrit)     (:63-66)
 *     BulgeMass            += unstable_stars                             (:77)
 *     InstabilityBulgeMass += unstable_stars                             (:78)
 *     MetalsBulgeMass      += Z_disk * unstable_stars                    (:79)
 *
 * with no cap on the transfer and no clamp of the bulge to the stellar totals. On R0
 * (BulgeSizeOn 0) the bulge radii are left as they were (R/src/model_misc.c:395).
 *
 * Trigger: where unstable_gas > 0 and ColdGas > 0 (:90), UnstableDiskGasFraction is set to
 * unstable_gas / ColdGas (:101), the fraction SAGE26 hands to grow_black_hole and
 * collisional_starburst_recipe (:102-108). In the package those are
 * sage26_quasar_mode and sage26_starburst_feedback, which run after this module.
 *
 * References:
 *   - SAGE26 R/src/model_disk_instability.c:35, :47-108; R/src/model_misc.c:385-395
 *   - Mo, Mao & White (1998): disk stability criterion
 */

#include "error.h"
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

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_disk_instability_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");

  VERBOSE_LOG("SAGE disk instability module initialized");
  VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
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

  gal->UnstableDiskGasFraction =
      mimic_sage_apply_disk_instability(halo, ctx, STAR_FORMING_DISK_FACTOR);

  return 0;
}

int sage26_disk_instability_cleanup(void) {
  VERBOSE_LOG("SAGE disk instability module cleaned up");
  return 0;
}
