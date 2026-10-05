/**
 * @file    test_horizontal_distribution.c
 * @brief   Unit tests for the distributed horizontal driver's link rebase
 *
 * Under NTask > 1 each task loads only its rows of every snapshot slab, and the
 * reader leaves every link a global row of its target snapshot. The driver then
 * rewrites the five link fields to local indices with
 * horizontal_rebase_slab_links(): FoF links by the slab's own row_offset,
 * progenitor and descendant links by the first row of the task's range in the
 * snapshot their target-snapshot column names, aborting on any link that leaves
 * that range (the forest is cut by the partition).
 *
 * These tests drive the helper directly on hand-built slabs in the non-MPI
 * build, which never distributes, so the MPI path is not needed to pin it. Every
 * rebased link is read back through the matching mimic_tree_get_<role>()
 * getter, so a setter that wrote the wrong field would show. The aborts run in a
 * re-executed copy of this test binary (the fork-and-re-execute pattern of
 * test_parameter_parsing.c), whose exit handler reports the offending field
 * after the abort, which pins that the range check precedes the setter.
 *
 * Links are written through the generated mimic_tree_set_<role>() setters, so
 * the fixture is independent of the selected package's catalog field names.
 */

#include "../../src/core/horizontal_partition.h"
#include "../../src/include/types.h"
#include "../../src/io/horizontal/reader.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../framework/test_framework.h"
#include "../framework/child_capture.h"

#include "../../src/include/generated/tree_property_accessors.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Defined in src/core/horizontal_driver.c, exported for these tests only; the
 * driver's prototypes header is not part of the slice that introduced it. */
void horizontal_rebase_slab_links(struct SnapshotSlab *slab,
                                  const struct HorizontalForestPartition *partition, int task);

/* This binary's path, so an abort case can re-execute it in child mode. */
static const char *TestExecutablePath = NULL;

#define FIXTURE_TASKS 2
#define FIXTURE_SNAPSHOTS 3
#define FIXTURE_FORESTS 4
#define MAX_ROWS 4

/*
 * The fixture partition, two tasks over three snapshots:
 *
 *   snapshot 0: task 0 rows [0, 2), task 1 rows [2, 5)
 *   snapshot 1: task 0 rows [0, 3), task 1 rows [3, 7)
 *   snapshot 2: task 0 rows [0, 1), task 1 rows [1, 4)
 *
 * Task 1's offsets differ per snapshot, so a progenitor rebased by the current
 * slab's offset instead of its target's comes out visibly wrong.
 */
static const int64_t FIXTURE_ROW_CUTS[FIXTURE_SNAPSHOTS][FIXTURE_TASKS + 1] = {
    {0, 2, 5}, {0, 3, 7}, {0, 1, 4}};

/* One row's five links (global rows) and the snapshots its three tree links name. */
struct FixtureRow {
  int64_t first_fof;
  int64_t next_fof;
  int64_t first_prog;
  int32_t first_prog_snap;
  int64_t next_prog;
  int32_t next_prog_snap;
  int64_t descendant;
  int32_t descendant_snap;
};

/* A hand-built slab with its own storage. */
struct FixtureSlab {
  struct SnapshotSlab slab;
  struct RawHalo halos[MAX_ROWS];
  int64_t forest_index[MAX_ROWS];
  int64_t halo_rank_in_forest[MAX_ROWS];
  int32_t descendant_snapshot[MAX_ROWS];
  int32_t first_progenitor_snapshot[MAX_ROWS];
  int32_t next_progenitor_snapshot[MAX_ROWS];
  int64_t source_halo_id[MAX_ROWS];
};

static struct HorizontalForestPartition *make_fixture_partition(void) {
  struct HorizontalForestPartition *partition =
      horizontal_partition_create(FIXTURE_TASKS, FIXTURE_SNAPSHOTS, FIXTURE_FORESTS);
  partition->forest_cuts[0] = 0;
  partition->forest_cuts[1] = 2;
  partition->forest_cuts[2] = FIXTURE_FORESTS;
  for (int s = 0; s < FIXTURE_SNAPSHOTS; s++) {
    for (int r = 0; r <= FIXTURE_TASKS; r++) {
      partition->row_cuts[s * (FIXTURE_TASKS + 1) + r] = FIXTURE_ROW_CUTS[s][r];
    }
  }
  return partition;
}

/* Build the slab of snapshot `snapnum` holding rows [row_offset, row_offset + n). */
static void build_slab(struct FixtureSlab *fixture, int64_t snapnum, int64_t row_offset,
                       const struct FixtureRow *rows, int64_t n) {
  memset(fixture, 0, sizeof(*fixture));
  fixture->slab = snapshot_slab_empty();
  fixture->slab.snapnum = snapnum;
  fixture->slab.nhalos = n;
  fixture->slab.row_offset = row_offset;
  fixture->slab.halos = fixture->halos;
  fixture->slab.forest_index = fixture->forest_index;
  fixture->slab.halo_rank_in_forest = fixture->halo_rank_in_forest;
  fixture->slab.descendant_snapshot = fixture->descendant_snapshot;
  fixture->slab.first_progenitor_snapshot = fixture->first_progenitor_snapshot;
  fixture->slab.next_progenitor_snapshot = fixture->next_progenitor_snapshot;
  fixture->slab.source_halo_id = fixture->source_halo_id;

  const struct HaloInputView view = {fixture->halos, n};
  for (int64_t i = 0; i < n; i++) {
    mimic_tree_set_FirstHaloInFOFgroup(view, i, rows[i].first_fof);
    mimic_tree_set_NextHaloInFOFgroup(view, i, rows[i].next_fof);
    mimic_tree_set_FirstProgenitor(view, i, rows[i].first_prog);
    mimic_tree_set_NextProgenitor(view, i, rows[i].next_prog);
    mimic_tree_set_Descendant(view, i, rows[i].descendant);
    fixture->first_progenitor_snapshot[i] = rows[i].first_prog_snap;
    fixture->next_progenitor_snapshot[i] = rows[i].next_prog_snap;
    fixture->descendant_snapshot[i] = rows[i].descendant_snap;
  }
}

/*
 * Task 1's rows [3, 7) of snapshot 1, all links inside task 1's ranges: two FoF
 * groups (global centrals 3 and 5), progenitors in snapshot 0 and a
 * same-snapshot sibling, descendants in snapshot 2.
 */
static const struct FixtureRow TASK1_ROWS[4] = {
    /* global 3: FoF central of 3,4; FirstProgenitor 2@s0; Descendant 1@s2 */
    {3, 4, 2, 0, -1, -1, 1, 2},
    /* global 4: FoF member; FirstProgenitor 4@s0; NextProgenitor 6@s1; Descendant 3@s2 */
    {3, -1, 4, 0, 6, 1, 3, 2},
    /* global 5: FoF central of 5,6; NextProgenitor 3@s0; Descendant 2@s2 */
    {5, 6, -1, -1, 3, 0, 2, 2},
    /* global 6: FoF member; no tree links */
    {5, -1, -1, -1, -1, -1, -1, -1},
};

/* The same rows rebased onto task 1's local indices of each target snapshot. */
static const struct FixtureRow TASK1_LOCAL[4] = {
    {0, 1, 0, 0, -1, -1, 0, 2},
    {0, -1, 2, 0, 3, 1, 2, 2},
    {2, 3, -1, -1, 1, 0, 1, 2},
    {2, -1, -1, -1, -1, -1, -1, -1},
};

/**
 * @test    test_rebase_links_to_local_indices
 * @brief   FoF links move by the slab's offset, tree links by their target's offset
 */
int test_rebase_links_to_local_indices(void) {
  struct HorizontalForestPartition *partition = make_fixture_partition();
  static struct FixtureSlab fixture;
  build_slab(&fixture, 1, 3, TASK1_ROWS, 4);

  horizontal_rebase_slab_links(&fixture.slab, partition, 1);

  const struct HaloInputView view = {fixture.halos, 4};
  for (int64_t i = 0; i < 4; i++) {
    char message[128];
    snprintf(message, sizeof(message), "row %" PRId64 " FirstHaloInFOFgroup", i);
    TEST_ASSERT_EQUAL(mimic_tree_get_FirstHaloInFOFgroup(view, i), TASK1_LOCAL[i].first_fof,
                      message);
    snprintf(message, sizeof(message), "row %" PRId64 " NextHaloInFOFgroup", i);
    TEST_ASSERT_EQUAL(mimic_tree_get_NextHaloInFOFgroup(view, i), TASK1_LOCAL[i].next_fof, message);
    snprintf(message, sizeof(message), "row %" PRId64 " FirstProgenitor", i);
    TEST_ASSERT_EQUAL(mimic_tree_get_FirstProgenitor(view, i), TASK1_LOCAL[i].first_prog, message);
    snprintf(message, sizeof(message), "row %" PRId64 " NextProgenitor", i);
    TEST_ASSERT_EQUAL(mimic_tree_get_NextProgenitor(view, i), TASK1_LOCAL[i].next_prog, message);
    snprintf(message, sizeof(message), "row %" PRId64 " Descendant", i);
    TEST_ASSERT_EQUAL(mimic_tree_get_Descendant(view, i), TASK1_LOCAL[i].descendant, message);

    snprintf(message, sizeof(message), "row %" PRId64 " target-snapshot columns unchanged", i);
    TEST_ASSERT(fixture.first_progenitor_snapshot[i] == TASK1_ROWS[i].first_prog_snap &&
                    fixture.next_progenitor_snapshot[i] == TASK1_ROWS[i].next_prog_snap &&
                    fixture.descendant_snapshot[i] == TASK1_ROWS[i].descendant_snap,
                message);
  }
  TEST_ASSERT_EQUAL(fixture.slab.row_offset, 3, "the slab's row_offset is left as loaded");
  TEST_ASSERT_EQUAL(fixture.slab.nhalos, 4, "the slab's row count is left as loaded");

  /* The rebased slab is locally self-consistent: every FoF member names a
   * central whose own FirstHaloInFOFgroup is itself, as the sweep requires. */
  for (int64_t i = 0; i < 4; i++) {
    const int64_t central = mimic_tree_get_FirstHaloInFOFgroup(view, i);
    TEST_ASSERT_EQUAL(mimic_tree_get_FirstHaloInFOFgroup(view, central), central,
                      "a rebased central names itself");
  }

  horizontal_partition_destroy(partition);
  return TEST_PASS;
}

/**
 * @test    test_row_offset_zero_leaves_slab_unchanged
 * @brief   A range that starts at row 0 of every snapshot rewrites nothing
 *
 * Task 0 of the fixture partition, and the single task of a one-task partition
 * (the serial layout), both have offset 0 in every snapshot, so the rebase must
 * leave every byte of the raw halos as loaded.
 */
int test_row_offset_zero_leaves_slab_unchanged(void) {
  static const struct FixtureRow task0_rows[3] = {
      {0, 1, 1, 0, -1, -1, 0, 2},
      {0, 2, 0, 0, 1, 0, 0, 2},
      {0, -1, -1, -1, 2, 1, -1, -1},
  };
  struct HorizontalForestPartition *partition = make_fixture_partition();
  static struct FixtureSlab fixture;
  static struct RawHalo before[MAX_ROWS];

  build_slab(&fixture, 1, 0, task0_rows, 3);
  memcpy(before, fixture.halos, sizeof(before));
  horizontal_rebase_slab_links(&fixture.slab, partition, 0);
  TEST_ASSERT(memcmp(before, fixture.halos, sizeof(before)) == 0,
              "task 0 (offset 0 everywhere) leaves the raw halos unchanged");

  const struct HaloInputView view = {fixture.halos, 3};
  for (int64_t i = 0; i < 3; i++) {
    TEST_ASSERT(mimic_tree_get_FirstHaloInFOFgroup(view, i) == task0_rows[i].first_fof &&
                    mimic_tree_get_NextHaloInFOFgroup(view, i) == task0_rows[i].next_fof &&
                    mimic_tree_get_FirstProgenitor(view, i) == task0_rows[i].first_prog &&
                    mimic_tree_get_NextProgenitor(view, i) == task0_rows[i].next_prog &&
                    mimic_tree_get_Descendant(view, i) == task0_rows[i].descendant,
                "every link of a task-0 row reads back as loaded");
  }
  horizontal_partition_destroy(partition);

  /* One task owning every row: the whole-slab layout of a serial run. */
  struct HorizontalForestPartition *single =
      horizontal_partition_create(1, FIXTURE_SNAPSHOTS, FIXTURE_FORESTS);
  single->forest_cuts[1] = FIXTURE_FORESTS;
  for (int s = 0; s < FIXTURE_SNAPSHOTS; s++) {
    single->row_cuts[s * 2 + 1] = FIXTURE_ROW_CUTS[s][FIXTURE_TASKS];
  }
  /* All four rows of snapshot 2: links name rows of snapshots 0 (five rows), 1
   * (seven) and 2 (four), every one inside the single task's whole range. */
  static const struct FixtureRow whole_rows[4] = {
      {0, 1, 4, 0, -1, -1, -1, -1},
      {0, -1, 6, 1, 3, 0, -1, -1},
      {2, 3, -1, -1, 0, 0, -1, -1},
      {2, -1, 1, 0, -1, -1, -1, -1},
  };
  build_slab(&fixture, 2, 0, whole_rows, 4);
  memcpy(before, fixture.halos, sizeof(before));
  horizontal_rebase_slab_links(&fixture.slab, single, 0);
  TEST_ASSERT(memcmp(before, fixture.halos, sizeof(before)) == 0,
              "a one-task partition leaves the raw halos unchanged");
  horizontal_partition_destroy(single);

  return TEST_PASS;
}

/* ------------------------------------------------------------------------- */
/* Aborts, in a re-executed child                                            */
/* ------------------------------------------------------------------------- */

/* The child's slab, kept static so its exit handler can read it after the abort. */
static struct FixtureSlab child_fixture;
static int64_t child_reported_row = -1;
static const char *child_reported_role = NULL;

/* Exit handler of an abort case: report the offending field as the abort left
 * it, which shows whether anything was written before the range check. */
static void report_offending_field(void) {
  if (child_reported_role == NULL) {
    return;
  }
  const struct HaloInputView view = {child_fixture.halos, child_fixture.slab.nhalos};
  const int64_t i = child_reported_row;
  int64_t value = 0;
  if (strcmp(child_reported_role, "FirstProgenitor") == 0) {
    value = mimic_tree_get_FirstProgenitor(view, i);
  } else if (strcmp(child_reported_role, "NextHaloInFOFgroup") == 0) {
    value = mimic_tree_get_NextHaloInFOFgroup(view, i);
  } else if (strcmp(child_reported_role, "Descendant") == 0) {
    value = mimic_tree_get_Descendant(view, i);
  }
  fprintf(stderr, "after abort: %s = %" PRId64 "\n", child_reported_role, value);
}

/* Run one abort case in this (re-executed) process; returns only if it did not abort. */
static void run_abort_case(const char *name) {
  struct FixtureRow rows[4];
  memcpy(rows, TASK1_ROWS, sizeof(rows));
  int64_t snapnum = 1;
  int64_t row_offset = 3;

  if (strcmp(name, "first_progenitor_below") == 0) {
    /* Global row 1 of snapshot 0 belongs to task 0 ([0, 2)). */
    rows[0].first_prog = 1;
    child_reported_row = 0;
    child_reported_role = "FirstProgenitor";
  } else if (strcmp(name, "next_fof_above") == 0) {
    /* Global row 7 of snapshot 1 is beyond task 1's [3, 7). */
    rows[3].next_fof = 7;
    child_reported_row = 3;
    child_reported_role = "NextHaloInFOFgroup";
  } else if (strcmp(name, "descendant_above") == 0) {
    /* Global row 4 of snapshot 2 is beyond task 1's [1, 4). */
    rows[1].descendant = 4;
    child_reported_row = 1;
    child_reported_role = "Descendant";
  } else if (strcmp(name, "range_mismatch") == 0) {
    /* A slab loaded from row 2 is not task 1's range of snapshot 1. */
    row_offset = 2;
  } else {
    fprintf(stderr, "unknown abort case '%s'\n", name);
    return;
  }

  build_slab(&child_fixture, snapnum, row_offset, rows, 4);
  if (atexit(report_offending_field) != 0) {
    return;
  }
  horizontal_rebase_slab_links(&child_fixture.slab, make_fixture_partition(), 1);
}

/* Body run in the forked child: replace it with a fresh image of this binary in
 * abort-case mode. Returning (an exec failure) exits 0, which the parent reads
 * as "did not abort". */
static void reexecute_abort_case(const char *name) {
  execl(TestExecutablePath, TestExecutablePath, "--rebase-abort-case", name, (char *)NULL);
}

/**
 * @test    test_link_outside_range_aborts
 * @brief   A link leaving the task's range of its target aborts before it is written
 */
int test_link_outside_range_aborts(void) {
  TEST_ASSERT(expect_fatal("first_progenitor_below", reexecute_abort_case,
                           "FirstProgenitor link of snapshot 1 global row 3 names row 1 of target "
                           "snapshot 0, outside task 1's rows [2, 5) there: the forest is cut by "
                           "the partition",
                           "after abort: FirstProgenitor = 1") == 1,
              "a progenitor below the task's range of its target aborts, unwritten");
  TEST_ASSERT(expect_fatal("next_fof_above", reexecute_abort_case,
                           "NextHaloInFOFgroup link of snapshot 1 global row 6 names row 7 of "
                           "target snapshot 1, outside task 1's rows [3, 7) there: the forest is "
                           "cut by the partition",
                           "after abort: NextHaloInFOFgroup = 7") == 1,
              "a FoF link beyond the slab's own range aborts, unwritten");
  TEST_ASSERT(expect_fatal("descendant_above", reexecute_abort_case,
                           "Descendant link of snapshot 1 global row 4 names row 4 of target "
                           "snapshot 2, outside task 1's rows [1, 4) there: the forest is cut by "
                           "the partition",
                           "after abort: Descendant = 4") == 1,
              "a descendant beyond the task's range of its target aborts, unwritten");
  TEST_ASSERT(expect_fatal("range_mismatch", reexecute_abort_case,
                           "slab holds rows [2, 6), but task 1's partition range there is [3, 7)",
                           NULL) == 1,
              "a slab that is not the task's range aborts before any link is touched");
  return TEST_PASS;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--rebase-abort-case") == 0) {
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);
    run_abort_case(argv[2]);
    return 0;
  }

  TestExecutablePath = argv[0];

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Horizontal Distribution (link rebase)\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_rebase_links_to_local_indices);
  TEST_RUN(test_row_offset_zero_leaves_slab_unchanged);
  TEST_RUN(test_link_outside_range_aborts);

  TEST_SUMMARY();
  return TEST_RESULT();
}
