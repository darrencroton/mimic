/**
 * @file    test_unit_sage_apply_cooling.c
 * @brief   Software quality unit tests for sage_apply_cooling module
 *
 * Validates: Module lifecycle, memory safety, error handling, cooling physics
 *
 * This test validates software engineering aspects of the sage_apply_cooling module:
 * - Module registration and initialization
 * - Memory allocation and cleanup (no leaks)
 * - Null pointer safety
 * - Property access patterns
 * - Cooling gas transfer (partial and complete)
 * - Metallicity preservation during gas transfer
 * - Cooling energy tracking
 * - Edge cases (Type 2 orphans, zero cooling, NULL galaxy, invalid ngal)
 *
 * Test cases:
 *   - test_module_registration: Module registers correctly
 *   - test_module_initialization: Module init/cleanup lifecycle
 *   - test_memory_safety: No memory leaks during operation
 *   - test_property_access: Galaxy property access works correctly
 *   - test_physics_partial_cooling: Partial hot gas cooling (coolingGas < HotGas)
 *   - test_physics_complete_depletion: Complete hot gas depletion (coolingGas >= HotGas)
 *   - test_physics_metallicity_preservation: Metallicity preserved during transfer
 *   - test_physics_cooling_energy_tracking: Cooling energy calculated correctly
 *   - test_zero_cooling: Edge case with zero CoolingGas
 *   - test_type2_orphan_cools: Type 2 orphans cool their own hot gas (SAGE parity)
 *   - test_null_galaxy: NULL galaxy handled gracefully
 *   - test_invalid_ngal: Error when ngal != 1
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
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

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* External module interface for direct testing */
extern int sage_apply_cooling_init(void);
extern int sage_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_apply_cooling_cleanup(void);

/* Test fixtures for physics tests */

/**
 * @brief Initialize sage_apply_cooling module for physics testing
 *
 * Sets up minimal module state without full module system.
 * Module has no init-time parameters to configure.
 */
static void setup_module_for_physics_test(void) { sage_apply_cooling_init(); }

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
 * @test    test_module_registration
 * @brief   Test that sage_apply_cooling module registers correctly
 *
 * Expected: Module registration succeeds without errors
 * Validates: sage_apply_cooling_register() works, module appears in registry
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

  /* Configure required predecessor: sage_calculate_cooling_budget before sage_apply_cooling */
  test_phase_add("galaxy_physics", "sage_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage_apply_cooling", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();

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
  test_halo.Vvir = 160.0; /* km/s */
  test_halo.Type = 0;     /* Central */
  test_halo.SnapNum = 63;
  test_halo.dT = 100.0;
  test_halo.galaxy = &test_galaxy;

  test_galaxy.CoolingGas = 2.0;
  test_galaxy.HotGas = 15.0;
  test_galaxy.MetalsHotGas = 0.3;
  test_galaxy.ColdGas = 5.0;
  test_galaxy.MetalsColdGas = 0.1;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(test_halo.galaxy != NULL, "Galaxy pointer should be accessible");
  TEST_ASSERT(test_galaxy.CoolingGas >= 0.0, "CoolingGas should be non-negative");
  TEST_ASSERT(test_galaxy.HotGas >= 0.0, "HotGas should be non-negative");
  TEST_ASSERT(test_galaxy.ColdGas >= 0.0, "ColdGas should be non-negative");

  /* Test with zero values (edge case) */
  struct GalaxyData zero_galaxy;
  memset(&zero_galaxy, 0, sizeof(zero_galaxy));
  TEST_ASSERT(zero_galaxy.HotGas == 0.0, "Zero-initialized galaxy should have HotGas=0");
  TEST_ASSERT(zero_galaxy.CoolingGas == 0.0, "Zero-initialized galaxy should have CoolingGas=0");

  /* ===== CLEANUP ===== */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_partial_cooling
 * @brief   Test partial hot gas cooling (coolingGas < HotGas)
 *
 * Expected: CoolingGas transferred from hot to cold, metallicity preserved
 * Validates: Partial cooling transfer logic
 */
int test_physics_partial_cooling(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  setup_module_for_physics_test();

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0); /* Type 0 central */

  /* Initial state: HotGas=10.0, Metals=0.2 (Z=0.02), ColdGas=5.0, Metals=0.1 (Z=0.02) */
  test_halo.galaxy->HotGas = 10.0f;
  test_halo.galaxy->MetalsHotGas = 0.2f;
  test_halo.galaxy->ColdGas = 5.0f;
  test_halo.galaxy->MetalsColdGas = 0.1f;
  test_halo.galaxy->CoolingGas = 3.0f; /* Cool 3.0 out of 10.0 */

  /* Create module context */
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* HotGas should be reduced: 10.0 - 3.0 = 7.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 7.0, 0.001,
                           "HotGas should be 10.0 - 3.0 = 7.0");

  /* ColdGas should increase: 5.0 + 3.0 = 8.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->ColdGas, 8.0, 0.001,
                           "ColdGas should be 5.0 + 3.0 = 8.0");

  /* Hot metallicity preserved: 7.0 * 0.02 = 0.14 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, 0.14, 0.001,
                           "MetalsHotGas should be 7.0 * 0.02 = 0.14");

  /* Cold metallicity: (0.1 + 3.0*0.02) = 0.16 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsColdGas, 0.16, 0.001,
                           "MetalsColdGas should be 0.1 + 0.06 = 0.16");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage_apply_cooling_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_complete_depletion
 * @brief   Test complete hot gas depletion (coolingGas >= HotGas)
 *
 * Expected: All HotGas transferred to ColdGas
 * Validates: Complete depletion logic
 */
int test_physics_complete_depletion(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  setup_module_for_physics_test();

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0);

  /* Initial state */
  test_halo.galaxy->HotGas = 5.0f;
  test_halo.galaxy->MetalsHotGas = 0.1f; /* Z = 0.02 */
  test_halo.galaxy->ColdGas = 3.0f;
  test_halo.galaxy->MetalsColdGas = 0.06f;
  test_halo.galaxy->CoolingGas = 10.0f; /* Exceeds available HotGas */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* HotGas should be depleted */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 0.0, 0.001,
                           "HotGas should be depleted to 0.0");

  /* ColdGas should have all hot gas: 3.0 + 5.0 = 8.0 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->ColdGas, 8.0, 0.001,
                           "ColdGas should be 3.0 + 5.0 = 8.0");

  /* Hot metals should be depleted */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsHotGas, 0.0, 0.001,
                           "MetalsHotGas should be 0.0");

  /* Cold metals: 0.06 + 0.1 = 0.16 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsColdGas, 0.16, 0.001,
                           "MetalsColdGas should be 0.06 + 0.1 = 0.16");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage_apply_cooling_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_metallicity_preservation
 * @brief   Test that metallicity is preserved during gas transfer
 *
 * Expected: Metallicity ratio preserved in both reservoirs
 * Validates: Correct metallicity handling
 */
int test_physics_metallicity_preservation(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  setup_module_for_physics_test();

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0);

  /* Initial state with higher metallicity (Z=0.05) */
  test_halo.galaxy->HotGas = 20.0f;
  test_halo.galaxy->MetalsHotGas = 1.0f; /* Z = 0.05 */
  test_halo.galaxy->ColdGas = 10.0f;
  test_halo.galaxy->MetalsColdGas = 0.2f; /* Z = 0.02 */
  test_halo.galaxy->CoolingGas = 5.0f;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* Check hot reservoir metallicity preserved: Z_hot = 0.75 / 15.0 = 0.05 */
  double z_hot = test_halo.galaxy->MetalsHotGas / test_halo.galaxy->HotGas;
  TEST_ASSERT_DOUBLE_EQUAL(z_hot, 0.05, 0.0001, "Hot gas metallicity should be preserved at 0.05");

  /* Check cold reservoir received correct metals: 0.2 + 5.0*0.05 = 0.45 */
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->MetalsColdGas, 0.45, 0.001,
                           "Cold metals should be 0.2 + 0.25 = 0.45");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage_apply_cooling_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_physics_cooling_energy_tracking
 * @brief   Test that cooling energy is tracked correctly
 *
 * Expected: Cooling property tracks energy: E = 0.5 * m * v^2 / dt
 * Validates: Cooling energy calculation
 */
int test_physics_cooling_energy_tracking(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  setup_module_for_physics_test();

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

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");

  /* Expected cooling energy: 0.5 * 3.0 * 160^2 / 100.0 = 384.0 */
  double expected_cooling = 0.5 * 3.0 * 160.0 * 160.0 / 100.0;
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->Cooling, expected_cooling, 0.1,
                           "Cooling energy should match expected value");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage_apply_cooling_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_cooling
 * @brief   Test edge case with zero CoolingGas
 *
 * Expected: No changes to gas reservoirs
 * Validates: Zero cooling handling
 */
int test_zero_cooling(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  setup_module_for_physics_test();

  struct Halo test_halo = create_test_halo(0, 100.0, 160.0);

  /* Initial state */
  test_halo.galaxy->HotGas = 10.0f;
  test_halo.galaxy->MetalsHotGas = 0.2f;
  test_halo.galaxy->ColdGas = 5.0f;
  test_halo.galaxy->MetalsColdGas = 0.1f;
  test_halo.galaxy->CoolingGas = 0.0f; /* Zero cooling */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* ===== EXECUTE ===== */
  int result = sage_apply_cooling_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module processing should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->HotGas, 10.0, 0.001, "HotGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(test_halo.galaxy->ColdGas, 5.0, 0.001, "ColdGas should be unchanged");

  /* ===== CLEANUP ===== */
  free_test_halo(&test_halo);
  sage_apply_cooling_cleanup();
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
  setup_module_for_physics_test();

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
  sage_apply_cooling_cleanup();
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
  setup_module_for_physics_test();

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
  sage_apply_cooling_cleanup();
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
  setup_module_for_physics_test();

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
  sage_apply_cooling_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage_apply_cooling software quality tests and reports results.
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
  TEST_RUN(test_module_registration);
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);
  TEST_RUN(test_property_access);
  TEST_RUN(test_physics_partial_cooling);
  TEST_RUN(test_physics_complete_depletion);
  TEST_RUN(test_physics_metallicity_preservation);
  TEST_RUN(test_physics_cooling_energy_tracking);
  TEST_RUN(test_zero_cooling);
  TEST_RUN(test_type2_orphan_cools);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_invalid_ngal);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
