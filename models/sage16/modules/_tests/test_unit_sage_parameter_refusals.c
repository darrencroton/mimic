/**
 * @file    test_unit_sage_parameter_refusals.c
 * @brief   Parameter refusals of the sage16 modules at initialisation
 *
 * Every module validates the run-file parameters it reads when it initialises: a number outside
 * its range makes init() fail with an error that names the parameter. This file is the single home
 * of those refusals. Placement and partner refusals are in test_unit_sage_dependency_contracts.c.
 *
 * Each row changes one parameter of the package's complete default pipeline and parameter set,
 * the modules and values of the full-physics run file
 * (models/sage16/modules/_tests/input/test_physics_binary.yaml), and lists the modules that must
 * refuse it, in pipeline order. A row asserts that
 *
 *   - module_system_init() fails, the log names the parameter, and the module that refused is the
 *     first listed (the first module in pipeline order that reads the parameter at that value);
 *   - each listed module's own init() fails when called with the changed configuration, naming the
 *     parameter, so a module that stopped validating a parameter an earlier module also validates
 *     is still caught.
 *
 * The rows cover, for every parameter, a missing key, every value that is out of range, and each
 * distinct bound a parameter has (for example the two different upper bounds of
 * FeedbackEjectionEfficiency). sage16 validates no switch parameter other than AGNrecipe, whose
 * accepted values are 0 to 3. A second table holds the accepted values: the inclusive edges of each
 * range and each AGNrecipe value initialise the whole pipeline, so a refusal cannot pass because
 * the pipeline itself is broken.
 *
 * Test cases:
 *   - test_parameter_refusals: the refusal rows
 *   - test_accepted_values: the default configuration and the accepted rows initialise the pipeline
 */

#include "framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <string.h>

/* Shared sage16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* The modules that read run-file parameters, in the order the default pipeline initialises them */
#define PARAMETER_MODULES(X)                                                                       \
  X(sage_reionization)                                                                             \
  X(sage_prepare_infall_budget)                                                                    \
  X(sage_reincorporation)                                                                          \
  X(sage_satellite_stripping)                                                                      \
  X(sage_radio_mode_heating)                                                                       \
  X(sage_calculate_star_formation)                                                                 \
  X(sage_calculate_supernova_feedback)                                                             \
  X(sage_apply_star_formation_supernova)                                                           \
  X(sage_disk_instability)                                                                         \
  X(sage_quasar_mode)                                                                              \
  X(sage_starburst_feedback)                                                                       \
  X(sage_apply_metal_enrichment)                                                                   \
  X(sage_resolve_mergers_and_disruption)

#define DECLARE_INIT(module) extern int module##_init(void);
PARAMETER_MODULES(DECLARE_INIT)

static const struct {
  const char *name;
  int (*init)(void);
} MODULES[] = {
#define MODULE_ENTRY(module) {#module, module##_init},
    PARAMETER_MODULES(MODULE_ENTRY)
#undef MODULE_ENTRY
};

/** @brief One pipeline entry: the phase it sits in, the module and its processing mode */
struct entry {
  const char *phase;
  const char *module;
  enum ProcessingMode mode;
};

#define PRE_TIMESTEP "pre_timestep"
#define PRE(module) {PRE_TIMESTEP, module, PROCESSING_MODE_FULL_HALO}
#define GAL_FH(module) {"galaxy_physics", module, PROCESSING_MODE_FULL_HALO}
#define GAL(module) {"galaxy_physics", module, PROCESSING_MODE_BY_GALAXY}
#define MRG_FH(module) {"satellite_mergers", module, PROCESSING_MODE_FULL_HALO}
#define MRG_EV(module) {"satellite_mergers", module, PROCESSING_MODE_PER_EVENT}

/* The complete default pipeline: the modules, phases, order and modes of the full-physics run file
 */
static const struct entry PIPELINE[] = {
    PRE("sage_reionization"),
    PRE("sage_prepare_infall_budget"),
    PRE("sage_set_disk_scale_radius"),
    PRE("sage_initialise_merger_clock"),
    GAL_FH("sage_apply_infall"),
    GAL_FH("sage_reincorporation"),
    GAL("sage_satellite_stripping"),
    GAL("sage_calculate_cooling_budget"),
    GAL("sage_radio_mode_heating"),
    GAL("sage_apply_cooling"),
    GAL("sage_calculate_star_formation"),
    GAL("sage_calculate_supernova_feedback"),
    GAL("sage_apply_star_formation_supernova"),
    GAL("sage_disk_instability"),
    GAL("sage_quasar_mode"),
    GAL("sage_starburst_feedback"),
    GAL("sage_apply_metal_enrichment"),
    MRG_FH("sage_resolve_mergers_and_disruption"),
    MRG_EV("sage_quasar_mode"),
    MRG_EV("sage_starburst_feedback"),
};

/* The parameters of the full-physics run file, at the run file's values */
static const struct {
  const char *name;
  const char *value;
} DEFAULTS[] = {
    {"GlobalBaryonFraction", "0.17"},
    {"SfrEfficiency", "0.05"},
    {"StarFormingDiskFactor", "3.0"},
    {"FeedbackReheatingEpsilon", "3.0"},
    {"FeedbackEjectionEfficiency", "0.3"},
    {"ReIncorporationFactor", "0.15"},
    {"AGNrecipe", "2"},
    {"RadioModeEfficiency", "0.08"},
    {"BlackHoleGrowthRate", "0.015"},
    {"QuasarModeEfficiency", "0.005"},
    {"RecycleFraction", "0.43"},
    {"Yield", "0.025"},
    {"FracZleaveDisk", "0.0"},
    {"ThresholdMajorMerger", "0.3"},
    {"ThresholdSatDisruption", "1.0"},
};

/** @brief One row: the changed parameter and the modules that must refuse the change */
struct parameter_case {
  const char *key;
  const char *value;          /* NULL removes the key */
  const char *const *readers; /* in pipeline order; NULL ends the list; unused by accepted rows */
};

#define M(name) "sage_" #name
#define READERS(name, ...) static const char *const name[] = {__VA_ARGS__, NULL}

READERS(GLOBAL_BARYON_FRACTION, M(reionization), M(prepare_infall_budget), M(satellite_stripping));
READERS(REINCORPORATION_FACTOR, M(reincorporation));
READERS(RADIO_MODE_EFFICIENCY, M(radio_mode_heating));
READERS(AGN_RECIPE, M(radio_mode_heating), M(quasar_mode), M(starburst_feedback));
READERS(SFR_EFFICIENCY, M(calculate_star_formation));
READERS(STAR_FORMING_DISK_FACTOR, M(calculate_star_formation), M(disk_instability),
        M(starburst_feedback));
READERS(REHEATING_EPSILON_ABOVE_100, M(calculate_supernova_feedback), M(starburst_feedback));
READERS(REHEATING_EPSILON_ABOVE_10, M(starburst_feedback));
READERS(EJECTION_EFFICIENCY, M(calculate_supernova_feedback), M(starburst_feedback));
READERS(EJECTION_ABOVE_10, M(starburst_feedback));
READERS(RECYCLE_FRACTION, M(apply_star_formation_supernova), M(starburst_feedback));
READERS(YIELD_AND_FRAC_Z, M(starburst_feedback), M(apply_metal_enrichment));
READERS(THRESHOLD_MAJOR_MERGER, M(starburst_feedback), M(resolve_mergers_and_disruption));
READERS(THRESHOLD_SAT_DISRUPTION, M(resolve_mergers_and_disruption));
READERS(BH_AND_QUASAR, M(quasar_mode), M(starburst_feedback));

static const struct parameter_case REFUSALS[] = {
    /* Cosmic baryon fraction, (0, 1] */
    {"GlobalBaryonFraction", NULL, GLOBAL_BARYON_FRACTION},
    {"GlobalBaryonFraction", "0.0", GLOBAL_BARYON_FRACTION},
    {"GlobalBaryonFraction", "-0.1", GLOBAL_BARYON_FRACTION},
    {"GlobalBaryonFraction", "1.5", GLOBAL_BARYON_FRACTION},

    /* AGN recipe, 0 to 3, and the radio-mode efficiency, [0, 1] */
    {"AGNrecipe", NULL, AGN_RECIPE},
    {"AGNrecipe", "-1", AGN_RECIPE},
    {"AGNrecipe", "4", AGN_RECIPE},
    {"RadioModeEfficiency", NULL, RADIO_MODE_EFFICIENCY},
    {"RadioModeEfficiency", "-0.1", RADIO_MODE_EFFICIENCY},
    {"RadioModeEfficiency", "1.5", RADIO_MODE_EFFICIENCY},

    /* Reincorporation, [0, 10] */
    {"ReIncorporationFactor", NULL, REINCORPORATION_FACTOR},
    {"ReIncorporationFactor", "-0.1", REINCORPORATION_FACTOR},
    {"ReIncorporationFactor", "10.5", REINCORPORATION_FACTOR},

    /* Metal yield, the fraction of metals leaving the disk and the recycled fraction, each [0, 1]
     */
    {"Yield", NULL, YIELD_AND_FRAC_Z},
    {"Yield", "-0.1", YIELD_AND_FRAC_Z},
    {"Yield", "1.5", YIELD_AND_FRAC_Z},
    {"FracZleaveDisk", NULL, YIELD_AND_FRAC_Z},
    {"FracZleaveDisk", "-0.1", YIELD_AND_FRAC_Z},
    {"FracZleaveDisk", "1.5", YIELD_AND_FRAC_Z},
    {"RecycleFraction", NULL, RECYCLE_FRACTION},
    {"RecycleFraction", "-0.1", RECYCLE_FRACTION},
    {"RecycleFraction", "1.5", RECYCLE_FRACTION},

    /* Star formation: efficiency [0, 1], disk factor [0, 10] */
    {"SfrEfficiency", NULL, SFR_EFFICIENCY},
    {"SfrEfficiency", "-0.1", SFR_EFFICIENCY},
    {"SfrEfficiency", "1.5", SFR_EFFICIENCY},
    {"StarFormingDiskFactor", NULL, STAR_FORMING_DISK_FACTOR},
    {"StarFormingDiskFactor", "-0.1", STAR_FORMING_DISK_FACTOR},
    {"StarFormingDiskFactor", "10.5", STAR_FORMING_DISK_FACTOR},

    /* Stellar feedback: reheating in [0, 100] for the supernova module and (0, 10] for the burst
     * module, ejection in [0, 100] and [0, 10] */
    {"FeedbackReheatingEpsilon", NULL, REHEATING_EPSILON_ABOVE_100},
    {"FeedbackReheatingEpsilon", "-1.0", REHEATING_EPSILON_ABOVE_100},
    {"FeedbackReheatingEpsilon", "150.0", REHEATING_EPSILON_ABOVE_100},
    {"FeedbackReheatingEpsilon", "50.0", REHEATING_EPSILON_ABOVE_10},
    {"FeedbackReheatingEpsilon", "0.0", REHEATING_EPSILON_ABOVE_10},
    {"FeedbackEjectionEfficiency", NULL, EJECTION_EFFICIENCY},
    {"FeedbackEjectionEfficiency", "-0.1", EJECTION_EFFICIENCY},
    {"FeedbackEjectionEfficiency", "150.0", EJECTION_EFFICIENCY},
    {"FeedbackEjectionEfficiency", "50.0", EJECTION_ABOVE_10},

    /* Mergers and black holes */
    {"ThresholdMajorMerger", NULL, THRESHOLD_MAJOR_MERGER},
    {"ThresholdMajorMerger", "-0.1", THRESHOLD_MAJOR_MERGER},
    {"ThresholdMajorMerger", "1.5", THRESHOLD_MAJOR_MERGER},
    {"ThresholdSatDisruption", NULL, THRESHOLD_SAT_DISRUPTION},
    {"ThresholdSatDisruption", "0.0", THRESHOLD_SAT_DISRUPTION},
    {"ThresholdSatDisruption", "1500.0", THRESHOLD_SAT_DISRUPTION},
    {"BlackHoleGrowthRate", NULL, BH_AND_QUASAR},
    {"BlackHoleGrowthRate", "0.0", BH_AND_QUASAR},
    {"BlackHoleGrowthRate", "1.5", BH_AND_QUASAR},
    {"QuasarModeEfficiency", NULL, BH_AND_QUASAR},
    {"QuasarModeEfficiency", "-0.1", BH_AND_QUASAR},
    {"QuasarModeEfficiency", "1.5", BH_AND_QUASAR},
};

/* Configurations that must initialise: the unchanged defaults, each AGNrecipe value and the
 * inclusive edges of the ranges */
static const struct parameter_case ACCEPTED[] = {
    {NULL, NULL, NULL},
    {"AGNrecipe", "0", NULL},
    {"AGNrecipe", "1", NULL},
    {"AGNrecipe", "3", NULL},
    {"GlobalBaryonFraction", "1.0", NULL},
    {"RadioModeEfficiency", "0.0", NULL},
    {"RadioModeEfficiency", "1.0", NULL},
    {"ReIncorporationFactor", "0.0", NULL},
    {"ReIncorporationFactor", "10.0", NULL},
    {"SfrEfficiency", "0.0", NULL},
    {"SfrEfficiency", "1.0", NULL},
    {"StarFormingDiskFactor", "0.0", NULL},
    {"StarFormingDiskFactor", "10.0", NULL},
    {"FeedbackReheatingEpsilon", "10.0", NULL},
    {"FeedbackEjectionEfficiency", "0.0", NULL},
    {"FeedbackEjectionEfficiency", "10.0", NULL},
    {"Yield", "1.0", NULL},
    {"ThresholdMajorMerger", "1.0", NULL},
    {"ThresholdSatDisruption", "1000.0", NULL},
    {"BlackHoleGrowthRate", "1.0", NULL},
    {"QuasarModeEfficiency", "0.0", NULL},
};

/** @brief Free the phase arrays of a pipeline whose init() never reached cleanup */
static void free_pipeline(void) {
  test_free_pre_timestep();
  test_free_substep_phases();
}

/** @brief Set a run-file parameter to `value`, or remove it when `value` is NULL */
static void change_parameter(const char *key, const char *value) {
  int kept = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, key) == 0) {
      if (value == NULL) {
        continue;
      }
      snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
    }
    MimicConfig.ModelParams[kept++] = MimicConfig.ModelParams[i];
  }
  MimicConfig.NumModelParams = kept;
}

/** @brief Configure the complete default pipeline and parameters, then apply a row's changes */
static void configure(const struct parameter_case *row) {
  reset_config();
  for (size_t i = 0; i < COUNT(DEFAULTS); i++) {
    snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", DEFAULTS[i].name);
    snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", DEFAULTS[i].value);
  }
  MimicConfig.NumModelParams = (int)COUNT(DEFAULTS);
  if (row->key != NULL) {
    change_parameter(row->key, row->value);
  }
  MimicConfig.SubSteps = 10;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitLength_in_cm = 3.08568e24;
  MimicConfig.UnitMass_in_g = 1.989e43;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  for (size_t i = 0; i < COUNT(PIPELINE); i++) {
    const struct entry *e = &PIPELINE[i];
    if (strcmp(e->phase, PRE_TIMESTEP) == 0) {
      test_pre_timestep_add(e->module, e->mode);
    } else {
      test_phase_add(e->phase, e->module, e->mode);
    }
  }
}

/** @brief The init() of a module that reads parameters, or NULL for an unknown name */
static int (*find_init(const char *module))(void) {
  for (size_t m = 0; m < COUNT(MODULES); m++) {
    if (strcmp(MODULES[m].name, module) == 0) {
      return MODULES[m].init;
    }
  }
  return NULL;
}

/**
 * @brief Initialise `row`'s configuration, capturing the log: the whole pipeline through
 *        module_system_init(), or only `init` (one module's own init()) when it is given
 */
static int init_captured(const struct parameter_case *row, int (*init)(void), char *log,
                         const size_t log_size) {
  configure(row);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = init != NULL ? init() : module_system_init();
  log[0] = '\0';
  if (capture != NULL) {
    set_log_output(old_out);
    snprintf(log, log_size, "%s", read_captured_log(capture));
    fclose(capture);
  }
  if (init != NULL) {
    free_pipeline();
  } else {
    module_system_cleanup();
  }
  check_memory_leaks();
  return status;
}

/** @brief The row's label for failure messages: the changed parameter and value */
static void describe(const struct parameter_case *row, char *out, const size_t size) {
  if (row->key == NULL) {
    snprintf(out, size, "the default configuration");
    return;
  }
  snprintf(out, size, "%s %s", row->key, row->value != NULL ? row->value : "missing");
}

/**
 * @test    test_parameter_refusals
 * @brief   Each row is refused by the pipeline's first reader of the parameter and by every
 *          listed module's own init(), each naming the parameter
 */
int test_parameter_refusals(void) {
  static char log[16384];
  for (size_t r = 0; r < COUNT(REFUSALS); r++) {
    const struct parameter_case *row = &REFUSALS[r];
    char label[160];
    char message[320];
    describe(row, label, sizeof(label));

    int status = init_captured(row, NULL, log, sizeof(log));
    printf("\n  %s: pipeline init %d", label, status);
    snprintf(message, sizeof(message), "%s: the pipeline must refuse it", label);
    TEST_ASSERT(status != 0, message);
    snprintf(message, sizeof(message), "%s: the error must name the parameter", label);
    TEST_ASSERT(strstr(log, row->key) != NULL, message);
    char refuser[160];
    snprintf(refuser, sizeof(refuser), "Module '%s' initialization failed", row->readers[0]);
    snprintf(message, sizeof(message), "%s: the first reader, %s, must refuse it", label,
             row->readers[0]);
    TEST_ASSERT(strstr(log, refuser) != NULL, message);

    for (size_t k = 0; row->readers[k] != NULL; k++) {
      int (*init)(void) = find_init(row->readers[k]);
      snprintf(message, sizeof(message), "%s: %s must be a module that reads parameters", label,
               row->readers[k]);
      TEST_ASSERT(init != NULL, message);
      status = init_captured(row, init, log, sizeof(log));
      snprintf(message, sizeof(message), "%s: %s's own init must refuse it", label,
               row->readers[k]);
      TEST_ASSERT(status != 0, message);
      snprintf(message, sizeof(message), "%s: %s's error must name the parameter", label,
               row->readers[k]);
      TEST_ASSERT(strstr(log, row->key) != NULL, message);
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_accepted_values
 * @brief   The default configuration, each supported switch value, the inclusive range edges and
 *          the optional key left out all initialise the complete pipeline
 */
int test_accepted_values(void) {
  static char log[16384];
  for (size_t r = 0; r < COUNT(ACCEPTED); r++) {
    const struct parameter_case *row = &ACCEPTED[r];
    char label[160];
    char message[320];
    describe(row, label, sizeof(label));
    const int status = init_captured(row, NULL, log, sizeof(log));
    printf("\n  %s: pipeline init %d", label, status);
    snprintf(message, sizeof(message), "%s: the pipeline must initialise", label);
    TEST_ASSERT(status == 0, message);
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE16 parameter refusals at initialisation\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);
  ensure_modules_registered();

  TEST_RUN(test_parameter_refusals);
  TEST_RUN(test_accepted_values);

  TEST_SUMMARY();
  return TEST_RESULT();
}
