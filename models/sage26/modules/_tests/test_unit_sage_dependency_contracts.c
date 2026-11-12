/**
 * @file    test_unit_sage_dependency_contracts.c
 * @brief   Placement and partner contracts of the sage26 modules at initialisation
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
 * Every row runs on the package's full parameter set with CGMrecipeOn, EnhancedStarFormationOn,
 * ConcentrationOn and BulgeSizeOn at 0 (so the checks those switches add apply only to the rows
 * that turn them on), and a row sets any parameter it changes. A check is exercised through
 * module_system_init() wherever a pipeline reaches it. A few checks no pipeline reaches, so their
 * rows call the module's init() directly (the rows say "its own guard"): reionization's ordering
 * guard, always forestalled by the module it names; the per-event producer checks of the quasar and
 * starburst modules, forestalled by the core's event-contract validation (one row asserts that
 * refusal); and the placement counts of a module that is configured nowhere or in a processing mode
 * it does not support, which the core refuses before init() runs or never initialises. Parameter
 * refusals live in test_unit_sage26_parameter_refusals.c.
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
#include <stdlib.h>
#include <string.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_reionization_init(void);
extern int sage26_quasar_mode_init(void);
extern int sage26_starburst_feedback_init(void);
extern int sage26_determine_regime_init(void);
extern int sage26_determine_ffb_regime_init(void);
extern int sage26_ffb_star_formation_init(void);
extern int sage26_set_halo_concentration_init(void);
extern int sage26_initialise_merger_clock_init(void);
extern int sage26_finalize_cooling_heating_init(void);
extern int sage26_finalize_sfr_histories_init(void);
extern int sage26_record_transition_state_init(void);

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define MAX_ENTRIES 8
#define MAX_SET 2

/** @brief One pipeline entry: the phase it sits in, the module and its processing mode */
struct entry {
  const char *phase;
  const char *module;
  enum ProcessingMode mode;
};

#define PRE_TIMESTEP "pre_timestep"
#define POST_TIMESTEP "post_timestep"
#define SUBSTEP_PHASE "galaxy_physics"
#define OTHER_PHASE "satellite_mergers"

#define PRE(module) {PRE_TIMESTEP, module, PROCESSING_MODE_FULL_HALO}
#define POST(module) {POST_TIMESTEP, module, PROCESSING_MODE_FULL_HALO}
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
  const char *set[MAX_SET][2];            /* parameters the row changes: {name, value} */
  enum outcome outcome;
  const char *refuser;      /* REFUSED: the module whose check fires */
  const char *needle;       /* REFUSED or WARNS: wording the log must contain */
  int (*direct_init)(void); /* when set, call this init() instead of module_system_init() */
};

/* The full-physics run file's parameters with CGMrecipeOn, EnhancedStarFormationOn,
 * ConcentrationOn and BulgeSizeOn at 0, so the extra placement checks of those switches apply only
 * where a row turns them on */
static const char *const SWITCHES_OFF[][2] = {
    {"CGMrecipeOn", "0"},
    {"EnhancedStarFormationOn", "0"},
    {"ConcentrationOn", "0"},
    {"BulgeSizeOn", "0"},
};

static const struct dependency_case CASES[] = {
    /* Infall budget and reionization */
    {.name = "apply_infall without prepare_infall_budget",
     .pipeline = {GAL_FH("sage26_apply_infall")},
     .outcome = REFUSED,
     .refuser = "sage26_apply_infall",
     .needle = "requires sage26_prepare_infall_budget in pre_timestep"},
    {.name = "prepare_infall_budget before reionization",
     .pipeline = {PRE("sage26_prepare_infall_budget"), PRE("sage26_reionization")},
     .outcome = REFUSED,
     .refuser = "sage26_prepare_infall_budget",
     .needle = "requires sage26_reionization to run earlier in pre_timestep"},
    {.name = "reionization after prepare_infall_budget (its own guard)",
     .pipeline = {PRE("sage26_prepare_infall_budget"), PRE("sage26_reionization")},
     .outcome = REFUSED,
     .needle = "sage26_reionization must run before sage26_prepare_infall_budget",
     .direct_init = sage26_reionization_init},

    /* Cooling */
    {.name = "apply_cooling before calculate_cooling_budget",
     .pipeline = {GAL("sage26_apply_cooling"), GAL("sage26_calculate_cooling_budget")},
     .outcome = REFUSED,
     .refuser = "sage26_apply_cooling",
     .needle = "requires sage26_calculate_cooling_budget to precede it"},
    {.name = "radio_mode_heating before calculate_cooling_budget",
     .pipeline = {GAL("sage26_radio_mode_heating"), GAL("sage26_calculate_cooling_budget"),
                  GAL("sage26_apply_cooling")},
     .outcome = REFUSED,
     .refuser = "sage26_radio_mode_heating",
     .needle = "requires sage26_calculate_cooling_budget to precede it"},
    {.name = "radio_mode_heating after apply_cooling",
     .pipeline = {GAL("sage26_calculate_cooling_budget"), GAL("sage26_apply_cooling"),
                  GAL("sage26_radio_mode_heating")},
     .outcome = REFUSED,
     .refuser = "sage26_radio_mode_heating",
     .needle = "must precede sage26_apply_cooling"},
    {.name = "radio_mode_heating second entry in a phase without the budget",
     .pipeline = {GAL("sage26_calculate_cooling_budget"), GAL("sage26_radio_mode_heating"),
                  GAL("sage26_apply_cooling"), OTHER_BG("sage26_radio_mode_heating")},
     .outcome = REFUSED,
     .refuser = "sage26_radio_mode_heating",
     .needle = "to precede it in substep phase 'satellite_mergers'"},
    {.name = "radio_mode_heating alone",
     .pipeline = {GAL("sage26_radio_mode_heating")},
     .outcome = ACCEPTED},
    {.name = "CGMrecipeOn 1 budget without regime_aware_cooling",
     .pipeline = {PRE("sage26_determine_regime"), GAL("sage26_calculate_cooling_budget")},
     .set = {{"CGMrecipeOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_calculate_cooling_budget",
     .needle = "requires sage26_regime_aware_cooling (process_by_galaxy)"},
    {.name = "CGMrecipeOn 1 budget without determine_regime",
     .pipeline = {GAL("sage26_calculate_cooling_budget"), GAL("sage26_regime_aware_cooling")},
     .set = {{"CGMrecipeOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_calculate_cooling_budget",
     .needle = "requires sage26_determine_regime (process_full_halo) in pre_timestep"},
    {.name = "CGMrecipeOn 1 regime_aware_cooling not directly after apply_cooling",
     .pipeline = {PRE("sage26_determine_regime"), GAL("sage26_calculate_cooling_budget"),
                  GAL("sage26_regime_aware_cooling"), GAL("sage26_apply_cooling")},
     .set = {{"CGMrecipeOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_regime_aware_cooling",
     .needle = "must run immediately after sage26_apply_cooling"},
    {.name = "CGMrecipeOn 1 regime_aware_cooling twice",
     .pipeline = {PRE("sage26_determine_regime"), GAL("sage26_calculate_cooling_budget"),
                  GAL("sage26_apply_cooling"), GAL("sage26_regime_aware_cooling"),
                  GAL("sage26_regime_aware_cooling")},
     .set = {{"CGMrecipeOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_regime_aware_cooling",
     .needle = "found 2 substep-phase entries, 2 as process_by_galaxy"},
    {.name = "CGMrecipeOn 1 regime_aware_cooling also in post_timestep",
     .pipeline = {PRE("sage26_determine_regime"),
                  GAL("sage26_calculate_cooling_budget"),
                  GAL("sage26_apply_cooling"),
                  GAL("sage26_regime_aware_cooling"),
                  {POST_TIMESTEP, "sage26_regime_aware_cooling", PROCESSING_MODE_BY_GALAXY}},
     .set = {{"CGMrecipeOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_regime_aware_cooling",
     .needle = "0 in pre_timestep, 1 in post_timestep"},

    /* CGM regime classifier */
    {.name = "determine_regime twice in pre_timestep",
     .pipeline = {PRE("sage26_determine_regime"), PRE("sage26_determine_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_regime",
     .needle = "found 2 pre_timestep and 0 post_timestep entries"},
    {.name = "determine_regime also in post_timestep",
     .pipeline = {PRE("sage26_determine_regime"), POST("sage26_determine_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_regime",
     .needle = "found 1 pre_timestep and 1 post_timestep entries"},
    {.name = "determine_regime also in a substep phase",
     .pipeline = {PRE("sage26_determine_regime"), GAL_FH("sage26_determine_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_regime",
     .needle = "and a substep-phase entry"},
    {.name = "determine_regime after prepare_infall_budget",
     .pipeline = {PRE("sage26_prepare_infall_budget"), PRE("sage26_determine_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_regime",
     .needle = "requires to run before sage26_prepare_infall_budget"},

    /* Star formation and supernova feedback */
    {.name = "calculate_supernova_feedback before calculate_star_formation",
     .pipeline = {GAL("sage26_calculate_supernova_feedback"),
                  GAL("sage26_calculate_star_formation"),
                  GAL("sage26_apply_star_formation_supernova")},
     .outcome = REFUSED,
     .refuser = "sage26_calculate_supernova_feedback",
     .needle = "requires sage26_calculate_star_formation to precede it"},
    {.name = "apply_star_formation_supernova before calculate_star_formation",
     .pipeline = {GAL("sage26_apply_star_formation_supernova"),
                  GAL("sage26_calculate_star_formation")},
     .outcome = REFUSED,
     .refuser = "sage26_apply_star_formation_supernova",
     .needle = "requires sage26_calculate_star_formation to precede it"},
    {.name = "apply_star_formation_supernova before calculate_supernova_feedback",
     .pipeline = {GAL("sage26_apply_star_formation_supernova"),
                  GAL("sage26_calculate_supernova_feedback")},
     .outcome = REFUSED,
     .refuser = "sage26_apply_star_formation_supernova",
     .needle = "requires sage26_calculate_supernova_feedback to precede it"},
    {.name = "calculate_star_formation without apply_star_formation_supernova",
     .pipeline = {GAL("sage26_calculate_star_formation")},
     .outcome = REFUSED,
     .refuser = "sage26_calculate_star_formation",
     .needle = "requires sage26_apply_star_formation_supernova in the pipeline"},
    {.name = "calculate_supernova_feedback without apply_star_formation_supernova",
     .pipeline = {GAL("sage26_calculate_supernova_feedback")},
     .outcome = REFUSED,
     .refuser = "sage26_calculate_supernova_feedback",
     .needle = "requires sage26_apply_star_formation_supernova in the pipeline"},
    {.name = "apply_star_formation_supernova alone",
     .pipeline = {GAL("sage26_apply_star_formation_supernova")},
     .outcome = WARNS,
     .needle = "neither sage26_calculate_star_formation nor sage26_calculate_supernova_feedback "
               "is configured"},
    {.name = "apply_star_formation_supernova without apply_metal_enrichment",
     .pipeline = {GAL("sage26_calculate_star_formation"),
                  GAL("sage26_apply_star_formation_supernova")},
     .outcome = WARNS,
     .needle = "sage26_apply_metal_enrichment is not configured"},
    {.name = "apply_metal_enrichment before apply_star_formation_supernova",
     .pipeline = {GAL("sage26_calculate_star_formation"), GAL("sage26_apply_metal_enrichment"),
                  GAL("sage26_apply_star_formation_supernova")},
     .outcome = REFUSED,
     .refuser = "sage26_apply_metal_enrichment",
     .needle = "requires sage26_apply_star_formation_supernova to precede it"},
    {.name = "apply_metal_enrichment before starburst_feedback",
     .pipeline = {GAL("sage26_apply_metal_enrichment"), GAL("sage26_starburst_feedback")},
     .outcome = REFUSED,
     .refuser = "sage26_apply_metal_enrichment",
     .needle = "must run after sage26_starburst_feedback"},

    /* Mergers, quasar mode, starburst and disk instability */
    {.name = "quasar_mode per event without a merger producer (the module's own guard)",
     .pipeline = {OTHER_EV("sage26_quasar_mode")},
     .outcome = REFUSED,
     .needle = "sage26_quasar_mode (process_per_event) requires "
               "sage26_resolve_mergers_and_disruption as process_full_halo",
     .direct_init = sage26_quasar_mode_init},
    {.name = "starburst_feedback per event without a merger producer (the module's own guard)",
     .pipeline = {OTHER_EV("sage26_starburst_feedback")},
     .outcome = REFUSED,
     .needle = "sage26_starburst_feedback (process_per_event) requires "
               "sage26_resolve_mergers_and_disruption as process_full_halo",
     .direct_init = sage26_starburst_feedback_init},
    {.name = "per-event consumers without a merger producer (the event contract)",
     .pipeline = {OTHER_EV("sage26_quasar_mode"), OTHER_EV("sage26_starburst_feedback")},
     .outcome = REFUSED,
     .needle = "subscribes to producer 'sage26_resolve_mergers_and_disruption'"},
    {.name = "starburst_feedback by galaxy without disk_instability",
     .pipeline = {GAL("sage26_starburst_feedback")},
     .outcome = WARNS,
     .needle = "sage26_disk_instability does not precede it"},
    {.name = "starburst_feedback with disk_instability but no quasar_mode consumer",
     .pipeline = {PRE("sage26_initialise_merger_clock"), GAL("sage26_disk_instability"),
                  OTHER_FH("sage26_resolve_mergers_and_disruption"),
                  OTHER_EV("sage26_starburst_feedback")},
     .outcome = WARNS,
     .needle = "sage26_quasar_mode is absent as process_per_event"},
    {.name = "disk_instability after quasar_mode",
     .pipeline = {GAL("sage26_quasar_mode"), GAL("sage26_disk_instability")},
     .outcome = REFUSED,
     .refuser = "sage26_disk_instability",
     .needle = "must precede sage26_quasar_mode (process_by_galaxy)"},
    {.name = "disk_instability with quasar_mode only in another phase",
     .pipeline = {GAL("sage26_disk_instability"), OTHER_BG("sage26_quasar_mode")},
     .outcome = REFUSED,
     .refuser = "sage26_disk_instability",
     .needle = "must precede sage26_quasar_mode (process_by_galaxy)"},
    {.name = "disk_instability before a by-galaxy quasar_mode beside its per-event entry",
     .pipeline = {PRE("sage26_initialise_merger_clock"),
                  GAL_FH("sage26_resolve_mergers_and_disruption"), GAL_EV("sage26_quasar_mode"),
                  GAL("sage26_disk_instability"), GAL("sage26_quasar_mode")},
     .outcome = ACCEPTED},
    {.name = "resolve_mergers_and_disruption without initialise_merger_clock",
     .pipeline = {OTHER_FH("sage26_resolve_mergers_and_disruption")},
     .outcome = REFUSED,
     .refuser = "sage26_resolve_mergers_and_disruption",
     .needle = "requires sage26_initialise_merger_clock in pre_timestep"},
    {.name = "resolve_mergers_and_disruption before initialise_merger_clock",
     .pipeline = {PRE("sage26_resolve_mergers_and_disruption"),
                  PRE("sage26_initialise_merger_clock")},
     .outcome = REFUSED,
     .refuser = "sage26_resolve_mergers_and_disruption",
     .needle = "requires sage26_initialise_merger_clock to precede it in pre_timestep"},
    {.name = "merger consumers with starburst_feedback before quasar_mode",
     .pipeline = {PRE("sage26_initialise_merger_clock"),
                  OTHER_FH("sage26_resolve_mergers_and_disruption"),
                  OTHER_EV("sage26_starburst_feedback"), OTHER_EV("sage26_quasar_mode")},
     .outcome = REFUSED,
     .refuser = "sage26_resolve_mergers_and_disruption",
     .needle = "requires sage26_quasar_mode to precede sage26_starburst_feedback"},

    /* initialise_merger_clock */
    {.name = "initialise_merger_clock in a substep phase",
     .pipeline = {PRE("sage26_initialise_merger_clock"), GAL_FH("sage26_initialise_merger_clock")},
     .outcome = REFUSED,
     .refuser = "sage26_initialise_merger_clock",
     .needle = "is configured in substep phase 'galaxy_physics'"},
    {.name = "initialise_merger_clock also in post_timestep",
     .pipeline = {PRE("sage26_initialise_merger_clock"), POST("sage26_initialise_merger_clock")},
     .outcome = REFUSED,
     .refuser = "sage26_initialise_merger_clock",
     .needle = "is configured in modules.post_timestep"},
    {.name = "initialise_merger_clock twice in pre_timestep",
     .pipeline = {PRE("sage26_initialise_merger_clock"), PRE("sage26_initialise_merger_clock")},
     .outcome = REFUSED,
     .refuser = "sage26_initialise_merger_clock",
     .needle = "found 2 entries, 2 as process_full_halo"},
    {.name = "initialise_merger_clock without record_transition_state",
     .pipeline = {PRE("sage26_initialise_merger_clock")},
     .outcome = WARNS,
     .needle = "runs without sage26_record_transition_state in modules.post_timestep"},

    /* Halo concentration */
    {.name = "set_halo_concentration in a substep phase",
     .pipeline = {PRE("sage26_set_halo_concentration"), GAL_FH("sage26_set_halo_concentration")},
     .outcome = REFUSED,
     .refuser = "sage26_set_halo_concentration",
     .needle = "is configured in substep phase 'galaxy_physics'"},
    {.name = "set_halo_concentration also in post_timestep",
     .pipeline = {PRE("sage26_set_halo_concentration"), POST("sage26_set_halo_concentration")},
     .outcome = REFUSED,
     .refuser = "sage26_set_halo_concentration",
     .needle = "is configured in modules.post_timestep"},
    {.name = "set_halo_concentration twice in pre_timestep",
     .pipeline = {PRE("sage26_set_halo_concentration"), PRE("sage26_set_halo_concentration")},
     .outcome = REFUSED,
     .refuser = "sage26_set_halo_concentration",
     .needle = "found 2 entries, 2 as process_full_halo"},
    {.name = "ConcentrationOn 3 without record_transition_state",
     .pipeline = {PRE("sage26_set_halo_concentration")},
     .set = {{"ConcentrationOn", "3"}},
     .outcome = REFUSED,
     .refuser = "sage26_set_halo_concentration",
     .needle = "ConcentrationOn = 3 requires sage26_record_transition_state"},

    /* Feedback-free bursts */
    {.name = "determine_ffb_regime in a substep phase",
     .pipeline = {PRE("sage26_determine_ffb_regime"), GAL_FH("sage26_determine_ffb_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_ffb_regime",
     .needle = "is configured in substep phase 'galaxy_physics'"},
    {.name = "determine_ffb_regime also in post_timestep",
     .pipeline = {PRE("sage26_determine_ffb_regime"), POST("sage26_determine_ffb_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_ffb_regime",
     .needle = "is configured in modules.post_timestep"},
    {.name = "determine_ffb_regime twice in pre_timestep",
     .pipeline = {PRE("sage26_determine_ffb_regime"), PRE("sage26_determine_ffb_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_ffb_regime",
     .needle = "found 2 entries, 2 as process_full_halo"},
    {.name = "determine_ffb_regime before determine_regime",
     .pipeline = {PRE("sage26_determine_ffb_regime"), PRE("sage26_determine_regime")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_ffb_regime",
     .needle = "must run after sage26_determine_regime in modules.pre_timestep"},
    {.name = "determine_ffb_regime before prepare_infall_budget",
     .pipeline = {PRE("sage26_determine_ffb_regime"), PRE("sage26_prepare_infall_budget")},
     .outcome = REFUSED,
     .refuser = "sage26_determine_ffb_regime",
     .needle = "must run after sage26_prepare_infall_budget in modules.pre_timestep"},
    {.name = "EnhancedStarFormationOn 1 classifier without ffb_star_formation",
     .pipeline = {PRE("sage26_determine_ffb_regime")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_determine_ffb_regime",
     .needle = "requires sage26_ffb_star_formation in a substep phase"},
    {.name = "ffb_star_formation in pre_timestep",
     .pipeline = {{PRE_TIMESTEP, "sage26_ffb_star_formation", PROCESSING_MODE_BY_GALAXY}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "is configured in modules.pre_timestep"},
    {.name = "ffb_star_formation also in post_timestep",
     .pipeline = {GAL("sage26_ffb_star_formation"),
                  {POST_TIMESTEP, "sage26_ffb_star_formation", PROCESSING_MODE_BY_GALAXY}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "is configured in modules.post_timestep"},
    {.name = "ffb_star_formation twice in a substep phase",
     .pipeline = {GAL("sage26_ffb_star_formation"), GAL("sage26_ffb_star_formation")},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "found 2 substep entries, 2 as process_by_galaxy"},
    {.name = "EnhancedStarFormationOn 1 ffb_star_formation not before calculate_star_formation",
     .pipeline = {PRE("sage26_determine_ffb_regime"), GAL("sage26_ffb_star_formation")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "must run immediately before sage26_calculate_star_formation"},
    {.name = "EnhancedStarFormationOn 1 ffb_star_formation without determine_ffb_regime",
     .pipeline = {GAL("sage26_ffb_star_formation"), GAL("sage26_calculate_star_formation"),
                  GAL("sage26_apply_star_formation_supernova")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "requires sage26_determine_ffb_regime in modules.pre_timestep"},

    /* Finalisers and the transition-state recorder */
    {.name = "finalize_cooling_heating in pre_timestep",
     .pipeline = {PRE("sage26_finalize_cooling_heating")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_cooling_heating",
     .needle = "is configured in modules.pre_timestep"},
    {.name = "finalize_cooling_heating in a substep phase",
     .pipeline = {GAL_FH("sage26_finalize_cooling_heating")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_cooling_heating",
     .needle = "is configured in substep phase 'galaxy_physics'"},
    {.name = "finalize_cooling_heating twice in post_timestep",
     .pipeline = {POST("sage26_finalize_cooling_heating"), POST("sage26_finalize_cooling_heating")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_cooling_heating",
     .needle = "found 2 entries, 2 as process_full_halo"},
    {.name = "finalize_sfr_histories in pre_timestep",
     .pipeline = {PRE("sage26_finalize_sfr_histories")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_sfr_histories",
     .needle = "is configured in modules.pre_timestep"},
    {.name = "finalize_sfr_histories in a substep phase",
     .pipeline = {GAL_FH("sage26_finalize_sfr_histories")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_sfr_histories",
     .needle = "is configured in substep phase 'galaxy_physics'"},
    {.name = "finalize_sfr_histories twice in post_timestep",
     .pipeline = {POST("sage26_finalize_sfr_histories"), POST("sage26_finalize_sfr_histories")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_sfr_histories",
     .needle = "found 2 entries, 2 as process_full_halo"},
    {.name = "finalize_sfr_histories without initialise_merger_clock",
     .pipeline = {POST("sage26_finalize_sfr_histories")},
     .outcome = REFUSED,
     .refuser = "sage26_finalize_sfr_histories",
     .needle = "requires sage26_initialise_merger_clock in pre_timestep"},
    {.name = "record_transition_state in pre_timestep",
     .pipeline = {PRE("sage26_record_transition_state")},
     .outcome = REFUSED,
     .refuser = "sage26_record_transition_state",
     .needle = "is configured in modules.pre_timestep"},
    {.name = "record_transition_state in a substep phase",
     .pipeline = {GAL_FH("sage26_record_transition_state")},
     .outcome = REFUSED,
     .refuser = "sage26_record_transition_state",
     .needle = "is configured in substep phase 'galaxy_physics'"},
    {.name = "record_transition_state twice in post_timestep",
     .pipeline = {POST("sage26_record_transition_state"), POST("sage26_record_transition_state")},
     .outcome = REFUSED,
     .refuser = "sage26_record_transition_state",
     .needle = "found 2 entries, 2 as process_full_halo"},
    {.name = "record_transition_state not last in post_timestep",
     .pipeline = {POST("sage26_record_transition_state"), POST("sage26_finalize_cooling_heating")},
     .outcome = REFUSED,
     .refuser = "sage26_record_transition_state",
     .needle = "must be the last modules.post_timestep entry"},

    /* Feedback-free-burst dispatch at EnhancedStarFormationOn 1: the transaction immediately
     * before sage26_calculate_star_formation, in the same phase and as process_by_galaxy */
    {.name = "EnhancedStarFormationOn 1 ffb_star_formation after calculate_star_formation",
     .pipeline = {PRE("sage26_determine_ffb_regime"), GAL("sage26_calculate_star_formation"),
                  GAL("sage26_ffb_star_formation"), GAL("sage26_apply_star_formation_supernova")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "must run immediately before sage26_calculate_star_formation"},
    {.name = "EnhancedStarFormationOn 1 ffb_star_formation separated from calculate_star_formation",
     .pipeline = {PRE("sage26_determine_ffb_regime"), GAL("sage26_ffb_star_formation"),
                  GAL("sage26_satellite_stripping"), GAL("sage26_calculate_star_formation"),
                  GAL("sage26_apply_star_formation_supernova")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "must run immediately before sage26_calculate_star_formation"},
    {.name = "EnhancedStarFormationOn 1 ffb_star_formation with calculate_star_formation in "
             "another phase",
     .pipeline = {PRE("sage26_determine_ffb_regime"), GAL("sage26_ffb_star_formation"),
                  OTHER_BG("sage26_calculate_star_formation"),
                  OTHER_BG("sage26_apply_star_formation_supernova")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .refuser = "sage26_ffb_star_formation",
     .needle = "must run immediately before sage26_calculate_star_formation"},
    {.name = "ffb_star_formation separated from calculate_star_formation at "
             "EnhancedStarFormationOn 0",
     .pipeline = {GAL("sage26_ffb_star_formation"), GAL("sage26_satellite_stripping"),
                  GAL("sage26_calculate_star_formation"),
                  GAL("sage26_apply_star_formation_supernova")},
     .outcome = ACCEPTED},
    {.name = "ffb_star_formation after calculate_star_formation at EnhancedStarFormationOn 0",
     .pipeline = {GAL("sage26_calculate_star_formation"), GAL("sage26_ffb_star_formation"),
                  GAL("sage26_apply_star_formation_supernova")},
     .outcome = ACCEPTED},
    {.name = "ffb_star_formation with calculate_star_formation in another phase at "
             "EnhancedStarFormationOn 0",
     .pipeline = {GAL("sage26_ffb_star_formation"), OTHER_BG("sage26_calculate_star_formation"),
                  OTHER_BG("sage26_apply_star_formation_supernova")},
     .outcome = ACCEPTED},
    {.name = "ffb_star_formation alone at EnhancedStarFormationOn 0",
     .pipeline = {GAL("sage26_ffb_star_formation")},
     .outcome = ACCEPTED},
    {.name = "ffb_star_formation with calculate_star_formation as process_full_halo at "
             "EnhancedStarFormationOn 0 (its own guard)",
     .pipeline = {GAL("sage26_ffb_star_formation"), GAL_FH("sage26_calculate_star_formation")},
     .outcome = ACCEPTED,
     .direct_init = sage26_ffb_star_formation_init},

    /* Placement checks no pipeline reaches: init() called directly. A module whose entry is
     * missing is not in a pipeline to be initialised, and a processing mode the module does not
     * support is refused by the core's mode validation before init() runs */
    {.name = "determine_regime configured nowhere (its own guard)",
     .outcome = REFUSED,
     .needle = "found 0 pre_timestep and 0 post_timestep entries",
     .direct_init = sage26_determine_regime_init},
    {.name = "determine_ffb_regime in pre_timestep as process_by_galaxy (its own guard)",
     .pipeline = {{PRE_TIMESTEP, "sage26_determine_ffb_regime", PROCESSING_MODE_BY_GALAXY}},
     .outcome = REFUSED,
     .needle = "found 1 entries, 0 as process_full_halo",
     .direct_init = sage26_determine_ffb_regime_init},
    {.name = "ffb_star_formation as process_full_halo in a substep phase (its own guard)",
     .pipeline = {GAL_FH("sage26_ffb_star_formation")},
     .outcome = REFUSED,
     .needle = "found 1 substep entries, 0 as process_by_galaxy",
     .direct_init = sage26_ffb_star_formation_init},
    {.name = "EnhancedStarFormationOn 1 ffb_star_formation with calculate_star_formation as "
             "process_full_halo (its own guard)",
     .pipeline = {PRE("sage26_determine_ffb_regime"), GAL("sage26_ffb_star_formation"),
                  GAL_FH("sage26_calculate_star_formation")},
     .set = {{"EnhancedStarFormationOn", "1"}},
     .outcome = REFUSED,
     .needle = "must run immediately before sage26_calculate_star_formation",
     .direct_init = sage26_ffb_star_formation_init},
    {.name = "set_halo_concentration configured nowhere (its own guard)",
     .outcome = REFUSED,
     .needle = "found 0 entries, 0 as process_full_halo",
     .direct_init = sage26_set_halo_concentration_init},
    {.name = "set_halo_concentration in pre_timestep as process_by_galaxy (its own guard)",
     .pipeline = {{PRE_TIMESTEP, "sage26_set_halo_concentration", PROCESSING_MODE_BY_GALAXY}},
     .outcome = REFUSED,
     .needle = "found 1 entries, 0 as process_full_halo",
     .direct_init = sage26_set_halo_concentration_init},
    {.name = "ConcentrationOn 3 with record_transition_state as process_by_galaxy (its own guard)",
     .pipeline = {PRE("sage26_set_halo_concentration"),
                  {POST_TIMESTEP, "sage26_record_transition_state", PROCESSING_MODE_BY_GALAXY}},
     .set = {{"ConcentrationOn", "3"}},
     .outcome = REFUSED,
     .needle = "ConcentrationOn = 3 requires sage26_record_transition_state",
     .direct_init = sage26_set_halo_concentration_init},
    {.name = "initialise_merger_clock configured nowhere (its own guard)",
     .outcome = REFUSED,
     .needle = "found 0 entries, 0 as process_full_halo",
     .direct_init = sage26_initialise_merger_clock_init},
    {.name = "finalize_cooling_heating configured nowhere (its own guard)",
     .outcome = REFUSED,
     .needle = "found 0 entries, 0 as process_full_halo",
     .direct_init = sage26_finalize_cooling_heating_init},
    {.name = "finalize_sfr_histories configured nowhere (its own guard)",
     .outcome = REFUSED,
     .needle = "found 0 entries, 0 as process_full_halo",
     .direct_init = sage26_finalize_sfr_histories_init},
    {.name = "finalize_sfr_histories with initialise_merger_clock as process_by_galaxy (its own "
             "guard)",
     .pipeline = {{PRE_TIMESTEP, "sage26_initialise_merger_clock", PROCESSING_MODE_BY_GALAXY},
                  POST("sage26_finalize_sfr_histories")},
     .outcome = REFUSED,
     .needle = "requires sage26_initialise_merger_clock in pre_timestep",
     .direct_init = sage26_finalize_sfr_histories_init},
    {.name = "finalize_sfr_histories once in post_timestep as process_by_galaxy (its own guard)",
     .pipeline = {PRE("sage26_initialise_merger_clock"),
                  {POST_TIMESTEP, "sage26_finalize_sfr_histories", PROCESSING_MODE_BY_GALAXY}},
     .outcome = REFUSED,
     .needle = "found 1 entries, 0 as process_full_halo",
     .direct_init = sage26_finalize_sfr_histories_init},
    {.name = "record_transition_state configured nowhere (its own guard)",
     .outcome = REFUSED,
     .needle = "found 0 entries, 0 as process_full_halo",
     .direct_init = sage26_record_transition_state_init},

    /* Configurations a guard must not refuse */
    {.name = "finalize_sfr_histories before finalize_cooling_heating",
     .pipeline = {PRE("sage26_initialise_merger_clock"), POST("sage26_finalize_sfr_histories"),
                  POST("sage26_finalize_cooling_heating")},
     .outcome = ACCEPTED},
    {.name = "set_halo_concentration without record_transition_state at ConcentrationOn 0",
     .pipeline = {PRE("sage26_set_halo_concentration")},
     .outcome = ACCEPTED},
    {.name = "regime_aware_cooling misplaced and without determine_regime at CGMrecipeOn 0",
     .pipeline = {GAL("sage26_calculate_cooling_budget"), GAL("sage26_regime_aware_cooling"),
                  GAL("sage26_apply_cooling")},
     .outcome = ACCEPTED},
    {.name = "regime_aware_cooling placed but without determine_regime at CGMrecipeOn 0",
     .pipeline = {GAL("sage26_calculate_cooling_budget"), GAL("sage26_apply_cooling"),
                  GAL("sage26_regime_aware_cooling")},
     .outcome = ACCEPTED},
    {.name = "budget and apply_cooling without regime_aware_cooling at CGMrecipeOn 0",
     .pipeline = {GAL("sage26_calculate_cooling_budget"), GAL("sage26_apply_cooling")},
     .outcome = ACCEPTED},
};

/** @brief Free the three phase arrays of a pipeline whose init() never reached cleanup */
static void free_pipeline(void) {
  test_free_pre_timestep();
  test_free_substep_phases();
  test_free_post_timestep();
}

/** @brief Configure a row: the default parameters with the row's changes, units, the pipeline */
static void configure(const struct dependency_case *row) {
  reset_config();
  sage26_test_load_run_file_parameters(SWITCHES_OFF, COUNT(SWITCHES_OFF));
  for (int s = 0; s < MAX_SET && row->set[s][0] != NULL; s++) {
    for (int i = 0; i < MimicConfig.NumModelParams; i++) {
      if (strcmp(MimicConfig.ModelParams[i].param_name, row->set[s][0]) == 0) {
        snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", row->set[s][1]);
      }
    }
  }
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
    } else if (strcmp(e->phase, POST_TIMESTEP) == 0) {
      test_post_timestep_add(e->module, e->mode);
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
  printf("Test Suite: SAGE26 dependency and placement contracts\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);
  ensure_modules_registered();

  TEST_RUN(test_dependency_contracts);

  TEST_SUMMARY();
  return TEST_RESULT();
}
