/**
 * @file    test_unit_sage_calculate_cooling_budget.c
 * @brief   Unit tests for sage_calculate_cooling_budget module
 *
 * Validates: the cooling function tables, the module lifecycle and the cooling budget process()
 * writes (the hot-halo and rapid cooling branches and the HotGas cap).
 *
 * Test cases:
 *   Suite 1: Cooling Tables
 *     - test_cooling_function_interpolation: bracket interpolation in logT and logZ against table
 *       literals, the low-end clamps and the linear extrapolation above logT 8.5
 *     - test_super_solar_metallicity: logZ above the last table clamps to the maximum table
 *
 *   Suite 2: Module
 *     - test_module_initialization: the module initialises, cleans up and leaks nothing
 *     - test_hot_cooling_branches: the hot-halo and rapid cooling masses and the HotGas cap
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
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"
#include "module_system/physical_constants.h"

extern int sage_calculate_cooling_budget_init(void);
extern int sage_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage_calculate_cooling_budget_cleanup(void);

// ============================================================================
// SUITE 1: COOLING TABLES
// ============================================================================

/** log10(Z_SUN) with Z_SUN = 0.02: the table's metallicity axis is [Fe/H] plus this */
#define TEST_LOG_ZSUN (-1.6989700043360187)

/**
 * @test    test_cooling_function_interpolation
 * @brief   The cooling function interpolates the Sutherland & Dopita tables linearly in logT and
 *          logZ, clamps the low ends and extrapolates above logT 8.5
 *
 * Expected: each row is the bracketing table entries (stripped_*.cie column 6 at logT 5.50, 5.55,
 * 4.00, 5.90, 5.95, 8.45 and 8.50, read from the files and held as the floats the loader stores)
 * combined linearly, then 10^x. The metallicity nodes are [Fe/H] -1.0, -0.5, 0 and +0.5 plus
 * log10(Z_sun). A logT above 8.5 continues along the last interval: the high-T row is 0.2 dex above
 * the value a clamp at logT 8.5 would give. The relative tolerance 1e-9 covers only the order of
 * the floating-point operations.
 */
int test_cooling_function_interpolation(void) {
  static const struct {
    const char *what;
    double logT;
    double logZ;
    double expected_log10_lambda;
  } rows[] = {
      {"grid node in T and Z", 5.5, TEST_LOG_ZSUN, (double)-21.35f},
      {"between nodes in T", 5.525, TEST_LOG_ZSUN, 0.5 * ((double)-21.35f + (double)-21.55f)},
      {"between nodes in Z", 5.5, TEST_LOG_ZSUN - 0.75, 0.5 * ((double)-21.84f + (double)-21.60f)},
      {"solar Z at logT log10(35.9 * 150^2), the cooling budget's Vvir 150 row", 5.907276966689682,
       TEST_LOG_ZSUN, (double)-21.93f + 0.14553933379362505 * ((double)-21.95f - (double)-21.93f)},
      {"between nodes in T and Z", 5.525, TEST_LOG_ZSUN - 0.75,
       0.5 *
           (0.5 * ((double)-21.84f + (double)-22.03f) + 0.5 * ((double)-21.60f + (double)-21.79f))},
      {"below the lowest Z: the primordial table", 5.5, -10.0, (double)-23.13f},
      {"below the lowest T: clamped to logT 4.0", 3.0, TEST_LOG_ZSUN, (double)-23.06f},
      {"above the highest T: extrapolated along the last interval", 9.0, TEST_LOG_ZSUN,
       (double)-22.40f + 11.0 * ((double)-22.38f - (double)-22.40f)},
  };

  TEST_ASSERT(cooling_tables_init(MIMIC_COMPILED_MODEL_PATH
                                  "/modules/sage_calculate_cooling_budget/CoolFunctions") == 0,
              "the cooling tables must load");
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const double expected = pow(10.0, rows[k].expected_log10_lambda);
    const double lambda = get_metaldependent_cooling_rate(rows[k].logT, rows[k].logZ);
    printf("\n  %s: lambda %.6e, expected %.6e", rows[k].what, lambda, expected);
    TEST_ASSERT(fabs(lambda / expected - 1.0) < 1.0e-9, rows[k].what);
  }
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
  return TEST_PASS;
}

// ============================================================================
// SUITE 2: MODULE
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   The module initialises in a galaxy_physics phase and cleans up
 *
 * Expected: module_system_init succeeds (the tables load), and init and cleanup leak nothing
 */
int test_module_initialization(void) {
  ensure_modules_registered();

  /* Initialize module */
  reset_config();
  test_phase_add("galaxy_physics", "sage_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* Clean up */
  module_system_cleanup();

  /* Check for leaks */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_hot_cooling_branches
 * @brief   The cooling mass: the hot-halo (static) and rapid branches and the HotGas cap
 *
 * Expected: with SAGE's chain written inline (tcool = Rvir / Vvir, T = 35.9 Vvir^2, x,
 * rho_rcool = x / tcool * 0.885, rho0 = HotGas / (4 pi Rvir), rcool), a galaxy with rcool <= Rvir
 * is carried (HotGas / Rvir) * (rcool / (2 tcool)) * dt, one with rcool > Rvir
 * HotGas / tcool * dt (the two differ by a factor 2 at rcool = Rvir, SAGE parity), and a mass
 * above HotGas is carried as exactly HotGas. dt is dT / num_substeps. Each row names the branch
 * and cap it must reach, checked against the inline rcool, so an input change cannot silently
 * move a row to another branch. The cooling function lambda comes from the module's tables.
 */
int test_hot_cooling_branches(void) {
  static const struct {
    const char *what;
    float hot_gas;
    double dT;
    int rapid;
    int capped;
  } rows[] = {
      {"hot-halo branch: rcool inside Rvir", 1.0f, 0.002, 0, 0},
      {"rapid branch: rcool beyond Rvir", 50.0f, 0.002, 1, 0},
      {"rapid branch, mass above HotGas: capped to HotGas", 50.0f, 0.1, 1, 1},
  };

  reset_config();
  TEST_ASSERT(sage_calculate_cooling_budget_init() == 0, "module init must succeed");
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct Halo halo;
    struct GalaxyData gal;
    struct ModuleContext ctx;
    memset(&halo, 0, sizeof(halo));
    memset(&gal, 0, sizeof(gal));
    memset(&ctx, 0, sizeof(ctx));
    MimicConfig.UnitTime_in_s = 3.08568e19;
    MimicConfig.UnitDensity_in_cgs = 6.7698980144406312e-31;
    ctx.params = &MimicConfig;
    ctx.num_substeps = 10;
    halo.galaxy = &gal;
    halo.Type = 0;
    halo.SnapNum = 40;
    halo.dT = rows[k].dT;
    halo.Mvir = 100.0;
    halo.Rvir = 0.2;
    halo.Vvir = 150.0;
    gal.HotGas = rows[k].hot_gas;
    gal.MetalsHotGas = 0.02f * rows[k].hot_gas; /* Z = 0.02 */
    gal.CoolingGas = -7.0;
    TEST_ASSERT(sage_calculate_cooling_budget_process(&ctx, &halo, 1) == 0, "process succeeds");

    /* SAGE's chain, inline; the reservoirs are floats */
    const double rvir = 0.2, vvir = 150.0;
    const float hot = rows[k].hot_gas;
    const double dt = rows[k].dT / 10.0;
    const double tcool = rvir / vvir;
    const double temp = 35.9 * vvir * vvir;
    const double logZ = log10(gal.MetalsHotGas / hot);
    const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
    double x = PROTONMASS * BOLTZMANN * temp / lambda;
    x /= (MimicConfig.UnitDensity_in_cgs * MimicConfig.UnitTime_in_s);
    const double rho_rcool = x / tcool * 0.885;
    const double rho0 = hot / (4.0 * M_PI * rvir);
    const double rcool = sqrt(rho0 / rho_rcool);
    double expected =
        (rcool > rvir) ? hot / (rvir / vvir) * dt : (hot / rvir) * (rcool / (2.0 * tcool)) * dt;
    const int capped = expected > hot;
    if (capped) {
      expected = hot;
    }

    printf("\n  %s: rcool / Rvir %.4f, mass %.6e of HotGas %g", rows[k].what, rcool / rvir,
           gal.CoolingGas, (double)hot);
    TEST_ASSERT((rcool > rvir) == rows[k].rapid, "the row reaches the branch it names");
    TEST_ASSERT(capped == rows[k].capped, "the row reaches the cap state it names");
    TEST_ASSERT(gal.Rcool == rcool, "the uncapped cooling radius is carried");
    TEST_ASSERT(gal.CoolingLambda == lambda, "the cooling function is carried");
    TEST_ASSERT(gal.CoolingGas == expected, rows[k].what);
  }
  sage_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @brief Main test runner — executes all sage_calculate_cooling_budget unit tests
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_calculate_cooling_budget Module\n");
  printf("============================================================\n");
  printf("%s", NC);
  printf("\n");

  TEST_RUN(test_cooling_function_interpolation);
  TEST_RUN(test_super_solar_metallicity);
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_hot_cooling_branches);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
