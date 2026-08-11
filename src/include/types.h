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

/* Explicit view over the raw input halos a driver is currently processing.
 *
 * Passed by value from the driver down through the generated tree accessors,
 * the virial helpers, the halo-init payload populator, and output conversion,
 * so none of those layers has to reach for a file-scope input array. The tree
 * driver builds one view per loaded unit over its own halo storage; a
 * snapshot-ordered driver will build one per slab. It borrows that storage, and
 * carries no bounds enforcement: `halonr` indices are trusted exactly as they
 * were when the accessors read a global array. */
struct HaloInputView {
  const struct RawHalo *halos;
  int64_t count;
};

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

/* Active input reader, resolved from tree_type at config time. Exactly one of
 * the two is non-NULL after a successful configuration: struct TreeReader
 * (tree/reader.h, registered in tree/registry.c) for forest-ordered input, or
 * struct SnapshotReader (snapshot/reader.h, registered in snapshot/registry.c)
 * for snapshot-ordered input. */
struct TreeReader;
struct SnapshotReader;

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

  /* Active input reader (resolved from tree_type). Exactly one is non-NULL. */
  const struct TreeReader *reader;
  const struct SnapshotReader *snapshot_reader;

  /* Forest multiplier used to encode UniqueGalaxyID
   * (simulation.unique_galaxy_id_multiplier, default 10^9 from constants.h).
   * Every galaxy_id.h helper takes this value explicitly, so both processing
   * orders honour a configured multiplier; configuration requires only that it
   * be positive, and HDF5 output records the value used as the
   * RunProperties/UniqueGalaxyIDMultiplier attribute. */
  int64_t UniqueGalaxyIDMultiplier;

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
  int MaxDynamicSubsteps; /* Safety ceiling on computed dynamic substeps (scheme: dynamic only) */

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

/* Snapshot-driver counterpart of struct HaloAuxData's FirstHalo/NHalos pair:
 * where one snapshot's processed halos landed in that snapshot's output buffer,
 * indexed by slab halo index. The snapshot driver keeps one of these arrays per
 * live slab generation and reads the previous generation's when it gathers
 * progenitor galaxies.
 *
 * The traversal flags (DoneFlag/HaloFlag) have no counterpart here: they exist
 * to sequence the tree driver's depth-first recursion, and a snapshot slab is
 * walked once in slab order instead. The range fields are int64_t because a
 * production slab's output can exceed a tree's (Slice 6 widened the output
 * buffer's own counts to int64_t for the same reason). */
struct SnapshotHaloAux {
  int64_t FirstHalo; /* first output index for this halo, or -1 when it has none */
  int64_t NHalos;    /* output halos produced for this halo */
};

/* The previous slab generation, as the snapshot driver's gather step sees it.
 *
 * Bundled into one struct so a gather cannot be handed the current slab's view
 * with the previous slab's aux array (or vice versa): the three members are
 * always the same generation, and the compiler cannot catch a mismatch between
 * three separately passed arguments whose indices happen to overlap.
 * `view.halos` is NULL and `aux`/`processed` are NULL at snapshot 0, where no
 * previous generation exists; every progenitor chain is then empty because a
 * snapshot-0 halo can carry no FirstProgenitor. */
struct SnapshotGatherContext {
  struct HaloInputView view;         /* raw halos of snapshot N-1 */
  const struct SnapshotHaloAux *aux; /* [view.count], snapshot N-1 output ranges */
  const struct Halo *processed;      /* snapshot N-1 output buffer */
};

#endif /* #ifndef TYPES_H */
