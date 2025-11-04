#ifndef IO_SAVE_UTIL_H
#define IO_SAVE_UTIL_H

/**
 * @file    io_save_util.h
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * This file provides common functions used by both binary and HDF5 output
 * writers to prepare halo data for writing. These utilities handle the
 * mapping between internal halo indices and output file indices, ensuring
 * consistent halo ordering and cross-references across output formats.
 */

#include "constants.h"

/**
 * @brief Prepares halo output ordering and updates merger pointers for a tree.
 *
 * This function calculates the final output order for all processed halos
 * across all output snapshots and updates their mergeIntoID fields accordingly.
 * It is called once per tree before writing to any output format (binary or
 * HDF5).
 *
 * The function performs three key operations:
 * 1. Allocates and initializes the OutputGalOrder workspace array
 * 2. Determines the output index for each halo in each snapshot
 * 3. Updates mergeIntoID fields to use output indices instead of internal
 * indices
 *
 * This ensures that merger pointers in output files correctly reference the
 * target halo's position in the output file, not its position in the internal
 * ProcessedHalos array.
 *
 * @param[out] OutputGalCount  An array to be filled with the number of halos
 *                             per output snapshot. Must be allocated by caller
 *                             with size [MAXSNAPS].
 *
 * @return A dynamically allocated array (OutputGalOrder) containing the
 *         output index for each processed halo. Size is NumProcessedHalos.
 *         The caller is responsible for freeing this array using myfree().
 *         Returns NULL if memory allocation fails (after calling FATAL_ERROR).
 *
 * @note Uses the global variables: ProcessedHalos, NumProcessedHalos,
 *       ListOutputSnaps, and MimicConfig.NOUT
 * @note Modifies ProcessedHalos[].mergeIntoID fields in-place
 * @note Memory is allocated using mymalloc_cat() with MEM_IO category
 */
int *prepare_output_for_tree(int OutputGalCount[MAXSNAPS]);

#endif /* #ifndef IO_SAVE_UTIL_H */
