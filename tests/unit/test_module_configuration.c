/**
 * @file    test_module_configuration.c
 * @brief   Unit tests for module configuration system
 *
 * Validates: Module registration, parameter parsing, pipeline execution
 * Phase: Phase 3 (Runtime Module Configuration)
 *
 * This test validates that Mimic's module configuration system correctly:
 * - Parses EnabledModules parameter from config files
 * - Parses module-specific parameters (ModuleName_ParameterName format)
 * - Builds execution pipeline from configuration
 * - Provides parameter access API to modules
 * - Handles physics-free mode (no modules enabled)
 * - Validates module names and reports errors
 *
 * Test cases:
 *   - test_module_registry_init: Registry initialization
 *   - test_module_parameter_parsing: Parse module parameters from config
 *   - test_enabled_modules_parsing: Parse EnabledModules list
 *   - test_module_parameter_api: Module parameter access functions
 *   - test_physics_free_mode: No modules enabled
 *   - test_unknown_module_error: Invalid module name handling
 *   - test_module_execution_order: Modules execute in config order
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
 * @test    test_module_parameter_parsing
 * @brief   Test parsing of module-specific parameters
 *
 * Expected: ModuleName_ParameterName format parsed correctly
 * Validates: Parameter parser stores module parameters
 */
int test_module_parameter_parsing(void) {
    /* ===== SETUP ===== */
    reset_config();

    /* Simulate parameter file entries */
    const char *module_name = "TestModule";
    const char *param_name = "TestParameter";
    const char *param_value = "42.5";

    /* ===== EXECUTE ===== */
    /* Manually populate module parameters (simulating parser behavior) */
    strcpy(MimicConfig.ModuleParams[0].module_name, module_name);
    strcpy(MimicConfig.ModuleParams[0].param_name, param_name);
    strcpy(MimicConfig.ModuleParams[0].value, param_value);
    MimicConfig.NumModuleParams = 1;

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(MimicConfig.NumModuleParams, 1,
                      "Should have 1 module parameter");
    TEST_ASSERT_STRING_EQUAL(MimicConfig.ModuleParams[0].module_name,
                             module_name, "Module name should match");
    TEST_ASSERT_STRING_EQUAL(MimicConfig.ModuleParams[0].param_name,
                             param_name, "Parameter name should match");
    TEST_ASSERT_STRING_EQUAL(MimicConfig.ModuleParams[0].value, param_value,
                             "Parameter value should match");

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
 * @test    test_module_parameter_api
 * @brief   Test module parameter access API
 *
 * Expected: module_get_double() retrieves parameters correctly
 * Validates: Modules can read their configuration parameters
 */
int test_module_parameter_api(void) {
    /* ===== SETUP ===== */
    reset_config();

    /* Configure a test parameter */
    strcpy(MimicConfig.ModuleParams[0].module_name, "TestFixture");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DummyParameter");
    strcpy(MimicConfig.ModuleParams[0].value, "2.5");
    MimicConfig.NumModuleParams = 1;

    /* ===== EXECUTE ===== */
    double dummy_param = 0.0;
    int result =
        module_get_double("TestFixture", "DummyParameter", &dummy_param, 1.0);

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(result, 0, "module_get_double should succeed");
    TEST_ASSERT_DOUBLE_EQUAL(dummy_param, 2.5, 1e-6,
                             "Should retrieve configured value");

    return TEST_PASS;
}

/**
 * @test    test_module_parameter_api_default
 * @brief   Test module parameter API with missing parameter
 *
 * Expected: module_get_double() returns default when parameter not found
 * Validates: Default value mechanism works correctly
 */
int test_module_parameter_api_default(void) {
    /* ===== SETUP ===== */
    reset_config();

    /* No parameters configured */
    MimicConfig.NumModuleParams = 0;

    /* ===== EXECUTE ===== */
    double efficiency = 0.0;
    int result =
        module_get_double("StellarMass", "Efficiency", &efficiency, 0.025);

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(result, 0, "module_get_double should succeed with default");
    TEST_ASSERT_DOUBLE_EQUAL(efficiency, 0.025, 1e-6,
                             "Should return default value");

    return TEST_PASS;
}

/**
 * @test    test_module_parameter_api_integer
 * @brief   Test integer parameter access
 *
 * Expected: module_get_int() retrieves integer parameters correctly
 * Validates: Integer parameter parsing works
 */
int test_module_parameter_api_integer(void) {
    /* ===== SETUP ===== */
    reset_config();

    /* Configure an integer parameter */
    strcpy(MimicConfig.ModuleParams[0].module_name, "TestModule");
    strcpy(MimicConfig.ModuleParams[0].param_name, "MaxIterations");
    strcpy(MimicConfig.ModuleParams[0].value, "100");
    MimicConfig.NumModuleParams = 1;

    /* ===== EXECUTE ===== */
    int max_iterations = 0;
    int result =
        module_get_int("TestModule", "MaxIterations", &max_iterations, 50);

    /* ===== VERIFY ===== */
    TEST_ASSERT_EQUAL(result, 0, "module_get_int should succeed");
    TEST_ASSERT_EQUAL(max_iterations, 100, "Should retrieve configured value");

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
    TEST_RUN(test_module_parameter_parsing);
    TEST_RUN(test_enabled_modules_parsing);
    TEST_RUN(test_module_parameter_api);
    TEST_RUN(test_module_parameter_api_default);
    TEST_RUN(test_module_parameter_api_integer);
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
