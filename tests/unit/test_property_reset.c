/**
 * @file    test_property_reset.c
 * @brief   Unit tests for init_repeat property reset behavior
 *
 * Validates: Properties with init_repeat: true reset each snapshot
 * Phase: Property Reset Implementation
 *
 * This test validates that Mimic generates and compiles snapshot-scoped
 * property reset code from the selected model metadata. Hand-written core
 * tests must not name model-owned galaxy fields directly.
 *
 * Test cases:
 *   - test_generated_metadata_available: Verify generated helper metadata exists
 *   - test_generated_init_code_executes: Verify generated init code resets all default properties
 *   - test_generated_reset_code_executes: Verify init_repeat fields reset to init_value
 *
 * @author  Mimic Testing Team
 * @date    2025-12-03
 */

#include "../framework/test_framework.h"
#include "../../src/include/globals.h"
#include "../../src/include/types.h"
#include "../../src/include/generated/property_test_helpers.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static void setup_workspace(struct Halo *workspace, struct GalaxyData *galaxy) {
    memset(workspace, 0, sizeof(struct Halo));
    memset(galaxy, 0, sizeof(struct GalaxyData));
    FoFWorkspace = workspace;
    FoFWorkspace[0].galaxy = galaxy;
}

int test_generated_metadata_available(void) {
    init_memory_system(0);

    TEST_ASSERT(GENERATED_GALAXY_PROPERTY_COUNT > 0,
                "Selected model should have generated galaxy properties");
    TEST_ASSERT(GENERATED_DEFAULT_GALAXY_PROPERTY_COUNT > 0,
                "Selected model should have generated default-initialized galaxy properties");
    TEST_ASSERT(GENERATED_INIT_REPEAT_PROPERTY_COUNT >= 0,
                "Generated init_repeat property count should be available");

    printf("  generated galaxy properties: %d\n", GENERATED_GALAXY_PROPERTY_COUNT);
    printf("  generated default-initialized galaxy properties: %d\n",
           GENERATED_DEFAULT_GALAXY_PROPERTY_COUNT);
    printf("  generated init_repeat properties: %d\n", GENERATED_INIT_REPEAT_PROPERTY_COUNT);

    check_memory_leaks();
    return TEST_PASS;
}

int test_generated_init_code_executes(void) {
    init_memory_system(0);

    struct Halo workspace[1];
    struct GalaxyData galaxy;
    setup_workspace(workspace, &galaxy);

    generated_test_seed_default_galaxy_properties(&galaxy);
    TEST_ASSERT(!generated_test_default_galaxy_properties_equal_init(&galaxy),
                "Generated seed helper should move default galaxy properties away from init values");

    init_galaxy_defaults(&galaxy);

    TEST_ASSERT(generated_test_default_galaxy_properties_equal_init(&galaxy),
                "init_galaxy_defaults() should initialize all default galaxy properties");

    check_memory_leaks();
    return TEST_PASS;
}

int test_generated_reset_code_executes(void) {
    init_memory_system(0);

    struct Halo workspace[1];
    struct GalaxyData galaxy;
    setup_workspace(workspace, &galaxy);

    init_galaxy_defaults(&galaxy);

    generated_test_seed_init_repeat_properties(&galaxy);
    if (GENERATED_INIT_REPEAT_PROPERTY_COUNT > 0) {
        TEST_ASSERT(!generated_test_init_repeat_properties_equal_init(&galaxy),
                    "Generated seed helper should move init_repeat properties away from init values");
    }

    reset_galaxy_snapshot_accumulators(&galaxy);

    TEST_ASSERT(generated_test_init_repeat_properties_equal_init(&galaxy),
                "reset_galaxy_snapshot_accumulators() should restore init_repeat properties to init values");

    check_memory_leaks();
    return TEST_PASS;
}

/* ========================================================================== */
/* TEST SUITE MAIN                                                            */
/* ========================================================================== */

/**
 * @brief   Main test runner
 *
 * Executes all test cases and reports results.
 */
int main(void) {
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: Property Reset System\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all tests */
    TEST_RUN(test_generated_metadata_available);
    TEST_RUN(test_generated_init_code_executes);
    TEST_RUN(test_generated_reset_code_executes);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
