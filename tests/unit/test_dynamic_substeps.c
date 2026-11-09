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
    int min_dynamic_substeps;
    int max_dynamic_substeps;
    int expected;
  } cases[] = {
      {"low-z interval below dynamical time", 0.2, 1.0, 3, 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 1},
      {"exact integer dynamical resolution", 2.0, 1.0, 4, 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 8},
      {"ceil fractional request", 1.1, 1.0, 4, 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 5},
      {"high-z interval spans many dynamical times", 2.0, 0.25, 5, 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       40},
      {"negative requested resolution defaults to one", 2.0, 1.0, -3, 1,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS, 2},
      {"zero requested resolution defaults to one", 2.0, 1.0, 0, 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       2},
      {"clamps at default maximum", 1000.0, 0.1, 10, 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS},
      {"clamps at a caller-supplied maximum below the default", 1000.0, 0.1, 10, 1, 50, 50},
      {"clamps at a caller-supplied maximum above the default", 1000.0, 0.1, 10, 1, 500, 500},
      {"non-positive caller-supplied maximum falls back to the default", 1000.0, 0.1, 10, 1, 0,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS},
      {"negative caller-supplied maximum falls back to the default", 1000.0, 0.1, 10, 1, -1,
       DEFAULT_MAX_DYNAMIC_SUBSTEPS},
      {"floor above the request", 0.2, 1.0, 3, 4, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 4},
      {"floor equal to the request", 2.0, 1.0, 4, 8, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 8},
      {"floor below the request leaves it unchanged", 2.0, 1.0, 4, 3, DEFAULT_MAX_DYNAMIC_SUBSTEPS,
       8},
      {"floor above the ceiling clamps to the ceiling", 0.2, 1.0, 3, 20, 10, 10},
      {"ceiling wins over a request above it", 1000.0, 0.1, 10, 4, 50, 50},
      {"non-positive floor behaves as one", 0.2, 1.0, 3, 0, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 1},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int actual =
        compute_dynamic_substeps(cases[i].time_interval, cases[i].t_dyn, cases[i].substeps_per_tdyn,
                                 cases[i].min_dynamic_substeps, cases[i].max_dynamic_substeps);
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
    int actual =
        compute_dynamic_substeps(cases[i].time_interval, cases[i].t_dyn, cases[i].substeps_per_tdyn,
                                 1, DEFAULT_MAX_DYNAMIC_SUBSTEPS);
    TEST_ASSERT(actual == 1, cases[i].label);

    /* A configured floor is a true floor: degenerate input returns it, not 1 */
    actual = compute_dynamic_substeps(cases[i].time_interval, cases[i].t_dyn,
                                      cases[i].substeps_per_tdyn, 4, DEFAULT_MAX_DYNAMIC_SUBSTEPS);
    TEST_ASSERT(actual == 4, cases[i].label);
  }

  return TEST_PASS;
}

static int test_dynamic_substep_overflow_guard(void) {
  int actual = compute_dynamic_substeps(1.0e308, 1.0e-308, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 1,
                                        DEFAULT_MAX_DYNAMIC_SUBSTEPS);
  TEST_ASSERT(actual == 1, "Non-finite requested substep count should return one");

  actual = compute_dynamic_substeps(1.0e308, 1.0e-308, DEFAULT_MAX_DYNAMIC_SUBSTEPS, 4,
                                    DEFAULT_MAX_DYNAMIC_SUBSTEPS);
  TEST_ASSERT(actual == 4, "Non-finite requested substep count should return the floor");

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
