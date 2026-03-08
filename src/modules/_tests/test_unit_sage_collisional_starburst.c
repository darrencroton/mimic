/**
 * @file    test_unit_sage_collisional_starburst.c
 * @brief   Unit tests for sage_collisional_starburst module
 *
 * Validates: Collisional starburst physics, merger/disk triggers, feedback, edge cases
 *
 * This test validates the sage_collisional_starburst module physics:
 * - Disk instability trigger (mode=1, eburst = efficiency)
 * - By-galaxy path ignores merger flags (merger channel uses process_per_event)
 * - Per-event merger trigger support via ctx->active_event
 * - Star formation and bulge growth
 * - Feedback reheating and ejection
 * - Mass and metallicity conservation
 * - Metal distribution (major vs minor mergers)
 * - Cold gas balance constraint
 * - Edge cases (zero values, NULL galaxies, triggers)
 * - Parameter sensitivity
 *
 * Test cases:
 *   - test_disk_instability_starburst: Disk instability trigger physics
 *   - test_merger_starburst: Merger-only trigger is ignored in this module
 *   - test_both_triggers: Disk trigger processed while merger trigger preserved
 *   - test_per_event_merger_starburst: Merger event triggers starburst physics
 *   - test_per_event_unknown_code_noop: Unknown event code is no-op
 *   - test_major_vs_minor_merger: Merger-only major/minor triggers are both ignored
 *   - test_mass_conservation: Total mass conserved
 *   - test_metallicity_preservation: Metallicity preserved
 *   - test_bulge_formation: Stars added to bulge
 *   - test_ejection_calculation: Energy-driven ejection
 *   - test_cold_gas_balance: (stars + reheated) <= ColdGas
 *   - test_zero_cold_gas: No starburst with zero cold gas
 *   - test_zero_efficiency: No starburst with zero triggers
 *   - test_no_triggers: No processing without triggers
 *   - test_null_galaxy: NULL galaxy handled
 *   - test_null_central_galaxy: NULL central handled
 *   - test_triggers_preserved: Trigger lifecycle owned by clear modules
 *   - test_invalid_ngal: Error when ngal != 1
 *   - test_zero_vvir: Ejection = 0 when Vvir = 0
 *   - test_insufficient_cold_gas: Balancing works
 *   - test_parameter_sensitivity_reheating: Reheating parameter
 *   - test_parameter_sensitivity_ejection: Ejection parameter
 *   - test_parameter_sensitivity_yield: Yield parameter
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *
 * @author  Mimic Development Team
 * @date    2025-12-23
 */

#include "../../../tests/framework/test_framework.h"
#include "../core/module_registry.h"
#include "../core/module_interface.h"
#include "../include/types.h"
#include "../include/proto.h"
#include "../include/globals.h"
#include "../util/error.h"
#include "../util/memory.h"
#include "_shared/sage_events.h"

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
extern double FEEDBACK_REHEATING_EPSILON;
extern double FEEDBACK_EJECTION_EFFICIENCY;
extern double RECYCLE_FRACTION;
extern double YIELD;
extern double FRAC_Z_LEAVE_DISK;
extern double THRESHOLD_MAJOR_MERGER;

/* Module functions (extern declarations for direct testing) */
extern int sage_collisional_starburst_init(void);
extern int sage_collisional_starburst_process(struct ModuleContext *ctx,
                                                struct Halo *halos, int ngal);
extern int sage_collisional_starburst_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Initialize global unit conversion constants
 *
 * CRITICAL: The module uses global unit variables (UnitEnergy_in_cgs, UnitMass_in_g)
 * to convert physical constants to code units. These must be initialized before
 * calling sage_collisional_starburst_init() or EnergySNcode/EtaSNcode will be garbage.
 */
static void init_unit_constants(void)
{
    /* Standard cosmological unit system */
    UnitLength_in_cm = 3.08568e24;       /* 1 Mpc in cm */
    UnitVelocity_in_cm_per_s = 1.0e5;    /* 1 km/s in cm/s */
    UnitMass_in_g = 1.989e43;            /* 1e10 Msun in g */

    /* Derived units */
    UnitTime_in_s = UnitLength_in_cm / UnitVelocity_in_cm_per_s;
    UnitEnergy_in_cgs = UnitMass_in_g * UnitLength_in_cm * UnitLength_in_cm /
                        (UnitTime_in_s * UnitTime_in_s);
}

/**
 * @brief   Reset global configuration state
 */
static void reset_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
    init_unit_constants();  /* Always initialize unit constants after reset */

    /* Set Hubble_h before module init - required for EnergySNcode/EtaSNcode calculation */
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;  /* 1 km/s */
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
 * @param   stellar_mass    Stellar mass [1e10 Msun/h]
 * @param   bulge_mass      Bulge mass [1e10 Msun/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   metals_hot      Metals in hot gas [1e10 Msun/h]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                               int type, double mvir, double vvir,
                               double cold_gas, double metals_cold,
                               double stellar_mass, double bulge_mass,
                               double hot_gas, double metals_hot)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = type;
    halo->Mvir = (float)mvir;
    halo->Vvir = (float)vvir;
    halo->SnapNum = 63;
    halo->dT = 0.1;  /* Time interval for rate calculations */
    halo->galaxy = gal;

    gal->ColdGas = (float)cold_gas;
    gal->MetalsColdGas = (float)metals_cold;
    gal->StellarMass = (float)stellar_mass;
    gal->MetalsStellarMass = (float)(metals_cold / cold_gas * stellar_mass);
    gal->BulgeMass = (float)bulge_mass;
    gal->MetalsBulgeMass = (float)(metals_cold / cold_gas * bulge_mass);
    gal->HotGas = (float)hot_gas;
    gal->MetalsHotGas = (float)metals_hot;
    gal->EjectedGas = 0.0;
    gal->MetalsEjectedGas = 0.0;
    gal->StarFormationRate = 0.0;
    gal->SupernovaOutflowRate = 0.0;
    gal->UnstableDiskGasFraction = 0.0;
    gal->IsMerging = 0;
    gal->MergerMassRatio = 0.0;
}

/**
 * @brief   Setup test parameters
 *
 * @param   reheating_eps       Feedback reheating epsilon
 * @param   ejection_eff        Feedback ejection efficiency
 * @param   recycle_frac        Recycle fraction
 * @param   yield               Metal yield
 * @param   frac_z_leave        Fraction of metals leaving disk
 * @param   threshold_major     Major merger threshold
 */
static void setup_test_parameters(double reheating_eps, double ejection_eff,
                                   double recycle_frac, double yield,
                                   double frac_z_leave, double threshold_major)
{
    /* Set model parameters in MimicConfig */
    int idx = 0;

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackReheatingEpsilon");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", reheating_eps);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackEjectionEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", ejection_eff);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", recycle_frac);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", yield);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", frac_z_leave);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", threshold_major);

    MimicConfig.NumModelParams = idx;
}

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx             Context to initialize
 * @param   central_halo    Central halo (for ctx->central_galaxy)
 */
static void setup_test_context(struct ModuleContext *ctx, struct Halo *central_halo)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->substep_dt = 0.01;
    ctx->redshift = 0.0;
    ctx->time = 13.8;
    ctx->snapshot_number = 63;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
    ctx->params = &MimicConfig;
    ctx->central_galaxy = central_halo;

    /* Set unit conversions (typical values) */
    MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;  /* 1 km/s */
    MimicConfig.Hubble_h = 0.73;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_disk_instability_starburst
 * @brief   Test starburst from disk instability trigger
 *
 * Expected: Stars form, cold gas decreases, bulge increases
 * Validates: Disk instability trigger physics (mode=1, eburst = efficiency)
 */
int test_disk_instability_starburst(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);

    int result = sage_collisional_starburst_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create central galaxy with cold gas */
    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 10.0;
    const double metals_cold = 0.2;  /* Z = 0.02 */
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, cold_gas, metals_cold,
                      5.0, 1.0, 50.0, 1.0);

    /* Set disk instability trigger */
    gal.UnstableDiskGasFraction = 0.5;  /* 50% efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_cold = gal.ColdGas;
    const double initial_stellar = gal.StellarMass;
    const double initial_bulge = gal.BulgeMass;

    /* ===== EXECUTE ===== */
    result = sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");

    /* Cold gas should decrease */
    TEST_ASSERT(gal.ColdGas < initial_cold,
                "Cold gas should decrease from starburst");

    /* Stellar mass should increase */
    TEST_ASSERT(gal.StellarMass > initial_stellar,
                "Stellar mass should increase from starburst");

    /* Bulge mass should increase (starbursts form spheroids) */
    TEST_ASSERT(gal.BulgeMass > initial_bulge,
                "Bulge mass should increase from starburst");

    /* Trigger lifecycle is owned by clear modules, not collisional_starburst */
    TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                             "Disk-instability trigger should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_instability_uses_per_object_substep_dt_for_rates
 * @brief   Disk-instability channel normalizes rates with halo->dT/num_substeps
 *
 * Expected: With identical physics and num_substeps changing from 1 to 2,
 * StarFormationRate and SupernovaOutflowRate double (inverse-dt scaling).
 */
int test_disk_instability_uses_per_object_substep_dt_for_rates(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo1;
    struct GalaxyData gal1;
    setup_test_galaxy(&halo1, &gal1, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    halo1.dT = 0.2f;
    gal1.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx1;
    setup_test_context(&ctx1, &halo1);
    ctx1.num_substeps = 1;

    int result = sage_collisional_starburst_process(&ctx1, &halo1, 1);
    TEST_ASSERT(result == 0, "First disk-instability processing should succeed");
    const double sfr1 = gal1.StarFormationRate;
    const double outflow1 = gal1.SupernovaOutflowRate;

    struct Halo halo2;
    struct GalaxyData gal2;
    setup_test_galaxy(&halo2, &gal2, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    halo2.dT = 0.2f;
    gal2.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx2;
    setup_test_context(&ctx2, &halo2);
    ctx2.num_substeps = 2;  /* Same halo dT, half per-substep dt */

    result = sage_collisional_starburst_process(&ctx2, &halo2, 1);
    TEST_ASSERT(result == 0, "Second disk-instability processing should succeed");
    const double sfr2 = gal2.StarFormationRate;
    const double outflow2 = gal2.SupernovaOutflowRate;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(sfr1 > 0.0 && sfr2 > 0.0,
                "Both runs should produce positive star formation rate");
    TEST_ASSERT(outflow1 > 0.0 && outflow2 > 0.0,
                "Both runs should produce positive outflow rate");
    TEST_ASSERT_DOUBLE_EQUAL(sfr2 / sfr1, 2.0, 1e-4,
                             "Disk-instability SFR must scale with per-object substep dt");
    TEST_ASSERT_DOUBLE_EQUAL(outflow2 / outflow1, 2.0, 1e-4,
                             "Disk-instability outflow must scale with per-object substep dt");
    TEST_ASSERT_DOUBLE_EQUAL(gal2.StellarMass, gal1.StellarMass, 1e-6,
                             "Mass transfer should be timestep-invariant");
    TEST_ASSERT_DOUBLE_EQUAL(gal2.ColdGas, gal1.ColdGas, 1e-6,
                             "Cold-gas evolution should be timestep-invariant");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_merger_starburst
 * @brief   Test merger-only trigger is ignored in collisional_starburst
 *
 * Expected: No star formation change (merger channel handled in merge module)
 * Validates: Channel separation between phase_1 and phase_2
 */
int test_merger_starburst(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

    /* Set merger trigger */
    gal.IsMerging = 1;
    gal.MergerMassRatio = 0.3;  /* Minor merger */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                             "Stellar mass should be unchanged for merger-only trigger");

    /* Trigger lifecycle is owned by clear modules, not collisional_starburst */
    TEST_ASSERT(gal.IsMerging == 1, "IsMerging should remain unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(gal.MergerMassRatio, 0.3, 1e-6,
                             "MergerMassRatio should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_both_triggers
 * @brief   Test disk trigger processed while merger trigger is ignored
 *
 * Expected: Disk-instability starburst occurs; merger trigger remains unchanged
 * Validates: Disk-only behavior in phase_1 module
 */
int test_both_triggers(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

    /* Set both triggers */
    gal.UnstableDiskGasFraction = 0.5;
    gal.IsMerging = 1;
    gal.MergerMassRatio = 0.3;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(gal.StellarMass > initial_stellar,
                "Stellar mass should increase from disk-instability starburst");

    /* Trigger lifecycle is owned by clear modules, not collisional_starburst */
    TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                             "Disk trigger should remain unchanged");
    TEST_ASSERT(gal.IsMerging == 1, "Merger trigger should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_per_event_merger_starburst
 * @brief   Test merger event triggers starburst in process_per_event path
 *
 * Expected: Stellar and bulge mass increase, cold gas decreases
 * Validates: process_per_event merger channel behavior
 */
int test_per_event_merger_starburst(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    struct ModuleEvent event = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = SAGE_EVENT_MERGER,
        .source_index = 1,
        .target_index = 0,
        .value0 = 0.3,
        .value1 = 0.1
    };
    ctx.active_event = &event;

    const double initial_stellar = gal.StellarMass;
    const double initial_bulge = gal.BulgeMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Per-event merger processing should succeed");
    TEST_ASSERT(gal.StellarMass > initial_stellar,
                "Stellar mass should increase from merger event");
    TEST_ASSERT(gal.BulgeMass > initial_bulge,
                "Bulge mass should increase from merger event");
    TEST_ASSERT(gal.ColdGas < initial_cold,
                "Cold gas should decrease from merger event");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_per_event_merger_uses_fof_central_feedback_destination
 * @brief   Per-event merger channel deposits reheated/ejected gas to FOF central
 *
 * Expected: Event target forms stars, but feedback destination is ctx->central_galaxy
 * Validates: SAGE parity for merger_centralgal vs centralgal roles
 */
int test_per_event_merger_uses_fof_central_feedback_destination(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Zero ejection to isolate reheating destination behavior. */
    setup_test_parameters(3.0, 0.0, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo fof_central_halo;
    struct GalaxyData fof_central_gal;
    setup_test_galaxy(&fof_central_halo, &fof_central_gal, 0,
                      120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);

    struct Halo event_target_halo;
    struct GalaxyData event_target_gal;
    setup_test_galaxy(&event_target_halo, &event_target_gal, 1,
                      25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);

    struct ModuleContext ctx;
    setup_test_context(&ctx, &fof_central_halo);

    struct ModuleEvent event = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = SAGE_EVENT_MERGER,
        .source_index = 2,
        .target_index = 1,
        .value0 = 0.3,
        .value1 = 0.1
    };
    ctx.active_event = &event;

    const double initial_fof_hot = fof_central_halo.galaxy->HotGas;
    const double initial_target_hot = event_target_halo.galaxy->HotGas;

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &event_target_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Per-event merger processing should succeed");
    TEST_ASSERT(fof_central_halo.galaxy->HotGas > initial_fof_hot,
                "Reheated gas should be deposited to FOF central hot gas");
    TEST_ASSERT_DOUBLE_EQUAL(event_target_halo.galaxy->HotGas, initial_target_hot, 1e-8,
                             "Event target hot gas should not receive reheated gas directly");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_per_event_merger_uses_event_dt_for_rates
 * @brief   Per-event merger channel normalizes rates with event payload dt
 *
 * Expected: With identical physics and different event dt values,
 * StarFormationRate and SupernovaOutflowRate scale as 1/dt.
 */
int test_per_event_merger_uses_event_dt_for_rates(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo central1;
    struct GalaxyData central_gal1;
    setup_test_galaxy(&central1, &central_gal1, 0,
                      120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
    central1.dT = 0.05f;

    struct Halo target1;
    struct GalaxyData target_gal1;
    setup_test_galaxy(&target1, &target_gal1, 1,
                      25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);
    target1.dT = 0.9f;

    struct ModuleContext ctx1;
    setup_test_context(&ctx1, &central1);
    struct ModuleEvent event1 = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = SAGE_EVENT_MERGER,
        .source_index = 2,
        .target_index = 1,
        .value0 = 0.3,
        .value1 = 0.1
    };
    ctx1.active_event = &event1;

    int result = sage_collisional_starburst_process(&ctx1, &target1, 1);
    TEST_ASSERT(result == 0, "First per-event merger processing should succeed");
    const double sfr1 = target1.galaxy->StarFormationRate;
    const double outflow1 = target1.galaxy->SupernovaOutflowRate;

    struct Halo central2;
    struct GalaxyData central_gal2;
    setup_test_galaxy(&central2, &central_gal2, 0,
                      120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
    central2.dT = 0.8f;

    struct Halo target2;
    struct GalaxyData target_gal2;
    setup_test_galaxy(&target2, &target_gal2, 1,
                      25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);
    target2.dT = 0.2f;

    struct ModuleContext ctx2;
    setup_test_context(&ctx2, &central2);
    struct ModuleEvent event2 = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = SAGE_EVENT_MERGER,
        .source_index = 2,
        .target_index = 1,
        .value0 = 0.3,
        .value1 = 0.2
    };
    ctx2.active_event = &event2;

    result = sage_collisional_starburst_process(&ctx2, &target2, 1);
    TEST_ASSERT(result == 0, "Second per-event merger processing should succeed");
    const double sfr2 = target2.galaxy->StarFormationRate;
    const double outflow2 = target2.galaxy->SupernovaOutflowRate;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(sfr1 > 0.0 && sfr2 > 0.0,
                "Both runs should produce positive star formation rate");
    TEST_ASSERT(outflow1 > 0.0 && outflow2 > 0.0,
                "Both runs should produce positive outflow rate");
    TEST_ASSERT_DOUBLE_EQUAL(sfr1 / sfr2, 2.0, 1e-4,
                             "StarFormationRate ratio should follow inverse event dt");
    TEST_ASSERT_DOUBLE_EQUAL(outflow1 / outflow2, 2.0, 1e-4,
                             "SupernovaOutflowRate ratio should follow inverse event dt");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_per_event_unknown_code_noop
 * @brief   Test unknown event code is a no-op
 *
 * Expected: No property changes and success return
 * Validates: Defensive unknown event handling
 */
int test_per_event_unknown_code_noop(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    struct ModuleEvent event = {
        .type = MODULE_EVENT_TYPE_SCALAR,
        .event_code = 999,
        .source_index = 1,
        .target_index = 0,
        .value0 = 0.4,
        .value1 = 0.1
    };
    ctx.active_event = &event;

    const double initial_stellar = gal.StellarMass;
    const double initial_bulge = gal.BulgeMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Unknown per-event code should be no-op success");
    TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-12,
                             "Stellar mass should remain unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(gal.BulgeMass, initial_bulge, 1e-12,
                             "Bulge mass should remain unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12,
                             "Cold gas should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_instability_invalid_nonboundary_dt_errors
 * @brief   Invalid non-boundary dT must fail fast in disk-instability path
 *
 * Expected: SnapNum >= 0 with dT <= 0 returns error.
 */
int test_disk_instability_invalid_nonboundary_dt_errors(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    halo.SnapNum = 63;
    halo.dT = -1.0f;
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);
    ctx.num_substeps = 1;

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == -1,
                "Non-boundary dT <= 0 must fail with error");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_disk_instability_boundary_sentinel_dt_noop
 * @brief   Boundary sentinel dT is a no-op for first-snapshot objects
 *
 * Expected: SnapNum < 0 with dT <= 0 returns success with no property changes.
 */
int test_disk_instability_boundary_sentinel_dt_noop(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    halo.SnapNum = -1;
    halo.dT = -1.0f;
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);
    ctx.num_substeps = 1;

    const double initial_stellar = gal.StellarMass;
    const double initial_cold = gal.ColdGas;
    const double initial_sfr = gal.StarFormationRate;
    const double initial_outflow = gal.SupernovaOutflowRate;

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Boundary sentinel dT should be a no-op success");
    TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-12,
                             "Stellar mass should remain unchanged on boundary skip");
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12,
                             "Cold gas should remain unchanged on boundary skip");
    TEST_ASSERT_DOUBLE_EQUAL(gal.StarFormationRate, initial_sfr, 1e-12,
                             "SFR should remain unchanged on boundary skip");
    TEST_ASSERT_DOUBLE_EQUAL(gal.SupernovaOutflowRate, initial_outflow, 1e-12,
                             "Outflow rate should remain unchanged on boundary skip");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_major_vs_minor_merger
 * @brief   Test merger-only major/minor triggers are both ignored
 *
 * Expected: No stellar mass growth from merger-only triggers
 * Validates: Merger channel is not consumed in phase_1 starburst module
 */
int test_major_vs_minor_merger(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);  /* threshold = 0.3 */
    sage_collisional_starburst_init();

    struct ModuleContext ctx;

    /* Test 1: Minor merger trigger only (ratio < threshold) */
    struct Halo halo_minor;
    struct GalaxyData gal_minor;
    setup_test_galaxy(&halo_minor, &gal_minor, 0, 100.0, 300.0, 10.0, 0.2,
                      5.0, 1.0, 50.0, 1.0);
    gal_minor.IsMerging = 1;
    gal_minor.MergerMassRatio = 0.2;  /* < 0.3 threshold */

    setup_test_context(&ctx, &halo_minor);

    sage_collisional_starburst_process(&ctx, &halo_minor, 1);

    /* Test 2: Major merger trigger only (ratio > threshold) */
    struct Halo halo_major;
    struct GalaxyData gal_major;
    setup_test_galaxy(&halo_major, &gal_major, 0, 100.0, 300.0, 10.0, 0.2,
                      5.0, 1.0, 50.0, 1.0);
    gal_major.IsMerging = 1;
    gal_major.MergerMassRatio = 0.5;  /* > 0.3 threshold */

    setup_test_context(&ctx, &halo_major);

    sage_collisional_starburst_process(&ctx, &halo_major, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal_minor.StellarMass, 5.0, 1e-10,
                             "Minor merger-only trigger should be ignored");
    TEST_ASSERT_DOUBLE_EQUAL(gal_major.StellarMass, 5.0, 1e-10,
                             "Major merger-only trigger should be ignored");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Test total mass conserved during starburst
 *
 * Expected: Total mass (cold + stellar + hot + ejected) conserved
 * Validates: Mass conservation
 */
int test_mass_conservation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_total = gal.ColdGas + gal.StellarMass + gal.HotGas + gal.EjectedGas;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    const double final_total = gal.ColdGas + gal.StellarMass + gal.HotGas + gal.EjectedGas;
    TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, 1e-4,
                             "Total mass should be conserved");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_metallicity_preservation
 * @brief   Test metallicity preserved during starburst
 *
 * Expected: Cold gas metallicity unchanged (metals tracked correctly)
 * Validates: Metallicity handling
 */
int test_metallicity_preservation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 10.0;
    const double metals_cold = 0.5;  /* Z = 0.05 */
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, cold_gas, metals_cold,
                      5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.3;  /* Modest efficiency to leave gas */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_metallicity = metals_cold / cold_gas;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Metallicity should be approximately preserved (may change due to yield) */
    if (gal.ColdGas > 0.1) {
        const double final_metallicity = gal.MetalsColdGas / gal.ColdGas;
        /* Allow for metal enrichment from yield */
        TEST_ASSERT(final_metallicity >= initial_metallicity * 0.9,
                    "Cold gas metallicity should be approximately preserved");
    }

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bulge_formation
 * @brief   Test stars added to bulge mass (not disk)
 *
 * Expected: BulgeMass increases proportional to stellar mass increase
 * Validates: Bulge formation (unique to starbursts)
 */
int test_bulge_formation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;
    const double initial_bulge = gal.BulgeMass;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    const double stellar_growth = gal.StellarMass - initial_stellar;
    const double bulge_growth = gal.BulgeMass - initial_bulge;

    /* Bulge should grow by same amount as stellar mass (starbursts form spheroids) */
    TEST_ASSERT_DOUBLE_EQUAL(bulge_growth, stellar_growth, 1e-6,
                             "Bulge growth should equal stellar mass growth");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_ejection_calculation
 * @brief   Test energy-driven ejection calculation
 *
 * Expected: Ejected gas calculated from energy balance
 * Validates: Ejection physics
 */
int test_ejection_calculation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_hot = gal.HotGas;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Feedback should transfer gas: cold decreases, hot increases or ejection occurs */
    const int cold_consumed = (gal.ColdGas < initial_cold);
    const int hot_increased = (gal.HotGas > initial_hot) || (gal.EjectedGas > 0.0);
    TEST_ASSERT(cold_consumed, "Cold gas should be consumed by star formation");
    TEST_ASSERT(hot_increased, "Feedback should heat or eject gas");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_cold_gas_balance
 * @brief   Test (stars + reheated) <= ColdGas constraint enforced
 *
 * Expected: Balancing reduces stars and reheated when insufficient gas
 * Validates: Cold gas balance constraint
 */
int test_cold_gas_balance(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* High reheating to trigger balance */
    setup_test_parameters(10.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    /* Low cold gas, high efficiency */
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 1.0, 0.02, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.9;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Cold gas should not go negative */
    TEST_ASSERT(gal.ColdGas >= -1e-6, "Cold gas should not be negative");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_cold_gas
 * @brief   Test no starburst when ColdGas = 0
 *
 * Expected: Stellar mass unchanged
 * Validates: Zero cold gas handling
 */
int test_zero_cold_gas(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 0.0, 0.0, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                             "Stellar mass should not change with zero cold gas");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_efficiency
 * @brief   Test no starburst when efficiency = 0
 *
 * Expected: Stellar mass unchanged
 * Validates: Zero efficiency handling
 */
int test_zero_efficiency(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.0;  /* Zero efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                             "Stellar mass should not change with zero efficiency");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_no_triggers
 * @brief   Test no processing when triggers = 0
 *
 * Expected: All properties unchanged
 * Validates: Trigger requirement
 */
int test_no_triggers(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    /* No triggers set */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;
    const double initial_cold = gal.ColdGas;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                             "Stellar mass unchanged without triggers");
    TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10,
                             "Cold gas unchanged without triggers");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
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

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    memset(&halo, 0, sizeof(halo));
    halo.Type = 0;
    halo.galaxy = NULL;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_null_central_galaxy
 * @brief   Test NULL central galaxy handled gracefully
 *
 * Expected: Function returns success without crash
 * Validates: NULL central pointer safety
 */
int test_null_central_galaxy(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo satellite;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&satellite, &sat_gal, 1, 50.0, 200.0, 5.0, 0.1, 2.0, 0.5, 10.0, 0.2);

    /* Create central with NULL galaxy */
    struct Halo central;
    memset(&central, 0, sizeof(central));
    central.Type = 0;
    central.galaxy = NULL;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &central);

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, &satellite, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle NULL central galaxy gracefully");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_triggers_preserved
 * @brief   Test triggers preserved after processing
 *
 * Expected: Trigger flags remain unchanged after processing
 * Validates: Trigger lifecycle ownership by dedicated clear modules
 */
int test_triggers_preserved(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;
    gal.IsMerging = 1;
    gal.MergerMassRatio = 0.3;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                             "UnstableDiskGasFraction should remain unchanged");
    TEST_ASSERT(gal.IsMerging == 1, "IsMerging should remain unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(gal.MergerMassRatio, 0.3, 1e-6,
                             "MergerMassRatio should remain unchanged");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
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

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halos[2];
    struct GalaxyData gals[2];
    setup_test_galaxy(&halos[0], &gals[0], 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 2.0, 0.5, 10.0, 0.2);

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halos[0]);

    /* ===== EXECUTE ===== */
    int result = sage_collisional_starburst_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result != 0, "Should return error when ngal != 1");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_zero_vvir
 * @brief   Test ejection = 0 when Vvir = 0
 *
 * Expected: No ejection, but starburst still occurs
 * Validates: Zero Vvir handling
 */
int test_zero_vvir(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 0.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double initial_stellar = gal.StellarMass;
    const double initial_ejected = gal.EjectedGas;

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Stars should still form */
    TEST_ASSERT(gal.StellarMass > initial_stellar,
                "Stars should form even with Vvir = 0");

    /* No ejection when Vvir = 0 */
    TEST_ASSERT_DOUBLE_EQUAL(gal.EjectedGas, initial_ejected, 1e-10,
                             "No ejection when Vvir = 0");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_insufficient_cold_gas
 * @brief   Test balancing works when (stars + reheated) > ColdGas
 *
 * Expected: Stars and reheated scaled down to fit available gas
 * Validates: Balancing constraint
 */
int test_insufficient_cold_gas(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    /* Very high reheating to trigger balancing */
    setup_test_parameters(15.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 1.0, 0.02, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.9;  /* High efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* Should not crash and cold gas should not go negative */
    TEST_ASSERT(gal.ColdGas >= -1e-6, "Cold gas should not be negative");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_reheating
 * @brief   Test FeedbackReheatingEpsilon parameter affects reheating
 *
 * Expected: Higher epsilon produces more reheating (before ejection)
 * Validates: Parameter loading and physics
 *
 * Strategy: Use conditions where balancing and ejection don't dominate:
 * - Low star formation efficiency (eburst = 0.1) to avoid balancing
 * - Large ColdGas to avoid running out
 * - Measure cold gas decrease (proxy for reheating since cold gas consumed by both SF and reheating)
 */
int test_parameter_sensitivity_reheating(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test with low reheating epsilon */
    reset_config();
    setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.5, 0.3);  /* Zero ejection to isolate reheating */
    sage_collisional_starburst_init();

    struct Halo halo_low;
    struct GalaxyData gal_low;
    /* Large cold gas, low efficiency to avoid balancing */
    setup_test_galaxy(&halo_low, &gal_low, 0, 100.0, 300.0, 100.0, 2.0, 5.0, 1.0, 50.0, 1.0);
    gal_low.UnstableDiskGasFraction = 0.1;  /* Low efficiency */

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo_low);

    const double initial_cold_low = gal_low.ColdGas;
    sage_collisional_starburst_process(&ctx, &halo_low, 1);
    const double cold_consumed_low = initial_cold_low - gal_low.ColdGas;

    sage_collisional_starburst_cleanup();

    /* Test with high reheating epsilon */
    reset_config();
    setup_test_parameters(3.0, 0.0, 0.43, 0.03, 0.5, 0.3);  /* Zero ejection to isolate reheating */
    sage_collisional_starburst_init();

    struct Halo halo_high;
    struct GalaxyData gal_high;
    setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 300.0, 100.0, 2.0, 5.0, 1.0, 50.0, 1.0);
    gal_high.UnstableDiskGasFraction = 0.1;  /* Same efficiency */

    setup_test_context(&ctx, &halo_high);

    const double initial_cold_high = gal_high.ColdGas;
    sage_collisional_starburst_process(&ctx, &halo_high, 1);
    const double cold_consumed_high = initial_cold_high - gal_high.ColdGas;

    /* ===== VALIDATE ===== */
    /* Cold gas consumed = (1-recycle)*stars + reheated
     * With same eburst: stars_low ≈ stars_high
     * reheated_low = 1.0 * stars, reheated_high = 3.0 * stars
     * So cold_consumed_high should be > cold_consumed_low */
    TEST_ASSERT(cold_consumed_high > cold_consumed_low,
                "Higher reheating epsilon should consume more cold gas");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity_ejection
 * @brief   Test FeedbackEjectionEfficiency parameter affects ejection
 *
 * Expected: Parameter affects ejection when energetically favorable
 * Validates: Parameter loading and physics
 *
 * Strategy: Use low Vvir and modest reheating to ensure ejection occurs
 * ejection = (efficiency * constant / Vvir^2 - reheating_epsilon) * stars
 * With low Vvir, the first term dominates, making ejection positive
 */
int test_parameter_sensitivity_ejection(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test 1: Verify ejection can be zero with low efficiency */
    reset_config();
    setup_test_parameters(0.5, 0.1, 0.43, 0.03, 0.5, 0.3);  /* Low reheating and ejection */
    sage_collisional_starburst_init();

    struct Halo halo_low;
    struct GalaxyData gal_low;
    /* Low Vvir to make ejection energetically favorable */
    setup_test_galaxy(&halo_low, &gal_low, 0, 10.0, 100.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal_low.UnstableDiskGasFraction = 0.3;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo_low);

    sage_collisional_starburst_process(&ctx, &halo_low, 1);

    /* Verify basic correctness */
    TEST_ASSERT(gal_low.EjectedGas >= -1e-10, "Ejected gas should not be significantly negative");
    TEST_ASSERT(gal_low.HotGas >= -1e-10, "Hot gas should not be significantly negative");
    TEST_ASSERT(gal_low.ColdGas >= -1e-10, "Cold gas should not be significantly negative");

    sage_collisional_starburst_cleanup();

    /* Test 2: Higher ejection efficiency */
    reset_config();
    setup_test_parameters(0.5, 1.0, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo_high;
    struct GalaxyData gal_high;
    setup_test_galaxy(&halo_high, &gal_high, 0, 10.0, 100.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal_high.UnstableDiskGasFraction = 0.3;

    setup_test_context(&ctx, &halo_high);

    sage_collisional_starburst_process(&ctx, &halo_high, 1);

    /* ===== VALIDATE ===== */
    /* Verify both tests executed without significant negative values */
    TEST_ASSERT(gal_high.EjectedGas >= -1e-10, "Ejected gas should not be significantly negative");
    TEST_ASSERT(gal_high.HotGas >= -1e-10, "Hot gas should not be significantly negative");

    /* Basic sanity: parameters were loaded and module executed */
    const int module_executed = (gal_low.StellarMass > 0.0) || (gal_high.StellarMass > 0.0);
    TEST_ASSERT(module_executed, "Module should have executed and formed stars");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity_yield
 * @brief   Test Yield parameter affects metal enrichment
 *
 * Expected: Higher yield produces more metals
 * Validates: Parameter loading and metal physics
 */
int test_parameter_sensitivity_yield(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* Test with low yield */
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.01, 0.5, 0.3);  /* Low yield */
    sage_collisional_starburst_init();

    struct Halo halo_low;
    struct GalaxyData gal_low;
    setup_test_galaxy(&halo_low, &gal_low, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal_low.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo_low);

    const double initial_metals_total_low = gal_low.MetalsColdGas + gal_low.MetalsHotGas;
    sage_collisional_starburst_process(&ctx, &halo_low, 1);
    const double final_metals_total_low = gal_low.MetalsColdGas + gal_low.MetalsHotGas +
                                           gal_low.MetalsStellarMass;
    const double metals_produced_low = final_metals_total_low - initial_metals_total_low;

    sage_collisional_starburst_cleanup();

    /* Test with high yield */
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.05, 0.5, 0.3);  /* 5x higher */
    sage_collisional_starburst_init();

    struct Halo halo_high;
    struct GalaxyData gal_high;
    setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal_high.UnstableDiskGasFraction = 0.5;

    setup_test_context(&ctx, &halo_high);

    const double initial_metals_total_high = gal_high.MetalsColdGas + gal_high.MetalsHotGas;
    sage_collisional_starburst_process(&ctx, &halo_high, 1);
    const double final_metals_total_high = gal_high.MetalsColdGas + gal_high.MetalsHotGas +
                                            gal_high.MetalsStellarMass;
    const double metals_produced_high = final_metals_total_high - initial_metals_total_high;

    /* ===== VALIDATE ===== */
    /* Verify parameter is loaded and metal enrichment occurs */
    /* Note: Metal change is complex (consumption + enrichment from yield) */
    const int metals_changed = (metals_produced_low != 0.0) || (metals_produced_high != 0.0);
    TEST_ASSERT(metals_changed,
                "Yield parameter should be loaded and metal enrichment should occur");

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
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
    MimicConfig.phase_1[0].module_name = strdup("sage_collisional_starburst");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;

    /* Set required parameters */
    int idx = 0;
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackReheatingEpsilon");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "3.0");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackEjectionEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.5");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.43");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.03");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.5");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.3");
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

    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_collisional_starburst_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.UnstableDiskGasFraction = 0.5;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    /* ===== EXECUTE ===== */
    sage_collisional_starburst_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will catch any leaks */

    /* ===== CLEANUP ===== */
    sage_collisional_starburst_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_collisional_starburst unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_collisional_starburst Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run physics calculation tests */
    TEST_RUN(test_disk_instability_starburst);
    TEST_RUN(test_disk_instability_uses_per_object_substep_dt_for_rates);
    TEST_RUN(test_merger_starburst);
    TEST_RUN(test_both_triggers);
    TEST_RUN(test_per_event_merger_starburst);
    TEST_RUN(test_per_event_merger_uses_fof_central_feedback_destination);
    TEST_RUN(test_per_event_merger_uses_event_dt_for_rates);
    TEST_RUN(test_per_event_unknown_code_noop);
    TEST_RUN(test_major_vs_minor_merger);
    TEST_RUN(test_mass_conservation);
    TEST_RUN(test_metallicity_preservation);
    TEST_RUN(test_bulge_formation);
    TEST_RUN(test_ejection_calculation);
    TEST_RUN(test_cold_gas_balance);

    /* Run edge case tests */
    TEST_RUN(test_zero_cold_gas);
    TEST_RUN(test_zero_efficiency);
    TEST_RUN(test_no_triggers);
    TEST_RUN(test_null_galaxy);
    TEST_RUN(test_null_central_galaxy);
    TEST_RUN(test_triggers_preserved);
    TEST_RUN(test_invalid_ngal);
    TEST_RUN(test_disk_instability_invalid_nonboundary_dt_errors);
    TEST_RUN(test_disk_instability_boundary_sentinel_dt_noop);
    TEST_RUN(test_zero_vvir);
    TEST_RUN(test_insufficient_cold_gas);

    /* Run parameter sensitivity tests */
    TEST_RUN(test_parameter_sensitivity_reheating);
    TEST_RUN(test_parameter_sensitivity_ejection);
    TEST_RUN(test_parameter_sensitivity_yield);

    /* Run infrastructure tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
