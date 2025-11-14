#ifndef TYPES_H
#define TYPES_H

#include "constants.h"
#include "generated/property_defs.h"

/* Raw merger tree input structure read from treefiles */
struct RawHalo {
  /* merger tree pointers */
  int Descendant;
  int FirstProgenitor;
  int NextProgenitor;
  int FirstHaloInFOFgroup;
  int NextHaloInFOFgroup;

  /* properties of halo */
  int Len;
  float M_Mean200, Mvir, M_TopHat; /* for Millennium, Mvir=M_Crit200 */
  float Pos[3];
  float Vel[3];
  float VelDisp;
  float Vmax;
  float Spin[3];
  long long MostBoundID; /* for LHaloTrees, this is the ID of the most bound
                            particle; for other mergertree codes, let this
                            contain a unique haloid */

  /* original position in simulation tree files */
  int SnapNum;
  int FileNr;
  int SubhaloIndex;
  float SubHalfMass;
};

/* Enum for tree types */
enum Valid_TreeTypes {
  genesis_lhalo_hdf5 = 0,
  lhalo_binary = 1,
  num_tree_types
};

/* Enum for output formats */
enum Valid_OutputFormats {
  output_binary = 0,
  output_hdf5 = 1,
  num_output_formats
};

/* Configuration structure to hold global parameters */
struct MimicConfig {
  /* file information */
  int FirstFile; /* first and last file for processing */
  int LastFile;
  int LastSnapshotNr;
  double BoxSize;

  /* paths */
  char OutputDir[MAX_STRING_LEN];
  char OutputFileBaseName[MAX_STRING_LEN];
  char TreeName[MAX_STRING_LEN];
  char TreeExtension[MAX_STRING_LEN];
  char SimulationDir[MAX_STRING_LEN];
  char FileWithSnapList[MAX_STRING_LEN];

  /* cosmological parameters */
  double Omega;
  double OmegaLambda;
  double PartMass;
  double Hubble_h;

  /* flags */
  int OverwriteOutputFiles; // Flag: 1=overwrite (default), 0=skip existing
                            // files

  /* output parameters */
  int NOUT;
  int ListOutputSnaps[ABSOLUTEMAXSNAPS];
  double ZZ[ABSOLUTEMAXSNAPS];
  double AA[ABSOLUTEMAXSNAPS];
  int MAXSNAPS;
  int Snaplistlen;

  /* units */
  double UnitLength_in_cm;
  double UnitTime_in_s;
  double UnitVelocity_in_cm_per_s;
  double UnitMass_in_g;
  double UnitTime_in_Megayears;
  double UnitPressure_in_cgs;
  double UnitDensity_in_cgs;
  double UnitCoolingRate_in_cgs;
  double UnitEnergy_in_cgs;

  /* derived parameters */
  double RhoCrit;
  double G;
  double Hubble;

  /* Tree type */
  enum Valid_TreeTypes TreeType;

  /* Output format */
  enum Valid_OutputFormats OutputFormat;

  /* Module system configuration (Phase 3) */
  int NumEnabledModules;                  /* Number of enabled modules */
  char EnabledModules[32][MAX_STRING_LEN]; /* Module names in execution order */

  /* Module-specific parameters (Phase 3) */
  int NumModuleParams; /* Number of module-specific parameters */
  struct {
    char module_name[MAX_STRING_LEN]; /* Module name (e.g., "SimpleCooling") */
    char param_name[MAX_STRING_LEN];  /* Parameter name (e.g., "BaryonFraction")
                                       */
    char value[MAX_STRING_LEN];       /* String value (modules parse to type) */
  } ModuleParams[256]; /* Up to 256 module parameters */
};

/* Halo tracking structures defined in generated/property_defs.h:
 *   - struct Halo         (internal processing, 23 properties + galaxy pointer)
 *   - struct GalaxyData   (baryonic physics properties)
 *   - struct HaloOutput   (file output, 26 properties)
 *
 * These are auto-generated from metadata/ YAML files
 * To regenerate: make generate
 */

/* auxiliary halo data */
struct HaloAuxData {
  int DoneFlag;
  int HaloFlag;
  int NHalos;
  int FirstHalo;
};

#endif /* #ifndef TYPES_H */
