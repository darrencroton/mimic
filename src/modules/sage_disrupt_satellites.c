/**
 * @file    sage_disrupt_satellites.c
 * @brief   Tidal disruption of satellites to intracluster stars
 *
 * Handles complete tidal stripping of severely disrupted satellites.
 * Gas is heated and added to central hot halo. Stars are added to ICS.
 */

#include "error.h"
#include "module_interface.h"
#include "types.h"

int sage_disrupt_satellites_init(void)
{
    INFO_LOG("SAGE Disrupt Satellites initialized");
    return 0;
}

int sage_disrupt_satellites_cleanup(void)
{
    return 0;
}

// Disrupt satellites to intracluster stars
int sage_disrupt_satellites_process(struct ModuleContext *ctx,
                                     struct Halo *halos,
                                     int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    // Access FOF central galaxy (always non-NULL, guaranteed by core)
    struct GalaxyData *central = ctx->central_galaxy->galaxy;
    if (central == NULL) {
        ERROR_LOG("Central galaxy has NULL galaxy data");
        return -1;
    }

    // Process all disrupting satellites
    for (int i = 0; i < ngal; i++) {
        if (halos[i].galaxy == NULL || !halos[i].galaxy->IsDisrupting || halos[i].Type > 2) {
            continue;
        }

        // Type 0 centrals should never be marked for disruption (upstream module bug)
        if (halos[i].Type == 0) {
            ERROR_LOG("Type 0 central (HaloNr=%lld) marked IsDisrupting=1 - skipping (upstream module bug)",
                      halos[i].HaloNr);
            continue;
        }

        const struct GalaxyData *sat = halos[i].galaxy;

        // Transfer gas to hot phase (disruption heats gas)
        central->HotGas += sat->ColdGas + sat->HotGas;
        central->MetalsHotGas += sat->MetalsColdGas + sat->MetalsHotGas;

        // Transfer ejected mass
        central->EjectedGas += sat->EjectedGas;
        central->MetalsEjectedGas += sat->MetalsEjectedGas;

        // Transfer existing ICS
        central->ICS += sat->ICS;
        central->MetalsICS += sat->MetalsICS;

        // Add ALL stellar mass to intracluster stars
        central->ICS += sat->StellarMass;
        central->MetalsICS += sat->MetalsStellarMass;

        // Note: Black hole is lost during disruption

        // Mark satellite as disrupted (Type 3)
        halos[i].Type = 3;

        DEBUG_LOG("Disrupted satellite %d to ICS (%.3e Msun)",
                  halos[i].HaloNr, sat->StellarMass);
    }

    return 0;
}
