/**
 * @file    io_save_util.c
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * This file implements common functions used by both binary and HDF5 output
 * writers. By centralizing this logic, we eliminate code duplication and
 * ensure consistent behavior across output formats.
 *
 * Key functions:
 * - prepare_output_for_tree(): Calculates output ordering and updates merger
 * pointers
 */

#include <stdio.h>
#include <stdlib.h>

#include "allvars.h"
#include "proto.h"
#include "output/util.h"

/**
 * @brief Prepares halo output ordering and updates merger pointers for a tree.
 *
 * See io_save_util.h for full documentation.
 */
int *prepare_output_for_tree(int OutputGalCount[MAXSNAPS]) {
  int i, n;
  int *OutputGalOrder;

  /* Allocate workspace array using tracked memory allocation */
  OutputGalOrder = (int *)mymalloc_cat(NumProcessedHalos * sizeof(int), MEM_IO);
  if (OutputGalOrder == NULL) {
    FATAL_ERROR("Memory allocation failed for OutputGalOrder array (%d "
                "elements, %zu bytes)",
                NumProcessedHalos, NumProcessedHalos * sizeof(int));
    return NULL; /* Never reached due to FATAL_ERROR, but satisfies static
                    analysis */
  }

  /* Initialize the output halo count and order arrays */
  for (i = 0; i < MAXSNAPS; i++)
    OutputGalCount[i] = 0;
  for (i = 0; i < NumProcessedHalos; i++)
    OutputGalOrder[i] = -1;

  /*
   * Build the output ordering map.
   * For each output snapshot, iterate through all processed halos and assign
   * sequential output indices to halos belonging to that snapshot.
   *
   * Complexity: O(NumProcessedHalos × NOUT)
   *
   * Performance note: For large trees (100K+ halos) and many output snapshots
   * (20+), this can become a bottleneck. An alternative approach would be to
   * sort ProcessedHalos by SnapNum first (O(N log N)), then iterate once
   * (O(N)). The crossover point is around NOUT=17. Current implementation
   * favors simplicity for typical use cases where NOUT < 20.
   */
  for (n = 0; n < MimicConfig.NOUT; n++) {
    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum == ListOutputSnaps[n]) {
        OutputGalOrder[i] = OutputGalCount[n];
        OutputGalCount[n]++;
      }
    }
  }

  /*
   * Update merger pointers to use output indices.
   * The mergeIntoID field stores the internal index of the halo that this
   * halo merges into. We need to convert this to the output file index
   * so that readers can correctly follow merger chains.
   */
  for (i = 0; i < NumProcessedHalos; i++)
    if (ProcessedHalos[i].mergeIntoID > -1)
      ProcessedHalos[i].mergeIntoID =
          OutputGalOrder[ProcessedHalos[i].mergeIntoID];

  return OutputGalOrder;
}
