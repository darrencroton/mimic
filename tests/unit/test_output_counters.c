/**
 * @file    test_output_counters.c
 * @brief   Unit tests for shared output counter guards.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tests/framework/test_framework.h"
#include "globals.h"
#include "output/util.h"
#include "tree/reader.h" /* enum InputProcessingOrder */
#include "util/error.h"

static int passed = 0;
static int failed = 0;

static int counter_storage;

static void reset_counter_state(int total_count, int tree_count) {
  memset(TotHalosPerSnap, 0, sizeof(TotHalosPerSnap));
  memset(InputHalosPerSnap, 0, sizeof(InputHalosPerSnap));
  counter_storage = tree_count;
  TotHalosPerSnap[0] = total_count;
  InputHalosPerSnap[0] = &counter_storage;
}

static int run_counter_increment_in_child(int total_count, int tree_count) {
  fflush(NULL);

  pid_t pid = fork();
  int status;

  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);
    reset_counter_state(total_count, tree_count);
    output_increment_halo_counters_checked(7, 0, 49, 0);
    _exit(0);
  }

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }

  if (WIFSIGNALED(status)) {
    return -1;
  }

  return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

/**
 * @test    test_counter_increment_updates_total_and_tree_counts
 * @brief   Incrementing once raises both the snapshot total and per-tree halo counters
 */
static int test_counter_increment_updates_total_and_tree_counts(void) {
  reset_counter_state(11, 5);

  output_increment_halo_counters_checked(3, 0, 49, 0);

  TEST_ASSERT_EQUAL(TotHalosPerSnap[0], 12, "total halo counter should increment");
  TEST_ASSERT_EQUAL(InputHalosPerSnap[0][0], 6, "per-tree halo counter should increment");

  return TEST_PASS;
}

/**
 * @test    test_counter_increment_fatals_at_total_int_limit
 * @brief   Incrementing past INT_MAX on the snapshot total triggers a fatal
 */
static int test_counter_increment_fatals_at_total_int_limit(void) {
  int result = run_counter_increment_in_child(INT_MAX, 0);

  TEST_ASSERT(result != -1, "total-limit child should not crash");
  TEST_ASSERT(result == 1, "total-limit counter increment should fatal");

  return TEST_PASS;
}

/**
 * @test    test_counter_increment_fatals_at_tree_int_limit
 * @brief   Incrementing past INT_MAX on the per-tree counter triggers a fatal
 */
static int test_counter_increment_fatals_at_tree_int_limit(void) {
  int result = run_counter_increment_in_child(0, INT_MAX);

  TEST_ASSERT(result != -1, "tree-limit child should not crash");
  TEST_ASSERT(result == 1, "tree-limit counter increment should fatal");

  return TEST_PASS;
}

/**
 * @brief   Run the snapshot-mode counter increment in a forked child.
 *
 * Snapshot-ordered runs never allocate InputHalosPerSnap (only the tree
 * reader does, src/io/tree/interface.c); if the tree-only split regressed,
 * the increment would dereference the NULL InputHalosPerSnap[0] set up here
 * and crash. Forking isolates that crash so it shows up as a failed
 * assertion rather than taking down the whole suite, mirroring
 * run_counter_increment_in_child() above. The child itself checks the
 * post-increment state and reports the result via its exit code, so a wrong
 * total (logic present but broken) is distinguished from a crash (logic
 * missing entirely).
 */
static int run_snapshot_mode_counter_in_child(void) {
  fflush(NULL);

  pid_t pid = fork();
  int status;

  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    (void)freopen("/dev/null", "w", stdout);
    (void)freopen("/dev/null", "w", stderr);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    memset(TotHalosPerSnap, 0, sizeof(TotHalosPerSnap));
    memset(InputHalosPerSnap, 0, sizeof(InputHalosPerSnap));
    TotHalosPerSnap[0] = 41;
    InputHalosPerSnap[0] = NULL; /* never allocated: only the tree reader does this */
    MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_SNAPSHOT;

    output_increment_halo_counters_checked(9, 0, 49, 0);

    if (TotHalosPerSnap[0] != 42) {
      _exit(2);
    }
    if (InputHalosPerSnap[0] != NULL) {
      _exit(3);
    }
    _exit(0);
  }

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }

  if (WIFSIGNALED(status)) {
    return -1; /* crashed: NULL InputHalosPerSnap[0] was dereferenced */
  }

  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/**
 * @test    test_counter_increment_snapshot_mode_skips_tree_side
 * @brief   Snapshot-ordered increments update only the snapshot total, never InputHalosPerSnap
 */
static int test_counter_increment_snapshot_mode_skips_tree_side(void) {
  int result = run_snapshot_mode_counter_in_child();

  TEST_ASSERT(result != -1,
              "snapshot-mode counter increment must not dereference NULL InputHalosPerSnap");
  TEST_ASSERT_EQUAL(result, 0,
                    "snapshot-mode counter increment should update only the snapshot total");

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Output Counters\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_counter_increment_updates_total_and_tree_counts);
  TEST_RUN(test_counter_increment_fatals_at_total_int_limit);
  TEST_RUN(test_counter_increment_fatals_at_tree_int_limit);
  TEST_RUN(test_counter_increment_snapshot_mode_skips_tree_side);

  TEST_SUMMARY();
  return TEST_RESULT();
}
