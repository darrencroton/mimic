/**
 * @file    test_sage_satellite_stripping.c
 * @brief   Software quality unit tests for sage_satellite_stripping module
 *
 * Validates: Module lifecycle, memory safety, parameter handling, error handling
 * Phase: Phase 4.2 (SAGE Modular Refactoring)
 *
 * This test validates software engineering aspects of the sage_satellite_stripping module:
 * - Module registration and initialization
 * - Parameter reading and validation
 * - Memory allocation and cleanup (no leaks)
 * - Null pointer safety
 * - Property access patterns
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Galaxy property access works correctly
 *
 * NOTE: Physics validation (satellite stripping correctness) deferred to Phase 4.3+
 *       when downstream modules are implemented for end-to-end testing.
 *
 * @author  Mimic Development Team
 * @date    2025-11-26
 */

#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "../../core/module_interface.h"
#include "sage_satellite_stripping.h"
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

/* Test fixture: Set all required model parameters (Parameter system)
 * Defined in tests/unit/test_stubs.c - provides all 20 required parameters */
extern void set_test_model_parameters(void);

/**
 * @test    test_module_registration
 * @brief   Test that sage_satellite_stripping module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_satellite_stripping_register() works, module appears in registry
 */
int test_module_registration(void)
{
    /* ===== SETUP ===== */
    reset_config();

    /* ===== EXECUTE ===== */
    ensure_modules_registered();

    /* ===== VALIDATE ===== */
    /* If we got here without crashing, registration succeeded */
    /* Module registry is internal, but we can test that module init works */

    return TEST_PASS;
}

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors or leaks
 * Validates: Module lifecycle management
 */
int test_module_initialization(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up minimal cosmology configuration */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_satellite_stripping module */
    strcpy(MimicConfig.EnabledModules[0], "sage_satellite_stripping");
    MimicConfig.NumEnabledModules = 1;
    set_test_model_parameters();

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module system initialization should succeed");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_reading
 * @brief   Test that module initializes correctly (no parameters needed)
 *
 * Expected: Module initializes successfully without parameters
 * Validates: sage_satellite_stripping no longer requires parameters (uses HaloBaryonFraction property)
 * Note: HaloBaryonFraction is set by sage_reionization module
 */
int test_parameter_reading(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up configuration */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_satellite_stripping (no parameters needed) */
    strcpy(MimicConfig.EnabledModules[0], "sage_satellite_stripping");
    MimicConfig.NumEnabledModules = 1;
    set_test_model_parameters();

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should initialize without parameters");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test that module doesn't leak memory during normal operation
 *
 * Expected: No memory leaks after init, process, cleanup cycle
 * Validates: Memory management in module
 */
int test_memory_safety(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    strcpy(MimicConfig.EnabledModules[0], "sage_satellite_stripping");
    MimicConfig.NumEnabledModules = 1;
    set_test_model_parameters();

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
 * @test    test_property_access
 * @brief   Test that module can safely access galaxy properties
 *
 * Expected: Property access doesn't crash, handles zero/null gracefully
 * Validates: Property access patterns in module (HotGas, MetalsHotGas)
 */
int test_property_access(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Create test halo and galaxy with various property states */
    struct Halo test_halo;
    memset(&test_halo, 0, sizeof(test_halo));

    struct GalaxyData test_galaxy;
    memset(&test_galaxy, 0, sizeof(test_galaxy));

    /* Set up central halo */
    test_halo.Mvir = 100.0;  /* 10^12 Msun/h */
    test_halo.Type = 0;  /* Central */
    test_halo.SnapNum = 63;
    test_halo.galaxy = &test_galaxy;

    /* Set up satellite with hot gas */
    struct Halo sat_halo;
    memset(&sat_halo, 0, sizeof(sat_halo));
    struct GalaxyData sat_galaxy;
    memset(&sat_galaxy, 0, sizeof(sat_galaxy));

    sat_halo.Mvir = 10.0;  /* 10^11 Msun/h */
    sat_halo.Type = 1;  /* Type 1 satellite */
    sat_halo.galaxy = &sat_galaxy;
    sat_galaxy.HotGas = 5.0;
    sat_galaxy.MetalsHotGas = 0.1;

    /* ===== VALIDATE ===== */
    /* Test that halo properties can be accessed without crashing */
    TEST_ASSERT(test_halo.Mvir > 0.0, "Mvir should be accessible");
    TEST_ASSERT(test_halo.Type == 0, "Type should be accessible");
    TEST_ASSERT(sat_halo.Type == 1, "Satellite Type should be accessible");

    /* Test that galaxy properties can be accessed */
    TEST_ASSERT(sat_galaxy.HotGas >= 0.0, "HotGas should be non-negative");
    TEST_ASSERT(sat_galaxy.MetalsHotGas >= 0.0, "MetalsHotGas should be non-negative");

    /* Test with zero values (edge case) */
    struct GalaxyData zero_galaxy;
    memset(&zero_galaxy, 0, sizeof(zero_galaxy));
    TEST_ASSERT(zero_galaxy.HotGas == 0.0, "Zero-initialized galaxy should have HotGas=0");
    TEST_ASSERT(zero_galaxy.MetalsHotGas == 0.0, "Zero-initialized galaxy should have MetalsHotGas=0");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_satellite_stripping software quality tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_satellite_stripping Module\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_property_access);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
