/**
 * @file    sage_merge_galaxies.c
 * @brief   Galaxy merger execution and morphological transformation
 *
 * Implements SAGE merger physics:
 *   - add_galaxies_together: Transfer all baryonic components
 *   - make_bulge_from_burst: Morphological transformation for major mergers
 */

#include <assert.h>
#include "constants.h"
#include "error.h"
#include "../modules/_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"

static double THRESHOLD_MAJOR_MERGER;

int sage_merge_galaxies_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER,
                                       0.0, 1.0, "major merger mass ratio threshold");
    VERBOSE_LOG("SAGE Merge Galaxies initialized");
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

    // Process all merging satellites
    for (int i = 0; i < ngal; i++) {
        if (!halos[i].galaxy || !halos[i].galaxy->IsMerging || halos[i].Type > 2) {
            continue;
        }

        // Cantrals shouldn't be marked as merging!
        assert(halos[i].Type != 0);

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
        // PART 2: Major merger morphological transformation
        // Major mergers destroy disk structure through violent relaxation
        // =====================================================================

        if (mass_ratio > THRESHOLD_MAJOR_MERGER) {
            // Major merger: transform entire stellar mass to bulge
            central->BulgeMass = central->StellarMass;
            central->MetalsBulgeMass = central->MetalsStellarMass;
            central->TimeOfLastMajorMerger = ctx->time;

            DEBUG_LOG("Major merger: ratio=%.3f, transformed to spheroid", mass_ratio);
        }

        // Track minor merger timing
        if (mass_ratio > 0.1) {  // 0.1 = threshold for significant merger
            central->TimeOfLastMinorMerger = ctx->time;
        }

        // Mark satellite as merged (Type 3 for internal tracking)
        halos[i].Type = 3;

        DEBUG_LOG("Merged satellite %d into central (ratio=%.3f)",
                  halos[i].HaloNr, mass_ratio);
    }

    return 0;
}
