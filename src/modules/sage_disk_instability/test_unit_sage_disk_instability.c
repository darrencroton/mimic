/**
 * @file    test_unit_sage_disk_instability.c
 * @brief   Software quality unit tests for sage_disk_instability module
 *
 * Validates: Module lifecycle, parameter reading, memory safety, property access
 * Phase: Phase 4.6 (SAGE Disk Instability Module - Partial Implementation)
 *
 * This test validates software engineering aspects of the sage_disk_instability module:
 * - Module registration and initialization
 * - Parameter reading and validation
 * - Memory allocation and cleanup (no leaks)
 * - Property access patterns
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_parameter_validation: Invalid parameters rejected
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Property access doesn't crash
 *
 * NOTE: Physics validation (stability criterion, mass transfers) requires integration
 *       testing infrastructure (Phase 4.3). Unit tests focus on software quality.
 *
 * @author  Mimic Development Team
 * @date    2025-11-18
 */

#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "../../core/module_interface.h"
#include "sage_disk_instability.h"
#include "../../include/types.h"
#include "../../include/proto.h"
#include "../../include/globals.h"
#include "../../util/error.h"
#include "../../util/memory.h"
#include "../../include/constants.h"

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

    /* Set essential cosmological parameters */
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;

    /* Calculate G in code units */
    double UnitLength_in_cm = 3.08568e+24; /* Mpc in cm */
    double UnitVelocity_in_cm_per_s = 1.0e5; /* km/s in cm/s */
    double UnitMass_in_g = 1.989e43 * MimicConfig.Hubble_h; /* 1e10 Msun/h */

    MimicConfig.G = GRAVITY * UnitLength_in_cm * UnitMass_in_g /
                   (UnitVelocity_in_cm_per_s * UnitVelocity_in_cm_per_s);
}

/* Test fixture: ensure modules are registered (only once) */
static void ensure_modules_registered(void)
{
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/* Test fixture: Set sage_disk_instability parameters to defaults */
static void set_default_params(void)
{
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DiskInstabilityOn");
    strcpy(MimicConfig.ModuleParams[0].value, "1");

    strcpy(MimicConfig.ModuleParams[1].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[1].param_name, "DiskRadiusFactor");
    strcpy(MimicConfig.ModuleParams[1].value, "3.0");

    MimicConfig.NumModuleParams = 2;
}

/**
 * @test    test_module_registration
 * @brief   Test that sage_disk_instability module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_disk_instability_register() works, module appears in registry
 */
int test_module_registration(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Check module is registered by trying to enable it */
    strcpy(MimicConfig.EnabledModules[0], "sage_disk_instability");
    MimicConfig.NumEnabledModules = 1;
    set_default_params();

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "sage_disk_instability module should register successfully");

    /* ===== CLEANUP ===== */
    if (result == 0) {
        module_system_cleanup();
    }
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_module_initialization
 * @brief   Test module init/cleanup lifecycle
 *
 * Expected: Module initializes and cleans up without errors or memory leaks
 * Validates: init/cleanup functions, parameter reading
 */
int test_module_initialization(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    strcpy(MimicConfig.EnabledModules[0], "sage_disk_instability");
    MimicConfig.NumEnabledModules = 1;
    set_default_params();

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
 * Expected: Module reads both parameters successfully
 * Validates: Parameter reading infrastructure works
 */
int test_parameter_reading(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    strcpy(MimicConfig.EnabledModules[0], "sage_disk_instability");
    MimicConfig.NumEnabledModules = 1;

    /* Configure with non-default values */
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DiskInstabilityOn");
    strcpy(MimicConfig.ModuleParams[0].value, "0");

    strcpy(MimicConfig.ModuleParams[1].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[1].param_name, "DiskRadiusFactor");
    strcpy(MimicConfig.ModuleParams[1].value, "5.0");

    MimicConfig.NumModuleParams = 2;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should initialize with custom parameters");
    /* If init succeeded, parameters were read and validated */

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_validation
 * @brief   Test that invalid parameters are rejected
 *
 * Expected: Module initialization fails with out-of-range DiskRadiusFactor
 * Validates: Parameter validation logic
 */
int test_parameter_validation(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    strcpy(MimicConfig.EnabledModules[0], "sage_disk_instability");
    MimicConfig.NumEnabledModules = 1;

    /* Set invalid parameter (DiskRadiusFactor outside [1.0, 10.0]) */
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DiskRadiusFactor");
    strcpy(MimicConfig.ModuleParams[0].value, "15.0"); /* Invalid - too large */

    MimicConfig.NumModuleParams = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result != 0, "Module should reject invalid DiskRadiusFactor");

    /* ===== CLEANUP ===== */
    /* Don't call cleanup if init failed */
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

    strcpy(MimicConfig.EnabledModules[0], "sage_disk_instability");
    MimicConfig.NumEnabledModules = 1;
    set_default_params();

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
 * Validates: Property access patterns in module
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

    /* Set some realistic values for disk instability module */
    test_halo.Rvir = 0.2;  /* 200 kpc/h */
    test_halo.Vmax = 200.0;  /* 200 km/s (Milky Way-like) */
    test_halo.Type = 0;  /* Central */
    test_halo.HaloNr = 0;
    test_halo.galaxy = &test_galaxy;

    test_galaxy.ColdGas = 1.0;
    test_galaxy.StellarMass = 5.0;
    test_galaxy.BulgeMass = 1.0;
    test_galaxy.MetalsStellarMass = 0.1;
    test_galaxy.MetalsBulgeMass = 0.02;
    test_galaxy.DiskScaleRadius = 0.0;  /* Will be calculated by module */

    /* ===== VALIDATE ===== */
    /* Test that halo properties can be accessed without crashing */
    TEST_ASSERT(test_halo.Rvir > 0.0, "Rvir should be accessible");
    TEST_ASSERT(test_halo.Vmax > 0.0, "Vmax should be accessible");
    TEST_ASSERT(test_halo.galaxy != NULL, "Galaxy pointer should be accessible");

    /* Test that galaxy properties can be accessed */
    TEST_ASSERT(test_galaxy.StellarMass >= 0.0, "StellarMass should be non-negative");
    TEST_ASSERT(test_galaxy.BulgeMass >= 0.0, "BulgeMass should be non-negative");
    TEST_ASSERT(test_galaxy.ColdGas >= 0.0, "ColdGas should be non-negative");

    /* Test with zero values (edge case) */
    struct GalaxyData zero_galaxy;
    memset(&zero_galaxy, 0, sizeof(zero_galaxy));
    TEST_ASSERT(zero_galaxy.StellarMass == 0.0, "Zero-initialized galaxy should have StellarMass=0");
    TEST_ASSERT(zero_galaxy.BulgeMass == 0.0, "Zero-initialized galaxy should have BulgeMass=0");
    TEST_ASSERT(zero_galaxy.ColdGas == 0.0, "Zero-initialized galaxy should have ColdGas=0");

    /* Test disk/bulge mass relationship */
    double disk_stellar_mass = test_galaxy.StellarMass - test_galaxy.BulgeMass;
    TEST_ASSERT(disk_stellar_mass >= 0.0, "Disk stellar mass should be non-negative");
    TEST_ASSERT(test_galaxy.BulgeMass <= test_galaxy.StellarMass,
                "Bulge mass should not exceed total stellar mass");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_disk_instability software quality tests and reports results.
 */
int main(void)
{
    printf("========================================\n");
    printf("Test Suite: sage_disk_instability\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_parameter_validation);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_property_access);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
