#ifndef IO_SAVE_UTIL_H
#define IO_SAVE_UTIL_H

/**
 * @file    io_save_util.h
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * This file provides common functions used by both binary and HDF5 output
 * writers to prepare halo data for writing. These utilities handle per-snapshot
 * output counts and conversion from internal halo format to output format.
 */

#include "constants.h"

/**
 * @brief Counts processed halos per requested output snapshot.
 *
 * This function fills an array with the number of processed halos belonging to
 * each requested output snapshot. Writers preserve output order by scanning
 * ProcessedHalos directly.
 *
 * @param[out] OutputGalCount  An array to be filled with the number of halos
 *                             per output snapshot. Must be allocated by caller
 *                             with size [MAXSNAPS].
 *
 * @note Uses the global variables: ProcessedHalos, NumProcessedHalos,
 *       ListOutputSnaps, and MimicConfig.NOUT
 */
void count_output_halos_by_snapshot(int OutputGalCount[MAXSNAPS]);

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
