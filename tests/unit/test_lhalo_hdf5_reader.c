/**
 * @file    test_lhalo_hdf5_reader.c
 * @brief   Unit tests for the lhalo_hdf5 vertical reader (src/io/vertical/hdf5.c).
 *
 * Runs the reader's hooks against the committed two-partition fixture in
 * tests/data/lhalo_hdf5 (written by generate_fixture.py there, which documents the
 * layout): the count hook equals each file's Ntrees and the largest-tree hook its
 * largest InputTreeNHalos entry, both read independently here with the HDF5 API; one
 * partition opens and loads a unit; and a file whose InputTreeNHalos element count
 * disagrees with Ntrees is refused by the extent-checked header reader at open and by
 * the largest-tree hook. FATAL_ERROR ends the process, so the refusals run in a
 * forked child (tests/framework/child_capture.h).
 *
 * The header hooks are catalog-independent. Loading a unit reads one dataset per field
 * of the compiled package's catalog, and the fixture carries mini-Millennium's L-Halo
 * record, so the load test runs only when every compiled catalog dataset is present in
 * the fixture and skips otherwise.
 */

#include "../framework/test_framework.h"
#include "../framework/child_capture.h"

#include "globals.h"
#include "memory.h"
#include "vertical/reader.h"
#include "generated/tree_property_accessors.h"

#include <hdf5.h>

#ifndef HDF5
#error "test_lhalo_hdf5_reader needs -DHDF5: list it in the HDF5 tests of tests/unit/run_tests.sh"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct VerticalReader LHaloHDF5Reader;

static int passed = 0, failed = 0;

/* ---------------------------------------------------------------------------
 * Fixture facts (tests/data/lhalo_hdf5/generate_fixture.py)
 * ------------------------------------------------------------------------- */

#define FIXTURE_DIR "tests/data/lhalo_hdf5"
#define FIXTURE_TREE_NAME "trees_fixture.%d.hdf5"
#define FIXTURE_MISMATCH_TREE_NAME "trees_mismatch.%d.hdf5"
#define FIXTURE_PARTITIONS 2
#define FIXTURE_MAX_TREES 8
#define FIXTURE_LAST_SNAPSHOT 63

static const int FIXTURE_NTREES[FIXTURE_PARTITIONS] = {3, 2};
static const int FIXTURE_LARGEST_TREE[FIXTURE_PARTITIONS] = {5, 6};

/* The compiled catalog's on-disk dataset names, one per field load_unit_hdf5() reads. */
#define CATALOG_FIELD(member, dataset, ...) dataset,
static const char *const COMPILED_CATALOG_DATASETS[] = {
#include "generated/catalog_field_metadata.inc"
};
#undef CATALOG_FIELD

static void configure_fixture(const char *tree_name) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", FIXTURE_DIR);
  snprintf(MimicConfig.TreeName, sizeof(MimicConfig.TreeName), "%s", tree_name);
  MimicConfig.FirstFile = 0;
  MimicConfig.LastFile = FIXTURE_PARTITIONS - 1;
  MimicConfig.vertical_reader = &LHaloHDF5Reader;

  Ntrees = -777;
  InputTreeNHalos = NULL;
  InputTreeFirstHalo = NULL;
  InputTreeHalos = NULL;
}

static void fixture_path(char *buf, size_t size, int file_nr) {
  snprintf(buf, size, "%s/trees_fixture.%d.hdf5", FIXTURE_DIR, file_nr);
}

/* Reads one file's /Header InputTreeNHalos with the HDF5 API, independently of the
   reader; returns its element count, or -1 on failure or more than max_trees entries. */
static int read_fixture_tree_nhalos(const char *path, int *tree_nhalos, int max_trees) {
  int ntrees = -1;
  const hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const hid_t attr = H5Aopen_by_name(file, "/Header", "InputTreeNHalos", H5P_DEFAULT, H5P_DEFAULT);
  const hid_t space = attr >= 0 ? H5Aget_space(attr) : -1;
  const hssize_t npoints = space >= 0 ? H5Sget_simple_extent_npoints(space) : -1;
  if (npoints >= 0 && npoints <= max_trees && H5Aread(attr, H5T_NATIVE_INT, tree_nhalos) >= 0) {
    ntrees = (int)npoints;
  }
  if (space >= 0) {
    H5Sclose(space);
  }
  if (attr >= 0) {
    H5Aclose(attr);
  }
  H5Fclose(file);
  return ntrees;
}

/* True when every dataset the compiled catalog names exists in the file's tree_000. */
static int fixture_carries_compiled_catalog(const char *path) {
  int carries = 1;
  const hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return 0;
  }
  const size_t nfields = sizeof(COMPILED_CATALOG_DATASETS) / sizeof(COMPILED_CATALOG_DATASETS[0]);
  for (size_t i = 0; carries && i < nfields; i++) {
    char link[256];
    snprintf(link, sizeof(link), "tree_000/%s", COMPILED_CATALOG_DATASETS[i]);
    carries = H5Lexists(file, link, H5P_DEFAULT) > 0;
  }
  H5Fclose(file);
  return carries;
}

/**
 * @test    test_header_hooks_match_each_partition_table
 * @brief   count_partition_units equals Ntrees and max_partition_unit_halos equals the
 *          largest InputTreeNHalos entry for each partition, staging no globals and
 *          leaving no tree memory allocated.
 */
int test_header_hooks_match_each_partition_table(void) {
  init_memory_system(0);
  configure_fixture(FIXTURE_TREE_NAME);
  const size_t trees_bytes_before = memory_category_bytes(MEM_TREES);

  TEST_ASSERT(LHaloHDF5Reader.num_partitions() == FIXTURE_PARTITIONS,
              "the configured file range should give two partitions");
  for (int partition = 0; partition < FIXTURE_PARTITIONS; partition++) {
    char path[512];
    int tree_nhalos[FIXTURE_MAX_TREES] = {0};
    fixture_path(path, sizeof(path), partition);
    const int ntrees = read_fixture_tree_nhalos(path, tree_nhalos, FIXTURE_MAX_TREES);
    TEST_ASSERT(ntrees == FIXTURE_NTREES[partition],
                "the fixture's InputTreeNHalos should hold one entry per tree");
    int largest = 0;
    for (int i = 0; i < ntrees; i++) {
      largest = tree_nhalos[i] > largest ? tree_nhalos[i] : largest;
    }
    TEST_ASSERT(largest == FIXTURE_LARGEST_TREE[partition],
                "the fixture's largest tree should be the generator's");

    TEST_ASSERT(LHaloHDF5Reader.partition_exists(partition), "each fixture file should exist");
    TEST_ASSERT_EQUAL(LHaloHDF5Reader.count_partition_units(partition), ntrees,
                      "count_partition_units should equal the file's Ntrees");
    TEST_ASSERT_EQUAL(LHaloHDF5Reader.max_partition_unit_halos(partition), largest,
                      "max_partition_unit_halos should equal the largest InputTreeNHalos entry");
  }

  TEST_ASSERT_EQUAL(Ntrees, -777, "the header hooks should not stage Ntrees");
  TEST_ASSERT(InputTreeNHalos == NULL && InputTreeFirstHalo == NULL,
              "the header hooks should not stage the tree tables");
  TEST_ASSERT(memory_category_bytes(MEM_TREES) == trees_bytes_before,
              "the largest-tree hook should release its transient table");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_open_partition_stages_tables_and_loads_a_unit
 * @brief   open_partition_hdf5() stages Ntrees, InputTreeNHalos and InputTreeFirstHalo
 *          from the header, and load_unit_hdf5() reads one tree's rows.
 */
int test_open_partition_stages_tables_and_loads_a_unit(void) {
  char path[512];
  fixture_path(path, sizeof(path), 1);
  if (!fixture_carries_compiled_catalog(path)) {
    return TEST_SKIP_WITH("the fixture carries mini-Millennium's L-Halo catalog, which the "
                          "compiled simulation package does not declare");
  }

  init_memory_system(0);
  configure_fixture(FIXTURE_TREE_NAME);
  const size_t trees_bytes_before = memory_category_bytes(MEM_TREES);

  /* EXECUTE: open file 1 (trees of 3 and 6 halos) and load its larger tree. */
  const int file_nr = 1, unit = 1;
  LHaloHDF5Reader.open_partition(LHaloHDF5Reader.partition_output_id(file_nr));
  TEST_ASSERT_EQUAL(Ntrees, 2, "open should stage the file's Ntrees");
  TEST_ASSERT(InputTreeNHalos != NULL && InputTreeNHalos[0] == 3 && InputTreeNHalos[1] == 6,
              "open should stage InputTreeNHalos from the header");
  TEST_ASSERT(InputTreeFirstHalo != NULL && InputTreeFirstHalo[0] == 0 &&
                  InputTreeFirstHalo[1] == 3,
              "open should build InputTreeFirstHalo as the running sum");

  LHaloHDF5Reader.load_unit(unit);
  TEST_ASSERT(InputTreeHalos != NULL, "load_unit should stage the tree's halos");

  /* VALIDATE: Len encodes 1000 * file + 100 * tree + halo + 20; each tree is one chain. */
  const struct HaloInputView view = {InputTreeHalos, (int64_t)InputTreeNHalos[unit]};
  for (int64_t halo = 0; halo < view.count; halo++) {
    TEST_ASSERT_EQUAL(mimic_tree_get_Len(view, halo), 1000 * file_nr + 100 * unit + halo + 20,
                      "each loaded row should carry its own Len");
    TEST_ASSERT_EQUAL(mimic_tree_get_SnapNum(view, halo), FIXTURE_LAST_SNAPSHOT - halo,
                      "each loaded row should carry its own SnapNum");
    TEST_ASSERT_EQUAL(mimic_tree_get_Descendant(view, halo), halo - 1,
                      "each loaded row should link to its descendant in the chain");
  }

  /* CLEANUP: release in reverse allocation order, as close_partition() does. */
  myfree(InputTreeHalos);
  InputTreeHalos = NULL;
  myfree(InputTreeFirstHalo);
  InputTreeFirstHalo = NULL;
  myfree(InputTreeNHalos);
  InputTreeNHalos = NULL;
  LHaloHDF5Reader.close_partition();
  TEST_ASSERT(memory_category_bytes(MEM_TREES) == trees_bytes_before,
              "the partition's tree memory should all be released");
  check_memory_leaks();
  return TEST_PASS;
}

static void open_mismatched_partition(const char *tree_name) {
  configure_fixture(tree_name);
  LHaloHDF5Reader.open_partition(0);
}

static void scan_mismatched_partition(const char *tree_name) {
  configure_fixture(tree_name);
  (void)LHaloHDF5Reader.max_partition_unit_halos(0);
}

/**
 * @test    test_tree_table_size_must_match_ntrees
 * @brief   A file whose InputTreeNHalos holds 2 entries for Ntrees = 3 is refused, naming
 *          the attribute and the expected count, at open and by the largest-tree hook.
 */
int test_tree_table_size_must_match_ntrees(void) {
  init_memory_system(0);
  configure_fixture(FIXTURE_MISMATCH_TREE_NAME);
  TEST_ASSERT_EQUAL(LHaloHDF5Reader.count_partition_units(0), 3,
                    "the count hook reads Ntrees alone and should still answer");

  const int open_refused =
      expect_fatal_capture(FIXTURE_MISMATCH_TREE_NAME, open_mismatched_partition, "InputTreeNHalos",
                           "expected 3", NULL, 0);
  TEST_ASSERT(open_refused == 1, "open should refuse an InputTreeNHalos that disagrees with "
                                 "Ntrees");

  const int scan_refused =
      expect_fatal_capture(FIXTURE_MISMATCH_TREE_NAME, scan_mismatched_partition, "InputTreeNHalos",
                           "expected 3", NULL, 0);
  TEST_ASSERT(scan_refused == 1, "the largest-tree hook should refuse the same file");

  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: L-Halo HDF5 Reader\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_header_hooks_match_each_partition_table);
  TEST_RUN(test_open_partition_stages_tables_and_loads_a_unit);
  TEST_RUN(test_tree_table_size_must_match_ntrees);

  TEST_SUMMARY();
  return TEST_RESULT();
}
