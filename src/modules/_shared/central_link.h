#ifndef MIMIC_SHARED_CENTRAL_LINK_H
#define MIMIC_SHARED_CENTRAL_LINK_H

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
                                                   int satellite_index,
                                                   int fallback_index) {
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

#endif /* MIMIC_SHARED_CENTRAL_LINK_H */
