/**
 * @file    test_unit_sage26_apply_infall.c
 * @brief   Software quality unit tests for sage26_apply_infall module
 *
 * Validates: Module lifecycle, memory safety, error handling, infall distribution
 *
 * This test validates software engineering aspects of the sage26_apply_infall module:
 * - Module registration and initialization
 * - Memory allocation and cleanup (no leaks)
 * - Null pointer safety
 * - Property access patterns
 * - Infall distribution over substeps (actual module execution)
 * - Negative infall handling (ejected→hot priority)
 * - Metallicity preservation during transfers
 * - Edge cases (zero infall, multiple substeps)
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Galaxy property access works correctly
 *   - test_physics_positive_infall_single_substep: Positive infall with 1 substep
 *   - test_physics_positive_infall_multiple_substeps: Infall distributed over 4 substeps
 *   - test_physics_positive_infall_max_dynamic_substeps: Infall conserved over max dynamic substeps
 *   - test_physics_infall_budget_invariant_across_substep_counts: Budget total is N-independent
 *   - test_physics_negative_infall_from_ejected: Negative infall depletes ejected first
 *   - test_physics_negative_infall_from_hot: Negative infall depletes hot when ejected empty
 *   - test_physics_negative_infall_cascade: Negative infall depletes ejected then hot
 *   - test_zero_infall: Edge case with zero infall
 *   - test_no_central_galaxy: Edge case with no Type 0 central
 *   - test_cgm_positive_infall_selected_pair: positive infall into the selected pair only
 *   - test_cgm_negative_infall_metal_debit: residual debits the selected metals and gas
 *   - test_cgm_negative_infall_floor_and_clear: selected metals floored, both fields cleared
 *   - test_cgm_negative_infall_floor_without_clear: metals floored, gas exactly consumed
 *   - test_cgm_negative_infall_non_positive_metals: no debit of non-positive metals
 *   - test_cgm_recipe_on_refused: CGMrecipeOn missing or not 0/1 refused, naming it
 *
 * The CGMrecipeOn 1 tests run each case at CGMrecipeOn 1 with Regime 0 (CGM pair selected), at
 * CGMrecipeOn 1 with Regime 1 and at CGMrecipeOn 0 (hot pair), with exact literal expectations.
 *
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* External module interface for direct testing */
extern int sage26_apply_infall_init(void);
extern int sage26_apply_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_apply_infall_cleanup(void);

/* Test fixtures for physics tests */

/**
 * @brief Initialize sage26_apply_infall module for physics testing
 *
 * Sets up minimal module state without the full module system: the required CGMrecipeOn at 0 (the
 * hot reservoir, as these tests expect) and the budget module in pre_timestep, which init()
 * requires. Every physics test asserts that init() succeeds, so none depends on run order or on the
 * module's state left by an earlier test.
 *
 * @return init()'s status, 0 on success
 */
static int setup_module_for_physics_test(void) {
  static struct PhaseModuleConfig pre_timestep[1];
  static char budget_name[] = "sage26_prepare_infall_budget";
  reset_config();
  pre_timestep[0].module_name = budget_name;
  pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep;
  MimicConfig.num_pre_timestep = 1;
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  const int status = sage26_apply_infall_init();
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
 * @test    test_module_registration
 * @brief   Test that sage26_apply_infall module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage26_apply_infall_register() works, module appears in registry
 */
int test_module_registration(void) {
  /* ===== SETUP ===== */
  reset_config();

  /* ===== EXECUTE ===== */
  ensure_modules_registered();

  /* ===== VALIDATE ===== */
  /* If we got here without crashing, registration succeeded */

  return TEST_PASS;
}

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors or leaks
 * Validates: Module lifecycle management
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

  /* Configure required predecessor: sage26_prepare_infall_budget in pre_timestep */
  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("sage26_prepare_infall_budget");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;

  /* Configure sage26_apply_infall module in galaxy_physics */
  test_phase_add("galaxy_physics", "sage26_apply_infall", PROCESSING_MODE_FULL_HALO);
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
 * @test    test_memory_safety
 * @brief   Test that module doesn't leak memory during normal operation
 *
 * Expected: No memory leaks after init, process, cleanup cycle
 * Validates: Memory management in module
 */
int test_memory_safety(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  /* Configure required predecessor: sage26_prepare_infall_budget in pre_timestep */
  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("sage26_prepare_infall_budget");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;

  /* Configure module */
  test_phase_add("galaxy_physics", "sage26_apply_infall", PROCESSING_MODE_FULL_HALO);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  /* ===== EXECUTE ===== */
  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module initialization should succeed");

  /* ===== VALIDATE ===== */
  /* Module initialized successfully without memory leaks */

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_property_access
 * @brief   Test that module can safely access galaxy properties
 *
 * Expected: Property access doesn't crash, handles zero/null gracefully
 * Validates: Property access patterns in module
 */
int test_property_access(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Create test halo and galaxy */
  struct Halo test_halo;
  memset(&test_halo, 0, sizeof(test_halo));

  struct GalaxyData test_galaxy;
  memset(&test_galaxy, 0, sizeof(test_galaxy));

  /* Set some realistic values */
  test_halo.Mvir = 100.0; /* 10^12 Msun/h */
  test_halo.Type = 0;     /* Central */
  test_halo.SnapNum = 63;
  test_halo.galaxy = &test_galaxy;

  test_galaxy.InfallingGas = 5.0;
  test_galaxy.HotGas = 15.0;
  test_galaxy.MetalsHotGas = 0.3;
  test_galaxy.EjectedGas = 2.0;
  test_galaxy.MetalsEjectedGas = 0.04;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(test_halo.galaxy != NULL, "Galaxy pointer should be accessible");
  TEST_ASSERT(test_galaxy.InfallingGas >= 0.0, "InfallingGas should be non-negative");
  TEST_ASSERT(test_galaxy.HotGas >= 0.0, "HotGas should be non-negative");

  /* Test with zero values (edge case) */
  struct GalaxyData zero_galaxy;
  memset(&zero_galaxy, 0, sizeof(zero_galaxy));
  TEST_ASSERT(zero_galaxy.HotGas == 0.0, "Zero-initialized galaxy should have HotGas=0");
  TEST_ASSERT(zero_galaxy.InfallingGas == 0.0,
              "Zero-initialized galaxy should have InfallingGas=0");

  /* ===== CLEANUP ===== */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_positive_infall_single_substep
 * @brief   Test positive infall with single substep
 *
 * Expected: InfallingGas added to HotGas in single substep
 * Validates: Basic infall distribution
 */
int test_physics_positive_infall_single_substep(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo test_halo = create_test_halo(0, 100.0); /* Type 0 central */

  /* Initial state */
  test_halo.galaxy->InfallingGas = 10.0;
  test_halo.galaxy->HotGas = 5.0;
  test_halo.galaxy->MetalsHotGas = 0.1;

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.substep_number = 0;

  /* ===== EXECUTE ===== */
  int result = sage26_apply_infall_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 15.0, 0.001,
                           "HotGas should be 5.0 + 10.0 = 15.0");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_positive_infall_multiple_substeps
 * @brief   Test positive infall distributed over 4 substeps
 *
 * Expected: InfallingGas/4 added each substep
 * Validates: Substep distribution logic
 */
int test_physics_positive_infall_multiple_substeps(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo test_halo = create_test_halo(0, 100.0);

  /* Initial state */
  test_halo.galaxy->InfallingGas = 12.0; /* 3.0 per substep */
  test_halo.galaxy->HotGas = 5.0;
  test_halo.galaxy->MetalsHotGas = 0.1;

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 4;

  /* ===== EXECUTE ===== */
  /* Simulate 4 substeps */
  for (int i = 0; i < 4; i++) {
    ctx.substep_number = i;
    int result = sage26_apply_infall_process(&ctx, &test_halo, 1);
    TEST_ASSERT(result == 0, "Module processing should succeed for each substep");
  }

  /* ===== VALIDATE ===== */
  /* After 4 substeps: HotGas = 5.0 + 4*(12.0/4) = 5.0 + 12.0 = 17.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 17.0, 0.001,
                           "HotGas should be 5.0 + 12.0 = 17.0");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_positive_infall_max_dynamic_substeps
 * @brief   Test positive infall sums correctly over the dynamic substep cap
 *
 * Expected: Total HotGas gain equals the original InfallingGas budget after
 * DEFAULT_MAX_DYNAMIC_SUBSTEPS applications.
 * Validates: Direct /num_substeps distribution conserves the infall budget at
 * the largest dynamic timestep count.
 */
int test_physics_positive_infall_max_dynamic_substeps(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo test_halo = create_test_halo(0, 100.0);

  const double initial_hot = 5.0;
  const double infall_budget = 12.7;
  test_halo.galaxy->InfallingGas = infall_budget;
  test_halo.galaxy->HotGas = initial_hot;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = DEFAULT_MAX_DYNAMIC_SUBSTEPS;

  /* ===== EXECUTE ===== */
  for (int i = 0; i < DEFAULT_MAX_DYNAMIC_SUBSTEPS; i++) {
    ctx.substep_number = i;
    int result = sage26_apply_infall_process(&ctx, &test_halo, 1);
    TEST_ASSERT(result == 0, "Module processing should succeed for each dynamic substep");
  }

  /* ===== VALIDATE ===== */
  /* Wider than a single-substep tolerance: float HotGas accrues roundoff over
   * N substeps (docs/DEVELOPER-GUIDE.md#property-precision). */
  const double expected_hot = initial_hot + infall_budget;
  const double hot_tol = fmax(1e-5, fabs(expected_hot) * 1e-4);
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, expected_hot, hot_tol,
                           "HotGas gain should equal total infall budget at max dynamic substeps");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_infall_budget_invariant_across_substep_counts
 * @brief   Total infall applied over a full interval must not depend on num_substeps
 *
 * Expected: Two arbitrary substep counts (7 and 23) yield the same final HotGas.
 * Validates: the fixed pre_timestep budget is partitioned, not recomputed per substep
 * (contrast sage26_satellite_stripping's N-dependent formula).
 */
int test_physics_infall_budget_invariant_across_substep_counts(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  const double initial_hot = 5.0;
  const double infall_budget = 12.7;

  struct Halo halo_n7 = create_test_halo(0, 100.0);
  halo_n7.galaxy->InfallingGas = infall_budget;
  halo_n7.galaxy->HotGas = initial_hot;

  struct Halo halo_n23 = create_test_halo(0, 100.0);
  halo_n23.galaxy->InfallingGas = infall_budget;
  halo_n23.galaxy->HotGas = initial_hot;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  ctx.num_substeps = 7;
  for (int i = 0; i < 7; i++) {
    ctx.substep_number = i;
    int result = sage26_apply_infall_process(&ctx, &halo_n7, 1);
    TEST_ASSERT(result == 0, "Module processing should succeed for N=7");
  }

  ctx.num_substeps = 23;
  for (int i = 0; i < 23; i++) {
    ctx.substep_number = i;
    int result = sage26_apply_infall_process(&ctx, &halo_n23, 1);
    TEST_ASSERT(result == 0, "Module processing should succeed for N=23");
  }

  /* ===== VALIDATE ===== */
  const double tol = fmax(1e-5, fabs(initial_hot + infall_budget) * 1e-6);
  TEST_ASSERT_DOUBLE_EQUAL(halo_n7.galaxy->HotGas, initial_hot + infall_budget, tol,
                           "N=7 should deliver the full infall budget");
  TEST_ASSERT_DOUBLE_EQUAL(halo_n23.galaxy->HotGas, initial_hot + infall_budget, tol,
                           "N=23 should deliver the full infall budget");
  TEST_ASSERT_DOUBLE_EQUAL(halo_n7.galaxy->HotGas, halo_n23.galaxy->HotGas, tol,
                           "Final HotGas must be independent of num_substeps");

  /* ===== CLEANUP ===== */
  free_test_halo(&halo_n7);
  free_test_halo(&halo_n23);
  sage26_apply_infall_cleanup();
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
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

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
  int result = sage26_apply_infall_process(&ctx, &test_halo, 1);

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
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_from_hot
 * @brief   Test negative infall removes from hot gas when ejected is empty
 *
 * Expected: HotGas reduced, EjectedGas remains 0
 * Validates: Fallback to hot gas when ejected is depleted
 */
int test_physics_negative_infall_from_hot(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo test_halo = create_test_halo(0, 100.0);

  /* Initial state */
  test_halo.galaxy->InfallingGas = -5.0; /* Mass loss */
  test_halo.galaxy->EjectedGas = 0.0;    /* Empty ejected */
  test_halo.galaxy->MetalsEjectedGas = 0.0;
  test_halo.galaxy->HotGas = 20.0;
  test_halo.galaxy->MetalsHotGas = 0.4; /* Z = 0.02 */

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.substep_number = 0;

  /* ===== EXECUTE ===== */
  int result = sage26_apply_infall_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* Ejected should remain 0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 0.0, 0.001,
                           "EjectedGas should remain 0.0");

  /* Hot should be reduced: 20.0 - 5.0 = 15.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 15.0, 0.001,
                           "HotGas should be 20.0 - 5.0 = 15.0");

  /* Metallicity preserved in hot: 15.0 * 0.02 = 0.3 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, 0.3, 0.001,
                           "MetalsHotGas should be 0.3");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_negative_infall_cascade
 * @brief   Test negative infall cascades: depletes ejected, then hot
 *
 * Expected: EjectedGas → 0, HotGas reduced by remaining deficit
 * Validates: Cascade logic (ejected → hot)
 */
int test_physics_negative_infall_cascade(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo test_halo = create_test_halo(0, 100.0);

  /* Initial state */
  test_halo.galaxy->InfallingGas = -8.0;     /* Mass loss exceeds ejected */
  test_halo.galaxy->EjectedGas = 3.0;        /* Only 3.0 available */
  test_halo.galaxy->MetalsEjectedGas = 0.06; /* Z = 0.02 */
  test_halo.galaxy->HotGas = 10.0;
  test_halo.galaxy->MetalsHotGas = 0.2; /* Z = 0.02 */

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.substep_number = 0;

  /* ===== EXECUTE ===== */
  int result = sage26_apply_infall_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* Ejected should be depleted */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 0.0, 0.001,
                           "EjectedGas should be depleted to 0.0");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsEjectedGas, 0.0, 0.001,
                           "MetalsEjectedGas should be 0.0");

  /* Hot should absorb remaining: 10.0 + (-8.0 + 3.0) = 5.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 5.0, 0.001,
                           "HotGas should be 10.0 - 5.0 = 5.0");

  /* Metallicity preserved in hot: 5.0 * 0.02 = 0.1 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, 0.1, 0.001,
                           "MetalsHotGas should be 0.1");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_infall
 * @brief   Test edge case with zero infall
 *
 * Expected: No changes to gas reservoirs
 * Validates: Zero infall handling
 */
int test_zero_infall(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo test_halo = create_test_halo(0, 100.0);

  /* Initial state */
  test_halo.galaxy->InfallingGas = 0.0; /* Zero infall */
  test_halo.galaxy->HotGas = 10.0;
  test_halo.galaxy->MetalsHotGas = 0.2;
  test_halo.galaxy->EjectedGas = 5.0;
  test_halo.galaxy->MetalsEjectedGas = 0.1;

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.substep_number = 0;

  /* ===== EXECUTE ===== */
  int result = sage26_apply_infall_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 10.0, 0.001, "HotGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->EjectedGas, 5.0, 0.001,
                           "EjectedGas should be unchanged");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage26_apply_infall_cleanup();
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
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

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
  int result = sage26_apply_infall_process(&ctx, &satellite, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should handle no-central case gracefully");

  /* Satellite should be unchanged (infall only applies to centrals) */
  TEST_ASSERT_DOUBLE_EQUAL(satellite.galaxy->HotGas, 3.0, 0.001,
                           "Satellite HotGas should be unchanged");

  /* ===== CLEANUP ===== */
  free_test_halo(&satellite);
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// CGMrecipeOn 1 AGAINST 0 (platform independent)
// ============================================================================
//
// SAGE26's add_infall_to_hot (R = the staged SAGE26 source, R/src/model_infall.c:351-414) selects
// the central's CGM pair for CGMrecipeOn 1 and Regime 0 (:371-385) and its hot pair otherwise
// (:386-413). Each case runs the same central at CGMrecipeOn 1 with Regime 0 (CGM selected), at
// CGMrecipeOn 1 with Regime 1 and at CGMrecipeOn 0 with Regime 0 (hot selected in both), and
// asserts the selected pair, the unselected pair unchanged and the ejected pair. Every input is a
// short binary fraction (or, for the floor without a clear, a literal float pair), so each
// expectation is an exact literal.

/** @brief Read a captured log stream into a static buffer (valid until the next call) */
static const char *c10_read_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/** @brief Initialise with the budget module in pre_timestep and CGMrecipeOn `cgm` (NULL: missing)
 */
static int c10_init(const char *cgm, const char **log) {
  static struct PhaseModuleConfig pre_timestep[1];
  static char budget_name[] = "sage26_prepare_infall_budget";
  reset_config();
  pre_timestep[0].module_name = budget_name;
  pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep;
  MimicConfig.num_pre_timestep = 1;
  if (cgm != NULL) {
    sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  }
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_apply_infall_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = c10_read_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  return status;
}

/** @brief The central's reservoirs: ejected, hot and CGM pairs */
struct c10_pairs {
  float ejected, metals_ejected, hot, metals_hot, cgm, metals_cgm;
};

/** @brief Run one central (Type 0, given Regime and reservoirs) at CGMrecipeOn `cgm` */
static int c10_apply(const char *cgm, const int regime, const struct c10_pairs *in,
                     const double infalling_gas, const int substeps, struct c10_pairs *out) {
  const char *log = "";
  if (c10_init(cgm, &log) != 0) {
    return -1;
  }
  struct GalaxyData gal;
  struct Halo halo;
  memset(&gal, 0, sizeof(gal));
  memset(&halo, 0, sizeof(halo));
  halo.galaxy = &gal;
  halo.Type = 0;
  halo.Mvir = 100.0;
  gal.Regime = regime;
  gal.InfallingGas = infalling_gas;
  gal.EjectedGas = in->ejected;
  gal.MetalsEjectedGas = in->metals_ejected;
  gal.HotGas = in->hot;
  gal.MetalsHotGas = in->metals_hot;
  gal.CGMgas = in->cgm;
  gal.MetalsCGMgas = in->metals_cgm;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = substeps;
  ctx.central_galaxy = &halo;
  const int status = sage26_apply_infall_process(&ctx, &halo, 1);
  out->ejected = gal.EjectedGas;
  out->metals_ejected = gal.MetalsEjectedGas;
  out->hot = gal.HotGas;
  out->metals_hot = gal.MetalsHotGas;
  out->cgm = gal.CGMgas;
  out->metals_cgm = gal.MetalsCGMgas;
  return status;
}

static int c10_same_bits(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

/**
 * @brief Run the three configurations and compare: the selected pair against (gas, metals), the
 *        unselected pair against its input, the ejected pair against (ejected, metals_ejected)
 * @return 1 when every configuration matches
 */
static int c10_check_routing(const struct c10_pairs *in, const double infalling_gas,
                             const int substeps, const float gas, const float metals,
                             const float ejected, const float metals_ejected) {
  const struct {
    const char *cgm;
    int regime;
    int selects_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}};
  int ok = 1;
  for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
    struct c10_pairs out;
    if (c10_apply(runs[k].cgm, runs[k].regime, in, infalling_gas, substeps, &out) != 0) {
      printf("\n  CGMrecipeOn %s, Regime %d: init or process failed", runs[k].cgm, runs[k].regime);
      ok = 0;
      continue;
    }
    const float sel_gas = runs[k].selects_cgm ? out.cgm : out.hot;
    const float sel_metals = runs[k].selects_cgm ? out.metals_cgm : out.metals_hot;
    const float other_gas = runs[k].selects_cgm ? out.hot : out.cgm;
    const float other_metals = runs[k].selects_cgm ? out.metals_hot : out.metals_cgm;
    const float other_gas_in = runs[k].selects_cgm ? in->hot : in->cgm;
    const float other_metals_in = runs[k].selects_cgm ? in->metals_hot : in->metals_cgm;
    const int match =
        c10_same_bits(sel_gas, gas) && c10_same_bits(sel_metals, metals) &&
        c10_same_bits(other_gas, other_gas_in) && c10_same_bits(other_metals, other_metals_in) &&
        c10_same_bits(out.ejected, ejected) && c10_same_bits(out.metals_ejected, metals_ejected);
    if (!match) {
      printf("\n  CGMrecipeOn %s, Regime %d (%s selected): selected %.9g/%.9g (want %.9g/%.9g), "
             "unselected %.9g/%.9g (input %.9g/%.9g), ejected %.9g/%.9g (want %.9g/%.9g)",
             runs[k].cgm, runs[k].regime, runs[k].selects_cgm ? "CGM" : "hot", (double)sel_gas,
             (double)sel_metals, (double)gas, (double)metals, (double)other_gas,
             (double)other_metals, (double)other_gas_in, (double)other_metals_in,
             (double)out.ejected, (double)out.metals_ejected, (double)ejected,
             (double)metals_ejected);
      ok = 0;
    }
  }
  return ok;
}

/**
 * @test    test_cgm_positive_infall_selected_pair
 * @brief   Positive infall adds gas, and no metals, to the selected pair only (I:383, :397, :411)
 */
int test_cgm_positive_infall_selected_pair(void) {
  /* Both pairs hold 2.0 / 0.25: budget 1.0 over 4 substeps is 0.25 per substep */
  const struct c10_pairs in = {0.5f, 0.0625f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(c10_check_routing(&in, 1.0, 4, 2.25f, 0.25f, 0.5f, 0.0625f),
              "positive infall must add 0.25 to the selected gas only");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_metal_debit
 * @brief   Negative infall exhausts the ejected gas, then debits the selected metals at their
 *          metallicity and the selected gas (I:357-368, :376-383, :390-397)
 *
 * Ejected 0.5 / 0.0625 (Z 1/8) takes -2.0: its metals floor at 0 and the residual is the narrowed
 * -1.5. The selected pair 2.0 / 0.25 (Z 1/8) then loses 1.5 and 0.1875: 0.5 / 0.0625.
 */
int test_cgm_negative_infall_metal_debit(void) {
  const struct c10_pairs in = {0.5f, 0.0625f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(c10_check_routing(&in, -2.0, 1, 0.5f, 0.0625f, 0.0f, 0.0f),
              "the residual must debit the selected pair only");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_floor_and_clear
 * @brief   Negative infall beyond the selected gas floors its metals and clears both selected
 *          fields (I:379, :384; :393, :398)
 *
 * No ejected gas; -4.0 against 2.0 / 0.25 (Z 1/8): metals 0.25 - 0.5 floor at 0, gas -2 clears.
 */
int test_cgm_negative_infall_floor_and_clear(void) {
  const struct c10_pairs in = {0.0f, 0.0f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(c10_check_routing(&in, -4.0, 1, 0.0f, 0.0f, 0.0f, 0.0f),
              "the selected pair must be cleared, the other unchanged");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_floor_without_clear
 * @brief   The selected metals floor at zero while the gas reaches exactly zero (I:379 without
 *          :384): 1.5 of gas with 0x3c75c290 (0.0150000006) of metals takes exactly -1.5
 *
 * The float metallicity of that pair rounds above M / G, so M - 1.5 * Z is negative (exactly, by
 * any rounding of the exact product of two floats) and is floored to +0, while 1.5 - 1.5 = +0 is
 * not negative and clears nothing.
 */
int test_cgm_negative_infall_floor_without_clear(void) {
  const uint32_t metals_bits = 0x3c75c290u;
  float metals;
  memcpy(&metals, &metals_bits, sizeof(metals));
  const struct c10_pairs in = {0.0f, 0.0f, 1.5f, metals, 1.5f, metals};
  TEST_ASSERT(c10_check_routing(&in, -1.5, 1, 0.0f, 0.0f, 0.0f, 0.0f),
              "the selected metals must floor at +0 with the gas exactly consumed");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_non_positive_metals
 * @brief   Selected metals that are not positive are neither debited nor floored (I:376, :390):
 *          -2^-30 stays -2^-30 while the gas loses 1.0
 */
int test_cgm_negative_infall_non_positive_metals(void) {
  const float tiny_negative = -0x1p-30f;
  const struct c10_pairs in = {0.0f, 0.0f, 2.0f, tiny_negative, 2.0f, tiny_negative};
  TEST_ASSERT(c10_check_routing(&in, -1.0, 1, 1.0f, tiny_negative, 0.0f, 0.0f),
              "non-positive selected metals must be left as they are");
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_refused
 * @brief   CGMrecipeOn is required (missing key fails init) and only 0 or 1 is accepted,
 *          a refusal naming the parameter
 */
int test_cgm_recipe_on_refused(void) {
  const char *log = "";
  TEST_ASSERT(c10_init(NULL, &log) != 0, "init must fail when CGMrecipeOn is missing");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the missing-key error must name CGMrecipeOn");
  TEST_ASSERT(c10_init("2", &log) != 0, "init must refuse CGMrecipeOn 2");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init("-1", &log) != 0, "init must refuse CGMrecipeOn -1");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init("1", &log) == 0, "init must accept CGMrecipeOn 1");
  TEST_ASSERT(c10_init("0", &log) == 0, "init must accept CGMrecipeOn 0");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_apply_infall software quality tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_apply_infall Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Run all test cases */
  TEST_RUN(test_module_registration);
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);
  TEST_RUN(test_property_access);
  TEST_RUN(test_physics_positive_infall_single_substep);
  TEST_RUN(test_physics_positive_infall_multiple_substeps);
  TEST_RUN(test_physics_positive_infall_max_dynamic_substeps);
  TEST_RUN(test_physics_infall_budget_invariant_across_substep_counts);
  TEST_RUN(test_physics_negative_infall_from_ejected);
  TEST_RUN(test_physics_negative_infall_from_hot);
  TEST_RUN(test_physics_negative_infall_cascade);
  TEST_RUN(test_zero_infall);
  TEST_RUN(test_no_central_galaxy);

  /* CGMrecipeOn 1 against 0 (platform independent) */
  TEST_RUN(test_cgm_positive_infall_selected_pair);
  TEST_RUN(test_cgm_negative_infall_metal_debit);
  TEST_RUN(test_cgm_negative_infall_floor_and_clear);
  TEST_RUN(test_cgm_negative_infall_floor_without_clear);
  TEST_RUN(test_cgm_negative_infall_non_positive_metals);
  TEST_RUN(test_cgm_recipe_on_refused);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
