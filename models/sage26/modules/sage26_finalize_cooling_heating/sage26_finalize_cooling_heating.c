/**
 * @file    sage26_finalize_cooling_heating.c
 * @brief   SAGE26 end-of-halo normalisation of the Cooling and Heating energies
 *
 * sage26_apply_cooling and sage26_radio_mode_heating accumulate the cooling and
 * AGN heating energies 0.5 * m * Vvir^2 undivided over every substep, as SAGE26
 * does. After the last substep SAGE26 turns the sums into rates once per halo
 * (R/src/core_build_model.c:590-603):
 *
 *   deltaT     = Age[galaxies[0].SnapNum] - halo_age;
 *   inv_deltaT = 1.0 / deltaT;
 *   Cooling   *= inv_deltaT;  Heating *= inv_deltaT;   (every galaxy with mergeType 0)
 *
 * This module reproduces that block in post_timestep, once per FoF group. The
 * interval is the dT of the FoF's first galaxy in the order Mimic holds the rows;
 * the core computes each row's dT with SAGE26's deltaT expression. A galaxy that
 * merged or was disrupted this snapshot (the package's Type 3, which
 * sage26_resolve_mergers_and_disruption sets where SAGE26 sets mergeType > 0) is
 * not normalised. The normalisation's OutflowRate term belongs to a later node.
 */

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/** @brief Whether a row merged or was disrupted this snapshot (SAGE26's mergeType > 0) */
static int has_merged(const struct Halo *halo) { return halo->Type == 3; }

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_finalize_cooling_heating_init(void) {
  VERBOSE_LOG("SAGE finalize cooling and heating module initialized");
  return 0;
}

int sage26_finalize_cooling_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                            int ngal) {
  (void)ctx;

  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // SAGE26's galaxies[0]: the FoF's first galaxy in the order Mimic holds the
  // rows. Workspace rows without a galaxy are not galaxies and are skipped. Its
  // interval is used even when it has merged itself (R/src/core_build_model.c:591).
  int first = -1;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy != NULL) {
      first = i;
      break;
    }
  }
  if (first == -1) {
    return 0;
  }

  const double deltaT = halos[first].dT;

  // Where the core gives the first galaxy no positive interval (its snapshot-zero
  // birth, dT = -1) no physics ran this snapshot, and the energies are set to 0.
  // SAGE26 always has a positive interval there (R/src/core_init.c:60-67): a
  // core-boundary difference, not reproduced.
  if (!(deltaT > 0.0)) {
    for (int i = 0; i < ngal; i++) {
      if (halos[i].galaxy == NULL || has_merged(&halos[i])) {
        continue;
      }
      halos[i].galaxy->Cooling = 0.0;
      halos[i].galaxy->Heating = 0.0;
    }
    return 0;
  }

  const double inv_deltaT = 1.0 / deltaT;

  for (int i = 0; i < ngal; i++) {
    // Don't bother with galaxies that have already merged (:597-600)
    if (halos[i].galaxy == NULL || has_merged(&halos[i])) {
      continue;
    }
    halos[i].galaxy->Cooling *= inv_deltaT;
    halos[i].galaxy->Heating *= inv_deltaT;
  }

  return 0;
}

int sage26_finalize_cooling_heating_cleanup(void) {
  VERBOSE_LOG("SAGE finalize cooling and heating module cleaned up");
  return 0;
}
