/**
 * @file    sage_calculate_merger_timescale.c
 * @brief   Calculate dynamical friction merger timescales for satellites
 *
 * Implements Binney & Tremaine (1987) dynamical friction formula to
 * estimate when satellites will merge with the central galaxy.
 *
 * References:
 *   - SAGE: sage-code/model_mergers.c (lines 14-46: estimate_merging_time)
 *   - Binney & Tremaine (1987, 2008) - Orbital decay
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"

/* Chandrasekhar coefficient for dynamical friction (Binney & Tremaine) */
static const double CHANDRASEKHAR_COEFF = 2.0 * 1.17;

/* Coulomb logarithm approximation factor */
static const double COULOMB_LOG_APPROX = 1.0;

/* Minimum number of particles for reliable merger timescale */
static const int MIN_NUM_PART_SAT_HALO = 10;

/* Maximum merger timescale (ceiling for low-mass satellites) */
static const double MAX_MERGER_TIME = 998.0;

int sage_calculate_merger_timescale_init(void)
{
    VERBOSE_LOG("SAGE Calculate Merger Timescale initialized");
    return 0;
}

int sage_calculate_merger_timescale_cleanup(void)
{
    return 0;
}

/**
 * @brief   Calculate merger timescales for all satellites in FOF group
 *
 * Uses Binney & Tremaine dynamical friction formula:
 *   t_merge = 2 * 1.17 * R_vir^2 * V_vir / (ln(N_cen/N_sat) * G * M_sat)
 *
 * SAGE reference: model_mergers.c lines 14-46
 */
int sage_calculate_merger_timescale_process(struct ModuleContext *ctx,
                                             struct Halo *halos,
                                             int ngal)
{
    if (halos == NULL || ngal <= 0) {
        return 0;
    }

    /* Find central halo (Type 0) */
    int central_idx = -1;
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type == 0) {
            central_idx = i;
            break;
        }
    }

    if (central_idx == -1) {
        return 0;  /* No central (shouldn't happen) */
    }

    struct Halo *central = &halos[central_idx];

    /* Calculate merger timescale for each Type 1 satellite */
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type != 1) continue;  /* Only Type 1 satellites */
        if (halos[i].galaxy == NULL) continue;

        /* Skip satellites with too few particles (SAGE line 30) */
        if (halos[i].Len < MIN_NUM_PART_SAT_HALO) {
            halos[i].galaxy->MergTime = -1.0;
            continue;
        }

        /* Calculate Coulomb logarithm from particle number ratio (SAGE line 25) */
        double coulomb = log1p(safe_div((double)central->Len, (double)halos[i].Len, 1.0));

        /* Total satellite mass (dark matter + baryons) (SAGE line 27) */
        double sat_mass = halos[i].Mvir + halos[i].galaxy->StellarMass + halos[i].galaxy->ColdGas;

        /* Host halo properties (SAGE line 28) */
        double sat_radius = central->Rvir;
        double vvir = central->Vvir;

        /* Merger timescale (Binney & Tremaine 1987) (SAGE lines 31-32) */
        double mergtime = -1.0;
        if (sat_mass > 0.0 && coulomb > 0.0) {
            mergtime = safe_div(CHANDRASEKHAR_COEFF * COULOMB_LOG_APPROX *
                                sat_radius * sat_radius * vvir,
                                coulomb * ctx->params->G * sat_mass, -1.0);
        }

        /* Apply ceiling for very long merger times (SAGE lines 37-42) */
        if (mergtime >= 999.0) {
            mergtime = MAX_MERGER_TIME;
        }

        halos[i].galaxy->MergTime = mergtime;

        DEBUG_LOG("Satellite %d: MergTime=%.3f Gyr (Len=%d, Mvir=%.3e)",
                  halos[i].HaloNr, mergtime, halos[i].Len, halos[i].Mvir);
    }

    return 0;
}
