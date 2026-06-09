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

int sage_initialise_merger_clock_init(void) {
  VERBOSE_LOG("SAGE initialise merger clock initialized");
  return 0;
}

// Calculate merger timescale for satellites using dynamical friction formula.
// Runs only when MergTime is still at the unset sentinel (> 999.0).
//
// SAGE parity note:
// - Type 2 satellites with sentinel MergTime are forced to immediate merge
//   (MergTime = 0.0), matching SAGE's Type 0->2 orphan transition handling.
// - Dynamical friction timescale is computed for Type 1 satellites only.
int sage_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  /* Find FOF central (Type 0) used as fallback target. */
  const int fof_central_idx = mimic_find_fof_central_index(halos, ngal);
  if (fof_central_idx == -1) {
    return 0; // No central (shouldn't happen in normal FOF groups)
  }

  // Calculate merger timescale for Type 1/2 satellites that just became satellites
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL)
      continue;

    // Reset MergTime for all Type 0 centrals to sentinel value
    // This handles Type 1/2→0 transitions where satellites become centrals
    // (e.g., original central disrupted, satellite promoted to central)
    if (halos[i].Type == 0) {
      if (halos[i].galaxy->MergTime < 999.0f) {
        DEBUG_LOG("Reset MergTime for central %d (Type→0 transition, was %.3f)", halos[i].HaloNr,
                  halos[i].galaxy->MergTime);

        halos[i].galaxy->MergTime = 999.9f;
      }
      continue;
    }

    if (halos[i].Type > 2)
      continue; // Only Type 1/2 satellites

    // SAGE parity: unresolved Type 2 entries with unset merger time must
    // merge immediately rather than receive a new dynamical-friction clock.
    if (halos[i].Type == 2 && halos[i].galaxy->MergTime > 999.0f) {
      halos[i].galaxy->MergTime = 0.0f;
      DEBUG_LOG("Type 2 satellite %d: forcing immediate merge (MergTime=0.0)", halos[i].HaloNr);
      continue;
    }

    const int target_idx = mimic_resolve_type2_target_index(halos, ngal, i, fof_central_idx);
    if (target_idx < 0 || target_idx >= ngal) {
      continue;
    }

    const struct Halo *central = &halos[target_idx];

    // Calculate only once per satellite episode: skip if already set.
    // SAGE parity uses MergTime sentinel state, not infallMvir sign.
    if (halos[i].galaxy->MergTime <= 999.0) {
      continue; // Already calculated
    }

    const int MinNumPartSatHalo = 10;

    // Coulomb logarithm from particle number ratio, log1p(x) is better than log(1 + x)
    // SAGE parity: use the satellite's actual Len (no floor); the Len >= 10
    // requirement is enforced in the merger-time condition below.
    const int sat_len = halos[i].Len;
    const double coulomb = (sat_len > 0) ? log1p((double)central->Len / (double)sat_len) : 0.0;

    // Total satellite mass: dark matter + stars + cold gas
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
      mergtime = -1.0; // Invalid: zero mass, or below the SAGE particle threshold
    }

    // Apply ceiling for very long merger times
    if (mergtime >= 999.0) {
      mergtime = 998.0;
    }

    halos[i].galaxy->MergTime = mergtime;

    DEBUG_LOG("Satellite %d: Set MergTime=%.3f (target=%d, Len=%d, Mvir=%.3e, infallMvir=%.3e)",
              halos[i].HaloNr, mergtime, target_idx, halos[i].Len, halos[i].Mvir,
              halos[i].infallMvir);
  }

  return 0;
}

int sage_initialise_merger_clock_cleanup(void) { return 0; }
