/**
 * @file    sage_calculate_supernova_feedback.c
 * @brief   SAGE supernova feedback - computes SN reheating and ejection (swappable SN feedback prescription)
 *
 * Calculates reheated and ejected gas masses from supernova feedback based on
 * NewStellarMass. Applies renormalization to ensure star formation and feedback
 * don't exceed available cold gas. Stores results in SupernovaReheatedMass and
 * SupernovaEjectedMass properties.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

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

    /* Dependency checks — §7 of SAGE-MODULE-REVIEW.md */

    /* ERROR: apply step is required to commit SN transport fields to galaxy reservoirs */
    if (!module_configured_anywhere("sage_apply_star_formation_supernova")) {
        ERROR_LOG("sage_calculate_supernova_feedback requires sage_apply_star_formation_supernova "
                  "in the pipeline — without it, SupernovaReheatedMass and "
                  "SupernovaEjectedMass are computed each substep but never "
                  "committed to galaxy reservoirs (silent output loss)");
        return -1;
    }

    /* ERROR: if both SF and SN are configured, SF must precede SN in the same phase */
    if (module_configured_anywhere("sage_calculate_star_formation") &&
        !module_precedes_in_substep_phase("sage_calculate_star_formation",
                                          PROCESSING_MODE_BY_GALAXY,
                                          "sage_calculate_supernova_feedback",
                                          PROCESSING_MODE_BY_GALAXY)) {
        ERROR_LOG("sage_calculate_supernova_feedback requires sage_calculate_star_formation to "
                  "precede it in the same substep phase — SN reads NewStellarMass written by "
                  "SF; wrong order applies stale values from previous substep");
        return -1;
    }

    // Convert physical constants to code units (module-local conversion)
    EnergySNcode = ENERGY_SN / UnitEnergy_in_cgs * MimicConfig.Hubble_h;
    EtaSNcode = ETA_SN * (UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

    INFO_LOG("SAGE supernova feedback module initialized");
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    VERBOSE_LOG("  EnergySNcode = %.6e (from ENERGY_SN physical constant)", EnergySNcode);
    VERBOSE_LOG("  EtaSNcode = %.6e (from ETA_SN physical constant)", EtaSNcode);

    return 0;
}

int sage_calculate_supernova_feedback_process(struct ModuleContext *ctx,
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

    // Read calculated star formation from previous module
    double stars = gal->NewStellarMass;

    // Calculate initial feedback amounts
    double reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;

    // Renormalization: Can't use more cold gas than is available
    // Balance star formation and feedback
    if((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
        const double fac = gal->ColdGas / (stars + reheated_mass);
        stars *= fac;
        reheated_mass *= fac;

        // Update NewStellarMass with renormalized value
        gal->NewStellarMass = stars;
    }

    // Determine ejection relative to central's potential well
    // Both centrals and satellites eject into the FOF group's halo
    double ejected_mass = 0.0;
    const double central_vvir = ctx->central_galaxy->Vvir;
    if(central_vvir > 0.0) {
        ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (EtaSNcode * EnergySNcode) /
                       (central_vvir * central_vvir) - FEEDBACK_REHEATING_EPSILON) * stars;
    }

    if(ejected_mass < 0.0) {
        ejected_mass = 0.0;
    }

    // Store calculated feedback masses for subsequent processing
    gal->SupernovaReheatedMass = reheated_mass;
    gal->SupernovaEjectedMass = ejected_mass;

    DEBUG_LOG("Type=%d: Stars=%.3e, Reheat=%.3e, Eject=%.3e (CentralVvir=%.2f)",
             halo->Type, stars, reheated_mass, ejected_mass, central_vvir);

    return 0;
}

int sage_calculate_supernova_feedback_cleanup(void)
{
    INFO_LOG("SAGE supernova feedback module cleaned up");
    return 0;
}
