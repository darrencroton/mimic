/**
 * @file    test_framework.h
 * @brief   Minimal dependency-free C unit testing framework for Mimic
 *
 * This framework provides basic testing capabilities without external dependencies.
 * It integrates with Mimic's existing error handling and memory management systems.
 *
 * Usage:
 *   1. Include this header in test files
 *   2. Define test functions that return 0 (pass) or 1 (fail)
 *   3. Use TEST_ASSERT() for validations
 *   4. Use TEST_RUN() to execute tests
 *   5. Use TEST_SUMMARY() to report results
 *
 * Example:
 *   #include "test_framework.h"
 *
 *   static int passed = 0, failed = 0;
 *
 *   int test_example(void) {
 *       int result = 42;
 *       TEST_ASSERT(result == 42, "Expected 42");
 *       return 0;
 *   }
 *
 *   int main(void) {
 *       TEST_RUN(test_example);
 *       TEST_SUMMARY();
 *       return TEST_RESULT();
 *   }
 *
 * @author  Mimic Development Team
 * @date    2025-11-08
 * @version 1.0 (Phase 2: Testing Framework)
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ANSI color codes for test output. These short names are claimed by this
 * header; test translation units must not define their own. */
#define BLUE "\033[1;34m"
#define GREEN "\033[0;32m"
#define RED "\033[0;31m"
#define YELLOW "\033[1;33m"
#define NC "\033[0m"

/* Per-translation-unit framework state. Test files own 'passed'/'failed'
 * (see TEST_RUN); the framework owns these. */
static int test_skips __attribute__((unused)) = 0;
static const char *test_skip_reason __attribute__((unused)) = "";
static int test_fail_marked __attribute__((unused)) = 0;

/**
 * @def     TEST_MARKER_PASS / TEST_MARKER_FAIL / TEST_MARKER_SKIP / TEST_MARKER_WARN /
 * TEST_MARKER_ERROR
 * @brief   Emit a structured MIMIC_RESULT: line for summary-mode filtering.
 *
 * These are the only lines the summary filter matches — no natural-language
 * regex needed.  Emit to stdout so they land in the same captured stream as
 * all other test output.  fflush ensures ordering when stderr is interleaved.
 *
 * TEST_MARKER_FAIL also records that this test emitted a failing marker, so
 * TEST_RUN can guarantee every failure is visible in summary mode without
 * double-reporting assertion failures.
 */
#define TEST_MARKER_WITH_REASON(status, name, reason)                                              \
  do {                                                                                             \
    printf("MIMIC_RESULT: " status " %s -- %s\n", (name), (reason));                               \
    fflush(stdout);                                                                                \
  } while (0)
#define TEST_MARKER_PASS(name)                                                                     \
  do {                                                                                             \
    printf("MIMIC_RESULT: PASS %s\n", (name));                                                     \
    fflush(stdout);                                                                                \
  } while (0)
#define TEST_MARKER_FAIL(name, reason)                                                             \
  do {                                                                                             \
    TEST_MARKER_WITH_REASON("FAIL", (name), (reason));                                             \
    test_fail_marked = 1;                                                                          \
  } while (0)
#define TEST_MARKER_SKIP(name, reason) TEST_MARKER_WITH_REASON("SKIP", (name), (reason))
#define TEST_MARKER_WARN(name, msg) TEST_MARKER_WITH_REASON("WARN", (name), (msg))
#define TEST_MARKER_ERROR(name, msg) TEST_MARKER_WITH_REASON("ERROR", (name), (msg))

/**
 * @def     TEST_ASSERT
 * @brief   Assert that a condition is true, fail test if false
 *
 * @param   cond    Condition to evaluate
 * @param   msg     Error message if assertion fails
 *
 * Usage:
 *   TEST_ASSERT(value > 0, "Value must be positive");
 *   TEST_ASSERT(ptr != NULL, "Pointer cannot be NULL");
 */
#define TEST_ASSERT(cond, msg)                                                                     \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      TEST_MARKER_FAIL(__func__, msg);                                                             \
      fprintf(stderr, "✗ FAIL: %s\n", msg);                                                        \
      fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);                                  \
      fprintf(stderr, "  Condition: %s\n", #cond);                                                 \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

/**
 * @def     TEST_ASSERT_EQUAL
 * @brief   Assert that two values are equal
 *
 * @param   a       First value
 * @param   b       Second value
 * @param   msg     Error message if assertion fails
 */
#define TEST_ASSERT_EQUAL(a, b, msg)                                                               \
  do {                                                                                             \
    if ((a) != (b)) {                                                                              \
      TEST_MARKER_FAIL(__func__, msg);                                                             \
      fprintf(stderr, "✗ FAIL: %s\n", msg);                                                        \
      fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);                                  \
      fprintf(stderr, "  Expected: %lld, Got: %lld\n", (long long)(b), (long long)(a));            \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

/**
 * @def     TEST_ASSERT_DOUBLE_EQUAL
 * @brief   Assert that two doubles are equal within tolerance
 *
 * @param   a       First value
 * @param   b       Second value
 * @param   tol     Tolerance (absolute)
 * @param   msg     Error message if assertion fails
 */
#define TEST_ASSERT_DOUBLE_EQUAL(a, b, tol, msg)                                                   \
  do {                                                                                             \
    if (fabs((a) - (b)) > (tol)) {                                                                 \
      TEST_MARKER_FAIL(__func__, msg);                                                             \
      fprintf(stderr, "✗ FAIL: %s\n", msg);                                                        \
      fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);                                  \
      fprintf(stderr, "  Expected: %.6f, Got: %.6f (tol: %.6f)\n", (double)(b), (double)(a),       \
              (double)(tol));                                                                      \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

/**
 * @def     TEST_ASSERT_STRING_EQUAL
 * @brief   Assert that two strings are equal
 *
 * @param   a       First string
 * @param   b       Second string
 * @param   msg     Error message if assertion fails
 */
#define TEST_ASSERT_STRING_EQUAL(a, b, msg)                                                        \
  do {                                                                                             \
    if (strcmp((a), (b)) != 0) {                                                                   \
      TEST_MARKER_FAIL(__func__, msg);                                                             \
      fprintf(stderr, "✗ FAIL: %s\n", msg);                                                        \
      fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);                                  \
      fprintf(stderr, "  Expected: \"%s\", Got: \"%s\"\n", (b), (a));                              \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

/**
 * @def     TEST_RUN
 * @brief   Run a test function and track results
 *
 * @param   test_func   Test function to execute; returns TEST_PASS, TEST_FAIL,
 *                      or TEST_SKIP (see those macros)
 *
 * Automatically increments 'passed' or 'failed' counters (skips are tracked
 * by the framework and reported by TEST_SUMMARY; they count as neither).
 * Requires: static int passed = 0, failed = 0; in the file scope.
 *
 * Every outcome emits a MIMIC_RESULT marker: assertion failures emit theirs
 * from TEST_ASSERT*, and a test that fails without one gets a generic FAIL
 * marker here so no failure is invisible in summary mode.
 *
 * Usage:
 *   TEST_RUN(test_memory_allocation);
 *   TEST_RUN(test_parameter_parsing);
 */
#define TEST_RUN(test_func)                                                                        \
  do {                                                                                             \
    int test_run_rc;                                                                               \
    printf("\nRunning: %-50s ", #test_func);                                                       \
    fflush(stdout);                                                                                \
    test_fail_marked = 0;                                                                          \
    test_skip_reason = "";                                                                         \
    test_run_rc = test_func();                                                                     \
    if (test_run_rc == TEST_PASS) {                                                                \
      printf("✓ PASS\n");                                                                          \
      TEST_MARKER_PASS(#test_func);                                                                \
      passed++;                                                                                    \
    } else if (test_run_rc == TEST_SKIP) {                                                         \
      printf("– SKIP\n");                                                                          \
      TEST_MARKER_SKIP(#test_func, test_skip_reason[0] ? test_skip_reason : "skipped");            \
      test_skips++;                                                                                \
    } else {                                                                                       \
      printf("✗ FAIL\n");                                                                          \
      if (!test_fail_marked) {                                                                     \
        TEST_MARKER_FAIL(#test_func, "test returned failure without a failing assertion");         \
      }                                                                                            \
      failed++;                                                                                    \
    }                                                                                              \
  } while (0)

/**
 * @def     TEST_SUMMARY
 * @brief   Print test execution summary
 *
 * Displays total passed and failed tests.
 *
 * Usage:
 *   TEST_SUMMARY();
 */
#define TEST_SUMMARY()                                                                             \
  do {                                                                                             \
    printf("\n%s", BLUE);                                                                          \
    printf("============================================================\n");                      \
    printf("Test Summary\n");                                                                      \
    printf("============================================================\n");                      \
    printf("%s", NC);                                                                              \
    printf("Passed: %d\n", passed);                                                                \
    if (test_skips > 0) {                                                                          \
      printf("Skipped: %d\n", test_skips);                                                         \
    }                                                                                              \
    printf("Failed: %d\n", failed);                                                                \
    printf("Total:  %d\n", passed + test_skips + failed);                                          \
    printf("%s", BLUE);                                                                            \
    printf("============================================================\n");                      \
    printf("%s\n", NC);                                                                            \
    if (failed == 0) {                                                                             \
      printf("%s✓ All tests passed!%s\n", GREEN, NC);                                              \
    } else {                                                                                       \
      printf("%s✗ %d test(s) failed%s\n", RED, failed, NC);                                        \
    }                                                                                              \
  } while (0)

/**
 * @def     TEST_RESULT
 * @brief   Return appropriate exit code based on test results
 *
 * Returns 0 if all tests passed, 1 if any tests failed.
 *
 * Usage:
 *   return TEST_RESULT();
 */
#define TEST_RESULT() (failed > 0 ? 1 : 0)

/**
 * @def     TEST_PASS / TEST_FAIL / TEST_SKIP
 * @brief   Return-value vocabulary for test functions run via TEST_RUN.
 *
 * Return TEST_SKIP (optionally via TEST_SKIP_WITH for a reason) from a test
 * that cannot run in this configuration; TEST_RUN emits the SKIP marker so
 * the skip stays visible in summary mode instead of masquerading as a pass.
 */
#define TEST_PASS 0
#define TEST_FAIL 1
#define TEST_SKIP 2

/**
 * @def     TEST_SKIP_WITH
 * @brief   Skip the current test with a reason shown in the SKIP marker.
 *
 * Usage:
 *   return TEST_SKIP_WITH("requires process isolation");
 */
#define TEST_SKIP_WITH(reason) (test_skip_reason = (reason), TEST_SKIP)

#endif /* TEST_FRAMEWORK_H */
