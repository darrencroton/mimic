/**
 * @file test_unit_metallicity.c
 * @brief Unit tests for metallicity calculation utility
 *
 * Tests the shared metallicity utility function for correctness,
 * edge cases, and numerical stability. This utility is used by
 * 6+ physics modules for safe metallicity calculations.
 *
 * @author Mimic Testing Team
 * @date 2025-11-23
 */

#include "../../../tests/framework/test_framework.h"
#include "metallicity.h"
#include <math.h>
#include <stdio.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @test test_metallicity_normal
 * @brief Test normal metallicity calculation
 *
 * Expected: Z = metals / gas for typical values
 * Validates: Basic arithmetic and float precision
 */
int test_metallicity_normal(void) {
    float Z;

    /* Test case 1: Simple calculation */
    /* Z = 2.0 / 100.0 = 0.02 (solar metallicity) */
    Z = mimic_get_metallicity(100.0f, 2.0f);
    TEST_ASSERT(fabsf(Z - 0.02f) < 1e-6f,
                "Metallicity for 2 Msun metals in 100 Msun gas should be 0.02");

    /* Test case 2: Sub-solar metallicity */
    /* Z = 0.2 / 100.0 = 0.002 (0.1 solar) */
    Z = mimic_get_metallicity(100.0f, 0.2f);
    TEST_ASSERT(fabsf(Z - 0.002f) < 1e-6f,
                "Sub-solar metallicity should calculate correctly");

    /* Test case 3: Super-solar metallicity */
    /* Z = 10.0 / 100.0 = 0.1 (5x solar) */
    Z = mimic_get_metallicity(100.0f, 10.0f);
    TEST_ASSERT(fabsf(Z - 0.1f) < 1e-6f,
                "Super-solar metallicity should calculate correctly");

    /* Test case 4: Very small gas mass (but above EPSILON_SMALL) */
    /* Z = 0.0001 / 0.01 = 0.01 */
    Z = mimic_get_metallicity(0.01f, 0.0001f);
    TEST_ASSERT(fabsf(Z - 0.01f) < 1e-6f,
                "Small but valid gas mass should calculate correctly");

    printf("  Normal metallicity calculations: Z=%.4f (solar), Z=%.4f (sub-solar), Z=%.4f (super-solar)\n",
           mimic_get_metallicity(100.0f, 2.0f),
           mimic_get_metallicity(100.0f, 0.2f),
           mimic_get_metallicity(100.0f, 10.0f));

    return TEST_PASS;
}

/**
 * @test test_metallicity_zero_gas
 * @brief Test metallicity with zero or negligible gas mass
 *
 * Expected: Returns 0.0 when gas <= EPSILON_SMALL
 * Validates: Division by zero protection
 */
int test_metallicity_zero_gas(void) {
    float Z;

    /* Test case 1: Exactly zero gas */
    Z = mimic_get_metallicity(0.0f, 5.0f);
    TEST_ASSERT(fabsf(Z - 0.0f) < 1e-10f,
                "Zero gas mass should return Z=0.0");

    /* Test case 2: Very small gas (below EPSILON_SMALL = 1e-10) */
    Z = mimic_get_metallicity(1e-12f, 1.0f);
    TEST_ASSERT(fabsf(Z - 0.0f) < 1e-10f,
                "Gas below EPSILON_SMALL should return Z=0.0");

    /* Test case 3: Negative gas (unphysical but should be safe) */
    Z = mimic_get_metallicity(-1.0f, 1.0f);
    TEST_ASSERT(fabsf(Z - 0.0f) < 1e-10f,
                "Negative gas should safely return Z=0.0");

    printf("  Zero gas cases safely return Z=0.0\n");

    return TEST_PASS;
}

/**
 * @test test_metallicity_zero_metals
 * @brief Test metallicity with zero metals (primordial gas)
 *
 * Expected: Returns 0.0 for primordial (metal-free) gas
 * Validates: Correct handling of Z=0 case
 */
int test_metallicity_zero_metals(void) {
    float Z;

    /* Test case 1: Primordial gas (no metals) */
    Z = mimic_get_metallicity(100.0f, 0.0f);
    TEST_ASSERT(fabsf(Z - 0.0f) < 1e-10f,
                "Primordial gas (zero metals) should give Z=0.0");

    /* Test case 2: Very small metals */
    Z = mimic_get_metallicity(100.0f, 1e-12f);
    TEST_ASSERT(fabsf(Z) < 1e-10f,
                "Very small metals should give ~zero metallicity");

    /* Test case 3: Negative metals (unphysical but should handle) */
    Z = mimic_get_metallicity(100.0f, -1.0f);
    TEST_ASSERT(Z < 0.01f,  /* Just check it doesn't explode */
                "Negative metals should not cause crash");

    printf("  Primordial (metal-free) gas: Z=%.6f\n",
           mimic_get_metallicity(100.0f, 0.0f));

    return TEST_PASS;
}

/**
 * @test test_metallicity_solar_reference
 * @brief Test metallicity against solar reference value
 *
 * Expected: Solar metallicity Z_sun = 0.02 (commonly used value)
 * Validates: Physically meaningful reference case
 */
int test_metallicity_solar_reference(void) {
    float Z;
    const float Z_SOLAR = 0.02f;  /* Solar metallicity reference */

    /* Test case 1: Exact solar metallicity */
    Z = mimic_get_metallicity(100.0f, 2.0f);
    TEST_ASSERT(fabsf(Z - Z_SOLAR) < 1e-6f,
                "2% metal fraction should match solar metallicity");

    /* Test case 2: 0.1 solar */
    Z = mimic_get_metallicity(100.0f, 0.2f);
    TEST_ASSERT(fabsf(Z - 0.1f * Z_SOLAR) < 1e-6f,
                "0.1 solar metallicity should be Z=0.002");

    /* Test case 3: 3 solar */
    Z = mimic_get_metallicity(100.0f, 6.0f);
    TEST_ASSERT(fabsf(Z - 3.0f * Z_SOLAR) < 1e-6f,
                "3 solar metallicity should be Z=0.06");

    printf("  Solar reference: Z_sun=%.4f, 0.1*Z_sun=%.4f, 3*Z_sun=%.4f\n",
           Z_SOLAR,
           mimic_get_metallicity(100.0f, 0.2f),
           mimic_get_metallicity(100.0f, 6.0f));

    return TEST_PASS;
}

/**
 * @test test_metallicity_numerical_stability
 * @brief Test numerical stability with extreme values
 *
 * Expected: No NaN, Inf, or crashes with extreme inputs
 * Validates: Robustness for edge cases in simulations
 */
int test_metallicity_numerical_stability(void) {
    float Z;

    /* Test case 1: Very large gas mass */
    Z = mimic_get_metallicity(1e10f, 1e8f);  /* 10^10 Msun gas, 10^8 Msun metals */
    TEST_ASSERT(fabsf(Z - 0.01f) < 1e-5f,
                "Very large masses should calculate correctly");
    TEST_ASSERT(isfinite(Z), "Very large masses should not produce NaN/Inf");

    /* Test case 2: Very small masses (but above threshold) */
    Z = mimic_get_metallicity(1e-8f, 1e-10f);
    TEST_ASSERT(isfinite(Z), "Very small masses should not produce NaN/Inf");

    /* Test case 3: Gas at EPSILON_SMALL boundary */
    /* Just above threshold */
    Z = mimic_get_metallicity(1.1e-10f, 1e-12f);
    TEST_ASSERT(isfinite(Z), "Gas just above EPSILON_SMALL should work");

    /* Just below threshold */
    Z = mimic_get_metallicity(0.9e-10f, 1e-12f);
    TEST_ASSERT(fabsf(Z - 0.0f) < 1e-10f,
                "Gas just below EPSILON_SMALL should return 0.0");

    /* Test case 4: Metals > Gas (unphysical but should handle) */
    Z = mimic_get_metallicity(10.0f, 20.0f);  /* Z > 1.0 */
    TEST_ASSERT(isfinite(Z), "Metals > Gas should not crash");
    TEST_ASSERT(Z > 1.0f, "Metals > Gas should give Z > 1.0 (even if unphysical)");

    printf("  Numerical stability tests passed (no NaN/Inf)\n");

    return TEST_PASS;
}

int main(void) {
    printf("============================================================\n");
    printf("RUNNING METALLICITY UTILITY UNIT TESTS\n");
    printf("============================================================\n");

    printf("\n========================================\n");
    printf("Test Suite: Metallicity Calculations\n");
    printf("========================================\n");

    TEST_RUN(test_metallicity_normal);
    TEST_RUN(test_metallicity_zero_gas);
    TEST_RUN(test_metallicity_zero_metals);
    TEST_RUN(test_metallicity_solar_reference);
    TEST_RUN(test_metallicity_numerical_stability);

    printf("============================================================\n");
    TEST_SUMMARY();
    printf("============================================================\n");

    return TEST_RESULT();
}
