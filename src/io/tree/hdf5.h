#ifndef TREE_HDF5_H
#define TREE_HDF5_H

/**
 * @file    tree/hdf5.h
 * @brief   L-Halo HDF5 merger-tree reader callbacks (HDF5 builds only).
 */

#ifdef HDF5
#include <hdf5.h>

/** @brief Open the HDF5 file for this partition and read its tree-count table. */
void open_partition_hdf5(int output_id);
/** @brief Load halo data for one tree from the open HDF5 file. */
void load_unit_hdf5(int unit);
/** @brief Close the open HDF5 file handle. */
void close_partition_hdf5(void);

#endif /* HDF5 */
#endif /* TREE_HDF5_H */
