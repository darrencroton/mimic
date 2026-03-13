/**
 * @file    test_unit_sage_radio_mode_heating.c
 * @brief   Unit tests for sage_radio_mode_heating module
 *
 * Validates: AGN physics, cooling suppression, accretion modes, edge cases
 *
 * This test validates the sage_radio_mode_heating module physics:
 * - AGN accretion calculations (empirical and cold cloud modes)
 * - Eddington rate limiting
 * - Cooling suppression based on heating radius
 * - Black hole growth and hot gas consumption
 * - Heating radius evolution
 * - Edge cases (zero values, orphan galaxies, AGN off)
 * - Parameter sensitivity
 *
 * Test cases:
 *   - test_empirical_agn_rate_calculation: Empirical accretion mode physics
 *   - test_cold_cloud_agn_rate_above_threshold: Cold cloud mode when triggered
 *   - test_cold_cloud_agn_rate_below_threshold: No accretion below threshold
 *   - test_eddington_limit: Accretion limited by Eddington rate
 *   - test_agn_suppresses_cooling: CoolingGas reduced by AGN
 *   - test_cooling_suppression_by_rheat: Rheat/Rcool suppression logic
 *   - test_heating_radius_update: Rheat increases with AGN heating
 *   - test_heating_energy_tracking: Heating accumulator tracks energy
 *   - test_agn_off_no_changes: No changes when AGN disabled
 *   - test_zero_cooling_gas: No AGN when no cooling
 *   - test_zero_hot_gas: No accretion without hot gas
 *   - test_zero_black_hole_mass: Edge case handling
 *   - test_orphan_galaxy_skipped: Type 2 galaxies unaffected
 *   - test_parameter_sensitivity_efficiency: RadioModeEfficiency affects results
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

/* Module parameters (extern declarations to access module internals for testing) */
extern double RADIO_MODE_EFFICIENCY;
extern int AGN_RECIPE_ON;

/* Module functions (extern declarations for direct testing) */
extern int sage_radio_mode_heating_init(void);
extern int sage_radio_mode_heating_process(struct ModuleContext *ctx,
                                            struct Halo *halos, int ngal);
extern int sage_radio_mode_heating_cleanup(void);

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
 * @param   halo            Halo structure to initialize
 * @param   gal             Galaxy structure to initialize
 * @param   mvir            Virial mass [1e10 Msun/h]
 * @param   vvir            Virial velocity [km/s]
 * @param   rvir            Virial radius [Mpc/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   black_hole_mass Black hole mass [1e10 Msun/h]
 * @param   cooling_gas     Cooling gas rate [1e10 Msun/h per timestep]
 * @param   rcool           Cooling radius [Mpc/h]
 * @param   rheat           Heating radius [Mpc/h]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                               double mvir, double vvir, double rvir,
                               double hot_gas, double black_hole_mass,
                               double cooling_gas, double rcool, double rheat)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = 0;  /* Central galaxy */
    halo->Mvir = (float)mvir;
    halo->Vvir = (float)vvir;
    halo->Rvir = (float)rvir;
    halo->SnapNum = 63;
    halo->dT = 0.01;  /* Timestep for heating rate calculation */
    halo->galaxy = gal;

    gal->HotGas = (float)hot_gas;
    gal->MetalsHotGas = (float)(hot_gas * 0.02);  /* 2% metallicity */
    gal->BlackHoleMass = (float)black_hole_mass;
    gal->CoolingGas = (float)cooling_gas;
    gal->Rcool = (float)rcool;
    gal->Rheat = (float)rheat;
    gal->Heating = 0.0;
}

/**
 * @brief   Setup test parameters
 *
 * @param   efficiency  Radio mode efficiency
 * @param   recipe      AGN recipe (0=off, 1=empirical, 2=Bondi, 3=cold cloud)
 */
static void setup_test_parameters(double efficiency, int recipe)
{
    /* Set model parameters in MimicConfig */
    int idx = 0;

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", efficiency);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "AGNrecipe");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", recipe);

    MimicConfig.NumModelParams = idx;
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

    /* Set unit conversions (typical values) */
    MimicConfig.UnitMass_in_g = 1.989e43;         /* 1e10 Msun/h */
    MimicConfig.UnitTime_in_s = 3.08568e16;       /* ~1 Gyr/h */
    MimicConfig.UnitEnergy_in_cgs = 1.989e53;     /* Energy units */
    MimicConfig.Hubble_h = 0.73;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_empirical_agn_rate_calculation
 * @brief   Test empirical AGN accretion rate calculation
 *
 * Expected: AGN rate scales with BH mass, Vvir^3, and hot gas fraction
 * Validates: Core empirical accretion physics
 */
int test_empirical_agn_rate_calculation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.08;
    setup_test_parameters(efficiency, 1);  /* Empirical mode */

    int result = sage_radio_mode_heating_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create test galaxy with typical massive galaxy properties */
    struct Halo halo;
    struct GalaxyData gal;
    const double mvir = 100.0;         /* 10^12 Msun/h */
    const double vvir = 200.0;         /* 200 km/s */
    const double rvir = 0.2;           /* 200 kpc/h */
    const double hot_gas = 10.0;       /* 10^11 Msun/h */
    const double black_hole_mass = 0.01;  /* 10^8 Msun/h (normalized value) */
    const double cooling_gas = 1.0;    /* Some cooling occurring */
    const double rcool = 0.05;         /* 50 kpc/h */
    const double rheat = 0.0;          /* No prior heating */

    setup_test_galaxy(&halo, &gal, mvir, vvir, rvir, hot_gas, black_hole_mass,
                      cooling_gas, rcool, rheat);

    /* Setup context */
    struct ModuleContext ctx;
    const double dt = 0.01;  /* 10 Myr/h */
    setup_test_context(&ctx, dt);

    /* Store initial values */
    const double initial_bh_mass = gal.BlackHoleMass;
    const double initial_hot_gas = gal.HotGas;

    /* ===== EXECUTE ===== */
    result = sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");

    /* AGN should have some effect (either BH growth or cooling suppression or both) */
    const int bh_grew = (gal.BlackHoleMass > initial_bh_mass);
    const int cooling_suppressed = (gal.CoolingGas < cooling_gas);
    TEST_ASSERT(bh_grew || cooling_suppressed,
                "AGN should have some effect (BH growth or cooling suppression)");

    /* If BH accreted, validate mass conservation */
    if (gal.BlackHoleMass > initial_bh_mass) {
        TEST_ASSERT(gal.HotGas < initial_hot_gas, "Hot gas should decrease if BH accreted");
        const double accreted_mass = gal.BlackHoleMass - initial_bh_mass;
        const double consumed_hot_gas = initial_hot_gas - gal.HotGas;
        TEST_ASSERT_DOUBLE_EQUAL(accreted_mass, consumed_hot_gas, 1e-6,
                                 "Accreted mass should equal consumed hot gas");
    }

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_cold_cloud_agn_rate_above_threshold
 * @brief   Test cold cloud AGN accretion when BH mass above threshold
 *
 * Expected: AGN accretion = 0.01% of cooling rate
 * Validates: Cold cloud mode physics when triggered
 */
int test_cold_cloud_agn_rate_above_threshold(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 3);  /* Cold cloud mode */
    sage_radio_mode_heating_init();

    /* Setup galaxy where BH is above cold cloud threshold */
    struct Halo halo;
    struct GalaxyData gal;
    const double mvir = 100.0;
    const double vvir = 200.0;
    const double rvir = 0.2;
    const double rcool = 0.05;
    const double hot_gas = 10.0;
    const double cooling_gas = 1.0;

    /* Calculate threshold: M_BH > 10^-4 * M_vir * (R_cool/R_vir)^3 */
    const double threshold = 0.0001 * mvir * pow(rcool / rvir, 3.0);
    const double black_hole_mass = threshold * 2.0;  /* Well above threshold */

    setup_test_galaxy(&halo, &gal, mvir, vvir, rvir, hot_gas, black_hole_mass,
                      cooling_gas, rcool, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* AGN should have some effect when above threshold */
    const int bh_grew = (gal.BlackHoleMass > initial_bh_mass);
    const int cooling_suppressed = (gal.CoolingGas < cooling_gas);
    TEST_ASSERT(bh_grew || cooling_suppressed,
                "AGN should have some effect when BH above threshold");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_cold_cloud_agn_rate_below_threshold
 * @brief   Test cold cloud AGN accretion when BH mass below threshold
 *
 * Expected: No AGN accretion, but cooling still suppressed by Rheat
 * Validates: Cold cloud threshold logic
 */
int test_cold_cloud_agn_rate_below_threshold(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 3);  /* Cold cloud mode */
    sage_radio_mode_heating_init();

    /* Setup galaxy where BH is below cold cloud threshold */
    struct Halo halo;
    struct GalaxyData gal;
    const double mvir = 100.0;
    const double vvir = 200.0;
    const double rvir = 0.2;
    const double rcool = 0.05;
    const double hot_gas = 10.0;
    const double cooling_gas = 1.0;

    /* Calculate threshold and set BH mass below it */
    const double threshold = 0.0001 * mvir * pow(rcool / rvir, 3.0);
    const double black_hole_mass = threshold * 0.5;  /* Below threshold */

    setup_test_galaxy(&halo, &gal, mvir, vvir, rvir, hot_gas, black_hole_mass,
                      cooling_gas, rcool, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                             "BH mass should not change when below threshold");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_eddington_limit
 * @brief   Test that AGN accretion is limited by Eddington rate
 *
 * Expected: Accretion limited even with high cooling rate
 * Validates: Eddington rate limiting physics
 */
int test_eddington_limit(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Use maximum valid efficiency */
    setup_test_parameters(1.0, 1);  /* Maximum valid efficiency */
    sage_radio_mode_heating_init();

    /* Small BH mass = low Eddington rate */
    struct Halo halo;
    struct GalaxyData gal;
    const double black_hole_mass = 0.0001;  /* Very small BH */
    const double hot_gas = 10.0;
    const double cooling_gas = 10.0;  /* Very high cooling */

    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, hot_gas, black_hole_mass,
                      cooling_gas, 0.05, 0.0);

    struct ModuleContext ctx;
    const double dt = 0.01;
    setup_test_context(&ctx, dt);

    /* Calculate expected Eddington rate */
    const double edd_luminosity = 1.3e38 * black_hole_mass * 1e10 / ctx.params->Hubble_h;
    const double edd_rate = edd_luminosity /
                           (ctx.params->UnitEnergy_in_cgs / ctx.params->UnitTime_in_s) /
                           (0.1 * 9e10);
    const double max_accretion = edd_rate * dt;

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    const double accreted = gal.BlackHoleMass - initial_bh_mass;
    TEST_ASSERT(accreted > 0.0, "Some accretion should occur");
    TEST_ASSERT(accreted <= max_accretion * 1.01, "Accretion should be Eddington-limited");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_agn_suppresses_cooling
 * @brief   Test that AGN heating suppresses cooling
 *
 * Expected: CoolingGas reduced after AGN heating or BH grows
 * Validates: Cooling suppression mechanism
 */
int test_agn_suppresses_cooling(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double initial_cooling = 1.0;

    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      initial_cooling, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.CoolingGas >= 0.0, "CoolingGas should remain non-negative");

    /* AGN should have some effect */
    const int bh_grew = (gal.BlackHoleMass > initial_bh_mass);
    const int cooling_changed = (gal.CoolingGas != initial_cooling);
    TEST_ASSERT(bh_grew || cooling_changed,
                "AGN should have some effect (BH growth or cooling change)");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_cooling_suppression_by_rheat
 * @brief   Test cooling suppression based on Rheat/Rcool ratio
 *
 * Expected: Complete suppression when Rheat >= Rcool, partial when Rheat < Rcool
 * Validates: Heating radius suppression logic
 */
int test_cooling_suppression_by_rheat(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* Test 1: Rheat >= Rcool → complete suppression */
    struct Halo halo1;
    struct GalaxyData gal1;
    const double rcool = 0.05;
    const double rheat_high = 0.06;  /* rheat > rcool */

    setup_test_galaxy(&halo1, &gal1, 100.0, 200.0, 0.2, 10.0, 0.01,
                      1.0, rcool, rheat_high);

    sage_radio_mode_heating_process(&ctx, &halo1, 1);

    TEST_ASSERT_DOUBLE_EQUAL(gal1.CoolingGas, 0.0, 1e-10,
                             "CoolingGas should be zero when Rheat > Rcool");

    /* Test 2: Rheat < Rcool → partial suppression */
    struct Halo halo2;
    struct GalaxyData gal2;
    const double rheat_low = 0.025;  /* rheat = 0.5 * rcool */
    const double initial_cooling = 1.0;

    setup_test_galaxy(&halo2, &gal2, 100.0, 200.0, 0.2, 10.0, 0.01,
                      initial_cooling, rcool, rheat_low);

    sage_radio_mode_heating_process(&ctx, &halo2, 1);

    /* Expected: CoolingGas = (1 - rheat/rcool) * initial_cooling */
    const double expected_cooling_after_rheat = (1.0 - rheat_low / rcool) * initial_cooling;
    /* After Rheat suppression, AGN will further suppress it */
    TEST_ASSERT(gal2.CoolingGas <= expected_cooling_after_rheat,
                "CoolingGas should be suppressed when Rheat < Rcool");
    TEST_ASSERT(gal2.CoolingGas >= 0.0, "CoolingGas should be non-negative");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_heating_radius_update
 * @brief   Test that heating radius increases with AGN heating
 *
 * Expected: Rheat increases when AGN heats gas
 * Validates: Heating radius evolution
 */
int test_heating_radius_update(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double initial_rheat = 0.01;  /* Small initial heating radius */

    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      1.0, 0.05, initial_rheat);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.Rheat >= initial_rheat, "Rheat should not decrease");
    /* Rheat should increase if AGN heating occurred */
    if (gal.BlackHoleMass > 0.01) {  /* If accretion occurred */
        /* Rheat may increase, or stay the same if heating is small */
        TEST_ASSERT(gal.Rheat <= gal.Rcool * 1.1,
                    "Rheat should not exceed Rcool by large margin");
    }

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_heating_energy_tracking
 * @brief   Test that Heating accumulator tracks AGN heating energy
 *
 * Expected: Heating > 0 when AGN active
 * Validates: Energy tracking mechanism
 */
int test_heating_energy_tracking(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;

    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      1.0, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    TEST_ASSERT_DOUBLE_EQUAL(gal.Heating, 0.0, 1e-10, "Initial Heating should be zero");

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Heating should be positive if AGN was active and suppressed cooling */
    if (gal.BlackHoleMass > 0.01 && gal.CoolingGas < 1.0) {
        TEST_ASSERT(gal.Heating >= 0.0, "Heating should be non-negative");
    }

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_agn_off_no_changes
 * @brief   Test that no changes occur when AGN is disabled
 *
 * Expected: All galaxy properties unchanged
 * Validates: AGNrecipe = 0 disables AGN
 */
int test_agn_off_no_changes(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 0);  /* AGN off */
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double initial_cooling = 1.0;

    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      initial_cooling, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;
    const double initial_hot_gas = gal.HotGas;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                             "BH mass should not change when AGN off");
    TEST_ASSERT_DOUBLE_EQUAL(gal.HotGas, initial_hot_gas, 1e-10,
                             "Hot gas should not change when AGN off");
    /* Note: CoolingGas may still be suppressed by Rheat even with AGN off */

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_cooling_gas
 * @brief   Test that no AGN accretion occurs when cooling is zero
 *
 * Expected: No changes to BH or hot gas
 * Validates: Early exit when CoolingGas = 0
 */
int test_zero_cooling_gas(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;

    /* CoolingGas = 0 */
    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      0.0, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;
    const double initial_hot_gas = gal.HotGas;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                             "BH mass should not change when no cooling");
    TEST_ASSERT_DOUBLE_EQUAL(gal.HotGas, initial_hot_gas, 1e-10,
                             "Hot gas should not change when no cooling");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_hot_gas
 * @brief   Test behavior when HotGas is zero
 *
 * Expected: No accretion (nothing to accrete from)
 * Validates: Edge case handling
 */
int test_zero_hot_gas(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;

    /* HotGas = 0 */
    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 0.0, 0.01,
                      1.0, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                             "BH mass should not change when no hot gas");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_black_hole_mass
 * @brief   Test behavior when black hole mass is zero
 *
 * Expected: Handles gracefully (may or may not accrete depending on mode)
 * Validates: Edge case handling
 */
int test_zero_black_hole_mass(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;

    /* BlackHoleMass = 0 */
    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.0,
                      1.0, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero BH mass gracefully");
    TEST_ASSERT(gal.BlackHoleMass >= 0.0, "BH mass should remain non-negative");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_orphan_galaxy_skipped
 * @brief   Test that orphan galaxies (Type 2) are not processed
 *
 * Expected: No changes to orphan galaxy properties
 * Validates: Type 2 galaxy handling
 */
int test_orphan_galaxy_skipped(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    struct Halo halo;
    struct GalaxyData gal;

    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      1.0, 0.05, 0.0);

    halo.Type = 2;  /* Orphan galaxy */

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    const double initial_bh_mass = gal.BlackHoleMass;
    const double initial_hot_gas = gal.HotGas;
    const double initial_cooling = gal.CoolingGas;

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                             "Orphan BH mass should not change");
    TEST_ASSERT_DOUBLE_EQUAL(gal.HotGas, initial_hot_gas, 1e-10,
                             "Orphan hot gas should not change");
    TEST_ASSERT_DOUBLE_EQUAL(gal.CoolingGas, initial_cooling, 1e-10,
                             "Orphan cooling gas should not change");

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_efficiency
 * @brief   Test that changing RadioModeEfficiency parameter is respected
 *
 * Expected: Module loads different efficiency values correctly
 * Validates: Parameter loading and validation
 */
int test_parameter_sensitivity_efficiency(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test with low efficiency */
    reset_config();
    setup_test_parameters(0.02, 1);
    int result_low = sage_radio_mode_heating_init();
    TEST_ASSERT(result_low == 0, "Module should initialize with low efficiency");
    sage_radio_mode_heating_cleanup();

    /* Test with high efficiency */
    reset_config();
    setup_test_parameters(0.80, 1);
    int result_high = sage_radio_mode_heating_init();
    TEST_ASSERT(result_high == 0, "Module should initialize with high efficiency");
    sage_radio_mode_heating_cleanup();

    /* Test with maximum efficiency */
    reset_config();
    setup_test_parameters(1.0, 1);
    int result_max = sage_radio_mode_heating_init();
    TEST_ASSERT(result_max == 0, "Module should initialize with maximum efficiency");
    sage_radio_mode_heating_cleanup();

    /* Test with minimum efficiency */
    reset_config();
    setup_test_parameters(0.0, 1);
    int result_min = sage_radio_mode_heating_init();
    TEST_ASSERT(result_min == 0, "Module should initialize with minimum efficiency");
    sage_radio_mode_heating_cleanup();

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

    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_radio_mode_heating");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* Set required parameters */
    int idx = 0;
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.08");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "AGNrecipe");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "1");
    MimicConfig.NumModelParams = idx;

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

    setup_test_parameters(0.08, 1);
    sage_radio_mode_heating_init();

    /* Process a galaxy */
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01,
                      1.0, 0.05, 0.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    sage_radio_mode_heating_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will catch any leaks */

    /* ===== CLEANUP ===== */
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_radio_mode_heating unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_radio_mode_heating Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run physics calculation tests */
    TEST_RUN(test_empirical_agn_rate_calculation);
    TEST_RUN(test_cold_cloud_agn_rate_above_threshold);
    TEST_RUN(test_cold_cloud_agn_rate_below_threshold);
    TEST_RUN(test_eddington_limit);
    TEST_RUN(test_agn_suppresses_cooling);
    TEST_RUN(test_cooling_suppression_by_rheat);
    TEST_RUN(test_heating_radius_update);
    TEST_RUN(test_heating_energy_tracking);

    /* Run edge case tests */
    TEST_RUN(test_agn_off_no_changes);
    TEST_RUN(test_zero_cooling_gas);
    TEST_RUN(test_zero_hot_gas);
    TEST_RUN(test_zero_black_hole_mass);
    TEST_RUN(test_orphan_galaxy_skipped);

    /* Run parameter sensitivity tests */
    TEST_RUN(test_parameter_sensitivity_efficiency);

    /* Run infrastructure tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
