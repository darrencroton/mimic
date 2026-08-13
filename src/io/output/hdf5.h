#ifndef IO_SAVE_HDF5_H
#define IO_SAVE_HDF5_H

/**
 * @file    output/hdf5.h
 * @brief   HDF5 output writer public interface.
 */

#include "config.h"
#include "globals.h"
#include "output/util.h" /* struct OutputSnapshotSelection */
#include "types.h"

/** @brief Initialise the HDF5 field table (names, types, offsets) from generated metadata. */
void calc_hdf5_props(void);
/** @brief Free the HDF5 field table metadata arrays allocated by calc_hdf5_props(). */
void free_hdf5_ids(void);

/** @brief Create one HDF5 file with empty tables for the given snapshot selection. */
void prep_hdf5_file(char *fname, struct OutputSnapshotSelection selection);
/** @brief Create and open this filenr's HDF5 output file; leaves it open for writes. */
void open_hdf5_output_file(int filenr, struct OutputSnapshotSelection selection);

/** @brief Buffer ProcessedHalos into the cross-tree write buffers for the snapshots in the
 *         supplied selection. */
void save_halos_hdf5(int filenr, int tree, struct HaloInputView view,
                     struct OutputSnapshotSelection selection);
/** @brief Flush and release the selection's per-snapshot write buffers. */
void flush_hdf5_buffers(int filenr, struct OutputSnapshotSelection selection);
/** @brief Append a prepared batch of HaloOutput records to the open HDF5 file. */
void write_hdf5_halo_batch(struct HaloOutput *halo_batch, int num_halos, int n, int filenr);

/** @brief Write per-snapshot count attributes and per-tree halo count dataset. */
void write_hdf5_attrs(int n, int filenr);

/** @brief Build the run-level master HDF5 file with external links into all per-filenr files. */
void write_master_file(void);

#endif /* IO_SAVE_HDF5_H */
