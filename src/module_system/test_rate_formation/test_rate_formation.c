/**
 * @file    test_rate_formation.c
 * @brief   Rate fixture module: threshold star formation from the cold reservoir
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * One of the four process_rates modules of the model-neutral rate fixture. Its
 * law returns one coefficient (README.md states the law):
 * - star_formation: RateFixtureCold to RateFixtureStars at the specific rate
 *   ((Cold - TestRateFormationThreshold) / Cold) * TestRateFormationEfficiency
 *   when Cold exceeds the threshold, else 0
 *
 * The flux, coefficient times Cold, is then efficiency * max(0, Cold - threshold):
 * zero below the threshold and continuous across it. The module also declares the
 * observer RateFixtureFormed, which accumulates the amount star_formation moves.
 *
 * The rate mode is declared but not yet dispatched, so only unit tests call
 * test_rate_formation_rates() today.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <math.h>
#include <stddef.h>

#include "error.h"
#include "module_interface.h"
#include "module_system/generated/rate_contracts.h"
#include "module_system/parameter_helpers.h"
#include "types.h"

/** Star formation efficiency above the threshold, in inverse internal time units */
static double efficiency = 0.0;

/** Cold content at and below which no stars form, in RateFixtureCold's units */
static double threshold = 0.0;

/**
 * @brief   Initialize the formation fixture
 *
 * Reads the two required parameters and refuses either if it is negative or not
 * finite.
 *
 * @return  0 on success, -1 on a missing or invalid parameter
 */
int test_rate_formation_init(void) {
  LOAD_PARAM_DOUBLE("TestRateFormationEfficiency", efficiency);
  LOAD_PARAM_DOUBLE("TestRateFormationThreshold", threshold);

  if (!isfinite(efficiency) || efficiency < 0.0) {
    ERROR_LOG("TestRateFormationEfficiency must be finite and non-negative (got %g)", efficiency);
    return -1;
  }
  if (!isfinite(threshold) || threshold < 0.0) {
    ERROR_LOG("TestRateFormationThreshold must be finite and non-negative (got %g)", threshold);
    return -1;
  }

  INFO_LOG("Test rate formation fixture initialized (efficiency %g, threshold %g)", efficiency,
           threshold);
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/**
 * @brief   Return the formation fixture's coefficient for one row
 *
 * Cold above a non-negative threshold is positive, so the division is safe. The
 * ratio (Cold - threshold) / Cold lies in (0, 1] and is formed before the
 * efficiency multiplies it, so any finite efficiency gives a finite coefficient.
 *
 * @param   ctx           Rate context (unused: the law depends on the row alone)
 * @param   halo          Scratch copy of the row at the trial state
 * @param   coefficients  One slot per declared transfer, zeroed by the caller
 * @return  0 on success, -1 for a row without a galaxy
 */
int test_rate_formation_rates(const struct RateContext *ctx, const struct Halo *halo,
                              double *coefficients) {
  (void)ctx;
  if (halo->galaxy == NULL) {
    ERROR_LOG("test_rate_formation: rate law called for a row without a galaxy");
    return -1;
  }
  const double cold = halo->galaxy->RateFixtureCold;

  coefficients[TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION] =
      (cold > threshold) ? ((cold - threshold) / cold) * efficiency : 0.0;
  return 0;
}

/**
 * @brief   Clean up the formation fixture (it holds no resources)
 *
 * @return  0
 */
int test_rate_formation_cleanup(void) { return 0; }
