/**
 * @file    test_unit_snapshot_driver_gather.c
 * @brief   Unit tests for the snapshot driver's cross-generation progenitor lookup.
 *
 * The snapshot driver is the first place in Mimic where **two** input
 * generations are live at once: while snapshot N is processed, snapshot N-1's
 * raw slab, its per-halo output ranges and its output buffer are all still in
 * memory. Progenitor lookup is the one step that has to reach across that
 * boundary, and it has to reach across it in exactly one direction:
 * `FirstProgenitor` is read from slab N, and everything the chain touches after
 * that -- `NextProgenitor`, `Len`, occupancy, and the galaxies themselves --
 * belongs to slab N-1.
 *
 * Nothing else in the suite can catch a transposition there. The compiler
 * cannot: both generations have the same types. The cross-format bitwise
 * comparison cannot either, because it only sees the end result, and at the
 * indices where two slabs overlap a transposed read produces plausible
 * galaxies rather than a crash. So these tests build two slabs with
 * deliberately DIFFERENT values at the SAME indices, chosen so that reading the
 * wrong generation gives a different, checkable answer -- and every assertion
 * below is paired with the wrong-generation call that shows it can fail.
 *
 * The names used here (Len, SnapNum, FirstProgenitor, NextProgenitor) are
 * catalog field names bound to the core roles the accessors read, the same
 * convention tests/unit/test_input_view.c uses.
 */

#include "../../../../tests/framework/test_framework.h"

#include "../../../../src/core/inheritance.h"
#include "../../../../src/include/proto.h"
#include "../../../../src/include/types.h"
#include "../../../../src/util/error.h"
#include "../../../../src/util/memory.h"

#include "../../../../src/include/generated/tree_property_accessors.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

extern double *Age;

#define NHALOS 6
#define NPROCESSED 12

/* Lookback times of the three snapshots the fixtures reference. Distinct and
 * far apart so a source_time taken from the wrong snapshot is unmistakable. */
static double fixture_age[3] = {13.0, 11.0, 9.0};

static struct RawHalo previous_halos[NHALOS];
static struct RawHalo current_halos[NHALOS];
static struct SnapshotHaloAux previous_aux[NHALOS];
static struct SnapshotHaloAux current_aux[NHALOS];
static struct Halo previous_processed[NPROCESSED];
static struct Halo current_processed[NPROCESSED];

/*
 * Slab N-1 (snapshot 1).
 *
 *   chain from current halo 0:  p0 -> p1 -> p2
 *   chain from current halo 1:  p4 -> p5
 *
 * p0 is unoccupied, so the "pin an occupied FirstProgenitor" rule does not
 * apply to the first chain and the scan runs; p4 IS occupied, so the second
 * chain must stop at p4 even though p5 is far more massive. p3 is referenced by
 * nothing and carries the largest Len and range of all, so a lookup that walked
 * the aux array instead of the chain would be caught.
 */
static void seed_previous_slab(void) {
  memset(previous_halos, 0, sizeof(previous_halos));
  memset(previous_aux, 0, sizeof(previous_aux));

  const int len[NHALOS] = {10, 50, 30, 999, 20, 800};
  const int next_progenitor[NHALOS] = {1, 2, -1, -1, 5, -1};
  const int nhalos[NHALOS] = {0, 2, 1, 5, 1, 3};
  const int first_halo[NHALOS] = {-1, 0, 2, 3, 8, 9};

  for (int i = 0; i < NHALOS; i++) {
    previous_halos[i].Len = len[i];
    previous_halos[i].NextProgenitor = next_progenitor[i];
    previous_halos[i].FirstProgenitor = -1;
    previous_halos[i].FirstHaloInFOFgroup = i;
    previous_halos[i].NextHaloInFOFgroup = -1;
    previous_halos[i].SnapNum = 1;

    previous_aux[i].NHalos = nhalos[i];
    previous_aux[i].FirstHalo = first_halo[i];
  }
}

/*
 * Slab N (snapshot 2). Every value that the lookup must NOT read is different
 * from its slab N-1 counterpart at the same index:
 *
 *   Len          - ordered so that scanning the current slab would pick halo 2
 *                  where scanning the previous slab picks halo 1
 *   NextProgenitor - all -1, so a chain walked in the current slab stops dead
 *                  at its first entry
 */
static void seed_current_slab(void) {
  memset(current_halos, 0, sizeof(current_halos));
  memset(current_aux, 0, sizeof(current_aux));

  const int len[NHALOS] = {7, 5, 900, 4, 1, 1};
  const int first_progenitor[NHALOS] = {0, 4, -1, -1, -1, -1};

  for (int i = 0; i < NHALOS; i++) {
    current_halos[i].Len = len[i];
    current_halos[i].FirstProgenitor = first_progenitor[i];
    current_halos[i].NextProgenitor = -1;
    current_halos[i].FirstHaloInFOFgroup = i;
    current_halos[i].NextHaloInFOFgroup = -1;
    current_halos[i].SnapNum = 2;

    /* The current generation's own ranges, all different from the previous
     * generation's at the same index. Handing these to a gather is the
     * wrong-generation control below. */
    current_aux[i].NHalos = 7;
    current_aux[i].FirstHalo = 0;
  }
}

/* Output buffers of the two generations. Only SnapNum matters here: it is what
 * the gather turns into source_time. The previous generation's entry 1 carries
 * an OLDER snapshot than the slab it sits in -- an orphan that skipped a
 * snapshot -- which is exactly the case that distinguishes "the source galaxy's
 * stored SnapNum" from "the previous slab's snapshot number". */
static void seed_processed_buffers(void) {
  memset(previous_processed, 0, sizeof(previous_processed));
  memset(current_processed, 0, sizeof(current_processed));

  for (int i = 0; i < NPROCESSED; i++) {
    previous_processed[i].SnapNum = 1;
    current_processed[i].SnapNum = 2;
  }
  previous_processed[1].SnapNum = 0;
}

static struct HaloInputView current_view(void) {
  struct HaloInputView view = {current_halos, NHALOS};
  return view;
}

/* The correct previous generation: slab N-1's halos, ranges and galaxies. */
static struct SnapshotGatherContext previous_generation(void) {
  struct SnapshotGatherContext context;
  context.view.halos = previous_halos;
  context.view.count = NHALOS;
  context.aux = previous_aux;
  context.processed = previous_processed;
  return context;
}

/* The transposition under test: the CURRENT generation offered where the
 * previous one belongs. Every assertion below is paired with a call through
 * this context, so a test that could not fail is visible immediately. */
static struct SnapshotGatherContext transposed_generation(void) {
  struct SnapshotGatherContext context;
  context.view.halos = current_halos;
  context.view.count = NHALOS;
  context.aux = current_aux;
  context.processed = current_processed;
  return context;
}

static void seed_fixtures(void) {
  seed_previous_slab();
  seed_current_slab();
  seed_processed_buffers();
  Age = fixture_age;
}

/**
 * @brief   Most-massive-progenitor selection reads Len and the chain from slab N-1.
 */
static int test_most_massive_progenitor_reads_the_previous_slab(void) {
  seed_fixtures();
  const struct HaloInputView view = current_view();
  const struct SnapshotGatherContext prev = previous_generation();
  const struct SnapshotGatherContext transposed = transposed_generation();

  const int chosen = snapshot_find_most_massive_progenitor(view, &prev, 0);
  TEST_ASSERT_EQUAL(chosen, 1,
                    "the chain p0->p1->p2 should select p1: the most massive OCCUPIED "
                    "progenitor by slab N-1's Len");

  /* Same call, previous generation replaced by the current one. Slab N's Len
   * ordering would select halo 2 and its NextProgenitor chain terminates
   * immediately, so a transposed read cannot return 1 here. */
  const int transposed_choice = snapshot_find_most_massive_progenitor(view, &transposed, 0);
  TEST_ASSERT(transposed_choice != chosen,
              "reading the wrong generation must change the answer, or this test proves nothing");

  return TEST_PASS;
}

/**
 * @brief   An occupied FirstProgenitor pins the selection (lenoccmax = -1).
 */
static int test_occupied_first_progenitor_is_pinned(void) {
  seed_fixtures();
  const struct HaloInputView view = current_view();
  const struct SnapshotGatherContext prev = previous_generation();

  /* Current halo 1's chain is p4 -> p5. p4 is occupied and its Len is 20; p5 is
   * occupied and its Len is 800. The tree-side rule (build_model.c) pins p4
   * anyway, and this is the assertion that would catch a rewrite that "fixed"
   * the pin into a plain maximum. */
  TEST_ASSERT_EQUAL(snapshot_find_most_massive_progenitor(view, &prev, 1), 4,
                    "an occupied FirstProgenitor must win over a more massive later "
                    "chain entry");
  TEST_ASSERT(previous_halos[5].Len > previous_halos[4].Len,
              "the fixture must actually offer a more massive later entry");
  TEST_ASSERT(previous_aux[5].NHalos > 0, "the more massive later entry must be occupied");

  return TEST_PASS;
}

/**
 * @brief   A halo with no progenitor reads nothing, even with no previous generation.
 */
static int test_no_progenitor_needs_no_previous_generation(void) {
  seed_fixtures();
  const struct HaloInputView view = current_view();
  struct SnapshotGatherContext empty;

  /* Snapshot 0's context: no slab, no ranges, no galaxies. Every halo there has
   * FirstProgenitor -1 (the format's adjacency invariant), so the lookup must
   * return without touching any member of the context. */
  empty.view.halos = NULL;
  empty.view.count = 0;
  empty.aux = NULL;
  empty.processed = NULL;

  TEST_ASSERT_EQUAL(snapshot_find_most_massive_progenitor(view, &empty, 2), -1,
                    "a halo with no FirstProgenitor selects nothing");
  TEST_ASSERT_EQUAL((int)snapshot_count_progenitor_galaxies(view, &empty, 2), 0,
                    "a halo with no FirstProgenitor gathers no galaxies");

  return TEST_PASS;
}

/**
 * @brief   The progenitor-galaxy count sums slab N-1's ranges along slab N-1's chain.
 */
static int test_progenitor_count_uses_the_previous_generation(void) {
  seed_fixtures();
  const struct HaloInputView view = current_view();
  const struct SnapshotGatherContext prev = previous_generation();
  const struct SnapshotGatherContext transposed = transposed_generation();

  /* p0 contributes 0, p1 contributes 2, p2 contributes 1. */
  TEST_ASSERT_EQUAL((int)snapshot_count_progenitor_galaxies(view, &prev, 0), 3,
                    "the count should sum slab N-1's output ranges along its own chain");
  /* p4 contributes 1, p5 contributes 3. */
  TEST_ASSERT_EQUAL((int)snapshot_count_progenitor_galaxies(view, &prev, 1), 4,
                    "the second chain's ranges should sum to 4");

  TEST_ASSERT(snapshot_count_progenitor_galaxies(view, &transposed, 0) !=
                  snapshot_count_progenitor_galaxies(view, &prev, 0),
              "reading the wrong generation must change the count");

  return TEST_PASS;
}

/**
 * @brief   The gather visits chain order then range order, from slab N-1's buffer.
 */
static int test_gather_order_sources_and_times(void) {
  seed_fixtures();
  const struct HaloInputView view = current_view();
  const struct SnapshotGatherContext prev = previous_generation();
  struct InheritanceProgenitorGalaxy gathered[4];

  const int first_occupied = snapshot_find_most_massive_progenitor(view, &prev, 0);
  const int64_t count = snapshot_count_progenitor_galaxies(view, &prev, 0);
  TEST_ASSERT_EQUAL((int)count, 3, "the fixture chain should gather three galaxies");

  memset(gathered, 0, sizeof(gathered));
  snapshot_gather_progenitor_galaxies(view, &prev, 0, first_occupied, gathered);

  /* Chain order p0 (0 galaxies), p1 (range [0,2)), p2 (range [2,3)). */
  TEST_ASSERT(gathered[0].source == &previous_processed[0],
              "the first gathered galaxy is p1's range start in slab N-1's buffer");
  TEST_ASSERT(gathered[1].source == &previous_processed[1], "ranges are visited in index order");
  TEST_ASSERT(gathered[2].source == &previous_processed[2],
              "the next chain entry's range follows, at its own FirstHalo offset");

  /* p1 is the most massive occupied progenitor, so only its galaxies are on the
   * main branch. */
  TEST_ASSERT_EQUAL(gathered[0].is_main_branch, 1, "p1's galaxies are on the main branch");
  TEST_ASSERT_EQUAL(gathered[1].is_main_branch, 1, "p1's galaxies are on the main branch");
  TEST_ASSERT_EQUAL(gathered[2].is_main_branch, 0, "p2's galaxy is not on the main branch");

  /* source_time comes from each source galaxy's own stored SnapNum, not from
   * the slab it was found in: entry 1 carries snapshot 0 while its slab is
   * snapshot 1, so these three times cannot all be equal. */
  TEST_ASSERT_DOUBLE_EQUAL(gathered[0].source_time, fixture_age[1], 0.0,
                           "source_time is Age[the source galaxy's SnapNum]");
  TEST_ASSERT_DOUBLE_EQUAL(gathered[1].source_time, fixture_age[0], 0.0,
                           "a galaxy that skipped a snapshot keeps its own older age");
  TEST_ASSERT_DOUBLE_EQUAL(gathered[2].source_time, fixture_age[1], 0.0,
                           "source_time is Age[the source galaxy's SnapNum]");

  /* Nothing from the unreferenced p3 range (which is both the largest and the
   * one an aux-array sweep would reach) may appear. */
  for (int i = 0; i < (int)count; i++) {
    TEST_ASSERT(gathered[i].source < &previous_processed[3],
                "only the chain's ranges are gathered, never the whole aux array");
  }

  return TEST_PASS;
}

/**
 * @brief   The gather reads galaxies from slab N-1's buffer, not slab N's.
 */
static int test_gather_reads_the_previous_output_buffer(void) {
  seed_fixtures();
  const struct HaloInputView view = current_view();
  const struct SnapshotGatherContext prev = previous_generation();
  const struct SnapshotGatherContext transposed = transposed_generation();
  struct InheritanceProgenitorGalaxy gathered[4];
  struct InheritanceProgenitorGalaxy wrong[8];

  memset(gathered, 0, sizeof(gathered));
  snapshot_gather_progenitor_galaxies(
      view, &prev, 0, snapshot_find_most_massive_progenitor(view, &prev, 0), gathered);

  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(gathered[i].source >= &previous_processed[0] &&
                    gathered[i].source < &previous_processed[NPROCESSED],
                "every gathered galaxy comes from the previous generation's buffer");
  }

  /* The same gather through the transposed context lands in the other buffer
   * entirely, which is what makes the assertion above meaningful. */
  memset(wrong, 0, sizeof(wrong));
  snapshot_gather_progenitor_galaxies(view, &transposed, 0, 0, wrong);
  TEST_ASSERT(wrong[0].source >= &current_processed[0] &&
                  wrong[0].source < &current_processed[NPROCESSED],
              "the transposed control must read the other buffer, or the check above "
              "could not fail");

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Snapshot Driver Cross-Generation Gather\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_most_massive_progenitor_reads_the_previous_slab);
  TEST_RUN(test_occupied_first_progenitor_is_pinned);
  TEST_RUN(test_no_progenitor_needs_no_previous_generation);
  TEST_RUN(test_progenitor_count_uses_the_previous_generation);
  TEST_RUN(test_gather_order_sources_and_times);
  TEST_RUN(test_gather_reads_the_previous_output_buffer);

  TEST_SUMMARY();
  return TEST_RESULT();
}
