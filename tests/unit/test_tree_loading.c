/**
 * @file    test_tree_loading.c
 * @brief   Unit tests for merger tree loading
 *
 * Validates: Tree file reading and basic data structure integrity
 * Phase: Phase 2 (Testing Framework)
 *
 * This test validates that Mimic's tree loading system correctly:
 * - Loads tree table from file
 * - Reads tree data without errors
 * - Populates InputTreeHalos array
 * - Handles binary format correctly
 * - Validates tree structure integrity
 *
 * Test cases:
 *   - test_tree_table_loading: Load tree table successfully
 *   - test_tree_data_loading: Load tree data successfully
 *   - test_tree_halo_count: Verify halo count > 0
 *   - test_tree_data_validity: Check for NaN/Inf in loaded data
 *   - test_tree_cleanup: Verify proper cleanup
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
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* External globals */
extern struct MimicConfig MimicConfig;
extern struct RawHalo *InputTreeHalos;
extern int *InputTreeNHalos;
extern int Ntrees;

/**
 * @test    test_tree_table_loading
 * @brief   Test that tree table can be loaded from file
 *
 * Expected: Tree table loads successfully, Ntrees > 0
 * Validates: Tree table reading functionality
 */
int test_tree_table_loading(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Load parameter file and initialize (mirrors main.c flow) */
    read_parameter_file("tests/data/test_binary.par");
    init();  /* Initializes units, reads snapshot list, populates ZZ[] array */

    /* ===== EXECUTE ===== */
    /* Load tree table for file 0 */
    int filenr = 0;
    load_tree_table(filenr, lhalo_binary);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(Ntrees > 0, "Should have at least one tree");

    printf("  Number of trees in file: %d\n", Ntrees);
    if (InputTreeNHalos != NULL && Ntrees > 0) {
        printf("  First tree has %d halos\n", InputTreeNHalos[0]);
    }

    /* ===== CLEANUP ===== */
    free_tree_table(lhalo_binary);
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_tree_data_loading
 * @brief   Test that tree data can be loaded
 *
 * Expected: Tree loads successfully, InputTreeHalos populated
 * Validates: Tree data reading functionality
 */
int test_tree_data_loading(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Load parameter file, initialize, and load tree table */
    read_parameter_file("tests/data/test_binary.par");
    init();
    load_tree_table(0, lhalo_binary);

    /* ===== EXECUTE ===== */
    /* Load first tree */
    TEST_ASSERT(Ntrees > 0, "Need at least one tree for this test");
    load_tree(0, lhalo_binary);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(InputTreeHalos != NULL, "InputTreeHalos should be allocated");
    TEST_ASSERT(InputTreeNHalos[0] > 0, "First tree should have halos");

    printf("  Tree 0 loaded successfully\n");
    printf("  Number of halos: %d\n", InputTreeNHalos[0]);

    /* ===== CLEANUP ===== */
    free_halos_and_tree();
    free_tree_table(lhalo_binary);
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_tree_halo_count
 * @brief   Test that loaded tree has reasonable halo count
 *
 * Expected: Halo count > 0 and < some reasonable maximum
 * Validates: Tree data integrity
 */
int test_tree_halo_count(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    read_parameter_file("tests/data/test_binary.par");
    init();
    load_tree_table(0, lhalo_binary);

    /* ===== EXECUTE ===== */
    load_tree(0, lhalo_binary);

    /* ===== VALIDATE ===== */
    int nhalo = InputTreeNHalos[0];
    TEST_ASSERT(nhalo > 0, "Tree should have at least one halo");
    TEST_ASSERT(nhalo < 1000000, "Halo count should be reasonable (< 1M)");

    printf("  Halo count: %d (reasonable)\n", nhalo);

    /* ===== CLEANUP ===== */
    free_halos_and_tree();
    free_tree_table(lhalo_binary);
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_tree_data_validity
 * @brief   Test that loaded halo data contains valid values
 *
 * Expected: No NaN, no Inf, physical quantities reasonable
 * Validates: Data integrity and format correctness
 */
int test_tree_data_validity(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    read_parameter_file("tests/data/test_binary.par");
    init();
    load_tree_table(0, lhalo_binary);
    load_tree(0, lhalo_binary);

    /* ===== EXECUTE & VALIDATE ===== */
    int nhalo = InputTreeNHalos[0];

    /* Check first 10 halos (or all if fewer) */
    int check_count = nhalo < 10 ? nhalo : 10;
    printf("  Checking first %d halos for validity...\n", check_count);

    for (int i = 0; i < check_count; i++) {
        struct RawHalo *h = &InputTreeHalos[i];

        /* Check for NaN/Inf in critical fields */
        TEST_ASSERT(isfinite(h->Mvir), "Mvir should be finite");
        TEST_ASSERT(isfinite(h->Vmax), "Vmax should be finite");
        TEST_ASSERT(isfinite(h->Pos[0]), "Pos[0] should be finite");
        TEST_ASSERT(isfinite(h->Pos[1]), "Pos[1] should be finite");
        TEST_ASSERT(isfinite(h->Pos[2]), "Pos[2] should be finite");

        /* Check that masses are positive */
        TEST_ASSERT(h->Mvir > 0, "Mvir should be positive");
        TEST_ASSERT(h->Len > 0, "Len should be positive");

        /* Check snapshot number is reasonable */
        TEST_ASSERT(h->SnapNum >= 0, "SnapNum should be non-negative");
        TEST_ASSERT(h->SnapNum <= 63, "SnapNum should be <= 63 for mini-Millennium");
    }

    printf("  ✓ All checked halos have valid data\n");

    /* Print sample halo data */
    if (nhalo > 0) {
        struct RawHalo *h = &InputTreeHalos[0];
        printf("  Sample halo [0]:\n");
        printf("    SnapNum: %d\n", h->SnapNum);
        printf("    Mvir: %.2e\n", h->Mvir);
        printf("    Vmax: %.2f\n", h->Vmax);
        printf("    Len: %d\n", h->Len);
    }

    /* ===== CLEANUP ===== */
    free_halos_and_tree();
    free_tree_table(lhalo_binary);
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_tree_cleanup
 * @brief   Test that tree cleanup properly frees memory
 *
 * Expected: All tree memory freed, no leaks
 * Validates: Memory management in tree loading
 */
int test_tree_cleanup(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    read_parameter_file("tests/data/test_binary.par");
    init();
    load_tree_table(0, lhalo_binary);
    load_tree(0, lhalo_binary);

    /* ===== EXECUTE ===== */
    /* Cleanup tree data */
    free_halos_and_tree();
    free_tree_table(lhalo_binary);

    /* ===== VALIDATE ===== */
    /* Check for memory leaks */
    check_memory_leaks();

    printf("  ✓ Tree cleanup successful, no memory leaks\n");

    /* ===== CLEANUP ===== */
    /* Already cleaned up above */

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all test cases and reports results.
 */
int main(void) {
    printf("========================================\n");
    printf("Test Suite: Tree Loading\n");
    printf("========================================\n\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run all test cases */
    TEST_RUN(test_tree_table_loading);
    TEST_RUN(test_tree_data_loading);
    TEST_RUN(test_tree_halo_count);
    TEST_RUN(test_tree_data_validity);
    TEST_RUN(test_tree_cleanup);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
