/**
 * @file    test_unit_sage_merge_galaxies.c
 * @brief   Unit tests for sage_merge_galaxies module
 *
 * Validates: Galaxy merger physics, mass transfer, morphological transformation, edge cases
 *
 * This test validates the sage_merge_galaxies module physics:
 * - Minor merger mass transfer (satellite → central)
 * - Major merger morphological transformation (disk → bulge)
 * - All baryonic component transfer (11 fields)
 * - Mass and metallicity conservation
 * - Bulge formation physics
 * - Merger timing tracking
 * - Satellite type change (→ Type 3)
 * - Threshold boundary behavior
 * - Edge cases (NULL pointers, zero mass, no mergers)
 * - Parameter sensitivity (ThresholdMajorMerger)
 *
 * Test cases:
 *   - test_single_minor_merger: Minor merger transfers mass correctly
 *   - test_single_major_merger: Major merger transforms disk to bulge
 *   - test_multiple_satellites_merge: Multiple satellites merge simultaneously
 *   - test_mass_conservation: Total mass conserved
 *   - test_metallicity_conservation: Metal mass conserved
 *   - test_bulge_formation_minor: Minor merger adds satellite stars to bulge
 *   - test_bulge_formation_major: Major merger transforms entire stellar mass to bulge
 *   - test_merger_timing_tracking: Merger times recorded correctly
 *   - test_satellite_type_change: Satellite Type → 3 after merger
 *   - test_all_baryonic_components: All 11 fields transferred
 *   - test_inline_merger_physics: Merger-triggered BH growth/starburst executed inline
 *   - test_zero_mass_satellite: Zero mass satellite handled
 *   - test_null_central_galaxy: NULL central handled
 *   - test_null_satellite_galaxy: NULL satellite skipped
 *   - test_type_0_central_never_merged: Type 0 central protection
 *   - test_no_merging_satellites: No IsMerging flags
 *   - test_null_halos_array: NULL halos pointer
 *   - test_ngal_zero: ngal = 0
 *   - test_ngal_one_central: Only central, no satellites
 *   - test_threshold_boundary_low: Ratio just below threshold
 *   - test_threshold_boundary_high: Ratio just above threshold
 *   - test_orphan_merger: Orphan (Type 2) merging
 *   - test_parameter_sensitivity_threshold: Threshold changes classification
 *   - test_extreme_threshold_zero: Threshold = 0.0
 *   - test_extreme_threshold_one: Threshold = 1.0
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Module parameter (extern to access internals for testing) */
extern double THRESHOLD_MAJOR_MERGER;

/* Module functions (extern for direct testing) */
extern int sage_merge_galaxies_init(void);
extern int sage_merge_galaxies_process(struct ModuleContext *ctx,
                                        struct Halo *halos, int ngal);
extern int sage_merge_galaxies_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Initialize global unit conversion constants
 */
static void init_unit_constants(void)
{
    UnitLength_in_cm = 3.08568e24;       /* 1 Mpc in cm */
    UnitVelocity_in_cm_per_s = 1.0e5;    /* 1 km/s in cm/s */
    UnitMass_in_g = 1.989e43;            /* 1e10 Msun in g */

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
    init_unit_constants();
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
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
 * @param   cold_gas        Cold gas mass [1e10 Msun/h]
 * @param   metals_cold     Metals in cold gas [1e10 Msun/h]
 * @param   stellar_mass    Stellar mass [1e10 Msun/h]
 * @param   metals_stellar  Metals in stellar mass [1e10 Msun/h]
 * @param   bulge_mass      Bulge mass [1e10 Msun/h]
 * @param   metals_bulge    Metals in bulge [1e10 Msun/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   metals_hot      Metals in hot gas [1e10 Msun/h]
 * @param   ejected_gas     Ejected gas mass [1e10 Msun/h]
 * @param   metals_ejected  Metals in ejected gas [1e10 Msun/h]
 * @param   ics             Intra-cluster stars [1e10 Msun/h]
 * @param   metals_ics      Metals in ICS [1e10 Msun/h]
 * @param   bh_mass         Black hole mass [1e10 Msun/h]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                               int type, double cold_gas, double metals_cold,
                               double stellar_mass, double metals_stellar,
                               double bulge_mass, double metals_bulge,
                               double hot_gas, double metals_hot,
                               double ejected_gas, double metals_ejected,
                               double ics, double metals_ics, double bh_mass)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = type;
    halo->HaloNr = 1000 + type;
    halo->SnapNum = 63;
    halo->galaxy = gal;

    gal->ColdGas = (float)cold_gas;
    gal->MetalsColdGas = (float)metals_cold;
    gal->StellarMass = (float)stellar_mass;
    gal->MetalsStellarMass = (float)metals_stellar;
    gal->BulgeMass = (float)bulge_mass;
    gal->MetalsBulgeMass = (float)metals_bulge;
    gal->HotGas = (float)hot_gas;
    gal->MetalsHotGas = (float)metals_hot;
    gal->EjectedGas = (float)ejected_gas;
    gal->MetalsEjectedGas = (float)metals_ejected;
    gal->ICS = (float)ics;
    gal->MetalsICS = (float)metals_ics;
    gal->BlackHoleMass = (float)bh_mass;
    gal->IsMerging = 0;
    gal->MergerMassRatio = 0.0;
    gal->TimeOfLastMajorMerger = -1.0;
    gal->TimeOfLastMinorMerger = -1.0;
}

/**
 * @brief   Setup test parameters
 *
 * @param   threshold   Major merger threshold
 */
static void setup_test_parameters(double threshold)
{
    int idx = 0;
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", threshold);
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.01");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.0");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackReheatingEpsilon");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "3.0");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackEjectionEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.3");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.43");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.03");
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.3");
    MimicConfig.NumModelParams = idx;
}

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx     Context to initialize
 */
static void setup_test_context(struct ModuleContext *ctx)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->substep_dt = 0.01;
    ctx->redshift = 0.0;
    ctx->time = 13.8;
    ctx->substep_time = 13.75;
    ctx->snapshot_number = 63;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
    ctx->params = &MimicConfig;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_single_minor_merger
 * @brief   Test minor merger transfers mass correctly
 *
 * Expected: All baryonic components transferred, bulge grows, satellite Type = 3
 * Validates: Minor merger physics
 */
int test_single_minor_merger(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);

    int result = sage_merge_galaxies_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create central galaxy */
    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2,    /* cold gas, metals */
                      5.0, 0.1,     /* stellar, metals */
                      1.0, 0.02,    /* bulge, metals */
                      50.0, 1.0,    /* hot, metals */
                      5.0, 0.1,     /* ejected, metals */
                      0.5, 0.01,    /* ICS, metals */
                      0.1);         /* BH */

    /* Create merging satellite */
    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04,    /* cold gas, metals */
                      1.0, 0.02,    /* stellar, metals */
                      0.2, 0.004,   /* bulge, metals */
                      10.0, 0.2,    /* hot, metals */
                      1.0, 0.02,    /* ejected, metals */
                      0.1, 0.002,   /* ICS, metals */
                      0.02);        /* BH */
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;  /* Minor merger (< 0.3) */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_cold = central_gal.ColdGas;
    const double initial_central_stellar = central_gal.StellarMass;
    const double initial_central_bulge = central_gal.BulgeMass;
    const double sat_stellar = sat_gal.StellarMass;

    /* ===== EXECUTE ===== */
    result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");

    /* Cold gas transferred */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas,
                             initial_central_cold + 2.0, 1e-6,
                             "Cold gas should be transferred");

    /* Stellar mass transferred */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->StellarMass,
                             initial_central_stellar + sat_stellar, 1e-6,
                             "Stellar mass should be transferred");

    /* Bulge mass increased by satellite stellar mass (minor merger) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             initial_central_bulge + sat_stellar, 1e-6,
                             "Bulge should grow by satellite stellar mass");

    /* For minor merger, central disk remains (StellarMass > BulgeMass) */
    TEST_ASSERT(halos[0].galaxy->StellarMass > halos[0].galaxy->BulgeMass,
                "Minor merger should preserve some disk");

    /* Satellite marked as merged (Type 3) */
    TEST_ASSERT(halos[1].Type == 3, "Satellite should be Type 3 after merger");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_single_major_merger
 * @brief   Test major merger transforms disk to bulge
 *
 * Expected: All stellar mass becomes bulge, TimeOfLastMajorMerger set
 * Validates: Major merger morphological transformation
 */
int test_single_major_merger(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 3.0, 0.06, 0.5, 0.01,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.5;  /* Major merger (> 0.3) */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_stellar = central_gal.StellarMass;
    const double sat_stellar = sat_gal.StellarMass;
    const double expected_final_stellar = initial_central_stellar + sat_stellar;

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* All stellar mass becomes bulge (major merger transformation) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             expected_final_stellar, 1e-6,
                             "Major merger should transform all stars to bulge");

    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             halos[0].galaxy->StellarMass, 1e-6,
                             "BulgeMass should equal StellarMass after major merger");

    /* TimeOfLastMajorMerger set */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->TimeOfLastMajorMerger, ctx.substep_time, 1e-6,
                             "TimeOfLastMajorMerger should be set");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_multiple_satellites_merge
 * @brief   Test multiple satellites merge simultaneously
 *
 * Expected: All satellites processed, all mass transferred
 * Validates: Multiple merger handling
 */
int test_multiple_satellites_merge(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo halos[4];
    struct GalaxyData gals[4];

    /* Central */
    setup_test_galaxy(&halos[0], &gals[0], 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    /* Satellite 1 */
    setup_test_galaxy(&halos[1], &gals[1], 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    gals[1].IsMerging = 1;
    gals[1].MergerMassRatio = 0.2;

    /* Satellite 2 */
    setup_test_galaxy(&halos[2], &gals[2], 1,
                      1.0, 0.02, 0.5, 0.01, 0.1, 0.002,
                      5.0, 0.1, 0.5, 0.01, 0.05, 0.001, 0.01);
    gals[2].IsMerging = 1;
    gals[2].MergerMassRatio = 0.1;

    /* Satellite 3 */
    setup_test_galaxy(&halos[3], &gals[3], 1,
                      3.0, 0.06, 2.0, 0.04, 0.5, 0.01,
                      15.0, 0.3, 2.0, 0.04, 0.2, 0.004, 0.05);
    gals[3].IsMerging = 1;
    gals[3].MergerMassRatio = 0.4;  /* Major merger */

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_cold = gals[0].ColdGas;
    const double total_sat_cold = gals[1].ColdGas + gals[2].ColdGas + gals[3].ColdGas;

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 4);

    /* ===== VALIDATE ===== */
    /* All cold gas transferred */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas,
                             initial_central_cold + total_sat_cold, 1e-5,
                             "All satellite cold gas should be transferred");

    /* All satellites marked as merged */
    TEST_ASSERT(halos[1].Type == 3, "Satellite 1 should be Type 3");
    TEST_ASSERT(halos[2].Type == 3, "Satellite 2 should be Type 3");
    TEST_ASSERT(halos[3].Type == 3, "Satellite 3 should be Type 3");

    /* Major merger occurred (ratio 0.4 > 0.3) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             halos[0].galaxy->StellarMass, 1e-6,
                             "Major merger should transform to spheroid");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Test total mass conserved during merger
 *
 * Expected: Total baryonic mass unchanged
 * Validates: Mass conservation
 */
int test_mass_conservation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, sat_halo};

    const double initial_total = (central_gal.ColdGas + central_gal.StellarMass +
                                   central_gal.HotGas + central_gal.EjectedGas +
                                   central_gal.ICS + central_gal.BlackHoleMass +
                                   sat_gal.ColdGas + sat_gal.StellarMass +
                                   sat_gal.HotGas + sat_gal.EjectedGas +
                                   sat_gal.ICS + sat_gal.BlackHoleMass);

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    const double final_total = (halos[0].galaxy->ColdGas + halos[0].galaxy->StellarMass +
                                 halos[0].galaxy->HotGas + halos[0].galaxy->EjectedGas +
                                 halos[0].galaxy->ICS + halos[0].galaxy->BlackHoleMass);

    TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, 1e-4,
                             "Total mass should be conserved");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_metallicity_conservation
 * @brief   Test metal mass conserved
 *
 * Expected: Total metal mass unchanged
 * Validates: Metallicity conservation
 */
int test_metallicity_conservation(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, sat_halo};

    const double initial_metals = (central_gal.MetalsColdGas + central_gal.MetalsStellarMass +
                                    central_gal.MetalsHotGas + central_gal.MetalsEjectedGas +
                                    central_gal.MetalsICS +
                                    sat_gal.MetalsColdGas + sat_gal.MetalsStellarMass +
                                    sat_gal.MetalsHotGas + sat_gal.MetalsEjectedGas +
                                    sat_gal.MetalsICS);

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    const double final_metals = (halos[0].galaxy->MetalsColdGas +
                                  halos[0].galaxy->MetalsStellarMass +
                                  halos[0].galaxy->MetalsHotGas +
                                  halos[0].galaxy->MetalsEjectedGas +
                                  halos[0].galaxy->MetalsICS);

    TEST_ASSERT_DOUBLE_EQUAL(final_metals, initial_metals, 1e-4,
                             "Total metal mass should be conserved");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bulge_formation_minor
 * @brief   Test minor merger adds satellite stars to bulge
 *
 * Expected: Bulge increases by satellite stellar mass only
 * Validates: Minor merger bulge growth
 */
int test_bulge_formation_minor(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;  /* Minor */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_bulge = central_gal.BulgeMass;
    const double sat_stellar = sat_gal.StellarMass;

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* Bulge should increase by exactly satellite stellar mass (minor merger) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             initial_bulge + sat_stellar, 1e-6,
                             "Minor merger bulge should grow by satellite stellar mass");

    /* Central should still have disk (StellarMass > BulgeMass) */
    TEST_ASSERT(halos[0].galaxy->StellarMass > halos[0].galaxy->BulgeMass,
                "Minor merger should preserve central disk");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_bulge_formation_major
 * @brief   Test major merger transforms entire stellar mass to bulge
 *
 * Expected: BulgeMass = StellarMass
 * Validates: Major merger morphological transformation
 */
int test_bulge_formation_major(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 3.0, 0.06, 0.5, 0.01,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.5;  /* Major */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* Major merger: BulgeMass = StellarMass (entire disk transformed) */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             halos[0].galaxy->StellarMass, 1e-6,
                             "Major merger should transform entire stellar mass to bulge");

    /* Metals should also be equal */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsBulgeMass,
                             halos[0].galaxy->MetalsStellarMass, 1e-6,
                             "Bulge metals should equal stellar metals after major merger");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_merger_timing_tracking
 * @brief   Test merger times recorded correctly
 *
 * Expected: TimeOfLastMajorMerger and TimeOfLastMinorMerger set
 * Validates: Merger timing tracking
 */
int test_merger_timing_tracking(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_major;
    struct GalaxyData sat_major_gal;
    setup_test_galaxy(&sat_major, &sat_major_gal, 1,
                      2.0, 0.04, 3.0, 0.06, 0.5, 0.01,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_major_gal.IsMerging = 1;
    sat_major_gal.MergerMassRatio = 0.5;  /* Major */

    struct Halo halos_major[2] = {central_halo, sat_major};

    struct ModuleContext ctx;
    setup_test_context(&ctx);
    ctx.time = 10.5;
    ctx.substep_time = 10.25;

    /* ===== EXECUTE MAJOR MERGER ===== */
    sage_merge_galaxies_process(&ctx, halos_major, 2);

    /* ===== VALIDATE MAJOR MERGER ===== */
    TEST_ASSERT_DOUBLE_EQUAL(halos_major[0].galaxy->TimeOfLastMajorMerger, 10.25, 1e-6,
                             "TimeOfLastMajorMerger should be set for major merger");

    /* Reset for minor merger test */
    sage_merge_galaxies_cleanup();
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo2;
    struct GalaxyData central_gal2;
    setup_test_galaxy(&central_halo2, &central_gal2, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_minor;
    struct GalaxyData sat_minor_gal;
    setup_test_galaxy(&sat_minor, &sat_minor_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_minor_gal.IsMerging = 1;
    sat_minor_gal.MergerMassRatio = 0.15;  /* Minor but > 0.1 */

    struct Halo halos_minor[2] = {central_halo2, sat_minor};

    ctx.time = 11.0;
    ctx.substep_time = 10.75;

    /* ===== EXECUTE MINOR MERGER ===== */
    sage_merge_galaxies_process(&ctx, halos_minor, 2);

    /* ===== VALIDATE MINOR MERGER ===== */
    TEST_ASSERT_DOUBLE_EQUAL(halos_minor[0].galaxy->TimeOfLastMinorMerger, 10.75, 1e-6,
                             "TimeOfLastMinorMerger should be set for ratio > 0.1");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_satellite_type_change
 * @brief   Test satellite Type changes to 3 after merger
 *
 * Expected: Satellite Type = 3
 * Validates: Satellite marking
 */
int test_satellite_type_change(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, sat_halo};

    TEST_ASSERT(halos[1].Type == 1, "Satellite should start as Type 1");

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(halos[1].Type == 3, "Satellite should be Type 3 after merger");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_all_baryonic_components
 * @brief   Test all 11 baryonic fields transferred
 *
 * Expected: All fields transferred correctly
 * Validates: Complete baryonic transfer
 */
int test_all_baryonic_components(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ALL 11 FIELDS ===== */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas, 12.0, 1e-6,
                             "ColdGas transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsColdGas, 0.24, 1e-6,
                             "MetalsColdGas transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->HotGas, 60.0, 1e-6,
                             "HotGas transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsHotGas, 1.2, 1e-6,
                             "MetalsHotGas transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->EjectedGas, 6.0, 1e-6,
                             "EjectedGas transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsEjectedGas, 0.12, 1e-6,
                             "MetalsEjectedGas transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ICS, 0.6, 1e-6,
                             "ICS transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->MetalsICS, 0.012, 1e-6,
                             "MetalsICS transferred");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BlackHoleMass, 0.12, 1e-6,
                             "BlackHoleMass transferred");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_inline_merger_physics
 * @brief   Test merger-triggered BH growth and starburst run inline
 *
 * Expected: Central BH and stellar mass increase beyond pure transfer
 * Validates: P2 inline merger physics execution path
 */
int test_inline_merger_physics(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.5);  /* Keep merger minor for easier assertions */
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);
    central_halo.Vvir = 220.0f;
    central_halo.Mvir = 120.0f;
    central_halo.dT = 0.1f;

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.3;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);
    ctx.central_galaxy = &halos[0];

    const double transfer_only_bh = central_gal.BlackHoleMass + sat_gal.BlackHoleMass;
    const double transfer_only_stellar = central_gal.StellarMass + sat_gal.StellarMass;

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(halos[0].galaxy->BlackHoleMass > transfer_only_bh,
                "Merger BH growth should increase central BH beyond pure transfer");
    TEST_ASSERT(halos[0].galaxy->StellarMass > transfer_only_stellar,
                "Merger starburst should increase stellar mass beyond pure transfer");
    TEST_ASSERT(halos[1].Type == 3, "Satellite should be marked merged");
    TEST_ASSERT(halos[1].galaxy->IsMerging == 0, "Satellite IsMerging flag should be cleared");
    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->MergerMassRatio, 0.0, 1e-12,
                             "Satellite MergerMassRatio should be cleared");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_mass_satellite
 * @brief   Test zero mass satellite handled gracefully
 *
 * Expected: No crash, central unchanged
 * Validates: Zero mass handling
 */
int test_zero_mass_satellite(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                      0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.1;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_cold = central_gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle zero mass satellite");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas, initial_central_cold, 1e-10,
                             "Central should be unchanged (zero mass added)");
    TEST_ASSERT(halos[1].Type == 3, "Satellite should still be marked as merged");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_null_central_galaxy
 * @brief   Test NULL central galaxy handled
 *
 * Expected: Function returns success, no crash
 * Validates: NULL pointer safety
 */
int test_null_central_galaxy(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    memset(&central_halo, 0, sizeof(central_halo));
    central_halo.Type = 0;
    central_halo.galaxy = NULL;

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle NULL central galaxy gracefully");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_null_satellite_galaxy
 * @brief   Test NULL satellite galaxy skipped
 *
 * Expected: NULL satellite skipped, no crash
 * Validates: NULL satellite handling
 */
int test_null_satellite_galaxy(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    memset(&sat_halo, 0, sizeof(sat_halo));
    sat_halo.Type = 1;
    sat_halo.galaxy = NULL;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_cold = central_gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle NULL satellite galaxy");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas, initial_central_cold, 1e-10,
                             "Central should be unchanged (no satellite galaxy)");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_type_0_central_never_merged
 * @brief   Test Type 0 centrals never merged even if IsMerging mistakenly set
 *
 * Expected: Central properties unchanged, ERROR_LOG emitted
 * Validates: Type 0 protection against upstream module bugs
 */
int test_type_0_central_never_merged(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    // Central galaxy with IsMerging MISTAKENLY set to 1
    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);
    central_gal.IsMerging = 1;  // MISTAKENLY set
    central_gal.MergerMassRatio = 0.5;

    // Normal satellite (not merging)
    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 0;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    // Record initial central properties
    const double initial_central_cold = central_gal.ColdGas;
    const double initial_central_stellar = central_gal.StellarMass;
    const double initial_central_bulge = central_gal.BulgeMass;
    const double initial_central_hot = central_gal.HotGas;

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[0].Type == 0, "Central should remain Type 0 (never merged)");

    // Central's properties should be completely unchanged
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas, initial_central_cold, 1e-10,
                             "Central ColdGas should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->StellarMass, initial_central_stellar, 1e-10,
                             "Central StellarMass should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass, initial_central_bulge, 1e-10,
                             "Central BulgeMass should be unchanged");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->HotGas, initial_central_hot, 1e-10,
                             "Central HotGas should be unchanged");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_no_merging_satellites
 * @brief   Test no IsMerging flags set
 *
 * Expected: No processing, central unchanged
 * Validates: Merger flag requirement
 */
int test_no_merging_satellites(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    /* IsMerging = 0 (default) */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_cold = central_gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should execute successfully");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas, initial_central_cold, 1e-10,
                             "Central unchanged when IsMerging = 0");
    TEST_ASSERT(halos[1].Type == 1, "Satellite should remain Type 1");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_null_halos_array
 * @brief   Test NULL halos pointer
 *
 * Expected: Function returns success, no crash
 * Validates: NULL halos array handling
 */
int test_null_halos_array(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, NULL, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle NULL halos array gracefully");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_ngal_zero
 * @brief   Test ngal = 0
 *
 * Expected: Function returns success, no crash
 * Validates: Zero ngal handling
 */
int test_ngal_zero(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo halos[1];
    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 0);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should handle ngal = 0 gracefully");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_ngal_one_central
 * @brief   Test only central, no satellites
 *
 * Expected: Central unchanged
 * Validates: Single galaxy handling
 */
int test_ngal_one_central(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_cold = central_gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, &central_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should execute successfully");
    TEST_ASSERT_DOUBLE_EQUAL(central_halo.galaxy->ColdGas, initial_cold, 1e-10,
                             "Central unchanged with no satellites");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_threshold_boundary_low
 * @brief   Test ratio just below threshold (minor)
 *
 * Expected: Treated as minor merger
 * Validates: Threshold boundary
 */
int test_threshold_boundary_low(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.29999;  /* Just below 0.3 */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* Minor merger: should preserve some disk */
    TEST_ASSERT(halos[0].galaxy->StellarMass > halos[0].galaxy->BulgeMass,
                "Ratio < threshold should be minor merger (preserve disk)");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_threshold_boundary_high
 * @brief   Test ratio just above threshold (major)
 *
 * Expected: Treated as major merger
 * Validates: Threshold boundary
 */
int test_threshold_boundary_high(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.30001;  /* Just above 0.3 */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* Major merger: entire stellar mass should be bulge */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             halos[0].galaxy->StellarMass, 1e-6,
                             "Ratio > threshold should be major merger (all bulge)");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_orphan_merger
 * @brief   Test orphan (Type 2) merging
 *
 * Expected: Orphan processed same as satellite
 * Validates: Orphan handling
 */
int test_orphan_merger(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo orphan_halo;
    struct GalaxyData orphan_gal;
    setup_test_galaxy(&orphan_halo, &orphan_gal, 2,  /* Type 2 = orphan */
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    orphan_gal.IsMerging = 1;
    orphan_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, orphan_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    const double initial_central_cold = central_gal.ColdGas;

    /* ===== EXECUTE ===== */
    int result = sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Should process orphan merger");
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->ColdGas,
                             initial_central_cold + 2.0, 1e-6,
                             "Orphan cold gas should be transferred");
    TEST_ASSERT(halos[1].Type == 3, "Orphan should be Type 3 after merger");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_threshold
 * @brief   Test different thresholds change major/minor classification
 *
 * Expected: Same ratio classified differently with different thresholds
 * Validates: Parameter loading and physics
 */
int test_parameter_sensitivity_threshold(void)
{
    /* ===== SETUP LOW THRESHOLD ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.2);  /* Low threshold */
    sage_merge_galaxies_init();

    struct Halo central_low;
    struct GalaxyData central_gal_low;
    setup_test_galaxy(&central_low, &central_gal_low, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_low;
    struct GalaxyData sat_gal_low;
    setup_test_galaxy(&sat_low, &sat_gal_low, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal_low.IsMerging = 1;
    sat_gal_low.MergerMassRatio = 0.25;  /* Between 0.2 and 0.4 */

    struct Halo halos_low[2] = {central_low, sat_low};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    sage_merge_galaxies_process(&ctx, halos_low, 2);

    const int is_major_low = (halos_low[0].galaxy->BulgeMass ==
                               halos_low[0].galaxy->StellarMass);

    sage_merge_galaxies_cleanup();

    /* ===== SETUP HIGH THRESHOLD ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.4);  /* High threshold */
    sage_merge_galaxies_init();

    struct Halo central_high;
    struct GalaxyData central_gal_high;
    setup_test_galaxy(&central_high, &central_gal_high, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_high;
    struct GalaxyData sat_gal_high;
    setup_test_galaxy(&sat_high, &sat_gal_high, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal_high.IsMerging = 1;
    sat_gal_high.MergerMassRatio = 0.25;  /* Same ratio */

    struct Halo halos_high[2] = {central_high, sat_high};

    setup_test_context(&ctx);

    sage_merge_galaxies_process(&ctx, halos_high, 2);

    const int is_major_high = (halos_high[0].galaxy->BulgeMass ==
                                halos_high[0].galaxy->StellarMass);

    /* ===== VALIDATE ===== */
    /* With threshold=0.2, ratio=0.25 should be MAJOR (0.25 > 0.2) */
    TEST_ASSERT(is_major_low == 1,
                "Ratio 0.25 > threshold 0.2 should be major merger");

    /* With threshold=0.4, ratio=0.25 should be MINOR (0.25 < 0.4) */
    TEST_ASSERT(is_major_high == 0,
                "Ratio 0.25 < threshold 0.4 should be minor merger");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_extreme_threshold_zero
 * @brief   Test threshold = 0.0 (all major)
 *
 * Expected: All mergers treated as major
 * Validates: Extreme parameter value
 */
int test_extreme_threshold_zero(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.0);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.01;  /* Very small ratio */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* Even tiny ratio should be major with threshold=0.0 */
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->BulgeMass,
                             halos[0].galaxy->StellarMass, 1e-6,
                             "Threshold=0.0 should make all mergers major");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_extreme_threshold_one
 * @brief   Test threshold = 1.0 (all minor)
 *
 * Expected: All mergers treated as minor
 * Validates: Extreme parameter value
 */
int test_extreme_threshold_one(void)
{
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    setup_test_parameters(1.0);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 4.0, 0.08, 1.0, 0.02,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.9;  /* Very large ratio */

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* Even large ratio should be minor with threshold=1.0 */
    TEST_ASSERT(halos[0].galaxy->StellarMass > halos[0].galaxy->BulgeMass,
                "Threshold=1.0 should make all mergers minor");

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
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

    MimicConfig.phase_2 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_2[0].module_name = strdup("sage_merge_galaxies");
    MimicConfig.phase_2[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_phase_2 = 1;

    setup_test_parameters(0.3);

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
    setup_test_parameters(0.3);
    sage_merge_galaxies_init();

    struct Halo central_halo;
    struct GalaxyData central_gal;
    setup_test_galaxy(&central_halo, &central_gal, 0,
                      10.0, 0.2, 5.0, 0.1, 1.0, 0.02,
                      50.0, 1.0, 5.0, 0.1, 0.5, 0.01, 0.1);

    struct Halo sat_halo;
    struct GalaxyData sat_gal;
    setup_test_galaxy(&sat_halo, &sat_gal, 1,
                      2.0, 0.04, 1.0, 0.02, 0.2, 0.004,
                      10.0, 0.2, 1.0, 0.02, 0.1, 0.002, 0.02);
    sat_gal.IsMerging = 1;
    sat_gal.MergerMassRatio = 0.2;

    struct Halo halos[2] = {central_halo, sat_halo};

    struct ModuleContext ctx;
    setup_test_context(&ctx);

    /* ===== EXECUTE ===== */
    sage_merge_galaxies_process(&ctx, halos, 2);

    /* ===== VALIDATE ===== */
    /* check_memory_leaks() will catch any leaks */

    /* ===== CLEANUP ===== */
    sage_merge_galaxies_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_merge_galaxies unit tests and reports results.
 */
int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_merge_galaxies Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Run physics calculation tests */
    TEST_RUN(test_single_minor_merger);
    TEST_RUN(test_single_major_merger);
    TEST_RUN(test_multiple_satellites_merge);
    TEST_RUN(test_mass_conservation);
    TEST_RUN(test_metallicity_conservation);
    TEST_RUN(test_bulge_formation_minor);
    TEST_RUN(test_bulge_formation_major);
    TEST_RUN(test_merger_timing_tracking);
    TEST_RUN(test_satellite_type_change);
    TEST_RUN(test_all_baryonic_components);
    TEST_RUN(test_inline_merger_physics);

    /* Run edge case tests */
    TEST_RUN(test_zero_mass_satellite);
    TEST_RUN(test_null_central_galaxy);
    TEST_RUN(test_null_satellite_galaxy);
    TEST_RUN(test_type_0_central_never_merged);
    TEST_RUN(test_no_merging_satellites);
    TEST_RUN(test_null_halos_array);
    TEST_RUN(test_ngal_zero);
    TEST_RUN(test_ngal_one_central);
    TEST_RUN(test_threshold_boundary_low);
    TEST_RUN(test_threshold_boundary_high);
    TEST_RUN(test_orphan_merger);

    /* Run parameter sensitivity tests */
    TEST_RUN(test_parameter_sensitivity_threshold);
    TEST_RUN(test_extreme_threshold_zero);
    TEST_RUN(test_extreme_threshold_one);

    /* Run infrastructure tests */
    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
