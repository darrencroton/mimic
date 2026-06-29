/**
 * @file    sage_starburst_feedback.c
 * @brief   Collisional starburst from disk instability and merger channels
 *
 * Implements starburst recipe similar to Somerville et al. (2001).
 * The efficiency coefficients are taken from TJ Cox's PhD thesis.
 *
 * Trigger channels:
 *   - process_by_galaxy: Disk instability (UnstableDiskGasFraction > 0, mode=1)
 *   - process_per_event: Merger event payload (mode=0)
 *
 * Physics: Stars form in burst, feedback reheats cold gas to hot, ejects hot gas.
 * Newly formed stars contribute to bulge mass (starbursts form spheroids).
 *
 * References:
 *   - SAGE model_mergers.c: collisional_starburst_recipe() lines 203-293
 *   - Somerville et al. (2001): Starburst efficiency scaling
 *   - Cox PhD thesis: Starburst efficiency coefficients
 *   - Croton et al. (2006, 2016)
 */

#include <stdbool.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/sage_agn_physics.h"
#include "shared/sage_starburst_physics.h"
#include "shared/sage_disk_instability_physics.h"
#include "module_system/generated/event_contracts.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;
static double THRESHOLD_MAJOR_MERGER;
static double POST_MERGER_STAR_FORMING_DISK_FACTOR;
static double POST_MERGER_BLACK_HOLE_GROWTH_RATE;
static double POST_MERGER_QUASAR_MODE_EFFICIENCY;

// ============================================================================
// MODULE-LOCAL CONSTANTS (converted from physical constants)
// ============================================================================

static double energy_sn_code;
static double eta_sn_code;
static bool POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED;
static bool POST_MERGER_QUASAR_FOLLOWUP_ENABLED;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Run SAGE's same-step minor-merger disk-instability follow-up when configured.
 *
 * The event halo is the live merger target after baryon transfer. Optional quasar
 * follow-up mirrors SAGE's grow_black_hole() call before the collisional burst.
 */
static void maybe_apply_post_merger_disk_instability_followup(
    struct ModuleContext *ctx, struct Halo *event_halo, struct Halo *central_halo,
    double mass_ratio, double event_dt, const struct MimicStarburstParams *params) {
  if (!POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED || ctx == NULL || event_halo == NULL ||
      event_halo->galaxy == NULL || central_halo == NULL || central_halo->galaxy == NULL ||
      params == NULL || mass_ratio >= params->threshold_major_merger) {
    return;
  }

  const double unstable_gas_fraction =
      mimic_sage_apply_disk_instability(event_halo, ctx, POST_MERGER_STAR_FORMING_DISK_FACTOR);
  if (unstable_gas_fraction <= 0.0) {
    return;
  }

  if (POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
    const double bh_accrete = mimic_apply_black_hole_growth(event_halo, unstable_gas_fraction,
                                                            POST_MERGER_BLACK_HOLE_GROWTH_RATE);
    if (bh_accrete > 0.0) {
      /* SAGE parity: grow_black_hole() itself applies quasar_mode_wind(),
       * so the post-merger disk-instability recheck must include the wind
       * before the collisional starburst. */
      mimic_apply_quasar_mode_wind(event_halo, bh_accrete, POST_MERGER_QUASAR_MODE_EFFICIENCY, ctx);
    }
  }

  mimic_apply_collisional_starburst(unstable_gas_fraction, event_halo->galaxy, central_halo->galaxy,
                                    central_halo, 1, event_dt, params);
  DEBUG_LOG("Post-merger disk instability follow-up (eff=%.3f)", unstable_gas_fraction);
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_starburst_feedback_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("FeedbackReheatingEpsilon", FEEDBACK_REHEATING_EPSILON, 0.0,
                                    10.0, "feedback reheating epsilon");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency", FEEDBACK_EJECTION_EFFICIENCY, 0.0,
                                    10.0, "feedback ejection efficiency");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                    "recycle fraction");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                    "frac Z leave disk");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER, 0.0, 1.0,
                                    "major merger threshold");

  /* Dependency checks */

  /* ERROR: process_per_event requires a merger event producer in the same phase */
  if (module_in_substep_phase("sage_starburst_feedback", PROCESSING_MODE_PER_EVENT) &&
      !modules_in_same_substep_phase("sage_starburst_feedback", PROCESSING_MODE_PER_EVENT,
                                     "sage_resolve_mergers_and_disruption",
                                     PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("sage_starburst_feedback (process_per_event) requires "
              "sage_resolve_mergers_and_disruption as process_full_halo in the "
              "same substep phase — no merger events will be emitted without it");
    return -1;
  }

  /* WARNING: disk-instability channel requires the trigger writer */
  if (module_in_substep_phase("sage_starburst_feedback", PROCESSING_MODE_BY_GALAXY) &&
      !module_precedes_in_substep_phase("sage_disk_instability", PROCESSING_MODE_BY_GALAXY,
                                        "sage_starburst_feedback", PROCESSING_MODE_BY_GALAXY)) {
    WARNING_LOG("sage_starburst_feedback: sage_disk_instability does not "
                "precede it in the same substep phase — disk-instability channel "
                "will be silently inactive (UnstableDiskGasFraction always 0)");
  }

  POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED =
      module_in_substep_phase("sage_disk_instability", PROCESSING_MODE_BY_GALAXY);
  POST_MERGER_QUASAR_FOLLOWUP_ENABLED =
      module_in_substep_phase("sage_quasar_mode", PROCESSING_MODE_PER_EVENT);

  /* WARNING: post-merger disk instability path loses quasar wind (SAGE parity) */
  if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED && !POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
    WARNING_LOG("sage_starburst_feedback: sage_disk_instability is configured "
                "but sage_quasar_mode is absent as process_per_event — "
                "post-merger disk instability quasar wind is silently skipped "
                "(SAGE parity loss)");
  }

  if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED) {
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", POST_MERGER_STAR_FORMING_DISK_FACTOR,
                                      0.0, 10.0, "star forming disk factor");
  } else {
    VERBOSE_LOG("  (StarFormingDiskFactor not loaded: sage_disk_instability"
                " not present as process_by_galaxy)");
  }

  if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED && POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", POST_MERGER_BLACK_HOLE_GROWTH_RATE,
                                      0.0, 1.0, "BH growth rate");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", POST_MERGER_QUASAR_MODE_EFFICIENCY,
                                      0.0, 1.0, "quasar mode efficiency");
  } else if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED) {
    VERBOSE_LOG("  (BlackHoleGrowthRate, QuasarModeEfficiency not loaded:"
                " sage_quasar_mode not present as process_per_event)");
  }

  // Convert physical constants to code units (same as sage_calculate_supernova_feedback)
  energy_sn_code = ENERGY_SN / MimicConfig.UnitEnergy_in_cgs * MimicConfig.Hubble_h;
  eta_sn_code = ETA_SN * (MimicConfig.UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

  VERBOSE_LOG("SAGE starburst feedback module initialized");
  VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
  VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
  VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);
  VERBOSE_LOG("  Yield = %.4f", YIELD);
  VERBOSE_LOG("  energy_sn_code = %.6e (from ENERGY_SN physical constant)", energy_sn_code);
  VERBOSE_LOG("  eta_sn_code = %.6e (from ETA_SN physical constant)", eta_sn_code);
  VERBOSE_LOG("  post-minor-merger disk instability follow-up = %s",
              POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED ? "enabled" : "disabled");
  VERBOSE_LOG("  post-minor-merger quasar follow-up = %s",
              (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED && POST_MERGER_QUASAR_FOLLOWUP_ENABLED)
                  ? "enabled"
                  : "disabled");

  return 0;
}

int sage_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ctx == NULL || halos == NULL || ngal <= 0) {
    return 0;
  }

  const struct MimicStarburstParams params = {
      .feedback_reheating_epsilon = FEEDBACK_REHEATING_EPSILON,
      .feedback_ejection_efficiency = FEEDBACK_EJECTION_EFFICIENCY,
      .recycle_fraction = RECYCLE_FRACTION,
      .yield = YIELD,
      .frac_z_leave_disk = FRAC_Z_LEAVE_DISK,
      .threshold_major_merger = THRESHOLD_MAJOR_MERGER,
      .energy_sn_code = energy_sn_code,
      .eta_sn_code = eta_sn_code,
  };

  if (ctx->active_event != NULL) {
    if (ngal != 1) {
      ERROR_LOG("sage_starburst_feedback (process_per_event) expects ngal=1, got %d", ngal);
      return -1;
    }

    struct Halo *event_halo = &halos[0];
    struct GalaxyData *gal = event_halo->galaxy;
    struct Halo *central_halo = ctx->central_galaxy;
    struct GalaxyData *central_gal = NULL;
    const struct ModuleEvent *event = ctx->active_event;

    if (central_halo != NULL) {
      central_gal = central_halo->galaxy;
    }

    if (gal == NULL || central_gal == NULL) {
      return 0;
    }

    /* Subscription routing (see module_info.yaml events.consumes) ensures
     * only merger events reach this module — no event_id check needed. */
    if (event->value0 <= 0.0) {
      return 0;
    }

    if (event->value1 <= 0.0) {
      ERROR_LOG("Merger event missing valid source dt (value1=%.3e, source=%d, target=%d)",
                event->value1, event->source_index, event->target_index);
      return -1;
    }

    /* SAGE parity: burst SFR/outflow are snapshot-averaged rates, so the
     * rate denominator is the full interval (event_halo->dT), matching the
     * disk SF convention (StarFormationRate += stars / halo->dT). Passing the
     * substep dt (event->value1) here would inflate the burst SFR by STEPS.
     * Masses are unaffected — rate_dt only scales the rate diagnostics. */
    mimic_apply_collisional_starburst(event->value0, gal, central_gal, central_halo, 0,
                                      event_halo->dT, &params);
    maybe_apply_post_merger_disk_instability_followup(ctx, event_halo, central_halo, event->value0,
                                                      event_halo->dT, &params);
    DEBUG_LOG("Starburst from merger event (ratio=%.3f, source=%d, target=%d)", event->value0,
              event->source_index, event->target_index);
    return 0;
  }

  if (ngal != 1) {
    ERROR_LOG("sage_starburst_feedback expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];
  struct GalaxyData *gal = halo->galaxy;

  struct Halo *central_halo = NULL;
  struct GalaxyData *central_gal = NULL;
  if (halo->Type == 0) {
    central_halo = halo;
    central_gal = gal;
  } else if (ctx->central_galaxy != NULL) {
    central_halo = ctx->central_galaxy;
    central_gal = central_halo->galaxy;
  }

  if (gal == NULL || central_gal == NULL) {
    return 0;
  }

  if (gal->UnstableDiskGasFraction > 0.0) {
    double disk_dt = 0.0;
    enum MimicObjectTimeStatus dt_status = mimic_object_substep_dt(halo, ctx, &disk_dt);

    if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
      return 0;
    }
    if (dt_status != MIMIC_OBJECT_TIME_OK) {
      ERROR_LOG("Invalid disk-instability dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, "
                "status=%s)",
                halo->HaloNr, halo->SnapNum, halo->dT, ctx->num_substeps,
                mimic_object_time_status_str(dt_status));
      return -1;
    }

    /* SAGE parity: snapshot-averaged burst rate uses the full interval
     * (halo->dT), like disk SF. disk_dt (substep) is still computed above to
     * validate timing and skip the initial snapshot. */
    mimic_apply_collisional_starburst(gal->UnstableDiskGasFraction, gal, central_gal, central_halo,
                                      1, halo->dT, &params);
    DEBUG_LOG("Starburst from disk instability (eff=%.3f)", gal->UnstableDiskGasFraction);
  }

  return 0;
}

int sage_starburst_feedback_cleanup(void) {
  POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED = false;
  POST_MERGER_QUASAR_FOLLOWUP_ENABLED = false;
  POST_MERGER_STAR_FORMING_DISK_FACTOR = 0.0;
  POST_MERGER_BLACK_HOLE_GROWTH_RATE = 0.0;
  POST_MERGER_QUASAR_MODE_EFFICIENCY = 0.0;
  VERBOSE_LOG("SAGE starburst feedback module cleaned up");
  return 0;
}
