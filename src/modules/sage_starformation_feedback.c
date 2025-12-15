/**
 * @file    sage_starformation_feedback.c
 * @brief   SAGE star formation and feedback module
 *
 * Converts cold gas to stars via efficiency-based star formation, with stellar
 * recycling and metal enrichment. Implements supernova feedback that reheats gas
 * to hot halo and ejects gas from the virial radius.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "globals.h" // For access to InputTreeHalos
#include "module_interface.h"
#include "numeric.h"
#include "types.h"
#include "_shared/disk_radius.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double SFR_EFFICIENCY;
static double FEEDBACK_REHEATING_EPSILON;
static double FEEDBACK_EJECTION_EFFICIENCY;
static double ENERGY_SN_CODE;
static double ETA_SN_CODE;
static double RECYCLE_FRACTION;
static double YIELD;
static double FRAC_Z_LEAVE_DISK;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static void update_from_star_formation(struct GalaxyData *gal, const double stars, const double metallicity)
{
    const double RecycleFraction = RECYCLE_FRACTION;

    // Update gas and metals from star formation
    gal->ColdGas -= (1.0 - RecycleFraction) * stars;
    gal->MetalsColdGas -= metallicity * (1.0 - RecycleFraction) * stars;
    gal->StellarMass += (1.0 - RecycleFraction) * stars;
    gal->MetalsStellarMass += metallicity * (1.0 - RecycleFraction) * stars;
}

static void update_from_feedback(struct GalaxyData *gal, struct GalaxyData *central_gal,
                                 const double reheated_mass, double ejected_mass, const double metallicity)
{
    // Remove reheated gas from cold phase
    gal->ColdGas -= reheated_mass;
    gal->MetalsColdGas -= metallicity * reheated_mass;

    // Add reheated gas to hot phase of central galaxy
    central_gal->HotGas += reheated_mass;
    central_gal->MetalsHotGas += metallicity * reheated_mass;

    // Limit ejected mass to available hot gas
    if(ejected_mass > central_gal->HotGas) {
        ejected_mass = central_gal->HotGas;
    }

    // Calculate current hot gas metallicity
    const double metallicity_hot = mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);

    // Remove ejected gas from hot phase
    central_gal->HotGas -= ejected_mass;
    central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;

    // Add ejected gas to ejected reservoir
    central_gal->EjectedGas += ejected_mass;
    central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

    // Update outflow rate
    gal->OutflowRate += reheated_mass;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_starformation_feedback_init(void)
{
    // Load and validate parameters
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("SfrEfficiency", SFR_EFFICIENCY, 0.0, 1.0,
                                      "star formation efficiency");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackReheatingEpsilon", FEEDBACK_REHEATING_EPSILON, 0.0, 100.0,
                                      "reheating efficiency");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FeedbackEjectionEfficiency", FEEDBACK_EJECTION_EFFICIENCY, 0.0, 100.0,
                                      "ejection efficiency");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("EnergySNcode", ENERGY_SN_CODE, 0.0, 100.0,
                                      "supernova energy");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("EtaSNcode", ETA_SN_CODE, 0.0, 10.0,
                                      "mass loading factor");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RecycleFraction", RECYCLE_FRACTION, 0.0, 1.0,
                                      "recycle fraction");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("Yield", YIELD, 0.0, 1.0,
                                      "metal yield");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("FracZleaveDisk", FRAC_Z_LEAVE_DISK, 0.0, 1.0,
                                      "metal outflow fraction");

    VERBOSE_LOG("SAGE Star Formation and Feedback initialized");
    VERBOSE_LOG("  SfrEfficiency = %.4f", SFR_EFFICIENCY);
    VERBOSE_LOG("  FeedbackReheatingEpsilon = %.3f", FEEDBACK_REHEATING_EPSILON);
    VERBOSE_LOG("  FeedbackEjectionEfficiency = %.3f", FEEDBACK_EJECTION_EFFICIENCY);
    VERBOSE_LOG("  EnergySNcode = %.3f", ENERGY_SN_CODE);
    VERBOSE_LOG("  EtaSNcode = %.3f", ETA_SN_CODE);
    VERBOSE_LOG("  RecycleFraction = %.3f", RECYCLE_FRACTION);
    VERBOSE_LOG("  Yield = %.4f", YIELD);
    VERBOSE_LOG("  FracZleaveDisk = %.3f", FRAC_Z_LEAVE_DISK);

    return 0;
}

int sage_starformation_feedback_process(struct ModuleContext *ctx,
                                        struct Halo *halos, int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    // Find central galaxy (Type == 0)
    int central_idx = -1;
    for (int j = 0; j < ngal; j++) {
        if (halos[j].Type == 0) {
            central_idx = j;
            break;
        }
    }

    if (central_idx < 0) {
        DEBUG_LOG("No central galaxy found in FOF group, skipping %d halos", ngal);
        return 0;
    }

    struct Halo *central_halo = &halos[central_idx];
    struct GalaxyData *central_gal = central_halo->galaxy;

    // Process each halo
    for (int i = 0; i < ngal; i++) {
        if (halos[i].galaxy == NULL) {
            ERROR_LOG("Halo %d has NULL galaxy data", i);
            return -1;
        }

        struct GalaxyData *gal = halos[i].galaxy;

        // Validate HaloNr bounds
        if (halos[i].HaloNr < 0 || halos[i].HaloNr >= InputTreeNHalos[TreeID]) {
            ERROR_LOG("Halo %d has invalid HaloNr=%d (valid range: 0-%d)", i,
                     halos[i].HaloNr, InputTreeNHalos[TreeID] - 1);
            return -1;
        }

        // Update disk scale radius
        gal->DiskScaleRadius = mimic_get_disk_radius(
            InputTreeHalos[halos[i].HaloNr].Spin[0],
            InputTreeHalos[halos[i].HaloNr].Spin[1],
            InputTreeHalos[halos[i].HaloNr].Spin[2],
            halos[i].Vvir, halos[i].Rvir);

        const double dt = ctx->substep_dt;

        // Star formation recipe: Kennicutt-Schmidt with critical threshold
        // We take the typical star forming region as 3.0*r_s using the Milky Way as a guide
        const double reff = 3.0 * gal->DiskScaleRadius;
        const double tdyn = reff / halos[i].Vvir;

        // From Kauffmann (1996) eq7 x piR^2, (Vvir in km/s, reff in Mpc/h) in units of 10^10Msun/h
        const double cold_crit = 0.19 * halos[i].Vvir * reff;

        double strdot = 0.0;
        if(gal->ColdGas > cold_crit && tdyn > 0.0) {
            strdot = SFR_EFFICIENCY * (gal->ColdGas - cold_crit) / tdyn;
        }

        double stars = strdot * dt;
        if(stars < 0.0) {
            stars = 0.0;
        }

        const double reheated_mass = FEEDBACK_REHEATING_EPSILON * stars;

        // Can't use more cold gas than is available - balance SF and feedback
        if((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
            const double fac = gal->ColdGas / (stars + reheated_mass);
            stars *= fac;
        }

        // Determine ejection
        double ejected_mass = 0.0;
        if(central_halo->Vvir > 0.0) {
            ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (ETA_SN_CODE * ENERGY_SN_CODE) /
                           (central_halo->Vvir * central_halo->Vvir) - FEEDBACK_REHEATING_EPSILON) * stars;
        }

        if(ejected_mass < 0.0) {
            ejected_mass = 0.0;
        }

        // Update from star formation
        double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
        update_from_star_formation(gal, stars, metallicity);

        // Recompute metallicity after star formation
        metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

        // Update from SN feedback
        update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity);

        // Formation of new metals - instantaneous recycling approximation
        if(gal->ColdGas > 1.0e-8) {
            const double FracZleaveDiskVal = FRAC_Z_LEAVE_DISK * exp(-1.0 * central_halo->Mvir / 30.0);  // Krumholz & Dekel 2011 Eq. 22
            gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
            central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
        } else {
            central_gal->MetalsHotGas += YIELD * stars;
        }

        DEBUG_LOG("Halo %d (Type=%d): SFR=%.3e, Reheat=%.3e, Eject=%.3e, ColdGas=%.3e, StellarMass=%.3e",
                 i, halos[i].Type, stars / dt, reheated_mass, ejected_mass, gal->ColdGas, gal->StellarMass);
    }

    return 0;
}

int sage_starformation_feedback_cleanup(void)
{
    INFO_LOG("SAGE Star Formation and Feedback cleaned up");
    return 0;
}
