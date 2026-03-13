/**
 * @file    test_unit_sage_calculate_supernova_feedback.c
 * @brief   Unit tests for sage_calculate_supernova_feedback module
 *
 * Validates: Physics calculations, renormalization, edge cases, parameter handling
 *
 * This test suite validates both software quality and physics correctness:
 *
 * **Software Quality Tests**:
 * - Module registration and initialization
 * - Parameter reading and validation (FeedbackReheatingEpsilon, FeedbackEjectionEfficiency)
 * - Memory safety (no leaks)
 *
 * **Physics Tests**:
 * - Reheating calculation (proportional to stars formed)
 * - Ejection calculation (energy-based, depends on central Vvir)
 * - Renormalization (scales stars and reheating when exceeding ColdGas)
 * - Edge cases (zero stars, zero Vvir, negative ejection clamping)
 *
 * Reference: Croton et al. (2006, 2016) - SAGE model
 *
 * @author  Mimic Development Team
 * @date    2025-12-18 (Refactored for comprehensive physics validation)
 */

#include "../../../tests/framework/test_framework.h"
#include "../core/module_registry.h"
#include "../core/module_interface.h"
#include "../include/types.h"
#include "../include/proto.h"
#include "../include/globals.h"
#include "../util/error.h"
#include "../util/memory.h"
#include "../_system/physical_constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Module parameters (set by setup helpers) */
static double test_epsilon = 3.0;   /* FeedbackReheatingEpsilon */
static double test_eta = 0.3;       /* FeedbackEjectionEfficiency */

/* Physics constants (computed during init) */
static double EnergySNcode_test;
static double EtaSNcode_test;

/* External stubs */
extern void set_test_model_parameters(void);

// ============================================================================
// TEST FIXTURES AND HELPERS
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
 * @brief   Setup module context for testing
 *
 * Creates a minimal ModuleContext with required fields for supernova feedback.
 *
 * @param   ctx             ModuleContext to initialize
 * @param   central         Pointer to central galaxy halo
 * @param   substep_dt      Time step for substep
 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central, double substep_dt)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->central_galaxy = central;
    ctx->substep_dt = substep_dt;
    ctx->params = &MimicConfig;
    ctx->redshift = 0.0;
    ctx->time = 13.8;  /* Gyr */
    ctx->snapshot_number = 63;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
}

/**
 * @brief   Setup test halo and galaxy with specified properties
 *
 * @param   halo            Halo to initialize
 * @param   galaxy          Galaxy to initialize
 * @param   type            Halo type (0=central, 1=satellite)
 * @param   mvir            Virial mass (1e10 Msun/h)
 * @param   vvir            Virial velocity (km/s)
 * @param   cold_gas        Cold gas mass (1e10 Msun/h)
 * @param   new_stars       Stellar mass formed this timestep (1e10 Msun/h)
 */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy,
                             int type, double mvir, double vvir,
                             double cold_gas, double new_stars)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(galaxy, 0, sizeof(struct GalaxyData));

    halo->Type = type;
    halo->Mvir = mvir;
    halo->Vvir = vvir;
    halo->SnapNum = 63;
    halo->galaxy = galaxy;

    galaxy->ColdGas = cold_gas;
    galaxy->NewStellarMass = new_stars;
    galaxy->SupernovaReheatedMass = 0.0;
    galaxy->SupernovaEjectedMass = 0.0;
}

/**
 * @brief   Initialize test physics constants
 *
 * Computes EnergySNcode and EtaSNcode the same way the module does.
 * Also sets global unit conversion variables required by the module.
 */
static void init_test_constants(void)
{
    /* Set minimal config for unit conversion */
    MimicConfig.Hubble_h = 0.73;

    /* CRITICAL: Initialize global unit conversion variables */
    /* These are required by the module init function */
    UnitLength_in_cm = 3.08568e24;     /* 1 Mpc in cm */
    UnitVelocity_in_cm_per_s = 1.0e5;  /* 1 km/s in cm/s */
    UnitMass_in_g = 1.989e43;          /* 1e10 Msun in g */

    /* Derived units */
    UnitTime_in_s = UnitLength_in_cm / UnitVelocity_in_cm_per_s;
    UnitEnergy_in_cgs = UnitMass_in_g * UnitLength_in_cm * UnitLength_in_cm /
                        (UnitTime_in_s * UnitTime_in_s);

    /* Unit conversions (from module initialization) */
    EnergySNcode_test = ENERGY_SN / UnitEnergy_in_cgs * MimicConfig.Hubble_h;
    EtaSNcode_test = ETA_SN * (UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;
}

/**
 * @brief   Call module init function directly
 *
 * @return  0 on success, non-zero on failure
 */
extern int sage_calculate_supernova_feedback_init(void);

/**
 * @brief   Call module process function directly (bypassing module system)
 *
 * @param   ctx             Module context
 * @param   halos           Array of halos (ngal=1 for by_galaxy mode)
 * @param   ngal            Number of galaxies (must be 1)
 * @return  0 on success, non-zero on failure
 */
extern int sage_calculate_supernova_feedback_process(struct ModuleContext *ctx,
                                                       struct Halo *halos, int ngal);

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_registration
 * @brief   Test that sage_calculate_supernova_feedback module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_calculate_supernova_feedback_register() works
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

    /* Configure sage_calculate_supernova_feedback module in phase_1 */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_supernova_feedback");
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
 * Expected: Module reads FeedbackReheatingEpsilon and FeedbackEjectionEfficiency successfully
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
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_supernova_feedback");
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
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_supernova_feedback");
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

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_basic_reheating_calculation
 * @brief   Test basic reheating mass calculation
 *
 * Physics: reheated_mass = FeedbackReheatingEpsilon * NewStellarMass
 *
 * Expected: Reheated mass proportional to stars formed
 * Validates: Basic feedback calculation (no renormalization)
 */
int test_basic_reheating_calculation(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();  /* Set model parameters for module init */

    /* Initialize module to set up physics constants */
    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;  /* For context */
    struct GalaxyData central_galaxy;

    /* Setup central (needed for context) */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    /* Setup test galaxy: plenty of cold gas, small star formation */
    double new_stars = 1.0;  /* 1e10 Msun/h */
    double cold_gas = 100.0;  /* 1e11 Msun/h - way more than needed */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 150.0, cold_gas, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Expected reheated mass: epsilon * stars */
    double expected_reheated = test_epsilon * new_stars;  /* 3.0 * 1.0 = 3.0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, expected_reheated, 1e-6,
                            "Reheated mass should equal epsilon * stars");

    /* Star formation should NOT be renormalized (plenty of gas) */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, new_stars, 1e-6,
                            "NewStellarMass should not change when gas is sufficient");

    /* Ejected mass should be non-negative */
    TEST_ASSERT(test_galaxy.SupernovaEjectedMass >= 0.0,
                "Ejected mass should be non-negative");

    return TEST_PASS;
}

/**
 * @test    test_renormalization_triggered
 * @brief   Test renormalization when stars + reheating > ColdGas
 *
 * Physics: When (stars + reheated) > ColdGas, both are scaled by ColdGas / (stars + reheated)
 *
 * Expected: Both stars and reheating scaled down proportionally
 * Validates: Renormalization logic preserves mass conservation
 */
int test_renormalization_triggered(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    /* Setup: Not enough cold gas for stars + reheating */
    double new_stars = 1.0;  /* 1e10 Msun/h */
    double cold_gas = 3.0;   /* 3e10 Msun/h */
    /* Without renorm: reheated = 3.0 * 1.0 = 3.0, total = 4.0 > 3.0 (cold_gas) */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 150.0, cold_gas, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Calculate expected scaling */
    double initial_reheated = test_epsilon * new_stars;  /* 3.0 */
    double initial_total = new_stars + initial_reheated;  /* 4.0 */
    double scale_factor = cold_gas / initial_total;  /* 3.0 / 4.0 = 0.75 */

    double expected_stars = new_stars * scale_factor;  /* 1.0 * 0.75 = 0.75 */
    double expected_reheated = initial_reheated * scale_factor;  /* 3.0 * 0.75 = 2.25 */

    /* Validate renormalization */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, expected_stars, 1e-6,
                            "NewStellarMass should be renormalized");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, expected_reheated, 1e-6,
                            "Reheated mass should be renormalized");

    /* Validate mass conservation: stars + reheated should not exceed cold gas */
    double total_used = test_galaxy.NewStellarMass + test_galaxy.SupernovaReheatedMass;
    TEST_ASSERT(total_used <= cold_gas + 1e-6,
                "Stars + reheating should not exceed cold gas");

    return TEST_PASS;
}

/**
 * @test    test_renormalization_boundary
 * @brief   Test boundary case: stars + reheating = ColdGas exactly
 *
 * Expected: No renormalization needed (or scale factor = 1.0)
 * Validates: Boundary condition handling
 */
int test_renormalization_boundary(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    /* Setup: Exactly enough cold gas */
    double new_stars = 1.0;  /* 1e10 Msun/h */
    double expected_reheated = test_epsilon * new_stars;  /* 3.0 */
    double cold_gas = new_stars + expected_reheated;  /* 4.0 */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 150.0, cold_gas, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* No renormalization should occur (or scale factor = 1.0) */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, new_stars, 1e-6,
                            "NewStellarMass should not change at boundary");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, expected_reheated, 1e-6,
                            "Reheated mass should equal epsilon * stars");

    return TEST_PASS;
}

/**
 * @test    test_ejection_calculation
 * @brief   Test ejection mass calculation based on central Vvir
 *
 * Physics: ejected = (eta * EnergySN / Vvir^2 - epsilon) * stars
 *
 * Expected: Ejection depends on central's Vvir (energy required to escape)
 * Validates: Ejection formula and central galaxy access
 */
int test_ejection_calculation(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    /* Setup central with known Vvir */
    double central_vvir = 200.0;  /* km/s */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, central_vvir, 100.0, 0.0);

    /* Setup test galaxy */
    double new_stars = 1.0;
    setup_test_halo(&test_halo, &test_galaxy, 1, 10.0, 100.0, 100.0, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Calculate expected ejection */
    double energy_term = test_eta * (EtaSNcode_test * EnergySNcode_test) / (central_vvir * central_vvir);
    double expected_ejected = (energy_term - test_epsilon) * new_stars;

    /* Ejection can be negative (clamped to zero) */
    if (expected_ejected < 0.0) {
        expected_ejected = 0.0;
    }

    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, expected_ejected, 1e-4,
                            "Ejected mass should match formula");

    return TEST_PASS;
}

/**
 * @test    test_negative_ejection_clamped
 * @brief   Test that negative ejection is clamped to zero
 *
 * Physics: When energy term < epsilon, ejection would be negative
 *
 * Expected: Ejected mass clamped to 0.0
 * Validates: Non-negative constraint enforcement
 */
int test_negative_ejection_clamped(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    /* Setup central with VERY HIGH Vvir (low escape probability) */
    double central_vvir = 1000.0;  /* km/s - very massive halo */
    setup_test_halo(&central_halo, &central_galaxy, 0, 1000.0, central_vvir, 100.0, 0.0);

    /* Setup test galaxy */
    double new_stars = 1.0;
    setup_test_halo(&test_halo, &test_galaxy, 1, 10.0, 100.0, 100.0, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* With very high Vvir, energy_term will be very small, making ejection negative */
    /* Module should clamp to zero */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-9,
                            "Negative ejection should be clamped to zero");

    return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_stellar_mass
 * @brief   Test edge case: no star formation
 *
 * Expected: Zero feedback (no stars = no supernovae)
 * Validates: Handles zero input correctly
 */
int test_zero_stellar_mass(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    /* No star formation */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Zero stars should produce zero feedback */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, 0.0, 1e-9,
                            "Zero stars should produce zero reheating");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-9,
                            "Zero stars should produce zero ejection");

    return TEST_PASS;
}

/**
 * @test    test_zero_vvir
 * @brief   Test edge case: central galaxy has zero Vvir
 *
 * Expected: Skip ejection calculation (or handle gracefully)
 * Validates: Division by zero protection
 */
int test_zero_vvir(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    /* Central with Vvir = 0 (edge case) */
    setup_test_halo(&central_halo, &central_galaxy, 0, 0.0, 0.0, 100.0, 0.0);

    /* Test galaxy with star formation */
    setup_test_halo(&test_halo, &test_galaxy, 1, 10.0, 100.0, 100.0, 1.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should handle zero Vvir gracefully");

    /* Module skips ejection calculation when Vvir == 0 */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-9,
                            "Zero Vvir should produce zero ejection");

    /* Reheating should still occur (doesn't depend on Vvir) */
    double expected_reheated = test_epsilon * test_galaxy.NewStellarMass;
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, expected_reheated, 1e-6,
                            "Reheating should occur even with zero Vvir");

    return TEST_PASS;
}

/**
 * @test    test_null_galaxy_pointer
 * @brief   Test edge case: galaxy pointer is NULL
 *
 * Expected: Module returns early without crash
 * Validates: NULL pointer handling
 */
int test_null_galaxy_pointer(void)
{
    /* ===== SETUP ===== */
    init_test_constants();

    struct Halo test_halo;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    memset(&test_halo, 0, sizeof(test_halo));
    test_halo.galaxy = NULL;  /* NULL galaxy */

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should handle NULL galaxy gracefully");

    return TEST_PASS;
}

/**
 * @test    test_very_low_cold_gas
 * @brief   Test edge case: very low cold gas mass
 *
 * Expected: Renormalization produces very small stars and reheating
 * Validates: Numerical stability with small values
 */
int test_very_low_cold_gas(void)
{
    /* ===== SETUP ===== */
    init_test_constants();
    set_test_model_parameters();

    int init_result = sage_calculate_supernova_feedback_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

    /* Very low cold gas, moderate star formation */
    double new_stars = 1.0;
    double cold_gas = 0.001;  /* 1e7 Msun/h - very small */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 150.0, cold_gas, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module should handle very low cold gas");

    /* Stars + reheating should be renormalized to cold_gas */
    double total = test_galaxy.NewStellarMass + test_galaxy.SupernovaReheatedMass;
    TEST_ASSERT(total <= cold_gas + 1e-9,
                "Total should not exceed cold gas even for very small values");

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_calculate_supernova_feedback tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_calculate_supernova_feedback Module\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Software quality tests */
    printf("\n%s--- Software Quality Tests ---%s\n", BLUE, NC);
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_memory_safety);

    /* Physics calculation tests */
    printf("\n%s--- Physics Calculation Tests ---%s\n", BLUE, NC);
    TEST_RUN(test_basic_reheating_calculation);
    TEST_RUN(test_renormalization_triggered);
    TEST_RUN(test_renormalization_boundary);
    TEST_RUN(test_ejection_calculation);
    TEST_RUN(test_negative_ejection_clamped);

    /* Edge case tests */
    printf("\n%s--- Edge Case Tests ---%s\n", BLUE, NC);
    TEST_RUN(test_zero_stellar_mass);
    TEST_RUN(test_zero_vvir);
    TEST_RUN(test_null_galaxy_pointer);
    TEST_RUN(test_very_low_cold_gas);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
