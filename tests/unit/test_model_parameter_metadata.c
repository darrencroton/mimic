/**
 * @file    test_model_parameter_metadata.c
 * @brief   Unit tests for model parameter metadata system
 *
 * Validates: Generated parameter metadata and validation correctness
 * Phase: Phase 4.4 (Centralized Model Parameters)
 *
 * This test validates that the model parameter metadata system correctly:
 * - Defines all required parameters (NUM_REQUIRED_MODEL_PARAMETERS)
 * - Provides metadata lookup for all parameters
 * - Validates parameter values against ranges (doubles and ints)
 * - Rejects out-of-range values with clear errors
 * - Detects unknown/nonexistent parameters
 * - Maintains type safety and validation correctness
 *
 * Test cases:
 *   - test_parameter_count: Verify 22 parameters defined
 *   - test_metadata_lookup: get_model_param_metadata() finds all params
 *   - test_double_validation: Valid doubles pass range checking
 *   - test_int_validation: Valid ints pass range checking
 *   - test_out_of_range_rejection: Out-of-range values rejected
 *   - test_unknown_parameter: Unknown params return NULL/error
 *
 * @author  Mimic Testing Team
 * @date    2025-11-28
 */

#include "../framework/test_framework.h"
#include "../../src/include/generated/model_parameters.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @test    test_parameter_count
 * @brief   Test that expected number of parameters are defined
 *
 * Expected: NUM_REQUIRED_MODEL_PARAMETERS == 22
 * Validates: Metadata generation includes all parameters
 */
int test_parameter_count(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    printf("  NUM_REQUIRED_MODEL_PARAMETERS: %d\n", NUM_REQUIRED_MODEL_PARAMETERS);

    /* Verify count matches expected (22 parameters as of Phase 4.4) */
    TEST_ASSERT_EQUAL(NUM_REQUIRED_MODEL_PARAMETERS, 22,
                      "Should have 22 required model parameters");

    /* Verify REQUIRED_MODEL_PARAMETERS array is populated */
    TEST_ASSERT(REQUIRED_MODEL_PARAMETERS != NULL,
                "REQUIRED_MODEL_PARAMETERS array should be defined");

    /* Verify all entries are non-NULL */
    for (int i = 0; i < NUM_REQUIRED_MODEL_PARAMETERS; i++) {
        TEST_ASSERT(REQUIRED_MODEL_PARAMETERS[i] != NULL,
                    "All parameter names should be non-NULL");
        TEST_ASSERT(strlen(REQUIRED_MODEL_PARAMETERS[i]) > 0,
                    "All parameter names should be non-empty");
    }

    /* Verify MODEL_PARAMETER_METADATA array is defined */
    TEST_ASSERT(MODEL_PARAMETER_METADATA != NULL,
                "MODEL_PARAMETER_METADATA array should be defined");

    printf("  ✓ All 22 parameters defined with metadata\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_metadata_lookup
 * @brief   Test metadata lookup for all parameters
 *
 * Expected: get_model_param_metadata() finds all 22 parameters
 * Validates: Metadata lookup function works correctly
 */
int test_metadata_lookup(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    int found_count = 0;

    /* Test lookup for all defined parameters */
    for (int i = 0; i < NUM_REQUIRED_MODEL_PARAMETERS; i++) {
        const char *param_name = REQUIRED_MODEL_PARAMETERS[i];
        const struct ModelParameterMetadata *meta = get_model_param_metadata(param_name);

        /* Verify metadata found */
        TEST_ASSERT(meta != NULL,
                    "Metadata should be found for all required parameters");

        /* Verify metadata contents */
        TEST_ASSERT(meta->name != NULL, "Parameter name should be set");
        TEST_ASSERT_STRING_EQUAL(meta->name, param_name,
                                 "Metadata name should match queried name");
        TEST_ASSERT(meta->type != NULL, "Parameter type should be set");
        TEST_ASSERT(meta->description != NULL, "Parameter description should be set");

        found_count++;
    }

    TEST_ASSERT_EQUAL(found_count, NUM_REQUIRED_MODEL_PARAMETERS,
                      "Should find metadata for all parameters");

    printf("  ✓ Metadata lookup successful for all %d parameters\n", found_count);

    /* Test some specific parameters */
    const struct ModelParameterMetadata *baryon_meta = get_model_param_metadata("BaryonFrac");
    TEST_ASSERT(baryon_meta != NULL, "BaryonFrac metadata should exist");
    TEST_ASSERT_STRING_EQUAL(baryon_meta->type, "double", "BaryonFrac should be double");
    TEST_ASSERT(baryon_meta->has_range == 1, "BaryonFrac should have range");
    TEST_ASSERT_DOUBLE_EQUAL(baryon_meta->range_min, 0.0, 0.001,
                            "BaryonFrac min should be 0.0");
    TEST_ASSERT_DOUBLE_EQUAL(baryon_meta->range_max, 1.0, 0.001,
                            "BaryonFrac max should be 1.0");

    const struct ModelParameterMetadata *agn_meta = get_model_param_metadata("AGNrecipeOn");
    TEST_ASSERT(agn_meta != NULL, "AGNrecipeOn metadata should exist");
    TEST_ASSERT_STRING_EQUAL(agn_meta->type, "int", "AGNrecipeOn should be int");

    printf("  ✓ Specific parameter metadata verified (BaryonFrac, AGNrecipeOn)\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_double_validation
 * @brief   Test validation of double parameters
 *
 * Expected: Valid doubles within range pass validation
 * Validates: validate_model_param_double() accepts valid values
 */
int test_double_validation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL); /* Suppress info logs */

    /* ===== EXECUTE & VALIDATE ===== */

    /* Test BaryonFrac (range: [0.0, 1.0]) */
    int result = validate_model_param_double("BaryonFrac", 0.17);
    TEST_ASSERT_EQUAL(result, 0, "Valid BaryonFrac (0.17) should pass");

    result = validate_model_param_double("BaryonFrac", 0.0);
    TEST_ASSERT_EQUAL(result, 0, "BaryonFrac at min (0.0) should pass");

    result = validate_model_param_double("BaryonFrac", 1.0);
    TEST_ASSERT_EQUAL(result, 0, "BaryonFrac at max (1.0) should pass");

    result = validate_model_param_double("BaryonFrac", 0.5);
    TEST_ASSERT_EQUAL(result, 0, "BaryonFrac mid-range (0.5) should pass");

    printf("  ✓ BaryonFrac validation: min/mid/max values pass\n");

    /* Test SfrEfficiency (range: [0.0, 1.0]) */
    result = validate_model_param_double("SfrEfficiency", 0.02);
    TEST_ASSERT_EQUAL(result, 0, "Valid SfrEfficiency (0.02) should pass");

    /* Test FeedbackReheatingEpsilon (range: [0.0, 100.0]) */
    result = validate_model_param_double("FeedbackReheatingEpsilon", 3.0);
    TEST_ASSERT_EQUAL(result, 0, "Valid FeedbackReheatingEpsilon (3.0) should pass");

    result = validate_model_param_double("FeedbackReheatingEpsilon", 0.0);
    TEST_ASSERT_EQUAL(result, 0, "FeedbackReheatingEpsilon at min (0.0) should pass");

    result = validate_model_param_double("FeedbackReheatingEpsilon", 100.0);
    TEST_ASSERT_EQUAL(result, 0, "FeedbackReheatingEpsilon at max (100.0) should pass");

    printf("  ✓ Multiple double parameter validations successful\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_int_validation
 * @brief   Test validation of integer parameters
 *
 * Expected: Valid ints within range pass validation
 * Validates: validate_model_param_int() accepts valid values
 */
int test_int_validation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE & VALIDATE ===== */

    /* Test AGNrecipeOn (range: [0, 3]) */
    int result = validate_model_param_int("AGNrecipeOn", 0);
    TEST_ASSERT_EQUAL(result, 0, "AGNrecipeOn = 0 (off) should pass");

    result = validate_model_param_int("AGNrecipeOn", 1);
    TEST_ASSERT_EQUAL(result, 0, "AGNrecipeOn = 1 (empirical) should pass");

    result = validate_model_param_int("AGNrecipeOn", 2);
    TEST_ASSERT_EQUAL(result, 0, "AGNrecipeOn = 2 (Bondi-Hoyle) should pass");

    result = validate_model_param_int("AGNrecipeOn", 3);
    TEST_ASSERT_EQUAL(result, 0, "AGNrecipeOn = 3 (cold cloud) should pass");

    printf("  ✓ AGNrecipeOn validation: all valid modes (0-3) pass\n");

    /* Test SupernovaRecipeOn (range: [0, 1]) */
    result = validate_model_param_int("SupernovaRecipeOn", 0);
    TEST_ASSERT_EQUAL(result, 0, "SupernovaRecipeOn = 0 (off) should pass");

    result = validate_model_param_int("SupernovaRecipeOn", 1);
    TEST_ASSERT_EQUAL(result, 0, "SupernovaRecipeOn = 1 (on) should pass");

    /* Test DiskInstabilityOn (range: [0, 1]) */
    result = validate_model_param_int("DiskInstabilityOn", 0);
    TEST_ASSERT_EQUAL(result, 0, "DiskInstabilityOn = 0 should pass");

    result = validate_model_param_int("DiskInstabilityOn", 1);
    TEST_ASSERT_EQUAL(result, 0, "DiskInstabilityOn = 1 should pass");

    printf("  ✓ Multiple int parameter validations successful\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_out_of_range_rejection
 * @brief   Test that out-of-range values are rejected
 *
 * Expected: Values outside defined ranges fail validation
 * Validates: Range checking works correctly
 */
int test_out_of_range_rejection(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE & VALIDATE ===== */

    /* Test BaryonFrac out-of-range (valid: [0.0, 1.0]) */
    int result = validate_model_param_double("BaryonFrac", -0.1);
    TEST_ASSERT(result != 0, "BaryonFrac = -0.1 (below min) should fail");

    result = validate_model_param_double("BaryonFrac", 1.5);
    TEST_ASSERT(result != 0, "BaryonFrac = 1.5 (above max) should fail");

    result = validate_model_param_double("BaryonFrac", 999.0);
    TEST_ASSERT(result != 0, "BaryonFrac = 999.0 (far above max) should fail");

    printf("  ✓ BaryonFrac out-of-range values correctly rejected\n");

    /* Test AGNrecipeOn out-of-range (valid: [0, 3]) */
    result = validate_model_param_int("AGNrecipeOn", -1);
    TEST_ASSERT(result != 0, "AGNrecipeOn = -1 (below min) should fail");

    result = validate_model_param_int("AGNrecipeOn", 4);
    TEST_ASSERT(result != 0, "AGNrecipeOn = 4 (above max) should fail");

    result = validate_model_param_int("AGNrecipeOn", 100);
    TEST_ASSERT(result != 0, "AGNrecipeOn = 100 (far above max) should fail");

    printf("  ✓ AGNrecipeOn out-of-range values correctly rejected\n");

    /* Test FeedbackReheatingEpsilon out-of-range (valid: [0.0, 100.0]) */
    result = validate_model_param_double("FeedbackReheatingEpsilon", -1.0);
    TEST_ASSERT(result != 0, "FeedbackReheatingEpsilon = -1.0 should fail");

    result = validate_model_param_double("FeedbackReheatingEpsilon", 101.0);
    TEST_ASSERT(result != 0, "FeedbackReheatingEpsilon = 101.0 should fail");

    printf("  ✓ Range validation working for multiple parameter types\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_unknown_parameter
 * @brief   Test handling of unknown/nonexistent parameters
 *
 * Expected: Unknown parameters return NULL for metadata, error for validation
 * Validates: Unknown parameter detection works
 */
int test_unknown_parameter(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE & VALIDATE ===== */

    /* Test metadata lookup for nonexistent parameter */
    const struct ModelParameterMetadata *meta = get_model_param_metadata("NonexistentParam");
    TEST_ASSERT(meta == NULL,
                "Metadata lookup for unknown parameter should return NULL");

    meta = get_model_param_metadata("InvalidParameter123");
    TEST_ASSERT(meta == NULL,
                "Metadata lookup for invalid parameter should return NULL");

    printf("  ✓ Unknown parameters return NULL from metadata lookup\n");

    /* Test validation of nonexistent parameter */
    int result = validate_model_param_double("NonexistentParam", 1.0);
    TEST_ASSERT(result != 0,
                "Validation of unknown parameter should fail");

    result = validate_model_param_int("InvalidParameter123", 1);
    TEST_ASSERT(result != 0,
                "Validation of invalid int parameter should fail");

    printf("  ✓ Unknown parameters fail validation with error\n");

    /* Test empty string parameter name */
    meta = get_model_param_metadata("");
    TEST_ASSERT(meta == NULL,
                "Empty parameter name should return NULL");

    printf("  ✓ Edge cases (empty string) handled correctly\n");

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
    printf("Test Suite: Model Parameter Metadata System\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run all test cases */
    TEST_RUN(test_parameter_count);
    TEST_RUN(test_metadata_lookup);
    TEST_RUN(test_double_validation);
    TEST_RUN(test_int_validation);
    TEST_RUN(test_out_of_range_rejection);
    TEST_RUN(test_unknown_parameter);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
