#ifndef GLOBALS_H
#define GLOBALS_H

#include "constants.h"
#include "types.h"
#include <stdio.h>

/* Global configuration structure */
extern struct MimicConfig MimicConfig;

/* MPI rank and task count. Declared for every build so the per-task tree
   readers can split work uniformly: serial builds leave these at their zero
   defaults (ThisTask == 0, NTask == 0) and readers treat NTask <= 0 as one task.
   Only MPI builds set them (in main()). */
extern int ThisTask, NTask;
#ifdef MPI
extern int nodeNameLen;
extern char *ThisNode;
#endif

/*
 * Halo Data Structure Lifecycle Documentation
 * ============================================
 *
 * Mimic uses a three-tier architecture for halo tracking through merger trees.
 * The tree driver gathers already-processed progenitor galaxies, the shared
 * inheritance service deep-copies them into FoFWorkspace, physics mutates the
 * workspace in place, and output marshalling transfers surviving workspace
 * entries into a driver-owned output buffer.
 *
 * Data Flow: InputTreeHalos → FoFWorkspace → output buffer
 *
 * 1. InputTreeHalos (struct RawHalo*) - IMMUTABLE INPUT
 *    - Source: Read from merger tree files (binary or HDF5)
 *    - Lifetime: Per-tree (allocated in load_unit(), freed in
 * free_unit_halos())
 *    - Ownership: Read-only reference to simulation data
 *    - Size: InputTreeNHalos[treenr] elements
 *    - Purpose: Provides immutable snapshot of halo properties from simulation
 *    - Memory: Allocated via mymalloc_cat(..., MEM_TREES)
 *
 * 2. FoFWorkspace (struct Halo*) - TEMPORARY PROCESSING
 *    - Source: Created during FoF processing in build_halo_tree()
 *    - Lifetime: Per-tree, grows dynamically during processing
 *    - Ownership: Temporary working space, contents copied to ProcessedHalos
 *    - Size: MaxFoFWorkspace elements (grows as needed via myrealloc_cat)
 *    - Purpose: Accumulates halos during recursive tree building
 *    - Memory: Allocated via mymalloc_cat(..., MEM_HALOS)
 *
 * 3. ProcessedHalos (struct Halo*) - TREE-DRIVER OUTPUT BUFFER
 *    - Source: Final halos copied from FoFWorkspace after physics execution
 *    - Lifetime: Per-tree (allocated in load_unit(), freed in
 * free_unit_halos())
 *    - Ownership: Tree-driver output buffer, indexed by NumProcessedHalos
 *    - Size: MaxProcessedHalos elements (initial estimate; grows via myrealloc_cat
 *      if orphan halos cause output count to exceed the initial allocation)
 *    - Purpose: Stores all processed halos for current tree until output
 *    - Memory: Allocated via mymalloc_cat(..., MEM_HALOS)
 *
 * 4. HaloAux (struct HaloAuxData*) - PROCESSING METADATA
 *    - Source: Auxiliary data for tracking processing state
 *    - Lifetime: Per-tree (parallel to InputTreeHalos)
 *    - Ownership: Processing metadata, indexed by InputTreeHalos indices
 *    - Size: InputTreeNHalos[treenr] elements
 *    - Purpose: Tracks DoneFlag, HaloFlag, NHalos for each input halo
 *    - Memory: Allocated via mymalloc_cat(..., MEM_HALOS)
 *
 * Allocation Pattern (per tree):
 *   load_unit():
 *     InputTreeHalos = mymalloc_cat(InputTreeNHalos[treenr] * sizeof(RawHalo), MEM_TREES)
 *     HaloAux        = mymalloc_cat(InputTreeNHalos[treenr] * sizeof(HaloAuxData), MEM_HALOS)
 *     ProcessedHalos = mymalloc_cat(MaxProcessedHalos * sizeof(Halo), MEM_HALOS)
 *     FoFWorkspace   = mymalloc_cat(MaxFoFWorkspace   * sizeof(Halo), MEM_HALOS)
 *
 *   During tree processing:
 *     FoFWorkspace   may grow via myrealloc_cat (see ensure_fof_workspace_capacity)
 *     ProcessedHalos may grow via myrealloc_cat (see marshal_workspace_to_output_buffer);
 *       build_halo_tree syncs ProcessedHalos and MaxProcessedHalos back from the
 *       OutputBuffer struct after each marshal call
 *
 *   free_unit_halos():
 *     galaxy_pool_reset()   // reclaim all galaxy slots for the next tree
 *     myfree(FoFWorkspace)   // frees the final (possibly grown) pointer
 *     myfree(ProcessedHalos) // frees the final (possibly grown) pointer
 *     myfree(HaloAux)
 *     myfree(InputTreeHalos)
 *
 * IMPORTANT: GalaxyData is owned by the per-tree galaxy pool (see galaxy_pool.h),
 * not by individual halos. Inheritance allocates each workspace galaxy from the
 * pool; the output-buffer marshaller transfers surviving halos (and their galaxy
 * pointers) into ProcessedHalos by struct copy; the pool's slots stay valid
 * because chunks never move. No per-halo galaxy frees occur — free_unit_halos()
 * resets the pool in one step, which is why the same galaxy pointer can be held
 * by both a FoFWorkspace and a ProcessedHalos slot without any double-free risk.
 */

/* halo data pointers */
extern struct Halo *FoFWorkspace, *ProcessedHalos;
extern struct RawHalo *InputTreeHalos;
extern struct HaloAuxData *HaloAux;

/* runtime file information */
extern int Ntrees;            /* number of trees in current file  */
extern int NumProcessedHalos; /* Total number of halos stored for current tree */
extern int MaxProcessedHalos; /* Maximum number of halos allowed for current tree */
extern int MaxFoFWorkspace;

/* halo information */
extern int TotHalosPerSnap[ABSOLUTEMAXSNAPS];
extern int *InputHalosPerSnap[ABSOLUTEMAXSNAPS];
extern int *InputTreeNHalos;
extern int *InputTreeFirstHalo;

/*
 * Lookback-time table (internal time units), indexed by snapshot number.
 *
 * INVARIANT: Age points one element past the start of its allocation
 * (Age = Age_base + 1). Age[snap] is the lookback time to snapshot `snap`,
 * and the extra leading slot (Age_base[0], addressable as Age[-1]) holds the
 * lookback time to INITIAL_REDSHIFT (recombination). Free with Age_base,
 * never with Age.
 */
extern double *Age;
extern double *Age_base;

/* tree and file information */
extern int TreeID;
extern int FileNum;

/* HDF5 specific globals */
#ifdef HDF5
#include <hdf5.h>
extern size_t HDF5_dst_size;
extern size_t *HDF5_dst_offsets;
extern size_t *HDF5_dst_sizes;
extern const char **HDF5_field_names;
extern hid_t *HDF5_field_types;
extern int HDF5_n_props;
extern hid_t HDF5_current_file_id; /* Keep file open during processing */
#endif

#endif /* #ifndef GLOBALS_H */
