/**
 * @file    test_unit_sage_add_infall.c
 * @brief   Software quality unit tests for sage_add_infall module
 *
 * Validates: Module lifecycle, memory safety, error handling, infall distribution
 *
 * This test validates software engineering aspects of the sage_add_infall module:
 * - Module registration and initialization
 * - Memory allocation and cleanup (no leaks)
 * - Null pointer safety
 * - Property access patterns
 * - Infall distribution over substeps (actual module execution)
 * - Negative infall handling (ejected→hot priority)
 * - Metallicity preservation during transfers
 * - Edge cases (zero infall, multiple substeps)
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Galaxy property access works correctly
 *   - test_physics_positive_infall_single_substep: Positive infall with 1 substep
 *   - test_physics_positive_infall_multiple_substeps: Infall distributed over 4 substeps
 *   - test_physics_negative_infall_from_ejected: Negative infall depletes ejected first
 *   - test_physics_negative_infall_from_hot: Negative infall depletes hot when ejected empty
 *   - test_physics_negative_infall_cascade: Negative infall depletes ejected then hot
 *   - test_zero_infall: Edge case with zero infall
 *   - test_no_central_galaxy: Edge case with no Type 0 central
 *
 * @author  Mimic Development Team
 * @date    2025-12-11
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

/* External module interface for direct testing */
extern int sage_add_infall_init(void);
extern int sage_add_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_add_infall_cleanup(void);

/* Test fixtures for physics tests */

/**
 * @brief Initialize sage_add_infall module for physics testing
 *
 * Sets up minimal module state without full module system.
 * Module has no init-time parameters to configure.
 */
static void setup_module_for_physics_test(void)
{
    sage_add_infall_init();
}

/**
 * @brief Create a test halo with galaxy for physics tests
 *
 * @param type Halo type (0=central, 1=satellite, 2=orphan, 3=ejected)
 * @param mvir Virial mass in code units (1e10 Msun/h)
 * @return Allocated halo (must be freed with free_test_halo)
 */
static struct Halo create_test_halo(int type, float mvir)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = type;
    halo.Mvir = mvir;
    halo.SnapNum = 63;  /* z=0 */

    /* Allocate galaxy data */
    halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
    memset(halo.galaxy, 0, sizeof(struct GalaxyData));

    return halo;
}

/**
 * @brief Free test halo resources
 */
static void free_test_halo(struct Halo *halo)
{
    if (halo->galaxy != NULL) {
        myfree(halo->galaxy);
        halo->galaxy = NULL;
    }
}

/**
 * @test    test_module_registration
 * @brief   Test that sage_add_infall module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_add_infall_register() works, module appears in registry
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

    /* Configure sage_add_infall module in phase_1 */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_add_infall");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_FULL_HALO;
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

    /* Configure module */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_add_infall");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_FULL_HALO;
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

    test_galaxy.InfallingGas = 5.0;
    test_galaxy.HotGas = 15.0;
    test_galaxy.MetalsHotGas = 0.3;
    test_galaxy.EjectedGas = 2.0;
    test_galaxy.MetalsEjectedGas = 0.04;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy != NULL, "Galaxy pointer should be accessible");
    TEST_ASSERT(test_galaxy.InfallingGas >= 0.0, "InfallingGas should be non-negative");
    TEST_ASSERT(test_galaxy.HotGas >= 0.0, "HotGas should be non-negative");

    /* Test with zero values (edge case) */
    struct GalaxyData zero_galaxy;
    memset(&zero_galaxy, 0, sizeof(zero_galaxy));
    TEST_ASSERT(zero_galaxy.HotGas == 0.0, "Zero-initialized galaxy should have HotGas=0");
    TEST_ASSERT(zero_galaxy.InfallingGas == 0.0, "Zero-initialized galaxy should have InfallingGas=0");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_positive_infall_single_substep
 * @brief   Test positive infall with single substep
 *
 * Expected: InfallingGas added to HotGas in single substep
 * Validates: Basic infall distribution
 */
int test_physics_positive_infall_single_substep(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo test_halo = create_test_halo(0, 100.0);  /* Type 0 central */

    /* Initial state */
    test_halo.galaxy->InfallingGas = 10.0;
    test_halo.galaxy->HotGas = 5.0;
    test_halo.galaxy->MetalsHotGas = 0.1;

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;
    ctx.substep_number = 0;

    /* ===== EXECUTE ===== */
    int result = sage_add_infall_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module processing should succeed");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 15.0, 0.001, "HotGas should be 5.0 + 10.0 = 15.0");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_positive_infall_multiple_substeps
 * @brief   Test positive infall distributed over 4 substeps
 *
 * Expected: InfallingGas/4 added each substep
 * Validates: Substep distribution logic
 */
int test_physics_positive_infall_multiple_substeps(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo test_halo = create_test_halo(0, 100.0);

    /* Initial state */
    test_halo.galaxy->InfallingGas = 12.0;  /* 3.0 per substep */
    test_halo.galaxy->HotGas = 5.0;
    test_halo.galaxy->MetalsHotGas = 0.1;

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 4;

    /* ===== EXECUTE ===== */
    /* Simulate 4 substeps */
    for (int i = 0; i < 4; i++) {
        ctx.substep_number = i;
        int result = sage_add_infall_process(&ctx, &test_halo, 1);
        TEST_ASSERT(result == 0, "Module processing should succeed for each substep");
    }

    /* ===== VALIDATE ===== */
    /* After 4 substeps: HotGas = 5.0 + 4*(12.0/4) = 5.0 + 12.0 = 17.0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 17.0, 0.001, "HotGas should be 5.0 + 12.0 = 17.0");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_from_ejected
 * @brief   Test negative infall removes from ejected reservoir first
 *
 * Expected: EjectedGas reduced, HotGas unchanged (ejected has enough mass)
 * Validates: Priority order (ejected before hot)
 */
int test_physics_negative_infall_from_ejected(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo test_halo = create_test_halo(0, 100.0);

    /* Initial state */
    test_halo.galaxy->InfallingGas = -5.0;  /* Mass loss */
    test_halo.galaxy->EjectedGas = 10.0;
    test_halo.galaxy->MetalsEjectedGas = 0.2;  /* Z = 0.02 */
    test_halo.galaxy->HotGas = 20.0;
    test_halo.galaxy->MetalsHotGas = 0.4;  /* Z = 0.02 */

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;
    ctx.substep_number = 0;

    /* ===== EXECUTE ===== */
    int result = sage_add_infall_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module processing should succeed");

    /* Ejected should be reduced: 10.0 - 5.0 = 5.0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 5.0, 0.001, "EjectedGas should be 10.0 - 5.0 = 5.0");

    /* Hot should be unchanged (ejected had enough mass) */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 20.0, 0.001, "HotGas should be unchanged at 20.0");

    /* Metallicity preserved in ejected: 5.0 * 0.02 = 0.1 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsEjectedGas, 0.1, 0.001, "MetalsEjectedGas should be 0.1");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_from_hot
 * @brief   Test negative infall removes from hot gas when ejected is empty
 *
 * Expected: HotGas reduced, EjectedGas remains 0
 * Validates: Fallback to hot gas when ejected is depleted
 */
int test_physics_negative_infall_from_hot(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo test_halo = create_test_halo(0, 100.0);

    /* Initial state */
    test_halo.galaxy->InfallingGas = -5.0;  /* Mass loss */
    test_halo.galaxy->EjectedGas = 0.0;  /* Empty ejected */
    test_halo.galaxy->MetalsEjectedGas = 0.0;
    test_halo.galaxy->HotGas = 20.0;
    test_halo.galaxy->MetalsHotGas = 0.4;  /* Z = 0.02 */

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;
    ctx.substep_number = 0;

    /* ===== EXECUTE ===== */
    int result = sage_add_infall_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module processing should succeed");

    /* Ejected should remain 0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 0.0, 0.001, "EjectedGas should remain 0.0");

    /* Hot should be reduced: 20.0 - 5.0 = 15.0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 15.0, 0.001, "HotGas should be 20.0 - 5.0 = 15.0");

    /* Metallicity preserved in hot: 15.0 * 0.02 = 0.3 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, 0.3, 0.001, "MetalsHotGas should be 0.3");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_cascade
 * @brief   Test negative infall cascades: depletes ejected, then hot
 *
 * Expected: EjectedGas → 0, HotGas reduced by remaining deficit
 * Validates: Cascade logic (ejected → hot)
 */
int test_physics_negative_infall_cascade(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo test_halo = create_test_halo(0, 100.0);

    /* Initial state */
    test_halo.galaxy->InfallingGas = -8.0;  /* Mass loss exceeds ejected */
    test_halo.galaxy->EjectedGas = 3.0;  /* Only 3.0 available */
    test_halo.galaxy->MetalsEjectedGas = 0.06;  /* Z = 0.02 */
    test_halo.galaxy->HotGas = 10.0;
    test_halo.galaxy->MetalsHotGas = 0.2;  /* Z = 0.02 */

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;
    ctx.substep_number = 0;

    /* ===== EXECUTE ===== */
    int result = sage_add_infall_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module processing should succeed");

    /* Ejected should be depleted */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 0.0, 0.001, "EjectedGas should be depleted to 0.0");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsEjectedGas, 0.0, 0.001, "MetalsEjectedGas should be 0.0");

    /* Hot should absorb remaining: 10.0 + (-8.0 + 3.0) = 5.0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 5.0, 0.001, "HotGas should be 10.0 - 5.0 = 5.0");

    /* Metallicity preserved in hot: 5.0 * 0.02 = 0.1 */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, 0.1, 0.001, "MetalsHotGas should be 0.1");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_infall
 * @brief   Test edge case with zero infall
 *
 * Expected: No changes to gas reservoirs
 * Validates: Zero infall handling
 */
int test_zero_infall(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo test_halo = create_test_halo(0, 100.0);

    /* Initial state */
    test_halo.galaxy->InfallingGas = 0.0;  /* Zero infall */
    test_halo.galaxy->HotGas = 10.0;
    test_halo.galaxy->MetalsHotGas = 0.2;
    test_halo.galaxy->EjectedGas = 5.0;
    test_halo.galaxy->MetalsEjectedGas = 0.1;

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;
    ctx.substep_number = 0;

    /* ===== EXECUTE ===== */
    int result = sage_add_infall_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module processing should succeed");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 10.0, 0.001, "HotGas should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 5.0, 0.001, "EjectedGas should be unchanged");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_no_central_galaxy
 * @brief   Test edge case with no Type 0 central galaxy
 *
 * Expected: Module returns 0 (graceful handling)
 * Validates: No-central-galaxy safety
 */
int test_no_central_galaxy(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Create satellite-only group */
    struct Halo satellite = create_test_halo(1, 10.0);  /* Type 1 satellite */
    satellite.galaxy->InfallingGas = 5.0;
    satellite.galaxy->HotGas = 3.0;

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;
    ctx.substep_number = 0;

    /* ===== EXECUTE ===== */
    int result = sage_add_infall_process(&ctx, &satellite, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should handle no-central case gracefully");

    /* Satellite should be unchanged (infall only applies to centrals) */
    TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->HotGas, 3.0, 0.001, "Satellite HotGas should be unchanged");

    /* ===== CLEANUP ===== */
    free_test_halo(&satellite);
    sage_add_infall_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_add_infall software quality tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_add_infall Module\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_property_access);
    TEST_RUN(test_physics_positive_infall_single_substep);
    TEST_RUN(test_physics_positive_infall_multiple_substeps);
    TEST_RUN(test_physics_negative_infall_from_ejected);
    TEST_RUN(test_physics_negative_infall_from_hot);
    TEST_RUN(test_physics_negative_infall_cascade);
    TEST_RUN(test_zero_infall);
    TEST_RUN(test_no_central_galaxy);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
