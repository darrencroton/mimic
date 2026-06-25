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

static int write_scalar_attr(hid_t object, const char *name, hid_t type, const void *value) {
  hid_t space = H5Screate(H5S_SCALAR);
  hid_t attr = H5Acreate2(object, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
  int status = 0;
  if (space < 0 || attr < 0 || H5Awrite(attr, type, value) < 0) {
    status = -1;
  }
  if (attr >= 0)
    H5Aclose(attr);
  if (space >= 0)
    H5Sclose(space);
  return status;
}

static int write_simulation_params_group(hid_t file_group) {
  hid_t params = H5Gcreate2(file_group, "simulation_params", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (params < 0)
    return -1;

  int status = 0;
  const double omega_m = 0.3;
  const double omega_l = 0.7;
  const double hubble = 0.68;
  const double boxsize = 50.0;
  status |= write_scalar_attr(params, "Omega_M", H5T_NATIVE_DOUBLE, &omega_m);
  status |= write_scalar_attr(params, "Omega_L", H5T_NATIVE_DOUBLE, &omega_l);
  status |= write_scalar_attr(params, "hubble", H5T_NATIVE_DOUBLE, &hubble);
  status |= write_scalar_attr(params, "Boxsize", H5T_NATIVE_DOUBLE, &boxsize);
  H5Gclose(params);
  return status;
}

static int write_minimal_forests_file_with_options(const char *path, int snap_as_double,
                                                   double snap_double, int64_t snap_int,
                                                   int mismatched_mvir_length) {
  hid_t file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0)
    return -1;
  hid_t file0 = H5Gcreate2(file, "File0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  hid_t forests = H5Gcreate2(file0, "Forests", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const hsize_t n = 1;
  const int64_t link[1] = {-1};
  const int64_t id[1] = {42};
  const double scalar[1] = {1.0};
  const double mvir_mismatched[2] = {1.0, 2.0};
  int status = 0;

  const char *links[] = {"Descendant", "FirstProgenitor", "NextProgenitor", "FirstHaloInFOFgroup",
                         "NextHaloInFOFgroup"};
  for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
    status |= write_i64_dataset(forests, links[i], link, n);
  }
  const hsize_t mvir_n = mismatched_mvir_length ? n + 1 : n;
  status |= write_double_dataset(forests, "Mvir", mismatched_mvir_length ? mvir_mismatched : scalar,
                                 mvir_n);
  const char *doubles[] = {"x", "y", "z", "vrms", "vmax", "vx", "vy", "vz", "Jx", "Jy", "Jz"};
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

static int write_sequence_fields_to_group(hid_t forests, int64_t base_id, hsize_t n) {
  if (n > 4) {
    return -1;
  }
  int64_t link[4] = {-1, -1, -1, -1};
  int64_t id[4];
  int64_t snap[4];
  double values[4];
  int status = 0;
  for (hsize_t i = 0; i < n; i++) {
    id[i] = base_id + (int64_t)i;
    snap[i] = (int64_t)i;
    values[i] = 10.0 + (double)base_id + (double)i;
  }

  const char *links[] = {"Descendant", "FirstProgenitor", "NextProgenitor", "FirstHaloInFOFgroup",
                         "NextHaloInFOFgroup"};
  for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
    status |= write_i64_dataset(forests, links[i], link, n);
  }
  status |= write_double_dataset(forests, "Mvir", values, n);
  const char *doubles[] = {"x", "y", "z", "vrms", "vmax", "vx", "vy", "vz", "Jx", "Jy", "Jz"};
  for (size_t i = 0; i < sizeof(doubles) / sizeof(doubles[0]); i++) {
    status |= write_double_dataset(forests, doubles[i], values, n);
  }
  status |= write_i64_dataset(forests, "id", id, n);
  status |= write_i64_dataset(forests, "Snap_idx", snap, n);
  return status;
}

static int write_file_group_with_forests(hid_t file, int filenum,
                                         const struct test_forestinfo *rows, hsize_t nrows,
                                         hsize_t nhalos) {
  char group_name[32];
  snprintf(group_name, sizeof(group_name), "File%d", filenum);
  hid_t file_group = H5Gcreate2(file, group_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  hid_t forests = file_group >= 0
                      ? H5Gcreate2(file_group, "Forests", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT)
                      : -1;
  hid_t dtype = create_forestinfo_type();
  hid_t space = H5Screate_simple(1, &nrows, NULL);
  hid_t dset = file_group >= 0 ? H5Dcreate2(file_group, "ForestInfo", dtype, space, H5P_DEFAULT,
                                            H5P_DEFAULT, H5P_DEFAULT)
                               : -1;
  int status = 0;
  const int64_t nforests = (int64_t)nrows;
  const int8_t contig = 1;

  if (file_group < 0 || forests < 0 || dtype < 0 || space < 0 || dset < 0 ||
      H5Dwrite(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, rows) < 0) {
    status = -1;
  }
  if (status == 0) {
    status |= write_scalar_attr(file_group, "Nforests", H5T_NATIVE_INT64, &nforests);
    status |= write_scalar_attr(file_group, "contiguous-halo-props", H5T_NATIVE_INT8, &contig);
    status |= write_simulation_params_group(file_group);
    status |= write_sequence_fields_to_group(forests, (int64_t)filenum * 100, nhalos);
  }

  if (dset >= 0)
    H5Dclose(dset);
  if (space >= 0)
    H5Sclose(space);
  if (dtype >= 0)
    H5Tclose(dtype);
  if (forests >= 0)
    H5Gclose(forests);
  if (file_group >= 0)
    H5Gclose(file_group);
  return status;
}

static int write_multifile_forests_metadata(const char *path) {
  hid_t file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0)
    return -1;

  int status = 0;
  const int64_t nfiles = 3;
  status |= write_scalar_attr(file, "Nfiles", H5T_NATIVE_INT64, &nfiles);
  const struct test_forestinfo file0[2] = {{0, 0, 1, 1}, {1, 1, 2, 1}};
  const struct test_forestinfo file1[3] = {{2, 0, 1, 1}, {3, 1, 1, 1}, {4, 2, 2, 1}};
  const struct test_forestinfo file2[2] = {{5, 0, 1, 1}, {6, 1, 1, 1}};
  status |= write_file_group_with_forests(file, 0, file0, 2, 3);
  status |= write_file_group_with_forests(file, 1, file1, 3, 4);
  status |= write_file_group_with_forests(file, 2, file2, 2, 2);

  H5Fclose(file);
  return status;
}

static int write_minimal_forests_file(const char *path, int snap_as_double, double snap_double,
                                      int64_t snap_int) {
  return write_minimal_forests_file_with_options(path, snap_as_double, snap_double, snap_int, 0);
}

static int write_sequence_forests_file(const char *path) {
  hid_t file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0)
    return -1;
  hid_t file0 = H5Gcreate2(file, "File0", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  hid_t forests = H5Gcreate2(file0, "Forests", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const hsize_t n = 5;
  const int64_t descendant[5] = {1, -1, -1, -1, -1};
  const int64_t first_progenitor[5] = {-1, -1, -1, -1, -1};
  const int64_t next_progenitor[5] = {-1, -1, -1, -1, -1};
  const int64_t first_fof[5] = {1, -1, -1, -1, -1};
  const int64_t next_fof[5] = {-1, -1, -1, -1, -1};
  const int64_t id[5] = {100, 101, 102, 103, 104};
  const int64_t snap[5] = {0, 1, 2, 3, 4};
  const double mvir[5] = {10.0, 11.0, 12.0, 13.0, 14.0};
  const double x[5] = {20.0, 21.0, 22.0, 23.0, 24.0};
  const double y[5] = {30.0, 31.0, 32.0, 33.0, 34.0};
  const double z[5] = {40.0, 41.0, 42.0, 43.0, 44.0};
  const double vrms[5] = {50.0, 51.0, 52.0, 53.0, 54.0};
  const double vmax[5] = {60.0, 61.0, 62.0, 63.0, 64.0};
  const double vx[5] = {70.0, 71.0, 72.0, 73.0, 74.0};
  const double vy[5] = {80.0, 81.0, 82.0, 83.0, 84.0};
  const double vz[5] = {90.0, 91.0, 92.0, 93.0, 94.0};
  const double jx[5] = {1000.0, 1001.0, 1002.0, 1003.0, 1004.0};
  const double jy[5] = {2000.0, 2001.0, 2002.0, 2003.0, 2004.0};
  const double jz[5] = {3000.0, 3001.0, 3002.0, 3003.0, 3004.0};
  int status = 0;

  status |= write_i64_dataset(forests, "Descendant", descendant, n);
  status |= write_i64_dataset(forests, "FirstProgenitor", first_progenitor, n);
  status |= write_i64_dataset(forests, "NextProgenitor", next_progenitor, n);
  status |= write_i64_dataset(forests, "FirstHaloInFOFgroup", first_fof, n);
  status |= write_i64_dataset(forests, "NextHaloInFOFgroup", next_fof, n);
  status |= write_double_dataset(forests, "Mvir", mvir, n);
  status |= write_double_dataset(forests, "x", x, n);
  status |= write_double_dataset(forests, "y", y, n);
  status |= write_double_dataset(forests, "z", z, n);
  status |= write_double_dataset(forests, "vrms", vrms, n);
  status |= write_double_dataset(forests, "vmax", vmax, n);
  status |= write_i64_dataset(forests, "id", id, n);
  status |= write_i64_dataset(forests, "Snap_idx", snap, n);
  status |= write_double_dataset(forests, "vx", vx, n);
  status |= write_double_dataset(forests, "vy", vy, n);
  status |= write_double_dataset(forests, "vz", vz, n);
  status |= write_double_dataset(forests, "Jx", jx, n);
  status |= write_double_dataset(forests, "Jy", jy, n);
  status |= write_double_dataset(forests, "Jz", jz, n);

  if (forests >= 0)
    H5Gclose(forests);
  if (file0 >= 0)
    H5Gclose(file0);
  H5Fclose(file);
  return status;
}

static int assert_sequence_halo(const struct halo_data *halo, int source_index) {
  TEST_ASSERT(halo->Mvir == 10.0 + source_index, "Mvir should come from the requested slab");
  TEST_ASSERT(halo->Pos[0] == 20.0 + source_index, "x should come from the requested slab");
  TEST_ASSERT(halo->Pos[1] == 30.0 + source_index, "y should come from the requested slab");
  TEST_ASSERT(halo->Pos[2] == 40.0 + source_index, "z should come from the requested slab");
  TEST_ASSERT(halo->VelDisp == 50.0 + source_index, "vrms should come from the requested slab");
  TEST_ASSERT(halo->Vmax == 60.0 + source_index, "vmax should come from the requested slab");
  TEST_ASSERT(halo->MostBoundID == 100 + source_index, "id should come from the requested slab");
  TEST_ASSERT(halo->SnapNum == source_index, "Snap_idx should come from the requested slab");
  TEST_ASSERT(halo->Vel[0] == 70.0 + source_index, "vx should come from the requested slab");
  TEST_ASSERT(halo->Vel[1] == 80.0 + source_index, "vy should come from the requested slab");
  TEST_ASSERT(halo->Vel[2] == 90.0 + source_index, "vz should come from the requested slab");
  TEST_ASSERT(halo->Spin[0] == 1000.0 + source_index, "Jx should come from the requested slab");
  TEST_ASSERT(halo->Spin[1] == 2000.0 + source_index, "Jy should come from the requested slab");
  TEST_ASSERT(halo->Spin[2] == 3000.0 + source_index, "Jz should come from the requested slab");
  return TEST_PASS;
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

int test_field_cache_validates_schema_at_open(void) {
  init_memory_system(0);
  char dir_template[] = "/tmp/mimic_ctrees_h5_fields_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);
  TEST_ASSERT(write_minimal_forests_file(path, 0, 0.0, 0) == 0,
              "should write valid Forests datasets");
  TEST_ASSERT(ctrees_hdf5_test_open_field_cache(path, "Snap_idx", 0) == EXIT_SUCCESS,
              "valid field handles should open and validate once");

  TEST_ASSERT(write_minimal_forests_file_with_options(path, 0, 0.0, 0, 1) == 0,
              "should rewrite Forests datasets with a mismatched Mvir extent");
  TEST_ASSERT(ctrees_hdf5_test_open_field_cache(path, "Snap_idx", 0) != EXIT_SUCCESS,
              "field extent mismatch must fail during cache setup");

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

int test_window_refills_across_sequential_forests(void) {
  init_memory_system(0);
  MimicConfig.LastSnapshotNr = 10;

  char dir_template[] = "/tmp/mimic_ctrees_h5_window_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);
  TEST_ASSERT(write_sequence_forests_file(path) == 0, "should write sequence Forests datasets");

  struct halo_data first[2];
  struct halo_data second[2];
  TEST_ASSERT(ctrees_hdf5_test_read_two_forests_windowed(path, "Snap_idx", 0, 0, 2, first, 3, 2,
                                                         second) == EXIT_SUCCESS,
              "two window-sized forests should read across a forced refill");
  TEST_ASSERT(assert_sequence_halo(&first[0], 0) == TEST_PASS,
              "first forest should start at source halo 0");
  TEST_ASSERT(assert_sequence_halo(&first[1], 1) == TEST_PASS,
              "first forest should include source halo 1");
  TEST_ASSERT(assert_sequence_halo(&second[0], 3) == TEST_PASS,
              "second forest should start at source halo 3 after refill");
  TEST_ASSERT(assert_sequence_halo(&second[1], 4) == TEST_PASS,
              "second forest should include source halo 4 after refill");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int test_giant_forest_uses_direct_read_path(void) {
  init_memory_system(0);
  MimicConfig.LastSnapshotNr = 10;

  char dir_template[] = "/tmp/mimic_ctrees_h5_giant_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  char path[512];
  snprintf(path, sizeof(path), "%s/trees.h5", dir_template);
  TEST_ASSERT(write_sequence_forests_file(path) == 0, "should write sequence Forests datasets");

  struct halo_data halos[3];
  TEST_ASSERT(ctrees_hdf5_test_read_forest(path, "Snap_idx", 0, 0, 3, halos) == EXIT_SUCCESS,
              "forest larger than the test window should use the direct read path");
  TEST_ASSERT(assert_sequence_halo(&halos[0], 0) == TEST_PASS,
              "giant direct path should read source halo 0");
  TEST_ASSERT(assert_sequence_halo(&halos[2], 2) == TEST_PASS,
              "giant direct path should read source halo 2");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int test_run_prepare_survives_repeated_range_staging(void) {
  init_memory_system(0);
  MimicConfig.LastSnapshotNr = 10;
  MimicConfig.Omega = 0.3;
  MimicConfig.OmegaLambda = 0.7;
  MimicConfig.Hubble_h = 0.68;
  MimicConfig.BoxSize = 50.0;

  char dir_template[] = "/tmp/mimic_ctrees_h5_stage_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  const char *tree_name = "trees.h5";
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir_template, tree_name);
  TEST_ASSERT(write_multifile_forests_metadata(path) == 0,
              "should write a multi-file forests-HDF5 metadata fixture");

  const int64_t starts[2] = {1, 5};
  const int64_t counts[2] = {1, 2};
  struct ctrees_hdf5_test_stage_probe probes[2];
  TEST_ASSERT(ctrees_hdf5_test_prepare_and_stage_ranges(dir_template, tree_name, starts, counts,
                                                        probes) == EXIT_SUCCESS,
              "run prepare should support repeated non-adjacent range staging");

  TEST_ASSERT(probes[0].ntrees == 1, "first staged range should expose one forest");
  TEST_ASSERT(probes[0].global_forest_offset == 1,
              "first staged range should publish its global forest offset");
  TEST_ASSERT(probes[0].start_filenum == 0 && probes[0].end_filenum == 0,
              "first staged range should touch only file 0");
  TEST_ASSERT(probes[0].first_unit_filenum == 0 && probes[0].first_unit_treenr_in_file == 1,
              "first staged range should map to file 0 row 1");
  TEST_ASSERT(probes[0].has_active_field_cache, "first staged file should have open field handles");

  TEST_ASSERT(probes[1].ntrees == 2, "second staged range should expose two forests");
  TEST_ASSERT(probes[1].global_forest_offset == 5,
              "second staged range should publish its global forest offset");
  TEST_ASSERT(probes[1].start_filenum == 2 && probes[1].end_filenum == 2,
              "second staged range should touch only file 2");
  TEST_ASSERT(probes[1].first_unit_filenum == 2 && probes[1].first_unit_treenr_in_file == 0,
              "second staged range should map to file 2 row 0");
  TEST_ASSERT(probes[1].has_active_field_cache,
              "second staged file should rebuild its field handles");
  TEST_ASSERT(!probes[1].stale_file0_cache_present,
              "second staging should not retain file 0 field handles");
  TEST_ASSERT(probes[0].run_counts_available && probes[1].run_counts_available,
              "run-scoped forest counts should survive across stage/unstage");

  unlink(path);
  rmdir(dir_template);
  check_memory_leaks();
  return TEST_PASS;
}

int test_prepare_builds_chunk_plan_from_forest_counts(void) {
  init_memory_system(0);
  MimicConfig.LastSnapshotNr = 10;
  MimicConfig.Omega = 0.3;
  MimicConfig.OmegaLambda = 0.7;
  MimicConfig.Hubble_h = 0.68;
  MimicConfig.BoxSize = 50.0;

  char dir_template[] = "/tmp/mimic_ctrees_h5_chunks_XXXXXX";
  TEST_ASSERT(create_dir(dir_template) == 0, "mkdtemp should create a temp directory");

  const char *tree_name = "trees.h5";
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir_template, tree_name);
  TEST_ASSERT(write_multifile_forests_metadata(path) == 0,
              "should write a multi-file forests-HDF5 metadata fixture");

  int nchunks = 0;
  int64_t starts[4] = {0};
  int64_t counts[4] = {0};
  double costs[4] = {0.0};
  TEST_ASSERT(ctrees_hdf5_test_prepare_chunk_plan(dir_template, tree_name, 3, 1024, 4, starts,
                                                  counts, costs, &nchunks) == EXIT_SUCCESS,
              "run prepare should build a chunk plan from ForestInfo halo counts");

  TEST_ASSERT(nchunks == 3, "seven forests with forests_per_file=3 should produce three chunks");
  TEST_ASSERT(starts[0] == 0 && counts[0] == 3, "chunk 0 should cover forests [0, 3)");
  TEST_ASSERT(starts[1] == 3 && counts[1] == 3, "chunk 1 should cover forests [3, 6)");
  TEST_ASSERT(starts[2] == 6 && counts[2] == 1, "chunk 2 should cover forests [6, 7)");
  TEST_ASSERT(costs[0] == 4.0 && costs[1] == 4.0 && costs[2] == 1.0,
              "linear chunk costs should sum ForestNhalos over each chunk");

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
  TEST_RUN(test_field_cache_validates_schema_at_open);
  TEST_RUN(test_snapshot_values_are_strict);
  TEST_RUN(test_window_refills_across_sequential_forests);
  TEST_RUN(test_giant_forest_uses_direct_read_path);
  TEST_RUN(test_run_prepare_survives_repeated_range_staging);
  TEST_RUN(test_prepare_builds_chunk_plan_from_forest_counts);

  TEST_SUMMARY();
  return TEST_RESULT();
}
