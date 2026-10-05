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
#include "horizontal/reader.h"
#include "vertical/reader.h"

#include <dirent.h>
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

/*
 * The real per-file HDF5 writer, compiled into this translation unit so that
 * open_hdf5_output_file() can be checked to name a file by the task it is given
 * (test_writer_names_file_by_given_task). The unit
 * runner links src/io/output/hdf5.c only for the tests it names
 * (tests/unit/run_tests.sh), and allvars.c is compiled without -DHDF5 there, so
 * this file supplies the per-file HDF5 globals and stubs the two metadata_hdf5.c
 * symbols the writer calls -- the same pattern as test_hdf5_write_attrs.c.
 */
#include "../../src/io/output/hdf5.c"

size_t HDF5_dst_size;
size_t *HDF5_dst_offsets;
size_t *HDF5_dst_sizes;
const char **HDF5_field_names;
hid_t *HDF5_field_types;
int HDF5_n_props;
hid_t HDF5_current_file_id = -1;

void write_perfile_metadata(hid_t file_id) { (void)file_id; }

void write_description_attr(hid_t obj_id, const char *text) {
  (void)obj_id;
  (void)text;
}

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

static const struct VerticalReader EnumeratedMasterReader = {
    .name = "master_enumerated",
    .file_extension = "",
    .partition_model = PARTITION_ENUMERATED,
    .processing_order = INPUT_PROCESSING_ORDER_VERTICAL,
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

static const struct VerticalReader PerFileMasterReader = {
    .name = "master_lhalo_style",
    .file_extension = "",
    .partition_model = PARTITION_PER_FILE,
    .processing_order = INPUT_PROCESSING_ORDER_VERTICAL,
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
                                    const struct VerticalReader *reader) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.vertical_reader = reader;
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

/* One horizontal partition file: exactly its own Snap%03d group, with no
 * TreeHalosPerSnap dataset, which is what the snapshot writers produce. @p task
 * is the partition's task component, -1 for none. */
static int create_snapshot_partition_file(int snapnum, int task, int64_t total) {
  char path[512];
  char group_name[64];
  output_path_hdf5(path, sizeof(path), snapnum, task);

  hid_t file_id = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  TEST_ASSERT(file_id >= 0, "snapshot partition HDF5 file should be created");

  snprintf(group_name, sizeof(group_name), "Snap%03d", snapnum);
  hid_t group_id = H5Gcreate(file_id, group_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  TEST_ASSERT(group_id >= 0, "snapshot group should be created");
  TEST_ASSERT(write_galaxies_dataset(group_id, total) == TEST_PASS,
              "Galaxies dataset should be created");
  H5Gclose(group_id);

  H5Fclose(file_id);
  return TEST_PASS;
}

static int create_partition_file(int filenr, const int64_t *totals, int nout) {
  char path[512];
  char group_name[64];
  output_path_hdf5(path, sizeof(path), filenr, -1);

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

/* An external link's target must be the RIGHT file, not merely a link that
 * resolves: a master that linked every snapshot into one partition file would
 * satisfy an existence check while contradicting the partitioning contract. */
static int assert_external_link_target(hid_t file_id, const char *path, const char *expected_file,
                                       const char *expected_object) {
  /* The unversioned H5L_info_t/H5Lget_info() spellings are deliberate: they
   * follow HDF5's default API version mapping, so this compiles against 1.10
   * (which has no H5L_info2_t at all) as well as 1.12+. Both members read below
   * exist in either struct generation. Do not "modernise" these to the
   * explicitly numbered names — it would break the Linux install path the
   * Makefile recommends. */
  H5L_info_t info;
  TEST_ASSERT(H5Lget_info(file_id, path, &info, H5P_DEFAULT) >= 0,
              "master link should be queryable");
  TEST_ASSERT_EQUAL((int)info.type, (int)H5L_TYPE_EXTERNAL, "master link should be external");

  char buffer[512];
  TEST_ASSERT(info.u.val_size <= sizeof(buffer), "external link value should fit the buffer");
  TEST_ASSERT(H5Lget_val(file_id, path, buffer, sizeof(buffer), H5P_DEFAULT) >= 0,
              "external link value should be readable");

  const char *target_file = NULL;
  const char *target_object = NULL;
  unsigned flags = 0;
  TEST_ASSERT(H5Lunpack_elink_val(buffer, info.u.val_size, &flags, &target_file, &target_object) >=
                  0,
              "external link value should unpack");
  TEST_ASSERT(target_file != NULL && strcmp(target_file, expected_file) == 0,
              "external link should name the expected partition file");
  TEST_ASSERT(target_object != NULL && strcmp(target_object, expected_object) == 0,
              "external link should name the expected object in that file");
  return TEST_PASS;
}

static void cleanup_outputs(const int *filenrs, int nfiles) {
  char path[512];
  for (int i = 0; i < nfiles; i++) {
    output_path_hdf5(path, sizeof(path), filenrs[i], -1);
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

/* Minimal resolved horizontal reader for the partition-source test: only .name is
 * consulted by horizontal_output_partition_source(), which takes the format name
 * from the resolved reader (as config validation guarantees one exists for a
 * horizontal run). */
static const struct HorizontalReader HorizontalSourceReader = {
    .name = "horizontal_hdf5",
    .processing_order = INPUT_PROCESSING_ORDER_HORIZONTAL,
};

/**
 * @test    test_horizontal_output_partition_source_is_one_partition_per_output_snapshot
 * @brief   The horizontal source publishes one partition per requested output snapshot
 *
 * The requested list here is deliberately UNSORTED, which is what
 * output.snapshot_list validation admits (range and uniqueness only). Each
 * partition's output id must be its own snapshot number rather than its index,
 * so a dense-numbering regression cannot pass: under dense ids partition 0
 * would report 0 while carrying snapshot 5.
 */
static int test_horizontal_output_partition_source_is_one_partition_per_output_snapshot(void) {
  const int requested[] = {5, 1, 0};
  const int nout = (int)(sizeof(requested) / sizeof(requested[0]));

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  /* A horizontal configuration always carries a resolved horizontal reader
   * (config validation rejects it otherwise); the source reads its name. */
  MimicConfig.horizontal_reader = &HorizontalSourceReader;
  MimicConfig.NOUT = nout;
  for (int n = 0; n < nout; n++) {
    MimicConfig.ListOutputSnaps[n] = requested[n];
  }

  struct OutputPartitionSource source = get_output_partition_source();

  TEST_ASSERT(source.num_partitions != NULL, "snapshot source must supply num_partitions");
  TEST_ASSERT(source.partition_task != NULL, "snapshot source must supply partition_task");
  TEST_ASSERT(source.partition_output_id != NULL,
              "snapshot source must supply partition_output_id");
  TEST_ASSERT(source.partition_exists != NULL, "snapshot source must supply partition_exists");
  TEST_ASSERT(source.partition_snapshots != NULL,
              "snapshot source must supply partition_snapshots");
  TEST_ASSERT_EQUAL(source.num_partitions(), nout,
                    "snapshot source has one partition per requested output snapshot");

  for (int partition = 0; partition < nout; partition++) {
    TEST_ASSERT_EQUAL(source.partition_output_id(partition), requested[partition],
                      "a snapshot partition's output id is its own snapshot number");
    TEST_ASSERT(source.partition_exists(partition) != 0,
                "every snapshot partition exists; the run creates all of them");
    TEST_ASSERT_EQUAL(source.partition_task(partition), -1,
                      "a serial run's snapshot partitions carry no task component");

    struct OutputSnapshotSelection selection = source.partition_snapshots(partition);
    TEST_ASSERT_EQUAL(selection.count, 1,
                      "a snapshot partition carries exactly one requested snapshot");
    TEST_ASSERT(selection.indices != NULL, "a snapshot partition's selection must name its index");
    TEST_ASSERT_EQUAL(selection.indices[0], partition,
                      "a snapshot partition carries the requested snapshot at its own index");
  }

  TEST_ASSERT(source.format_name != NULL && strcmp(source.format_name, "horizontal_hdf5") == 0,
              "snapshot source records format name horizontal_hdf5");
  TEST_ASSERT(source.prepare_run == NULL, "snapshot source keeps no run-scoped prepare hook");
  TEST_ASSERT(source.teardown_run == NULL, "snapshot source keeps no run-scoped teardown hook");

  return TEST_PASS;
}

/**
 * @test    test_snapshot_master_links_each_snapshot_to_its_own_partition
 * @brief   The master file links every requested snapshot to the partition file named for it
 *
 * The requested list is unsorted, so index order and snapshot order disagree and
 * a master that used the partition index in either the group name or the link
 * target would produce visibly wrong pairings. The link targets are read back
 * and unpacked rather than merely checked for existence, and each snapshot group
 * is asserted to hold exactly its one File group.
 */
static int test_snapshot_master_links_each_snapshot_to_its_own_partition(void) {
  char dir_template[] = "/tmp/mimic_master_snapshot_XXXXXX";
  const int requested[] = {5, 1, 0};
  const int64_t totals[] = {7, 3, 0};
  const int nout = (int)(sizeof(requested) / sizeof(requested[0]));
  hid_t master_file_id;

  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be available");

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  MimicConfig.horizontal_reader = &HorizontalSourceReader;
  MimicConfig.NOUT = nout;
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", dir_template);
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "%s", "model");
  for (int n = 0; n < nout; n++) {
    MimicConfig.ListOutputSnaps[n] = requested[n];
    MimicConfig.ZZ[requested[n]] = (double)(nout - n);
  }

  for (int n = 0; n < nout; n++) {
    TEST_ASSERT(create_snapshot_partition_file(requested[n], -1, totals[n]) == TEST_PASS,
                "each snapshot's partition fixture should be created");
  }

  write_master_file();

  TEST_ASSERT(open_master_file(&master_file_id) == TEST_PASS, "master file should be readable");

  for (int n = 0; n < nout; n++) {
    char group_path[64], link_path[96], target_file[64], target_object[64];
    snprintf(group_path, sizeof(group_path), "Snap%03d/File%03d", requested[n], requested[n]);
    snprintf(link_path, sizeof(link_path), "%s/Galaxies", group_path);
    snprintf(target_file, sizeof(target_file), "model_%03d.hdf5", requested[n]);
    snprintf(target_object, sizeof(target_object), "Snap%03d/Galaxies", requested[n]);

    TEST_ASSERT(assert_external_link_target(master_file_id, link_path, target_file,
                                            target_object) == TEST_PASS,
                "each snapshot should link into the partition file named for it");
    TEST_ASSERT(assert_total_attr(master_file_id, group_path, totals[n]) == TEST_PASS,
                "each master link should carry its own partition's TotHalosPerSnap");

    /* A horizontal run has no trees, so the master links no per-tree
     * counts, and no other snapshot's File group appears under this snapshot. */
    snprintf(link_path, sizeof(link_path), "%s/TreeHalosPerSnap", group_path);
    TEST_ASSERT(assert_link_exists(master_file_id, link_path, 0) == TEST_PASS,
                "a horizontal master links no TreeHalosPerSnap");

    hid_t snap_group_id = H5Gopen(master_file_id, group_path, H5P_DEFAULT);
    TEST_ASSERT(snap_group_id >= 0, "master snapshot File group should open");
    H5Gclose(snap_group_id);

    for (int other = 0; other < nout; other++) {
      if (other == n) {
        continue;
      }
      snprintf(link_path, sizeof(link_path), "Snap%03d/File%03d", requested[n], requested[other]);
      TEST_ASSERT(assert_link_exists(master_file_id, link_path, 0) == TEST_PASS,
                  "a snapshot group must not link any partition but its own");
    }
  }

  H5Fclose(master_file_id);

  cleanup_outputs(requested, nout);
  return TEST_PASS;
}

/**
 * @test    test_tree_output_partition_source_wraps_configured_reader
 * @brief   The vertical output partition source wraps the configured reader's hooks
 */
static int test_tree_output_partition_source_wraps_configured_reader(void) {
  reset_master_partitions();
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_VERTICAL;
  MimicConfig.vertical_reader = &EnumeratedMasterReader;
  MimicConfig.NOUT = 2;

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
  TEST_ASSERT(source.partition_snapshots != NULL, "tree source must supply partition_snapshots");
  TEST_ASSERT(source.partition_task != NULL, "tree source must supply partition_task");
  TEST_ASSERT_EQUAL(source.partition_task(0), -1, "a tree partition carries no task component");
  struct OutputSnapshotSelection selection = source.partition_snapshots(0);
  TEST_ASSERT_EQUAL(selection.count, MimicConfig.NOUT,
                    "tree source's partition carries every requested snapshot");

  /* PARTITION_PER_FILE readers keep their prior behaviour bit for bit: the
   * seam never consults their exists hook (the output-file access() check
   * downstream is what gates them), so this must read as always-existing
   * even though the fake hook below says otherwise. */
  reset_master_partitions();
  MimicConfig.vertical_reader = &PerFileMasterReader;
  master_npartitions = 1;
  master_output_ids[0] = 9;
  master_exists[0] = 0;

  struct OutputPartitionSource per_file_source = get_output_partition_source();
  TEST_ASSERT(per_file_source.partition_exists(0) != 0,
              "tree source treats a per-file partition as existing regardless of its exists hook");

  return TEST_PASS;
}

/* Remove a flat temporary directory and every file in it. */
static void remove_temp_dir(const char *dir) {
  DIR *handle = opendir(dir);
  if (handle != NULL) {
    const struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
      unlink(path);
    }
    closedir(handle);
  }
  rmdir(dir);
}

/*
 * The NTask = 3 case's checks, run against an existing temporary directory by
 * test_multi_task_snapshot_partitions_and_master_carry_task(), which owns the
 * cleanup: an assertion returning from here early still ends in that cleanup,
 * so a failure never leaves NTask set for the cases that follow.
 */
static int check_multi_task_snapshot_partitions(const char *dir_template) {
  const int requested[] = {7, 2};
  const int nout = (int)(sizeof(requested) / sizeof(requested[0]));
  const int ntask = 3;
  const int expected_ids[] = {7, 2, 7, 2, 7, 2};
  const int expected_tasks[] = {0, 0, 1, 1, 2, 2};
  hid_t master_file_id;

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  MimicConfig.horizontal_reader = &HorizontalSourceReader;
  MimicConfig.NOUT = nout;
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", dir_template);
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "%s", "model");
  for (int n = 0; n < nout; n++) {
    MimicConfig.ListOutputSnaps[n] = requested[n];
    MimicConfig.ZZ[requested[n]] = (double)(nout - n);
  }
  ThisTask = 0;
  NTask = ntask;

  struct OutputPartitionSource source = get_output_partition_source();
  TEST_ASSERT(source.partition_task != NULL, "snapshot source must supply partition_task");
  TEST_ASSERT_EQUAL(source.num_partitions(), nout * ntask,
                    "a multi-task snapshot source has NOUT x NTask partitions");

  for (int partition = 0; partition < nout * ntask; partition++) {
    TEST_ASSERT_EQUAL(source.partition_output_id(partition), expected_ids[partition],
                      "partition p's output id is requested snapshot p % NOUT");
    TEST_ASSERT_EQUAL(source.partition_task(partition), expected_tasks[partition],
                      "partition p is written by task p / NOUT");
    TEST_ASSERT(source.partition_exists(partition) != 0,
                "every task's snapshot partition exists, empty or not");

    struct OutputSnapshotSelection selection = source.partition_snapshots(partition);
    TEST_ASSERT_EQUAL(selection.count, 1,
                      "a multi-task snapshot partition carries exactly one requested snapshot");
    TEST_ASSERT_EQUAL(selection.indices[0], partition % nout,
                      "a multi-task snapshot partition carries requested snapshot p % NOUT");

    char path[512], expected_path[512];
    output_path_hdf5(path, sizeof(path), expected_ids[partition], expected_tasks[partition]);
    snprintf(expected_path, sizeof(expected_path), "%s/model_%03d_task%03d.hdf5", dir_template,
             expected_ids[partition], expected_tasks[partition]);
    TEST_ASSERT(strcmp(path, expected_path) == 0,
                "a partition with a task component is named <base>_<snap>_task<task>.hdf5");

    TEST_ASSERT(create_snapshot_partition_file(expected_ids[partition], expected_tasks[partition],
                                               10 * partition + 1) == TEST_PASS,
                "each task's snapshot partition fixture should be created");
  }

  char serial_path[512], expected_serial_path[512];
  output_path_hdf5(serial_path, sizeof(serial_path), 7, -1);
  snprintf(expected_serial_path, sizeof(expected_serial_path), "%s/model_007.hdf5", dir_template);
  TEST_ASSERT(strcmp(serial_path, expected_serial_path) == 0,
              "a partition with no task component keeps the unsuffixed name");

  write_master_file();

  TEST_ASSERT(open_master_file(&master_file_id) == TEST_PASS, "master file should be readable");
  for (int partition = 0; partition < nout * ntask; partition++) {
    char group_path[64], link_path[96], target_file[64], target_object[64];
    snprintf(group_path, sizeof(group_path), "Snap%03d/File%03d_task%03d", expected_ids[partition],
             expected_ids[partition], expected_tasks[partition]);
    snprintf(link_path, sizeof(link_path), "%s/Galaxies", group_path);
    snprintf(target_file, sizeof(target_file), "model_%03d_task%03d.hdf5", expected_ids[partition],
             expected_tasks[partition]);
    snprintf(target_object, sizeof(target_object), "Snap%03d/Galaxies", expected_ids[partition]);

    TEST_ASSERT(assert_external_link_target(master_file_id, link_path, target_file,
                                            target_object) == TEST_PASS,
                "each task's partition is linked under its task-suffixed File group");
    TEST_ASSERT(assert_total_attr(master_file_id, group_path, 10 * partition + 1) == TEST_PASS,
                "each task-suffixed group republishes its own partition's TotHalosPerSnap");
  }
  for (int n = 0; n < nout; n++) {
    char link_path[64];
    snprintf(link_path, sizeof(link_path), "Snap%03d/File%03d", requested[n], requested[n]);
    TEST_ASSERT(assert_link_exists(master_file_id, link_path, 0) == TEST_PASS,
                "a multi-task master carries no unsuffixed File group");
  }
  H5Fclose(master_file_id);

  /* The vertical source never carries a task component, whatever the task count. */
  reset_master_partitions();
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_VERTICAL;
  MimicConfig.vertical_reader = &EnumeratedMasterReader;
  master_npartitions = 2;
  struct OutputPartitionSource vertical_source = get_output_partition_source();
  TEST_ASSERT(vertical_source.partition_task != NULL, "tree source must supply partition_task");
  TEST_ASSERT_EQUAL(vertical_source.partition_task(0), -1,
                    "a tree partition carries no task component under NTask > 1");
  TEST_ASSERT_EQUAL(vertical_source.partition_task(1), -1,
                    "a tree partition carries no task component under NTask > 1");
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  return TEST_PASS;
}

/**
 * @test    test_multi_task_snapshot_partitions_and_master_carry_task
 * @brief   Under NTask > 1 the horizontal source and master name every partition by snapshot and
 *          task
 *
 * NTask = 3 and NOUT = 2 are set by hand: NTask > 1 is accepted at configuration
 * time and the horizontal driver distributes, but this unit test drives the
 * source and the master writer directly, without a driver. The requested list is
 * unsorted so that a partition's snapshot index, snapshot number and partition
 * index all disagree: partition p must carry requested snapshot p % NOUT, be
 * written by task p / NOUT, and be named and linked with both. The vertical
 * source under the same task count must still report no task component, and the
 * unsuffixed names must not appear in the master.
 *
 * The checks live in check_multi_task_snapshot_partitions(); this wrapper runs
 * them and then always resets ThisTask/NTask and removes the temporary directory,
 * whatever the checks returned.
 */
static int test_multi_task_snapshot_partitions_and_master_carry_task(void) {
  /* SETUP */
  char dir_template[] = "/tmp/mimic_master_tasks_XXXXXX";
  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be available");

  /* EXECUTE and VALIDATE */
  const int result = check_multi_task_snapshot_partitions(dir_template);

  /* CLEANUP: the single path every outcome takes */
  ThisTask = 0;
  NTask = 0;
  remove_temp_dir(dir_template);
  return result;
}

/* Whether a file exists at @p path. */
static int file_exists(const char *path) { return access(path, F_OK) == 0; }

/* Open one partition through the real writer with @p task and assert it landed at
 * exactly output_path_hdf5(id, task), and at no other partition name. */
static int assert_writer_names_file_by_task(int output_id, int task, int other_task) {
  char expected[512], other[512];
  output_path_hdf5(expected, sizeof(expected), output_id, task);
  output_path_hdf5(other, sizeof(other), output_id, other_task);

  const int index = 0;
  const struct OutputSnapshotSelection selection = {1, &index};
  open_hdf5_output_file(output_id, task, selection);
  TEST_ASSERT(HDF5_current_file_id >= 0, "the writer should leave the partition file open");
  TEST_ASSERT(H5Fclose(HDF5_current_file_id) >= 0, "the partition file should close");
  HDF5_current_file_id = -1;

  TEST_ASSERT(file_exists(expected), "the writer creates the file named by the task it is given");
  TEST_ASSERT(!file_exists(other), "the writer creates no file under the other partition name");
  unlink(expected);
  return TEST_PASS;
}

/*
 * The writer case's checks, run against an existing temporary directory by
 * test_writer_names_file_by_given_task(), which owns the cleanup.
 */
static int check_writer_names_file_by_given_task(const char *dir_template) {
  const int snapnum = 4;

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  MimicConfig.OutputFormat = output_hdf5;
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = snapnum;
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", dir_template);
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "%s", "model");
  calc_hdf5_props();

  /* The task layout is set to disagree with the given task each time, so only the
   * argument can be what names the file. */
  ThisTask = 1;
  NTask = 3;
  TEST_ASSERT(assert_writer_names_file_by_task(snapnum, -1, 1) == TEST_PASS,
              "task -1 names an unsuffixed file whatever the run's task layout");

  ThisTask = 0;
  NTask = 0;
  TEST_ASSERT(assert_writer_names_file_by_task(snapnum, 2, -1) == TEST_PASS,
              "task 2 names a _task002 file whatever the run's task layout");
  return TEST_PASS;
}

/**
 * @test    test_writer_names_file_by_given_task
 * @brief   open_hdf5_output_file() names the partition file by the task it is given
 *
 * The horizontal driver passes the task it reads from partition_task(p) and the
 * vertical driver passes -1, so the writer must name the file by that argument
 * alone: a disagreement with the master's name would make write_master_file()
 * skip the file as missing, an INFO line and a successful exit. Checked for task
 * -1 and a positive task, each under a task layout that points the other way.
 * The names themselves are pinned through output_path_hdf5() by the master cases.
 */
static int test_writer_names_file_by_given_task(void) {
  /* SETUP */
  char dir_template[] = "/tmp/mimic_writer_task_XXXXXX";
  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be available");

  /* EXECUTE and VALIDATE */
  const int result = check_writer_names_file_by_given_task(dir_template);

  /* CLEANUP: the single path every outcome takes */
  ThisTask = 0;
  NTask = 0;
  HDF5_current_file_id = -1;
  free_hdf5_ids();
  remove_temp_dir(dir_template);
  return result;
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
  TEST_RUN(test_horizontal_output_partition_source_is_one_partition_per_output_snapshot);
  TEST_RUN(test_snapshot_master_links_each_snapshot_to_its_own_partition);
  TEST_RUN(test_tree_output_partition_source_wraps_configured_reader);
  TEST_RUN(test_multi_task_snapshot_partitions_and_master_carry_task);
  TEST_RUN(test_writer_names_file_by_given_task);

  TEST_SUMMARY();
  return TEST_RESULT();
}
