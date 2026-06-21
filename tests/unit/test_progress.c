/**
 * @file    test_progress.c
 * @brief   Unit tests for the terminal progress bar formatter
 *
 * Validates the pure, terminal-independent rendering helper
 * progress_bar_format(): bar fill, percentage, item counts, elapsed/ETA
 * formatting, and ANSI colour gating. The live-drawing and TTY/MPI fallback
 * paths in progress.c depend on a real terminal and are exercised by manual
 * runs (see USER-GUIDE.md), not here.
 */

#include "../framework/test_framework.h"
#include "../../src/util/progress.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Count filled bar cells: dash shaft ('-') plus arrowhead ('>'). */
static int count_filled(const char *s) {
  int n = 0;
  for (; *s; s++)
    if (*s == '-' || *s == '>')
      n++;
  return n;
}

/**
 * @test    test_bounds
 * @brief   0%, 50%, and 100% render the expected fill and percentage
 */
int test_bounds(void) {
  char buf[256];

  /* Empty */
  progress_bar_format(buf, sizeof(buf), 0, 100, 0.0, 30, 0, "file 1");
  TEST_ASSERT(strstr(buf, "  0%") != NULL, "0/100 should report 0%");
  TEST_ASSERT(strstr(buf, "(0/100)") != NULL, "count should be (0/100)");
  TEST_ASSERT(count_filled(buf) == 0, "0% should have no filled cells");

  /* Half */
  progress_bar_format(buf, sizeof(buf), 50, 100, 1.0, 30, 0, "file 1");
  TEST_ASSERT(strstr(buf, " 50%") != NULL, "50/100 should report 50%");
  TEST_ASSERT(count_filled(buf) == 15, "50% of width 30 should fill 15 cells");

  /* Full */
  progress_bar_format(buf, sizeof(buf), 100, 100, 2.0, 30, 0, "file 1");
  TEST_ASSERT(strstr(buf, "100%") != NULL, "100/100 should report 100%");
  TEST_ASSERT(count_filled(buf) == 30, "100% should fill the whole width");

  return TEST_PASS;
}

/**
 * @test    test_label_and_counts
 * @brief   Label prefix and item counts appear; NULL label is tolerated
 */
int test_label_and_counts(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 42, 1000, 0.0, 30, 0, "file 7");
  TEST_ASSERT(strncmp(buf, "file 7 [", 8) == 0, "label should prefix the bar");
  TEST_ASSERT(strstr(buf, "(42/1000)") != NULL, "counts should be (42/1000)");

  /* NULL label must not crash and must not emit a leading space. */
  progress_bar_format(buf, sizeof(buf), 1, 2, 0.0, 30, 0, NULL);
  TEST_ASSERT(buf[0] == '[', "NULL label should start directly at the bar");

  return TEST_PASS;
}

/**
 * @test    test_eta_and_elapsed
 * @brief   ETA is zero at completion; elapsed/ETA formatting is correct
 */
int test_eta_and_elapsed(void) {
  char buf[256];

  /* Half done after 10s -> ~10s remaining; elapsed shown as 0:10. */
  progress_bar_format(buf, sizeof(buf), 50, 100, 10.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "elapsed 0:10") != NULL, "elapsed should format as 0:10");
  TEST_ASSERT(strstr(buf, "ETA 0:10") != NULL, "ETA at 50%% after 10s should be ~0:10");

  /* Complete -> ETA 0:00 regardless of elapsed. */
  progress_bar_format(buf, sizeof(buf), 100, 100, 30.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "ETA 0:00") != NULL, "ETA at completion should be 0:00");

  /* Durations of an hour or more switch to H:MM:SS. */
  progress_bar_format(buf, sizeof(buf), 50, 100, 3661.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "elapsed 1:01:01") != NULL, "3661s should format as 1:01:01");

  return TEST_PASS;
}

/**
 * @test    test_color_gating
 * @brief   ANSI escapes appear only when colour is requested
 */
int test_color_gating(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 50, 100, 1.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "\x1b[") == NULL, "no ANSI escapes when colour is off");

  progress_bar_format(buf, sizeof(buf), 50, 100, 1.0, 30, 1, "f");
  TEST_ASSERT(strstr(buf, "\x1b[92m") != NULL, "green start code present when colour is on");
  TEST_ASSERT(strstr(buf, "\x1b[0m") != NULL, "reset code present when colour is on");

  return TEST_PASS;
}

/**
 * @test    test_zero_total
 * @brief   A zero/empty total is treated as complete without dividing by zero
 */
int test_zero_total(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 0, 0, 0.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "100%") != NULL, "empty file should render as complete");
  TEST_ASSERT(strstr(buf, "(0/0)") != NULL, "counts should be (0/0)");

  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Progress Bar Formatter\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_bounds);
  TEST_RUN(test_label_and_counts);
  TEST_RUN(test_eta_and_elapsed);
  TEST_RUN(test_color_gating);
  TEST_RUN(test_zero_total);

  TEST_SUMMARY();
  return TEST_RESULT();
}
