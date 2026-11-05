/**
 * @file sage26_quasar_mode.c
 * @brief SAGE26's black-hole growth and quasar-mode wind
 *
 * Runs SAGE26's grow_black_hole, which calls quasar_mode_wind before it records the
 * accreted mass (R/src/model_mergers.c:346-441), through the shared
 * mimic_apply_black_hole_growth (shared/sage_agn_physics.h): accretion capped to ColdGas,
 * the black-hole, cold-gas and cold-metal updates with the negative-metal floor, for
 * SFprescription 1 the hydrogen cap (H2gas and H1gas each capped at the hydrogen of the
 * remaining cold gas, R/src/model_mergers.c:366-372), the wind against the cold and then the
 * cold plus hot binding energies, and the float QuasarModeBHaccretionMass accumulation.
 *
 * Trigger channels:
 * - process_by_galaxy: the disk instability's gas branch (UnstableDiskGasFraction > 0,
 *   written by sage26_disk_instability), before sage26_starburst_feedback's burst
 *   (R/src/model_disk_instability.c:101-108)
 * - process_per_event: the merger event, on the merger target (R/src/model_mergers.c:262-264)
 *
 * The post-minor-merger recheck's black-hole growth runs inside
 * sage26_starburst_feedback's burst transaction, with this module's parameters and that
 * module's own SFprescription.
 *
 * Parameters: BlackHoleGrowthRate, QuasarModeEfficiency, and the required SFprescription
 * (0 or 1; a missing key fails at initialisation, any other value is refused), which selects
 * the accretion's hydrogen cap.
 *
 * References:
 *   - SAGE26 R/src/model_mergers.c:346-441 (grow_black_hole, quasar_mode_wind)
 *   - SAGE26 R/src/model_disk_instability.c:101-104 (the instability's call)
 *   - Kauffmann & Haehnelt (2000) - BH growth model
 *   - Croton et al. (2006, 2016) - SAGE model papers
 */

#include "constants.h"
#include "error.h"
#include "shared/sage_agn_physics.h"
#include "shared/sage_h2_physics.h"
#include "module_system/generated/event_contracts.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"
#include "globals.h"
#include "module_registry.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double BLACK_HOLE_GROWTH_RATE;
static double QUASAR_MODE_EFFICIENCY;
/* SAGE26's SFprescription: 0 (R0, R1) or 1 (R2), which selects the accretion's hydrogen cap
 * (R/src/model_mergers.c:366-372); a required parameter with no default */
static int SF_PRESCRIPTION;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_quasar_mode_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", BLACK_HOLE_GROWTH_RATE, 0.0, 1.0,
                                    "BH growth rate");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", QUASAR_MODE_EFFICIENCY, 0.0, 1.0,
                                    "quasar mode efficiency");
  LOAD_PARAM_INT("SFprescription", SF_PRESCRIPTION);
  if (mimic_sage_validate_sf_prescription("sage26_quasar_mode", SF_PRESCRIPTION) != 0) {
    return -1;
  }

  /* Dependency check: process_per_event requires a merger event producer in
   * the same substep phase */
  if (module_in_substep_phase("sage26_quasar_mode", PROCESSING_MODE_PER_EVENT) &&
      !modules_in_same_substep_phase("sage26_quasar_mode", PROCESSING_MODE_PER_EVENT,
                                     "sage26_resolve_mergers_and_disruption",
                                     PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("sage26_quasar_mode (process_per_event) requires "
              "sage26_resolve_mergers_and_disruption as process_full_halo in the "
              "same substep phase — no merger events will be emitted without it");
    return -1;
  }

  VERBOSE_LOG("SAGE quasar-mode AGN feedback initialized");
  VERBOSE_LOG("  BlackHoleGrowthRate = %.4f", BLACK_HOLE_GROWTH_RATE);
  VERBOSE_LOG("  QuasarModeEfficiency = %.3f", QUASAR_MODE_EFFICIENCY);
  VERBOSE_LOG("  SFprescription = %d", SF_PRESCRIPTION);

  return 0;
}

int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ctx == NULL || halos == NULL || ngal <= 0) {
    return 0;
  }

  if (ctx->active_event != NULL) {
    if (ngal != 1) {
      ERROR_LOG("sage26_quasar_mode (process_per_event) expects ngal=1, got %d", ngal);
      return -1;
    }

    struct Halo *event_halo = &halos[0];
    if (event_halo->galaxy == NULL) {
      return 0;
    }

    const struct ModuleEvent *event = ctx->active_event;

    /* Subscription routing (see module_info.yaml events.consumes) ensures
     * only merger events reach this module — no event_id check needed. */
    const double merger_ratio = event->value0;

    /* SAGE parity: grow_black_hole on the merger target, its quasar wind included, for
     * every merger, a ratio of 0 included (R/src/model_mergers.c:262-264, :346-378) */
    (void)mimic_apply_black_hole_growth(event_halo, merger_ratio, BLACK_HOLE_GROWTH_RATE,
                                        QUASAR_MODE_EFFICIENCY, ctx, SF_PRESCRIPTION, NULL);

    DEBUG_LOG("Quasar mode from merger event (ratio=%.3f, source=%d, target=%d)", merger_ratio,
              event->source_index, event->target_index);
    return 0;
  }

  if (ngal != 1) {
    ERROR_LOG("sage26_quasar_mode expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];
  struct GalaxyData *gal = halo->galaxy;

  if (gal == NULL) {
    return 0;
  }

  /* SAGE parity: the instability's gas branch grows the black hole of the unstable galaxy
   * itself, its quasar wind included, before the burst (R/src/model_disk_instability.c:90-105);
   * sage26_disk_instability hands the fraction on through UnstableDiskGasFraction */
  if (gal->UnstableDiskGasFraction > 0.0) {
    (void)mimic_apply_black_hole_growth(halo, gal->UnstableDiskGasFraction, BLACK_HOLE_GROWTH_RATE,
                                        QUASAR_MODE_EFFICIENCY, ctx, SF_PRESCRIPTION, NULL);
    DEBUG_LOG("Quasar mode from disk instability (eff=%.3f)", gal->UnstableDiskGasFraction);
  }

  return 0;
}

int sage26_quasar_mode_cleanup(void) {
  VERBOSE_LOG("SAGE quasar-mode AGN feedback cleaned up");
  return 0;
}
