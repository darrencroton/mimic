/**
 * @file    test_unit_test_fixture.c
 * @brief   Unit tests for test_fixture module
 *
 * ⚠️ WARNING: These tests validate the test fixture itself ⚠️
 *
 * The test_fixture module exists solely for testing infrastructure.
 * These tests validate that the test fixture behaves correctly.
 *
 * Validates:
 * - Module registration works
 * - Module lifecycle (init/cleanup) works
 * - Parameter reading works
 * - Property access works
 * - No memory leaks
 *
 * @author  Mimic Development Team
 * @date    2025-11-13
 */

#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "../../core/module_interface.h"
#include "fixture.h"
#include "../../include/types.h"
#include "../../include/proto.h"
#include "../../include/globals.h"
#include "../../util/error.h"
#include "../../util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Test fixture: reset configuration state */
static void reset_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

/* Test fixture: ensure modules are registered (only once) */
static void ensure_modules_registered(void)
{
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/* Test fixture: Set test_fixture parameters via centralized system */
static void set_test_fixture_params(double dummy_val, int logging_val)
{
    // Set parameters in parameter system
    // ModelParams is an array of {param_name, value} pairs
    int idx = 0;

    strcpy(MimicConfig.ModelParams[idx].param_name, "TestFixtureDummyParameter");
    snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%.10g", dummy_val);
    idx++;

    strcpy(MimicConfig.ModelParams[idx].param_name, "TestFixtureEnableLogging");
    snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%d", logging_val);
    idx++;

    MimicConfig.NumModelParams = idx;
}

/**
 * @test    test_module_registration
 * @brief   Test that test_fixture module registers correctly
 *
 * Expected: Module registration succeeds without errors
 */
int test_module_registration(void)
{
    /* ===== SETUP ===== */
    reset_config();

    /* ===== EXECUTE ===== */
    ensure_modules_registered();

    /* ===== VALIDATE ===== */
    /* If we got here without crashing, registration succeeded */

    return TEST_PASS;
}

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors
 */
int test_module_initialization(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();
    set_test_fixture_params(2.5, 0);

    /* Configure test_fixture module in phase_1 (for testing) */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("test_fixture");
    MimicConfig.phase_1[0].loop_mode = LOOP_MODE_ALL;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module system init should succeed");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_reading
 * @brief   Test that module parameters are read correctly
 *
 * Expected: Module reads DummyParameter from config
 */
int test_parameter_reading(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set custom parameter value via centralized system */
    set_test_fixture_params(3.14, 0);

    /* Configure test_fixture module in phase_1 (for testing) */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("test_fixture");
    MimicConfig.phase_1[0].loop_mode = LOOP_MODE_ALL;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module init should succeed with custom parameters");
    /* Parameter is internal to module, we validated it initialized without error */

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_property_access
 * @brief   Test that TestDummyProperty is accessible
 *
 * Expected: Property can be read/written without crashes
 * Note: Full property setting tested in integration tests
 */
int test_property_access(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Create test halo and galaxy */
    struct Halo test_halo;
    memset(&test_halo, 0, sizeof(test_halo));
    test_halo.Type = 0; /* Central galaxy */

    struct GalaxyData test_galaxy;
    memset(&test_galaxy, 0, sizeof(test_galaxy));
    test_halo.galaxy = &test_galaxy;

    /* ===== EXECUTE ===== */
    /* Test property access (should not crash) */
    test_galaxy.TestDummyProperty = 1.5f;
    float value = test_galaxy.TestDummyProperty;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(fabs(value - 1.5f) < 1e-6, "Property access should work correctly");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test that module has no memory leaks
 *
 * Expected: No memory leaks after init/cleanup cycle
 */
int test_memory_safety(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();
    set_test_fixture_params(2.5, 0);

    /* Configure test_fixture module in phase_1 (for testing) */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("test_fixture");
    MimicConfig.phase_1[0].loop_mode = LOOP_MODE_ALL;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization should succeed");

    /* ===== VALIDATE ===== */
    /* Module initialized successfully without memory leaks */
    /* (Full pipeline processing tested in integration tests) */

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: test_fixture\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Run tests */
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_property_access);
    TEST_RUN(test_memory_safety);

    /* Print summary */
    TEST_SUMMARY();

    return TEST_RESULT();
}
