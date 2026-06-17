/**
 * @file    allvars.c
 * @brief   Defines global variables used throughout the Mimic framework
 *
 * This file contains the definitions of all global variables used by the
 * Mimic framework. These variables fall into several categories:
 *
 * 1. Core data structures (e.g. halos, auxiliary data)
 * 2. Configuration parameters and derived values
 * 3. Simulation state variables (counts, indices, etc.)
 * 4. Physical constants and units
 * 5. File and output control variables
 *
 * Configuration parameters are stored in the MimicConfig structure.
 * Runtime simulation state is tracked via individual global variables.
 *
 * Note: This file contains only variable definitions - the declarations
 * are in globals.h and other header files.
 */

#include "config.h"
#include "globals.h"
#include "types.h"

/*  Global configuration structure */
struct MimicConfig MimicConfig;

/*  halo data  */
struct Halo *FoFWorkspace, *ProcessedHalos;

struct RawHalo *InputTreeHalos;

/*  auxiliary halo data  */
struct HaloAuxData *HaloAux;

/*  misc  */

#ifdef HDF5
size_t HDF5_dst_size;
size_t *HDF5_dst_offsets;
size_t *HDF5_dst_sizes;
const char **HDF5_field_names;
hid_t *HDF5_field_types;
int HDF5_n_props;
hid_t HDF5_current_file_id = -1; /* -1 means no file currently open */
#endif

int MaxProcessedHalos;
int MaxFoFWorkspace;
int Ntrees;            /*  number of trees in current file  */
int NumProcessedHalos; /*  Total number of halos stored for current tree  */

int TotHalosPerSnap[ABSOLUTEMAXSNAPS];
int *InputHalosPerSnap[ABSOLUTEMAXSNAPS];

int *InputTreeNHalos;
int *InputTreeFirstHalo;

/* ThisTask/NTask exist in every build (see globals.h); only MPI builds set them
   and use the MPI-only node-name fields. */
int ThisTask, NTask;
#ifdef MPI
int nodeNameLen;
char *ThisNode;
#endif

/* Lookback-time table; see globals.h for the Age/Age_base offset invariant */
double *Age;
double *Age_base;

/* Current tree/file indices, read by model modules building unique IDs */
int TreeID;
int FileNum;
