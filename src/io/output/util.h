#ifndef IO_SAVE_UTIL_H
#define IO_SAVE_UTIL_H

/**
 * @file    output/util.h
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * This file provides common functions used by both binary and HDF5 output
 * writers to prepare halo data for writing. These utilities handle per-snapshot
 * output counts and conversion from internal halo format to output format.
 */

#include <stddef.h>

#include "constants.h"
/* struct HaloInputView is passed by value below, so the type must be complete
 * here rather than relying on every includer reaching types.h first. */
#include "types.h"

/**
 * @brief   Build the path of one binary output file (one file per snapshot per filenr)
 *
 * Single home for the binary output naming scheme: <dir>/<base>_z<zzz>_<filenr>.
 * Fatal if the path does not fit in @p size.
 */
void output_path_binary(char *buf, size_t size, int filenr, int snap_index);

/**
 * @brief   Build the path of one HDF5 output file (one file per filenr)
 *
 * Single home for the HDF5 output naming scheme: <dir>/<base>_<NNN>.hdf5.
 * Fatal if the path does not fit in @p size.
 */
void output_path_hdf5(char *buf, size_t size, int filenr);

/**
 * @brief   A partition's selection of requested output snapshots.
 *
 * count is the number of entries carried; indices holds that many ascending
 * indices into MimicConfig.ListOutputSnaps naming which requested output
 * snapshots this partition writes.
 */
struct OutputSnapshotSelection {
  int count;
  const int *indices;
};

/**
 * @brief   Create/initialize this filenr's output files (dispatches on OutputFormat)
 *
 * Binary: creates one empty file per requested snapshot. HDF5: creates the
 * per-filenr file with tables for @p selection's snapshots and leaves it open
 * for writing (HDF5_current_file_id).
 */
void prepare_output_files(int filenr, struct OutputSnapshotSelection selection);

/**
 * @brief   Increment per-file halo counters with the 32-bit output guard
 *
 * Always increments the snapshot total (TotHalosPerSnap). Tree-ordered runs
 * additionally increment the per-tree count (InputHalosPerSnap), which only
 * the tree reader allocates; snapshot-ordered runs never touch it. Fatal
 * before incrementing a counter that would overflow its 32-bit output
 * contract.
 */
void output_increment_halo_counters_checked(int filenr, int snap_index, int snap_num, int tree);

/**
 * @brief   Driver-neutral view onto the run's output partitions.
 *
 * Output writers under src/io/output/ enumerate, check existence of, and
 * name the format of output partitions through this source instead of
 * reading the active tree reader pointer directly, so the same writer code
 * serves both the tree-ordered and snapshot-ordered drivers. Populated by
 * get_output_partition_source() (src/core/tree_driver.c), which resolves the
 * active processing order and constructs the matching source: the tree-side
 * construction wraps the configured tree reader's partition hooks (including
 * a prepare_run/teardown_run pass-through); the snapshot side constructs its
 * own hooks directly, taking the format name from the resolved snapshot
 * reader so a second registered reader records its own name in provenance.
 * Each partition also names the requested output snapshots it carries via
 * partition_snapshots(); a tree-ordered partition always carries every
 * requested snapshot, and the snapshot side does today too, though nothing
 * here requires it to stay that way.
 */
struct OutputPartitionSource {
  /** Number of output partitions this run produces. */
  int (*num_partitions)(void);
  /** Output id (used in output file names) for a given partition index. */
  int (*partition_output_id)(int partition);
  /** Whether a given partition's input is present and should be linked. */
  int (*partition_exists)(int partition);
  /** Requested output snapshots carried by a given partition index. */
  struct OutputSnapshotSelection (*partition_snapshots)(int partition);
  /** Optional run-scoped lifecycle hooks; NULL if the source keeps no state. */
  void (*prepare_run)(void);
  void (*teardown_run)(void);
  /** Resolved reader-format name recorded in output provenance. */
  const char *format_name;
};

/**
 * @brief   Resolve this run's output partition source from the active processing order.
 *
 * Defined beside the driver dispatch in src/core/tree_driver.c.
 */
struct OutputPartitionSource get_output_partition_source(void);

/**
 * @brief Converts internal halo structure to output format
 *
 * This function transforms the internal halo representation (struct Halo)
 * to the output format (struct HaloOutput). It includes auto-generated code to
 * copy or convert all property values from the internal representation.
 *
 * This function is format-agnostic and used by both binary and HDF5 output
 * writers, ensuring consistent halo conversion across all output formats.
 *
 * @param   view      Input view over the raw halos this record was built from
 * @param   g         Pointer to the internal halo tracking structure (const)
 * @param   o         Pointer to the output halo structure to be filled
 *
 * @note Includes auto-generated code from copy_to_output.inc
 */
void prepare_halo_for_output(struct HaloInputView view, const struct Halo *g, struct HaloOutput *o);

#endif /* #ifndef IO_SAVE_UTIL_H */
