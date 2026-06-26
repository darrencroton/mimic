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
 * @brief   Create/initialize this filenr's output files (dispatches on OutputFormat)
 *
 * Binary: creates one empty file per requested snapshot. HDF5: creates the
 * per-filenr file with its snapshot tables and leaves it open for writing
 * (HDF5_current_file_id).
 */
void prepare_output_files(int filenr);

/**
 * @brief   Increment per-file halo counters with the 32-bit output guard
 *
 * Output headers and HDF5 attributes currently store per-snapshot and per-tree
 * halo counts as int. Fatal before incrementing if the next record would
 * overflow that contract.
 */
void output_increment_halo_counters_checked(int filenr, int snap_index, int snap_num, int tree);

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
 * @param   g         Pointer to the internal halo tracking structure (const)
 * @param   o         Pointer to the output halo structure to be filled
 *
 * @note Includes auto-generated code from copy_to_output.inc
 */
void prepare_halo_for_output(const struct Halo *g, struct HaloOutput *o);

#endif /* #ifndef IO_SAVE_UTIL_H */
