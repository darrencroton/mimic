/**
 * @file    sage_merge_galaxies.c
 * @brief   Galaxy merger execution and morphological transformation
 *
 * Implements SAGE merger physics:
 *   - add_galaxies_together: Transfer all baryonic components
 *   - make_bulge_from_burst: Morphological transformation for major mergers
 *   - emit merger events for per-event quasar/starburst consumers
 */

#include "error.h"
#include "module_interface.h"
#include "types.h"
#include "_shared/sage_events.h"
#include "_shared/central_link.h"
#include "_shared/time_parity.h"
#include "_system/parameter_helpers.h"

static double THRESHOLD_MAJOR_MERGER;

// SAGE mass-ratio convention: mi/ma, with fallback to 1.0 when both are zero.
static double calculate_mass_ratio(const struct GalaxyData *sat,
                                   const struct GalaxyData *cen)
{
    const double sat_mass = sat->StellarMass + sat->ColdGas;
    const double cen_mass = cen->StellarMass + cen->ColdGas;
    const double mi = (sat_mass < cen_mass) ? sat_mass : cen_mass;
    const double ma = (sat_mass < cen_mass) ? cen_mass : sat_mass;

    return (ma > 0.0) ? (mi / ma) : 1.0;
}

int sage_merge_galaxies_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER,
                                       0.0, 1.0, "major merger mass ratio threshold");

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

    // Find FOF central (Type 0), used as fallback target for non-Type2 satellites.
    const int central_idx = mimic_find_fof_central_index(halos, ngal);
    if (central_idx < 0 || halos[central_idx].galaxy == NULL) {
        return 0;
    }

    // Process all merging satellites
    for (int i = 0; i < ngal; i++) {
        double source_dt = 0.0;
        double source_time = 0.0;
        enum MimicObjectTimeStatus dt_status;
        enum MimicObjectTimeStatus time_status;

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

        dt_status = mimic_object_substep_dt(&halos[i], ctx, &source_dt);
        time_status = mimic_object_substep_time(&halos[i], ctx, &source_time);
        if (dt_status != MIMIC_OBJECT_TIME_OK || time_status != MIMIC_OBJECT_TIME_OK) {
            ERROR_LOG("Invalid merger event timing for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, dt_status=%s, time_status=%s)",
                      halos[i].HaloNr, halos[i].SnapNum, halos[i].dT,
                      (ctx != NULL) ? ctx->num_substeps : -1,
                      mimic_object_time_status_str(dt_status),
                      mimic_object_time_status_str(time_status));
            return -1;
        }

        const int target_idx =
            mimic_resolve_type2_target_index(halos, ngal, i, central_idx);

        if (target_idx < 0 || target_idx >= ngal || target_idx == i ||
            halos[target_idx].galaxy == NULL) {
            ERROR_LOG("Invalid merger target (satellite=%d, target=%d)", i, target_idx);
            return -1;
        }

        struct GalaxyData *central = halos[target_idx].galaxy;
        const double mass_ratio = calculate_mass_ratio(satellite, central);

        // Keep this property synchronized with the final execution target.
        satellite->MergerMassRatio = mass_ratio;

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
        // PART 2: Emit merger event for per-event consumers
        // =====================================================================
        if (mass_ratio > 0.0) {
            if (module_emit_event(ctx, SAGE_EVENT_MERGER, i, target_idx,
                                  mass_ratio, source_dt) != 0) {
                ERROR_LOG("Failed to emit merger event (source=%d, target=%d, "
                          "ratio=%.6f)",
                          i, target_idx, mass_ratio);
                return -1;
            }
        }

        // =====================================================================
        // PART 3: Merger timing and major-merger morphology
        // =====================================================================

        // Track minor merger timing
        if (mass_ratio > 0.1) {  // 0.1 = threshold for significant merger
            central->TimeOfLastMinorMerger = source_time;
        }

        if (mass_ratio > THRESHOLD_MAJOR_MERGER) {
            // Major merger: transform entire stellar mass to bulge
            central->BulgeMass = central->StellarMass;
            central->MetalsBulgeMass = central->MetalsStellarMass;
            central->TimeOfLastMajorMerger = source_time;

            DEBUG_LOG("Major merger: ratio=%.3f, transformed to spheroid", mass_ratio);
        }

        satellite->IsMerging = 0;
        satellite->MergerMassRatio = 0.0;

        // Mark satellite as merged (Type 3 for internal tracking)
        halos[i].Type = 3;

        DEBUG_LOG("Merged satellite %lld into target %d (ratio=%.3f)",
                  halos[i].HaloNr, target_idx, mass_ratio);
    }

    return 0;
}
