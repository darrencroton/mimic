/**
 * @file    sage_calculate_merger_timescale.c
 * @brief   Calculate dynamical friction merger timescales for satellites
 *
 * Implements Binney & Tremaine (1987) dynamical friction formula to estimate
 * when satellites will merge with the central galaxy. Calculates merger time
 * ONCE when a galaxy first becomes a satellite (Type 0→1 transition).
 *
 * Physics: Chandrasekhar dynamical friction with Coulomb logarithm
 *   t_merge = 2 * 1.17 * R_vir^2 * V_vir / (ln(N_cen/N_sat) * G * M_sat)
 *
 * References:
 *   - SAGE: sage-code/model_mergers.c (estimate_merging_time)
 *   - Binney & Tremaine (1987, 2008)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "types.h"

int sage_calculate_merger_timescale_init(void)
{
    VERBOSE_LOG("SAGE calculate merger timescale initialized");
    return 0;
}

// Calculate merger timescale for satellites using dynamical friction formula.
// Only calculates when galaxy first becomes a satellite (infallMvir > 0 and MergTime unset).
int sage_calculate_merger_timescale_process(struct ModuleContext *ctx,
                                             struct Halo *halos,
                                             int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    // Find central halo (Type 0)
    int central_idx = -1;
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0) {
            central_idx = i;
            break;
        }
    }

    if (central_idx == -1) {
        return 0;  // No central (shouldn't happen in normal FOF groups)
    }

    const struct Halo *central = &halos[central_idx];

    // Calculate merger timescale for Type 1/2 satellites that just became satellites
    for (int i = 0; i < ngal; i++) {
        if (halos[i].galaxy == NULL) continue;

        // Reset MergTime for all Type 0 centrals to sentinel value
        // This handles Type 1/2→0 transitions where satellites become centrals
        // (e.g., original central disrupted, satellite promoted to central)
        if (halos[i].Type == 0) {
            if (halos[i].galaxy->MergTime < 999.0f) {
                DEBUG_LOG("Reset MergTime for central %d (Type→0 transition, was %.3f)",
                    halos[i].HaloNr, halos[i].galaxy->MergTime);

                halos[i].galaxy->MergTime = 999.9f;
            }
            continue;
        }
            
        if (halos[i].Type > 2) continue;  // Only Type 1/2 satellites

        // Only calculate when galaxy FIRST becomes a satellite
        // infallMvir > 0 means infall properties have been set (Type 0→1/2 transition)
        // MergTime > 999.0 means MergTime hasn't been calculated yet (sentinel = 999.9)
        if (halos[i].infallMvir <= 0.0 || halos[i].galaxy->MergTime <= 999.0) {
            continue;  // Already calculated or not yet a satellite
        }

        const int MinNumPartSatHalo = 10;

        // Coulomb logarithm from particle number ratio, log1p(x) is better than log(1 + x)
        // Guard against Type 2 orphans (Len=0) by setting floor at MinNumPartSatHalo
        const int sat_len = (halos[i].Len < MinNumPartSatHalo) ? MinNumPartSatHalo : halos[i].Len;
        const double coulomb = log1p((double)central->Len / (double)sat_len);

        // Total satellite mass: dark matter + stars + cold gas
        const double SatelliteMass = halos[i].Mvir + halos[i].galaxy->StellarMass + halos[i].galaxy->ColdGas;

        // Central halo properties for orbital decay (SAGE line 28)
        const double SatelliteRadius = central->Rvir;  // Orbital radius ~ central's Rvir
        const double Vvir = central->Vvir;

        // Merger timescale from Binney & Tremaine dynamical friction (SAGE lines 30-35)
        double mergtime;
        if (SatelliteMass > 0.0 && coulomb > 0.0) {
            mergtime = 2.0 * 1.17 * SatelliteRadius * SatelliteRadius * Vvir /
                       (coulomb * ctx->params->G * SatelliteMass);
        } else {
            mergtime = -1.0;  // Invalid: zero mass
        }

        // Apply ceiling for very long merger times
        if (mergtime >= 999.0) {
            mergtime = 998.0;
        }

        halos[i].galaxy->MergTime = mergtime;

        DEBUG_LOG("Satellite %d: Set MergTime=%.3f (Len=%d, Mvir=%.3e, infallMvir=%.3e)",
                  halos[i].HaloNr, mergtime, halos[i].Len, halos[i].Mvir, halos[i].infallMvir);
    }

    return 0;
}

int sage_calculate_merger_timescale_cleanup(void)
{
    return 0;
}
