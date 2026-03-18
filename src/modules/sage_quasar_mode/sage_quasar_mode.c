/**
 * @file sage_quasar_mode.c
 * @brief SAGE quasar-mode AGN feedback (BH growth + energy-driven winds)
 *
 * Implements Kauffmann & Haehnelt (2000) black hole growth coupled with
 * energy-driven gas ejection.
 *
 * Trigger channels:
 * - process_by_galaxy: disk instability trigger (phase_1)
 * - process_per_event: merger event trigger (phase_2)
 *
 * References:
 *   - SAGE: model_mergers.c (grow_black_hole, quasar_mode_wind)
 *   - SAGE: model_disk_instability.c (grow_black_hole call)
 *   - Kauffmann & Haehnelt (2000) - BH growth model
 *   - Croton et al. (2006, 2016) - SAGE model papers
 */

#include "constants.h"
#include "error.h"
#include "_shared/sage_agn_physics.h"
#include "_system/generated/event_contracts.h"
#include "_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"
#include "globals.h"
#include "module_registry.h"

// Module parameters
static double BLACK_HOLE_GROWTH_RATE;
static double QUASAR_MODE_EFFICIENCY;

// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

int sage_quasar_mode_init(void)
{
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", BLACK_HOLE_GROWTH_RATE,
                                        0.0, 1.0, "BH growth rate");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", QUASAR_MODE_EFFICIENCY,
                                        0.0, 1.0, "quasar mode efficiency");

    /* Dependency check: process_per_event requires a merger event producer */
    if (module_configured_in_phase("sage_quasar_mode",
                                   MimicConfig.phase_2, MimicConfig.num_phase_2,
                                   PROCESSING_MODE_PER_EVENT) &&
        !module_configured_in_phase("sage_resolve_mergers_and_disruption",
                                    MimicConfig.phase_2, MimicConfig.num_phase_2,
                                    PROCESSING_MODE_FULL_HALO)) {
        ERROR_LOG("sage_quasar_mode (process_per_event) requires "
                  "sage_resolve_mergers_and_disruption in phase_2 as "
                  "process_full_halo — no merger events will be emitted without it");
        return -1;
    }

    INFO_LOG("SAGE quasar-mode AGN feedback initialized");
    VERBOSE_LOG("  BlackHoleGrowthRate = %.4f", BLACK_HOLE_GROWTH_RATE);
    VERBOSE_LOG("  QuasarModeEfficiency = %.3f", QUASAR_MODE_EFFICIENCY);

    return 0;
}

int sage_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (ctx == NULL || halos == NULL || ngal <= 0) {
        return 0;
    }

    if (ctx->active_event != NULL) {
        if (ngal != 1) {
            ERROR_LOG("sage_quasar_mode (process_per_event) expects ngal=1, got %d", ngal);
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
        if (merger_ratio <= 0.0) {
            return 0;
        }

        const double bh_accrete = mimic_apply_black_hole_growth(
            event_halo, merger_ratio, BLACK_HOLE_GROWTH_RATE);
        if (bh_accrete > 0.0) {
            mimic_apply_quasar_mode_wind(event_halo, bh_accrete,
                                         QUASAR_MODE_EFFICIENCY, ctx);
        }

        DEBUG_LOG("Quasar mode from merger event (ratio=%.3f, source=%d, target=%d)",
                  merger_ratio, event->source_index, event->target_index);
        return 0;
    }

    if (ngal != 1) {
        ERROR_LOG("sage_quasar_mode expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    /* Disk-instability channel (by-galaxy path). */
    if (gal->UnstableDiskGasFraction > 0.0) {
        const double BHaccrete = mimic_apply_black_hole_growth(
            halo, gal->UnstableDiskGasFraction, BLACK_HOLE_GROWTH_RATE);
        if (BHaccrete > 0.0) {
            mimic_apply_quasar_mode_wind(halo, BHaccrete, QUASAR_MODE_EFFICIENCY,
                                         ctx);
        }
        DEBUG_LOG("Quasar mode from disk instability (eff=%.3f)",
                  gal->UnstableDiskGasFraction);
    }

    return 0;
}

int sage_quasar_mode_cleanup(void)
{
    VERBOSE_LOG("SAGE quasar-mode AGN feedback cleaned up");
    return 0;
}
