/**
 * @file    test_unit_sage26_parameter_refusals.c
 * @brief   Parameter refusals of the sage26 modules at initialisation
 *
 * Every module validates the run-file parameters it reads when it initialises: a required key that
 * is absent, a switch at a value the package does not support, or a number outside its range makes
 * init() fail with an error that names the parameter. This file is the single home of those
 * refusals. Placement and partner refusals are in test_unit_sage_dependency_contracts.c.
 *
 * Each row changes one parameter of the package's complete default pipeline and parameter set (a
 * row may name a second parameter to change with it), the modules and values of the full-physics
 * run file (models/sage26/modules/_tests/input/test_physics_binary.yaml), and lists the modules
 * that must refuse it, in pipeline order. A row asserts that
 *
 *   - module_system_init() fails, the log names the parameter, and the module that refused is the
 *     first listed (the first module in pipeline order that reads the parameter at that value);
 *   - each listed module's own init() fails when called with the changed configuration, naming the
 *     parameter, so a module that stopped validating a parameter an earlier module also validates
 *     is still caught.
 *
 * The rows cover, for every parameter, a missing key where the key is required, every value that is
 * out of range or unsupported, and each distinct bound a parameter has (for example the three
 * different upper bounds of FeedbackEjectionEfficiency). A second table holds the accepted values:
 * every supported switch value, the inclusive edges of each range and the optional key left out
 * each initialise the whole pipeline, so a refusal cannot pass because the pipeline itself is
 * broken.
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
#include <stdlib.h>
#include <string.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* The modules that read run-file parameters, in the order the default pipeline initialises them */
#define PARAMETER_MODULES(X)                                                                       \
  X(sage26_initialise_merger_clock)                                                                \
  X(sage26_set_halo_concentration)                                                                 \
  X(sage26_set_disk_scale_radius)                                                                  \
  X(sage26_reionization)                                                                           \
  X(sage26_determine_regime)                                                                       \
  X(sage26_prepare_infall_budget)                                                                  \
  X(sage26_determine_ffb_regime)                                                                   \
  X(sage26_apply_infall)                                                                           \
  X(sage26_reincorporation)                                                                        \
  X(sage26_satellite_stripping)                                                                    \
  X(sage26_calculate_cooling_budget)                                                               \
  X(sage26_radio_mode_heating)                                                                     \
  X(sage26_regime_aware_cooling)                                                                   \
  X(sage26_ffb_star_formation)                                                                     \
  X(sage26_calculate_star_formation)                                                               \
  X(sage26_calculate_supernova_feedback)                                                           \
  X(sage26_apply_star_formation_supernova)                                                         \
  X(sage26_disk_instability)                                                                       \
  X(sage26_quasar_mode)                                                                            \
  X(sage26_starburst_feedback)                                                                     \
  X(sage26_apply_metal_enrichment)                                                                 \
  X(sage26_resolve_mergers_and_disruption)

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
#define POST_TIMESTEP "post_timestep"
#define PRE(module) {PRE_TIMESTEP, module, PROCESSING_MODE_FULL_HALO}
#define POST(module) {POST_TIMESTEP, module, PROCESSING_MODE_FULL_HALO}
#define GAL_FH(module) {"galaxy_physics", module, PROCESSING_MODE_FULL_HALO}
#define GAL(module) {"galaxy_physics", module, PROCESSING_MODE_BY_GALAXY}
#define MRG_FH(module) {"satellite_mergers", module, PROCESSING_MODE_FULL_HALO}
#define MRG_EV(module) {"satellite_mergers", module, PROCESSING_MODE_PER_EVENT}

/* The complete default pipeline: the modules, phases, order and modes of the full-physics run file
 */
static const struct entry PIPELINE[] = {
    PRE("sage26_initialise_merger_clock"),
    PRE("sage26_set_halo_concentration"),
    PRE("sage26_set_disk_scale_radius"),
    PRE("sage26_reionization"),
    PRE("sage26_determine_regime"),
    PRE("sage26_prepare_infall_budget"),
    PRE("sage26_determine_ffb_regime"),
    GAL_FH("sage26_apply_infall"),
    GAL_FH("sage26_reincorporation"),
    GAL("sage26_satellite_stripping"),
    GAL("sage26_calculate_cooling_budget"),
    GAL("sage26_radio_mode_heating"),
    GAL("sage26_apply_cooling"),
    GAL("sage26_regime_aware_cooling"),
    GAL("sage26_ffb_star_formation"),
    GAL("sage26_calculate_star_formation"),
    GAL("sage26_calculate_supernova_feedback"),
    GAL("sage26_apply_star_formation_supernova"),
    GAL("sage26_disk_instability"),
    GAL("sage26_quasar_mode"),
    GAL("sage26_starburst_feedback"),
    GAL("sage26_apply_metal_enrichment"),
    MRG_FH("sage26_resolve_mergers_and_disruption"),
    MRG_EV("sage26_quasar_mode"),
    MRG_EV("sage26_starburst_feedback"),
    POST("sage26_finalize_cooling_heating"),
    POST("sage26_finalize_sfr_histories"),
    POST("sage26_record_transition_state"),
};

/** @brief One row: the changed parameter(s) and the modules that must refuse the change */
struct parameter_case {
  const char *key;
  const char *value; /* NULL removes the key */
  const char *with_key;
  const char *with_value;     /* the second parameter's value, where the row changes two */
  const char *const *readers; /* in pipeline order; NULL ends the list; unused by accepted rows */
};

#define M(name) "sage26_" #name
#define READERS(name, ...) static const char *const name[] = {__VA_ARGS__, NULL}

READERS(GLOBAL_BARYON_FRACTION, M(reionization), M(prepare_infall_budget), M(satellite_stripping));
READERS(CGM_RECIPE_ON, M(determine_regime), M(prepare_infall_budget), M(apply_infall),
        M(reincorporation), M(satellite_stripping), M(calculate_cooling_budget),
        M(regime_aware_cooling), M(ffb_star_formation), M(apply_star_formation_supernova),
        M(quasar_mode), M(starburst_feedback), M(apply_metal_enrichment),
        M(resolve_mergers_and_disruption));
READERS(MSHOCK_MSUN, M(determine_regime), M(regime_aware_cooling));
READERS(RADIO_MODE_EFFICIENCY, M(radio_mode_heating), M(regime_aware_cooling));
READERS(AGN_RECIPE, M(radio_mode_heating), M(regime_aware_cooling), M(quasar_mode),
        M(starburst_feedback));
READERS(REINCORPORATION_FACTOR, M(reincorporation));
READERS(YIELD_AND_FRAC_Z, M(ffb_star_formation), M(starburst_feedback), M(apply_metal_enrichment));
READERS(SFR_EFFICIENCY, M(calculate_star_formation));
READERS(STAR_FORMING_DISK_FACTOR, M(ffb_star_formation), M(calculate_star_formation),
        M(disk_instability), M(starburst_feedback));
READERS(SF_PRESCRIPTION, M(ffb_star_formation), M(calculate_star_formation),
        M(apply_star_formation_supernova), M(quasar_mode), M(starburst_feedback));
READERS(H2_RADIAL, M(calculate_star_formation));
READERS(STARBURST_COLD_GAS, M(starburst_feedback));
READERS(REHEATING_EPSILON, M(ffb_star_formation), M(calculate_supernova_feedback),
        M(starburst_feedback));
READERS(EJECTION_EFFICIENCY, M(ffb_star_formation), M(calculate_supernova_feedback),
        M(starburst_feedback));
READERS(EJECTION_ABOVE_10, M(starburst_feedback));
READERS(FIRE_MODE, M(ffb_star_formation), M(calculate_supernova_feedback), M(starburst_feedback));
READERS(FFB_REQUIRES_FIRE, M(ffb_star_formation));
READERS(THRESHOLD_MAJOR_MERGER, M(starburst_feedback), M(resolve_mergers_and_disruption));
READERS(THRESHOLD_SAT_DISRUPTION, M(resolve_mergers_and_disruption));
READERS(RECYCLE_FRACTION, M(ffb_star_formation), M(apply_star_formation_supernova),
        M(starburst_feedback));
READERS(BH_AND_QUASAR, M(quasar_mode), M(starburst_feedback));
READERS(BULGE_SIZE_ON, M(set_disk_scale_radius), M(disk_instability), M(starburst_feedback),
        M(resolve_mergers_and_disruption));
READERS(MERGER_TIME_FACTOR, M(initialise_merger_clock));
READERS(CONCENTRATION_ON, M(set_halo_concentration));
READERS(ENHANCED_STAR_FORMATION, M(determine_ffb_regime), M(ffb_star_formation));
READERS(ENHANCED_SF_EFFICIENCY, M(ffb_star_formation));

static const struct parameter_case REFUSALS[] = {
    /* Cosmic baryon fraction, (0, 1] */
    {"GlobalBaryonFraction", NULL, NULL, NULL, GLOBAL_BARYON_FRACTION},
    {"GlobalBaryonFraction", "0.0", NULL, NULL, GLOBAL_BARYON_FRACTION},
    {"GlobalBaryonFraction", "-0.1", NULL, NULL, GLOBAL_BARYON_FRACTION},
    {"GlobalBaryonFraction", "1.5", NULL, NULL, GLOBAL_BARYON_FRACTION},

    /* CGM recipe, 0 or 1 */
    {"CGMrecipeOn", NULL, NULL, NULL, CGM_RECIPE_ON},
    {"CGMrecipeOn", "2", NULL, NULL, CGM_RECIPE_ON},
    {"CGMrecipeOn", "-1", NULL, NULL, CGM_RECIPE_ON},
    {"MShockMsun", NULL, NULL, NULL, MSHOCK_MSUN},
    {"MShockMsun", "0.0", NULL, NULL, MSHOCK_MSUN},
    {"MShockMsun", "-6.0e11", NULL, NULL, MSHOCK_MSUN},
    {"MShockMsun", "nan", NULL, NULL, MSHOCK_MSUN},
    {"MShockMsun", "inf", NULL, NULL, MSHOCK_MSUN},
    /* validated whether or not the CGM recipe is on */
    {"MShockMsun", NULL, "CGMrecipeOn", "0", MSHOCK_MSUN},
    {"MShockMsun", "0.0", "CGMrecipeOn", "0", MSHOCK_MSUN},

    /* AGN recipe, 0 or 2, and the radio-mode efficiency, [0, 1] */
    {"AGNrecipe", NULL, NULL, NULL, AGN_RECIPE},
    {"AGNrecipe", "1", NULL, NULL, AGN_RECIPE},
    {"AGNrecipe", "3", NULL, NULL, AGN_RECIPE},
    {"AGNrecipe", "-1", NULL, NULL, AGN_RECIPE},
    {"RadioModeEfficiency", NULL, NULL, NULL, RADIO_MODE_EFFICIENCY},
    {"RadioModeEfficiency", "-0.1", NULL, NULL, RADIO_MODE_EFFICIENCY},
    {"RadioModeEfficiency", "1.5", NULL, NULL, RADIO_MODE_EFFICIENCY},

    /* Reincorporation, [0, 10] */
    {"ReIncorporationFactor", NULL, NULL, NULL, REINCORPORATION_FACTOR},
    {"ReIncorporationFactor", "-0.1", NULL, NULL, REINCORPORATION_FACTOR},
    {"ReIncorporationFactor", "10.5", NULL, NULL, REINCORPORATION_FACTOR},

    /* Metal yield and the fraction of metals leaving the disk, each [0, 1] */
    {"Yield", NULL, NULL, NULL, YIELD_AND_FRAC_Z},
    {"Yield", "-0.1", NULL, NULL, YIELD_AND_FRAC_Z},
    {"Yield", "1.5", NULL, NULL, YIELD_AND_FRAC_Z},
    {"FracZleaveDisk", NULL, NULL, NULL, YIELD_AND_FRAC_Z},
    {"FracZleaveDisk", "-0.1", NULL, NULL, YIELD_AND_FRAC_Z},
    {"FracZleaveDisk", "1.5", NULL, NULL, YIELD_AND_FRAC_Z},
    {"RecycleFraction", NULL, NULL, NULL, RECYCLE_FRACTION},
    {"RecycleFraction", "-0.1", NULL, NULL, RECYCLE_FRACTION},
    {"RecycleFraction", "1.5", NULL, NULL, RECYCLE_FRACTION},

    /* Star formation: efficiency [0, 1], disk factor [0, 10], prescription 0 or 1, and the H2
     * integration, whose only supported values are SAGE26's defaults */
    {"SfrEfficiency", NULL, NULL, NULL, SFR_EFFICIENCY},
    {"SfrEfficiency", "-0.1", NULL, NULL, SFR_EFFICIENCY},
    {"SfrEfficiency", "1.5", NULL, NULL, SFR_EFFICIENCY},
    {"StarFormingDiskFactor", NULL, NULL, NULL, STAR_FORMING_DISK_FACTOR},
    {"StarFormingDiskFactor", "-0.1", NULL, NULL, STAR_FORMING_DISK_FACTOR},
    {"StarFormingDiskFactor", "10.5", NULL, NULL, STAR_FORMING_DISK_FACTOR},
    {"SFprescription", NULL, NULL, NULL, SF_PRESCRIPTION},
    {"SFprescription", "2", NULL, NULL, SF_PRESCRIPTION},
    {"SFprescription", "3", NULL, NULL, SF_PRESCRIPTION},
    {"SFprescription", "7", NULL, NULL, SF_PRESCRIPTION},
    {"SFprescription", "-1", NULL, NULL, SF_PRESCRIPTION},
    {"H2RadialIntegrationOn", NULL, NULL, NULL, H2_RADIAL},
    {"H2RadialIntegrationOn", "0", NULL, NULL, H2_RADIAL},
    {"H2RadialIntegrationOn", "2", NULL, NULL, H2_RADIAL},
    {"H2RadialNBins", NULL, NULL, NULL, H2_RADIAL},
    {"H2RadialNBins", "24", NULL, NULL, H2_RADIAL},
    {"H2RadialNBins", "50", NULL, NULL, H2_RADIAL},
    {"H2RadialNBins", "0", NULL, NULL, H2_RADIAL},
    {"H2RadialRMaxFactor", NULL, NULL, NULL, H2_RADIAL},
    {"H2RadialRMaxFactor", "4.0", NULL, NULL, H2_RADIAL},
    {"H2RadialRMaxFactor", "5.5", NULL, NULL, H2_RADIAL},
    {"H2RadialRMaxFactor", "0.0", NULL, NULL, H2_RADIAL},
    {"StarburstColdGasOn", NULL, NULL, NULL, STARBURST_COLD_GAS},
    {"StarburstColdGasOn", "0", NULL, NULL, STARBURST_COLD_GAS},
    {"StarburstColdGasOn", "2", NULL, NULL, STARBURST_COLD_GAS},

    /* Stellar feedback: reheating >= 0, ejection with a bound of 100 in the supernova and
     * feedback-free-burst modules and of 10 in the burst module, FIRE mode 0 or 1 (0 not
     * supported with feedback-free bursts) */
    {"FeedbackReheatingEpsilon", NULL, NULL, NULL, REHEATING_EPSILON},
    {"FeedbackReheatingEpsilon", "-1.0", NULL, NULL, REHEATING_EPSILON},
    {"FeedbackEjectionEfficiency", NULL, NULL, NULL, EJECTION_EFFICIENCY},
    {"FeedbackEjectionEfficiency", "-0.1", NULL, NULL, EJECTION_EFFICIENCY},
    {"FeedbackEjectionEfficiency", "150.0", NULL, NULL, EJECTION_EFFICIENCY},
    {"FeedbackEjectionEfficiency", "50.0", NULL, NULL, EJECTION_ABOVE_10},
    {"FIREmodeOn", NULL, NULL, NULL, FIRE_MODE},
    {"FIREmodeOn", "2", NULL, NULL, FIRE_MODE},
    {"FIREmodeOn", "-1", NULL, NULL, FIRE_MODE},
    {"FIREmodeOn", "0", NULL, NULL, FFB_REQUIRES_FIRE},

    /* Mergers, black holes and bulge sizes */
    {"ThresholdMajorMerger", NULL, NULL, NULL, THRESHOLD_MAJOR_MERGER},
    {"ThresholdMajorMerger", "-0.1", NULL, NULL, THRESHOLD_MAJOR_MERGER},
    {"ThresholdMajorMerger", "1.5", NULL, NULL, THRESHOLD_MAJOR_MERGER},
    {"ThresholdSatDisruption", NULL, NULL, NULL, THRESHOLD_SAT_DISRUPTION},
    {"ThresholdSatDisruption", "0.0", NULL, NULL, THRESHOLD_SAT_DISRUPTION},
    {"ThresholdSatDisruption", "1500.0", NULL, NULL, THRESHOLD_SAT_DISRUPTION},
    {"MergerTimeFactor", "0.0", NULL, NULL, MERGER_TIME_FACTOR},
    {"MergerTimeFactor", "1500.0", NULL, NULL, MERGER_TIME_FACTOR},
    {"BlackHoleGrowthRate", NULL, NULL, NULL, BH_AND_QUASAR},
    {"BlackHoleGrowthRate", "0.0", NULL, NULL, BH_AND_QUASAR},
    {"BlackHoleGrowthRate", "1.5", NULL, NULL, BH_AND_QUASAR},
    {"QuasarModeEfficiency", NULL, NULL, NULL, BH_AND_QUASAR},
    {"QuasarModeEfficiency", "-0.1", NULL, NULL, BH_AND_QUASAR},
    {"QuasarModeEfficiency", "1.5", NULL, NULL, BH_AND_QUASAR},
    {"BulgeSizeOn", NULL, NULL, NULL, BULGE_SIZE_ON},
    {"BulgeSizeOn", "1", NULL, NULL, BULGE_SIZE_ON},
    {"BulgeSizeOn", "2", NULL, NULL, BULGE_SIZE_ON},
    {"BulgeSizeOn", "4", NULL, NULL, BULGE_SIZE_ON},
    {"BulgeSizeOn", "-1", NULL, NULL, BULGE_SIZE_ON},

    /* Halo concentration and feedback-free bursts */
    {"ConcentrationOn", NULL, NULL, NULL, CONCENTRATION_ON},
    {"ConcentrationOn", "1", NULL, NULL, CONCENTRATION_ON},
    {"ConcentrationOn", "2", NULL, NULL, CONCENTRATION_ON},
    {"ConcentrationOn", "-1", NULL, NULL, CONCENTRATION_ON},
    {"EnhancedStarFormationOn", NULL, NULL, NULL, ENHANCED_STAR_FORMATION},
    {"EnhancedStarFormationOn", "2", NULL, NULL, ENHANCED_STAR_FORMATION},
    {"EnhancedStarFormationOn", "-1", NULL, NULL, ENHANCED_STAR_FORMATION},
    {"EnhancedSFEfficiency", NULL, NULL, NULL, ENHANCED_SF_EFFICIENCY},
    {"EnhancedSFEfficiency", "-0.1", NULL, NULL, ENHANCED_SF_EFFICIENCY},
    {"EnhancedSFEfficiency", "1.5", NULL, NULL, ENHANCED_SF_EFFICIENCY},
    {"EnhancedSFEfficiency", "nan", NULL, NULL, ENHANCED_SF_EFFICIENCY},
};

/* Configurations that must initialise: the unchanged defaults, each supported switch value, the
 * inclusive edges of the ranges and the optional key left out */
static const struct parameter_case ACCEPTED[] = {
    {NULL, NULL, NULL, NULL, NULL},
    {"CGMrecipeOn", "0", NULL, NULL, NULL},
    {"EnhancedStarFormationOn", "0", NULL, NULL, NULL},
    {"ConcentrationOn", "0", NULL, NULL, NULL},
    {"SFprescription", "0", NULL, NULL, NULL},
    {"FIREmodeOn", "0", "EnhancedStarFormationOn", "0", NULL},
    {"AGNrecipe", "0", NULL, NULL, NULL},
    {"BulgeSizeOn", "0", NULL, NULL, NULL},
    {"MergerTimeFactor", NULL, NULL, NULL, NULL},
    {"GlobalBaryonFraction", "1.0", NULL, NULL, NULL},
    {"RadioModeEfficiency", "0.0", NULL, NULL, NULL},
    {"RadioModeEfficiency", "1.0", NULL, NULL, NULL},
    {"ReIncorporationFactor", "0.0", NULL, NULL, NULL},
    {"ReIncorporationFactor", "10.0", NULL, NULL, NULL},
    {"SfrEfficiency", "0.0", NULL, NULL, NULL},
    {"SfrEfficiency", "1.0", NULL, NULL, NULL},
    {"StarFormingDiskFactor", "0.0", NULL, NULL, NULL},
    {"StarFormingDiskFactor", "10.0", NULL, NULL, NULL},
    {"FeedbackReheatingEpsilon", "0.0", NULL, NULL, NULL},
    {"FeedbackEjectionEfficiency", "10.0", NULL, NULL, NULL},
    {"Yield", "1.0", NULL, NULL, NULL},
    {"ThresholdMajorMerger", "1.0", NULL, NULL, NULL},
    {"ThresholdSatDisruption", "1000.0", NULL, NULL, NULL},
    {"BlackHoleGrowthRate", "1.0", NULL, NULL, NULL},
    {"QuasarModeEfficiency", "0.0", NULL, NULL, NULL},
    {"EnhancedSFEfficiency", "1.0", NULL, NULL, NULL},
};

/** @brief Free the three phase arrays of a pipeline whose init() never reached cleanup */
static void free_pipeline(void) {
  test_free_pre_timestep();
  test_free_substep_phases();
  test_free_post_timestep();
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
  sage26_test_load_run_file_parameters(NULL, 0);
  if (row->key != NULL) {
    change_parameter(row->key, row->value);
  }
  if (row->with_key != NULL) {
    change_parameter(row->with_key, row->with_value);
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
    } else if (strcmp(e->phase, POST_TIMESTEP) == 0) {
      test_post_timestep_add(e->module, e->mode);
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

/** @brief The row's label for failure messages: the changed parameter(s) and value(s) */
static void describe(const struct parameter_case *row, char *out, const size_t size) {
  if (row->key == NULL) {
    snprintf(out, size, "the default configuration");
    return;
  }
  const int n = snprintf(out, size, "%s %s", row->key, row->value != NULL ? row->value : "missing");
  if (row->with_key != NULL && (size_t)n < size) {
    snprintf(out + n, size - (size_t)n, " with %s %s", row->with_key, row->with_value);
  }
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
  printf("Test Suite: SAGE26 parameter refusals at initialisation\n");
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
