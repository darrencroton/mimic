/**
 * @file    tree/binary.c
 * @brief   Functions for reading binary merger tree files
 *
 * This file implements functionality for loading merger trees from
 * binary format files. It handles the reading of tree metadata and
 * halo data for individual trees, providing an interface to the core
 * Mimic code that is independent of the specific file format.
 *
 * Binary format trees are the traditional binary input format, consisting
 * of a simple structure with tree counts, halo counts, and arrays of
 * halo data. This format is efficient to read but less flexible than
 * newer formats like HDF5.
 *
 * Key functions:
 * - open_partition_binary(): Reads tree metadata from a binary file
 * - load_unit_binary(): Loads a specific tree's halo data
 * - close_partition_binary(): Closes the binary file
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "proto.h"
#include "globals.h"
#include "tree/interface.h"
#include "tree/binary.h"
#include "tree/reader.h"
#include "types.h"
#include "error.h"

// Local Variables //

static FILE *load_fd;

// Local Proto-Types //

// External Functions //

#ifndef MAX_BUF_SIZE
#define MAX_BUF_SIZE (3 * MAX_STRING_LEN + 40)
#endif

/**
 * @brief   Loads merger tree metadata from a binary file
 *
 * @param   output_id    Output id of the partition (the L-Halo filenr)
 *
 * This function opens and reads the metadata from a binary merger tree file.
 * It extracts:
 * 1. The number of trees in the file
 * 2. The total number of halos across all trees
 * 3. The number of halos in each individual tree
 *
 * It allocates memory for tree metadata arrays and calculates the
 * starting index of each tree in the file. This information is used
 * later when loading individual trees.
 */
void open_partition_binary(int output_id) {
  int i, totNHalos;
  char buf[MAX_BUF_SIZE + 1];

  // Open the file
  snprintf(buf, MAX_BUF_SIZE, "%s/%s.%d%s", MimicConfig.SimulationDir, MimicConfig.TreeName,
           output_id, MimicConfig.TreeExtension);
  if (!(load_fd = fopen(buf, "r"))) {
    FATAL_ERROR("Failed to open binary tree file '%s' (filenr %d)", buf, output_id);
  }

  // Read the tree metadata (legacy headerless format, host endianness assumed)
  if (fread(&Ntrees, sizeof(int), 1, load_fd) != 1) {
    FATAL_ERROR("Failed to read Ntrees from file '%s'", buf);
  }

  if (fread(&totNHalos, sizeof(int), 1, load_fd) != 1) {
    FATAL_ERROR("Failed to read totNHalos from file '%s'", buf);
  }

  DEBUG_LOG("Reading %d trees with %d total halos", Ntrees, totNHalos);

  // Allocate arrays for tree data
  InputTreeNHalos = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
  InputTreeFirstHalo = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
  // Read the number of halos per tree - using direct fread for now
  if (fread(InputTreeNHalos, sizeof(int), Ntrees, load_fd) != (size_t)Ntrees) {
    FATAL_ERROR("Failed to read tree halo counts from file '%s'", buf);
  }

  // Calculate starting indices for each tree
  if (Ntrees > 0) {
    InputTreeFirstHalo[0] = 0;
    for (i = 1; i < Ntrees; i++)
      InputTreeFirstHalo[i] = InputTreeFirstHalo[i - 1] + InputTreeNHalos[i - 1];
  }
}

/**
 * @brief   Loads a specific merger tree from a binary file
 *
 * @param   treenr    Index of the tree to load
 *
 * This function reads the halo data for a specific merger tree from
 * an already-opened binary file. It:
 * 1. Allocates memory for the halos in this tree
 * 2. Reads the halo data from the file into the allocated memory
 *
 * The function assumes that open_partition_binary() has already been
 * called to load the tree metadata and that the file is properly positioned
 * for reading.
 *
 * The halos are stored in the global Halo array for processing by the
 * Mimic framework.
 */
void load_unit_binary(int unit) {

  // must have an FD
  assert(load_fd);

  InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo) * InputTreeNHalos[unit], MEM_TREES);
  // Use direct fread to avoid our problematic wrapper
  if (fread(InputTreeHalos, sizeof(struct RawHalo), InputTreeNHalos[unit], load_fd) !=
      (size_t)InputTreeNHalos[unit]) {
    FATAL_ERROR("Failed to read halo data for tree %d", unit);
  }
}

/**
 * @brief   Closes the binary merger tree file
 *
 * This function closes the file handle for the currently open binary
 * merger tree file. It's called when all trees have been processed
 * or when switching to a different file.
 *
 * The function checks if the file is actually open before attempting
 * to close it, and sets the file handle to NULL after closing to
 * prevent multiple close attempts.
 */
void close_partition_binary(void) {
  if (load_fd) {
    fclose(load_fd);
    load_fd = NULL;
  }
}

/* L-Halo binary merger trees: the traditional headerless binary catalog. One
   partition per input file, one unit per tree; see tree/registry.c. */
const struct TreeReader LHaloBinaryReader = {
    .name = "lhalo_binary",
    .file_extension = "",
    .num_partitions = tree_partition_per_file_count,
    .partition_output_id = tree_partition_per_file_output_id,
    .open_partition = open_partition_binary,
    .load_unit = load_unit_binary,
    .close_partition = close_partition_binary,
};
// Local Functions //
