/**
 * @file    sage_grow_black_hole.c
 * @brief   Black hole growth from disk instability OR mergers
 *
 * Implements Kauffmann & Haehnelt (2000) BH growth model.
 * Triggers: UnstableDiskGasFraction > 0 (disk instability) OR IsMerging (mergers)
 * Same physics, different efficiency factors.
 *
 * References:
 *   - SAGE: model_mergers.c lines 98-120 (grow_black_hole)
 *   - model_disk_instability.c lines 67-68
 *   - Kauffmann & Haehnelt (2000)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "../modules/_shared/metallicity.h"
#include "../modules/_system/parameter_helpers.h"
#include "../modules/_system/physical_constants.h"
#include "module_interface.h"
#include "types.h"

static int AGN_RECIPE_ON;
static double BLACK_HOLE_GROWTH_RATE;
static double BH_GROWTH_VEL_THRESHOLD;  /* 280 km/s in SAGE */

int sage_grow_black_hole_init(void)
{
    LOAD_AND_VALIDATE_OPTION("AGNrecipeOn", AGN_RECIPE_ON, 1, "0=disabled, 1=enabled");
    if (AGN_RECIPE_ON) {
        LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", BLACK_HOLE_GROWTH_RATE,
                                           0.0, 1.0, "BH growth rate");
        BH_GROWTH_VEL_THRESHOLD = 280.0;  /* km/s, from SAGE */
        VERBOSE_LOG("SAGE Grow Black Hole initialized");
        VERBOSE_LOG("  BlackHoleGrowthRate = %.4f", BLACK_HOLE_GROWTH_RATE);
    } else {
        VERBOSE_LOG("SAGE Grow Black Hole initialized but DISABLED");
    }
    return 0;
}

int sage_grow_black_hole_cleanup(void)
{
    return 0;
}

/**
 * @brief   Grow black hole from disk instability OR mergers
 *
 * Module is independent - checks properties set by previous modules:
 * - UnstableDiskGasFraction (from sage_check_disk_instability)
 * - IsMerging + MergerMassRatio (from sage_update_merger_time / sage_merge_galaxies)
 *
 * SAGE reference: lines 102-119
 */
int sage_grow_black_hole_process(struct ModuleContext *ctx,
                                  struct Halo *halos,
                                  int ngal)
{
    (void)ctx;  /* Unused */

    if (!AGN_RECIPE_ON) return 0;

    if (ngal != 1) {
        ERROR_LOG("sage_grow_black_hole expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) return 0;
    if (gal->ColdGas <= 0.0) return 0;

    /* Check trigger conditions - module is independent, just checks properties */
    double efficiency_factor = 0.0;

    if (gal->UnstableDiskGasFraction > 0.0) {
        /* Disk instability triggered BH growth */
        efficiency_factor = gal->UnstableDiskGasFraction;
        DEBUG_LOG("BH growth from disk instability (eff=%.3f)", efficiency_factor);
    }
    else if (gal->IsMerging && gal->MergerMassRatio > 0.0) {
        /* Merger triggered BH growth */
        efficiency_factor = gal->MergerMassRatio;
        DEBUG_LOG("BH growth from merger (ratio=%.3f)", efficiency_factor);
    }
    else {
        return 0;  /* No trigger */
    }

    /* Execute BH growth physics (Kauffmann & Haehnelt 2000) (SAGE lines 103-104) */
    double BHaccrete = BLACK_HOLE_GROWTH_RATE * efficiency_factor /
                       (1.0 + pow(BH_GROWTH_VEL_THRESHOLD / halo->Vvir, 2.0)) *
                       gal->ColdGas;

    /* Limit accretion to available cold gas (SAGE lines 107-109) */
    if (BHaccrete > gal->ColdGas) {
        BHaccrete = gal->ColdGas;
    }

    /* Calculate metallicity of accreted gas (SAGE line 111) */
    float metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    /* Update galaxy properties (SAGE lines 112-116) */
    gal->BlackHoleMass += BHaccrete;
    gal->ColdGas -= BHaccrete;
    gal->MetalsColdGas -= metallicity * BHaccrete;
    gal->QuasarModeBHaccretionMass += BHaccrete;

    DEBUG_LOG("BH accreted %.3e Msun, new BH mass=%.3e",
              BHaccrete, gal->BlackHoleMass);

    return 0;
}
