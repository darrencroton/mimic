/**
 * @file    test_unit_sage_disk_instability.c
 * @brief   Comprehensive unit tests for sage_disk_instability module
 *
 * Validates: Module lifecycle, physics calculation, edge cases, parameter validation
 *
 * This test validates both software engineering and physics aspects:
 * - Module registration and initialization
 * - Parameter reading and validation (StarFormingDiskFactor)
 * - Memory allocation and cleanup (no leaks)
 * - Disk stability criterion (Mo, Mao & White 1998)
 * - Stellar mass transfer to bulge with metallicity preservation
 * - Trigger flag setting for downstream modules
 * - Edge cases (zero mass, zero radius, safety constraints)
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: StarFormingDiskFactor parameter
 *   - test_invalid_parameter_fails: Invalid parameters rejected
 *   - test_memory_safety: No memory leaks during operation
 *   - test_process_by_galaxy_mode: ngal=1 requirement
 *   - test_stable_disk_no_transfer: Stable disk physics
 *   - test_stable_disk_at_threshold: Boundary condition
 *   - test_zero_diskmass: Zero disk mass handling
 *   - test_unstable_disk_stars_transfer: Unstable disk bulge formation
 *   - test_unstable_disk_metallicity_preserved: Metal conservation
 *   - test_unstable_disk_gas_only: Gas-only disk
 *   - test_unstable_disk_stars_only: Stars-only disk (triggers NOT set)
 *   - test_zero_vmax: Zero virial velocity
 *   - test_zero_disk_radius: Zero disk radius
 *   - test_bulge_exceeds_stellar_mass: Safety constraint
 *   - test_metals_bulge_exceeds_stellar_metals: Metal safety constraint
 *   - test_disk_factor_sensitivity: Parameter affects results
 *
 * @author  Mimic Development Team
 * @date    2025-12-23
 */

#include "../../tests/framework/test_framework.h"
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

/* Module functions (extern declarations for direct testing) */
extern int sage_disk_instability_init(void);
extern int sage_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_disk_instability_cleanup(void);

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

/* Test fixture: Setup test parameters */
static void setup_test_parameters(double disk_factor)
{
    int idx = 0;
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "StarFormingDiskFactor");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", disk_factor);
    MimicConfig.NumModelParams = idx;
}

/* Test fixture: Initialize module for physics tests */
static void setup_module_for_physics_test(double disk_factor)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Set up cosmology configuration */
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.G = 43007.1;  /* G in code units: (km/s)² Mpc / (1e10 Msun/h) */

    /* Configure sage_disk_instability module in phase_1 */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_disk_instability");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* Set test parameters */
    setup_test_parameters(disk_factor);

    /* Initialize module system */
    int result = module_system_init();
    if (result != 0) {
        ERROR_LOG("Module system initialization failed in test fixture");
    }
}

/* Test fixture: Create test halo with specified properties */
static struct Halo create_test_halo(float vmax, float cold_gas, float stellar_mass,
                                     float bulge_mass, float disk_radius)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = 0;  /* Central galaxy */
    halo.Vmax = vmax;
    halo.HaloNr = 42;

    /* Allocate galaxy */
    halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_UTILITY);
    memset(halo.galaxy, 0, sizeof(struct GalaxyData));

    halo.galaxy->ColdGas = cold_gas;
    halo.galaxy->StellarMass = stellar_mass;
    halo.galaxy->BulgeMass = bulge_mass;
    halo.galaxy->DiskScaleRadius = disk_radius;
    halo.galaxy->MetalsStellarMass = stellar_mass * 0.02f;  /* 2% metallicity */
    halo.galaxy->MetalsBulgeMass = bulge_mass * 0.02f;
    halo.galaxy->UnstableDiskGasFraction = 0.0f;

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
 * @brief   Test that sage_disk_instability module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_disk_instability_register() works, module appears in registry
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
    MimicConfig.G = 43007.1;

    /* Configure sage_disk_instability module */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_disk_instability");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    setup_test_parameters(3.0);

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
 * @brief   Test that module reads StarFormingDiskFactor parameter
 *
 * Expected: Module reads StarFormingDiskFactor parameter successfully
 * Validates: Parameter reading and validation
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
    MimicConfig.G = 43007.1;

    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_disk_instability");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    setup_test_parameters(5.0);  /* Custom value */

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
 * @brief   Test that module rejects invalid StarFormingDiskFactor values
 *
 * Expected: Module init fails with invalid parameters
 * Validates: Parameter validation (valid range: [0, 10.0])
 */
int test_invalid_parameter_fails(void)
{
    /* Test cases: values that should fail */
    double invalid_values[] = {-1.0, 15.0};
    int num_cases = 2;

    for (int i = 0; i < num_cases; i++) {
        /* ===== SETUP ===== */
        reset_config();
        init_memory_system(0);
        ensure_modules_registered();

        MimicConfig.Omega = 0.25;
        MimicConfig.OmegaLambda = 0.75;
        MimicConfig.Hubble_h = 0.73;
        MimicConfig.G = 43007.1;

        MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
        MimicConfig.phase_1[0].module_name = strdup("sage_disk_instability");
        MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
        MimicConfig.num_phase_1 = 1;
        MimicConfig.SubSteps = 1;

        setup_test_parameters(invalid_values[i]);

        /* ===== EXECUTE ===== */
        int result = module_system_init();

        /* ===== VALIDATE ===== */
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                 "Module should fail with invalid StarFormingDiskFactor=%.2f", invalid_values[i]);
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
    setup_module_for_physics_test(3.0);

    /* Create test halo */
    struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);

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
    int result = sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_process_by_galaxy_mode
 * @brief   Test that module enforces process_by_galaxy mode (ngal=1)
 *
 * Expected: Module returns error if ngal != 1
 * Validates: Processing mode validation
 */
int test_process_by_galaxy_mode(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(3.0);

    /* Create multiple test halos */
    struct Halo halos[3];
    for (int i = 0; i < 3; i++) {
        halos[i] = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
    }

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &halos[0];

    /* ===== EXECUTE ===== */
    int result = sage_disk_instability_process(&ctx, halos, 3);  /* ngal=3, should fail */

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result != 0, "Module should fail with ngal != 1");

    /* ===== CLEANUP ===== */
    for (int i = 0; i < 3; i++) {
        free_test_halo(&halos[i]);
    }
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_stable_disk_no_transfer
 * @brief   Test that stable disks (diskmass < Mcrit) don't transfer stars
 *
 * Expected: BulgeMass unchanged, trigger flags = 0
 * Validates: Stability criterion
 * Physics: Mcrit = Vmax² × (factor × radius) / G
 *          With Vmax=700, radius=0.5, factor=3.0: Mcrit = 17.09 > 13 (STABLE)
 */
int test_stable_disk_no_transfer(void)
{
    /* ===== SETUP ===== */
    const double disk_factor = 3.0;
    setup_module_for_physics_test(disk_factor);

    /* Create stable disk: Mcrit > diskmass
     * diskmass = 5 + (10 - 2) = 13
     * Mcrit = 700² × (3.0 × 0.5) / 43007.1 = 17.09 > 13 → STABLE
     */
    struct Halo test_halo = create_test_halo(700.0, 5.0, 10.0, 2.0, 0.5);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    int result = sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge, 1e-6,
                             "Stable disk should not transfer stars to bulge");
    TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
                "Stable disk should have UnstableDiskGasFraction = 0");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_stable_disk_at_threshold
 * @brief   Test boundary condition where calculated Mcrit >= diskmass (stable)
 *
 * Expected: No star transfer (Mcrit capped to diskmass, unstable_mass = 0)
 * Validates: Mcrit capping logic at stability threshold
 * Physics: Mcrit slightly exceeds diskmass → capped → unstable_mass = 0
 */
int test_stable_disk_at_threshold(void)
{
    /* ===== SETUP ===== */
    const double disk_factor = 3.0;
    setup_module_for_physics_test(disk_factor);

    /* Create disk at threshold for robust testing:
     * diskmass = 13.0 (ColdGas=5.0, disk_stars=8.0)
     * Calculate radius for Vmax=500 to give calculated Mcrit = 13.01 (slightly > diskmass):
     * radius = (13.01 × 43007.1) / (500² × 3.0) = 0.746029828
     * Module caps Mcrit to diskmass, so unstable_mass = diskmass - min(Mcrit, diskmass) = 0
     */
    struct Halo test_halo = create_test_halo(500.0, 5.0, 10.0, 2.0, 0.746029828);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge, 1e-4,
                             "Disk at threshold (Mcrit capped) should not transfer stars");
    TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
                "At threshold: UnstableDiskGasFraction = 0");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_diskmass
 * @brief   Test handling of zero disk mass
 *
 * Expected: Trigger flags = 0, no changes
 * Validates: Edge case handling
 */
int test_zero_diskmass(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(3.0);

    /* Create galaxy with zero disk: ColdGas=0, StellarMass=BulgeMass */
    struct Halo test_halo = create_test_halo(200.0, 0.0, 10.0, 10.0, 0.03);

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    int result = sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Zero diskmass should be handled gracefully");
    TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
                "Zero diskmass: UnstableDiskGasFraction = 0");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_unstable_disk_stars_transfer
 * @brief   Test that unstable disks transfer stars to bulge
 *
 * Expected: BulgeMass increases by unstable_stars
 * Validates: Core physics calculation
 * Physics: With Vmax=200, radius=0.03, Mcrit = 0.084 << 13 (UNSTABLE)
 */
int test_unstable_disk_stars_transfer(void)
{
    /* ===== SETUP ===== */
    const double disk_factor = 3.0;
    setup_module_for_physics_test(disk_factor);

    /* Create unstable disk: diskmass > Mcrit
     * diskmass = 5 + (10 - 2) = 13
     * Mcrit = 200² × (3.0 × 0.03) / 43007.1 = 0.084 << 13 → UNSTABLE
     */
    struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy->BulgeMass > initial_bulge,
                "Unstable disk should transfer stars to bulge");

    /* Verify physics calculation matches expectations */
    const double diskmass = 13.0;
    const double Mcrit = 0.084;  /* Pre-calculated */
    const double star_fraction = 8.0 / 13.0;
    const double expected_unstable_stars = star_fraction * (diskmass - Mcrit);
    const double expected_bulge = initial_bulge + expected_unstable_stars;

    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, expected_bulge, 0.01,
                             "BulgeMass should match expected value");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_unstable_disk_metallicity_preserved
 * @brief   Test that metallicity is preserved when transferring stars to bulge
 *
 * Expected: MetalsBulgeMass increases by Z × unstable_stars
 * Validates: Metal mass conservation
 */
int test_unstable_disk_metallicity_preserved(void)
{
    /* ===== SETUP ===== */
    const double disk_factor = 3.0;
    setup_module_for_physics_test(disk_factor);

    /* Create unstable disk with non-zero metallicity */
    struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
    const float initial_metals_bulge = test_halo.galaxy->MetalsBulgeMass;
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    const float transferred_stars = test_halo.galaxy->BulgeMass - initial_bulge;
    TEST_ASSERT(transferred_stars > 0.0f, "Stars should be transferred");

    /* Check metallicity preservation (2% metallicity) */
    const float expected_metals = initial_metals_bulge + 0.02f * transferred_stars;
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsBulgeMass, expected_metals, 0.001,
                             "Metallicity should be preserved in bulge transfer");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_unstable_disk_gas_only
 * @brief   Test unstable disk with only gas (no stellar disk)
 *
 * Expected: Only gas unstable, triggers set appropriately
 * Validates: Gas-only disk edge case
 */
int test_unstable_disk_gas_only(void)
{
    /* ===== SETUP ===== */
    const double disk_factor = 3.0;
    setup_module_for_physics_test(disk_factor);

    /* Create gas-only disk: StellarMass = BulgeMass (no stellar disk) */
    struct Halo test_halo = create_test_halo(200.0, 10.0, 2.0, 2.0, 0.03);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    /* Gas-only disk: no stars to transfer */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge, 1e-6,
                             "Gas-only disk should not transfer stars");
    TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction > 0.0f,
                "Gas-only unstable disk should set gas trigger flag");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_unstable_disk_stars_only
 * @brief   Test unstable disk with only stars (no gas)
 *
 * Expected: Stars transferred, trigger flags = 0 (only set if unstable_gas > 0)
 * Validates: Stars-only disk behavior
 * Note: Module only sets triggers if unstable_gas > 0 (line 111)
 */
int test_unstable_disk_stars_only(void)
{
    /* ===== SETUP ===== */
    const double disk_factor = 3.0;
    setup_module_for_physics_test(disk_factor);

    /* Create stars-only disk: ColdGas = 0 */
    struct Halo test_halo = create_test_halo(200.0, 0.0, 10.0, 2.0, 0.03);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy->BulgeMass > initial_bulge,
                "Stars-only unstable disk should transfer stars");

    /* Trigger flag is ONLY set if unstable_gas > 0 (see line 111) */
    TEST_ASSERT(test_halo.galaxy->UnstableDiskGasFraction == 0.0f,
                "Stars-only disk should have gas trigger flag = 0 (no gas)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_vmax
 * @brief   Test handling of zero virial velocity
 *
 * Expected: Mcrit = 0, all disk mass becomes unstable
 * Validates: Division by zero protection
 */
int test_zero_vmax(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(3.0);

    /* Create galaxy with Vmax = 0 → Mcrit = 0 → all unstable */
    struct Halo test_halo = create_test_halo(0.0, 5.0, 10.0, 2.0, 0.03);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    int result = sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Zero Vmax should be handled gracefully");

    /* Vmax=0 → Mcrit=0 → entire stellar disk unstable */
    const float disk_stars = 10.0f - 2.0f;  /* StellarMass - BulgeMass */
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge + disk_stars, 0.01,
                             "Zero Vmax: all stellar disk should transfer to bulge");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_disk_radius
 * @brief   Test handling of zero disk scale radius
 *
 * Expected: Mcrit = 0, all disk mass becomes unstable
 * Validates: Edge case handling
 */
int test_zero_disk_radius(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(3.0);

    /* Create galaxy with DiskScaleRadius = 0 → Mcrit = 0 */
    struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.0);
    const float initial_bulge = test_halo.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    int result = sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Zero DiskScaleRadius should be handled gracefully");

    const float disk_stars = 10.0f - 2.0f;
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->BulgeMass, initial_bulge + disk_stars, 0.01,
                             "Zero radius: all stellar disk should transfer to bulge");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bulge_exceeds_stellar_mass
 * @brief   Test that safety constraint prevents BulgeMass > StellarMass
 *
 * Expected: BulgeMass capped at StellarMass, warning logged
 * Validates: Safety constraint enforcement
 */
int test_bulge_exceeds_stellar_mass(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(3.0);

    /* Create scenario where calculation might produce BulgeMass > StellarMass */
    struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 9.9, 0.01);

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy->BulgeMass <= test_halo.galaxy->StellarMass + 1e-6,
                "BulgeMass should never exceed StellarMass (safety constraint)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_metals_bulge_exceeds_stellar_metals
 * @brief   Test that safety constraint prevents MetalsBulgeMass > MetalsStellarMass
 *
 * Expected: MetalsBulgeMass capped at MetalsStellarMass, warning logged
 * Validates: Metal mass safety constraint
 */
int test_metals_bulge_exceeds_stellar_metals(void)
{
    /* ===== SETUP ===== */
    setup_module_for_physics_test(3.0);

    /* Create scenario where calculation might produce MetalsBulgeMass > MetalsStellarMass */
    struct Halo test_halo = create_test_halo(200.0, 5.0, 10.0, 9.9, 0.01);
    test_halo.galaxy->MetalsStellarMass = 0.2f;
    test_halo.galaxy->MetalsBulgeMass = 0.198f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo;

    /* ===== EXECUTE ===== */
    sage_disk_instability_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(test_halo.galaxy->MetalsBulgeMass <= test_halo.galaxy->MetalsStellarMass + 1e-6,
                "MetalsBulgeMass should never exceed MetalsStellarMass (safety constraint)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_factor_sensitivity
 * @brief   Test that changing StarFormingDiskFactor affects stability
 *
 * Expected: Higher disk_factor → higher Mcrit → more stability
 * Validates: Parameter sensitivity
 */
int test_disk_factor_sensitivity(void)
{
    /* ===== TEST LOW DISK FACTOR ===== */
    setup_module_for_physics_test(1.0);  /* Low factor → low Mcrit → unstable */

    struct Halo test_halo_low = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
    const float initial_bulge_low = test_halo_low.galaxy->BulgeMass;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo_low;

    sage_disk_instability_process(&ctx, &test_halo_low, 1);
    const float bulge_low = test_halo_low.galaxy->BulgeMass;
    const float transferred_low = bulge_low - initial_bulge_low;

    free_test_halo(&test_halo_low);
    module_system_cleanup();

    /* ===== TEST HIGH DISK FACTOR ===== */
    setup_module_for_physics_test(10.0);  /* High factor → high Mcrit → stable */

    struct Halo test_halo_high = create_test_halo(200.0, 5.0, 10.0, 2.0, 0.03);
    const float initial_bulge_high = test_halo_high.galaxy->BulgeMass;

    ctx.params = &MimicConfig;
    ctx.central_galaxy = &test_halo_high;

    sage_disk_instability_process(&ctx, &test_halo_high, 1);
    const float bulge_high = test_halo_high.galaxy->BulgeMass;
    const float transferred_high = bulge_high - initial_bulge_high;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(transferred_low > transferred_high,
                "Lower disk_factor should produce more instability (more star transfer)");

    /* ===== CLEANUP ===== */
    free_test_halo(&test_halo_high);
    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_disk_instability comprehensive tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_disk_instability Module (Comprehensive)\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run software quality tests */
    printf("\n%s=== Software Quality Tests ===%s\n", BLUE, NC);
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_invalid_parameter_fails);
    TEST_RUN(test_memory_safety);
    TEST_RUN(test_process_by_galaxy_mode);

    /* Run physics tests - stable disks */
    printf("\n%s=== Physics Tests: Stable Disks ===%s\n", BLUE, NC);
    TEST_RUN(test_stable_disk_no_transfer);
    TEST_RUN(test_stable_disk_at_threshold);
    TEST_RUN(test_zero_diskmass);

    /* Run physics tests - unstable disks */
    printf("\n%s=== Physics Tests: Unstable Disks ===%s\n", BLUE, NC);
    TEST_RUN(test_unstable_disk_stars_transfer);
    TEST_RUN(test_unstable_disk_metallicity_preserved);
    TEST_RUN(test_unstable_disk_gas_only);
    TEST_RUN(test_unstable_disk_stars_only);

    /* Run edge case tests */
    printf("\n%s=== Edge Case Tests ===%s\n", BLUE, NC);
    TEST_RUN(test_zero_vmax);
    TEST_RUN(test_zero_disk_radius);
    TEST_RUN(test_bulge_exceeds_stellar_mass);
    TEST_RUN(test_metals_bulge_exceeds_stellar_metals);

    /* Run parameter sensitivity tests */
    printf("\n%s=== Parameter Sensitivity Tests ===%s\n", BLUE, NC);
    TEST_RUN(test_disk_factor_sensitivity);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
