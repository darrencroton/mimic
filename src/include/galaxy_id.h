#ifndef GALAXY_ID_H
#define GALAXY_ID_H

#include "constants.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @file    galaxy_id.h
 * @brief   Helpers for UniqueGalaxyID component bounds and encoding.
 */

static inline int64_t mimic_unique_galaxy_id_max_forests(void) { return LLONG_MAX / TREE_MUL_FAC; }

static inline bool mimic_unique_galaxy_id_total_forests_valid(int64_t total_forests) {
  return total_forests >= 0 && total_forests <= mimic_unique_galaxy_id_max_forests();
}

static inline bool mimic_unique_galaxy_id_components_valid(int64_t halonr,
                                                           int64_t forestnr_global) {
  return halonr >= 0 && halonr < TREE_MUL_FAC && forestnr_global >= 0 &&
         forestnr_global < mimic_unique_galaxy_id_max_forests();
}

/* Precondition: mimic_unique_galaxy_id_components_valid() must be true. */
static inline int64_t mimic_encode_unique_galaxy_id(int64_t halonr, int64_t forestnr_global) {
  return halonr + TREE_MUL_FAC * forestnr_global;
}

#endif /* #ifndef GALAXY_ID_H */
