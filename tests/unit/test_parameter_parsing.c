/**
 * @file    test_parameter_parsing.c
 * @brief   Unit tests for parameter file parsing
 *
 * Validates: Configuration file parsing and parameter validation
 * Phase: Phase 2 (Testing Framework)
 *
 * This test validates that Mimic's parameter file parser correctly:
 * - Reads parameter files without errors
 * - Extracts parameter values correctly
 * - Handles comments and whitespace
 * - Handles arrow notation for lists
 * - Populates MimicConfig structure correctly
 *
 * Test cases:
 *   - test_basic_parsing: Parse test_binary.yaml successfully
 *   - test_integer_parameters: Integer parameters read correctly
 *   - test_float_parameters: Float parameters read correctly
 *   - test_string_parameters: String parameters read correctly
 *   - test_cosmology_parameters: Cosmological parameters read correctly
 *
 * @author  Mimic Testing Team
 * @date    2025-11-08
 */

#include "../framework/test_framework.h"
#include "../../src/include/types.h"
#include "../../src/include/proto.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* External config structure */
extern struct MimicConfig MimicConfig;

/**
 * @test    test_basic_parsing
 * @brief   Test that parameter file can be parsed without errors
 *
 * Expected: test_binary.yaml loads successfully
 * Validates: Parser can read and process parameter file
 */
int test_basic_parsing(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL); /* Suppress info logs */

    /* ===== EXECUTE ===== */
    /* Read test parameter file */
    read_parameter_file("tests/data/test_binary.yaml");

    /* ===== VALIDATE ===== */
    /* If we got here without crashing, parsing succeeded */
    printf("  ✓ Parameter file parsed successfully\n");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_integer_parameters
 * @brief   Test that integer parameters are read correctly
 *
 * Expected: FirstFile=0, LastFile=0, NumOutputs=1
 * Validates: Integer parameter parsing
 */
int test_integer_parameters(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE ===== */
    read_parameter_file("tests/data/test_binary.yaml");

    /* ===== VALIDATE ===== */
    TEST_ASSERT(MimicConfig.FirstFile == 0, "FirstFile should be 0");
    TEST_ASSERT(MimicConfig.LastFile == 0, "LastFile should be 0");
    TEST_ASSERT(MimicConfig.NOUT == 1, "NumOutputs should be 1");
    TEST_ASSERT(MimicConfig.LastSnapshotNr == 63, "LastSnapshotNr should be 63");

    printf("  FirstFile: %d\n", MimicConfig.FirstFile);
    printf("  LastFile: %d\n", MimicConfig.LastFile);
    printf("  NumOutputs: %d\n", MimicConfig.NOUT);
    printf("  LastSnapshotNr: %d\n", MimicConfig.LastSnapshotNr);

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_float_parameters
 * @brief   Test that float parameters are read correctly
 *
 * Expected: BoxSize=62.5, Omega=0.25, etc.
 * Validates: Float parameter parsing
 */
int test_float_parameters(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE ===== */
    read_parameter_file("tests/data/test_binary.yaml");

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(MimicConfig.BoxSize, 62.5, 0.01,
                            "BoxSize should be 62.5 Mpc/h");
    TEST_ASSERT_DOUBLE_EQUAL(MimicConfig.PartMass, 0.0860657, 0.000001,
                            "PartMass should match");

    printf("  BoxSize: %.2f Mpc/h\n", MimicConfig.BoxSize);
    printf("  PartMass: %.7f\n", MimicConfig.PartMass);

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_string_parameters
 * @brief   Test that string parameters are read correctly
 *
 * Expected: OutputDir, TreeName, etc. correctly parsed
 * Validates: String parameter parsing
 */
int test_string_parameters(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE ===== */
    read_parameter_file("tests/data/test_binary.yaml");

    /* ===== VALIDATE ===== */
    TEST_ASSERT_STRING_EQUAL(MimicConfig.OutputFileBaseName, "model",
                            "OutputFileBaseName should be 'model'");
    TEST_ASSERT_STRING_EQUAL(MimicConfig.TreeName, "trees_063",
                            "TreeName should be 'trees_063'");

    /* Check that OutputDir contains expected path */
    TEST_ASSERT(strstr(MimicConfig.OutputDir, "test") != NULL,
                "OutputDir should contain 'test'");

    printf("  OutputFileBaseName: %s\n", MimicConfig.OutputFileBaseName);
    printf("  TreeName: %s\n", MimicConfig.TreeName);
    printf("  OutputDir: %s\n", MimicConfig.OutputDir);
    printf("  SimulationDir: %s\n", MimicConfig.SimulationDir);

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_cosmology_parameters
 * @brief   Test that cosmological parameters are read correctly
 *
 * Expected: Omega=0.25, OmegaLambda=0.75, Hubble_h=0.73
 * Validates: Cosmological parameter parsing
 */
int test_cosmology_parameters(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE ===== */
    read_parameter_file("tests/data/test_binary.yaml");

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(MimicConfig.Omega, 0.25, 0.001,
                            "Omega should be 0.25");
    TEST_ASSERT_DOUBLE_EQUAL(MimicConfig.OmegaLambda, 0.75, 0.001,
                            "OmegaLambda should be 0.75");
    TEST_ASSERT_DOUBLE_EQUAL(MimicConfig.Hubble_h, 0.73, 0.001,
                            "Hubble_h should be 0.73");

    printf("  Omega: %.3f\n", MimicConfig.Omega);
    printf("  OmegaLambda: %.3f\n", MimicConfig.OmegaLambda);
    printf("  Hubble_h: %.3f\n", MimicConfig.Hubble_h);

    /* Sanity check: Omega + OmegaLambda should be ~1.0 for flat universe */
    double omega_total = MimicConfig.Omega + MimicConfig.OmegaLambda;
    TEST_ASSERT_DOUBLE_EQUAL(omega_total, 1.0, 0.01,
                            "Omega + OmegaLambda should be ~1.0 (flat universe)");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_snapshot_list
 * @brief   Test that snapshot list is parsed correctly
 *
 * Expected: Snapshot 63 in output list
 * Validates: Arrow notation parsing
 */
int test_snapshot_list(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* ===== EXECUTE ===== */
    read_parameter_file("tests/data/test_binary.yaml");

    /* ===== VALIDATE ===== */
    TEST_ASSERT(MimicConfig.NOUT == 1, "Should have 1 output snapshot");
    TEST_ASSERT(MimicConfig.ListOutputSnaps[0] == 63,
                "First output snapshot should be 63");

    printf("  Number of output snapshots: %d\n", MimicConfig.NOUT);
    printf("  Output snapshot list:");
    for (int i = 0; i < MimicConfig.NOUT; i++) {
        printf(" %d", MimicConfig.ListOutputSnaps[i]);
    }
    printf("\n");

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
    printf("Test Suite: Parameter Parsing\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run all test cases */
    TEST_RUN(test_basic_parsing);
    TEST_RUN(test_integer_parameters);
    TEST_RUN(test_float_parameters);
    TEST_RUN(test_string_parameters);
    TEST_RUN(test_cosmology_parameters);
    TEST_RUN(test_snapshot_list);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
