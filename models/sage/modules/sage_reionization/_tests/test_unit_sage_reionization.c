/**
 * @file    test_unit_sage_reionization.c
 * @brief   Comprehensive unit tests for sage_reionization module
 *
 * Validates: Module lifecycle, physics calculation, memory safety, edge cases, parameter validation
 *
 * This test validates both software engineering and physics aspects:
 * - Module registration and initialization
 * - Parameter reading and validation (including invalid values)
 * - Memory allocation and cleanup (no leaks)
 * - Property setting patterns
 * - Physics calculation (reionization suppression)
 * - Edge cases (zero mass, very small/large mass, different Types)
 * - Redshift dependence
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_invalid_parameter_fails: Invalid parameters rejected
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: HaloBaryonFraction property access
 *   - test_physics_calculation_type0: Physics for Type 0 centrals
 *   - test_physics_calculation_type1_satellite: Physics for Type 1 satellites
 *   - test_zero_mass_halos: Zero mass halos handled correctly
 *   - test_very_small_mass_halos: Very small mass halos
 *   - test_very_large_mass_halos: Very large mass minimal suppression
 *   - test_mass_dependence: Low-mass more suppressed than high-mass
 *   - test_redshift_dependence: Suppression varies with redshift
 *   - test_type3_halos_skipped: Type 3 halos are skipped
 *
 * @author  Mimic Development Team
 * @date    2025-12-17 (Refactored)
 */

#include "../../tests/framework/test_framework.h"
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

/* Test fixture: Set all required model parameters (Parameter system) */
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
    MimicConfig.G = 43007.1;  /* G in code units */

    /* Configure sage_reionization module in pre_timestep phase */
    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_reionization");
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
    halo.Vvir = 100.0;  /* km/s */
    halo.Rvir = 0.1;    /* Mpc/h */

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
 * Note: 1.0 is VALID (inclusive upper bound), so only test 0.0, negative, and >1.0
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
        MimicConfig.pre_timestep[0].module_name = strdup("sage_reionization");
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
        /* Note: module_system_cleanup() not called since init failed */
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

    /* Create module context */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.time = 13.6;
    ctx.snapshot_number = 63;
    ctx.substep_dt = 0.1;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    /* Get module from registry and call process */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

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
 * @test    test_physics_calculation_type0
 * @brief   Test physics calculation for Type 0 central halos
 *
 * Expected: HaloBaryonFraction = GlobalBaryonFraction × reionization_modifier
 * Validates: Correct physics calculation for centrals
 */
int test_physics_calculation_type0(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create Type 0 central with moderate mass */
    struct Halo test_halo = create_test_halo(0, 100.0);  /* 1e12 Msun/h */

    /* Create module context at z=0 */
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction > 0.0f,
                "Type 0 halo should have HaloBaryonFraction > 0");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction <= 0.17f,
                "HaloBaryonFraction should be <= GlobalBaryonFraction");

    /* For massive halo at z=0, suppression should be minimal */
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction > 0.15f,
                "Massive halo at z=0 should have minimal suppression (> 0.15)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_calculation_type1_satellite
 * @brief   Test physics calculation for Type 1 satellite halos
 *
 * Expected: HaloBaryonFraction = GlobalBaryonFraction × f_reion(satellite_Mvir, z)
 * Validates: Per-satellite reionization suppression (SAGE parity fix)
 * SAGE parity: strip_from_satellite() calls do_reionization(gal,...) per satellite
 * using the satellite's own Mvir (model_infall.c:97), so each satellite must
 * receive its own suppression modifier, not the central's.
 */
int test_physics_calculation_type1_satellite(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create Type 1 satellite with moderate mass */
    struct Halo test_halo = create_test_halo(1, 50.0);  /* 5e11 Msun/h */

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction > 0.0f,
                "Type 1 satellite should have HaloBaryonFraction > 0");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction <= 0.17f,
                "Type 1 satellite HaloBaryonFraction should be <= GlobalBaryonFraction");

    /* Verify that repeated calls update the value (not preserve it) */
    float first_value = test_halo.galaxy->HaloBaryonFraction;
    test_halo.galaxy->HaloBaryonFraction = 0.10f;  /* Manually set to a different value */
    result = sage_reionization_process(&ctx, &test_halo, 1);
    TEST_ASSERT(fabs(test_halo.galaxy->HaloBaryonFraction - first_value) < 1e-5f,
                "Type 1 satellite HaloBaryonFraction should be recomputed each call");

    /* Verify low-mass satellite gets stronger suppression than high-mass at z=2 */
    struct Halo low_mass_sat = create_test_halo(1, 0.5);   /* 5e9 Msun/h */
    struct Halo high_mass_sat = create_test_halo(1, 50.0); /* 5e11 Msun/h */

    ctx.redshift = 2.0;

    ctx.central_galaxy = &low_mass_sat;
    sage_reionization_process(&ctx, &low_mass_sat, 1);

    ctx.central_galaxy = &high_mass_sat;
    sage_reionization_process(&ctx, &high_mass_sat, 1);

    TEST_ASSERT(low_mass_sat.galaxy->HaloBaryonFraction < high_mass_sat.galaxy->HaloBaryonFraction,
                "Low-mass satellite should be more suppressed than high-mass satellite");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    free_test_halo(&low_mass_sat);
    free_test_halo(&high_mass_sat);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_mass_halos
 * @brief   Test that zero-mass halos are handled correctly
 *
 * Expected: HaloBaryonFraction = 0 for Mvir = 0
 * Validates: Edge case handling
 */
int test_zero_mass_halos(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create halo with zero mass */
    struct Halo test_halo = create_test_halo(0, 0.0);

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed with zero mass");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction == 0.0f,
                "Zero-mass halo should have HaloBaryonFraction = 0");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_very_small_mass_halos
 * @brief   Test that very small mass halos have strong suppression after reionization
 *
 * Expected: HaloBaryonFraction << GlobalBaryonFraction for low-mass halos
 * Validates: Strong suppression for small halos after reionization
 * Physics: At z=2 (after reionization at z=7), low-mass halos should be strongly suppressed
 *          Code has z0=8 (UV background on), zr=7 (full reionization)
 *          Filtering mass ~ 1e8-1e9 Msun/h, so test with 1e6 Msun/h for strong suppression
 */
int test_very_small_mass_halos(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create very low-mass halo (1e6 Msun/h = 0.1 in code units) */
    struct Halo test_halo = create_test_halo(0, 0.1);

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 2.0;  /* After reionization (zr=7) for maximum suppression effect */
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction > 0.0f,
                "Small halo should have HaloBaryonFraction > 0");

    /* Low-mass halo should be strongly suppressed (< 50% of GlobalBaryonFraction) */
    float actual = test_halo.galaxy->HaloBaryonFraction;
    float suppression_fraction = actual / 0.17f;

    /* Print diagnostic info */
    printf("  Mvir=%.2e (1e9 Msun/h) at z=2.0: HaloBaryonFraction=%.4f (%.1f%% of GlobalBaryonFraction)\n",
           test_halo.Mvir, actual, suppression_fraction * 100.0f);

    /* After reionization, low-mass halos should have significant suppression */
    TEST_ASSERT(suppression_fraction < 0.5f,
                "Small halo (1e9 Msun/h) at z=2 should have strong suppression (< 50% of GlobalBaryonFraction)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_very_large_mass_halos
 * @brief   Test that very large mass halos have minimal suppression
 *
 * Expected: HaloBaryonFraction ≈ GlobalBaryonFraction for massive halos
 * Validates: Minimal suppression for large halos
 */
int test_very_large_mass_halos(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create very massive halo (1e15 Msun/h = 100000 in code units) */
    struct Halo test_halo = create_test_halo(0, 100000.0);

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction > 0.16f,
                "Massive halo should have minimal suppression (> 0.16)");
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction <= 0.17f,
                "HaloBaryonFraction should never exceed GlobalBaryonFraction");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_mass_dependence
 * @brief   Test that low-mass halos are more suppressed than high-mass halos
 *
 * Expected: HaloBaryonFraction increases with Mvir
 * Validates: Correct mass-dependence of suppression
 */
int test_mass_dependence(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create halos with different masses */
    struct Halo low_mass = create_test_halo(0, 1.0);    /* 1e11 Msun/h */
    struct Halo mid_mass = create_test_halo(0, 10.0);   /* 1e12 Msun/h */
    struct Halo high_mass = create_test_halo(0, 100.0); /* 1e13 Msun/h */

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 2.0;  /* Moderate redshift */
    ctx.params = &MimicConfig;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);

    ctx.central_galaxy = &low_mass;
    sage_reionization_process(&ctx, &low_mass, 1);

    ctx.central_galaxy = &mid_mass;
    sage_reionization_process(&ctx, &mid_mass, 1);

    ctx.central_galaxy = &high_mass;
    sage_reionization_process(&ctx, &high_mass, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(low_mass.galaxy->HaloBaryonFraction < mid_mass.galaxy->HaloBaryonFraction,
                "Low-mass halo should be more suppressed than mid-mass");
    TEST_ASSERT(mid_mass.galaxy->HaloBaryonFraction < high_mass.galaxy->HaloBaryonFraction,
                "Mid-mass halo should be more suppressed than high-mass");

    /* ===== CLEANUP ===== */
    free_test_halo(&low_mass);
    free_test_halo(&mid_mass);
    free_test_halo(&high_mass);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_redshift_dependence
 * @brief   Test that suppression varies with redshift for low-mass halos
 *
 * Expected: For LOW-MASS halos, values should vary with redshift
 * Validates: Redshift-dependence of reionization (filtering mass evolution)
 * Physics: Filtering mass evolves with redshift, affecting suppression
 */
int test_redshift_dependence(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create LOW-MASS halos at different redshifts to see redshift effect */
    struct Halo halo_z0 = create_test_halo(0, 1.0);   /* 1e11 Msun/h at z=0 */
    struct Halo halo_z2 = create_test_halo(0, 1.0);   /* 1e11 Msun/h at z=2 */
    struct Halo halo_z8 = create_test_halo(0, 1.0);   /* 1e11 Msun/h at z=8 */

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);

    ctx.redshift = 0.0;
    ctx.central_galaxy = &halo_z0;
    sage_reionization_process(&ctx, &halo_z0, 1);

    ctx.redshift = 2.0;
    ctx.central_galaxy = &halo_z2;
    sage_reionization_process(&ctx, &halo_z2, 1);

    ctx.redshift = 8.0;
    ctx.central_galaxy = &halo_z8;
    sage_reionization_process(&ctx, &halo_z8, 1);

    /* ===== VALIDATE ===== */
    /* All values should be physical */
    TEST_ASSERT(halo_z0.galaxy->HaloBaryonFraction > 0.0f &&
                halo_z0.galaxy->HaloBaryonFraction <= 0.17f,
                "z=0 HaloBaryonFraction should be physical");
    TEST_ASSERT(halo_z2.galaxy->HaloBaryonFraction > 0.0f &&
                halo_z2.galaxy->HaloBaryonFraction <= 0.17f,
                "z=2 HaloBaryonFraction should be physical");
    TEST_ASSERT(halo_z8.galaxy->HaloBaryonFraction > 0.0f &&
                halo_z8.galaxy->HaloBaryonFraction <= 0.17f,
                "z=8 HaloBaryonFraction should be physical");

    /* Print diagnostic info to understand redshift dependence */
    float frac_z0 = halo_z0.galaxy->HaloBaryonFraction;
    float frac_z2 = halo_z2.galaxy->HaloBaryonFraction;
    float frac_z8 = halo_z8.galaxy->HaloBaryonFraction;

    printf("  Redshift dependence for Mvir=%.2e:\n", halo_z0.Mvir);
    printf("    z=0.0: HaloBaryonFraction = %.4f (%.1f%% of GlobalBaryonFraction)\n",
           frac_z0, frac_z0 / 0.17f * 100.0f);
    printf("    z=2.0: HaloBaryonFraction = %.4f (%.1f%% of GlobalBaryonFraction)\n",
           frac_z2, frac_z2 / 0.17f * 100.0f);
    printf("    z=8.0: HaloBaryonFraction = %.4f (%.1f%% of GlobalBaryonFraction)\n",
           frac_z8, frac_z8 / 0.17f * 100.0f);

    /* For low-mass halos, values should differ across redshifts (redshift-dependence exists) */
    /* We verify that NOT all values are identical (would indicate no redshift dependence) */
    int all_identical = (frac_z0 == frac_z2) && (frac_z2 == frac_z8);
    TEST_ASSERT(!all_identical,
                "HaloBaryonFraction should vary with redshift (not all identical)");

    /* ===== CLEANUP ===== */
    free_test_halo(&halo_z0);
    free_test_halo(&halo_z2);
    free_test_halo(&halo_z8);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_type3_halos_skipped
 * @brief   Test that Type 3 halos are skipped
 *
 * Expected: Type 3 halos are not processed
 * Validates: Type 3 handling
 */
int test_type3_halos_skipped(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(0.17);

    /* Create Type 3 halo */
    struct Halo test_halo = create_test_halo(3, 50.0);

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.redshift = 0.0;
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    extern int sage_reionization_process(struct ModuleContext *, struct Halo *, int);
    int result = sage_reionization_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    /* Type 3 halos should be skipped, HaloBaryonFraction remains at sentinel value */
    TEST_ASSERT(test_halo.galaxy->HaloBaryonFraction == -1.0f,
                "Type 3 halo should be skipped (HaloBaryonFraction unchanged)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_reionization comprehensive tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_reionization Module (Comprehensive)\n");
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
    TEST_RUN(test_physics_calculation_type0);
    TEST_RUN(test_physics_calculation_type1_satellite);
    TEST_RUN(test_zero_mass_halos);
    TEST_RUN(test_very_small_mass_halos);
    TEST_RUN(test_very_large_mass_halos);
    TEST_RUN(test_mass_dependence);
    TEST_RUN(test_redshift_dependence);
    TEST_RUN(test_type3_halos_skipped);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
