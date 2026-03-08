/**
 * @file    sage_update_merger_time.c
 * @brief   SAGE merger time evolution - trigger merger and disruption events
 *
 * Decrements merger timescales and detects when satellites merge or disrupt.
 * Uses halo-to-baryonic mass ratio to determine eligibility: satellites with
 * Mvir/Mbaryons <= ThresholdSatDisruption either merge (MergTime <= 0) or
 * disrupt to ICS (MergTime > 0, too stripped to survive until merger).
 */

#include <math.h>

#include "_system/parameter_helpers.h"
#include "error.h"
#include "module_interface.h"
#include "types.h"
#include "_shared/central_link.h"
#include "_shared/time_parity.h"

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

    return (ma > 0.0) ? mi / ma : 1.0;
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
int sage_update_merger_time_process(struct ModuleContext *ctx,
                                     struct Halo *halos,
                                     int ngal)
{
    if (ctx == NULL || ctx->num_substeps <= 0) {
        ERROR_LOG("Invalid merger-time context (ctx=%p, num_substeps=%d)",
                  (void *)ctx, (ctx != NULL) ? ctx->num_substeps : -1);
        return -1;
    }

    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    // Find FOF central (Type 0), used as fallback target for non-Type2 satellites.
    const int central_idx = mimic_find_fof_central_index(halos, ngal);
    if (central_idx == -1) {
        return 0;  // No central in this FOF group
    }

    if (halos[central_idx].galaxy == NULL) {
        ERROR_LOG("Central galaxy has NULL galaxy data");
        return -1;
    }
    // Process each satellite (Type 1 or Type 2)
    for (int i = 0; i < ngal; i++) {
        double dt = 0.0;
        enum MimicObjectTimeStatus dt_status;

        if (halos[i].Type == 0 || halos[i].Type > 2) continue;
        if (halos[i].galaxy == NULL) continue;

        struct GalaxyData *sat = halos[i].galaxy;

        dt_status = mimic_object_substep_dt(&halos[i], ctx, &dt);
        if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
            continue;
        }
        if (dt_status != MIMIC_OBJECT_TIME_OK) {
            ERROR_LOG("Invalid merger-time dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
                      halos[i].HaloNr, halos[i].SnapNum, halos[i].dT,
                      ctx->num_substeps, mimic_object_time_status_str(dt_status));
            return -1;
        }

        // Validate MergTime has been set (should be < 999.0 for satellites)
        if (sat->MergTime >= 999.0) {
            ERROR_LOG("Satellite %d has unset MergTime (%.1f)", halos[i].HaloNr, sat->MergTime);
            return -1;
        }

        // Decrement merger time
        sat->MergTime -= dt;

        // Calculate current Mvir with substep interpolation
        // Linearly interpolate between snapshots based on substep progress
        const double fraction = ((double)ctx->substep_number + 1.0) / (double)ctx->num_substeps;
        double currentMvir = halos[i].Mvir - halos[i].deltaMvir * (1.0 - fraction);
        if (currentMvir < 0.0) currentMvir = 0.0;

        // Calculate total baryonic mass
        const double galaxyBaryons = sat->StellarMass + sat->ColdGas;

        // Check if satellite is eligible for merger/disruption.
        // SAGE parity: eligibility is based on zero baryons OR Mvir/Mbaryons threshold.
        const int eligible = (galaxyBaryons == 0.0) ||
                            (galaxyBaryons > 0.0 && (currentMvir / galaxyBaryons <= THRESHOLD_SAT_DISRUPTION));

        if (!eligible) continue;

        // Satellite is eligible - check if disruption or merger occurs
        if (!isfinite(sat->MergTime)) {
            WARNING_LOG("Satellite %d has non-finite MergTime", halos[i].HaloNr);
            continue;
        }

        // Disruption if MergTime > 0, merger if MergTime <= 0
        if (sat->MergTime > 0.0) {
            // Disruption: Satellite too stripped to survive until merger
            sat->IsDisrupting = 1;
            DEBUG_LOG("Satellite %d disrupting (MergTime=%.3f, Mvir/Mbary=%.1f)",
                      halos[i].HaloNr, sat->MergTime, currentMvir / galaxyBaryons);
        } else {
            // Merger: Orbital decay complete
            const int target_idx =
                mimic_resolve_type2_target_index(halos, ngal, i, central_idx);
            if (target_idx < 0 || target_idx >= ngal || halos[target_idx].galaxy == NULL) {
                ERROR_LOG("Satellite %d has invalid merger target index %d",
                          halos[i].HaloNr, target_idx);
                return -1;
            }

            sat->IsMerging = 1;
            sat->MergerMassRatio = calculate_mass_ratio(sat, halos[target_idx].galaxy);
            DEBUG_LOG("Satellite %d merging into %d (ratio=%.3f, Mvir/Mbary=%.1f)",
                      halos[i].HaloNr, target_idx, sat->MergerMassRatio,
                      currentMvir / galaxyBaryons);
        }
    }

    return 0;
}

int sage_update_merger_time_cleanup(void)
{
    return 0;
}
