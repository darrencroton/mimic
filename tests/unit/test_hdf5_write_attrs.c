/**
 * @file    test_hdf5_write_attrs.c
 * @brief   Unit tests for write_hdf5_attrs()'s driver-neutral snapshot count attributes, and
 *          for a partition written in two visits through reopen_hdf5_output_file().
 */

#include "../framework/test_framework.h"

#include "error.h"
#include "globals.h"
#include "output/hdf5.h"
#include "output/hdf5_internal.h"
#include "output/util.h"
#include "vertical/reader.h"

#include <hdf5.h>
#include <hdf5_hl.h>
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
 * @brief   A vertical run's attrs path writes Ntrees, TreeHalosPerSnap, and int64
 *          TotHalosPerSnap
 */
static int test_tree_run_attrs_include_ntrees_and_tree_halos_per_snap(void) {
  char dir_template[] = "/tmp/mimic_hdf5_attrs_tree_XXXXXX";
  char path[512];

  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS, "temp dir should be created");
  snprintf(path, sizeof(path), "%s/model_000.hdf5", dir_template);
  TEST_ASSERT(create_minimal_snap_fixture(path) == TEST_PASS, "fixture should be created");

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_VERTICAL;
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

  TEST_ASSERT_EQUAL(perfile_metadata_calls, 0,
                    "write_hdf5_attrs must never write per-file metadata (moved to file open)");

  hid_t group_id = H5Gopen(HDF5_current_file_id, "Snap000", H5P_DEFAULT);
  hid_t dataset_id = H5Dopen(group_id, "Galaxies", H5P_DEFAULT);

  TEST_ASSERT(attr_exists(dataset_id, "Ntrees"), "vertical run should write Ntrees");
  int64_t ntrees_value = -1;
  TEST_ASSERT(read_int64_attr(dataset_id, "TotHalosPerSnap", &ntrees_value) == TEST_PASS,
              "TotHalosPerSnap attribute should read");
  TEST_ASSERT_EQUAL(ntrees_value, TotHalosPerSnap[0],
                    "TotHalosPerSnap attribute should carry the full int64 value");

  TEST_ASSERT(link_exists(group_id, "TreeHalosPerSnap"),
              "vertical run should write the TreeHalosPerSnap dataset");
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
 * @test    test_horizontal_run_attrs_omit_ntrees_and_tree_halos_per_snap
 * @brief   A horizontal run's attrs path never writes Ntrees or TreeHalosPerSnap, and
 *          never reads the vertical-only InputHalosPerSnap
 */
static int test_horizontal_run_attrs_omit_ntrees_and_tree_halos_per_snap(void) {
  char dir_template[] = "/tmp/mimic_hdf5_attrs_snap_XXXXXX";
  char path[512];

  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS, "temp dir should be created");
  snprintf(path, sizeof(path), "%s/model_000.hdf5", dir_template);
  TEST_ASSERT(create_minimal_snap_fixture(path) == TEST_PASS, "fixture should be created");

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  MimicConfig.ListOutputSnaps[0] = 0;

  /* Left at the never-allocated shape a real horizontal run has: NULL
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

  TEST_ASSERT(!attr_exists(dataset_id, "Ntrees"), "horizontal run must not write Ntrees");
  TEST_ASSERT(!link_exists(group_id, "TreeHalosPerSnap"),
              "horizontal run must not write TreeHalosPerSnap");

  TEST_ASSERT(attr_exists(dataset_id, "TotHalosPerSnap"),
              "horizontal run should still write TotHalosPerSnap");
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

/* The two-visit case's partition: output snapshot 5 at index 0, no task
 * component, so the file is <dir>/model_005.hdf5. Two splits of the 2,500 rows are
 * written. 1,300 + 1,200 puts the visit boundary inside a 1,000-record table
 * chunk, and the second visit's first append lands in a chunk the first visit left
 * partly filled. 0 + 2,500 has the first visit create the file with an empty
 * table (chunk 0 holds no rows of the output snapshot) and the second visit
 * append every row. */
#define TWO_VISIT_SNAPNUM 5
#define TWO_VISIT_ROWS 2500
#define TWO_VISIT_SPLIT 1300
#define TWO_VISIT_FIRST_ID 1000LL

/**
 * @brief   Write @p count hand-built rows to this partition in one or two visits.
 * @param   halos   The output buffer, in row order.
 * @param   count   Rows in it.
 * @param   split   Rows written on the first visit; the rest go on a second
 *                  visit through reopen_hdf5_output_file(). Equal to @p count for
 *                  a one-visit file.
 *
 * Follows the horizontal driver's visit sequence at the writer's public seam:
 * create, save, flush and close on the first visit; reopen, save, flush on the
 * second; stamp TotHalosPerSnap once, on the last visit; close.
 */
static int write_partition_in_visits(struct Halo *halos, int64_t count, int64_t split) {
  const int indices[1] = {0};
  const struct OutputSnapshotSelection selection = {1, indices};
  const struct HaloInputView view = {NULL, 0};

  TotHalosPerSnap[0] = 0;

  open_hdf5_output_file(TWO_VISIT_SNAPNUM, -1, selection);
  ProcessedHalos = halos;
  NumProcessedHalos = split;
  save_halos_hdf5(TWO_VISIT_SNAPNUM, 0, view, selection);
  flush_hdf5_buffers(TWO_VISIT_SNAPNUM, selection);

  if (split < count) {
    TEST_ASSERT(H5Fclose(HDF5_current_file_id) >= 0, "first visit should close cleanly");
    HDF5_current_file_id = -1;

    reopen_hdf5_output_file(TWO_VISIT_SNAPNUM, -1);
    TEST_ASSERT(HDF5_current_file_id >= 0, "reopen should leave the partition open");
    ProcessedHalos = halos + split;
    NumProcessedHalos = count - split;
    save_halos_hdf5(TWO_VISIT_SNAPNUM, 0, view, selection);
    flush_hdf5_buffers(TWO_VISIT_SNAPNUM, selection);
  }

  ProcessedHalos = NULL;
  NumProcessedHalos = 0;

  write_hdf5_attrs(0, TWO_VISIT_SNAPNUM);
  TEST_ASSERT(H5Fclose(HDF5_current_file_id) >= 0, "last visit should close cleanly");
  HDF5_current_file_id = -1;
  return TEST_PASS;
}

/**
 * @brief   Read a partition's whole Galaxies table as its stored bytes and its
 *          TotHalosPerSnap stamp.
 * @param   path       The partition file.
 * @param   bytes      Receives a malloc'd copy of the table in the file's own
 *                     record type; the caller frees it.
 * @param   nbytes     Receives its size.
 * @param   nrows      Receives the table's row count.
 * @param   tot        Receives the TotHalosPerSnap attribute.
 */
static int read_partition_table(const char *path, unsigned char **bytes, size_t *nbytes,
                                hsize_t *nrows, int64_t *tot) {
  hid_t file_id = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  TEST_ASSERT(file_id >= 0, "partition should open for reading");
  hid_t dataset_id = H5Dopen(file_id, "Snap005/Galaxies", H5P_DEFAULT);
  TEST_ASSERT(dataset_id >= 0, "Snap005/Galaxies should open");

  hid_t space_id = H5Dget_space(dataset_id);
  TEST_ASSERT(H5Sget_simple_extent_dims(space_id, nrows, NULL) == 1, "the table is 1-D");
  hid_t type_id = H5Dget_type(dataset_id);
  *nbytes = (size_t)*nrows * H5Tget_size(type_id);
  *bytes = malloc(*nbytes > 0 ? *nbytes : 1);
  TEST_ASSERT(*bytes != NULL, "table buffer should allocate");
  TEST_ASSERT(H5Dread(dataset_id, type_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, *bytes) >= 0,
              "the table should read in its stored type");
  TEST_ASSERT(read_int64_attr(dataset_id, "TotHalosPerSnap", tot) == TEST_PASS,
              "TotHalosPerSnap should read");

  H5Tclose(type_id);
  H5Sclose(space_id);
  H5Dclose(dataset_id);
  H5Fclose(file_id);
  return TEST_PASS;
}

/**
 * @test    test_partition_written_in_two_visits_matches_one_visit
 * @brief   A partition reopened by reopen_hdf5_output_file() and appended to on a
 *          second visit holds every row in order, stamps the accumulated count
 *          once, writes per-file metadata only at creation, and stores the same
 *          table bytes as the same rows written in one visit, whether the first
 *          visit wrote 1,300 rows or none
 */
static int test_partition_written_in_two_visits_matches_one_visit(void) {
  char one_dir[] = "/tmp/mimic_hdf5_one_visit_XXXXXX";
  char two_dir[] = "/tmp/mimic_hdf5_two_visits_XXXXXX";
  char one_path[512], two_path[512];

  TEST_ASSERT(create_temp_output_dir(one_dir) == TEST_PASS, "temp dir should be created");
  TEST_ASSERT(create_temp_output_dir(two_dir) == TEST_PASS, "temp dir should be created");
  snprintf(one_path, sizeof(one_path), "%s/model_%03d.hdf5", one_dir, TWO_VISIT_SNAPNUM);
  snprintf(two_path, sizeof(two_path), "%s/model_%03d.hdf5", two_dir, TWO_VISIT_SNAPNUM);

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  MimicConfig.OutputFormat = output_hdf5;
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = TWO_VISIT_SNAPNUM;
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "model");

  calc_hdf5_props();

  /* Orphans (Type 2), so output conversion keeps the stored Rvir/Vvir and never
   * reads the raw-halo view; every row carries a distinct id and payload, so a
   * dropped, duplicated or reordered row changes the table. */
  struct GalaxyData *galaxy = calloc(1, sizeof(*galaxy));
  struct Halo *halos = calloc(TWO_VISIT_ROWS, sizeof(*halos));
  TEST_ASSERT(galaxy != NULL && halos != NULL, "fixture buffers should allocate");
  for (int i = 0; i < TWO_VISIT_ROWS; i++) {
    halos[i].SnapNum = TWO_VISIT_SNAPNUM;
    halos[i].Type = 2;
    halos[i].HaloNr = i;
    halos[i].UniqueGalaxyID = TWO_VISIT_FIRST_ID + i;
    halos[i].dT = -1.0;
    halos[i].Len = 20 + i;
    halos[i].Mvir = 0.25 * i;
    halos[i].Rvir = 0.001 * i;
    halos[i].Vvir = 1.5 * i;
    halos[i].galaxy = galaxy;
  }

  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", one_dir);
  perfile_metadata_calls = 0;
  TEST_ASSERT(write_partition_in_visits(halos, TWO_VISIT_ROWS, TWO_VISIT_ROWS) == TEST_PASS,
              "the one-visit partition should write");
  TEST_ASSERT_EQUAL(perfile_metadata_calls, 1, "a one-visit partition writes metadata once");

  unsigned char *one_bytes = NULL;
  size_t one_nbytes = 0;
  hsize_t one_rows = 0;
  int64_t one_tot = -1;
  TEST_ASSERT(read_partition_table(one_path, &one_bytes, &one_nbytes, &one_rows, &one_tot) ==
                  TEST_PASS,
              "the one-visit table should read");

  const int64_t splits[] = {TWO_VISIT_SPLIT, 0};
  unsigned char *two_bytes = NULL;
  for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); s++) {
    unlink(two_path);
    snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", two_dir);
    perfile_metadata_calls = 0;
    TEST_ASSERT(write_partition_in_visits(halos, TWO_VISIT_ROWS, splits[s]) == TEST_PASS,
                "the two-visit partition should write");
    TEST_ASSERT_EQUAL(perfile_metadata_calls, 1,
                      "per-file metadata is written on the first visit, not on the reopen");

    size_t two_nbytes = 0;
    hsize_t two_rows = 0;
    int64_t two_tot = -1;
    TEST_ASSERT(read_partition_table(two_path, &two_bytes, &two_nbytes, &two_rows, &two_tot) ==
                    TEST_PASS,
                "the two-visit table should read");

    TEST_ASSERT_EQUAL((int64_t)two_rows, (int64_t)TWO_VISIT_ROWS,
                      "the two-visit table should hold every row of both visits");
    TEST_ASSERT_EQUAL(two_tot, (int64_t)TWO_VISIT_ROWS,
                      "TotHalosPerSnap should accumulate across both visits");
    TEST_ASSERT_EQUAL(one_tot, two_tot, "both partitions should stamp the same count");

    /* Row order: the id column read back in file order is the buffer's order. */
    struct HaloOutput *rows = malloc(TWO_VISIT_ROWS * sizeof(*rows));
    TEST_ASSERT(rows != NULL, "row buffer should allocate");
    hid_t file_id = H5Fopen(two_path, H5F_ACC_RDONLY, H5P_DEFAULT);
    TEST_ASSERT(file_id >= 0, "the two-visit partition should reopen for reading");
    TEST_ASSERT(H5TBread_table(file_id, "Snap005/Galaxies", HDF5_dst_size, HDF5_dst_offsets,
                               HDF5_dst_sizes, rows) >= 0,
                "the two-visit table should read as HaloOutput records");
    H5Fclose(file_id);
    int64_t out_of_order = 0;
    for (int i = 0; i < TWO_VISIT_ROWS; i++) {
      if (rows[i].UniqueGalaxyID != TWO_VISIT_FIRST_ID + i) {
        out_of_order++;
      }
    }
    free(rows);
    TEST_ASSERT_EQUAL(out_of_order, (int64_t)0,
                      "every row should be in the original order across the visit boundary");

    TEST_ASSERT_EQUAL((int64_t)two_nbytes, (int64_t)one_nbytes,
                      "both tables should hold the same number of bytes");
    TEST_ASSERT(memcmp(one_bytes, two_bytes, one_nbytes) == 0,
                "the two-visit table's bytes should equal the one-visit table's");
    free(two_bytes);
    two_bytes = NULL;
  }

  free(one_bytes);
  free(halos);
  free(galaxy);
  free_hdf5_ids();

  unlink(one_path);
  unlink(two_path);
  rmdir(one_dir);
  rmdir(two_dir);
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
  TEST_RUN(test_horizontal_run_attrs_omit_ntrees_and_tree_halos_per_snap);
  TEST_RUN(test_partition_written_in_two_visits_matches_one_visit);

  TEST_SUMMARY();
  return TEST_RESULT();
}
