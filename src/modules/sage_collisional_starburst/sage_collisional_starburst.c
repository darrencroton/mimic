/**
 * @file    sage_collisional_starburst.c
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
#include <string.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "types.h"
#include "_shared/merger_physics.h"
#include "_shared/sage_disk_instability_physics.h"
#include "_shared/sage_events.h"
#include "_shared/time_parity.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"

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

// Calculated from physical constants (converted to code units)
static double EnergySNcode;
static double EtaSNcode;
static bool POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED;
static bool POST_MERGER_QUASAR_FOLLOWUP_ENABLED;

static bool phase_has_module(const struct PhaseModuleConfig *phase_config,
                             int num_modules, const char *module_name,
                             enum ProcessingMode mode)
{
    if (phase_config == NULL || module_name == NULL || num_modules <= 0) {
        return false;
    }

    for (int i = 0; i < num_modules; i++) {
        if (phase_config[i].module_name == NULL) {
            continue;
        }
        if (phase_config[i].processing_mode == mode &&
            strcmp(phase_config[i].module_name, module_name) == 0) {
            return true;
        }
    }

    return false;
}

static void maybe_apply_post_merger_disk_instability_followup(
    struct ModuleContext *ctx, struct Halo *event_halo,
    struct Halo *central_halo, double mass_ratio, double event_dt,
    const struct MimicStarburstParams *params)
{
    if (!POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED || ctx == NULL ||
        event_halo == NULL || event_halo->galaxy == NULL ||
        central_halo == NULL || central_halo->galaxy == NULL ||
        params == NULL || mass_ratio >= params->threshold_major_merger) {
        return;
    }

    const double unstable_gas_fraction = mimic_sage_apply_disk_instability(
        event_halo, ctx, POST_MERGER_STAR_FORMING_DISK_FACTOR);
    if (unstable_gas_fraction <= 0.0) {
        return;
    }

    if (POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
        const double bh_accrete = mimic_apply_black_hole_growth(
            event_halo, unstable_gas_fraction,
            POST_MERGER_BLACK_HOLE_GROWTH_RATE);
        if (bh_accrete > 0.0) {
            /* Note: SAGE's check_disk_instability() only calls grow_black_hole()
             * here; it does not apply a quasar wind in this context. This wind
             * step is a deliberate Mimic extension that mirrors the same-step
             * consequences sage_quasar_mode would produce as a phase_2 event
             * consumer, applied inline when that consumer is configured. */
            mimic_apply_quasar_mode_wind(
                event_halo, bh_accrete,
                POST_MERGER_QUASAR_MODE_EFFICIENCY, ctx);
        }
    }

    mimic_apply_collisional_starburst(
        unstable_gas_fraction, event_halo->galaxy, central_halo->galaxy,
        central_halo, 1, event_dt, params);
    DEBUG_LOG("Post-merger disk instability follow-up (eff=%.3f)",
              unstable_gas_fraction);
}

int sage_collisional_starburst_init(void)
{
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("FeedbackReheatingEpsilon",
                                      FEEDBACK_REHEATING_EPSILON, 0.0, 10.0,
                                      "feedback reheating epsilon");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency",
                                      FEEDBACK_EJECTION_EFFICIENCY, 0.0, 10.0,
                                      "feedback ejection efficiency");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION,
                                      0.0, 1.0, "recycle fraction");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK,
                                      0.0, 1.0, "frac Z leave disk");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER,
                                      0.0, 1.0, "major merger threshold");

    POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED = phase_has_module(
        MimicConfig.phase_1, MimicConfig.num_phase_1,
        "sage_disk_instability", PROCESSING_MODE_BY_GALAXY);
    POST_MERGER_QUASAR_FOLLOWUP_ENABLED = phase_has_module(
        MimicConfig.phase_2, MimicConfig.num_phase_2,
        "sage_quasar_mode", PROCESSING_MODE_PER_EVENT);

    if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED) {
        LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor",
                                          POST_MERGER_STAR_FORMING_DISK_FACTOR,
                                          0.0, 10.0, "star forming disk factor");
    } else {
        VERBOSE_LOG("  (StarFormingDiskFactor not loaded: sage_disk_instability"
                    " not present in phase_1 as process_by_galaxy)");
    }

    if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED &&
        POST_MERGER_QUASAR_FOLLOWUP_ENABLED) {
        LOAD_AND_VALIDATE_RANGE_EXCLUSIVE(
            "BlackHoleGrowthRate", POST_MERGER_BLACK_HOLE_GROWTH_RATE,
            0.0, 1.0, "BH growth rate");
        LOAD_AND_VALIDATE_RANGE_INCLUSIVE(
            "QuasarModeEfficiency", POST_MERGER_QUASAR_MODE_EFFICIENCY,
            0.0, 1.0, "quasar mode efficiency");
    } else if (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED) {
        VERBOSE_LOG("  (BlackHoleGrowthRate, QuasarModeEfficiency not loaded:"
                    " sage_quasar_mode not present in phase_2 as process_per_event)");
    }

    // Convert physical constants to code units (same as sage_calculate_supernova_feedback)
    EnergySNcode = ENERGY_SN / UnitEnergy_in_cgs * MimicConfig.Hubble_h;
    EtaSNcode = ETA_SN * (UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

    INFO_LOG("SAGE collisional starburst module initialized");
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);
    VERBOSE_LOG("  Yield = %.4f", YIELD);
    VERBOSE_LOG("  EnergySNcode = %.6e (from ENERGY_SN physical constant)", EnergySNcode);
    VERBOSE_LOG("  EtaSNcode = %.6e (from ETA_SN physical constant)", EtaSNcode);
    VERBOSE_LOG("  post-minor-merger disk instability follow-up = %s",
                POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED ? "enabled" : "disabled");
    VERBOSE_LOG("  post-minor-merger quasar follow-up = %s",
                (POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED &&
                 POST_MERGER_QUASAR_FOLLOWUP_ENABLED)
                    ? "enabled"
                    : "disabled");

    return 0;
}

int sage_collisional_starburst_process(struct ModuleContext *ctx,
                                       struct Halo *halos,
                                       int ngal)
{
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
        .energy_sn_code = EnergySNcode,
        .eta_sn_code = EtaSNcode,
    };

    if (ctx->active_event != NULL) {
        if (ngal != 1) {
            ERROR_LOG("sage_collisional_starburst (process_per_event) expects ngal=1, got %d",
                      ngal);
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

        if (event->event_code != SAGE_EVENT_MERGER) {
            return 0; /* Unknown event code: graceful no-op */
        }

        if (event->value0 <= 0.0) {
            return 0;
        }

        if (event->value1 <= 0.0) {
            ERROR_LOG("Merger event missing valid source dt (value1=%.3e, source=%d, target=%d)",
                      event->value1, event->source_index, event->target_index);
            return -1;
        }

        mimic_apply_collisional_starburst(event->value0, gal, central_gal,
                                          central_halo, 0, event->value1,
                                          &params);
        maybe_apply_post_merger_disk_instability_followup(
            ctx, event_halo, central_halo, event->value0, event->value1,
            &params);
        DEBUG_LOG("Starburst from merger event (ratio=%.3f, source=%d, target=%d)",
                  event->value0, event->source_index, event->target_index);
        return 0;
    }

    // Verify process_by_galaxy mode
    if (ngal != 1) {
        ERROR_LOG("sage_collisional_starburst expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    // Get central galaxy for feedback destination
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

    /* Disk-instability channel (by-galaxy path). */
    if (gal->UnstableDiskGasFraction > 0.0) {
        double disk_dt = 0.0;
        enum MimicObjectTimeStatus dt_status = mimic_object_substep_dt(halo, ctx, &disk_dt);

        if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
            return 0;
        }
        if (dt_status != MIMIC_OBJECT_TIME_OK) {
            ERROR_LOG("Invalid disk-instability dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
                      halo->HaloNr, halo->SnapNum, halo->dT,
                      ctx->num_substeps, mimic_object_time_status_str(dt_status));
            return -1;
        }

        mimic_apply_collisional_starburst(gal->UnstableDiskGasFraction, gal,
                                          central_gal, central_halo, 1, disk_dt,
                                          &params);
        DEBUG_LOG("Starburst from disk instability (eff=%.3f)",
                  gal->UnstableDiskGasFraction);
    }

    return 0;
}

int sage_collisional_starburst_cleanup(void)
{
    POST_MERGER_DISK_INSTABILITY_RECHECK_ENABLED = false;
    POST_MERGER_QUASAR_FOLLOWUP_ENABLED = false;
    POST_MERGER_STAR_FORMING_DISK_FACTOR = 0.0;
    POST_MERGER_BLACK_HOLE_GROWTH_RATE = 0.0;
    POST_MERGER_QUASAR_MODE_EFFICIENCY = 0.0;
    INFO_LOG("SAGE collisional starburst module cleaned up");
    return 0;
}
