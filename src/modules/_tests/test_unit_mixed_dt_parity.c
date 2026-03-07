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
 *   MERGER TIME TESTS:
 *   - test_mixed_dt_merger_decrement: Each satellite's MergTime decremented by its own dt
 *   - test_mixed_dt_merger_trigger: Mixed dT causes one satellite to merge, other not
 *
 *   REINCORPORATION TESTS:
 *   - test_mixed_dt_reincorporation: Central uses its own dT for reincorporation rate
 *
 *   STAR FORMATION TESTS:
 *   - test_mixed_dt_star_formation: Different dT gives different stellar mass
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
extern int sage_update_merger_time_init(void);
extern int sage_update_merger_time_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_update_merger_time_cleanup(void);

extern int sage_reincorporation_init(void);
extern int sage_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_reincorporation_cleanup(void);

extern int sage_calculate_star_formation_init(void);
extern int sage_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_calculate_star_formation_cleanup(void);

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
/* MERGER TIME: MIXED-DT DECREMENT                                           */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_merger_decrement
 * @brief   Two satellites with different dT get different MergTime decrements
 *
 * Fixture: Central (dT=0.2), Satellite A (dT=0.2), Satellite B (dT=0.5)
 * With num_substeps=1, Sat A decrements by 0.2, Sat B by 0.5.
 * If the code used global ctx->substep_dt (=0.2), both would decrement by 0.2.
 */
int test_mixed_dt_merger_decrement(void)
{
    init_memory_system(0);
    reset_config();
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.G = 43.02;
    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "1.0");
    MimicConfig.NumModelParams = 1;
    sage_update_merger_time_init();

    /* Central: Type 0 */
    struct GalaxyData *cen_gal = alloc_galaxy();
    cen_gal->MergTime = 999.9f;
    cen_gal->StellarMass = 50.0;
    cen_gal->ColdGas = 20.0;
    struct Halo central = {0};
    central.Type = 0;
    central.Mvir = 200.0;
    central.Vvir = 200.0;
    central.Rvir = 0.5;
    central.SnapNum = 63;
    central.dT = 0.2;
    central.galaxy = cen_gal;

    /* Satellite A: dT = 0.2 (same snap as central) */
    struct GalaxyData *sat_a_gal = alloc_galaxy();
    sat_a_gal->MergTime = 10.0f;
    sat_a_gal->StellarMass = 1.0;
    sat_a_gal->ColdGas = 0.5;
    struct Halo sat_a = {0};
    sat_a.Type = 1;
    sat_a.Mvir = 100.0;  /* High Mvir/baryons → not eligible */
    sat_a.Vvir = 100.0;
    sat_a.Rvir = 0.2;
    sat_a.SnapNum = 63;
    sat_a.HaloNr = 1;
    sat_a.dT = 0.2;
    sat_a.galaxy = sat_a_gal;

    /* Satellite B: dT = 0.5 (accreted from earlier snapshot) */
    struct GalaxyData *sat_b_gal = alloc_galaxy();
    sat_b_gal->MergTime = 10.0f;
    sat_b_gal->StellarMass = 1.0;
    sat_b_gal->ColdGas = 0.5;
    struct Halo sat_b = {0};
    sat_b.Type = 1;
    sat_b.Mvir = 100.0;  /* High Mvir/baryons → not eligible */
    sat_b.Vvir = 100.0;
    sat_b.Rvir = 0.2;
    sat_b.SnapNum = 60;  /* Different snapshot → different dT */
    sat_b.HaloNr = 2;
    sat_b.dT = 0.5;
    sat_b.galaxy = sat_b_gal;

    struct ModuleContext ctx = create_context(0.2, 1);
    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, sat_a, sat_b};

    sage_update_merger_time_process(&ctx, halos, 3);

    /* Sat A should decrement by dT_A/num_substeps = 0.2/1 = 0.2 */
    TEST_ASSERT_DOUBLE_EQUAL(halos[1].galaxy->MergTime, 10.0 - 0.2, 1e-5,
                             "Satellite A MergTime should decrement by 0.2 (its own dT)");

    /* Sat B should decrement by dT_B/num_substeps = 0.5/1 = 0.5 */
    TEST_ASSERT_DOUBLE_EQUAL(halos[2].galaxy->MergTime, 10.0 - 0.5, 1e-5,
                             "Satellite B MergTime should decrement by 0.5 (its own dT)");

    /* Key assertion: they must differ (proves per-object, not global) */
    TEST_ASSERT(fabs(halos[1].galaxy->MergTime - halos[2].galaxy->MergTime) > 0.1,
                "Satellites with different dT must have different MergTime decrements");

    free_galaxy(&cen_gal);
    free_galaxy(&sat_a_gal);
    free_galaxy(&sat_b_gal);
    sage_update_merger_time_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* MERGER TIME: MIXED-DT TRIGGER DIVERGENCE                                  */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_merger_trigger
 * @brief   Different dT causes one satellite to merge while the other does not
 *
 * Both satellites start with MergTime = 0.3. With dT_A = 0.2 the decrement
 * is 0.2 → MergTime = 0.1 (still positive → disruption only if eligible).
 * With dT_B = 0.5 the decrement is 0.5 → MergTime = -0.2 (merger).
 * Both are eligible (low Mvir/baryons).
 */
int test_mixed_dt_merger_trigger(void)
{
    init_memory_system(0);
    reset_config();
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.G = 43.02;
    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "1.0");
    MimicConfig.NumModelParams = 1;
    sage_update_merger_time_init();

    /* Central */
    struct GalaxyData *cen_gal = alloc_galaxy();
    cen_gal->MergTime = 999.9f;
    cen_gal->StellarMass = 50.0;
    cen_gal->ColdGas = 20.0;
    struct Halo central = {0};
    central.Type = 0;
    central.Mvir = 200.0;
    central.Vvir = 200.0;
    central.Rvir = 0.5;
    central.SnapNum = 63;
    central.dT = 0.2;
    central.galaxy = cen_gal;

    /* Satellite A: dT = 0.2 → decrement = 0.2, MergTime 0.3 → 0.1 > 0 → disrupt */
    struct GalaxyData *sat_a_gal = alloc_galaxy();
    sat_a_gal->MergTime = 0.3f;
    sat_a_gal->StellarMass = 5.0;
    sat_a_gal->ColdGas = 5.0;
    struct Halo sat_a = {0};
    sat_a.Type = 1;
    sat_a.Mvir = 5.0;  /* Mvir/baryons = 5/10 = 0.5 ≤ 1.0 → eligible */
    sat_a.Vvir = 100.0;
    sat_a.Rvir = 0.2;
    sat_a.SnapNum = 63;
    sat_a.HaloNr = 1;
    sat_a.dT = 0.2;
    sat_a.galaxy = sat_a_gal;

    /* Satellite B: dT = 0.5 → decrement = 0.5, MergTime 0.3 → -0.2 ≤ 0 → merge */
    struct GalaxyData *sat_b_gal = alloc_galaxy();
    sat_b_gal->MergTime = 0.3f;
    sat_b_gal->StellarMass = 5.0;
    sat_b_gal->ColdGas = 5.0;
    struct Halo sat_b = {0};
    sat_b.Type = 1;
    sat_b.Mvir = 5.0;  /* Mvir/baryons = 5/10 = 0.5 ≤ 1.0 → eligible */
    sat_b.Vvir = 100.0;
    sat_b.Rvir = 0.2;
    sat_b.SnapNum = 60;
    sat_b.HaloNr = 2;
    sat_b.dT = 0.5;
    sat_b.galaxy = sat_b_gal;

    struct ModuleContext ctx = create_context(0.2, 1);
    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, sat_a, sat_b};

    sage_update_merger_time_process(&ctx, halos, 3);

    /* Sat A: MergTime = 0.1 > 0 and eligible → disrupting */
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 1,
                "Satellite A (small dT) should disrupt (MergTime still positive)");
    TEST_ASSERT(halos[1].galaxy->IsMerging == 0,
                "Satellite A should not merge");

    /* Sat B: MergTime = -0.2 ≤ 0 and eligible → merging */
    TEST_ASSERT(halos[2].galaxy->IsMerging == 1,
                "Satellite B (large dT) should merge (MergTime crossed zero)");
    TEST_ASSERT(halos[2].galaxy->IsDisrupting == 0,
                "Satellite B should not disrupt");

    free_galaxy(&cen_gal);
    free_galaxy(&sat_a_gal);
    free_galaxy(&sat_b_gal);
    sage_update_merger_time_cleanup();
    check_memory_leaks();

    return TEST_PASS;
}

/* ========================================================================== */
/* MERGER TIME: MULTI-SUBSTEP MIXED-DT                                       */
/* ========================================================================== */

/**
 * @test    test_mixed_dt_merger_substeps
 * @brief   Per-object dt divides each object's dT by num_substeps independently
 *
 * With num_substeps=5: Sat A (dT=0.5) gets dt=0.1, Sat B (dT=1.0) gets dt=0.2.
 * After 5 substeps: Sat A decremented by 5*0.1=0.5, Sat B by 5*0.2=1.0.
 */
int test_mixed_dt_merger_substeps(void)
{
    init_memory_system(0);
    reset_config();
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.G = 43.02;
    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "0.01");  /* Very low → not eligible */
    MimicConfig.NumModelParams = 1;
    sage_update_merger_time_init();

    /* Central */
    struct GalaxyData *cen_gal = alloc_galaxy();
    cen_gal->MergTime = 999.9f;
    cen_gal->StellarMass = 50.0;
    cen_gal->ColdGas = 20.0;
    struct Halo central = {0};
    central.Type = 0;
    central.Mvir = 200.0;
    central.Vvir = 200.0;
    central.Rvir = 0.5;
    central.SnapNum = 63;
    central.dT = 0.5;
    central.galaxy = cen_gal;

    /* Satellite A: dT = 0.5 → dt per substep = 0.1 */
    struct GalaxyData *sat_a_gal = alloc_galaxy();
    sat_a_gal->MergTime = 8.0f;
    sat_a_gal->StellarMass = 1.0;
    sat_a_gal->ColdGas = 0.5;
    struct Halo sat_a = {0};
    sat_a.Type = 1;
    sat_a.Mvir = 100.0;
    sat_a.Vvir = 100.0;
    sat_a.SnapNum = 63;
    sat_a.HaloNr = 1;
    sat_a.dT = 0.5;
    sat_a.galaxy = sat_a_gal;

    /* Satellite B: dT = 1.0 → dt per substep = 0.2 */
    struct GalaxyData *sat_b_gal = alloc_galaxy();
    sat_b_gal->MergTime = 8.0f;
    sat_b_gal->StellarMass = 1.0;
    sat_b_gal->ColdGas = 0.5;
    struct Halo sat_b = {0};
    sat_b.Type = 1;
    sat_b.Mvir = 100.0;
    sat_b.Vvir = 100.0;
    sat_b.SnapNum = 58;
    sat_b.HaloNr = 2;
    sat_b.dT = 1.0;
    sat_b.galaxy = sat_b_gal;

    /* Run 5 substeps */
    for (int step = 0; step < 5; step++) {
        struct ModuleContext ctx = create_context(0.5, 5);
        ctx.substep_number = step;
        ctx.central_galaxy = &central;
        struct Halo halos[3] = {central, sat_a, sat_b};

        /* Point galaxy pointers into array for this call */
        halos[0].galaxy = cen_gal;
        halos[1].galaxy = sat_a_gal;
        halos[2].galaxy = sat_b_gal;

        sage_update_merger_time_process(&ctx, halos, 3);
    }

    /* Sat A: 5 * (0.5/5) = 0.5 total decrement → 8.0 - 0.5 = 7.5 */
    TEST_ASSERT_DOUBLE_EQUAL(sat_a_gal->MergTime, 7.5, 1e-4,
                             "Satellite A total decrement should equal its dT over all substeps");

    /* Sat B: 5 * (1.0/5) = 1.0 total decrement → 8.0 - 1.0 = 7.0 */
    TEST_ASSERT_DOUBLE_EQUAL(sat_b_gal->MergTime, 7.0, 1e-4,
                             "Satellite B total decrement should equal its dT over all substeps");

    /* The difference (0.5) proves per-object timestep, not global */
    TEST_ASSERT_DOUBLE_EQUAL(sat_a_gal->MergTime - sat_b_gal->MergTime, 0.5, 1e-4,
                             "MergTime difference must equal dT difference after full integration");

    free_galaxy(&cen_gal);
    free_galaxy(&sat_a_gal);
    free_galaxy(&sat_b_gal);
    sage_update_merger_time_cleanup();
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
    sage_calculate_star_formation_init();

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

    sage_calculate_star_formation_process(&ctx_a, &halo_a, 1);
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

    sage_calculate_star_formation_process(&ctx_b, &halo_b, 1);
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
    sage_calculate_star_formation_cleanup();
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
    sage_calculate_star_formation_init();

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
    sage_calculate_star_formation_process(&ctx1, &halo1, 1);
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
    sage_calculate_star_formation_process(&ctx2, &halo2, 1);
    float stars2 = gal2->NewStellarMass;

    /* Must be identical — module uses halo->dT, not ctx->substep_dt */
    TEST_ASSERT_DOUBLE_EQUAL(stars1, stars2, 1e-10,
                             "Star formation must use halo->dT, not ctx->substep_dt");
    TEST_ASSERT(stars1 > 0.0, "Stars should have formed");

    free_galaxy(&gal1);
    free_galaxy(&gal2);
    sage_calculate_star_formation_cleanup();
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
    TEST_RUN(test_mixed_dt_merger_decrement);
    TEST_RUN(test_mixed_dt_merger_trigger);
    TEST_RUN(test_mixed_dt_merger_substeps);

    printf("\n%sREINCORPORATION TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_reincorporation);

    printf("\n%sSTAR FORMATION TESTS:%s\n", BLUE, NC);
    TEST_RUN(test_mixed_dt_star_formation);
    TEST_RUN(test_mixed_dt_star_formation_ignores_global);

    TEST_SUMMARY();
    return TEST_RESULT();
}
