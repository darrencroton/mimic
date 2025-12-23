/**
 * @file    sage_collisional_starburst.c
 * @brief   Collisional starburst from mergers or disk instability
 *
 * Implements starburst recipe similar to Somerville et al. (2001).
 * The efficiency coefficients are taken from TJ Cox's PhD thesis.
 *
 * Triggered by:
 *   - Disk instability: UnstableDiskGasFraction > 0 (mode=1, eburst = efficiency)
 *   - Mergers: IsMerging = 1 (mode=0, eburst = 0.56 * mass_ratio^0.7)
 *
 * Physics: Stars form in burst, feedback reheats cold gas to hot, ejects hot gas.
 * Newly formed stars contribute to bulge mass (starbursts form spheroids).
 *
 * References:
 *   - SAGE model_mergers.c: collisional_starburst_recipe() lines 203-293
 *   - Somerville et al. (2001): Starburst efficiency scaling
 *   - Cox PhD thesis: Starburst efficiency coefficients
 *   - Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;
static double THRESHOLD_MAJOR_MERGER;

// Calculated from physical constants (converted to code units)
static double EnergySNcode;
static double EtaSNcode;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static void collisional_starburst_recipe(const double efficiency_factor,
                                         struct GalaxyData *gal,
                                         struct GalaxyData *central_gal,
                                         const struct ModuleContext *ctx,
                                         const int mode);

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_collisional_starburst_init(void)
{
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
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER,
                                      0.0, 1.0, "major merger threshold");

    // Convert physical constants to code units (same as sage_calculate_supernova_feedback)
    EnergySNcode = ENERGY_SN / UnitEnergy_in_cgs * MimicConfig.Hubble_h;
    EtaSNcode = ETA_SN * (UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

    INFO_LOG("SAGE collisional starburst module initialized");
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);
    VERBOSE_LOG("  Yield = %.4f", YIELD);
    VERBOSE_LOG("  EnergySNcode = %.6e (from ENERGY_SN physical constant)", EnergySNcode);
    VERBOSE_LOG("  EtaSNcode = %.6e (from ETA_SN physical constant)", EtaSNcode);

    return 0;
}

int sage_collisional_starburst_process(struct ModuleContext *ctx,
                                       struct Halo *halos,
                                       int ngal)
{
    // Verify process_by_galaxy mode
    if (ngal != 1) {
        ERROR_LOG("sage_collisional_starburst expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    // Get central galaxy for feedback destination
    struct GalaxyData *central_gal = (halo->Type == 0) ?
                                     gal : ctx->central_galaxy->galaxy;

    if (gal == NULL || central_gal == NULL) {
        return 0;
    }

    // Process disk instability trigger (if present)
    if (gal->UnstableDiskGasFraction > 0.0) {
        DEBUG_LOG("Starburst from disk instability (eff=%.3f)", gal->UnstableDiskGasFraction);
        collisional_starburst_recipe(gal->UnstableDiskGasFraction, gal, central_gal, ctx, 1);
    }

    // Process merger trigger (if present)
    if (gal->IsMerging && gal->MergerMassRatio > 0.0) {
        DEBUG_LOG("Starburst from merger (ratio=%.3f)", gal->MergerMassRatio);
        collisional_starburst_recipe(gal->MergerMassRatio, gal, central_gal, ctx, 0);
    }

    // Clear triggers after processing
    gal->UnstableDiskGasFraction = 0.0;
    gal->IsMerging = 0;
    gal->MergerMassRatio = 0.0;

    return 0;
}

/**
 * Collisional starburst recipe (Somerville et al. 2001, Cox PhD thesis coefficients)
 *
 * @param efficiency_factor Disk gas fraction (mode=1) or merger mass ratio (mode=0)
 * @param gal               Galaxy experiencing starburst
 * @param central_gal       Central galaxy (feedback destination)
 * @param ctx               Module context
 * @param mode              Trigger mode: 1=disk instability, 0=merger
 */
static void collisional_starburst_recipe(const double efficiency_factor,
                                         struct GalaxyData *gal,
                                         struct GalaxyData *central_gal,
                                         const struct ModuleContext *ctx,
                                         const int mode)
{
    // Get central halo for property access
    const struct Halo *central_halo = ctx->central_galaxy;

    // Calculate bursting fraction
    double eburst;
    if (mode == 1) {
        // Disk instability: use efficiency directly
        eburst = efficiency_factor;
    } else {
        // Merger: Somerville et al. (2001) with Cox thesis coefficients
        // 0.56 and 0.7 from TJ Cox PhD thesis (more accurate than previous values)
        eburst = 0.56 * pow(efficiency_factor, 0.7);
    }

    double stars = eburst * gal->ColdGas;
    if (stars < 0.0) {
        stars = 0.0;
    }

    // This bursting results in SN feedback on the cold/hot gas
    double reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;

    // Can't use more cold gas than is available! Balance SF and feedback
    if ((stars + reheated_mass) > gal->ColdGas) {
        const double fac = gal->ColdGas / (stars + reheated_mass);
        stars *= fac;
        reheated_mass *= fac;
    }

    // Determine ejection
    double ejected_mass;
    const double vvir_central = ctx->central_galaxy->Vvir;
    if (vvir_central > 0.0) {
        ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (EtaSNcode * EnergySNcode) /
                       (vvir_central * vvir_central) - FEEDBACK_REHEATING_EPSILON) * stars;
    } else {
        ejected_mass = 0.0;
    }

    if (ejected_mass < 0.0) {
        ejected_mass = 0.0;
    }

    // Update from star formation
    const double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    gal->ColdGas -= (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsColdGas -= metallicity * (1.0 - RECYCLE_FRACTION) * stars;
    gal->StellarMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsStellarMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    // Starbursts add to the bulge (starbursts form spheroids)
    gal->BulgeMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsBulgeMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    // Accumulate star formation rate (convert burst to rate by dividing by timestep)
    if (central_halo->dT > 0.0) {
        gal->StarFormationRate += stars / central_halo->dT;
    }

    // Recompute the metallicity of the cold phase
    const double metallicity_cold = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    // Update from feedback
    gal->ColdGas -= reheated_mass;
    gal->MetalsColdGas -= metallicity_cold * reheated_mass;

    central_gal->HotGas += reheated_mass;
    central_gal->MetalsHotGas += metallicity_cold * reheated_mass;

    if (ejected_mass > central_gal->HotGas) {
        ejected_mass = central_gal->HotGas;
    }

    const double metallicity_hot = mimic_get_metallicity(central_gal->HotGas,
                                                         central_gal->MetalsHotGas);

    central_gal->HotGas -= ejected_mass;
    central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;
    central_gal->EjectedGas += ejected_mass;
    central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

    // Accumulate outflow rate (convert mass to rate by dividing by timestep)
    if (central_halo->dT > 0.0) {
        gal->SupernovaOutflowRate += reheated_mass / central_halo->dT;
    }

    // Formation of new metals - instantaneous recycling approximation - only SNII
    if (gal->ColdGas > EPSILON_SMALL && efficiency_factor < THRESHOLD_MAJOR_MERGER) {
        // Minor merger or disk instability: metals distributed between cold and hot
        // Metal ejection scale from Krumholz & Dekel (2011) Eq. 22
        const double FracZleaveDiskVal = FRAC_Z_LEAVE_DISK * exp(-1.0 * central_halo->Mvir / 30.0);  // 30.0 in units of 1e10 Msun/h
        gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
        central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
    } else {
        // Major merger: all metals to hot phase
        central_gal->MetalsHotGas += YIELD * stars;
    }

    DEBUG_LOG("Starburst: formed %.3e Msun stars, reheated %.3e, ejected %.3e",
              stars, reheated_mass, ejected_mass);
}

int sage_collisional_starburst_cleanup(void)
{
    INFO_LOG("SAGE collisional starburst module cleaned up");
    return 0;
}
