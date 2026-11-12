/**
 * @file    sage_test_fixtures.h
 * @brief   Shared fixture boilerplate for the sage26 package's C unit tests
 *
 * Provides the test-statistics counters required by the TEST_RUN macro plus the
 * configuration/registration fixtures every sage26 module test file would otherwise
 * repeat. Include it after the standard test includes:
 *
 *   #include "modules/_tests/sage_test_fixtures.h"
 *
 * (resolved via the -Imodels/sage26 flag set by tests/unit/run_tests.sh).
 *
 * Files that need a customised reset_config() (e.g. unit-system setup) define
 * SAGE_TEST_LOCAL_RESET_CONFIG before including this header and keep their local
 * version. Fixtures that differ between test files (e.g. per-module
 * create_test_halo() builders) stay local to those files. The helpers every file would
 * otherwise copy verbatim live here: log capture (read_captured_log), bit-pattern
 * conversions (double_bits, f32_from_bits), the post_timestep builders and the CGMrecipeOn
 * parameter setter.
 *
 * This header carries fixtures only: it must never weaken or absorb test
 * assertions.
 */

#ifndef SAGE_TEST_FIXTURES_H
#define SAGE_TEST_FIXTURES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/memory.h"

#include "framework/test_phase_config.h"

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

/* Test fixture: set the package's common required model parameters to standard
 * test values (SAGE's defaults). Model-owned: every model package provides its own
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
      {"AGNrecipe", "2"}, /* Bondi: the package accepts only 0 and 2 */
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
      /* Bulge sizes: off */
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
 * Test fixture: the parameters of the full-physics run file, at its values, for a test that
 * initialises the complete default pipeline. sage26_test_load_run_file_parameters() writes them to
 * MimicConfig.ModelParams, replacing the value of each key listed in `overrides` (name, value
 * pairs; NULL and 0 for none).
 */
static const struct {
  const char *name;
  const char *value;
} SAGE26_TEST_RUN_FILE_PARAMETERS[] = {
    {"GlobalBaryonFraction", "0.17"},
    {"CGMrecipeOn", "1"},
    {"MShockMsun", "6.0e11"},
    {"EnhancedStarFormationOn", "1"},
    {"EnhancedSFEfficiency", "0.2"},
    {"ConcentrationOn", "3"},
    {"SfrEfficiency", "0.05"},
    {"StarFormingDiskFactor", "3.0"},
    {"SFprescription", "1"},
    {"H2RadialIntegrationOn", "1"},
    {"H2RadialNBins", "25"},
    {"H2RadialRMaxFactor", "5.0"},
    {"StarburstColdGasOn", "1"},
    {"FeedbackReheatingEpsilon", "2.9"},
    {"FeedbackEjectionEfficiency", "0.3"},
    {"FIREmodeOn", "1"},
    {"ReIncorporationFactor", "0.19"},
    {"AGNrecipe", "2"},
    {"RadioModeEfficiency", "0.08"},
    {"BlackHoleGrowthRate", "0.015"},
    {"QuasarModeEfficiency", "0.005"},
    {"RecycleFraction", "0.43"},
    {"Yield", "0.025"},
    {"FracZleaveDisk", "0.0"},
    {"ThresholdMajorMerger", "0.3"},
    {"ThresholdSatDisruption", "1.0"},
    {"MergerTimeFactor", "2.0"},
    {"BulgeSizeOn", "3"},
};

static inline void sage26_test_load_run_file_parameters(const char *const (*overrides)[2],
                                                        const size_t n_overrides) {
  const size_t n =
      sizeof(SAGE26_TEST_RUN_FILE_PARAMETERS) / sizeof(SAGE26_TEST_RUN_FILE_PARAMETERS[0]);
  for (size_t i = 0; i < n; i++) {
    const char *value = SAGE26_TEST_RUN_FILE_PARAMETERS[i].value;
    for (size_t k = 0; k < n_overrides; k++) {
      if (strcmp(overrides[k][0], SAGE26_TEST_RUN_FILE_PARAMETERS[i].name) == 0) {
        value = overrides[k][1];
      }
    }
    snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s",
             SAGE26_TEST_RUN_FILE_PARAMETERS[i].name);
    snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
  }
  MimicConfig.NumModelParams = (int)n;
}

/*
 * Test fixture: the required parameters SAGE26's SF prescription selects, for a test that
 * initialises a module reading them with its own parameters rather than the run files'. Each
 * test adds exactly the keys its modules read (a union of the flags below):
 * sage26_calculate_star_formation reads SFprescription and the three H2 integration
 * parameters, sage26_apply_star_formation_supernova and sage26_quasar_mode SFprescription,
 * and sage26_starburst_feedback SFprescription, StarburstColdGasOn and FIREmodeOn (0, fixed-epsilon
 * feedback: a required parameter). A key already present is not added twice.
 */
#define SAGE26_TEST_KEY_SF_PRESCRIPTION 1 /* SFprescription: 0 (the Croton law) */
#define SAGE26_TEST_KEYS_H2_RADIAL                                                                 \
  2                                          /* H2RadialIntegrationOn 1, H2RadialNBins 25,         \
                                                H2RadialRMaxFactor 5.0 (SAGE26's defaults) */
#define SAGE26_TEST_KEY_STARBURST_COLD_GAS 4 /* StarburstColdGasOn: 1 (cold-gas bursts) */
#define SAGE26_TEST_KEY_FIRE_MODE 8          /* FIREmodeOn: 0 (fixed-epsilon feedback) */
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
 * Test fixture: the required CGMrecipeOn at value 0, for a test that initialises
 * sage26_prepare_infall_budget, sage26_apply_infall, sage26_satellite_stripping,
 * sage26_reincorporation, sage26_apply_star_formation_supernova, sage26_apply_metal_enrichment,
 * sage26_starburst_feedback, sage26_quasar_mode, sage26_resolve_mergers_and_disruption or
 * sage26_calculate_cooling_budget with its own parameters rather than the run files'. A key
 * already present is not added twice.
 */
static inline void add_test_cgm_recipe_parameter(void) {
  sage26_test_append_model_parameter("CGMrecipeOn", "0");
}

/*
 * Test fixture: set the run-file parameter CGMrecipeOn to `value`, or remove it when `value`
 * is NULL (the key is required, so a missing one must be refused at init).
 */
static inline void set_test_cgm_recipe_on(const char *value) {
  int kept = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "CGMrecipeOn") != 0) {
      MimicConfig.ModelParams[kept++] = MimicConfig.ModelParams[i];
    }
  }
  if (value != NULL) {
    snprintf(MimicConfig.ModelParams[kept].param_name, MAX_STRING_LEN, "%s", "CGMrecipeOn");
    snprintf(MimicConfig.ModelParams[kept].value, MAX_STRING_LEN, "%s", value);
    kept++;
  }
  MimicConfig.NumModelParams = kept;
}

/**
 * @brief Read everything written to a log-capture FILE* into a static buffer
 *
 * The buffer is valid until the next call. Pair with tmpfile() and set_log_output() to capture
 * what an init() or process() call logs.
 */
static inline const char *read_captured_log(FILE *fp) {
  static char buf[16384];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/** @brief The IEEE-754 bit pattern of a double, for exact comparisons */
static inline uint64_t double_bits(const double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/** @brief A float from its IEEE-754 bit pattern */
static inline float f32_from_bits(const uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief Append one entry to MimicConfig.post_timestep (freed by module_system_cleanup) */
static inline void test_post_timestep_add(const char *module_name, enum ProcessingMode mode) {
  if (MimicConfig.post_timestep == NULL) {
    MimicConfig.post_timestep =
        mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.num_post_timestep = 0;
  }
  const int i = MimicConfig.num_post_timestep++;
  MimicConfig.post_timestep[i].module_name = strdup(module_name);
  MimicConfig.post_timestep[i].processing_mode = mode;
  MimicConfig.post_timestep[i].resolved = NULL;
}

/** @brief Free the post_timestep array of a pipeline whose init() never reached cleanup */
static inline void test_free_post_timestep(void) {
  if (MimicConfig.post_timestep == NULL) {
    return;
  }
  for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
    free((void *)MimicConfig.post_timestep[i].module_name);
  }
  myfree(MimicConfig.post_timestep);
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
}

/* Test fixture: free the galaxy attached to a locally-built test halo */
static inline void free_test_halo(struct Halo *halo) {
  if (halo->galaxy != NULL) {
    myfree(halo->galaxy);
    halo->galaxy = NULL;
  }
}

#endif /* SAGE_TEST_FIXTURES_H */
