/**
 * @file    test_unit_sage_update_disk_radius.c
 * @brief   Unit tests for sage_update_disk_radius module
 *
 * Validates: Physics calculation logic, edge cases, parameter handling
 *
 * This test validates the sage_update_disk_radius module physics:
 * - Disk radius calculation (Mo, Mao & White 1998 model)
 * - Spin parameter calculation (Bullock-style λ)
 * - Edge cases (zero spin, zero velocity, zero radius)
 * - Fallback logic (invalid virial properties)
 * - Type filtering (Type 0/1 processed, Type 2+ skipped)
 * - Module lifecycle and memory safety
 *
 * Test cases:
 *   - test_disk_radius_normal_halo: Normal disk radius calculation
 *   - test_disk_radius_high_spin: High spin parameter case
 *   - test_disk_radius_low_spin: Low spin parameter case
 *   - test_disk_radius_zero_spin: Zero spin components
 *   - test_disk_radius_zero_vvir: Zero virial velocity fallback
 *   - test_disk_radius_zero_rvir: Zero virial radius fallback
 *   - test_disk_radius_small_vvir_rvir: Small virial properties fallback
 *   - test_disk_radius_type2_skipped: Type 2 galaxy filtering
 *   - test_disk_radius_null_galaxy: Null pointer safety
 *   - test_disk_radius_multiple_galaxies: Array processing
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../tests/framework/test_framework.h"
#include "../core/module_registry.h"
#include "../core/module_interface.h"
#include "../include/types.h"
#include "../include/proto.h"
#include "../include/globals.h"
#include "../include/constants.h"
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
extern int sage_update_disk_radius_init(void);
extern int sage_update_disk_radius_process(struct ModuleContext *ctx,
                                            struct Halo *halos, int ngal);
extern int sage_update_disk_radius_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Reset global configuration state
 */
static void reset_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

/**
 * @brief   Ensure modules are registered (only once)
 */
static void ensure_modules_registered(void)
{
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/**
 * @brief   Setup test galaxy with specified properties
 *
 * @param   halo        Halo structure to initialize
 * @param   gal         Galaxy structure to initialize
 * @param   spin_x      Spin x-component [km/s * Mpc/h]
 * @param   spin_y      Spin y-component [km/s * Mpc/h]
 * @param   spin_z      Spin z-component [km/s * Mpc/h]
 * @param   vvir        Virial velocity [km/s]
 * @param   rvir        Virial radius [Mpc/h]
 * @param   type        Halo type (0=central, 1=satellite, 2=orphan)
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                               float spin_x, float spin_y, float spin_z,
                               float vvir, float rvir, int type)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = type;
    halo->Spin[0] = spin_x;
    halo->Spin[1] = spin_y;
    halo->Spin[2] = spin_z;
    halo->Vvir = vvir;
    halo->Rvir = rvir;
    halo->Mvir = 100.0;  /* 10^12 Msun/h */
    halo->SnapNum = 63;
    halo->galaxy = gal;

    gal->DiskScaleRadius = -1.0;  /* Sentinel value to check if modified */
}

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx         Context to initialize
 * @param   dt          Time step
 */
static void setup_test_context(struct ModuleContext *ctx, double dt)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->substep_dt = dt;
    ctx->redshift = 0.0;
    ctx->time = 13.8;
    ctx->snapshot_number = 63;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
    ctx->params = &MimicConfig;
}

/**
 * @brief   Calculate expected disk radius for validation
 *
 * Reproduces module logic for comparison.
 *
 * @param   spin_x      Spin x-component [km/s * Mpc/h]
 * @param   spin_y      Spin y-component [km/s * Mpc/h]
 * @param   spin_z      Spin z-component [km/s * Mpc/h]
 * @param   vvir        Virial velocity [km/s]
 * @param   rvir        Virial radius [Mpc/h]
 * @return  Expected disk scale radius [Mpc/h]
 */
static float calculate_expected_disk_radius(float spin_x, float spin_y, float spin_z,
                                              float vvir, float rvir)
{
    if (vvir > EPSILON_SMALL && rvir > EPSILON_SMALL) {
        const float spin_mag = sqrtf(spin_x * spin_x + spin_y * spin_y + spin_z * spin_z);
        const float lambda = spin_mag / (1.414213562f * vvir * rvir);
        return (lambda / 1.414213562f) * rvir;
    } else {
        return 0.1f * rvir;
    }
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_disk_radius_normal_halo
 * @brief   Test disk radius calculation for normal halo with valid properties
 *
 * Expected: DiskScaleRadius = (λ / √2) * Rvir
 * Validates: Core physics calculation
 */
int test_disk_radius_normal_halo(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    int result = sage_update_disk_radius_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create test galaxy with typical properties */
    struct Halo halo;
    struct GalaxyData gal;
    const float spin_x = 100.0f;   /* km/s * Mpc/h */
    const float spin_y = 150.0f;
    const float spin_z = 200.0f;
    const float vvir = 200.0f;     /* km/s */
    const float rvir = 0.2f;       /* Mpc/h */
    setup_test_galaxy(&halo, &gal, spin_x, spin_y, spin_z, vvir, rvir, 0);

    /* Setup context */
    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* Calculate expected disk radius */
    const float expected_rd = calculate_expected_disk_radius(spin_x, spin_y, spin_z,
                                                               vvir, rvir);

    /* ===== EXECUTE ===== */
    result = sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");
    TEST_ASSERT(gal.DiskScaleRadius > 0.0f, "DiskScaleRadius should be positive");
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, (double)expected_rd, 1e-6,
                             "DiskScaleRadius should match expected value");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_high_spin
 * @brief   Test disk radius calculation for high spin parameter (λ ~ 0.1)
 *
 * Expected: Larger disk radius
 * Validates: High spin regime
 */
int test_disk_radius_high_spin(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create galaxy with high spin parameter
     * λ = |J| / (√2 * Vvir * Rvir)
     * For λ ~ 0.1: |J| ~ 0.1 * √2 * Vvir * Rvir
     *              |J| ~ 0.1 * 1.414 * 200 * 0.2 ~ 5.66
     */
    struct Halo halo;
    struct GalaxyData gal;
    const float spin_x = 3.0f;
    const float spin_y = 4.0f;
    const float spin_z = 0.0f;     /* |J| = 5.0 */
    const float vvir = 200.0f;
    const float rvir = 0.2f;
    setup_test_galaxy(&halo, &gal, spin_x, spin_y, spin_z, vvir, rvir, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const float expected_rd = calculate_expected_disk_radius(spin_x, spin_y, spin_z,
                                                               vvir, rvir);

    /* ===== EXECUTE ===== */
    sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.DiskScaleRadius > 0.0f, "High spin should give positive radius");
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, (double)expected_rd, 1e-6,
                             "DiskScaleRadius should match expected value");

    /* Disk radius should be reasonable fraction of Rvir (typically 0.01-0.1 * Rvir) */
    TEST_ASSERT(gal.DiskScaleRadius < rvir, "Disk radius should be < Rvir");
    TEST_ASSERT(gal.DiskScaleRadius > 0.001f * rvir, "Disk radius should be > 0.001*Rvir");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_low_spin
 * @brief   Test disk radius calculation for low spin parameter (λ ~ 0.01)
 *
 * Expected: Smaller disk radius
 * Validates: Low spin regime
 */
int test_disk_radius_low_spin(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create galaxy with low spin parameter */
    struct Halo halo;
    struct GalaxyData gal;
    const float spin_x = 0.3f;
    const float spin_y = 0.4f;
    const float spin_z = 0.0f;     /* |J| = 0.5 (10x smaller than high spin) */
    const float vvir = 200.0f;
    const float rvir = 0.2f;
    setup_test_galaxy(&halo, &gal, spin_x, spin_y, spin_z, vvir, rvir, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const float expected_rd = calculate_expected_disk_radius(spin_x, spin_y, spin_z,
                                                               vvir, rvir);

    /* ===== EXECUTE ===== */
    sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.DiskScaleRadius > 0.0f, "Low spin should still give positive radius");
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, (double)expected_rd, 1e-6,
                             "DiskScaleRadius should match expected value");

    /* Low spin should give smaller radius than typical */
    TEST_ASSERT(gal.DiskScaleRadius < 0.01f * rvir, "Low spin should give small radius");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_zero_spin
 * @brief   Test disk radius when all spin components are zero
 *
 * Expected: DiskScaleRadius = 0 (λ = 0)
 * Validates: Zero spin handling
 */
int test_disk_radius_zero_spin(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create galaxy with zero spin */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0.0f, 0.0f, 0.0f, 200.0f, 0.2f, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero spin gracefully");
    TEST_ASSERT(gal.DiskScaleRadius == 0.0f, "Zero spin should give zero disk radius");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_zero_vvir
 * @brief   Test disk radius when Vvir is zero (edge case)
 *
 * Expected: Fallback → DiskScaleRadius = 0.1 * Rvir
 * Validates: Edge case fallback logic
 */
int test_disk_radius_zero_vvir(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create galaxy with zero virial velocity */
    struct Halo halo;
    struct GalaxyData gal;
    const float rvir = 0.2f;
    setup_test_galaxy(&halo, &gal, 100.0f, 100.0f, 100.0f, 0.0f, rvir, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const float expected_rd = 0.1f * rvir;  /* Fallback value */

    /* ===== EXECUTE ===== */
    int result = sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero Vvir gracefully");
    TEST_ASSERT(halo.Vvir == 0.0f, "Vvir is zero");
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, (double)expected_rd, 1e-6,
                             "Should use fallback: 0.1 * Rvir");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_zero_rvir
 * @brief   Test disk radius when Rvir is zero (edge case)
 *
 * Expected: Fallback → DiskScaleRadius = 0.0 (0.1 * 0 = 0)
 * Validates: Edge case fallback logic
 */
int test_disk_radius_zero_rvir(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create galaxy with zero virial radius */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 100.0f, 100.0f, 100.0f, 200.0f, 0.0f, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const float expected_rd = 0.0f;  /* Fallback: 0.1 * 0 = 0 */

    /* ===== EXECUTE ===== */
    int result = sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero Rvir gracefully");
    TEST_ASSERT(halo.Rvir == 0.0f, "Rvir is zero");
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, (double)expected_rd, 1e-6,
                             "Should give zero disk radius");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_small_vvir_rvir
 * @brief   Test disk radius when Vvir and Rvir are very small (< EPSILON_SMALL)
 *
 * Expected: Fallback → DiskScaleRadius = 0.1 * Rvir
 * Validates: Epsilon threshold logic
 */
int test_disk_radius_small_vvir_rvir(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create galaxy with very small virial properties */
    struct Halo halo;
    struct GalaxyData gal;
    const float vvir = 1e-12f;     /* < EPSILON_SMALL */
    const float rvir = 1e-12f;     /* < EPSILON_SMALL */
    setup_test_galaxy(&halo, &gal, 100.0f, 100.0f, 100.0f, vvir, rvir, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const float expected_rd = 0.1f * rvir;  /* Fallback value */

    /* ===== EXECUTE ===== */
    sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, (double)expected_rd, 1e-15,
                             "Should use fallback for small virial properties");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_type2_skipped
 * @brief   Test that Type 2 (orphan) galaxies are skipped
 *
 * Expected: DiskScaleRadius unchanged (sentinel value)
 * Validates: Type filtering logic
 */
int test_disk_radius_type2_skipped(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create Type 2 galaxy */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, 2);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const float initial_rd = gal.DiskScaleRadius;  /* Should be -1.0 (sentinel) */

    /* ===== EXECUTE ===== */
    int result = sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halo.Type == 2, "Type is 2");
    TEST_ASSERT(gal.DiskScaleRadius == initial_rd,
                "Type 2 galaxy should not have DiskScaleRadius modified");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_null_galaxy
 * @brief   Test that null galaxy pointer is handled gracefully
 *
 * Expected: No crash, function returns successfully
 * Validates: Null pointer safety
 */
int test_disk_radius_null_galaxy(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create halo with null galaxy pointer */
    struct Halo halo;
    memset(&halo, 0, sizeof(struct Halo));
    halo.Type = 0;
    halo.Spin[0] = 100.0f;
    halo.Spin[1] = 100.0f;
    halo.Spin[2] = 100.0f;
    halo.Vvir = 200.0f;
    halo.Rvir = 0.2f;
    halo.galaxy = NULL;  /* Null pointer */

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle null galaxy gracefully");
    TEST_ASSERT(halo.galaxy == NULL, "Galaxy pointer is null");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_radius_multiple_galaxies
 * @brief   Test processing array of multiple galaxies
 *
 * Expected: All galaxies processed correctly
 * Validates: Array loop logic
 */
int test_disk_radius_multiple_galaxies(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Create array of 3 galaxies with different properties */
    const int ngal = 3;
    struct Halo halos[3];
    struct GalaxyData gals[3];

    /* Galaxy 0: Normal */
    setup_test_galaxy(&halos[0], &gals[0], 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, 0);

    /* Galaxy 1: High spin */
    setup_test_galaxy(&halos[1], &gals[1], 300.0f, 400.0f, 0.0f, 200.0f, 0.2f, 0);

    /* Galaxy 2: Type 2 (should be skipped) */
    setup_test_galaxy(&halos[2], &gals[2], 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, 2);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* Calculate expected values */
    const float expected_rd0 = calculate_expected_disk_radius(100.0f, 100.0f, 100.0f,
                                                                200.0f, 0.2f);
    const float expected_rd1 = calculate_expected_disk_radius(300.0f, 400.0f, 0.0f,
                                                                200.0f, 0.2f);

    /* ===== EXECUTE ===== */
    int result = sage_update_disk_radius_process(&ctx, halos, ngal);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed for array");

    /* Galaxy 0: Should be processed */
    TEST_ASSERT_DOUBLE_EQUAL((double)gals[0].DiskScaleRadius, (double)expected_rd0, 1e-6,
                             "Galaxy 0 should have correct disk radius");

    /* Galaxy 1: Should be processed */
    TEST_ASSERT_DOUBLE_EQUAL((double)gals[1].DiskScaleRadius, (double)expected_rd1, 1e-6,
                             "Galaxy 1 should have correct disk radius");

    /* Galaxy 2: Should be skipped (Type 2) */
    TEST_ASSERT(gals[2].DiskScaleRadius == -1.0f,
                "Galaxy 2 (Type 2) should not be modified");

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MODULE INFRASTRUCTURE TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors
 * Validates: Module lifecycle management
 */
int test_module_initialization(void)
{
    /* ===== SETUP ===== */
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.pre_timestep[0].module_name = strdup("sage_update_disk_radius");
    MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_pre_timestep = 1;
    MimicConfig.SubSteps = 1;

    /* No parameters needed for this module */
    MimicConfig.NumModelParams = 0;

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
 * @brief   Test that module doesn't leak memory during operation
 *
 * Expected: No memory leaks after init/process/cleanup cycle
 * Validates: Memory management
 */
int test_memory_safety(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    sage_update_disk_radius_init();

    /* Process a galaxy */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, 0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    sage_update_disk_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will catch any leaks */

    /* ===== CLEANUP ===== */
    sage_update_disk_radius_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_update_disk_radius unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_update_disk_radius Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run physics calculation tests */
    TEST_RUN(test_disk_radius_normal_halo);
    TEST_RUN(test_disk_radius_high_spin);
    TEST_RUN(test_disk_radius_low_spin);
    TEST_RUN(test_disk_radius_zero_spin);
    TEST_RUN(test_disk_radius_zero_vvir);
    TEST_RUN(test_disk_radius_zero_rvir);
    TEST_RUN(test_disk_radius_small_vvir_rvir);
    TEST_RUN(test_disk_radius_type2_skipped);
    TEST_RUN(test_disk_radius_null_galaxy);
    TEST_RUN(test_disk_radius_multiple_galaxies);

    /* Run infrastructure tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
