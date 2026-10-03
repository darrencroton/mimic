/**
 * @file    hod_test_fixtures.h
 * @brief   Shared fixture boilerplate for HOD C unit tests
 *
 * The HOD package's model-owned fixture header, mirroring
 * models/sham/modules/_tests/sham_test_fixtures.h: each model package brings
 * its own parameter fixture alongside its tests, so core test scaffolding
 * carries no model knowledge. Include it after the standard test includes
 * (resolved via the -Imodels/hod flag set by tests/unit/run_tests.sh):
 *
 *   #include "modules/_tests/hod_test_fixtures.h"
 *
 * This header carries fixtures only: it must never weaken or absorb test
 * assertions.
 */

#ifndef HOD_TEST_FIXTURES_H
#define HOD_TEST_FIXTURES_H

#include <stdio.h>
#include <string.h>

#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** Box side of every synthetic HOD run, Mpc/h (the micro-Uchuu fixture's) */
#define HOD_TEST_BOX_SIZE 100.0

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
static inline void hod_add_parameter(const char *name, const char *value) {
  const int i = MimicConfig.NumModelParams++;
  snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", name);
  snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
}

/** The ten HOD parameters in run-file order */
static const char *const hod_parameter_names[10] = {
    "HODLogMmin", "HODSigmaLogM", "HODLogM0",         "HODLogM1", "HODAlpha",
    "HODSeed",    "HODConcA",     "HODConcLogMpivot", "HODConcB", "HODConcC",
};

/**
 * Shipped defaults: Zheng, Coil & Zehavi (2007) SDSS Mr < -20 and Duffy et al.
 * (2008) full-sample NFW 200c, as in models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml.
 */
static const char *const hod_default_values[10] = {
    "12.02", "0.26", "11.38", "13.31", "1.06", "1", "5.71", "12.301030", "-0.084", "-0.47",
};

/**
 * Test fixture: reset the configuration and set every HOD parameter to its
 * default, except that parameter @p override_name (when non-NULL) takes
 * @p override_value, or is omitted when @p override_value is NULL. BoxSize is
 * HOD_TEST_BOX_SIZE and SubSteps 1.
 */
static inline void set_hod_test_parameters(const char *override_name, const char *override_value) {
  reset_config();
  MimicConfig.BoxSize = HOD_TEST_BOX_SIZE;
  MimicConfig.SubSteps = 1;
  for (int k = 0; k < 10; k++) {
    if (override_name != NULL && strcmp(override_name, hod_parameter_names[k]) == 0) {
      if (override_value != NULL) {
        hod_add_parameter(hod_parameter_names[k], override_value);
      }
      continue;
    }
    hod_add_parameter(hod_parameter_names[k], hod_default_values[k]);
  }
}

#endif /* HOD_TEST_FIXTURES_H */
