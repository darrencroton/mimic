#ifndef IO_TREE_READ_CTREES_HDF5_H
#define IO_TREE_READ_CTREES_HDF5_H

/**
 * @file    tree/read_ctrees_hdf5.h
 * @brief   Consistent-Trees forests-HDF5 merger-tree reader (HDF5 builds only).
 *
 * The reader's open/load/close callbacks are static (reached only through the
 * registered TreeReader, see tree/registry.c). The reader and its registration
 * are compiled only when HDF5 is enabled. The scientifically-meaningful seams it
 * shares with the ASCII reader (the halo_data -> RawHalo bridge and the
 * Consistent-Trees -> L-Halo value conventions) are exposed by
 * read_ctrees_common.h and unit-tested through the always-compiled ASCII reader
 * translation unit; this header carries no separate public surface.
 */

#endif /* IO_TREE_READ_CTREES_HDF5_H */
