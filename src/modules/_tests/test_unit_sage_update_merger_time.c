/**
 * @file    test_unit_sage_update_merger_time.c
 * @brief   Unit tests for sage_update_merger_time module physics
 *
 * Tests the merger time evolution physics in isolation using minimal mocks.
 * Validates:
 *   - MergTime decrement by substep dt
 *   - Eligibility conditions (zero baryons, Mvir/Mbaryons threshold)
 *   - Merger triggering (MergTime <= 0 AND eligible)
 *   - Disruption triggering (MergTime > 0 AND eligible)
 *   - MergerMassRatio calculation (min/max baryonic mass)
 *   - Mvir interpolation across substeps
 *   - Edge cases (NULL galaxy, no central, unset MergTime)
 *
 * Physics: Satellites merge or disrupt based on orbital decay and stripping.
 *   - Merger: Orbital decay complete (MergTime <= 0) and stripped enough
 *   - Disruption: Too stripped to survive (Mvir/Mbaryons <= threshold) before merger
 *
 * @author  Mimic Development Team
 * @date    2025-12-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

// Minimal includes for types (MUST come before extern declarations)
#include "../include/types.h"
#include "../core/module_interface.h"
#include "../include/globals.h"
#include "../util/memory.h"
#include "../util/error.h"

// Include module under test
extern int sage_update_merger_time_init(void);
extern int sage_update_merger_time_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_update_merger_time_cleanup(void);

/* Test statistics */
static int passed = 0;
static int failed = 0;

/* ANSI colors */
#define GREEN "\033[0;32m"
#define RED "\033[0;31m"
#define BLUE "\033[1;34m"
#define NC "\033[0m"

/* Test macros */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("%s✗ FAIL: %s%s\n", RED, message, NC); \
            printf("  at %s:%d\n", __FILE__, __LINE__); \
            failed++; \
            return 1; \
        } \
    } while(0)

#define TEST_PASS \
    do { \
        passed++; \
        return 0; \
    } while(0)

#define FLOAT_EQ(a, b, epsilon) (fabs((a) - (b)) < (epsilon))

/* Mock configuration */
static void setup_mock_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;
    MimicConfig.G = 43.02;

    /* Set ThresholdSatDisruption parameter */
    snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "ThresholdSatDisruption");
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "1.0");
    MimicConfig.NumModelParams = 1;
}

/* Helper: Create minimal module context */
static struct ModuleContext create_test_context(double dt, int substep_num, int num_substeps)
{
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.redshift = 0.0;
    ctx.time = 13.6;
    ctx.snapshot_number = 63;
    ctx.substep_number = substep_num;
    ctx.num_substeps = num_substeps;
    ctx.time_interval = dt * num_substeps;
    ctx.substep_dt = dt;
    ctx.params = &MimicConfig;

    return ctx;
}

/* Helper: Create test halo */
static struct Halo create_test_halo(int type, double mvir, double delta_mvir,
                                     double rvir, double vvir, struct GalaxyData *galaxy)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = type;
    halo.Mvir = mvir;
    halo.deltaMvir = delta_mvir;
    halo.Rvir = rvir;
    halo.Vvir = vvir;
    halo.SnapNum = 63;
    halo.HaloNr = 1;
    halo.galaxy = galaxy;

    return halo;
}

/* Helper: Create test galaxy with specified properties */
static struct GalaxyData create_test_galaxy(float mergtime, float stellar_mass,
                                             float cold_gas, int is_merging,
                                             int is_disrupting, float merger_ratio)
{
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));

    gal.MergTime = mergtime;
    gal.StellarMass = stellar_mass;
    gal.ColdGas = cold_gas;
    gal.IsMerging = is_merging;
    gal.IsDisrupting = is_disrupting;
    gal.MergerMassRatio = merger_ratio;

    return gal;
}

// ============================================================================
// TYPE FILTERING TESTS
// ============================================================================

/**
 * @test    test_type0_central_skipped
 * @brief   Type 0 centrals are not processed (only satellites)
 */
int test_type0_central_skipped(void)
{
    printf("  Testing: Type 0 central skipped...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Type 0 central with valid MergTime (should remain unchanged)
    struct GalaxyData cen_gal = create_test_galaxy(5.0f, 10.0, 5.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 150.0, &cen_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[1] = {central};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 1);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 5.0f, 0.01),
                "Type 0 central MergTime should remain unchanged");
    TEST_ASSERT(halos[0].galaxy->IsMerging == 0, "Central should not be merging");
    TEST_ASSERT(halos[0].galaxy->IsDisrupting == 0, "Central should not be disrupting");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_type1_satellite_processed
 * @brief   Type 1 satellites get MergTime decremented
 */
int test_type1_satellite_processed(void)
{
    printf("  Testing: Type 1 satellite MergTime decremented...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    double dt = 0.1;
    struct ModuleContext ctx = create_test_context(dt, 0, 1);

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Type 1 satellite with high Mvir (not eligible for merger/disruption)
    float initial_mergtime = 5.0f;
    struct GalaxyData sat_gal = create_test_galaxy(initial_mergtime, 5.0, 2.0, 0, 0, 0.0f);
    // Mvir = 100, baryons = 7, ratio = 100/7 ≈ 14.3 > threshold (1.0) → not eligible
    struct Halo satellite = create_test_halo(1, 100.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, initial_mergtime - dt, 0.001),
                "Type 1 satellite MergTime should be decremented by dt");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_type2_orphan_processed
 * @brief   Type 2 orphans get MergTime decremented
 */
int test_type2_orphan_processed(void)
{
    printf("  Testing: Type 2 orphan MergTime decremented...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    double dt = 0.1;
    struct ModuleContext ctx = create_test_context(dt, 0, 1);

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Type 2 orphan with high MergTime and high ratio (not eligible at threshold=1.0)
    float initial_mergtime = 10.0f;
    struct GalaxyData orphan_gal = create_test_galaxy(initial_mergtime, 3.0, 1.0, 0, 0, 0.0f);
    struct Halo orphan = create_test_halo(2, 5.0, 0.0, 0.1, 50.0, &orphan_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, orphan};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 0,
                "Type 2 orphan should not disrupt when ratio is above threshold");
    // MergTime was decremented before eligibility check
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, initial_mergtime - dt, 0.001),
                "Type 2 orphan MergTime should be decremented");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_type3_plus_skipped
 * @brief   Type > 2 halos are not processed
 */
int test_type3_plus_skipped(void)
{
    printf("  Testing: Type > 2 halos skipped...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Type 3 (should be skipped)
    struct GalaxyData type3_gal = create_test_galaxy(5.0f, 5.0, 2.0, 0, 0, 0.0f);
    struct Halo type3_halo = create_test_halo(3, 20.0, 0.0, 0.2, 100.0, &type3_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, type3_halo};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 5.0f, 0.01),
                "Type 3 halo MergTime should remain unchanged (skipped)");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// MERGTIME DECREMENT TESTS
// ============================================================================

/**
 * @test    test_mergtime_decrement
 * @brief   MergTime is decremented by exactly dt
 */
int test_mergtime_decrement(void)
{
    printf("  Testing: MergTime decrement by dt...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    double dt = 0.25;
    struct ModuleContext ctx = create_test_context(dt, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with high Mvir (not eligible)
    float initial_mergtime = 10.0f;
    struct GalaxyData sat_gal = create_test_galaxy(initial_mergtime, 1.0, 0.5, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 100.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    float expected_mergtime = initial_mergtime - dt;
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, expected_mergtime, 0.0001),
                "MergTime should decrease by exactly dt");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_mergtime_accumulates
 * @brief   Multiple calls accumulate MergTime decrements
 */
int test_mergtime_accumulates(void)
{
    printf("  Testing: MergTime decrements accumulate...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    double dt = 0.1;
    struct ModuleContext ctx = create_test_context(dt, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with high Mvir (not eligible)
    float initial_mergtime = 5.0f;
    struct GalaxyData sat_gal = create_test_galaxy(initial_mergtime, 1.0, 0.5, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 100.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute multiple substeps */
    for (int i = 0; i < 10; i++) {
        ctx.substep_number = i;
        sage_update_merger_time_process(&ctx, halos, 2);
    }

    /* Validate */
    float expected_mergtime = initial_mergtime - (10 * dt);
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, expected_mergtime, 0.001),
                "MergTime should accumulate decrements over multiple calls");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// ELIGIBILITY LOGIC TESTS
// ============================================================================

/**
 * @test    test_zero_baryons_eligible
 * @brief   Zero baryonic mass makes satellite eligible
 */
int test_zero_baryons_eligible(void)
{
    printf("  Testing: Zero baryons makes satellite eligible...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with zero baryons and positive MergTime → should disrupt
    struct GalaxyData sat_gal = create_test_galaxy(5.0f, 0.0, 0.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 50.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 1,
                "Zero baryons satellite should be disrupting (eligible, MergTime > 0)");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_type2_threshold_controlled
 * @brief   Type 2 orphan eligibility still uses Mvir/Mbaryons threshold
 */
int test_type2_threshold_controlled(void)
{
    printf("  Testing: Type 2 orphan threshold eligibility...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Type 2 orphan with HIGH Mvir/Mbaryons ratio (would be ineligible if Type 1)
    // Mvir = 1000, baryons = 10, ratio = 100 >> threshold
    struct GalaxyData orphan_gal = create_test_galaxy(5.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo orphan = create_test_halo(2, 1000.0, 0.0, 0.5, 300.0, &orphan_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, orphan};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 0,
                "Type 2 orphan above threshold should not be eligible");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_threshold_at_boundary
 * @brief   Mvir/Mbaryons exactly at threshold is eligible
 */
int test_threshold_at_boundary(void)
{
    printf("  Testing: Threshold boundary (Mvir/Mbaryons <= threshold)...\n");

    /* Setup - threshold is 1.0 */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with Mvir/Mbaryons = 1.0 (exactly at threshold)
    // Mvir = 10, baryons = 10, ratio = 1.0 <= threshold (1.0) → eligible
    struct GalaxyData sat_gal = create_test_galaxy(5.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 10.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 1,
                "Satellite at threshold boundary should be eligible and disrupting");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_above_threshold_not_eligible
 * @brief   Mvir/Mbaryons > threshold is not eligible
 */
int test_above_threshold_not_eligible(void)
{
    printf("  Testing: Above threshold not eligible...\n");

    /* Setup - threshold is 1.0 */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with Mvir/Mbaryons > threshold
    // Mvir = 20, baryons = 10, ratio = 2.0 > threshold (1.0) → NOT eligible
    struct GalaxyData sat_gal = create_test_galaxy(5.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 20.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].galaxy->IsMerging == 0, "Satellite above threshold should not merge");
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 0, "Satellite above threshold should not disrupt");
    // MergTime still decremented
    TEST_ASSERT(halos[1].galaxy->MergTime < 5.0f, "MergTime should still decrement");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// MERGER TRIGGERING TESTS
// ============================================================================

/**
 * @test    test_merger_triggered
 * @brief   Merger triggered when MergTime <= 0 AND eligible
 */
int test_merger_triggered(void)
{
    printf("  Testing: Merger triggered when MergTime <= 0 AND eligible...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.2, 0, 1);  // dt = 0.2

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with MergTime = 0.1 (will become <= 0 after dt decrement)
    // Also eligible: Mvir/Mbaryons = 5/10 = 0.5 <= 1.0
    struct GalaxyData sat_gal = create_test_galaxy(0.1f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 5.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].galaxy->IsMerging == 1,
                "Satellite should be merging (MergTime <= 0 and eligible)");
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 0,
                "Satellite should not be disrupting when merging");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_merger_mass_ratio_calculation
 * @brief   MergerMassRatio is calculated as min/max of baryonic masses
 */
int test_merger_mass_ratio_calculation(void)
{
    printf("  Testing: MergerMassRatio calculation (min/max)...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.2, 0, 1);

    // Central with baryons = 50 + 20 = 70
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with baryons = 5 + 2 = 7 (smaller than central)
    // MergTime will go negative → merger
    struct GalaxyData sat_gal = create_test_galaxy(0.1f, 5.0, 2.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 0.0, 0.0, 0.2, 100.0, &sat_gal);  // Low Mvir → eligible

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    // mi = 7 (satellite), ma = 70 (central), ratio = 7/70 = 0.1
    float expected_ratio = 7.0f / 70.0f;
    TEST_ASSERT(halos[1].galaxy->IsMerging == 1, "Satellite should be merging");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergerMassRatio, expected_ratio, 0.01),
                "MergerMassRatio should be satellite_baryons / central_baryons");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_mass_ratio_satellite_larger
 * @brief   MergerMassRatio correct when satellite is more massive
 */
int test_mass_ratio_satellite_larger(void)
{
    printf("  Testing: MergerMassRatio when satellite larger...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.2, 0, 1);

    // Central with baryons = 5 + 2 = 7 (smaller)
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 5.0, 2.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with baryons = 50 + 20 = 70 (larger than central)
    struct GalaxyData sat_gal = create_test_galaxy(0.1f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 0.0, 0.0, 0.2, 100.0, &sat_gal);  // Low Mvir → eligible

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    // mi = 7 (central), ma = 70 (satellite), ratio = 7/70 = 0.1
    // Still min/max regardless of which is satellite
    float expected_ratio = 7.0f / 70.0f;
    TEST_ASSERT(halos[1].galaxy->IsMerging == 1, "Satellite should be merging");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergerMassRatio, expected_ratio, 0.01),
                "MergerMassRatio should be min/max regardless of which is larger");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// DISRUPTION TRIGGERING TESTS
// ============================================================================

/**
 * @test    test_disruption_triggered
 * @brief   Disruption triggered when MergTime > 0 AND eligible
 */
int test_disruption_triggered(void)
{
    printf("  Testing: Disruption triggered when MergTime > 0 AND eligible...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite eligible (Mvir/Mbaryons = 5/10 = 0.5 <= 1.0) but MergTime still positive
    struct GalaxyData sat_gal = create_test_galaxy(5.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 5.0, 0.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 1,
                "Satellite should be disrupting (MergTime > 0 and eligible)");
    TEST_ASSERT(halos[1].galaxy->IsMerging == 0,
                "Satellite should not be merging when disrupting");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_no_double_trigger
 * @brief   Cannot be both merging and disrupting
 */
int test_no_double_trigger(void)
{
    printf("  Testing: No double trigger (merging XOR disrupting)...\n");

    /* Setup for merger case */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.2, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Merger case
    struct GalaxyData sat_gal1 = create_test_galaxy(0.1f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite1 = create_test_halo(1, 5.0, 0.0, 0.2, 100.0, &sat_gal1);

    // Disruption case
    struct GalaxyData sat_gal2 = create_test_galaxy(5.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite2 = create_test_halo(1, 5.0, 0.0, 0.2, 100.0, &sat_gal2);

    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, satellite1, satellite2};

    /* Execute */
    sage_update_merger_time_process(&ctx, halos, 3);

    /* Validate */
    // Merger case: exactly one flag
    int merger_flags = halos[1].galaxy->IsMerging + halos[1].galaxy->IsDisrupting;
    TEST_ASSERT(merger_flags == 1, "Merging satellite should have exactly one flag set");
    TEST_ASSERT(halos[1].galaxy->IsMerging == 1, "Should be merging not disrupting");

    // Disruption case: exactly one flag
    int disrupt_flags = halos[2].galaxy->IsMerging + halos[2].galaxy->IsDisrupting;
    TEST_ASSERT(disrupt_flags == 1, "Disrupting satellite should have exactly one flag set");
    TEST_ASSERT(halos[2].galaxy->IsDisrupting == 1, "Should be disrupting not merging");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// MVIR INTERPOLATION TESTS
// ============================================================================

/**
 * @test    test_mvir_interpolation
 * @brief   Current Mvir is interpolated based on substep progress
 */
int test_mvir_interpolation(void)
{
    printf("  Testing: Mvir interpolation across substeps...\n");

    /* Setup - test at different substeps */
    init_memory_system(0);
    sage_update_merger_time_init();

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with deltaMvir = 10 (Mvir decreasing from 20 to 10)
    // At substep 0/10: fraction = 0.1, currentMvir = 20 - 10*(1-0.1) = 11
    // At substep 9/10: fraction = 1.0, currentMvir = 20 - 10*(1-1.0) = 20
    struct GalaxyData sat_gal = create_test_galaxy(10.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 20.0, 10.0, 0.2, 100.0, &sat_gal);
    // baryons = 10, at substep 0: Mvir/baryons = 11/10 = 1.1 > 1.0 → not eligible
    // At substep 9: Mvir/baryons = 20/10 = 2.0 > 1.0 → not eligible

    /* Test at substep 0 (start) */
    struct ModuleContext ctx = create_test_context(0.1, 0, 10);
    ctx.central_galaxy = &central;
    struct Halo halos1[2] = {central, satellite};
    sage_update_merger_time_process(&ctx, halos1, 2);

    // At substep 0: fraction = 0.1, currentMvir = 20 - 10*0.9 = 11
    // ratio = 11/10 = 1.1 > 1.0 → not eligible
    TEST_ASSERT(halos1[1].galaxy->IsDisrupting == 0,
                "At substep 0, Mvir interpolation should give ratio > threshold");

    /* Reset and test at high threshold */
    // Now set threshold higher so ratio 1.1 is eligible
    snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "1.5");
    sage_update_merger_time_init();

    struct GalaxyData sat_gal2 = create_test_galaxy(10.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite2 = create_test_halo(1, 20.0, 10.0, 0.2, 100.0, &sat_gal2);
    struct Halo halos2[2] = {central, satellite2};
    sage_update_merger_time_process(&ctx, halos2, 2);

    // With threshold 1.5, ratio 1.1 <= 1.5 → eligible
    TEST_ASSERT(halos2[1].galaxy->IsDisrupting == 1,
                "With higher threshold, interpolated Mvir should make satellite eligible");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_mvir_clamped_at_zero
 * @brief   Negative Mvir is clamped to 0
 */
int test_mvir_clamped_at_zero(void)
{
    printf("  Testing: Negative Mvir clamped to zero...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with large deltaMvir that would make currentMvir negative
    // Mvir = 5, deltaMvir = 20 → at fraction 1.0: currentMvir = 5 - 20*0 = 5
    // But at fraction 0.1: currentMvir = 5 - 20*0.9 = 5 - 18 = -13 → should clamp to 0
    struct GalaxyData sat_gal = create_test_galaxy(10.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite = create_test_halo(1, 5.0, 20.0, 0.2, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    ctx.substep_number = 0;
    ctx.num_substeps = 10;
    struct Halo halos[2] = {central, satellite};

    /* Execute - should not crash even with negative interpolated Mvir */
    int result = sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed even with negative Mvir interpolation");
    // With currentMvir clamped to 0, ratio = 0/10 = 0 <= threshold → eligible
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 1,
                "Clamped zero Mvir should make satellite eligible for disruption");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_null_halos
 * @brief   NULL halos handled gracefully
 */
int test_null_halos(void)
{
    printf("  Testing: NULL halos handled...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, NULL, 0);

    /* Validate */
    TEST_ASSERT(result == 0, "Should handle NULL halos gracefully");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_zero_ngal
 * @brief   ngal=0 handled gracefully
 */
int test_zero_ngal(void)
{
    printf("  Testing: ngal=0 handled...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);
    struct Halo dummy;

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, &dummy, 0);

    /* Validate */
    TEST_ASSERT(result == 0, "Should handle ngal=0 gracefully");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_no_central
 * @brief   No Type 0 central returns early
 */
int test_no_central(void)
{
    printf("  Testing: No central returns early...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Only satellites, no central
    struct GalaxyData sat_gal1 = create_test_galaxy(5.0f, 5.0, 2.0, 0, 0, 0.0f);
    struct Halo satellite1 = create_test_halo(1, 20.0, 0.0, 0.2, 100.0, &sat_gal1);

    struct GalaxyData sat_gal2 = create_test_galaxy(5.0f, 3.0, 1.0, 0, 0, 0.0f);
    struct Halo satellite2 = create_test_halo(2, 10.0, 0.0, 0.1, 80.0, &sat_gal2);

    ctx.central_galaxy = &satellite1;  // No actual central
    struct Halo halos[2] = {satellite1, satellite2};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Should return 0 when no central found");
    // Neither should be processed (no central)
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 5.0f, 0.01),
                "Satellite 1 MergTime should remain unchanged (no central)");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 5.0f, 0.01),
                "Satellite 2 MergTime should remain unchanged (no central)");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_null_galaxy_skipped
 * @brief   Halos with NULL galaxy pointer are skipped
 */
int test_null_galaxy_skipped(void)
{
    printf("  Testing: NULL galaxy pointer skipped...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central with galaxy
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite with NULL galaxy
    struct Halo satellite = create_test_halo(1, 20.0, 0.0, 0.2, 100.0, NULL);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Multiple satellites processed independently
 */
int test_multiple_satellites(void)
{
    printf("  Testing: Multiple satellites processed...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0, 0, 0, 0.0f);
    struct Halo central = create_test_halo(0, 100.0, 0.0, 0.5, 200.0, &cen_gal);

    // Satellite 1: Not eligible (high Mvir ratio)
    struct GalaxyData sat_gal1 = create_test_galaxy(5.0f, 1.0, 0.0, 0, 0, 0.0f);
    struct Halo satellite1 = create_test_halo(1, 100.0, 0.0, 0.2, 100.0, &sat_gal1);

    // Satellite 2: Eligible and will disrupt
    struct GalaxyData sat_gal2 = create_test_galaxy(5.0f, 5.0, 5.0, 0, 0, 0.0f);
    struct Halo satellite2 = create_test_halo(1, 5.0, 0.0, 0.1, 80.0, &sat_gal2);

    // Satellite 3: Will merge (MergTime goes negative)
    struct GalaxyData sat_gal3 = create_test_galaxy(0.05f, 3.0, 2.0, 0, 0, 0.0f);
    struct Halo satellite3 = create_test_halo(1, 2.0, 0.0, 0.1, 60.0, &sat_gal3);

    ctx.central_galaxy = &central;
    struct Halo halos[4] = {central, satellite1, satellite2, satellite3};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 4);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");

    // Satellite 1: Not eligible, just decremented
    TEST_ASSERT(halos[1].galaxy->IsMerging == 0, "Sat 1 should not merge");
    TEST_ASSERT(halos[1].galaxy->IsDisrupting == 0, "Sat 1 should not disrupt");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 4.9f, 0.01), "Sat 1 MergTime decremented");

    // Satellite 2: Eligible, disrupting
    TEST_ASSERT(halos[2].galaxy->IsDisrupting == 1, "Sat 2 should disrupt");

    // Satellite 3: Eligible, merging
    TEST_ASSERT(halos[3].galaxy->IsMerging == 1, "Sat 3 should merge");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

/**
 * @test    test_type2_merger_ratio_uses_centralhalo
 * @brief   Type 2 merger ratio uses CentralHalo target, not FOF Type 0
 */
int test_type2_merger_ratio_uses_centralhalo(void)
{
    printf("  Testing: Type 2 merger ratio uses CentralHalo target...\n");

    /* Setup */
    init_memory_system(0);
    sage_update_merger_time_init();
    struct ModuleContext ctx = create_test_context(0.1, 0, 1);

    /* FOF Type 0 central: very massive baryons (would give tiny ratio if used) */
    struct GalaxyData fof_central_gal = create_test_galaxy(999.9f, 80.0, 20.0, 0, 0, 0.0f);
    struct Halo fof_central = create_test_halo(0, 300.0, 0.0, 0.8, 220.0, &fof_central_gal);

    /* Type 1 subhalo central: modest baryons (intended target for Type 2) */
    struct GalaxyData subhalo_central_gal = create_test_galaxy(5.0f, 0.5, 0.5, 0, 0, 0.0f);
    struct Halo subhalo_central = create_test_halo(1, 100.0, 0.0, 0.3, 140.0, &subhalo_central_gal);

    /* Type 2 orphan set to merge this substep and linked to Type 1 central */
    struct GalaxyData orphan_gal = create_test_galaxy(0.05f, 1.0, 1.0, 0, 0, 0.0f);
    struct Halo orphan = create_test_halo(2, 0.1, 0.0, 0.1, 60.0, &orphan_gal);
    orphan.CentralHalo = 1;

    ctx.central_galaxy = &fof_central;
    struct Halo halos[3] = {fof_central, subhalo_central, orphan};

    /* Execute */
    int result = sage_update_merger_time_process(&ctx, halos, 3);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[2].galaxy->IsMerging == 1, "Type 2 orphan should merge");
    TEST_ASSERT(FLOAT_EQ(halos[2].galaxy->MergerMassRatio, 0.5f, 1e-3),
                "MergerMassRatio should use Type 1 CentralHalo target");

    sage_update_merger_time_cleanup();
    TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 */
int main(void)
{
    /* Suppress INFO/DEBUG messages during tests - only show warnings and errors */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Unit Test Suite: sage_update_merger_time Physics\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize mock configuration */
    setup_mock_config();

    /* Run type filtering tests */
    test_type0_central_skipped();
    test_type1_satellite_processed();
    test_type2_orphan_processed();
    test_type3_plus_skipped();

    /* Run MergTime decrement tests */
    test_mergtime_decrement();
    test_mergtime_accumulates();

    /* Run eligibility logic tests */
    setup_mock_config();  // Reset config
    test_zero_baryons_eligible();
    setup_mock_config();
    test_type2_threshold_controlled();
    setup_mock_config();
    test_threshold_at_boundary();
    setup_mock_config();
    test_above_threshold_not_eligible();

    /* Run merger triggering tests */
    setup_mock_config();
    test_merger_triggered();
    setup_mock_config();
    test_merger_mass_ratio_calculation();
    setup_mock_config();
    test_mass_ratio_satellite_larger();
    setup_mock_config();
    test_type2_merger_ratio_uses_centralhalo();

    /* Run disruption triggering tests */
    setup_mock_config();
    test_disruption_triggered();
    setup_mock_config();
    test_no_double_trigger();

    /* Run Mvir interpolation tests */
    setup_mock_config();
    test_mvir_interpolation();
    setup_mock_config();
    test_mvir_clamped_at_zero();

    /* Run edge case tests */
    setup_mock_config();
    test_null_halos();
    setup_mock_config();
    test_zero_ngal();
    setup_mock_config();
    test_no_central();
    setup_mock_config();
    test_null_galaxy_skipped();
    setup_mock_config();
    test_multiple_satellites();

    /* Print summary */
    printf("\n%s", BLUE);
    printf("============================================================\n");
    printf("Test Summary\n");
    printf("============================================================\n");
    printf("%s", NC);
    printf("Passed: %s%d%s\n", GREEN, passed, NC);
    printf("Failed: %s%d%s\n", failed > 0 ? RED : NC, failed, NC);
    printf("Total:  %d\n", passed + failed);
    printf("%s============================================================%s\n\n", BLUE, NC);

    if (failed == 0) {
        printf("%s✓ All tests passed!%s\n\n", GREEN, NC);
        return 0;
    } else {
        printf("%s✗ %d test(s) failed%s\n\n", RED, failed, NC);
        return 1;
    }
}
