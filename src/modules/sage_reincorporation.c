/**
 * @file    sage_reincorporation.c
 * @brief   SAGE reincorporation module - returns ejected gas to hot halo in massive systems
 *
 * Massive halos (Vvir > Vcrit) recapture previously ejected gas back to the hot halo,
 * with rate proportional to (Vvir/Vcrit - 1) and inverse dynamical time. Metallicity
 * is preserved during the transfer from ejected to hot reservoir.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double REINCORPORATION_FACTOR;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_reincorporation_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ReIncorporationFactor", REINCORPORATION_FACTOR, 0.0, 10.0,
                                      "reincorporation efficiency factor");

    INFO_LOG("SAGE reincorporation module initialized");
    VERBOSE_LOG("  ReIncorporationFactor = %.3f", REINCORPORATION_FACTOR);
    VERBOSE_LOG("  Critical velocity = %.2f km/s", 445.48 * REINCORPORATION_FACTOR);

    return 0;
}

int sage_reincorporation_process(struct ModuleContext *ctx,
                                  struct Halo *halos,
                                  int ngal)
{
    // Only central galaxies can reincorporate gas
    if (halos == NULL || ngal <= 0 || halos[0].Type != 0) {
        return 0;
    }

    if (halos[0].galaxy == NULL) {
        ERROR_LOG("Central halo has NULL galaxy data");
        return -1;
    }

    struct GalaxyData *gal = halos[0].galaxy;

    // Skip if no ejected gas to reincorporate
    if (gal->EjectedGas <= EPSILON_SMALL) {
        return 0;
    }

    const float Vvir = halos[0].Vvir;
    const float Rvir = halos[0].Rvir;

    // SN velocity 630 km/s → critical velocity = 630/sqrt(2) = 445.48 km/s
    const double Vcrit = 445.48 * REINCORPORATION_FACTOR;

    // Reincorporation only occurs when Vvir > Vcrit
    if (Vvir <= Vcrit) {
        return 0;
    }

    // Calculate reincorporation rate: (Vvir/Vcrit - 1) * M_eject / (Rvir/Vvir) * dt
    double reincorporated = (Vvir / Vcrit - 1.0) * gal->EjectedGas / (Rvir / Vvir) * ctx->substep_dt;

    // Limit to available ejected mass
    if (reincorporated > gal->EjectedGas) {
        reincorporated = gal->EjectedGas;
    }

    // Preserve metallicity during transfer
    const double metallicity = mimic_get_metallicity(gal->EjectedGas, gal->MetalsEjectedGas);

    // Transfer from ejected to hot reservoir
    gal->EjectedGas -= reincorporated;
    gal->MetalsEjectedGas -= metallicity * reincorporated;
    gal->HotGas += reincorporated;
    gal->MetalsHotGas += metallicity * reincorporated;

    return 0;
}

int sage_reincorporation_cleanup(void)
{
    INFO_LOG("SAGE reincorporation module cleaned up");
    return 0;
}
