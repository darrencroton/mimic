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
#define TEST_ASSERT(cond, msg)                                                \
    if (!(cond)) {                                                            \
        fprintf(stderr, "✗ FAIL: %s\n", msg);                                 \
        fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);           \
        fprintf(stderr, "  Condition: %s\n", #cond);                          \
        return 1;                                                             \
    }

/**
 * @def     TEST_ASSERT_EQUAL
 * @brief   Assert that two values are equal
 *
 * @param   a       First value
 * @param   b       Second value
 * @param   msg     Error message if assertion fails
 */
#define TEST_ASSERT_EQUAL(a, b, msg)                                          \
    if ((a) != (b)) {                                                         \
        fprintf(stderr, "✗ FAIL: %s\n", msg);                                 \
        fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);           \
        fprintf(stderr, "  Expected: %d, Got: %d\n", (int)(b), (int)(a));     \
        return 1;                                                             \
    }

/**
 * @def     TEST_ASSERT_DOUBLE_EQUAL
 * @brief   Assert that two doubles are equal within tolerance
 *
 * @param   a       First value
 * @param   b       Second value
 * @param   tol     Tolerance (absolute)
 * @param   msg     Error message if assertion fails
 */
#define TEST_ASSERT_DOUBLE_EQUAL(a, b, tol, msg)                              \
    if (fabs((a) - (b)) > (tol)) {                                            \
        fprintf(stderr, "✗ FAIL: %s\n", msg);                                 \
        fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);           \
        fprintf(stderr, "  Expected: %.6f, Got: %.6f (tol: %.6f)\n",          \
                (double)(b), (double)(a), (double)(tol));                     \
        return 1;                                                             \
    }

/**
 * @def     TEST_ASSERT_STRING_EQUAL
 * @brief   Assert that two strings are equal
 *
 * @param   a       First string
 * @param   b       Second string
 * @param   msg     Error message if assertion fails
 */
#define TEST_ASSERT_STRING_EQUAL(a, b, msg)                                   \
    if (strcmp((a), (b)) != 0) {                                              \
        fprintf(stderr, "✗ FAIL: %s\n", msg);                                 \
        fprintf(stderr, "  Location: %s:%d\n", __FILE__, __LINE__);           \
        fprintf(stderr, "  Expected: \"%s\", Got: \"%s\"\n", (b), (a));       \
        return 1;                                                             \
    }

/**
 * @def     TEST_RUN
 * @brief   Run a test function and track results
 *
 * @param   test_func   Test function to execute (must return 0 for pass, 1 for fail)
 *
 * Automatically increments 'passed' or 'failed' counters.
 * Requires: static int passed = 0, failed = 0; in the file scope.
 *
 * Usage:
 *   TEST_RUN(test_memory_allocation);
 *   TEST_RUN(test_parameter_parsing);
 */
#define TEST_RUN(test_func)                                                   \
    do {                                                                      \
        printf("Running %-50s ", #test_func "...");                           \
        fflush(stdout);                                                       \
        if (test_func() == 0) {                                               \
            printf("✓ PASS\n");                                               \
            passed++;                                                         \
        } else {                                                              \
            printf("✗ FAIL\n");                                               \
            failed++;                                                         \
        }                                                                     \
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
#define TEST_SUMMARY()                                                        \
    do {                                                                      \
        printf("\n");                                                         \
        printf("=========================================\n");                 \
        printf("Test Summary\n");                                             \
        printf("=========================================\n");                 \
        printf("Passed: %d\n", passed);                                       \
        printf("Failed: %d\n", failed);                                       \
        printf("Total:  %d\n", passed + failed);                              \
        printf("=========================================\n");                 \
        if (failed == 0) {                                                    \
            printf("✓ All tests passed!\n");                                  \
        } else {                                                              \
            printf("✗ %d test(s) failed\n", failed);                          \
        }                                                                     \
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
 * @def     TEST_PASS
 * @brief   Constant representing test pass
 */
#define TEST_PASS 0

/**
 * @def     TEST_FAIL
 * @brief   Constant representing test fail
 */
#define TEST_FAIL 1

#endif /* TEST_FRAMEWORK_H */
