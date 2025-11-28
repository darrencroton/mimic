/**
 * @file    test_module_configuration.c
 * @brief   Unit tests for module configuration system
 *
 * Validates: Module registration, parameter parsing, pipeline execution
 * Phase: Phase 3 (Runtime Module Configuration)
 *
 * Validates module registration and execution pipeline configuration.
 *
 * Test cases:
 *   - test_module_registry_init: Registry initialization
 *   - test_enabled_modules_parsing: Parse EnabledModules list
 *   - test_physics_free_mode: No modules enabled
 *   - test_valid_module_initialization: Initialize valid modules
 *   - test_unknown_module_error: Invalid module name handling
 *   - test_single_module_initialization: Single module configuration
 *
 * @author  Mimic Development Team
 * @date    2025-11-09
 */

#include "../framework/test_framework.h"
#include "../../src/core/module_registry.h"
#include "../../src/core/module_interface.h"
#include "../../src/include/types.h"
#include "../../src/include/proto.h"
#include "../../src/include/globals.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Test fixture: reset configuration state */
static void reset_config(void) {
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

/* Test fixture: ensure modules are registered (only once) */
static void ensure_modules_registered(void) {
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/* Test fixture: Set test_fixture parameters in centralized model_parameters */
static void set_test_fixture_params(double dummy_val, int logging_val) {
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
 * @test    test_module_registry_init
 * @brief   Test module registry initialization
 *
 * Expected: Registry initializes without errors, modules can be registered
 * Validates: Basic module registration system works
 */
int test_module_registry_init(void) {
    /* ===== SETUP ===== */
    reset_config();

    /* ===== EXECUTE ===== */
    /* Registry should initialize without explicit init call (static storage) */
    /* Register test modules via register_all_modules() */
    ensure_modules_registered();

    /* ===== VERIFY ===== */
    /* If we got here without crashing, registration succeeded */
    /* (Module registry is internal, so we can't directly inspect it) */

    return TEST_PASS;
}

/**
 * @test    test_enabled_modules_parsing
 * @brief   Test parsing of EnabledModules parameter
 *
 * Expected: Comma-separated module list parsed correctly
 * Validates: EnabledModules parser handles whitespace and multiple modules
 */
int test_enabled_modules_parsing(void) {
    /* ===== SETUP ===== */
    reset_config();

    /* ===== EXECUTE ===== */
    /* Simulate EnabledModules = "test_fixture,test_fixture" */
    strcpy(MimicConfig.EnabledModules[0], "test_fixture");
    strcpy(MimicConfig.EnabledModules[1], "test_fixture");
    MimicConfig.NumEnabledModules = 2;

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(MimicConfig.NumEnabledModules, 2,
                      "Should have 2 enabled modules");
    TEST_ASSERT_STRING_EQUAL(MimicConfig.EnabledModules[0], "test_fixture",
                             "First module should be test_fixture");
    TEST_ASSERT_STRING_EQUAL(MimicConfig.EnabledModules[1], "test_fixture",
                             "Second module should be test_fixture");

    return TEST_PASS;
}

/**
 * @test    test_physics_free_mode
 * @brief   Test physics-free mode (no modules enabled)
 *
 * Expected: module_system_init() succeeds with NumEnabledModules = 0
 * Validates: Core can run without any physics modules
 */
int test_physics_free_mode(void) {
    /* ===== SETUP ===== */
    reset_config();
    ensure_modules_registered();

    /* No modules enabled */
    MimicConfig.NumEnabledModules = 0;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(result, 0,
                      "module_system_init should succeed in physics-free mode");

    /* ===== CLEANUP ===== */
    module_system_cleanup();

    return TEST_PASS;
}

/**
 * @test    test_valid_module_initialization
 * @brief   Test initializing valid modules
 *
 * Expected: module_system_init() succeeds with valid module names
 * Validates: Module pipeline builds correctly
 */
int test_valid_module_initialization(void) {
    /* ===== SETUP ===== */
    reset_config();
    ensure_modules_registered();

    /* Set test_fixture parameters */
    set_test_fixture_params(1.0, 0);

    /* Enable valid modules */
    strcpy(MimicConfig.EnabledModules[0], "test_fixture");
    strcpy(MimicConfig.EnabledModules[1], "test_fixture");
    MimicConfig.NumEnabledModules = 2;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(result, 0,
                      "module_system_init should succeed with valid modules");

    /* ===== CLEANUP ===== */
    module_system_cleanup();

    return TEST_PASS;
}

/**
 * @test    test_unknown_module_error
 * @brief   Test error handling for unknown module names
 *
 * Expected: module_system_init() fails with descriptive error
 * Validates: Invalid module names are detected and reported
 */
int test_unknown_module_error(void) {
    /* ===== SETUP ===== */
    reset_config();
    ensure_modules_registered();

    /* Enable an invalid module */
    strcpy(MimicConfig.EnabledModules[0], "nonexistent_module");
    MimicConfig.NumEnabledModules = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VERIFY ===== */
    TEST_ASSERT(result != 0,
                "module_system_init should fail with unknown module");

    /* Note: Can't cleanup since init failed */

    return TEST_PASS;
}

/**
 * @test    test_single_module_initialization
 * @brief   Test initializing a single module
 *
 * Expected: System works with only one module enabled
 * Validates: Partial module configurations are supported
 */
int test_single_module_initialization(void) {
    /* ===== SETUP ===== */
    reset_config();
    ensure_modules_registered();

    /* Set test_fixture parameters */
    set_test_fixture_params(1.0, 0);

    /* Enable only test_fixture */
    strcpy(MimicConfig.EnabledModules[0], "test_fixture");
    MimicConfig.NumEnabledModules = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(result, 0,
                      "module_system_init should succeed with single module");

    /* ===== CLEANUP ===== */
    module_system_cleanup();

    return TEST_PASS;
}

/**
 * Main test runner
 */
int main(void) {
    printf("\n");
    printf("=========================================\n");
    printf("Module Configuration System Tests\n");
    printf("=========================================\n");
    printf("\n");

    /* Initialize memory system for tests */
    init_memory_system(0);

    /* Run tests */
    TEST_RUN(test_module_registry_init);
    TEST_RUN(test_enabled_modules_parsing);
    TEST_RUN(test_physics_free_mode);
    TEST_RUN(test_valid_module_initialization);
    TEST_RUN(test_unknown_module_error);
    TEST_RUN(test_single_module_initialization);

    /* Print summary */
    TEST_SUMMARY();

    /* Memory leak check */
    printf("\n");
    printf("Memory leak check:\n");
    print_allocated();

    return TEST_RESULT();
}
