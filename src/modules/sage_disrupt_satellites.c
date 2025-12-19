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
    VERBOSE_LOG("SAGE Disrupt Satellites initialized");
    return 0;
}

int sage_disrupt_satellites_cleanup(void)
{
    return 0;
}

static int find_central(struct Halo *halos, int ngal)
{
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0) return i;
    }
    return -1;
}

/**
 * @brief   Disrupt satellites to intracluster stars
 */
int sage_disrupt_satellites_process(struct ModuleContext *ctx,
                                     struct Halo *halos,
                                     int ngal)
{
    (void)ctx;  /* Unused */

    if (halos == NULL || ngal <= 0) return 0;

    int central_idx = find_central(halos, ngal);
    if (central_idx < 0) return 0;

    struct GalaxyData *central = halos[central_idx].galaxy;
    if (central == NULL) return 0;

    for (int i = 0; i < ngal; i++) {
        if (!halos[i].galaxy || !halos[i].galaxy->IsDisrupting) continue;

        struct GalaxyData *satellite = halos[i].galaxy;

        /* Transfer gas to hot phase (disruption heats gas) */
        central->HotGas += satellite->ColdGas + satellite->HotGas;
        central->MetalsHotGas += satellite->MetalsColdGas + satellite->MetalsHotGas;

        /* Transfer ejected mass */
        central->EjectedGas += satellite->EjectedGas;
        central->MetalsEjectedGas += satellite->MetalsEjectedGas;

        /* Transfer existing ICS */
        central->ICS += satellite->ICS;
        central->MetalsICS += satellite->MetalsICS;

        /* Add ALL stellar mass to intracluster stars */
        central->ICS += satellite->StellarMass;
        central->MetalsICS += satellite->MetalsStellarMass;

        /* Note: Black hole is lost during disruption (as in SAGE) */

        /* Mark satellite as disrupted (Type 3) */
        halos[i].Type = 3;

        DEBUG_LOG("Disrupted satellite %d to ICS (%.3e Msun)",
                  halos[i].HaloNr, satellite->StellarMass);
    }

    return 0;
}
