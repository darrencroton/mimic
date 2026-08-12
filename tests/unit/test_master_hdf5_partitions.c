/**
 * @file    test_master_hdf5_partitions.c
 * @brief   Unit tests for HDF5 master-file partition enumeration.
 */

#include "../framework/test_framework.h"

#include "error.h"
#include "globals.h"
#include "output/hdf5.h"
#include "output/hdf5_internal.h"
#include "output/util.h"
#include "snapshot/reader.h"
#include "tree/reader.h"

#include <hdf5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0, failed = 0;

#define MAX_MASTER_PARTITIONS 8

static int master_npartitions;
static int master_output_ids[MAX_MASTER_PARTITIONS];
static int master_exists[MAX_MASTER_PARTITIONS];
static int master_requires_prepared_state;
static int master_prepared;
static int master_prepare_calls;
static int master_teardown_calls;

void store_run_properties(hid_t master_file_id) {
  hid_t group_id =
      H5Gcreate(master_file_id, "RunProperties", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (group_id >= 0) {
    H5Gclose(group_id);
  }
}

static void master_prepare_run(void) {
  master_prepared = 1;
  master_prepare_calls++;
}

static void master_teardown_run(void) {
  master_prepared = 0;
  master_teardown_calls++;
}

static int master_reader_ready(void) { return !master_requires_prepared_state || master_prepared; }

static int master_num_partitions(void) { return master_reader_ready() ? master_npartitions : 0; }

static int master_partition_output_id(int partition) {
  return master_reader_ready() ? master_output_ids[partition] : -1;
}

static int master_partition_exists(int partition) {
  return master_reader_ready() && master_exists[partition];
}

static const struct TreeReader EnumeratedMasterReader = {
    .name = "master_enumerated",
    .file_extension = "",
    .partition_model = PARTITION_ENUMERATED,
    .processing_order = INPUT_PROCESSING_ORDER_TREE,
    .prepare_run = master_prepare_run,
    .teardown_run = master_teardown_run,
    .num_partitions = master_num_partitions,
    .partition_output_id = master_partition_output_id,
    .partition_exists = master_partition_exists,
    .format_partition_path = NULL,
    .count_partition_units = NULL,
    .global_forest_offset = NULL,
    .partition_cost = NULL,
    .open_partition = NULL,
    .load_unit = NULL,
    .close_partition = NULL,
};

static const struct TreeReader PerFileMasterReader = {
    .name = "master_lhalo_style",
    .file_extension = "",
    .partition_model = PARTITION_PER_FILE,
    .processing_order = INPUT_PROCESSING_ORDER_TREE,
    .prepare_run = NULL,
    .teardown_run = NULL,
    .num_partitions = master_num_partitions,
    .partition_output_id = master_partition_output_id,
    .partition_exists = master_partition_exists,
    .format_partition_path = NULL,
    .count_partition_units = NULL,
    .global_forest_offset = NULL,
    .partition_cost = NULL,
    .open_partition = NULL,
    .load_unit = NULL,
    .close_partition = NULL,
};

static void reset_master_partitions(void) {
  master_npartitions = 0;
  memset(master_output_ids, 0, sizeof(master_output_ids));
  memset(master_exists, 0, sizeof(master_exists));
  master_requires_prepared_state = 0;
  master_prepared = 0;
  master_prepare_calls = 0;
  master_teardown_calls = 0;
}

static void configure_master_output(const char *dir, const char *base, int nout,
                                    const struct TreeReader *reader) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.reader = reader;
  MimicConfig.NOUT = nout;
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", dir);
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "%s", base);

  for (int n = 0; n < nout; n++) {
    MimicConfig.ListOutputSnaps[n] = n;
    MimicConfig.ZZ[n] = (double)(nout - n);
  }
}

static int create_temp_output_dir(char *dir_template) {
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create an output directory");
  return TEST_PASS;
}

static int write_int_dataset(hid_t group_id, const char *name, int value) {
  hsize_t dims = 1;
  hid_t space_id = H5Screate_simple(1, &dims, NULL);
  hid_t dataset_id =
      H5Dcreate2(group_id, name, H5T_NATIVE_INT, space_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (space_id < 0 || dataset_id < 0) {
    return TEST_FAIL;
  }
  if (H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) < 0) {
    return TEST_FAIL;
  }
  H5Dclose(dataset_id);
  H5Sclose(space_id);
  return TEST_PASS;
}

static int write_galaxies_dataset(hid_t group_id, int64_t total) {
  int value = 0;
  hsize_t dims = 1;
  hid_t space_id = H5Screate_simple(1, &dims, NULL);
  hid_t dataset_id = H5Dcreate2(group_id, "Galaxies", H5T_NATIVE_INT, space_id, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT);
  if (space_id < 0 || dataset_id < 0) {
    return TEST_FAIL;
  }
  if (H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) < 0) {
    return TEST_FAIL;
  }

  hid_t attribute_id = H5Acreate(dataset_id, "TotHalosPerSnap", H5T_NATIVE_INT64, space_id,
                                 H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0 || H5Awrite(attribute_id, H5T_NATIVE_INT64, &total) < 0) {
    return TEST_FAIL;
  }

  H5Aclose(attribute_id);
  H5Dclose(dataset_id);
  H5Sclose(space_id);
  return TEST_PASS;
}

static int create_partition_file(int filenr, const int64_t *totals, int nout) {
  char path[512];
  char group_name[64];
  output_path_hdf5(path, sizeof(path), filenr);

  hid_t file_id = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  TEST_ASSERT(file_id >= 0, "partition HDF5 file should be created");

  for (int n = 0; n < nout; n++) {
    snprintf(group_name, sizeof(group_name), "Snap%03d", MimicConfig.ListOutputSnaps[n]);
    hid_t group_id = H5Gcreate(file_id, group_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    TEST_ASSERT(group_id >= 0, "snapshot group should be created");
    TEST_ASSERT(write_galaxies_dataset(group_id, totals[n]) == TEST_PASS,
                "Galaxies dataset should be created");
    TEST_ASSERT(write_int_dataset(group_id, "TreeHalosPerSnap", (int)totals[n]) == TEST_PASS,
                "TreeHalosPerSnap dataset should be created");
    H5Gclose(group_id);
  }

  H5Fclose(file_id);
  return TEST_PASS;
}

static int open_master_file(hid_t *file_id) {
  char path[512];
  int written = snprintf(path, sizeof(path), "%s/%s.hdf5", MimicConfig.OutputDir,
                         MimicConfig.OutputFileBaseName);
  TEST_ASSERT(written > 0 && written < (int)sizeof(path), "master path should fit");
  *file_id = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  TEST_ASSERT(*file_id >= 0, "master HDF5 file should open");
  return TEST_PASS;
}

static int assert_link_exists(hid_t file_id, const char *path, int should_exist) {
  htri_t exists;

  H5E_BEGIN_TRY { exists = H5Lexists(file_id, path, H5P_DEFAULT); }
  H5E_END_TRY;

  if (!should_exist && exists < 0) {
    return TEST_PASS;
  }

  TEST_ASSERT(exists >= 0, "HDF5 link existence check should succeed");
  TEST_ASSERT((exists > 0) == should_exist, "HDF5 link existence should match expectation");
  return TEST_PASS;
}

static int assert_total_attr(hid_t file_id, const char *group_path, int64_t expected) {
  int64_t value = -1;
  hid_t group_id = H5Gopen(file_id, group_path, H5P_DEFAULT);
  TEST_ASSERT(group_id >= 0, "master File group should open");
  hid_t attribute_id = H5Aopen(group_id, "TotHalosPerSnap", H5P_DEFAULT);
  TEST_ASSERT(attribute_id >= 0, "TotHalosPerSnap attribute should open");

  /* H5Aread(..., H5T_NATIVE_INT64, ...) below is a CONVERTING read: HDF5
   * silently promotes a stored int32 attribute to an int64_t destination, so
   * value equality alone cannot catch a regression back to H5T_NATIVE_INT at
   * the write site. Query the attribute's own stored type first. */
  hid_t type_id = H5Aget_type(attribute_id);
  TEST_ASSERT(type_id >= 0, "TotHalosPerSnap attribute type should be queryable");
  TEST_ASSERT_EQUAL(H5Tget_class(type_id), H5T_INTEGER,
                    "TotHalosPerSnap must be stored as an integer type");
  TEST_ASSERT_EQUAL(H5Tget_sign(type_id), H5T_SGN_2,
                    "TotHalosPerSnap must be stored as a signed integer");
  TEST_ASSERT_EQUAL((int64_t)H5Tget_size(type_id), (int64_t)8,
                    "TotHalosPerSnap must be stored 8 bytes wide (int64), not narrowed at the "
                    "write site and merely widened by this converting read");
  H5Tclose(type_id);

  TEST_ASSERT(H5Aread(attribute_id, H5T_NATIVE_INT64, &value) >= 0,
              "TotHalosPerSnap attribute should read");
  H5Aclose(attribute_id);
  H5Gclose(group_id);
  TEST_ASSERT_EQUAL(value, expected, "TotHalosPerSnap value should match source file");
  return TEST_PASS;
}

static void cleanup_outputs(const int *filenrs, int nfiles) {
  char path[512];
  for (int i = 0; i < nfiles; i++) {
    output_path_hdf5(path, sizeof(path), filenrs[i]);
    unlink(path);
  }
  snprintf(path, sizeof(path), "%s/%s.hdf5", MimicConfig.OutputDir, MimicConfig.OutputFileBaseName);
  unlink(path);
  rmdir(MimicConfig.OutputDir);
}

/**
 * @test    test_enumerated_master_links_existing_partitions_only
 * @brief   Master file links existing partitions and skips missing ones for enumerated readers
 */
static int test_enumerated_master_links_existing_partitions_only(void) {
  char dir_template[] = "/tmp/mimic_master_enum_XXXXXX";
  const int64_t file10_totals[] = {4, 5};
  const int64_t file11_totals[] = {99, 99};
  /* Snap001's total is deliberately above INT32_MAX so a silent 32-bit
   * narrowing at the write site would produce a visibly wrong number, not
   * just a value that happens to still fit. */
  const int64_t file12_totals[] = {6, 5000000012LL};
  const int cleanup_filenrs[] = {10, 11, 12};
  hid_t master_file_id;

  reset_master_partitions();
  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be available");
  configure_master_output(dir_template, "model", 2, &EnumeratedMasterReader);

  master_npartitions = 3;
  master_output_ids[0] = 10;
  master_output_ids[1] = 11;
  master_output_ids[2] = 12;
  master_exists[0] = 1;
  master_exists[1] = 0;
  master_exists[2] = 1;
  master_requires_prepared_state = 1;

  TEST_ASSERT(create_partition_file(10, file10_totals, 2) == TEST_PASS,
              "partition 10 fixture should be created");
  TEST_ASSERT(create_partition_file(11, file11_totals, 2) == TEST_PASS,
              "stale partition 11 fixture should be created");
  TEST_ASSERT(create_partition_file(12, file12_totals, 2) == TEST_PASS,
              "partition 12 fixture should be created");

  write_master_file();

  TEST_ASSERT_EQUAL(master_prepare_calls, 1, "master generation should prepare the reader once");
  TEST_ASSERT_EQUAL(master_teardown_calls, 1, "master generation should tear the reader down once");
  TEST_ASSERT_EQUAL(master_prepared, 0, "master reader state should be torn down after writing");

  TEST_ASSERT(open_master_file(&master_file_id) == TEST_PASS, "master file should be readable");
  TEST_ASSERT(assert_link_exists(master_file_id, "Snap000/File010/Galaxies", 1) == TEST_PASS,
              "existing partition 10 should be linked");
  TEST_ASSERT(assert_link_exists(master_file_id, "Snap000/File011/Galaxies", 0) == TEST_PASS,
              "missing partition 11 should not be linked");
  TEST_ASSERT(assert_link_exists(master_file_id, "Snap001/File012/Galaxies", 1) == TEST_PASS,
              "existing partition 12 should be linked");
  TEST_ASSERT(assert_total_attr(master_file_id, "Snap000/File010", 4) == TEST_PASS,
              "partition 10 snap 0 total should be copied");
  TEST_ASSERT(assert_total_attr(master_file_id, "Snap001/File012", 5000000012LL) == TEST_PASS,
              "partition 12 snap 1 total should be copied");
  H5Fclose(master_file_id);

  cleanup_outputs(cleanup_filenrs, 3);
  return TEST_PASS;
}

/**
 * @test    test_per_file_master_links_match_lhalo_layout
 * @brief   Per-file master links all partitions without lifecycle hooks for L-Halo readers
 */
static int test_per_file_master_links_match_lhalo_layout(void) {
  char dir_template[] = "/tmp/mimic_master_lhalo_XXXXXX";
  const int64_t file0_totals[] = {3};
  const int64_t file1_totals[] = {8};
  const int cleanup_filenrs[] = {0, 1};
  hid_t master_file_id;

  reset_master_partitions();
  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be available");
  configure_master_output(dir_template, "model", 1, &PerFileMasterReader);

  master_npartitions = 2;
  master_output_ids[0] = 0;
  master_output_ids[1] = 1;
  master_exists[0] = 1;
  master_exists[1] = 1;

  TEST_ASSERT(create_partition_file(0, file0_totals, 1) == TEST_PASS,
              "partition 0 fixture should be created");
  TEST_ASSERT(create_partition_file(1, file1_totals, 1) == TEST_PASS,
              "partition 1 fixture should be created");

  write_master_file();

  TEST_ASSERT_EQUAL(master_prepare_calls, 0,
                    "per-file master regression should not require lifecycle hooks");
  TEST_ASSERT_EQUAL(master_teardown_calls, 0,
                    "per-file master regression should not require lifecycle teardown");

  TEST_ASSERT(open_master_file(&master_file_id) == TEST_PASS, "master file should be readable");
  TEST_ASSERT(assert_link_exists(master_file_id, "Snap000/File000/Galaxies", 1) == TEST_PASS,
              "L-Halo-style file 0 should be linked");
  TEST_ASSERT(assert_link_exists(master_file_id, "Snap000/File001/TreeHalosPerSnap", 1) ==
                  TEST_PASS,
              "L-Halo-style file 1 tree-count link should be present");
  TEST_ASSERT(assert_total_attr(master_file_id, "Snap000/File000", 3) == TEST_PASS,
              "file 0 total should be copied");
  TEST_ASSERT(assert_total_attr(master_file_id, "Snap000/File001", 8) == TEST_PASS,
              "file 1 total should be copied");
  H5Fclose(master_file_id);

  cleanup_outputs(cleanup_filenrs, 2);
  return TEST_PASS;
}

/* Minimal resolved snapshot reader for the partition-source test: only .name is
 * consulted by snapshot_output_partition_source(), which takes the format name
 * from the resolved reader (as config validation guarantees one exists for a
 * snapshot-ordered run). */
static const struct SnapshotReader SnapshotSourceReader = {
    .name = "snapshot_hdf5",
    .processing_order = INPUT_PROCESSING_ORDER_SNAPSHOT,
};

/**
 * @test    test_snapshot_output_partition_source_is_trivial_single_partition
 * @brief   The snapshot-ordered output partition source is the frozen single-partition shape
 */
static int test_snapshot_output_partition_source_is_trivial_single_partition(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_SNAPSHOT;
  /* A snapshot-ordered configuration always carries a resolved snapshot reader
   * (config validation rejects it otherwise); the source reads its name. */
  MimicConfig.snapshot_reader = &SnapshotSourceReader;

  struct OutputPartitionSource source = get_output_partition_source();

  TEST_ASSERT(source.num_partitions != NULL, "snapshot source must supply num_partitions");
  TEST_ASSERT(source.partition_output_id != NULL,
              "snapshot source must supply partition_output_id");
  TEST_ASSERT(source.partition_exists != NULL, "snapshot source must supply partition_exists");
  TEST_ASSERT_EQUAL(source.num_partitions(), 1, "snapshot source has exactly one partition");
  TEST_ASSERT_EQUAL(source.partition_output_id(0), 0, "snapshot source's partition output id is 0");
  TEST_ASSERT(source.partition_exists(0) != 0, "snapshot source's single partition always exists");
  TEST_ASSERT(source.format_name != NULL && strcmp(source.format_name, "snapshot_hdf5") == 0,
              "snapshot source records format name snapshot_hdf5");
  TEST_ASSERT(source.prepare_run == NULL, "snapshot source keeps no run-scoped prepare hook");
  TEST_ASSERT(source.teardown_run == NULL, "snapshot source keeps no run-scoped teardown hook");

  return TEST_PASS;
}

/**
 * @test    test_tree_output_partition_source_wraps_configured_reader
 * @brief   The tree-ordered output partition source wraps the configured reader's hooks
 */
static int test_tree_output_partition_source_wraps_configured_reader(void) {
  reset_master_partitions();
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_TREE;
  MimicConfig.reader = &EnumeratedMasterReader;

  master_npartitions = 2;
  master_output_ids[0] = 5;
  master_output_ids[1] = 6;
  master_exists[0] = 1;
  master_exists[1] = 0;

  struct OutputPartitionSource source = get_output_partition_source();

  TEST_ASSERT(strcmp(source.format_name, "master_enumerated") == 0,
              "tree source records the configured reader's name");
  TEST_ASSERT_EQUAL(source.num_partitions(), 2, "tree source passes through num_partitions");
  TEST_ASSERT_EQUAL(source.partition_output_id(0), 5,
                    "tree source passes through partition_output_id");
  TEST_ASSERT(source.partition_exists(0) != 0,
              "tree source honours an existing enumerated partition");
  TEST_ASSERT(source.partition_exists(1) == 0,
              "tree source honours a missing enumerated partition");

  /* PARTITION_PER_FILE readers keep their prior behaviour bit for bit: the
   * seam never consults their exists hook (the output-file access() check
   * downstream is what gates them), so this must read as always-existing
   * even though the fake hook below says otherwise. */
  reset_master_partitions();
  MimicConfig.reader = &PerFileMasterReader;
  master_npartitions = 1;
  master_output_ids[0] = 9;
  master_exists[0] = 0;

  struct OutputPartitionSource per_file_source = get_output_partition_source();
  TEST_ASSERT(per_file_source.partition_exists(0) != 0,
              "tree source treats a per-file partition as existing regardless of its exists hook");

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: HDF5 Master Partitions\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_enumerated_master_links_existing_partitions_only);
  TEST_RUN(test_per_file_master_links_match_lhalo_layout);
  TEST_RUN(test_snapshot_output_partition_source_is_trivial_single_partition);
  TEST_RUN(test_tree_output_partition_source_wraps_configured_reader);

  TEST_SUMMARY();
  return TEST_RESULT();
}
