/**
 * @file    sage_collisional_starburst.c
 * @brief   SAGE collisional starburst module - merger or disk-instability induced starbursts
 *
 * Implements Somerville et al. (2001) starburst recipe with coefficients from
 * TJ Cox PhD thesis. Triggered by disk instability OR mergers, with the same
 * physics but different efficiency factors for each trigger.
 *
 * Physics Summary:
 *   Trigger Detection:
 *     - UnstableDiskGasFraction > 0 (disk instability): eburst = efficiency_factor
 *     - IsMerging (mergers): eburst = 0.56 * efficiency_factor^0.7
 *
 *   Starburst Calculation:
 *     stars = eburst * ColdGas
 *     reheated_mass = FeedbackReheatingEpsilon * stars
 *     ejected_mass = (FeedbackEjectionEfficiency * η_SN * E_SN / V_vir^2 - ε) * stars
 *
 *   Mass Updates:
 *     ColdGas → StellarMass (with recycling fraction)
 *     Stars → BulgeMass (starbursts form spheroids)
 *     ColdGas → HotGas (feedback reheating to central)
 *     HotGas → EjectedGas (SN-driven ejection from central)
 *     Add metals via instantaneous recycling (YIELD parameter)
 *
 * Module Communication:
 *   Reads:  UnstableDiskGasFraction, IsMerging, MergerMassRatio, ColdGas,
 *           MetalsColdGas, StellarMass, MetalsStellarMass, BulgeMass, MetalsBulgeMass
 *   Writes: ColdGas, MetalsColdGas, StellarMass, MetalsStellarMass, BulgeMass,
 *           MetalsBulgeMass, SupernovaOutflowRate (and central: HotGas, MetalsHotGas,
 *           EjectedGas, MetalsEjectedGas)
 *
 * Trigger Sources:
 *   - UnstableDiskGasFraction: Set by sage_disk_instability
 *   - IsMerging + MergerMassRatio: Set by sage_update_merger_time / sage_merge_galaxies
 *
 * References:
 *   - SAGE: model_mergers.c lines 203-293 (collisional_starburst_recipe)
 *   - model_starformation_and_feedback.c (update functions)
 *   - Somerville et al. (2001) - Starburst efficiency
 *   - Cox PhD thesis - Starburst efficiency coefficients
 *
 * Vision Principles:
 *   - Module Independence: Checks trigger properties, acts independently
 *   - Multi-Trigger Pattern: Same physics for disk instability and mergers
 *   - KISS: Complex physics, but clearly structured implementation
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static int SUPERNOVA_RECIPE_ON;
static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double ETA_SN_CODE;
static double ENERGY_SN_CODE;
static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;
static double THRESH_MAJOR_MERGER;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

/* Metal mass scale for metal enrichment (30.0 * 1e10 Msun/h) */
static const double METAL_MASS_SCALE = 30.0;

/* Starburst efficiency coefficients (Cox PhD thesis) */
static const double STARBURST_EFFICIENCY_NORM = 0.56;
static const double STARBURST_MASS_RATIO_POWER = 0.7;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_collisional_starburst_init(void)
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
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThreshMajorMerger", THRESH_MAJOR_MERGER,
                                       0.0, 1.0, "major merger threshold");

    INFO_LOG("SAGE collisional starburst module initialized");
    VERBOSE_LOG("  SupernovaRecipeOn = %d", SUPERNOVA_RECIPE_ON);
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    VERBOSE_LOG("  RecycleFraction = %.2f", RECYCLE_FRACTION);
    VERBOSE_LOG("  Yield = %.3f", YIELD);

    return 0;
}

int sage_collisional_starburst_process(struct ModuleContext *ctx,
                                        struct Halo *halos,
                                        int ngal)
{
    /* Verify process_by_galaxy mode */
    if (ngal != 1) {
        ERROR_LOG("sage_collisional_starburst expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    /* Get central galaxy for feedback destination */
    struct GalaxyData *central_gal = (halo->Type == 0) ?
                                      gal : ctx->central_galaxy->galaxy;

    if (gal == NULL || central_gal == NULL) {
        return 0;
    }

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

    /* Calculate starburst efficiency */
    double eburst;
    if (mode == 1) {
        /* Disk instability mode */
        eburst = efficiency_factor;
    } else {
        /* Merger mode - Somerville et al. (2001), Cox thesis coefficients */
        eburst = STARBURST_EFFICIENCY_NORM * pow(efficiency_factor, STARBURST_MASS_RATIO_POWER);
    }

    /* Calculate stars formed during burst */
    double stars = eburst * gal->ColdGas;
    if (stars < 0.0) {
        stars = 0.0;
    }

    /* Calculate supernova feedback */
    double reheated_mass = 0.0;
    if (SUPERNOVA_RECIPE_ON == 1) {
        reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;
    }

    /* Mass conservation: can't exceed available cold gas */
    if ((stars + reheated_mass) > gal->ColdGas) {
        double fac = safe_div(gal->ColdGas, (stars + reheated_mass), 1.0);
        stars *= fac;
        reheated_mass *= fac;
    }

    /* Calculate gas ejection (energy-driven) */
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
        if (ejected_mass < 0.0) {
            ejected_mass = 0.0;
        }
    }

    /* Get current cold gas metallicity */
    float metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    /* Update from star formation - inline */
    gal->ColdGas -= (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsColdGas -= metallicity * (1.0 - RECYCLE_FRACTION) * stars;
    gal->StellarMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsStellarMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    /* Add newly formed stars to bulge (starbursts form spheroids) */
    gal->BulgeMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsBulgeMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    /* Recalculate metallicity after star formation */
    metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    /* Update from feedback - inline */
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

    /* Metal enrichment - instantaneous recycling */
    if (gal->ColdGas > 1.0e-8 && efficiency_factor < THRESH_MAJOR_MERGER) {
        /* Minor mergers/disk instability: metals distributed between cold/hot */
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

int sage_collisional_starburst_cleanup(void)
{
    VERBOSE_LOG("SAGE collisional starburst module cleaned up");
    return 0;
}
