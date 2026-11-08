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

#include <stdbool.h>
#include <string.h>
#include "error.h"
#include "globals.h"
#include "shared/sage_disk_instability_physics.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double STAR_FORMING_DISK_FACTOR;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief Index of a module's first entry in a given mode within one substep phase, or -1
 */
static int first_entry_in_mode(const char *name, const enum ProcessingMode mode,
                               const struct ModulePhaseConfig *phase) {
  for (int i = 0; i < phase->num_modules; i++) {
    if (phase->modules[i].processing_mode == mode &&
        strcmp(phase->modules[i].module_name, name) == 0) {
      return i;
    }
  }
  return -1;
}

int sage_disk_instability_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");

  /* Placement, checked in every substep phase that holds this module when sage_quasar_mode's
   * by-galaxy entry is configured: this module's first by-galaxy entry before that entry, which
   * reads this substep's UnstableDiskGasFraction, in that same phase (the quasar module's
   * per-event entry is a different consumer and does not count) */
  if (module_in_substep_phase("sage_quasar_mode", PROCESSING_MODE_BY_GALAXY)) {
    for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
      const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
      const int disk_at =
          first_entry_in_mode("sage_disk_instability", PROCESSING_MODE_BY_GALAXY, phase);
      if (disk_at < 0) {
        continue;
      }
      const int quasar_at =
          first_entry_in_mode("sage_quasar_mode", PROCESSING_MODE_BY_GALAXY, phase);
      if (quasar_at < 0 || quasar_at < disk_at) {
        ERROR_LOG("sage_disk_instability must precede sage_quasar_mode (process_by_galaxy) in "
                  "substep phase '%s' "
                  "— otherwise the quasar entry reads the previous substep's "
                  "UnstableDiskGasFraction",
                  phase->name);
        return -1;
      }
    }
  }

  VERBOSE_LOG("SAGE disk instability module initialized");
  VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
  return 0;
}

int sage_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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
