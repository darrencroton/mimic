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
    int max_dynamic_substeps;
    int expected;
  } cases[] = {
      {"low-z interval below dynamical time", 0.2, 1.0, 3, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 1},
      {"exact integer dynamical resolution", 2.0, 1.0, 4, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 8},
      {"ceil fractional request", 1.1, 1.0, 4, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 5},
      {"high-z interval spans many dynamical times", 2.0, 0.25, 5, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       40},
      {"negative requested resolution defaults to one", 2.0, 1.0, -3, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       2},
      {"zero requested resolution defaults to one", 2.0, 1.0, 0, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 2},
      {"clamps at default maximum", 1000.0, 0.1, 10, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS},
      {"clamps at a caller-supplied maximum below the default", 1000.0, 0.1, 10, 50, 50},
      {"clamps at a caller-supplied maximum above the default", 1000.0, 0.1, 10, 500, 500},
      {"non-positive caller-supplied maximum falls back to the default", 1000.0, 0.1, 10, 0,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS},
      {"negative caller-supplied maximum falls back to the default", 1000.0, 0.1, 10, -1,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int actual =
        compute_dynamic_substeps(cases[i].time_interval, cases[i].t_dyn, cases[i].substeps_per_tdyn,
                                 cases[i].max_dynamic_substeps);
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
                                          cases[i].substeps_per_tdyn, DEFAULT_MAX_DYNAMIC_SUBSTEPS);
    TEST_ASSERT(actual == 1, cases[i].label);
  }

  return TEST_PASS;
}

static int test_dynamic_substep_overflow_guard(void) {
  int actual = compute_dynamic_substeps(1.0e308, 1.0e-308, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
                                        DEFAULT_MAX_DYNAMIC_SUBSTEPS);
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
