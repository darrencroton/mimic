/**
 * @file    test_unit_sage_disrupt_satellites.c
 * @brief   Unit tests for sage_disrupt_satellites module physics
 *
 * Tests the satellite disruption physics calculation in isolation using minimal mocks.
 * Validates:
 *   - Disruption flag filtering
 *   - Gas and stellar mass transfer to ICS
 *   - Metal conservation during disruption
 *   - Type marking (disrupted satellites become Type 3)
 *   - Edge cases (null pointers, Type 0 protection, Type 3 skipping)
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
extern int sage_disrupt_satellites_init(void);
extern int sage_disrupt_satellites_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_disrupt_satellites_cleanup(void);

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

/* Helper: Create test galaxy with full properties */
static struct GalaxyData create_test_galaxy(float cold_gas, float hot_gas,
                                             float stellar_mass, float ejected_gas,
                                             float ics, int is_disrupting)
{
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));

    gal.ColdGas = cold_gas;
    gal.HotGas = hot_gas;
    gal.StellarMass = stellar_mass;
    gal.EjectedGas = ejected_gas;
    gal.ICS = ics;

    // Set metals proportional to masses (Z ~ 0.02 for testing)
    gal.MetalsColdGas = cold_gas * 0.02;
    gal.MetalsHotGas = hot_gas * 0.02;
    gal.MetalsStellarMass = stellar_mass * 0.02;
    gal.MetalsEjectedGas = ejected_gas * 0.02;
    gal.MetalsICS = ics * 0.02;

    gal.IsDisrupting = is_disrupting;

    return gal;
}

/**
 * @test    test_no_disruption_when_flag_not_set
 * @brief   Satellite with IsDisrupting=false should not be processed
 */
int test_no_disruption_when_flag_not_set(void)
{
    printf("  Testing: No disruption when flag not set...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    // Central galaxy
    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite NOT marked for disruption
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;

    struct Halo halos[2] = {central, satellite};

    float initial_cen_ics = cen_gal.ICS;
    float initial_sat_stellar = sat_gal.StellarMass;
    int initial_sat_type = satellite.Type;

    /* Execute */
    int result = sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[1].Type == initial_sat_type,
                "Satellite Type should be unchanged (not disrupting)");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, initial_cen_ics, 1e-6),
                "Central ICS should be unchanged");
    TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->StellarMass, initial_sat_stellar, 1e-6),
                "Satellite StellarMass should be unchanged");

    TEST_PASS;
}

/**
 * @test    test_disruption_transfers_all_gas
 * @brief   Cold + Hot gas transferred to central's HotGas
 */
int test_disruption_transfers_all_gas(void)
{
    printf("  Testing: Disruption transfers all gas to central HotGas...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    // Central galaxy
    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite marked for disruption
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;

    struct Halo halos[2] = {central, satellite};

    float initial_cen_hot = cen_gal.HotGas;
    float sat_cold = sat_gal.ColdGas;
    float sat_hot = sat_gal.HotGas;

    /* Execute */
    int result = sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot + sat_cold + sat_hot, 1e-4),
                "Central HotGas should receive all satellite gas (cold + hot)");

    TEST_PASS;
}

/**
 * @test    test_disruption_transfers_all_metals
 * @brief   Metals from cold + hot gas transferred correctly
 */
int test_disruption_transfers_all_metals(void)
{
    printf("  Testing: Disruption transfers all gas metals...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    // Central galaxy
    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite marked for disruption
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;

    struct Halo halos[2] = {central, satellite};

    float initial_cen_metals_hot = cen_gal.MetalsHotGas;
    float sat_metals_cold = sat_gal.MetalsColdGas;
    float sat_metals_hot = sat_gal.MetalsHotGas;

    /* Execute */
    int result = sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas,
                         initial_cen_metals_hot + sat_metals_cold + sat_metals_hot, 1e-5),
                "Central MetalsHotGas should receive all satellite gas metals");

    TEST_PASS;
}

/**
 * @test    test_disruption_transfers_ejected_mass
 * @brief   EjectedGas transferred to central
 */
int test_disruption_transfers_ejected_mass(void)
{
    printf("  Testing: Disruption transfers ejected mass...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 7.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_cen_ejected = cen_gal.EjectedGas;
    float sat_ejected = sat_gal.EjectedGas;
    float initial_cen_metals_ejected = cen_gal.MetalsEjectedGas;
    float sat_metals_ejected = sat_gal.MetalsEjectedGas;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->EjectedGas, initial_cen_ejected + sat_ejected, 1e-4),
                "Central EjectedGas should receive satellite's ejected mass");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsEjectedGas,
                         initial_cen_metals_ejected + sat_metals_ejected, 1e-5),
                "Central MetalsEjectedGas should receive satellite's ejected metals");

    TEST_PASS;
}

/**
 * @test    test_disruption_transfers_stellar_to_ics
 * @brief   StellarMass added to central's ICS
 */
int test_disruption_transfers_stellar_to_ics(void)
{
    printf("  Testing: Disruption transfers stellar mass to ICS...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 15.0, 2.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_cen_ics = cen_gal.ICS;
    float sat_stellar = sat_gal.StellarMass;
    float sat_existing_ics = sat_gal.ICS;
    float initial_cen_metals_ics = cen_gal.MetalsICS;
    float sat_metals_stellar = sat_gal.MetalsStellarMass;
    float sat_metals_ics = sat_gal.MetalsICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    // ICS should receive both existing satellite ICS and all stellar mass
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS,
                         initial_cen_ics + sat_existing_ics + sat_stellar, 1e-4),
                "Central ICS should receive satellite's ICS + stellar mass");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsICS,
                         initial_cen_metals_ics + sat_metals_ics + sat_metals_stellar, 1e-5),
                "Central MetalsICS should receive satellite's ICS metals + stellar metals");

    TEST_PASS;
}

/**
 * @test    test_disruption_transfers_existing_ics
 * @brief   Satellite's existing ICS added to central
 */
int test_disruption_transfers_existing_ics(void)
{
    printf("  Testing: Disruption transfers existing ICS...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with significant existing ICS
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 8.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_cen_ics = cen_gal.ICS;
    float sat_ics = sat_gal.ICS;
    float sat_stellar = sat_gal.StellarMass;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    // Central should get both satellite's existing ICS AND stellar mass
    float expected_ics = initial_cen_ics + sat_ics + sat_stellar;
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, expected_ics, 1e-4),
                "Central ICS should receive satellite's existing ICS + stellar mass");

    TEST_PASS;
}

/**
 * @test    test_satellite_marked_type_3
 * @brief   Disrupted satellite's Type changed to 3
 */
int test_satellite_marked_type_3(void)
{
    printf("  Testing: Disrupted satellite marked as Type 3...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].Type == 3, "Disrupted satellite should be marked Type 3");
    TEST_ASSERT(halos[0].Type == 0, "Central should remain Type 0");

    TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Total baryonic mass conserved during disruption
 */
int test_mass_conservation(void)
{
    printf("  Testing: Mass conservation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    // Calculate total baryonic mass before disruption
    float cen_total = cen_gal.ColdGas + cen_gal.HotGas + cen_gal.StellarMass +
                      cen_gal.EjectedGas + cen_gal.ICS;
    float sat_total = sat_gal.ColdGas + sat_gal.HotGas + sat_gal.StellarMass +
                      sat_gal.EjectedGas + sat_gal.ICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    // After disruption, satellite's baryons are on central
    // Note: Satellite's cold/hot/stellar/ejected/ICS all transferred
    float final_cen = halos[0].galaxy->ColdGas + halos[0].galaxy->HotGas +
                      halos[0].galaxy->StellarMass + halos[0].galaxy->EjectedGas +
                      halos[0].galaxy->ICS;

    // Note: Satellite properties aren't zeroed by this module,
    // but its mass is now on the central. Since we're just validating
    // the central received the correct mass:
    float expected_cen = cen_total + sat_total;
    TEST_ASSERT(FLOAT_EQ(final_cen, expected_cen, 1e-4),
                "Central should have received all satellite mass");

    TEST_PASS;
}

/**
 * @test    test_metal_conservation
 * @brief   Total metals conserved during disruption
 */
int test_metal_conservation(void)
{
    printf("  Testing: Metal conservation...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 1);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    // Calculate total metals before disruption
    float cen_metals = cen_gal.MetalsColdGas + cen_gal.MetalsHotGas +
                       cen_gal.MetalsStellarMass + cen_gal.MetalsEjectedGas +
                       cen_gal.MetalsICS;
    float sat_metals = sat_gal.MetalsColdGas + sat_gal.MetalsHotGas +
                       sat_gal.MetalsStellarMass + sat_gal.MetalsEjectedGas +
                       sat_gal.MetalsICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    float final_cen_metals = halos[0].galaxy->MetalsColdGas + halos[0].galaxy->MetalsHotGas +
                             halos[0].galaxy->MetalsStellarMass + halos[0].galaxy->MetalsEjectedGas +
                             halos[0].galaxy->MetalsICS;

    float expected_metals = cen_metals + sat_metals;
    TEST_ASSERT(FLOAT_EQ(final_cen_metals, expected_metals, 1e-5),
                "Central should have received all satellite metals");

    TEST_PASS;
}

/**
 * @test    test_type_2_orphan_disruption
 * @brief   Type 2 orphans with IsDisrupting can be disrupted
 */
int test_type_2_orphan_disruption(void)
{
    printf("  Testing: Type 2 orphan disruption...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Type 2 orphan marked for disruption
    struct GalaxyData orphan_gal = create_test_galaxy(3.0, 2.0, 8.0, 1.0, 0.5, 1);
    struct Halo orphan = create_test_halo(2, 5.0, &orphan_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, orphan};

    float initial_cen_ics = cen_gal.ICS;
    float orphan_stellar = orphan_gal.StellarMass;
    float orphan_ics = orphan_gal.ICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(halos[1].Type == 3, "Type 2 orphan should be disrupted to Type 3");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS,
                         initial_cen_ics + orphan_stellar + orphan_ics, 1e-4),
                "Central should gain orphan's stellar mass and ICS");

    TEST_PASS;
}

/**
 * @test    test_type_3_already_disrupted_skipped
 * @brief   Type 3 galaxies (already disrupted) skipped
 */
int test_type_3_already_disrupted_skipped(void)
{
    printf("  Testing: Type 3 already disrupted skipped...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Type 3 galaxy (already disrupted) - should be skipped even if IsDisrupting
    struct GalaxyData type3_gal = create_test_galaxy(0.0, 0.0, 5.0, 0.0, 0.0, 1);
    struct Halo type3 = create_test_halo(3, 1.0, &type3_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, type3};

    float initial_cen_ics = cen_gal.ICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, initial_cen_ics, 1e-6),
                "Central ICS should be unchanged (Type 3 skipped)");
    TEST_ASSERT(halos[1].Type == 3, "Type 3 should remain Type 3");

    TEST_PASS;
}

/**
 * @test    test_type_0_central_never_disrupted
 * @brief   Type 0 centrals never disrupted even if IsDisrupting mistakenly set
 */
int test_type_0_central_never_disrupted(void)
{
    printf("  Testing: Type 0 central never disrupted (IsDisrupting protection)...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    // Central galaxy with IsDisrupting MISTAKENLY set to 1
    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 1);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Normal satellite (not disrupting)
    struct GalaxyData sat_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 0);
    struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    // Record initial central properties
    float initial_cen_cold = cen_gal.ColdGas;
    float initial_cen_hot = cen_gal.HotGas;
    float initial_cen_stellar = cen_gal.StellarMass;
    float initial_cen_ejected = cen_gal.EjectedGas;
    float initial_cen_ics = cen_gal.ICS;

    /* Execute */
    int result = sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(halos[0].Type == 0, "Central should remain Type 0 (never disrupted)");

    // Central's properties should be completely unchanged
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ColdGas, initial_cen_cold, 1e-6),
                "Central ColdGas should be unchanged");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
                "Central HotGas should be unchanged");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->StellarMass, initial_cen_stellar, 1e-6),
                "Central StellarMass should be unchanged");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->EjectedGas, initial_cen_ejected, 1e-6),
                "Central EjectedGas should be unchanged");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, initial_cen_ics, 1e-6),
                "Central ICS should be unchanged");

    TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   NULL galaxy pointer handled gracefully
 */
int test_null_galaxy_handling(void)
{
    printf("  Testing: NULL galaxy handling...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite with NULL galaxy pointer
    struct Halo satellite = create_test_halo(1, 10.0, NULL);

    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};

    float initial_cen_ics = cen_gal.ICS;

    /* Execute */
    int result = sage_disrupt_satellites_process(&ctx, halos, 2);

    /* Validate */
    TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, initial_cen_ics, 1e-6),
                "Central should be unchanged when satellite has NULL galaxy");

    TEST_PASS;
}

/**
 * @test    test_multiple_disrupting_satellites
 * @brief   Multiple satellites disrupting simultaneously
 */
int test_multiple_disrupting_satellites(void)
{
    printf("  Testing: Multiple disrupting satellites...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Two satellites both disrupting
    struct GalaxyData sat1_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 1);
    struct Halo sat1 = create_test_halo(1, 10.0, &sat1_gal);

    struct GalaxyData sat2_gal = create_test_galaxy(4.0, 2.0, 8.0, 1.0, 0.5, 1);
    struct Halo sat2 = create_test_halo(1, 8.0, &sat2_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, sat1, sat2};

    float initial_cen_ics = cen_gal.ICS;
    float sat1_stellar = sat1_gal.StellarMass;
    float sat2_stellar = sat2_gal.StellarMass;
    float sat1_ics = sat1_gal.ICS;
    float sat2_ics = sat2_gal.ICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 3);

    /* Validate */
    TEST_ASSERT(halos[1].Type == 3, "Satellite 1 should be Type 3");
    TEST_ASSERT(halos[2].Type == 3, "Satellite 2 should be Type 3");

    float expected_ics = initial_cen_ics + sat1_stellar + sat1_ics + sat2_stellar + sat2_ics;
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, expected_ics, 1e-4),
                "Central should gain ICS from both satellites");

    TEST_PASS;
}

/**
 * @test    test_empty_halos_array
 * @brief   Empty or NULL halos array handled gracefully
 */
int test_empty_halos_array(void)
{
    printf("  Testing: Empty halos array handling...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    ctx.central_galaxy = &central;

    /* Execute with NULL halos */
    int result1 = sage_disrupt_satellites_process(&ctx, NULL, 0);

    /* Execute with zero ngal */
    struct Halo halos[1] = {central};
    int result2 = sage_disrupt_satellites_process(&ctx, halos, 0);

    /* Validate */
    TEST_ASSERT(result1 == 0, "Should handle NULL halos gracefully");
    TEST_ASSERT(result2 == 0, "Should handle zero ngal gracefully");

    TEST_PASS;
}

/**
 * @test    test_mixed_disrupting_non_disrupting
 * @brief   Mix of disrupting and non-disrupting satellites
 */
int test_mixed_disrupting_non_disrupting(void)
{
    printf("  Testing: Mixed disrupting and non-disrupting satellites...\n");

    /* Setup */
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);

    struct GalaxyData cen_gal = create_test_galaxy(20.0, 100.0, 50.0, 10.0, 5.0, 0);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);

    // Satellite NOT disrupting
    struct GalaxyData sat1_gal = create_test_galaxy(5.0, 3.0, 10.0, 2.0, 1.0, 0);
    struct Halo sat1 = create_test_halo(1, 10.0, &sat1_gal);

    // Satellite IS disrupting
    struct GalaxyData sat2_gal = create_test_galaxy(4.0, 2.0, 8.0, 1.0, 0.5, 1);
    struct Halo sat2 = create_test_halo(1, 8.0, &sat2_gal);

    ctx.central_galaxy = &central;
    struct Halo halos[3] = {central, sat1, sat2};

    float initial_cen_ics = cen_gal.ICS;
    float sat2_stellar = sat2_gal.StellarMass;
    float sat2_ics = sat2_gal.ICS;

    /* Execute */
    sage_disrupt_satellites_process(&ctx, halos, 3);

    /* Validate */
    TEST_ASSERT(halos[1].Type == 1, "Non-disrupting satellite should remain Type 1");
    TEST_ASSERT(halos[2].Type == 3, "Disrupting satellite should become Type 3");

    // Only sat2 should contribute to ICS
    float expected_ics = initial_cen_ics + sat2_stellar + sat2_ics;
    TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->ICS, expected_ics, 1e-4),
                "Central should gain ICS only from disrupting satellite");

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
    printf("Unit Test Suite: sage_disrupt_satellites Physics\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    /* Initialize mock configuration */
    setup_mock_config();

    /* Run all tests */
    test_no_disruption_when_flag_not_set();
    test_disruption_transfers_all_gas();
    test_disruption_transfers_all_metals();
    test_disruption_transfers_ejected_mass();
    test_disruption_transfers_stellar_to_ics();
    test_disruption_transfers_existing_ics();
    test_satellite_marked_type_3();
    test_mass_conservation();
    test_metal_conservation();
    test_type_2_orphan_disruption();
    test_type_3_already_disrupted_skipped();
    test_type_0_central_never_disrupted();
    test_null_galaxy_handling();
    test_multiple_disrupting_satellites();
    test_empty_halos_array();
    test_mixed_disrupting_non_disrupting();

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
