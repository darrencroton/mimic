/**
 * @file sage_quasar_mode.c
 * @brief SAGE quasar-mode AGN feedback (BH growth + energy-driven winds)
 *
 * Implements Kauffmann & Haehnelt (2000) black hole growth coupled with
 * energy-driven gas ejection. Triggered by disk instability in phase_1.
 *
 * References:
 *   - SAGE: model_mergers.c (grow_black_hole, quasar_mode_wind)
 *   - SAGE: model_disk_instability.c (grow_black_hole call)
 *   - Kauffmann & Haehnelt (2000) - BH growth model
 *   - Croton et al. (2006, 2016) - SAGE model papers
 */

#include "constants.h"
#include "error.h"
#include "_shared/merger_physics.h"
#include "_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"

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

    INFO_LOG("SAGE quasar-mode AGN feedback initialized");
    VERBOSE_LOG("  BlackHoleGrowthRate = %.4f", BLACK_HOLE_GROWTH_RATE);
    VERBOSE_LOG("  QuasarModeEfficiency = %.3f", QUASAR_MODE_EFFICIENCY);

    return 0;
}

int sage_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("sage_quasar_mode expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    /* Disk-instability channel only. Merger channel is handled inline in merge module. */
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
