/**
 * @file    test_unit_sage_reincorporation.c
 * @brief   Comprehensive unit tests for sage_reincorporation module
 *
 * Validates: Module lifecycle, physics calculations, edge cases, error handling
 *
 * This test validates the sage_reincorporation module:
 * - Module registration and initialization
 * - Parameter reading and validation
 * - Memory allocation and cleanup (no leaks)
 * - Reincorporation physics calculation
 * - Central-only constraint
 * - Velocity threshold (Vvir > Vcrit)
 * - Mass capping (reincorporated ≤ EjectedGas)
 * - Metallicity preservation during transfer
 * - Zero ejected gas edge case
 * - NULL pointer safety
 * - Negative value prevention
 *
 * Test cases:
 *   LIFECYCLE TESTS:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_memory_safety: No memory leaks during operation
 *
 *   PHYSICS TESTS:
 *   - test_physics_basic_reincorporation: Standard reincorporation calculation
 *   - test_physics_central_only: Only Type 0 centrals reincorporate
 *   - test_physics_full_halo_nonzero_central_index: Full-halo dispatch uses the true FOF central
 *   - test_physics_velocity_threshold: Vvir > Vcrit requirement
 *   - test_physics_mass_capping: Reincorporated limited to available EjectedGas
 *   - test_physics_metallicity_preservation: Metallicity conserved in transfer
 *   - test_physics_zero_ejected_gas: No reincorporation when EjectedGas = 0
 *   - test_physics_mass_conservation: Total mass conserved in transfer
 *   - test_physics_substep_integration: Reincorporation works over multiple substeps
 *
 *   ERROR HANDLING TESTS:
 *   - test_null_galaxy_safety: Handles NULL galaxy gracefully
 *   - test_negative_prevention: Catches negative reincorporation values
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
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

/* Test fixture: Set all required model parameters */
extern void set_test_model_parameters(void);

/* External module interface for direct testing */
extern int sage_reincorporation_init(void);
extern int sage_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_reincorporation_cleanup(void);

/* Test fixtures for physics tests */

/**
 * @brief Initialize sage_reincorporation module for physics testing
 *
 * Sets up module with ReIncorporationFactor = 1.0 (standard SAGE value)
 */
static void setup_module_for_physics_test(void)
{
    reset_config();
    set_test_model_parameters();

    /* ReIncorporationFactor = 1.0 is default in test_stubs.c */
    /* This gives Vcrit = 445.48 km/s */

    sage_reincorporation_init();
}

/**
 * @brief Create a test halo with galaxy for physics tests
 *
 * @param type Halo type (0=central, 1=satellite, 2=orphan)
 * @param vvir Virial velocity (km/s)
 * @param rvir Virial radius (Mpc/h)
 * @return Allocated halo (must be freed with free_test_halo)
 */
static struct Halo create_test_halo(int type, float vvir, float rvir)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = type;
    halo.Vvir = vvir;
    halo.Rvir = rvir;
    halo.SnapNum = 63;  /* z=0 */
    halo.dT = 0.1;      /* Default test timestep (overridden when needed) */

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
 * @brief Create minimal module context for testing
 */
static struct ModuleContext create_test_context(double dt)
{
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.substep_dt = dt;
    ctx.redshift = 0.0;
    ctx.time = 13.6;  /* Gyr */
    ctx.snapshot_number = 63;
    ctx.substep_number = 0;
    ctx.num_substeps = 1;
    ctx.params = &MimicConfig;

    return ctx;
}

/* ========================================================================== */
/* LIFECYCLE TESTS                                                           */
/* ========================================================================== */

/**
 * @test    test_module_registration
 * @brief   Test that sage_reincorporation module registers correctly
 */
int test_module_registration(void)
{
    reset_config();
    ensure_modules_registered();
    return TEST_PASS;
}

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 */
int test_module_initialization(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    test_phase_add("galaxy_physics", "sage_reincorporation", PROCESSING_MODE_FULL_HALO);
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module system initialization should succeed");

    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_reading
 * @brief   Test that module reads parameters from configuration correctly
 */
int test_parameter_reading(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    test_phase_add("galaxy_physics", "sage_reincorporation", PROCESSING_MODE_FULL_HALO);
    MimicConfig.SubSteps = 1;

    set_test_model_parameters();
    strcpy(MimicConfig.ModelParams[14].value, "0.5");  /* ReIncorporationFactor custom value */

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization with custom parameters should succeed");

    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test that module has no memory leaks during operation
 */
int test_memory_safety(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    test_phase_add("galaxy_physics", "sage_reincorporation", PROCESSING_MODE_FULL_HALO);
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization should succeed");

    module_system_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* PHYSICS TESTS                                                             */
/* ========================================================================== */

/**
 * @test    test_physics_basic_reincorporation
 * @brief   Test standard reincorporation calculation
 *
 * Physics: reincorporated = (Vvir/Vcrit - 1) * EjectedGas / (Rvir/Vvir) * dt
 * With ReIncorporationFactor = 1.0, Vcrit = 445.48 km/s
 */
int test_physics_basic_reincorporation(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Create massive central halo (Vvir > Vcrit = 445.48 km/s) */
    struct Halo halo = create_test_halo(0, 500.0, 0.2);
    halo.galaxy->EjectedGas = 10.0;           /* 1e11 Msun/h */
    halo.galaxy->MetalsEjectedGas = 0.2;      /* 2e9 Msun/h (2% metallicity) */
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.1;

    struct ModuleContext ctx = create_test_context(0.1);  /* dt = 0.1 Gyr/h */
    ctx.central_galaxy = &halo;  /* Point to itself (it's a central) */

    /* Store initial values */
    double initial_ejected = halo.galaxy->EjectedGas;
    double initial_hot = halo.galaxy->HotGas;

    /* Execute */
    int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result == 0, "Reincorporation should succeed");

    /* Validate: Mass should transfer from ejected to hot */
    TEST_ASSERT(halo.galaxy->EjectedGas < initial_ejected, "EjectedGas should decrease");
    TEST_ASSERT(halo.galaxy->HotGas > initial_hot, "HotGas should increase");

    /* Validate: Mass conservation */
    double transferred = initial_ejected - halo.galaxy->EjectedGas;
    double received = halo.galaxy->HotGas - initial_hot;
    TEST_ASSERT_DOUBLE_EQUAL(transferred, received, 1e-6, "Mass should be conserved");

    /* Validate: Positive transfer */
    TEST_ASSERT(transferred > 0.0, "Some mass should be reincorporated");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_central_only
 * @brief   Test that only Type 0 centrals reincorporate gas
 */
int test_physics_central_only(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Test satellite (Type 1) */
    struct Halo satellite = create_test_halo(1, 500.0, 0.2);
    satellite.galaxy->EjectedGas = 10.0;
    satellite.galaxy->HotGas = 5.0;

    struct ModuleContext ctx = create_test_context(0.1);

    /* Create a central for the context */
    struct Halo central = create_test_halo(0, 600.0, 0.3);
    ctx.central_galaxy = &central;

    double initial_ejected = satellite.galaxy->EjectedGas;
    double initial_hot = satellite.galaxy->HotGas;

    /* Execute on satellite */
    int result = sage_reincorporation_process(&ctx, &satellite, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Validate: Satellite properties should be unchanged */
    TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->EjectedGas, initial_ejected, 1e-10,
                             "Satellite EjectedGas should not change");
    TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->HotGas, initial_hot, 1e-10,
                             "Satellite HotGas should not change");

    /* Test orphan (Type 2) */
    struct Halo orphan = create_test_halo(2, 500.0, 0.2);
    orphan.galaxy->EjectedGas = 10.0;
    orphan.galaxy->HotGas = 5.0;

    initial_ejected = orphan.galaxy->EjectedGas;
    initial_hot = orphan.galaxy->HotGas;

    result = sage_reincorporation_process(&ctx, &orphan, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Validate: Orphan properties should be unchanged */
    TEST_ASSERT_DOUBLE_EQUAL(orphan.galaxy->EjectedGas, initial_ejected, 1e-10,
                             "Orphan EjectedGas should not change");
    TEST_ASSERT_DOUBLE_EQUAL(orphan.galaxy->HotGas, initial_hot, 1e-10,
                             "Orphan HotGas should not change");

    /* Cleanup */
    free_test_halo(&satellite);
    free_test_halo(&orphan);
    free_test_halo(&central);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_full_halo_nonzero_central_index
 * @brief   Test that full-halo dispatch uses the context central, not halos[0]
 */
int test_physics_full_halo_nonzero_central_index(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo halos[2];
    halos[0] = create_test_halo(1, 500.0, 0.2);
    halos[1] = create_test_halo(0, 600.0, 0.3);

    halos[0].galaxy->EjectedGas = 4.0;
    halos[0].galaxy->HotGas = 2.0;
    halos[1].galaxy->EjectedGas = 10.0;
    halos[1].galaxy->HotGas = 5.0;

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_index = 1;
    ctx.central_galaxy = &halos[1];

    const double central_initial_ejected = halos[1].galaxy->EjectedGas;
    const double central_initial_hot = halos[1].galaxy->HotGas;
    const double satellite_initial_ejected = halos[0].galaxy->EjectedGas;
    const double satellite_initial_hot = halos[0].galaxy->HotGas;

    int result = sage_reincorporation_process(&ctx, halos, 2);
    TEST_ASSERT(result == 0, "Full-halo reincorporation should succeed");

    TEST_ASSERT(halos[1].galaxy->EjectedGas < central_initial_ejected,
                "Central EjectedGas should decrease");
    TEST_ASSERT(halos[1].galaxy->HotGas > central_initial_hot,
                "Central HotGas should increase");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->EjectedGas, satellite_initial_ejected, 1e-10,
                             "Satellite EjectedGas should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->HotGas, satellite_initial_hot, 1e-10,
                             "Satellite HotGas should be unchanged");

    free_test_halo(&halos[0]);
    free_test_halo(&halos[1]);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_velocity_threshold
 * @brief   Test that reincorporation only occurs when Vvir > Vcrit
 *
 * With ReIncorporationFactor = 1.0, Vcrit = 445.48 km/s
 */
int test_physics_velocity_threshold(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Test halo below threshold (Vvir < Vcrit) */
    struct Halo low_vvir = create_test_halo(0, 400.0, 0.2);  /* Below 445.48 */
    low_vvir.galaxy->EjectedGas = 10.0;
    low_vvir.galaxy->MetalsEjectedGas = 0.2;
    low_vvir.galaxy->HotGas = 5.0;
    low_vvir.galaxy->MetalsHotGas = 0.1;

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_galaxy = &low_vvir;

    double initial_ejected = low_vvir.galaxy->EjectedGas;
    double initial_hot = low_vvir.galaxy->HotGas;

    /* Execute */
    int result = sage_reincorporation_process(&ctx, &low_vvir, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Validate: No reincorporation should occur */
    TEST_ASSERT_DOUBLE_EQUAL(low_vvir.galaxy->EjectedGas, initial_ejected, 1e-10,
                             "EjectedGas should not change when Vvir < Vcrit");
    TEST_ASSERT_DOUBLE_EQUAL(low_vvir.galaxy->HotGas, initial_hot, 1e-10,
                             "HotGas should not change when Vvir < Vcrit");

    /* Test halo at threshold (Vvir = Vcrit) */
    struct Halo at_threshold = create_test_halo(0, 445.48, 0.2);
    at_threshold.galaxy->EjectedGas = 10.0;
    at_threshold.galaxy->HotGas = 5.0;
    ctx.central_galaxy = &at_threshold;

    initial_ejected = at_threshold.galaxy->EjectedGas;
    initial_hot = at_threshold.galaxy->HotGas;

    result = sage_reincorporation_process(&ctx, &at_threshold, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Validate: Minimal reincorporation at Vcrit (near-zero from floating point effects) */
    double change = initial_ejected - at_threshold.galaxy->EjectedGas;
    TEST_ASSERT(change < 1e-3, "Very little reincorporation should occur near Vcrit threshold");

    /* Test halo above threshold (Vvir > Vcrit) */
    struct Halo high_vvir = create_test_halo(0, 500.0, 0.2);
    high_vvir.galaxy->EjectedGas = 10.0;
    high_vvir.galaxy->HotGas = 5.0;
    ctx.central_galaxy = &high_vvir;

    initial_ejected = high_vvir.galaxy->EjectedGas;

    result = sage_reincorporation_process(&ctx, &high_vvir, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Validate: Reincorporation should occur */
    TEST_ASSERT(high_vvir.galaxy->EjectedGas < initial_ejected,
                "EjectedGas should decrease when Vvir > Vcrit");

    /* Cleanup */
    free_test_halo(&low_vvir);
    free_test_halo(&at_threshold);
    free_test_halo(&high_vvir);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_mass_capping
 * @brief   Test that reincorporated mass is capped to available EjectedGas
 */
int test_physics_mass_capping(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Create very massive halo with small ejected reservoir */
    struct Halo halo = create_test_halo(0, 800.0, 0.3);  /* Very high Vvir */
    halo.dT = 1.0;  /* Override default: large dt for this test */
    halo.galaxy->EjectedGas = 0.5;           /* Small reservoir */
    halo.galaxy->MetalsEjectedGas = 0.01;
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.1;

    struct ModuleContext ctx = create_test_context(1.0);  /* Large dt */
    ctx.central_galaxy = &halo;

    /* Execute */
    int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Validate: All ejected gas should be reincorporated (capped) */
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->EjectedGas, 0.0, 1e-10,
                             "EjectedGas should be completely depleted");
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->MetalsEjectedGas, 0.0, 1e-10,
                             "MetalsEjectedGas should be completely depleted");

    /* Validate: Positive values (no negative mass) */
    TEST_ASSERT(halo.galaxy->EjectedGas >= 0.0, "EjectedGas should not be negative");
    TEST_ASSERT(halo.galaxy->HotGas > 0.0, "HotGas should increase");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_metallicity_preservation
 * @brief   Test that metallicity is preserved during gas transfer
 */
int test_physics_metallicity_preservation(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Create halo with specific metallicity in ejected gas */
    struct Halo halo = create_test_halo(0, 500.0, 0.2);
    halo.galaxy->EjectedGas = 10.0;           /* 1e11 Msun/h */
    halo.galaxy->MetalsEjectedGas = 0.3;      /* 3e9 Msun/h → Z = 0.03 */
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.05;         /* Z_hot initial = 0.01 */

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_galaxy = &halo;

    /* Calculate expected metallicity */
    double Z_ejected = halo.galaxy->MetalsEjectedGas / halo.galaxy->EjectedGas;

    /* Store initial values */
    double initial_ejected = halo.galaxy->EjectedGas;
    double initial_metals_ejected = halo.galaxy->MetalsEjectedGas;
    double initial_hot = halo.galaxy->HotGas;
    double initial_metals_hot = halo.galaxy->MetalsHotGas;

    /* Execute */
    int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Calculate transferred amounts */
    double mass_transferred = initial_ejected - halo.galaxy->EjectedGas;
    double metals_transferred = initial_metals_ejected - halo.galaxy->MetalsEjectedGas;
    double expected_metals_transferred = Z_ejected * mass_transferred;

    /* Validate: Metallicity preserved in transfer */
    TEST_ASSERT_DOUBLE_EQUAL(metals_transferred, expected_metals_transferred, 1e-6,
                             "Metals transferred should match ejected metallicity");

    /* Validate: Total mass conservation */
    double total_mass_before = initial_ejected + initial_hot;
    double total_mass_after = halo.galaxy->EjectedGas + halo.galaxy->HotGas;
    TEST_ASSERT_DOUBLE_EQUAL(total_mass_before, total_mass_after, 1e-6,
                             "Total mass should be conserved");

    /* Validate: Total metals conservation */
    double total_metals_before = initial_metals_ejected + initial_metals_hot;
    double total_metals_after = halo.galaxy->MetalsEjectedGas + halo.galaxy->MetalsHotGas;
    TEST_ASSERT_DOUBLE_EQUAL(total_metals_before, total_metals_after, 1e-6,
                             "Total metals should be conserved");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_zero_ejected_gas
 * @brief   Test that module handles zero ejected gas gracefully
 */
int test_physics_zero_ejected_gas(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Create halo with no ejected gas */
    struct Halo halo = create_test_halo(0, 500.0, 0.2);
    halo.galaxy->EjectedGas = 0.0;
    halo.galaxy->MetalsEjectedGas = 0.0;
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.1;

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_galaxy = &halo;

    double initial_hot = halo.galaxy->HotGas;

    /* Execute */
    int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result == 0, "Process should succeed with zero ejected gas");

    /* Validate: Nothing should change */
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->EjectedGas, 0.0, 1e-10,
                             "EjectedGas should remain zero");
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->HotGas, initial_hot, 1e-10,
                             "HotGas should not change");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_mass_conservation
 * @brief   Test that total mass is strictly conserved
 */
int test_physics_mass_conservation(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo halo = create_test_halo(0, 500.0, 0.2);
    halo.galaxy->EjectedGas = 10.0;
    halo.galaxy->MetalsEjectedGas = 0.2;
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.1;

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_galaxy = &halo;

    /* Calculate totals before */
    double total_mass_before = halo.galaxy->EjectedGas + halo.galaxy->HotGas;
    double total_metals_before = halo.galaxy->MetalsEjectedGas + halo.galaxy->MetalsHotGas;

    /* Execute */
    int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    /* Calculate totals after */
    double total_mass_after = halo.galaxy->EjectedGas + halo.galaxy->HotGas;
    double total_metals_after = halo.galaxy->MetalsEjectedGas + halo.galaxy->MetalsHotGas;

    /* Validate: Perfect conservation */
    TEST_ASSERT_DOUBLE_EQUAL(total_mass_before, total_mass_after, 1e-10,
                             "Total mass must be strictly conserved");
    TEST_ASSERT_DOUBLE_EQUAL(total_metals_before, total_metals_after, 1e-10,
                             "Total metals must be strictly conserved");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_physics_substep_integration
 * @brief   Test that reincorporation works correctly over multiple substeps
 */
int test_physics_substep_integration(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo halo = create_test_halo(0, 500.0, 0.2);
    halo.galaxy->EjectedGas = 10.0;
    halo.galaxy->MetalsEjectedGas = 0.2;
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.1;

    double total_mass_before = halo.galaxy->EjectedGas + halo.galaxy->HotGas;

    /* Run 5 substeps */
    for (int substep = 0; substep < 5; substep++) {
        struct ModuleContext ctx = create_test_context(0.02);  /* 5 × 0.02 = 0.1 total */
        ctx.substep_number = substep;
        ctx.num_substeps = 5;
        ctx.central_galaxy = &halo;

        int result = sage_reincorporation_process(&ctx, &halo, 1);
        TEST_ASSERT(result == 0, "Each substep should succeed");
    }

    /* Validate: Mass should have been reincorporated */
    TEST_ASSERT(halo.galaxy->EjectedGas < 10.0, "Some EjectedGas should be reincorporated");
    TEST_ASSERT(halo.galaxy->HotGas > 5.0, "HotGas should increase");

    /* Validate: Total mass conserved over all substeps */
    double total_mass_after = halo.galaxy->EjectedGas + halo.galaxy->HotGas;
    TEST_ASSERT_DOUBLE_EQUAL(total_mass_before, total_mass_after, 1e-6,
                             "Mass conservation must hold across substeps");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* ERROR HANDLING TESTS                                                      */
/* ========================================================================== */

/**
 * @test    test_null_galaxy_safety
 * @brief   Test that module handles NULL galaxy pointer safely
 */
int test_null_galaxy_safety(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    struct Halo halo = create_test_halo(0, 500.0, 0.2);

    /* Free the galaxy to create NULL pointer */
    myfree(halo.galaxy);
    halo.galaxy = NULL;

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_galaxy = &halo;  /* Still need valid central */

    /* Execute - should handle NULL gracefully */
    int result = sage_reincorporation_process(&ctx, &halo, 1);

    /* Module returns -1 for NULL galaxy (error condition) */
    TEST_ASSERT(result == -1, "Should return error for NULL galaxy");

    /* Cleanup (no galaxy to free) */
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_negative_prevention
 * @brief   Test that negative reincorporation values are caught
 *
 * This is a defensive test - the module should never produce negative
 * reincorporation, but if it does, it should be caught and reported.
 */
int test_negative_prevention(void)
{
    init_memory_system(0);
    setup_module_for_physics_test();

    /* Normal case - should never produce negative values */
    struct Halo halo = create_test_halo(0, 500.0, 0.2);
    halo.galaxy->EjectedGas = 10.0;
    halo.galaxy->MetalsEjectedGas = 0.2;
    halo.galaxy->HotGas = 5.0;
    halo.galaxy->MetalsHotGas = 0.1;

    struct ModuleContext ctx = create_test_context(0.1);
    ctx.central_galaxy = &halo;

    int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result == 0, "Normal case should succeed");

    /* Validate: No negative masses */
    TEST_ASSERT(halo.galaxy->EjectedGas >= 0.0, "EjectedGas must not be negative");
    TEST_ASSERT(halo.galaxy->HotGas >= 0.0, "HotGas must not be negative");
    TEST_ASSERT(halo.galaxy->MetalsEjectedGas >= 0.0, "MetalsEjectedGas must not be negative");
    TEST_ASSERT(halo.galaxy->MetalsHotGas >= 0.0, "MetalsHotGas must not be negative");

    /* Cleanup */
    free_test_halo(&halo);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* MAIN TEST RUNNER                                                          */
/* ========================================================================== */

/**
 * @brief   Main test runner
 *
 * Executes all sage_reincorporation tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_reincorporation Module\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run lifecycle tests */
    printf("\n%sLIFECYCLE TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_memory_safety);

    /* Run physics tests */
    printf("\n%sPHYSICS TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_physics_basic_reincorporation);
    TEST_RUN(test_physics_central_only);
    TEST_RUN(test_physics_full_halo_nonzero_central_index);
    TEST_RUN(test_physics_velocity_threshold);
    TEST_RUN(test_physics_mass_capping);
    TEST_RUN(test_physics_metallicity_preservation);
    TEST_RUN(test_physics_zero_ejected_gas);
    TEST_RUN(test_physics_mass_conservation);
    TEST_RUN(test_physics_substep_integration);

    /* Run error handling tests */
    printf("\n%sERROR HANDLING TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_null_galaxy_safety);
    TEST_RUN(test_negative_prevention);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
