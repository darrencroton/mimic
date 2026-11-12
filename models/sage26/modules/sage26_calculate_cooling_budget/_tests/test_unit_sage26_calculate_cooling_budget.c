/**
 * @file    test_unit_sage26_calculate_cooling_budget.c
 * @brief   Unit tests for sage26_calculate_cooling_budget module
 *
 * Validates: the cooling function tables, the hot-halo cooling budget with CGMrecipeOn 0 (the
 * rapid and static branches, the HotGas cap, the carried state process() writes on every call)
 * and the CGMrecipeOn 1 no-cooling transport. The shared CGMrecipeOn 1 helpers (clustering table,
 * cold streams, bulk CGM cooling) are tested in shared/_tests/test_unit_sage_cooling_physics.c. The
 * module's CGMrecipeOn refusals are asserted by test_unit_sage26_parameter_refusals.c.
 *
 * Test cases:
 *   Suite 1: Cooling Tables
 *     - test_cooling_function_interpolation: bracket interpolation in logT and logZ against table
 *       literals, the low-end clamps and the linear extrapolation above logT 8.5
 *     - test_super_solar_metallicity: logZ above the last table clamps to the maximum table
 *
 *   Suite 2: Module Lifecycle
 *     - test_module_initialization: the module initialises, cleans up and leaks nothing
 *
 *   Suite 3: Hot-halo Budget and Carried State (SAGE26 cooling_recipe_hot)
 *     - test_outer_guard_writes_carried_state: HotGas 0 -> path 1, every carried entry
 *       written, tcool reset to 0, tff and RcoolToRvir left as they were
 *     - test_tail_writes_carried_state: a cooling galaxy -> path 0, positive mass, x, rcool
 *     - test_no_interval_marks_not_evaluated: no positive interval -> path -1, no mass
 *     - test_hot_cooling_branches: the static and rapid cooling masses and the HotGas cap
 *
 *   Suite 4: CGMrecipeOn
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
 * 4.00, 8.45 and 8.50, read from the files and held as the floats the loader stores) combined
 * linearly, then 10^x. The metallicity nodes are [Fe/H] -1.0, -0.5, 0 and +0.5 plus log10(Z_sun).
 * A logT above 8.5 continues along the last interval: the high-T row is 0.2 dex above the value a
 * clamp at logT 8.5 would give. The last two rows are the points the cooling chains evaluate, so
 * the chains' lookups are pinned here independently of the chains: T = 35.9 Vvir^2 for Vvir 150
 * (logT 5.9073, Z = 0.02, on the solar node: stripped_m-00.cie rows logT 5.90 and 5.95, weight
 * 0.14554 of the interval) and for Vvir 200 (logT 6.1572, Z = 0.01, weight log10(2.5) = 0.39794
 * between [Fe/H] -0.5 and 0: stripped_m-05.cie and stripped_m-00.cie rows logT 6.15 and 6.20,
 * weight 0.14309 of the interval). The relative tolerance 1e-9 covers only the order of the
 * floating-point operations.
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
      {"between nodes in T and Z", 5.525, TEST_LOG_ZSUN - 0.75,
       0.5 *
           (0.5 * ((double)-21.84f + (double)-22.03f) + 0.5 * ((double)-21.60f + (double)-21.79f))},
      {"below the lowest Z: the primordial table", 5.5, -10.0, (double)-23.13f},
      {"below the lowest T: clamped to logT 4.0", 3.0, TEST_LOG_ZSUN, (double)-23.06f},
      {"above the highest T: extrapolated along the last interval", 9.0, TEST_LOG_ZSUN,
       (double)-22.40f + 11.0 * ((double)-22.38f - (double)-22.40f)},
      {"the Vvir 150 chain's point: between nodes in T, on the solar node in Z", 5.907276966689682,
       TEST_LOG_ZSUN, (double)-21.93f + 0.14553933379362505 * ((double)-21.95f - (double)-21.93f)},
      {"the Vvir 200 chain's point: between nodes in T and Z", 6.157154439906281, -2.0,
       0.6020599913279625 *
               ((double)-22.34f + 0.14308879812562125 * ((double)-22.33f - (double)-22.34f)) +
           0.3979400086720375 *
               ((double)-21.95f + 0.14308879812562125 * ((double)-21.94f - (double)-21.95f))},
  };

  TEST_ASSERT(cooling_tables_init(MIMIC_COMPILED_MODEL_PATH
                                  "/modules/sage26_calculate_cooling_budget/CoolFunctions") == 0,
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

// ============================================================================
// SUITE 2: MODULE LIFECYCLE
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   The module initialises in a CGMrecipeOn 0 galaxy_physics phase and cleans up
 *
 * Expected: module_system_init succeeds (the tables load), and init, cleanup leak nothing
 */
int test_module_initialization(void) {
  ensure_modules_registered();

  /* Initialize module */
  reset_config();
  test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* Clean up */
  module_system_cleanup();

  /* Check for leaks */
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// SUITE 3: HOT-HALO BUDGET AND CARRIED STATE
// ============================================================================

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

/**
 * @test    test_hot_cooling_branches
 * @brief   The CGMrecipeOn 0 cooling mass: the static and rapid branches and the HotGas cap
 *
 * Expected: with SAGE26's chain written inline from the float Rvir and Vvir (tcool = Rvir / Vvir,
 * T = 35.9 Vvir^2, x, rho_rcool = x / tcool * 1.5 * 0.59, rho0 = HotGas / (4 pi Rvir), rcool), a
 * galaxy with rcool <= Rvir is carried (HotGas / Rvir) * (rcool / (2 tcool)) * dt, one with
 * rcool > Rvir HotGas / tcool * dt (the two differ by a factor 2 at rcool = Rvir, SAGE parity),
 * and a mass above HotGas is carried as exactly HotGas. dt is dT / num_substeps. Each row names
 * the branch and cap it must reach, checked against the inline rcool, so an input change cannot
 * silently move a row to another branch. The cooling function lambda comes from the tables
 * (test_cooling_function_interpolation pins those).
 */
int test_hot_cooling_branches(void) {
  static const struct {
    const char *what;
    float hot_gas;
    double dT;
    int rapid;
    int capped;
  } rows[] = {
      {"static branch: rcool inside Rvir", 1.0f, 0.002, 0, 0},
      {"rapid branch: rcool beyond Rvir", 50.0f, 0.002, 1, 0},
      {"rapid branch, mass above HotGas: capped to HotGas", 50.0f, 0.1, 1, 1},
  };

  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "module init must succeed");
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct Halo halo;
    struct GalaxyData gal;
    struct ModuleContext ctx;
    setup_cooling_galaxy(&halo, &gal, &ctx);
    gal.HotGas = rows[k].hot_gas;
    gal.MetalsHotGas = 0.02f * rows[k].hot_gas; /* Z = 0.02 */
    halo.dT = rows[k].dT;
    TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx, &halo, 1) == 0, "process succeeds");

    /* SAGE26's chain, inline; Rvir, Vvir and the reservoirs are floats */
    const float rvir = 0.2f, vvir = 150.0f, hot = rows[k].hot_gas;
    const double dt = rows[k].dT / 10.0;
    const double tcool = rvir / vvir;
    const double temp = 35.9 * vvir * vvir;
    const double lambda =
        get_metaldependent_cooling_rate(log10(temp), log10(gal.MetalsHotGas / hot));
    double x = PROTONMASS * BOLTZMANN * temp / lambda;
    x /= (MimicConfig.UnitDensity_in_cgs * MimicConfig.UnitTime_in_s);
    const double rho_rcool = x / tcool * (1.5 * 0.59);
    const double rho0 = hot / (4 * M_PI * rvir);
    const double rcool = sqrt(rho0 / rho_rcool);
    double expected =
        (rcool > rvir) ? hot / tcool * dt : (hot / rvir) * (rcool / (2.0 * tcool)) * dt;
    const int capped = expected > hot;
    if (capped) {
      expected = hot;
    }

    printf("\n  %s: rcool / Rvir %.4f, mass %.6e of HotGas %g", rows[k].what, rcool / rvir,
           gal.CoolingGas, (double)hot);
    TEST_ASSERT((rcool > rvir) == rows[k].rapid, "the row reaches the branch it names");
    TEST_ASSERT(capped == rows[k].capped, "the row reaches the cap state it names");
    TEST_ASSERT(gal.CoolingReturnPath == SAGE_COOLING_PATH_TAIL, "the normal tail");
    TEST_ASSERT(gal.Rcool == rcool, "the uncapped cooling radius is carried");
    TEST_ASSERT(gal.CoolingGas == expected, rows[k].what);
  }
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

// ============================================================================
// SUITE 4: CGMrecipeOn
// ============================================================================

extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

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
 * @brief Main test runner: executes all sage26_calculate_cooling_budget unit tests
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_calculate_cooling_budget Module\n");
  printf("============================================================\n");
  printf("%s", NC);
  printf("\n");

  TEST_RUN(test_cooling_function_interpolation);
  TEST_RUN(test_super_solar_metallicity);
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_outer_guard_writes_carried_state);
  TEST_RUN(test_tail_writes_carried_state);
  TEST_RUN(test_no_interval_marks_not_evaluated);
  TEST_RUN(test_hot_cooling_branches);
  TEST_RUN(test_cgm_on_writes_only_transport);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
