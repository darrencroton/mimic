/**
 * @file    test_unit_sage26_calculate_cooling_budget.c
 * @brief   Unit tests for sage26_calculate_cooling_budget module
 *
 * Validates: Module lifecycle, cooling tables, interpolation, memory safety, the carried state
 * process() writes on every call, and the CGMrecipeOn 1 helpers (clustering table, cold streams,
 * bulk CGM cooling).
 *
 * Test cases:
 *   Suite 1: Cooling Tables
 *     - test_cooling_tables_loading: All 8 metallicity tables load from disk
 *     - test_temperature_interpolation: Temperature interpolation accuracy
 *     - test_metallicity_interpolation: 2D metallicity-dependent interpolation
 *     - test_primordial_gas_cooling: Zero-metallicity gas handled correctly
 *     - test_super_solar_metallicity: Clamping to maximum table at high Z
 *     - test_extreme_temperatures: Bounds enforcement outside table range
 *
 *   Suite 2: Module Integration
 *     - test_module_registration: Module registers successfully
 *     - test_memory_safety: No memory leaks after init/cleanup cycle
 *
 *   Suite 3: Carried State (SAGE26 cooling_recipe_hot)
 *     - test_outer_guard_writes_carried_state: HotGas 0 -> path 1, every carried entry
 *       written, tcool reset to 0, tff and RcoolToRvir left as they were
 *     - test_tail_writes_carried_state: a cooling galaxy -> path 0, positive mass, x, rcool
 *     - test_no_interval_marks_not_evaluated: no positive interval -> path -1, no mass
 *
 *   Suite 4: the CGMrecipeOn 1 shared helpers (clustering table, streams, CGM cooling)
 *
 *   Suite 5: CGMrecipeOn
 *     - test_cgm_recipe_on_refusals: a missing or unsupported CGMrecipeOn fails init()
 *     - test_cgm_on_writes_only_transport: with CGMrecipeOn 1 the budget writes only its
 *       no-cooling transport, and the radio and apply modules write no galaxy property
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "../cooling_tables.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_constants.h"
#include "shared/sage_cooling_physics.h"

extern int sage26_calculate_cooling_budget_init(void);
extern int sage26_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_cooling_budget_cleanup(void);

/**
 * @brief A central with stale carried state and distinguishable diagnostics
 *
 * Millennium-like units (UnitTime_in_s = 3.08568e19 s, UnitDensity_in_cgs =
 * 6.7699e-31 g/cm^3); the carried entries hold values no call writes, so a test
 * sees that process() overwrites every one of them.
 */
static void setup_cooling_galaxy(struct Halo *halo, struct GalaxyData *gal,
                                 struct ModuleContext *ctx) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  memset(ctx, 0, sizeof(*ctx));
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitTime_in_s = 3.08568e19;
  MimicConfig.UnitDensity_in_cgs = 6.7698980144406312e-31;
  ctx->params = &MimicConfig;
  ctx->num_substeps = 10;

  halo->galaxy = gal;
  halo->Type = 0;
  halo->SnapNum = 40;
  halo->dT = 0.002;
  halo->Mvir = 100.0;
  halo->Rvir = 0.2;
  halo->Vvir = 150.0;

  gal->HotGas = 10.0f;
  gal->MetalsHotGas = 0.2f;
  gal->CoolingGas = -7.0;
  gal->CoolingX = -7.0;
  gal->Rcool = -7.0;
  gal->CoolingReturnPath = 99;
  gal->tcool = -11.0f;
  gal->tff = -22.0f;
  gal->RcoolToRvir = -33.0f;
}

/**
 * @test    test_module_registration
 * @brief   Test that sage26_cooling module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage26_cooling_register() works, module appears in registry
 */
int test_module_registration(void) {
  ensure_modules_registered();

  /* Check module is registered by trying to enable it */
  reset_config();
  test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  /* Should fail if module not registered */
  int result = module_system_init();

  /* Clean up */
  if (result == 0) {
    module_system_cleanup();
  }

  TEST_ASSERT(result == 0, "sage26_cooling module should register successfully");
  return TEST_PASS;
}

/**
 * @test    test_cooling_tables_loading
 * @brief   Test that cooling function tables load correctly
 *
 * Expected: All 8 metallicity tables load successfully
 * Validates: cooling_tables_init() succeeds, files exist in module directory
 */
int test_cooling_tables_loading(void) {
  /* Test loading from module directory */
  const char *cool_dir =
      MIMIC_COMPILED_MODEL_PATH "/modules/sage26_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);

  TEST_ASSERT(result == 0, "Cooling tables should load successfully from module directory");

  /* Clean up */
  cooling_tables_cleanup();

  return TEST_PASS;
}

/**
 * @test    test_temperature_interpolation
 * @brief   Test temperature interpolation accuracy
 *
 * Expected: Interpolation between table points is accurate
 * Validates: get_metaldependent_cooling_rate() temperature interpolation
 */
int test_temperature_interpolation(void) {
  const char *cool_dir =
      MIMIC_COMPILED_MODEL_PATH "/modules/sage26_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);
  TEST_ASSERT(result == 0, "Tables must load for interpolation test");

  /* Test interpolation at solar metallicity (logZ = log10(0.02) = -1.699)
   * Test temperature T = 10^5 K (logT = 5.0) */
  double logT = 5.0;
  double logZ = log10(0.02); /* Solar metallicity */

  double lambda = get_metaldependent_cooling_rate(logT, logZ);

  /* Lambda should be positive and reasonable (order of magnitude 10^-22 to 10^-20) */
  TEST_ASSERT(lambda > 0.0, "Cooling rate must be positive");
  TEST_ASSERT(lambda < 1e-18, "Cooling rate should be reasonable magnitude");
  TEST_ASSERT(lambda > 1e-26, "Cooling rate should not be too small");

  cooling_tables_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_metallicity_interpolation
 * @brief   Test 2D metallicity-dependent interpolation
 *
 * Expected: Cooling rate varies smoothly with metallicity
 * Validates: get_metaldependent_cooling_rate() 2D interpolation
 */
int test_metallicity_interpolation(void) {
  const char *cool_dir =
      MIMIC_COMPILED_MODEL_PATH "/modules/sage26_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);
  TEST_ASSERT(result == 0, "Tables must load for interpolation test");

  /* Test at fixed temperature, varying metallicity */
  double logT = 5.5; /* T = 10^5.5 K */

  /* Get cooling rates at different metallicities */
  double lambda_primordial = get_metaldependent_cooling_rate(logT, -10.0);       /* Very low Z */
  double lambda_subsolar = get_metaldependent_cooling_rate(logT, log10(0.002));  /* 0.1 solar */
  double lambda_solar = get_metaldependent_cooling_rate(logT, log10(0.02));      /* solar */
  double lambda_supersolar = get_metaldependent_cooling_rate(logT, log10(0.04)); /* 2 solar */

  /* All should be positive */
  TEST_ASSERT(lambda_primordial > 0.0, "Primordial cooling rate must be positive");
  TEST_ASSERT(lambda_subsolar > 0.0, "Sub-solar cooling rate must be positive");
  TEST_ASSERT(lambda_solar > 0.0, "Solar cooling rate must be positive");
  TEST_ASSERT(lambda_supersolar > 0.0, "Super-solar cooling rate must be positive");

  /* Metallicity should increase cooling rate (more metal lines) */
  TEST_ASSERT(lambda_solar > lambda_primordial,
              "Solar metallicity should cool faster than primordial");

  cooling_tables_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_primordial_gas_cooling
 * @brief   Test cooling of zero-metallicity gas
 *
 * Expected: Primordial cooling handled correctly (no metals, only H/He)
 * Validates: Edge case handling for logZ → -infinity
 */
int test_primordial_gas_cooling(void) {
  const char *cool_dir =
      MIMIC_COMPILED_MODEL_PATH "/modules/sage26_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);
  TEST_ASSERT(result == 0, "Tables must load for primordial test");

  /* Test primordial gas (Z = 0, logZ → -infinity) */
  double logT = 4.5;   /* T = 10^4.5 K */
  double logZ = -10.0; /* Effectively zero metallicity */

  double lambda = get_metaldependent_cooling_rate(logT, logZ);

  /* Should use primordial table (table 0) */
  TEST_ASSERT(lambda > 0.0, "Primordial cooling must be positive");
  TEST_ASSERT(isfinite(lambda), "Primordial cooling must be finite");

  cooling_tables_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_super_solar_metallicity
 * @brief   Test cooling at super-solar metallicity
 *
 * Expected: Handles Z > Z_sun correctly (clamps to maximum table)
 * Validates: Edge case handling for high metallicity
 */
int test_super_solar_metallicity(void) {
  const char *cool_dir =
      MIMIC_COMPILED_MODEL_PATH "/modules/sage26_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);
  TEST_ASSERT(result == 0, "Tables must load for super-solar test");

  /* Test super-solar metallicity (Z = 10 * Z_sun) */
  double logT = 6.0;        /* T = 10^6 K */
  double logZ = log10(0.2); /* 10 times solar */

  double lambda = get_metaldependent_cooling_rate(logT, logZ);

  /* Should clamp to maximum table (super-solar) */
  TEST_ASSERT(lambda > 0.0, "Super-solar cooling must be positive");
  TEST_ASSERT(isfinite(lambda), "Super-solar cooling must be finite");

  /* Should give same result as the maximum table metallicity, [Fe/H]=+0.5,
   * i.e. logZ = 0.5 + log10(Z_sun). (The previous probe log10(0.063) sits just
   * BELOW this boundary and only matched while a table-corruption bug shifted
   * the boundary downward on re-init; see cooling_tables.c metallicities.) */
  double lambda_max = get_metaldependent_cooling_rate(logT, 0.5 + log10(0.02));
  TEST_ASSERT(fabs(lambda - lambda_max) < 1e-30, "Super-solar should clamp to maximum table");

  cooling_tables_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_extreme_temperatures
 * @brief   Test cooling at extreme temperatures
 *
 * Expected: Handles T < 10^4 K and T > 10^8.5 K correctly
 * Validates: Temperature bounds enforcement
 */
int test_extreme_temperatures(void) {
  const char *cool_dir =
      MIMIC_COMPILED_MODEL_PATH "/modules/sage26_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);
  TEST_ASSERT(result == 0, "Tables must load for temperature test");

  double logZ = log10(0.02); /* Solar metallicity */

  /* Test very low temperature (below table range) */
  double logT_low = 3.0; /* T = 10^3 K, below minimum */
  double lambda_low = get_metaldependent_cooling_rate(logT_low, logZ);

  /* Should clamp to minimum temperature */
  TEST_ASSERT(lambda_low > 0.0, "Low-T cooling must be positive");
  TEST_ASSERT(isfinite(lambda_low), "Low-T cooling must be finite");

  /* Test very high temperature (above table range) */
  double logT_high = 9.0; /* T = 10^9 K, above maximum */
  double lambda_high = get_metaldependent_cooling_rate(logT_high, logZ);

  /* Should clamp to maximum temperature */
  TEST_ASSERT(lambda_high > 0.0, "High-T cooling must be positive");
  TEST_ASSERT(isfinite(lambda_high), "High-T cooling must be finite");

  cooling_tables_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   Test for memory leaks during module operation
 *
 * Expected: No memory leaks after init/cleanup cycle
 * Validates: Proper memory management in cooling tables
 */
int test_memory_safety(void) {
  ensure_modules_registered();

  /* Initialize module */
  reset_config();
  test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module init must succeed for memory test");

  /* Clean up */
  module_system_cleanup();

  /* Check for leaks */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_outer_guard_writes_carried_state
 * @brief   SAGE26's outer guard (HotGas > 0 && Vvir > 0) writes the carried state
 *
 * Expected: tcool reset to 0 at entry; tff and RcoolToRvir untouched; CoolingGas,
 *           CoolingX and Rcool written as 0 and the return path as 1, since the
 *           init_repeat reset happens only once per snapshot
 */
int test_outer_guard_writes_carried_state(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct ModuleContext ctx;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_cooling_galaxy(&halo, &gal, &ctx);
  gal.HotGas = 0.0f;

  TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx, &halo, 1) == 0, "process succeeds");
  TEST_ASSERT(gal.CoolingReturnPath == SAGE_COOLING_PATH_OUTER_GUARD, "path is the outer guard");
  TEST_ASSERT(gal.CoolingGas == 0.0 && gal.CoolingX == 0.0 && gal.Rcool == 0.0,
              "every carried entry is written");
  TEST_ASSERT(gal.tcool == 0.0f, "tcool is reset to 0 at every call");
  TEST_ASSERT(gal.tff == -22.0f && gal.RcoolToRvir == -33.0f,
              "tff and RcoolToRvir are left as they were");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_tail_writes_carried_state
 * @brief   A cooling central reaches the normal tail with SAGE26's diagnostics
 *
 * Expected: path 0; positive post-cap mass no larger than HotGas; positive x and
 *           rcool; RcoolToRvir = float(rcool / float Rvir); tcool from the float
 *           Rvir / Vvir with SAGE26's conversion
 */
int test_tail_writes_carried_state(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct ModuleContext ctx;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_cooling_galaxy(&halo, &gal, &ctx);

  TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx, &halo, 1) == 0, "process succeeds");
  TEST_ASSERT(gal.CoolingReturnPath == SAGE_COOLING_PATH_TAIL, "path is the normal tail");
  TEST_ASSERT(gal.CoolingGas > 0.0 && gal.CoolingGas <= gal.HotGas, "post-cap mass in (0, HotGas]");
  TEST_ASSERT(gal.CoolingX > 0.0 && gal.Rcool > 0.0, "x and rcool are carried");
  TEST_ASSERT(gal.RcoolToRvir == (float)(gal.Rcool / (float)halo.Rvir),
              "RcoolToRvir is rcool over the float Rvir");
  const double tcool = (float)halo.Rvir / (float)halo.Vvir;
  const float expected_tcool = (float)((tcool * 1.0e10 / MimicConfig.Hubble_h *
                                        SAGE_SEC_PER_GIGAYEAR / MimicConfig.UnitTime_in_s));
  TEST_ASSERT(gal.tcool == expected_tcool, "tcool uses the float Rvir / Vvir and SAGE26's factor");
  TEST_ASSERT(gal.tff != -22.0f, "tff is written");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_no_interval_marks_not_evaluated
 * @brief   Without a positive interval (SnapNum < 0, dT <= 0) cooling does not run
 *
 * Expected: path -1, no mass, no x or rcool; the diagnostics are left alone
 */
int test_no_interval_marks_not_evaluated(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct ModuleContext ctx;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_cooling_galaxy(&halo, &gal, &ctx);
  halo.SnapNum = -1;
  halo.dT = -1.0;

  TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx, &halo, 1) == 0, "process succeeds");
  TEST_ASSERT(gal.CoolingReturnPath == SAGE_COOLING_PATH_NOT_EVALUATED, "path is not evaluated");
  TEST_ASSERT(gal.CoolingGas == 0.0 && gal.CoolingX == 0.0 && gal.Rcool == 0.0,
              "every carried entry is written");
  TEST_ASSERT(gal.tcool == -11.0f, "tcool is left alone when cooling does not run");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

// ============================================================================
// SUITE 4: THE CGMrecipeOn 1 SHARED HELPERS (shared/sage_cooling_physics.h)
// ============================================================================
//
// Behavioural tests of the shared cooling helpers (shared/sage_cooling_physics.h). Every
// expectation is derived independently of the helper under test: a literal chosen so the result
// is exact, or SAGE26's expression written inline with its operand order.

/* SAGE26's literal clustering tables, copied independently of the package */
static const double TEST_MSTAR_MILL[31] = {
    12.8964, 12.2501, 11.5685, 10.9046, 10.2717, 9.6699, 9.0969, 8.5486, 8.0214, 7.5126, 7.0206,
    6.5435,  6.0794,  5.6266,  5.1842,  4.7514,  4.3276, 3.9122, 3.5045, 3.1039, 2.7098, 2.3217,
    1.9393,  1.5626,  1.1912,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000};
static const double TEST_MSTAR_UCHUU[31] = {
    12.8028, 12.0672, 11.3101, 10.5795, 9.8831, 9.2194, 8.5834, 7.9701, 7.3764, 6.8002, 6.2389,
    5.6901,  5.1521,  4.6241,  4.1054,  3.5950, 3.0915, 2.5943, 2.1027, 1.6166, 1.1359, 1.0000,
    1.0000,  1.0000,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000, 1.0000};

/* 10^x as SAGE26 computes pow(10.0, x) (__exp10 on Apple) */
static double test_exp10(const double x) {
#if defined(__APPLE__)
  return __exp10(x);
#else
  return pow(10.0, x);
#endif
}

/**
 * @test    test_clustering_table_grid_points
 * @brief   At every grid point z = 0.5 k the table returns SAGE26's literal entry, both tables
 *
 * Expected: fz = 0, so fma(table[k], 1, table[k+1] * 0) is table[k] exactly (k = 30 through the
 * clamped iz = 29 with fz = 1); the Uchuu table for Omega 0.3089 exactly.
 */
int test_clustering_table_grid_points(void) {
  for (int k = 0; k <= 30; k++) {
    struct MimicSageClusteringLocals l;
    const double mill = mimic_sage_interpolate_clustering_mass(0.5 * k, 0.25, &l);
    TEST_ASSERT(mill == TEST_MSTAR_MILL[k], "the Millennium grid value is the literal entry");
    TEST_ASSERT(l.uchuu == 0, "Omega 0.25 selects Millennium");
    const double uchuu = mimic_sage_interpolate_clustering_mass(0.5 * k, 0.3089, &l);
    TEST_ASSERT(uchuu == TEST_MSTAR_UCHUU[k], "the Uchuu grid value is the literal entry");
    TEST_ASSERT(l.uchuu == 1, "Omega 0.3089 selects Uchuu");
  }
  return TEST_PASS;
}

/**
 * @test    test_clustering_table_between_and_clamps
 * @brief   Between grid points, at each z clamp and at the iz clamp
 *
 * Expected: z = 2.25 gives SAGE26's fused multiply-add interpolation at iz 4, fz 0.5 written
 * inline; z = -1 the z = 0 entry; z = 20 and z = 15 the z = 15 entry with iz clamped to 29 and
 * fz 1.
 */
int test_clustering_table_between_and_clamps(void) {
  struct MimicSageClusteringLocals l;
  const double mid = mimic_sage_interpolate_clustering_mass(2.25, 0.25, &l);
  /* SAGE parity: a fused multiply-add of the first product, as SAGE26 writes it */
  const double expected_mid = fma(TEST_MSTAR_MILL[4], 1.0 - 0.5, TEST_MSTAR_MILL[5] * 0.5);
  TEST_ASSERT(mid == expected_mid, "z = 2.25 interpolates entries 4 and 5 with fz 0.5");
  TEST_ASSERT(l.iz == 4 && l.fz == 0.5, "iz 4 and fz 0.5 between grid points");

  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(-1.0, 0.25, &l) == TEST_MSTAR_MILL[0],
              "z below 0 is clamped to 0");
  TEST_ASSERT(l.zz == 0.0 && l.iz == 0, "the clamped z and index");
  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(20.0, 0.3089, &l) == TEST_MSTAR_UCHUU[30],
              "z above 15 is clamped to 15");
  TEST_ASSERT(l.zz == 15.0, "the clamped z");
  TEST_ASSERT(l.iz == 29 && l.fz == 1.0, "iz is clamped to 29 at z = 15, so fz is 1");
  (void)mimic_sage_interpolate_clustering_mass(15.0, 0.25, &l);
  TEST_ASSERT(l.iz == 29 && l.fz == 1.0, "z = 15 itself takes the clamped index 29");
  (void)mimic_sage_interpolate_clustering_mass(14.75, 0.25, &l);
  TEST_ASSERT(l.iz == 29 && l.fz == 0.5, "the last interval needs no clamp");

  /* z = 5.01e-05: the fused form differs from the separately rounded sum, so this pins the
   * fused interpolation */
  const double z = 5.0100000000000005e-05;
  const double fz = (z - 0.0) / (0.5 - 0.0);
  const volatile double first = TEST_MSTAR_MILL[0] * (1.0 - fz);
  const volatile double second = TEST_MSTAR_MILL[1] * fz;
  const double fused = fma(TEST_MSTAR_MILL[0], 1.0 - fz, TEST_MSTAR_MILL[1] * fz);
  TEST_ASSERT(fused != first + second, "the fused and unfused forms differ at this z");
  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(z, 0.25, NULL) == fused,
              "the interpolation is fused as SAGE26 computes it");
  return TEST_PASS;
}

/**
 * @test    test_clustering_table_cosmology_boundary
 * @brief   The Uchuu selection fabs(Omega - 0.3089) < 0.01, just inside and outside both ends
 *
 * Expected: the doubles 0.2989 and 0.3189 select Millennium; one double inside each selects Uchuu,
 * visible as the z = 0 entry.
 */
int test_clustering_table_cosmology_boundary(void) {
  const double low_out = 0.2989;
  const double low_in = nextafter(low_out, 1.0);
  const double high_out = 0.3189;
  const double high_in = nextafter(high_out, 0.0);
  TEST_ASSERT(!(fabs(low_out - 0.3089) < 0.01) && (fabs(low_in - 0.3089) < 0.01),
              "the low pair straddles SAGE26's window");
  TEST_ASSERT(!(fabs(high_out - 0.3089) < 0.01) && (fabs(high_in - 0.3089) < 0.01),
              "the high pair straddles SAGE26's window");
  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(0.0, low_out, NULL) == TEST_MSTAR_MILL[0],
              "0.2989 is Millennium");
  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(0.0, low_in, NULL) == TEST_MSTAR_UCHUU[0],
              "one double above 0.2989 is Uchuu");
  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(0.0, high_out, NULL) == TEST_MSTAR_MILL[0],
              "0.3189 is Millennium");
  TEST_ASSERT(mimic_sage_interpolate_clustering_mass(0.0, high_in, NULL) == TEST_MSTAR_UCHUU[0],
              "one double below 0.3189 is Uchuu");
  return TEST_PASS;
}

/** @brief A hot-regime central for the stream tests: stored SnapNum 40 with ZZ[40] = z */
static void setup_stream_galaxy(struct Halo *halo, struct GalaxyData *gal, const double z) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.Omega = 0.25;
  MimicConfig.UnitTime_in_s = 3.08568e19;
  MimicConfig.UnitDensity_in_cgs = 6.7698980144406312e-31;
  MimicConfig.Snaplistlen = 64;
  for (int k = 0; k < 64; k++) {
    MimicConfig.ZZ[k] = NAN;
  }
  MimicConfig.ZZ[40] = z;
  MimicConfig.ZZ[41] = 0.0; /* the current snapshot: the streams must not read it */
  halo->galaxy = gal;
  halo->SnapNum = 40;
  halo->Mvir = 100.0;
  halo->Rvir = 0.1;
  halo->Vvir = 200.0;
  gal->HotGas = 10.0f;
  gal->MetalsHotGas = 0.1f;
  gal->tcool = -11.0f;
  gal->tff = -22.0f;
  gal->RcoolToRvir = -33.0f;
}

/**
 * @test    test_streams_sigmoid_from_stored_snapshot
 * @brief   The cold streams at z = 3 (no cutoff): SAGE26's chain written inline
 *
 * Expected: every local and the cooling mass equal SAGE26's expressions with ZZ[stored SnapNum]
 * = 3 (the current snapshot's ZZ 0 would cut the streams off), the mdot stores floats of the
 * components before the cap.
 */
int test_streams_sigmoid_from_stored_snapshot(void) {
  struct Halo halo;
  struct GalaxyData gal;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_stream_galaxy(&halo, &gal, 3.0);
  const double dt = 1.0e-4;
  struct MimicSageHotCoolingLocals l;
  double cooling = -1.0;
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, 6.0e11, &l, &cooling) == 0,
              "the streams accept a stored SnapNum inside the table");
  TEST_ASSERT(l.branch == MIMIC_SAGE_HOT_BRANCH_STREAMS, "CGMrecipeOn 1 takes the streams");

  /* SAGE26's stream chain, inline */
  const float rvir = 0.1f, vvir = 200.0f, mvir = 100.0f, hot = 10.0f;
  const double tcool = rvir / vvir;
  const double M_shock = 6.0e11 * 0.73 / 1.0e10;
  const double mass_ratio = mvir / M_shock;
  const double Mstar = test_exp10(TEST_MSTAR_MILL[6]) * 0.73 / 1.0e10; /* z = 3 is grid point 6 */
  const double fMstar = 3.0 * Mstar;
  const double ratio = pow(fMstar / mvir, 2.0 / 3.0) * pow(mass_ratio, 4.0 / 3.0);
  const double exponent = log10(ratio) / 0.15;
  const double f_stream = 1.0 / (1.0 + test_exp10(exponent));
  TEST_ASSERT(l.stream.z == 3.0, "the stored snapshot's redshift is read");
  TEST_ASSERT(l.stream.M_shock == M_shock && l.stream.mass_ratio == mass_ratio, "M_shock, ratio");
  TEST_ASSERT(l.stream.fMstar == fMstar && fMstar < M_shock, "fMstar below M_shock: no cutoff");
  TEST_ASSERT(l.stream.ratio == ratio, "the stream ratio");
  TEST_ASSERT(l.stream.exponent == exponent && l.stream.exponent_capped_high == 0 &&
                  l.stream.exponent_capped_low == 0,
              "the exponent, unclamped");
  TEST_ASSERT(l.stream.cutoff == 0 && l.stream.f_stream == f_stream, "the sigmoid f_stream");
  /* The hot recipe's x and rcool from the literal inputs */
  const double temp = 35.9 * vvir * vvir;
  const double logZ = log10(0.1f / hot);
  const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
  double x = PROTONMASS * BOLTZMANN * temp / lambda;
  x /= (MimicConfig.UnitDensity_in_cgs * MimicConfig.UnitTime_in_s);
  const double rho_rcool = x / tcool * (1.5 * 0.59);
  const double rho0 = hot / (4 * M_PI * rvir);
  const double rcool = sqrt(rho0 / rho_rcool);
  TEST_ASSERT(l.x == x && l.rcool == rcool, "x and rcool from the inline hot chain");
  const double cold = f_stream * hot / tcool * dt;
  const double hot_halo = (1.0 - f_stream) * (hot / rvir) * (rcool / (2.0 * tcool)) * dt;
  TEST_ASSERT(l.stream.cold_stream_cooling == cold && l.stream.hot_halo_cooling == hot_halo,
              "both components");
  TEST_ASSERT(l.stream.mdot_cool == (float)(hot_halo / dt) &&
                  l.stream.mdot_stream == (float)(cold / dt),
              "the float mdot stores");
  TEST_ASSERT(cooling == cold + hot_halo && l.cap == MIMIC_SAGE_CAP_NONE, "the uncapped total");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_streams_cutoff_equality
 * @brief   The cutoff fMstar >= M_shock && mass_ratio > 1 at its equalities
 *
 * Expected: with M_shock equal to fMstar (a shock mass found by stepping MShockMsun one double at
 * a time until (MShockMsun * h) / 1e10 equals fMstar) and Mvir above it, f_stream is 0; one
 * MShockMsun step above, where M_shock exceeds fMstar, the cutoff does not bind; at mass_ratio
 * exactly 1 it does not bind either.
 */
int test_streams_cutoff_equality(void) {
  struct Halo halo;
  struct GalaxyData gal;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_stream_galaxy(&halo, &gal, 1.0);
  const double dt = 1.0e-4;
  const double fMstar = 3.0 * (test_exp10(TEST_MSTAR_MILL[2]) * 0.73 / 1.0e10); /* z = 1 */
  double mshock = fMstar * 1.0e10 / 0.73;
  for (int i = 0; i < 64; i++) {
    mshock = nextafter(mshock, 0.0);
  }
  int found = 0;
  for (int i = 0; i < 128 && !found; i++) {
    if (mshock * 0.73 / 1.0e10 == fMstar) {
      found = 1;
    } else {
      mshock = nextafter(mshock, INFINITY);
    }
  }
  TEST_ASSERT(found, "a shock mass with M_shock == fMstar exists near fMstar * 1e10 / h");
  TEST_ASSERT(100.0 > fMstar, "Mvir 100 exceeds M_shock, so mass_ratio > 1");

  struct MimicSageHotCoolingLocals l;
  double cooling;
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, mshock, &l, &cooling) == 0,
              "call succeeds");
  TEST_ASSERT(l.stream.fMstar == fMstar && l.stream.M_shock == fMstar,
              "the equality is reached: fMstar and M_shock both equal the inline fMstar");
  TEST_ASSERT(l.stream.cutoff == 1 && l.stream.f_stream == 0.0, "fMstar >= M_shock cuts off");
  TEST_ASSERT(l.stream.cold_stream_cooling == 0.0 && l.stream.mdot_stream == 0.0f,
              "no cold streams after the cutoff");

  double above = mshock;
  while (above * 0.73 / 1.0e10 <= fMstar) {
    above = nextafter(above, INFINITY);
  }
  setup_stream_galaxy(&halo, &gal, 1.0);
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, above, &l, &cooling) == 0,
              "call succeeds");
  TEST_ASSERT(l.stream.M_shock == above * 0.73 / 1.0e10 && above * 0.73 / 1.0e10 > fMstar &&
                  l.stream.cutoff == 0 && l.stream.f_stream > 0.0,
              "just below the equality the streams survive");

  /* mass_ratio exactly 1: Mvir 100 and M_shock 100, at z = 0.5 where fMstar exceeds 100 */
  double unit = 100.0 * 1.0e10 / 0.73;
  for (int i = 0; i < 64; i++) {
    unit = nextafter(unit, 0.0);
  }
  while ((float)100.0 / (unit * 0.73 / 1.0e10) != 1.0) {
    unit = nextafter(unit, INFINITY);
  }
  setup_stream_galaxy(&halo, &gal, 0.5);
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, unit, &l, &cooling) == 0,
              "call succeeds");
  const double fMstar_half = 3.0 * (test_exp10(TEST_MSTAR_MILL[1]) * 0.73 / 1.0e10); /* z = 0.5 */
  TEST_ASSERT(l.stream.mass_ratio == 1.0 && l.stream.fMstar == fMstar_half &&
                  fMstar_half >= unit * 0.73 / 1.0e10,
              "mass_ratio is exactly 1 with fMstar above M_shock");
  TEST_ASSERT(l.stream.cutoff == 0, "mass_ratio > 1 is strict: no cutoff at 1");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_streams_exponent_clamps_and_nonpositive_ratio
 * @brief   The exponent clamps above 300 and below -300, and ratio <= 0
 *
 * Expected: MShockMsun 1e-25 at z = 0.5 clamps the exponent to 300 exactly (the cutoff then
 * zeroes f_stream); Mvir 1e-30 with MShockMsun 1.37e50 clamps it to -300 exactly and f_stream is
 * 1 / (1 + 1e-300) = 1; MShockMsun 1e300 underflows ratio to 0, so f_stream is 1, mdot_cool 0 and
 * the mass HotGas / tcool * dt.
 */
int test_streams_exponent_clamps_and_nonpositive_ratio(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageHotCoolingLocals l;
  double cooling;
  const double dt = 1.0e-4;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");

  setup_stream_galaxy(&halo, &gal, 0.5);
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, 1.0e-25, &l, &cooling) == 0,
              "call succeeds");
  TEST_ASSERT(l.stream.exponent_capped_high == 1 && l.stream.exponent == 300.0,
              "the exponent is clamped to 300");
  TEST_ASSERT(l.stream.f_stream_criterion == 1.0 / (1.0 + test_exp10(300.0)),
              "the clamped sigmoid");

  setup_stream_galaxy(&halo, &gal, 0.0);
  halo.Mvir = 1.0e-30;
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, 1.37e50, &l, &cooling) == 0,
              "call succeeds");
  TEST_ASSERT(l.stream.ratio > 0.0 && l.stream.exponent_capped_low == 1 &&
                  l.stream.exponent == -300.0,
              "the exponent is clamped to -300");
  TEST_ASSERT(l.stream.cutoff == 0 && l.stream.f_stream == 1.0, "f_stream is 1");

  setup_stream_galaxy(&halo, &gal, 1.0);
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, 1.0e300, &l, &cooling) == 0,
              "call succeeds");
  TEST_ASSERT(l.stream.ratio == 0.0 && l.stream.ratio_nonpositive == 1, "ratio underflows to 0");
  TEST_ASSERT(l.stream.f_stream == 1.0 && l.stream.mdot_cool == 0.0f,
              "ratio <= 0 gives f_stream 1");
  const double tcool = 0.1f / 200.0f;
  TEST_ASSERT(cooling == 1.0 * 10.0f / tcool * dt, "every hot gas mass is a cold stream");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_streams_refuse_stored_snapnum_outside_table
 * @brief   A stored SnapNum outside the redshift table is refused through the streams
 *
 * Expected: with a positive dt and the stored SnapNum 64 (Snaplistlen 64) or -1, the CGMrecipeOn
 * 1 budget returns -1 (SAGE26 would read ZZ out of bounds); CGMrecipeOn 0 does not read the
 * table and accepts the same galaxy.
 */
int test_streams_refuse_stored_snapnum_outside_table(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageHotCoolingLocals l;
  double cooling;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_stream_galaxy(&halo, &gal, 1.0);
  halo.SnapNum = 64;
  halo.dT = 0.01;
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, 1.0e-4, 1, 6.0e11, &l, &cooling) ==
                  -1,
              "SnapNum 64 is outside ZZ: refused");
  /* The refusal state: the path is not evaluated and no stream local was formed */
  struct MimicSageColdStreamLocals no_stream;
  memset(&no_stream, 0, sizeof(no_stream));
  TEST_ASSERT(l.path == SAGE_COOLING_PATH_NOT_EVALUATED, "the refused path is not evaluated");
  TEST_ASSERT(memcmp(&l.stream, &no_stream, sizeof(no_stream)) == 0, "the stream locals are zero");
  /* The composed recipe returns 0 on the refusal */
  setup_stream_galaxy(&halo, &gal, 1.0);
  halo.SnapNum = 64;
  halo.dT = 0.01;
  const struct MimicSageAgnConfig agn_off = {0, 0.08};
  cooling = -5.0;
  TEST_ASSERT(mimic_sage_cooling_recipe_hot(&halo, &MimicConfig, &agn_off, 1, 6.0e11, 1.0e-4, &l,
                                            NULL, &cooling) == -1 &&
                  cooling == 0.0,
              "the composed recipe refuses with a zero mass");
  setup_stream_galaxy(&halo, &gal, 1.0);
  halo.SnapNum = -1;
  halo.dT = 0.01;
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, 1.0e-4, 1, 6.0e11, &l, &cooling) ==
                  -1,
              "SnapNum -1 is outside ZZ: refused");
  setup_stream_galaxy(&halo, &gal, 1.0);
  halo.SnapNum = 64;
  TEST_ASSERT(mimic_sage_hot_cooling_budget(&halo, &MimicConfig, 1.0e-4, 0, 6.0e11, &l, &cooling) ==
                  0,
              "CGMrecipeOn 0 reads no redshift");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/** @brief A CGM-regime central for the CGM cooling tests */
static void setup_cgm_galaxy(struct Halo *halo, struct GalaxyData *gal) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  MimicConfig.G = 43.007077856420629;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitTime_in_s = 3.08568e19;
  MimicConfig.UnitDensity_in_cgs = 6.7698980144406312e-31;
  halo->galaxy = gal;
  halo->Mvir = 100.0;
  halo->Rvir = 0.1;
  halo->Vvir = 200.0;
  gal->CGMgas = 5.0f;
  gal->MetalsCGMgas = 0.05f;
  gal->tcool = -11.0f;
  gal->tff = -22.0f;
  gal->RcoolToRvir = -33.0f;
}

/**
 * @brief SAGE26's bulk CGM cooling mass for setup_cgm_galaxy's literal inputs, inline
 */
static double expected_cgm_mass(const double dt) {
  const float mvir = 100.0f, rvir = 0.1f, vvir = 200.0f, cgm = 5.0f, zcgm = 0.05f;
  const double temp = 35.9 * vvir * vvir;
  const double lambda = get_metaldependent_cooling_rate(log10(temp), log10(zcgm / cgm));
  double x = PROTONMASS * BOLTZMANN * temp / lambda;
  x /= (MimicConfig.UnitDensity_in_cgs * MimicConfig.UnitTime_in_s);
  const double alpha = 1.4, r0 = 0.1 * rvir, ratio = 10.0;
  const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);
  const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha);
  const double rho0 = cgm / (4.0 * M_PI * r0 * r0 * r0 * I_M);
  const double rho_eff = rho0 * (I_cool / I_M);
  const double tcool = (x / rho_eff) * (1.5 * 0.59);
  const double g_accel = MimicConfig.G * mvir / (rvir * rvir);
  const double tff = sqrt(2.0 * rvir / g_accel);
  return (cgm / (tcool + tff)) * dt;
}

/**
 * @test    test_cgm_cooling_profile_and_times
 * @brief   Bulk CGM cooling: SAGE26's Carr et al. profile chain written inline
 *
 * Expected: alpha 1.4, r0 = 0.1 Rvir, Rvir / r0 = 10, the integrals, rho0, rho_eff, tcool with
 * 1.5 * 0.59, g_accel with the float Rvir * Rvir, tff, the Gyr diagnostics and the uncapped mass
 * equal the inline expressions; RcoolToRvir is not written and rcool is Rvir.
 */
int test_cgm_cooling_profile_and_times(void) {
  struct Halo halo;
  struct GalaxyData gal;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  setup_cgm_galaxy(&halo, &gal);
  const double dt = 1.0e-4;
  struct MimicSageCgmCoolingLocals l;
  const double cooling = mimic_sage_cgm_cooling_budget(&halo, &MimicConfig, dt, &l);

  const float mvir = 100.0f, rvir = 0.1f, vvir = 200.0f, cgm = 5.0f, zcgm = 0.05f;
  const double temp = 35.9 * vvir * vvir;
  const double logZ = log10(zcgm / cgm);
  const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
  double x = PROTONMASS * BOLTZMANN * temp / lambda;
  x /= (MimicConfig.UnitDensity_in_cgs * MimicConfig.UnitTime_in_s);
  const double alpha = 1.4;
  const double r0 = 0.1 * rvir;
  const double ratio = 10.0;
  const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);
  const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha);
  const double rho0 = cgm / (4.0 * M_PI * r0 * r0 * r0 * I_M);
  const double rho_eff = rho0 * (I_cool / I_M);
  const double tcool = (x / rho_eff) * (1.5 * 0.59);
  const double g_accel = MimicConfig.G * mvir / (rvir * rvir);
  const double tff = sqrt(2.0 * rvir / g_accel);
  const double mass = (cgm / (tcool + tff)) * dt;

  TEST_ASSERT(l.entered == 1 && l.lambda_positive == 1 && l.lambda == lambda, "the guards pass");
  TEST_ASSERT(l.x == x && l.rho0 == rho0 && l.rho_eff == rho_eff, "x, rho0 and rho_eff");
  TEST_ASSERT(l.tcool == tcool && l.g_accel == g_accel && l.tff == tff, "tcool, g_accel and tff");
  TEST_ASSERT(gal.tcool == (float)(tcool * MimicConfig.UnitTime_in_s / SAGE_SEC_PER_GIGAYEAR) &&
                  gal.tff == (float)(tff * MimicConfig.UnitTime_in_s / SAGE_SEC_PER_GIGAYEAR),
              "the Gyr diagnostics");
  TEST_ASSERT(l.rcool == (double)rvir && gal.RcoolToRvir == -33.0f,
              "rcool is Rvir and RcoolToRvir is not written");
  TEST_ASSERT(mass < cgm && cooling == mass && l.cap == MIMIC_SAGE_CAP_NONE, "the uncapped mass");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_cgm_cooling_guard_cap_and_floor
 * @brief   The lambda > 0 guard, the cap to CGMgas and the floor at zero
 *
 * Expected: CGMgas and MetalsCGMgas +inf make the float metallicity inf / inf NaN, so lambda is
 * NaN and fails lambda > 0: nothing is written and the mass is 0; dt 10 makes the mass exceed
 * CGMgas, capped to it exactly; a negative dt makes it negative, floored at 0; Vvir 0 is not
 * entered and leaves the diagnostics.
 */
int test_cgm_cooling_guard_cap_and_floor(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageCgmCoolingLocals l;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");

  setup_cgm_galaxy(&halo, &gal);
  gal.CGMgas = INFINITY;
  gal.MetalsCGMgas = INFINITY;
  TEST_ASSERT(mimic_sage_cgm_cooling_budget(&halo, &MimicConfig, 1.0e-4, &l) == 0.0,
              "a NaN cooling function cools nothing");
  TEST_ASSERT(l.entered == 1 && l.lambda_positive == 0 && isnan(l.lambda), "lambda > 0 fails");
  TEST_ASSERT(gal.tcool == -11.0f && gal.tff == -22.0f, "the stale diagnostics survive");

  setup_cgm_galaxy(&halo, &gal);
  TEST_ASSERT(mimic_sage_cgm_cooling_budget(&halo, &MimicConfig, 10.0, &l) == (double)5.0f,
              "the mass is capped to CGMgas");
  TEST_ASSERT(l.cap == MIMIC_SAGE_CAP_RESERVOIR && l.cooling_precap > 5.0, "the cap binds");

  setup_cgm_galaxy(&halo, &gal);
  TEST_ASSERT(mimic_sage_cgm_cooling_budget(&halo, &MimicConfig, -1.0e-4, &l) == 0.0,
              "a negative mass is floored at 0");
  TEST_ASSERT(l.cap == MIMIC_SAGE_CAP_FLOOR && l.cooling_precap < 0.0, "the floor binds");

  setup_cgm_galaxy(&halo, &gal);
  halo.Vvir = 0.0;
  TEST_ASSERT(mimic_sage_cgm_cooling_budget(&halo, &MimicConfig, 1.0e-4, &l) == 0.0 &&
                  l.entered == 0 && gal.tcool == -11.0f,
              "Vvir 0 is not entered");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_composition_tail
 * @brief   mimic_sage_cooling_recipe_cgm with AGN off: Cooling and CoolingRate
 *
 * Expected: Cooling grows by 0.5 * m * Vvir^2 and CoolingRate is SAGE26's float rate of the mass;
 * on the NaN-lambda state Cooling is left and CoolingRate is written 0 (always).
 */
int test_cgm_recipe_composition_tail(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageCgmCoolingLocals l;
  const struct MimicSageAgnConfig off = {0, 0.08};
  const double dt = 1.0e-4;
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");

  setup_cgm_galaxy(&halo, &gal);
  gal.Cooling = 2.0;
  gal.CoolingRate = -44.0f;
  const double ret = mimic_sage_cooling_recipe_cgm(&halo, &MimicConfig, &off, 1, dt, &l, NULL);
  const double m = expected_cgm_mass(dt); /* below CGMgas: no cap, and AGN is off */
  TEST_ASSERT(ret == m, "the composed recipe returns SAGE26's mass");
  const float vvir = 200.0f;
  TEST_ASSERT(m > 0.0 && gal.Cooling == 2.0 + 0.5 * m * vvir * vvir, "Cooling accumulates");
  TEST_ASSERT(gal.CoolingRate == (float)((m / dt) * 1.0e10 / 0.73 * 3.155e16 / 3.08568e19),
              "CoolingRate");

  setup_cgm_galaxy(&halo, &gal);
  gal.CGMgas = INFINITY;
  gal.MetalsCGMgas = INFINITY;
  gal.Cooling = 2.0;
  gal.CoolingRate = -44.0f;
  TEST_ASSERT(mimic_sage_cooling_recipe_cgm(&halo, &MimicConfig, &off, 1, dt, &l, NULL) == 0.0,
              "no mass");
  TEST_ASSERT(gal.Cooling == 2.0 && gal.CoolingRate == 0.0f,
              "CoolingRate is written 0 even when lambda fails");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

// ============================================================================
// SUITE 5: CGMrecipeOn
// ============================================================================

extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

/**
 * @test    test_cgm_recipe_on_refusals
 * @brief   CGMrecipeOn is required and must be 0 or 1
 *
 * Expected: init() fails without the key and with CGMrecipeOn 2, naming nothing it can default
 * to, and succeeds with 0.
 */
int test_cgm_recipe_on_refusals(void) {
  reset_config();
  TEST_ASSERT(sage26_calculate_cooling_budget_init() != 0, "a missing CGMrecipeOn is refused");
  reset_config();
  sage26_test_append_model_parameter("CGMrecipeOn", "2");
  TEST_ASSERT(sage26_calculate_cooling_budget_init() != 0, "CGMrecipeOn 2 is refused");
  reset_config();
  sage26_test_append_model_parameter("CGMrecipeOn", "0");
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "CGMrecipeOn 0 is accepted");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_cgm_on_writes_only_transport
 * @brief   With CGMrecipeOn 1 the classic modules write no galaxy property
 *
 * A cooling central with a black hole, which the classic trio would cool, heat and transfer
 * (test_tail_writes_carried_state): with CGMrecipeOn 1 (and the configuration the guard requires)
 * the budget writes CoolingGas = CoolingX = Rcool = 0 and CoolingReturnPath = -1 and nothing
 * else, and the radio (AGNrecipe 2) and apply modules then change no byte.
 *
 * Expected: the galaxy after each module equal, byte for byte, to the input with the four
 * transport properties set to the no-cooling literals.
 */
int test_cgm_on_writes_only_transport(void) {
  struct Halo halo;
  struct GalaxyData gal, expected;
  struct ModuleContext ctx;
  reset_config();
  init_memory_system(0);
  test_pre_timestep_add("sage26_determine_regime", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_radio_mode_heating", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_cooling", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_regime_aware_cooling", PROCESSING_MODE_BY_GALAXY);
  sage26_test_append_model_parameter("CGMrecipeOn", "1");
  sage26_test_append_model_parameter("AGNrecipe", "2");
  sage26_test_append_model_parameter("RadioModeEfficiency", "0.08");
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "budget init with CGMrecipeOn 1");
  TEST_ASSERT(sage26_radio_mode_heating_init() == 0, "radio init");
  setup_cooling_galaxy(&halo, &gal, &ctx);
  gal.BlackHoleMass = 0.1f;
  gal.ColdGas = 1.0f;
  gal.Cooling = 2.0;
  gal.Heating = 3.0;
  gal.CoolingRate = -44.0f;

  memcpy(&expected, &gal, sizeof(gal)); /* padding included, for memcmp */
  expected.CoolingGas = 0.0;
  expected.CoolingX = 0.0;
  expected.Rcool = 0.0;
  expected.CoolingReturnPath = SAGE_COOLING_PATH_NOT_EVALUATED;

  TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx, &halo, 1) == 0, "budget process");
  TEST_ASSERT(memcmp(&gal, &expected, sizeof(gal)) == 0, "the budget writes only its transport");
  TEST_ASSERT(sage26_radio_mode_heating_process(&ctx, &halo, 1) == 0, "radio process");
  TEST_ASSERT(memcmp(&gal, &expected, sizeof(gal)) == 0, "the radio module writes nothing");
  TEST_ASSERT(sage26_apply_cooling_process(&ctx, &halo, 1) == 0, "apply process");
  TEST_ASSERT(memcmp(&gal, &expected, sizeof(gal)) == 0, "the apply module writes nothing");

  sage26_calculate_cooling_budget_cleanup();
  test_free_substep_phases();
  test_free_pre_timestep();
  return TEST_PASS;
}

/**
 * @brief Main test runner — executes all sage26_calculate_cooling_budget unit tests
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_calculate_cooling Module\n");
  printf("============================================================\n");
  printf("%s", NC);
  printf("\n");
  printf("Suite 1: Cooling Tables (helper functions)\n");
  printf("------------------------------------------------------------\n");

  TEST_RUN(test_cooling_tables_loading);
  TEST_RUN(test_temperature_interpolation);
  TEST_RUN(test_metallicity_interpolation);
  TEST_RUN(test_primordial_gas_cooling);
  TEST_RUN(test_super_solar_metallicity);
  TEST_RUN(test_extreme_temperatures);

  printf("\n");
  printf("Suite 2: Module Integration\n");
  printf("------------------------------------------------------------\n");

  TEST_RUN(test_module_registration);
  TEST_RUN(test_memory_safety);

  printf("\n");
  printf("Suite 3: Carried State\n");
  printf("------------------------------------------------------------\n");

  TEST_RUN(test_outer_guard_writes_carried_state);
  TEST_RUN(test_tail_writes_carried_state);
  TEST_RUN(test_no_interval_marks_not_evaluated);

  printf("\n");
  printf("Suite 4: CGMrecipeOn 1 shared helpers (clustering table, streams, CGM cooling)\n");
  printf("------------------------------------------------------------\n");

  TEST_RUN(test_clustering_table_grid_points);
  TEST_RUN(test_clustering_table_between_and_clamps);
  TEST_RUN(test_clustering_table_cosmology_boundary);
  TEST_RUN(test_streams_sigmoid_from_stored_snapshot);
  TEST_RUN(test_streams_cutoff_equality);
  TEST_RUN(test_streams_exponent_clamps_and_nonpositive_ratio);
  TEST_RUN(test_streams_refuse_stored_snapnum_outside_table);
  TEST_RUN(test_cgm_cooling_profile_and_times);
  TEST_RUN(test_cgm_cooling_guard_cap_and_floor);
  TEST_RUN(test_cgm_recipe_composition_tail);

  printf("\n");
  printf("Suite 5: CGMrecipeOn\n");
  printf("------------------------------------------------------------\n");

  TEST_RUN(test_cgm_recipe_on_refusals);
  TEST_RUN(test_cgm_on_writes_only_transport);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
