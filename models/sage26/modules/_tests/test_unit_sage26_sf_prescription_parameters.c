/**
 * @file    test_unit_sage26_sf_prescription_parameters.c
 * @brief   SFprescription, the H2 integration parameters and StarburstColdGasOn: required package
 *          parameters with exactly their supported values
 *
 * Each module whose code reaches a branch or cap a parameter selects loads it at initialisation
 * through the required-parameter path, so a missing key fails initialisation and an unsupported
 * value is refused with an error naming the parameter; there is no fallback default:
 *
 *   sage26_calculate_star_formation        SFprescription, H2RadialIntegrationOn, H2RadialNBins,
 *                                          H2RadialRMaxFactor (the BR06 branch, sfr_br06)
 *   sage26_apply_star_formation_supernova  SFprescription (the commits' hydrogen caps)
 *   sage26_quasar_mode                     SFprescription (the black-hole accretion's hydrogen
 *                                          cap, R/src/model_mergers.c:366-372)
 *   sage26_starburst_feedback              SFprescription (the burst's commits' caps, its
 *                                          hydrogen refresh and its recheck's black-hole cap)
 *                                          and StarburstColdGasOn (the burst fuel)
 *
 * Supported: SFprescription 0 (Croton) and 1 (BR06 with radial integration);
 * H2RadialIntegrationOn 1, H2RadialNBins 25 and H2RadialRMaxFactor 5.0 (SAGE26's defaults,
 * R/src/core_read_parameter_file.c:99-101; the single-slab branch is not ported);
 * StarburstColdGasOn 1 (:109; the H2 burst fuel of 0 is not ported).
 *
 * Test cases:
 *   - test_missing_parameter_refused: each module's init fails without each key it reads
 *   - test_unsupported_values_refused: every unsupported value is refused, naming the parameter
 *   - test_supported_values_accepted: SFprescription 0 and 1 initialise every module
 *   - test_tracks_h2: the caps' guard is SAGE26's sf_prescription_tracks_h2
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_h2_physics.h"

extern int sage26_calculate_star_formation_init(void);
extern int sage26_calculate_star_formation_cleanup(void);
extern int sage26_apply_star_formation_supernova_init(void);
extern int sage26_apply_star_formation_supernova_cleanup(void);
extern int sage26_quasar_mode_init(void);
extern int sage26_quasar_mode_cleanup(void);
extern int sage26_starburst_feedback_init(void);
extern int sage26_starburst_feedback_cleanup(void);

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief One module that reads the parameters, and the keys it reads */
struct sf_module {
  const char *name;
  int (*init)(void);
  int (*cleanup)(void);
  int keys; /* the SAGE26_TEST_KEY* flags of sage_test_fixtures.h */
};

static const struct sf_module MODULES[] = {
    {"sage26_calculate_star_formation", sage26_calculate_star_formation_init,
     sage26_calculate_star_formation_cleanup, SAGE26_TEST_KEYS_SF_MODULE},
    {"sage26_apply_star_formation_supernova", sage26_apply_star_formation_supernova_init,
     sage26_apply_star_formation_supernova_cleanup, SAGE26_TEST_KEYS_APPLY_MODULE},
    {"sage26_quasar_mode", sage26_quasar_mode_init, sage26_quasar_mode_cleanup,
     SAGE26_TEST_KEYS_QUASAR_MODULE},
    {"sage26_starburst_feedback", sage26_starburst_feedback_init, sage26_starburst_feedback_cleanup,
     SAGE26_TEST_KEYS_STARBURST_MODULE},
};

/** @brief Each key, the flag that adds it and the unsupported values the modules must refuse */
static const struct {
  const char *key;
  int flag;
  const char *unsupported[5];
} KEYS[] = {
    {"SFprescription", SAGE26_TEST_KEY_SF_PRESCRIPTION, {"2", "3", "7", "-1", NULL}},
    {"H2RadialIntegrationOn", SAGE26_TEST_KEYS_H2_RADIAL, {"0", "2", NULL}},
    {"H2RadialNBins", SAGE26_TEST_KEYS_H2_RADIAL, {"24", "50", "0", NULL}},
    {"H2RadialRMaxFactor", SAGE26_TEST_KEYS_H2_RADIAL, {"4.0", "5.5", "0.0", NULL}},
    {"StarburstColdGasOn", SAGE26_TEST_KEY_STARBURST_COLD_GAS, {"0", "2", NULL}},
};

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/**
 * @brief The package's test parameters with the module's keys at R1's values, then `key` removed
 *        (value NULL) or set to `value`
 *
 * Also configures the dependencies each module's init checks (the SF chain in one substep phase,
 * as the run files have it) and SAGE26's unit system.
 */
static void setup(const int keys, const char *key, const char *value) {
  reset_config();
  set_test_model_parameters();
  add_test_sf_prescription_parameters(keys);
  if (key != NULL) {
    int n = 0;
    for (int i = 0; i < MimicConfig.NumModelParams; i++) {
      if (strcmp(MimicConfig.ModelParams[i].param_name, key) != 0) {
        MimicConfig.ModelParams[n++] = MimicConfig.ModelParams[i];
      }
    }
    MimicConfig.NumModelParams = n;
    if (value != NULL) {
      snprintf(MimicConfig.ModelParams[n].param_name, MAX_STRING_LEN, "%s", key);
      snprintf(MimicConfig.ModelParams[n].value, MAX_STRING_LEN, "%s", value);
      MimicConfig.NumModelParams = n + 1;
    }
  }
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitLength_in_cm = 3.08568e24;
  MimicConfig.UnitMass_in_g = 1.989e43;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  test_phase_add("galaxy_physics", "sage26_calculate_star_formation", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_calculate_supernova_feedback",
                 PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_metal_enrichment", PROCESSING_MODE_BY_GALAXY);
}

static void teardown(void) { test_free_substep_phases(); }

/** @brief Initialise one module, capturing its log; returns the init status */
static int init_captured(const struct sf_module *m, const char **log) {
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = m->init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/**
 * @test    test_missing_parameter_refused
 * @brief   Each module's init fails without each key it reads, naming the key (no default)
 */
int test_missing_parameter_refused(void) {
  for (size_t m = 0; m < COUNT(MODULES); m++) {
    for (size_t k = 0; k < COUNT(KEYS); k++) {
      if (!(MODULES[m].keys & KEYS[k].flag)) {
        continue;
      }
      setup(MODULES[m].keys, KEYS[k].key, NULL);
      const char *log = "";
      const int status = init_captured(&MODULES[m], &log);
      printf("\n  %s without %s: init %d", MODULES[m].name, KEYS[k].key, status);
      TEST_ASSERT(status != 0, "init must fail when a required key is missing");
      TEST_ASSERT(strstr(log, KEYS[k].key) != NULL, "the error must name the missing key");
      MODULES[m].cleanup();
      teardown();
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_unsupported_values_refused
 * @brief   Every unsupported value of each key a module reads is refused, naming the key
 */
int test_unsupported_values_refused(void) {
  for (size_t m = 0; m < COUNT(MODULES); m++) {
    for (size_t k = 0; k < COUNT(KEYS); k++) {
      if (!(MODULES[m].keys & KEYS[k].flag)) {
        continue;
      }
      for (size_t v = 0; KEYS[k].unsupported[v] != NULL; v++) {
        setup(MODULES[m].keys, KEYS[k].key, KEYS[k].unsupported[v]);
        const char *log = "";
        const int status = init_captured(&MODULES[m], &log);
        printf("\n  %s with %s %s: init %d", MODULES[m].name, KEYS[k].key, KEYS[k].unsupported[v],
               status);
        TEST_ASSERT(status != 0, "init must refuse an unsupported value");
        TEST_ASSERT(strstr(log, KEYS[k].key) != NULL, "the error must name the parameter");
        MODULES[m].cleanup();
        teardown();
      }
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_supported_values_accepted
 * @brief   SFprescription 0 (R1) and 1 (R2), with the supported H2 parameters and
 *          StarburstColdGasOn 1, initialise every module
 */
int test_supported_values_accepted(void) {
  const char *const prescriptions[] = {"0", "1"};
  for (size_t m = 0; m < COUNT(MODULES); m++) {
    for (size_t v = 0; v < COUNT(prescriptions); v++) {
      setup(MODULES[m].keys, "SFprescription", prescriptions[v]);
      const char *log = "";
      const int status = init_captured(&MODULES[m], &log);
      printf("\n  %s with SFprescription %s: init %d", MODULES[m].name, prescriptions[v], status);
      TEST_ASSERT(status == 0, "init must accept SFprescription 0 and 1");
      MODULES[m].cleanup();
      teardown();
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_tracks_h2
 * @brief   The caps' guard is SAGE26's sf_prescription_tracks_h2 (R/src/model_misc.h:93-96):
 *          false for 0 and 2, true for every other prescription
 */
int test_tracks_h2(void) {
  TEST_ASSERT(!mimic_sage_sf_prescription_tracks_h2(0), "prescription 0 does not track H2");
  TEST_ASSERT(mimic_sage_sf_prescription_tracks_h2(1), "prescription 1 tracks H2");
  TEST_ASSERT(!mimic_sage_sf_prescription_tracks_h2(2), "prescription 2 does not track H2");
  for (int p = 3; p <= 7; p++) {
    TEST_ASSERT(mimic_sage_sf_prescription_tracks_h2(p), "prescriptions 3-7 track H2");
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE26 SF-prescription parameters (required, supported values only)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);
  ensure_modules_registered();

  TEST_RUN(test_missing_parameter_refused);
  TEST_RUN(test_unsupported_values_refused);
  TEST_RUN(test_supported_values_accepted);
  TEST_RUN(test_tracks_h2);

  TEST_SUMMARY();
  return TEST_RESULT();
}
