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

/* Test fixture: set all SHAM model parameters, with the knobs unit tests vary
 * exposed as arguments. Fixed values are the canonical Moster et al. (2013)
 * z=0 SMHM parameters from models/sham/input/sham_mini-millennium.yaml. */
static inline void set_sham_test_parameters(int use_scatter, double scatter_dex,
                                            double max_baryon_fraction, double orphan_max_age_myr) {
  int idx = 0;

/* Parameter names deliberately avoid struct member names (macro substitution) */
#define SHAM_TEST_PARAM(pname, pfmt, pval)                                                         \
  do {                                                                                             \
    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "%s", (pname));              \
    snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, pfmt, (pval));                    \
    idx++;                                                                                         \
  } while (0)

  SHAM_TEST_PARAM("ShamLogM1", "%.10g", 11.590);
  SHAM_TEST_PARAM("ShamN", "%.10g", 0.0351);
  SHAM_TEST_PARAM("ShamBeta", "%.10g", 1.376);
  SHAM_TEST_PARAM("ShamGamma", "%.10g", 0.608);
  SHAM_TEST_PARAM("ShamScatterDex", "%.10g", scatter_dex);
  SHAM_TEST_PARAM("ShamMinMpeak", "%.10g", 0.10);
  SHAM_TEST_PARAM("ShamMinVpeak", "%.10g", 80.0);
  SHAM_TEST_PARAM("ShamMaxStellarBaryonFraction", "%.10g", max_baryon_fraction);
  SHAM_TEST_PARAM("ShamOrphanMaxAgeMyr", "%.10g", orphan_max_age_myr);
  SHAM_TEST_PARAM("ShamUseScatter", "%d", use_scatter);

#undef SHAM_TEST_PARAM

  MimicConfig.NumModelParams = idx;
}

/* Test fixture: canonical defaults (scatter on, production cap, orphans kept) */
static inline void set_test_model_parameters(void) { set_sham_test_parameters(1, 0.20, 0.17, 0.0); }

#endif /* SHAM_TEST_FIXTURES_H */
