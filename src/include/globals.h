#ifndef GLOBALS_H
#define GLOBALS_H

#include "constants.h"
#include "types.h"
#include <stdio.h>

/* Global configuration structure */
extern struct MimicConfig MimicConfig;

#ifdef MPI
extern int ThisTask, NTask, nodeNameLen;
extern char *ThisNode;
#endif

/*
 * Halo Data Structure Lifecycle Documentation
 * ============================================
 *
 * Mimic uses a three-tier architecture for halo tracking through merger trees.
 * Understanding this flow is critical for Phase 1-2 transformations.
 *
 * Data Flow: InputTreeHalos → FoFWorkspace → ProcessedHalos
 *
 * 1. InputTreeHalos (struct RawHalo*) - IMMUTABLE INPUT
 *    - Source: Read from merger tree files (binary or HDF5)
 *    - Lifetime: Per-tree (allocated in load_tree(), freed in
 * free_halos_and_tree())
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
 * 3. ProcessedHalos (struct Halo*) - PERMANENT STORAGE
 *    - Source: Final halos copied from FoFWorkspace after tree processing
 *    - Lifetime: Per-tree (allocated in load_tree(), freed in
 * free_halos_and_tree())
 *    - Ownership: Master copy for output, indexed by NumProcessedHalos
 *    - Size: MaxProcessedHalos elements (initial estimate, grows via
 * myrealloc_cat)
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
 *   load_tree():
 *     InputTreeHalos = mymalloc_cat(InputTreeNHalos[treenr] * sizeof(RawHalo),
 * MEM_TREES) HaloAux = mymalloc_cat(InputTreeNHalos[treenr] *
 * sizeof(HaloAuxData), MEM_HALOS) ProcessedHalos =
 * mymalloc_cat(MaxProcessedHalos * sizeof(Halo), MEM_HALOS) FoFWorkspace =
 * mymalloc_cat(MaxFoFWorkspace * sizeof(Halo), MEM_HALOS)
 *
 *   free_halos_and_tree():
 *     myfree(FoFWorkspace)
 *     myfree(ProcessedHalos)
 *     myfree(HaloAux)
 *     myfree(InputTreeHalos)
 *
 * IMPORTANT: Phase 1-2 transformations will add galaxy properties to the Halo
 * struct. The lifecycle pattern established here must be preserved to ensure
 * proper memory management as the structure grows.
 */

/* halo data pointers */
extern struct Halo *FoFWorkspace, *ProcessedHalos;
extern struct RawHalo *InputTreeHalos;
extern struct HaloAuxData *HaloAux;

/* runtime file information */
extern int Ntrees; /* number of trees in current file  */
extern int
    NumProcessedHalos; /* Total number of halos stored for current tree */
extern int
    MaxProcessedHalos; /* Maximum number of halos allowed for current tree */
extern int MaxFoFWorkspace;
extern int HaloCounter; /* unique halo ID for main progenitor line in tree */

/* halo information */
extern int TotHalos;
extern int TotHalosPerSnap[ABSOLUTEMAXSNAPS];
extern int *InputHalosPerSnap[ABSOLUTEMAXSNAPS];
extern int *FirstHaloInSnap;
extern int *InputTreeNHalos;
extern int *InputTreeFirstHalo;

/* parameter handling globals */
extern int NParam;
extern char ParamTag[MAXTAGS][50];
extern int ParamID[MAXTAGS];
extern void *ParamAddr[MAXTAGS];

/* units */
extern double UnitLength_in_cm, UnitTime_in_s, UnitVelocity_in_cm_per_s,
    UnitMass_in_g, RhoCrit, UnitPressure_in_cgs, UnitDensity_in_cgs,
    UnitCoolingRate_in_cgs, UnitEnergy_in_cgs, UnitTime_in_Megayears, G, Hubble;

/* output snapshots - kept for backward compatibility */
extern int ListOutputSnaps[ABSOLUTEMAXSNAPS];
extern double ZZ[ABSOLUTEMAXSNAPS];
extern double AA[ABSOLUTEMAXSNAPS];
extern double *Age;
extern double *Age_base; /* Original allocation pointer for Age (fix for issue 1.2.1) */
extern int MAXSNAPS;
extern int NOUT;
extern int Snaplistlen;

/* Random number generator removed - not used in computation */

/* tree and file information */
extern int TreeID;
extern int FileNum;

/* HDF5 specific globals */
#ifdef HDF5
#include <hdf5.h>
extern int HDF5Output;
extern char *core_output_file;
extern size_t HDF5_dst_size;
extern size_t *HDF5_dst_offsets;
extern size_t *HDF5_dst_sizes;
extern const char **HDF5_field_names;
extern hid_t *HDF5_field_types;
extern int HDF5_n_props;
extern hid_t HDF5_current_file_id; /* Keep file open during processing */
#endif

#endif /* #ifndef GLOBALS_H */
