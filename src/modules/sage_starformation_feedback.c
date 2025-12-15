/**
 * @file    sage_starformation_feedback.c
 * @brief   SAGE star formation and feedback module
 *
 * Converts cold gas to stars via efficiency-based star formation, with stellar
 * recycling and metal enrichment. Implements supernova feedback that reheats gas
 * to hot halo and ejects gas from the virial radius. For satellites, reheated gas
 * goes to the satellite's own hot reservoir (transfers to central upon merger).
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
#include "_system/physical_constants.h"

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

    // For centrals (Type 0), central_gal points to self
    // For satellites (Type 1), reheated gas goes to satellite's own HotGas
    // (will transfer to central when satellite merges)
    struct GalaxyData *central_gal = gal;

    // Validate HaloNr bounds
    if (halo->HaloNr < 0 || halo->HaloNr >= InputTreeNHalos[TreeID]) {
        ERROR_LOG("Halo has invalid HaloNr=%d (valid range: 0-%d)",
                 halo->HaloNr, InputTreeNHalos[TreeID] - 1);
        return -1;
    }

    // Update disk scale radius
    gal->DiskScaleRadius = mimic_get_disk_radius(
        InputTreeHalos[halo->HaloNr].Spin[0],
        InputTreeHalos[halo->HaloNr].Spin[1],
        InputTreeHalos[halo->HaloNr].Spin[2],
        halo->Vvir, halo->Rvir);

    const double dt = ctx->substep_dt;

    // Star formation recipe: Kennicutt-Schmidt with critical threshold
    // We take the typical star forming region as 3.0*r_s using the Milky Way as a guide
    const double reff = 3.0 * gal->DiskScaleRadius;
    const double tdyn = reff / halo->Vvir;

    // From Kauffmann (1996) eq7 x piR^2, (Vvir in km/s, reff in Mpc/h) in units of 10^10Msun/h
    const double cold_crit = 0.19 * halo->Vvir * reff;

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

    // Determine ejection (uses galaxy's own Vvir for both centrals and satellites)
    double ejected_mass = 0.0;
    if(halo->Vvir > 0.0) {
        ejected_mass = (FEEDBACK_EJECTION_EFFICIENCY * (ETA_SN_CODE * ENERGY_SN_CODE) /
                       (halo->Vvir * halo->Vvir) - FEEDBACK_REHEATING_EPSILON) * stars;
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
        const double FracZleaveDiskVal = FRAC_Z_LEAVE_DISK * exp(-1.0 * halo->Mvir / 30.0);  // Krumholz & Dekel 2011 Eq. 22 (metal ejection scale = 30.0 in 10^10 Msun/h)
        gal->MetalsColdGas += YIELD * (1.0 - FracZleaveDiskVal) * stars;
        central_gal->MetalsHotGas += YIELD * FracZleaveDiskVal * stars;
    } else {
        central_gal->MetalsHotGas += YIELD * stars;
    }

    DEBUG_LOG("Type=%d: SFR=%.3e, Reheat=%.3e, Eject=%.3e, ColdGas=%.3e, StellarMass=%.3e",
             halo->Type, stars / dt, reheated_mass, ejected_mass, gal->ColdGas, gal->StellarMass);

    return 0;
}

int sage_starformation_feedback_cleanup(void)
{
    INFO_LOG("SAGE Star Formation and Feedback cleaned up");
    return 0;
}
