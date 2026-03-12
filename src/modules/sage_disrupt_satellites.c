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
#include "_shared/central_link.h"
#include "_shared/sage_merger_ops.h"

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
    (void)ctx;

    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    // Find FOF central (Type 0), used as fallback target for non-Type2 satellites.
    const int fof_central_idx = mimic_find_fof_central_index(halos, ngal);
    if (fof_central_idx < 0 || halos[fof_central_idx].galaxy == NULL) {
        return 0;
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
        const int target_idx =
            mimic_resolve_type2_target_index(halos, ngal, i, fof_central_idx);

        if (target_idx < 0 || target_idx >= ngal || target_idx == i ||
            halos[target_idx].galaxy == NULL) {
            ERROR_LOG("Invalid disruption target (satellite=%d, target=%d)",
                      i, target_idx);
            return -1;
        }

        struct GalaxyData *central = halos[target_idx].galaxy;

        mimic_sage_disruption_transfer(central, sat);

        // Note: Black hole is lost during disruption

        // Mark satellite as disrupted (Type 3)
        halos[i].Type = 3;

        DEBUG_LOG("Disrupted satellite %d into target %d ICS (%.3e Msun)",
                  halos[i].HaloNr, target_idx, sat->StellarMass);
    }

    return 0;
}
