/**
 * @file    metadata_hdf5.c
 * @brief   Run-metadata writers for HDF5 output
 *
 * Writes the RunProperties content that makes Mimic HDF5 outputs
 * self-describing: version/provenance, the enabled module pipeline, resolved
 * event contracts, model parameters, the snapshot-redshift mapping, and the
 * generated field schema. Used by both the per-file writer (hdf5.c) and the
 * master-file aggregator (master_hdf5.c).
 */

#include <hdf5.h>
#include <hdf5_hl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "proto.h"
#include "error.h"
#include "hdf5_internal.h"
#include "module_registry.h" /* for_each_phase, event-contract enumeration */
#include "tree/reader.h"     /* struct TreeReader (MimicConfig.reader->name) */

static void copy_hdf5_string(char dest[MAX_STRING_LEN], const char *src) {
  if (src == NULL) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, MAX_STRING_LEN - 1);
  dest[MAX_STRING_LEN - 1] = '\0';
}

/**
 * @brief   Attach a scalar string "description" attribute to an HDF5 object.
 */
void write_description_attr(hid_t obj_id, const char *text) {
  hid_t attr_space = H5Screate(H5S_SCALAR);
  hid_t str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, strlen(text) + 1);
  hid_t attr_id = H5Acreate(obj_id, "description", str_type, attr_space, H5P_DEFAULT, H5P_DEFAULT);
  herr_t status = H5Awrite(attr_id, str_type, text);
  if (status < 0) {
    FATAL_ERROR("Failed to write description attribute");
  }
  H5Aclose(attr_id);
  H5Tclose(str_type);
  H5Sclose(attr_space);
}

/**
 * @brief   Configuration parameter descriptor for HDF5 output
 *
 * Local structure to define which MimicConfig fields to write to HDF5.
 * This provides a generic, table-driven approach that maintains
 * core-physics separation (Vision Principle #1).
 */
#define CONFIG_PARAM_INT64 4

typedef struct {
  const char *name; /* HDF5 attribute name */
  int type;         /* Parameter type (INT, INT64, DOUBLE, STRING) */
  void *address;    /* Pointer to MimicConfig field */
} ConfigParamDescriptor;

/**
 * @brief   Writes version information to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create Version subgroup in
 *
 * Creates a Version subgroup containing git version information and
 * HDF5 format version for reproducibility tracking.
 */
static void write_version_metadata(hid_t parent_group_id) {
  hid_t version_group_id, attribute_id, dataspace_id, str_type;
  hsize_t dims = 1;
  herr_t status;

/* Include git version info generated at build time. Use the build-dir include
 * path (-I$(BUILD_DIR)/generated), like version.c/run_log.c, so the location
 * follows the selected build directory (e.g. build/ or build/test/). */
#include "git_version.h"

  /* Create Version subgroup */
  version_group_id = H5Gcreate(parent_group_id, "Version", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (version_group_id < 0) {
    FATAL_ERROR("Failed to create Version subgroup in HDF5 file");
  }

  /* Set up string type and dataspace */
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, 128);
  if (status < 0) {
    FATAL_ERROR("Failed to set HDF5 string type size for version metadata");
  }

  /* Write git commit SHA */
  attribute_id =
      H5Acreate(version_group_id, "git_commit", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, GIT_COMMIT);
  H5Aclose(attribute_id);

  /* Write git branch */
  attribute_id =
      H5Acreate(version_group_id, "git_branch", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, GIT_BRANCH);
  H5Aclose(attribute_id);

  /* Write git date */
  attribute_id =
      H5Acreate(version_group_id, "git_date", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, GIT_DATE);
  H5Aclose(attribute_id);

  /* Write build date */
  attribute_id =
      H5Acreate(version_group_id, "build_date", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, BUILD_DATE);
  H5Aclose(attribute_id);

  /* Write HDF5 format version (increment when output schema changes) */
  const char *hdf5_format_version = "1.1";
  attribute_id = H5Acreate(version_group_id, "hdf5_format_version", str_type, dataspace_id,
                           H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, hdf5_format_version);
  H5Aclose(attribute_id);

  /* Clean up */
  H5Sclose(dataspace_id);
  H5Tclose(str_type);
  H5Gclose(version_group_id);
}

/**
 * @brief   Writes runtime parameters to HDF5 file as compound dataset
 *
 * @param   parent_group_id   HDF5 group ID to create Parameters dataset in
 *
 * Creates a Parameters dataset containing all model parameters from the
 * input YAML file. Stores parameters as string key-value pairs, preserving
 * exact input values for perfect reproducibility.
 *
 * Vision Principle 1 (Physics-Agnostic Core): Iterates parameters generically
 * without knowledge of specific physics meanings.
 *
 * Vision Principle 4 (Single Source of Truth): Parameters are already validated
 * and stored in MimicConfig.ModelParams[] during input parsing.
 */
static void write_parameters_metadata(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, rowtype;
  hsize_t dims;
  herr_t status;

  /* Check if there are any parameters to write */
  if (MimicConfig.NumModelParams == 0) {
    DEBUG_LOG("No model parameters to write to HDF5");
    return;
  }

  /* One compound type (memory and file layouts are identical): (name, value) */
  hid_t str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, MAX_STRING_LEN);

  rowtype = H5Tcreate(H5T_COMPOUND, 2 * MAX_STRING_LEN);
  H5Tinsert(rowtype, "param_name", 0, str_type);
  H5Tinsert(rowtype, "value", MAX_STRING_LEN, str_type);

  /* Create dataspace */
  dims = MimicConfig.NumModelParams;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  /* Create dataset */
  dataset_id = H5Dcreate(parent_group_id, "Parameters", rowtype, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create Parameters dataset in HDF5 file");
  }

  /* Write the parameter data directly from MimicConfig.ModelParams */
  status = H5Dwrite(dataset_id, rowtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, MimicConfig.ModelParams);
  if (status < 0) {
    FATAL_ERROR("Failed to write Parameters dataset to HDF5 file");
  }

  write_description_attr(
      dataset_id, "Runtime model parameters from input YAML file (modules.parameters section)");

  /* Clean up */
  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(rowtype);
  H5Tclose(str_type);
}

/**
 * @brief   Writes redshift array to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create Redshifts dataset in
 *
 * Creates a Redshifts dataset containing the redshift for each snapshot index.
 * This enables self-contained files that don't require external snapshot list
 * files for analysis.
 */
static void write_redshifts(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id;
  hsize_t dims;
  herr_t status;

  /* Create dataspace for redshift array */
  dims = MimicConfig.LastSnapshotNr + 1; /* E.g., 64 snapshots (0-63) */
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  /* Create dataset */
  dataset_id = H5Dcreate(parent_group_id, "Redshifts", H5T_NATIVE_DOUBLE, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create Redshifts dataset in HDF5 file");
  }

  /* Write the redshift array from MimicConfig.ZZ */
  status = H5Dwrite(dataset_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, MimicConfig.ZZ);
  if (status < 0) {
    FATAL_ERROR("Failed to write Redshifts dataset to HDF5 file");
  }

  write_description_attr(dataset_id, "Redshift for each snapshot index (0 to LastSnapshotNr)");

  /* Clean up */
  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
}

/** Row layout for the EnabledModules HDF5 compound dataset */
typedef struct {
  char module_name[MAX_STRING_LEN];
  char phase[MAX_STRING_LEN];
  char processing_mode[MAX_STRING_LEN];
} ModuleEntry;

/** Accumulator for counting/filling EnabledModules rows via for_each_phase */
typedef struct {
  ModuleEntry *buf; /**< NULL during the count pass */
  int idx;
} ModuleEntryState;

static void module_entry_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                 int num_modules, void *userdata) {
  ModuleEntryState *state = userdata;
  for (int i = 0; i < num_modules; i++) {
    if (state->buf != NULL) {
      ModuleEntry *e = &state->buf[state->idx];
      copy_hdf5_string(e->module_name, modules[i].module_name);
      copy_hdf5_string(e->phase, phase_name);
      copy_hdf5_string(e->processing_mode, processing_mode_to_string(modules[i].processing_mode));
    }
    state->idx++;
  }
}

/**
 * @brief   Writes enabled modules configuration to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create EnabledModules dataset in
 *
 * Creates an EnabledModules compound dataset containing the complete pipeline
 * configuration: one row per module instance with its name, execution phase
 * (pre_timestep, user-named substep phase, or post_timestep), and processing
 * mode. If a module appears in multiple phases, it has multiple entries.
 */
static void write_enabled_modules(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, rowtype, str_type;
  hsize_t dims;
  herr_t status;

  /* Count pass */
  ModuleEntryState state = {NULL, 0};
  for_each_phase(module_entry_visitor, &state);
  if (state.idx == 0) {
    DEBUG_LOG("No enabled modules to write to HDF5");
    return;
  }

  /* Fill pass */
  int total_entries = state.idx;
  ModuleEntry *entries = (ModuleEntry *)mymalloc_cat(total_entries * sizeof(ModuleEntry), MEM_IO);
  state.buf = entries;
  state.idx = 0;
  for_each_phase(module_entry_visitor, &state);

  /* Memory and file layouts are identical, so one compound type serves both. */
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, MAX_STRING_LEN);
  if (status < 0) {
    FATAL_ERROR("Failed to set string type size for EnabledModules");
  }

  rowtype = H5Tcreate(H5T_COMPOUND, sizeof(ModuleEntry));
  H5Tinsert(rowtype, "module_name", HOFFSET(ModuleEntry, module_name), str_type);
  H5Tinsert(rowtype, "phase", HOFFSET(ModuleEntry, phase), str_type);
  H5Tinsert(rowtype, "processing_mode", HOFFSET(ModuleEntry, processing_mode), str_type);

  dims = total_entries;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  dataset_id = H5Dcreate(parent_group_id, "EnabledModules", rowtype, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create EnabledModules dataset in HDF5 file");
  }

  status = H5Dwrite(dataset_id, rowtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, entries);
  if (status < 0) {
    FATAL_ERROR("Failed to write EnabledModules dataset to HDF5 file");
  }

  write_description_attr(dataset_id,
                         "Complete module execution pipeline configuration. Each row specifies one "
                         "module's name, execution phase, and processing mode. Preserves full "
                         "pipeline including modules in multiple phases.");

  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(rowtype);
  H5Tclose(str_type);
  myfree(entries);
}

/* ── Event contract types and file-scope callbacks ───────────────────────── */

/** Row layout for the EventContracts HDF5 compound dataset */
typedef struct {
  char phase[MAX_STRING_LEN];
  char consumer_module[MAX_STRING_LEN];
  char producer_module[MAX_STRING_LEN];
  char event_name[MAX_STRING_LEN];
  int event_id;
} ContractEntry;

/** Accumulator state passed to fill_contract_cb */
typedef struct {
  ContractEntry *buf;
  int idx;
} FillState;

/** EventContractCallback: count pass — increments *(int *)userdata */
static void count_contract_cb(const char *phase, const char *consumer, const char *producer,
                              const char *event_name, int event_id, void *userdata) {
  (void)phase;
  (void)consumer;
  (void)producer;
  (void)event_name;
  (void)event_id;
  (*(int *)userdata)++;
}

/** EventContractCallback: fill pass — appends one row via FillState */
static void fill_contract_cb(const char *phase, const char *consumer, const char *producer,
                             const char *event_name, int event_id, void *userdata) {
  FillState *fs = (FillState *)userdata;
  ContractEntry *e = &fs->buf[fs->idx++];
  copy_hdf5_string(e->phase, phase);
  copy_hdf5_string(e->consumer_module, consumer);
  copy_hdf5_string(e->producer_module, producer);
  copy_hdf5_string(e->event_name, event_name);
  e->event_id = event_id;
}

/**
 * @brief   Writes resolved event contracts to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create EventContracts dataset in
 *
 * Creates an EventContracts compound dataset containing all resolved event
 * subscription contracts active for this run. Each row represents one
 * consumer's subscription to one producer event with five fields:
 * - phase:           Execution phase (e.g., "phase_2")
 * - consumer_module: Name of the subscribing module
 * - producer_module: Name of the event-emitting module
 * - event_name:      Human-readable event name (e.g., "merger")
 * - event_id:        Generated numeric event ID
 *
 * This makes event wiring explicit in output for reproducibility.
 * If no event contracts are configured, no dataset is written.
 *
 * Vision Principle 7 (Preserve reproducibility): Event contracts are resolved
 * at startup and recorded here so runs remain self-describing.
 */
static void write_event_contracts(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, rowtype, str_type;
  hsize_t dims;
  herr_t status;

  /* --- Count pass --- */
  int count = 0;
  module_system_enumerate_event_contracts(count_contract_cb, &count);

  if (count == 0) {
    DEBUG_LOG("No event contracts to write to HDF5");
    return;
  }

  /* --- Allocate buffer --- */
  ContractEntry *entries = (ContractEntry *)mymalloc_cat(count * sizeof(ContractEntry), MEM_IO);
  /* --- Fill pass --- */
  FillState fs = {entries, 0};
  module_system_enumerate_event_contracts(fill_contract_cb, &fs);

  /* --- Build HDF5 compound type --- */
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, MAX_STRING_LEN);
  if (status < 0) {
    FATAL_ERROR("Failed to set string type size for EventContracts");
  }

  /* Memory and file layouts are identical, so one compound type serves both. */
  rowtype = H5Tcreate(H5T_COMPOUND, sizeof(ContractEntry));
  H5Tinsert(rowtype, "phase", HOFFSET(ContractEntry, phase), str_type);
  H5Tinsert(rowtype, "consumer_module", HOFFSET(ContractEntry, consumer_module), str_type);
  H5Tinsert(rowtype, "producer_module", HOFFSET(ContractEntry, producer_module), str_type);
  H5Tinsert(rowtype, "event_name", HOFFSET(ContractEntry, event_name), str_type);
  H5Tinsert(rowtype, "event_id", HOFFSET(ContractEntry, event_id), H5T_NATIVE_INT);

  dims = (hsize_t)count;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  dataset_id = H5Dcreate(parent_group_id, "EventContracts", rowtype, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create EventContracts dataset in HDF5 file");
  }

  status = H5Dwrite(dataset_id, rowtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, entries);
  if (status < 0) {
    FATAL_ERROR("Failed to write EventContracts dataset to HDF5 file");
  }

  write_description_attr(
      dataset_id, "Resolved event subscription contracts active for this run. Each row "
                  "specifies one consumer module's subscription to one producer event "
                  "with the execution phase, module names, event name, and numeric event ID.");

  /* Cleanup */
  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(rowtype);
  H5Tclose(str_type);
  myfree(entries);
}

/**
 * @brief   Writes essential metadata to per-file output for self-containment
 *
 * @param   file_id   HDF5 file ID for per-file output
 *
 * Creates a RunProperties group in per-file outputs containing version
 * information and runtime parameters. This makes each output file self-
 * contained and analyzable without the master file.
 *
 * Vision Principle 4 (Single Source of Truth): Metadata is written by
 * calling the same helper functions used for master file, maintaining DRY.
 */
void write_perfile_metadata(hid_t file_id) {
  hid_t props_group_id;

  /* Create RunProperties group for per-file metadata */
  props_group_id = H5Gcreate(file_id, "RunProperties", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (props_group_id < 0) {
    FATAL_ERROR("Failed to create RunProperties group in per-file HDF5 output");
  }

  /* Write essential metadata for self-containment (same order as master) */
  write_version_metadata(props_group_id);    /* Identity */
  write_enabled_modules(props_group_id);     /* Configuration */
  write_event_contracts(props_group_id);     /* Configuration: event wiring */
  write_parameters_metadata(props_group_id); /* Configuration */
  write_redshifts(props_group_id);           /* Auxiliary */

  /* Field schema (names, units, descriptions) is identical across snapshots,
   * so write it once per file here. The generated snippet targets `group_id`. */
  {
    hid_t group_id = props_group_id;
#include "../../include/generated/hdf5_field_metadata.inc"
  }

  H5Gclose(props_group_id);
}

/**
 * @brief   Stores simulation configuration to HDF5 file as attributes
 *
 * @param   master_file_id   HDF5 file ID to write parameters to
 *
 * This function creates a group in the HDF5 file to store simulation
 * configuration as attributes. Uses a local parameter table for generic,
 * physics-agnostic iteration over MimicConfig fields (Vision Principle #1).
 *
 * The table-driven approach ensures:
 * - Core code doesn't hardcode specific parameter names
 * - Adding/removing config fields only requires updating the local table
 * - Maintains separation between core infrastructure and configuration
 *
 * Note: OutputDir is intentionally excluded (may contain sensitive paths)
 */
void store_run_properties(hid_t master_file_id) {
  hid_t props_group_id, dataspace_id, attribute_id, str_type;
  hsize_t dims;
  herr_t status;
  time_t t;
  struct tm *local;
  int i;

  /* Configuration parameter table - defines what to write to HDF5
   * This table approach keeps the code generic and agnostic to specific parameters */
  ConfigParamDescriptor config_params[] = {
      /* File information */
      {"OutputFileBaseName", STRING, &MimicConfig.OutputFileBaseName},
      {"TreeName", STRING, &MimicConfig.TreeName},
      {"SimulationDir", STRING, &MimicConfig.SimulationDir},
      {"FileWithSnapList", STRING, &MimicConfig.FileWithSnapList},
      {"ModelName", STRING, &MimicConfig.ModelName},
      {"ModelPath", STRING, &MimicConfig.ModelPath},
      {"ModelPropertiesPath", STRING, &MimicConfig.ModelPropertiesPath},
      {"SimulationName", STRING, &MimicConfig.SimulationName},
      {"SimulationPath", STRING, &MimicConfig.SimulationPath},
      {"SimulationConfigPath", STRING, &MimicConfig.SimulationConfigPath},
      {"SimulationHaloPropertiesPath", STRING, &MimicConfig.SimulationHaloPropertiesPath},
      {"PlottingProfilePath", STRING, &MimicConfig.PlottingProfilePath},

      /* Simulation parameters */
      {"LastSnapshotNr", INT, &MimicConfig.LastSnapshotNr},
      {"FirstFile", INT, &MimicConfig.FirstFile},
      {"LastFile", INT, &MimicConfig.LastFile},
      {"NumOutputs", INT, &MimicConfig.NOUT},
      {"BoxSize", DOUBLE, &MimicConfig.BoxSize},

      /* Cosmology */
      {"Omega", DOUBLE, &MimicConfig.Omega},
      {"OmegaLambda", DOUBLE, &MimicConfig.OmegaLambda},
      {"Hubble_h", DOUBLE, &MimicConfig.Hubble_h},
      {"PartMass", DOUBLE, &MimicConfig.PartMass},

      /* Output chunking controls */
      {"TargetFileSize", CONFIG_PARAM_INT64, &MimicConfig.TargetFileSize},
      {"ForestsPerFile", CONFIG_PARAM_INT64, &MimicConfig.ForestsPerFile},

      /* Units */
      {"UnitVelocity_in_cm_per_s", DOUBLE, &MimicConfig.UnitVelocity_in_cm_per_s},
      {"UnitLength_in_cm", DOUBLE, &MimicConfig.UnitLength_in_cm},
      {"UnitMass_in_g", DOUBLE, &MimicConfig.UnitMass_in_g},
  };
  int num_config_params = sizeof(config_params) / sizeof(ConfigParamDescriptor);

  /* Create the group to hold the run properties */
  props_group_id =
      H5Gcreate(master_file_id, "RunProperties", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

  /* Set up common data structures for attributes */
  dims = 1;
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, MAX_STRING_LEN);
  if (status < 0) {
    FATAL_ERROR("Failed to set HDF5 string type size for run properties");
  }

  /* Write all config parameters from table (generic iteration) */
  for (i = 0; i < num_config_params; i++) {
    switch (config_params[i].type) {
    case INT:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, H5T_NATIVE_INT, dataspace_id,
                               H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_INT, config_params[i].address);
      H5Aclose(attribute_id);
      break;

    case CONFIG_PARAM_INT64:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, H5T_NATIVE_INT64,
                               dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_INT64, config_params[i].address);
      H5Aclose(attribute_id);
      break;

    case DOUBLE:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, H5T_NATIVE_DOUBLE,
                               dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_DOUBLE, config_params[i].address);
      H5Aclose(attribute_id);
      break;

    case STRING:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, str_type, dataspace_id,
                               H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, str_type, config_params[i].address);
      H5Aclose(attribute_id);
      break;
    }
  }

  /* Record the active reader's format name (see tree/registry.c) */
  const char *tree_type_str = MimicConfig.reader->name;
  attribute_id =
      H5Acreate(props_group_id, "TreeType", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, tree_type_str);
  H5Aclose(attribute_id);

  /* Runtime metadata */
  attribute_id =
      H5Acreate(props_group_id, "NCores", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
#ifdef MPI
  H5Awrite(attribute_id, H5T_NATIVE_INT, &NTask);
#else
  int ncores = 1;
  H5Awrite(attribute_id, H5T_NATIVE_INT, &ncores);
#endif
  H5Aclose(attribute_id);

  time(&t);
  local = localtime(&t);
  char end_time[64];
  strftime(end_time, sizeof(end_time), "%Y-%m-%dT%H:%M:%S", local);
  attribute_id =
      H5Acreate(props_group_id, "RunEndTime", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, end_time);
  H5Aclose(attribute_id);

  /* Add input simulation info if defined */
#ifdef INPUTSIM
  attribute_id = H5Acreate(props_group_id, "InputSimulation", str_type, dataspace_id, H5P_DEFAULT,
                           H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, INPUTSIM);
  H5Aclose(attribute_id);
#endif

  /* Clean up attribute resources (reused above) */
  H5Sclose(dataspace_id);
  H5Tclose(str_type);

  /* Add extended metadata using helper functions (ordered by importance) */
  write_version_metadata(props_group_id);    /* Identity: version & provenance */
  write_enabled_modules(props_group_id);     /* Configuration: which physics */
  write_event_contracts(props_group_id);     /* Configuration: event wiring */
  write_parameters_metadata(props_group_id); /* Configuration: parameter values */
  write_redshifts(props_group_id);           /* Auxiliary: snapshot mapping */

  /* Field schema, written once (generated snippet targets `group_id`) */
  {
    hid_t group_id = props_group_id;
#include "../../include/generated/hdf5_field_metadata.inc"
  }

  /* Close the RunProperties group */
  H5Gclose(props_group_id);
}
