#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

#include "constants.h"
#include "generated/property_defs.h"

/* struct RawHalo (the on-disk merger-tree record) is generated from the active
 * simulation's halo_properties.yaml, in on-disk order, by
 * scripts/generate_properties.py. The binary reader reads it wholesale, so its
 * field order and types are the binary file layout. */
#include "generated/raw_halo_defs.h"

#define MIMIC_DEFAULT_TARGET_FILE_SIZE (4LL * 1024LL * 1024LL * 1024LL)
#define MIMIC_DEFAULT_FORESTS_PER_FILE 0LL

/* Enum for output formats */
enum Valid_OutputFormats { output_binary = 0, output_hdf5 = 1, num_output_formats };

/* Enum for timestep schemes. Fixed must remain zero for memset-zeroed test fixtures. */
enum TimestepScheme { TIMESTEP_SCHEME_FIXED = 0, TIMESTEP_SCHEME_DYNAMIC = 1 };

/* Forward declarations for phase config structs (defined in module_registry.h,
 * which uses enum ProcessingMode from module_interface.h) */
struct PhaseModuleConfig;
struct ModulePhaseConfig;

/* Active merger-tree reader, resolved from tree_type at config time
 * (defined in tree/reader.h, registered in tree/registry.c). */
struct TreeReader;

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
  int OverwriteOutputFiles; /* 1=overwrite (default), 0=skip existing output files (--skip) */
  int HDF5CompressionLevel; /* 0=off (default), nonzero=gzip on; set via --compress */

  /* tree traversal */
  int MaxTreeDepth;    // Maximum recursion depth (default: 500)
  int ProcessingOrder; // enum InputProcessingOrder from tree/reader.h

  /* Forest -> MPI-task load balancing for forest-oriented readers. Values are
   * enum ForestDistributionScheme (tree/forest_distribution.h), stored as int so
   * this core header only carries the serialized configuration shape. */
  int ForestDistributionScheme;       // default: 0 (uniform_in_forests)
  double Exponent_Forest_Dist_Scheme; // power-law index for the power schemes

  /* output parameters */
  int64_t TargetFileSize;
  int64_t ForestsPerFile;
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

  /* Active merger-tree reader (resolved from tree_type) */
  const struct TreeReader *reader;

  /* Output format */
  enum Valid_OutputFormats OutputFormat;

  /* ===== Multi-Phase Pipeline Configuration =====
   * Pipeline structure defined in input YAML file, not in module metadata.
   * This provides maximum flexibility - users control execution structure.
   *
   * Lifecycle per snapshot interval:
   *   pre_timestep (once) -> [ substep_phases[0..N) ] x num_substeps -> post_timestep (once)
   *
   * The middle phases are user-named and arbitrary in number (see
   * struct ModulePhaseConfig). Legacy top-level phase_1/phase_2 inputs are
   * rejected by the parser rather than translated.
   */

  /* Time sub-stepping */
  int SubSteps;                       /* Fixed count or dynamic resolution per dynamical time */
  enum TimestepScheme TimestepScheme; /* How SubSteps is interpreted */

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
  } ModelParams[MAX_MODEL_PARAMS];
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
