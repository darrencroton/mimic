/**
 * @file    sage_apply_star_formation_supernova.c
 * @brief   SAGE SF/SN apply step - commits SF and SN transport fields to galaxy reservoirs (infrastructure)
 *
 * Applies calculated star formation and supernova feedback to galaxy properties.
 * Handles stellar recycling, metal enrichment, gas transfers (cold→hot→ejected),
 * and resets temporary calculation properties (NewStellarMass, SupernovaReheatedMass,
 * SupernovaEjectedMass) to zero after processing.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "sage/shared/metallicity.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_apply_star_formation_supernova_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                      "recycle fraction");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0,
                                      "metal yield");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                      "metal outflow fraction");

    /* Dependency checks: this is an infrastructure apply step that must follow
     * its SF/SN prescription modules. §7 of SAGE-MODULE-REVIEW.md. */

    const bool sf_present  = module_configured_anywhere("sage_calculate_star_formation");
    const bool sn_present  = module_configured_anywhere("sage_calculate_supernova_feedback");

    /* WARNING: apply step configured with no prescriptions — all fields will be zero */
    if (!sf_present && !sn_present) {
        WARNING_LOG("sage_apply_star_formation_supernova: neither "
                    "sage_calculate_star_formation nor sage_calculate_supernova_feedback is "
                    "configured — all SF/SN transport fields will be zero; "
                    "likely a configuration mistake");
    }

    /* ERROR: any SF/SN module must precede this apply step in the same phase */
    if (sf_present &&
        !module_precedes_in_phase("sage_calculate_star_formation",
                                  "sage_apply_star_formation_supernova",
                                  MimicConfig.phase_1, MimicConfig.num_phase_1)) {
        ERROR_LOG("sage_apply_star_formation_supernova requires "
                  "sage_calculate_star_formation to precede it in phase_1 — apply step "
                  "would commit stale values from the previous substep");
        return -1;
    }
    if (sn_present &&
        !module_precedes_in_phase("sage_calculate_supernova_feedback",
                                  "sage_apply_star_formation_supernova",
                                  MimicConfig.phase_1, MimicConfig.num_phase_1)) {
        ERROR_LOG("sage_apply_star_formation_supernova requires "
                  "sage_calculate_supernova_feedback to precede it in phase_1 — apply "
                  "step would commit stale values from the previous substep");
        return -1;
    }

    INFO_LOG("SAGE SF/SN apply step module initialized");
    VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);
    VERBOSE_LOG("  Yield = %.4f", YIELD);
    VERBOSE_LOG("  FracZleaveDisk = %.3f", FRAC_Z_LEAVE_DISK);

    return 0;
}

int sage_apply_star_formation_supernova_process(struct ModuleContext *ctx,
                                                  struct Halo *halos, int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];

    if (halo->galaxy == NULL) {
        return 0;
    }

    struct GalaxyData *gal = halo->galaxy;
    struct GalaxyData *central_gal = ctx->central_galaxy->galaxy;

    // Read calculated values from previous modules
    const double stars = gal->NewStellarMass;
    const double reheated_mass = gal->SupernovaReheatedMass;
    double ejected_mass = gal->SupernovaEjectedMass;

    // Skip if no star formation occurred
    if (stars <= EPSILON_SMALL) {
        // Zero out temporary properties even if no SF
        gal->NewStellarMass = 0.0;
        gal->SupernovaReheatedMass = 0.0;
        gal->SupernovaEjectedMass = 0.0;
        return 0;
    }

    // ========================================================================
    // STAR FORMATION: Update gas and stars with recycling
    // ========================================================================

    double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    // Remove gas consumed by star formation (accounting for recycling)
    gal->ColdGas -= (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsColdGas -= metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    // Add to stellar mass (accounting for recycling)
    gal->StellarMass += (1.0 - RECYCLE_FRACTION) * stars;
    gal->MetalsStellarMass += metallicity * (1.0 - RECYCLE_FRACTION) * stars;

    // Accumulate star formation rate
    if (halo->dT > 0.0) {
        gal->StarFormationRate += stars / halo->dT;
    }

    // ========================================================================
    // SUPERNOVA FEEDBACK: Reheating (cold → hot)
    // ========================================================================

    // Recompute metallicity after star formation
    metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    // Remove reheated gas from this galaxy's cold phase
    gal->ColdGas -= reheated_mass;
    gal->MetalsColdGas -= metallicity * reheated_mass;

    // Add reheated gas to central's hot halo (satellites are stripped)
    central_gal->HotGas += reheated_mass;
    central_gal->MetalsHotGas += metallicity * reheated_mass;

    // ========================================================================
    // SUPERNOVA FEEDBACK: Ejection (hot → ejected)
    // ========================================================================

    // Limit ejected mass to available hot gas in central
    if(ejected_mass > central_gal->HotGas) {
        ejected_mass = central_gal->HotGas;
    }

    // Calculate central's hot gas metallicity
    const double metallicity_hot = mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);

    // Remove ejected gas from central's hot phase
    central_gal->HotGas -= ejected_mass;
    central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;

    // Add ejected gas to central's ejected reservoir
    central_gal->EjectedGas += ejected_mass;
    central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

    // Accumulate outflow rate
    if (halo->dT > 0.0) {
        gal->SupernovaOutflowRate += reheated_mass / halo->dT;
    }

    // ========================================================================
    // METAL ENRICHMENT: Instantaneous recycling approximation
    // ========================================================================

    if(gal->ColdGas > EPSILON_SMALL) {
        const double FracZleaveDiskVal = FRAC_Z_LEAVE_DISK * exp(-1.0 * halo->Mvir / 30.0);  // Krumholz & Dekel 2011 Eq. 22 (metal ejection scale = 30.0 in 10^10 Msun/h)
        gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
        central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
    } else {
        central_gal->MetalsHotGas += YIELD * stars;
    }

    // ========================================================================
    // CLEANUP: Zero out temporary calculation properties
    // ========================================================================

    gal->NewStellarMass = 0.0;
    gal->SupernovaReheatedMass = 0.0;
    gal->SupernovaEjectedMass = 0.0;

    DEBUG_LOG("Type=%d: Applied SF=%.3e, Reheat=%.3e, Eject=%.3e → ColdGas=%.3e, StellarMass=%.3e",
             halo->Type, stars, reheated_mass, ejected_mass, gal->ColdGas, gal->StellarMass);

    return 0;
}

int sage_apply_star_formation_supernova_cleanup(void)
{
    INFO_LOG("SAGE apply star formation supernova module cleaned up");
    return 0;
}
