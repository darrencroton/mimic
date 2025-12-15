/**
 * @file    sage_calculate_star_formation.c
 * @brief   SAGE star formation calculation module
 *
 * Calculates star formation via Kennicutt-Schmidt efficiency-based model with
 * critical threshold. Stores result in NewStarsMass property for subsequent
 * processing by feedback and update modules.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h" // For access to InputTreeHalos
#include "module_interface.h"
#include "types.h"
#include "_shared/disk_radius.h"
#include "_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double SFR_EFFICIENCY;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_calculate_star_formation_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("SfrEfficiency", SFR_EFFICIENCY, 0.0, 1.0,
                                      "star formation efficiency");

    INFO_LOG("SAGE calculate star formation module initialized");
    VERBOSE_LOG("  SfrEfficiency = %.4f", SFR_EFFICIENCY);

    return 0;
}

int sage_calculate_star_formation_process(struct ModuleContext *ctx,
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

    // Store in NewStarsMass property for subsequent processing
    gal->NewStarsMass = stars;

    DEBUG_LOG("Type=%d: Calculated SF=%.3e, SFR=%.3e, ColdGas=%.3e, DiskScaleRadius=%.3e",
             halo->Type, stars, stars / dt, gal->ColdGas, gal->DiskScaleRadius);

    return 0;
}

int sage_calculate_star_formation_cleanup(void)
{
    INFO_LOG("SAGE calculate star formation module cleaned up");
    return 0;
}
