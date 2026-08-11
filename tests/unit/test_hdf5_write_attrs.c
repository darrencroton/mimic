/**
 * @file    test_hdf5_write_attrs.c
 * @brief   Unit tests for write_hdf5_attrs()'s driver-neutral snapshot count attributes.
 */

#include "../framework/test_framework.h"

#include "error.h"
#include "globals.h"
#include "output/hdf5.h"
#include "output/hdf5_internal.h"
#include "output/util.h"
#include "tree/reader.h"

#include <hdf5.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0, failed = 0;

/*
 * write_hdf5_attrs() (src/io/output/hdf5.c) is the only production caller
 * that touches these per-file HDF5 globals. allvars.c is compiled without
 * -DHDF5 in the shared unit-test object pool (tests/unit/run_tests.sh), so
 * this translation unit supplies them -- mirroring the store_run_properties
 * stub precedent in test_master_hdf5_partitions.c, which substitutes for
 * metadata_hdf5.c not being linked either.
 */
size_t HDF5_dst_size;
size_t *HDF5_dst_offsets;
size_t *HDF5_dst_sizes;
const char **HDF5_field_names;
hid_t *HDF5_field_types;
int HDF5_n_props;
hid_t HDF5_current_file_id = -1;

static int perfile_metadata_calls;

void write_perfile_metadata(hid_t file_id) {
  (void)file_id;
  perfile_metadata_calls++;
}

void write_description_attr(hid_t obj_id, const char *text) {
  (void)obj_id;
  (void)text;
}

static int create_temp_output_dir(char *dir_template) {
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create an output directory");
  return TEST_PASS;
}

/**
 * @brief   Minimal Snap000/Galaxies fixture: write_hdf5_attrs only needs the
 *          group and dataset to exist, never their contents.
 */
static int create_minimal_snap_fixture(const char *path) {
  hid_t file_id = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  TEST_ASSERT(file_id >= 0, "fixture file should be created");

  hid_t group_id = H5Gcreate(file_id, "Snap000", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  TEST_ASSERT(group_id >= 0, "Snap000 group should be created");

  hsize_t dims = 1;
  hid_t space_id = H5Screate_simple(1, &dims, NULL);
  int fill = 0;
  hid_t dataset_id = H5Dcreate2(group_id, "Galaxies", H5T_NATIVE_INT, space_id, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT);
  TEST_ASSERT(dataset_id >= 0, "Galaxies dataset should be created");
  TEST_ASSERT(H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &fill) >= 0,
              "Galaxies dataset should be writable");

  H5Dclose(dataset_id);
  H5Sclose(space_id);
  H5Gclose(group_id);
  H5Fclose(file_id);
  return TEST_PASS;
}

static int attr_exists(hid_t obj_id, const char *name) {
  htri_t exists;
  H5E_BEGIN_TRY { exists = H5Aexists(obj_id, name); }
  H5E_END_TRY;
  return exists > 0;
}

static int link_exists(hid_t loc_id, const char *name) {
  htri_t exists;
  H5E_BEGIN_TRY { exists = H5Lexists(loc_id, name, H5P_DEFAULT); }
  H5E_END_TRY;
  return exists > 0;
}

static int read_int64_attr(hid_t obj_id, const char *name, int64_t *out) {
  hid_t attribute_id = H5Aopen(obj_id, name, H5P_DEFAULT);
  if (attribute_id < 0) {
    return TEST_FAIL;
  }
  herr_t status = H5Aread(attribute_id, H5T_NATIVE_INT64, out);
  H5Aclose(attribute_id);
  return status >= 0 ? TEST_PASS : TEST_FAIL;
}

/**
 * @test    test_tree_run_attrs_include_ntrees_and_tree_halos_per_snap
 * @brief   A tree-ordered run's attrs path writes Ntrees, TreeHalosPerSnap, and int64
 *          TotHalosPerSnap
 */
static int test_tree_run_attrs_include_ntrees_and_tree_halos_per_snap(void) {
  char dir_template[] = "/tmp/mimic_hdf5_attrs_tree_XXXXXX";
  char path[512];

  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS, "temp dir should be created");
  snprintf(path, sizeof(path), "%s/model_000.hdf5", dir_template);
  TEST_ASSERT(create_minimal_snap_fixture(path) == TEST_PASS, "fixture should be created");

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_TREE;
  MimicConfig.ListOutputSnaps[0] = 0;

  Ntrees = 3;
  int tree_halos[3] = {2, 5, 1};
  InputHalosPerSnap[0] = tree_halos;
  /* Above INT32_MAX: proves the widened int64 write, not a silent narrowing. */
  TotHalosPerSnap[0] = 5000000123LL;

  perfile_metadata_calls = 0;
  HDF5_current_file_id = H5Fopen(path, H5F_ACC_RDWR, H5P_DEFAULT);
  TEST_ASSERT(HDF5_current_file_id >= 0, "fixture file should reopen for writing");

  write_hdf5_attrs(0, 0);

  TEST_ASSERT_EQUAL(perfile_metadata_calls, 1, "n==0 should write per-file metadata once");

  hid_t group_id = H5Gopen(HDF5_current_file_id, "Snap000", H5P_DEFAULT);
  hid_t dataset_id = H5Dopen(group_id, "Galaxies", H5P_DEFAULT);

  TEST_ASSERT(attr_exists(dataset_id, "Ntrees"), "tree run should write Ntrees");
  int64_t ntrees_value = -1;
  TEST_ASSERT(read_int64_attr(dataset_id, "TotHalosPerSnap", &ntrees_value) == TEST_PASS,
              "TotHalosPerSnap attribute should read");
  TEST_ASSERT_EQUAL(ntrees_value, TotHalosPerSnap[0],
                    "TotHalosPerSnap attribute should carry the full int64 value");

  TEST_ASSERT(link_exists(group_id, "TreeHalosPerSnap"),
              "tree run should write the TreeHalosPerSnap dataset");
  hid_t tree_ds = H5Dopen(group_id, "TreeHalosPerSnap", H5P_DEFAULT);
  TEST_ASSERT(tree_ds >= 0, "TreeHalosPerSnap dataset should open");
  int read_back[3] = {0};
  TEST_ASSERT(H5Dread(tree_ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_back) >= 0,
              "TreeHalosPerSnap dataset should read");
  H5Dclose(tree_ds);
  TEST_ASSERT(memcmp(read_back, tree_halos, sizeof(tree_halos)) == 0,
              "TreeHalosPerSnap contents should match InputHalosPerSnap");

  H5Dclose(dataset_id);
  H5Gclose(group_id);
  H5Fclose(HDF5_current_file_id);
  HDF5_current_file_id = -1;

  unlink(path);
  rmdir(dir_template);
  return TEST_PASS;
}

/**
 * @test    test_snapshot_run_attrs_omit_ntrees_and_tree_halos_per_snap
 * @brief   A snapshot-ordered run's attrs path never writes Ntrees or TreeHalosPerSnap, and
 *          never reads the tree-only InputHalosPerSnap
 */
static int test_snapshot_run_attrs_omit_ntrees_and_tree_halos_per_snap(void) {
  char dir_template[] = "/tmp/mimic_hdf5_attrs_snap_XXXXXX";
  char path[512];

  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS, "temp dir should be created");
  snprintf(path, sizeof(path), "%s/model_000.hdf5", dir_template);
  TEST_ASSERT(create_minimal_snap_fixture(path) == TEST_PASS, "fixture should be created");

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_SNAPSHOT;
  MimicConfig.ListOutputSnaps[0] = 0;

  /* Left at the never-allocated shape a real snapshot-ordered run has: NULL
   * InputHalosPerSnap and a poison Ntrees. If the mode gate regressed and
   * either were read, this would either crash (NULL deref) or write the
   * poison value -- both are things the assertions below would catch. */
  Ntrees = -1;
  InputHalosPerSnap[0] = NULL;
  TotHalosPerSnap[0] = 7000000456LL;

  perfile_metadata_calls = 0;
  HDF5_current_file_id = H5Fopen(path, H5F_ACC_RDWR, H5P_DEFAULT);
  TEST_ASSERT(HDF5_current_file_id >= 0, "fixture file should reopen for writing");

  write_hdf5_attrs(0, 0);

  hid_t group_id = H5Gopen(HDF5_current_file_id, "Snap000", H5P_DEFAULT);
  hid_t dataset_id = H5Dopen(group_id, "Galaxies", H5P_DEFAULT);

  TEST_ASSERT(!attr_exists(dataset_id, "Ntrees"), "snapshot run must not write Ntrees");
  TEST_ASSERT(!link_exists(group_id, "TreeHalosPerSnap"),
              "snapshot run must not write TreeHalosPerSnap");

  TEST_ASSERT(attr_exists(dataset_id, "TotHalosPerSnap"),
              "snapshot run should still write TotHalosPerSnap");
  int64_t tot_value = -1;
  TEST_ASSERT(read_int64_attr(dataset_id, "TotHalosPerSnap", &tot_value) == TEST_PASS,
              "TotHalosPerSnap attribute should read");
  TEST_ASSERT_EQUAL(tot_value, TotHalosPerSnap[0],
                    "TotHalosPerSnap attribute should carry the full int64 value");

  H5Dclose(dataset_id);
  H5Gclose(group_id);
  H5Fclose(HDF5_current_file_id);
  HDF5_current_file_id = -1;

  unlink(path);
  rmdir(dir_template);
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: HDF5 Write Attrs\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_tree_run_attrs_include_ntrees_and_tree_halos_per_snap);
  TEST_RUN(test_snapshot_run_attrs_omit_ntrees_and_tree_halos_per_snap);

  TEST_SUMMARY();
  return TEST_RESULT();
}
