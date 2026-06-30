/**
 * @file    test_unit_sage_satellite_stripping.c
 * @brief   Unit tests for sage_satellite_stripping module physics
 *
 * Tests the satellite stripping physics calculation in isolation using minimal mocks.
 *
 * SAGE parity: the module runs process_by_galaxy (one satellite at a time, ngal=1),
 * stripping a Type 1 satellite and depositing into the FOF central via
 * ctx->central_galaxy. Tests therefore call process with the single satellite halo
 * (&halos[1], 1) and the central supplied through ctx->central_galaxy.
 *
 * Validates:
 *   - Stripping calculation logic
 *   - Mass and metal conservation
 *   - Metallicity preservation
 *   - Edge cases (zero gas, boundary conditions, type filtering)
 *   - Substep distribution
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/types.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/constants.h"
#include "util/memory.h"
#include "util/error.h"

extern int sage_satellite_stripping_init(void);
extern int sage_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos,
                                            int ngal);
extern int sage_satellite_stripping_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((a) - (b)) < (epsilon))

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static void setup_mock_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
}

static struct ModuleContext create_test_context(int num_substeps) {
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

static struct Halo create_test_halo(int type, double mvir, struct GalaxyData *galaxy) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.Type = type;
  halo.Mvir = mvir;
  halo.SnapNum = 63;
  halo.galaxy = galaxy;
  return halo;
}

static struct GalaxyData create_test_galaxy(float hot_gas, float metals_hot, float stellar_mass,
                                            float cold_gas) {
  struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  gal.HotGas = hot_gas;
  gal.MetalsHotGas = metals_hot;
  gal.StellarMass = stellar_mass;
  gal.ColdGas = cold_gas;
  gal.HaloBaryonFraction = 0.17;
  return gal;
}

/**
 * @test    test_no_stripping_when_below_threshold
 * @brief   Satellites below baryon fraction threshold should not be stripped
 */
static int test_no_stripping_when_below_threshold(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  /* Mvir=10, baryon_frac=0.17 → allowed=1.7, actual=1.0 → excess=-0.7 (no strip) */
  struct GalaxyData sat_gal = create_test_galaxy(1.0, 0.02, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot, 1e-6),
              "Satellite HotGas should be unchanged (below threshold)");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central HotGas should be unchanged");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_stripping_when_above_threshold
 * @brief   Satellites above baryon fraction threshold should be stripped
 */
static int test_stripping_when_above_threshold(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(10);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  /* Mvir=10, baryon_frac=0.17 → allowed=1.7; actual=5.0 → excess=3.3; per substep=0.33 */
  struct GalaxyData sat_gal = create_test_galaxy(5.0, 0.1, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);

  double baryon_frac = 0.17;
  double total_baryons = 5.0;
  double allowed = baryon_frac * 10.0;
  double excess = total_baryons - allowed;
  double stripped_per_substep = excess / 10.0;

  TEST_ASSERT(result == 0, "Process should succeed");
  TEST_ASSERT(halos[1].galaxy->HotGas < initial_sat_hot, "Satellite HotGas should decrease");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_sat_hot - stripped_per_substep, 0.01),
              "Satellite should lose correct amount");
  TEST_ASSERT(halos[0].galaxy->HotGas > initial_cen_hot, "Central HotGas should increase");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Stripped mass from satellite equals mass gained by central
 */
static int test_mass_conservation(void) {
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

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  float final_total = halos[1].galaxy->HotGas + halos[0].galaxy->HotGas;
  float sat_lost = initial_sat_hot - halos[1].galaxy->HotGas;
  float cen_gained = halos[0].galaxy->HotGas - initial_cen_hot;

  TEST_ASSERT(FLOAT_EQ(final_total, initial_total, 1e-4), "Total mass should be conserved");
  TEST_ASSERT(FLOAT_EQ(sat_lost, cen_gained, 1e-4),
              "Satellite mass lost should equal central mass gained");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_mass_conservation_max_dynamic_substeps
 * @brief   Repeated stripping at the dynamic substep cap conserves mass
 */
static int test_mass_conservation_max_dynamic_substeps(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(MAX_DYNAMIC_SUBSTEPS);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  const double initial_sat_hot = sat_gal.HotGas;
  const double initial_cen_hot = cen_gal.HotGas;
  const double initial_total = initial_sat_hot + initial_cen_hot;

  for (int i = 0; i < MAX_DYNAMIC_SUBSTEPS; i++) {
    ctx.substep_number = i;
    int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed for each dynamic substep");
  }

  const double final_total = (double)halos[1].galaxy->HotGas + (double)halos[0].galaxy->HotGas;
  const double sat_lost = initial_sat_hot - (double)halos[1].galaxy->HotGas;
  const double cen_gained = (double)halos[0].galaxy->HotGas - initial_cen_hot;
  const double mass_tol = fmax(1e-4, fabs(initial_total) * 2e-6);
  const double transfer_tol = fmax(1e-4, fabs(initial_sat_hot) * 2e-5);

  TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, mass_tol,
                           "Total mass should be conserved at max dynamic substeps");
  TEST_ASSERT_DOUBLE_EQUAL(sat_lost, cen_gained, transfer_tol,
                           "Satellite mass lost should equal central mass gained");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metal_conservation
 * @brief   Stripped metals from satellite equals metals gained by central
 */
static int test_metal_conservation(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_metals = sat_gal.MetalsHotGas;
  float initial_cen_metals = cen_gal.MetalsHotGas;
  float initial_total_metals = initial_sat_metals + initial_cen_metals;

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  float final_total_metals = halos[1].galaxy->MetalsHotGas + halos[0].galaxy->MetalsHotGas;
  float sat_metals_lost = initial_sat_metals - halos[1].galaxy->MetalsHotGas;
  float cen_metals_gained = halos[0].galaxy->MetalsHotGas - initial_cen_metals;

  TEST_ASSERT(FLOAT_EQ(final_total_metals, initial_total_metals, 1e-5),
              "Total metals should be conserved");
  TEST_ASSERT(FLOAT_EQ(sat_metals_lost, cen_metals_gained, 1e-5),
              "Satellite metals lost should equal central metals gained");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metal_conservation_all_regimes
 * @brief   Satellite metal loss must equal central metal gain in every clamp
 *          regime, including constructed unphysical M > G states where the
 *          metallicity cap (Z = 1) is active.
 *
 * Regression guard for the inherited SAGE metal-destruction bug (fixed
 * 2026-06-16). The pre-fix code credited the central with clamped-gas *
 * metallicity, destroying metals in the two capped-Z rows below; the fix
 * credits the central with the same strippedMetals debited from the satellite,
 * conserving metals everywhere.
 */
static int test_metal_conservation_all_regimes(void) {
  /* Each row drives a distinct branch of the stripping transfer. With
   * num_substeps = 1 and only HotGas + StellarMass carrying baryons, the
   * unclamped strip demand is s = HotGas + StellarMass - bf*Mvir. G = HotGas,
   * M = MetalsHotGas. The last two rows construct M >= G so Z is capped at 1. */
  struct {
    const char *label;
    float hot;
    float metals;
    float stellar;
    double bf;
    double mvir;
  } cases[] = {
      {"no-clamp", 10.0f, 0.2f, 0.0f, 0.5, 10.0},
      {"gas-clamp normal-Z", 10.0f, 0.2f, 20.0f, 0.5, 20.0},
      {"cap both-clamps", 2.0f, 3.0f, 10.0f, 0.2, 10.0},
      {"cap gas-clamp-only", 2.0f, 10.0f, 5.0f, 0.2, 10.0},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    init_memory_system(0);
    struct ModuleContext ctx = create_test_context(1);
    struct GalaxyData cen_gal = create_test_galaxy(100.0f, 2.0f, 50.0f, 20.0f);
    struct Halo central = create_test_halo(0, 100.0, &cen_gal);
    struct GalaxyData sat_gal =
        create_test_galaxy(cases[i].hot, cases[i].metals, cases[i].stellar, 0.0f);
    sat_gal.HaloBaryonFraction = cases[i].bf;
    struct Halo satellite = create_test_halo(1, cases[i].mvir, &sat_gal);
    ctx.central_galaxy = &central;
    struct Halo halos[2] = {central, satellite};
    float sat_metals_before = sat_gal.MetalsHotGas;
    float cen_metals_before = cen_gal.MetalsHotGas;

    int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);
    TEST_ASSERT(result == 0, "Process should succeed");

    float sat_lost = sat_metals_before - halos[1].galaxy->MetalsHotGas;
    float cen_gained = halos[0].galaxy->MetalsHotGas - cen_metals_before;

    if (!FLOAT_EQ(sat_lost, cen_gained, 1e-4)) {
      printf("FAIL: metals not conserved in regime '%s' (lost=%.6f, gained=%.6f)\n", cases[i].label,
             sat_lost, cen_gained);
      return TEST_FAIL;
    }
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_metallicity_preservation
 * @brief   Metallicity should be preserved during stripping
 */
static int test_metallicity_preservation(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 1.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(10.0, 0.3, 0.0, 0.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  float initial_sat_Z = sat_gal.MetalsHotGas / sat_gal.HotGas;
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  if (halos[1].galaxy->HotGas > 0.0) {
    float final_sat_Z = halos[1].galaxy->MetalsHotGas / halos[1].galaxy->HotGas;
    TEST_ASSERT(FLOAT_EQ(final_sat_Z, initial_sat_Z, 1e-4),
                "Satellite metallicity should be preserved");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_hot_gas_no_stripping
 * @brief   Satellites with zero hot gas should not be stripped
 */
static int test_zero_hot_gas_no_stripping(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData sat_gal = create_test_galaxy(0.0, 0.0, 5.0, 2.0);
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_cen_hot = cen_gal.HotGas;

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6), "Satellite HotGas should remain zero");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas (satellite has none to strip)");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_clamping_to_available_gas
 * @brief   Cannot strip more gas than satellite has available
 */
static int test_clamping_to_available_gas(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  /* Mvir=100 → allowed=17; stellar=50 + hot=2 = 52 → excess=35; strip clamped to 2.0 */
  struct GalaxyData sat_gal = create_test_galaxy(2.0, 0.04, 50.0, 0.0);
  struct Halo satellite = create_test_halo(1, 100.0, &sat_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_sat_hot = sat_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, 0.0, 1e-6),
              "Satellite should be stripped to zero (clamped)");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot + initial_sat_hot, 1e-4),
              "Central should gain all available satellite hot gas");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type_2_orphans_skipped
 * @brief   Type 2 orphans should not be processed
 */
static int test_type_2_orphans_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData orphan_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo orphan = create_test_halo(2, 10.0, &orphan_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, orphan};
  float initial_orphan_hot = orphan_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_orphan_hot, 1e-6),
              "Type 2 orphan should not be stripped");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas from orphan");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type_3_ejected_skipped
 * @brief   Type 3 ejected galaxies should not be processed
 */
static int test_type_3_ejected_skipped(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct GalaxyData ejected_gal = create_test_galaxy(10.0, 0.2, 0.0, 0.0);
  struct Halo ejected = create_test_halo(3, 10.0, &ejected_gal);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, ejected};
  float initial_ejected_hot = ejected_gal.HotGas;
  float initial_cen_hot = cen_gal.HotGas;

  sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->HotGas, initial_ejected_hot, 1e-6),
              "Type 3 ejected galaxy should not be stripped");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not gain gas from ejected galaxy");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_galaxy_handling
 * @brief   NULL galaxy pointers should be handled safely
 */
static int test_null_galaxy_handling(void) {
  init_memory_system(0);
  struct ModuleContext ctx = create_test_context(1);
  struct GalaxyData cen_gal = create_test_galaxy(100.0, 2.0, 50.0, 20.0);
  struct Halo central = create_test_halo(0, 100.0, &cen_gal);
  struct Halo satellite = create_test_halo(1, 10.0, NULL);
  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  float initial_cen_hot = cen_gal.HotGas;

  int result = sage_satellite_stripping_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->HotGas, initial_cen_hot, 1e-6),
              "Central should not change when satellite has NULL galaxy");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_multiple_satellites
 * @brief   Multiple satellites should all be processed correctly
 */
static int test_multiple_satellites(void) {
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

  /* by-galaxy contract — the galaxy-major loop calls the module once per satellite */
  sage_satellite_stripping_process(&ctx, &halos[1], 1);
  sage_satellite_stripping_process(&ctx, &halos[2], 1);

  TEST_ASSERT(halos[0].galaxy->HotGas > initial_cen_hot,
              "Central should gain gas from both satellites");
  TEST_ASSERT(halos[1].galaxy->HotGas < initial_sat1_hot, "Satellite 1 should lose gas");
  TEST_ASSERT(halos[2].galaxy->HotGas < initial_sat2_hot, "Satellite 2 should lose gas");
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  setup_mock_config();

  TEST_RUN(test_no_stripping_when_below_threshold);
  TEST_RUN(test_stripping_when_above_threshold);
  TEST_RUN(test_mass_conservation);
  TEST_RUN(test_mass_conservation_max_dynamic_substeps);
  TEST_RUN(test_metal_conservation);
  TEST_RUN(test_metal_conservation_all_regimes);
  TEST_RUN(test_metallicity_preservation);
  TEST_RUN(test_zero_hot_gas_no_stripping);
  TEST_RUN(test_clamping_to_available_gas);
  TEST_RUN(test_type_2_orphans_skipped);
  TEST_RUN(test_type_3_ejected_skipped);
  TEST_RUN(test_null_galaxy_handling);
  TEST_RUN(test_multiple_satellites);

  TEST_SUMMARY();
  return TEST_RESULT();
}
