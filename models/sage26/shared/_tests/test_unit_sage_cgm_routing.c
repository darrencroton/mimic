/**
 * @file    test_unit_sage_cgm_routing.c
 * @brief   Unit tests of SAGE26's regime-aware hot-phase deposits (shared/sage_cgm_routing.h)
 *
 * SAGE26's add_metals_to_hot_reservoir and add_gas_to_hot_reservoir deposit into the CGM pair
 * when CGMrecipeOn is 1 and the receiving galaxy's Regime is 0, and into the hot pair
 * otherwise. Every input and deposit is a short binary fraction, so each sum is exact and each
 * expectation is a literal.
 *
 * - test_metals_helper_selected_field: mimic_sage_add_metals_to_hot_reservoir adds to exactly
 *   MetalsCGMgas at (CGMrecipeOn 1, Regime 0) and to exactly MetalsHotGas at (1, Regime 1) and
 *   (0, Regime 0); every other field unchanged;
 * - test_gas_helper_selected_pair: mimic_sage_add_gas_to_hot_reservoir adds gas and metals to
 *   exactly the CGM pair at (1, Regime 0) and to the hot pair at (1, Regime 1) and (0, Regime 0);
 *   every other field unchanged;
 * - test_selection_and_validator: mimic_sage_cgm_reservoir_selected is CGMrecipeOn 1 && Regime 0,
 *   and mimic_sage_validate_cgm_recipe_on accepts 0 and 1 only.
 */

#include "../../../../tests/framework/test_framework.h"
#include "include/types.h"
#include "shared/sage_cgm_routing.h"
#include "util/error.h"

#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** @brief A galaxy holding both pairs: hot 2.0 / 0.25, CGM 1.5 / 0.125, Regime as given */
static struct GalaxyData make_galaxy(const int regime) {
  struct GalaxyData g;
  memset(&g, 0, sizeof(g));
  g.Regime = regime;
  g.HotGas = 2.0f;
  g.MetalsHotGas = 0.25f;
  g.CGMgas = 1.5f;
  g.MetalsCGMgas = 0.125f;
  g.ColdGas = 0.75f;
  g.EjectedGas = 0.5f;
  return g;
}

/** @brief Whether the four pairs' fields and the two bystanders hold exactly these values */
static int holds(const struct GalaxyData *g, const float hot, const float metals_hot,
                 const float cgm, const float metals_cgm) {
  return g->HotGas == hot && g->MetalsHotGas == metals_hot && g->CGMgas == cgm &&
         g->MetalsCGMgas == metals_cgm && g->ColdGas == 0.75f && g->EjectedGas == 0.5f;
}

/** The three configurations: (CGMrecipeOn, Regime) and whether the CGM pair is selected */
static const struct {
  int cgm_recipe_on;
  int regime;
  int selects_cgm;
} RUNS[] = {{1, 0, 1}, {1, 1, 0}, {0, 0, 0}};

#define NUM_RUNS ((int)(sizeof(RUNS) / sizeof(RUNS[0])))

/**
 * @test    test_metals_helper_selected_field
 * @brief   The metals deposit 0.0625 reaches exactly the selected metals field
 */
int test_metals_helper_selected_field(void) {
  for (int k = 0; k < NUM_RUNS; k++) {
    struct GalaxyData g = make_galaxy(RUNS[k].regime);
    mimic_sage_add_metals_to_hot_reservoir(&g, RUNS[k].cgm_recipe_on, 0.0625);
    const int ok = RUNS[k].selects_cgm ? holds(&g, 2.0f, 0.25f, 1.5f, 0.1875f)
                                       : holds(&g, 2.0f, 0.3125f, 1.5f, 0.125f);
    printf("\n  CGMrecipeOn %d, Regime %d: hot %g / %g, CGM %g / %g", RUNS[k].cgm_recipe_on,
           RUNS[k].regime, (double)g.HotGas, (double)g.MetalsHotGas, (double)g.CGMgas,
           (double)g.MetalsCGMgas);
    TEST_ASSERT(ok, "the metals must reach exactly the selected field, every other unchanged");
  }
  return TEST_PASS;
}

/**
 * @test    test_gas_helper_selected_pair
 * @brief   The gas deposit 0.5 with metals 0.0625 reaches exactly the selected pair
 */
int test_gas_helper_selected_pair(void) {
  for (int k = 0; k < NUM_RUNS; k++) {
    struct GalaxyData g = make_galaxy(RUNS[k].regime);
    mimic_sage_add_gas_to_hot_reservoir(&g, RUNS[k].cgm_recipe_on, 0.5, 0.0625);
    const int ok = RUNS[k].selects_cgm ? holds(&g, 2.0f, 0.25f, 2.0f, 0.1875f)
                                       : holds(&g, 2.5f, 0.3125f, 1.5f, 0.125f);
    printf("\n  CGMrecipeOn %d, Regime %d: hot %g / %g, CGM %g / %g", RUNS[k].cgm_recipe_on,
           RUNS[k].regime, (double)g.HotGas, (double)g.MetalsHotGas, (double)g.CGMgas,
           (double)g.MetalsCGMgas);
    TEST_ASSERT(ok,
                "the gas and metals must reach exactly the selected pair, every other unchanged");
  }
  return TEST_PASS;
}

/**
 * @test    test_selection_and_validator
 * @brief   The selection is CGMrecipeOn 1 && Regime 0; only 0 and 1 are valid
 */
int test_selection_and_validator(void) {
  TEST_ASSERT(mimic_sage_cgm_reservoir_selected(1, 0) == 1, "CGMrecipeOn 1, Regime 0: CGM");
  TEST_ASSERT(mimic_sage_cgm_reservoir_selected(1, 1) == 0, "CGMrecipeOn 1, Regime 1: hot");
  TEST_ASSERT(mimic_sage_cgm_reservoir_selected(1, -1) == 0, "CGMrecipeOn 1, Regime -1: hot");
  TEST_ASSERT(mimic_sage_cgm_reservoir_selected(0, 0) == 0, "CGMrecipeOn 0, Regime 0: hot");
  TEST_ASSERT(mimic_sage_validate_cgm_recipe_on("test", 0) == 0, "0 is valid");
  TEST_ASSERT(mimic_sage_validate_cgm_recipe_on("test", 1) == 0, "1 is valid");
  TEST_ASSERT(mimic_sage_validate_cgm_recipe_on("test", 2) != 0, "2 is refused");
  TEST_ASSERT(mimic_sage_validate_cgm_recipe_on("test", -1) != 0, "-1 is refused");
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: Regime-aware hot-phase deposits (sage_cgm_routing.h)\n");
  printf("============================================================\n");

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_metals_helper_selected_field);
  TEST_RUN(test_gas_helper_selected_pair);
  TEST_RUN(test_selection_and_validator);

  TEST_SUMMARY();
  return TEST_RESULT();
}
