#ifndef GALAXY_ID_H
#define GALAXY_ID_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    galaxy_id.h
 * @brief   Helpers for UniqueGalaxyID component bounds and encoding.
 *
 * Every helper takes the run's forest multiplier as an explicit parameter
 * instead of reading the compile-time default in constants.h, so both
 * processing orders encode with the configured
 * simulation.unique_galaxy_id_multiplier (MimicConfig.UniqueGalaxyIDMultiplier).
 * Callers pass that field; nothing here reaches into configuration itself,
 * which keeps the helpers usable from readers, drivers and unit tests alike.
 *
 * Precondition for every helper: multiplier > 0. Configuration enforces it at
 * parse time (read_parameter_file.c is fatal on a non-positive value), so the
 * helpers do not re-check it and must never be handed an unvalidated value --
 * mimic_unique_galaxy_id_max_forests() divides by it.
 */

/**
 * @brief   Largest run-scoped forest count the two-term encoding can represent.
 *
 * `INT64_MAX / multiplier - 1` reserves the encoder's `+ 1` forest offset and
 * cannot itself overflow. This is the same bound the snapshot input's open-time
 * header check applies; snapshot_identity_bounds_valid() delegates here so the
 * codebase carries one bound expression rather than two.
 */
static inline int64_t mimic_unique_galaxy_id_max_forests(int64_t multiplier) {
  return INT64_MAX / multiplier - 1;
}

static inline bool mimic_unique_galaxy_id_total_forests_valid(int64_t multiplier,
                                                              int64_t total_forests) {
  return total_forests >= 0 && total_forests <= mimic_unique_galaxy_id_max_forests(multiplier);
}

static inline bool mimic_unique_galaxy_id_components_valid(int64_t multiplier, int64_t halonr,
                                                           int64_t forestnr_global) {
  return halonr >= 0 && halonr < multiplier && forestnr_global >= 0 &&
         forestnr_global < mimic_unique_galaxy_id_max_forests(multiplier);
}

/* Precondition: mimic_unique_galaxy_id_components_valid() must be true. */
static inline int64_t mimic_encode_unique_galaxy_id(int64_t multiplier, int64_t halonr,
                                                    int64_t forestnr_global) {
  return halonr + multiplier * (forestnr_global + 1LL);
}

#endif /* #ifndef GALAXY_ID_H */
