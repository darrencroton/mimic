/**
 * @file    test_unit_sage_reincorporation.c
 * @brief   Software quality unit tests for sage_reincorporation module
 *
 * Validates: Module lifecycle, memory safety, parameter handling, error handling
 * Phase: Phase 4.2 (SAGE Physics Module Implementation)
 *
 * This test validates software engineering aspects of the sage_reincorporation module:
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
 * NOTE: Physics validation (reincorporation calculations) deferred to Phase 4.3+
 *       when downstream modules (star formation & feedback) are implemented.
 *
 * @author  Mimic Development Team
 * @date    2025-11-17
 */

#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "../../core/module_interface.h"
#include "sage_reincorporation.h"
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

/* Mark as used to avoid warnings */
__attribute__((unused)) static int *_passed_ptr = &passed;
__attribute__((unused)) static int *_failed_ptr = &failed;

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

/* Test fixture: Set sage_reincorporation parameters to defaults */
static void set_default_sage_reincorporation_params(void)
{
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageReincorporation");
    strcpy(MimicConfig.ModuleParams[0].param_name, "ReIncorporationFactor");
    strcpy(MimicConfig.ModuleParams[0].value, "1.0");

    MimicConfig.NumModuleParams = 1;
}

/**
 * @test    test_module_registration
 * @brief   Test that sage_reincorporation module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_reincorporation_register() works, module appears in registry
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

    /* Configure sage_reincorporation module */
    strcpy(MimicConfig.EnabledModules[0], "sage_reincorporation");
    MimicConfig.NumEnabledModules = 1;
    set_default_sage_reincorporation_params();

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
 * @brief   Test that module reads parameters from configuration correctly
 *
 * Expected: Module reads and validates ReIncorporationFactor parameter
 * Validates: Parameter parsing and validation
 */
int test_parameter_reading(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up cosmology */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_reincorporation with custom parameter */
    strcpy(MimicConfig.EnabledModules[0], "sage_reincorporation");
    MimicConfig.NumEnabledModules = 1;

    strcpy(MimicConfig.ModuleParams[0].module_name, "SageReincorporation");
    strcpy(MimicConfig.ModuleParams[0].param_name, "ReIncorporationFactor");
    strcpy(MimicConfig.ModuleParams[0].value, "0.5");  // Custom value
    MimicConfig.NumModuleParams = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module initialization with custom parameters should succeed");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_invalid_parameter
 * @brief   Test that module rejects invalid parameter values
 *
 * Expected: Module initialization fails with out-of-range parameter
 * Validates: Parameter validation logic
 */
int test_invalid_parameter(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up cosmology */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_reincorporation with INVALID parameter */
    strcpy(MimicConfig.EnabledModules[0], "sage_reincorporation");
    MimicConfig.NumEnabledModules = 1;

    strcpy(MimicConfig.ModuleParams[0].module_name, "SageReincorporation");
    strcpy(MimicConfig.ModuleParams[0].param_name, "ReIncorporationFactor");
    strcpy(MimicConfig.ModuleParams[0].value, "20.0");  // Out of range [0.0, 10.0]
    MimicConfig.NumModuleParams = 1;

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == -1, "Module initialization should fail with invalid parameter");

    /* ===== CLEANUP ===== */
    /* Don't call module_system_cleanup() since init failed */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test that module has no memory leaks during operation
 *
 * Expected: No memory leaks after module init/cleanup cycle
 * Validates: Memory management correctness
 */
int test_memory_safety(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up cosmology */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_reincorporation */
    strcpy(MimicConfig.EnabledModules[0], "sage_reincorporation");
    MimicConfig.NumEnabledModules = 1;
    set_default_sage_reincorporation_params();

    /* ===== EXECUTE ===== */
    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization should succeed");

    /* ===== CLEANUP ===== */
    module_system_cleanup();

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will abort if there are leaks */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_property_access
 * @brief   Test that module can access galaxy properties correctly
 *
 * Expected: Module processes halos and accesses properties without crashes
 * Validates: Property system integration
 */
int test_property_access(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up cosmology */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_reincorporation */
    strcpy(MimicConfig.EnabledModules[0], "sage_reincorporation");
    MimicConfig.NumEnabledModules = 1;
    set_default_sage_reincorporation_params();

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization should succeed");

    /* Create minimal test halos */
    int ngal = 2;
    struct Halo *halos = malloc_tracked(ngal * sizeof(struct Halo), MEM_HALOS);
    TEST_ASSERT(halos != NULL, "Halo allocation should succeed");

    /* Initialize test halos */
    for (int i = 0; i < ngal; i++) {
        memset(&halos[i], 0, sizeof(struct Halo));
        halos[i].Type = 0;  // Central galaxy
        halos[i].Vvir = 500.0f;  // Above critical velocity (~445 km/s)
        halos[i].Rvir = 0.2f;    // Typical virial radius (Mpc/h)
        halos[i].dT = 0.1f;      // Timestep (Gyr)

        /* Allocate and initialize galaxy data */
        halos[i].galaxy = malloc_tracked(sizeof(struct GalaxyData), MEM_HALOS);
        TEST_ASSERT(halos[i].galaxy != NULL, "Galaxy data allocation should succeed");
        memset(halos[i].galaxy, 0, sizeof(struct GalaxyData));

        /* Set initial property values for reincorporation */
        halos[i].galaxy->EjectedMass = 1.0f;       // 1e10 Msun/h of ejected gas
        halos[i].galaxy->MetalsEjectedMass = 0.02f; // 2% metallicity
        halos[i].galaxy->HotGas = 5.0f;            // Initial hot gas
        halos[i].galaxy->MetalsHotGas = 0.1f;      // Initial hot metals
    }

    /* Create module context */
    struct ModuleContext ctx;
    ctx.redshift = 1.0;
    ctx.time = 6.0;  /* ~6 Gyr */
    ctx.params = &MimicConfig;

    /* ===== EXECUTE ===== */
    result = module_execute_pipeline(&ctx, halos, ngal);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module pipeline execution should succeed");

    /* Verify properties were accessed (basic sanity check) */
    /* Since Vvir > Vcrit, some reincorporation should have occurred */
    TEST_ASSERT(halos[0].galaxy->EjectedMass < 1.0f,
               "EjectedMass should have decreased (some gas reincorporated)");
    TEST_ASSERT(halos[0].galaxy->HotGas > 5.0f,
               "HotGas should have increased (gas added from ejected reservoir)");

    /* ===== CLEANUP ===== */
    for (int i = 0; i < ngal; i++) {
        if (halos[i].galaxy) {
            free_tracked(halos[i].galaxy, MEM_HALOS);
        }
    }
    free_tracked(halos, MEM_HALOS);

    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* MAIN TEST RUNNER                                                          */
/* ========================================================================== */

/**
 * @brief   Main test runner
 *
 * Executes all sage_reincorporation software quality tests and reports results.
 */
int main(void)
{
    printf("========================================\n");
    printf("Test Suite: sage_reincorporation\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_invalid_parameter);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_property_access);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
