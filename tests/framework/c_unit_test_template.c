/**
 * @file    test_unit_mymodule.c (template)
 * @brief   Unit-test template for a Mimic physics module
 *
 * HOW TO USE THIS TEMPLATE (works for any model package, not just sage16):
 *
 *   1. Copy to models/<model>/modules/<mymodule>/_tests/test_unit_<mymodule>.c
 *   2. Rename every 'mymodule' to your module name (it is a valid identifier,
 *      so a plain search-and-replace and the code formatter are both safe).
 *   3. Declare the test in your module_info.yaml:
 *        tests:
 *          unit: _tests/test_unit_<mymodule>.c
 *      The registry generator discovers it from there (make tests-unit).
 *   4. Replace the example physics tests with checks of your module's actual
 *      calculations, edge cases, and conservation laws.
 *
 * Include paths: tests/unit/run_tests.sh compiles module tests with -I flags
 * for src/, the framework, and your model root, so the includes below resolve
 * as written from any models/<model>/modules/<mymodule>/_tests/ directory.
 *
 * Fixtures: prefer a model-owned shared fixture header over inline copies.
 * sage16 keeps its counters/reset/registration fixtures in
 * models/sage16/modules/_tests/sage_test_fixtures.h and every test includes
 * it; a new model package should create the equivalent header once and do the
 * same. The inline fixtures below are the minimal stand-alone fallback for a
 * model that does not have its shared header yet.
 *
 * KEY PRINCIPLES:
 *   - C unit tests validate PHYSICS and MATH via direct function calls, not
 *     the full pipeline (that is the integration tier's job).
 *   - Test normal cases, edge cases (zero/boundary values), parameter
 *     sensitivity, and conservation laws where applicable.
 *   - Every test path that allocates must end with check_memory_leaks().
 *   - A test that cannot run in this configuration returns
 *     TEST_SKIP_WITH("reason") so it is reported as a SKIP, never as a pass.
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Fixtures — replace this block with your model's shared fixture header once
 * it exists (see models/sage16/modules/_tests/sage_test_fixtures.h):
 *
 *   #include "modules/_tests/mymodel_test_fixtures.h"
 * ------------------------------------------------------------------------- */

/* Test statistics (required by TEST_RUN) */
static int passed = 0;
static int failed = 0;

/* Reset global configuration state between tests */
static void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }

/* Register all compiled modules exactly once */
static void ensure_modules_registered(void) {
  static int modules_registered = 0;
  if (!modules_registered) {
    register_all_modules();
    modules_registered = 1;
  }
}

/* Set the model parameters your module's init() requires. A model package's
 * shared fixture header normally provides this for the whole model. */
static void set_test_parameters(double efficiency) {
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "MyModuleEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.10g", efficiency);
  MimicConfig.NumModelParams = idx;
}

/* ---------------------------------------------------------------------------
 * Module under test — direct entry points for unit-level calls.
 * ------------------------------------------------------------------------- */
extern int mymodule_init(void);
extern int mymodule_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int mymodule_cleanup(void);

/* Build a minimal halo + galaxy pair for direct process() calls. Extend with
 * whatever properties your module reads; everything else stays zeroed. */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir,
                            double vvir) {
  memset(halo, 0, sizeof(*halo));
  memset(galaxy, 0, sizeof(*galaxy));
  halo->Type = type;
  halo->Mvir = mvir;
  halo->Vvir = vvir;
  halo->SnapNum = 63;
  halo->galaxy = galaxy;
}

/* Build a ModuleContext for a single-substep call at z=0 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central, double dt) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->central_galaxy = central;
  ctx->substep_dt = dt;
  ctx->params = &MimicConfig;
  ctx->redshift = 0.0;
  ctx->time = 13.8; /* Gyr */
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
}

/* ---------------------------------------------------------------------------
 * Lifecycle tests
 * ------------------------------------------------------------------------- */

/* Module init/cleanup succeed through the pipeline with no leaks */
int test_module_initialization(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  test_phase_add("galaxy_physics", "mymodule", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_parameters(1.0);

  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Physics tests — one focused check per behavior. Replace the bodies with
 * your module's real calculations and expected values.
 * ------------------------------------------------------------------------- */

/* Normal case: known inputs produce the analytically expected output */
int test_basic_physics_calculation(void) {
  reset_config();
  init_memory_system(0);
  set_test_parameters(1.0);
  TEST_ASSERT(mymodule_init() == 0, "Module initialization should succeed");

  struct Halo central;
  struct GalaxyData central_galaxy;
  setup_test_halo(&central, &central_galaxy, 0, 100.0, 150.0);
  /* central_galaxy.SomeInput = ...; */

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central, 0.1);

  TEST_ASSERT(mymodule_process(&ctx, &central, 1) == 0, "Module process should succeed");
  /* double expected = ...analytic value...;
   * TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.SomeOutput, expected, 1e-6,
   *                          "Output should match the analytic expectation"); */

  TEST_ASSERT(mymodule_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/* Edge case: zero input is handled without producing garbage */
int test_edge_case_zero_input(void) {
  reset_config();
  init_memory_system(0);
  set_test_parameters(1.0);
  TEST_ASSERT(mymodule_init() == 0, "Module initialization should succeed");

  struct Halo central;
  struct GalaxyData central_galaxy;
  setup_test_halo(&central, &central_galaxy, 0, 100.0, 150.0);
  /* all inputs left at zero */

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central, 0.1);

  TEST_ASSERT(mymodule_process(&ctx, &central, 1) == 0,
              "Module should handle zero input gracefully");
  /* TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.SomeOutput, 0.0, 1e-12,
   *                          "Zero input should produce zero output"); */

  TEST_ASSERT(mymodule_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/* Conservation: the quantity your module moves between reservoirs is
 * conserved across the call (mass, metals, energy — whichever applies) */
int test_conservation_law(void) {
  reset_config();
  init_memory_system(0);
  set_test_parameters(1.0);
  TEST_ASSERT(mymodule_init() == 0, "Module initialization should succeed");

  struct Halo central;
  struct GalaxyData central_galaxy;
  setup_test_halo(&central, &central_galaxy, 0, 100.0, 150.0);
  /* central_galaxy.ReservoirA = 1.0;
   * double initial_total = central_galaxy.ReservoirA + central_galaxy.ReservoirB; */

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central, 0.1);

  TEST_ASSERT(mymodule_process(&ctx, &central, 1) == 0, "Module process should succeed");
  /* double final_total = central_galaxy.ReservoirA + central_galaxy.ReservoirB;
   * TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, 1e-10,
   *                          "Total mass should be conserved by the transfer"); */

  TEST_ASSERT(mymodule_cleanup() == 0, "Module cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/* Parameter sensitivity: changing the parameter changes the result */
int test_parameter_sensitivity(void) {
  reset_config();
  init_memory_system(0);

  /* Run 1: efficiency = 1.0 */
  set_test_parameters(1.0);
  TEST_ASSERT(mymodule_init() == 0, "Module initialization should succeed");
  /* ...run process(), record result1, cleanup... */
  TEST_ASSERT(mymodule_cleanup() == 0, "Module cleanup should succeed");

  /* Run 2: efficiency = 2.0 */
  set_test_parameters(2.0);
  TEST_ASSERT(mymodule_init() == 0, "Module re-initialization should succeed");
  /* ...run process(), record result2, cleanup...
   * TEST_ASSERT(result2 != result1, "Parameter should affect the result"); */
  TEST_ASSERT(mymodule_cleanup() == 0, "Module cleanup should succeed");

  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: mymodule\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_module_initialization);
  TEST_RUN(test_basic_physics_calculation);
  TEST_RUN(test_edge_case_zero_input);
  TEST_RUN(test_conservation_law);
  TEST_RUN(test_parameter_sensitivity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
