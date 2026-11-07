/**
 * @file    sage26_starburst_feedback.c
 * @brief   SAGE26's collisional starburst for the disk-instability and merger channels
 *
 * Runs SAGE26's collisional_starburst_recipe (R/src/model_mergers.c:584-831) through the
 * shared transaction mimic_apply_collisional_starburst (shared/sage_starburst_physics.h),
 * which owns SAGE26's whole sequence: the bursting fraction, the burst gas, the
 * reheating and the ejection from the burst host's own Vvir (fixed-epsilon with FIREmodeOn 0;
 * with FIREmodeOn 1 the FIRE scaling from the host's float Vvir and the redshift of its stored
 * snapshot, the capped loading without a MassLoading write and the energy-based ejection,
 * :690-752), the bulge history
 * sampling, the star-formation commit, the locked stars into the bulge and the selected
 * channel (the instability channel followed by its incremental bulge radius, BulgeSizeOn
 * 3), the feedback commit to the FoF central, the post-minor-merger instability recheck,
 * and only then the burst's metal yield.
 *
 * Trigger channels:
 *   - process_by_galaxy: the disk instability (UnstableDiskGasFraction > 0, mode 1, the
 *     instability channel), after sage26_disk_instability and sage26_quasar_mode, as
 *     SAGE26's check_disk_instability calls grow_black_hole and then the burst
 *     (R/src/model_disk_instability.c:101-108). It never acts on a feedback-free-burst row
 *     (FFBRegime 1): SAGE26's FFB call runs no disk-instability check
 *     (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367), and the package's
 *     sage26_disk_instability writes nothing on such a row, so its UnstableDiskGasFraction keeps
 *     its per-snapshot reset value 0;
 *   - process_per_event: the merger event (mode 0), after sage26_quasar_mode's merger
 *     black-hole growth (R/src/model_mergers.c:262-287), on an FFB target as on any other, its
 *     post-merger recheck included (SAGE26's merger routines do not read FFBRegime).
 *
 * FIREmodeOn is a required parameter (campaign decision D33), 0 or 1, refused otherwise. This
 * module builds the burst parameter bundle, so it passes FIREmodeOn and the run's redshift table
 * to every burst it runs: the instability's, the merger's and the recheck's. A FIREmodeOn 1 burst
 * whose host's stored SnapNum lies outside the redshift table is refused (campaign decision D35)
 * and process() returns -1.
 *
 * CGMrecipeOn is a required parameter too (campaign decision D33), 0 or 1, refused otherwise; the
 * bundle carries it to every burst's feedback commit and yield, which route to the FoF central's
 * CGM pair with CGMrecipeOn 1 and the CENTRAL's Regime 0 and to its hot pair otherwise, and to the
 * recheck's black-hole growth, whose wind selects the burst HOST's pair by the host's own Regime
 * (R/src/model_starformation_and_feedback.c:1175, R/src/model_misc.h:69,
 * R/src/model_mergers.c:406).
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
 * Each call also passes SAGE26's old_disk_radius, the disk radius its caller saved, which
 * the instability channel's radius update reads (R/src/model_mergers.c:783-784): the
 * instability's burst passes the galaxy's current DiskScaleRadius, as SAGE26's
 * check_disk_instability does (R/src/model_disk_instability.c:106-107); the merger burst
 * passes the target's DiskScaleRadius, which SAGE26 saves before add_galaxies_together
 * (R/src/model_mergers.c:257, :285-287). No package writer of DiskScaleRadius runs between
 * that point of the resolver's transaction and this consumer (the addition and the
 * black-hole growth leave it alone, R/src/model_mergers.c:346-543; only
 * sage26_set_disk_scale_radius writes it, at pre-timestep), so the target's field read
 * here is the saved value.
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
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_h2_physics.h"
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

/* SAGE26's BulgeSizeOn: 0 (R0) or 3 (Tonini sizes), the mode of the instability channel's
 * radius update and of the recheck's (R/src/model_misc.c:395) */
static int BULGE_SIZE_ON;

/* SAGE26's SFprescription: 0 (R0, R1) or 1 (BR06, R2), which the burst's star-formation and
 * feedback commits test for their hydrogen caps (R/src/model_starformation_and_feedback.c:1131,
 * :1169), the burst for its hydrogen refresh (R/src/model_mergers.c:798) and the recheck's
 * black-hole growth for its accretion cap (:366-367) */
static int SF_PRESCRIPTION;

/* SAGE26's StarburstColdGasOn: 1, every burst uses the cold gas (R/src/model_mergers.c:680-683);
 * 0, the H2 burst fuel, is not ported */
static int STARBURST_COLD_GAS_ON;

/* SAGE26's FIREmodeOn: 0 (R0-R2, fixed-epsilon) or 1 (R3, the FIRE budget of every burst,
 * R/src/model_mergers.c:690-752), required and checked at init
 * (shared/sage_fire_feedback_physics.h) */
static int FIRE_MODE_ON;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and checked at
 * init (campaign decision D33); the bundle carries it to every burst
 * (shared/sage_starburst_physics.h) */
static int CGM_RECIPE_ON;

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
  LOAD_PARAM_INT("BulgeSizeOn", BULGE_SIZE_ON);
  if (mimic_sage_validate_bulge_size_on("sage26_starburst_feedback", BULGE_SIZE_ON) != 0) {
    return -1;
  }
  LOAD_PARAM_INT("SFprescription", SF_PRESCRIPTION);
  if (mimic_sage_validate_sf_prescription("sage26_starburst_feedback", SF_PRESCRIPTION) != 0) {
    return -1;
  }
  LOAD_PARAM_INT("StarburstColdGasOn", STARBURST_COLD_GAS_ON);
  if (mimic_sage_validate_starburst_cold_gas_on("sage26_starburst_feedback",
                                                STARBURST_COLD_GAS_ON) != 0) {
    return -1;
  }
  /* Required, no default (campaign decision D33): a missing key fails here */
  LOAD_PARAM_INT("FIREmodeOn", FIRE_MODE_ON);
  if (mimic_sage_validate_fire_mode("sage26_starburst_feedback", FIRE_MODE_ON) != 0) {
    return -1;
  }
  /* Required, no default (campaign decision D33): a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on("sage26_starburst_feedback", CGM_RECIPE_ON) != 0) {
    return -1;
  }

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
  VERBOSE_LOG("  BulgeSizeOn = %d", BULGE_SIZE_ON);
  VERBOSE_LOG("  SFprescription = %d", SF_PRESCRIPTION);
  VERBOSE_LOG("  StarburstColdGasOn = %d", STARBURST_COLD_GAS_ON);
  VERBOSE_LOG("  FIREmodeOn = %d", FIRE_MODE_ON);
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);
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
      /* SAGE26's run_params->BulgeSizeOn and Hubble_h (R/src/model_misc.c:395, :398) */
      .bulge_size = {BULGE_SIZE_ON, ctx->params != NULL ? ctx->params->Hubble_h : 0.0},
      /* SAGE26's run_params->SFprescription (R/src/model_starformation_and_feedback.c:1131,
       * :1169; R/src/model_mergers.c:366-367, :798) */
      .sf_prescription = SF_PRESCRIPTION,
      /* SAGE26's run_params->FIREmodeOn and ZZ, which every burst's FIRE budget reads with its
       * host's stored SnapNum (R/src/model_mergers.c:692-694) */
      .fire_mode_on = FIRE_MODE_ON,
      .redshift_config = ctx->params,
      /* SAGE26's run_params->CGMrecipeOn, which every burst's feedback and yield and the recheck's
       * wind read (R/src/model_starformation_and_feedback.c:1175, R/src/model_misc.h:69,
       * R/src/model_mergers.c:406) */
      .cgm_recipe_on = CGM_RECIPE_ON,
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
    /* SAGE parity: SAGE26's old_disk_radius is the target's DiskScaleRadius saved before
     * add_galaxies_together (R/src/model_mergers.c:257); nothing since has written it (file
     * comment), so the target's field is that value */
    const double old_disk_radius = gal->DiskScaleRadius;
    /* SAGE26's whole burst transaction, the post-minor-merger instability recheck before
     * the yield included (R/src/model_mergers.c:584-831) */
    if (mimic_apply_collisional_starburst(event->value0, event_halo, central_halo, 0,
                                          burst_to_merger_bulge, old_disk_radius, event_halo->dT,
                                          sfr_bin, event->value1, ctx, &params, NULL) != 0) {
      ERROR_LOG("Merger starburst refused (source=%d, target=%d)", event->source_index,
                event->target_index);
      return -1;
    }
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

  /* The instability's trigger, never positive on an FFB row: SAGE26 runs no instability check
   * after its FFB call (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367), and
   * sage26_disk_instability writes nothing on that row, which keeps the per-snapshot reset value 0
   */
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
     * (burst_to_merger_bulge 0) with the galaxy's current DiskScaleRadius as its
     * old_disk_radius, after sage26_quasar_mode's black-hole growth
     * (R/src/model_disk_instability.c:101-108) */
    if (mimic_apply_collisional_starburst(gal->UnstableDiskGasFraction, halo, central_halo, 1, 0,
                                          (double)gal->DiskScaleRadius, halo->dT, sfr_bin, disk_dt,
                                          ctx, &params, NULL) != 0) {
      ERROR_LOG("Disk-instability starburst refused for halo %lld", halo->HaloNr);
      return -1;
    }
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
  BULGE_SIZE_ON = MIMIC_SAGE_BULGE_SIZE_OFF;
  FIRE_MODE_ON = MIMIC_SAGE_FIRE_MODE_OFF;
  CGM_RECIPE_ON = MIMIC_SAGE_CGM_RECIPE_OFF;
  VERBOSE_LOG("SAGE starburst feedback module cleaned up");
  return 0;
}
