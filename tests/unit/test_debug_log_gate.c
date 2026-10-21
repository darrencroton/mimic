/**
 * @file    test_debug_log_gate.c
 * @brief   Unit tests for the DEBUG_LOG level gate and per-site rate-limit budget
 *
 * DEBUG_LOG has two contracts, both asserted here against log output redirected
 * through set_log_output():
 *
 *   1. With the log level above DEBUG it does nothing: its arguments are not
 *      evaluated, no line is written, and it reaches no function in error.c.
 *   2. With the level at DEBUG, a call site armed for rate limiting emits its first
 *      DEBUG_LOG_MAX_CALLS messages and then exactly one suppression line; the
 *      budget is consumed only by a message that is emitted, so calls made while
 *      the level was above DEBUG do not spend it. With rate limiting disarmed
 *      every call emits.
 *
 * The budget is per call site, so each scenario calls one dedicated site function
 * that holds the single DEBUG_LOG under test.
 */

#include "../framework/test_framework.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** Times the DEBUG_LOG argument below was evaluated */
static int argument_evaluations = 0;

/** @brief Evaluate to a value while counting the evaluation (a side-effecting argument) */
static int counted_argument(void) { return ++argument_evaluations; }

/** @brief The one call site under test: its static budget persists across calls */
static void budgeted_site(void) { DEBUG_LOG("gate test message %d", counted_argument()); }

/** @brief A second, independent call site, for the independence of budgets */
static void other_site(void) { DEBUG_LOG("other site message"); }

/** Redirected log output; the tests write here and count its lines */
static FILE *capture = NULL;

/** @brief Count the lines written to the capture file so far, and the suppression lines */
static void count_lines(int *lines, int *suppression_lines) {
  char line[512];

  *lines = 0;
  *suppression_lines = 0;
  fflush(capture);
  rewind(capture);
  while (fgets(line, sizeof(line), capture) != NULL) {
    (*lines)++;
    if (strstr(line, "further debug output from this location suppressed") != NULL) {
      (*suppression_lines)++;
    }
  }
  fseek(capture, 0, SEEK_END);
}

/** @brief Discard everything captured so far */
static void reset_capture(void) {
  fflush(capture);
  if (ftruncate(fileno(capture), 0) != 0) {
    return;
  }
  rewind(capture);
}

/**
 * @test    test_disabled_path_is_free
 * @brief   Above DEBUG, a DEBUG_LOG evaluates no argument and writes nothing
 */
int test_disabled_path_is_free(void) {
  int lines, suppression_lines;

  set_log_level(LOG_LEVEL_INFO);
  disable_debug_log_rate_limiting();
  reset_capture();
  argument_evaluations = 0;

  for (int i = 0; i < 100; i++) {
    budgeted_site();
  }
  TEST_ASSERT(!debug_log_enabled(), "debug output should be off at INFO");
  TEST_ASSERT(argument_evaluations == 0, "a disabled DEBUG_LOG must not evaluate its arguments");
  count_lines(&lines, &suppression_lines);
  TEST_ASSERT(lines == 0, "a disabled DEBUG_LOG must write nothing");

  /* The same holds with rate limiting armed: the level is tested first. */
  enable_debug_log_rate_limiting();
  for (int i = 0; i < 100; i++) {
    budgeted_site();
  }
  disable_debug_log_rate_limiting();
  TEST_ASSERT(argument_evaluations == 0, "armed and disabled: arguments must not be evaluated");
  count_lines(&lines, &suppression_lines);
  TEST_ASSERT(lines == 0, "armed and disabled: nothing must be written");

  /* Every level above DEBUG disables it. */
  set_log_level(LOG_LEVEL_FATAL);
  budgeted_site();
  TEST_ASSERT(argument_evaluations == 0, "FATAL level must also disable DEBUG_LOG");

  set_log_level(LOG_LEVEL_DEBUG);
  TEST_ASSERT(debug_log_enabled(), "debug output should be on at DEBUG");
  set_log_level(LOG_LEVEL_INFO);

  return TEST_PASS;
}

/**
 * @test    test_enabled_unarmed_emits_every_call
 * @brief   At DEBUG with rate limiting disarmed, every call emits and evaluates once
 */
int test_enabled_unarmed_emits_every_call(void) {
  int lines, suppression_lines;

  set_log_level(LOG_LEVEL_DEBUG);
  disable_debug_log_rate_limiting();
  reset_capture();
  argument_evaluations = 0;

  const int calls = 3 * DEBUG_LOG_MAX_CALLS;
  for (int i = 0; i < calls; i++) {
    budgeted_site();
  }
  count_lines(&lines, &suppression_lines);
  TEST_ASSERT(lines == calls, "every unarmed call should emit one line");
  TEST_ASSERT(suppression_lines == 0, "an unarmed site must never print a suppression line");
  TEST_ASSERT(argument_evaluations == calls, "each emitted call evaluates its argument once");

  set_log_level(LOG_LEVEL_INFO);
  return TEST_PASS;
}

/**
 * @test    test_armed_budget_then_one_suppression_line
 * @brief   At DEBUG and armed, a site emits its budget, then exactly one suppression line
 */
int test_armed_budget_then_one_suppression_line(void) {
  int lines, suppression_lines;

  set_log_level(LOG_LEVEL_DEBUG);
  enable_debug_log_rate_limiting();
  reset_capture();
  argument_evaluations = 0;

  /* A site no earlier test has called: its static budget is untouched. */
  for (int i = 0; i < 4 * DEBUG_LOG_MAX_CALLS; i++) {
    other_site();
  }
  count_lines(&lines, &suppression_lines);
  TEST_ASSERT(lines == DEBUG_LOG_MAX_CALLS + 1,
              "a site should emit DEBUG_LOG_MAX_CALLS messages plus one suppression line");
  TEST_ASSERT(suppression_lines == 1, "exactly one suppression line should be written");

  disable_debug_log_rate_limiting();
  set_log_level(LOG_LEVEL_INFO);
  return TEST_PASS;
}

/**
 * @test    test_disabled_calls_do_not_spend_the_budget
 * @brief   Calls made above DEBUG leave the site's full budget for when DEBUG returns
 */
int test_disabled_calls_do_not_spend_the_budget(void) {
  int lines, suppression_lines;

  /* budgeted_site() has not yet been called with rate limiting armed and the level at
   * DEBUG: the earlier tests called it unarmed (no budget) or disabled (no budget). */
  set_log_level(LOG_LEVEL_INFO);
  enable_debug_log_rate_limiting();
  reset_capture();
  argument_evaluations = 0;

  for (int i = 0; i < 10 * DEBUG_LOG_MAX_CALLS; i++) {
    budgeted_site();
  }
  count_lines(&lines, &suppression_lines);
  TEST_ASSERT(lines == 0, "no line while the level is above DEBUG");

  set_log_level(LOG_LEVEL_DEBUG);
  for (int i = 0; i < 4 * DEBUG_LOG_MAX_CALLS; i++) {
    budgeted_site();
  }
  count_lines(&lines, &suppression_lines);
  TEST_ASSERT(lines == DEBUG_LOG_MAX_CALLS + 1,
              "the full budget plus one suppression line should still be emitted");
  TEST_ASSERT(suppression_lines == 1, "exactly one suppression line should be written");
  TEST_ASSERT(argument_evaluations == DEBUG_LOG_MAX_CALLS,
              "only the emitted messages evaluate their argument");

  disable_debug_log_rate_limiting();
  set_log_level(LOG_LEVEL_INFO);
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: DEBUG_LOG Level Gate\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  capture = tmpfile();
  if (capture == NULL) {
    printf("cannot create the log capture file\n");
    return 1;
  }
  FILE *previous_output = set_log_output(capture);

  TEST_RUN(test_disabled_path_is_free);
  TEST_RUN(test_enabled_unarmed_emits_every_call);
  TEST_RUN(test_armed_budget_then_one_suppression_line);
  TEST_RUN(test_disabled_calls_do_not_spend_the_budget);

  set_log_output(previous_output);
  fclose(capture);

  TEST_SUMMARY();
  return TEST_RESULT();
}
