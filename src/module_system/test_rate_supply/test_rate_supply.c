/**
 * @file    test_rate_supply.c
 * @brief   Rate fixture module: infall onto central rows and cooling
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * One of the four process_rates modules of the model-neutral rate fixture. Its
 * law returns three coefficients, in the order module_info.yaml declares the
 * transfers (README.md states the laws):
 * - infall: source into RateFixtureHot, TestRateSupplyInfallRate * Mvir on a
 *   Type 0 row with positive Mvir, else 0 (absolute rate)
 * - infall_tracer: source into RateFixtureHotTracer, TestRateSupplyTracerFraction
 *   times the infall coefficient (absolute rate)
 * - cooling: RateFixtureHot to RateFixtureCold at TestRateSupplyCoolingRate
 *   (specific rate)
 *
 * The rate mode is declared but not yet dispatched, so only unit tests call
 * test_rate_supply_rates() today.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <math.h>

#include "error.h"
#include "module_interface.h"
#include "module_system/generated/rate_contracts.h"
#include "module_system/parameter_helpers.h"
#include "types.h"

/** Infall per unit virial mass on a central row, in inverse internal time units */
static double infall_rate = 0.0;

/** Tracer infall as a fraction of infall, in [0, 1) */
static double tracer_fraction = 0.0;

/** Specific cooling rate from hot to cold, in inverse internal time units */
static double cooling_rate = 0.0;

/**
 * @brief   Initialize the supply fixture
 *
 * Reads the three required parameters and refuses a rate that is negative or
 * not finite, and a tracer fraction outside [0, 1).
 *
 * @return  0 on success, -1 on a missing or invalid parameter
 */
int test_rate_supply_init(void) {
  LOAD_PARAM_DOUBLE("TestRateSupplyInfallRate", infall_rate);
  LOAD_PARAM_DOUBLE("TestRateSupplyTracerFraction", tracer_fraction);
  LOAD_PARAM_DOUBLE("TestRateSupplyCoolingRate", cooling_rate);

  if (!isfinite(infall_rate) || infall_rate < 0.0) {
    ERROR_LOG("TestRateSupplyInfallRate must be finite and non-negative (got %g)", infall_rate);
    return -1;
  }
  if (!(tracer_fraction >= 0.0 && tracer_fraction < 1.0)) {
    ERROR_LOG("TestRateSupplyTracerFraction must lie in [0, 1) (got %g)", tracer_fraction);
    return -1;
  }
  if (!isfinite(cooling_rate) || cooling_rate < 0.0) {
    ERROR_LOG("TestRateSupplyCoolingRate must be finite and non-negative (got %g)", cooling_rate);
    return -1;
  }

  INFO_LOG("Test rate supply fixture initialized (infall %g, tracer fraction %g, cooling %g)",
           infall_rate, tracer_fraction, cooling_rate);
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/**
 * @brief   Return the supply fixture's coefficients for one row
 *
 * @param   ctx           Rate context (unused: the law depends on the row alone)
 * @param   halo          Scratch copy of the row at the trial state
 * @param   coefficients  One slot per declared transfer, zeroed by the caller
 * @return  0 on success
 */
int test_rate_supply_rates(const struct RateContext *ctx, const struct Halo *halo,
                           double *coefficients) {
  (void)ctx;
  const double infall = (halo->Type == 0 && halo->Mvir > 0.0) ? infall_rate * halo->Mvir : 0.0;

  coefficients[TEST_RATE_SUPPLY_TRANSFER_INFALL] = infall;
  coefficients[TEST_RATE_SUPPLY_TRANSFER_INFALL_TRACER] = tracer_fraction * infall;
  coefficients[TEST_RATE_SUPPLY_TRANSFER_COOLING] = cooling_rate;
  return 0;
}

/**
 * @brief   Clean up the supply fixture (it holds no resources)
 *
 * @return  0
 */
int test_rate_supply_cleanup(void) { return 0; }
