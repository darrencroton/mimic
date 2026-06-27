/**
 * @file    test_module_configuration.c
 * @brief   Unit tests for multi-phase module configuration system
 *
 * Validates module registration and multi-phase execution pipeline configuration.
 */

#include "../framework/test_framework.h"
#include "../../src/core/module_registry.h"
#include "../framework/test_phase_config.h"
#include "../../src/core/module_interface.h"
#include "../../src/include/types.h"
#include "../../src/include/proto.h"
#include "../../src/include/globals.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Shared core-test fixtures (config reset, registration) */
#include "../framework/core_test_fixtures.h"

/* Test fixture: Set test_fixture parameters in centralized model_parameters */
static void set_test_fixture_params(double dummy_val, int logging_val) {
  int idx = 0;

  strcpy(MimicConfig.ModelParams[idx].param_name, "TestFixtureDummyParameter");
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%.10g", dummy_val);
  idx++;

  strcpy(MimicConfig.ModelParams[idx].param_name, "TestFixtureEnableLogging");
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%d", logging_val);
  idx++;

  MimicConfig.NumModelParams = idx;
}

/**
 * @test    test_module_registry_init
 * @brief   Test module registry initialization
 *
 * Expected: Registry initializes without errors, modules can be registered
 * Validates: Basic module registration system works
 */
int test_module_registry_init(void) {
  /* ===== SETUP ===== */
  reset_config();

  /* ===== EXECUTE ===== */
  /* Registry should initialize without explicit init call (static storage) */
  /* Register test modules via register_all_modules() */
  ensure_modules_registered();

  /* ===== VALIDATE ===== */
  /* If we got here without crashing, registration succeeded */
  /* (Module registry is internal, so we can't directly inspect it) */

  return TEST_PASS;
}

/**
 * @test    test_phase_configuration
 * @brief   Test multi-phase pipeline configuration
 *
 * Expected: Phase arrays configured correctly with module names and loop modes
 * Validates: Multi-phase configuration structure works
 */
int test_phase_configuration(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);

  /* ===== EXECUTE ===== */
  /* Configure modules across multiple phases */
  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("test_fixture");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;

  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);

  MimicConfig.SubSteps = 1;

  /* ===== VERIFY ===== */
  TEST_ASSERT_EQUAL(MimicConfig.num_pre_timestep, 1, "Should have 1 module in pre_timestep");
  TEST_ASSERT_EQUAL(MimicConfig.num_substep_phases, 1, "Should have 1 substep phase");
  TEST_ASSERT_EQUAL(MimicConfig.substep_phases[0].num_modules, 1,
                    "Should have 1 module in the substep phase");
  TEST_ASSERT_STRING_EQUAL(MimicConfig.pre_timestep[0].module_name, "test_fixture",
                           "pre_timestep module should be test_fixture");
  TEST_ASSERT_EQUAL(MimicConfig.pre_timestep[0].processing_mode, PROCESSING_MODE_FULL_HALO,
                    "pre_timestep loop mode should be PROCESSING_MODE_FULL_HALO");
  TEST_ASSERT_STRING_EQUAL(MimicConfig.substep_phases[0].modules[0].module_name, "test_fixture",
                           "substep phase module should be test_fixture");
  TEST_ASSERT_EQUAL(MimicConfig.substep_phases[0].modules[0].processing_mode,
                    PROCESSING_MODE_BY_GALAXY,
                    "substep phase module mode should be PROCESSING_MODE_BY_GALAXY");

  return TEST_PASS;
}

/**
 * @test    test_physics_free_mode
 * @brief   Test physics-free mode (no modules enabled, all phases empty)
 *
 * Expected: module_system_init() succeeds with all phases empty
 * Validates: Core can run without any physics modules
 */
int test_physics_free_mode(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* No modules in any phase (all NULL, counts = 0) */
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  MimicConfig.substep_phases = NULL;
  MimicConfig.num_substep_phases = 0;
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
  MimicConfig.SubSteps = 1;

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VERIFY ===== */
  TEST_ASSERT_EQUAL(result, 0, "module_system_init should succeed in physics-free mode");

  /* ===== CLEANUP ===== */
  module_system_cleanup();

  return TEST_PASS;
}

/**
 * @test    test_empty_named_phase_cleanup
 * @brief   Test cleanup releases empty named substep phases in physics-free mode
 *
 * Expected: module_system_cleanup() frees named phase config even when no modules
 * are initialized.
 * Validates: Empty named phases do not leak when the pipeline is physics-free
 */
int test_empty_named_phase_cleanup(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.substep_phases =
      mymalloc_cat(MAX_SUBSTEP_PHASES * sizeof(struct ModulePhaseConfig), MEM_UTILITY);
  MimicConfig.substep_phases[0].name = strdup("galaxy_physics");
  MimicConfig.substep_phases[0].modules = NULL;
  MimicConfig.substep_phases[0].num_modules = 0;
  MimicConfig.num_substep_phases = 1;
  MimicConfig.SubSteps = 1;

  /* ===== EXECUTE ===== */
  int result = module_system_init();
  module_system_cleanup();

  /* ===== VERIFY ===== */
  TEST_ASSERT_EQUAL(result, 0, "module_system_init should succeed with an empty named phase");
  TEST_ASSERT(MimicConfig.substep_phases == NULL,
              "cleanup should release empty named phase arrays");
  TEST_ASSERT_EQUAL(MimicConfig.num_substep_phases, 0, "cleanup should reset named phase count");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_valid_module_initialization
 * @brief   Test initializing valid modules across phases
 *
 * Expected: module_system_init() succeeds with modules in multiple phases
 * Validates: Multi-phase pipeline builds correctly
 */
int test_valid_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set test_fixture parameters */
  set_test_fixture_params(1.0, 0);

  /* Configure modules in multiple phases */
  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("test_fixture");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;

  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);

  MimicConfig.SubSteps = 1;

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VERIFY ===== */
  TEST_ASSERT_EQUAL(result, 0, "module_system_init should succeed with valid modules");

  /* ===== CLEANUP ===== */
  module_system_cleanup();

  return TEST_PASS;
}

/**
 * @test    test_unknown_module_error
 * @brief   Test error handling for unknown module names
 *
 * Expected: module_system_init() exits with error for invalid module
 * Validates: Invalid module names are detected and reported
 *
 * Skipped: module_system_init() calls exit() on invalid module names
 * (fail-fast design), so this needs process isolation. The behavior is
 * covered by tests/integration/test_module_pipeline.py
 * (test_unknown_module_error), which runs Mimic as a subprocess.
 */
int test_unknown_module_error(void) {
  return TEST_SKIP_WITH("requires process isolation; covered by test_module_pipeline.py");
}

/**
 * @test    test_single_phase_configuration
 * @brief   Test initializing modules in a single phase only
 *
 * Expected: System works with modules in only one phase
 * Validates: Partial phase configurations are supported
 */
int test_single_phase_configuration(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set test_fixture parameters */
  set_test_fixture_params(1.0, 0);

  /* Enable module only in galaxy_physics */
  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);

  MimicConfig.SubSteps = 1;

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VERIFY ===== */
  TEST_ASSERT_EQUAL(result, 0, "module_system_init should succeed with single phase configured");

  /* ===== CLEANUP ===== */
  module_system_cleanup();

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Multi-Phase Module Configuration System\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);

  TEST_RUN(test_module_registry_init);
  TEST_RUN(test_phase_configuration);
  TEST_RUN(test_physics_free_mode);
  TEST_RUN(test_empty_named_phase_cleanup);
  TEST_RUN(test_valid_module_initialization);
  TEST_RUN(test_unknown_module_error);
  TEST_RUN(test_single_phase_configuration);

  TEST_SUMMARY();

  printf("\n");
  printf("Memory leak check:\n");
  print_allocated();

  return TEST_RESULT();
}
