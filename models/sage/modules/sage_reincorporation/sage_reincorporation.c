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
#include "shared/metallicity.h"
#include "sage/shared/time_parity.h"
#include "module_system/parameter_helpers.h"

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
    double dt_obj = 0.0;
    enum MimicObjectTimeStatus dt_status;
    struct Halo *central_halo;
    struct GalaxyData *gal;

    // Reincorporation follows SAGE: act on the FOF central only.
    if (ctx == NULL || halos == NULL || ngal <= 0 || ctx->central_galaxy == NULL) {
        return 0;
    }

    // Direct single-halo calls on satellites/orphans should remain a no-op.
    if (ngal == 1 && halos[0].Type != 0) {
        return 0;
    }

    central_halo = ctx->central_galaxy;
    if (central_halo->Type != 0) {
        return 0;
    }

    if (central_halo->galaxy == NULL) {
        ERROR_LOG("Central halo has NULL galaxy data");
        return -1;
    }

    gal = central_halo->galaxy;

    // Skip if no ejected gas to reincorporate
    if (gal->EjectedGas <= EPSILON_SMALL) {
        return 0;
    }

    const float Vvir = central_halo->Vvir;
    const float Rvir = central_halo->Rvir;

    // SN velocity 630 km/s → critical velocity = 630/sqrt(2) = 445.48 km/s
    const double Vcrit = 445.48 * REINCORPORATION_FACTOR;

    // Reincorporation only occurs when Vvir > Vcrit
    if (Vvir <= Vcrit) {
        return 0;
    }

    dt_status = mimic_object_substep_dt(central_halo, ctx, &dt_obj);
    if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
        return 0;
    }
    if (dt_status != MIMIC_OBJECT_TIME_OK) {
        ERROR_LOG("Invalid reincorporation dt for halo %d (SnapNum=%d, dT=%.3e, num_substeps=%d, status=%s)",
                  central_halo->HaloNr, central_halo->SnapNum, central_halo->dT,
                  (ctx != NULL) ? ctx->num_substeps : -1,
                  mimic_object_time_status_str(dt_status));
        return -1;
    }

    // Calculate reincorporation rate: (Vvir/Vcrit - 1) * M_eject / (Rvir/Vvir) * dt
    double reincorporated = (Vvir / Vcrit - 1.0) * gal->EjectedGas / (Rvir / Vvir) * dt_obj;

    // Sanity check: reincorporation rate must be positive
    if (reincorporated < 0.0) {
        ERROR_LOG("Negative reincorporation: %.3e (Vvir=%.1f, Rvir=%.3f, EjectedGas=%.3e, dt=%.3e)",
                  reincorporated, Vvir, Rvir, gal->EjectedGas, dt_obj);
        return -1;
    }

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
