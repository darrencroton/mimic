#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
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
 * so none of those layers has to reach for a file-scope input array. The vertical
 * driver builds one view per loaded unit over its own halo storage; a
 * horizontal driver will build one per slab. It borrows that storage, and
 * carries no bounds enforcement: `halonr` indices are trusted exactly as they
 * were when the accessors read a global array. */
struct HaloInputView {
  const struct RawHalo *halos;
  int64_t count;
};

/**
 * @brief   Created-record identity space a driver publishes for the processing unit it is
 *          about to process (galaxy_id.h, mimic_encode_created_galaxy_id()).
 *
 * The vertical driver publishes unit = GlobalForestOffset + unit index and the
 * run-wide largest forest (or MimicConfig.UniqueGalaxyIDMultiplier when a reader
 * cannot report it) as rows_per_unit; the horizontal driver publishes unit =
 * snapshot number and the largest slab as rows_per_unit. `fits` is the run's
 * startup verdict, mimic_created_record_space_fits(units, rows_per_unit) over the
 * run's whole unit count; it is the same for every unit of a run. A space that
 * does not fit never stops a run: only creating a record in it fails.
 *
 * `units` is the run's unit count that verdict was evaluated with (the total
 * forest count under the vertical driver, the dataset's snapshot count under the
 * horizontal driver), carried so a refused creation can report both numbers.
 * `driver` names the publishing driver ("vertical" or "horizontal", a string
 * literal) for the same refusal, so module dispatch never needs the reader
 * headers to name it; a hand-built space may leave it NULL. Members are only
 * ever appended, so positional initialisers of the leading members stay valid.
 *
 * `row_offset` is the unit's global row of the driver's local row 0: a created
 * record is encoded with row = host HaloNr + row_offset, so its identity is the
 * same whichever part of the unit a process holds. The distributed horizontal
 * driver sets it to the first row of the task's range in the current snapshot;
 * the vertical driver, a serial horizontal run and every hand-built space leave
 * it 0, where the encoded row is HaloNr itself.
 */
struct RecordIdentitySpace {
  int64_t unit;
  int64_t rows_per_unit;
  bool fits;
  int64_t units;
  const char *driver;
  int64_t row_offset;
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
 * the two is non-NULL after a successful configuration: struct VerticalReader
 * (vertical/reader.h, registered in vertical/registry.c) for forest-ordered input, or
 * struct HorizontalReader (horizontal/reader.h, registered in horizontal/registry.c)
 * for horizontal input. */
struct VerticalReader;
struct HorizontalReader;

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
  int ProcessingOrder; // enum InputProcessingOrder from vertical/reader.h

  /* Forest -> MPI-task load balancing for forest-oriented readers. Values are
   * enum ForestDistributionScheme (vertical/forest_distribution.h), stored as int so
   * this core header only carries the serialized configuration shape. */
  int ForestDistributionScheme;       // default: 0 (uniform_in_forests)
  double Exponent_Forest_Dist_Scheme; // power-law index for the power schemes

  /* Resident-memory ceiling for the horizontal driver's retained generations, in
   * bytes (input.retention_memory_ceiling_mb, 1 MB = 1024^2 B). 0 means no ceiling:
   * the driver still computes and reports every generation's resident bytes but
   * refuses nothing. Checked before each generation is allocated
   * (src/core/horizontal_driver.c); a vertical run rejects the key at
   * configuration. Not recorded in output metadata. */
  int64_t RetentionMemoryCeiling;

  /* Number of contiguous forest sub-ranges (chunks) each task sweeps in turn
   * (input.forest_chunks). Default 1: the task sweeps its whole range at once. A
   * horizontal-reader option; a vertical run rejects a value above 1 at
   * configuration. Not recorded in output metadata, as it changes no output. */
  int ForestChunks;

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
  const struct VerticalReader *vertical_reader;
  const struct HorizontalReader *horizontal_reader;

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
   * Lifecycle per snapshot interval, for each FoF group:
   *   pre_timestep (once) -> [ substep_phases[0..N) ] x num_substeps -> post_timestep (once)
   *
   * The middle phases are user-named and arbitrary in number (see
   * struct ModulePhaseConfig). Legacy top-level phase_1/phase_2 inputs are
   * rejected by the parser rather than translated.
   *
   * post_snapshot is not part of that per-FoF lifecycle: the horizontal driver
   * runs it once per snapshot, after every FoF group of the snapshot has been
   * processed, over the snapshot's whole processed population. Only the
   * process_snapshot mode is legal there, and a vertical run rejects a
   * non-empty post_snapshot at configuration.
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

  /* Post-snapshot: runs once per snapshot over the whole processed population
   * (horizontal driver only); every entry is process_snapshot, in YAML order */
  struct PhaseModuleConfig *post_snapshot; /* Array of modules for this phase */
  int num_post_snapshot;                   /* Number of modules in this phase */

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

/* auxiliary halo data. The traversal flags are int; the output range is int64_t,
 * matching the output buffer's own counts and struct HorizontalHaloAux, so one
 * index type runs through both drivers. */
struct HaloAuxData {
  int DoneFlag;
  int HaloFlag;
  int64_t NHalos;    /* output halos produced for this halo */
  int64_t FirstHalo; /* first output index for this halo */
};

/* Horizontal-driver counterpart of struct HaloAuxData's FirstHalo/NHalos pair:
 * where one snapshot's processed halos landed in that snapshot's output buffer,
 * indexed by slab halo index. The horizontal driver keeps one of these arrays per
 * retained slab generation and reads the progenitor's generation's when it
 * gathers progenitor galaxies.
 *
 * The traversal flags (DoneFlag/HaloFlag) have no counterpart here: they exist
 * to sequence the vertical driver's depth-first recursion, and a snapshot slab is
 * walked once in slab order instead. The range fields are int64_t because a
 * production slab's output can exceed a tree's (the output buffer's own counts
 * are int64_t for the same reason). */
struct HorizontalHaloAux {
  int64_t FirstHalo; /* first output index for this halo, or -1 when it has none */
  int64_t NHalos;    /* output halos produced for this halo */
};

/* One retained slab generation, as the horizontal driver's progenitor lookup
 * sees it: a snapshot's raw halos, where each of them landed in that snapshot's
 * output buffer, the buffer itself, and the snapshot each of its NextProgenitor
 * links names.
 *
 * Bundled into one struct so a lookup cannot be handed one generation's view
 * with another generation's aux array (or vice versa): the members are always
 * the same generation, and the compiler cannot catch a mismatch between
 * separately passed arguments whose indices happen to overlap.
 *
 * `next_progenitor_snapshot` is the reader-owned version 3 column. It is NULL for
 * a version 2 generation, whose NextProgenitor links implicitly stay inside the
 * owner's own snapshot (HORIZONTAL-HDF5-FORMAT.md "Link Scope"). */
struct HorizontalRetainedGeneration {
  int64_t snapnum;                         /* this generation's snapshot; -1 marks an empty slot */
  struct HaloInputView view;               /* raw halos of that snapshot */
  const struct HorizontalHaloAux *aux;     /* [view.count] output ranges */
  const struct Halo *processed;            /* that snapshot's output buffer */
  const int32_t *next_progenitor_snapshot; /* [view.count], or NULL (version 2) */
};

struct HorizontalForestPartition; /* opaque here; defined in core/horizontal_partition.h */

/* Every progenitor generation a snapshot's lookup can reach.
 *
 * `generations` is the driver's retention pool, indexed by snapshot number: slot
 * k describes snapshot k while that generation is retained, and carries snapnum
 * -1 otherwise. A progenitor link therefore resolves in one step, through the
 * target-snapshot column that names its generation -- never by assuming N-1.
 * `first_progenitor_snapshot` is the descendant slab's own version 3 column,
 * NULL for version 2, whose FirstProgenitor links implicitly name N-1.
 *
 * `retained_population` is the halo count summed over every retained
 * generation. A progenitor chain can visit each retained halo at most once, so
 * it bounds the chain walk's cycle guard; no single slab's count does once a
 * chain can span several snapshots.
 *
 * `partition` and `range` let the lookup's diagnostics name global rows and the
 * range's share of a snapshot in a partitioned (distributed or chunked) run; they only
 * word messages, and a context that leaves them NULL and 0 (unpartitioned runs, unit
 * tests) keeps the serial bytes. */
struct HorizontalGatherContext {
  int64_t snapnum;                                        /* snapshot of the descendants */
  const int32_t *first_progenitor_snapshot;               /* [descendant slab], or NULL (v2) */
  const struct HorizontalRetainedGeneration *generations; /* [run snapshot count], by snapshot */
  int64_t retained_population;
  const struct HorizontalForestPartition
      *partition; /* the partitioned run's partition; NULL if unpartitioned */
  int range; /* range of `partition` the slab holds (task * forest_chunks + chunk); 0 if none */
};

/* A progenitor named by its generation and its row there. A slab index alone no
 * longer identifies a progenitor once one chain can span several retained
 * generations, because the same index recurs in every slab. {-1, -1} names no
 * progenitor. */
struct HorizontalProgenitorRef {
  int64_t snapnum;
  int64_t halonr;
};

#endif /* #ifndef TYPES_H */
