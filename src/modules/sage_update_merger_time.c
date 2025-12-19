/**
 * @file    sage_update_merger_time.c
 * @brief   Decrement merger timescales and set merger/disruption flags
 *
 * This module manages merger timing by:
 * 1. Decrementing MergTime by dt for each satellite
 * 2. Detecting when mergers occur (MergTime <= 0)
 * 3. Setting IsMerging or IsDisrupting flags based on mass ratio
 *
 * Severely stripped satellites (very low mass ratio) are marked for
 * disruption to ICS instead of merging.
 */

#include "error.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"

/* Threshold for satellite disruption (instead of merger) */
static const double DISRUPTION_THRESHOLD = 0.001;  /* 1:1000 mass ratio */

int sage_update_merger_time_init(void)
{
    VERBOSE_LOG("SAGE Update Merger Time initialized");
    return 0;
}

int sage_update_merger_time_cleanup(void)
{
    return 0;
}

/**
 * @brief   Helper to find central halo index
 */
static int find_central(struct Halo *halos, int ngal)
{
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief   Calculate mass ratio (mi/ma where mi <= ma)
 */
static double calculate_mass_ratio(struct GalaxyData *sat, struct GalaxyData *cen)
{
    double sat_mass = sat->StellarMass + sat->ColdGas;
    double cen_mass = cen->StellarMass + cen->ColdGas;

    double mi = (sat_mass < cen_mass) ? sat_mass : cen_mass;
    double ma = (sat_mass < cen_mass) ? cen_mass : sat_mass;

    return safe_div(mi, ma, 0.0);
}

/**
 * @brief   Decrement merger timescales and set merger/disruption flags
 */
int sage_update_merger_time_process(struct ModuleContext *ctx,
                                     struct Halo *halos,
                                     int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    double dt = ctx->substep_dt;

    int central_idx = find_central(halos, ngal);
    if (central_idx < 0) {
        return 0;  /* No central */
    }

    struct GalaxyData *central_gal = halos[central_idx].galaxy;
    if (central_gal == NULL) {
        return 0;
    }

    /* Process each satellite */
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type != 1) continue;  /* Only satellites */
        if (halos[i].galaxy == NULL) continue;

        /* Decrement merger time */
        halos[i].galaxy->MergTime -= dt;

        /* Check if merger/disruption occurs this substep */
        if (halos[i].galaxy->MergTime <= 0.0) {
            double mass_ratio = calculate_mass_ratio(halos[i].galaxy, central_gal);

            /* Severely stripped satellites disrupt to ICS */
            if (mass_ratio < DISRUPTION_THRESHOLD) {
                halos[i].galaxy->IsDisrupting = 1;
                DEBUG_LOG("Satellite %d disrupting (ratio=%.6f < %.6f)",
                          halos[i].HaloNr, mass_ratio, DISRUPTION_THRESHOLD);
            } else {
                /* Otherwise merge with central */
                halos[i].galaxy->IsMerging = 1;
                halos[i].galaxy->MergerMassRatio = mass_ratio;
                DEBUG_LOG("Satellite %d merging (ratio=%.3f)",
                          halos[i].HaloNr, mass_ratio);
            }
        }
    }

    return 0;
}
