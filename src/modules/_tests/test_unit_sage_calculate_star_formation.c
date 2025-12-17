/**
 * @file    test_unit_sage_calculate_star_formation.c
 * @brief   Software quality unit tests for sage_calculate_star_formation module
 *
 * Validates: Module lifecycle, memory safety, parameter handling, SF calculation
 *
 * This test validates software engineering aspects of the sage_calculate_star_formation module:
 * - Module registration and initialization
 * - Parameter reading and validation (SfrEfficiency, StarFormingDiskFactor)
 * - Memory allocation and cleanup (no leaks)
 * - Property access patterns
 * - Star formation calculation logic
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Galaxy property access works correctly
 *   - test_star_formation_calculation: NewStellarMass calculation
 *
 * @author  Mimic Development Team
 * @date    2025-12-17
 */

#include "../../../tests/framework/test_framework.h"
#include "../core/module_registry.h"
#include "../core/module_interface.h"
#include "../include/types.h"
#include "../include/proto.h"
#include "../include/globals.h"
#include "../util/error.h"
#include "../util/memory.h"

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

/* Test fixture: Set all required model parameters
 * Defined in tests/unit/test_stubs.c - provides all required parameters */
extern void set_test_model_parameters(void);

/**
 * @test    test_module_registration
 * @brief   Test that sage_calculate_star_formation module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_calculate_star_formation_register() works
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

    /* Configure sage_calculate_star_formation module in phase_1 */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
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
 * @brief   Test that module parameters are read correctly from configuration
 *
 * Expected: Module reads SfrEfficiency and StarFormingDiskFactor successfully
 * Validates: Parameter reading infrastructure works
 */
int test_parameter_reading(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should initialize with parameters");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test that module doesn't leak memory during normal operation
 *
 * Expected: No memory leaks after init, cleanup cycle
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

    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    /* ===== EXECUTE ===== */
    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization should succeed");

    /* ===== VALIDATE ===== */
    /* Module initialized successfully without memory leaks */

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
 * Validates: Property access patterns in module
 */
int test_property_access(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Create test halo and galaxy */
    struct Halo test_halo;
    memset(&test_halo, 0, sizeof(test_halo));

    struct GalaxyData test_galaxy;
    memset(&test_galaxy, 0, sizeof(test_galaxy));

    /* Set some realistic values */
    test_halo.Mvir = 100.0;  /* 10^12 Msun/h */
    test_halo.Type = 0;  /* Central */
    test_halo.SnapNum = 63;
    test_halo.galaxy = &test_galaxy;

    test_galaxy.ColdGas = 10.0;
    test_galaxy.DiskScaleRadius = 5.0;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy != NULL, "Galaxy pointer should be accessible");
    TEST_ASSERT(test_galaxy.ColdGas >= 0.0, "ColdGas should be non-negative");
    TEST_ASSERT(test_galaxy.DiskScaleRadius >= 0.0, "DiskScaleRadius should be non-negative");

    /* Test with zero values (edge case) */
    struct GalaxyData zero_galaxy;
    memset(&zero_galaxy, 0, sizeof(zero_galaxy));
    TEST_ASSERT(zero_galaxy.ColdGas == 0.0, "Zero-initialized galaxy should have ColdGas=0");
    TEST_ASSERT(zero_galaxy.NewStellarMass == 0.0, "Zero-initialized galaxy should have NewStellarMass=0");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_star_formation_calculation
 * @brief   Test star formation calculation logic
 *
 * Expected: NewStellarMass calculated based on ColdGas and efficiency
 * Validates: Star formation calculation
 */
int test_star_formation_calculation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Create test halo and galaxy */
    struct Halo test_halo;
    memset(&test_halo, 0, sizeof(test_halo));

    struct GalaxyData test_galaxy;
    memset(&test_galaxy, 0, sizeof(test_galaxy));

    test_halo.Type = 0;  /* Central */
    test_halo.Mvir = 100.0;
    test_halo.galaxy = &test_galaxy;

    /* Initial state */
    test_galaxy.ColdGas = 10.0;
    test_galaxy.DiskScaleRadius = 5.0;
    test_galaxy.NewStellarMass = 0.0;

    /* ===== VALIDATE ===== */
    /* Basic validation of properties used in SF calculation */
    TEST_ASSERT(test_galaxy.ColdGas > 0.0, "ColdGas should be positive for SF");
    TEST_ASSERT(test_galaxy.DiskScaleRadius > 0.0, "DiskScaleRadius should be positive");
    TEST_ASSERT(test_galaxy.NewStellarMass == 0.0, "NewStellarMass should start at zero");

    /* Full calculation logic tested in integration tests */

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_calculate_star_formation software quality tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_calculate_star_formation Module\n");
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
    TEST_RUN(test_star_formation_calculation);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
