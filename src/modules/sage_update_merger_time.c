/**
 * @file    sage_update_merger_time.c
 * @brief   SAGE merger time evolution - trigger merger and disruption events
 *
 * Decrements merger timescales and detects when satellites merge or disrupt.
 * Uses halo-to-baryonic mass ratio to determine eligibility: satellites with
 * Mvir/Mbaryons <= ThresholdSatDisruption either merge (MergTime <= 0) or
 * disrupt to ICS (MergTime > 0, too stripped to survive until merger).
 *
 * Reference: SAGE core_build_model.c lines 366-406
 */

#include <math.h>

#include "_system/parameter_helpers.h"
#include "error.h"
#include "module_interface.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double THRESHOLD_SAT_DISRUPTION;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Calculate baryonic mass ratio (mi/ma where mi <= ma) for merger efficiency.
static double calculate_mass_ratio(const struct GalaxyData *sat, const struct GalaxyData *cen)
{
    const double sat_mass = sat->StellarMass + sat->ColdGas;
    const double cen_mass = cen->StellarMass + cen->ColdGas;

    const double mi = (sat_mass < cen_mass) ? sat_mass : cen_mass;
    const double ma = (sat_mass < cen_mass) ? cen_mass : sat_mass;

    return (ma > 0.0) ? mi / ma : 0.0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_update_merger_time_init(void)
{
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ThresholdSatDisruption", THRESHOLD_SAT_DISRUPTION,
                                       0.0, 1000.0, "halo-to-baryonic mass ratio threshold");

    INFO_LOG("SAGE merger time evolution initialized");
    VERBOSE_LOG("  ThresholdSatDisruption = %.3f", THRESHOLD_SAT_DISRUPTION);
    VERBOSE_LOG("  Physics: Satellites merge if MergTime <= 0 AND Mvir/Mbaryons <= threshold");
    VERBOSE_LOG("           Satellites disrupt if MergTime > 0 AND Mvir/Mbaryons <= threshold");

    return 0;
}

// Decrement merger timescales and trigger merger/disruption events.
// Matches SAGE core_build_model.c lines 366-406.
int sage_update_merger_time_process(struct ModuleContext *ctx,
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

    if (central_idx == -1) {
        return 0;  // No central in this FOF group
    }

    if (halos[central_idx].galaxy == NULL) {
        ERROR_LOG("Central galaxy has NULL galaxy data");
        return -1;
    }

    const struct GalaxyData *central_gal = halos[central_idx].galaxy;
    const double dt = ctx->substep_dt;

    // Process each satellite (Type 1 or Type 2)
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0 || halos[i].Type > 2) continue;
        if (halos[i].galaxy == NULL) continue;

        struct GalaxyData *sat = halos[i].galaxy;

        // Validate MergTime has been set (should be < 999.0 for satellites)
        if (sat->MergTime >= 999.0) {
            ERROR_LOG("Satellite %d has unset MergTime (%.1f)", halos[i].HaloNr, sat->MergTime);
            return -1;
        }

        // Decrement merger time (SAGE line 377)
        sat->MergTime -= dt;

        // Calculate current Mvir with substep interpolation (SAGE line 381)
        // Linearly interpolate between snapshots based on substep progress
        const double fraction = ((double)ctx->substep_number + 1.0) / (double)ctx->num_substeps;
        const double currentMvir = halos[i].Mvir + halos[i].deltaMvir * fraction;

        // Calculate total baryonic mass (SAGE line 382)
        const double galaxyBaryons = sat->StellarMass + sat->ColdGas;

        // Check if satellite is eligible for merger/disruption (SAGE line 383)
        // Condition: Zero baryonic mass OR halo-to-baryonic ratio exceeds threshold
        const int eligible = (galaxyBaryons == 0.0) ||
                            (galaxyBaryons > 0.0 && (currentMvir / galaxyBaryons <= THRESHOLD_SAT_DISRUPTION));

        if (!eligible) continue;

        // Satellite is eligible - check if disruption or merger occurs
        if (!isfinite(sat->MergTime)) {
            WARNING_LOG("Satellite %d has non-finite MergTime", halos[i].HaloNr);
            continue;
        }

        // SAGE lines 394-401: Disruption if MergTime > 0, merger if MergTime <= 0
        if (sat->MergTime > 0.0) {
            // Disruption: Satellite too stripped to survive until merger (SAGE line 396)
            sat->IsDisrupting = 1;
            DEBUG_LOG("Satellite %d disrupting (MergTime=%.3f, Mvir/Mbary=%.1f)",
                      halos[i].HaloNr, sat->MergTime, currentMvir / galaxyBaryons);
        } else {
            // Merger: Orbital decay complete (SAGE lines 398-400)
            sat->IsMerging = 1;
            sat->MergerMassRatio = calculate_mass_ratio(sat, central_gal);
            DEBUG_LOG("Satellite %d merging (ratio=%.3f, Mvir/Mbary=%.1f)",
                      halos[i].HaloNr, sat->MergerMassRatio, currentMvir / galaxyBaryons);
        }
    }

    return 0;
}

int sage_update_merger_time_cleanup(void)
{
    return 0;
}
