/**
 * @file    test_ctrees_hdf5_reader.c
 * @brief   Focused unit tests for Consistent-Trees HDF5 reader validation.
 *
 * The fixture files are synthetic and tiny: they exercise ForestInfo length and
 * halo-slab validation plus strict snapshot parsing without depending on a real
 * Consistent-Trees production dataset.
 */

#include "../framework/test_framework.h"

#include "error.h"
#include "globals.h"
#include "memory.h"
#include "tree/read_ctrees_hdf5.h"

#include <hdf5.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0, failed = 0;

struct test_forestinfo {
  int64_t forestid;
  int64_t foresthalosoffset;
  int64_t forestnhalos;
  int64_t forestntrees;
};

static int create_dir(char *template) { return mkdtemp(template) == NULL ? -1 : 0; }

static hid_t create_forestinfo_type(void) {
  hid_t dtype = H5Tcreate(H5T_COMPOUND, sizeof(struct test_forestinfo));
  if (dtype < 0)
    return -1;
  H5Tinsert(dtype, "ForestID", HOFFSET(struct test_forestinfo, forestid), H5T_NATIVE_INT64);
  H5Tinsert(dtype, "ForestHalosOffset", HOFFSET(struct test_forestinfo, foresthalosoffset),
            H5T_NATIVE_INT64);
  H5Tinsert(dtype, "ForestNhalos", HOFFSET(struct test_forestinfo, forestnhalos), H5T_NATIVE_INT64);
  H5Tinsert(dtype, "ForestNTrees", HOFFSET(struct test_forestinfo, forestntrees), H5T_NATIVE_INT64);
  return dtype;
}

static hid_t create_reordered_forestinfo_file_type(void) {
  hid_t dtype = H5Tcreate(H5T_COMPOUND, sizeof(struct test_forestinfo));
  if (dtype < 0)
    return -1;
  H5Tinsert(dtype, "ForestNhalos", 0, H5T_NATIVE_INT64);
  H5Tinsert(dtype, "ForestHalosOffset", sizeof(int64_t), H5T_NATIVE_INT64);
  H5Tinsert(dtype, "ForestID", 2 * sizeof(int64_t), H5T_NATIVE_INT64);
  H5Tinsert(dtype, "ForestNTrees", 3 * sizeof(int64_t), H5T_NATIVE_INT64);
  return dtype;
}

static int write_forestinfo_file(const char *path, const struct test_forestinfo *rows,
                                 hsize_t nrows) {
  hid_t file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0)
    return -1;
  hid_t file0 = H5Gcreate2(file, "File0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  hid_t dtype = create_forestinfo_type();
  hid_t space = H5Screate_simple(1, &nrows, NULL);
  hid_t dset = H5Dcreate2(file0, "ForestInfo", dtype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  int status = 0;
  if (file0 < 0 || dtype < 0 || space < 0 || dset < 0 ||
      H5Dwrite(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, rows) < 0) {
    status = -1;
  }
  if (dset >= 0)
    H5Dclose(dset);
  if (space >= 0)
    H5Sclose(space);
  if (dtype >= 0)
    H5Tclose(dtype);
  if (file0 >= 0)
    H5Gclose(file0);
  H5Fclose(file);
  return status;
}

static int write_reordered_forestinfo_file(const char *path, const struct test_forestinfo *rows,
                                           hsize_t nrows) {
  hid_t file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0)
    return -1;
  hid_t file0 = H5Gcreate2(file, "File0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  hid_t file_dtype = create_reordered_forestinfo_file_type();
  hid_t mem_dtype = create_forestinfo_type();
  hid_t space = H5Screate_simple(1, &nrows, NULL);
  hid_t dset =
      H5Dcreate2(file0, "ForestInfo", file_dtype, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  int status = 0;
  if (file0 < 0 || file_dtype < 0 || mem_dtype < 0 || space < 0 || dset < 0 ||
      H5Dwrite(dset, mem_dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, rows) < 0) {
    status = -1;
  }
  if (dset >= 0)
    H5Dclose(dset);
  if (space >= 0)
    H5Sclose(space);
  if (mem_dtype >= 0)
    H5Tclose(mem_dtype);
  if (file_dtype >= 0)
    H5Tclose(file_dtype);
  if (file0 >= 0)
    H5Gclose(file0);
  H5Fclose(file);
  return status;
}

static int write_i64_dataset(hid_t group, const char *name, const int64_t *values, hsize_t n) {
  hid_t space = H5Screate_simple(1, &n, NULL);
  hid_t dset =
      H5Dcreate2(group, name, H5T_NATIVE_INT64, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  int status = 0;
  if (space < 0 || dset < 0 ||
      H5Dwrite(dset, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, values) < 0) {
    status = -1;
  }
  if (dset >= 0)
    H5Dclose(dset);
  if (space >= 0)
    H5Sclose(space);
  return status;
}

static int write_double_dataset(hid_t group, const char *name, const double *values, hsize_t n) {
  hid_t space = H5Screate_simple(1, &n, NULL);
  hid_t dset =
      H5Dcreate2(group, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  int status = 0;
  if (space < 0 || dset < 0 ||
      H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, values) < 0) {
    status = -1;
  }
  if (dset >= 0)
    H5Dclose(dset);
  if (space >= 0)
    H5Sclose(space);
  return status;
}

static int write_minimal_forests_file(const char *path, int snap_as_double, double snap_double,
                                      int64_t snap_int) {
  hid_t file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0)
    return -1;
  hid_t file0 = H5Gcreate2(file, "File0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  hid_t forests = H5Gcreate2(file0, "Forests", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const hsize_t n = 1;
  const int64_t link[1] = {-1};
  const int64_t id[1] = {42};
  const double scalar[1] = {1.0};
  int status = 0;

  const char *links[] = {"Descendant", "FirstProgenitor", "NextProgenitor", "FirstHaloInFOFgroup",
                         "NextHaloInFOFgroup"};
  for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
    status |= write_i64_dataset(forests, links[i], link, n);
  }
  const char *doubles[] = {"Mvir", "x",  "y",  "z",  "vrms", "vmax",
                           "vx",   "vy", "vz", "Jx", "Jy",   "Jz"};
  for (size_t i = 0; i < sizeof(doubles) / sizeof(doubles[0]); i++) {
    status |= write_double_dataset(forests, doubles[i], scalar, n);
  }
  status |= write_i64_dataset(forests, "id", id, n);
  if (snap_as_double) {
    const double snap[1] = {snap_double};
    status |= write_double_dataset(forests, "Snap_idx", snap, n);
  } else {
    const int64_t snap[1] = {snap_int};
    status |= write_i64_dataset(forests, "Snap_idx", snap, n);
  }

  if (forests >= 0)
    H5Gclose(forests);
  if (file0 >= 0)
    H5Gclose(file0);
  H5Fclose(file);
  return status;
}

int test_forestinfo_length_and_counts_are_validated(void) {
  init_memory_system(0);
  char dir_template[] = "/tmp/mimic_ctrees_h5_info_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);
  const struct test_forestinfo valid_rows[2] = {{0, 0, 3, 1}, {1, 3, 4, 1}};
  TEST_ASSERT(write_forestinfo_file(path, valid_rows, 2) == 0, "should write ForestInfo");

  int64_t nhalos[2] = {0, 0};
  TEST_ASSERT(ctrees_hdf5_test_read_nhalos_per_forest(path, 2, nhalos) == EXIT_SUCCESS,
              "matching ForestInfo length should be accepted");
  TEST_ASSERT(nhalos[0] == 3 && nhalos[1] == 4, "ForestNhalos values should be read by row");
  TEST_ASSERT(ctrees_hdf5_test_read_nhalos_per_forest(path, 1, nhalos) != EXIT_SUCCESS,
              "mismatched Nforests and ForestInfo length must fail");

  int64_t halosoffset = -1, cached_nhalos = -1;
  TEST_ASSERT(ctrees_hdf5_test_read_forestinfo_cache(path, 2, 1, &halosoffset, &cached_nhalos) ==
                  EXIT_SUCCESS,
              "task ForestInfo cache should accept matching rows");
  TEST_ASSERT(halosoffset == 3 && cached_nhalos == 4,
              "task ForestInfo cache should preserve offset and count by row");
  TEST_ASSERT(ctrees_hdf5_test_read_forestinfo_cache(path, 1, 0, &halosoffset, &cached_nhalos) !=
                  EXIT_SUCCESS,
              "task ForestInfo cache should reject mismatched row count");

  const struct test_forestinfo negative_row[1] = {{0, 0, -1, 1}};
  TEST_ASSERT(write_forestinfo_file(path, negative_row, 1) == 0, "should rewrite ForestInfo");
  TEST_ASSERT(ctrees_hdf5_test_read_nhalos_per_forest(path, 1, nhalos) != EXIT_SUCCESS,
              "negative ForestNhalos must fail");
  TEST_ASSERT(ctrees_hdf5_test_read_forestinfo_cache(path, 1, 0, &halosoffset, &cached_nhalos) !=
                  EXIT_SUCCESS,
              "task ForestInfo cache should reject negative ForestNhalos at setup");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int test_forestinfo_cache_reads_members_by_name(void) {
  init_memory_system(0);
  char dir_template[] = "/tmp/mimic_ctrees_h5_info_order_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);
  const struct test_forestinfo rows[1] = {{101, 7, 5, 1}};
  TEST_ASSERT(write_reordered_forestinfo_file(path, rows, 1) == 0,
              "should write reordered ForestInfo members");

  int64_t halosoffset = -1, cached_nhalos = -1;
  TEST_ASSERT(ctrees_hdf5_test_read_forestinfo_cache(path, 1, 0, &halosoffset, &cached_nhalos) ==
                  EXIT_SUCCESS,
              "task ForestInfo cache should accept reordered members");
  TEST_ASSERT(halosoffset == 7 && cached_nhalos == 5,
              "task ForestInfo cache should map ForestInfo members by name");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int test_forest_slab_bounds_are_validated(void) {
  init_memory_system(0);
  char dir_template[] = "/tmp/mimic_ctrees_h5_slab_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);
  TEST_ASSERT(write_minimal_forests_file(path, 0, 0.0, 0) == 0, "should write Forests datasets");

  TEST_ASSERT(ctrees_hdf5_test_validate_forest_slab(path, 0, 1) == EXIT_SUCCESS,
              "valid slab should pass");
  TEST_ASSERT(ctrees_hdf5_test_validate_forest_slab(path, -1, 1) != EXIT_SUCCESS,
              "negative halo offset must fail");
  TEST_ASSERT(ctrees_hdf5_test_validate_forest_slab(path, 1, 1) != EXIT_SUCCESS,
              "slab past dataset extent must fail");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int test_snapshot_values_are_strict(void) {
  init_memory_system(0);
  MimicConfig.LastSnapshotNr = 10;

  char dir_template[] = "/tmp/mimic_ctrees_h5_snap_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  struct halo_data halo[1];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);

  TEST_ASSERT(write_minimal_forests_file(path, 0, 0.0, 3) == 0, "should write valid snap file");
  TEST_ASSERT(ctrees_hdf5_test_read_forest(path, "Snap_idx", 0, 0, 1, halo) == EXIT_SUCCESS,
              "integer Snap_idx in range should pass");
  TEST_ASSERT(halo[0].SnapNum == 3, "SnapNum should be assigned from the integer dataset");

  TEST_ASSERT(write_minimal_forests_file(path, 1, 1.5, 0) == 0,
              "should rewrite fractional snap file");
  TEST_ASSERT(ctrees_hdf5_test_read_forest(path, "Snap_idx", 1, 0, 1, halo) != EXIT_SUCCESS,
              "fractional floating Snap_idx must fail");

  TEST_ASSERT(write_minimal_forests_file(path, 0, 0.0, 11) == 0,
              "should rewrite out-of-range snap file");
  TEST_ASSERT(ctrees_hdf5_test_read_forest(path, "Snap_idx", 0, 0, 1, halo) != EXIT_SUCCESS,
              "Snap_idx beyond LastSnapshotNr must fail");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Consistent-Trees HDF5 Reader\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_forestinfo_length_and_counts_are_validated);
  TEST_RUN(test_forestinfo_cache_reads_members_by_name);
  TEST_RUN(test_forest_slab_bounds_are_validated);
  TEST_RUN(test_snapshot_values_are_strict);

  TEST_SUMMARY();
  return TEST_RESULT();
}
