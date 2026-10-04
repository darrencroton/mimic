/**
 * @file    sham_test_fixtures.h
 * @brief   Shared fixture boilerplate for SHAM C unit tests
 *
 * SHAM's model-owned fixture header, mirroring
 * models/sage16/modules/_tests/sage_test_fixtures.h: each model package brings
 * its own parameter fixture alongside its tests, so core test scaffolding
 * carries no model knowledge. Include it after the standard test includes
 * (resolved via the -Imodels/sham flag set by tests/unit/run_tests.sh):
 *
 *   #include "modules/_tests/sham_test_fixtures.h"
 *
 * This header carries fixtures only: it must never weaken or absorb test
 * assertions.
 */

#ifndef SHAM_TEST_FIXTURES_H
#define SHAM_TEST_FIXTURES_H

#include <stdio.h>
#include <string.h>

#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** Box side of every synthetic SHAM run, Mpc/h (the micro-Uchuu fixture's) */
#define SHAM_TEST_BOX_SIZE 100.0

/** Simulation Hubble parameter of every synthetic SHAM run (the micro-Uchuu fixture's) */
#define SHAM_TEST_HUBBLE 0.6774

/** Snapshots 0 to SHAM_TEST_NUM_SNAPSHOTS - 1 are the output snapshots of every synthetic run */
#define SHAM_TEST_NUM_SNAPSHOTS 100

/** Number of sham_rank_match parameters */
#define SHAM_NUM_PARAMETERS 9

/* Test fixture: reset configuration state */
static inline void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }

/* Test fixture: ensure modules are registered (only once) */
static inline void ensure_modules_registered(void) {
  static int modules_registered = 0;
  if (!modules_registered) {
    register_all_modules();
    modules_registered = 1;
  }
}

/** Append one module parameter as a string, exactly as the run-file parser stores it */
static inline void sham_add_parameter(const char *name, const char *value) {
  const int i = MimicConfig.NumModelParams++;
  snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", name);
  snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
}

/** The nine sham_rank_match parameters in run-file order */
static const char *const sham_parameter_names[SHAM_NUM_PARAMETERS] = {
    "ShamTargetLogMstar",     "ShamTargetPhi1",        "ShamTargetAlpha1",
    "ShamTargetPhi2",         "ShamTargetAlpha2",      "ShamTargetHubble",
    "ShamTargetLogMassFloor", "ShamTargetRedshiftMax", "ShamMinVpeak",
};

/**
 * Shipped values: the Baldry et al. (2012) GAMA double Schechter fit at h = 0.7
 * with the fixture's window and completeness floor, as in
 * models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml.
 */
static const char *const sham_default_values[SHAM_NUM_PARAMETERS] = {
    "10.66", "3.96e-3", "-0.35", "0.79e-3", "-1.47", "0.7", "8.0", "0.2", "80",
};

/**
 * Test fixture: reset the configuration and set every SHAM parameter to its
 * shipped value, except that parameter @p override_name (when non-NULL) takes
 * @p override_value, or is omitted when @p override_value is NULL. BoxSize is
 * SHAM_TEST_BOX_SIZE, Hubble_h SHAM_TEST_HUBBLE and SubSteps 1. Every snapshot
 * below SHAM_TEST_NUM_SNAPSHOTS is an output snapshot (a test narrows the list
 * itself) and all redshifts are 0, so the redshift window has nothing to reject.
 */
static inline void set_sham_test_parameters(const char *override_name, const char *override_value) {
  reset_config();
  MimicConfig.BoxSize = SHAM_TEST_BOX_SIZE;
  MimicConfig.Hubble_h = SHAM_TEST_HUBBLE;
  MimicConfig.SubSteps = 1;
  MimicConfig.NOUT = SHAM_TEST_NUM_SNAPSHOTS;
  for (int n = 0; n < SHAM_TEST_NUM_SNAPSHOTS; n++) {
    MimicConfig.ListOutputSnaps[n] = n;
  }
  for (int k = 0; k < SHAM_NUM_PARAMETERS; k++) {
    if (override_name != NULL && strcmp(override_name, sham_parameter_names[k]) == 0) {
      if (override_value != NULL) {
        sham_add_parameter(sham_parameter_names[k], override_value);
      }
      continue;
    }
    sham_add_parameter(sham_parameter_names[k], sham_default_values[k]);
  }
}

#endif /* SHAM_TEST_FIXTURES_H */
