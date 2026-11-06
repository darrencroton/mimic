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
 * (resolved via the -Imodels/sage26 flag set by tests/unit/run_tests.sh).
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

/* Test fixture: set all required SAGE16 model parameters to standard test
 * values (SAGE defaults). Model-owned: every model package provides its own
 * parameter fixture; the core test stubs carry no model knowledge. */
static inline void set_test_model_parameters(void) {
  static const struct {
    const char *name;
    const char *value;
  } params[] = {
      /* Cosmological */
      {"GlobalBaryonFraction", "0.17"},
      /* Cooling & AGN feedback */
      {"RadioModeEfficiency", "0.01"},
      {"AGNrecipe", "1"},
      /* Star formation */
      {"SfrEfficiency", "0.02"},
      /* Stellar feedback */
      {"FeedbackReheatingEpsilon", "3.0"},
      {"FeedbackEjectionEfficiency", "0.3"},
      /* Stellar evolution */
      {"RecycleFraction", "0.43"},
      {"Yield", "0.03"},
      {"FracZleaveDisk", "0.3"},
      /* Reincorporation */
      {"ReIncorporationFactor", "1.0"},
      /* Mergers */
      {"BlackHoleGrowthRate", "0.01"},
      {"QuasarModeEfficiency", "0.001"},
      {"ThresholdMajorMerger", "0.3"},
      {"ThresholdSatDisruption", "1.0"},
      /* Disk instability */
      {"StarFormingDiskFactor", "3.0"},
      /* Bulge sizes: SAGE26's R0 mode (no sizes) */
      {"BulgeSizeOn", "0"},
  };
  const int n = (int)(sizeof(params) / sizeof(params[0]));
  for (int i = 0; i < n; i++) {
    strncpy(MimicConfig.ModelParams[i].param_name, params[i].name, MAX_STRING_LEN - 1);
    strncpy(MimicConfig.ModelParams[i].value, params[i].value, MAX_STRING_LEN - 1);
  }
  MimicConfig.NumModelParams = n;
}

/*
 * Test fixture: the required parameters SAGE26's SF prescription selects, at R1's values, for a
 * test that initialises a module reading them with its own parameters rather than the run files'.
 * Each test adds exactly the keys its modules read (a union of the flags below):
 * sage26_calculate_star_formation reads SFprescription and the three H2 integration parameters,
 * sage26_apply_star_formation_supernova and sage26_quasar_mode SFprescription, and
 * sage26_starburst_feedback SFprescription, StarburstColdGasOn and FIREmodeOn (0, R2's
 * fixed-epsilon feedback: the required parameter of campaign decision D33). A key already present
 * is not added twice.
 */
#define SAGE26_TEST_KEY_SF_PRESCRIPTION 1 /* SFprescription: 0 (R1, the Croton law) */
#define SAGE26_TEST_KEYS_H2_RADIAL                                                                 \
  2                                          /* H2RadialIntegrationOn 1, H2RadialNBins 25,         \
                                                H2RadialRMaxFactor 5.0 (SAGE26's defaults) */
#define SAGE26_TEST_KEY_STARBURST_COLD_GAS 4 /* StarburstColdGasOn: 1 (cold-gas bursts) */
#define SAGE26_TEST_KEY_FIRE_MODE 8          /* FIREmodeOn: 0 (fixed-epsilon feedback, D33) */
#define SAGE26_TEST_KEYS_SF_MODULE (SAGE26_TEST_KEY_SF_PRESCRIPTION | SAGE26_TEST_KEYS_H2_RADIAL)
#define SAGE26_TEST_KEYS_APPLY_MODULE SAGE26_TEST_KEY_SF_PRESCRIPTION
#define SAGE26_TEST_KEYS_QUASAR_MODULE SAGE26_TEST_KEY_SF_PRESCRIPTION
#define SAGE26_TEST_KEYS_STARBURST_MODULE                                                          \
  (SAGE26_TEST_KEY_SF_PRESCRIPTION | SAGE26_TEST_KEY_STARBURST_COLD_GAS | SAGE26_TEST_KEY_FIRE_MODE)

static inline void sage26_test_append_model_parameter(const char *name, const char *value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, name) == 0) {
      return;
    }
  }
  const int idx = MimicConfig.NumModelParams;
  strncpy(MimicConfig.ModelParams[idx].param_name, name, MAX_STRING_LEN - 1);
  strncpy(MimicConfig.ModelParams[idx].value, value, MAX_STRING_LEN - 1);
  MimicConfig.NumModelParams = idx + 1;
}

static inline void add_test_sf_prescription_parameters(const int keys) {
  if (keys & SAGE26_TEST_KEY_SF_PRESCRIPTION) {
    sage26_test_append_model_parameter("SFprescription", "0");
  }
  if (keys & SAGE26_TEST_KEYS_H2_RADIAL) {
    sage26_test_append_model_parameter("H2RadialIntegrationOn", "1");
    sage26_test_append_model_parameter("H2RadialNBins", "25");
    sage26_test_append_model_parameter("H2RadialRMaxFactor", "5.0");
  }
  if (keys & SAGE26_TEST_KEY_STARBURST_COLD_GAS) {
    sage26_test_append_model_parameter("StarburstColdGasOn", "1");
  }
  if (keys & SAGE26_TEST_KEY_FIRE_MODE) {
    sage26_test_append_model_parameter("FIREmodeOn", "0");
  }
}

/*
 * Test fixture: the required CGMrecipeOn (campaign decision D33) at R3's value 0, for a test that
 * initialises sage26_prepare_infall_budget, sage26_apply_infall, sage26_satellite_stripping,
 * sage26_reincorporation, sage26_apply_star_formation_supernova, sage26_apply_metal_enrichment,
 * sage26_starburst_feedback, sage26_quasar_mode or sage26_resolve_mergers_and_disruption with its
 * own parameters rather than the run files'. A key already present is not added twice.
 */
static inline void add_test_cgm_recipe_parameter(void) {
  sage26_test_append_model_parameter("CGMrecipeOn", "0");
}

/* Test fixture: free the galaxy attached to a locally-built test halo */
static inline void free_test_halo(struct Halo *halo) {
  if (halo->galaxy != NULL) {
    myfree(halo->galaxy);
    halo->galaxy = NULL;
  }
}

#endif /* SAGE_TEST_FIXTURES_H */
