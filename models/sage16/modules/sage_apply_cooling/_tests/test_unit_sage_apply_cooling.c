/**
 * @file    test_unit_sage_apply_cooling.c
 * @brief   Unit tests for sage_apply_cooling module
 *
 * Validates: the cooling transfer from the hot to the cold reservoir with its metals, the cooling
 * energy it accumulates (0.5 m Vvir^2 / dT, none for a non-positive dT), and the edge cases
 * (Type 2 orphans, zero mass, NULL galaxy, invalid ngal).
 *
 * Test cases:
 *   - test_module_initialization: the module initialises with its predecessor placed and leaks
 *     nothing
 *   - test_physics_cooling_transfer: partial cooling (hot and cold metallicity differ or agree),
 *     complete depletion and the coolingGas == HotGas boundary, gas and metals
 *   - test_physics_cooling_energy_tracking: Cooling accumulates 0.5 m Vvir^2 / dT, and a
 *     non-positive dT (a newly formed halo) adds none while the gas still moves
 *   - test_zero_cooling: zero CoolingGas moves no gas and adds no Cooling
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
#include <string.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* External module interface for direct testing */
extern int sage_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

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
 * @brief   The module initialises with sage_calculate_cooling_budget placed before it
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

  /* Configure required predecessor: sage_calculate_cooling_budget before sage_apply_cooling */
  test_phase_add("galaxy_physics", "sage_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage_apply_cooling", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

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
 * row pins the strict comparison: only the whole-reservoir branch zeroes the hot metals exactly,
 * where the partial branch leaves a rounding residue of about 1e-17.
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
      /* coolingGas == HotGas 7: the whole-reservoir branch, hot metals exactly 0. The partial
       * branch would leave 0.11f - (0.11f / 7.0f) * 7.0 = about 1e-17 of metals (with or without a
       * fused multiply-add), so only the strict comparison empties them */
      {"coolingGas equal to HotGas", 7.0f, 0.11f, 2.0f, 0.04f, 7.0f, 0.0, 0.0, 9.0, 0.15},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct Halo test_halo = create_test_halo(0, 100.0, 160.0);
    test_halo.galaxy->HotGas = rows[k].hot;
    test_halo.galaxy->MetalsHotGas = rows[k].metals_hot;
    test_halo.galaxy->ColdGas = rows[k].cold;
    test_halo.galaxy->MetalsColdGas = rows[k].metals_cold;
    test_halo.galaxy->CoolingGas = rows[k].cooling_gas;
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    TEST_ASSERT(sage_apply_cooling_process(&ctx, &test_halo, 1) == 0,
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
 * @brief   Cooling accumulates the cooling energy 0.5 m Vvir^2 / dT, and only for a positive dT
 *
 * Expected: 3.0 cooled at Vvir 160 over dT 100 adds 0.5 * 3 * 160^2 / 100 = 384; a second call
 * of mass 1.0 accumulates 128 more; with dT -1 (the sentinel of a halo with no progenitor) the
 * gas still moves but no energy is added. Every value is exact in double precision.
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

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE AND VALIDATE ===== */
  TEST_ASSERT(sage_apply_cooling_process(&ctx, &test_halo, 1) == 0,
              "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->Cooling, 384.0, 1e-9,
                           "Cooling should be 0.5 * 3.0 * 160^2 / 100 = 384");

  /* A second call accumulates onto the first */
  test_halo.galaxy->CoolingGas = 1.0;
  TEST_ASSERT(sage_apply_cooling_process(&ctx, &test_halo, 1) == 0,
              "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->Cooling, 384.0 + 128.0, 1e-9,
                           "Cooling should accumulate over calls");

  /* A non-positive dT adds no energy, but the gas still cools */
  const float cold_before = test_halo.galaxy->ColdGas;
  test_halo.dT = -1.0;
  test_halo.galaxy->CoolingGas = 2.0;
  TEST_ASSERT(sage_apply_cooling_process(&ctx, &test_halo, 1) == 0,
              "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->Cooling, 384.0 + 128.0, 1e-9,
                           "A non-positive dT adds no Cooling energy");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->ColdGas, cold_before + 2.0, 0.001,
                           "The gas still moves to the cold reservoir");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_cooling
 * @brief   Test edge case with zero CoolingGas
 *
 * Expected: No changes to the gas reservoirs, their metals or Cooling
 * Validates: Zero cooling handling
 */
int test_zero_cooling(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0);

  /* Initial state */
  test_halo.galaxy->HotGas = 10.0f;
  test_halo.galaxy->MetalsHotGas = 0.2f;
  test_halo.galaxy->ColdGas = 5.0f;
  test_halo.galaxy->MetalsColdGas = 0.1f;
  test_halo.galaxy->CoolingGas = 0.0f; /* Zero cooling */
  test_halo.galaxy->Cooling = -55.0;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 10.0, 0.001, "HotGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->ColdGas, 5.0, 0.001, "ColdGas should be unchanged");
  TEST_ASSERT(test_halo.galaxy->MetalsHotGas == 0.2f && test_halo.galaxy->MetalsColdGas == 0.1f,
              "Metals should be unchanged");
  TEST_ASSERT(test_halo.galaxy->Cooling == -55.0, "Zero mass should add no Cooling energy");

  /* ===== CLEANUP ===== */
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

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &orphan, 1);

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

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

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

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, halos, 2); /* ngal=2, should fail */

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
 * Executes all sage_apply_cooling unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_apply_cooling Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Run all test cases */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_physics_cooling_transfer);
  TEST_RUN(test_physics_cooling_energy_tracking);
  TEST_RUN(test_zero_cooling);
  TEST_RUN(test_type2_orphan_cools);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_invalid_ngal);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
