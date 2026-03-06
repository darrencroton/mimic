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

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "types.h"
#include "_shared/merger_physics.h"
#include "_shared/sage_events.h"
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

// Calculated from physical constants (converted to code units)
static double EnergySNcode;
static double EtaSNcode;

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
        struct GalaxyData *central_gal = NULL;
        const struct ModuleEvent *event = ctx->active_event;

        if (ctx->central_galaxy != NULL) {
            central_gal = ctx->central_galaxy->galaxy;
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

        mimic_apply_collisional_starburst(event->value0, gal, central_gal, ctx, 0,
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
    struct GalaxyData *central_gal = NULL;
    if (halo->Type == 0) {
        central_gal = gal;
    } else if (ctx->central_galaxy != NULL) {
        central_gal = ctx->central_galaxy->galaxy;
    }

    if (gal == NULL || central_gal == NULL) {
        return 0;
    }

    /* Disk-instability channel (by-galaxy path). */
    if (gal->UnstableDiskGasFraction > 0.0) {
        mimic_apply_collisional_starburst(gal->UnstableDiskGasFraction, gal,
                                          central_gal, ctx, 1, &params);
        DEBUG_LOG("Starburst from disk instability (eff=%.3f)",
                  gal->UnstableDiskGasFraction);
    }

    return 0;
}

int sage_collisional_starburst_cleanup(void)
{
    INFO_LOG("SAGE collisional starburst module cleaned up");
    return 0;
}
