/**
 * @file    core_test_fixtures.h
 * @brief   Shared fixture boilerplate for core (model-neutral) C unit tests
 *
 * Core-test sibling of the model-owned fixture headers (e.g.
 * models/sage16/modules/_tests/sage_test_fixtures.h). Include it after the
 * standard test includes; paths resolve via the -I flags set by
 * tests/unit/run_tests.sh.
 *
 * This header carries fixtures only: it must never weaken or absorb test
 * assertions. Test-statistics counters stay file-owned.
 */

#ifndef CORE_TEST_FIXTURES_H
#define CORE_TEST_FIXTURES_H

#include <stdio.h>
#include <string.h>

#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"

/* Test fixture: reset global configuration state between tests */
static inline void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }

/* Test fixture: register all compiled modules exactly once */
static inline void ensure_modules_registered(void) {
  static int modules_registered = 0;
  if (!modules_registered) {
    register_all_modules();
    modules_registered = 1;
  }
}

/* Test fixture: generated core run file for the compiled MODEL/SIMULATION pair */
static inline const char *test_binary_param_file(void) {
  static char path[MAX_STRING_LEN];
  snprintf(path, sizeof(path), "build/generated/test_inputs/%s/%s/core/test_binary.yaml",
           MIMIC_COMPILED_MODEL, MIMIC_COMPILED_SIMULATION);
  return path;
}

#endif /* CORE_TEST_FIXTURES_H */
