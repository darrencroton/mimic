/**
 * @file    test_rate_jump.c
 * @brief   Rate fixture module: merges satellites into the FoF central between rate segments
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * The discrete jump of the model-neutral rate fixture. On every call it merges
 * each orphan satellite (a Type 2 row with a galaxy) whose RateFixtureStars
 * exceeds TestRateJumpStarsThreshold into the FoF central: every reservoir and
 * tracer of the satellite is added to the central's, scaled by
 * 1 - TestRateJumpLossFraction, then zeroed, and the row is retired (Type 3).
 * With no loss the conserved totals over the active rows are unchanged; with a
 * loss each merge removes that fraction of both totals, which the module
 * declares in changes_totals (README.md states the behaviour). A Type 1
 * satellite is never merged: it hosts its subhalo's orphans, and the next
 * snapshot's inheritance needs every subhalo slice to keep a Type 0 or 1 row.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <inttypes.h>
#include <math.h>
#include <stdint.h>

#include "error.h"
#include "module_interface.h"
#include "module_system/parameter_helpers.h"
#include "types.h"

/** A satellite merges when its RateFixtureStars exceeds this, in internal mass units */
static double stars_threshold = 0.0;

/** Fraction of every moved reservoir and tracer the merge destroys, in [0, 1) */
static double loss_fraction = 0.0;

/** Satellites merged since init(), reported at cleanup */
static int64_t merges = 0;

/**
 * @brief   Initialize the jump fixture
 *
 * Reads the two required parameters; refuses a threshold that is negative or not
 * finite and a loss fraction outside [0, 1).
 *
 * @return  0 on success, -1 on a missing or invalid parameter
 */
int test_rate_jump_init(void) {
  LOAD_PARAM_DOUBLE("TestRateJumpStarsThreshold", stars_threshold);
  LOAD_PARAM_DOUBLE("TestRateJumpLossFraction", loss_fraction);

  if (!isfinite(stars_threshold) || stars_threshold < 0.0) {
    ERROR_LOG("TestRateJumpStarsThreshold must be finite and non-negative (got %g)",
              stars_threshold);
    return -1;
  }
  if (!(loss_fraction >= 0.0 && loss_fraction < 1.0)) {
    ERROR_LOG("TestRateJumpLossFraction must lie in [0, 1) (got %g)", loss_fraction);
    return -1;
  }
  merges = 0;

  INFO_LOG("Test rate jump fixture initialized (stars threshold %g, loss fraction %g)",
           stars_threshold, loss_fraction);
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/** @brief Move one reservoir or tracer into the central, keeping 1 - loss of it */
static void move_content(double *central, double *satellite) {
  *central += (1.0 - loss_fraction) * *satellite;
  *satellite = 0.0;
}

/**
 * @brief   Merge every orphan satellite whose stars exceed the threshold into the FoF central
 *
 * @param   ctx    Module context: the FoF central is row ctx->central_index
 * @param   halos  The FoF workspace's rows
 * @param   ngal   Number of rows
 * @return  0 (a group without an active central merges nothing)
 */
int test_rate_jump_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  const int c = ctx->central_index;
  if (halos == NULL || c < 0 || c >= ngal || halos[c].galaxy == NULL || halos[c].Type == 3) {
    return 0;
  }
  struct GalaxyData *central = halos[c].galaxy;
  for (int i = 0; i < ngal; i++) {
    struct Halo *row = &halos[i];
    if (i == c || row->galaxy == NULL || row->Type != 2) {
      continue;
    }
    struct GalaxyData *satellite = row->galaxy;
    if (!(satellite->RateFixtureStars > stars_threshold)) {
      continue;
    }
    move_content(&central->RateFixtureHot, &satellite->RateFixtureHot);
    move_content(&central->RateFixtureCold, &satellite->RateFixtureCold);
    move_content(&central->RateFixtureStars, &satellite->RateFixtureStars);
    move_content(&central->RateFixtureHotTracer, &satellite->RateFixtureHotTracer);
    move_content(&central->RateFixtureColdTracer, &satellite->RateFixtureColdTracer);
    move_content(&central->RateFixtureStarsTracer, &satellite->RateFixtureStarsTracer);
    row->Type = 3;
    merges++;
  }
  return 0;
}

/**
 * @brief   Clean up the jump fixture, reporting its merge count
 *
 * @return  0
 */
int test_rate_jump_cleanup(void) {
  INFO_LOG("Test rate jump fixture: %" PRId64 " satellite(s) merged", merges);
  return 0;
}
