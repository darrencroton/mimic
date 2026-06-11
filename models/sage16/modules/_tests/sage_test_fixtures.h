/**
 * @file    sage_test_fixtures.h
 * @brief   Shared fixture boilerplate for SAGE16 C unit tests
 *
 * Provides the test-statistics counters required by the TEST_RUN macro plus the
 * configuration/registration fixtures that were previously copy-pasted into every
 * SAGE16 module test file. Include it after the standard test includes:
 *
 *   #include "modules/_tests/sage_test_fixtures.h"
 *
 * (resolved via the -Imodels/sage16 flag set by tests/unit/run_tests.sh).
 *
 * Files that need a customised reset_config() (e.g. unit-system setup) define
 * SAGE_TEST_LOCAL_RESET_CONFIG before including this header and keep their local
 * version. Fixtures that differ between test files (e.g. per-module
 * create_test_halo() builders) stay local to those files.
 *
 * This header carries fixtures only: it must never weaken or absorb test
 * assertions.
 */

#ifndef SAGE_TEST_FIXTURES_H
#define SAGE_TEST_FIXTURES_H

#include <string.h>

#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/memory.h"

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Test fixture: reset configuration state */
#ifndef SAGE_TEST_LOCAL_RESET_CONFIG
static inline void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }
#endif

/* Test fixture: ensure modules are registered (only once) */
static inline void ensure_modules_registered(void) {
  static int modules_registered = 0;
  if (!modules_registered) {
    register_all_modules();
    modules_registered = 1;
  }
}

/* Test fixture: Set all required model parameters
 * Defined in tests/unit/test_stubs.c - provides all required parameters */
extern void set_test_model_parameters(void);

/* Test fixture: free the galaxy attached to a locally-built test halo */
static inline void free_test_halo(struct Halo *halo) {
  if (halo->galaxy != NULL) {
    myfree(halo->galaxy);
    halo->galaxy = NULL;
  }
}

#endif /* SAGE_TEST_FIXTURES_H */
