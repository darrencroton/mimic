/**
 * @file    core_allvars.c
 * @brief   Defines global variables used throughout the SAGE model
 *
 * This file contains the definitions of all global variables used by the
 * SAGE model. These variables fall into several categories:
 *
 * 1. Core data structures (e.g. halos, auxiliary data)
 * 2. Configuration parameters and derived values
 * 3. Simulation state variables (counts, indices, etc.)
 * 4. Physical constants and units
 * 5. File and output control variables
 *
 * Configuration parameters are stored in the SageConfig structure.
 * Runtime simulation state is tracked via individual global variables.
 *
 * Note: This file contains only variable definitions - the declarations
 * are in globals.h and other header files.
 */

#include "config.h"
#include "globals.h"
#include "types.h"

/*  Global configuration structure */
struct SageConfig SageConfig;

/*  halo data  */
struct Halo *FoFWorkspace, *ProcessedHalos;

struct RawHalo *InputTreeHalos;

/*  auxiliary halo data  */
struct HaloAuxData *HaloAux;

/*  misc  */

int HDF5Output;
#ifdef HDF5
char *core_output_file;
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

int HaloCounter; /*  unique halo ID for main progenitor line in tree */

int TotHalos;
int TotHalosPerSnap[ABSOLUTEMAXSNAPS];
int *InputHalosPerSnap[ABSOLUTEMAXSNAPS];

int LastSnapshotNr;
double BoxSize;

int *FirstHaloInSnap;
int *InputTreeNHalos;
int *InputTreeFirstHalo;

#ifdef MPI
int ThisTask, NTask, nodeNameLen;
char *ThisNode;
#endif

/*  recipe parameters  */
int NParam;
char ParamTag[MAXTAGS][50];
int ParamID[MAXTAGS];
void *ParamAddr[MAXTAGS];

/*  more misc - kept for backward compatibility */
double UnitLength_in_cm, UnitTime_in_s, UnitVelocity_in_cm_per_s, UnitMass_in_g,
    RhoCrit, UnitPressure_in_cgs, UnitDensity_in_cgs, UnitCoolingRate_in_cgs,
    UnitEnergy_in_cgs, UnitTime_in_Megayears, G, Hubble;

int ListOutputSnaps[ABSOLUTEMAXSNAPS];
double ZZ[ABSOLUTEMAXSNAPS];
double AA[ABSOLUTEMAXSNAPS];
double *Age;

int MAXSNAPS;
int NOUT;
int Snaplistlen;

/* Random number generator removed - not used in computation */

int TreeID;
int FileNum;

enum Valid_TreeTypes TreeType;
