/**
 * @file    test_rate_stripping.c
 * @brief   Rate fixture module: stripping onto the FoF central and ejection
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * One of the four process_rates modules of the model-neutral rate fixture. Its
 * law returns two coefficients, in the order module_info.yaml declares the
 * transfers (README.md states the laws):
 * - stripping: RateFixtureHot to the FoF central's RateFixtureHot
 *   (to_record: central) at TestRateStrippingRate on a row whose Type is not 0,
 *   and 0 on the central itself (specific rate)
 * - ejection: sink out of RateFixtureCold at TestRateStrippingEjectionRate
 *   (specific rate)
 *
 * Test builds dispatch the rate mode: the core integrator calls test_rate_stripping_rates()
 * in the rate phase, and unit tests also call it directly.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <math.h>

#include "error.h"
#include "module_interface.h"
#include "module_system/generated/rate_contracts.h"
#include "module_system/parameter_helpers.h"
#include "types.h"

/** Specific stripping rate of a satellite's hot reservoir, in inverse internal time units */
static double stripping_rate = 0.0;

/** Specific ejection rate out of the cold reservoir, in inverse internal time units */
static double ejection_rate = 0.0;

/**
 * @brief   Initialize the stripping fixture
 *
 * Reads the two required parameters and refuses either if it is negative or not
 * finite.
 *
 * @return  0 on success, -1 on a missing or invalid parameter
 */
int test_rate_stripping_init(void) {
  LOAD_PARAM_DOUBLE("TestRateStrippingRate", stripping_rate);
  LOAD_PARAM_DOUBLE("TestRateStrippingEjectionRate", ejection_rate);

  if (!isfinite(stripping_rate) || stripping_rate < 0.0) {
    ERROR_LOG("TestRateStrippingRate must be finite and non-negative (got %g)", stripping_rate);
    return -1;
  }
  if (!isfinite(ejection_rate) || ejection_rate < 0.0) {
    ERROR_LOG("TestRateStrippingEjectionRate must be finite and non-negative (got %g)",
              ejection_rate);
    return -1;
  }

  INFO_LOG("Test rate stripping fixture initialized (stripping %g, ejection %g)", stripping_rate,
           ejection_rate);
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/**
 * @brief   Return the stripping fixture's coefficients for one row
 *
 * @param   ctx           Rate context (unused: the law depends on the row alone)
 * @param   halo          Scratch copy of the row at the trial state
 * @param   coefficients  One slot per declared transfer, zeroed by the caller
 * @return  0 on success
 */
int test_rate_stripping_rates(const struct RateContext *ctx, const struct Halo *halo,
                              double *coefficients) {
  (void)ctx;
  coefficients[TEST_RATE_STRIPPING_TRANSFER_STRIPPING] = (halo->Type == 0) ? 0.0 : stripping_rate;
  coefficients[TEST_RATE_STRIPPING_TRANSFER_EJECTION] = ejection_rate;
  return 0;
}

/**
 * @brief   Clean up the stripping fixture (it holds no resources)
 *
 * @return  0
 */
int test_rate_stripping_cleanup(void) { return 0; }
