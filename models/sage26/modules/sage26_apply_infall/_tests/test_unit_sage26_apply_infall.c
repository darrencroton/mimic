/**
 * @file    test_unit_sage26_apply_infall.c
 * @brief   Unit tests for the sage26_apply_infall module
 *
 * Validates the infall application of SAGE26's add_infall_to_hot: the per-substep share of the
 * budget that sage26_prepare_infall_budget stores in InfallingGas goes to the central's selected
 * reservoir, and a negative budget is taken from the ejected gas first and then from the selected
 * reservoir, with metals following at the reservoir's metallicity.
 *
 * Test cases:
 *   - test_module_initialization: a pipeline with the budget module in pre_timestep initialises
 *   - test_physics_infall_budget_invariant_across_substep_counts: the budget is partitioned over
 *     the substeps, not recomputed, for substep counts from 1 to the dynamic maximum and for a
 *     zero budget
 *   - test_no_central_galaxy: no Type 0 row is a no-op
 *   - test_null_central_galaxy_is_an_error: a Type 0 row without galaxy data is an error
 *   - test_cgm_positive_infall_selected_pair: positive infall into the selected pair only
 *   - test_cgm_negative_infall_metal_debit: exhausted ejected gas, then the selected metals and
 *     gas are debited; with no ejected gas the whole loss is
 *   - test_cgm_negative_infall_from_ejected: ejected gas that covers the loss leaves the selected
 *     pair untouched
 *   - test_cgm_negative_infall_floor_and_clear: selected metals floored, both fields cleared
 *   - test_cgm_negative_infall_floor_without_clear: metals floored, gas exactly consumed
 *   - test_cgm_negative_infall_non_positive_metals: no debit of non-positive metals
 *
 * The CGMrecipeOn tests run each case at CGMrecipeOn 1 with Regime 0 (CGM pair selected), at
 * CGMrecipeOn 1 with Regime 1 and at CGMrecipeOn 0 (hot pair), with exact literal expectations.
 *
 * The CGMrecipeOn refusals of the module are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
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

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
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
 * @test    test_module_initialization
 * @brief   The budget module in pre_timestep and sage26_apply_infall in galaxy_physics initialise
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
  MimicConfig.pre_timestep[0].module_name = strdup("sage26_prepare_infall_budget");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;

  test_phase_add("galaxy_physics", "sage26_apply_infall", PROCESSING_MODE_FULL_HALO);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

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
 * sage26_satellite_stripping's N-dependent formula. Tolerances are absolute on the hot gas: the
 * first two rows are exact binary sums, the others allow the float rounding of N additions into
 * HotGas (1e-6 relative at N = 7 and 23, 1e-4 at the dynamic maximum;
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
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

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
      TEST_ASSERT(sage26_apply_infall_process(&ctx, &halo, 1) == 0, msg);
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

/**
 * @test    test_null_central_galaxy_is_an_error
 * @brief   A Type 0 row with no galaxy data fails with -1 instead of being skipped
 */
int test_null_central_galaxy_is_an_error(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  TEST_ASSERT(setup_module_for_physics_test() == 0, "init must succeed with CGMrecipeOn 0");

  struct Halo central;
  memset(&central, 0, sizeof(central));
  central.Type = 0;
  central.Mvir = 100.0;
  central.galaxy = NULL;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;

  /* ===== EXECUTE AND VALIDATE ===== */
  TEST_ASSERT(sage26_apply_infall_process(&ctx, &central, 1) == -1,
              "a central without galaxy data must be an error");

  /* ===== CLEANUP ===== */
  sage26_apply_infall_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// CGMrecipeOn 1 AGAINST 0
// ============================================================================
//
// SAGE26's add_infall_to_hot selects the central's CGM pair for CGMrecipeOn 1 and Regime 0 and
// its hot pair otherwise. Each case runs the same central at CGMrecipeOn 1 with Regime 0 (CGM
// selected), at CGMrecipeOn 1 with Regime 1 and at CGMrecipeOn 0 with Regime 0 (hot selected in
// both), and asserts the selected pair, the unselected pair unchanged and the ejected pair.
// Every input is a short binary fraction (or, for the floor without a clear, a literal float
// pair), so each expectation is an exact literal.

/** @brief Initialise with the budget module in pre_timestep and CGMrecipeOn `cgm`
 */
static int cgm_init(const char *cgm) {
  static struct PhaseModuleConfig pre_timestep[1];
  static char budget_name[] = "sage26_prepare_infall_budget";
  reset_config();
  pre_timestep[0].module_name = budget_name;
  pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep;
  MimicConfig.num_pre_timestep = 1;
  sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  const int status = sage26_apply_infall_init();
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  return status;
}

/** @brief The central's reservoirs: ejected, hot and CGM pairs */
struct cgm_pairs {
  float ejected, metals_ejected, hot, metals_hot, cgm, metals_cgm;
};

/** @brief Run one central (Type 0, given Regime and reservoirs) at CGMrecipeOn `cgm` */
static int cgm_apply(const char *cgm, const int regime, const struct cgm_pairs *in,
                     const double infalling_gas, const int substeps, struct cgm_pairs *out) {
  if (cgm_init(cgm) != 0) {
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

static int cgm_same_bits(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

/**
 * @brief Run the three configurations and compare: the selected pair against (gas, metals), the
 *        unselected pair against its input, the ejected pair against (ejected, metals_ejected)
 * @return 1 when every configuration matches
 */
static int cgm_check_routing(const struct cgm_pairs *in, const double infalling_gas,
                             const int substeps, const float gas, const float metals,
                             const float ejected, const float metals_ejected) {
  const struct {
    const char *cgm;
    int regime;
    int selects_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}};
  int ok = 1;
  for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
    struct cgm_pairs out;
    if (cgm_apply(runs[k].cgm, runs[k].regime, in, infalling_gas, substeps, &out) != 0) {
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
        cgm_same_bits(sel_gas, gas) && cgm_same_bits(sel_metals, metals) &&
        cgm_same_bits(other_gas, other_gas_in) && cgm_same_bits(other_metals, other_metals_in) &&
        cgm_same_bits(out.ejected, ejected) && cgm_same_bits(out.metals_ejected, metals_ejected);
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
 * @brief   Positive infall adds gas, and no metals, to the selected pair only
 */
int test_cgm_positive_infall_selected_pair(void) {
  /* Both pairs hold 2.0 / 0.25: budget 1.0 over 4 substeps is 0.25 per substep */
  const struct cgm_pairs in = {0.5f, 0.0625f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(cgm_check_routing(&in, 1.0, 4, 2.25f, 0.25f, 0.5f, 0.0625f),
              "positive infall must add 0.25 to the selected gas only");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_metal_debit
 * @brief   Negative infall exhausts the ejected gas, then debits the selected metals at their
 *          metallicity and the selected gas
 *
 * Ejected 0.5 / 0.0625 (Z 1/8) takes -2.0: its metals floor at 0 and the residual is the narrowed
 * -1.5. The selected pair 2.0 / 0.25 (Z 1/8) then loses 1.5 and 0.1875: 0.5 / 0.0625. With no
 * ejected gas the same -1.5 is the whole loss and debits the selected pair identically.
 */
int test_cgm_negative_infall_metal_debit(void) {
  const struct cgm_pairs in = {0.5f, 0.0625f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(cgm_check_routing(&in, -2.0, 1, 0.5f, 0.0625f, 0.0f, 0.0f),
              "the residual must debit the selected pair only");
  const struct cgm_pairs no_ejected = {0.0f, 0.0f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(cgm_check_routing(&no_ejected, -1.5, 1, 0.5f, 0.0625f, 0.0f, 0.0f),
              "with no ejected gas the whole loss must debit the selected pair only");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_from_ejected
 * @brief   Ejected gas that covers the loss absorbs all of it, whichever pair is selected
 *
 * Ejected 1.0 / 0.125 (Z 1/8) takes -0.5: 0.5 / 0.0625 remain and both selected and unselected
 * pairs keep their inputs.
 */
int test_cgm_negative_infall_from_ejected(void) {
  const struct cgm_pairs in = {1.0f, 0.125f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(cgm_check_routing(&in, -0.5, 1, 2.0f, 0.25f, 0.5f, 0.0625f),
              "covered by the ejected gas, the selected pair must be untouched");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_floor_and_clear
 * @brief   Negative infall beyond the selected gas floors its metals and clears both selected
 *          fields
 *
 * No ejected gas; -4.0 against 2.0 / 0.25 (Z 1/8): metals 0.25 - 0.5 floor at 0, gas -2 clears.
 */
int test_cgm_negative_infall_floor_and_clear(void) {
  const struct cgm_pairs in = {0.0f, 0.0f, 2.0f, 0.25f, 2.0f, 0.25f};
  TEST_ASSERT(cgm_check_routing(&in, -4.0, 1, 0.0f, 0.0f, 0.0f, 0.0f),
              "the selected pair must be cleared, the other unchanged");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_floor_without_clear
 * @brief   The selected metals floor at zero while the gas reaches exactly zero without being
 *          cleared: 1.5 of gas with 0x3c75c290 (0.0150000006) of metals takes exactly -1.5
 *
 * The float metallicity of that pair rounds above M / G, so M - 1.5 * Z is negative (exactly, by
 * any rounding of the exact product of two floats) and is floored to +0, while 1.5 - 1.5 = +0 is
 * not negative and clears nothing.
 */
int test_cgm_negative_infall_floor_without_clear(void) {
  const uint32_t metals_bits = 0x3c75c290u;
  float metals;
  memcpy(&metals, &metals_bits, sizeof(metals));
  const struct cgm_pairs in = {0.0f, 0.0f, 1.5f, metals, 1.5f, metals};
  TEST_ASSERT(cgm_check_routing(&in, -1.5, 1, 0.0f, 0.0f, 0.0f, 0.0f),
              "the selected metals must floor at +0 with the gas exactly consumed");
  return TEST_PASS;
}

/**
 * @test    test_cgm_negative_infall_non_positive_metals
 * @brief   Selected metals that are not positive are neither debited nor floored:
 *          -2^-30 stays -2^-30 while the gas loses 1.0
 */
int test_cgm_negative_infall_non_positive_metals(void) {
  const float tiny_negative = -0x1p-30f;
  const struct cgm_pairs in = {0.0f, 0.0f, 2.0f, tiny_negative, 2.0f, tiny_negative};
  TEST_ASSERT(cgm_check_routing(&in, -1.0, 1, 1.0f, tiny_negative, 0.0f, 0.0f),
              "non-positive selected metals must be left as they are");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_apply_infall unit tests and reports results.
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
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_physics_infall_budget_invariant_across_substep_counts);
  TEST_RUN(test_no_central_galaxy);
  TEST_RUN(test_null_central_galaxy_is_an_error);

  /* CGMrecipeOn 1 against 0 */
  TEST_RUN(test_cgm_positive_infall_selected_pair);
  TEST_RUN(test_cgm_negative_infall_metal_debit);
  TEST_RUN(test_cgm_negative_infall_from_ejected);
  TEST_RUN(test_cgm_negative_infall_floor_and_clear);
  TEST_RUN(test_cgm_negative_infall_floor_without_clear);
  TEST_RUN(test_cgm_negative_infall_non_positive_metals);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
