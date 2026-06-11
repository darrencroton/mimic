/**
 * @file    test_unit_sage_calculate_cooling.c
 * @brief   Unit tests for sage_calculate_cooling module
 *
 * Validates: Module lifecycle, cooling tables, interpolation, memory safety
 * Phase: Phase 4.2 (SAGE Physics Module Implementation)
 *
 * This test validates software engineering aspects of the sage_calculate_cooling module:
 * - Module registration and initialization
 * - Cooling table loading and validation
 * - Temperature and metallicity interpolation (2D)
 * - Edge case handling (primordial gas, super-solar Z, extreme temps)
 * - Memory allocation and cleanup (no leaks)
 *
 * Test Organization:
 *   Suite 1: Cooling Tables (helper functions)
 *     - Table loading from disk
 *     - Temperature interpolation (1D)
 *     - Metallicity interpolation (2D)
 *     - Edge cases (primordial, super-solar, extreme temps)
 *
 *   Suite 2: Module Integration
 *     - Module registration
 *     - Memory safety
 *
 * NOTE:
 * - Core physics (cooling_recipe) tested indirectly via process function in integration tests
 * - This module has NO runtime parameters (CoolFunctions path is hardcoded)
 * - Physics validation (comparison with SAGE) handled in scientific tests
 *
 * @author  Mimic Development Team
 * @date    2025-11-13 (Updated 2025-12-18)
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

/**
 * @test    test_module_registration
 * @brief   Test that sage_cooling module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_cooling_register() works, module appears in registry
 */
int test_module_registration(void) {
  ensure_modules_registered();

  /* Check module is registered by trying to enable it */
  reset_config();
  test_phase_add("galaxy_physics", "sage_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

  /* Should fail if module not registered */
  int result = module_system_init();

  /* Clean up */
  if (result == 0) {
    module_system_cleanup();
  }

  TEST_ASSERT(result == 0, "sage_cooling module should register successfully");
  return 0;
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
      MIMIC_COMPILED_MODEL_PATH "/modules/sage_calculate_cooling_budget/CoolFunctions";

  int result = cooling_tables_init(cool_dir);

  TEST_ASSERT(result == 0, "Cooling tables should load successfully from module directory");

  /* Clean up */
  cooling_tables_cleanup();

  return 0;
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
      MIMIC_COMPILED_MODEL_PATH "/modules/sage_calculate_cooling_budget/CoolFunctions";

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
  return 0;
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
      MIMIC_COMPILED_MODEL_PATH "/modules/sage_calculate_cooling_budget/CoolFunctions";

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
  return 0;
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
      MIMIC_COMPILED_MODEL_PATH "/modules/sage_calculate_cooling_budget/CoolFunctions";

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
  return 0;
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
      MIMIC_COMPILED_MODEL_PATH "/modules/sage_calculate_cooling_budget/CoolFunctions";

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
  return 0;
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
      MIMIC_COMPILED_MODEL_PATH "/modules/sage_calculate_cooling_budget/CoolFunctions";

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
  return 0;
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
  test_phase_add("galaxy_physics", "sage_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module init must succeed for memory test");

  /* Clean up */
  module_system_cleanup();

  /* Check for leaks */
  check_memory_leaks();

  return 0;
}

/* Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_calculate_cooling Module\n");
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

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
