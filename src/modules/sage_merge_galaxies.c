/**
 * @file    sage_merge_galaxies.c
 * @brief   Galaxy merger execution and morphological transformation
 *
 * Implements SAGE merger physics:
 *   - add_galaxies_together: Transfer all baryonic components
 *   - make_bulge_from_burst: Morphological transformation for major mergers
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "_shared/merger_physics.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"
#include "module_interface.h"
#include "types.h"

static double THRESHOLD_MAJOR_MERGER;
static double BLACK_HOLE_GROWTH_RATE;
static double QUASAR_MODE_EFFICIENCY;
static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;
static double ENERGY_SN_CODE;
static double ETA_SN_CODE;

int sage_merge_galaxies_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER,
                                       0.0, 1.0, "major merger mass ratio threshold");
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", BLACK_HOLE_GROWTH_RATE,
                                       0.0, 1.0, "BH growth rate");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", QUASAR_MODE_EFFICIENCY,
                                       0.0, 1.0, "quasar mode efficiency");
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

    ENERGY_SN_CODE = ENERGY_SN / UnitEnergy_in_cgs * MimicConfig.Hubble_h;
    ETA_SN_CODE = ETA_SN * (UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

    INFO_LOG("SAGE Merge Galaxies initialized");
    VERBOSE_LOG("  ThresholdMajorMerger = %.3f", THRESHOLD_MAJOR_MERGER);
    return 0;
}

int sage_merge_galaxies_cleanup(void)
{
    return 0;
}

int sage_merge_galaxies_process(struct ModuleContext *ctx,
                                 struct Halo *halos,
                                 int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    // Find central galaxy (Type 0)
    int central_idx = -1;
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0) {
            central_idx = i;
            break;
        }
    }

    if (central_idx < 0 || halos[central_idx].galaxy == NULL) {
        return 0;
    }

    struct GalaxyData *central = halos[central_idx].galaxy;
    struct MimicStarburstParams starburst_params = {
        .feedback_reheating_epsilon = FEEDBACK_REHEATING_EPSILON,
        .feedback_ejection_efficiency = FEEDBACK_EJECTION_EFFICIENCY,
        .recycle_fraction = RECYCLE_FRACTION,
        .yield = YIELD,
        .frac_z_leave_disk = FRAC_Z_LEAVE_DISK,
        .threshold_major_merger = THRESHOLD_MAJOR_MERGER,
        .energy_sn_code = ENERGY_SN_CODE,
        .eta_sn_code = ETA_SN_CODE,
    };

    // Process all merging satellites
    for (int i = 0; i < ngal; i++) {
        if (!halos[i].galaxy || !halos[i].galaxy->IsMerging || halos[i].Type > 2) {
            continue;
        }

        // Type 0 centrals should never be marked for merging (upstream module bug)
        if (halos[i].Type == 0) {
            ERROR_LOG("Type 0 central (HaloNr=%lld) marked IsMerging=1 - skipping (upstream module bug)",
                      halos[i].HaloNr);
            continue;
        }

        struct GalaxyData *satellite = halos[i].galaxy;
        const double mass_ratio = satellite->MergerMassRatio;

        // =====================================================================
        // PART 1: Add galaxies together
        // Transfer all baryonic components from satellite to central
        // =====================================================================

        central->ColdGas += satellite->ColdGas;
        central->MetalsColdGas += satellite->MetalsColdGas;

        central->StellarMass += satellite->StellarMass;
        central->MetalsStellarMass += satellite->MetalsStellarMass;

        central->HotGas += satellite->HotGas;
        central->MetalsHotGas += satellite->MetalsHotGas;

        central->EjectedGas += satellite->EjectedGas;
        central->MetalsEjectedGas += satellite->MetalsEjectedGas;

        central->ICS += satellite->ICS;
        central->MetalsICS += satellite->MetalsICS;

        central->BlackHoleMass += satellite->BlackHoleMass;

        // Add satellite stars to central bulge (all mergers contribute to bulge)
        central->BulgeMass += satellite->StellarMass;
        central->MetalsBulgeMass += satellite->MetalsStellarMass;

        // =====================================================================
        // PART 2: Inline merger-triggered BH growth + starburst (SAGE parity)
        // =====================================================================
        if (mass_ratio > 0.0) {
            const double bh_accrete = mimic_apply_black_hole_growth(
                &halos[central_idx], mass_ratio, BLACK_HOLE_GROWTH_RATE);
            if (bh_accrete > 0.0) {
                mimic_apply_quasar_mode_wind(&halos[central_idx], bh_accrete,
                                             QUASAR_MODE_EFFICIENCY, ctx);
            }
            mimic_apply_collisional_starburst(mass_ratio, central, central, ctx, 0,
                                              &starburst_params);
        }

        // =====================================================================
        // PART 3: Merger timing and major-merger morphology
        // =====================================================================

        // Track minor merger timing
        if (mass_ratio > 0.1) {  // 0.1 = threshold for significant merger
            central->TimeOfLastMinorMerger = ctx->substep_time;
        }

        if (mass_ratio > THRESHOLD_MAJOR_MERGER) {
            // Major merger: transform entire stellar mass to bulge
            central->BulgeMass = central->StellarMass;
            central->MetalsBulgeMass = central->MetalsStellarMass;
            central->TimeOfLastMajorMerger = ctx->substep_time;

            DEBUG_LOG("Major merger: ratio=%.3f, transformed to spheroid", mass_ratio);
        }

        satellite->IsMerging = 0;
        satellite->MergerMassRatio = 0.0;

        // Mark satellite as merged (Type 3 for internal tracking)
        halos[i].Type = 3;

        DEBUG_LOG("Merged satellite %lld into central (ratio=%.3f)",
                  halos[i].HaloNr, mass_ratio);
    }

    return 0;
}
