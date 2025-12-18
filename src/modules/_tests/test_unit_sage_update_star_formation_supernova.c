/**
 * @file    test_unit_sage_update_star_formation_supernova.c
 * @brief   Unit tests for sage_update_star_formation_supernova module
 *
 * Validates: Physics update logic, gas transfers, metal enrichment, conservation, edge cases
 *
 * This test validates the sage_update_star_formation_supernova module physics:
 * - Star formation updates (gas depletion, stellar mass growth, recycling)
 * - Supernova reheating (cold→hot transfer to central)
 * - Supernova ejection (hot→ejected transfer from central, limiting)
 * - Metal enrichment (yield production, FracZleaveDisk scaling)
 * - Temporary property cleanup (NewStellarMass, etc. zeroed after processing)
 * - Conservation laws (mass, metals)
 * - Edge cases (zero stars, zero gas, ejection exceeds hot gas)
 * - Module lifecycle and memory safety
 *
 * Test cases:
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *   - test_star_formation_with_recycling: Gas depletion and stellar mass growth
 *   - test_star_formation_metal_transfer: Metal conservation during SF
 *   - test_star_formation_rate_accumulation: SFR calculation
 *   - test_reheating_gas_transfer: Cold→hot transfer to central
 *   - test_reheating_metal_transfer: Metals move with reheated gas
 *   - test_reheating_outflow_rate: Outflow rate accumulation
 *   - test_ejection_gas_transfer: Hot→ejected transfer from central
 *   - test_ejection_limiting: Ejection capped at available hot gas
 *   - test_ejection_metal_transfer: Metals move with ejected gas
 *   - test_metal_yield_production: New metals from yield
 *   - test_metal_disk_vs_halo_split: FracZleaveDisk scaling
 *   - test_metal_enrichment_zero_cold_gas: All metals to hot halo when ColdGas=0
 *   - test_zero_stars_no_updates: No changes when NewStellarMass=0
 *   - test_ejection_exceeds_hot_gas: Ejection limited to HotGas
 *   - test_satellite_reheats_to_central: Satellites reheat to central's hot halo
 *   - test_temporary_properties_reset: Cleanup after processing
 *   - test_mass_conservation: Total baryons unchanged
 *   - test_metal_conservation: Total metals conserved with yield production
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
#include "../_shared/metallicity.h"

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
static double test_recycle_fraction = 0.43;
static double test_yield = 0.025;
static double test_frac_z_leave_disk = 0.0;

/* Module functions (extern declarations for direct testing) */
extern int sage_update_star_formation_supernova_init(void);
extern int sage_update_star_formation_supernova_process(struct ModuleContext *ctx,
                                                          struct Halo *halos, int ngal);
extern int sage_update_star_formation_supernova_cleanup(void);

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
 * @brief   Setup module parameters for testing
 *
 * @param   recycle     RecycleFraction [0,1]
 * @param   yield       Metal yield [0,1]
 * @param   frac_z      FracZleaveDisk [0,1]
 */
static void setup_test_parameters(double recycle, double yield, double frac_z)
{
    /* Set model parameters in MimicConfig */
    int idx = 0;

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", recycle);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", yield);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", frac_z);

    MimicConfig.NumModelParams = idx;

    /* Update test variables */
    test_recycle_fraction = recycle;
    test_yield = yield;
    test_frac_z_leave_disk = frac_z;
}

/**
 * @brief   Setup module context for testing
 *
 * @param   ctx             ModuleContext to initialize
 * @param   central         Pointer to central galaxy halo
 * @param   dt              Time step (Gyr/h)
 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central, double dt)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->central_galaxy = central;
    ctx->substep_dt = dt;
    ctx->params = &MimicConfig;
    ctx->redshift = 0.0;
    ctx->time = 13.8;  /* Gyr/h */
    ctx->snapshot_number = 63;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
}

/**
 * @brief   Setup test halo and galaxy with specified properties
 *
 * @param   halo            Halo to initialize
 * @param   galaxy          Galaxy to initialize
 * @param   type            Halo type (0=central, 1=satellite, 2=orphan)
 * @param   mvir            Virial mass (1e10 Msun/h)
 * @param   dt              Time step (Gyr/h) for rate calculations
 * @param   cold_gas        Cold gas mass (1e10 Msun/h)
 * @param   hot_gas         Hot gas mass (1e10 Msun/h)
 * @param   ejected_gas     Ejected gas mass (1e10 Msun/h)
 * @param   stellar_mass    Stellar mass (1e10 Msun/h)
 * @param   metals_cold     Metals in cold gas (1e10 Msun/h)
 * @param   metals_hot      Metals in hot gas (1e10 Msun/h)
 * @param   metals_ejected  Metals in ejected gas (1e10 Msun/h)
 * @param   metals_stellar  Metals in stellar mass (1e10 Msun/h)
 * @param   new_stars       NewStellarMass (calculated by previous module)
 * @param   reheated_mass   SupernovaReheatedMass (calculated by previous module)
 * @param   ejected_mass    SupernovaEjectedMass (calculated by previous module)
 */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy,
                             int type, double mvir, double dt,
                             double cold_gas, double hot_gas, double ejected_gas, double stellar_mass,
                             double metals_cold, double metals_hot, double metals_ejected, double metals_stellar,
                             double new_stars, double reheated_mass, double ejected_mass)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(galaxy, 0, sizeof(struct GalaxyData));

    halo->Type = type;
    halo->Mvir = mvir;
    halo->Vvir = 150.0;  /* km/s */
    halo->SnapNum = 63;
    halo->dT = dt;
    halo->galaxy = galaxy;

    galaxy->ColdGas = cold_gas;
    galaxy->HotGas = hot_gas;
    galaxy->EjectedGas = ejected_gas;
    galaxy->StellarMass = stellar_mass;
    galaxy->MetalsColdGas = metals_cold;
    galaxy->MetalsHotGas = metals_hot;
    galaxy->MetalsEjectedGas = metals_ejected;
    galaxy->MetalsStellarMass = metals_stellar;
    galaxy->NewStellarMass = new_stars;
    galaxy->SupernovaReheatedMass = reheated_mass;
    galaxy->SupernovaEjectedMass = ejected_mass;
    galaxy->StarFormationRate = 0.0;
    galaxy->SupernovaOutflowRate = 0.0;
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

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

    /* Configure sage_update_star_formation_supernova module in phase_1 */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_update_star_formation_supernova");
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
    MimicConfig.phase_1[0].module_name = strdup("sage_update_star_formation_supernova");
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
// STAR FORMATION UPDATE TESTS
// ============================================================================

/**
 * @test    test_star_formation_with_recycling
 * @brief   Test star formation updates with recycling fraction
 *
 * Physics: ColdGas -= (1 - RecycleFraction) * NewStellarMass
 *          StellarMass += (1 - RecycleFraction) * NewStellarMass
 *
 * Expected: Gas depletion and stellar mass growth respect recycling fraction
 * Validates: Recycling fraction correctly applied
 */
int test_star_formation_with_recycling(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);  /* RecycleFraction=0.43, no yield */

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,  /* ColdGas, HotGas, EjectedGas, StellarMass */
                    0.0, 0.0, 0.0, 0.0,     /* Metals */
                    0.0, 0.0, 0.0);         /* NewStellarMass, SupernovaReheatedMass, SupernovaEjectedMass */

    /* Setup test galaxy: 10 ColdGas, form 1.0 new stars */
    double initial_cold = 10.0;
    double initial_stellar = 20.0;
    double new_stars = 1.0;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    initial_cold, 50.0, 0.0, initial_stellar,
                    0.0, 0.0, 0.0, 0.0,
                    new_stars, 0.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Expected: Gas consumed = (1 - RecycleFraction) * stars = (1 - 0.43) * 1.0 = 0.57 */
    double expected_gas_consumed = (1.0 - test_recycle_fraction) * new_stars;
    double expected_cold = initial_cold - expected_gas_consumed;
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, expected_cold, 1e-6,
                            "ColdGas should decrease by (1 - RecycleFraction) * NewStellarMass");

    /* Expected: Stellar mass gain = (1 - RecycleFraction) * stars = 0.57 */
    double expected_stellar = initial_stellar + expected_gas_consumed;
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StellarMass, expected_stellar, 1e-6,
                            "StellarMass should increase by (1 - RecycleFraction) * NewStellarMass");

    return TEST_PASS;
}

/**
 * @test    test_star_formation_metal_transfer
 * @brief   Test that metals move from cold gas to stellar mass during star formation
 *
 * Physics: MetalsColdGas -= metallicity * (1 - RecycleFraction) * NewStellarMass
 *          MetalsStellarMass += metallicity * (1 - RecycleFraction) * NewStellarMass
 *
 * Expected: Metals conserved (transferred, not destroyed)
 * Validates: Metal conservation during star formation
 */
int test_star_formation_metal_transfer(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Setup with metallicity = 0.02 (solar) */
    double initial_cold = 10.0;
    double initial_metals_cold = 0.2;  /* Z = 0.2/10.0 = 0.02 */
    double initial_metals_stellar = 0.3;
    double new_stars = 1.0;
    double metallicity = initial_metals_cold / initial_cold;

    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    initial_cold, 50.0, 0.0, 20.0,
                    initial_metals_cold, 0.0, 0.0, initial_metals_stellar,
                    new_stars, 0.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Expected metal transfer */
    double metal_transferred = metallicity * (1.0 - test_recycle_fraction) * new_stars;
    double expected_metals_cold = initial_metals_cold - metal_transferred;
    double expected_metals_stellar = initial_metals_stellar + metal_transferred;

    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsColdGas, expected_metals_cold, 1e-6,
                            "MetalsColdGas should decrease");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsStellarMass, expected_metals_stellar, 1e-6,
                            "MetalsStellarMass should increase by same amount");

    return TEST_PASS;
}

/**
 * @test    test_star_formation_rate_accumulation
 * @brief   Test that StarFormationRate is calculated correctly
 *
 * Physics: StarFormationRate += NewStellarMass / dT
 *
 * Expected: SFR in code units (1e10 Msun/h) / (Gyr/h)
 * Validates: SFR accumulation
 */
int test_star_formation_rate_accumulation(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    double dt = 0.1;  /* Gyr/h */
    double new_stars = 1.0;  /* 1e10 Msun/h */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, dt,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    new_stars, 0.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, dt);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Expected SFR = stars / dt = 1.0 / 0.1 = 10.0 */
    double expected_sfr = new_stars / dt;
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StarFormationRate, expected_sfr, 1e-6,
                            "StarFormationRate should equal NewStellarMass / dT");

    return TEST_PASS;
}

// ============================================================================
// SUPERNOVA REHEATING TESTS
// ============================================================================

/**
 * @test    test_reheating_gas_transfer
 * @brief   Test that reheating transfers gas from galaxy's cold to central's hot
 *
 * Physics: Galaxy's ColdGas -= SupernovaReheatedMass
 *          Central's HotGas += SupernovaReheatedMass
 *
 * Expected: Gas moved from galaxy to central (satellites too)
 * Validates: Reheating gas transfer logic
 */
int test_reheating_gas_transfer(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central */
    double central_hot_initial = 50.0;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, central_hot_initial, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Setup test galaxy: 10 ColdGas, form 1.0 stars, reheat 0.5 */
    double initial_cold = 10.0;
    double new_stars = 1.0;
    double reheated_mass = 0.5;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    initial_cold, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    new_stars, reheated_mass, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Galaxy's cold gas should decrease by reheated mass (plus SF depletion) */
    double sf_depletion = (1.0 - test_recycle_fraction) * new_stars;
    double expected_cold = initial_cold - sf_depletion - reheated_mass;
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, expected_cold, 1e-6,
                            "Galaxy ColdGas should decrease by SF + reheating");

    /* Central's hot gas should increase by reheated mass */
    double expected_central_hot = central_hot_initial + reheated_mass;
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, expected_central_hot, 1e-6,
                            "Central HotGas should increase by reheated mass");

    return TEST_PASS;
}

/**
 * @test    test_reheating_metal_transfer
 * @brief   Test that metals move with reheated gas
 *
 * Physics: Metals transferred at galaxy's cold gas metallicity
 *
 * Expected: Metals conserved during reheating transfer
 * Validates: Metal transfer during reheating
 */
int test_reheating_metal_transfer(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    double central_metals_hot_initial = 0.5;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, central_metals_hot_initial, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Setup with metallicity = 0.02 */
    double initial_cold = 10.0;
    double initial_metals_cold = 0.2;  /* Z = 0.02 */
    double new_stars = 1.0;
    double reheated_mass = 0.5;
    double metallicity_before_sf = initial_metals_cold / initial_cold;

    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    initial_cold, 50.0, 0.0, 20.0,
                    initial_metals_cold, 0.0, 0.0, 0.0,
                    new_stars, reheated_mass, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Metallicity is recomputed after SF, so we need to calculate it */
    double sf_gas_consumed = (1.0 - test_recycle_fraction) * new_stars;
    double sf_metals_consumed = metallicity_before_sf * sf_gas_consumed;
    double cold_gas_after_sf = initial_cold - sf_gas_consumed;
    double metals_cold_after_sf = initial_metals_cold - sf_metals_consumed;
    double metallicity_after_sf = (cold_gas_after_sf > 0) ? (metals_cold_after_sf / cold_gas_after_sf) : 0.0;

    /* Expected metal transfer during reheating */
    double metal_transferred_reheat = metallicity_after_sf * reheated_mass;
    double expected_metals_cold = metals_cold_after_sf - metal_transferred_reheat;
    double expected_central_metals_hot = central_metals_hot_initial + metal_transferred_reheat;

    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsColdGas, expected_metals_cold, 1e-6,
                            "Galaxy MetalsColdGas should decrease by reheated amount");
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsHotGas, expected_central_metals_hot, 1e-6,
                            "Central MetalsHotGas should increase by same amount");

    return TEST_PASS;
}

/**
 * @test    test_reheating_outflow_rate
 * @brief   Test that SupernovaOutflowRate is calculated correctly
 *
 * Physics: SupernovaOutflowRate += SupernovaReheatedMass / dT
 *
 * Expected: Outflow rate in code units
 * Validates: Outflow rate accumulation
 */
int test_reheating_outflow_rate(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    double dt = 0.1;  /* Gyr/h */
    double reheated_mass = 0.5;  /* 1e10 Msun/h */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, dt,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    1.0, reheated_mass, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, dt);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Expected outflow rate = reheated_mass / dt = 0.5 / 0.1 = 5.0 */
    double expected_outflow_rate = reheated_mass / dt;
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaOutflowRate, expected_outflow_rate, 1e-6,
                            "SupernovaOutflowRate should equal SupernovaReheatedMass / dT");

    return TEST_PASS;
}

// ============================================================================
// SUPERNOVA EJECTION TESTS
// ============================================================================

/**
 * @test    test_ejection_gas_transfer
 * @brief   Test that ejection transfers gas from central's hot to central's ejected
 *
 * Physics: Central's HotGas -= SupernovaEjectedMass
 *          Central's EjectedGas += SupernovaEjectedMass
 *
 * Expected: Gas moved within central halo
 * Validates: Ejection gas transfer logic
 */
int test_ejection_gas_transfer(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central with plenty of hot gas */
    double central_hot_initial = 50.0;
    double central_ejected_initial = 5.0;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, central_hot_initial, central_ejected_initial, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Galaxy forms stars, reheats, and ejects */
    double new_stars = 1.0;
    double reheated_mass = 0.5;
    double ejected_mass = 0.3;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    new_stars, reheated_mass, ejected_mass);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Central's hot gas: gain from reheating, lose to ejection */
    double expected_central_hot = central_hot_initial + reheated_mass - ejected_mass;
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, expected_central_hot, 1e-6,
                            "Central HotGas should gain from reheating and lose to ejection");

    /* Central's ejected gas should increase */
    double expected_central_ejected = central_ejected_initial + ejected_mass;
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.EjectedGas, expected_central_ejected, 1e-6,
                            "Central EjectedGas should increase by ejected mass");

    return TEST_PASS;
}

/**
 * @test    test_ejection_limiting
 * @brief   Test that ejection is limited to available hot gas
 *
 * Physics: If SupernovaEjectedMass > Central's HotGas, ejection = HotGas
 *
 * Expected: Ejection capped at available hot gas
 * Validates: Ejection limiting logic
 */
int test_ejection_limiting(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central with limited hot gas (less than requested ejection) */
    double central_hot_initial = 0.2;  /* Only 0.2 available */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, central_hot_initial, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Galaxy wants to eject 0.5, but central only has 0.2 */
    double requested_ejection = 0.5;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    1.0, 0.0, requested_ejection);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Central's hot gas should be completely depleted */
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, 0.0, 1e-6,
                            "Central HotGas should be zero (all ejected)");

    /* Central's ejected gas should increase by only what was available */
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.EjectedGas, central_hot_initial, 1e-6,
                            "Ejection should be limited to available HotGas");

    return TEST_PASS;
}

/**
 * @test    test_ejection_metal_transfer
 * @brief   Test that metals move with ejected gas
 *
 * Physics: Metals transferred at central's hot gas metallicity
 *
 * Expected: Metals conserved during ejection transfer
 * Validates: Metal transfer during ejection
 */
int test_ejection_metal_transfer(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central with hot gas metallicity = 0.01 */
    double central_hot_initial = 50.0;
    double central_metals_hot_initial = 0.5;  /* Z = 0.5/50 = 0.01 */
    double central_metals_ejected_initial = 0.1;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, central_hot_initial, 5.0, 20.0,
                    0.0, central_metals_hot_initial, central_metals_ejected_initial, 0.0,
                    0.0, 0.0, 0.0);

    double ejected_mass = 0.3;
    double metallicity_hot = central_metals_hot_initial / central_hot_initial;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    1.0, 0.0, ejected_mass);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Expected metal transfer */
    double metal_transferred = metallicity_hot * ejected_mass;
    double expected_metals_hot = central_metals_hot_initial - metal_transferred;
    double expected_metals_ejected = central_metals_ejected_initial + metal_transferred;

    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsHotGas, expected_metals_hot, 1e-6,
                            "Central MetalsHotGas should decrease");
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsEjectedGas, expected_metals_ejected, 1e-6,
                            "Central MetalsEjectedGas should increase by same amount");

    return TEST_PASS;
}

// ============================================================================
// METAL ENRICHMENT TESTS
// ============================================================================

/**
 * @test    test_metal_yield_production
 * @brief   Test that new metals are produced from stellar yields
 *
 * Physics: Total metals increase by Yield * NewStellarMass
 *
 * Expected: New metals created (not conserved - stellar nucleosynthesis)
 * Validates: Metal yield production
 */
int test_metal_yield_production(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.025, 0.0);  /* Yield = 0.025 */

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Galaxy with plenty of cold gas and forms 1.0 new stars */
    double initial_metals_cold = 0.2;
    double initial_metals_hot = 0.5;
    double new_stars = 1.0;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    initial_metals_cold, initial_metals_hot, 0.0, 0.0,
                    new_stars, 0.0, 0.0);

    /* Calculate total metals before */
    double total_metals_before = initial_metals_cold + initial_metals_hot;

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Calculate total metals after (all reservoirs) */
    double total_metals_after = test_galaxy.MetalsColdGas + test_galaxy.MetalsHotGas +
                                test_galaxy.MetalsEjectedGas + test_galaxy.MetalsStellarMass;

    /* Expected new metals = Yield * NewStellarMass = 0.025 * 1.0 = 0.025 */
    double expected_new_metals = test_yield * new_stars;
    double expected_total_metals = total_metals_before + expected_new_metals;

    TEST_ASSERT_DOUBLE_EQUAL(total_metals_after, expected_total_metals, 1e-6,
                            "Total metals should increase by Yield * NewStellarMass");

    return TEST_PASS;
}

/**
 * @test    test_metal_disk_vs_halo_split
 * @brief   Test that metal enrichment splits between disk and hot halo
 *
 * Physics: FracZleaveDiskVal = FracZleaveDisk * exp(-Mvir/30)
 *          Disk gets: Yield * (1 - FracZleaveDiskVal) * NewStellarMass
 *          Halo gets: Yield * FracZleaveDiskVal * NewStellarMass
 *
 * Expected: Low mass halos retain more metals in disk
 * Validates: FracZleaveDisk scaling with halo mass
 */
int test_metal_disk_vs_halo_split(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.025, 0.5);  /* Yield=0.025, FracZleaveDisk=0.5 */

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;

    /* Setup test galaxy as a central (processes itself) */
    double mvir = 100.0;  /* 10^12 Msun/h */
    double initial_metals_cold = 0.2;
    double initial_metals_hot = 0.5;
    double new_stars = 1.0;
    setup_test_halo(&test_halo, &test_galaxy, 0, mvir, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    initial_metals_cold, initial_metals_hot, 0.0, 0.0,
                    new_stars, 0.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &test_halo, 0.1);  /* Central points to itself */

    /* Store initial metals */
    double metals_cold_initial = initial_metals_cold;
    double metals_hot_initial = initial_metals_hot;

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Calculate expected split */
    double frac_z_leave_disk_val = test_frac_z_leave_disk * exp(-1.0 * mvir / 30.0);
    double metals_to_disk = test_yield * (1.0 - frac_z_leave_disk_val) * new_stars;
    double metals_to_halo = test_yield * frac_z_leave_disk_val * new_stars;

    /* Account for SF metal transfer and yield production */
    double metallicity_before_sf = metals_cold_initial / 10.0;
    double metals_from_sf_transfer = metallicity_before_sf * (1.0 - test_recycle_fraction) * new_stars;
    double expected_metals_cold = metals_cold_initial - metals_from_sf_transfer + metals_to_disk;
    double expected_metals_hot = metals_hot_initial + metals_to_halo;

    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsColdGas, expected_metals_cold, 1e-5,
                            "MetalsColdGas should include yield production to disk");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsHotGas, expected_metals_hot, 1e-5,
                            "MetalsHotGas should include yield production to halo");

    return TEST_PASS;
}

/**
 * @test    test_metal_enrichment_zero_cold_gas
 * @brief   Test metal enrichment when ColdGas = 0 (all metals go to hot halo)
 *
 * Physics: If ColdGas <= EPSILON_SMALL, all new metals go to central's hot halo
 *
 * Expected: All yield metals added to hot halo
 * Validates: Zero cold gas edge case
 */
int test_metal_enrichment_zero_cold_gas(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.025, 0.5);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    double initial_central_metals_hot = 0.5;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, initial_central_metals_hot, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Galaxy with exactly enough cold gas for SF, will have ~0 after depletion */
    double new_stars = 1.0;
    double gas_consumed = (1.0 - test_recycle_fraction) * new_stars;
    double initial_cold = gas_consumed;  /* Exactly consumed by SF */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    initial_cold, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    new_stars, 0.0, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* All yield metals should go to central's hot halo */
    double expected_new_metals = test_yield * new_stars;
    double expected_central_metals_hot = initial_central_metals_hot + expected_new_metals;

    TEST_ASSERT(test_galaxy.ColdGas < 1e-6,
                "ColdGas should be ~0 after SF consumption");
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsHotGas, expected_central_metals_hot, 1e-6,
                            "All yield metals should go to central's hot halo when ColdGas=0");

    return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_stars_no_updates
 * @brief   Test that no updates occur when NewStellarMass = 0
 *
 * Expected: All properties unchanged except temporary properties zeroed
 * Validates: Early exit logic for zero star formation
 */
int test_zero_stars_no_updates(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.025, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 5.0, 20.0,
                    0.0, 0.5, 0.1, 0.0,
                    0.0, 0.0, 0.0);

    /* Galaxy with zero star formation */
    double initial_cold = 10.0;
    double initial_hot = 50.0;
    double initial_ejected = 5.0;
    double initial_stellar = 20.0;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    initial_cold, initial_hot, initial_ejected, initial_stellar,
                    0.2, 0.5, 0.1, 0.3,
                    0.0, 0.0, 0.0);  /* NewStellarMass = 0 */

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* All properties should be unchanged */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, initial_cold, 1e-6,
                            "ColdGas should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.HotGas, initial_hot, 1e-6,
                            "HotGas should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.EjectedGas, initial_ejected, 1e-6,
                            "EjectedGas should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StellarMass, initial_stellar, 1e-6,
                            "StellarMass should be unchanged");

    /* Temporary properties should be zeroed */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, 0.0, 1e-6,
                            "NewStellarMass should be zeroed");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, 0.0, 1e-6,
                            "SupernovaReheatedMass should be zeroed");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-6,
                            "SupernovaEjectedMass should be zeroed");

    return TEST_PASS;
}

/**
 * @test    test_satellite_reheats_to_central
 * @brief   Test that satellites reheat gas to central's hot halo (not their own)
 *
 * Physics: Satellites are stripped, so reheated gas goes to central
 *
 * Expected: Satellite's cold gas decreases, central's hot gas increases
 * Validates: Satellite vs central behavior
 */
int test_satellite_reheats_to_central(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo satellite_halo, central_halo;
    struct GalaxyData satellite_galaxy, central_galaxy;

    /* Setup central */
    double central_hot_initial = 50.0;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, central_hot_initial, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Setup satellite (Type=1) */
    double satellite_cold_initial = 5.0;
    double satellite_hot_initial = 10.0;  /* Satellite has own hot gas, but reheating goes to central */
    double new_stars = 0.5;
    double reheated_mass = 0.3;
    setup_test_halo(&satellite_halo, &satellite_galaxy, 1, 20.0, 0.1,  /* Type=1 (satellite) */
                    satellite_cold_initial, satellite_hot_initial, 0.0, 5.0,
                    0.0, 0.0, 0.0, 0.0,
                    new_stars, reheated_mass, 0.0);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &satellite_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Satellite's cold gas should decrease */
    double sf_depletion = (1.0 - test_recycle_fraction) * new_stars;
    double expected_satellite_cold = satellite_cold_initial - sf_depletion - reheated_mass;
    TEST_ASSERT_DOUBLE_EQUAL(satellite_galaxy.ColdGas, expected_satellite_cold, 1e-6,
                            "Satellite ColdGas should decrease by SF + reheating");

    /* Satellite's hot gas should be unchanged (reheating goes to central) */
    TEST_ASSERT_DOUBLE_EQUAL(satellite_galaxy.HotGas, satellite_hot_initial, 1e-6,
                            "Satellite HotGas should be unchanged");

    /* Central's hot gas should increase */
    double expected_central_hot = central_hot_initial + reheated_mass;
    TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, expected_central_hot, 1e-6,
                            "Central HotGas should increase by satellite's reheated mass");

    return TEST_PASS;
}

// ============================================================================
// CLEANUP AND CONSERVATION TESTS
// ============================================================================

/**
 * @test    test_temporary_properties_reset
 * @brief   Test that temporary properties are zeroed after processing
 *
 * Physics: NewStellarMass, SupernovaReheatedMass, SupernovaEjectedMass must be zeroed
 *
 * Expected: All temporary properties = 0 after processing
 * Validates: Cleanup logic prevents accumulation across timesteps
 */
int test_temporary_properties_reset(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Galaxy with non-zero temporary properties */
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 0.0, 20.0,
                    0.0, 0.0, 0.0, 0.0,
                    1.0, 0.5, 0.3);  /* Non-zero temporary values */

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* All temporary properties should be zero */
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, 0.0, 1e-6,
                            "NewStellarMass should be zeroed after processing");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, 0.0, 1e-6,
                            "SupernovaReheatedMass should be zeroed after processing");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-6,
                            "SupernovaEjectedMass should be zeroed after processing");

    return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Test that total baryonic mass is conserved (excluding yield production)
 *
 * Physics: (ColdGas + HotGas + EjectedGas + StellarMass)_after =
 *          (ColdGas + HotGas + EjectedGas + StellarMass)_before
 *
 * Expected: Total baryons unchanged (recycling preserves mass)
 * Validates: Mass conservation
 */
int test_mass_conservation(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.0, 0.0);  /* Yield=0 to test pure conservation */

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central */
    double central_cold_initial = 10.0;
    double central_hot_initial = 50.0;
    double central_ejected_initial = 5.0;
    double central_stellar_initial = 20.0;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    central_cold_initial, central_hot_initial, central_ejected_initial, central_stellar_initial,
                    0.0, 0.0, 0.0, 0.0,
                    0.0, 0.0, 0.0);

    /* Setup test galaxy */
    double galaxy_cold_initial = 10.0;
    double galaxy_hot_initial = 50.0;
    double galaxy_ejected_initial = 5.0;
    double galaxy_stellar_initial = 20.0;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    galaxy_cold_initial, galaxy_hot_initial, galaxy_ejected_initial, galaxy_stellar_initial,
                    0.0, 0.0, 0.0, 0.0,
                    1.0, 0.5, 0.3);

    /* Calculate total baryons before (galaxy + central since transfers happen) */
    double total_before = (galaxy_cold_initial + galaxy_hot_initial + galaxy_ejected_initial + galaxy_stellar_initial) +
                         (central_cold_initial + central_hot_initial + central_ejected_initial + central_stellar_initial);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Calculate total baryons after */
    double total_after = (test_galaxy.ColdGas + test_galaxy.HotGas + test_galaxy.EjectedGas + test_galaxy.StellarMass) +
                        (central_galaxy.ColdGas + central_galaxy.HotGas + central_galaxy.EjectedGas + central_galaxy.StellarMass);

    TEST_ASSERT_DOUBLE_EQUAL(total_after, total_before, 1e-5,
                            "Total baryonic mass should be conserved");

    return TEST_PASS;
}

/**
 * @test    test_metal_conservation
 * @brief   Test that total metals are conserved (except for new yield production)
 *
 * Physics: Total_metals_after = Total_metals_before + Yield * NewStellarMass
 *
 * Expected: Metals conserved with yield production accounted for
 * Validates: Metal conservation with stellar nucleosynthesis
 */
int test_metal_conservation(void)
{
    /* ===== SETUP ===== */
    setup_test_parameters(0.43, 0.025, 0.0);  /* Yield=0.025 */

    int init_result = sage_update_star_formation_supernova_init();
    TEST_ASSERT(init_result == 0, "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;

    /* Setup central */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 5.0, 20.0,
                    0.1, 0.5, 0.05, 0.3,
                    0.0, 0.0, 0.0);

    /* Setup test galaxy */
    double new_stars = 1.0;
    setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1,
                    10.0, 50.0, 5.0, 20.0,
                    0.2, 0.5, 0.1, 0.4,
                    new_stars, 0.5, 0.3);

    /* Calculate total metals before (all reservoirs, galaxy + central) */
    double total_metals_before = (test_galaxy.MetalsColdGas + test_galaxy.MetalsHotGas +
                                  test_galaxy.MetalsEjectedGas + test_galaxy.MetalsStellarMass) +
                                 (central_galaxy.MetalsColdGas + central_galaxy.MetalsHotGas +
                                  central_galaxy.MetalsEjectedGas + central_galaxy.MetalsStellarMass);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.1);

    /* ===== EXECUTE ===== */
    int result = sage_update_star_formation_supernova_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    /* Calculate total metals after */
    double total_metals_after = (test_galaxy.MetalsColdGas + test_galaxy.MetalsHotGas +
                                 test_galaxy.MetalsEjectedGas + test_galaxy.MetalsStellarMass) +
                                (central_galaxy.MetalsColdGas + central_galaxy.MetalsHotGas +
                                 central_galaxy.MetalsEjectedGas + central_galaxy.MetalsStellarMass);

    /* Expected new metals from yield */
    double expected_new_metals = test_yield * new_stars;
    double expected_total_metals = total_metals_before + expected_new_metals;

    TEST_ASSERT_DOUBLE_EQUAL(total_metals_after, expected_total_metals, 1e-5,
                            "Total metals should equal initial + yield production");

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_update_star_formation_supernova unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_update_star_formation_supernova Module\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run software quality tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Run star formation update tests */
    TEST_RUN(test_star_formation_with_recycling);
    TEST_RUN(test_star_formation_metal_transfer);
    TEST_RUN(test_star_formation_rate_accumulation);

    /* Run supernova reheating tests */
    TEST_RUN(test_reheating_gas_transfer);
    TEST_RUN(test_reheating_metal_transfer);
    TEST_RUN(test_reheating_outflow_rate);

    /* Run supernova ejection tests */
    TEST_RUN(test_ejection_gas_transfer);
    TEST_RUN(test_ejection_limiting);
    TEST_RUN(test_ejection_metal_transfer);

    /* Run metal enrichment tests */
    TEST_RUN(test_metal_yield_production);
    TEST_RUN(test_metal_disk_vs_halo_split);
    TEST_RUN(test_metal_enrichment_zero_cold_gas);

    /* Run edge case tests */
    TEST_RUN(test_zero_stars_no_updates);
    TEST_RUN(test_satellite_reheats_to_central);

    /* Run cleanup and conservation tests */
    TEST_RUN(test_temporary_properties_reset);
    TEST_RUN(test_mass_conservation);
    TEST_RUN(test_metal_conservation);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
