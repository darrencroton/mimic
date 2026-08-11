/**
 * @file    tree/interface.c
 * @brief   Functions for loading and managing merger trees
 *
 * This file contains the core functionality for loading merger trees from
 * various file formats and managing the tree data in memory. It serves as a
 * central hub for different tree file formats (binary, HDF5) and handles the
 * allocation/deallocation of tree-related data structures.
 *
 * Key functions:
 * - open_partition(): Loads unit metadata for one output partition
 * - load_unit(): Loads a specific merger tree into memory
 * - close_partition(): Frees partition metadata and closes the input file
 * - free_unit_halos(): Cleans up the current unit's halo data structures
 * - tree_partition_per_file_*(): Per-file partition enumeration shared by the
 *   L-Halo readers
 *
 * The code supports different tree formats through a plugin architecture,
 * with format-specific implementations in the io/ directory.
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
#include "galaxy_pool.h"
#include "globals.h"
#include "types.h"
#include "proto.h"
#include "tree/interface.h"
#include "tree/reader.h"
#include "error.h"

/**
 * @brief   Open a partition and load its unit (tree) metadata.
 * @param   output_id   Output id of the partition.
 *
 * Dispatches to the format-specific reader (MimicConfig.reader, see tree/registry.c),
 * then allocates and zeroes per-snapshot halo counters (InputHalosPerSnap, TotHalosPerSnap).
 */
void open_partition(int output_id) {
  int i, n;

  MimicConfig.reader->open_partition(output_id);

  for (n = 0; n < MimicConfig.NOUT; n++) {
    InputHalosPerSnap[n] = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
    for (i = 0; i < Ntrees; i++)
      InputHalosPerSnap[n][i] = 0;

    TotHalosPerSnap[n] = 0;
  }
}

/**
 * @brief   Per-file partition model shared by the L-Halo readers
 *
 * One partition per input file across FirstFile..LastFile, with the output id
 * equal to the file number. num_partitions returns the full count; the driver
 * applies the MPI stride over the partition index.
 */
int tree_partition_per_file_count(void) { return MimicConfig.LastFile - MimicConfig.FirstFile + 1; }

int tree_partition_per_file_output_id(int partition) { return MimicConfig.FirstFile + partition; }

static void format_tree_partition_path(const struct TreeReader *reader, char *tree_path,
                                       size_t tree_path_size, int output_id) {
  if (reader->format_partition_path != NULL) {
    reader->format_partition_path(tree_path, tree_path_size, output_id);
  } else {
    snprintf(tree_path, tree_path_size, "%s/%s.%d%s", MimicConfig.SimulationDir,
             MimicConfig.TreeName, output_id, MimicConfig.TreeExtension);
  }
}

int tree_partition_per_file_exists(int partition) {
  FILE *fd;
  char tree_path[3 * MAX_STRING_LEN + 26];
  const int output_id = tree_partition_per_file_output_id(partition);

  format_tree_partition_path(MimicConfig.reader, tree_path, sizeof(tree_path), output_id);
  if (!(fd = fopen(tree_path, "r"))) {
    return 0;
  }

  fclose(fd);
  return 1;
}

/**
 * @brief   Free partition metadata and close the input file.
 *
 * Releases InputHalosPerSnap, InputTreeFirstHalo, InputTreeNHalos (in reverse
 * allocation order), then calls the reader's close_partition hook.
 */
void close_partition(void) {
  int n;

  for (n = MimicConfig.NOUT - 1; n >= 0; n--) {
    myfree(InputHalosPerSnap[n]);
    InputHalosPerSnap[n] = NULL;
  }

  myfree(InputTreeFirstHalo);
  InputTreeFirstHalo = NULL;

  myfree(InputTreeNHalos);
  InputTreeNHalos = NULL;

  /* Close the open input file handle. */
  MimicConfig.reader->close_partition();
}

/**
 * @brief   Load one unit (merger tree) into memory and allocate processing structures.
 * @param   unit   Unit index within the open partition.
 *
 * After the format-specific loader populates InputTreeHalos, allocates HaloAux,
 * ProcessedHalos, and FoFWorkspace. See the LIFECYCLE comment below and globals.h
 * for the complete data-structure lifetime.
 */
void load_unit(int unit) {
  int32_t i;

  MimicConfig.reader->load_unit(unit);

  /*
   * LIFECYCLE: Allocation Phase
   * See globals.h for complete data structure lifecycle documentation.
   *
   * At this point:
   * - InputTreeHalos has been populated by format-specific loader
   * - We now allocate the processing structures:
   *   1. HaloAux: Parallel metadata for each InputTreeHalo
   *   2. ProcessedHalos: Storage for processed halos
   *   3. FoFWorkspace: Temporary workspace for FoF processing (grows
   * dynamically)
   *
   * All allocations use MEM_HALOS except InputTreeHalos (MEM_TREES).
   */

  MaxProcessedHalos = (int64_t)MAXHALOFAC * InputTreeNHalos[unit];
  if (MaxProcessedHalos < MIN_HALO_ARRAY_GROWTH)
    MaxProcessedHalos = MIN_HALO_ARRAY_GROWTH;

  MaxFoFWorkspace = INITIAL_FOF_HALOS;
  const int fof_from_processed = narrow_int64_to_int_checked(
      (int64_t)(0.1 * (double)MaxProcessedHalos), "initial MaxFoFWorkspace estimate");
  if (fof_from_processed > MaxFoFWorkspace)
    MaxFoFWorkspace = fof_from_processed;

  HaloAux = mymalloc_cat(sizeof(struct HaloAuxData) * InputTreeNHalos[unit], MEM_HALOS);

  ProcessedHalos = mymalloc_cat((size_t)MaxProcessedHalos * sizeof(struct Halo), MEM_HALOS);
  memset(ProcessedHalos, 0,
         (size_t)MaxProcessedHalos * sizeof(struct Halo)); /* NULL galaxy pointers */

  FoFWorkspace = mymalloc_cat(sizeof(struct Halo) * MaxFoFWorkspace, MEM_HALOS);
  memset(FoFWorkspace, 0, sizeof(struct Halo) * MaxFoFWorkspace); /* NULL galaxy pointers */

  for (i = 0; i < InputTreeNHalos[unit]; i++) {
    HaloAux[i].DoneFlag = 0;
    HaloAux[i].HaloFlag = 0;
    HaloAux[i].NHalos = 0;
  }
}

/**
 * @brief   Free the current unit's halo and processing memory (LIFECYCLE: Deallocation Phase).
 *
 * Frees in reverse allocation order: FoFWorkspace, ProcessedHalos, HaloAux,
 * InputTreeHalos. Galaxy data is reclaimed via galaxy_pool_reset(pool) first,
 * unless `pool` is NULL (the caller allocated no galaxies, so there is nothing
 * to reset). See globals.h for the complete data-structure lifetime.
 */
void free_unit_halos(struct GalaxyPool *pool) {
  /* Galaxy data is owned by the caller's galaxy pool, not by individual halos.
   * Resetting the pool reclaims every galaxy slot for the next unit in one step
   * (and avoids the per-halo free traffic that previously capped tree size). */
  if (pool != NULL) {
    galaxy_pool_reset(pool);
  }

  /* Reverse allocation order — see load_unit() */
  myfree(FoFWorkspace);
  myfree(ProcessedHalos);
  myfree(HaloAux);
  myfree(InputTreeHalos);
}
