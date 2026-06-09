#ifndef MIMIC_SHARED_CENTRAL_LINK_H
#define MIMIC_SHARED_CENTRAL_LINK_H

#include <stddef.h>

#include "types.h"

/* Return the Type 0 FOF central index, or -1 if absent. */
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

/*
 * Resolve per-SAGE-parity merge/disruption target.
 * - Type 2 satellites use their CentralHalo link when valid.
 * - All other types use the provided fallback (normally FOF Type 0).
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

/*
 * Resolve the immediate-ordering target used by SAGE's in-loop merger handler.
 * - First choose the ordinary live target (Type 1 -> FOF central, Type 2 ->
 *   CentralHalo when valid).
 * - If that chosen target has already been consumed in the same pass, redirect
 *   exactly one hop via the consumed target's CentralHalo.
 * - Do not recurse and do not replace the redirect hop with a generic FOF
 *   fallback.
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
