/**
 * @file    sage_merge_galaxies.c
 * @brief   Combine galaxies and transform morphology during mergers
 *
 * Combines functionality of SAGE's add_galaxies_together and make_bulge_from_burst.
 * Transfers all baryonic mass from satellite to central and handles morphological
 * transformation for major mergers (violent relaxation).
 *
 * References:
 *   - SAGE: model_mergers.c lines 152-199 (add_galaxies_together, make_bulge_from_burst)
 */

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

static int find_central(struct Halo *halos, int ngal)
{
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0) return i;
    }
    return -1;
}

/**
 * @brief   Combine galaxies and handle morphological transformation
 *
 * Part 1: Transfer all baryonic components (SAGE add_galaxies_together lines 152-180)
 * Part 2: Morphological transformation for major mergers (SAGE make_bulge_from_burst lines 184-199)
 */
int sage_merge_galaxies_process(struct ModuleContext *ctx,
                                 struct Halo *halos,
                                 int ngal)
{
    if (halos == NULL || ngal <= 0) return 0;

    int central_idx = find_central(halos, ngal);
    if (central_idx < 0) return 0;

    struct GalaxyData *central = halos[central_idx].galaxy;
    if (central == NULL) return 0;

    for (int i = 0; i < ngal; i++) {
        if (!halos[i].galaxy || !halos[i].galaxy->IsMerging) continue;

        struct GalaxyData *satellite = halos[i].galaxy;
        double mass_ratio = satellite->MergerMassRatio;

        /* PART 1: Transfer all baryonic components (SAGE lines 154-173) */
        central->ColdGas += satellite->ColdGas;
        central->MetalsColdGas += satellite->MetalsColdGas;

        central->HotGas += satellite->HotGas;
        central->MetalsHotGas += satellite->MetalsHotGas;

        central->EjectedGas += satellite->EjectedGas;
        central->MetalsEjectedGas += satellite->MetalsEjectedGas;

        central->StellarMass += satellite->StellarMass;
        central->MetalsStellarMass += satellite->MetalsStellarMass;

        central->ICS += satellite->ICS;
        central->MetalsICS += satellite->MetalsICS;

        central->BlackHoleMass += satellite->BlackHoleMass;

        /* Add satellite stars to central bulge (SAGE line 172-173) */
        central->BulgeMass += satellite->StellarMass;
        central->MetalsBulgeMass += satellite->MetalsStellarMass;

        /* PART 2: Morphological transformation for major mergers (SAGE lines 186-188) */
        if (mass_ratio > THRESHOLD_MAJOR_MERGER) {
            /* Major merger: violent relaxation destroys disk */
            central->BulgeMass = central->StellarMass;
            central->MetalsBulgeMass = central->MetalsStellarMass;
            central->TimeOfLastMajorMerger = ctx->time;

            DEBUG_LOG("Major merger: ratio=%.3f, transformed to spheroid", mass_ratio);
        }

        /* Update minor merger timing (SAGE lines 82-84) */
        if (mass_ratio > 0.1) {
            central->TimeOfLastMinorMerger = ctx->time;
        }

        /* Mark satellite as merged (Type 3 for internal tracking) */
        halos[i].Type = 3;

        DEBUG_LOG("Merged satellite %d into central (ratio=%.3f)",
                  halos[i].HaloNr, mass_ratio);
    }

    return 0;
}
