/**
 * @file    test_tree_loading.c
 * @brief   Loads the committed worked_ascii_lhalo fixture through the lhalo_binary reader
 *
 * Validates: the shin-uchuu-lhalo fixture read through open_partition/load_unit
 *
 * The fixture (_tests/data/worked_ascii_lhalo/, rebuilt by _tests/data/regenerate.sh)
 * is three synthetic forests in two files. Its file_plan.json gives file 0 two
 * forests and 11 halos and file 1 one forest and 2 halos; the forests' totals are
 * 7, 4 and 2 halos. Those values are typed in below. Every file is opened through
 * the production reader entry points, every forest is loaded, and each halo's
 * links are checked against the forest it was loaded with.
 *
 * Test cases:
 *   - test_file_tree_and_halo_counts: each file's tree count, per-tree halo counts
 *     and halo total equal the plan's
 *   - test_forest_links_and_snapshot_order: every link is in [-1, n), every
 *     FirstHaloInFOFgroup in [0, n), and SnapNum never increases along the ranks
 *     of a forest
 *   - test_loading_leaks_nothing: opening, loading, freeing and closing every file
 *     returns the tracked tree and halo memory to where it started
 *
 * Skips, naming the compiled simulation, unless SIMULATION=shin-uchuu-lhalo.
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../src/include/types.h"
#include "../../../../src/include/proto.h"
#include "../../../../src/io/vertical/interface.h"
#include "../../../../src/util/memory.h"
#include "../../../../src/util/error.h"

#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* External globals */
extern struct RawHalo *InputTreeHalos;
extern int *InputTreeNHalos;
extern int Ntrees;

#define PACKAGE "shin-uchuu-lhalo"
#define N_FILES 2
#define MAX_TREES_PER_FILE 2

/* From the fixture's file_plan.json, and the forests' totals 7, 4 and 2. */
static const int ExpectedTrees[N_FILES] = {2, 1};
static const int ExpectedHalos[N_FILES] = {11, 2};
static const int ExpectedTreeHalos[N_FILES][MAX_TREES_PER_FILE] = {{7, 4}, {2, 0}};

/* Why the fixture tests skip under another compiled simulation, or NULL when they run. */
static const char *skip_reason(void) {
  static char reason[MAX_STRING_LEN];
  const char *compiled = MIMIC_COMPILED_SIMULATION;
  if (strcmp(compiled, PACKAGE) == 0) {
    return NULL;
  }
  snprintf(reason, sizeof(reason),
           "compiled simulation is %s, not " PACKAGE "; the fixture is " PACKAGE "'s", compiled);
  return reason;
}

#define REQUIRE_PACKAGE()                                                                          \
  do {                                                                                             \
    if (skip_reason() != NULL) {                                                                   \
      return TEST_SKIP_WITH(skip_reason());                                                        \
    }                                                                                              \
  } while (0)

static const char *test_binary_param_file(void) {
  static char path[MAX_STRING_LEN];
  snprintf(path, sizeof(path), "build/generated/test_inputs/%s/%s/simulations/%s/test_binary.yaml",
           MIMIC_COMPILED_MODEL, MIMIC_COMPILED_SIMULATION, MIMIC_COMPILED_SIMULATION);
  return path;
}

/**
 * @brief   Initialise memory, logging and the configuration from the generated run file.
 *
 * The generated run file caps the input range at file 0, but open_partition()
 * takes the file number directly, so every fixture file can still be opened.
 */
static void setup_test(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  read_parameter_file(test_binary_param_file());
  init();
}

static int link_in_forest(int link, int nhalos) { return link >= -1 && link < nhalos; }

/**
 * @test    test_file_tree_and_halo_counts
 * @brief   Each file's header matches the fixture's plan
 *
 * Expected: file 0 holds trees of 7 and 4 halos, file 1 one tree of 2 halos
 * Validates: the reader's header parse of converted files
 */
int test_file_tree_and_halo_counts(void) {
  REQUIRE_PACKAGE();
  setup_test();

  for (int filenr = 0; filenr < N_FILES; filenr++) {
    open_partition(filenr);
    TEST_ASSERT_EQUAL(Ntrees, ExpectedTrees[filenr], "tree count differs from file_plan.json");
    int total = 0;
    for (int tree = 0; tree < Ntrees; tree++) {
      TEST_ASSERT_EQUAL(InputTreeNHalos[tree], ExpectedTreeHalos[filenr][tree],
                        "a tree's halo count differs from its forest's total");
      total += InputTreeNHalos[tree];
    }
    TEST_ASSERT_EQUAL(total, ExpectedHalos[filenr], "file halo total differs from file_plan.json");
    close_partition();
  }

  return TEST_PASS;
}

/**
 * @test    test_forest_links_and_snapshot_order
 * @brief   Every loaded forest is internally consistent
 *
 * Expected: links in [-1, n), FirstHaloInFOFgroup in [0, n), SnapNum non-increasing
 * Validates: forest-local links and the descending-snapshot rank order on disk
 */
int test_forest_links_and_snapshot_order(void) {
  REQUIRE_PACKAGE();
  setup_test();

  for (int filenr = 0; filenr < N_FILES; filenr++) {
    open_partition(filenr);
    for (int tree = 0; tree < Ntrees; tree++) {
      load_unit(tree);
      const int n = InputTreeNHalos[tree];
      for (int i = 0; i < n; i++) {
        const struct RawHalo *h = &InputTreeHalos[i];
        TEST_ASSERT(link_in_forest(h->Descendant, n), "Descendant outside [-1, n)");
        TEST_ASSERT(link_in_forest(h->FirstProgenitor, n), "FirstProgenitor outside [-1, n)");
        TEST_ASSERT(link_in_forest(h->NextProgenitor, n), "NextProgenitor outside [-1, n)");
        TEST_ASSERT(link_in_forest(h->NextHaloInFOFgroup, n), "NextHaloInFOFgroup outside [-1, n)");
        TEST_ASSERT(h->FirstHaloInFOFgroup >= 0 && h->FirstHaloInFOFgroup < n,
                    "FirstHaloInFOFgroup outside [0, n)");
        if (i > 0) {
          TEST_ASSERT(h->SnapNum <= InputTreeHalos[i - 1].SnapNum,
                      "SnapNum increases along the ranks of a forest");
        }
      }
      free_unit_halos(NULL);
    }
    close_partition();
  }

  return TEST_PASS;
}

/**
 * @test    test_loading_leaks_nothing
 * @brief   Reading every file returns all tracked reader memory
 *
 * Expected: MEM_TREES and MEM_HALOS bytes after closing equal those before opening
 * Validates: the open/load/free/close lifecycle on converted files
 */
int test_loading_leaks_nothing(void) {
  REQUIRE_PACKAGE();
  setup_test();

  const size_t trees_before = memory_category_bytes(MEM_TREES);
  const size_t halos_before = memory_category_bytes(MEM_HALOS);
  for (int filenr = 0; filenr < N_FILES; filenr++) {
    open_partition(filenr);
    for (int tree = 0; tree < Ntrees; tree++) {
      load_unit(tree);
      free_unit_halos(NULL);
    }
    close_partition();
  }
  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_TREES), trees_before,
                    "tree memory left allocated after closing every file");
  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_HALOS), halos_before,
                    "halo memory left allocated after closing every file");

  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: shin-uchuu-lhalo Tree Loading\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_file_tree_and_halo_counts);
  TEST_RUN(test_forest_links_and_snapshot_order);
  TEST_RUN(test_loading_leaks_nothing);

  TEST_SUMMARY();
  return TEST_RESULT();
}
