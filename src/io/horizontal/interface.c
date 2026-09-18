/**
 * @file    horizontal/interface.c
 * @brief   Dispatch layer for horizontal input readers.
 *
 * Thin wrappers around the `struct HorizontalReader` hooks. Every wrapper checks,
 * at its point of use, that the reader implements the hook it is about to call.
 * Point-of-use checking (rather than one up-front completeness check) is
 * deliberate: it lets a reader register with only part of its lifecycle
 * implemented, and it turns a missing hook into a named diagnostic instead of a
 * NULL dereference.
 *
 * This file is deliberately NOT named *hdf5.c -- see horizontal/registry.c for
 * why the non-HDF5 build still needs this translation unit.
 */

#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "galaxy_id.h"
#include "horizontal/reader.h"

/**
 * @def     HORIZONTAL_READER_NAME
 * @brief   Reader name for diagnostics, tolerating an unnamed reader.
 *
 * Every message below goes through this, so none can pass NULL to a %s
 * conversion.
 */
#define HORIZONTAL_READER_NAME(reader) ((reader)->name != NULL ? (reader)->name : "(unnamed)")

/**
 * @def     REQUIRE_HORIZONTAL_READER_HOOK
 * @brief   Abort unless `reader` is present and implements `hook`.
 *
 * The horizontal counterpart of REQUIRE_READER_HOOK in core/vertical_driver.c, kept
 * here so the horizontal dispatchers own their own fail-fast contract.
 */
#define REQUIRE_HORIZONTAL_READER_HOOK(reader, hook)                                               \
  do {                                                                                             \
    if ((reader) == NULL) {                                                                        \
      FATAL_ERROR("No horizontal reader is configured; cannot call hook '%s'", #hook);             \
    }                                                                                              \
    if ((reader)->hook == NULL) {                                                                  \
      FATAL_ERROR("Horizontal reader '%s' does not implement the required hook '%s'",              \
                  HORIZONTAL_READER_NAME(reader), #hook);                                          \
    }                                                                                              \
  } while (0)

/** @brief Open the configured dataset and publish run-scoped metadata. */
void horizontal_reader_open_run(const struct HorizontalReader *reader,
                                struct HorizontalRunInfo *info) {
  REQUIRE_HORIZONTAL_READER_HOOK(reader, open_run);
  if (info == NULL) {
    FATAL_ERROR("Horizontal reader '%s': open_run requires a destination HorizontalRunInfo",
                HORIZONTAL_READER_NAME(reader));
  }
  reader->open_run(info);
}

/** @brief Release every run-scoped resource acquired by open_run. */
void horizontal_reader_close_run(const struct HorizontalReader *reader) {
  REQUIRE_HORIZONTAL_READER_HOOK(reader, close_run);
  reader->close_run();
}

/** @brief Halo count of one snapshot, without loading it. */
int64_t horizontal_reader_halo_count(const struct HorizontalReader *reader, int64_t snapnum) {
  REQUIRE_HORIZONTAL_READER_HOOK(reader, snapshot_halo_count);
  return reader->snapshot_halo_count(snapnum);
}

/** @brief Load one snapshot into a reader-owned slab. */
void horizontal_reader_load_slab(const struct HorizontalReader *reader, int64_t snapnum,
                                 struct SnapshotSlab *slab) {
  REQUIRE_HORIZONTAL_READER_HOOK(reader, load_slab);
  if (slab == NULL) {
    FATAL_ERROR("Horizontal reader '%s': load_slab requires a destination slab handle",
                HORIZONTAL_READER_NAME(reader));
  }
  reader->load_slab(snapnum, slab);
}

/** @brief Release a loaded slab and return the handle to its empty state. */
void horizontal_reader_release_slab(const struct HorizontalReader *reader,
                                    struct SnapshotSlab *slab) {
  REQUIRE_HORIZONTAL_READER_HOOK(reader, release_slab);
  if (slab == NULL) {
    FATAL_ERROR("Horizontal reader '%s': release_slab requires a slab handle",
                HORIZONTAL_READER_NAME(reader));
  }
  reader->release_slab(slab);
}

/**
 * @brief   Are the run-scoped identity bounds encodable with this multiplier?
 *
 * Order matters: the non-positive multiplier is rejected first, so the division
 * below always has a positive divisor, and the bound is expressed as a division
 * rather than a product so it cannot overflow while being checked.
 */
int horizontal_identity_bounds_valid(const struct HorizontalRunInfo *info, int64_t multiplier) {
  if (info == NULL) {
    return 0;
  }
  if (multiplier <= 0) {
    return 0;
  }

  /* A dataset with no halos anywhere carries no forest count and no rank. */
  if (info->n_forests_total == HORIZONTAL_EMPTY_N_FORESTS &&
      info->max_halo_rank_in_forest == HORIZONTAL_EMPTY_MAX_RANK) {
    return 1;
  }

  if (info->n_forests_total < 0 || info->max_halo_rank_in_forest < 0) {
    return 0;
  }
  /* Every rank must be representable below one multiplier step. */
  if (multiplier <= info->max_halo_rank_in_forest) {
    return 0;
  }
  /* (n_forests_total + 1) * multiplier must stay inside int64_t. The bound is
     the encoder's own, taken from galaxy_id.h so this check and the encoder
     cannot drift apart; the multiplier is known positive by the guard above,
     which is that helper's precondition. */
  if (info->n_forests_total > mimic_unique_galaxy_id_max_forests(multiplier)) {
    return 0;
  }
  return 1;
}
