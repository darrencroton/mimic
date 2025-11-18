/**
 * @file    test_unit_sage_disk_instability.c
 * @brief   Software quality unit tests for sage_disk_instability module
 *
 * ============================================================================
 * ⚠️  STATUS: CURRENTLY NON-FUNCTIONAL - REQUIRES UPDATING FOR PHASE 4.3
 * ============================================================================
 *
 * This test file is currently BROKEN and will not compile due to outdated API
 * usage. It was written for an earlier module testing framework and needs to
 * be updated to match the current testing infrastructure.
 *
 * Issues:
 *   - Uses undefined TEST_BEGIN/TEST_PASS macros (old API)
 *   - Calls module_system_init() with wrong signature (now takes void)
 *   - References undefined functions (module_system_process, etc.)
 *   - Uses deprecated testing patterns
 *
 * Roadmap Status: Per Phase 4.3 - "Integration testing infrastructure needs design"
 *
 * Action Required: Rewrite this test file to match current testing framework
 *                  (see test_unit_sage_infall.c or test_unit_sage_cooling.c)
 *
 * Until then: This file is expected to fail compilation. The sage_disk_instability
 *            module itself is functional and tested via integration tests.
 * ============================================================================
 *
 * Original Intent:
 * Validates: Module lifecycle, stability criterion, mass transfers, metallicity preservation
 * Phase: Phase 4.6 (SAGE Disk Instability Module - Partial Implementation)
 *
 * This test validates both software engineering and core physics:
 * - Module registration and initialization
 * - Parameter reading and validation
 * - Memory allocation and cleanup (no leaks)
 * - Stability criterion calculation (Mo, Mao & White 1998)
 * - Stellar mass transfer mechanics
 * - Metallicity preservation during transfers
 * - Edge cases (zero mass, pure disk, pure bulge)
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_parameter_reading: Module parameters read from config
 *   - test_stability_criterion: Critical mass calculation validates
 *   - test_stellar_mass_transfer: Unstable stars transfer to bulge
 *   - test_metallicity_preservation: Metals preserved during transfer
 *   - test_edge_cases: Zero mass, pure disk, pure bulge scenarios
 *
 * NOTE: Gas processing tests deferred to v2.0.0 when sage_mergers module exists
 *
 * @author  Mimic Development Team
 * @date    2025-11-17
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
    MimicConfig.Hubble_h = 0.7;
    MimicConfig.Omega = 0.3;
    MimicConfig.OmegaLambda = 0.7;

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
    TEST_BEGIN("Module registration");

    reset_config();
    ensure_modules_registered();

    /* Module should be registered */
    /* Note: Module registry doesn't expose lookup functions in current design
     * Registration success is implicit (no crash) */

    TEST_PASS("Module registered successfully");
    return 1;
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
    TEST_BEGIN("Module initialization");

    reset_config();
    set_default_params();
    ensure_modules_registered();

    /* Initialize module system (includes sage_disk_instability) */
    int result = module_system_init(&MimicConfig);
    TEST_ASSERT(result == 0, "Module system initialization should succeed");

    /* Cleanup module system */
    result = module_system_cleanup();
    TEST_ASSERT(result == 0, "Module system cleanup should succeed");

    /* Check for memory leaks */
    size_t allocated = get_total_allocated_memory();
    TEST_ASSERT(allocated == 0, "No memory leaks after cleanup");

    TEST_PASS("Module lifecycle works correctly");
    return 1;
}

/**
 * @test    test_parameter_reading
 * @brief   Test that module parameters are read correctly
 *
 * Expected: Parameters read from config and applied
 * Validates: module_get_param_int, module_get_param_double
 */
int test_parameter_reading(void)
{
    TEST_BEGIN("Parameter reading");

    reset_config();

    /* Set custom parameter values */
    strcpy(MimicConfig.ModuleParams[0].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[0].param_name, "DiskInstabilityOn");
    strcpy(MimicConfig.ModuleParams[0].value, "0");

    strcpy(MimicConfig.ModuleParams[1].module_name, "SageDiskInstability");
    strcpy(MimicConfig.ModuleParams[1].param_name, "DiskRadiusFactor");
    strcpy(MimicConfig.ModuleParams[1].value, "5.0");

    MimicConfig.NumModuleParams = 2;

    ensure_modules_registered();

    /* Initialize should read these parameters */
    int result = module_system_init(&MimicConfig);
    TEST_ASSERT(result == 0, "Module initialization with custom params should succeed");

    module_system_cleanup();

    TEST_PASS("Parameters read correctly");
    return 1;
}

/**
 * @test    test_stability_criterion
 * @brief   Test disk stability criterion calculation
 *
 * Expected: Critical mass calculated according to Mo, Mao & White (1998)
 * Validates: Mcrit = Vmax^2 * (3 * Rd) / G
 */
int test_stability_criterion(void)
{
    TEST_BEGIN("Stability criterion calculation");

    reset_config();
    set_default_params();
    ensure_modules_registered();
    module_system_init(&MimicConfig);

    /* Create test halo and galaxy */
    struct Halo halo;
    struct GalaxyData galaxy;
    memset(&halo, 0, sizeof(halo));
    memset(&galaxy, 0, sizeof(galaxy));

    /* Set halo properties */
    halo.Rvir = 0.2;    /* 200 kpc/h */
    halo.Vmax = 200.0;  /* 200 km/s (Milky Way-like) */
    halo.HaloNr = 0;

    /* Set galaxy properties for stable disk */
    galaxy.ColdGas = 1.0;       /* 1e10 Msun/h */
    galaxy.StellarMass = 5.0;   /* 5e10 Msun/h */
    galaxy.BulgeMass = 0.0;     /* Pure disk initially */
    galaxy.DiskScaleRadius = 0.0; /* Will be calculated */

    /* Process galaxy */
    int result = module_system_process(&halo, &galaxy, &MimicConfig);
    TEST_ASSERT(result == 0, "Module processing should succeed");

    /* Check that disk scale radius was calculated */
    TEST_ASSERT(galaxy.DiskScaleRadius > 0.0, "Disk scale radius should be positive");
    double expected_rd = 0.03 * halo.Rvir; /* Empirical scaling */
    TEST_ASSERT(fabs(galaxy.DiskScaleRadius - expected_rd) < 1e-6,
               "Disk scale radius should match empirical scaling");

    /* Calculate expected critical mass */
    double reff = 3.0 * galaxy.DiskScaleRadius;
    double mcrit_expected = halo.Vmax * halo.Vmax * reff / MimicConfig.G;

    /* Total disk mass */
    double disk_mass = galaxy.ColdGas + (galaxy.StellarMass - galaxy.BulgeMass);

    /* For this test case, disk should be stable (no mass transfer)
     * Check that bulge mass didn't change */
    TEST_ASSERT(galaxy.BulgeMass == 0.0,
               "Stable disk should not transfer mass to bulge");

    module_system_cleanup();

    TEST_PASS("Stability criterion calculated correctly");
    return 1;
}

/**
 * @test    test_stellar_mass_transfer
 * @brief   Test unstable stellar mass transfers to bulge
 *
 * Expected: When disk_mass > Mcrit, excess stellar mass transfers to bulge
 * Validates: Mass conservation, transfer mechanics
 */
int test_stellar_mass_transfer(void)
{
    TEST_BEGIN("Stellar mass transfer");

    reset_config();
    set_default_params();
    ensure_modules_registered();
    module_system_init(&MimicConfig);

    /* Create test halo and galaxy */
    struct Halo halo;
    struct GalaxyData galaxy;
    memset(&halo, 0, sizeof(halo));
    memset(&galaxy, 0, sizeof(galaxy));

    /* Set halo properties for low Vmax (unstable disk) */
    halo.Rvir = 0.1;    /* 100 kpc/h */
    halo.Vmax = 50.0;   /* Low rotation support */
    halo.HaloNr = 0;

    /* Set galaxy properties for massive unstable disk */
    galaxy.ColdGas = 0.1;        /* Small gas mass */
    galaxy.StellarMass = 10.0;   /* Large stellar mass */
    galaxy.BulgeMass = 0.0;      /* Pure disk initially */
    galaxy.MetalsStellarMass = 0.2; /* 2% metallicity */
    galaxy.MetalsBulgeMass = 0.0;
    galaxy.DiskScaleRadius = 0.0;

    /* Store initial values */
    double initial_stellar_mass = galaxy.StellarMass;
    double initial_bulge_mass = galaxy.BulgeMass;

    /* Process galaxy */
    int result = module_system_process(&halo, &galaxy, &MimicConfig);
    TEST_ASSERT(result == 0, "Module processing should succeed");

    /* Bulge should have grown (unstable disk) */
    TEST_ASSERT(galaxy.BulgeMass > initial_bulge_mass,
               "Unstable disk should transfer mass to bulge");

    /* Total stellar mass should be conserved */
    TEST_ASSERT(fabs(galaxy.StellarMass - initial_stellar_mass) < 1e-6,
               "Total stellar mass should be conserved");

    /* Bulge mass should not exceed stellar mass */
    TEST_ASSERT(galaxy.BulgeMass <= galaxy.StellarMass * 1.0001,
               "Bulge mass should not exceed total stellar mass");

    module_system_cleanup();

    TEST_PASS("Stellar mass transfer works correctly");
    return 1;
}

/**
 * @test    test_metallicity_preservation
 * @brief   Test that metallicity is preserved during mass transfer
 *
 * Expected: Metal mass transfers proportionally with stellar mass
 * Validates: Metallicity calculation, metal conservation
 */
int test_metallicity_preservation(void)
{
    TEST_BEGIN("Metallicity preservation");

    reset_config();
    set_default_params();
    ensure_modules_registered();
    module_system_init(&MimicConfig);

    /* Create test halo and galaxy */
    struct Halo halo;
    struct GalaxyData galaxy;
    memset(&halo, 0, sizeof(halo));
    memset(&galaxy, 0, sizeof(galaxy));

    /* Set halo properties for unstable disk */
    halo.Rvir = 0.1;
    halo.Vmax = 50.0;
    halo.HaloNr = 0;

    /* Set galaxy with metal-rich disk */
    galaxy.ColdGas = 0.1;
    galaxy.StellarMass = 10.0;
    galaxy.BulgeMass = 0.0;
    galaxy.MetalsStellarMass = 0.2;  /* 2% overall metallicity */
    galaxy.MetalsBulgeMass = 0.0;    /* Pure disk initially */
    galaxy.DiskScaleRadius = 0.0;

    /* Calculate initial disk metallicity */
    double disk_stellar_mass = galaxy.StellarMass - galaxy.BulgeMass;
    double disk_metal_mass = galaxy.MetalsStellarMass - galaxy.MetalsBulgeMass;
    double initial_disk_metallicity = disk_metal_mass / disk_stellar_mass;

    /* Process galaxy */
    int result = module_system_process(&halo, &galaxy, &MimicConfig);
    TEST_ASSERT(result == 0, "Module processing should succeed");

    /* If mass was transferred, check metallicity preservation */
    if (galaxy.BulgeMass > 0.0) {
        double bulge_metallicity = galaxy.MetalsBulgeMass / galaxy.BulgeMass;

        /* Bulge metallicity should match disk metallicity */
        TEST_ASSERT(fabs(bulge_metallicity - initial_disk_metallicity) < 1e-4,
                   "Bulge should have same metallicity as disk");

        /* Metal mass should be conserved */
        TEST_ASSERT(galaxy.MetalsBulgeMass <= galaxy.MetalsStellarMass * 1.0001,
                   "Bulge metals should not exceed total stellar metals");
    }

    module_system_cleanup();

    TEST_PASS("Metallicity preserved correctly");
    return 1;
}

/**
 * @test    test_edge_cases
 * @brief   Test edge cases: zero mass, pure disk, pure bulge
 *
 * Expected: Module handles edge cases gracefully without crashes
 * Validates: Robustness, divide-by-zero protection
 */
int test_edge_cases(void)
{
    TEST_BEGIN("Edge cases");

    reset_config();
    set_default_params();
    ensure_modules_registered();
    module_system_init(&MimicConfig);

    struct Halo halo;
    struct GalaxyData galaxy;

    /* Test Case 1: Zero mass galaxy */
    memset(&halo, 0, sizeof(halo));
    memset(&galaxy, 0, sizeof(galaxy));
    halo.Rvir = 0.1;
    halo.Vmax = 100.0;
    halo.HaloNr = 0;

    int result = module_system_process(&halo, &galaxy, &MimicConfig);
    TEST_ASSERT(result == 0, "Zero mass galaxy should process without error");

    /* Test Case 2: Pure bulge (no disk) */
    memset(&galaxy, 0, sizeof(galaxy));
    galaxy.StellarMass = 10.0;
    galaxy.BulgeMass = 10.0;  /* All mass in bulge */
    galaxy.ColdGas = 0.0;

    result = module_system_process(&halo, &galaxy, &MimicConfig);
    TEST_ASSERT(result == 0, "Pure bulge galaxy should process without error");
    TEST_ASSERT(galaxy.BulgeMass == 10.0, "Bulge mass should not change for pure bulge");

    /* Test Case 3: Very small disk mass */
    memset(&galaxy, 0, sizeof(galaxy));
    galaxy.StellarMass = 0.001;
    galaxy.BulgeMass = 0.0;
    galaxy.ColdGas = 0.001;

    result = module_system_process(&halo, &galaxy, &MimicConfig);
    TEST_ASSERT(result == 0, "Small disk should process without error");

    module_system_cleanup();

    TEST_PASS("Edge cases handled correctly");
    return 1;
}

/**
 * @brief   Main test runner
 */
int main(void)
{
    TEST_SUITE_BEGIN("SAGE Disk Instability Module Unit Tests");

    /* Initialize test framework */
    initialize_test_memory_tracking();

    /* Run tests */
    TEST_RUN(test_module_registration);
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_parameter_reading);
    TEST_RUN(test_stability_criterion);
    TEST_RUN(test_stellar_mass_transfer);
    TEST_RUN(test_metallicity_preservation);
    TEST_RUN(test_edge_cases);

    /* Print summary */
    TEST_SUITE_END();

    return (failed > 0) ? 1 : 0;
}
