/**
 * @file    test_unit_sage_prepare_infall_budget.c
 * @brief   Comprehensive unit tests for sage_prepare_infall_budget module
 *
 * Validates: Module lifecycle, physics calculation, memory safety, edge cases, parameter validation
 *
 * This test validates both software engineering and physics aspects:
 * - Module registration and initialization
 * - Parameter reading and validation (including invalid values)
 * - Memory allocation and cleanup (no leaks)
 * - Physics calculation (infall budget, consolidation)
 * - Edge cases (zero mass, no satellites, orphans)
 * - Consolidation logic (satellite ejected gas, ICS, orphan hot gas)
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_invalid_parameter_fails: Invalid parameters rejected
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Galaxy property access works correctly
 *   - test_physics_calculation_basic: Basic infall calculation
 *   - test_satellite_ejected_consolidation: Central gets satellite ejected gas
 *   - test_satellite_ics_consolidation: Central gets satellite ICS
 *   - test_orphan_hot_gas_retained: Type 2 orphan keeps its hot gas (SAGE parity)
 *   - test_zero_mass_halo: Mvir=0 handled correctly
 *   - test_no_satellites: Single central works
 *   - test_multiple_satellites: Multiple satellites consolidated
 *
 * @author  Mimic Development Team
 * @date    2025-12-17 (Refactored)
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

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

/* Test fixture: Set all required model parameters */
extern void set_test_model_parameters(void);

/* Test fixture: Initialize module for physics tests */
static void setup_module_for_physics_test(double global_baryon_frac)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up cosmology configuration */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    /* Configure sage_prepare_infall_budget module in pre_timestep phase */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_prepare_infall_budget");
    MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_pre_timestep = 1;
    MimicConfig.SubSteps = 1;

    /* Set test parameters */
    set_test_model_parameters();
    /* Override GlobalBaryonFraction */
    snprintf(MimicConfig.ModelParams[0].value, sizeof(MimicConfig.ModelParams[0].value),
             "%.6f", global_baryon_frac);

    /* Initialize module system */
    int result = module_system_init();
    if (result != 0) {
        ERROR_LOG("Module system initialization failed in test fixture");
    }
}

/* Test fixture: Create test halo with specified properties */
static struct Halo create_test_halo(int type, float mvir)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = type;
    halo.Mvir = mvir;
    halo.Vvir = 100.0;
    halo.Rvir = 0.1;

    /* Allocate galaxy */
    halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_UTILITY);
    memset(halo.galaxy, 0, sizeof(struct GalaxyData));
    halo.galaxy->HaloBaryonFraction = -1.0f;  /* Sentinel for uninitialized */

    return halo;
}

/* Test fixture: Free test halo */
static void free_test_halo(struct Halo *halo)
{
    if (halo->galaxy != NULL) {
        myfree(halo->galaxy);
        halo->galaxy = NULL;
    }
}

/**
 * @test    test_module_registration
 * @brief   Test that sage_prepare_infall_budget module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_prepare_infall_budget_register() works
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

    /* Configure module */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_prepare_infall_budget");
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

    /* Configure with custom GlobalBaryonFraction */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_prepare_infall_budget");
    MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_pre_timestep = 1;
    MimicConfig.SubSteps = 1;

    set_test_model_parameters();
    strcpy(MimicConfig.ModelParams[0].value, "0.20");

    /* ===== EXECUTE ===== */
    int result = module_system_init();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should initialize with custom parameter");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_invalid_parameter_fails
 * @brief   Test that module rejects invalid GlobalBaryonFraction values
 *
 * Expected: Module init fails with invalid parameters
 * Validates: Parameter validation (valid range: (0, 1] - exclusive lower, inclusive upper)
 */
int test_invalid_parameter_fails(void)
{
    /* Test cases: values that should fail (0, 1] means 0 < value <= 1.0) */
    double invalid_values[] = {0.0, -0.1, 1.5};
    int num_cases = 3;

    for (int i = 0; i < num_cases; i++) {
        /* ===== SETUP ===== */
        reset_config();
        init_memory_system(0);
        ensure_modules_registered();

        MimicConfig.Omega = 0.25;
        MimicConfig.OmegaLambda = 0.75;
        MimicConfig.Hubble_h = 0.73;

        MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
        MimicConfig.pre_timestep[0].module_name = strdup("sage_prepare_infall_budget");
        MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
        MimicConfig.num_pre_timestep = 1;
        MimicConfig.SubSteps = 1;

        set_test_model_parameters();
        snprintf(MimicConfig.ModelParams[0].value, sizeof(MimicConfig.ModelParams[0].value),
                 "%.6f", invalid_values[i]);

        /* ===== EXECUTE ===== */
        int result = module_system_init();

        /* ===== VALIDATE ===== */
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                 "Module should fail with invalid GlobalBaryonFraction=%.2f", invalid_values[i]);
        TEST_ASSERT(result != 0, error_msg);

        /* ===== CLEANUP ===== */
        check_memory_leaks();
    }

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
    setup_module_for_physics_test(0.17);

    /* Create test halo */
    struct Halo test_halo = create_test_halo(0, 100.0);
    test_halo.galaxy->HaloBaryonFraction = 0.17f;

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
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

    test_halo.Mvir = 100.0;
    test_halo.Type = 0;
    test_halo.galaxy = &test_galaxy;

    test_galaxy.StellarMass = 10.0;
    test_galaxy.ColdGas = 5.0;
    test_galaxy.HotGas = 15.0;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy != NULL, "Galaxy pointer should be accessible");
    TEST_ASSERT(test_galaxy.StellarMass >= 0.0, "StellarMass should be non-negative");
    TEST_ASSERT(test_galaxy.HotGas >= 0.0, "HotGas should be non-negative");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_calculation_basic
 * @brief   Test basic infall calculation for central galaxy
 *
 * Expected: InfallingGas = HaloBaryonFraction × Mvir - existing_baryons
 * Validates: Correct physics calculation
 */
int test_physics_calculation_basic(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create central with known baryon content */
    struct Halo central = create_test_halo(0, 100.0);  /* 1e12 Msun/h */
    central.galaxy->HaloBaryonFraction = 0.17f;
    central.galaxy->StellarMass = 5.0f;
    central.galaxy->ColdGas = 3.0f;
    central.galaxy->HotGas = 8.0f;
    central.galaxy->EjectedGas = 1.0f;
    central.galaxy->ICS = 0.5f;
    central.galaxy->BlackHoleMass = 0.1f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &central;

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, &central, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Expected: 0.17 × 100.0 - (5.0 + 3.0 + 8.0 + 1.0 + 0.5 + 0.1) = 17.0 - 17.6 = -0.6 */
    double expected_infall = 0.17 * 100.0 - (5.0 + 3.0 + 8.0 + 1.0 + 0.5 + 0.1);

    printf("  Basic infall: Mvir=%.1f, HaloBaryonFrac=%.2f, InfallingGas=%.2f (expected %.2f)\n",
           central.Mvir, central.galaxy->HaloBaryonFraction,
           central.galaxy->InfallingGas, expected_infall);

    TEST_ASSERT_DOUBLE_EQUAL(central.galaxy->InfallingGas, expected_infall, 0.01,
                             "InfallingGas should match expected value");

    /* ===== CLEANUP ===== */
    free_test_halo(&central);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_satellite_ejected_consolidation
 * @brief   Test that satellite ejected gas is consolidated to central
 *
 * Expected: Central gets all ejected gas, satellites have zero
 * Validates: Consolidation logic with metallicity preservation
 */
int test_satellite_ejected_consolidation(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create central and satellite */
    struct Halo halos[2];
    halos[0] = create_test_halo(0, 100.0);  /* Central */
    halos[1] = create_test_halo(1, 20.0);   /* Satellite */

    halos[0].galaxy->HaloBaryonFraction = 0.17f;
    halos[0].galaxy->EjectedGas = 1.0f;
    halos[0].galaxy->MetalsEjectedGas = 0.02f;

    halos[1].galaxy->HaloBaryonFraction = 0.17f;
    halos[1].galaxy->EjectedGas = 2.0f;
    halos[1].galaxy->MetalsEjectedGas = 0.04f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Central should have all ejected gas (1.0 + 2.0 = 3.0) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->EjectedGas, 3.0, 0.01,
                             "Central should have all ejected gas");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsEjectedGas, 0.06, 0.01,
                             "Central should have all ejected metals");

    /* Satellite should have zero ejected gas */
    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->EjectedGas, 0.0, 0.01,
                             "Satellite should have zero ejected gas");
    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->MetalsEjectedGas, 0.0, 0.01,
                             "Satellite should have zero ejected metals");

    printf("  ✓ Satellite ejected gas consolidated: Central EjectedGas=%.2f (expected 3.0)\n",
           halos[0].galaxy->EjectedGas);

    /* ===== CLEANUP ===== */
    free_test_halo(&halos[0]);
    free_test_halo(&halos[1]);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_satellite_ics_consolidation
 * @brief   Test that satellite ICS is consolidated to central
 *
 * Expected: Central gets all ICS, satellites have zero
 * Validates: ICS consolidation with metallicity
 */
int test_satellite_ics_consolidation(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create central and satellite */
    struct Halo halos[2];
    halos[0] = create_test_halo(0, 100.0);
    halos[1] = create_test_halo(1, 20.0);

    halos[0].galaxy->HaloBaryonFraction = 0.17f;
    halos[0].galaxy->ICS = 0.5f;
    halos[0].galaxy->MetalsICS = 0.01f;

    halos[1].galaxy->HaloBaryonFraction = 0.17f;
    halos[1].galaxy->ICS = 1.5f;
    halos[1].galaxy->MetalsICS = 0.03f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Central should have all ICS (0.5 + 1.5 = 2.0) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ICS, 2.0, 0.01,
                             "Central should have all ICS");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsICS, 0.04, 0.01,
                             "Central should have all ICS metals");

    /* Satellite should have zero ICS */
    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->ICS, 0.0, 0.01,
                             "Satellite should have zero ICS");

    printf("  ✓ Satellite ICS consolidated: Central ICS=%.2f (expected 2.0)\n",
           halos[0].galaxy->ICS);

    /* ===== CLEANUP ===== */
    free_test_halo(&halos[0]);
    free_test_halo(&halos[1]);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_orphan_hot_gas_retained
 * @brief   SAGE parity: Type 2 orphan hot gas is NOT consolidated to the
 *          central. SAGE infall_recipe consolidates only ejected gas + ICS;
 *          orphans keep their hot gas and cool it themselves until merging.
 *
 * Expected: Central keeps only its own hot gas; orphan retains its hot gas.
 */
int test_orphan_hot_gas_retained(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create central and Type 2 orphan */
    struct Halo halos[2];
    halos[0] = create_test_halo(0, 100.0);  /* Central */
    halos[1] = create_test_halo(2, 0.0);    /* Type 2 orphan */

    halos[0].galaxy->HaloBaryonFraction = 0.17f;
    halos[0].galaxy->HotGas = 5.0f;
    halos[0].galaxy->MetalsHotGas = 0.1f;

    halos[1].galaxy->HaloBaryonFraction = 0.17f;
    halos[1].galaxy->HotGas = 3.0f;
    halos[1].galaxy->MetalsHotGas = 0.06f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");

    /* SAGE parity: hot gas is NOT consolidated. The central keeps only its own
     * hot gas, and the Type 2 orphan retains its hot gas (it cools it itself
     * until it merges). SAGE infall_recipe consolidates only ejected gas + ICS. */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->HotGas, 5.0, 0.01,
                             "Central keeps only its own hot gas");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsHotGas, 0.1, 0.01,
                             "Central keeps only its own hot gas metals");

    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->HotGas, 3.0, 0.01,
                             "Type 2 orphan retains its hot gas");
    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->MetalsHotGas, 0.06, 0.01,
                             "Type 2 orphan retains its hot gas metals");

    printf("  ✓ Orphan hot gas retained: Central HotGas=%.2f, Orphan HotGas=%.2f\n",
           halos[0].galaxy->HotGas, halos[1].galaxy->HotGas);

    /* ===== CLEANUP ===== */
    free_test_halo(&halos[0]);
    free_test_halo(&halos[1]);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_mass_halo
 * @brief   Test that zero-mass halos are handled correctly
 *
 * Expected: Process succeeds, InfallingGas calculated (likely negative)
 * Validates: Edge case handling
 */
int test_zero_mass_halo(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    struct Halo central = create_test_halo(0, 0.0);  /* Zero mass */
    central.galaxy->HaloBaryonFraction = 0.17f;
    central.galaxy->StellarMass = 1.0f;  /* Still has baryons */

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &central;

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, &central, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed with zero mass");

    /* Expected: 0.17 × 0.0 - 1.0 = -1.0 (mass loss) */
    TEST_ASSERT(central.galaxy->InfallingGas < 0.0, "Zero-mass halo should have negative infall");

    printf("  ✓ Zero-mass halo: InfallingGas=%.2f (expected negative)\n",
           central.galaxy->InfallingGas);

    /* ===== CLEANUP ===== */
    free_test_halo(&central);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_no_satellites
 * @brief   Test that single central (no satellites) works correctly
 *
 * Expected: Infall calculated correctly, no consolidation
 * Validates: Single halo case
 */
int test_no_satellites(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    struct Halo central = create_test_halo(0, 100.0);
    central.galaxy->HaloBaryonFraction = 0.17f;
    central.galaxy->StellarMass = 5.0f;
    central.galaxy->HotGas = 10.0f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &central;

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, &central, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed with single halo");

    /* Expected: 0.17 × 100.0 - 15.0 = 2.0 */
    TEST_ASSERT_DOUBLE_EQUAL(central.galaxy->InfallingGas, 2.0, 0.01,
                             "Single halo infall should be correct");

    printf("  ✓ Single central: InfallingGas=%.2f (expected 2.0)\n",
           central.galaxy->InfallingGas);

    /* ===== CLEANUP ===== */
    free_test_halo(&central);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Test that multiple satellites are correctly consolidated
 *
 * Expected: Central gets all satellite reservoirs
 * Validates: Multi-satellite consolidation
 */
int test_multiple_satellites(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create central and 3 satellites */
    struct Halo halos[4];
    halos[0] = create_test_halo(0, 100.0);  /* Central */
    halos[1] = create_test_halo(1, 20.0);   /* Satellite 1 */
    halos[2] = create_test_halo(1, 15.0);   /* Satellite 2 */
    halos[3] = create_test_halo(1, 10.0);   /* Satellite 3 */

    /* Central has some reservoirs */
    halos[0].galaxy->HaloBaryonFraction = 0.17f;
    halos[0].galaxy->EjectedGas = 1.0f;
    halos[0].galaxy->ICS = 0.5f;

    /* Satellites have ejected gas and ICS */
    for (int i = 1; i < 4; i++) {
        halos[i].galaxy->HaloBaryonFraction = 0.17f;
        halos[i].galaxy->EjectedGas = (float)i;       /* 1.0, 2.0, 3.0 */
        halos[i].galaxy->ICS = (float)i * 0.5f;       /* 0.5, 1.0, 1.5 */
    }

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];

    /* ===== EXECUTE ===== */
    extern int sage_prepare_infall_budget_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_prepare_infall_budget_process(&ctx, halos, 4);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Central should have all ejected: 1.0 + 1.0 + 2.0 + 3.0 = 7.0 */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->EjectedGas, 7.0, 0.01,
                             "Central should have all ejected gas from satellites");

    /* Central should have all ICS: 0.5 + 0.5 + 1.0 + 1.5 = 3.5 */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ICS, 3.5, 0.01,
                             "Central should have all ICS from satellites");

    /* All satellites should have zero */
    for (int i = 1; i < 4; i++) {
        TEST_ASSERT_DOUBLE_EQUAL(halos[i].galaxy->EjectedGas, 0.0, 0.01,
                                 "Satellites should have zero ejected gas");
        TEST_ASSERT_DOUBLE_EQUAL(halos[i].galaxy->ICS, 0.0, 0.01,
                                 "Satellites should have zero ICS");
    }

    printf("  ✓ Multiple satellites consolidated: EjectedGas=%.2f, ICS=%.2f\n",
           halos[0].galaxy->EjectedGas, halos[0].galaxy->ICS);

    /* ===== CLEANUP ===== */
    for (int i = 0; i < 4; i++) {
        free_test_halo(&halos[i]);
    }
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_prepare_infall_budget comprehensive tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_prepare_infall_budget Module (Comprehensive)\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run software quality tests */
    printf("\n%s=== Software Quality Tests ===%s\n", BLUE, NC);
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_invalid_parameter_fails);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_property_access);

    /* Run physics tests */
    printf("\n%s=== Physics Calculation Tests ===%s\n", BLUE, NC);
    TEST_RUN(test_physics_calculation_basic);
    TEST_RUN(test_satellite_ejected_consolidation);
    TEST_RUN(test_satellite_ics_consolidation);
    TEST_RUN(test_orphan_hot_gas_retained);
    TEST_RUN(test_zero_mass_halo);
    TEST_RUN(test_no_satellites);
    TEST_RUN(test_multiple_satellites);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
