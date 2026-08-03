/**
 * @file    snapshot/interface.c
 * @brief   Dispatch layer for snapshot-ordered input readers.
 *
 * Thin wrappers around the `struct SnapshotReader` hooks. Every wrapper checks,
 * at its point of use, that the reader implements the hook it is about to call.
 * Point-of-use checking (rather than one up-front completeness check) is
 * deliberate: it lets a reader register with only part of its lifecycle
 * implemented, and it turns a missing hook into a named diagnostic instead of a
 * NULL dereference.
 *
 * This file is deliberately NOT named *hdf5.c -- see snapshot/registry.c for
 * why the non-HDF5 build still needs this translation unit.
 */

#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "snapshot/reader.h"

/**
 * @def     REQUIRE_SNAPSHOT_READER_HOOK
 * @brief   Abort unless `reader` is present and implements `hook`.
 *
 * The snapshot counterpart of REQUIRE_READER_HOOK in core/tree_driver.c, kept
 * here so the snapshot dispatchers own their own fail-fast contract.
 */
#define REQUIRE_SNAPSHOT_READER_HOOK(reader, hook)                                                 \
  do {                                                                                             \
    if ((reader) == NULL) {                                                                        \
      FATAL_ERROR("No snapshot reader is configured; cannot call hook '%s'", #hook);               \
    }                                                                                              \
    if ((reader)->hook == NULL) {                                                                  \
      FATAL_ERROR("Snapshot reader '%s' does not implement the required hook '%s'",                \
                  (reader)->name != NULL ? (reader)->name : "(unnamed)", #hook);                   \
    }                                                                                              \
  } while (0)

/** @brief Open the configured dataset and publish run-scoped metadata. */
void snapshot_reader_open_run(const struct SnapshotReader *reader, struct SnapshotRunInfo *info) {
  REQUIRE_SNAPSHOT_READER_HOOK(reader, open_run);
  if (info == NULL) {
    FATAL_ERROR("Snapshot reader '%s': open_run requires a destination SnapshotRunInfo",
                reader->name);
  }
  reader->open_run(info);
}

/** @brief Release every run-scoped resource acquired by open_run. */
void snapshot_reader_close_run(const struct SnapshotReader *reader) {
  REQUIRE_SNAPSHOT_READER_HOOK(reader, close_run);
  reader->close_run();
}

/** @brief Halo count of one snapshot, without loading it. */
int64_t snapshot_reader_halo_count(const struct SnapshotReader *reader, int64_t snapnum) {
  REQUIRE_SNAPSHOT_READER_HOOK(reader, snapshot_halo_count);
  return reader->snapshot_halo_count(snapnum);
}

/** @brief Load one snapshot into a reader-owned slab. */
void snapshot_reader_load_slab(const struct SnapshotReader *reader, int64_t snapnum,
                               struct SnapshotSlab *slab) {
  REQUIRE_SNAPSHOT_READER_HOOK(reader, load_slab);
  if (slab == NULL) {
    FATAL_ERROR("Snapshot reader '%s': load_slab requires a destination slab handle", reader->name);
  }
  reader->load_slab(snapnum, slab);
}

/** @brief Release a loaded slab and return the handle to its empty state. */
void snapshot_reader_release_slab(const struct SnapshotReader *reader, struct SnapshotSlab *slab) {
  REQUIRE_SNAPSHOT_READER_HOOK(reader, release_slab);
  if (slab == NULL) {
    FATAL_ERROR("Snapshot reader '%s': release_slab requires a slab handle", reader->name);
  }
  reader->release_slab(slab);
}
