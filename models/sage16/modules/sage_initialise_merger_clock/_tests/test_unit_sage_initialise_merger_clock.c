/**
 * @file    test_unit_sage_initialise_merger_clock.c
 * @brief   Unit tests for sage_initialise_merger_clock module physics
 *
 * Tests the merger timescale calculation physics in isolation using minimal mocks.
 * Validates:
 *   - Dynamical friction formula correctness
 *   - Coulomb logarithm calculation
 *   - Type filtering (centrals vs satellites)
 *   - MergTime reset for Type 0 centrals
 *   - Type 2 sentinel policy (force immediate merge via MergTime=0.0)
 *   - Sentinel value handling (999.9 = unset)
 *   - Edge cases (zero mass, NULL galaxy, no central)
 *   - MergTime capping at 998.0
 *
 * Physics: Binney & Tremaine (1987) dynamical friction
 *   t_merge = 2 * 1.17 * R_vir^2 * V_vir / (ln(N_cen/N_sat) * G * M_sat)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/types.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "util/memory.h"
#include "util/error.h"

extern int sage_initialise_merger_clock_init(void);
extern int sage_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage_initialise_merger_clock_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((a) - (b)) < (epsilon))

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static void setup_mock_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43.02;
}

static struct ModuleContext create_test_context(void) {
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

static struct Halo create_test_halo(int type, double mvir, double rvir, double vvir, int len,
                                    double infall_mvir, struct GalaxyData *galaxy) {
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

static struct GalaxyData create_test_galaxy(float mergtime, float stellar_mass, float cold_gas) {
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
static int test_type0_central_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 10.0, 5.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 150.0, 1000, 0.0, &cen_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[1] = {central};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 1);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
              "Type 0 central MergTime should remain at sentinel value");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type0_mergtime_reset
 * @brief   Type 0 centrals with MergTime < 999.0 get reset to 999.9
 */
static int test_type0_mergtime_reset(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(5.0f, 10.0, 5.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 150.0, 1000, 0.0, &cen_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[1] = {central};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 1);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
              "Type 0 central should have MergTime reset to 999.9");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type1_satellite_calculation
 * @brief   Type 1 satellite gets MergTime calculated
 */
static int test_type1_satellite_calculation(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 5.0, 2.0);
  struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime < 999.0f,
              "Type 1 satellite should have MergTime calculated");
  TEST_ASSERT(halos[1].galaxy->MergTime > 0.0f, "MergTime should be positive");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type2_orphan_forces_immediate_merge
 * @brief   Type 2 orphans with unset MergTime are forced to immediate merge
 */
static int test_type2_orphan_forces_immediate_merge(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData orphan_gal = create_test_galaxy(999.9f, 3.0, 1.0);
  struct Halo orphan = create_test_halo(2, 5.0, 0.1, 50.0, 0, 10.0, &orphan_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, orphan};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 0.0f, 0.01),
              "Type 2 orphan with sentinel MergTime should be forced to 0.0");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type3_plus_skipped
 * @brief   Type > 2 halos are not processed
 */
static int test_type3_plus_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData type3_gal = create_test_galaxy(999.9f, 5.0, 2.0);
  struct Halo type3_halo = create_test_halo(3, 20.0, 0.2, 100.0, 200, 25.0, &type3_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, type3_halo};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 999.9f, 0.01),
              "Type 3 halo should remain at sentinel value (skipped)");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// SENTINEL AND CONDITION TESTS
// ============================================================================

/**
 * @test    test_already_calculated_skipped
 * @brief   Satellites with MergTime <= 999.0 are skipped (already calculated)
 */
static int test_already_calculated_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(5.5f, 5.0, 2.0);
  struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 5.5f, 0.01),
              "Already-calculated satellite should keep original MergTime");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_no_infall_mvir_still_calculated
 * @brief   Satellites with infallMvir <= 0 still get MergTime when unset
 */
static int test_no_infall_mvir_still_calculated(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 5.0, 2.0);
  struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 0.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime < 999.0f,
              "Satellite with infallMvir=0 should still get a merger timescale");
  TEST_ASSERT(halos[1].galaxy->MergTime > 0.0f, "Calculated merger timescale should be positive");
  check_memory_leaks();
  return TEST_PASS;
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
static int test_dynamical_friction_formula(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  double coulomb = log1p(1000.0 / 100.0);
  double expected = 2.0 * 1.17 * 0.25 * 200.0 / (coulomb * 43.02 * 10.0);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, expected, 0.01),
              "MergTime should match dynamical friction formula");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_coulomb_logarithm
 * @brief   Verify Coulomb logarithm uses log1p(N_cen/N_sat)
 */
static int test_coulomb_logarithm(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 10000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  sage_initialise_merger_clock_process(&ctx, halos, 2);
  float mergtime1 = halos[1].galaxy->MergTime;

  halos[0].Len = 1000;
  halos[1].galaxy->MergTime = 999.9f;
  sage_initialise_merger_clock_process(&ctx, halos, 2);
  float mergtime2 = halos[1].galaxy->MergTime;

  /* Larger central Len → larger Coulomb log → shorter merger time */
  TEST_ASSERT(mergtime2 > mergtime1, "Smaller particle ratio should give longer merger time");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_small_satellite_immediate_merge
 * @brief   SAGE parity: satellites with Len < MinNumPartSatHalo (10) get
 *          mergtime = -1.0 (immediate merge), not a finite dynamical-friction
 *          clock. Satellites with Len >= 10 get a finite positive time.
 *          (SAGE model_mergers.c estimate_merging_time: the Len >= 10 condition.)
 */
static int test_small_satellite_immediate_merge(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite1 = create_test_halo(1, 10.0, 0.1, 100.0, 5, 15.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite2 = create_test_halo(1, 10.0, 0.1, 100.0, 10, 15.0, &sat_gal2);
  ctx.central_galaxy = &central;

  struct Halo halos1[2] = {central, satellite1};
  sage_initialise_merger_clock_process(&ctx, halos1, 2);
  float mergtime_below = halos1[1].galaxy->MergTime;

  struct Halo halos2[2] = {central, satellite2};
  sage_initialise_merger_clock_process(&ctx, halos2, 2);
  float mergtime_at = halos2[1].galaxy->MergTime;

  TEST_ASSERT(FLOAT_EQ(mergtime_below, -1.0f, 0.01),
              "Len<10 satellite should get mergtime=-1 (immediate merge)");
  TEST_ASSERT(mergtime_at > 0.0f, "Len>=10 satellite should get a finite positive merger time");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_baryonic_mass_included
 * @brief   StellarMass and ColdGas are included in satellite mass
 */
static int test_baryonic_mass_included(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite1 = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 5.0, 5.0);
  struct Halo satellite2 = create_test_halo(1, 10.0, 0.1, 100.0, 100, 15.0, &sat_gal2);
  ctx.central_galaxy = &central;

  struct Halo halos1[2] = {central, satellite1};
  sage_initialise_merger_clock_process(&ctx, halos1, 2);
  float mergtime_no_baryons = halos1[1].galaxy->MergTime;

  struct Halo halos2[2] = {central, satellite2};
  sage_initialise_merger_clock_process(&ctx, halos2, 2);
  float mergtime_with_baryons = halos2[1].galaxy->MergTime;

  /* More mass → shorter merger time (mass in denominator) */
  TEST_ASSERT(mergtime_with_baryons < mergtime_no_baryons,
              "More massive satellite should merge faster");
  TEST_ASSERT(FLOAT_EQ(mergtime_no_baryons / mergtime_with_baryons, 2.0, 0.1),
              "Twice the mass should give half the merger time");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_mergtime_cap
 * @brief   MergTime >= 999.0 is capped to 998.0
 */
static int test_mergtime_cap(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 100.0, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 0.001, 0.001, 10.0, 10, 0.002, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime <= 998.0f, "MergTime should be capped at 998.0");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_mass_handling
 * @brief   Zero satellite mass returns -1.0
 */
static int test_zero_mass_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 0.0, 0.1, 100.0, 100, 0.001, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime < 0.0f, "Zero mass should give MergTime = -1.0 (invalid)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   NULL galaxy pointers are handled safely
 */
static int test_null_galaxy_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct Halo satellite = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, NULL);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_no_central_returns_early
 * @brief   No Type 0 central returns 0 (early exit)
 */
static int test_no_central_returns_early(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 5.0, 2.0);
  struct Halo satellite1 = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 3.0, 1.0);
  struct Halo satellite2 = create_test_halo(2, 10.0, 0.1, 80.0, 100, 15.0, &sat_gal2);
  ctx.central_galaxy = &satellite1;
  struct Halo halos[2] = {satellite1, satellite2};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 2);

  TEST_ASSERT(result == 0, "Should return 0 when no central found");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MergTime, 999.9f, 0.01),
              "Satellite 1 should remain at sentinel (no central)");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MergTime, 999.9f, 0.01),
              "Satellite 2 should remain at sentinel (no central)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Multiple satellites are processed independently
 */
static int test_multiple_satellites(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, 0.5, 200.0, 1000, 0.0, &cen_gal);
  struct GalaxyData sat_gal1 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite1 = create_test_halo(1, 20.0, 0.2, 100.0, 200, 25.0, &sat_gal1);
  struct GalaxyData sat_gal2 = create_test_galaxy(999.9f, 0.0, 0.0);
  struct Halo satellite2 = create_test_halo(1, 5.0, 0.1, 50.0, 50, 8.0, &sat_gal2);
  ctx.central_galaxy = &central;
  struct Halo halos[3] = {central, satellite1, satellite2};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 3);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->MergTime < 999.0f, "Satellite 1 should have MergTime calculated");
  TEST_ASSERT(halos[2].galaxy->MergTime < 999.0f, "Satellite 2 should have MergTime calculated");
  TEST_ASSERT(halos[2].galaxy->MergTime > halos[1].galaxy->MergTime,
              "Less massive satellite should have longer merger time");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_empty_halos_array
 * @brief   Empty halos array handled gracefully
 */
static int test_empty_halos_array(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();

  int result1 = sage_initialise_merger_clock_process(&ctx, NULL, 0);
  TEST_ASSERT(result1 == 0, "Should handle NULL halos gracefully");

  struct Halo dummy;
  int result2 = sage_initialise_merger_clock_process(&ctx, &dummy, 0);
  TEST_ASSERT(result2 == 0, "Should handle ngal=0 gracefully");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type2_non_sentinel_preserved
 * @brief   Type 2 satellites with pre-set MergTime keep existing value
 */
static int test_type2_non_sentinel_preserved(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context();
  struct GalaxyData fof_central_gal = create_test_galaxy(999.9f, 50.0, 20.0);
  struct Halo fof_central = create_test_halo(0, 200.0, 1.0, 300.0, 1000, 0.0, &fof_central_gal);
  struct GalaxyData subhalo_central_gal = create_test_galaxy(999.9f, 5.0, 2.0);
  struct Halo subhalo_central =
      create_test_halo(1, 40.0, 0.2, 100.0, 20, 0.0, &subhalo_central_gal);
  struct GalaxyData orphan_gal = create_test_galaxy(5.0f, 1.0, 0.5);
  struct Halo orphan = create_test_halo(2, 5.0, 0.1, 50.0, 0, 10.0, &orphan_gal);
  orphan.CentralHalo = 1;
  ctx.central_galaxy = &fof_central;
  struct Halo halos[3] = {fof_central, subhalo_central, orphan};

  int result = sage_initialise_merger_clock_process(&ctx, halos, 3);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[2].galaxy->MergTime, 5.0f, 0.01),
              "Type 2 with non-sentinel MergTime should keep prior value");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  setup_mock_config();

  TEST_RUN(test_type0_central_skipped);
  TEST_RUN(test_type0_mergtime_reset);
  TEST_RUN(test_type1_satellite_calculation);
  TEST_RUN(test_type2_orphan_forces_immediate_merge);
  TEST_RUN(test_type3_plus_skipped);
  TEST_RUN(test_already_calculated_skipped);
  TEST_RUN(test_no_infall_mvir_still_calculated);
  TEST_RUN(test_dynamical_friction_formula);
  TEST_RUN(test_coulomb_logarithm);
  TEST_RUN(test_small_satellite_immediate_merge);
  TEST_RUN(test_baryonic_mass_included);
  TEST_RUN(test_mergtime_cap);
  TEST_RUN(test_zero_mass_handling);
  TEST_RUN(test_null_galaxy_handling);
  TEST_RUN(test_no_central_returns_early);
  TEST_RUN(test_multiple_satellites);
  TEST_RUN(test_empty_halos_array);
  TEST_RUN(test_type2_non_sentinel_preserved);

  TEST_SUMMARY();
  return TEST_RESULT();
}
