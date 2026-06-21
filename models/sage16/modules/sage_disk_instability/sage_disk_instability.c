/**
 * @file    sage_disk_instability.c
 * @brief   SAGE disk instability - stability check and stellar mass transfer to bulge
 *
 * Implements the Mo, Mao & White (1998) disk stability criterion. Unstable stellar
 * mass is transferred to the bulge. Unstable gas triggers downstream physics modules.
 *
 * Physics:
 *   Calculate disk mass and critical mass, then transfer unstable stars to bulge:
 *     diskmass = ColdGas + (StellarMass - BulgeMass)
 *     Mcrit = Vmax^2 * (3 * DiskScaleRadius) / G
 *     unstable_stars = star_fraction * (diskmass - Mcrit)
 *     BulgeMass += unstable_stars (with metallicity preservation)
 *
 * Trigger Flag:
 *   Sets UnstableDiskGasFraction for downstream modules:
 *     - sage_quasar_mode: BH growth + quasar winds from unstable gas
 *     - sage_starburst_feedback: Starburst + SN feedback from unstable gas
 *
 * References:
 *   - SAGE: model_disk_instability.c (lines 20-54)
 *   - Mo, Mao & White (1998) - Disk stability criterion
 */

#include "error.h"
#include "shared/sage_disk_instability_physics.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double STAR_FORMING_DISK_FACTOR;

// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

int sage_disk_instability_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");

  VERBOSE_LOG("SAGE disk instability module initialized");
  VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
  return 0;
}

int sage_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // Verify process_by_galaxy mode
  if (ngal != 1) {
    ERROR_LOG("sage_disk_instability expects ngal=1, got %d", ngal);
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

int sage_disk_instability_cleanup(void) {
  VERBOSE_LOG("SAGE disk instability module cleaned up");
  return 0;
}
