/**
 * @file    test_numeric_utilities.c
 * @brief   Unit tests for numerical utility functions
 *
 * Validates: Safe floating-point comparison and numerical stability utilities
 * Phase: Phase 2 (Testing Framework)
 *
 * This test validates that Mimic's numerical utilities correctly:
 * - Detect effectively zero values
 * - Compare floats with appropriate tolerance
 * - Handle edge cases (zero, infinity, NaN)
 * - Provide consistent comparison results
 * - Handle range checking
 *
 * Test cases:
 *   - test_is_zero: Zero detection with tolerance
 *   - test_is_equal: Equality comparison with tolerance
 *   - test_comparison_operators: Greater/less than comparisons
 *   - test_range_checking: Value within range
 *   - test_edge_cases: NaN, Inf, and extreme values
 *
 * @author  Mimic Testing Team
 * @date    2025-11-08
 */

#include "../framework/test_framework.h"
#include "../../src/util/numeric.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"
#include "../../src/include/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @test    test_is_zero
 * @brief   Test zero detection with tolerance
 *
 * Expected: Detects values close to zero, rejects non-zero values
 * Validates: is_zero() function
 */
int test_is_zero(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    /* True zeros */
    TEST_ASSERT(is_zero(0.0) == true, "Exact zero should be detected");
    TEST_ASSERT(is_zero(-0.0) == true, "Negative zero should be detected");

    /* Values smaller than EPSILON_SMALL */
    TEST_ASSERT(is_zero(EPSILON_SMALL / 10.0) == true,
                "Tiny positive value should be treated as zero");
    TEST_ASSERT(is_zero(-EPSILON_SMALL / 10.0) == true,
                "Tiny negative value should be treated as zero");

    /* Values larger than EPSILON_SMALL */
    TEST_ASSERT(is_zero(EPSILON_SMALL * 10.0) == false,
                "Value > EPSILON_SMALL should not be zero");
    TEST_ASSERT(is_zero(-EPSILON_SMALL * 10.0) == false,
                "Negative value > EPSILON_SMALL should not be zero");

    /* Clearly non-zero values */
    TEST_ASSERT(is_zero(1.0) == false, "1.0 is not zero");
    TEST_ASSERT(is_zero(-1.0) == false, "-1.0 is not zero");
    TEST_ASSERT(is_zero(1.0e10) == false, "Large value is not zero");

    printf("  ✓ is_zero() works correctly\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_is_equal
 * @brief   Test equality comparison with tolerance
 *
 * Expected: Detects equality within EPSILON_MEDIUM
 * Validates: is_equal() function
 */
int test_is_equal(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    /* Exact equality */
    TEST_ASSERT(is_equal(1.0, 1.0) == true, "Exact values should be equal");
    TEST_ASSERT(is_equal(0.0, 0.0) == true, "Exact zeros should be equal");

    /* Nearly equal values */
    double x = 1.0;
    double y = 1.0 + EPSILON_MEDIUM / 10.0;
    TEST_ASSERT(is_equal(x, y) == true,
                "Values within EPSILON_MEDIUM should be equal");

    /* Different values */
    TEST_ASSERT(is_equal(1.0, 2.0) == false, "Different values should not be equal");
    TEST_ASSERT(is_equal(0.0, 1.0) == false, "0 and 1 should not be equal");

    /* Slightly different values (beyond tolerance) */
    double a = 1.0;
    double b = 1.0 + EPSILON_MEDIUM * 10.0;
    TEST_ASSERT(is_equal(a, b) == false,
                "Values beyond EPSILON_MEDIUM should not be equal");

    printf("  ✓ is_equal() works correctly\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_comparison_operators
 * @brief   Test greater/less than comparisons
 *
 * Expected: Comparisons account for floating-point tolerance
 * Validates: is_greater(), is_less(), is_greater_or_equal(), is_less_or_equal()
 */
int test_comparison_operators(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */

    /* is_greater() tests */
    TEST_ASSERT(is_greater(2.0, 1.0) == true, "2.0 > 1.0");
    TEST_ASSERT(is_greater(1.0, 1.0) == false, "1.0 not > 1.0");
    TEST_ASSERT(is_greater(1.0, 2.0) == false, "1.0 not > 2.0");

    /* is_less() tests */
    TEST_ASSERT(is_less(1.0, 2.0) == true, "1.0 < 2.0");
    TEST_ASSERT(is_less(1.0, 1.0) == false, "1.0 not < 1.0");
    TEST_ASSERT(is_less(2.0, 1.0) == false, "2.0 not < 1.0");

    /* is_greater_or_equal() tests */
    TEST_ASSERT(is_greater_or_equal(2.0, 1.0) == true, "2.0 >= 1.0");
    TEST_ASSERT(is_greater_or_equal(1.0, 1.0) == true, "1.0 >= 1.0");
    TEST_ASSERT(is_greater_or_equal(1.0, 2.0) == false, "1.0 not >= 2.0");

    /* is_less_or_equal() tests */
    TEST_ASSERT(is_less_or_equal(1.0, 2.0) == true, "1.0 <= 2.0");
    TEST_ASSERT(is_less_or_equal(1.0, 1.0) == true, "1.0 <= 1.0");
    TEST_ASSERT(is_less_or_equal(2.0, 1.0) == false, "2.0 not <= 1.0");

    printf("  ✓ Comparison operators work correctly\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_range_checking
 * @brief   Test value within range checking
 *
 * Expected: Correctly identifies values inside/outside range
 * Validates: is_within() function
 */
int test_range_checking(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */

    /* Values within range */
    TEST_ASSERT(is_within(5.0, 0.0, 10.0) == true, "5.0 is within [0, 10]");
    TEST_ASSERT(is_within(0.0, 0.0, 10.0) == true, "0.0 is within [0, 10]");
    TEST_ASSERT(is_within(10.0, 0.0, 10.0) == true, "10.0 is within [0, 10]");

    /* Values outside range */
    TEST_ASSERT(is_within(-1.0, 0.0, 10.0) == false, "-1.0 is not within [0, 10]");
    TEST_ASSERT(is_within(11.0, 0.0, 10.0) == false, "11.0 is not within [0, 10]");

    /* Edge cases with floating-point tolerance */
    double val_at_edge = 10.0 + EPSILON_SMALL / 10.0;
    TEST_ASSERT(is_within(val_at_edge, 0.0, 10.0) == true,
                "Value barely > max should still be within (tolerance)");

    printf("  ✓ is_within() works correctly\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_edge_cases
 * @brief   Test handling of special floating-point values
 *
 * Expected: Handle NaN, Inf gracefully (return false for comparisons)
 * Validates: Robustness of numerical utilities
 */
int test_edge_cases(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */

    /* Test with infinity */
    double inf = INFINITY;
    TEST_ASSERT(is_zero(inf) == false, "Infinity is not zero");
    TEST_ASSERT(is_equal(inf, inf) == false, "Infinity should not equal infinity (NaN propagation)");

    /* Test with NaN */
    double nan_val = NAN;
    TEST_ASSERT(is_zero(nan_val) == false, "NaN is not zero");
    TEST_ASSERT(is_equal(nan_val, nan_val) == false, "NaN should not equal NaN");
    TEST_ASSERT(is_equal(nan_val, 1.0) == false, "NaN should not equal 1.0");

    /* Test with very large values */
    double large = 1.0e100;
    TEST_ASSERT(is_zero(large) == false, "Very large value is not zero");
    TEST_ASSERT(is_equal(large, large) == true, "Large value should equal itself");

    /* Test with very small values (just above EPSILON_SMALL threshold) */
    double tiny = 1.0e-9;  /* Slightly larger than EPSILON_SMALL (1e-10) */
    TEST_ASSERT(is_zero(tiny) == false, "Tiny but non-zero value is not zero");

    /* Test with value below threshold (should be treated as zero) */
    double ultra_tiny = 1.0e-100;  /* Far below EPSILON_SMALL */
    TEST_ASSERT(is_zero(ultra_tiny) == true, "Ultra-tiny value treated as zero");

    printf("  ✓ Edge cases handled correctly\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_consistency
 * @brief   Test that comparison functions are internally consistent
 *
 * Expected: Logical consistency (if a > b, then b < a, etc.)
 * Validates: Internal consistency of comparison functions
 */
int test_consistency(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */

    double a = 5.0;
    double b = 10.0;

    /* If a < b, then NOT (a > b) */
    if (is_less(a, b)) {
        TEST_ASSERT(is_greater(a, b) == false,
                    "If a < b, then NOT (a > b)");
    }

    /* If a > b, then NOT (a < b) */
    if (is_greater(a, b)) {
        TEST_ASSERT(is_less(a, b) == false,
                    "If a > b, then NOT (a < b)");
    }

    /* If a == b, then NOT (a > b) AND NOT (a < b) */
    double c = 5.0;
    double d = 5.0;
    if (is_equal(c, d)) {
        TEST_ASSERT(is_greater(c, d) == false,
                    "If a == b, then NOT (a > b)");
        TEST_ASSERT(is_less(c, d) == false,
                    "If a == b, then NOT (a < b)");
    }

    /* a >= b should be equivalent to (a > b OR a == b) */
    bool ge = is_greater_or_equal(a, b);
    bool g_or_e = is_greater(a, b) || is_equal(a, b);
    TEST_ASSERT(ge == g_or_e, "a >= b should equal (a > b OR a == b)");

    printf("  ✓ Comparison functions are internally consistent\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all test cases and reports results.
 */
int main(void) {
    printf("========================================\n");
    printf("Test Suite: Numeric Utilities\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_is_zero);
    TEST_RUN(test_is_equal);
    TEST_RUN(test_comparison_operators);
    TEST_RUN(test_range_checking);
    TEST_RUN(test_edge_cases);
    TEST_RUN(test_consistency);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
