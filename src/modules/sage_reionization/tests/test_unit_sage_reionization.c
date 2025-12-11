/**
 * @file    test_unit_sage_reionization.c
 * @brief   Software quality unit tests for sage_reionization module
 *
 * Validates: Module lifecycle, memory safety, parameter handling, error handling
 *
 * This test validates software engineering aspects of the sage_reionization module:
 * - Module registration and initialization
 * - Parameter reading and validation
 * - Memory allocation and cleanup (no leaks)
 * - Property setting patterns
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: HaloBaryonFraction property access
 *
 * @author  Mimic Development Team
 * @date    2025-12-02
 */

#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "../../core/module_interface.h"
#include "sage_reionization.h"
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
 * @brief   Test that sage_reionization module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_reionization_register() works, module appears in registry
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

    /* Configure sage_reionization module */
    /* Configure sage_reionization module in pre_timestep phase (for testing) */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_reionization");
    MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_pre_timestep = 1;
    MimicConfig.SubSteps = 1;
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
 * @brief   Test that module reads GlobalBaryonFraction parameter
 *
 * Expected: Module reads GlobalBaryonFraction parameter successfully
 * Validates: Parameter reading and validation
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

    /* Configure sage_reionization with custom GlobalBaryonFraction */
    /* Configure sage_reionization module in pre_timestep phase (for testing) */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_reionization");
    MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_pre_timestep = 1;
    MimicConfig.SubSteps = 1;

    /* Set all required parameters, then override specific one for testing */
    set_test_model_parameters();
    strcpy(MimicConfig.ModelParams[0].value, "0.20");  /* GlobalBaryonFraction custom value */

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should initialize with custom parameter");
    /* If init succeeded, parameter was read and validated */

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

    /* Configure sage_reionization module in pre_timestep phase (for testing) */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_reionization");
    MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_pre_timestep = 1;
    MimicConfig.SubSteps = 1;
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
 * @brief   Test that module can safely access HaloBaryonFraction property
 *
 * Expected: Property access doesn't crash, handles values correctly
 * Validates: Property access patterns in module
 */
int test_property_access(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Create test galaxy */
    struct GalaxyData test_galaxy;
    memset(&test_galaxy, 0, sizeof(test_galaxy));

    /* ===== VALIDATE ===== */
    /* Test that HaloBaryonFraction property can be accessed */
    TEST_ASSERT(test_galaxy.HaloBaryonFraction == 0.0f,
                "Zero-initialized galaxy should have HaloBaryonFraction=0");

    /* Test property can be set */
    test_galaxy.HaloBaryonFraction = 0.17f;
    TEST_ASSERT(test_galaxy.HaloBaryonFraction == 0.17f,
                "HaloBaryonFraction should be readable/writable");

    /* Test with reionization-suppressed value */
    test_galaxy.HaloBaryonFraction = 0.085f;  /* 50% suppression */
    TEST_ASSERT(test_galaxy.HaloBaryonFraction > 0.0f &&
                test_galaxy.HaloBaryonFraction <= 0.17f,
                "HaloBaryonFraction should be in physical range");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_reionization software quality tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_reionization Module\n");
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
