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
enum Valid_TreeTypes { genesis_lhalo_hdf5 = 0, lhalo_binary = 1, num_tree_types };

/* Enum for output formats */
enum Valid_OutputFormats { output_binary = 0, output_hdf5 = 1, num_output_formats };

/* Forward declarations for phase config structs (defined in module_registry.h,
 * which uses enum ProcessingMode from module_interface.h) */
struct PhaseModuleConfig;
struct ModulePhaseConfig;

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

  /* package provenance */
  char ModelName[MAX_STRING_LEN];
  char ModelPath[MAX_STRING_LEN];
  char ModelPropertiesPath[MAX_STRING_LEN];
  char SimulationName[MAX_STRING_LEN];
  char SimulationPath[MAX_STRING_LEN];
  char SimulationConfigPath[MAX_STRING_LEN];
  char SimulationHaloPropertiesPath[MAX_STRING_LEN];
  char PlottingProfilePath[MAX_STRING_LEN];

  /* cosmological parameters */
  double Omega;
  double OmegaLambda;
  double PartMass;
  double Hubble_h;

  /* flags */
  int OverwriteOutputFiles; // Flag: 1=overwrite (default), 0=skip existing
                            // files

  /* tree traversal */
  int MaxTreeDepth; // Maximum recursion depth (default: 500)

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

  /* ===== Multi-Phase Pipeline Configuration =====
   * Pipeline structure defined in input YAML file, not in module metadata.
   * This provides maximum flexibility - users control execution structure.
   *
   * Lifecycle per snapshot interval:
   *   pre_timestep (once) -> [ substep_phases[0..N) ] x SubSteps -> post_timestep (once)
   *
   * The middle phases are user-named and arbitrary in number (see
   * struct ModulePhaseConfig). Legacy top-level phase_1/phase_2 inputs are
   * rejected by the parser rather than translated.
   */

  /* Time sub-stepping */
  int SubSteps; /* Number of substeps per snapshot interval (0 = no substeps) */

  /* Pre-timestep: runs once before substeps */
  struct PhaseModuleConfig *pre_timestep; /* Array of modules for this phase */
  int num_pre_timestep;                   /* Number of modules in this phase */

  /* Ordered user-named middle phases, each run once per substep in input order */
  struct ModulePhaseConfig *substep_phases; /* Array of named phases */
  int num_substep_phases;                   /* Number of substep phases */

  /* Post-timestep: runs once after substeps */
  struct PhaseModuleConfig *post_timestep; /* Array of modules for this phase */
  int num_post_timestep;                   /* Number of modules in this phase */

  /* Model parameters - ALL physics parameters */
  int NumModelParams; /* Number of model parameters loaded from input file */
  struct {
    char param_name[MAX_STRING_LEN]; /* Parameter name (e.g., "BaryonFrac") */
    char value[MAX_STRING_LEN];      /* String value (parsed to type by modules) */
  } ModelParams[256];                /* Maximum parameters in input file */
};

/* Halo tracking structures defined in generated/property_defs.h:
 *   - struct Halo         (internal processing, 23 properties + galaxy pointer)
 *   - struct GalaxyData   (baryonic physics properties)
 *   - struct HaloOutput   (file output, 26 properties)
 *
 * These are auto-generated from selected metadata YAML files.
 * To regenerate defaults: make generate
 * For another package pair: make MODEL=<name> SIMULATION=<name> generate
 */

/* auxiliary halo data */
struct HaloAuxData {
  int DoneFlag;
  int HaloFlag;
  int NHalos;
  int FirstHalo;
};

#endif /* #ifndef TYPES_H */
