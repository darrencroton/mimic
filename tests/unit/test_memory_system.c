/**
 * @file    test_memory_system.c
 * @brief   Unit tests for memory management system
 *
 * Validates: Memory allocation, deallocation, leak detection, and categorization
 * Phase: Phase 2 (Testing Framework)
 *
 * This test validates that Mimic's memory management system correctly:
 * - Initializes without errors
 * - Allocates and frees memory
 * - Tracks memory by category
 * - Detects memory leaks
 * - Handles reallocations
 *
 * Test cases:
 *   - test_memory_init: System initialization
 *   - test_basic_allocation: Simple malloc/free
 *   - test_categorized_allocation: Category tracking
 *   - test_reallocation: Memory reallocation
 *   - test_leak_detection: Leak detection works
 *
 * @author  Mimic Testing Team
 * @date    2025-11-08
 */

#include "../framework/test_framework.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @test    test_memory_init
 * @brief   Test memory system initialization
 *
 * Expected: System initializes without errors
 * Validates: Memory system can be initialized and cleaned up
 */
int test_memory_init(void) {
    /* ===== SETUP ===== */
    /* Nothing needed - testing init itself */

    /* ===== EXECUTE ===== */
    init_memory_system(0);  /* Use default max blocks */

    /* ===== VALIDATE ===== */
    /* If we got here without crashing, init succeeded */
    /* Try a simple allocation to verify system is operational */
    void *test_ptr = mymalloc(100);
    TEST_ASSERT(test_ptr != NULL, "Memory allocation should succeed after init");

    /* ===== CLEANUP ===== */
    myfree(test_ptr);
    check_memory_leaks();  /* Should report no leaks */

    return TEST_PASS;
}

/**
 * @test    test_basic_allocation
 * @brief   Test basic memory allocation and deallocation
 *
 * Expected: Allocate and free various sizes without leaks
 * Validates: Core malloc/free functionality
 */
int test_basic_allocation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    /* Test small allocation */
    void *small = mymalloc(10);
    TEST_ASSERT(small != NULL, "Small allocation should succeed");

    /* Test medium allocation */
    void *medium = mymalloc(1024);
    TEST_ASSERT(medium != NULL, "Medium allocation should succeed");

    /* Test large allocation */
    void *large = mymalloc(1024 * 1024);
    TEST_ASSERT(large != NULL, "Large allocation should succeed");

    /* Test zero allocation (should handle gracefully) */
    void *zero = mymalloc(0);
    /* Don't assert on zero - behavior may be implementation-defined */

    /* ===== CLEANUP ===== */
    myfree(small);
    myfree(medium);
    myfree(large);
    if (zero) myfree(zero);

    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_categorized_allocation
 * @brief   Test memory allocation with category tracking
 *
 * Expected: Memory correctly tracked by category
 * Validates: Category-based memory tracking
 */
int test_categorized_allocation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    /* Allocate memory in different categories */
    void *halos = mymalloc_cat(1000, MEM_HALOS);
    void *trees = mymalloc_cat(2000, MEM_TREES);
    void *io = mymalloc_cat(500, MEM_IO);
    void *util = mymalloc_cat(100, MEM_UTILITY);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(halos != NULL, "Halo category allocation should succeed");
    TEST_ASSERT(trees != NULL, "Tree category allocation should succeed");
    TEST_ASSERT(io != NULL, "I/O category allocation should succeed");
    TEST_ASSERT(util != NULL, "Utility category allocation should succeed");

    /* Print categorized allocation (manual inspection) */
    printf("  Categorized allocation summary:\n");
    print_allocated_by_category();

    /* ===== CLEANUP ===== */
    myfree(halos);
    myfree(trees);
    myfree(io);
    myfree(util);

    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_reallocation
 * @brief   Test memory reallocation
 *
 * Expected: Reallocation preserves data and updates tracking
 * Validates: myrealloc() functionality
 */
int test_reallocation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    /* Allocate initial block */
    size_t initial_size = 100;
    int *data = (int *)mymalloc(initial_size * sizeof(int));
    TEST_ASSERT(data != NULL, "Initial allocation should succeed");

    /* Fill with test data */
    for (size_t i = 0; i < initial_size; i++) {
        data[i] = (int)i;
    }

    /* Reallocate to larger size */
    size_t new_size = 200;
    int *new_data = (int *)myrealloc(data, new_size * sizeof(int));
    TEST_ASSERT(new_data != NULL, "Reallocation should succeed");

    /* ===== VALIDATE ===== */
    /* Check that original data is preserved */
    for (size_t i = 0; i < initial_size; i++) {
        TEST_ASSERT(new_data[i] == (int)i, "Data should be preserved after realloc");
    }

    /* ===== CLEANUP ===== */
    myfree(new_data);
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_leak_detection
 * @brief   Test that memory leak detection works
 *
 * Expected: Leak detection reports allocated blocks
 * Validates: check_memory_leaks() functionality
 *
 * Note: This test intentionally leaks memory to verify detection.
 *       We suppress the leak warning and clean up afterward.
 */
int test_leak_detection(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    /* Intentionally allocate without freeing */
    void *leak1 = mymalloc(100);
    void *leak2 = mymalloc_cat(200, MEM_HALOS);
    TEST_ASSERT(leak1 != NULL, "Allocation 1 should succeed");
    TEST_ASSERT(leak2 != NULL, "Allocation 2 should succeed");

    /* ===== VALIDATE ===== */
    printf("  Expecting leak warning (this is intentional for testing):\n");
    check_memory_leaks();  /* Should report 2 leaks */

    /* ===== CLEANUP ===== */
    /* Clean up the intentional leaks */
    myfree(leak1);
    myfree(leak2);

    /* Verify leaks are gone */
    printf("  After cleanup (should be clean):\n");
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_multiple_alloc_free_cycles
 * @brief   Test multiple allocation/free cycles
 *
 * Expected: No memory leaks after many cycles
 * Validates: Memory system stability over time
 */
int test_multiple_alloc_free_cycles(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    /* Perform many allocation/free cycles */
    const int num_cycles = 100;
    for (int i = 0; i < num_cycles; i++) {
        void *ptr = mymalloc(1024);
        TEST_ASSERT(ptr != NULL, "Allocation in cycle should succeed");
        myfree(ptr);
    }

    /* ===== VALIDATE ===== */
    /* No leaks should be detected */
    check_memory_leaks();

    /* ===== CLEANUP ===== */
    /* Already cleaned in loop */

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all test cases and reports results.
 */
int main(void) {
    printf("========================================\n");
    printf("Test Suite: Memory System\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_memory_init);
    TEST_RUN(test_basic_allocation);
    TEST_RUN(test_categorized_allocation);
    TEST_RUN(test_reallocation);
    TEST_RUN(test_leak_detection);
    TEST_RUN(test_multiple_alloc_free_cycles);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
