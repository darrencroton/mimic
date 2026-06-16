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
 * - load_tree_table(): Loads tree metadata from input files
 * - load_tree(): Loads a specific merger tree into memory
 * - free_tree_table(): Frees memory allocated for tree metadata
 * - free_halos_and_tree(): Cleans up halo and tree data structures
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
 * @brief   Loads merger tree metadata for one input file
 *
 * @param   filenr       Current file number being processed
 *
 * This function loads the table of merger trees from the specified file
 * and initializes data structures for processing these trees. It:
 *
 * 1. Calls the format-specific loader through the active reader
 * 2. Allocates memory for tracking objects per tree for each output snapshot
 * 3. Initializes per-snapshot halo counters
 *
 * The format-specific loader is selected once at configuration time
 * (MimicConfig.reader); see tree/registry.c.
 */
void load_tree_table(int filenr) {
  int i, n;

  MimicConfig.reader->load_tree_table(filenr);

  for (n = 0; n < MimicConfig.NOUT; n++) {
    InputHalosPerSnap[n] = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
    for (i = 0; i < Ntrees; i++)
      InputHalosPerSnap[n][i] = 0;

    TotHalosPerSnap[n] = 0;
  }
}

/**
 * @brief   Frees memory allocated for the merger tree table
 *
 * This function releases all memory allocated for the merger tree metadata.
 * It frees:
 *
 * 1. Arrays tracking objects per tree for each output snapshot
 * 2. The array of first halo indices for each tree
 * 3. The array of halo counts per tree
 * 4. Format-specific resources (the open input file handle)
 *
 * The function ensures proper cleanup of resources after processing
 * is complete, preventing memory leaks.
 */
void free_tree_table(void) {
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
  MimicConfig.reader->close_file();
}

/**
 * @brief   Loads a specific merger tree into memory
 *
 * @param   treenr       Index of the tree to load
 *
 * This function loads a single merger tree from the input file and allocates
 * memory for processing its halos. It:
 *
 * 1. Calls the format-specific loader through the active reader
 * 2. Calculates the maximum number of objects for this tree
 * 3. Allocates memory for halo auxiliary data
 * 4. Allocates memory for halo data structures
 * 5. Initializes the halo auxiliary data
 *
 * The memory allocation is proportional to the number of halos in the tree,
 * ensuring efficient memory usage while providing sufficient space for the
 * objects that will be created during processing.
 */
void load_tree(int treenr) {
  int32_t i;

  MimicConfig.reader->load_tree(treenr);

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
  MaxProcessedHalos = (int)(MAXHALOFAC * InputTreeNHalos[treenr]);
  if (MaxProcessedHalos < MIN_HALO_ARRAY_GROWTH)
    MaxProcessedHalos = MIN_HALO_ARRAY_GROWTH;

  /* Start with a reasonable size for MaxFoFWorkspace based on tree
   * characteristics */
  MaxFoFWorkspace = INITIAL_FOF_HALOS;
  if ((int)(0.1 * MaxProcessedHalos) > MaxFoFWorkspace)
    MaxFoFWorkspace = (int)(0.1 * MaxProcessedHalos);

  /* Allocate auxiliary metadata (parallel to InputTreeHalos) */
  HaloAux = mymalloc_cat(sizeof(struct HaloAuxData) * InputTreeNHalos[treenr], MEM_HALOS);
  /* Allocate permanent storage for processed halos */
  ProcessedHalos = mymalloc_cat(sizeof(struct Halo) * MaxProcessedHalos, MEM_HALOS);
  /* Zero the array to ensure uninitialized galaxy pointers are NULL */
  memset(ProcessedHalos, 0, sizeof(struct Halo) * MaxProcessedHalos);

  /* Allocate temporary workspace for FoF processing */
  FoFWorkspace = mymalloc_cat(sizeof(struct Halo) * MaxFoFWorkspace, MEM_HALOS);
  /* Zero the workspace to ensure uninitialized galaxy pointers are NULL */
  memset(FoFWorkspace, 0, sizeof(struct Halo) * MaxFoFWorkspace);

  for (i = 0; i < InputTreeNHalos[treenr]; i++) {
    HaloAux[i].DoneFlag = 0;
    HaloAux[i].HaloFlag = 0;
    HaloAux[i].NHalos = 0;
  }
}

/**
 * @brief   Frees memory allocated for the current merger tree
 *
 * LIFECYCLE: Deallocation Phase
 * See globals.h for complete data structure lifecycle documentation.
 *
 * This function releases all memory allocated for the current merger tree
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
 * This cleanup is performed after each tree is fully processed, allowing
 * the memory to be reused for the next tree.
 */
void free_halos_and_tree(void) {
  /* Galaxy data is owned by the per-tree galaxy pool, not by individual halos.
   * Resetting the pool reclaims every galaxy slot for the next tree in one step
   * (and avoids the per-halo free traffic that previously capped tree size). */
  galaxy_pool_reset();

  /* Free halo arrays in reverse allocation order - see load_tree() */
  myfree(FoFWorkspace);   // Temporary FoF workspace
  myfree(ProcessedHalos); // Permanent processed halo storage
  myfree(HaloAux);        // Auxiliary metadata
  myfree(InputTreeHalos); // Raw input from merger tree files
}
