/**
 * @file    sage26_disk_instability.c
 * @brief   SAGE26 disk instability: stellar transfer to the bulge and the unstable-gas trigger
 *
 * Moves the unstable part of the stellar disk (above the Mo, Mao & White 1998 critical mass) to
 * the bulge and its instability channel, updates the instability-bulge radius (BulgeSizeOn 3), and
 * sets UnstableDiskGasFraction for sage26_quasar_mode and sage26_starburst_feedback. Implements
 * SAGE26's check_disk_instability up to its black-hole and burst calls
 * (shared/sage_disk_instability_physics.h):
 *
 *     diskmass       = ColdGas + (StellarMass - BulgeMass)
 *     Mcrit          = Vmax * Vmax * (3.0 * DiskScaleRadius) / G, capped at diskmass
 *     unstable_stars = (1 - ColdGas / diskmass) * (diskmass - Mcrit)
 *     BulgeMass, InstabilityBulgeMass += unstable_stars
 *     MetalsBulgeMass                 += Z_disk * unstable_stars
 *
 * with no cap on the transfer. Where unstable_gas > 0 and ColdGas > 0, UnstableDiskGasFraction is
 * unstable_gas / ColdGas. A row whose FFBRegime is 1 is not checked.
 *
 * Reference: Mo, Mao & White (1998)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include <stdbool.h>
#include <string.h>
#include "error.h"
#include "globals.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_disk_instability_physics.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/* SAGE parity: SAGE26's TOOMRE_DISK_FACTOR is the double literal 3.0. The parameter enters
 * Mcrit exactly where that constant does; the run files hold 3.0. */
static double STAR_FORMING_DISK_FACTOR;

/* SAGE26's BulgeSizeOn: 0 (no bulge sizes) or 3 (Tonini sizes), the mode of the radius update
 * that follows the stellar transfer */
static int BULGE_SIZE_ON;

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

int sage26_disk_instability_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR, 0.0, 10.0,
                                    "star forming disk factor");
  LOAD_PARAM_INT("BulgeSizeOn", BULGE_SIZE_ON);
  if (mimic_sage_validate_bulge_size_on("sage26_disk_instability", BULGE_SIZE_ON) != 0) {
    return -1;
  }

  /* Placement, checked in every substep phase that holds this module when sage26_quasar_mode's
   * by-galaxy entry is configured: this module's first by-galaxy entry before that entry, which
   * reads this substep's UnstableDiskGasFraction, in that same phase (the quasar module's
   * per-event entry is a different consumer and does not count) */
  if (module_in_substep_phase("sage26_quasar_mode", PROCESSING_MODE_BY_GALAXY)) {
    for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
      const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
      const int disk_at =
          first_entry_in_mode("sage26_disk_instability", PROCESSING_MODE_BY_GALAXY, phase);
      if (disk_at < 0) {
        continue;
      }
      const int quasar_at =
          first_entry_in_mode("sage26_quasar_mode", PROCESSING_MODE_BY_GALAXY, phase);
      if (quasar_at < 0 || quasar_at < disk_at) {
        ERROR_LOG("sage26_disk_instability must precede sage26_quasar_mode (process_by_galaxy) in "
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
  /* SAGE parity: SAGE26's starformation_ffb returns with no disk-instability check, so this
   * module writes nothing on an FFBRegime 1 row; UnstableDiskGasFraction keeps its per-snapshot
   * reset value 0, so the quasar and burst entries do not act on it. The post-merger recheck
   * inside the burst transaction runs on an FFB target too. */
  if (gal->FFBRegime == 1) {
    return 0;
  }
  /* SAGE26's run_params (G, and BulgeSizeOn's Hubble_h): a run always passes them, so a
   * missing configuration is refused rather than read as Hubble_h 0 */
  if (ctx == NULL || ctx->params == NULL) {
    ERROR_LOG("sage26_disk_instability: the run configuration (G, Hubble_h) is missing");
    return -1;
  }
  const struct MimicSageBulgeSize sizes = {BULGE_SIZE_ON, ctx->params->Hubble_h};
  gal->UnstableDiskGasFraction =
      mimic_sage_apply_disk_instability(halo, ctx, STAR_FORMING_DISK_FACTOR, &sizes);

  return 0;
}

int sage26_disk_instability_cleanup(void) {
  BULGE_SIZE_ON = MIMIC_SAGE_BULGE_SIZE_OFF;
  VERBOSE_LOG("SAGE disk instability module cleaned up");
  return 0;
}
