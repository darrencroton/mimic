/**
 * @file    test_unit_sage_calculate_merger_timescale.c
 * @brief   Unit tests for sage_calculate_merger_timescale module physics
 *
 * Tests the merger timescale calculation physics in isolation using minimal mocks.
 * Validates:
 *   - Dynamical friction formula correctness
 *   - Coulomb logarithm calculation
 *   - Type filtering (centrals vs satellites)
 *   - MergTime reset for Type 0 centrals
 *   - Sentinel value handling (999.9 = unset)
 *   - Edge cases (zero mass, NULL galaxy, no central)
 *   - MergTime capping at 998.0
 *
 * Physics: Binney & Tremaine (1987) dynamical friction
 *   t_merge = 2 * 1.17 * R_vir^2 * V_vir / (ln(N_cen/N_sat) * G * M_sat)
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

// Include module under test
extern int sage_calculate_merger_timescale_init(void);
extern int sage_calculate_merger_timescale_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_calculate_merger_timescale_cleanup(void);

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
    MimicConfig.G = 43.02;  // Gravitational constant in internal units (km/s)^2 Mpc / (1e10 Msun)
}

/* Helper: Create minimal module context */
static struct ModuleContext create_test_context(void)
{
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.redshift = 0.0;
    ctx.time = 13.6;
    ctx.snapshot_number = 63;
    ctx.substep_number = 0;
    ctx.num_substeps = 1;
    ctx.time_interval = 0.1;
    ctx.substep_dt = ctx.time_interval;
    ctx.params = &MimicConfig;

    return ctx;
}

/* Helper: Create test halo */
static struct Halo create_test_halo(int type, double mvir, double rvir, double vvir,
                                     int len, double infall_mvir, struct GalaxyData *galaxy)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = type;
    halo.Mvir = mvir;
    halo.Rvir = rvir;
    halo.Vvir = vvir;
    halo.Len = len;
    halo.infallMvir = infall_mvir;
    halo.SnapNum = 63;
    halo.HaloNr = 1;
    halo.galaxy = galaxy;

    return halo;
}

/* Helper: Create test galaxy with MergTime and optional baryons */
static struct GalaxyData create_test_galaxy(float mergtime, float stellar_mass, float cold_gas)
{
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));

    gal.MergTime = mergtime;
    gal.StellarMass = stellar_mass;
    gal.ColdGas = cold_gas;

    return gal;
}

// ============================================================================
// TYPE FILTERING TESTS
// ============================================================================

/**
 * @test    test_type0_central_skipped
 * @brief   Type 0 centrals don't get MergTime calculated (only reset)
 */
int test_type0_central_skipped(void)
{
    printf("  Testing: Type 0 central skipped for calculation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Type 0 central with sentinel MergTime (should remain unchanged)
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 10.0, 5.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 150.0, 1000, 0.0, &cen_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[1] = {central};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 1);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
                "Type 0 central MergTime should remain at sentinel value");

    TEST_PASS;
}

/**
 * @test    test_type0_mergtime_reset
 * @brief   Type 0 centrals with MergTime < 999.0 get reset to 999.9
 */
int test_type0_mergtime_reset(void)
{
    printf("  Testing: Type 0 central MergTime reset...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Type 0 central that was previously a satellite (has old MergTime)
    struct GalaxyData cen_gal = create_test_galaxy(5.0f, 10.0, 5.0);  // Was satellite
    struct Halo central = create_test_halo(0, 100.0, 0.5, 150.0, 1000, 0.0, &cen_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[1] = {central};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 1);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
                "Type 0 central should have MergTime reset to 999.9");

    TEST_PASS;
}

/**
 * @test    test_type1_satellite_calculation
 * @brief   Type 1 satellite gets MergTime calculated
 */
int test_type1_satellite_calculation(void)
{
    printf("  Testing: Type 1 satellite MergTime calculation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Type 1 satellite just infalled (infallMvir > 0, MergTime = sentinel)
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, 5.0, 2.0);
    struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].galaxy->MergTime < 999.0f,
                "Type 1 satellite should have MergTime calculated");
    TEST_ASSERT(halos[1].galaxy->MergTime > 0.0f,
                "MergTime should be positive");

    TEST_PASS;
}

/**
 * @test    test_type2_orphan_calculation
 * @brief   Type 2 orphans also get MergTime calculated
 */
int test_type2_orphan_calculation(void)
{
    printf("  Testing: Type 2 orphan MergTime calculation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Type 2 orphan just became orphan (infallMvir > 0, MergTime = sentinel)
    struct GalaxyData orphan_gal = create_test_galaxy(999.9f, 3.0, 1.0);
    // Type 2 orphans have Len=0 (unresolved), should use floor of MinNumPartSatHalo=10
    struct Halo orphan = create_test_halo(2, 5.0, 0.1, 50.0, 0, 10.0, &orphan_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, orphan};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].galaxy->MergTime < 999.0f,
                "Type 2 orphan should have MergTime calculated");
    TEST_ASSERT(halos[1].galaxy->MergTime > 0.0f,
                "MergTime should be positive");

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
    struct ModuleContext ctx = create_test_context();

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Type 3 (should be skipped)
    struct GalaxyData type3_gal = create_test_galaxy(999.9f, 5.0, 2.0);
    struct Halo type3_halo = create_test_halo(3, 20.0, 0.2, 100.0, 200, 25.0, &type3_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, type3_halo};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 999.9f, 0.01),
                "Type 3 halo should remain at sentinel value (skipped)");

    TEST_PASS;
}

// ============================================================================
// SENTINEL AND CONDITION TESTS
// ============================================================================

/**
 * @test    test_already_calculated_skipped
 * @brief   Satellites with MergTime <= 999.0 are skipped (already calculated)
 */
int test_already_calculated_skipped(void)
{
    printf("  Testing: Already calculated satellites skipped...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Satellite with already-calculated MergTime (< 999.0)
    struct GalaxyData sat_gal = create_test_galaxy(5.5f, 5.0, 2.0);  // Already set
    struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 5.5f, 0.01),
                "Already-calculated satellite should keep original MergTime");

    TEST_PASS;
}

/**
 * @test    test_no_infall_mvir_skipped
 * @brief   Satellites with infallMvir <= 0 are skipped (not yet infalled)
 */
int test_no_infall_mvir_skipped(void)
{
    printf("  Testing: No infallMvir satellites skipped...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central halo
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Satellite with infallMvir = 0 (hasn't infalled yet)
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, 5.0, 2.0);
    struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 0.0, &sat_gal);  // infallMvir=0

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 999.9f, 0.01),
                "Satellite with infallMvir=0 should remain at sentinel");

    TEST_PASS;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_dynamical_friction_formula
 * @brief   Verify the Binney & Tremaine dynamical friction formula
 *
 * Physics: t_merge = 2 * 1.17 * R_vir^2 * V_vir / (ln(1 + N_cen/N_sat) * G * M_sat)
 */
int test_dynamical_friction_formula(void)
{
    printf("  Testing: Dynamical friction formula...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central halo with known properties
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
    // Central: Rvir=0.5 Mpc, Vvir=200 km/s, Len=1000 particles

    // Satellite with known properties (zero baryons for simple calculation)
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal);
    // Satellite: Mvir=10 (1e10 Msun/h), Len=100 particles

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Calculate expected value */
    // SatelliteMass = Mvir + StellarMass + ColdGas = 10.0 + 0.0 + 0.0 = 10.0
    // SatelliteRadius = central->Rvir = 0.5
    // Vvir = central->Vvir = 200.0
    // coulomb = log1p(1000.0 / 100.0) = log(11) ≈ 2.398
    // G = 43.02
    // mergtime = 2.0 * 1.17 * 0.5^2 * 200.0 / (2.398 * 43.02 * 10.0)
    //          = 2.0 * 1.17 * 0.25 * 200.0 / (1031.46)
    //          = 117.0 / 1031.46
    //          ≈ 0.1134 Gyr
    double coulomb = log1p(1000.0 / 100.0);
    double expected = 2.0 * 1.17 * 0.25 * 200.0 / (coulomb * 43.02 * 10.0);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, expected, 0.01),
                "MergTime should match dynamical friction formula");

    TEST_PASS;
}

/**
 * @test    test_coulomb_logarithm
 * @brief   Verify Coulomb logarithm uses log1p(N_cen/N_sat)
 */
int test_coulomb_logarithm(void)
{
    printf("  Testing: Coulomb logarithm calculation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central with 10000 particles
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 10000, 0.0, &cen_gal);

    // Satellite with 100 particles
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_calculate_merger_timescale_process(&ctx, halos, 2);
    float mergtime1 = halos[1].galaxy->MergTime;

    /* Now change particle ratio and re-run */
    halos[0].Len = 1000;  // Fewer central particles
    halos[1].galaxy->MergTime = 999.9f;  // Reset to sentinel

    sage_calculate_merger_timescale_process(&ctx, halos, 2);
    float mergtime2 = halos[1].galaxy->MergTime;

    /* Validate */
    // More particles in central → larger Coulomb log → smaller MergTime
    // 10000/100 = 100 → log(101) ≈ 4.62
    // 1000/100 = 10 → log(11) ≈ 2.40
    // So mergtime2 should be larger (smaller Coulomb log in denominator)
    TEST_ASSERT(mergtime2 > mergtime1,
                "Smaller particle ratio should give longer merger time");

    TEST_PASS;
}

/**
 * @test    test_small_satellite_floor
 * @brief   Satellites with Len < 10 use floor of MinNumPartSatHalo=10
 */
int test_small_satellite_floor(void)
{
    printf("  Testing: Small satellite particle floor...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Satellite with 0 particles (Type 2 orphan case)
    struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite1 = create_test_halo(1, 10.0, 0.1, 100.0, 0, 15.0, &sat_gal1);

    // Satellite with 10 particles (at floor)
    struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite2 = create_test_halo(1, 10.0, 0.1, 100.0, 10, 15.0, &sat_gal2);

    ctx.central_galaxy = &central;

    /* Execute for Len=0 satellite */
    struct Halo halos1[2] = {central, satellite1};
    sage_calculate_merger_timescale_process(&ctx, halos1, 2);
    float mergtime_len0 = halos1[1].galaxy->MergTime;

    /* Execute for Len=10 satellite */
    struct Halo halos2[2] = {central, satellite2};
    sage_calculate_merger_timescale_process(&ctx, halos2, 2);
    float mergtime_len10 = halos2[1].galaxy->MergTime;

    /* Validate */
    // Both should use floor of 10, so MergTime should be identical
    TEST_ASSERT(FLOAT_EQ(mergtime_len0, mergtime_len10, 0.01),
                "Len=0 and Len=10 should give same MergTime (both use floor=10)");

    TEST_PASS;
}

/**
 * @test    test_baryonic_mass_included
 * @brief   StellarMass and ColdGas are included in satellite mass
 */
int test_baryonic_mass_included(void)
{
    printf("  Testing: Baryonic mass included in satellite mass...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Satellite with NO baryons
    struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite1 = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal1);

    // Satellite with baryons (StellarMass=5, ColdGas=5)
    struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 5.0, 5.0);
    struct Halo satellite2 = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal2);

    ctx.central_galaxy = &central;

    /* Execute for satellite without baryons */
    struct Halo halos1[2] = {central, satellite1};
    sage_calculate_merger_timescale_process(&ctx, halos1, 2);
    float mergtime_no_baryons = halos1[1].galaxy->MergTime;

    /* Execute for satellite with baryons */
    struct Halo halos2[2] = {central, satellite2};
    sage_calculate_merger_timescale_process(&ctx, halos2, 2);
    float mergtime_with_baryons = halos2[1].galaxy->MergTime;

    /* Validate */
    // More mass → shorter merger time (mass in denominator)
    // No baryons: M_sat = 10.0
    // With baryons: M_sat = 10.0 + 5.0 + 5.0 = 20.0 (twice as massive)
    // So mergtime_with_baryons should be ~half
    TEST_ASSERT(mergtime_with_baryons < mergtime_no_baryons,
                "More massive satellite should merge faster");
    TEST_ASSERT(FLOAT_EQ(mergtime_no_baryons / mergtime_with_baryons, 2.0, 0.1),
                "Twice the mass should give half the merger time");

    TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_mergtime_cap
 * @brief   MergTime >= 999.0 is capped to 998.0
 */
int test_mergtime_cap(void)
{
    printf("  Testing: MergTime cap at 998.0...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central with huge virial radius (will give very long merger time)
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 100.0, 200.0, 1000, 0.0, &cen_gal);
    // Rvir = 100 Mpc (unrealistically large to force cap)

    // Tiny satellite
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 0.001, 0.001, 10.0, 10, 0.002, &sat_gal);
    // Very small mass → very long merger time

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].galaxy->MergTime <= 998.0f,
                "MergTime should be capped at 998.0");

    TEST_PASS;
}

/**
 * @test    test_zero_mass_handling
 * @brief   Zero satellite mass returns -1.0
 */
int test_zero_mass_handling(void)
{
    printf("  Testing: Zero mass handling...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Satellite with zero mass everywhere
    struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 0.0, 0.1, 100.0, 100, 0.001, &sat_gal);
    // Mvir=0, StellarMass=0, ColdGas=0 → total mass = 0

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].galaxy->MergTime < 0.0f,
                "Zero mass should give MergTime = -1.0 (invalid)");

    TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   NULL galaxy pointers are handled safely
 */
int test_null_galaxy_handling(void)
{
    printf("  Testing: NULL galaxy handling...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central with galaxy
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Satellite with NULL galaxy
    struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, NULL);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");

    TEST_PASS;
}

/**
 * @test    test_no_central_returns_early
 * @brief   No Type 0 central returns 0 (early exit)
 */
int test_no_central_returns_early(void)
{
    printf("  Testing: No central returns early...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Only satellites, no central
    struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 5.0, 2.0);
    struct Halo satellite1 = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal1);

    struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 3.0, 1.0);
    struct Halo satellite2 = create_test_halo(2, 10.0, 0.1, 80.0, 100, 15.0, &sat_gal2);

    ctx.central_galaxy = &satellite1;  // No actual central
    struct Halo halos[2] = {satellite1, satellite2};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Should return 0 when no central found");
    // Neither satellite should have MergTime calculated (no central for reference)
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
                "Satellite 1 should remain at sentinel (no central)");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 999.9f, 0.01),
                "Satellite 2 should remain at sentinel (no central)");

    TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Multiple satellites are processed independently
 */
int test_multiple_satellites(void)
{
    printf("  Testing: Multiple satellites processed...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    // Central
    struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);

    // Two satellites with different masses
    struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite1 = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal1);

    struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 0.0, 0.0);
    struct Halo satellite2 = create_test_halo(1, 5.0, 0.1, 50.0, 50, 8.0, &sat_gal2);

    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, satellite1, satellite2};

    /* Execute */
    int result = sage_calculate_merger_timescale_process(&ctx, halos, 3);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].galaxy->MergTime < 999.0f,
                "Satellite 1 should have MergTime calculated");
    TEST_ASSERT(halos[2].galaxy->MergTime < 999.0f,
                "Satellite 2 should have MergTime calculated");
    // Less massive satellite should take longer to merge
    TEST_ASSERT(halos[2].galaxy->MergTime > halos[1].galaxy->MergTime,
                "Less massive satellite should have longer merger time");

    TEST_PASS;
}

/**
 * @test    test_empty_halos_array
 * @brief   Empty halos array handled gracefully
 */
int test_empty_halos_array(void)
{
    printf("  Testing: Empty halos array...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context();

    /* Execute with NULL and ngal=0 */
    int result1 = sage_calculate_merger_timescale_process(&ctx, NULL, 0);

    /* Validate */
    TEST_ASSERT(result1 == 0, "Should handle NULL halos gracefully");

    /* Execute with valid pointer but ngal=0 */
    struct Halo dummy;
    int result2 = sage_calculate_merger_timescale_process(&ctx, &dummy, 0);
    TEST_ASSERT(result2 == 0, "Should handle ngal=0 gracefully");

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
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Unit Test Suite: sage_calculate_merger_timescale Physics\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize mock configuration */
    setup_mock_config();

    /* Run type filtering tests */
    test_type0_central_skipped();
    test_type0_mergtime_reset();
    test_type1_satellite_calculation();
    test_type2_orphan_calculation();
    test_type3_plus_skipped();

    /* Run sentinel and condition tests */
    test_already_calculated_skipped();
    test_no_infall_mvir_skipped();

    /* Run physics calculation tests */
    test_dynamical_friction_formula();
    test_coulomb_logarithm();
    test_small_satellite_floor();
    test_baryonic_mass_included();

    /* Run edge case tests */
    test_mergtime_cap();
    test_zero_mass_handling();
    test_null_galaxy_handling();
    test_no_central_returns_early();
    test_multiple_satellites();
    test_empty_halos_array();

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
