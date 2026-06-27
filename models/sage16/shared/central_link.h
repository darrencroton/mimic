/**
 * @file central_link.h
 * @brief FoF central halo index resolution utilities for SAGE physics modules
 *
 * Helpers to resolve the Type 0 central galaxy index and merge/disruption target
 * indices within a FoF workspace. Multiple modules use these to locate the central
 * galaxy without duplicating the Type == 0 scan or the CentralHalo traversal.
 *
 * @note SAGE parity: CentralHalo traversal rules follow the merge-target logic in
 *       SAGE's make_merger_galaxy() and the Type 2 orphan handling in
 *       sage_resolve_mergers_and_disruption.
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
 * @brief Resolve the merge/disruption target index for a satellite (SAGE parity).
 *
 * Type 2 satellites use their CentralHalo link when valid; all other types use the
 * provided fallback (normally the FoF Type 0 central).
 *
 * @param halos            FoF workspace halo array
 * @param ngal             Number of halos in the workspace
 * @param satellite_index  Index of the satellite being resolved
 * @param fallback_index   Index to use when CentralHalo is absent or invalid
 * @return Resolved target index, or -1 on invalid input
 */
static inline int mimic_resolve_type2_target_index(const struct Halo *halos, int ngal,
                                                   int satellite_index, int fallback_index) {
  if (halos == NULL || satellite_index < 0 || satellite_index >= ngal) {
    return -1;
  }

  if (halos[satellite_index].Type != 2) {
    return fallback_index;
  }

  const int candidate = halos[satellite_index].CentralHalo;
  if (candidate < 0 || candidate >= ngal || candidate == satellite_index) {
    return fallback_index;
  }

  if (halos[candidate].Type != 0 && halos[candidate].Type != 1) {
    return fallback_index;
  }

  return candidate;
}

/**
 * @brief Resolve the immediate merge target used by SAGE's in-loop merger handler.
 *
 * Selects the live target (Type 1 → FoF central; Type 2 → CentralHalo when valid).
 * If that target has already been consumed (Type 3) in the same pass, one redirect
 * hop through the consumed target's CentralHalo is applied. Does not recurse and
 * does not fall back to the FoF central on a failed redirect.
 *
 * @param halos            FoF workspace halo array
 * @param ngal             Number of halos in the workspace
 * @param satellite_index  Index of the satellite being merged
 * @param fallback_index   Index to use for Type 1 satellites (normally FoF Type 0)
 * @return Resolved immediate target index, or -1 if resolution fails
 */
static inline int mimic_resolve_immediate_target_index(const struct Halo *halos, int ngal,
                                                       int satellite_index, int fallback_index) {
  int target_idx = fallback_index;

  if (halos == NULL || satellite_index < 0 || satellite_index >= ngal) {
    return -1;
  }

  if (halos[satellite_index].Type == 2) {
    const int candidate = halos[satellite_index].CentralHalo;
    if (candidate >= 0 && candidate < ngal && candidate != satellite_index) {
      target_idx = candidate;
    }
  }

  if (target_idx < 0 || target_idx >= ngal || target_idx == satellite_index) {
    return -1;
  }

  if (halos[target_idx].Type != 3) {
    return target_idx;
  }

  const int redirect_idx = halos[target_idx].CentralHalo;
  if (redirect_idx < 0 || redirect_idx >= ngal || redirect_idx == satellite_index) {
    return -1;
  }

  return redirect_idx;
}

#endif /* MIMIC_SHARED_CENTRAL_LINK_H */
