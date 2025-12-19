/**
 * @file    sage_trigger_starburst.c
 * @brief   Merger-induced OR disk-instability-induced starbursts
 *
 * Implements Somerville et al. (2001) starburst recipe with coefficients
 * from TJ Cox PhD thesis. Same physics for both triggers, different efficiency.
 *
 * Triggers:
 * - UnstableDiskGasFraction > 0 (disk instability): eburst = efficiency_factor
 * - IsMerging (mergers): eburst = 0.56 * efficiency_factor^0.7
 *
 * References:
 *   - SAGE: model_mergers.c lines 203-293 (collisional_starburst_recipe)
 *   - model_starformation_and_feedback.c (update functions)
 *   - Somerville et al. (2001) - Starburst efficiency
 *   - Cox PhD thesis - Coefficients
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "../modules/_shared/metallicity.h"
#include "../modules/_system/parameter_helpers.h"
#include "../modules/_system/physical_constants.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"

static int SUPERNOVA_RECIPE_ON;
static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double ETA_SN_CODE;
static double ENERGY_SN_CODE;
static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;
static double METAL_MASS_SCALE;  /* 30.0 * 1e10 Msun/h in SAGE */
static double THRESH_MAJOR_MERGER;

/* Starburst efficiency coefficients (Cox PhD thesis) */
static const double STARBURST_EFFICIENCY_NORM = 0.56;
static const double STARBURST_MASS_RATIO_POWER = 0.7;

int sage_trigger_starburst_init(void)
{
    LOAD_AND_VALIDATE_OPTION("SupernovaRecipeOn", SUPERNOVA_RECIPE_ON, 1,
                              "0=disabled, 1=enabled");
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("FeedbackReheatingEpsilon",
                                       FEEDBACK_REHEATING_EPSILON, 0.0, 10.0,
                                       "feedback reheating epsilon");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency",
                                       FEEDBACK_EJECTION_EFFICIENCY, 0.0, 10.0,
                                       "feedback ejection efficiency");
    LOAD_PARAM_DOUBLE("EtaSNcode", ETA_SN_CODE);
    LOAD_PARAM_DOUBLE("EnergySNcode", ENERGY_SN_CODE);
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION,
                                       0.0, 1.0, "recycle fraction");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0, "metal yield");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK,
                                       0.0, 1.0, "frac Z leave disk");
    METAL_MASS_SCALE = 30.0;  /* 30.0 * 1e10 Msun/h (SAGE line 285) */
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThreshMajorMerger", THRESH_MAJOR_MERGER,
                                       0.0, 1.0, "major merger threshold");

    VERBOSE_LOG("SAGE Trigger Starburst initialized");
    VERBOSE_LOG("  SupernovaRecipeOn = %d", SUPERNOVA_RECIPE_ON);
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    return 0;
}

int sage_trigger_starburst_cleanup(void)
{
    return 0;
}

/**
 * @brief   Starburst triggered by disk instability OR mergers
 *
 * Module is independent - checks properties set by previous modules.
 * Same physics regardless of trigger, different efficiency factors.
 *
 * SAGE reference: collisional_starburst_recipe lines 203-293
 */
int sage_trigger_starburst_process(struct ModuleContext *ctx,
                                    struct Halo *halos,
                                    int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("sage_trigger_starburst expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    /* Get central galaxy for feedback destination */
    struct GalaxyData *central_gal = (halo->Type == 0) ?
                                      gal : ctx->central_galaxy->galaxy;

    if (gal == NULL || central_gal == NULL) return 0;

    /* Check trigger conditions - module is independent, just checks properties */
    double efficiency_factor = 0.0;
    int mode = 0;  /* 0=merger, 1=disk instability */

    if (gal->UnstableDiskGasFraction > 0.0) {
        /* Disk instability triggered starburst */
        efficiency_factor = gal->UnstableDiskGasFraction;
        mode = 1;
        DEBUG_LOG("Starburst from disk instability (eff=%.3f)", efficiency_factor);
    }
    else if (gal->IsMerging && gal->MergerMassRatio > 0.0) {
        /* Merger triggered starburst */
        efficiency_factor = gal->MergerMassRatio;
        mode = 0;
        DEBUG_LOG("Starburst from merger (ratio=%.3f)", efficiency_factor);
    }
    else {
        return 0;  /* No trigger */
    }

    /* Calculate starburst efficiency (SAGE lines 213-217) */
    double eburst;
    if (mode == 1) {
        /* Disk instability mode */
        eburst = efficiency_factor;
    } else {
        /* Merger mode - Somerville et al. 2001, Cox thesis coefficients */
        eburst = STARBURST_EFFICIENCY_NORM * pow(efficiency_factor, STARBURST_MASS_RATIO_POWER);
    }

    /* Calculate stars formed during burst (SAGE lines 219-222) */
    double stars = eburst * gal->ColdGas;
    if (stars < 0.0) stars = 0.0;

    /* Calculate supernova feedback (SAGE lines 225-229) */
    double reheated_mass = 0.0;
    if (SUPERNOVA_RECIPE_ON == 1) {
        reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;
    }

    /* Mass conservation: can't exceed available cold gas (SAGE lines 236-240) */
    if ((stars + reheated_mass) > gal->ColdGas) {
        double fac = safe_div(gal->ColdGas, (stars + reheated_mass), 1.0);
        stars *= fac;
        reheated_mass *= fac;
    }

    /* Calculate gas ejection (energy-driven) (SAGE lines 243-257) */
    double ejected_mass = 0.0;
    if (SUPERNOVA_RECIPE_ON == 1) {
        /* Need central halo Vvir, not galaxy property */
        struct Halo *central_halo = ctx->central_galaxy;
        if (central_halo->Vvir > 0.0) {
            ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY *
                            safe_div(ETA_SN_CODE * ENERGY_SN_CODE,
                                    (central_halo->Vvir * central_halo->Vvir), 0.0) -
                            FEEDBACK_REHEATING_EPSILON) * stars;
        }
        if (ejected_mass < 0.0) ejected_mass = 0.0;
    }

    /* Get current cold gas metallicity (SAGE line 264) */
    float metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    /* Update from star formation - inline (SAGE line 265, update_from_star_formation) */
    gal->ColdGas -= (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsColdGas -= metallicity * (1.0 - RECYCLE_FRACTION) * stars;
    gal->StellarMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsStellarMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    /* Add newly formed stars to bulge (starbursts form spheroids) (SAGE lines 267-268) */
    gal->BulgeMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsBulgeMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    /* Recalculate metallicity after star formation (SAGE line 271) */
    metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    /* Update from feedback - inline (SAGE line 274, update_from_feedback) */
    if (SUPERNOVA_RECIPE_ON == 1) {
        /* Remove reheated gas from cold phase */
        gal->ColdGas -= reheated_mass;
        gal->MetalsColdGas -= metallicity * reheated_mass;

        /* Add to hot phase of central */
        central_gal->HotGas += reheated_mass;
        central_gal->MetalsHotGas += metallicity * reheated_mass;

        /* Eject from hot phase */
        if (ejected_mass > central_gal->HotGas) {
            ejected_mass = central_gal->HotGas;
        }

        float metallicity_hot = mimic_get_metallicity(central_gal->HotGas,
                                                       central_gal->MetalsHotGas);

        central_gal->HotGas -= ejected_mass;
        central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;
        central_gal->EjectedGas += ejected_mass;
        central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

        gal->SupernovaOutflowRate += reheated_mass;
    }

    /* Metal enrichment - instantaneous recycling (SAGE lines 284-292) */
    if (gal->ColdGas > 1.0e-8 && efficiency_factor < THRESH_MAJOR_MERGER) {
        /* Minor mergers/disk instability: metals distributed between cold/hot */
        /* Need central halo Mvir, not galaxy property */
        struct Halo *central_halo = ctx->central_galaxy;
        double FracZleaveDiskVal = FRAC_Z_LEAVE_DISK *
                                   exp(-1.0 * central_halo->Mvir / METAL_MASS_SCALE);

        gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
        central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
    } else {
        /* Major mergers: all metals to hot phase */
        central_gal->MetalsHotGas += YIELD * stars;
    }

    DEBUG_LOG("Starburst: formed %.3e Msun stars, reheated %.3e, ejected %.3e",
              stars, reheated_mass, ejected_mass);

    return 0;
}
