/**
 * @file    sage_initialise_merger_clock.c
 * @brief   SAGE merger clock initialisation - assigns merger timescales, handles Type 0 reset and
 * Type 2 force-merge
 *
 * Implements Binney & Tremaine (1987) dynamical friction formula to estimate
 * when satellites will merge with the central galaxy. Calculates merger time
 * once per satellite episode, keyed by the MergTime sentinel state.
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
#include "shared/central_link.h"
#include "shared/sage_constants.h"

/* SAGE parity: satellites below this particle count get an immediate merger
 * clock rather than a dynamical-friction time (estimate_merging_time). */
static const int MinNumPartSatHalo = 10;

int sage_initialise_merger_clock_init(void) {
  VERBOSE_LOG("SAGE initialise merger clock initialized");
  return 0;
}

int sage_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  const int fof_central_idx = mimic_find_fof_central_index(halos, ngal);
  if (fof_central_idx == -1) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL)
      continue;

    /* SAGE parity: reset MergTime for Type 1/2→0 promotions (satellite-to-central). */
    if (halos[i].Type == 0) {
      if (halos[i].galaxy->MergTime < SAGE_MERGTIME_UNSET_THRESHOLD) {
        DEBUG_LOG("Reset MergTime for central %d (Type→0 transition, was %.3f)", halos[i].HaloNr,
                  halos[i].galaxy->MergTime);

        halos[i].galaxy->MergTime = SAGE_MERGTIME_UNSET;
      }
      continue;
    }

    if (halos[i].Type > 2)
      continue; // Only Type 1/2 satellites

    // SAGE parity: unresolved Type 2 entries with unset merger time must
    // merge immediately rather than receive a new dynamical-friction clock.
    if (halos[i].Type == 2 && halos[i].galaxy->MergTime > SAGE_MERGTIME_UNSET_THRESHOLD) {
      halos[i].galaxy->MergTime = 0.0f;
      DEBUG_LOG("Type 2 satellite %d: forcing immediate merge (MergTime=0.0)", halos[i].HaloNr);
      continue;
    }

    const int target_idx = mimic_resolve_type2_target_index(halos, ngal, i, fof_central_idx);
    if (target_idx < 0 || target_idx >= ngal) {
      continue;
    }

    const struct Halo *central = &halos[target_idx];

    // SAGE parity: sentinel state controls recalculation, not infallMvir sign.
    if (halos[i].galaxy->MergTime <= SAGE_MERGTIME_UNSET_THRESHOLD) {
      continue; // Already calculated
    }

    // log1p(N_cen/N_sat) — numerically better than log(1 + x) for small ratios
    // SAGE parity: use the satellite's actual Len (no floor); the Len >= 10
    // requirement is enforced in the merger-time condition below.
    const int sat_len = halos[i].Len;
    const double coulomb = (sat_len > 0) ? log1p((double)central->Len / (double)sat_len) : 0.0;

    const double SatelliteMass =
        halos[i].Mvir + halos[i].galaxy->StellarMass + halos[i].galaxy->ColdGas;

    // Central halo properties for orbital decay (SAGE line 28)
    const double SatelliteRadius = central->Rvir; // Orbital radius ~ central's Rvir
    const double Vvir = central->Vvir;

    // Merger timescale from Binney & Tremaine dynamical friction (SAGE lines 30-35).
    // SAGE parity: satellites with fewer than MinNumPartSatHalo particles get
    // mergtime = -1.0 (immediate merge), not a finite dynamical-friction clock.
    double mergtime;
    if (SatelliteMass > 0.0 && coulomb > 0.0 && halos[i].Len >= MinNumPartSatHalo) {
      mergtime = 2.0 * 1.17 * SatelliteRadius * SatelliteRadius * Vvir /
                 (coulomb * ctx->params->G * SatelliteMass);
    } else {
      // Invalid: zero mass, or below the SAGE particle threshold
      mergtime = SAGE_MERGTIME_IMMEDIATE;
    }

    if (mergtime >= SAGE_MERGTIME_UNSET_THRESHOLD) {
      mergtime = SAGE_MERGTIME_CEILING;
    }

    halos[i].galaxy->MergTime = mergtime;

    DEBUG_LOG("Satellite %d: Set MergTime=%.3f (target=%d, Len=%d, Mvir=%.3e, infallMvir=%.3e)",
              halos[i].HaloNr, mergtime, target_idx, halos[i].Len, halos[i].Mvir,
              halos[i].infallMvir);
  }

  return 0;
}

int sage_initialise_merger_clock_cleanup(void) { return 0; }
