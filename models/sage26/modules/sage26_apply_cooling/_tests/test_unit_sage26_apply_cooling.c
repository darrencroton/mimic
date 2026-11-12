/**
 * @file    test_unit_sage26_apply_cooling.c
 * @brief   Unit tests for sage26_apply_cooling module
 *
 * Validates: the cooling transfer from the hot to the cold reservoir with its metals, the tail of
 * SAGE26's cooling_recipe_hot (Cooling accumulated undivided, CoolingRate with the substep
 * dt = dT / num_substeps) and its early-return handling, and the edge cases (Type 2 orphans, zero
 * mass, NULL galaxy, invalid ngal, no interval).
 *
 * Test cases:
 *   - test_module_initialization: the module initialises with its predecessor placed and leaks
 *     nothing
 *   - test_physics_cooling_transfer: partial cooling (hot and cold metallicity differ or agree),
 *     complete depletion and the coolingGas == HotGas boundary, gas and metals
 *   - test_physics_cooling_energy_tracking: Cooling accumulates 0.5 m Vvir^2 undivided
 *   - test_cooling_rate_diagnostic: CoolingRate is SAGE26's conversion of mass / dt, with
 *     dt = dT / num_substeps
 *   - test_early_return_skips_tail: lambda/rho_rcool early returns leave Cooling, CoolingRate
 *   - test_zero_mass_writes_zero_rate: the outer guard and a zero-mass tail write CoolingRate 0
 *   - test_no_interval_is_noop: no positive interval (snapshot-zero birth) changes nothing
 *   - test_type2_orphan_cools: Type 2 orphans cool their own hot gas (SAGE parity)
 *   - test_null_galaxy: NULL galaxy handled gracefully
 *   - test_invalid_ngal: Error when ngal != 1
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
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

/* External module interface for direct testing */
extern int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

/* Hubble_h and UnitTime_in_s as SAGE26's default configuration sets them */
#define TEST_HUBBLE_H 0.73
#define TEST_UNIT_TIME_IN_S 3.08568e19

/**
 * @brief Module context with the run configuration and one substep (num_substeps 1)
 *
 * The module derives its substep dt from halo->dT / num_substeps and reads
 * Hubble_h and UnitTime_in_s for the CoolingRate diagnostic.
 */
static struct ModuleContext create_test_context(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  MimicConfig.Hubble_h = TEST_HUBBLE_H;
  MimicConfig.UnitTime_in_s = TEST_UNIT_TIME_IN_S;
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  return ctx;
}

/**
 * @brief Create a test halo with galaxy for physics tests
 *
 * @param type Halo type (0=central, 1=satellite, 2=orphan)
 * @param mvir Virial mass in code units (1e10 Msun/h)
 * @param vvir Virial velocity in km/s
 * @return Allocated halo (must be freed with free_test_halo)
 */
static struct Halo create_test_halo(int type, float mvir, float vvir) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));

  halo.Type = type;
  halo.Mvir = mvir;
  halo.Vvir = vvir;
  halo.SnapNum = 63; /* z=0 */
  halo.dT = 100.0;   /* dt in Myr for cooling rate calculation */

  /* Allocate galaxy data */
  halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
  memset(halo.galaxy, 0, sizeof(struct GalaxyData));

  return halo;
}

/**
 * @test    test_module_initialization
 * @brief   The module initialises with sage26_calculate_cooling_budget placed before it
 *
 * Expected: module_system_init succeeds for that placement, and init and cleanup leak nothing
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set up minimal cosmology configuration */
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  /* Configure required predecessor: sage26_calculate_cooling_budget before sage26_apply_cooling */
  test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_cooling", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_cooling_transfer
 * @brief   The cooled mass moves from the hot to the cold reservoir, metals with it
 *
 * Expected: below the hot reservoir the mass moves at the hot gas's metallicity (so a hot Z that
 * differs from the cold Z shows which reservoir's Z is used); at or above it the whole hot
 * reservoir moves, metals included, and the hot metals are exactly zero. The coolingGas == HotGas
 * row pins the strict comparison: only the whole-reservoir branch zeroes the hot metals exactly.
 * Values are hand-computed from the inputs; the 0.001 tolerance covers the float reservoirs.
 */
int test_physics_cooling_transfer(void) {
  static const struct {
    const char *what;
    float hot, metals_hot, cold, metals_cold, cooling_gas;
    double exp_hot, exp_metals_hot, exp_cold, exp_metals_cold;
  } rows[] = {
      /* Z_hot = Z_cold = 0.02: 10 - 3, 0.14 = 7 * 0.02, 0.1 + 3 * 0.02 */
      {"partial cooling, equal metallicities", 10.0f, 0.2f, 5.0f, 0.1f, 3.0f, 7.0, 0.14, 8.0, 0.16},
      /* Z_hot = 0.05, Z_cold = 0.02: 0.75 = 15 * 0.05 stays hot, 0.2 + 5 * 0.05 goes cold */
      {"partial cooling, hot Z above cold Z", 20.0f, 1.0f, 10.0f, 0.2f, 5.0f, 15.0, 0.75, 15.0,
       0.45},
      /* coolingGas 10 exceeds HotGas 5: all 5 moves, with its 0.1 metals; 0.06 + 0.1 */
      {"complete depletion", 5.0f, 0.1f, 3.0f, 0.06f, 10.0f, 0.0, 0.0, 8.0, 0.16},
      /* coolingGas == HotGas 3: the whole-reservoir branch, hot metals exactly 0 */
      {"coolingGas equal to HotGas", 3.0f, 0.1f, 2.0f, 0.04f, 3.0f, 0.0, 0.0, 5.0, 0.14},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
    test_halo.galaxy->HotGas = rows[k].hot;
    test_halo.galaxy->MetalsHotGas = rows[k].metals_hot;
    test_halo.galaxy->ColdGas = rows[k].cold;
    test_halo.galaxy->MetalsColdGas = rows[k].metals_cold;
    test_halo.galaxy->CoolingGas = rows[k].cooling_gas;
    struct ModuleContext ctx = create_test_context();

    TEST_ASSERT(sage26_apply_cooling_process(&ctx, &test_halo, 1) == 0,
                "Module processing should succeed");
    printf("\n  %s: hot %g / %g, cold %g / %g", rows[k].what, (double)test_halo.galaxy->HotGas,
           (double)test_halo.galaxy->MetalsHotGas, (double)test_halo.galaxy->ColdGas,
           (double)test_halo.galaxy->MetalsColdGas);
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, rows[k].exp_hot, 0.001,
                             "HotGas loses the cooled mass");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->ColdGas, rows[k].exp_cold, 0.001,
                             "ColdGas gains the cooled mass");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, rows[k].exp_metals_hot, 0.001,
                             "MetalsHotGas keeps the hot metallicity");
    TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsColdGas, rows[k].exp_metals_cold, 0.001,
                             "MetalsColdGas gains the cooled mass at the hot metallicity");
    if (rows[k].cooling_gas >= rows[k].hot) {
      TEST_ASSERT(test_halo.galaxy->HotGas == 0.0f && test_halo.galaxy->MetalsHotGas == 0.0f,
                  "a whole-reservoir transfer empties the hot gas and its metals exactly");
    }
    free_test_halo(&test_halo);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_physics_cooling_energy_tracking
 * @brief   Test that cooling energy is tracked correctly
 *
 * Expected: Cooling accumulates E = 0.5 * m * Vvir^2 undivided, as SAGE26 does;
 *           sage26_finalize_cooling_heating divides by the snapshot interval once per halo
 * Validates: Cooling energy calculation
 */
int test_physics_cooling_energy_tracking(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
  test_halo.dT = 100.0; /* Time interval in Myr */

  test_halo.galaxy->HotGas = 10.0f;
  test_halo.galaxy->MetalsHotGas = 0.2f;
  test_halo.galaxy->ColdGas = 5.0f;
  test_halo.galaxy->MetalsColdGas = 0.1f;
  test_halo.galaxy->CoolingGas = 3.0f;
  test_halo.galaxy->Cooling = 0.0; /* Start at zero */

  struct ModuleContext ctx = create_test_context();

  /* ===== EXECUTE ===== */
  int result = sage26_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* Expected cooling energy: 0.5 * 3.0 * 160^2 = 38400, not divided by dT */
  double expected_cooling = 0.5 * 3.0 * 160.0 * 160.0;
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->Cooling, expected_cooling, 0.1,
                           "Cooling energy should match expected value");

  /* A second call accumulates onto the first */
  test_halo.galaxy->CoolingGas = 1.0;
  result = sage26_apply_cooling_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->Cooling, expected_cooling + 0.5 * 160.0 * 160.0, 0.1,
                           "Cooling should accumulate over calls");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_cooling_rate_diagnostic
 * @brief   CoolingRate is SAGE26's float (m / dt) * 1e10 / Hubble_h * SEC_PER_GIGAYEAR /
 *          UnitTime_in_s, with the substep dt = dT / num_substeps
 *
 * Expected: dT 0.02 over 1 substep, 0.04 over 2 and 0.08 over 4 all give dt = 0.02 (each division
 * is exact), so one expected value covers every row; a module that used dT undivided would
 * write a rate 2 and 4 times too small.
 */
int test_cooling_rate_diagnostic(void) {
  static const struct {
    int num_substeps;
    double dT;
  } rows[] = {{1, 0.02}, {2, 0.04}, {4, 0.08}};

  init_memory_system(0);
  const float expected =
      (float)((3.0 / 0.02) * 1.0e10 / TEST_HUBBLE_H * SAGE_SEC_PER_GIGAYEAR / TEST_UNIT_TIME_IN_S);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
    test_halo.dT = rows[k].dT;
    test_halo.galaxy->HotGas = 10.0f;
    test_halo.galaxy->CoolingGas = 3.0;
    test_halo.galaxy->CoolingRate = -44.0f;

    struct ModuleContext ctx = create_test_context();
    ctx.num_substeps = rows[k].num_substeps;
    int result = sage26_apply_cooling_process(&ctx, &test_halo, 1);
    TEST_ASSERT(result == 0, "Module processing should succeed");

    printf("\n  %d substep(s), dT %g: CoolingRate %.9g, expected %.9g", rows[k].num_substeps,
           rows[k].dT, (double)test_halo.galaxy->CoolingRate, (double)expected);
    TEST_ASSERT(
        test_halo.galaxy->CoolingRate == expected,
        "CoolingRate must be SAGE26's conversion of the cooled mass over dT / num_substeps");
    free_test_halo(&test_halo);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_early_return_skips_tail
 * @brief   SAGE26's lambda <= 0 and rho_rcool <= 0 early returns skip the tail: Cooling
 *          and CoolingRate keep their previous values
 */
int test_early_return_skips_tail(void) {
  init_memory_system(0);

  const int paths[] = {SAGE_COOLING_PATH_LAMBDA, SAGE_COOLING_PATH_RHO_RCOOL};
  for (int k = 0; k < 2; k++) {
    struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
    test_halo.galaxy->HotGas = 10.0f;
    test_halo.galaxy->ColdGas = 1.0f;
    test_halo.galaxy->CoolingGas = 0.0; /* every early return leaves no mass */
    test_halo.galaxy->CoolingReturnPath = paths[k];
    test_halo.galaxy->Cooling = -55.0;
    test_halo.galaxy->CoolingRate = -44.0f;

    struct ModuleContext ctx = create_test_context();
    int result = sage26_apply_cooling_process(&ctx, &test_halo, 1);
    TEST_ASSERT(result == 0, "Module processing should succeed");
    TEST_ASSERT(test_halo.galaxy->Cooling == -55.0, "an early return leaves Cooling as it was");
    TEST_ASSERT(test_halo.galaxy->CoolingRate == -44.0f,
                "an early return leaves CoolingRate as it was");
    TEST_ASSERT(test_halo.galaxy->HotGas == 10.0f && test_halo.galaxy->ColdGas == 1.0f,
                "no gas moves on an early return");
    free_test_halo(&test_halo);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_zero_mass_writes_zero_rate
 * @brief   A zero cooling mass reaches SAGE26's tail with nothing to move
 *
 * Expected: an outer-guard failure (HotGas not positive) and a zero-mass normal tail both write
 * CoolingRate 0, leave Cooling as it was (no energy is added for zero mass) and move no gas.
 */
int test_zero_mass_writes_zero_rate(void) {
  static const struct {
    const char *what;
    int path;
    float hot_gas;
  } rows[] = {
      {"outer guard, no hot gas", SAGE_COOLING_PATH_OUTER_GUARD, 0.0f},
      {"normal tail, zero mass", SAGE_COOLING_PATH_TAIL, 10.0f},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
    test_halo.galaxy->HotGas = rows[k].hot_gas;
    test_halo.galaxy->MetalsHotGas = 0.2f * (rows[k].hot_gas > 0.0f);
    test_halo.galaxy->ColdGas = 5.0f;
    test_halo.galaxy->MetalsColdGas = 0.1f;
    test_halo.galaxy->CoolingGas = 0.0;
    test_halo.galaxy->CoolingReturnPath = rows[k].path;
    test_halo.galaxy->Cooling = -55.0;
    test_halo.galaxy->CoolingRate = -44.0f;
    struct ModuleContext ctx = create_test_context();

    TEST_ASSERT(sage26_apply_cooling_process(&ctx, &test_halo, 1) == 0,
                "Module processing should succeed");
    TEST_ASSERT(test_halo.galaxy->CoolingRate == 0.0f, rows[k].what);
    TEST_ASSERT(test_halo.galaxy->Cooling == -55.0, "zero mass adds no Cooling");
    TEST_ASSERT(test_halo.galaxy->HotGas == rows[k].hot_gas && test_halo.galaxy->ColdGas == 5.0f &&
                    test_halo.galaxy->MetalsColdGas == 0.1f,
                "zero mass moves no gas");
    free_test_halo(&test_halo);
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_no_interval_is_noop
 * @brief   Without a positive interval (the core's snapshot-zero birth: SnapNum < 0,
 *          dT <= 0) cooling did not run, and the module changes nothing
 */
int test_no_interval_is_noop(void) {
  init_memory_system(0);

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
  test_halo.SnapNum = -1;
  test_halo.dT = -1.0;
  test_halo.galaxy->HotGas = 10.0f;
  test_halo.galaxy->CoolingGas = 0.0;
  test_halo.galaxy->CoolingReturnPath = SAGE_COOLING_PATH_NOT_EVALUATED;
  test_halo.galaxy->CoolingRate = -44.0f;

  struct ModuleContext ctx = create_test_context();
  int result = sage26_apply_cooling_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Module processing should succeed");
  TEST_ASSERT(test_halo.galaxy->CoolingRate == -44.0f, "CoolingRate must be unchanged");
  TEST_ASSERT(test_halo.galaxy->HotGas == 10.0f, "HotGas must be unchanged");

  free_test_halo(&test_halo);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_type2_orphan_cools
 * @brief   SAGE parity: Type 2 orphans cool their own hot gas (NOT skipped).
 *          SAGE evolve_galaxies cools every non-merged galaxy; the apply step
 *          just commits the budget's CoolingGas.
 *
 * Expected: CoolingGas (2.0) moves from HotGas to ColdGas.
 */
int test_type2_orphan_cools(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  struct Halo orphan = create_test_halo(2, 0.0, 0.0); /* Type 2 orphan */
  orphan.galaxy->HotGas = 5.0f;
  orphan.galaxy->ColdGas = 3.0f;
  orphan.galaxy->CoolingGas = 2.0f;

  struct ModuleContext ctx = create_test_context();

  /* ===== EXECUTE ===== */
  int result = sage26_apply_cooling_process(&ctx, &orphan, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should process Type 2 orphan");

  /* The orphan cools its own gas: CoolingGas (2.0) HotGas -> ColdGas. */
  TEST_ASSERT_DOUBLE_EQUAL(orphan.galaxy->HotGas, 3.0, 0.001,
                           "Type 2 HotGas should drop by CoolingGas");
  TEST_ASSERT_DOUBLE_EQUAL(orphan.galaxy->ColdGas, 5.0, 0.001,
                           "Type 2 ColdGas should gain CoolingGas");

  /* ===== CLEANUP ===== */
  free_test_halo(&orphan);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_null_galaxy
 * @brief   Test edge case with NULL galaxy pointer
 *
 * Expected: Module returns 0 (graceful handling)
 * Validates: NULL galaxy safety
 */
int test_null_galaxy(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  struct Halo test_halo;
  memset(&test_halo, 0, sizeof(test_halo));
  test_halo.Type = 0;
  test_halo.galaxy = NULL; /* NULL galaxy */

  struct ModuleContext ctx = create_test_context();

  /* ===== EXECUTE ===== */
  int result = sage26_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should handle NULL galaxy gracefully");

  /* ===== CLEANUP ===== */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_invalid_ngal
 * @brief   Test error handling when ngal != 1
 *
 * Expected: Module returns error (-1)
 * Validates: process_by_galaxy mode enforcement
 */
int test_invalid_ngal(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  struct Halo halos[2];
  halos[0] = create_test_halo(0, 100.0, 160.0);
  halos[1] = create_test_halo(1, 50.0, 120.0);

  struct ModuleContext ctx = create_test_context();

  /* ===== EXECUTE ===== */
  int result = sage26_apply_cooling_process(&ctx, halos, 2); /* ngal=2, should fail */

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result != 0, "Module should return error when ngal != 1");

  /* ===== CLEANUP ===== */
  free_test_halo(&halos[0]);
  free_test_halo(&halos[1]);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_apply_cooling software quality tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_apply_cooling Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Run all test cases */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_physics_cooling_transfer);
  TEST_RUN(test_physics_cooling_energy_tracking);
  TEST_RUN(test_cooling_rate_diagnostic);
  TEST_RUN(test_early_return_skips_tail);
  TEST_RUN(test_zero_mass_writes_zero_rate);
  TEST_RUN(test_no_interval_is_noop);
  TEST_RUN(test_type2_orphan_cools);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_invalid_ngal);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
