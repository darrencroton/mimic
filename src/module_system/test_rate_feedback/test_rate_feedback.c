/**
 * @file    test_rate_feedback.c
 * @brief   Rate fixture module: reheating and a yield slaved to star formation
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * One of the four process_rates modules of the model-neutral rate fixture. Its
 * law returns two coefficients, in the order module_info.yaml declares the
 * transfers (README.md states the laws):
 * - reheating: RateFixtureCold to RateFixtureHot at TestRateFeedbackReheatRate
 *   (specific rate)
 * - yield: source into RateFixtureColdTracer, slaved to
 *   test_rate_formation.star_formation, at TestRateFeedbackYield (dimensionless:
 *   the core multiplies it by the master's flux)
 *
 * The rate mode is declared but not yet dispatched, so only unit tests call
 * test_rate_feedback_rates() today.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <math.h>

#include "error.h"
#include "module_interface.h"
#include "module_system/generated/rate_contracts.h"
#include "module_system/parameter_helpers.h"
#include "types.h"

/** Specific reheating rate from cold to hot, in inverse internal time units */
static double reheat_rate = 0.0;

/** Yield into the cold tracer per unit mass formed into stars, in [0, 1) */
static double yield_fraction = 0.0;

/**
 * @brief   Initialize the feedback fixture
 *
 * Reads the two required parameters and refuses a reheating rate that is
 * negative or not finite, and a yield outside [0, 1).
 *
 * @return  0 on success, -1 on a missing or invalid parameter
 */
int test_rate_feedback_init(void) {
  LOAD_PARAM_DOUBLE("TestRateFeedbackReheatRate", reheat_rate);
  LOAD_PARAM_DOUBLE("TestRateFeedbackYield", yield_fraction);

  if (!isfinite(reheat_rate) || reheat_rate < 0.0) {
    ERROR_LOG("TestRateFeedbackReheatRate must be finite and non-negative (got %g)", reheat_rate);
    return -1;
  }
  if (!(yield_fraction >= 0.0 && yield_fraction < 1.0)) {
    ERROR_LOG("TestRateFeedbackYield must lie in [0, 1) (got %g)", yield_fraction);
    return -1;
  }

  INFO_LOG("Test rate feedback fixture initialized (reheating %g, yield %g)", reheat_rate,
           yield_fraction);
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/**
 * @brief   Return the feedback fixture's coefficients for one row
 *
 * @param   ctx           Rate context (unused: the law depends on no state)
 * @param   halo          Scratch copy of the row at the trial state (unused)
 * @param   coefficients  One slot per declared transfer, zeroed by the caller
 * @return  0 on success
 */
int test_rate_feedback_rates(const struct RateContext *ctx, const struct Halo *halo,
                             double *coefficients) {
  (void)ctx;
  (void)halo;
  coefficients[TEST_RATE_FEEDBACK_TRANSFER_REHEATING] = reheat_rate;
  coefficients[TEST_RATE_FEEDBACK_TRANSFER_YIELD] = yield_fraction;
  return 0;
}

/**
 * @brief   Clean up the feedback fixture (it holds no resources)
 *
 * @return  0
 */
int test_rate_feedback_cleanup(void) { return 0; }
