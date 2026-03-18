/**
 * @file    test_unit_mixed_dt_parity.c
 * @brief   Mixed-dT parity test — verifies per-object timestep in FOF groups
 *
 * When satellites in a FOF group have different SnapNum values (and hence
 * different dT), each object must evolve with its own timestep. This test
 * constructs deterministic FOF fixtures where the central and satellites
 * have different dT values, then verifies that each module uses the
 * per-object dT rather than a single global value.
 *
 * SAGE parity rule: deltaT_p = Age[snap_of_p] - Age[current_halo_snap]
 * Per-substep dt for object p: deltaT_p / num_substeps
 *
 * Test cases:
 *   MERGER TIME TESTS (IMMEDIATE HANDLER):
 *   - test_mixed_dt_merger_timestamp_source_time: Merger timestamp uses source-object time
 *
 *   INVALID DT TESTS (IMMEDIATE HANDLER):
 *   - test_invalid_nonboundary_merger_time_error: non-boundary dT<=0 fails fast
 *
 *   REINCORPORATION TESTS:
 *   - test_mixed_dt_reincorporation: Central uses its own dT for reincorporation rate
 *   - test_boundary_sentinel_reincorporation_noop: SnapNum<0,dT<=0 is a deterministic no-op
 *   - test_invalid_nonboundary_reincorporation_error: SnapNum>=0,dT<=0 fails fast
 *
 *   STAR FORMATION TESTS:
 *   - test_mixed_dt_star_formation: Different dT gives different stellar mass
 *
 *   COOLING TESTS:
 *   - test_mixed_dt_cooling_ignores_global: Cooling uses halo->dT, not ctx->substep_dt
 *   - test_mixed_dt_cooling_scales_with_dt: CoolingGas scales linearly with dT
 *
 *   RADIO MODE HEATING TESTS:
 *   - test_mixed_dt_radio_mode_ignores_global: AGN heating uses halo->dT, not ctx->substep_dt
 *
 * @author  Mimic Development Team
 * @date    2026-03-07
 */

#include "../../../tests/framework/test_framework.h"
#include "../core/module_interface.h"
#include "../include/types.h"
#include "../include/globals.h"
#include "../util/error.h"
#include "../util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics */
static int passed = 0;
static int failed = 0;

/* Suppress unused warnings */
__attribute__((unused)) static int *_passed_ptr = &passed;
__attribute__((unused)) static int *_failed_ptr = &failed;

/* External module interfaces */
extern int sage_reincorporation_init(void);
extern int sage_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_reincorporation_cleanup(void);

extern int sage_star_formation_init(void);
extern int sage_star_formation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_star_formation_cleanup(void);

extern int sage_calculate_cooling_budget_init(void);
extern int sage_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_calculate_cooling_budget_cleanup(void);

extern int sage_radio_mode_heating_init(void);
extern int sage_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_radio_mode_heating_cleanup(void);

extern int sage_resolve_mergers_and_disruption_init(void);
extern int sage_resolve_mergers_and_disruption_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_resolve_mergers_and_disruption_cleanup(void);
extern void sage_resolve_mergers_and_disruption_set_action_hook(void (*hook)(const char *action, int source_index, int target_index, double mass_ratio));

extern void set_test_model_parameters(void);

/* ========================================================================== */
/* HELPERS                                                                    */
/* ========================================================================== */

static void reset_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

static struct ModuleContext create_context(double central_dt, int num_substeps)
{
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.redshift = 0.0;
    ctx.time = 13.6;
    ctx.snapshot_number = 63;
    ctx.substep_number = 0;
    ctx.num_substeps = num_substeps;
    ctx.time_interval = central_dt;
    ctx.substep_dt = central_dt / num_substeps;
    ctx.params = &MimicConfig;

    return ctx;
}

static struct GalaxyData *alloc_galaxy(void)
{
    struct GalaxyData *gal = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
    memset(gal, 0, sizeof(struct GalaxyData));
    return gal;
}

static void free_galaxy(struct GalaxyData **gal)
{
    if (*gal != NULL) {
        myfree(*gal);
        *gal = NULL;
    }
}

/* ========================================================================== */
/* MERGER TIME: MIXED-DT TIMESTAMP (IMMEDIATE HANDLER)                       */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_merger_timestamp_source_time
 * @brief   TimeOfLastMinorMerger uses source-object time, not ctx->substep_time
 *
 * Satellite dT=0.4, num_substeps=2, substep_number=1.
 * source_dt = 0.4/2 = 0.2.
 * Satellite MergTime = 0.1 -> after decrement: 0.1 - 0.2 = -0.1 <= 0 -> merge.
 * Expected TimeOfLastMinorMerger = ctx.time + sat.dT - (substep+0.5)*dt_obj
 *                                 = 13.0 + 0.4 - (1.5 * 0.2) = 13.1
 * ctx.substep_time = 12.7 (deliberately different to detect wrong source).
 */
int test_mixed_dt_merger_timestamp_source_time(void)
{
    init_memory_system(0);
    reset_config();

    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "0.3");
    snprintf(MimicConfig.ModelParams[1].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
    snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "2.0");
    MimicConfig.NumModelParams = 2;
    sage_resolve_mergers_and_disruption_init();

    struct GalaxyData *central_gal = alloc_galaxy();
    central_gal->StellarMass = 10.0;
    central_gal->ColdGas = 5.0;
    central_gal->TimeOfLastMinorMerger = 0.0;
    struct Halo central = {0};
    central.Type = 0;
    central.SnapNum = 63;
    central.dT = 0.2;
    central.galaxy = central_gal;

    struct GalaxyData *sat_gal = alloc_galaxy();
    sat_gal->StellarMass = 2.0;   /* mass_ratio = (2+1)/(10+5) = 0.2 -> minor merger */
    sat_gal->ColdGas = 1.0;
    sat_gal->MergTime = 0.1f;     /* 0.1 - source_dt(0.2) = -0.1 -> merge */
    struct Halo sat = {0};
    sat.Type = 1;
    sat.HaloNr = 7;
    sat.SnapNum = 60;
    sat.dT = 0.4;
    sat.Mvir = 5.0;    /* virial_to_baryons = 5/(2+1) = 1.67 <= 2.0 -> eligible */
    sat.galaxy = sat_gal;

    struct ModuleContext ctx = create_context(0.2, 2);
    ctx.time = 13.0;
    ctx.snapshot_number = 63;
    ctx.substep_number = 1;
    ctx.substep_time = 12.7;  /* Deliberately different from source-object time */
    ctx.central_galaxy = &central;

    struct Halo halos[2] = {central, sat};
    int result = sage_resolve_mergers_and_disruption_process(&ctx, halos, 2);
    TEST_ASSERT(result == 0, "Immediate merger processing should succeed");

    const double dt_obj = sat.dT / ctx.num_substeps;
    const double expected_time = (ctx.time + sat.dT) - ((double)ctx.substep_number + 0.5) * dt_obj;
    TEST_ASSERT_DOUBLE_EQUAL(halos[0].galaxy->TimeOfLastMinorMerger, expected_time, 1e-6,
                             "Minor merger timestamp should use source-object time");
    TEST_ASSERT(fabs(halos[0].galaxy->TimeOfLastMinorMerger - ctx.substep_time) > 1e-3,
                "Minor merger timestamp should not use global ctx->substep_time");

    free_galaxy(&central_gal);
    free_galaxy(&sat_gal);
    sage_resolve_mergers_and_disruption_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* INVALID DT: FAIL-FAST (IMMEDIATE HANDLER)                                 */
/* ========================================================================== */

/**
 * @test    test_invalid_nonboundary_merger_time_error
 * @brief   Immediate handler fails fast for non-boundary dT<=0
 */
int test_invalid_nonboundary_merger_time_error(void)
{
    init_memory_system(0);
    reset_config();

    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "0.3");
    snprintf(MimicConfig.ModelParams[1].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
    snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "1.0");
    MimicConfig.NumModelParams = 2;
    sage_resolve_mergers_and_disruption_init();

    struct GalaxyData *cen_gal = alloc_galaxy();
    cen_gal->StellarMass = 10.0;
    cen_gal->ColdGas = 5.0;
    struct Halo central = {0};
    central.Type = 0;
    central.SnapNum = 63;
    central.dT = 0.2;
    central.galaxy = cen_gal;

    struct GalaxyData *sat_gal = alloc_galaxy();
    sat_gal->MergTime = 1.0f;
    sat_gal->StellarMass = 1.0;
    sat_gal->ColdGas = 1.0;
    struct Halo sat = {0};
    sat.Type = 1;
    sat.HaloNr = 3;
    sat.SnapNum = 63;  /* Non-boundary */
    sat.dT = -1.0;     /* Invalid */
    sat.galaxy = sat_gal;

    struct ModuleContext ctx = create_context(0.2, 1);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, sat};

    const int result = sage_resolve_mergers_and_disruption_process(&ctx, halos, 2);
    TEST_ASSERT(result != 0, "Immediate handler should fail for non-boundary dT<=0");

    free_galaxy(&cen_gal);
    free_galaxy(&sat_gal);
    sage_resolve_mergers_and_disruption_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* REINCORPORATION: CENTRAL USES ITS OWN DT                                 */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_reincorporation
 * @brief   Reincorporation rate uses the central's own dT, not global substep_dt
 *
 * Two runs with the same central but different global ctx->substep_dt.
 * Since the module uses halos[0].dT, the result must be identical regardless
 * of what ctx->substep_dt is set to.
 */
int test_mixed_dt_reincorporation(void)
{
    init_memory_system(0);
    reset_config();
    set_test_model_parameters();
    sage_reincorporation_init();

    const float central_dT = 0.3;

    /* Run 1: ctx->substep_dt matches central dT (coincidental agreement) */
    struct GalaxyData *gal1 = alloc_galaxy();
    gal1->EjectedGas = 10.0;
    gal1->MetalsEjectedGas = 0.2;
    gal1->HotGas = 5.0;
    gal1->MetalsHotGas = 0.1;
    struct Halo halo1 = {0};
    halo1.Type = 0;
    halo1.Vvir = 500.0;
    halo1.Rvir = 0.2;
    halo1.SnapNum = 63;
    halo1.dT = central_dT;
    halo1.galaxy = gal1;

    struct ModuleContext ctx1 = create_context(central_dT, 1);
    ctx1.central_galaxy = &halo1;

    sage_reincorporation_process(&ctx1, &halo1, 1);
    float ejected_run1 = gal1->EjectedGas;
    float hot_run1 = gal1->HotGas;

    /* Run 2: ctx->substep_dt is DIFFERENT (0.8), but central dT is the same (0.3) */
    struct GalaxyData *gal2 = alloc_galaxy();
    gal2->EjectedGas = 10.0;
    gal2->MetalsEjectedGas = 0.2;
    gal2->HotGas = 5.0;
    gal2->MetalsHotGas = 0.1;
    struct Halo halo2 = {0};
    halo2.Type = 0;
    halo2.Vvir = 500.0;
    halo2.Rvir = 0.2;
    halo2.SnapNum = 63;
    halo2.dT = central_dT;  /* Same central dT */
    halo2.galaxy = gal2;

    struct ModuleContext ctx2 = create_context(0.8, 1);  /* Different global dt! */
    ctx2.central_galaxy = &halo2;

    sage_reincorporation_process(&ctx2, &halo2, 1);
    float ejected_run2 = gal2->EjectedGas;
    float hot_run2 = gal2->HotGas;

    /* Both runs must produce identical results (per-object dT, not global) */
    TEST_ASSERT_DOUBLE_EQUAL(ejected_run1, ejected_run2, 1e-10,
                             "Reincorporation must use central's dT, not ctx->substep_dt");
    TEST_ASSERT_DOUBLE_EQUAL(hot_run1, hot_run2, 1e-10,
                             "HotGas must match when central's dT is the same");

    /* Sanity: some reincorporation actually happened */
    TEST_ASSERT(ejected_run1 < 10.0, "Some gas should have been reincorporated");

    free_galaxy(&gal1);
    free_galaxy(&gal2);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_boundary_sentinel_reincorporation_noop
 * @brief   Boundary sentinel (SnapNum<0,dT<=0) is a deterministic no-op
 */
int test_boundary_sentinel_reincorporation_noop(void)
{
    init_memory_system(0);
    reset_config();
    set_test_model_parameters();
    sage_reincorporation_init();

    struct GalaxyData *gal = alloc_galaxy();
    gal->EjectedGas = 8.0;
    gal->MetalsEjectedGas = 0.16;
    gal->HotGas = 2.0;
    gal->MetalsHotGas = 0.04;

    struct Halo halo = {0};
    halo.Type = 0;
    halo.Vvir = 500.0;
    halo.Rvir = 0.2;
    halo.SnapNum = -1;  /* Boundary sentinel state */
    halo.dT = -1.0;
    halo.galaxy = gal;

    struct ModuleContext ctx = create_context(0.2, 1);
    ctx.central_galaxy = &halo;

    const float initial_ejected = gal->EjectedGas;
    const float initial_hot = gal->HotGas;
    const int result = sage_reincorporation_process(&ctx, &halo, 1);

    TEST_ASSERT(result == 0, "Boundary sentinel should be a no-op success");
    TEST_ASSERT_DOUBLE_EQUAL(gal->EjectedGas, initial_ejected, 1e-10,
                             "Boundary sentinel should not change EjectedGas");
    TEST_ASSERT_DOUBLE_EQUAL(gal->HotGas, initial_hot, 1e-10,
                             "Boundary sentinel should not change HotGas");

    free_galaxy(&gal);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_invalid_nonboundary_reincorporation_error
 * @brief   Non-boundary dT<=0 in reincorporation fails fast
 */
int test_invalid_nonboundary_reincorporation_error(void)
{
    init_memory_system(0);
    reset_config();
    set_test_model_parameters();
    sage_reincorporation_init();

    struct GalaxyData *gal = alloc_galaxy();
    gal->EjectedGas = 8.0;
    gal->MetalsEjectedGas = 0.16;
    gal->HotGas = 2.0;
    gal->MetalsHotGas = 0.04;

    struct Halo halo = {0};
    halo.Type = 0;
    halo.Vvir = 500.0;
    halo.Rvir = 0.2;
    halo.SnapNum = 63;  /* Non-boundary */
    halo.dT = -1.0;     /* Invalid */
    halo.galaxy = gal;

    struct ModuleContext ctx = create_context(0.2, 1);
    ctx.central_galaxy = &halo;

    const int result = sage_reincorporation_process(&ctx, &halo, 1);
    TEST_ASSERT(result != 0, "Non-boundary dT<=0 should fail fast");

    free_galaxy(&gal);
    sage_reincorporation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* STAR FORMATION: DIFFERENT DT GIVES DIFFERENT STELLAR MASS                 */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_star_formation
 * @brief   Two identical halos with different dT produce different NewStellarMass
 *
 * Stars = SFR_efficiency * (ColdGas - ColdCrit) / t_dyn * dt
 * With identical halo properties but dT_A=0.1 and dT_B=0.3, the ratio
 * of NewStellarMass must be 0.1/0.3 = 1/3.
 */
int test_mixed_dt_star_formation(void)
{
    init_memory_system(0);
    reset_config();
    set_test_model_parameters();
    sage_star_formation_init();

    /* Halo A: dT = 0.1 */
    struct GalaxyData *gal_a = alloc_galaxy();
    gal_a->ColdGas = 5.0;
    gal_a->DiskScaleRadius = 0.01;
    struct Halo halo_a = {0};
    halo_a.Type = 0;
    halo_a.Vvir = 200.0;
    halo_a.SnapNum = 63;
    halo_a.dT = 0.1;
    halo_a.galaxy = gal_a;

    struct ModuleContext ctx_a = create_context(0.1, 1);
    ctx_a.central_galaxy = &halo_a;

    sage_star_formation_process(&ctx_a, &halo_a, 1);
    float stars_a = gal_a->NewStellarMass;

    /* Halo B: identical properties but dT = 0.3 */
    struct GalaxyData *gal_b = alloc_galaxy();
    gal_b->ColdGas = 5.0;
    gal_b->DiskScaleRadius = 0.01;
    struct Halo halo_b = {0};
    halo_b.Type = 0;
    halo_b.Vvir = 200.0;
    halo_b.SnapNum = 63;
    halo_b.dT = 0.3;
    halo_b.galaxy = gal_b;

    struct ModuleContext ctx_b = create_context(0.3, 1);
    ctx_b.central_galaxy = &halo_b;

    sage_star_formation_process(&ctx_b, &halo_b, 1);
    float stars_b = gal_b->NewStellarMass;

    /* Both must form stars (sanity) */
    TEST_ASSERT(stars_a > 0.0, "Halo A should form stars");
    TEST_ASSERT(stars_b > 0.0, "Halo B should form stars");

    /* Stars scale linearly with dt → ratio must be dT_A/dT_B = 1/3 */
    double ratio = stars_a / stars_b;
    TEST_ASSERT_DOUBLE_EQUAL(ratio, 1.0 / 3.0, 1e-6,
                             "NewStellarMass ratio must equal dT ratio (proves per-object dt)");

    free_galaxy(&gal_a);
    free_galaxy(&gal_b);
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_mixed_dt_star_formation_ignores_global
 * @brief   Star formation uses halo->dT, not ctx->substep_dt
 *
 * Same halo->dT but different ctx->substep_dt → identical result.
 */
int test_mixed_dt_star_formation_ignores_global(void)
{
    init_memory_system(0);
    reset_config();
    set_test_model_parameters();
    sage_star_formation_init();

    const float halo_dT = 0.2;

    /* Run 1: ctx->substep_dt = 0.2 (matches halo dT) */
    struct GalaxyData *gal1 = alloc_galaxy();
    gal1->ColdGas = 5.0;
    gal1->DiskScaleRadius = 0.01;
    struct Halo halo1 = {0};
    halo1.Type = 0;
    halo1.Vvir = 200.0;
    halo1.SnapNum = 63;
    halo1.dT = halo_dT;
    halo1.galaxy = gal1;

    struct ModuleContext ctx1 = create_context(0.2, 1);
    sage_star_formation_process(&ctx1, &halo1, 1);
    float stars1 = gal1->NewStellarMass;

    /* Run 2: ctx->substep_dt = 0.9 (different!), but halo->dT still 0.2 */
    struct GalaxyData *gal2 = alloc_galaxy();
    gal2->ColdGas = 5.0;
    gal2->DiskScaleRadius = 0.01;
    struct Halo halo2 = {0};
    halo2.Type = 0;
    halo2.Vvir = 200.0;
    halo2.SnapNum = 63;
    halo2.dT = halo_dT;
    halo2.galaxy = gal2;

    struct ModuleContext ctx2 = create_context(0.9, 1);  /* Different global dt */
    sage_star_formation_process(&ctx2, &halo2, 1);
    float stars2 = gal2->NewStellarMass;

    /* Must be identical — module uses halo->dT, not ctx->substep_dt */
    TEST_ASSERT_DOUBLE_EQUAL(stars1, stars2, 1e-10,
                             "Star formation must use halo->dT, not ctx->substep_dt");
    TEST_ASSERT(stars1 > 0.0, "Stars should have formed");

    free_galaxy(&gal1);
    free_galaxy(&gal2);
    sage_star_formation_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* COOLING: USES PER-OBJECT DT, NOT GLOBAL SUBSTEP_DT                       */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_cooling_ignores_global
 * @brief   Cooling uses halo->dT, not ctx->substep_dt
 *
 * Same halo->dT but different ctx->substep_dt → identical CoolingGas.
 * If the code used ctx->substep_dt, the two runs would differ.
 */
int test_mixed_dt_cooling_ignores_global(void)
{
    init_memory_system(0);
    reset_config();

    /* Unit conversions needed by cooling_recipe() */
    MimicConfig.UnitDensity_in_cgs = 6.769911178294543e-22;
    MimicConfig.UnitTime_in_s = 3.08568e16;

    sage_calculate_cooling_budget_init();

    const float halo_dT = 0.2;

    /* Run 1: ctx->substep_dt = 0.2 (matches halo dT) */
    struct GalaxyData *gal1 = alloc_galaxy();
    gal1->HotGas = 10.0;
    gal1->MetalsHotGas = 0.02;  /* 0.2% metallicity */
    struct Halo halo1 = {0};
    halo1.Type = 0;
    halo1.Vvir = 200.0;
    halo1.Rvir = 0.2;
    halo1.SnapNum = 63;
    halo1.dT = halo_dT;
    halo1.galaxy = gal1;

    struct ModuleContext ctx1 = create_context(0.2, 1);
    sage_calculate_cooling_budget_process(&ctx1, &halo1, 1);
    float cooling1 = gal1->CoolingGas;

    /* Run 2: ctx->substep_dt = 0.9 (different!), but halo->dT still 0.2 */
    struct GalaxyData *gal2 = alloc_galaxy();
    gal2->HotGas = 10.0;
    gal2->MetalsHotGas = 0.02;
    struct Halo halo2 = {0};
    halo2.Type = 0;
    halo2.Vvir = 200.0;
    halo2.Rvir = 0.2;
    halo2.SnapNum = 63;
    halo2.dT = halo_dT;
    halo2.galaxy = gal2;

    struct ModuleContext ctx2 = create_context(0.9, 1);  /* Different global dt */
    sage_calculate_cooling_budget_process(&ctx2, &halo2, 1);
    float cooling2 = gal2->CoolingGas;

    /* Must be identical — module uses halo->dT, not ctx->substep_dt */
    TEST_ASSERT_DOUBLE_EQUAL(cooling1, cooling2, 1e-10,
                             "Cooling must use halo->dT, not ctx->substep_dt");
    TEST_ASSERT(cooling1 > 0.0, "Some cooling should have occurred");

    free_galaxy(&gal1);
    free_galaxy(&gal2);
    sage_calculate_cooling_budget_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_mixed_dt_cooling_scales_with_dt
 * @brief   Two identical halos with different dT produce different CoolingGas
 *
 * CoolingGas is proportional to dt. With dT_A=0.1 and dT_B=0.3,
 * the ratio of CoolingGas must be 1/3.
 */
int test_mixed_dt_cooling_scales_with_dt(void)
{
    init_memory_system(0);
    reset_config();

    MimicConfig.UnitDensity_in_cgs = 6.769911178294543e-22;
    MimicConfig.UnitTime_in_s = 3.08568e16;

    sage_calculate_cooling_budget_init();

    /* Use very small dT values to stay well below HotGas cap.
     * Cold accretion rate ~ HotGas/(Rvir/Vvir) * dt = 10/0.001 * dt = 10000*dt
     * So dt must be << 0.001 to avoid capping at HotGas=10. */

    /* Halo A: dT = 0.0001 → coolingGas ~ 1.0 (< HotGas=10) */
    struct GalaxyData *gal_a = alloc_galaxy();
    gal_a->HotGas = 10.0;
    gal_a->MetalsHotGas = 0.02;
    struct Halo halo_a = {0};
    halo_a.Type = 0;
    halo_a.Vvir = 200.0;
    halo_a.Rvir = 0.2;
    halo_a.SnapNum = 63;
    halo_a.dT = 0.0001;
    halo_a.galaxy = gal_a;

    struct ModuleContext ctx_a = create_context(0.0001, 1);
    sage_calculate_cooling_budget_process(&ctx_a, &halo_a, 1);
    float cooling_a = gal_a->CoolingGas;

    /* Halo B: identical properties but dT = 0.0003 → coolingGas ~ 3.0 (< HotGas=10) */
    struct GalaxyData *gal_b = alloc_galaxy();
    gal_b->HotGas = 10.0;
    gal_b->MetalsHotGas = 0.02;
    struct Halo halo_b = {0};
    halo_b.Type = 0;
    halo_b.Vvir = 200.0;
    halo_b.Rvir = 0.2;
    halo_b.SnapNum = 63;
    halo_b.dT = 0.0003;
    halo_b.galaxy = gal_b;

    struct ModuleContext ctx_b = create_context(0.0003, 1);
    sage_calculate_cooling_budget_process(&ctx_b, &halo_b, 1);
    float cooling_b = gal_b->CoolingGas;

    /* Both must cool (sanity) */
    TEST_ASSERT(cooling_a > 0.0, "Halo A should cool");
    TEST_ASSERT(cooling_b > 0.0, "Halo B should cool");

    /* CoolingGas scales linearly with dt → ratio must be dT_A/dT_B = 1/3 */
    double ratio = cooling_a / cooling_b;
    TEST_ASSERT_DOUBLE_EQUAL(ratio, 1.0 / 3.0, 1e-5,
                             "CoolingGas ratio must equal dT ratio (proves per-object dt)");

    free_galaxy(&gal_a);
    free_galaxy(&gal_b);
    sage_calculate_cooling_budget_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* RADIO MODE HEATING: USES PER-OBJECT DT, NOT GLOBAL SUBSTEP_DT            */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_radio_mode_ignores_global
 * @brief   Radio-mode AGN heating uses halo->dT, not ctx->substep_dt
 *
 * Same halo->dT but different ctx->substep_dt → identical BH accretion
 * and cooling suppression. If the code used ctx->substep_dt, results differ.
 */
int test_mixed_dt_radio_mode_ignores_global(void)
{
    init_memory_system(0);
    reset_config();

    /* Unit conversions needed by AGN rate calculations */
    MimicConfig.UnitMass_in_g = 1.989e43;
    MimicConfig.UnitTime_in_s = 3.08568e16;
    MimicConfig.UnitEnergy_in_cgs = 1.989e53;
    MimicConfig.Hubble_h = 0.73;

    /* Set radio mode parameters */
    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "0.08");
    snprintf(MimicConfig.ModelParams[1].param_name, MAX_STRING_LEN, "AGNrecipe");
    snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "1");
    MimicConfig.NumModelParams = 2;

    sage_radio_mode_heating_init();

    const float halo_dT = 0.2;

    /* Run 1: ctx->substep_dt = 0.2 (matches halo dT) */
    struct GalaxyData *gal1 = alloc_galaxy();
    gal1->HotGas = 10.0;
    gal1->MetalsHotGas = 0.2;
    gal1->BlackHoleMass = 0.01;
    gal1->CoolingGas = 1.0;
    gal1->Rcool = 0.05;
    gal1->Rheat = 0.0;
    struct Halo halo1 = {0};
    halo1.Type = 0;
    halo1.Mvir = 100.0;
    halo1.Vvir = 200.0;
    halo1.Rvir = 0.2;
    halo1.SnapNum = 63;
    halo1.dT = halo_dT;
    halo1.galaxy = gal1;

    struct ModuleContext ctx1 = create_context(0.2, 1);
    sage_radio_mode_heating_process(&ctx1, &halo1, 1);
    float bh1 = gal1->BlackHoleMass;
    float hot1 = gal1->HotGas;
    float cool1 = gal1->CoolingGas;

    /* Run 2: ctx->substep_dt = 0.9 (different!), but halo->dT still 0.2 */
    struct GalaxyData *gal2 = alloc_galaxy();
    gal2->HotGas = 10.0;
    gal2->MetalsHotGas = 0.2;
    gal2->BlackHoleMass = 0.01;
    gal2->CoolingGas = 1.0;
    gal2->Rcool = 0.05;
    gal2->Rheat = 0.0;
    struct Halo halo2 = {0};
    halo2.Type = 0;
    halo2.Mvir = 100.0;
    halo2.Vvir = 200.0;
    halo2.Rvir = 0.2;
    halo2.SnapNum = 63;
    halo2.dT = halo_dT;
    halo2.galaxy = gal2;

    struct ModuleContext ctx2 = create_context(0.9, 1);  /* Different global dt */
    sage_radio_mode_heating_process(&ctx2, &halo2, 1);
    float bh2 = gal2->BlackHoleMass;
    float hot2 = gal2->HotGas;
    float cool2 = gal2->CoolingGas;

    /* Must be identical — module uses halo->dT, not ctx->substep_dt */
    TEST_ASSERT_DOUBLE_EQUAL(bh1, bh2, 1e-10,
                             "BH mass must use halo->dT, not ctx->substep_dt");
    TEST_ASSERT_DOUBLE_EQUAL(hot1, hot2, 1e-10,
                             "HotGas must match when halo->dT is the same");
    TEST_ASSERT_DOUBLE_EQUAL(cool1, cool2, 1e-10,
                             "CoolingGas must match when halo->dT is the same");

    /* Sanity: AGN should have done something */
    TEST_ASSERT(bh1 > 0.01 || cool1 < 1.0,
                "AGN should have accreted or suppressed cooling");

    free_galaxy(&gal1);
    free_galaxy(&gal2);
    sage_radio_mode_heating_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* MAIN TEST RUNNER                                                          */
/* ========================================================================== */

int main(void)
{
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: Mixed-dT SAGE Parity\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    printf("\n%sMERGER TIME TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_merger_timestamp_source_time);
    TEST_RUN(test_invalid_nonboundary_merger_time_error);

    printf("\n%sREINCORPORATION TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_reincorporation);
    TEST_RUN(test_boundary_sentinel_reincorporation_noop);
    TEST_RUN(test_invalid_nonboundary_reincorporation_error);

    printf("\n%sSTAR FORMATION TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_star_formation);
    TEST_RUN(test_mixed_dt_star_formation_ignores_global);

    printf("\n%sCOOLING TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_cooling_ignores_global);
    TEST_RUN(test_mixed_dt_cooling_scales_with_dt);

    printf("\n%sRADIO MODE HEATING TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_radio_mode_ignores_global);

    TEST_SUMMARY();
    return TEST_RESULT();
}
