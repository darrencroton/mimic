/**
 * @file central_link.h
 * @brief FoF central halo index resolution utilities for SAGE physics modules
 *
 * Helpers to resolve the Type 0 central galaxy index and the merger and disruption
 * target index within a FoF workspace. Multiple modules use these to locate the central
 * galaxy without duplicating the Type == 0 scan or the CentralHalo traversal.
 *
 * @note SAGE parity: the merger and disruption target follows SAGE26's per-substep merger pass
 *       in evolve_galaxies; Mimic's core sets every row's CentralHalo to its subhalo's Type 0 or
 *       1 row (set_local_centrals), as SAGE26's joining sets CentralGal.
 */

#ifndef MIMIC_SHARED_CENTRAL_LINK_H
#define MIMIC_SHARED_CENTRAL_LINK_H

#include <stddef.h>

#include "types.h"

/**
 * @brief Return the Type 0 FoF central index, or -1 if absent.
 *
 * @param halos  FoF workspace halo array
 * @param ngal   Number of halos in the workspace
 * @return Index of the Type 0 central, or -1 if not found
 */
static inline int mimic_find_fof_central_index(const struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return -1;
  }

  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 0) {
      return i;
    }
  }

  return -1;
}

/**
 * @brief SAGE26's merger and disruption target, as evolve_galaxies chooses merger_centralgal
 *
 * A Type 1 targets the FoF central, a Type 2 its subhalo's central (its CentralHalo). A
 * target that has merged or been disrupted in this snapshot (the package's Type 3,
 * SAGE26's mergeType > 0) is redirected once, through its own CentralHalo, without
 * recursion and without checking the redirected row: a Type 1's CentralHalo is itself,
 * so an orphan whose Type 1 has just merged is redirected onto that merged galaxy, as in
 * SAGE26.
 *
 * There is no fallback to the FoF central for an invalid link: an index outside the
 * workspace returns -1, which the caller reports as an error instead of reading outside
 * the array. The core sets every CentralHalo inside its subhalo's slice
 * (set_local_centrals in src/core/inheritance.c), so a valid run never reaches it.
 *
 * @param halos              FoF workspace halo array
 * @param ngal               Number of halos in the workspace
 * @param satellite_index    The Type 1 or 2 satellite (SAGE26's p)
 * @param fof_central_index  The FoF's Type 0 row (SAGE26's centralgal)
 * @param redirected         Optional: 1 if the redirect was taken, else 0
 * @return The target index, or -1 if an index falls outside the workspace
 */
static inline int mimic_resolve_immediate_target_index(const struct Halo *halos, int ngal,
                                                       int satellite_index, int fof_central_index,
                                                       int *redirected) {
  if (redirected != NULL) {
    *redirected = 0;
  }
  if (halos == NULL || satellite_index < 0 || satellite_index >= ngal) {
    return -1;
  }

  int target_idx =
      (halos[satellite_index].Type == 1) ? fof_central_index : halos[satellite_index].CentralHalo;
  if (target_idx < 0 || target_idx >= ngal) {
    return -1;
  }

  if (halos[target_idx].Type == 3) {
    target_idx = halos[target_idx].CentralHalo;
    if (redirected != NULL) {
      *redirected = 1;
    }
    if (target_idx < 0 || target_idx >= ngal) {
      return -1;
    }
  }

  return target_idx;
}

#endif /* MIMIC_SHARED_CENTRAL_LINK_H */
