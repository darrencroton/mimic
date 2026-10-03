#ifndef IO_HORIZONTAL_READER_H
#define IO_HORIZONTAL_READER_H

#include <stddef.h>
#include <stdint.h>

/* enum InputProcessingOrder and input_processing_order_name() are shared with
   the vertical side rather than duplicated: the processing order is a property of
   the run, not of one reader family. */
#include "vertical/reader.h"

/**
 * @file    horizontal/reader.h
 * @brief   Format-agnostic horizontal input reader interface.
 *
 * The horizontal front end reads one snapshot at a time: the working set
 * of a run is one snapshot's halo population instead of one forest's history.
 * See convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md (versions 2 and 3) for the on-disk
 * contracts this interface consumes.
 *
 * This is deliberately a second, small vtable rather than a widening of
 * struct VerticalReader, whose thirteen hooks are partition/unit-shaped and carry no
 * meaning for horizontal input. Readers register in horizontal/registry.c
 * and are dispatched through the thin wrappers below (horizontal/interface.c),
 * which verify at each point of use that the hook they need is implemented.
 *
 * Lifecycle:
 *   open_run  -> [ load_slab / release_slab ]* -> close_run
 * with snapshot_halo_count queryable between open_run and close_run.
 */

/* Populated per simulation package from halo_properties.yaml
   (src/include/generated/raw_halo_defs.h). */
struct RawHalo;

/**
 * Run-scoped metadata published by open_run.
 *
 * slab_row_bytes is the reader's own account of its per-halo slab footprint. The
 * driver relies on it for retention accounting (the memory ceiling and the
 * per-generation report), so it must equal exactly what load_slab allocates per
 * row; the fixture tests measure it against the allocator's MEM_TREES category.
 * It is the sum of the element widths: the allocator's rounding of each block up
 * to 8 bytes (the three int32 target-snapshot columns of an odd row count, 4 B
 * each) is not part of it.
 */
struct HorizontalRunInfo {
  int64_t snapshot_count;          /* number of snapshots in the run */
  int32_t format_version;          /* on-disk contract version of the dataset */
  int32_t links_adjacent;          /* 1: every non-null Descendant targets snapshot N+1 (always 1
                                      for version 2); 0: version 3 gaps are present */
  int64_t slab_row_bytes;          /* bytes load_slab allocates per halo across every
                                      reader-owned slab array (the raw record, the two
                                      identity columns and, for version 3, the three
                                      target-snapshot columns and SourceHaloID) */
  int64_t n_forests_total;         /* run-scoped forest count (identity bound) */
  int64_t max_halo_rank_in_forest; /* run-scoped maximum rank (identity bound) */
};

/**
 * Empty-dataset sentinel for the run-scoped identity bounds. The converter
 * emits this pair when no snapshot in the dataset contains a halo, so there is
 * no forest count and no rank to report (`compute_identity` and `_max_rank` in
 * convert/mimic-convert/links.py).
 */
#define HORIZONTAL_EMPTY_N_FORESTS ((int64_t)0)
#define HORIZONTAL_EMPTY_MAX_RANK ((int64_t)-1)

/**
 * The only accepted input.tree_name for a horizontal configuration.
 *
 * This is a filename convention fixed by format_version 1, not a user-supplied
 * pattern: configuration copies arbitrary text into MimicConfig.TreeName, so
 * accepting anything else would either pass configured text to a printf-family
 * format argument or silently mismatch the files on disk. Readers build their
 * paths from a fixed internal format string; configuration only checks that the
 * declared convention is the one the reader implements.
 */
#define HORIZONTAL_READER_TREE_NAME "snapshot_%03d.h5"

/** Sentinel snapnum marking a slab that holds no loaded snapshot. */
#define SNAPSHOT_SLAB_NO_SNAPSHOT ((int64_t)-1)

/** Static initializer for the empty slab state. */
#define SNAPSHOT_SLAB_INIT {SNAPSHOT_SLAB_NO_SNAPSHOT, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

/**
 * One snapshot's halo population, owned by the reader between load_slab and
 * release_slab. Counts and indices are int64_t throughout: production slabs
 * reach hundreds of millions of halos, so the vertical driver's int idiom does not
 * carry over.
 *
 * forest_index/halo_rank_in_forest are the UniqueGalaxyID identity components
 * the frozen format carries explicitly (convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md).
 * They live here rather than as struct RawHalo members or halo_properties.yaml
 * catalog fields: they are horizontal-format identity metadata, not catalog halo
 * properties, and this reader compiles under every selected simulation package
 * (Makefile:112), so it must not depend on RawHalo members that only one
 * package's catalog would declare.
 *
 * The three target-snapshot columns and source_halo_id are version 3 format
 * metadata held the same way. Each *_snapshot entry
 * names the snapshot whose slab the matching RawHalo link indexes into, or is -1
 * exactly when that link is -1; load_slab has already validated both. They are
 * NULL for a version 2 slab, whose links are implicitly N+1 (Descendant), N-1
 * (FirstProgenitor) and N-1 (NextProgenitor), and for an empty snapshot.
 */
struct SnapshotSlab {
  int64_t snapnum;                    /* loaded snapshot, or SNAPSHOT_SLAB_NO_SNAPSHOT */
  int64_t nhalos;                     /* halos in this slab */
  struct RawHalo *halos;              /* [nhalos], reader-owned */
  int64_t *forest_index;              /* [nhalos], reader-owned */
  int64_t *halo_rank_in_forest;       /* [nhalos], reader-owned */
  int32_t *descendant_snapshot;       /* [nhalos], reader-owned; version 3 only */
  int32_t *first_progenitor_snapshot; /* [nhalos], reader-owned; version 3 only */
  int32_t *next_progenitor_snapshot;  /* [nhalos], reader-owned; version 3 only */
  int64_t *source_halo_id;            /* [nhalos], reader-owned; version 3 only */
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

struct HorizontalReader {
  const char *name; /* tree_type string in the input YAML */

  /* Processing-order driver this reader feeds. */
  enum InputProcessingOrder processing_order;

  /* Open the configured dataset, validate it, and publish run-scoped metadata.
     Every validation failure aborts; nothing is repaired. */
  void (*open_run)(struct HorizontalRunInfo *info);

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
 * @brief   Resolve a tree_type string to its horizontal reader.
 * @param   name  tree_type value from the input YAML.
 * @return  The matching reader, or NULL if no horizontal format with that name is
 *          registered in this build (readers needing HDF5 are absent from
 *          non-HDF5 builds) or if name is NULL.
 */
const struct HorizontalReader *horizontal_reader_lookup(const char *name);

/**
 * @brief   Number of horizontal readers registered in this build.
 *
 * Zero in a non-HDF5 build. Together with horizontal_reader_at() this lets a test
 * enumerate the registered names, which is how the disjointness of the vertical and
 * horizontal name sets is asserted.
 */
size_t horizontal_reader_count(void);

/**
 * @brief   Registered horizontal reader by index.
 * @param   index  Position in [0, horizontal_reader_count()).
 * @return  The reader, or NULL when index is out of range.
 */
const struct HorizontalReader *horizontal_reader_at(size_t index);

/**
 * @brief   Are the run-scoped identity bounds encodable with this multiplier?
 * @param   info        Run metadata carrying n_forests_total and
 *                      max_halo_rank_in_forest.
 * @param   multiplier  Configured simulation.unique_galaxy_id_multiplier.
 * @return  Non-zero when the bounds are valid, zero otherwise.
 *
 * The identity bounds the frozen format requires to be checked at startup
 * (convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md, "Galaxy Identity Encoding"): every halo rank
 * must fit below the multiplier, and multiplier * (n_forests_total + 1) must fit in int64_t (the
 * + 1 reserves the encoder's forest offset). A non-positive multiplier is
 * rejected before any division is performed, so the check itself can neither
 * divide by zero nor overflow.
 *
 * A predicate rather than a validator so it is directly unit-testable; callers
 * turn a zero return into a diagnostic naming the offending values.
 */
int horizontal_identity_bounds_valid(const struct HorizontalRunInfo *info, int64_t multiplier);

/* Dispatchers (horizontal/interface.c). Each verifies that the reader implements
   the hook it needs before calling it, so a reader may register with a subset
   of the hooks implemented. The reader is passed explicitly rather than read
   from a global, so unit tests can drive any registered reader directly. */
void horizontal_reader_open_run(const struct HorizontalReader *reader,
                                struct HorizontalRunInfo *info);
void horizontal_reader_close_run(const struct HorizontalReader *reader);
int64_t horizontal_reader_halo_count(const struct HorizontalReader *reader, int64_t snapnum);
void horizontal_reader_load_slab(const struct HorizontalReader *reader, int64_t snapnum,
                                 struct SnapshotSlab *slab);
void horizontal_reader_release_slab(const struct HorizontalReader *reader,
                                    struct SnapshotSlab *slab);

#endif /* IO_HORIZONTAL_READER_H */
