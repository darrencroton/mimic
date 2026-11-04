/**
 * @file    sage26_starburst_feedback.c
 * @brief   SAGE26's collisional starburst for the disk-instability and merger channels
 *
 * Runs SAGE26's collisional_starburst_recipe (R/src/model_mergers.c:584-831) through the
 * shared transaction mimic_apply_collisional_starburst (shared/sage_starburst_physics.h),
 * which owns SAGE26's whole sequence: the bursting fraction, the burst gas, the
 * reheating and the ejection from the burst host's own Vvir, the bulge history
 * sampling, the star-formation commit, the locked stars into the bulge and the selected
 * channel, the feedback commit to the FoF central, the post-minor-merger instability
 * recheck, and only then the burst's metal yield.
 *
 * Trigger channels:
 *   - process_by_galaxy: the disk instability (UnstableDiskGasFraction > 0, mode 1, the
 *     instability channel), after sage26_disk_instability and sage26_quasar_mode, as
 *     SAGE26's check_disk_instability calls grow_black_hole and then the burst
 *     (R/src/model_disk_instability.c:101-108);
 *   - process_per_event: the merger event (mode 0), after sage26_quasar_mode's merger
 *     black-hole growth (R/src/model_mergers.c:262-287).
 *
 * The post-minor-merger recheck (R/src/model_mergers.c:806-811) runs inside the shared
 * transaction when sage26_disk_instability is configured (SAGE26's DiskInstabilityOn);
 * its gas branch grows the black hole when the quasar event consumer is configured
 * (SAGE26's AGNrecipeOn), with that module's BlackHoleGrowthRate and QuasarModeEfficiency.
 *
 * Every burst samples the bursting galaxy's bulge star-formation history
 * (R/src/model_mergers.c:755-757). Each call passes the substep's history bin and SAGE26's
 * dt, the substep interval: the bursting galaxy's own dT / num_substeps for the
 * disk-instability channel (SAGE26's starformation_and_feedback dt,
 * R/src/core_build_model.c:516), and the merging satellite's, carried in the merger
 * event's value1, for the merger channel and its post-merger instability recheck
 * (R/src/core_build_model.c:581, R/src/model_mergers.c:285, :809).
 *
 * References:
 *   - SAGE26 R/src/model_mergers.c:584-831 (collisional_starburst_recipe)
 *   - Somerville et al. (2001): starburst efficiency scaling
 *   - Cox PhD thesis: starburst efficiency coefficients
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
#include "shared/sage_sfr_history.h"
#include "shared/sage_merger_physics.h"
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
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_starburst_feedback_init(void) {
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
  if (module_in_substep_phase("sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT) &&
      !modules_in_same_substep_phase("sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT,
                                     "sage26_resolve_mergers_and_disruption",
                                     PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("sage26_starburst_feedback (process_per_event) requires "
              "sage26_resolve_mergers_and_disruption as process_full_halo in the "
              "same substep phase — no merger events will be emitted without it");
    return -1;
  }

  /* WARNING: disk-instability channel requires the trigger writer */
  if (module_in_substep_phase("sage26_starburst_feedback", PROCESSING_MODE_BY_GALAXY) &&
      !module_precedes_in_substep_phase("sage26_disk_instability", PROCESSING_MODE_BY_GALAXY,
                                        "sage26_starburst_feedback", PROCESSING_MODE_BY_GALAXY)) {
    WARNING_LOG("sage26_starburst_feedback: sage26_disk_instability does not "
                "precede it in the same substep phase — disk-instability channel "
                "will be silently inactive (UnstableDiskGasFraction always 0)");
  }

  POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED =
      module_in_substep_phase("sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  POST_MERGER_QUASAR_FOLLOWUP_ENABLED =
      module_in_substep_phase("sage26_quasar_mode", PROCESSING_MODE_PER_EVENT);

  /* WARNING: without the quasar event consumer the post-merger recheck skips its black-hole
   * growth and wind, which SAGE26 runs before the recheck's burst
   * (R/src/model_disk_instability.c:102-104) */
  if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED && !POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
    WARNING_LOG("sage26_starburst_feedback: sage26_disk_instability is configured "
                "but sage26_quasar_mode is absent as process_per_event — "
                "post-merger disk instability quasar wind is silently skipped "
                "(SAGE parity loss)");
  }

  if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED) {
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", POST_MERGER_STAR_FORMING_DISK_FACTOR,
                                      0.0, 10.0, "star forming disk factor");
  } else {
    VERBOSE_LOG("  (StarFormingDiskFactor not loaded: sage26_disk_instability"
                " not present as process_by_galaxy)");
  }

  if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED && POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", POST_MERGER_BLACK_HOLE_GROWTH_RATE,
                                      0.0, 1.0, "BH growth rate");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", POST_MERGER_QUASAR_MODE_EFFICIENCY,
                                      0.0, 1.0, "quasar mode efficiency");
  } else if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED) {
    VERBOSE_LOG("  (BlackHoleGrowthRate, QuasarModeEfficiency not loaded:"
                " sage26_quasar_mode not present as process_per_event)");
  }

  /* SAGE26's EnergySNcode and EtaSNcode (R/src/core_init.c:102-103), through the helpers
   * sage26_calculate_supernova_feedback uses */
  energy_sn_code = mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
  eta_sn_code = mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);

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

int sage26_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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
      /* The post-minor-merger recheck: SAGE26's DiskInstabilityOn and AGNrecipeOn > 0
       * (R/src/model_mergers.c:807, R/src/model_disk_instability.c:102) are this run's
       * configuration of sage26_disk_instability and of the quasar event consumer */
      .disk_instability_on = POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED,
      .star_forming_disk_factor = POST_MERGER_STAR_FORMING_DISK_FACTOR,
      .agn_recipe_on = POST_MERGER_QUASAR_FOLLOWUP_ENABLED,
      .black_hole_growth_rate = POST_MERGER_BLACK_HOLE_GROWTH_RATE,
      .quasar_mode_efficiency = POST_MERGER_QUASAR_MODE_EFFICIENCY,
  };

  if (ctx->active_event != NULL) {
    if (ngal != 1) {
      ERROR_LOG("sage26_starburst_feedback (process_per_event) expects ngal=1, got %d", ngal);
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
     * only merger events reach this module — no event_id check needed. SAGE26 bursts on
     * every merger, a ratio of 0 included (R/src/model_mergers.c:284-287). */
    if (event->value1 <= 0.0) {
      ERROR_LOG("Merger event missing valid source dt (value1=%.3e, source=%d, target=%d)",
                event->value1, event->source_index, event->target_index);
      return -1;
    }

    /* The bulge history: this substep's bin and SAGE26's merger dt, the satellite's
     * substep interval (R/src/core_build_model.c:579-581) */
    const int sfr_bin = mimic_sage_sfr_step_bin(ctx->substep_number, ctx->num_substeps);
    if (sfr_bin < 0) {
      ERROR_LOG("Invalid merger SFR history bin (substep %d of %d)", ctx->substep_number,
                ctx->num_substeps);
      return -1;
    }

    /* The package's StarFormationRate diagnostic is a snapshot-averaged rate, so
     * its denominator is the full interval (event_halo->dT), matching the disk SF
     * convention (StarFormationRate += stars / halo->dT). Passing the substep dt
     * (event->value1) here would inflate the burst SFR by STEPS. Masses are
     * unaffected: rate_dt only scales that diagnostic. The outflow is committed
     * undivided by the shared feedback commit, as SAGE26's update_from_feedback
     * does (R/src/model_starformation_and_feedback.c:1249). */
    /* SAGE parity: the channel from the target's morphology BEFORE the satellite was
     * added, which the resolver carries with the event in MergerDiskDominated
     * (R/src/model_mergers.c:251-254, :266-282) */
    const int burst_to_merger_bulge = mimic_sage_merger_burst_channel(
        event->value0, THRESHOLD_MAJOR_MERGER, gal->MergerDiskDominated);
    /* SAGE26's whole burst transaction, the post-minor-merger instability recheck before
     * the yield included (R/src/model_mergers.c:584-831) */
    mimic_apply_collisional_starburst(event->value0, event_halo, central_halo, 0,
                                      burst_to_merger_bulge, event_halo->dT, sfr_bin, event->value1,
                                      ctx, &params, NULL);
    DEBUG_LOG("Starburst from merger event (ratio=%.3f, source=%d, target=%d)", event->value0,
              event->source_index, event->target_index);
    return 0;
  }

  if (ngal != 1) {
    ERROR_LOG("sage26_starburst_feedback expects ngal=1, got %d", ngal);
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
      ERROR_LOG("Invalid disk-instability dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
                "status=%s)",
                halo->HaloNr, halo->SnapNum, halo->dT, ctx->num_substeps,
                mimic_object_time_status_str(dt_status));
      return -1;
    }

    /* The bulge history: this substep's bin and SAGE26's dt, the galaxy's substep
     * interval (R/src/core_build_model.c:514-516, R/src/model_disk_instability.c:106) */
    const int sfr_bin = mimic_sage_sfr_step_bin(ctx->substep_number, ctx->num_substeps);
    if (sfr_bin < 0) {
      ERROR_LOG("Invalid disk-instability SFR history bin for halo %lld (substep %d of %d)",
                halo->HaloNr, ctx->substep_number, ctx->num_substeps);
      return -1;
    }

    /* The package's snapshot-averaged StarFormationRate diagnostic uses the full
     * interval (halo->dT), like disk SF; the outflow is committed undivided
     * (R/src/model_starformation_and_feedback.c:1249). The substep interval disk_dt
     * is the bulge history's denominator. */
    /* SAGE parity: the instability's burst, mode 1 into the instability channel
     * (burst_to_merger_bulge 0), after sage26_quasar_mode's black-hole growth
     * (R/src/model_disk_instability.c:101-108) */
    mimic_apply_collisional_starburst(gal->UnstableDiskGasFraction, halo, central_halo, 1, 0,
                                      halo->dT, sfr_bin, disk_dt, ctx, &params, NULL);
    DEBUG_LOG("Starburst from disk instability (eff=%.3f)", gal->UnstableDiskGasFraction);
  }

  return 0;
}

int sage26_starburst_feedback_cleanup(void) {
  POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED = false;
  POST_MERGER_QUASAR_FOLLOWUP_ENABLED = false;
  POST_MERGER_STAR_FORMING_DISK_FACTOR = 0.0;
  POST_MERGER_BLACK_HOLE_GROWTH_RATE = 0.0;
  POST_MERGER_QUASAR_MODE_EFFICIENCY = 0.0;
  VERBOSE_LOG("SAGE starburst feedback module cleaned up");
  return 0;
}
