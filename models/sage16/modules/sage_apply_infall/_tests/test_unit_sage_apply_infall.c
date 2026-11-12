/**
 * @file    test_unit_sage_apply_infall.c
 * @brief   Unit tests for the sage_apply_infall module
 *
 * Validates the infall application of SAGE's add_infall_to_hot: the per-substep share of the budget
 * that sage_prepare_infall_budget stores in InfallingGas goes to the central's hot gas, and a
 * negative budget is taken from the ejected gas first and then from the hot gas, with metals
 * following at the reservoir's metallicity.
 *
 * Test cases:
 *   - test_module_initialization: a pipeline with the budget module in pre_timestep initialises
 *   - test_physics_infall_budget_invariant_across_substep_counts: the budget is partitioned over
 *     the substeps, not recomputed, for substep counts from 1 to the dynamic maximum
 *   - test_physics_negative_infall_from_ejected: ejected gas that covers a negative budget absorbs
 *     all of it
 *   - test_physics_negative_infall_beyond_ejected: a loss the ejected gas cannot cover, or finds
 *     none to take, falls on the hot gas, with its metal debit, floor and clear
 *   - test_no_central_galaxy: no Type 0 row is a no-op
 *   - test_null_central_galaxy_is_an_error: a Type 0 row without galaxy data is an error
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "include/constants.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* External module interface for direct testing */
extern int sage_apply_infall_init(void);
extern int sage_apply_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_apply_infall_cleanup(void);

/* Test fixtures for physics tests */

/**
 * @brief Initialize sage_apply_infall module for physics testing
 *
 * Sets up minimal module state without the full module system: the budget module in pre_timestep,
 * which init() requires. Every physics test asserts that init() succeeds, so none depends on run
 * order or on the module's state left by an earlier test.
 *
 * @return init()'s status, 0 on success
 */
static int setup_module_for_physics_test(void) {
  static struct PhaseModuleConfig pre_timestep[1];
  static char budget_name[] = "sage_prepare_infall_budget";
  reset_config();
  pre_timestep[0].module_name = budget_name;
  pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep;
  MimicConfig.num_pre_timestep = 1;
  const int status = sage_apply_infall_init();
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  return status;
}

/**
 * @brief Create a test halo with galaxy for physics tests
 *
 * @param type Halo type (0=central, 1=satellite, 2=orphan, 3=ejected)
 * @param mvir Virial mass in code units (1e10 Msun/h)
 * @return Allocated halo (must be freed with free_test_halo)
 */
static struct Halo create_test_halo(int type, float mvir) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));

  halo.Type = type;
  halo.Mvir = mvir;
  halo.SnapNum = 63; /* z=0 */

  /* Allocate galaxy data */
  halo.galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
  memset(halo.galaxy, 0, sizeof(struct GalaxyData));

  return halo;
}

/**
 * @test    test_module_initialization
 * @brief   The budget module in pre_timestep and sage_apply_infall in galaxy_physics initialise
 *          through the module system and clean up without leaks
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("sage_prepare_infall_budget");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;

  test_phase_add("galaxy_physics", "sage_apply_infall", PROCESSING_MODE_FULL_HALO);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

  /* ===== EXECUTE ===== */
  const int result = module_system_init();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_infall_budget_invariant_across_substep_counts
 * @brief   The infall budget is partitioned over the substeps, so the interval's total does not
 *          depend on num_substeps
 *
 * Each row applies a budget at its substep count to a central holding hot and ejected gas and
 * expects the hot gas plus the budget, with the ejected pair and the hot metals unchanged
 * (positive infall adds gas and no metals). The budget is fixed in pre_timestep, unlike
 * sage_satellite_stripping's N-dependent formula. Tolerances are absolute on the hot gas: the first
 * two rows are exact binary sums, the others allow the float rounding of N additions into HotGas
 * (1e-6 relative at N = 7 and 23, 1e-4 at the dynamic maximum;
 * docs/DEVELOPER-GUIDE.md#property-precision).
 */
int test_physics_infall_budget_invariant_across_substep_counts(void) {
  static const struct {
    int substeps;
    double budget;
    double hot_tol;
  } rows[] = {
      {1, 10.0, 0.001},
      {4, 12.0, 0.001},
      {7, 12.7, 1.77e-5},
      {23, 12.7, 1.77e-5},
      {DEFAULT_MAX_DYNAMIC_SUBSTEPS, 12.7, 1.77e-3},
  };
  const double initial_hot = 5.0;

  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed");

  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    char msg[128];
    struct Halo halo = create_test_halo(0, 100.0);
    halo.galaxy->InfallingGas = rows[k].budget;
    halo.galaxy->HotGas = (float)initial_hot;
    halo.galaxy->MetalsHotGas = 0.1f;
    halo.galaxy->EjectedGas = 2.0f;
    halo.galaxy->MetalsEjectedGas = 0.04f;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = rows[k].substeps;

    /* ===== EXECUTE ===== */
    for (int i = 0; i < rows[k].substeps; i++) {
      ctx.substep_number = i;
      snprintf(msg, sizeof(msg), "process must succeed (N=%d, budget %g, substep %d)",
               rows[k].substeps, rows[k].budget, i);
      TEST_ASSERT(sage_apply_infall_process(&ctx, &halo, 1) == 0, msg);
    }

    /* ===== VALIDATE ===== */
    snprintf(msg, sizeof(msg), "HotGas must gain the whole budget (N=%d, budget %g)",
             rows[k].substeps, rows[k].budget);
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->HotGas, initial_hot + rows[k].budget, rows[k].hot_tol,
                             msg);
    snprintf(msg, sizeof(msg),
             "positive infall must leave the ejected pair and hot metals alone "
             "(N=%d, budget %g)",
             rows[k].substeps, rows[k].budget);
    TEST_ASSERT(halo.galaxy->EjectedGas == 2.0f && halo.galaxy->MetalsEjectedGas == 0.04f &&
                    halo.galaxy->MetalsHotGas == 0.1f,
                msg);
    free_test_halo(&halo);
  }

  /* ===== CLEANUP ===== */
  sage_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_from_ejected
 * @brief   Test negative infall removes from ejected reservoir first
 *
 * Expected: EjectedGas reduced, HotGas unchanged (ejected has enough mass)
 * Validates: Priority order (ejected before hot)
 */
int test_physics_negative_infall_from_ejected(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed");

  struct Halo test_halo = create_test_halo(0, 100.0);

  /* Initial state */
  test_halo.galaxy->InfallingGas = -5.0; /* Mass loss */
  test_halo.galaxy->EjectedGas = 10.0;
  test_halo.galaxy->MetalsEjectedGas = 0.2; /* Z = 0.02 */
  test_halo.galaxy->HotGas = 20.0;
  test_halo.galaxy->MetalsHotGas = 0.4; /* Z = 0.02 */

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.substep_number = 0;

  /* ===== EXECUTE ===== */
  int result = sage_apply_infall_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* Ejected should be reduced: 10.0 - 5.0 = 5.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 5.0, 0.001,
                           "EjectedGas should be 10.0 - 5.0 = 5.0");

  /* Hot should be unchanged (ejected had enough mass) */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 20.0, 0.001,
                           "HotGas should be unchanged at 20.0");

  /* Metallicity preserved in ejected: 5.0 * 0.02 = 0.1 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsEjectedGas, 0.1, 0.001,
                           "MetalsEjectedGas should be 0.1");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_beyond_ejected
 * @brief   A negative budget the ejected gas cannot cover (or that finds none) falls on the hot
 *          gas: its metals are debited at the hot metallicity, floored at zero, and a hot gas
 *          driven negative is cleared with its metals
 *
 * Rows give the central's (ejected, ejected metals, budget, hot, hot metals) and the expected
 * (ejected, ejected metals, hot, hot metals) after one substep:
 *   - no ejected gas: hot 20 / 0.4 (Z 0.02) loses 5 -> 15 / 0.3
 *   - ejected 3 / 0.06 exhausted by -8: the residual -5 debits hot 10 / 0.2 -> 5 / 0.1
 *   - ejected 0.5 / 0.0625 (Z 1/8) exhausted by -2: its metals floor at 0 and the residual -1.5
 *     debits hot 2 / 0.25 (Z 1/8) -> 0.5 / 0.0625
 *   - -4 against hot 2 / 0.25: the metals floor at 0 (0.25 - 0.5) and the gas (-2) clears both
 *   - hot metals of -2^-30 are not positive, so they are neither debited nor floored
 * The decimal rows allow 0.001, the binary-fraction rows are exact.
 */
int test_physics_negative_infall_beyond_ejected(void) {
  static const struct {
    float ejected, metals_ejected;
    double budget;
    float hot, metals_hot;
    float ejected_out, metals_ejected_out, hot_out, metals_hot_out;
    double tol;
  } rows[] = {
      {0.0f, 0.0f, -5.0, 20.0f, 0.4f, 0.0f, 0.0f, 15.0f, 0.3f, 0.001},
      {3.0f, 0.06f, -8.0, 10.0f, 0.2f, 0.0f, 0.0f, 5.0f, 0.1f, 0.001},
      {0.5f, 0.0625f, -2.0, 2.0f, 0.25f, 0.0f, 0.0f, 0.5f, 0.0625f, 0.0},
      {0.0f, 0.0f, -4.0, 2.0f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0},
      {0.0f, 0.0f, -1.0, 2.0f, -0x1p-30f, 0.0f, 0.0f, 1.0f, -0x1p-30f, 0.0},
  };

  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed");

  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    char msg[96];
    struct Halo halo = create_test_halo(0, 100.0);
    halo.galaxy->InfallingGas = rows[k].budget;
    halo.galaxy->EjectedGas = rows[k].ejected;
    halo.galaxy->MetalsEjectedGas = rows[k].metals_ejected;
    halo.galaxy->HotGas = rows[k].hot;
    halo.galaxy->MetalsHotGas = rows[k].metals_hot;

    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.num_substeps = 1;

    /* ===== EXECUTE ===== */
    snprintf(msg, sizeof(msg), "process must succeed (row %zu)", k);
    TEST_ASSERT(sage_apply_infall_process(&ctx, &halo, 1) == 0, msg);

    /* ===== VALIDATE ===== */
    snprintf(msg, sizeof(msg), "ejected gas of row %zu", k);
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->EjectedGas, rows[k].ejected_out, rows[k].tol, msg);
    snprintf(msg, sizeof(msg), "ejected metals of row %zu", k);
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->MetalsEjectedGas, rows[k].metals_ejected_out, rows[k].tol,
                             msg);
    snprintf(msg, sizeof(msg), "hot gas of row %zu", k);
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->HotGas, rows[k].hot_out, rows[k].tol, msg);
    snprintf(msg, sizeof(msg), "hot metals of row %zu", k);
    TEST_ASSERT_DOUBLE_EQUAL(halo.galaxy->MetalsHotGas, rows[k].metals_hot_out, rows[k].tol, msg);
    free_test_halo(&halo);
  }

  /* ===== CLEANUP ===== */
  sage_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_no_central_galaxy
 * @brief   Test edge case with no Type 0 central galaxy
 *
 * Expected: Module returns 0 (graceful handling)
 * Validates: No-central-galaxy safety
 */
int test_no_central_galaxy(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed");

  /* Create satellite-only group */
  struct Halo satellite = create_test_halo(1, 10.0); /* Type 1 satellite */
  satellite.galaxy->InfallingGas = 5.0;
  satellite.galaxy->HotGas = 3.0;

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.substep_number = 0;

  /* ===== EXECUTE ===== */
  int result = sage_apply_infall_process(&ctx, &satellite, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should handle no-central case gracefully");

  /* Satellite should be unchanged (infall only applies to centrals) */
  TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->HotGas, 3.0, 0.001,
                           "Satellite HotGas should be unchanged");

  /* ===== CLEANUP ===== */
  free_test_halo(&satellite);
  sage_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_null_central_galaxy_is_an_error
 * @brief   A Type 0 row with no galaxy data fails with -1 instead of being skipped
 */
int test_null_central_galaxy_is_an_error(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed");

  struct Halo central;
  memset(&central, 0, sizeof(central));
  central.Type = 0;
  central.Mvir = 100.0;
  central.galaxy = NULL;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;

  /* ===== EXECUTE AND VALIDATE ===== */
  TEST_ASSERT(sage_apply_infall_process(&ctx, &central, 1) == -1,
              "a central without galaxy data must be an error");

  /* ===== CLEANUP ===== */
  sage_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_apply_infall unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_apply_infall Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Run all test cases */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_physics_infall_budget_invariant_across_substep_counts);
  TEST_RUN(test_physics_negative_infall_from_ejected);
  TEST_RUN(test_physics_negative_infall_beyond_ejected);
  TEST_RUN(test_no_central_galaxy);
  TEST_RUN(test_null_central_galaxy_is_an_error);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
