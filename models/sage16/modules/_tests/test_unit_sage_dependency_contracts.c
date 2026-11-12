/**
 * @file    test_unit_sage_dependency_contracts.c
 * @brief   Placement and partner contracts of the sage16 modules at initialisation
 *
 * Each module's init() checks the pipeline around it: partners that must be configured, the order
 * in which modules must run, and where a module may be placed. This file is the single home of
 * those checks for the package. One table row configures a pipeline, initialises it, and asserts
 *
 *   - a refusal: module_system_init() fails, the log says that the intended module refused
 *     ("Module '<name>' initialization failed") and contains wording unique to the intended check,
 *     which names the missing or misplaced partner. A refusal for any other reason (a missing
 *     parameter, a changed phase, another module's check) fails the row;
 *   - a warning: initialisation succeeds and the log carries the warning;
 *   - acceptance: initialisation succeeds, showing a guard does not over-refuse.
 *
 * Every row runs on the package's full parameter set. Three checks cannot be reached through
 * module_system_init(): reionization's own ordering guard is always forestalled by the module it
 * names, and the per-event producer checks of the quasar and starburst modules are forestalled by
 * the core's event-contract validation, which refuses the same configuration first. Those rows run
 * the module's init() directly, and one row asserts the core's refusal. Parameter refusals live in
 * test_unit_sage_parameter_refusals.c.
 *
 * Test cases:
 *   - test_dependency_contracts: the table of rows
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

extern int sage_reionization_init(void);
extern int sage_quasar_mode_init(void);
extern int sage_starburst_feedback_init(void);

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define MAX_ENTRIES 8

/** @brief One pipeline entry: the phase it sits in, the module and its processing mode */
struct entry {
  const char *phase;
  const char *module;
  enum ProcessingMode mode;
};

#define PRE_TIMESTEP "pre_timestep"
#define SUBSTEP_PHASE "galaxy_physics"
#define OTHER_PHASE "satellite_mergers"

#define PRE(module) {PRE_TIMESTEP, module, PROCESSING_MODE_FULL_HALO}
#define GAL(module) {SUBSTEP_PHASE, module, PROCESSING_MODE_BY_GALAXY}
#define GAL_FH(module) {SUBSTEP_PHASE, module, PROCESSING_MODE_FULL_HALO}
#define GAL_EV(module) {SUBSTEP_PHASE, module, PROCESSING_MODE_PER_EVENT}
#define OTHER_FH(module) {OTHER_PHASE, module, PROCESSING_MODE_FULL_HALO}
#define OTHER_EV(module) {OTHER_PHASE, module, PROCESSING_MODE_PER_EVENT}
#define OTHER_BG(module) {OTHER_PHASE, module, PROCESSING_MODE_BY_GALAXY}

enum outcome { REFUSED, WARNS, ACCEPTED };

/** @brief One row: a pipeline, the parameters it changes, and the expected outcome */
struct dependency_case {
  const char *name;
  struct entry pipeline[MAX_ENTRIES + 1]; /* ends at the first entry without a module */
  enum outcome outcome;
  const char *refuser;      /* REFUSED: the module whose check fires */
  const char *needle;       /* REFUSED or WARNS: wording the log must contain */
  int (*direct_init)(void); /* when set, call this init() instead of module_system_init() */
};

/* The run-file parameters of the full-physics run file (models/sage16/modules/_tests/input) */
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

static const struct dependency_case CASES[] = {
    /* Infall budget and reionization */
    {.name = "apply_infall without prepare_infall_budget",
     .pipeline = {GAL_FH("sage_apply_infall")},
     .outcome = REFUSED,
     .refuser = "sage_apply_infall",
     .needle = "requires sage_prepare_infall_budget in pre_timestep"},
    {.name = "prepare_infall_budget before reionization",
     .pipeline = {PRE("sage_prepare_infall_budget"), PRE("sage_reionization")},
     .outcome = REFUSED,
     .refuser = "sage_prepare_infall_budget",
     .needle = "requires sage_reionization to run earlier in pre_timestep"},
    {.name = "reionization after prepare_infall_budget (its own guard)",
     .pipeline = {PRE("sage_prepare_infall_budget"), PRE("sage_reionization")},
     .outcome = REFUSED,
     .needle = "sage_reionization must run before sage_prepare_infall_budget",
     .direct_init = sage_reionization_init},

    /* Cooling */
    {.name = "apply_cooling before calculate_cooling_budget",
     .pipeline = {GAL("sage_apply_cooling"), GAL("sage_calculate_cooling_budget")},
     .outcome = REFUSED,
     .refuser = "sage_apply_cooling",
     .needle = "requires sage_calculate_cooling_budget to precede it"},
    {.name = "radio_mode_heating before calculate_cooling_budget",
     .pipeline = {GAL("sage_radio_mode_heating"), GAL("sage_calculate_cooling_budget"),
                  GAL("sage_apply_cooling")},
     .outcome = REFUSED,
     .refuser = "sage_radio_mode_heating",
     .needle = "requires sage_calculate_cooling_budget to precede it"},
    {.name = "radio_mode_heating after apply_cooling",
     .pipeline = {GAL("sage_calculate_cooling_budget"), GAL("sage_apply_cooling"),
                  GAL("sage_radio_mode_heating")},
     .outcome = REFUSED,
     .refuser = "sage_radio_mode_heating",
     .needle = "must precede sage_apply_cooling"},
    {.name = "radio_mode_heating second entry in a phase without the budget",
     .pipeline = {GAL("sage_calculate_cooling_budget"), GAL("sage_radio_mode_heating"),
                  GAL("sage_apply_cooling"), OTHER_BG("sage_radio_mode_heating")},
     .outcome = REFUSED,
     .refuser = "sage_radio_mode_heating",
     .needle = "to precede it in substep phase 'satellite_mergers'"},
    {.name = "radio_mode_heating alone",
     .pipeline = {GAL("sage_radio_mode_heating")},
     .outcome = ACCEPTED},
    /* Star formation and supernova feedback */
    {.name = "calculate_supernova_feedback before calculate_star_formation",
     .pipeline = {GAL("sage_calculate_supernova_feedback"), GAL("sage_calculate_star_formation"),
                  GAL("sage_apply_star_formation_supernova")},
     .outcome = REFUSED,
     .refuser = "sage_calculate_supernova_feedback",
     .needle = "requires sage_calculate_star_formation to precede it"},
    {.name = "apply_star_formation_supernova before calculate_star_formation",
     .pipeline = {GAL("sage_apply_star_formation_supernova"), GAL("sage_calculate_star_formation")},
     .outcome = REFUSED,
     .refuser = "sage_apply_star_formation_supernova",
     .needle = "requires sage_calculate_star_formation to precede it"},
    {.name = "apply_star_formation_supernova before calculate_supernova_feedback",
     .pipeline = {GAL("sage_apply_star_formation_supernova"),
                  GAL("sage_calculate_supernova_feedback")},
     .outcome = REFUSED,
     .refuser = "sage_apply_star_formation_supernova",
     .needle = "requires sage_calculate_supernova_feedback to precede it"},
    {.name = "calculate_star_formation without apply_star_formation_supernova",
     .pipeline = {GAL("sage_calculate_star_formation")},
     .outcome = REFUSED,
     .refuser = "sage_calculate_star_formation",
     .needle = "requires sage_apply_star_formation_supernova in the pipeline"},
    {.name = "calculate_supernova_feedback without apply_star_formation_supernova",
     .pipeline = {GAL("sage_calculate_supernova_feedback")},
     .outcome = REFUSED,
     .refuser = "sage_calculate_supernova_feedback",
     .needle = "requires sage_apply_star_formation_supernova in the pipeline"},
    {.name = "apply_star_formation_supernova alone",
     .pipeline = {GAL("sage_apply_star_formation_supernova")},
     .outcome = WARNS,
     .needle = "neither sage_calculate_star_formation nor sage_calculate_supernova_feedback "
               "is configured"},
    {.name = "apply_star_formation_supernova without apply_metal_enrichment",
     .pipeline = {GAL("sage_calculate_star_formation"), GAL("sage_apply_star_formation_supernova")},
     .outcome = WARNS,
     .needle = "sage_apply_metal_enrichment is not configured"},
    {.name = "apply_metal_enrichment before apply_star_formation_supernova",
     .pipeline = {GAL("sage_calculate_star_formation"), GAL("sage_apply_metal_enrichment"),
                  GAL("sage_apply_star_formation_supernova")},
     .outcome = REFUSED,
     .refuser = "sage_apply_metal_enrichment",
     .needle = "requires sage_apply_star_formation_supernova to precede it"},
    {.name = "apply_metal_enrichment before starburst_feedback",
     .pipeline = {GAL("sage_apply_metal_enrichment"), GAL("sage_starburst_feedback")},
     .outcome = REFUSED,
     .refuser = "sage_apply_metal_enrichment",
     .needle = "must run after sage_starburst_feedback"},

    /* Mergers, quasar mode, starburst and disk instability */
    {.name = "quasar_mode per event without a merger producer (the module's own guard)",
     .pipeline = {OTHER_EV("sage_quasar_mode")},
     .outcome = REFUSED,
     .needle = "sage_quasar_mode (process_per_event) requires "
               "sage_resolve_mergers_and_disruption as process_full_halo",
     .direct_init = sage_quasar_mode_init},
    {.name = "starburst_feedback per event without a merger producer (the module's own guard)",
     .pipeline = {OTHER_EV("sage_starburst_feedback")},
     .outcome = REFUSED,
     .needle = "sage_starburst_feedback (process_per_event) requires "
               "sage_resolve_mergers_and_disruption as process_full_halo",
     .direct_init = sage_starburst_feedback_init},
    {.name = "per-event consumers without a merger producer (the event contract)",
     .pipeline = {OTHER_EV("sage_quasar_mode"), OTHER_EV("sage_starburst_feedback")},
     .outcome = REFUSED,
     .needle = "subscribes to producer 'sage_resolve_mergers_and_disruption'"},
    {.name = "starburst_feedback by galaxy without disk_instability",
     .pipeline = {GAL("sage_starburst_feedback")},
     .outcome = WARNS,
     .needle = "sage_disk_instability does not precede it"},
    {.name = "starburst_feedback with disk_instability but no quasar_mode consumer",
     .pipeline = {PRE("sage_initialise_merger_clock"), GAL("sage_disk_instability"),
                  OTHER_FH("sage_resolve_mergers_and_disruption"),
                  OTHER_EV("sage_starburst_feedback")},
     .outcome = WARNS,
     .needle = "sage_quasar_mode is absent as process_per_event"},
    {.name = "disk_instability after quasar_mode",
     .pipeline = {GAL("sage_quasar_mode"), GAL("sage_disk_instability")},
     .outcome = REFUSED,
     .refuser = "sage_disk_instability",
     .needle = "must precede sage_quasar_mode (process_by_galaxy)"},
    {.name = "disk_instability with quasar_mode only in another phase",
     .pipeline = {GAL("sage_disk_instability"), OTHER_BG("sage_quasar_mode")},
     .outcome = REFUSED,
     .refuser = "sage_disk_instability",
     .needle = "must precede sage_quasar_mode (process_by_galaxy)"},
    {.name = "disk_instability before a by-galaxy quasar_mode beside its per-event entry",
     .pipeline = {PRE("sage_initialise_merger_clock"),
                  GAL_FH("sage_resolve_mergers_and_disruption"), GAL_EV("sage_quasar_mode"),
                  GAL("sage_disk_instability"), GAL("sage_quasar_mode")},
     .outcome = ACCEPTED},
    {.name = "resolve_mergers_and_disruption without initialise_merger_clock",
     .pipeline = {OTHER_FH("sage_resolve_mergers_and_disruption")},
     .outcome = REFUSED,
     .refuser = "sage_resolve_mergers_and_disruption",
     .needle = "requires sage_initialise_merger_clock in pre_timestep"},
    {.name = "resolve_mergers_and_disruption before initialise_merger_clock",
     .pipeline = {PRE("sage_resolve_mergers_and_disruption"), PRE("sage_initialise_merger_clock")},
     .outcome = REFUSED,
     .refuser = "sage_resolve_mergers_and_disruption",
     .needle = "requires sage_initialise_merger_clock to precede it in pre_timestep"},
    {.name = "initialise_merger_clock before resolve_mergers_and_disruption in pre_timestep",
     .pipeline = {PRE("sage_initialise_merger_clock"), PRE("sage_resolve_mergers_and_disruption")},
     .outcome = ACCEPTED},
};

/** @brief Free the phase arrays of a pipeline whose init() never reached cleanup */
static void free_pipeline(void) {
  test_free_pre_timestep();
  test_free_substep_phases();
}

/** @brief Configure a row: the default parameters, the units and the row's pipeline */
static void configure(const struct dependency_case *row) {
  reset_config();
  for (size_t i = 0; i < COUNT(DEFAULTS); i++) {
    snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", DEFAULTS[i].name);
    snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", DEFAULTS[i].value);
  }
  MimicConfig.NumModelParams = (int)COUNT(DEFAULTS);
  MimicConfig.SubSteps = 1;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitLength_in_cm = 3.08568e24;
  MimicConfig.UnitMass_in_g = 1.989e43;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  for (const struct entry *e = row->pipeline; e->module != NULL; e++) {
    if (strcmp(e->phase, PRE_TIMESTEP) == 0) {
      test_pre_timestep_add(e->module, e->mode);
    } else {
      test_phase_add(e->phase, e->module, e->mode);
    }
  }
}

/**
 * @test    test_dependency_contracts
 * @brief   Every row's pipeline is refused by the intended module naming the intended partner,
 *          warns with the intended text, or initialises, as the table states
 */
int test_dependency_contracts(void) {
  for (size_t r = 0; r < COUNT(CASES); r++) {
    const struct dependency_case *row = &CASES[r];
    configure(row);

    FILE *capture = tmpfile();
    TEST_ASSERT(capture != NULL, "a log capture file must open");
    FILE *old_out = set_log_output(capture);
    const int status = row->direct_init != NULL ? row->direct_init() : module_system_init();
    set_log_output(old_out);
    char log[16384];
    snprintf(log, sizeof(log), "%s", read_captured_log(capture));
    fclose(capture);

    if (row->direct_init != NULL) {
      free_pipeline();
    } else {
      module_system_cleanup();
    }
    check_memory_leaks();

    char message[256];
    char refuser[160] = "";
    if (row->refuser != NULL) {
      snprintf(refuser, sizeof(refuser), "Module '%s' initialization failed", row->refuser);
    }
    printf("\n  %s: init %d", row->name, status);
    if (row->outcome == REFUSED) {
      snprintf(message, sizeof(message), "%s: init must fail", row->name);
      TEST_ASSERT(status != 0, message);
      if (row->refuser != NULL) {
        snprintf(message, sizeof(message), "%s: the intended module must refuse", row->name);
        TEST_ASSERT(strstr(log, refuser) != NULL, message);
      }
      snprintf(message, sizeof(message), "%s: the error must name the intended check", row->name);
      TEST_ASSERT(strstr(log, row->needle) != NULL, message);
    } else {
      snprintf(message, sizeof(message), "%s: init must succeed", row->name);
      TEST_ASSERT(status == 0, message);
      if (row->outcome == WARNS) {
        snprintf(message, sizeof(message), "%s: the warning must be logged", row->name);
        TEST_ASSERT(strstr(log, row->needle) != NULL, message);
      }
    }
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE dependency and placement contracts\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);
  ensure_modules_registered();

  TEST_RUN(test_dependency_contracts);

  TEST_SUMMARY();
  return TEST_RESULT();
}
