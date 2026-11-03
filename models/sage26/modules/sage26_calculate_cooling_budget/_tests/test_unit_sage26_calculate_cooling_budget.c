/**
 * @file    test_unit_sage26_calculate_cooling_budget.c
 * @brief   Unit tests for sage26_calculate_cooling_budget module
 *
 * Validates: Module lifecycle, cooling tables, interpolation, memory safety, and the
 * carried state process() writes on every call. Bit-exact physics against SAGE26's
 * cooling_recipe_hot is tested by test_unit_sage26_calculate_cooling_budget_oracle.c.
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

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_constants.h"

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
 * @brief   SAGE26's outer guard (HotGas > 0 && Vvir > 0, model_cooling_heating.c:139-141)
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

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
