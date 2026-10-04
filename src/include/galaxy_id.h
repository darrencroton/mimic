#ifndef GALAXY_ID_H
#define GALAXY_ID_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "constants.h"

/**
 * @file    galaxy_id.h
 * @brief   Helpers for UniqueGalaxyID component bounds and encoding.
 *
 * Every tree-ID helper takes the run's forest multiplier as an explicit
 * parameter instead of reading the compile-time default in constants.h, so both
 * processing orders encode with the configured
 * simulation.unique_galaxy_id_multiplier (MimicConfig.UniqueGalaxyIDMultiplier).
 * Callers pass that field; nothing here reaches into configuration itself,
 * which keeps the helpers usable from readers, drivers and unit tests alike.
 * The created-record helpers at the end of this file likewise take their
 * published identity space as explicit parameters.
 *
 * Precondition for every tree-ID helper: multiplier > 0. Configuration enforces
 * it at parse time (read_parameter_file.c is fatal on a non-positive value), so
 * the helpers do not re-check it and must never be handed an unvalidated value --
 * mimic_unique_galaxy_id_max_forests() divides by it.
 */

/**
 * @brief   Largest run-scoped forest count the two-term encoding can represent.
 *
 * `INT64_MAX / multiplier - 1` reserves the encoder's `+ 1` forest offset and
 * cannot itself overflow. This is the same bound the snapshot input's open-time
 * header check applies; horizontal_identity_bounds_valid() delegates here so the
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

/*
 * Created-record identity.
 *
 * A record created during processing has no catalogue row of its own, so it is
 * named after its host row in a separate, strictly negative namespace:
 *
 *   UniqueGalaxyID = -(1 + ordinal + MAX_CREATED_RECORDS_PER_HOST * host_key)
 *   host_key       = row + rows_per_unit * unit
 *
 * A driver publishes (unit, rows_per_unit) once per processing unit through
 * struct RecordIdentitySpace (types.h): the vertical driver's unit is the run's
 * global forest number and its rows_per_unit the run-wide largest forest (or the
 * forest multiplier when a reader cannot report it); the horizontal driver's unit
 * is the snapshot number and its rows_per_unit the largest slab. `row` is the
 * host's HaloNr and `ordinal` counts the host's created records in creation order.
 * The positive tree encoding above is untouched, so the two namespaces never
 * meet.
 */

/**
 * @brief   Whether a created-record identity space fits a signed 64-bit ID.
 *
 * The largest magnitude mimic_encode_created_galaxy_id() can produce over
 * `units` units of `rows_per_unit` rows is exactly
 * `MAX_CREATED_RECORDS_PER_HOST * units * rows_per_unit`, so the space fits iff
 * that product is at most INT64_MAX. The product is compared through a quotient
 * bound and never formed, so no intermediate can overflow. A space with zero
 * units or zero rows fits: nothing can be created in it. Negative inputs are
 * not a space and never fit.
 */
static inline bool mimic_created_record_space_fits(int64_t units, int64_t rows_per_unit) {
  if (units < 0 || rows_per_unit < 0) {
    return false;
  }
  if (units == 0 || rows_per_unit == 0) {
    return true;
  }
  /* 1024 * P <= INT64_MAX  <=>  P <= floor(INT64_MAX / 1024) for integer P, and
   * units * rows_per_unit <= q  <=>  units <= floor(q / rows_per_unit). */
  const int64_t max_host_keys = INT64_MAX / MAX_CREATED_RECORDS_PER_HOST;
  return units <= max_host_keys / rows_per_unit;
}

/**
 * @brief   Encode a created record's UniqueGalaxyID.
 * @param   unit           Published processing unit (>= 0).
 * @param   row            Host row within the unit, `0 <= row < rows_per_unit`.
 * @param   rows_per_unit  Published rows per unit.
 * @param   ordinal        Host's created-record ordinal,
 *                         `0 <= ordinal < MAX_CREATED_RECORDS_PER_HOST`.
 * @return  `-(1 + ordinal + MAX_CREATED_RECORDS_PER_HOST * (row + rows_per_unit * unit))`,
 *          always strictly negative.
 *
 * Preconditions (asserted when assertions are enabled): the three bounds above,
 * and a space of `unit + 1` units of `rows_per_unit` rows that fits
 * (mimic_created_record_space_fits()), which a driver establishes once at startup
 * for the run's whole unit count. Under those preconditions no term overflows.
 */
static inline int64_t mimic_encode_created_galaxy_id(int64_t unit, int64_t row,
                                                     int64_t rows_per_unit, int ordinal) {
  assert(unit >= 0);
  assert(row >= 0 && row < rows_per_unit);
  assert(ordinal >= 0 && ordinal < MAX_CREATED_RECORDS_PER_HOST);
  assert(unit < INT64_MAX && mimic_created_record_space_fits(unit + 1, rows_per_unit));

  const int64_t host_key = row + rows_per_unit * unit;
  return -(1 + (int64_t)ordinal + (int64_t)MAX_CREATED_RECORDS_PER_HOST * host_key);
}

/**
 * @brief   Decode a created record's UniqueGalaxyID into the encoder's inputs.
 * @param   id             A created ID (strictly negative).
 * @param   rows_per_unit  The rows per unit it was encoded with (> 0), as the run logs it.
 * @param   unit           Receives the processing unit.
 * @param   row            Receives the host row (its HaloNr), `0 <= row < rows_per_unit`.
 * @param   ordinal        Receives the host's created-record ordinal.
 *
 * The exact inverse of mimic_encode_created_galaxy_id() for any ID it produced.
 * `-(id + 1)` is formed instead of `-id - 1`, so no step can overflow even for
 * INT64_MIN. Preconditions (asserted): `id < 0` and `rows_per_unit > 0`.
 */
static inline void mimic_decode_created_galaxy_id(int64_t id, int64_t rows_per_unit, int64_t *unit,
                                                  int64_t *row, int *ordinal) {
  assert(id < 0);
  assert(rows_per_unit > 0);

  const int64_t k = -(id + 1);
  const int64_t host_key = k / MAX_CREATED_RECORDS_PER_HOST;
  *ordinal = (int)(k % MAX_CREATED_RECORDS_PER_HOST);
  *row = host_key % rows_per_unit;
  *unit = host_key / rows_per_unit;
}

#endif /* #ifndef GALAXY_ID_H */
