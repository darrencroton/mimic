/**
 * @file    test_unit_sage_satellite_stripping.c
 * @brief   Unit tests for sage_satellite_stripping module physics
 *
 * Tests the satellite stripping physics calculation in isolation using minimal mocks.
 * Validates:
 *   - Stripping calculation logic
 *   - Mass and metal conservation
 *   - Metallicity preservation
 *   - Edge cases (zero gas, boundary conditions, type filtering)
 *   - Substep distribution
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
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
extern int sage_satellite_stripping_init(void);
extern int sage_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_satellite_stripping_cleanup(void);

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
}

/* Helper: Create minimal module context */
static struct ModuleContext create_test_context(int num_substeps)
{
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.redshift = 0.0;
    ctx.time = 13.6;
    ctx.snapshot_number = 63;
    ctx.substep_number = 0;
    ctx.num_substeps = num_substeps;
    ctx.time_interval = 0.1;
    ctx.substep_dt = ctx.time_interval / num_substeps;
    ctx.params = &MimicConfig;

    return ctx;
}

/* Helper: Create test halo */
static struct Halo create_test_halo(int type, double mvir, struct GalaxyData *galaxy)
{
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));

    halo.Type = type;
    halo.Mvir = mvir;
    halo.SnapNum = 63;
    halo.galaxy = galaxy;

    return halo;
}

/* Helper: Create test galaxy */
static struct GalaxyData create_test_galaxy(float hot_gas, float metals_hot,
                                             float stellar_mass, float cold_gas)
{
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));

    gal.HotGas = hot_gas;
    gal.MetalsHotGas = metals_hot;
    gal.StellarMass = stellar_mass;
    gal.ColdGas = cold_gas;
    gal.HaloBaryonFraction = 0.17;  // Default baryon fraction

    return gal;
}

/**
 * @test    test_no_stripping_when_below_threshold
 * @brief   Satellites below baryon fraction threshold should not be stripped
 */
int test_no_stripping_when_below_threshold(void)
{
    printf("  Testing: No stripping when below threshold...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    // Central galaxy
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with baryons BELOW threshold (should not strip)
    // Mvir=10, baryon_frac=0.17 → allowed=1.7, actual=1.0 → excess=-0.7 (no stripping)
    struct GalaxyData sat_gal = create_test_galaxy(1.0, 0.02, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;

    struct Halo halos[2] = {central, satellite};

    float initial_sat_hot = sat_gal.HotGas;
    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    int result = sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot, 1e-6),
                "Satellite HotGas should be unchanged (below threshold)");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
                "Central HotGas should be unchanged");

    TEST_PASS;
}

/**
 * @test    test_stripping_when_above_threshold
 * @brief   Satellites above baryon fraction threshold should be stripped
 */
int test_stripping_when_above_threshold(void)
{
    printf("  Testing: Stripping when above threshold...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(10);  // 10 substeps

    // Central galaxy
    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with baryons ABOVE threshold (should strip)
    // Mvir=10, baryon_frac=0.17 → allowed=1.7
    // actual=5.0 hot + 0.0 stellar = 5.0 → excess=5.0-1.7=3.3
    // Per substep: 3.3/10 = 0.33
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.1, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;

    struct Halo halos[2] = {central, satellite};

    float initial_sat_hot = sat_gal.HotGas;
    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    int result = sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");

    // Calculate expected stripping
    double baryon_frac = 0.17;
    double total_baryons = 5.0;  // Only hot gas
    double allowed = baryon_frac * 10.0;  // 1.7
    double excess = total_baryons - allowed;  // 3.3
    double stripped_per_substep = excess / 10.0;  // 0.33

    TEST_ASSERT(halos[1].galaxy->HotGas < initial_sat_hot,
                "Satellite HotGas should decrease");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot - stripped_per_substep, 0.01),
                "Satellite should lose correct amount");
    TEST_ASSERT(halos[0].galaxy->HotGas > initial_cen_hot,
                "Central HotGas should increase");

    TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Stripped mass from satellite equals mass gained by central
 */
int test_mass_conservation(void)
{
    printf("  Testing: Mass conservation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_sat_hot = sat_gal.HotGas;
    float initial_cen_hot = cen_gal.HotGas;
    float initial_total = initial_sat_hot + initial_cen_hot;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    float final_total = halos[1].galaxy->HotGas + halos[0].galaxy->HotGas;
    TEST_ASSERT(FLOAT_EQ(final_total, initial_total, 1e-4),
                "Total mass should be conserved");

    // Verify: satellite lost = central gained
    float sat_lost = initial_sat_hot - halos[1].galaxy->HotGas;
    float cen_gained = halos[0].galaxy->HotGas - initial_cen_hot;
    TEST_ASSERT(FLOAT_EQ(sat_lost, cen_gained, 1e-4),
                "Satellite mass lost should equal central mass gained");

    TEST_PASS;
}

/**
 * @test    test_metal_conservation
 * @brief   Stripped metals from satellite equals metals gained by central
 */
int test_metal_conservation(void)
{
    printf("  Testing: Metal conservation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);  // Z = 0.02
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_sat_metals = sat_gal.MetalsHotGas;
    float initial_cen_metals = cen_gal.MetalsHotGas;
    float initial_total_metals = initial_sat_metals + initial_cen_metals;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    float final_total_metals = halos[1].galaxy->MetalsHotGas + halos[0].galaxy->MetalsHotGas;
    TEST_ASSERT(FLOAT_EQ(final_total_metals, initial_total_metals, 1e-5),
                "Total metals should be conserved");

    // Verify: satellite metals lost = central metals gained
    float sat_metals_lost = initial_sat_metals - halos[1].galaxy->MetalsHotGas;
    float cen_metals_gained = halos[0].galaxy->MetalsHotGas - initial_cen_metals;
    TEST_ASSERT(FLOAT_EQ(sat_metals_lost, cen_metals_gained, 1e-5),
                "Satellite metals lost should equal central metals gained");

    TEST_PASS;
}

/**
 * @test    test_metallicity_preservation
 * @brief   Metallicity should be preserved during stripping
 */
int test_metallicity_preservation(void)
{
    printf("  Testing: Metallicity preservation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 1.0, 50.0, 20.0);  // Z=0.01
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.3, 0.0, 0.0);  // Z=0.03
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    float initial_sat_Z = sat_gal.MetalsHotGas / sat_gal.HotGas;

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    // Satellite metallicity should be unchanged (same Z, less mass)
    if (halos[1].galaxy->HotGas > 0.0) {
        float final_sat_Z = halos[1].galaxy->MetalsHotGas / halos[1].galaxy->HotGas;
        TEST_ASSERT(FLOAT_EQ(final_sat_Z, initial_sat_Z, 1e-4),
                    "Satellite metallicity should be preserved");
    }

    TEST_PASS;
}

/**
 * @test    test_zero_hot_gas_no_stripping
 * @brief   Satellites with zero hot gas should not be stripped
 */
int test_zero_hot_gas_no_stripping(void)
{
    printf("  Testing: Zero hot gas - no stripping...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with zero hot gas (but other baryons)
    struct GalaxyData sat_gal = create_test_galaxy(0.0, 0.0, 5.0, 2.0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6),
                "Satellite HotGas should remain zero");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
                "Central should not gain gas (satellite has none to strip)");

    TEST_PASS;
}

/**
 * @test    test_clamping_to_available_gas
 * @brief   Cannot strip more gas than satellite has available
 */
int test_clamping_to_available_gas(void)
{
    printf("  Testing: Clamping to available gas...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with huge excess but limited hot gas
    // Mvir=100 → allowed=17, but has stellar=50 + hot=2 = 52 → excess=35
    // Should only strip 2.0 (all available hot gas)
    struct GalaxyData sat_gal = create_test_galaxy(2.0, 0.04, 50.0, 0.0);
    struct Halo satellite = create_test_halo(1, 100.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_sat_hot = sat_gal.HotGas;
    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6),
                "Satellite should be stripped to zero (clamped)");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot + initial_sat_hot, 1e-4),
                "Central should gain all available satellite hot gas");

    TEST_PASS;
}

/**
 * @test    test_type_2_orphans_skipped
 * @brief   Type 2 orphans should not be processed
 */
int test_type_2_orphans_skipped(void)
{
    printf("  Testing: Type 2 orphans skipped...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Type 2 orphan (should be skipped)
    struct GalaxyData orphan_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
    struct Halo orphan = create_test_halo(2, 10.0, &orphan_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, orphan};

    float initial_orphan_hot = orphan_gal.HotGas;
    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_orphan_hot, 1e-6),
                "Type 2 orphan should not be stripped");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
                "Central should not gain gas from orphan");

    TEST_PASS;
}

/**
 * @test    test_type_3_ejected_skipped
 * @brief   Type 3 ejected galaxies should not be processed
 */
int test_type_3_ejected_skipped(void)
{
    printf("  Testing: Type 3 ejected galaxies skipped...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Type 3 ejected (should be skipped)
    struct GalaxyData ejected_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
    struct Halo ejected = create_test_halo(3, 10.0, &ejected_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, ejected};

    float initial_ejected_hot = ejected_gal.HotGas;
    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_ejected_hot, 1e-6),
                "Type 3 ejected galaxy should not be stripped");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
                "Central should not gain gas from ejected galaxy");

    TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   NULL galaxy pointers should be handled safely
 */
int test_null_galaxy_handling(void)
{
    printf("  Testing: NULL galaxy handling...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with NULL galaxy
    struct Halo satellite = create_test_halo(1, 10.0, NULL);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_cen_hot = cen_gal.HotGas;

    /* Execute */
    int result = sage_satellite_stripping_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
                "Central should not change when satellite has NULL galaxy");

    TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Multiple satellites should all be processed correctly
 */
int test_multiple_satellites(void)
{
    printf("  Testing: Multiple satellites...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat1_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
    struct Halo sat1 = create_test_halo(1, 10.0, &sat1_gal);

    struct GalaxyData sat2_gal = create_test_galaxy(8.0, 0.16, 0.0, 0.0);
    struct Halo sat2 = create_test_halo(1, 10.0, &sat2_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, sat1, sat2};

    float initial_cen_hot = cen_gal.HotGas;
    float initial_sat1_hot = sat1_gal.HotGas;
    float initial_sat2_hot = sat2_gal.HotGas;

    /* Execute */
    sage_satellite_stripping_process(&ctx, halos, 3);

    /* Validate */
    TEST_ASSERT(halos[0].galaxy->HotGas > initial_cen_hot,
                "Central should gain gas from both satellites");
    TEST_ASSERT(halos[1].galaxy->HotGas < initial_sat1_hot,
                "Satellite 1 should lose gas");
    TEST_ASSERT(halos[2].galaxy->HotGas < initial_sat2_hot,
                "Satellite 2 should lose gas");

    TEST_PASS;
}

/**
 * @brief   Main test runner
 */
int main(void)
{
    /* Suppress INFO/DEBUG messages during tests - only show warnings and errors */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Unit Test Suite: sage_satellite_stripping Physics\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize mock configuration */
    setup_mock_config();

    /* Run all tests */
    test_no_stripping_when_below_threshold();
    test_stripping_when_above_threshold();
    test_mass_conservation();
    test_metal_conservation();
    test_metallicity_preservation();
    test_zero_hot_gas_no_stripping();
    test_clamping_to_available_gas();
    test_type_2_orphans_skipped();
    test_type_3_ejected_skipped();
    test_null_galaxy_handling();
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
