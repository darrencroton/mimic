/**
 * @file    test_rate_faults.c
 * @brief   Rate fixture module: a sink out of cold whose law injects a chosen fault
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * The fault injector of the model-neutral rate fixture. Its law returns one
 * coefficient, the specific rate of the sink `fault` out of RateFixtureCold, and
 * the parameter TestRateFaultsLaw selects what it returns (README.md states the
 * laws):
 * - well_behaved: TestRateFaultsRate
 * - diverging: TestRateFaultsRate / RateFixtureCold, which grows without bound as the
 *   donor empties (0 on an empty donor)
 * - not_a_number: NaN
 * - negative: -1
 * - impure: TestRateFaultsRate plus a term that grows with a static call counter, so
 *   the same row and state give a different coefficient on every call
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <math.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "module_interface.h"
#include "module_system/generated/rate_contracts.h"
#include "module_system/parameter_helpers.h"
#include "types.h"

/** The fault a run selects (TestRateFaultsLaw) */
enum FaultLaw {
  FAULT_WELL_BEHAVED,
  FAULT_DIVERGING,
  FAULT_NOT_A_NUMBER,
  FAULT_NEGATIVE,
  FAULT_IMPURE,
  FAULT_LAW_COUNT
};

/** The accepted TestRateFaultsLaw values, indexed by enum FaultLaw */
static const char *const fault_law_names[FAULT_LAW_COUNT] = {"well_behaved", "diverging",
                                                             "not_a_number", "negative", "impure"};

/** The selected law */
static enum FaultLaw fault_law = FAULT_WELL_BEHAVED;

/** The law's rate scale, in inverse internal time units (a content per time for diverging) */
static double fault_rate = 0.0;

/** Calls of the impure law since init(): the hidden state that makes it impure */
static unsigned long impure_calls = 0;

/**
 * @brief   Initialize the faults fixture
 *
 * Reads the two required parameters; refuses an unknown law name and a rate that
 * is not finite and positive.
 *
 * @return  0 on success, -1 on a missing or invalid parameter
 */
int test_rate_faults_init(void) {
  char law_name[MAX_STRING_LEN];
  LOAD_PARAM_STRING("TestRateFaultsLaw", law_name, MAX_STRING_LEN);
  LOAD_PARAM_DOUBLE("TestRateFaultsRate", fault_rate);

  int found = -1;
  for (int i = 0; i < FAULT_LAW_COUNT; i++) {
    if (strcmp(law_name, fault_law_names[i]) == 0) {
      found = i;
    }
  }
  if (found < 0) {
    ERROR_LOG("TestRateFaultsLaw '%s' is not one of well_behaved, diverging, not_a_number, "
              "negative, impure",
              law_name);
    return -1;
  }
  if (!isfinite(fault_rate) || fault_rate <= 0.0) {
    ERROR_LOG("TestRateFaultsRate must be finite and positive (got %g)", fault_rate);
    return -1;
  }
  fault_law = (enum FaultLaw)found;
  impure_calls = 0;

  INFO_LOG("Test rate faults fixture initialized (law %s, rate %g)", fault_law_names[fault_law],
           fault_rate);
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/**
 * @brief   Return the faults fixture's coefficient for one row
 *
 * @param   ctx           Rate context (unused: the law depends on the row alone)
 * @param   halo          Scratch copy of the row at the trial state
 * @param   coefficients  One slot per declared transfer, zeroed by the caller
 * @return  0 on success
 */
int test_rate_faults_rates(const struct RateContext *ctx, const struct Halo *halo,
                           double *coefficients) {
  (void)ctx;
  const double cold = halo->galaxy->RateFixtureCold;
  double coefficient = 0.0;
  switch (fault_law) {
  case FAULT_WELL_BEHAVED:
    coefficient = fault_rate;
    break;
  case FAULT_DIVERGING:
    coefficient = cold > 0.0 ? fault_rate / cold : 0.0;
    break;
  case FAULT_NOT_A_NUMBER:
    coefficient = NAN;
    break;
  case FAULT_NEGATIVE:
    coefficient = -1.0;
    break;
  case FAULT_IMPURE:
    impure_calls++;
    coefficient = fault_rate + 1.0e-3 * (double)impure_calls;
    break;
  case FAULT_LAW_COUNT:
    break;
  }
  coefficients[TEST_RATE_FAULTS_TRANSFER_FAULT] = coefficient;
  return 0;
}

/**
 * @brief   Clean up the faults fixture (it holds no resources)
 *
 * @return  0
 */
int test_rate_faults_cleanup(void) { return 0; }
