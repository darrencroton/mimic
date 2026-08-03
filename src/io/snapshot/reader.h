#ifndef IO_SNAPSHOT_READER_H
#define IO_SNAPSHOT_READER_H

#include <stdint.h>

/* enum InputProcessingOrder and input_processing_order_name() are shared with
   the tree side rather than duplicated: the processing order is a property of
   the run, not of one reader family. */
#include "tree/reader.h"

/**
 * @file    snapshot/reader.h
 * @brief   Format-agnostic snapshot-ordered input reader interface.
 *
 * The snapshot-ordered front end reads one snapshot at a time: the working set
 * of a run is one snapshot's halo population instead of one forest's history.
 * See docs/dev/SNAPSHOT-HDF5-FORMAT.md for the on-disk contract this interface
 * consumes.
 *
 * This is deliberately a second, small vtable rather than a widening of
 * struct TreeReader, whose twelve hooks are partition/unit-shaped and carry no
 * meaning for snapshot-ordered input. Readers register in snapshot/registry.c
 * and are dispatched through the thin wrappers below (snapshot/interface.c),
 * which verify at each point of use that the hook they need is implemented.
 *
 * Lifecycle:
 *   open_run  -> [ load_slab / release_slab ]* -> close_run
 * with snapshot_halo_count queryable between open_run and close_run.
 */

/* Populated per simulation package from halo_properties.yaml
   (src/include/generated/raw_halo_defs.h). */
struct RawHalo;

/** Run-scoped metadata published by open_run. */
struct SnapshotRunInfo {
  int64_t snapshot_count;          /* number of snapshots in the run */
  int32_t format_version;          /* on-disk contract version of the dataset */
  int64_t n_forests_total;         /* run-scoped forest count (identity bound) */
  int64_t max_halo_rank_in_forest; /* run-scoped maximum rank (identity bound) */
};

/** Sentinel snapnum marking a slab that holds no loaded snapshot. */
#define SNAPSHOT_SLAB_NO_SNAPSHOT ((int64_t)-1)

/** Static initializer for the empty slab state. */
#define SNAPSHOT_SLAB_INIT {SNAPSHOT_SLAB_NO_SNAPSHOT, 0, NULL}

/**
 * One snapshot's halo population, owned by the reader between load_slab and
 * release_slab. Counts and indices are int64_t throughout: production slabs
 * reach hundreds of millions of halos, so the tree driver's int idiom does not
 * carry over.
 */
struct SnapshotSlab {
  int64_t snapnum;       /* loaded snapshot, or SNAPSHOT_SLAB_NO_SNAPSHOT */
  int64_t nhalos;        /* halos in this slab */
  struct RawHalo *halos; /* [nhalos], reader-owned */
};

/** @brief Value of an empty (unloaded) slab handle. */
static inline struct SnapshotSlab snapshot_slab_empty(void) {
  struct SnapshotSlab slab = SNAPSHOT_SLAB_INIT;
  return slab;
}

/**
 * @brief   Is this slab handle in its empty state?
 *
 * snapnum is the marker, not nhalos: a snapshot containing zero halos is a
 * legal load result and still carries its snapshot number.
 */
static inline int snapshot_slab_is_empty(const struct SnapshotSlab *slab) {
  return slab->snapnum == SNAPSHOT_SLAB_NO_SNAPSHOT;
}

struct SnapshotReader {
  const char *name; /* tree_type string in the input YAML */

  /* Processing-order driver this reader feeds. */
  enum InputProcessingOrder processing_order;

  /* Open the configured dataset, validate it, and publish run-scoped metadata.
     Every validation failure aborts; nothing is repaired. */
  void (*open_run)(struct SnapshotRunInfo *info);

  /* Release every run-scoped resource acquired by open_run. */
  void (*close_run)(void);

  /* Halo count of one snapshot, without loading it. Aborts for a snapshot
     index outside [0, snapshot_count). */
  int64_t (*snapshot_halo_count)(int64_t snapnum);

  /* Load one snapshot into a reader-owned slab. The destination handle must be
     in its empty state. */
  void (*load_slab)(int64_t snapnum, struct SnapshotSlab *slab);

  /* Release a loaded slab and return the handle to its empty state. */
  void (*release_slab)(struct SnapshotSlab *slab);
};

/**
 * @brief   Resolve a tree_type string to its snapshot reader.
 * @param   name  tree_type value from the input YAML.
 * @return  The matching reader, or NULL if no snapshot format with that name is
 *          registered in this build (readers needing HDF5 are absent from
 *          non-HDF5 builds) or if name is NULL.
 */
const struct SnapshotReader *snapshot_reader_lookup(const char *name);

/* Dispatchers (snapshot/interface.c). Each verifies that the reader implements
   the hook it needs before calling it, so a reader may register with a subset
   of the hooks implemented. The reader is passed explicitly rather than read
   from a global: the configuration seam that stores it is Phase 4b Slice 4. */
void snapshot_reader_open_run(const struct SnapshotReader *reader, struct SnapshotRunInfo *info);
void snapshot_reader_close_run(const struct SnapshotReader *reader);
int64_t snapshot_reader_halo_count(const struct SnapshotReader *reader, int64_t snapnum);
void snapshot_reader_load_slab(const struct SnapshotReader *reader, int64_t snapnum,
                               struct SnapshotSlab *slab);
void snapshot_reader_release_slab(const struct SnapshotReader *reader, struct SnapshotSlab *slab);

#endif /* IO_SNAPSHOT_READER_H */
