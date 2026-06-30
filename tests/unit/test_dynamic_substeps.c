/**
 * @file    test_dynamic_substeps.c
 * @brief   Unit tests for dynamic timestep substep-count computation.
 */

#include "../framework/test_framework.h"
#include "../../src/include/constants.h"
#include "../../src/include/proto.h"

#include <math.h>
#include <stddef.h>

/* Test statistics (required by TEST_RUN) */
static int passed = 0;
static int failed = 0;

static int test_dynamic_substep_formula(void) {
  struct {
    const char *label;
    double time_interval;
    double t_dyn;
    int substeps_per_tdyn;
    int expected;
  } cases[] = {
      {"low-z interval below dynamical time", 0.2, 1.0, 3, 1},
      {"exact integer dynamical resolution", 2.0, 1.0, 4, 8},
      {"ceil fractional request", 1.1, 1.0, 4, 5},
      {"high-z interval spans many dynamical times", 2.0, 0.25, 5, 40},
      {"negative requested resolution defaults to one", 2.0, 1.0, -3, 2},
      {"zero requested resolution defaults to one", 2.0, 1.0, 0, 2},
      {"clamps at dynamic maximum", 100.0, 0.1, 10, MAX_DYNAMIC_SUBSTEPS},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int actual = compute_dynamic_substeps(cases[i].time_interval, cases[i].t_dyn,
                                          cases[i].substeps_per_tdyn);
    TEST_ASSERT(actual == cases[i].expected, cases[i].label);
  }

  return TEST_PASS;
}

static int test_dynamic_substep_degenerate_inputs(void) {
  struct {
    const char *label;
    double time_interval;
    double t_dyn;
    int substeps_per_tdyn;
  } cases[] = {
      {"first snapshot interval", 0.0, 1.0, 10},     {"negative time interval", -1.0, 1.0, 10},
      {"zero dynamical time", 1.0, 0.0, 10},         {"negative dynamical time", 1.0, -1.0, 10},
      {"nan time interval", NAN, 1.0, 10},           {"nan dynamical time", 1.0, NAN, 10},
      {"infinite time interval", INFINITY, 1.0, 10}, {"infinite dynamical time", 1.0, INFINITY, 10},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int actual = compute_dynamic_substeps(cases[i].time_interval, cases[i].t_dyn,
                                          cases[i].substeps_per_tdyn);
    TEST_ASSERT(actual == 1, cases[i].label);
  }

  return TEST_PASS;
}

static int test_dynamic_substep_overflow_guard(void) {
  int actual = compute_dynamic_substeps(1.0e308, 1.0e-308, MAX_DYNAMIC_SUBSTEPS);
  TEST_ASSERT(actual == 1, "Non-finite requested substep count should return one");

  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_dynamic_substep_formula);
  TEST_RUN(test_dynamic_substep_degenerate_inputs);
  TEST_RUN(test_dynamic_substep_overflow_guard);

  TEST_SUMMARY();
  return TEST_RESULT();
}
