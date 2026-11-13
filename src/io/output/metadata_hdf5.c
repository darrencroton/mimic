/**
 * @file    metadata_hdf5.c
 * @brief   Run-metadata writers for HDF5 output
 *
 * Writes the RunProperties content that makes Mimic HDF5 outputs
 * self-describing: version/provenance, the enabled module pipeline, resolved
 * event contracts, model parameters, the snapshot-redshift mapping, and the
 * generated field schema. Used by both the per-file writer (hdf5.c) and the
 * master-file aggregator (master_hdf5.c). When a rate phase is configured, and
 * only then, it also writes the configured transfers (RateTransfers, every
 * file) and the integration settings with the run's step statistics
 * (RateIntegration, the master file).
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
#include "rate_assembly.h"   /* RateTransfers rows, step statistics */
#include "output/util.h"     /* struct OutputPartitionSource, get_output_partition_source() */

static void copy_hdf5_string(char dest[MAX_STRING_LEN], const char *src) {
  if (src == NULL) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, MAX_STRING_LEN - 1);
  dest[MAX_STRING_LEN - 1] = '\0';
}

/**
 * @brief   Write a scalar fixed-width string attribute from a value of any length.
 *
 * H5Awrite reads exactly the string type's width from the source buffer, so a
 * literal shorter than that width is read past its end (an out-of-bounds read
 * AddressSanitizer reports). Every fixed-width string attribute therefore goes
 * through a zero-padded MAX_STRING_LEN buffer; the on-disk type is unchanged.
 */
static void write_string_attribute(hid_t group_id, const char *name, hid_t str_type,
                                   hid_t dataspace_id, const char *value) {
  char padded[MAX_STRING_LEN];
  if (H5Tget_size(str_type) > sizeof(padded)) {
    FATAL_ERROR("HDF5 string type for attribute %s is wider than %zu bytes", name, sizeof(padded));
  }
  copy_hdf5_string(padded, value);
  hid_t attribute_id = H5Acreate(group_id, name, str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create %s attribute in HDF5 output", name);
  }
  if (H5Awrite(attribute_id, str_type, padded) < 0) {
    FATAL_ERROR("Failed to write %s attribute to HDF5 output", name);
  }
  H5Aclose(attribute_id);
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
 * @brief   Table-driven descriptor mapping MimicConfig fields to HDF5 attributes.
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
 * Creates a Version subgroup containing the release name (git describe), the
 * git commit, branch and date, the build date, and the HDF5 format version.
 */
static void write_version_metadata(hid_t parent_group_id) {
  hid_t version_group_id, dataspace_id, str_type;
  hsize_t dims = 1;
  herr_t status;

/* Include git version info generated at build time. Use the build-dir include
 * path (-I$(BUILD_DIR)/generated), like version.c/run_log.c, so the location
 * follows the selected build directory (e.g. build/ or build/test/). */
#include "git_version.h"

  version_group_id = H5Gcreate(parent_group_id, "Version", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (version_group_id < 0) {
    FATAL_ERROR("Failed to create Version subgroup in HDF5 file");
  }

  dataspace_id = H5Screate_simple(1, &dims, NULL);
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, 128);
  if (status < 0) {
    FATAL_ERROR("Failed to set HDF5 string type size for version metadata");
  }

  /* The release name: `git describe` over the release tags at build time, so a release
   * build reads its tag (v1.2) and a later build reads the tag plus its distance and commit. */
  write_string_attribute(version_group_id, "version", str_type, dataspace_id, GIT_VERSION);
  write_string_attribute(version_group_id, "git_commit", str_type, dataspace_id, GIT_COMMIT);

  write_string_attribute(version_group_id, "git_branch", str_type, dataspace_id, GIT_BRANCH);

  write_string_attribute(version_group_id, "git_date", str_type, dataspace_id, GIT_DATE);

  write_string_attribute(version_group_id, "build_date", str_type, dataspace_id, BUILD_DATE);

  /* Increment hdf5_format_version when the output schema changes. */
  const char *hdf5_format_version = "1.2";
  write_string_attribute(version_group_id, "hdf5_format_version", str_type, dataspace_id,
                         hdf5_format_version);

  H5Sclose(dataspace_id);
  H5Tclose(str_type);
  H5Gclose(version_group_id);
}

/**
 * @brief   Write all model parameters to HDF5 as a (param_name, value) compound dataset.
 * @param   parent_group_id   HDF5 group to create the Parameters dataset in.
 *
 * Iterates MimicConfig.ModelParams[] generically (no knowledge of specific physics meanings).
 */
static void write_parameters_metadata(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, rowtype;
  hsize_t dims;
  herr_t status;

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

  dataset_id = H5Dcreate(parent_group_id, "Parameters", rowtype, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create Parameters dataset in HDF5 file");
  }

  status = H5Dwrite(dataset_id, rowtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, MimicConfig.ModelParams);
  if (status < 0) {
    FATAL_ERROR("Failed to write Parameters dataset to HDF5 file");
  }

  write_description_attr(
      dataset_id, "Runtime model parameters from input YAML file (modules.parameters section)");

  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(rowtype);
  H5Tclose(str_type);
}

/**
 * @brief   Writes the run's UniqueGalaxyID forest multiplier as an int64 attribute
 *
 * @param   parent_group_id   RunProperties group to attach the attribute to
 *
 * Every output record carries a UniqueGalaxyID encoded as
 * `halonr + multiplier * (forestnr_global + 1)`, so a file that does not record
 * the multiplier cannot be decomposed back into its components. The master file
 * gets the same attribute through its configuration table in
 * store_run_properties(); per-file outputs write it here because their metadata
 * path is separate and must stay self-contained.
 */
static void write_unique_galaxy_id_multiplier(hid_t parent_group_id) {
  hsize_t dims = 1;
  hid_t dataspace_id = H5Screate_simple(1, &dims, NULL);
  hid_t attribute_id = H5Acreate(parent_group_id, "UniqueGalaxyIDMultiplier", H5T_NATIVE_INT64,
                                 dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create UniqueGalaxyIDMultiplier attribute in HDF5 output");
  }
  if (H5Awrite(attribute_id, H5T_NATIVE_INT64, &MimicConfig.UniqueGalaxyIDMultiplier) < 0) {
    FATAL_ERROR("Failed to write UniqueGalaxyIDMultiplier attribute to HDF5 output");
  }
  H5Aclose(attribute_id);
  H5Sclose(dataspace_id);
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
  dims = MimicConfig.LastSnapshotNr + 1;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  dataset_id = H5Dcreate(parent_group_id, "Redshifts", H5T_NATIVE_DOUBLE, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create Redshifts dataset in HDF5 file");
  }

  status = H5Dwrite(dataset_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, MimicConfig.ZZ);
  if (status < 0) {
    FATAL_ERROR("Failed to write Redshifts dataset to HDF5 file");
  }

  write_description_attr(dataset_id, "Redshift for each snapshot index (0 to LastSnapshotNr)");

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
 * (pre_timestep, user-named substep phase, post_timestep, or post_snapshot),
 * and processing mode, in execution order (for_each_phase). post_snapshot rows
 * follow every FoF-phase row and exist only when that phase is configured, so
 * a run without snapshot modules records exactly the rows it did before the
 * phase existed. If a module appears in multiple phases, it has multiple
 * entries.
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

/* ── The rate phase's records (written only when a rate phase is configured) ── */

/** Row layout for the RateTransfers HDF5 compound dataset */
typedef struct {
  char module[MAX_STRING_LEN];
  char transfer[MAX_STRING_LEN];
  char from[MAX_STRING_LEN];
  char to[MAX_STRING_LEN];
  char to_record[MAX_STRING_LEN];
  char kind[MAX_STRING_LEN];
  char master[MAX_STRING_LEN];
} RateTransferEntry;

/**
 * @brief   Writes the rate phase's configured transfers to HDF5 (master and per-file)
 *
 * @param   parent_group_id   HDF5 group ID to create the RateTransfers dataset in
 *
 * One row per transfer of every module configured in the rate phase, in global
 * transfer order (module name, then declaration order): the module, the
 * transfer, its two ends (a property name or "external"), its recipient record
 * ("self" or "central"), its kind ("internal", "sink", "source" or
 * "slaved_source") and a slaved source's master as <module>.<transfer> (empty
 * otherwise). Nothing is written when no rate phase is configured.
 */
static void write_rate_transfers(hid_t parent_group_id) {
  const struct RateAssemblyPlan *plan = module_rate_plan();
  if (plan == NULL) {
    return;
  }
  const int count = rate_assembly_plan_num_transfers(plan);
  RateTransferEntry *entries =
      (RateTransferEntry *)mymalloc_cat((size_t)count * sizeof(RateTransferEntry), MEM_IO);
  for (int i = 0; i < count; i++) {
    struct RateTransferRow record;
    rate_assembly_plan_transfer_record(plan, i, &record);
    copy_hdf5_string(entries[i].module, record.module);
    copy_hdf5_string(entries[i].transfer, record.transfer);
    copy_hdf5_string(entries[i].from, record.from);
    copy_hdf5_string(entries[i].to, record.to);
    copy_hdf5_string(entries[i].to_record, record.to_record);
    copy_hdf5_string(entries[i].kind, record.kind);
    copy_hdf5_string(entries[i].master, record.master);
  }

  hid_t str_type = H5Tcopy(H5T_C_S1);
  if (H5Tset_size(str_type, MAX_STRING_LEN) < 0) {
    FATAL_ERROR("Failed to set string type size for RateTransfers");
  }
  /* Memory and file layouts are identical, so one compound type serves both. */
  hid_t rowtype = H5Tcreate(H5T_COMPOUND, sizeof(RateTransferEntry));
  H5Tinsert(rowtype, "module", HOFFSET(RateTransferEntry, module), str_type);
  H5Tinsert(rowtype, "transfer", HOFFSET(RateTransferEntry, transfer), str_type);
  H5Tinsert(rowtype, "from", HOFFSET(RateTransferEntry, from), str_type);
  H5Tinsert(rowtype, "to", HOFFSET(RateTransferEntry, to), str_type);
  H5Tinsert(rowtype, "to_record", HOFFSET(RateTransferEntry, to_record), str_type);
  H5Tinsert(rowtype, "kind", HOFFSET(RateTransferEntry, kind), str_type);
  H5Tinsert(rowtype, "master", HOFFSET(RateTransferEntry, master), str_type);

  hsize_t dims = (hsize_t)count;
  hid_t dataspace_id = H5Screate_simple(1, &dims, NULL);
  hid_t dataset_id = H5Dcreate(parent_group_id, "RateTransfers", rowtype, dataspace_id, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create RateTransfers dataset in HDF5 file");
  }
  if (H5Dwrite(dataset_id, rowtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, entries) < 0) {
    FATAL_ERROR("Failed to write RateTransfers dataset to HDF5 file");
  }
  write_description_attr(dataset_id,
                         "Transfers of the modules configured in the rate phase, in global "
                         "transfer order (module name, then declaration order). Each row gives "
                         "the module, the transfer, its from and to ends (a property or "
                         "external), its recipient record (self or central), its kind "
                         "(internal, sink, source or slaved_source) and a slaved source's master "
                         "as module.transfer.");

  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(rowtype);
  H5Tclose(str_type);
  myfree(entries);
}

/** @brief Write one scalar numeric attribute (fatal on failure) */
static void write_scalar_attribute(hid_t group_id, const char *name, hid_t type,
                                   const void *value) {
  hid_t space_id = H5Screate(H5S_SCALAR);
  hid_t attribute_id = H5Acreate(group_id, name, type, space_id, H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create %s attribute in HDF5 output", name);
  }
  if (H5Awrite(attribute_id, type, value) < 0) {
    FATAL_ERROR("Failed to write %s attribute to HDF5 output", name);
  }
  H5Aclose(attribute_id);
  H5Sclose(space_id);
}

/** @brief Write a one-dimensional uint64 dataset with a description (fatal on failure) */
static hid_t write_uint64_dataset(hid_t group_id, const char *name, const uint64_t *values,
                                  hsize_t count, const char *description) {
  hid_t space_id = H5Screate_simple(1, &count, NULL);
  hid_t dataset_id =
      H5Dcreate(group_id, name, H5T_NATIVE_UINT64, space_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create %s dataset in HDF5 file", name);
  }
  if (H5Dwrite(dataset_id, H5T_NATIVE_UINT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, values) < 0) {
    FATAL_ERROR("Failed to write %s dataset to HDF5 file", name);
  }
  write_description_attr(dataset_id, description);
  H5Sclose(space_id);
  return dataset_id;
}

/** @brief Write a one-row compound dataset with a description (fatal on failure) */
static void write_single_row(hid_t group_id, const char *name, hid_t rowtype, const void *row,
                             const char *description) {
  hsize_t one = 1;
  hid_t space_id = H5Screate_simple(1, &one, NULL);
  hid_t dataset_id =
      H5Dcreate(group_id, name, rowtype, space_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create %s dataset in HDF5 file", name);
  }
  if (H5Dwrite(dataset_id, rowtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, row) < 0) {
    FATAL_ERROR("Failed to write %s dataset to HDF5 file", name);
  }
  write_description_attr(dataset_id, description);
  H5Dclose(dataset_id);
  H5Sclose(space_id);
}

/** One-row layouts of the StepTotals and StepMaxima datasets */
typedef struct {
  uint64_t segments;
  uint64_t accepted_steps;
  uint64_t rejected_steps;
  uint64_t law_evaluations;
} RateStepTotalsRow;

typedef struct {
  uint64_t accepted_steps_per_segment;
  uint64_t rejected_steps_per_segment;
  uint64_t law_evaluations_per_segment;
  double k_max;
} RateStepMaximaRow;

/** Row layout of the DeclaredDiscreteChanges dataset */
typedef struct {
  char total[MAX_STRING_LEN];
  uint64_t count;
} DeclaredChangeEntry;

/** @brief Write the step statistics datasets of RunProperties/RateIntegration */
static void write_rate_statistics(hid_t group_id, const struct RateStatistics *stats) {
  hid_t k_max = write_uint64_dataset(
      group_id, "KMaxHistogram", stats->counts.k_max_histogram, RATE_STATS_K_MAX_BINS,
      "Segments by the largest K_max the segment met where its laws were evaluated (K_max: over "
      "the entries holding content, the largest of an entry's segment length Delta times the sum "
      "of its outflow coefficients). Bin 0: zero; "
      "bin 1: below 2^octave_min_exponent; bin 2 + k - octave_min_exponent: [2^k, 2^(k+1)) for "
      "k from octave_min_exponent to octave_max_exponent; last bin: 2^(octave_max_exponent + 1) "
      "and above. The counts sum to StepTotals.segments.");
  const int min_exponent = RATE_STATS_K_MAX_MIN_EXPONENT;
  const int max_exponent = RATE_STATS_K_MAX_MAX_EXPONENT;
  write_scalar_attribute(k_max, "octave_min_exponent", H5T_NATIVE_INT, &min_exponent);
  write_scalar_attribute(k_max, "octave_max_exponent", H5T_NATIVE_INT, &max_exponent);
  H5Dclose(k_max);

  H5Dclose(write_uint64_dataset(
      group_id, "AcceptedStepsHistogram", stats->counts.accepted_steps_histogram,
      RATE_STATS_STEP_BINS,
      "Segments by their number of accepted steps n, binned by bit length: bin 0 holds n = 0, "
      "bin b > 0 holds 2^(b-1) <= n < 2^b. The counts sum to StepTotals.segments."));

  const RateStepTotalsRow totals = {stats->counts.segments, stats->counts.accepted_steps,
                                    stats->counts.rejected_steps, stats->counts.law_evaluations};
  hid_t totals_type = H5Tcreate(H5T_COMPOUND, sizeof(RateStepTotalsRow));
  H5Tinsert(totals_type, "segments", HOFFSET(RateStepTotalsRow, segments), H5T_NATIVE_UINT64);
  H5Tinsert(totals_type, "accepted_steps", HOFFSET(RateStepTotalsRow, accepted_steps),
            H5T_NATIVE_UINT64);
  H5Tinsert(totals_type, "rejected_steps", HOFFSET(RateStepTotalsRow, rejected_steps),
            H5T_NATIVE_UINT64);
  H5Tinsert(totals_type, "law_evaluations", HOFFSET(RateStepTotalsRow, law_evaluations),
            H5T_NATIVE_UINT64);
  write_single_row(group_id, "StepTotals", totals_type, &totals,
                   "Run totals over every task: segments (one component across one substep), "
                   "accepted and rejected steps, and law evaluations (each one call of every "
                   "rate module's law per active row of the component).");
  H5Tclose(totals_type);

  const RateStepMaximaRow maxima = {stats->maxima.accepted_steps, stats->maxima.rejected_steps,
                                    stats->maxima.law_evaluations,
                                    rate_statistics_max_k_max(stats)};
  hid_t maxima_type = H5Tcreate(H5T_COMPOUND, sizeof(RateStepMaximaRow));
  H5Tinsert(maxima_type, "accepted_steps_per_segment",
            HOFFSET(RateStepMaximaRow, accepted_steps_per_segment), H5T_NATIVE_UINT64);
  H5Tinsert(maxima_type, "rejected_steps_per_segment",
            HOFFSET(RateStepMaximaRow, rejected_steps_per_segment), H5T_NATIVE_UINT64);
  H5Tinsert(maxima_type, "law_evaluations_per_segment",
            HOFFSET(RateStepMaximaRow, law_evaluations_per_segment), H5T_NATIVE_UINT64);
  H5Tinsert(maxima_type, "k_max", HOFFSET(RateStepMaximaRow, k_max), H5T_NATIVE_DOUBLE);
  write_single_row(group_id, "StepMaxima", maxima_type, &maxima,
                   "Exact maxima over every segment of every task: accepted steps, rejected "
                   "steps and law evaluations in one segment, and the largest K_max.");
  H5Tclose(maxima_type);

  const int num_totals = MIMIC_RESERVOIR_TOTAL_COUNT;
  DeclaredChangeEntry *changes = (DeclaredChangeEntry *)mymalloc_cat(
      (size_t)(num_totals > 0 ? num_totals : 1) * sizeof(DeclaredChangeEntry), MEM_IO);
  for (int t = 0; t < num_totals; t++) {
    copy_hdf5_string(changes[t].total, mimic_reservoir_total_names()[t]);
    changes[t].count = stats->counts.declared_changes[t];
  }
  hid_t str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, MAX_STRING_LEN);
  hid_t change_type = H5Tcreate(H5T_COMPOUND, sizeof(DeclaredChangeEntry));
  H5Tinsert(change_type, "total", HOFFSET(DeclaredChangeEntry, total), str_type);
  H5Tinsert(change_type, "count", HOFFSET(DeclaredChangeEntry, count), H5T_NATIVE_UINT64);
  hsize_t dims = (hsize_t)num_totals;
  hid_t space_id = H5Screate_simple(1, &dims, NULL);
  hid_t dataset_id = H5Dcreate(group_id, "DeclaredDiscreteChanges", change_type, space_id,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create DeclaredDiscreteChanges dataset in HDF5 file");
  }
  if (num_totals > 0 &&
      H5Dwrite(dataset_id, change_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, changes) < 0) {
    FATAL_ERROR("Failed to write DeclaredDiscreteChanges dataset to HDF5 file");
  }
  write_description_attr(dataset_id,
                         "Per conserved total, the discrete-phase dispatch units (a full-halo "
                         "module's call with the per-event modules it triggers, or a phase's "
                         "by-galaxy pass) that changed the total beyond rounding under a "
                         "changes_totals declaration, over every task.");
  H5Dclose(dataset_id);
  H5Sclose(space_id);
  H5Tclose(change_type);
  H5Tclose(str_type);
  myfree(changes);
}

/**
 * @brief   Writes the rate phase's settings and step statistics (master file only)
 *
 * @param   parent_group_id   RunProperties group to create the RateIntegration group in
 *
 * Attributes: scheme, relative_tolerance, absolute_tolerance,
 * max_steps_per_segment, max_rejections_per_step, check_purity ("true" or
 * "false") and method (the scheme's method and controller constants).
 * Datasets: the step statistics, merged across tasks at run end
 * (module_rate_statistics()). Nothing is written when no rate phase is
 * configured.
 */
static void write_rate_integration(hid_t parent_group_id) {
  const struct RateStatistics *stats = module_rate_statistics();
  if (stats == NULL) {
    return;
  }
  const struct MimicRateIntegration *settings = &MimicConfig.RateIntegration;
  hid_t group_id =
      H5Gcreate(parent_group_id, "RateIntegration", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (group_id < 0) {
    FATAL_ERROR("Failed to create RateIntegration group in HDF5 output");
  }

  hsize_t dims = 1;
  hid_t dataspace_id = H5Screate_simple(1, &dims, NULL);
  hid_t str_type = H5Tcopy(H5T_C_S1);
  if (H5Tset_size(str_type, MAX_STRING_LEN) < 0) {
    FATAL_ERROR("Failed to set HDF5 string type size for RateIntegration");
  }
  write_string_attribute(group_id, "scheme", str_type, dataspace_id,
                         rate_assembly_scheme_name(settings->scheme));
  write_scalar_attribute(group_id, "relative_tolerance", H5T_NATIVE_DOUBLE,
                         &settings->relative_tolerance);
  write_scalar_attribute(group_id, "absolute_tolerance", H5T_NATIVE_DOUBLE,
                         &settings->absolute_tolerance);
  write_scalar_attribute(group_id, "max_steps_per_segment", H5T_NATIVE_INT64,
                         &settings->max_steps_per_segment);
  write_scalar_attribute(group_id, "max_rejections_per_step", H5T_NATIVE_INT,
                         &settings->max_rejections_per_step);
  write_string_attribute(group_id, "check_purity", str_type, dataspace_id,
                         settings->check_purity ? "true" : "false");
  write_string_attribute(group_id, "method", str_type, dataspace_id,
                         rate_assembly_method_description(settings->scheme));
  H5Sclose(dataspace_id);
  H5Tclose(str_type);

  write_rate_statistics(group_id, stats);
  write_description_attr(group_id,
                         "The rate phase's integration settings (modules.rate_integration) and "
                         "the run's step statistics, merged across every MPI task by integer sum "
                         "and maximum.");
  H5Gclose(group_id);
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
  write_version_metadata(props_group_id);            /* Identity */
  write_enabled_modules(props_group_id);             /* Configuration */
  write_event_contracts(props_group_id);             /* Configuration: event wiring */
  write_rate_transfers(props_group_id);              /* Configuration: rate phase, if any */
  write_parameters_metadata(props_group_id);         /* Configuration */
  write_unique_galaxy_id_multiplier(props_group_id); /* Configuration: identity encoding */
  write_redshifts(props_group_id);                   /* Auxiliary */

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
  struct tm utc;
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
      {"SubSteps", INT, &MimicConfig.SubSteps},
      {"MaxDynamicSubsteps", INT, &MimicConfig.MaxDynamicSubsteps},
      {"BoxSize", DOUBLE, &MimicConfig.BoxSize},

      /* Cosmology */
      {"Omega", DOUBLE, &MimicConfig.Omega},
      {"OmegaLambda", DOUBLE, &MimicConfig.OmegaLambda},
      {"Hubble_h", DOUBLE, &MimicConfig.Hubble_h},
      {"PartMass", DOUBLE, &MimicConfig.PartMass},

      /* Output chunking controls */
      {"TargetFileSize", CONFIG_PARAM_INT64, &MimicConfig.TargetFileSize},
      {"ForestsPerFile", CONFIG_PARAM_INT64, &MimicConfig.ForestsPerFile},

      /* Identity encoding: without this, output UniqueGalaxyIDs cannot be
         decomposed back into (halonr, forestnr_global). */
      {"UniqueGalaxyIDMultiplier", CONFIG_PARAM_INT64, &MimicConfig.UniqueGalaxyIDMultiplier},

      /* Units */
      {"UnitVelocity_in_cm_per_s", DOUBLE, &MimicConfig.UnitVelocity_in_cm_per_s},
      {"UnitLength_in_cm", DOUBLE, &MimicConfig.UnitLength_in_cm},
      {"UnitMass_in_g", DOUBLE, &MimicConfig.UnitMass_in_g},
  };
  int num_config_params = sizeof(config_params) / sizeof(ConfigParamDescriptor);

  /* Create the group to hold the run properties */
  props_group_id =
      H5Gcreate(master_file_id, "RunProperties", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (props_group_id < 0) {
    FATAL_ERROR("Failed to create RunProperties group in HDF5 output");
  }

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
      if (attribute_id < 0) {
        FATAL_ERROR("Failed to create %s attribute in HDF5 output", config_params[i].name);
      }
      status = H5Awrite(attribute_id, H5T_NATIVE_INT, config_params[i].address);
      if (status < 0) {
        FATAL_ERROR("Failed to write %s attribute to HDF5 output", config_params[i].name);
      }
      H5Aclose(attribute_id);
      break;

    case CONFIG_PARAM_INT64:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, H5T_NATIVE_INT64,
                               dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
      if (attribute_id < 0) {
        FATAL_ERROR("Failed to create %s attribute in HDF5 output", config_params[i].name);
      }
      status = H5Awrite(attribute_id, H5T_NATIVE_INT64, config_params[i].address);
      if (status < 0) {
        FATAL_ERROR("Failed to write %s attribute to HDF5 output", config_params[i].name);
      }
      H5Aclose(attribute_id);
      break;

    case DOUBLE:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, H5T_NATIVE_DOUBLE,
                               dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
      if (attribute_id < 0) {
        FATAL_ERROR("Failed to create %s attribute in HDF5 output", config_params[i].name);
      }
      status = H5Awrite(attribute_id, H5T_NATIVE_DOUBLE, config_params[i].address);
      if (status < 0) {
        FATAL_ERROR("Failed to write %s attribute to HDF5 output", config_params[i].name);
      }
      H5Aclose(attribute_id);
      break;

    case STRING:
      attribute_id = H5Acreate(props_group_id, config_params[i].name, str_type, dataspace_id,
                               H5P_DEFAULT, H5P_DEFAULT);
      if (attribute_id < 0) {
        FATAL_ERROR("Failed to create %s attribute in HDF5 output", config_params[i].name);
      }
      status = H5Awrite(attribute_id, str_type, config_params[i].address);
      if (status < 0) {
        FATAL_ERROR("Failed to write %s attribute to HDF5 output", config_params[i].name);
      }
      H5Aclose(attribute_id);
      break;
    }
  }

  write_string_attribute(props_group_id, "TimestepScheme", str_type, dataspace_id,
                         timestep_scheme_name(MimicConfig.TimestepScheme));

  /* Record the resolved output partition source's format name, never the
   * active reader pointer directly (see output/util.h). */
  const struct OutputPartitionSource partition_source = get_output_partition_source();
  const char *tree_type_str = partition_source.format_name;
  write_string_attribute(props_group_id, "TreeType", str_type, dataspace_id, tree_type_str);

  /* Runtime metadata */
  attribute_id =
      H5Acreate(props_group_id, "NCores", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create NCores attribute in HDF5 output");
  }
#ifdef MPI
  status = H5Awrite(attribute_id, H5T_NATIVE_INT, &NTask);
#else
  int ncores = 1;
  status = H5Awrite(attribute_id, H5T_NATIVE_INT, &ncores);
#endif
  if (status < 0) {
    FATAL_ERROR("Failed to write NCores attribute to HDF5 output");
  }
  H5Aclose(attribute_id);

  /* UTC with a Z suffix (ISO 8601), so the attribute reads the same on every host */
  time(&t);
  if (gmtime_r(&t, &utc) == NULL) {
    FATAL_ERROR("Failed to convert the run end time to UTC for HDF5 output");
  }
  char end_time[64];
  strftime(end_time, sizeof(end_time), "%Y-%m-%dT%H:%M:%SZ", &utc);
  write_string_attribute(props_group_id, "RunEndTime", str_type, dataspace_id, end_time);

  /* Add input simulation info if defined */
#ifdef INPUTSIM
  write_string_attribute(props_group_id, "InputSimulation", str_type, dataspace_id, INPUTSIM);
#endif

  /* Clean up attribute resources (reused above) */
  H5Sclose(dataspace_id);
  H5Tclose(str_type);

  /* Add extended metadata using helper functions (ordered by importance) */
  write_version_metadata(props_group_id);    /* Identity: version & provenance */
  write_enabled_modules(props_group_id);     /* Configuration: which physics */
  write_event_contracts(props_group_id);     /* Configuration: event wiring */
  write_rate_transfers(props_group_id);      /* Configuration: rate phase, if any */
  write_rate_integration(props_group_id);    /* Rate settings and step statistics, if any */
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
