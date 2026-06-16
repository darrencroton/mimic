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
 * - open_partition(): Loads unit metadata for one partition (input file)
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
 * @brief   Open a partition and load its unit (tree) metadata
 *
 * @param   output_id    Output id of the partition (filenr for per-file readers)
 *
 * This function opens the partition and loads its table of units (merger
 * trees), then initializes data structures for processing them. It:
 *
 * 1. Calls the format-specific loader through the active reader
 * 2. Allocates memory for tracking objects per unit for each output snapshot
 * 3. Initializes per-snapshot halo counters
 *
 * The format-specific loader is selected once at configuration time
 * (MimicConfig.reader); see tree/registry.c.
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

/**
 * @brief   Frees memory allocated for the merger tree table
 *
 * This function releases all memory allocated for the partition's unit
 * metadata. It frees:
 *
 * 1. Arrays tracking objects per unit for each output snapshot
 * 2. The array of first halo indices for each unit
 * 3. The array of halo counts per unit
 * 4. Format-specific resources (the open input file handle)
 *
 * The function ensures proper cleanup of resources after processing
 * is complete, preventing memory leaks.
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
 * @brief   Loads a specific unit (merger tree) into memory
 *
 * @param   unit         Index of the unit to load
 *
 * This function loads a single merger tree from the open partition and
 * allocates memory for processing its halos. It:
 *
 * 1. Calls the format-specific loader through the active reader
 * 2. Calculates the maximum number of objects for this unit
 * 3. Allocates memory for halo auxiliary data
 * 4. Allocates memory for halo data structures
 * 5. Initializes the halo auxiliary data
 *
 * The memory allocation is proportional to the number of halos in the unit,
 * ensuring efficient memory usage while providing sufficient space for the
 * objects that will be created during processing.
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

  /* Calculate MaxProcessedHalos based on number of halos with a sensible
   * minimum */
  MaxProcessedHalos = (int)(MAXHALOFAC * InputTreeNHalos[unit]);
  if (MaxProcessedHalos < MIN_HALO_ARRAY_GROWTH)
    MaxProcessedHalos = MIN_HALO_ARRAY_GROWTH;

  /* Start with a reasonable size for MaxFoFWorkspace based on tree
   * characteristics */
  MaxFoFWorkspace = INITIAL_FOF_HALOS;
  if ((int)(0.1 * MaxProcessedHalos) > MaxFoFWorkspace)
    MaxFoFWorkspace = (int)(0.1 * MaxProcessedHalos);

  /* Allocate auxiliary metadata (parallel to InputTreeHalos) */
  HaloAux = mymalloc_cat(sizeof(struct HaloAuxData) * InputTreeNHalos[unit], MEM_HALOS);
  /* Allocate permanent storage for processed halos */
  ProcessedHalos = mymalloc_cat(sizeof(struct Halo) * MaxProcessedHalos, MEM_HALOS);
  /* Zero the array to ensure uninitialized galaxy pointers are NULL */
  memset(ProcessedHalos, 0, sizeof(struct Halo) * MaxProcessedHalos);

  /* Allocate temporary workspace for FoF processing */
  FoFWorkspace = mymalloc_cat(sizeof(struct Halo) * MaxFoFWorkspace, MEM_HALOS);
  /* Zero the workspace to ensure uninitialized galaxy pointers are NULL */
  memset(FoFWorkspace, 0, sizeof(struct Halo) * MaxFoFWorkspace);

  for (i = 0; i < InputTreeNHalos[unit]; i++) {
    HaloAux[i].DoneFlag = 0;
    HaloAux[i].HaloFlag = 0;
    HaloAux[i].NHalos = 0;
  }
}

/**
 * @brief   Frees memory allocated for the current unit (merger tree)
 *
 * LIFECYCLE: Deallocation Phase
 * See globals.h for complete data structure lifecycle documentation.
 *
 * This function releases all memory allocated for the current unit
 * after it has been fully processed and output. It frees (in reverse
 * allocation order):
 *
 * 1. FoFWorkspace: Temporary workspace (MEM_HALOS)
 * 2. ProcessedHalos: Permanent storage for processed halos (MEM_HALOS)
 * 3. HaloAux: Auxiliary metadata (MEM_HALOS)
 * 4. InputTreeHalos: Raw merger tree input (MEM_TREES)
 *
 * IMPORTANT: The deallocation order (reverse of allocation) is critical for
 * proper memory management.
 *
 * This cleanup is performed after each unit is fully processed, allowing
 * the memory to be reused for the next unit.
 */
void free_unit_halos(void) {
  /* Galaxy data is owned by the per-unit galaxy pool, not by individual halos.
   * Resetting the pool reclaims every galaxy slot for the next unit in one step
   * (and avoids the per-halo free traffic that previously capped tree size). */
  galaxy_pool_reset();

  /* Free halo arrays in reverse allocation order - see load_unit() */
  myfree(FoFWorkspace);   // Temporary FoF workspace
  myfree(ProcessedHalos); // Permanent processed halo storage
  myfree(HaloAux);        // Auxiliary metadata
  myfree(InputTreeHalos); // Raw input from merger tree files
}
