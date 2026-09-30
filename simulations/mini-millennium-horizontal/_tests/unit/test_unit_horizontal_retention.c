/**
 * @file    test_unit_horizontal_retention.c
 * @brief   Unit tests for snapshot-qualified progenitor lookup over retained generations.
 *
 * A version 3 dataset may carry gaps: a halo's descendant can sit several
 * snapshots later, so one progenitor chain can span several earlier snapshots
 * and every link names the snapshot it points into (FirstProgenitorSnapshot,
 * NextProgenitorSnapshot). The horizontal driver therefore retains a pool of
 * generations keyed by snapshot number and resolves every link through its
 * target-snapshot column. These tests drive the lookup directly with synthetic
 * generations built from the worked five-halo mixed-gap graph defined in
 * _tests/data/source/generate_sources.py, a chain spanning three snapshots,
 * and a same-snapshot NextProgenitor, and check the retention horizon each
 * generation is released by.
 *
 * Each positive assertion is paired with the reading it rules out: a lookup that
 * assumed N-1, named a progenitor by row alone, or bounded its chain by one
 * slab's count gives a different, checkable answer on these fixtures. The two
 * aborts (a released target, a cycle) run in a child process.
 *
 * The end-to-end counterpart, which runs the committed converter-produced
 * fixtures of the same graphs through the real reader and driver, is
 * ../integration/test_gap_retention.py.
 *
 * Field names (Len, SnapNum, FirstProgenitor, ...) are this package's catalog
 * field names, bound to the core roles the accessors read.
 */

#include "../../../../tests/framework/test_framework.h"

#include "../../../../src/core/inheritance.h"
#include "../../../../src/include/proto.h"
#include "../../../../src/include/types.h"
#include "../../../../src/io/horizontal/reader.h"
#include "../../../../src/util/error.h"
#include "../../../../src/util/memory.h"

#include "../../../../src/include/generated/tree_property_accessors.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

extern double *Age;

#define MAX_SNAPSHOTS 5
#define MAX_ROWS 2
#define MAX_GALAXIES 4

/* Lookback times, distinct per snapshot so a source_time taken from the wrong
 * snapshot is unmistakable. */
static double fixture_age[MAX_SNAPSHOTS] = {13.0, 11.0, 9.0, 7.0, 5.0};

/* One synthetic generation: raw halos, the three target-snapshot columns, output
 * ranges and an output buffer. */
struct SyntheticGeneration {
  int64_t nhalos;
  struct RawHalo halos[MAX_ROWS];
  int32_t descendant_snapshot[MAX_ROWS];
  int32_t first_progenitor_snapshot[MAX_ROWS];
  int32_t next_progenitor_snapshot[MAX_ROWS];
  struct HorizontalHaloAux aux[MAX_ROWS];
  struct Halo processed[MAX_GALAXIES];
};

static struct SyntheticGeneration generations[MAX_SNAPSHOTS];
static struct HorizontalRetainedGeneration pool[MAX_SNAPSHOTS];

/* A link as the v3 format stores it: a row and the snapshot it names. */
struct Link {
  int64_t row;
  int32_t snap;
};

static const struct Link NONE = {-1, -1};

static void reset_generations(void) {
  memset(generations, 0, sizeof(generations));
  for (int k = 0; k < MAX_SNAPSHOTS; k++) {
    memset(&pool[k], 0, sizeof(pool[k]));
    pool[k].snapnum = -1;
  }
  Age = fixture_age;
}

/* Define row `row` of snapshot `snap`. `galaxies` output records are placed in
 * that generation's buffer, each carrying SnapNum `galaxy_snap` (the snapshot
 * the galaxy was last marshalled at). */
static void set_halo(int snap, int row, int len, struct Link descendant, struct Link first,
                     struct Link next, int64_t fof_central, int64_t next_fof, int galaxies,
                     int galaxy_snap) {
  struct SyntheticGeneration *gen = &generations[snap];
  struct RawHalo *halo = &gen->halos[row];

  if (row + 1 > gen->nhalos) {
    gen->nhalos = row + 1;
  }
  halo->SnapNum = snap;
  halo->Len = len;
  halo->Descendant = descendant.row;
  halo->FirstProgenitor = first.row;
  halo->NextProgenitor = next.row;
  halo->FirstHaloInFOFgroup = fof_central;
  halo->NextHaloInFOFgroup = next_fof;
  gen->descendant_snapshot[row] = descendant.snap;
  gen->first_progenitor_snapshot[row] = first.snap;
  gen->next_progenitor_snapshot[row] = next.snap;

  int64_t first_halo = 0;
  for (int r = 0; r < row; r++) {
    first_halo += gen->aux[r].NHalos;
  }
  gen->aux[row].FirstHalo = galaxies > 0 ? first_halo : -1;
  gen->aux[row].NHalos = galaxies;
  for (int g = 0; g < galaxies; g++) {
    gen->processed[first_halo + g].SnapNum = galaxy_snap;
  }
}

/* Publish snapshot `snap` in the retention pool, as the driver does after its
 * sweep. */
static void retain(int snap) {
  pool[snap].snapnum = snap;
  pool[snap].view.halos = generations[snap].halos;
  pool[snap].view.count = generations[snap].nhalos;
  pool[snap].aux = generations[snap].aux;
  pool[snap].processed = generations[snap].processed;
  pool[snap].next_progenitor_snapshot = generations[snap].next_progenitor_snapshot;
}

/* The lookup at snapshot `snap`, over whatever is retained, with the population
 * counted from the retained generations plus the descendant slab itself. */
static struct HorizontalGatherContext lookup_at(int snap) {
  struct HorizontalGatherContext lookup;
  int64_t population = generations[snap].nhalos;

  for (int k = 0; k < MAX_SNAPSHOTS; k++) {
    if (pool[k].snapnum == k) {
      population += pool[k].view.count;
    }
  }

  lookup.snapnum = snap;
  lookup.first_progenitor_snapshot = generations[snap].first_progenitor_snapshot;
  lookup.generations = pool;
  lookup.retained_population = population;
  return lookup;
}

static struct HaloInputView view_of(int snap) {
  struct HaloInputView view = {generations[snap].halos, generations[snap].nhalos};
  return view;
}

static struct Link at(int64_t row, int32_t snap) {
  struct Link link = {row, snap};
  return link;
}

/*
 * The design review's worked graph, as converted (committed as
 * ../data/worked_graph/, generated by ../data/source/generate_sources.py):
 *
 *   snapshot 0:  row 0 A  -> D(2);             NextProgenitor B (row 0 of snapshot 1)
 *   snapshot 1:  row 0 B  -> D(2);  FoF central; NextProgenitor C (row 1 of snapshot 1)
 *                row 1 C  -> D(2);  satellite of B, no galaxy (never a FoF central)
 *   snapshot 2:  row 0 D  -> E(4);  FirstProgenitor A (row 0 of snapshot 0)
 *   snapshot 3:  empty
 *   snapshot 4:  row 0 E;           FirstProgenitor D (row 0 of snapshot 2)
 *
 * Masses A 30, B 20, C 10, D 50, E 60. A, B and D each host one galaxy by the
 * time a later snapshot reads them; D's buffer holds A's continuing galaxy and
 * B's orphan (two records).
 */
static void seed_worked_graph(void) {
  reset_generations();
  set_halo(0, 0, 30, at(0, 2), NONE, at(0, 1), 0, -1, 1, 0);
  set_halo(1, 0, 20, at(0, 2), NONE, at(1, 1), 0, 1, 1, 1);
  set_halo(1, 1, 10, at(0, 2), NONE, NONE, 0, -1, 0, 1);
  set_halo(2, 0, 50, at(0, 4), at(0, 0), NONE, 0, -1, 2, 2);
  set_halo(4, 0, 60, NONE, at(0, 2), NONE, 0, -1, 0, 4);
}

/* A version 3 slab over one synthetic generation, for the horizon rule. */
static struct SnapshotSlab slab_of(int snap) {
  struct SnapshotSlab slab = snapshot_slab_empty();
  slab.snapnum = snap;
  slab.nhalos = generations[snap].nhalos;
  slab.halos = generations[snap].nhalos > 0 ? generations[snap].halos : NULL;
  slab.descendant_snapshot = generations[snap].descendant_snapshot;
  slab.first_progenitor_snapshot = generations[snap].first_progenitor_snapshot;
  slab.next_progenitor_snapshot = generations[snap].next_progenitor_snapshot;
  return slab;
}

/**
 * @brief   Horizons of the worked graph are the design review's: 2, 2, 4, 3, 4.
 */
static int test_worked_graph_horizons(void) {
  seed_worked_graph();

  const int64_t expected[MAX_SNAPSHOTS] = {2, 2, 4, 3, 4};
  for (int snap = 0; snap < MAX_SNAPSHOTS; snap++) {
    const struct SnapshotSlab slab = slab_of(snap);
    char message[160];
    snprintf(message, sizeof(message),
             "snapshot %d's horizon is the latest DescendantSnapshot of its rows, or its own "
             "snapshot when none has a descendant",
             snap);
    TEST_ASSERT_EQUAL(horizontal_generation_horizon(&slab), expected[snap], message);
  }

  return TEST_PASS;
}

/**
 * @brief   A version 2 slab (no DescendantSnapshot column) implicitly names N+1.
 */
static int test_version2_horizon_is_adjacent(void) {
  seed_worked_graph();

  /* Snapshot 0 of the worked graph, stripped of its version 3 columns: its one
   * halo has a descendant, so the version 2 reading is N+1, not the gapped 2. */
  struct SnapshotSlab slab = slab_of(0);
  slab.descendant_snapshot = NULL;
  slab.first_progenitor_snapshot = NULL;
  slab.next_progenitor_snapshot = NULL;
  TEST_ASSERT_EQUAL(horizontal_generation_horizon(&slab), 1,
                    "a version 2 halo with a descendant keeps its generation until N+1");

  /* Snapshot 4: no descendant anywhere, so the generation dies with its own
   * snapshot under either version. */
  struct SnapshotSlab terminal = slab_of(4);
  terminal.descendant_snapshot = NULL;
  TEST_ASSERT_EQUAL(horizontal_generation_horizon(&terminal), 4,
                    "a slab with no descendant is released after its own snapshot");

  return TEST_PASS;
}

/**
 * @brief   D's chain spans snapshots 0 and 1: A (the head) is found, then B and C.
 */
static int test_worked_graph_chain_spans_two_snapshots(void) {
  seed_worked_graph();
  retain(0);
  retain(1);
  const struct HaloInputView view = view_of(2);
  const struct HorizontalGatherContext lookup = lookup_at(2);

  /* A (occupied FirstProgenitor) pins the main branch. A lookup that assumed
   * N-1 would read row 0 of snapshot 1 (B) instead, a different generation. */
  const struct HorizontalProgenitorRef chosen =
      horizontal_find_most_massive_progenitor(view, &lookup, 0);
  TEST_ASSERT(chosen.snapnum == 0 && chosen.halonr == 0,
              "D's most massive progenitor is A, in snapshot 0, not row 0 of N-1");

  /* A (1) + B (1) + C (0): only a chain that reaches both snapshots counts 2. */
  TEST_ASSERT_EQUAL((int)horizontal_count_progenitor_galaxies(view, &lookup, 0), 2,
                    "the chain A -> B -> C gathers A's and B's galaxies");

  /* Three chain steps, while the largest retained slab holds two halos: the old
   * one-slab cycle bound would have aborted this valid chain. */
  TEST_ASSERT(generations[1].nhalos < 3 && lookup.retained_population >= 3,
              "the fixture's chain must be longer than any one slab for the bound to matter");

  struct InheritanceProgenitorGalaxy gathered[MAX_GALAXIES];
  memset(gathered, 0, sizeof(gathered));
  horizontal_gather_progenitor_galaxies(view, &lookup, 0, chosen, gathered);

  TEST_ASSERT(gathered[0].source == &generations[0].processed[0],
              "the first gathered galaxy is A's, from snapshot 0's buffer");
  TEST_ASSERT(gathered[1].source == &generations[1].processed[0],
              "the second is B's, from snapshot 1's buffer");
  TEST_ASSERT_EQUAL(gathered[0].is_main_branch, 1, "A's galaxy continues the main branch");
  TEST_ASSERT_EQUAL(gathered[1].is_main_branch, 0,
                    "B's galaxy is not main branch although it sits at the same row as A");
  TEST_ASSERT_DOUBLE_EQUAL(gathered[0].source_time, fixture_age[0], 0.0,
                           "A's galaxy evolves from Age[0], across the gap");
  TEST_ASSERT_DOUBLE_EQUAL(gathered[1].source_time, fixture_age[1], 0.0,
                           "B's galaxy evolves from Age[1]");

  return TEST_PASS;
}

/**
 * @brief   Snapshot 1 has no progenitor while snapshot 0 is retained.
 */
static int test_zero_progenitor_snapshot(void) {
  seed_worked_graph();
  retain(0);
  const struct HaloInputView view = view_of(1);
  const struct HorizontalGatherContext lookup = lookup_at(1);

  for (int64_t row = 0; row < generations[1].nhalos; row++) {
    const struct HorizontalProgenitorRef none =
        horizontal_find_most_massive_progenitor(view, &lookup, row);
    TEST_ASSERT(none.snapnum == -1 && none.halonr == -1,
                "a halo with no FirstProgenitor selects nothing, even with a generation retained");
    TEST_ASSERT_EQUAL((int)horizontal_count_progenitor_galaxies(view, &lookup, row), 0,
                      "a halo with no FirstProgenitor gathers nothing");
  }

  return TEST_PASS;
}

/**
 * @brief   E finds D in snapshot 2 across the empty, released snapshot 3.
 */
static int test_link_across_empty_snapshot(void) {
  seed_worked_graph();
  /* At snapshot 4 only snapshot 2 is retained: 0 and 1 went at their horizon 2,
   * and the empty snapshot 3 went at its own. */
  retain(2);
  const struct HaloInputView view = view_of(4);
  const struct HorizontalGatherContext lookup = lookup_at(4);

  const struct HorizontalProgenitorRef chosen =
      horizontal_find_most_massive_progenitor(view, &lookup, 0);
  TEST_ASSERT(chosen.snapnum == 2 && chosen.halonr == 0,
              "E's progenitor is D in snapshot 2, reached without snapshot 3");
  TEST_ASSERT_EQUAL((int)horizontal_count_progenitor_galaxies(view, &lookup, 0), 2,
                    "E inherits both of D's galaxies");

  return TEST_PASS;
}

/*
 * One chain spanning three snapshots: D(3) <- P0(0) -> P1(1) -> P2(2), each the
 * only halo of its snapshot, so every progenitor is row 0 of its slab. Masses
 * P0 20, P1 50, P2 30, D 100.
 */
static void seed_three_snapshot_chain(int p0_galaxies) {
  reset_generations();
  set_halo(0, 0, 20, at(0, 3), NONE, at(0, 1), 0, -1, p0_galaxies, 0);
  set_halo(1, 0, 50, at(0, 3), NONE, at(0, 2), 0, -1, 1, 1);
  set_halo(2, 0, 30, at(0, 3), NONE, NONE, 0, -1, 1, 2);
  set_halo(3, 0, 100, NONE, at(0, 0), NONE, 0, -1, 0, 3);
  retain(0);
  retain(1);
  retain(2);
}

/**
 * @brief   A chain over snapshots 0, 1 and 2 is walked in stored order, whole.
 */
static int test_chain_spanning_three_snapshots(void) {
  seed_three_snapshot_chain(1);
  const struct HaloInputView view = view_of(3);
  const struct HorizontalGatherContext lookup = lookup_at(3);

  for (int snap = 0; snap < 3; snap++) {
    const struct SnapshotSlab slab = slab_of(snap);
    TEST_ASSERT_EQUAL(horizontal_generation_horizon(&slab), 3,
                      "every progenitor generation is retained until snapshot 3");
  }

  TEST_ASSERT_EQUAL((int)horizontal_count_progenitor_galaxies(view, &lookup, 0), 3,
                    "one galaxy from each of the three progenitor snapshots");

  /* P0 is occupied, so it is pinned although P1 is more massive. */
  const struct HorizontalProgenitorRef chosen =
      horizontal_find_most_massive_progenitor(view, &lookup, 0);
  TEST_ASSERT(chosen.snapnum == 0 && chosen.halonr == 0, "the occupied head P0 is pinned");

  struct InheritanceProgenitorGalaxy gathered[MAX_GALAXIES];
  memset(gathered, 0, sizeof(gathered));
  horizontal_gather_progenitor_galaxies(view, &lookup, 0, chosen, gathered);
  for (int snap = 0; snap < 3; snap++) {
    TEST_ASSERT(gathered[snap].source == &generations[snap].processed[0],
                "chain order is P0, P1, P2, each from its own snapshot's buffer");
    TEST_ASSERT_DOUBLE_EQUAL(gathered[snap].source_time, fixture_age[snap], 0.0,
                             "each galaxy evolves from its own snapshot's age");
  }
  TEST_ASSERT_EQUAL(gathered[0].is_main_branch, 1, "P0's galaxy is the main branch");
  TEST_ASSERT_EQUAL(gathered[1].is_main_branch + gathered[2].is_main_branch, 0,
                    "P1 and P2 share P0's row but not its generation, so neither is main branch");

  return TEST_PASS;
}

/**
 * @brief   A NextProgenitor may name an EARLIER snapshot than its owner's:
 *          D(3) <- P0(2) -> P1(0) -> P2(1), each progenitor row 0 of its snapshot.
 *          The chain is walked in stored order and each entry is read from the
 *          generation its own target-snapshot column names.
 */
static int test_next_progenitor_into_an_earlier_snapshot(void) {
  reset_generations();
  set_halo(2, 0, 20, at(0, 3), NONE, at(0, 0), 0, -1, 1, 2);
  set_halo(0, 0, 50, at(0, 3), NONE, at(0, 1), 0, -1, 1, 0);
  set_halo(1, 0, 30, at(0, 3), NONE, NONE, 0, -1, 1, 1);
  set_halo(3, 0, 100, NONE, at(0, 2), NONE, 0, -1, 0, 3);
  retain(0);
  retain(1);
  retain(2);
  const struct HaloInputView view = view_of(3);
  const struct HorizontalGatherContext lookup = lookup_at(3);

  for (int snap = 0; snap < 3; snap++) {
    const struct SnapshotSlab slab = slab_of(snap);
    TEST_ASSERT_EQUAL(horizontal_generation_horizon(&slab), 3,
                      "every progenitor generation is retained until snapshot 3");
  }

  TEST_ASSERT_EQUAL((int)horizontal_count_progenitor_galaxies(view, &lookup, 0), 3,
                    "one galaxy from each of the three progenitor snapshots");

  /* P0 is occupied, so it is pinned although P1 is more massive. */
  const struct HorizontalProgenitorRef chosen =
      horizontal_find_most_massive_progenitor(view, &lookup, 0);
  TEST_ASSERT(chosen.snapnum == 2 && chosen.halonr == 0,
              "the occupied head P0 (snapshot 2, row 0) is pinned");

  struct InheritanceProgenitorGalaxy gathered[MAX_GALAXIES];
  memset(gathered, 0, sizeof(gathered));
  horizontal_gather_progenitor_galaxies(view, &lookup, 0, chosen, gathered);
  const int chain_snapshots[3] = {2, 0, 1};
  for (int i = 0; i < 3; i++) {
    const int snap = chain_snapshots[i];
    TEST_ASSERT(gathered[i].source == &generations[snap].processed[0],
                "chain order is P0, P1, P2, each from its own snapshot's buffer");
    TEST_ASSERT_DOUBLE_EQUAL(gathered[i].source_time, fixture_age[snap], 0.0,
                             "each galaxy evolves from its own snapshot's age");
  }
  TEST_ASSERT_EQUAL(gathered[0].is_main_branch, 1, "P0's galaxy is the main branch");
  TEST_ASSERT_EQUAL(gathered[1].is_main_branch + gathered[2].is_main_branch, 0,
                    "P1 and P2 share P0's row but not its generation, so neither is main branch");

  return TEST_PASS;
}

/**
 * @brief   With an unoccupied head, the scan crosses snapshots to the heaviest occupied entry.
 */
static int test_unoccupied_head_scans_across_snapshots(void) {
  seed_three_snapshot_chain(0);
  const struct HaloInputView view = view_of(3);
  const struct HorizontalGatherContext lookup = lookup_at(3);

  const struct HorizontalProgenitorRef chosen =
      horizontal_find_most_massive_progenitor(view, &lookup, 0);
  TEST_ASSERT(chosen.snapnum == 1 && chosen.halonr == 0,
              "P1 (Len 50, snapshot 1) beats P2 (Len 30, snapshot 2) once P0 is empty");

  struct InheritanceProgenitorGalaxy gathered[MAX_GALAXIES];
  memset(gathered, 0, sizeof(gathered));
  horizontal_gather_progenitor_galaxies(view, &lookup, 0, chosen, gathered);
  TEST_ASSERT(gathered[0].source == &generations[1].processed[0] &&
                  gathered[1].source == &generations[2].processed[0],
              "the empty P0 contributes nothing; P1's then P2's galaxies follow");
  TEST_ASSERT_EQUAL(gathered[0].is_main_branch, 1, "P1's galaxy is the main branch");
  TEST_ASSERT_EQUAL(gathered[1].is_main_branch, 0,
                    "P2's galaxy, at the same row in another snapshot, is not");

  return TEST_PASS;
}

/**
 * @brief   A NextProgenitor inside its owner's own snapshot (the common v3 case).
 */
static int test_same_snapshot_next_progenitor(void) {
  /* The adjacent fixture's shape: X(2) <- Y(1) -> W(1), Y the more massive. */
  reset_generations();
  set_halo(1, 0, 60, at(0, 2), NONE, at(1, 1), 0, -1, 1, 1);
  set_halo(1, 1, 25, at(0, 2), NONE, NONE, 1, -1, 1, 1);
  set_halo(2, 0, 90, NONE, at(0, 1), NONE, 0, -1, 0, 2);
  retain(1);
  const struct HaloInputView view = view_of(2);
  const struct HorizontalGatherContext lookup = lookup_at(2);

  TEST_ASSERT_EQUAL((int)horizontal_count_progenitor_galaxies(view, &lookup, 0), 2,
                    "Y and its same-snapshot sibling W are both gathered");

  struct InheritanceProgenitorGalaxy gathered[MAX_GALAXIES];
  memset(gathered, 0, sizeof(gathered));
  const struct HorizontalProgenitorRef chosen =
      horizontal_find_most_massive_progenitor(view, &lookup, 0);
  horizontal_gather_progenitor_galaxies(view, &lookup, 0, chosen, gathered);
  TEST_ASSERT(gathered[0].source == &generations[1].processed[0] &&
                  gathered[1].source == &generations[1].processed[1],
              "Y's galaxy then W's, both from snapshot 1's buffer");
  TEST_ASSERT(gathered[0].is_main_branch == 1 && gathered[1].is_main_branch == 0,
              "Y is the main branch, W is not");

  return TEST_PASS;
}

/* Run `body` in a child process and report its exit status and the start of
 * what it wrote to stderr. Returns 0 on a normal exit, -1 otherwise. */
static int run_in_child(void (*body)(void), int *exit_code, char *message, size_t message_size) {
  int channel[2];

  fflush(NULL);
  if (pipe(channel) != 0) {
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    close(channel[0]);
    (void)freopen("/dev/null", "w", stdout);
    dup2(channel[1], STDERR_FILENO);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);
    body();
    _exit(0);
  }

  close(channel[1]);
  size_t used = 0;
  ssize_t got;
  while (used + 1 < message_size &&
         (got = read(channel[0], message + used, message_size - 1 - used)) > 0) {
    used += (size_t)got;
  }
  message[used] = '\0';
  close(channel[0]);

  int status;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
    return -1;
  }
  *exit_code = WEXITSTATUS(status);
  return 0;
}

/* D looked up with snapshot 0 already released: A's generation is gone. */
static void count_with_released_head(void) {
  seed_worked_graph();
  retain(1);
  const struct HorizontalGatherContext lookup = lookup_at(2);
  (void)horizontal_count_progenitor_galaxies(view_of(2), &lookup, 0);
}

/* B's NextProgenitor rewritten to point back at itself: B -> C -> B -> ... */
static void count_around_a_cycle(void) {
  seed_worked_graph();
  generations[1].halos[1].NextProgenitor = 0;
  generations[1].next_progenitor_snapshot[1] = 1;
  retain(0);
  retain(1);
  const struct HorizontalGatherContext lookup = lookup_at(2);
  (void)horizontal_count_progenitor_galaxies(view_of(2), &lookup, 0);
}

/**
 * @brief   A link into a released generation aborts rather than reading N-1.
 */
static int test_link_into_released_generation_aborts(void) {
  int exit_code = 0;
  char message[1024];

  TEST_ASSERT_EQUAL(run_in_child(count_with_released_head, &exit_code, message, sizeof(message)), 0,
                    "the child should exit normally");
  TEST_ASSERT(exit_code != 0, "resolving into a released generation must abort");
  TEST_ASSERT(strstr(message, "not retained") != NULL,
              "the abort should say the named generation is not retained");

  return TEST_PASS;
}

/**
 * @brief   A NextProgenitor cycle aborts once it exceeds the retained population.
 */
static int test_cycle_is_bounded_by_retained_population(void) {
  int exit_code = 0;
  char message[1024];

  TEST_ASSERT_EQUAL(run_in_child(count_around_a_cycle, &exit_code, message, sizeof(message)), 0,
                    "the child should exit normally");
  TEST_ASSERT(exit_code != 0, "a progenitor cycle must abort rather than loop");
  TEST_ASSERT(strstr(message, "halos of every retained generation") != NULL,
              "the abort should name the retained-population bound");

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Horizontal Retained-Generation Progenitor Lookup\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_worked_graph_horizons);
  TEST_RUN(test_version2_horizon_is_adjacent);
  TEST_RUN(test_worked_graph_chain_spans_two_snapshots);
  TEST_RUN(test_zero_progenitor_snapshot);
  TEST_RUN(test_link_across_empty_snapshot);
  TEST_RUN(test_chain_spanning_three_snapshots);
  TEST_RUN(test_next_progenitor_into_an_earlier_snapshot);
  TEST_RUN(test_unoccupied_head_scans_across_snapshots);
  TEST_RUN(test_same_snapshot_next_progenitor);
  TEST_RUN(test_link_into_released_generation_aborts);
  TEST_RUN(test_cycle_is_bounded_by_retained_population);

  check_memory_leaks();

  TEST_SUMMARY();
  return TEST_RESULT();
}
