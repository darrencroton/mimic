/**
 * @file    test_snapshot_collectives_mpi.c
 * @brief   MPI control test of the snapshot collectives' cross-task results and error agreement
 *
 * The unit runner (tests/unit/run_tests.sh) compiles without -DMPI, so it only
 * proves the serial identity paths of src/core/snapshot_collectives.c. This test
 * is the multi-task half: tests/manual/test_distributed_identity.py (make
 * tests-distributed) compiles it with `mpicc -DMPI` against snapshot_collectives.c
 * and the util sources only, and runs it under `$MPIRUN -np 3` with a timeout, so
 * a collective that one task skips shows up as a timeout rather than a hang.
 *
 * module_registry.c is not linked: the stub below supplies the running-callback
 * accessor as RUNNING_CALLBACK_NONE, the state of a direct call outside any
 * dispatch, which the collectives allow. ThisTask and NTask are defined here
 * (allvars.c is not linked either) and set from MPI_COMM_WORLD.
 *
 * Coverage:
 * - module_snapshot_rank() against a brute-force oracle over the allgathered
 *   keys, with task 1 empty, values drawn from a small set so ties cross tasks,
 *   and more than 256 keys on a task (the regular-sample cap); also a case with
 *   fewer keys than tasks and one with every task empty;
 * - a NaN on one task and a duplicate id within one task each return -1 on every
 *   task, each after a positive control that ranks the same keys unperturbed;
 * - sum_i64, sum_f64 and min_max_f64 against hand sums (exactly representable
 *   doubles, NaN-free, the empty task seeding its extents with +/-INFINITY), an
 *   n == 0 call of every reduction with NULL arrays, and module_snapshot_any()
 *   with the flag on one task and on none.
 *
 * Every case computes its own verdict on every task and agrees it with an
 * MPI_Allreduce before asserting, so a failure on one task never returns that
 * task early into a collective the others have left. Only task 0 writes
 * stdout, so the MIMIC_RESULT markers appear once per case.
 *
 * Build and run by hand from the repository root after `make generate` (the gate
 * does this): compile this file, src/core/snapshot_collectives.c and every
 * src/util source with `mpicc -DMPI`, the include paths of tests/unit/run_tests.sh
 * plus -Ibuild/generated, and libyaml and -lm; then run the binary under
 * `mpirun -np 3`.
 */

#include <inttypes.h>
#include <math.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "framework/test_framework.h"
#include "memory.h"
#include "module_registry.h"
#include "snapshot_collectives.h"

static int passed = 0, failed = 0;

/* allvars.c is not linked: the two task globals task_layout.h reads. */
int ThisTask, NTask;

/** Test stub: no module callback is running (a direct call outside any dispatch). */
enum RunningCallbackKind module_registry_running_callback(const char **module_name) {
  if (module_name != NULL) {
    *module_name = NULL;
  }
  return RUNNING_CALLBACK_NONE;
}

/** Test stub: main.c is not linked; a FATAL_ERROR in the linked sources ends every task. */
void myexit(int signum) {
  MPI_Abort(MPI_COMM_WORLD, signum != 0 ? signum : 1);
  exit(signum != 0 ? signum : 1);
}

#define CONTROL_TASKS 3

/** malloc that ends every task on failure, so no task strands the others in a collective. */
static void *checked_malloc(size_t count, size_t size) {
  void *block = malloc((count > 0 ? count : 1) * size);
  if (block == NULL) {
    fprintf(stderr, "task %d: out of memory in the MPI control test\n", ThisTask);
    MPI_Abort(MPI_COMM_WORLD, 2);
    exit(2);
  }
  return block;
}

/** Whether @p local_ok holds on every task (logical and over MPI_COMM_WORLD). */
static int all_tasks(int local_ok) {
  int global_ok = 0;
  MPI_Allreduce(&local_ok, &global_ok, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);
  return global_ok;
}

/* ==========================================================================
 * module_snapshot_rank
 * ========================================================================== */

/**
 * Rank this task's keys collectively and check every rank against a brute-force
 * oracle over every task's keys: a key's rank is the number of keys, on any task,
 * with a larger value or an equal value and a smaller id.
 *
 * @return  1 on this task when the call succeeded and every local rank matches
 */
static int rank_matches_oracle(const struct SnapshotRankKey *keys, int count) {
  int counts[CONTROL_TASKS];
  int displs[CONTROL_TASKS];
  MPI_Allgather(&count, 1, MPI_INT, counts, 1, MPI_INT, MPI_COMM_WORLD);
  int total = 0;
  for (int t = 0; t < NTask; t++) {
    displs[t] = total;
    total += counts[t];
  }
  double *values = checked_malloc((size_t)total, sizeof(double));
  int64_t *ids = checked_malloc((size_t)total, sizeof(int64_t));
  double *my_values = checked_malloc((size_t)count, sizeof(double));
  int64_t *my_ids = checked_malloc((size_t)count, sizeof(int64_t));
  int64_t *ranks = checked_malloc((size_t)count, sizeof(int64_t));
  for (int i = 0; i < count; i++) {
    my_values[i] = keys[i].value;
    my_ids[i] = keys[i].id;
    ranks[i] = -7;
  }
  MPI_Allgatherv(my_values, count, MPI_DOUBLE, values, counts, displs, MPI_DOUBLE, MPI_COMM_WORLD);
  MPI_Allgatherv(my_ids, count, MPI_INT64_T, ids, counts, displs, MPI_INT64_T, MPI_COMM_WORLD);

  int ok = module_snapshot_rank(NULL, keys, count, ranks) == 0;
  for (int i = 0; ok && i < count; i++) {
    int64_t expected = 0;
    for (int j = 0; j < total; j++) {
      if (values[j] > keys[i].value || (values[j] == keys[i].value && ids[j] < keys[i].id)) {
        expected++;
      }
    }
    if (ranks[i] != expected) {
      fprintf(stderr,
              "task %d: key %d (value %g, id %" PRId64 ") ranked %" PRId64 ", oracle %" PRId64 "\n",
              ThisTask, i, keys[i].value, keys[i].id, ranks[i], expected);
      ok = 0;
    }
  }
  free(values);
  free(ids);
  free(my_values);
  free(my_ids);
  free(ranks);
  return ok;
}

/**
 * Fill @p keys with @p count keys for this task: values from a small set so ties
 * cross tasks, ids unique across tasks (task-strided) and stored out of order.
 */
static void fill_tied_keys(struct SnapshotRankKey *keys, int count, int value_levels) {
  for (int i = 0; i < count; i++) {
    keys[i].value = (double)((i * 7 + ThisTask * 3) % value_levels);
    keys[i].id = (int64_t)(count - 1 - i) * CONTROL_TASKS + ThisTask;
  }
}

/**
 * @brief   The rank matches the oracle with an empty task, cross-task ties and > 256 keys
 */
static int test_rank_oracle_empty_task_and_ties(void) {
  /* Task 1 is empty; task 0 holds more keys than the 256-key sample cap. */
  static const int sizes[CONTROL_TASKS] = {300, 0, 257};
  struct SnapshotRankKey *keys = checked_malloc(300, sizeof(*keys));
  fill_tied_keys(keys, sizes[ThisTask], 11);
  const int ok = rank_matches_oracle(keys, sizes[ThisTask]);
  free(keys);
  TEST_ASSERT(all_tasks(ok), "every task's ranks match the brute-force oracle");
  return TEST_PASS;
}

/**
 * @brief   The rank matches the oracle with fewer keys than tasks, all tied
 */
static int test_rank_oracle_fewer_keys_than_tasks(void) {
  static const int sizes[CONTROL_TASKS] = {1, 0, 1};
  struct SnapshotRankKey keys[1];
  fill_tied_keys(keys, sizes[ThisTask], 1); /* one value level: every key ties */
  const int ok = rank_matches_oracle(keys, sizes[ThisTask]);
  TEST_ASSERT(all_tasks(ok), "two tied keys on tasks 0 and 2 are ordered by id");
  return TEST_PASS;
}

/**
 * @brief   Every task empty: the rank succeeds on every task without a hang
 */
static int test_rank_every_task_empty(void) {
  const int ok = module_snapshot_rank(NULL, NULL, 0, NULL) == 0;
  TEST_ASSERT(all_tasks(ok), "an empty rank returns 0 on every task");
  return TEST_PASS;
}

/**
 * @brief   A NaN value on one task returns -1 on every task
 */
static int test_rank_nan_on_one_task(void) {
  struct SnapshotRankKey keys[4];
  int64_t ranks[4];
  fill_tied_keys(keys, 4, 3);
  /* Positive control: the unperturbed keys rank cleanly, so the failure below is the
   * perturbation's alone. */
  const int control = module_snapshot_rank(NULL, keys, 4, ranks);
  TEST_ASSERT(all_tasks(control == 0), "the unperturbed keys rank on every task");
  if (ThisTask == 2) {
    keys[1].value = NAN;
  }
  const int status = module_snapshot_rank(NULL, keys, 4, ranks);
  TEST_ASSERT(all_tasks(status == -1), "a NaN on task 2 fails the rank on every task");
  return TEST_PASS;
}

/**
 * @brief   A duplicate id within one task returns -1 on every task
 */
static int test_rank_duplicate_id_within_one_task(void) {
  struct SnapshotRankKey keys[4];
  int64_t ranks[4];
  fill_tied_keys(keys, 4, 3);
  /* Positive control: the unperturbed keys rank cleanly, so the failure below is the
   * perturbation's alone. */
  const int control = module_snapshot_rank(NULL, keys, 4, ranks);
  TEST_ASSERT(all_tasks(control == 0), "the unperturbed keys rank on every task");
  if (ThisTask == 0) {
    keys[3].id = keys[0].id;
  }
  const int status = module_snapshot_rank(NULL, keys, 4, ranks);
  TEST_ASSERT(all_tasks(status == -1), "a duplicate id on task 0 fails the rank on every task");
  return TEST_PASS;
}

/* ==========================================================================
 * Reductions and module_snapshot_any
 * ========================================================================== */

/**
 * @brief   sum_i64 and sum_f64 match hand sums on every task
 */
static int test_sums_match_hand_sums(void) {
  /* Task t contributes {t + 1, 10 * t, -t} and {(t + 1) / 2, t / 4, -1.5}. */
  int64_t ivalues[3] = {ThisTask + 1, 10 * ThisTask, -ThisTask};
  double fvalues[3] = {0.5 * (ThisTask + 1), 0.25 * ThisTask, -1.5};
  const int istatus = module_snapshot_sum_i64(NULL, ivalues, 3);
  const int fstatus = module_snapshot_sum_f64(NULL, fvalues, 3);
  /* Over tasks 0, 1, 2: 1+2+3 = 6, 0+10+20 = 30, 0-1-2 = -3; 0.5+1+1.5 = 3,
   * 0+0.25+0.5 = 0.75, 3 * -1.5 = -4.5 (all exact in binary). */
  const int ok = istatus == 0 && fstatus == 0 && ivalues[0] == 6 && ivalues[1] == 30 &&
                 ivalues[2] == -3 && fvalues[0] == 3.0 && fvalues[1] == 0.75 && fvalues[2] == -4.5;
  TEST_ASSERT(all_tasks(ok), "the integer and double sums equal the hand sums on every task");
  return TEST_PASS;
}

/**
 * @brief   min_max_f64 matches the hand extents, the empty task seeding +/-INFINITY
 */
static int test_min_max_matches_hand_extents(void) {
  double minima[2] = {INFINITY, INFINITY};
  double maxima[2] = {-INFINITY, -INFINITY};
  if (ThisTask != 1) { /* task 1 holds no rows and keeps the neutral seeds */
    const double a = ThisTask == 0 ? 2.0 : -3.0;
    const double b = ThisTask == 0 ? 7.5 : 4.25;
    minima[0] = fmin(a, b);
    maxima[0] = fmax(a, b);
    minima[1] = 100.0 + ThisTask;
    maxima[1] = 100.0 + ThisTask;
  }
  const int status = module_snapshot_min_max_f64(NULL, minima, maxima, 2);
  const int ok = status == 0 && minima[0] == -3.0 && maxima[0] == 7.5 && minima[1] == 100.0 &&
                 maxima[1] == 102.0;
  TEST_ASSERT(all_tasks(ok), "the extents equal the hand extents on every task");
  return TEST_PASS;
}

/**
 * @brief   Every reduction accepts n == 0 with NULL arrays on every task
 */
static int test_reductions_with_no_values(void) {
  const int ok = module_snapshot_sum_i64(NULL, NULL, 0) == 0 &&
                 module_snapshot_sum_f64(NULL, NULL, 0) == 0 &&
                 module_snapshot_min_max_f64(NULL, NULL, NULL, 0) == 0;
  TEST_ASSERT(all_tasks(ok), "n == 0 reductions return 0 on every task");
  return TEST_PASS;
}

/**
 * @brief   any() is the logical or across tasks
 */
static int test_any_is_logical_or(void) {
  const int one = module_snapshot_any(NULL, ThisTask == 2);
  const int none = module_snapshot_any(NULL, 0);
  TEST_ASSERT(all_tasks(one == 1 && none == 0),
              "any() is 1 with the flag on task 2 only and 0 with it on none");
  return TEST_PASS;
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);
  MPI_Comm_size(MPI_COMM_WORLD, &NTask);
  if (ThisTask != 0 && freopen("/dev/null", "w", stdout) == NULL) {
    MPI_Abort(MPI_COMM_WORLD, 2);
  }
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("\n========================================\n");
  printf("Snapshot collectives MPI control test (NTask = %d)\n", NTask);
  printf("========================================\n");
  if (NTask != CONTROL_TASKS) {
    TEST_MARKER_FAIL("test_snapshot_collectives_mpi", "must run at -np 3");
    MPI_Finalize();
    return 1;
  }

  TEST_RUN(test_rank_oracle_empty_task_and_ties);
  TEST_RUN(test_rank_oracle_fewer_keys_than_tasks);
  TEST_RUN(test_rank_every_task_empty);
  TEST_RUN(test_rank_nan_on_one_task);
  TEST_RUN(test_rank_duplicate_id_within_one_task);
  TEST_RUN(test_sums_match_hand_sums);
  TEST_RUN(test_min_max_matches_hand_extents);
  TEST_RUN(test_reductions_with_no_values);
  TEST_RUN(test_any_is_logical_or);

  TEST_SUMMARY();
  const int result = TEST_RESULT();
  MPI_Finalize();
  return result;
}
