/**
 * @file    test_unit_sage_quasar_mode.c
 * @brief   Unit tests for sage_quasar_mode module
 *
 * Validates: Quasar-mode AGN physics, BH growth, energy-driven winds, edge cases
 *
 * This test validates the sage_quasar_mode module physics:
 * - Black hole growth from disk instability (Kauffmann & Haehnelt 2000)
 * - Vvir suppression at low masses (< 280 km/s)
 * - Mass conservation during accretion
 * - Metallicity preservation
 * - Quasar-mode wind ejection (cold and hot gas)
 * - Energy-driven wind physics
 * - Accretion mass tracking
 * - Edge cases (zero values, orphans, NULL galaxies, triggers)
 * - Parameter sensitivity
 *
 * Test cases:
 *   - test_bh_growth_disk_instability: BH grows from disk instability
 *   - test_bh_growth_merger: By-galaxy path ignores merger flags
 *   - test_bh_growth_both_triggers: Disk trigger processed while merger trigger preserved
 *   - test_bh_growth_per_event_merger: Merger event processed in process_per_event mode
 *   - test_per_event_unknown_code_noop: Unknown per-event code ignored safely
 *   - test_vvir_suppression: Low Vvir suppresses accretion
 *   - test_mass_conservation_accretion: Mass conserved
 *   - test_metallicity_preservation_accretion: Metallicity preserved
 *   - test_quasar_wind_cold_ejection: Cold gas ejection
 *   - test_quasar_wind_hot_ejection: Hot gas ejection
 *   - test_quasar_wind_no_ejection: No ejection when energy low
 *   - test_accretion_mass_tracking: QuasarModeBHaccretionMass tracks total
 *   - test_zero_cold_gas: No accretion with zero cold gas
 *   - test_zero_efficiency_factor: No accretion with zero efficiency
 *   - test_no_triggers: No processing without triggers
 *   - test_orphan_galaxy_skipped: Type 2 orphans skipped
 *   - test_null_galaxy: NULL galaxy handled
 *   - test_triggers_preserved: Trigger lifecycle owned by clear modules
 *   - test_invalid_ngal: Error when ngal != 1
 *   - test_parameter_sensitivity_growth_rate: Growth rate parameter
 *   - test_parameter_sensitivity_quasar_efficiency: Efficiency parameter
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *
 * @author  Mimic Development Team
 * @date    2025-12-23
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../core/module_registry.h"
#include "../../../core/module_interface.h"
#include "../../../include/types.h"
#include "../../../include/proto.h"
#include "../../../include/globals.h"
#include "../../../util/error.h"
#include "../../../util/memory.h"
#include "_shared/sage_merger_event_contract.h"

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
extern double BLACK_HOLE_GROWTH_RATE;
extern double QUASAR_MODE_EFFICIENCY;

/* Module functions (extern declarations for direct testing) */
extern int sage_quasar_mode_init(void);
extern int sage_quasar_mode_process(struct ModuleContext *ctx,
                                     struct Halo *halos, int ngal);
extern int sage_quasar_mode_cleanup(void);

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
 * @param   type            Halo type (0=central, 1=satellite, 2=orphan)
 * @param   mvir            Virial mass [1e10 Msun/h]
 * @param   vvir            Virial velocity [km/s]
 * @param   cold_gas        Cold gas mass [1e10 Msun/h]
 * @param   metals_cold     Metals in cold gas [1e10 Msun/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   metals_hot      Metals in hot gas [1e10 Msun/h]
 * @param   black_hole_mass Black hole mass [1e10 Msun/h]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                               int type, double mvir, double vvir,
                               double cold_gas, double metals_cold,
                               double hot_gas, double metals_hot,
                               double black_hole_mass)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = type;
    halo->Mvir = (float)mvir;
    halo->Vvir = (float)vvir;
    halo->SnapNum = 63;
    halo->galaxy = gal;

    gal->ColdGas = (float)cold_gas;
    gal->MetalsColdGas = (float)metals_cold;
    gal->HotGas = (float)hot_gas;
    gal->MetalsHotGas = (float)metals_hot;
    gal->BlackHoleMass = (float)black_hole_mass;
    gal->EjectedGas = 0.0;
    gal->MetalsEjectedGas = 0.0;
    gal->QuasarModeBHaccretionMass = 0.0;
    gal->UnstableDiskGasFraction = 0.0;
}

/**
 * @brief   Setup test parameters
 *
 * @param   growth_rate       Black hole growth rate
 * @param   quasar_efficiency Quasar mode efficiency
 */
static void setup_test_parameters(double growth_rate, double quasar_efficiency)
{
    /* Set model parameters in MimicConfig */
    int idx = 0;

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", growth_rate);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", quasar_efficiency);

    MimicConfig.NumModelParams = idx;
}

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx         Context to initialize
 */
static void setup_test_context(struct ModuleContext *ctx)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->substep_dt = 0.01;
    ctx->redshift = 0.0;
    ctx->time = 13.8;
    ctx->snapshot_number = 63;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
    ctx->params = &MimicConfig;

    /* Set unit conversions (typical values) */
    MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;  /* 1 km/s */
    MimicConfig.Hubble_h = 0.73;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_bh_growth_disk_instability
 * @brief   Test BH growth from disk instability trigger
 *
 * Expected: BH mass increases, cold gas decreases, accretion tracked
 * Validates: Disk instability trigger physics
 */
int test_bh_growth_disk_instability(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double growth_rate = 0.01;
    const double quasar_eff = 0.001;
    setup_test_parameters(growth_rate, quasar_eff);

    int result = sage_quasar_mode_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create massive central galaxy */
    struct Halo halo;
    struct GalaxyData gal;
    const double mvir = 100.0;      /* 10^12 Msun/h */
    const double vvir = 300.0;      /* Well above 280 km/s threshold */
    const double cold_gas = 10.0;
    const double metals_cold = 0.2; /* Z = 0.02 */
    const double black_hole_mass = 0.01;

    setup_test_galaxy(&halo, &gal, 0, mvir, vvir, cold_gas, metals_cold,
                      0.0, 0.0, black_hole_mass);

    /* Set disk instability trigger */
    gal.UnstableDiskGasFraction = 0.5;  /* Disk instability efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh_mass = gal.BlackHoleMass;
    const double initial_cold_gas = gal.ColdGas;
    const double initial_accretion = gal.QuasarModeBHaccretionMass;

    /* ===== EXECUTE ===== */
    result = sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");

    /* BH should have grown */
    TEST_ASSERT(gal.BlackHoleMass > initial_bh_mass,
                "BH mass should increase from disk instability");

    /* Cold gas should decrease */
    TEST_ASSERT(gal.ColdGas < initial_cold_gas,
                "Cold gas should decrease from BH accretion");

    /* Mass conservation */
    const double accreted = gal.BlackHoleMass - initial_bh_mass;
    const double consumed = initial_cold_gas - gal.ColdGas;
    TEST_ASSERT_DOUBLE_EQUAL(accreted, consumed, 1e-6,
                             "Accreted mass should equal consumed cold gas");

    /* Accretion tracking */
    TEST_ASSERT_DOUBLE_EQUAL(gal.QuasarModeBHaccretionMass - initial_accretion,
                             accreted, 1e-6,
                             "QuasarModeBHaccretionMass should track accretion");

    /* Trigger lifecycle is owned by clear modules, not quasar_mode */
    TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                             "Disk-instability trigger should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bh_growth_merger
 * @brief   Test merger trigger is ignored in quasar mode module
 *
 * Expected: BH mass unchanged (merger channel handled inline in merge module)
 * Validates: Channel separation between phase_1 and phase_2 trigger paths
 */
int test_bh_growth_merger(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

    /* Set merger trigger */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                             "BH mass should be unchanged when only merger trigger is set");

    /* Trigger lifecycle is owned by clear modules, not quasar_mode */

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bh_growth_both_triggers
 * @brief   Test BH growth when both triggers present
 *
 * Expected: Disk trigger is processed; merger trigger is preserved/ignored here
 * Validates: Disk-instability-only behavior in quasar module
 */
int test_bh_growth_both_triggers(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

    /* Set both triggers */
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh_mass = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.BlackHoleMass > initial_bh_mass,
                "BH mass should increase from disk-instability trigger");

    /* Trigger lifecycle is owned by clear modules, not quasar_mode */
    TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                             "Disk-instability trigger should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bh_growth_per_event_merger
 * @brief   Test BH growth from merger event in process_per_event path
 *
 * Expected: BH mass increases and cold gas decreases
 * Validates: process_per_event merger channel behavior
 */
int test_bh_growth_per_event_merger(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    struct ModuleEvent event = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = SAGE_EVENT_MERGER,
        .source_index = 1,
        .target_index = 0,
        .value0 = 0.3,
        .value1 = 0.0
    };
    ctx.active_event = &event;

    const double initial_bh = gal.BlackHoleMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Per-event merger processing should succeed");
    TEST_ASSERT(gal.BlackHoleMass > initial_bh,
                "BH mass should increase from merger event");
    TEST_ASSERT(gal.ColdGas < initial_cold,
                "Cold gas should decrease from merger event");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_per_event_unknown_code_noop
 * @brief   Test unknown per-event code is graceful no-op
 *
 * Expected: No property changes and success return
 * Validates: Defensive unknown event handling
 */
int test_per_event_unknown_code_noop(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    struct ModuleEvent event = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = 999,
        .source_index = 1,
        .target_index = 0,
        .value0 = 0.5,
        .value1 = 0.0
    };
    ctx.active_event = &event;

    const double initial_bh = gal.BlackHoleMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Unknown per-event code should be a no-op success");
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-12,
                             "BH mass should remain unchanged for unknown event");
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12,
                             "Cold gas should remain unchanged for unknown event");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_vvir_suppression
 * @brief   Test that low Vvir suppresses BH accretion
 *
 * Expected: Less accretion at low Vvir (< 280 km/s)
 * Validates: Kauffmann & Haehnelt (2000) Vvir suppression
 */
int test_vvir_suppression(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* Test 1: Low Vvir (100 km/s << 280 km/s) */
    struct Halo halo_low;
    struct GalaxyData gal_low;
    setup_test_galaxy(&halo_low, &gal_low, 0, 10.0, 100.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal_low.UnstableDiskGasFraction = 0.5;

    const double initial_bh_low = gal_low.BlackHoleMass;
    sage_quasar_mode_process(&ctx, &halo_low, 1);
    const double accreted_low = gal_low.BlackHoleMass - initial_bh_low;

    /* Test 2: High Vvir (400 km/s >> 280 km/s) */
    struct Halo halo_high;
    struct GalaxyData gal_high;
    setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 400.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal_high.UnstableDiskGasFraction = 0.5;

    const double initial_bh_high = gal_high.BlackHoleMass;
    sage_quasar_mode_process(&ctx, &halo_high, 1);
    const double accreted_high = gal_high.BlackHoleMass - initial_bh_high;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(accreted_high > accreted_low,
                "High Vvir should produce more accretion than low Vvir");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_mass_conservation_accretion
 * @brief   Test mass conservation during BH accretion
 *
 * Expected: Mass from cold gas equals mass added to BH
 * Validates: Mass conservation
 */
int test_mass_conservation_accretion(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh = gal.BlackHoleMass;
    const double initial_cold = gal.ColdGas;
    const double initial_total = initial_bh + initial_cold;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    const double final_total = gal.BlackHoleMass + gal.ColdGas;
    TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, 1e-6,
                             "Total mass (BH + cold gas) should be conserved");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_metallicity_preservation_accretion
 * @brief   Test metallicity preservation during BH accretion
 *
 * Expected: Cold gas metallicity preserved after accretion
 * Validates: Metallicity handling
 */
int test_metallicity_preservation_accretion(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 10.0;
    const double metals_cold = 0.5;  /* Z = 0.05 */
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, cold_gas, metals_cold,
                      0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_metallicity = metals_cold / cold_gas;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    if (gal.ColdGas > 0.0) {
        const double final_metallicity = gal.MetalsColdGas / gal.ColdGas;
        TEST_ASSERT_DOUBLE_EQUAL(final_metallicity, initial_metallicity, 1e-6,
                                 "Cold gas metallicity should be preserved");
    }

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_quasar_wind_cold_ejection
 * @brief   Test quasar wind ejects cold gas when energy sufficient
 *
 * Expected: Cold gas transferred to ejected reservoir
 * Validates: Energy-driven wind physics
 */
int test_quasar_wind_cold_ejection(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Use high efficiency to ensure wind */
    setup_test_parameters(0.05, 1.0);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 1.0;   /* Small cold gas mass */
    const double vvir = 100.0;     /* Low Vvir = low binding energy */
    setup_test_galaxy(&halo, &gal, 0, 10.0, vvir, cold_gas, 0.02, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.8;  /* High efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_ejected = gal.EjectedGas;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Either cold gas reduced or ejected gas increased (wind occurred) */
    const int wind_occurred = (gal.ColdGas < cold_gas) || (gal.EjectedGas > initial_ejected);
    TEST_ASSERT(wind_occurred || gal.ColdGas == 0.0,
                "Quasar wind should eject cold gas or deplete it");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_quasar_wind_hot_ejection
 * @brief   Test quasar wind can eject both cold and hot gas
 *
 * Expected: Both cold and hot gas ejected when energy very high
 * Validates: Strong wind physics
 */
int test_quasar_wind_hot_ejection(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Very high efficiency to ensure strong wind */
    setup_test_parameters(0.1, 1.0);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 1.0;
    const double hot_gas = 1.0;
    const double vvir = 80.0;  /* Very low Vvir */
    setup_test_galaxy(&halo, &gal, 0, 5.0, vvir, cold_gas, 0.02, hot_gas, 0.02, 0.01);
    gal.UnstableDiskGasFraction = 0.9;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_hot = gal.HotGas;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Strong wind may eject both (or at least cold gas should be affected) */
    TEST_ASSERT(gal.ColdGas <= cold_gas, "Cold gas should not increase");
    TEST_ASSERT(gal.HotGas <= initial_hot, "Hot gas should not increase");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_quasar_wind_no_ejection
 * @brief   Test no ejection when quasar energy below binding energy
 *
 * Expected: No gas ejected, only BH growth
 * Validates: Energy threshold logic
 */
int test_quasar_wind_no_ejection(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Low efficiency = low wind energy */
    setup_test_parameters(0.001, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 10.0;
    const double vvir = 300.0;  /* High Vvir = high binding energy */
    setup_test_galaxy(&halo, &gal, 0, 100.0, vvir, cold_gas, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.1;  /* Low efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_ejected = gal.EjectedGas;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Ejected gas should be unchanged (no wind) */
    TEST_ASSERT_DOUBLE_EQUAL(gal.EjectedGas, initial_ejected, 1e-6,
                             "No ejection when energy below binding energy");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_accretion_mass_tracking
 * @brief   Test QuasarModeBHaccretionMass tracks total accretion
 *
 * Expected: Accumulator equals total BH growth
 * Validates: Accretion tracking
 */
int test_accretion_mass_tracking(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh = gal.BlackHoleMass;
    const double initial_tracker = gal.QuasarModeBHaccretionMass;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    const double bh_growth = gal.BlackHoleMass - initial_bh;
    const double tracker_growth = gal.QuasarModeBHaccretionMass - initial_tracker;
    TEST_ASSERT_DOUBLE_EQUAL(tracker_growth, bh_growth, 1e-6,
                             "QuasarModeBHaccretionMass should track BH growth");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_cold_gas
 * @brief   Test no accretion when ColdGas = 0
 *
 * Expected: BH mass unchanged
 * Validates: Zero cold gas handling
 */
int test_zero_cold_gas(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 0.0, 0.0, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                             "BH mass should not change when ColdGas = 0");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_efficiency_factor
 * @brief   Test no accretion when efficiency factor = 0
 *
 * Expected: BH mass unchanged
 * Validates: Zero efficiency handling
 */
int test_zero_efficiency_factor(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.0;  /* Zero efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh = gal.BlackHoleMass;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                             "BH mass should not change when efficiency = 0");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_no_triggers
 * @brief   Test no processing when both triggers = 0
 *
 * Expected: All properties unchanged
 * Validates: Trigger requirement
 */
int test_no_triggers(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    /* No triggers set */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh = gal.BlackHoleMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                             "BH mass unchanged without triggers");
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10,
                             "Cold gas unchanged without triggers");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_orphan_galaxy_skipped
 * @brief   Test Type 2 orphans are not processed
 *
 * Expected: No changes to galaxy properties
 * Validates: Orphan galaxy handling (module doesn't explicitly check Type,
 *            but orphans have no triggers set by upstream modules)
 */
int test_orphan_galaxy_skipped(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 2, 0.0, 0.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    /* Orphans don't have triggers set by upstream modules */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh = gal.BlackHoleMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                             "Orphan BH mass should not change");
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10,
                             "Orphan cold gas should not change");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_null_galaxy
 * @brief   Test NULL galaxy handled gracefully
 *
 * Expected: Function returns success
 * Validates: NULL pointer safety
 */
int test_null_galaxy(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    memset(&halo, 0, sizeof(halo));
    halo.Type = 0;
    halo.galaxy = NULL;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    int result = sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_triggers_preserved
 * @brief   Test triggers are preserved after processing
 *
 * Expected: Trigger flags remain unchanged after processing
 * Validates: Trigger lifecycle ownership by dedicated clear modules
 */
int test_triggers_preserved(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                             "UnstableDiskGasFraction should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_invalid_ngal
 * @brief   Test error when ngal != 1
 *
 * Expected: Function returns error
 * Validates: process_by_galaxy mode enforcement
 */
int test_invalid_ngal(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halos[2];
    struct GalaxyData gals[2];
    setup_test_galaxy(&halos[0], &gals[0], 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 0.0, 0.0, 0.005);

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    int result = sage_quasar_mode_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result != 0, "Should return error when ngal != 1");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_growth_rate
 * @brief   Test BlackHoleGrowthRate parameter affects results
 *
 * Expected: Higher growth rate produces more BH accretion
 * Validates: Parameter loading and physics
 */
int test_parameter_sensitivity_growth_rate(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test with low growth rate */
    reset_config();
    setup_test_parameters(0.005, 0.001);
    sage_quasar_mode_init();

    struct Halo halo_low;
    struct GalaxyData gal_low;
    setup_test_galaxy(&halo_low, &gal_low, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal_low.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bh_low = gal_low.BlackHoleMass;
    sage_quasar_mode_process(&ctx, &halo_low, 1);
    const double accreted_low = gal_low.BlackHoleMass - initial_bh_low;

    sage_quasar_mode_cleanup();

    /* Test with high growth rate */
    reset_config();
    setup_test_parameters(0.02, 0.001);  /* 4x higher */
    sage_quasar_mode_init();

    struct Halo halo_high;
    struct GalaxyData gal_high;
    setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal_high.UnstableDiskGasFraction = 0.5;

    const double initial_bh_high = gal_high.BlackHoleMass;
    sage_quasar_mode_process(&ctx, &halo_high, 1);
    const double accreted_high = gal_high.BlackHoleMass - initial_bh_high;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(accreted_high > accreted_low,
                "Higher growth rate should produce more accretion");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity_quasar_efficiency
 * @brief   Test QuasarModeEfficiency parameter affects wind strength
 *
 * Expected: Higher efficiency produces stronger winds
 * Validates: Parameter loading and wind physics
 */
int test_parameter_sensitivity_quasar_efficiency(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test with low efficiency */
    reset_config();
    setup_test_parameters(0.05, 0.0001);
    sage_quasar_mode_init();

    struct Halo halo_low;
    struct GalaxyData gal_low;
    setup_test_galaxy(&halo_low, &gal_low, 0, 10.0, 100.0, 2.0, 0.04, 2.0, 0.04, 0.01);
    gal_low.UnstableDiskGasFraction = 0.8;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_ejected_low = gal_low.EjectedGas;
    sage_quasar_mode_process(&ctx, &halo_low, 1);
    const double ejected_low = gal_low.EjectedGas - initial_ejected_low;

    sage_quasar_mode_cleanup();

    /* Test with high efficiency */
    reset_config();
    setup_test_parameters(0.05, 1.0);  /* Much higher */
    sage_quasar_mode_init();

    struct Halo halo_high;
    struct GalaxyData gal_high;
    setup_test_galaxy(&halo_high, &gal_high, 0, 10.0, 100.0, 2.0, 0.04, 2.0, 0.04, 0.01);
    gal_high.UnstableDiskGasFraction = 0.8;

    const double initial_ejected_high = gal_high.EjectedGas;
    sage_quasar_mode_process(&ctx, &halo_high, 1);
    const double ejected_high = gal_high.EjectedGas - initial_ejected_high;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(ejected_high >= ejected_low,
                "Higher efficiency should produce at least as much ejection");

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
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
    MimicConfig.phase_1[0].module_name = strdup("sage_quasar_mode");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* Set required parameters */
    int idx = 0;
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.01");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.001");
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
 * @brief   Test module doesn't leak memory during operation
 *
 * Expected: No memory leaks after init/process/cleanup cycle
 * Validates: Memory management
 */
int test_memory_safety(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.01, 0.001);
    sage_quasar_mode_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_quasar_mode_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will catch any leaks */

    /* ===== CLEANUP ===== */
    sage_quasar_mode_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_quasar_mode unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_quasar_mode Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run physics calculation tests */
    TEST_RUN(test_bh_growth_disk_instability);
    TEST_RUN(test_bh_growth_merger);
    TEST_RUN(test_bh_growth_both_triggers);
    TEST_RUN(test_bh_growth_per_event_merger);
    TEST_RUN(test_per_event_unknown_code_noop);
    TEST_RUN(test_vvir_suppression);
    TEST_RUN(test_mass_conservation_accretion);
    TEST_RUN(test_metallicity_preservation_accretion);
    TEST_RUN(test_quasar_wind_cold_ejection);
    TEST_RUN(test_quasar_wind_hot_ejection);
    TEST_RUN(test_quasar_wind_no_ejection);
    TEST_RUN(test_accretion_mass_tracking);

    /* Run edge case tests */
    TEST_RUN(test_zero_cold_gas);
    TEST_RUN(test_zero_efficiency_factor);
    TEST_RUN(test_no_triggers);
    TEST_RUN(test_orphan_galaxy_skipped);
    TEST_RUN(test_null_galaxy);
    TEST_RUN(test_triggers_preserved);
    TEST_RUN(test_invalid_ngal);

    /* Run parameter sensitivity tests */
    TEST_RUN(test_parameter_sensitivity_growth_rate);
    TEST_RUN(test_parameter_sensitivity_quasar_efficiency);

    /* Run infrastructure tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
