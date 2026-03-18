/**
 * @file    test_unit_sage_star_formation.c
 * @brief   Unit tests for sage_star_formation module
 *
 * Validates: Physics calculation logic, edge cases, parameter handling
 *
 * This test validates the sage_star_formation module physics:
 * - Star formation calculation (Kennicutt-Schmidt with critical threshold)
 * - Critical threshold logic (above/below/at threshold)
 * - Edge cases (zero radius, zero velocity, negative values)
 * - Parameter sensitivity (efficiency affects results)
 * - Module lifecycle and memory safety
 *
 * Test cases:
 *   - test_sf_calculation_above_threshold: Normal star formation
 *   - test_sf_calculation_below_threshold: No SF when below threshold
 *   - test_sf_calculation_at_threshold: Boundary condition
 *   - test_sf_calculation_zero_radius: Edge case handling
 *   - test_sf_calculation_zero_velocity: Edge case handling
 *   - test_sf_parameter_sensitivity: Efficiency changes results
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../core/module_registry.h"
#include "../../../core/module_interface.h"
#include "../../../include/types.h"
#include "../../../include/proto.h"
#include "../../../include/globals.h"
#include "../../../util/error.h"
#include "../../../util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Module parameters (extern declarations to access module internals for testing) */
extern double SFR_EFFICIENCY;
extern double STAR_FORMING_DISK_FACTOR;

/* Stable string storage for pipeline config used by physics tests that call
 * sage_star_formation_init() directly.  The SF dependency check requires the
 * apply step to be visible in MimicConfig; static storage avoids the memory
 * system requirement. */
static char sf_pipeline_name0[] = "sage_star_formation";
static char sf_pipeline_name1[] = "sage_apply_star_formation_supernova";
static struct PhaseModuleConfig sf_physics_pipeline[2];

/* External stubs */
extern void set_test_model_parameters(void);

/* Module functions (extern declarations for direct testing) */
extern int sage_star_formation_init(void);
extern int sage_star_formation_process(struct ModuleContext *ctx,
                                                  struct Halo *halos, int ngal);
extern int sage_star_formation_cleanup(void);

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
 * @param   cold_gas    Cold gas mass [1e10 Msun/h]
 * @param   disk_radius Disk scale radius [Mpc/h]
 * @param   vvir        Virial velocity [km/s]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                               double cold_gas, double disk_radius, double vvir)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = 0;  /* Central galaxy */
    halo->Mvir = 100.0;  /* 10^12 Msun/h */
    halo->Vvir = (float)vvir;
    halo->SnapNum = 63;
    halo->dT = 0.01;  /* Default substep dt for tests (num_substeps=1) */
    halo->galaxy = gal;

    gal->ColdGas = (float)cold_gas;
    gal->DiskScaleRadius = (float)disk_radius;
    gal->NewStellarMass = 0.0;  /* Should be calculated by module */
}

/**
 * @brief   Setup test parameters
 *
 * @param   efficiency  Star formation efficiency
 * @param   disk_factor Star forming disk factor
 */
static void setup_test_parameters(double efficiency, double disk_factor)
{
    /* Set model parameters in MimicConfig */
    int idx = 0;

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "SfrEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", efficiency);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "StarFormingDiskFactor");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", disk_factor);

    MimicConfig.NumModelParams = idx;

    /* The SF dependency check requires sage_apply_star_formation_supernova to be
     * visible in MimicConfig when sage_star_formation_init() is called directly. */
    sf_physics_pipeline[0].module_name = sf_pipeline_name0;
    sf_physics_pipeline[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    sf_physics_pipeline[1].module_name = sf_pipeline_name1;
    sf_physics_pipeline[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1 = sf_physics_pipeline;
    MimicConfig.num_phase_1 = 2;
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

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_sf_calculation_above_threshold
 * @brief   Test star formation when ColdGas is above critical threshold
 *
 * Expected: NewStellarMass > 0, follows SF equation
 * Validates: Core physics calculation
 */
int test_sf_calculation_above_threshold(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Initialize module parameters */
    const double efficiency = 0.02;
    const double disk_factor = 3.0;
    setup_test_parameters(efficiency, disk_factor);

    int result = sage_star_formation_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create test galaxy with ColdGas above threshold */
    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 10.0;      /* 10^11 Msun/h */
    const double disk_radius = 0.05;   /* 50 kpc/h */
    const double vvir = 200.0;         /* 200 km/s */
    setup_test_galaxy(&halo, &gal, cold_gas, disk_radius, vvir);

    /* Setup context */
    struct ModuleContext ctx;
    const double dt = 0.01;  /* 10 Myr/h */
    setup_test_context(&ctx, dt);

    /* Calculate expected values (reproduce module logic) */
    const double reff = disk_factor * disk_radius;
    const double tdyn = reff / vvir;
    const double cold_crit = 0.19 * vvir * reff;
    const double expected_strdot = (cold_gas > cold_crit && tdyn > 0.0)
                                    ? efficiency * (cold_gas - cold_crit) / tdyn
                                    : 0.0;
    const double expected_stars = expected_strdot * dt;

    /* ===== EXECUTE ===== */
    result = sage_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");
    TEST_ASSERT(gal.ColdGas > cold_crit, "ColdGas should be above threshold");
    TEST_ASSERT(gal.NewStellarMass > 0.0, "NewStellarMass should be positive");
    TEST_ASSERT_DOUBLE_EQUAL(gal.NewStellarMass, expected_stars, 1e-6,
                             "NewStellarMass should match expected value");

    /* ===== CLEANUP ===== */
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_sf_calculation_below_threshold
 * @brief   Test star formation when ColdGas is below critical threshold
 *
 * Expected: NewStellarMass = 0 (no star formation)
 * Validates: Critical threshold logic
 */
int test_sf_calculation_below_threshold(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.02;
    const double disk_factor = 3.0;
    setup_test_parameters(efficiency, disk_factor);

    sage_star_formation_init();

    /* Create test galaxy with ColdGas below threshold */
    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 0.1;       /* Very low gas */
    const double disk_radius = 0.05;   /* 50 kpc/h */
    const double vvir = 200.0;         /* 200 km/s */
    setup_test_galaxy(&halo, &gal, cold_gas, disk_radius, vvir);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* Calculate threshold */
    const double reff = disk_factor * disk_radius;
    const double cold_crit = 0.19 * vvir * reff;

    /* ===== EXECUTE ===== */
    sage_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.ColdGas < cold_crit, "ColdGas should be below threshold");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero below threshold");

    /* ===== CLEANUP ===== */
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_sf_calculation_at_threshold
 * @brief   Test star formation when ColdGas is exactly at critical threshold
 *
 * Expected: NewStellarMass = 0 (boundary condition)
 * Validates: Boundary condition handling
 */
int test_sf_calculation_at_threshold(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.02;
    const double disk_factor = 3.0;
    setup_test_parameters(efficiency, disk_factor);

    sage_star_formation_init();

    /* Setup galaxy parameters */
    const double disk_radius = 0.05;   /* 50 kpc/h */
    const double vvir = 200.0;         /* 200 km/s */
    const double reff = disk_factor * disk_radius;
    const double cold_crit = 0.19 * vvir * reff;

    /* Set ColdGas exactly at threshold */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, cold_crit, disk_radius, vvir);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    sage_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, cold_crit, 1e-6, "ColdGas should be at threshold");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero at threshold");

    /* ===== CLEANUP ===== */
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_sf_calculation_zero_radius
 * @brief   Test star formation when DiskScaleRadius is zero
 *
 * Expected: NewStellarMass = 0 (handles edge case gracefully)
 * Validates: Division by zero protection
 */
int test_sf_calculation_zero_radius(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.02;
    const double disk_factor = 3.0;
    setup_test_parameters(efficiency, disk_factor);

    sage_star_formation_init();

    /* Create galaxy with zero disk radius */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 10.0, 0.0, 200.0);  /* radius = 0 */

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero radius gracefully");
    TEST_ASSERT(gal.DiskScaleRadius == 0.0, "DiskScaleRadius is zero");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero with zero radius");

    /* ===== CLEANUP ===== */
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_sf_calculation_zero_velocity
 * @brief   Test star formation when Vvir is zero
 *
 * Expected: NewStellarMass = 0 (handles edge case gracefully)
 * Validates: Division by zero protection
 */
int test_sf_calculation_zero_velocity(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.02;
    const double disk_factor = 3.0;
    setup_test_parameters(efficiency, disk_factor);

    sage_star_formation_init();

    /* Create galaxy with zero virial velocity */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 10.0, 0.05, 0.0);  /* vvir = 0 */

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero velocity gracefully");
    TEST_ASSERT(halo.Vvir == 0.0, "Vvir is zero");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero with zero velocity");

    /* ===== CLEANUP ===== */
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_sf_parameter_sensitivity
 * @brief   Test that changing SfrEfficiency changes star formation
 *
 * Expected: Higher efficiency → more stars
 * Validates: Parameter sensitivity
 */
int test_sf_parameter_sensitivity(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test with low efficiency */
    reset_config();
    setup_test_parameters(0.01, 3.0);
    sage_star_formation_init();

    struct Halo halo1;
    struct GalaxyData gal1;
    setup_test_galaxy(&halo1, &gal1, 10.0, 0.05, 200.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    sage_star_formation_process(&ctx, &halo1, 1);
    const double stars_low_eff = gal1.NewStellarMass;
    sage_star_formation_cleanup();

    /* Test with high efficiency */
    reset_config();
    setup_test_parameters(0.05, 3.0);  /* 5x higher efficiency */
    sage_star_formation_init();

    struct Halo halo2;
    struct GalaxyData gal2;
    setup_test_galaxy(&halo2, &gal2, 10.0, 0.05, 200.0);  /* Same galaxy */

    sage_star_formation_process(&ctx, &halo2, 1);
    const double stars_high_eff = gal2.NewStellarMass;
    sage_star_formation_cleanup();

    /* ===== VALIDATE ===== */
    TEST_ASSERT(stars_low_eff > 0.0, "Low efficiency should produce stars");
    TEST_ASSERT(stars_high_eff > 0.0, "High efficiency should produce stars");
    TEST_ASSERT(stars_high_eff > stars_low_eff, "Higher efficiency should produce more stars");

    /* Should scale approximately linearly (5x efficiency → ~5x stars) */
    const double ratio = stars_high_eff / stars_low_eff;
    TEST_ASSERT(ratio > 4.5 && ratio < 5.5, "Stars should scale linearly with efficiency");

    /* ===== CLEANUP ===== */
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

    /* SF requires sage_apply_star_formation_supernova in the pipeline */
    MimicConfig.phase_1 = mymalloc_cat(2 * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_star_formation");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[1].module_name = strdup("sage_apply_star_formation_supernova");
    MimicConfig.phase_1[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 2;
    MimicConfig.SubSteps = 1;

    /* Set required parameters — use full set since apply step is also initialized */
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

    setup_test_parameters(0.02, 3.0);
    sage_star_formation_init();

    /* Process a galaxy */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 10.0, 0.05, 200.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    sage_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will catch any leaks */

    /* ===== CLEANUP ===== */
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_star_formation unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_star_formation Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run physics calculation tests */
    TEST_RUN(test_sf_calculation_above_threshold);
    TEST_RUN(test_sf_calculation_below_threshold);
    TEST_RUN(test_sf_calculation_at_threshold);
    TEST_RUN(test_sf_calculation_zero_radius);
    TEST_RUN(test_sf_calculation_zero_velocity);
    TEST_RUN(test_sf_parameter_sensitivity);

    /* Run infrastructure tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
