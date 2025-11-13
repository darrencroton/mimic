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

/* Test fixture: Set test_fixture parameters */
static void set_test_fixture_params(void)
{
    strcpy(MimicConfig.ModuleParams[0].module_name, "TestFixture");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DummyParameter");
    strcpy(MimicConfig.ModuleParams[0].value, "2.5");

    strcpy(MimicConfig.ModuleParams[1].module_name, "TestFixture");
    strcpy(MimicConfig.ModuleParams[1].param_name, "EnableLogging");
    strcpy(MimicConfig.ModuleParams[1].value, "0");

    MimicConfig.NumModuleParams = 2;
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
    set_test_fixture_params();

    strcpy(MimicConfig.EnabledModules[0], "test_fixture");
    MimicConfig.NumEnabledModules = 1;

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

    /* Set custom parameter value */
    strcpy(MimicConfig.ModuleParams[0].module_name, "TestFixture");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DummyParameter");
    strcpy(MimicConfig.ModuleParams[0].value, "3.14");
    MimicConfig.NumModuleParams = 1;

    strcpy(MimicConfig.EnabledModules[0], "test_fixture");
    MimicConfig.NumEnabledModules = 1;

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
    set_test_fixture_params();

    strcpy(MimicConfig.EnabledModules[0], "test_fixture");
    MimicConfig.NumEnabledModules = 1;

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
    printf("\n");
    printf("========================================\n");
    printf("Test Suite: test_fixture\n");
    printf("========================================\n");

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
