/**
 * @file    sage_calculate_supernova_feedback.c
 * @brief   SAGE supernova feedback calculation module
 *
 * Calculates reheated and ejected gas masses from supernova feedback based on
 * NewStarsMass. Applies renormalization to ensure star formation and feedback
 * don't exceed available cold gas. Stores results in SupernovaReheatedMass and
 * SupernovaEjectedMass properties.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "types.h"
#include "globals.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;

// ============================================================================
// MODULE-LOCAL CONSTANTS (converted from physical constants)
// ============================================================================

static double EnergySNcode;
static double EtaSNcode;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_calculate_supernova_feedback_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackReheatingEpsilon", FEEDBACK_REHEATING_EPSILON, 0.0, 100.0,
                                      "reheating efficiency");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency", FEEDBACK_EJECTION_EFFICIENCY, 0.0, 100.0,
                                      "ejection efficiency");

    // Convert physical constants to code units (module-local conversion)
    EnergySNcode = ENERGY_SN / UnitEnergy_in_cgs * MimicConfig.Hubble_h;
    EtaSNcode = ETA_SN * (UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

    INFO_LOG("SAGE calculate supernova feedback module initialized");
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    VERBOSE_LOG("  EnergySNcode = %.6e (from ENERGY_SN physical constant)", EnergySNcode);
    VERBOSE_LOG("  EtaSNcode = %.6e (from ETA_SN physical constant)", EtaSNcode);

    return 0;
}

int sage_calculate_supernova_feedback_process(struct ModuleContext *ctx,
                                               struct Halo *halos, int ngal)
{
    (void)ctx;  // Unused in this module

    if (ngal != 1) {
        ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];

    // Skip orphan galaxies (type 2)
    if (halo->Type == 2 || halo->galaxy == NULL) {
        return 0;
    }

    struct GalaxyData *gal = halo->galaxy;

    // Read calculated star formation from previous module
    double stars = gal->NewStarsMass;

    // Calculate initial feedback amounts
    double reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;

    // Renormalization: Can't use more cold gas than is available
    // Balance star formation and feedback
    if((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
        const double fac = gal->ColdGas / (stars + reheated_mass);
        stars *= fac;
        reheated_mass *= fac;

        // Update NewStarsMass with renormalized value
        gal->NewStarsMass = stars;
    }

    // Determine ejection (uses galaxy's own Vvir for both centrals and satellites)
    double ejected_mass = 0.0;
    if(halo->Vvir > 0.0) {
        ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (EtaSNcode * EnergySNcode) /
                       (halo->Vvir * halo->Vvir) - FEEDBACK_REHEATING_EPSILON) * stars;
    }

    if(ejected_mass < 0.0) {
        ejected_mass = 0.0;
    }

    // Store calculated feedback masses for subsequent processing
    gal->SupernovaReheatedMass = reheated_mass;
    gal->SupernovaEjectedMass = ejected_mass;

    DEBUG_LOG("Type=%d: Stars=%.3e, Reheat=%.3e, Eject=%.3e (Vvir=%.2f)",
             halo->Type, stars, reheated_mass, ejected_mass, halo->Vvir);

    return 0;
}

int sage_calculate_supernova_feedback_cleanup(void)
{
    INFO_LOG("SAGE calculate supernova feedback module cleaned up");
    return 0;
}
