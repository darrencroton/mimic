/**
 * @file    test_unit_sage26_resolve_mergers_and_disruption_oracle.c
 * @brief   Bit-exact comparison of the merger pass and its transactions with SAGE26's
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_resolve_mergers_and_disruption (its _tests/oracle/ directory), with R0's flags
 * (M = R/src/model_mergers.c, B = R/src/core_build_model.c):
 *
 * - add_galaxies_together (M:456-543, add_*) and make_bulge_from_burst (M:553-570, mbb_*):
 *   the shared helpers mimic_sage_add_galaxies_together and mimic_sage_make_bulge_from_burst
 *   (shared/sage_merger_physics.h), called directly at SAGE26's routine boundary. The c07
 *   add_s3_* fixtures run add_galaxies_together with BulgeSizeOn 3 (R1.par:109) and the
 *   fixture's Hubble_h, so the satellite disk stars credited to the instability channel
 *   update the target's instability radius (M:516-522); every other fixture has R0's 0;
 * - deal_with_galaxy_merger (M:228-333, merge_*): the resolver's whole transaction,
 *   sage26_resolve_mergers_and_disruption_merge, with the fixture's call.time and call.dt,
 *   whose merger event runs the module's real consumers in the order of the production
 *   phase: sage26_quasar_mode's and then sage26_starburst_feedback's process_per_event
 *   entry, with the event the registry would hand them (target halo, ratio, satellite dt),
 *   then the resolver's completion. Zero-ratio fixtures are included. The c07 merge_s3_*
 *   fixtures run it with BulgeSizeOn 3 (R1.par:109): the target's bulge-radius
 *   synchronisation after the burst, the remnant radius (M:135-212, the oracle's checked
 *   transcription of the static routine) with its locals and every branch, and the major,
 *   disk-dominated minor and spheroid-dominated minor assignments (M:293-330);
 * - disrupt_satellite_to_ICS (M:845-902, dis_*): the resolver's disruption entry,
 *   sage26_resolve_mergers_and_disruption_disrupt, with the fixture's call.time, at
 *   SAGE26's routine boundary: SAGE26's mergeType 4 is the package's Type 3 with the
 *   action hook's "disrupt" marker;
 * - the per-substep pass (B:519-587, tr_pass_*): the resolver's pass,
 *   sage26_resolve_mergers_and_disruption_pass, on the fixture's galaxies at substep
 *   call.step of call.effective_steps, with each row's dT = call.Age[SnapNum] -
 *   call.halo_age (the core's dT is that difference) and ctx->time = call.halo_age, its
 *   mergers through the consumers as above. Each row's pass record is asserted against
 *   SAGE26's driver locals (intermediate.pass.galaxy[i].*), and the merger events against
 *   the pass's order (tr_pass_two_mergers_one_bin holds two mergers in one bin);
 * - joining followed by the pass (B:210-372 then B:519-587, tr_jp_*): the core's
 *   inheritance and the package's joining modules (modules/_tests/sage26_oracle_join.h:
 *   sage26_initialise_merger_clock, which zeroes the histories of the rows SAGE26 resets
 *   and leaves a new orphan's, then sage26_set_disk_scale_radius), then the pass; the
 *   joined state (intermediate.join.galaxy[*]) and the end state (galaxy[*]) are asserted,
 *   including a new orphan's previous histories moved into the central's bulge histories
 *   by its merger (tr_jp_type0_orphan_history_merge) or kept through its disruption
 *   (tr_jp_type1_orphan_history_disrupt).
 *
 * The test configures the phases of the production run file (sage26_disk_instability in a
 * galaxy-physics phase, so the burst runs SAGE26's post-minor-merger recheck, and the
 * resolver, the quasar consumer and the burst consumer in that order in a merger phase)
 * and initialises the three modules from the fixture's parameters, so the consumers run
 * exactly as configured in production. The burst module derives its supernova constants
 * from the unit factors and Hubble_h 0.73 (R0.par); the test requires them to equal the
 * fixture's EnergySNcode and EtaSNcode bit for bit.
 *
 * Asserted bit for bit: the thirty floats of every recorded galaxy block, the sixty history
 * elements wherever the output records them, and the clock wherever it records it; every
 * other galaxy member unchanged by a transaction or the pass (the transport field
 * MergerDiskDominated aside, which carries the pre-transfer flag to the burst); the halo
 * fields unchanged except a removed satellite's Type, which becomes 3 exactly when SAGE26's
 * mergeType is set (2 major, 1 minor, 4 disruption, against the port's split and action);
 * each recorded row's SnapNum, Len, CentralGal (the package's CentralHalo) and float halo
 * values; the merger's and the disruption's own flags; is_disk_dominated and the burst
 * channel it selects; and each carried value where the oracle validates it two-sided.
 *
 * Not asserted, each named: SAGE26's driver bookkeeping mergeIntoID (B:545, no package
 * member: Mimic's output writes no merger target) and dT after joining (SAGE26's -1 of
 * B:237, which its physics loop replaces at B:483 and the pass never reads); the
 * black-hole, wind and burst locals of the merge fixtures, which the consumers do not
 * expose (their effect is the asserted end state, and the quasar and burst oracle tests
 * assert those locals at their own routine boundary); old_disk_radius, read on R0 only by
 * update_instability_bulge_radius, which returns at once (R/src/model_misc.c:395); and
 * new_merger_radius, which the port does not form because get_bulge_radius zeroes every
 * radius after it (M:293-331, R/src/model_misc.c:241-246): the test asserts that
 * inertness, the three radii zero in SAGE26's own output wherever the remnant radius it
 * computed was not. Those two hold for BulgeSizeOn 0 only: with BulgeSizeOn 3 both are
 * compared bit for bit, with the remnant radius's locals, as narrowing-limited values (one
 * float store is the only path of each to the state). For a galaxy that joining makes an orphan,
 * SAGE26 keeps its Len, its QuasarModeBHaccretionMass and OutflowRate and its Cooling and Heating
 * (reset only on the main branch, B:261, :273-276); the core zeros the orphan's Len
 * (src/core/inheritance.c make_orphan) and init_repeat resets the other four on every inherited
 * row, so they are excluded on that row only, by name, and counted.
 *
 * SFprescription 1 (the c08 merge_r2_* and tr_pass_r2_* fixtures, R2's flags): the three modules
 * read the fixture's SFprescription, so the merger's black-hole growth caps the hydrogen
 * (M:366-372), the burst's commits cap it and the burst refreshes it (M:798-803), the
 * post-minor-merger recheck included; the end state, H2gas and H1gas among the thirty floats, is
 * asserted bit for bit. The merger's black-hole cap is end-state invariant (the burst's commit
 * re-caps at a bound never looser and its refresh overwrites H1gas), so its record is asserted:
 * beside the quasar consumer's process() the test runs the shared growth on a copy of the target
 * with the fixture's SFprescription, requires the copy to equal the consumer's result bit for
 * bit (so the consumer's growth received the same SFprescription and arguments), and asserts that
 * shadow's cap locals against the oracle's bh_* record and max_h_bh, as the quasar oracle test
 * does at its routine boundary. The rest of the c08 record (the burst's caps and refresh, and the
 * observation of the recheck's growth) is consumed, not asserted, as the consumers' other locals:
 * the burst oracle test asserts it against the shared burst's locals.
 *
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports
 * SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "module_system/generated/event_contracts.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fire.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_hydrogen.h"
#include "modules/_tests/sage26_oracle_sizes.h"
#include "modules/_tests/sage26_oracle_join.h"
#include "modules/sage26_resolve_mergers_and_disruption/sage_merger_ops.h"
#include "shared/central_link.h"
#include "shared/sage_sf_feedback_physics.h"

extern int sage26_resolve_mergers_and_disruption_init(void);
extern int sage26_resolve_mergers_and_disruption_cleanup(void);
extern void sage26_resolve_mergers_and_disruption_set_action_hook(
    void (*hook)(const char *action, int source_index, int target_index, double mass_ratio));
extern int sage26_initialise_merger_clock_init(void);
extern int sage26_initialise_merger_clock_cleanup(void);
extern int sage26_quasar_mode_init(void);
extern int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_quasar_mode_cleanup(void);
extern int sage26_starburst_feedback_init(void);
extern int sage26_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_starburst_feedback_cleanup(void);

static const char *MODULE = "sage26_resolve_mergers_and_disruption";

#define MAX_GALAXIES 4
#define MAX_EVENTS 8
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* SAGE26's STEPS and the substep count of the transaction fixtures' bins: with ten
 * substeps the bin of substep k is k (R/src/core_build_model.c:514-515) */
#define FIXTURE_SUBSTEPS 10

/* SAGE26's TOOMRE_DISK_FACTOR (R/src/model_disk_instability.c:35), the recheck's factor */
#define SAGE26_TOOMRE_DISK_FACTOR 3.0

/* SAGE26's R0 unit factors and Hubble_h (R0.par:40, :106-107), from which the burst module
 * derives EnergySNcode and EtaSNcode (R/src/core_init.c:102-103) */
#define SAGE26_UNIT_LENGTH_IN_CM 3.08568e+24
#define SAGE26_UNIT_MASS_IN_G 1.989e+43
#define SAGE26_HUBBLE_H 0.73

/* The resolver's ThresholdSatDisruption, read by its init only (the pass's gate, R0.par:79);
 * no transaction fixture carries it */
#define SAGE26_THRESHOLD_SAT_DISRUPTION 1.0

/* Carried-value check kinds (oracle README "Carried Values, Check Kinds") */
#define KIND_CONTINUATION 1
#define KIND_TAIL 2

static struct oracle_c06_carried_tally tally;
static int counts_add = 0, counts_mbb = 0, counts_merge = 0, counts_multi = 0;
/* add_galaxies_together fixtures run with BulgeSizeOn 3 (c07) */
static int counts_add_sizes = 0;
static int counts_ratio_zero = 0, counts_inert_radius = 0;
/* deal_with_galaxy_merger fixtures run with BulgeSizeOn 3 (c07 Slice 3), per assignment branch
 * (0 major, 1 minor onto a disk-dominated target, 2 minor onto a spheroid-dominated one), and
 * per remnant-radius branch taken (the order of REMNANT_FLAGS) */
static int counts_merge_sizes = 0;
static int counts_merge_sizes_branch[3] = {0, 0, 0};
#define REMNANT_BRANCHES 7
static int counts_remnant_branch[REMNANT_BRANCHES];
static struct oracle_c07_local_tally remnant_tally;
static int counts_consumer_locals_validated = 0;
static int counts_disrupt = 0, counts_pass = 0, counts_join_and_pass = 0;
static int counts_pass_mergers = 0, counts_pass_disruptions = 0, counts_pass_redirects = 0;
static int counts_orphan_exclusions = 0;
/* c08: merge and pass fixtures run with SFprescription 1 */
static int counts_br06 = 0;
/* c08: the fixture's SFprescription and growth parameters, for the shadow of the quasar
 * consumer's merger growth (dispatch_merger_event), and that shadow's locals */
static int fixture_sf_prescription = 0;
static double fixture_black_hole_growth_rate = 0.0;
static double fixture_quasar_mode_efficiency = 0.0;
static struct MimicSageBlackHoleLocals shadow_bh;
static struct oracle_c08_local_tally bh_h_tally;
static int counts_merge_bh_caps = 0;

/* c09: a deal_with_galaxy_merger_fire or merger_pass_fire fixture, its FIREmodeOn, stored
 * snapshots and input, for the replay of the burst consumer's call (dispatch_merger_event); the
 * last replay, its problems, and the counts of the c09 assertions */
static int fixture_fire = 0;
static int fixture_fire_mode_on = 0;
static int fixture_ngal = 0;
static int fixture_snapnum[MAX_GALAXIES];
static struct MimicSageBulgeSize fixture_sizes;
static struct oracle_fixture *fixture_input = NULL;
static const char *fixture_name = NULL;
static struct oracle_c09_burst_replay last_burst_replay;
static int burst_replay_problems = 0;
static struct oracle_c09_tally fire_tally;
static int counts_fire_merge = 0, counts_fire_pass = 0;
static int counts_merger_wiring = 0, counts_recheck_wiring = 0, counts_pass_observed = 0;
static int counts_mass_loading = 0;

/* The removals the resolver's action hook reported, in order */
static int action_count = 0;
static int action_source[MAX_EVENTS];
static int action_is_disruption[MAX_EVENTS];

/* The merger events the test's sink delivered, in order */
static int event_count = 0;
static int event_source[MAX_EVENTS];
static int event_target[MAX_EVENTS];

// ============================================================================
// SETUP
// ============================================================================

/** @brief Consume every input field whose name starts with `prefix` (applied elsewhere) */
static void consume_prefix(struct oracle_fixture *fx, const char *prefix) {
  const size_t n = strlen(prefix);
  for (int i = 0; i < fx->num_fields; i++) {
    if (strncmp(fx->fields[i].name, prefix, n) == 0) {
      fx->fields[i].consumed = 1;
    }
  }
}

/**
 * @brief Consume one carried value's whole record without asserting it
 *
 * "intermediate.<name>" and its markers (_absent, _checked, _check_kind, _validated_up,
 * _validated_down, _validated, _validated_flip).
 *
 * @return 1 if the record says the value was validated two-sided, else 0
 */
static int consume_record(struct oracle_fixture *out, const char *name) {
  static const char *const suffixes[] = {
      "",           "_absent",        "_checked", "_check_kind", "_validated_up", "_validated_down",
      "_validated", "_validated_flip"};
  int validated = 0;
  for (size_t k = 0; k < COUNT(suffixes); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s%s", name, suffixes[k]);
    for (int i = 0; i < out->num_fields; i++) {
      if (strcmp(out->fields[i].name, field) == 0) {
        out->fields[i].consumed = 1;
        if (strcmp(suffixes[k], "_validated") == 0 && out->fields[i].ivalue == 1) {
          validated = 1;
        }
      }
    }
  }
  return validated;
}

/** @brief The resolver's action hook: records each removal the transactions report */
static void observe_action(const char *action, int source_index, int target_index,
                           double mass_ratio) {
  (void)target_index;
  (void)mass_ratio;
  if (action_count < MAX_EVENTS) {
    action_source[action_count] = source_index;
    action_is_disruption[action_count] = (strcmp(action, "disrupt") == 0);
  }
  action_count++;
}

/**
 * @brief Apply the fixture's parameters and initialise the resolver and its consumers
 * @return Number of problems
 */
static int setup_modules(struct oracle_fixture *in) {
  /* c06's flags with BulgeSizeOn read (0, or 3 for the c07 merge_s3_* fixtures, with the
   * fixture's Hubble_h) and SFprescription read (0, or 1 for the c08 fixtures, with the H2
   * configuration) */
  struct MimicSageBulgeSize sizes;
  int sf_prescription = 0;
  /* FIREmodeOn: 0, or 0 or 1 for a c09 fixture (validated by oracle_c09_read_fire_config) */
  int problems = fixture_fire ? oracle_c09_require_merger_flags(in, &sizes, &sf_prescription)
                              : oracle_c08_require_merger_flags(in, &sizes, &sf_prescription);
  fixture_fire_mode_on = fixture_fire ? oracle_in_i32(in, "params.FIREmodeOn") : 0;
  fixture_sizes = sizes;
  if (sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    counts_br06++;
  }
  fixture_sf_prescription = sf_prescription;
  if (sizes.bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI &&
      oracle_f64_to_bits(sizes.hubble_h) != oracle_f64_to_bits(SAGE26_HUBBLE_H)) {
    fprintf(stderr, "oracle: %s: params.Hubble_h differs from R1.par's 0.73\n", in->path);
    problems++;
  }

  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.Hubble_h = SAGE26_HUBBLE_H;
  MimicConfig.UnitLength_in_cm = SAGE26_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = SAGE26_UNIT_MASS_IN_G;
  MimicConfig.UnitVelocity_in_cm_per_s = oracle_in_f64(in, "params.UnitVelocity_in_cm_per_s");
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g *
                                  (MimicConfig.UnitLength_in_cm * MimicConfig.UnitLength_in_cm) /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  const double energy_sn_code =
      mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
  const double eta_sn_code =
      mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);
  if (oracle_f64_to_bits(energy_sn_code) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EnergySNcode")) ||
      oracle_f64_to_bits(eta_sn_code) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EtaSNcode"))) {
    fprintf(stderr, "oracle: %s: the package's EnergySNcode or EtaSNcode differs from SAGE26's\n",
            in->path);
    problems++;
  }

  // SAGE26's names and the package's (ThreshMajorMerger is ThresholdMajorMerger)
  const struct {
    const char *sage26;
    const char *package;
  } params[] = {{"FeedbackReheatingEpsilon", "FeedbackReheatingEpsilon"},
                {"FeedbackEjectionEfficiency", "FeedbackEjectionEfficiency"},
                {"RecycleFraction", "RecycleFraction"},
                {"Yield", "Yield"},
                {"FracZleaveDisk", "FracZleaveDisk"},
                {"ThreshMajorMerger", "ThresholdMajorMerger"},
                {"BlackHoleGrowthRate", "BlackHoleGrowthRate"},
                {"QuasarModeEfficiency", "QuasarModeEfficiency"}};
  for (size_t k = 0; k < COUNT(params); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", params[k].sage26);
    const double value = oracle_in_f64(in, field);
    if (oracle_set_model_param(params[k].package, value) != 0) {
      problems++;
    }
    if (strcmp(params[k].package, "BlackHoleGrowthRate") == 0) {
      fixture_black_hole_growth_rate = value;
    } else if (strcmp(params[k].package, "QuasarModeEfficiency") == 0) {
      fixture_quasar_mode_efficiency = value;
    }
  }
  const double threshold_sat_disruption = oracle_has(in, "params.ThresholdSatDisruption")
                                              ? oracle_in_f64(in, "params.ThresholdSatDisruption")
                                              : SAGE26_THRESHOLD_SAT_DISRUPTION;
  /* BulgeSizeOn: the fixture's, which the three modules read (their Hubble_h is the core's,
   * MimicConfig.Hubble_h, R1.par's 0.73 as above) */
  if (oracle_set_model_param("StarFormingDiskFactor", SAGE26_TOOMRE_DISK_FACTOR) != 0 ||
      oracle_set_model_param("ThresholdSatDisruption", threshold_sat_disruption) != 0 ||
      oracle_set_model_param("BulgeSizeOn", (double)sizes.bulge_size_on) != 0) {
    problems++;
  }
  /* sage26_quasar_mode's and sage26_starburst_feedback's required SFprescription, the fixture's
   * (0: the hydrogen caps and refresh off; 1, R2: on), and the burst's StarburstColdGasOn 1
   * (cold-gas bursts) */
  if (oracle_set_model_param("SFprescription", (double)sf_prescription) != 0 ||
      oracle_set_model_param("StarburstColdGasOn", 1.0) != 0) {
    problems++;
  }
  /* The burst consumer's required FIREmodeOn (campaign decision D33): the fixture's, 0 for every
   * fixture before c09 */
  if (oracle_set_model_param("FIREmodeOn", (double)fixture_fire_mode_on) != 0) {
    problems++;
  }

  if (sage26_resolve_mergers_and_disruption_init() != 0 || sage26_quasar_mode_init() != 0 ||
      sage26_starburst_feedback_init() != 0) {
    fprintf(stderr, "oracle: %s: the resolver or its consumers failed to initialise\n", in->path);
    problems++;
  }
  sage26_resolve_mergers_and_disruption_set_action_hook(observe_action);
  return problems;
}

static void cleanup_modules(void) {
  sage26_starburst_feedback_cleanup();
  sage26_quasar_mode_cleanup();
  sage26_resolve_mergers_and_disruption_cleanup();
}

/**
 * @brief The test's merger-event sink: the registry's immediate dispatch, by hand
 *
 * Hands the target halo and the event to the process_per_event consumers in the
 * production phase's order (module_registry.c dispatch_events_range): the quasar consumer,
 * then the burst consumer. Beside the quasar consumer it runs the shared growth
 * (mimic_apply_black_hole_growth) on a copy of the target with the fixture's SFprescription and
 * growth parameters, keeping its locals in shadow_bh, and fails unless the copy equals the
 * consumer's result bit for bit.
 */
static int dispatch_merger_event(struct ModuleContext *ctx, struct Halo *halos, int source_idx,
                                 int target_idx, double mass_ratio, double source_dt) {
  struct ModuleEvent event;
  memset(&event, 0, sizeof(event));
  event.event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER;
  event.source_index = source_idx;
  event.target_index = target_idx;
  event.value0 = mass_ratio;
  event.value1 = source_dt;
  if (event_count < MAX_EVENTS) {
    event_source[event_count] = source_idx;
    event_target[event_count] = target_idx;
  }
  event_count++;

  ctx->active_event = &event;
  struct Halo shadow_halo;
  struct GalaxyData shadow_gal;
  memcpy(&shadow_halo, &halos[target_idx], sizeof(shadow_halo));
  if (halos[target_idx].galaxy != NULL) {
    memcpy(&shadow_gal, halos[target_idx].galaxy, sizeof(shadow_gal));
  }
  shadow_halo.galaxy = &shadow_gal;
  int status = sage26_quasar_mode_process(ctx, &halos[target_idx], 1);
  if (status == 0 && halos[target_idx].galaxy != NULL) {
    (void)mimic_apply_black_hole_growth(&shadow_halo, mass_ratio, fixture_black_hole_growth_rate,
                                        fixture_quasar_mode_efficiency, ctx,
                                        fixture_sf_prescription, &shadow_bh);
    if (memcmp(&shadow_gal, halos[target_idx].galaxy, sizeof(shadow_gal)) != 0) {
      fprintf(stderr,
              "oracle: the quasar consumer's merger growth differs from the shared growth with "
              "the fixture's SFprescription %d\n",
              fixture_sf_prescription);
      status = -1;
    }
  }
  /* c09: the burst consumer's call replayed on copies, for its FIRE record (the c08 pattern) */
  const int replay = fixture_fire && status == 0 && fixture_ngal <= ORACLE_C09_REPLAY_GALAXIES &&
                     ctx->central_galaxy != NULL;
  if (replay) {
    struct GalaxyData copies[ORACLE_C09_REPLAY_GALAXIES];
    for (int i = 0; i < fixture_ngal; i++) {
      if (halos[i].galaxy != NULL) {
        copies[i] = *halos[i].galaxy;
      } else {
        oracle_poison(&copies[i], sizeof(copies[i]));
      }
    }
    struct MimicStarburstParams bundle;
    oracle_c09_burst_bundle(fixture_input, &fixture_sizes, fixture_sf_prescription,
                            fixture_fire_mode_on, 1, &bundle);
    oracle_c09_replay_merger_burst(&last_burst_replay, copies, halos, fixture_ngal, target_idx,
                                   (int)(ctx->central_galaxy - halos), ctx, &bundle);
  }
  if (status == 0) {
    status = sage26_starburst_feedback_process(ctx, &halos[target_idx], 1);
  }
  if (replay) {
    /* The replay ends in the consumer's state, and its burst read the TARGET's own stored
     * SnapNum, float Vvir and redshift (the fixture's), never the satellite's or the central's */
    int mismatch = last_burst_replay.status != 0;
    for (int i = 0; i < fixture_ngal && !mismatch; i++) {
      mismatch = halos[i].galaxy != NULL && memcmp(&last_burst_replay.galaxies[i], halos[i].galaxy,
                                                   sizeof(struct GalaxyData)) != 0;
    }
    if (mismatch) {
      fprintf(stderr, "oracle: %s: the merger-burst replay differs from the consumer\n",
              fixture_name);
      burst_replay_problems++;
    }
    burst_replay_problems += oracle_c09_check_burst_wiring(
        fixture_name, "merger burst", &last_burst_replay.locals.fire, fixture_fire_mode_on,
        fixture_snapnum[target_idx], (float)halos[target_idx].Vvir);
    counts_merger_wiring++;
  }
  ctx->active_event = NULL;
  return status;
}

/**
 * @brief c09: read a *_fire fixture's FIREmodeOn, stored snapshots (the redshift table) and
 *        MassLoading, after its galaxies
 * @return Number of problems
 */
static int load_fire(struct oracle_fixture *in, const char *name, const int ngal,
                     struct GalaxyData *galaxies) {
  fixture_ngal = ngal;
  fixture_input = in;
  fixture_name = name;
  burst_replay_problems = 0;
  if (!fixture_fire) {
    return 0;
  }
  int mode = MIMIC_SAGE_FIRE_MODE_OFF;
  const int problems = oracle_c09_read_fire_config(in, ngal, fixture_snapnum, &mode);
  oracle_c09_read_mass_loading(in, ngal, galaxies);
  return problems;
}

/** @brief c09: every recorded galaxy's MassLoading, equal to its input */
static int check_fire_mass_loading(struct oracle_fixture *out, const int ngal,
                                   const struct GalaxyData *before,
                                   const struct GalaxyData *galaxies) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    problems +=
        oracle_c09_check_mass_loading(out, "", i, before[i].MassLoading, galaxies[i].MassLoading);
    counts_mass_loading++;
  }
  return problems;
}

// ============================================================================
// COMPARISONS
// ============================================================================

/**
 * @brief Require every galaxy member outside the recorded fields to be unchanged
 *
 * Copies the thirty block floats, the histories when the output records them, and the
 * transport field MergerDiskDominated from `after` into a copy of `before`.
 *
 * @return 1 if another member changed, else 0
 */
static int check_rest_unchanged(const char *name, const int index, const struct GalaxyData *before,
                                struct GalaxyData *after, const int with_histories) {
  struct GalaxyData expected = *before;
  size_t count;
  const char *const *names = oracle_c06_block_names(&count);
  for (size_t k = 0; k < count; k++) {
    *oracle_c06_galaxy_float(&expected, names[k]) = *oracle_c06_galaxy_float(after, names[k]);
  }
  if (with_histories) {
    for (size_t h = 0; h < (size_t)MIMIC_SFR_HISTORY_COUNT; h++) {
      for (int k = 0; k < ORACLE_C05_STEPS; k++) {
        *oracle_c05_history_element(&expected, h, k) = *oracle_c05_history_element(after, h, k);
      }
    }
  }
  expected.MergerDiskDominated = after->MergerDiskDominated;
  if (memcmp(&expected, after, sizeof(expected)) != 0) {
    fprintf(stderr, "oracle: %s: galaxy[%d] changed a member outside its recorded fields\n", name,
            index);
    return 1;
  }
  return 0;
}

/** @brief Whether the output records the histories of galaxy[index] */
static int records_histories(const struct oracle_fixture *out, const int index) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "galaxy[%d].SfrDisk[0]", index);
  return oracle_has(out, field);
}

/**
 * @brief Compare every recorded galaxy and require the rest of each unchanged
 * @return Number of mismatches
 */
static int check_galaxies(const char *name, struct oracle_fixture *out, const int ngal,
                          struct GalaxyData *galaxies, const struct GalaxyData *before) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    const int with_histories = records_histories(out, i);
    problems += oracle_c06_check_galaxy(out, "", i, &galaxies[i], with_histories);
    problems += check_rest_unchanged(name, i, &before[i], &galaxies[i], with_histories);
  }
  return problems;
}

/**
 * @brief Assert the integer identity fields of each galaxy against the port's halos
 *
 * SAGE26 keeps a removed satellite's Type and sets its mergeType (2 major, 1 minor,
 * M:311, :315; 4 disruption, M:902); the package marks the row Type 3. Every other halo
 * field is unchanged. SAGE26's driver bookkeeping mergeIntoID (B:545) has no package
 * member; its CentralGal is the package's CentralHalo.
 *
 * @param expected_merge_type  Per galaxy: the mergeType the port's actions give
 * @return Number of mismatches
 */
static int check_identities(struct oracle_fixture *out, const int ngal, struct Halo *halos,
                            const struct Halo *halos_before, const int *expected_merge_type) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "galaxy[%d].mergeType", i);
    if (!oracle_has(out, field)) {
      continue;
    }
    const int merge_type = oracle_in_i32(out, field);
    if (merge_type != expected_merge_type[i]) {
      fprintf(stderr, "oracle: %s: galaxy[%d] mergeType %d, the port's actions give %d\n",
              out->path, i, merge_type, expected_merge_type[i]);
      problems++;
    }
    snprintf(field, sizeof(field), "galaxy[%d].Type", i);
    const int sage26_type = oracle_in_i32(out, field);
    const int expected_type = merge_type > 0 ? 3 : sage26_type;
    if (halos[i].Type != expected_type) {
      fprintf(stderr, "oracle: %s: halo[%d].Type %d, expected %d (SAGE26 Type %d, mergeType %d)\n",
              out->path, i, halos[i].Type, expected_type, sage26_type, merge_type);
      problems++;
    }
    struct Halo expected = halos_before[i];
    expected.Type = halos[i].Type;
    if (memcmp(&expected, &halos[i], sizeof(expected)) != 0) {
      fprintf(stderr, "oracle: %s: halo[%d] changed a field other than Type\n", out->path, i);
      problems++;
    }
    problems += oracle_check_i32(out, oracle_galaxy_field(i, "SnapNum"), halos[i].SnapNum);
    problems += oracle_check_i32(out, oracle_galaxy_field(i, "Len"), halos[i].Len);
    oracle_exclude(out, oracle_galaxy_field(i, "mergeIntoID"));
    problems += oracle_check_i32(out, oracle_galaxy_field(i, "CentralGal"), halos[i].CentralHalo);
  }
  return problems;
}

/** @brief Assert add_galaxies_together's flags and carried values; returns the problems */
static int check_add_locals(struct oracle_fixture *out, const struct MimicSageAddLocals *a) {
  int problems = 0;
  problems += oracle_c06_check_flag(out, "satellite_disk_positive", a->satellite_disk_positive);
  problems += oracle_c06_check_flag(out, "disk_fraction_above_half", a->disk_fraction_above_half);
  problems += oracle_c06_check_flag(out, "satellite_ics_positive", a->satellite_ics_positive);
  problems += oracle_c06_check_carried(out, "satellite_disk_mass", KIND_CONTINUATION,
                                       a->satellite_disk_mass, 0, &tally);
  problems +=
      oracle_c06_check_carried(out, "disk_mass_post", KIND_TAIL, a->disk_mass_post, 0, &tally);
  problems += oracle_c06_check_carried(out, "disk_fraction_post", KIND_TAIL, a->disk_fraction_post,
                                       0, &tally);
  return problems;
}

/** @brief Assert the merger's own flags and carried values; returns the problems */
static int check_merger_locals(struct oracle_fixture *out, const struct MimicSageMergerLocals *m,
                               const double threshold_major_merger) {
  int problems = 0;
  problems += oracle_c06_check_flag(out, "mass_ratio_path", m->mass_ratio_path);
  problems += oracle_c06_check_flag(out, "major_merger", m->major_merger);
  problems += oracle_c06_check_carried(out, "mi", KIND_TAIL, m->mi, 0, &tally);
  problems += oracle_c06_check_carried(out, "ma", KIND_TAIL, m->ma, 0, &tally);
  problems +=
      oracle_c06_check_carried(out, "mass_ratio", KIND_CONTINUATION, m->mass_ratio, 0, &tally);
  problems += oracle_c06_check_carried(out, "central_disk_mass", KIND_TAIL, m->central_disk_mass, 0,
                                       &tally);

  /* The two carried integers: their values are the port's (the flag the resolver carries
   * on the target, and the channel the burst consumer selects from it); the markers are
   * the oracle's record of its own checks */
  problems += oracle_check_i32(out, "intermediate.is_disk_dominated", m->is_disk_dominated);
  problems += oracle_check_i32(
      out, "intermediate.burst_to_merger_bulge",
      mimic_sage_merger_burst_channel(m->mass_ratio, threshold_major_merger, m->is_disk_dominated));
  (void)consume_record(out, "is_disk_dominated");
  (void)consume_record(out, "burst_to_merger_bulge");
  return problems;
}

/**
 * @brief BulgeSizeOn 0 only: the remnant radius is output-inert, SAGE26's own output zeroes
 *        the three radii
 *
 * With BulgeSizeOn 0 the port does not form new_merger_radius (get_bulge_radius zeroes the
 * radii after it on every path, M:293-331, R/src/model_misc.c:241-246); the record shows
 * SAGE26's value and its three radii. With BulgeSizeOn 3 the port forms it and
 * check_remnant_record() compares it instead.
 *
 * @return Number of problems
 */
static int check_remnant_radius_inert(struct oracle_fixture *out, const int target) {
  int problems = 0;
  const char *const radii[] = {"BulgeRadius", "MergerBulgeRadius", "InstabilityBulgeRadius"};
  for (size_t k = 0; k < COUNT(radii); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "galaxy[%d].%s", target, radii[k]);
    for (int i = 0; i < out->num_fields; i++) {
      if (strcmp(out->fields[i].name, field) == 0 && out->fields[i].bits != 0) {
        fprintf(stderr, "oracle: %s: SAGE26 left %s non-zero\n", out->path, field);
        problems++;
      }
    }
  }
  if (oracle_in_i32(out, "intermediate.new_merger_radius_absent") == 0 &&
      oracle_in_f64(out, "intermediate.new_merger_radius") != 0.0 && problems == 0) {
    counts_inert_radius++;
  }
  (void)consume_record(out, "new_merger_radius");
  // Read on R0 only by update_instability_bulge_radius, which returns at once (misc.c:395)
  (void)consume_record(out, "old_disk_radius");
  return problems;
}

/**
 * @brief The BulgeSizeOn 3 remnant record against the port's (oracle README "The c07 Group")
 *
 * The seven branch flags of calculate_merger_remnant_radius (M:142-202) and its twenty locals,
 * each compared bit for bit with mimic_sage_merger_remnant_radius()'s, and new_merger_radius
 * (M:297), all with the oracle's markers checked (tail-checked, _narrowing_limited 1: the only
 * path of each to the state is the float store into MergerBulgeRadius). old_disk_radius
 * (M:257, continuation-checked through the burst) is compared with the target's DiskScaleRadius
 * the transaction saved before the addition: its only path to the state is the burst's float
 * InstabilityBulgeRadius store (R/src/model_misc.c:422), so it too is narrowing-limited.
 *
 * @return Number of mismatches and marker errors
 */
static int check_remnant_record(struct oracle_fixture *out, const struct MimicSageMergerLocals *m) {
  const struct MimicSageRemnantLocals *r = &m->remnant;
  int problems = 0;
  const struct {
    const char *name;
    int actual;
  } flags[REMNANT_BRANCHES] = {{"remnant_total_nonpositive", r->total_nonpositive},
                               {"remnant_g1_mass_positive", r->g1_mass_positive},
                               {"remnant_g2_mass_positive", r->g2_mass_positive},
                               {"remnant_r1_fallback", r->r1_fallback},
                               {"remnant_r2_fallback", r->r2_fallback},
                               {"remnant_both_radii_zero", r->both_radii_zero},
                               {"remnant_energy_nonpositive", r->energy_nonpositive}};
  for (int k = 0; k < REMNANT_BRANCHES; k++) {
    problems += oracle_c06_check_flag(out, flags[k].name, flags[k].actual);
  }
  /* Branch coverage: a sum NOT positive (k = 1, 2) and every other flag set */
  for (int k = 0; k < REMNANT_BRANCHES; k++) {
    const int taken = (k == 1 || k == 2) ? flags[k].actual == 0 : flags[k].actual == 1;
    counts_remnant_branch[k] += taken;
  }

  const struct {
    const char *name;
    double actual;
  } locals[] = {{"remnant_M1", r->M1},
                {"remnant_M2", r->M2},
                {"remnant_M_tot", r->M_tot},
                {"remnant_R1_disk_half", r->R1_disk_half},
                {"remnant_R1_bulge_half", r->R1_bulge_half},
                {"remnant_M1_disk", r->M1_disk},
                {"remnant_M1_bulge", r->M1_bulge},
                {"remnant_R1_progenitor", r->R1_progenitor},
                {"remnant_R2_disk_half", r->R2_disk_half},
                {"remnant_R2_bulge_half", r->R2_bulge_half},
                {"remnant_M2_disk", r->M2_disk},
                {"remnant_M2_bulge", r->M2_bulge},
                {"remnant_R2_progenitor", r->R2_progenitor},
                {"remnant_R1", r->R1},
                {"remnant_R2", r->R2},
                {"remnant_E_init", r->E_init},
                {"remnant_E_orb", r->E_orb},
                {"remnant_f_gas", r->f_gas},
                {"remnant_E_rad", r->E_rad},
                {"remnant_E_final", r->E_final},
                {"new_merger_radius", r->new_merger_radius}};
  for (size_t k = 0; k < COUNT(locals); k++) {
    problems += oracle_c07_check_local(out, locals[k].name, locals[k].actual, 1, &remnant_tally);
  }
  (void)consume_record(out, "new_merger_radius");

  // old_disk_radius: continuation-checked (passed to the burst), narrowing-limited
  problems += oracle_check_i32(out, "intermediate.old_disk_radius_absent", 0);
  problems += oracle_check_i32(out, "intermediate.old_disk_radius_checked", 1);
  problems += oracle_check_i32(out, "intermediate.old_disk_radius_check_kind", KIND_CONTINUATION);
  problems += oracle_check_i32(out, "intermediate.old_disk_radius_narrowing_limited", 1);
  problems += oracle_check_f64(out, "intermediate.old_disk_radius", m->old_disk_radius);
  if (consume_record(out, "old_disk_radius")) {
    fprintf(stderr, "oracle: %s: old_disk_radius is narrowing-limited but validated two-sided\n",
            out->path);
    problems++;
  }
  return problems;
}

/**
 * @brief Consume the black-hole, wind and burst records of a merge fixture
 *
 * The consumers' process() entries do not expose their locals; their effect is the
 * asserted end state. Counts the values the oracle validated two-sided, for the report.
 */
static void consume_consumer_records(struct oracle_fixture *out) {
  static const char *const values[] = {"BHaccrete_pre_cap",
                                       "BHaccrete",
                                       "bh_metallicity",
                                       "quasar_energy",
                                       "cold_gas_energy",
                                       "hot_gas_energy",
                                       "eburst",
                                       "gas_for_starburst",
                                       "stars_pre_renorm",
                                       "reheated_mass_pre_renorm",
                                       "renorm_fac",
                                       "stars",
                                       "reheated_mass",
                                       "ejected_mass_pre_floor",
                                       "ejected_mass",
                                       "metallicity_sf",
                                       "recycled_stars",
                                       "metallicity_fb",
                                       "reheated_mass_fb_clamped",
                                       "ejected_mass_capped",
                                       "metallicity_hot",
                                       "metals_hot_to_eject",
                                       "metals_hot_to_eject_capped",
                                       "FracZleaveDiskVal",
                                       "yield_metals_to_hot"};
  static const char *const flags[] = {"bh_coldgas_positive",
                                      "bh_accretion_cap_binds",
                                      "bh_metals_floor_binds",
                                      "wind_ejects_cold",
                                      "wind_ejects_hot",
                                      "burst_gas_floored",
                                      "burst_stars_floored",
                                      "burst_renorm_active",
                                      "burst_ejection_vvir_nonpositive",
                                      "burst_ejected_floored",
                                      "reheat_clamp_fb_binds",
                                      "ejected_cap_binds",
                                      "metals_cap_binds",
                                      "burst_instability_recheck_ran",
                                      "burst_instability_recheck_changed_state",
                                      "burst_yield_minor_branch"};
  for (size_t k = 0; k < COUNT(values); k++) {
    counts_consumer_locals_validated += consume_record(out, values[k]);
  }
  for (size_t k = 0; k < COUNT(flags); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s", flags[k]);
    oracle_exclude(out, field);
  }
  /* c08: the burst's part of the SFprescription 1 record (present only on those fixtures),
   * likewise consumed; the black-hole part is asserted in check_merge() */
  if (!oracle_has(out, "intermediate.bh_hydrogen_caps_tracked")) {
    return;
  }
  static const char *const h_values[] = {"max_h_fb", "burst_h2_budget", "burst_h1_refresh"};
  static const char *const h_flags[] = {"burst_sf_commit_h2_capped",
                                        "burst_sf_commit_h1_capped",
                                        "hydrogen_caps_fb_tracked",
                                        "h2_cap_fb_binds",
                                        "h1_cap_fb_binds",
                                        "burst_hydrogen_refresh_tracked",
                                        "burst_refresh_h2_cap_binds",
                                        "burst_refresh_h1_floored"};
  for (size_t k = 0; k < COUNT(h_values); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s_narrowing_limited", h_values[k]);
    if (oracle_has(out, field)) {
      oracle_exclude(out, field);
    }
    counts_consumer_locals_validated += consume_record(out, h_values[k]);
  }
  for (size_t k = 0; k < COUNT(h_flags); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s", h_flags[k]);
    oracle_exclude(out, field);
  }
  /* The observation of the recheck's black-hole growth (the burst consumer's process() exposes
   * no locals; the burst oracle test asserts it against the shared burst's recheck_bh_caps) */
  static const char *const recheck[] = {
      "burst_recheck_bh_grown", "burst_recheck_bh_h2_capped", "burst_recheck_bh_h1_capped",
      "burst_recheck_after_bh.H2gas", "burst_recheck_after_bh.H1gas"};
  for (size_t k = 0; k < COUNT(recheck); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s", recheck[k]);
    if (k == 0 || oracle_has(out, field)) {
      oracle_exclude(out, field);
    }
  }
}

// ============================================================================
// FIXTURE CHECKS
// ============================================================================

/**
 * @brief Build the fixture's galaxies; returns the number of problems
 *
 * SAGE26's CentralGal is the package's CentralHalo (the core sets both to the subhalo's
 * Type 0 or 1 row at joining).
 */
static int load_galaxies(struct oracle_fixture *in, const int ngal, struct Halo *halos,
                         struct GalaxyData *galaxies) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    problems += oracle_c06_in_galaxy(in, i, &halos[i], &galaxies[i]);
    halos[i].CentralHalo = oracle_in_i32(in, oracle_galaxy_field(i, "CentralGal"));
  }
  return problems;
}

/** @brief add_galaxies_together, called directly */
static int check_add(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "add_galaxies_together", &in, &out) != 0) {
    return 1;
  }
  /* BulgeSizeOn 0 (R0) or 3 (the c07 add_s3_* fixtures), with Hubble_h for 3 */
  struct MimicSageBulgeSize sizes;
  int problems = oracle_c07_require_merger_flags(&in, &sizes);
  // The routine reads no parameter on R0 beyond the flags; the rest are the group's
  consume_prefix(&in, "params.");
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int t = oracle_in_i32(&in, "call.t");
  const int p = oracle_in_i32(&in, "call.p");
  if (ngal < 2 || ngal > MAX_GALAXIES || t < 0 || t >= ngal || p < 0 || p >= ngal || t == p) {
    fprintf(stderr, "oracle: %s: call.ngal, call.t or call.p out of range\n", name);
    return problems + 1;
  }

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES];
  problems += load_galaxies(&in, ngal, halos, galaxies);
  if (problems != 0) {
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));

  struct MimicSageAddLocals locals;
  mimic_sage_add_galaxies_together(&galaxies[t], &galaxies[p], &sizes, &locals);

  problems += check_galaxies(name, &out, ngal, galaxies, before);
  problems += check_add_locals(&out, &locals);
  counts_add++;
  if (sizes.bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
    counts_add_sizes++;
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief make_bulge_from_burst, called directly on galaxy 0 */
static int check_mbb(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "make_bulge_from_burst", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;
  consume_prefix(&in, "params.");
  static struct GalaxyData galaxies[1], before[1];
  static struct Halo halos[1];
  problems += load_galaxies(&in, 1, halos, galaxies);
  if (problems != 0) {
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));

  mimic_sage_make_bulge_from_burst(&galaxies[0]);

  problems += check_galaxies(name, &out, 1, galaxies, before);
  counts_mbb++;
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief A module context for a transaction at history bin `step` of ten substeps */
static int make_context(const char *name, struct ModuleContext *ctx, struct Halo *central,
                        const int step, const int num_substeps) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->params = &MimicConfig;
  ctx->central_galaxy = central;
  ctx->num_substeps = num_substeps;
  ctx->substep_number = step;
  if (mimic_sage_sfr_step_bin(step, num_substeps) < 0) {
    fprintf(stderr, "oracle: %s: substep %d of %d has no history bin\n", name, step, num_substeps);
    return 1;
  }
  return 0;
}

/** @brief deal_with_galaxy_merger through the resolver's transaction and its consumers */
static int check_merge(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  fixture_fire = strcmp(in.routine, ORACLE_C09_MERGER_ROUTINE) == 0;
  if (oracle_load_pair(MODULE, name,
                       fixture_fire ? ORACLE_C09_MERGER_ROUTINE : "deal_with_galaxy_merger", &in,
                       &out) != 0) {
    return 1;
  }
  int problems = setup_modules(&in);
  const double threshold_major_merger = oracle_in_f64(&in, "params.ThreshMajorMerger");
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int p = oracle_in_i32(&in, "call.p");
  const int target = oracle_in_i32(&in, "call.merger_centralgal");
  const int central = oracle_in_i32(&in, "call.centralgal");
  const double time = oracle_in_f64(&in, "call.time");
  const double dt = oracle_in_f64(&in, "call.dt");
  const int step = oracle_in_i32(&in, "call.step");
  if (ngal < 2 || ngal > MAX_GALAXIES || p < 0 || p >= ngal || target < 0 || target >= ngal ||
      central < 0 || central >= ngal || p == target) {
    fprintf(stderr, "oracle: %s: a call index is out of range\n", name);
    cleanup_modules();
    return problems + 1;
  }

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES], halos_before[MAX_GALAXIES];
  problems += load_galaxies(&in, ngal, halos, galaxies);
  problems += load_fire(&in, name, ngal, galaxies);
  struct ModuleContext ctx;
  problems += make_context(name, &ctx, &halos[central], step, FIXTURE_SUBSTEPS);
  if (problems != 0) {
    cleanup_modules();
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));
  memcpy(halos_before, halos, sizeof(halos_before));

  struct MimicSageMergerLocals merger;
  struct MimicSageAddLocals add;
  event_count = 0;
  oracle_poison(&shadow_bh, sizeof(shadow_bh));
  if (sage26_resolve_mergers_and_disruption_merge(&ctx, halos, ngal, p, target, time, dt,
                                                  dispatch_merger_event, &merger, &add) != 0) {
    fprintf(stderr, "oracle: %s: the merger transaction failed\n", name);
    problems++;
  }
  if (event_count != 1 || event_source[0] != p || event_target[0] != target) {
    fprintf(stderr, "oracle: %s: expected one merger event %d -> %d, got %d\n", name, p, target,
            event_count);
    problems++;
  }
  if (oracle_f64_to_bits(merger.mass_ratio) == oracle_f64_to_bits(0.0)) {
    counts_ratio_zero++;
  }

  int expected_merge_type[MAX_GALAXIES] = {0, 0, 0, 0};
  expected_merge_type[p] = merger.major_merger ? 2 : 1;
  problems += check_galaxies(name, &out, ngal, galaxies, before);
  problems += check_identities(&out, ngal, halos, halos_before, expected_merge_type);
  problems += check_merger_locals(&out, &merger, threshold_major_merger);
  problems += check_add_locals(&out, &add);
  if (oracle_in_i32(&in, "params.BulgeSizeOn") == MIMIC_SAGE_BULGE_SIZE_TONINI) {
    problems += check_remnant_record(&out, &merger);
    counts_merge_sizes++;
    counts_merge_sizes_branch[merger.major_merger ? 0 : (merger.is_disk_dominated ? 1 : 2)]++;
  } else {
    problems += check_remnant_radius_inert(&out, target);
  }
  /* c08: the merger's black-hole cap, through the shadow of the consumer's growth */
  if (fixture_sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    problems += oracle_c08_check_bh_hydrogen(&out, fixture_sf_prescription,
                                             &shadow_bh.hydrogen_caps, &bh_h_tally);
    counts_merge_bh_caps++;
  }
  /* c09: MassLoading, the burst's FIRE record and its wiring, and the recheck burst's where the
   * oracle observes it took the FIRE branch (the replay of the consumer's call holds the locals) */
  if (fixture_fire) {
    problems += check_fire_mass_loading(&out, ngal, before, galaxies);
    problems += burst_replay_problems;
    const struct MimicSageStarburstLocals *b = &last_burst_replay.locals;
    problems +=
        oracle_c09_check_fire_record(&out, fixture_fire_mode_on, &b->fire.l, 0, &fire_tally);
    const int recheck_mode = oracle_c09_observed(&out, "burst_recheck_fire_mode_sensitive");
    const int recheck_z = oracle_c09_observed(&out, "burst_recheck_fire_redshift_sensitive");
    if (recheck_mode == 1 || recheck_z == 1) {
      problems += oracle_c09_check_burst_wiring(name, "recheck burst", &b->recheck_fire,
                                                fixture_fire_mode_on, fixture_snapnum[target],
                                                (float)halos[target].Vvir);
      problems +=
          oracle_c09_check_same_host_budget(name, "recheck burst", &b->recheck_fire.l, &b->fire.l);
      counts_recheck_wiring++;
    }
    counts_fire_merge++;
  }
  consume_consumer_records(&out);
  counts_merge++;

  cleanup_modules();
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

// ============================================================================
// DISRUPTION
// ============================================================================

/** @brief disrupt_satellite_to_ICS through the resolver's disruption entry */
static int check_disrupt(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "disrupt_satellite_to_ICS", &in, &out) != 0) {
    return 1;
  }
  int problems = oracle_c06_require_r0_flags(&in);
  // The routine reads no parameter on R0 beyond the flags; the rest are the group's
  consume_prefix(&in, "params.");
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int target = oracle_in_i32(&in, "call.centralgal");
  const int gal = oracle_in_i32(&in, "call.gal");
  const double time = oracle_in_f64(&in, "call.time");
  if (ngal < 2 || ngal > MAX_GALAXIES || target < 0 || target >= ngal || gal < 0 || gal >= ngal ||
      target == gal) {
    fprintf(stderr, "oracle: %s: a call index is out of range\n", name);
    return problems + 1;
  }

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES], halos_before[MAX_GALAXIES];
  problems += load_galaxies(&in, ngal, halos, galaxies);
  if (problems != 0) {
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));
  memcpy(halos_before, halos, sizeof(halos_before));

  action_count = 0;
  sage26_resolve_mergers_and_disruption_set_action_hook(observe_action);
  struct MimicSageDisruptLocals locals;
  if (sage26_resolve_mergers_and_disruption_disrupt(halos, ngal, gal, target, time, &locals) != 0) {
    fprintf(stderr, "oracle: %s: the disruption failed\n", name);
    problems++;
  }
  sage26_resolve_mergers_and_disruption_set_action_hook(NULL);
  // The package's disruption marker: one "disrupt" action for the satellite
  if (action_count != 1 || action_source[0] != gal || !action_is_disruption[0]) {
    fprintf(stderr, "oracle: %s: expected one disruption of galaxy[%d], got %d actions\n", name,
            gal, action_count);
    problems++;
  }

  int expected_merge_type[MAX_GALAXIES] = {0, 0, 0, 0};
  expected_merge_type[gal] = 4;
  problems += check_galaxies(name, &out, ngal, galaxies, before);
  problems += check_identities(&out, ngal, halos, halos_before, expected_merge_type);
  problems += oracle_c06_check_flag(&out, "disrupt_stars_positive", locals.disrupt_stars_positive);
  problems += oracle_c06_check_flag(&out, "disrupt_ics_positive", locals.disrupt_ics_positive);
  problems +=
      oracle_c06_check_carried(&out, "total_gas", KIND_CONTINUATION, locals.total_gas, 0, &tally);
  problems += oracle_c06_check_carried(&out, "total_metals_gas", KIND_CONTINUATION,
                                       locals.total_metals_gas, 0, &tally);
  counts_disrupt++;

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

// ============================================================================
// THE PASS
// ============================================================================

/** @brief "intermediate.pass.galaxy[<i>].<member><suffix>" */
static const char *pass_field(const int i, const char *member, const char *suffix) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "intermediate.pass.galaxy[%d].%s%s", i, member, suffix);
  return buffer;
}

/**
 * @brief Assert one row's pass record against SAGE26's driver locals
 *
 * Each value is absent exactly where SAGE26's path leaves it unformed (the port's NAN or
 * -1), and otherwise equal bit for bit. The target index is the workspace row less
 * `offset`, SAGE26's index.
 *
 * @return Number of mismatches
 */
static int check_pass_record(struct oracle_fixture *out, const int i,
                             const struct MimicSagePassRecord *r, const int offset) {
  int problems = 0;
  problems += oracle_check_i32(out, pass_field(i, "considered", ""), r->considered);
  problems += oracle_check_i32(out, pass_field(i, "action", ""), r->action);

  const struct {
    const char *name;
    double value;
  } doubles[] = {{"deltaT", r->deltaT},
                 {"currentMvir", r->currentMvir},
                 {"galaxyBaryons", r->galaxyBaryons},
                 {"event_time", r->event_time},
                 {"dt", r->dt}};
  for (size_t k = 0; k < COUNT(doubles); k++) {
    const int absent = oracle_in_i32(out, pass_field(i, doubles[k].name, "_absent"));
    if (absent != (isnan(doubles[k].value) ? 1 : 0)) {
      fprintf(stderr, "oracle: %s: pass galaxy[%d] %s is %s in the oracle, not in the port\n",
              out->path, i, doubles[k].name, absent ? "absent" : "present");
      problems++;
    } else if (!absent) {
      problems += oracle_check_f64(out, pass_field(i, doubles[k].name, ""), doubles[k].value);
    }
  }

  const int target = (r->merger_centralgal >= 0) ? r->merger_centralgal - offset : -1;
  const struct {
    const char *name;
    int value;
  } ints[] = {{"gate_zero_baryons", r->gate_zero_baryons},
              {"gate_passed", r->gate_passed},
              {"merger_centralgal", target},
              {"redirected", r->redirected},
              {"clock_finite", r->clock_finite},
              {"step_bin", r->step_bin}};
  for (size_t k = 0; k < COUNT(ints); k++) {
    const int absent = oracle_in_i32(out, pass_field(i, ints[k].name, "_absent"));
    if (absent != (ints[k].value == -1 ? 1 : 0)) {
      fprintf(stderr, "oracle: %s: pass galaxy[%d] %s is %s in the oracle, not in the port\n",
              out->path, i, ints[k].name, absent ? "absent" : "present");
      problems++;
    } else if (!absent) {
      problems += oracle_check_i32(out, pass_field(i, ints[k].name, ""), ints[k].value);
    }
  }
  return problems;
}

/** @brief The mergeType the port's pass gives a row: its action, else its input mergeType */
static int pass_merge_type(const struct MimicSagePassRecord *r, const int input_merge_type) {
  if (r->action == 4) {
    return 4;
  }
  if (r->action == 1) {
    return r->major_merger ? 2 : 1;
  }
  return input_merge_type;
}

/**
 * @brief The pass's removals against the transactions' reports, in row order
 *
 * One merger event per merger (source and target), and one action per removal, a
 * disruption reported as "disrupt", in the order the pass took them.
 *
 * @return Number of mismatches
 */
static int check_pass_order(const char *name, const int nrows,
                            const struct MimicSagePassRecord *records) {
  int problems = 0;
  int mergers = 0, removals = 0;
  for (int i = 0; i < nrows; i++) {
    if (records[i].action == 0) {
      continue;
    }
    if (removals < MAX_EVENTS && removals < action_count &&
        (action_source[removals] != i ||
         action_is_disruption[removals] != (records[i].action == 4))) {
      fprintf(stderr, "oracle: %s: removal %d is not row %d's\n", name, removals, i);
      problems++;
    }
    removals++;
    if (records[i].action == 1) {
      if (mergers < MAX_EVENTS && mergers < event_count &&
          (event_source[mergers] != i || event_target[mergers] != records[i].merger_centralgal)) {
        fprintf(stderr, "oracle: %s: merger event %d is %d -> %d, expected %d -> %d\n", name,
                mergers, event_source[mergers], event_target[mergers], i,
                records[i].merger_centralgal);
        problems++;
      }
      mergers++;
    }
  }
  if (mergers != event_count || removals != action_count) {
    fprintf(stderr, "oracle: %s: %d mergers and %d removals, but %d events and %d actions\n", name,
            mergers, removals, event_count, action_count);
    problems++;
  }
  if (mergers >= 2) {
    counts_multi++;
  }
  counts_pass_mergers += mergers;
  counts_pass_disruptions += removals - mergers;
  for (int i = 0; i < nrows; i++) {
    counts_pass_redirects += (records[i].redirected == 1);
  }
  return problems;
}

/**
 * @brief Build a pass fixture's workspace: each row's galaxy, CentralHalo, dT and merged state
 *
 * A galaxy SAGE26 has already removed this snapshot (mergeType > 0, still its Type) is the
 * package's Type 3 row. Each row's dT is call.Age[SnapNum] - call.halo_age, SAGE26's
 * deltaT (B:529), as the core forms dT from its Age table.
 *
 * @return Number of problems
 */
static int load_pass_galaxies(struct oracle_fixture *in, const int ngal, struct Halo *halos,
                              struct GalaxyData *galaxies, int *input_merge_type) {
  int problems = load_galaxies(in, ngal, halos, galaxies);
  const double halo_age = oracle_in_f64(in, "call.halo_age");
  for (int i = 0; i < ngal; i++) {
    input_merge_type[i] = oracle_in_i32(in, oracle_galaxy_field(i, "mergeType"));
    if (input_merge_type[i] != 0) {
      halos[i].Type = 3;
    }
    char age[ORACLE_MAX_NAME];
    snprintf(age, sizeof(age), "call.Age[%d]", halos[i].SnapNum);
    halos[i].dT = oracle_in_f64(in, age) - halo_age;
  }
  return problems;
}

/** @brief The halo values a pass record holds for each row, as SAGE26's floats */
static int check_halo_floats(struct oracle_fixture *out, const char *prefix, const int j,
                             const struct Halo *row) {
  const char *const names[] = {"Mvir", "deltaMvir", "Rvir", "Vvir", "Vmax"};
  const double values[] = {row->Mvir, row->deltaMvir, row->Rvir, row->Vvir, row->Vmax};
  int problems = 0;
  for (size_t k = 0; k < COUNT(names); k++) {
    problems +=
        oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, names[k]), (float)values[k]);
  }
  return problems;
}

/** @brief SAGE26's per-substep pass (B:519-587) through the resolver's pass */
static int check_pass(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  fixture_fire = strcmp(in.routine, ORACLE_C09_PASS_ROUTINE) == 0;
  if (oracle_load_pair(MODULE, name, fixture_fire ? ORACLE_C09_PASS_ROUTINE : "merger_pass", &in,
                       &out) != 0) {
    return 1;
  }
  int problems = setup_modules(&in);
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int central = oracle_in_i32(&in, "call.centralgal");
  const int step = oracle_in_i32(&in, "call.step");
  const int num_substeps = oracle_in_i32(&in, "call.effective_steps");
  // The offset of mergeIntoID (B:545), SAGE26's output bookkeeping with no package member
  (void)oracle_in_i32(&in, "call.numgals");
  if (ngal < 2 || ngal > MAX_GALAXIES || central < 0 || central >= ngal) {
    fprintf(stderr, "oracle: %s: call.ngal or call.centralgal out of range\n", name);
    cleanup_modules();
    return problems + 1;
  }

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES], halos_before[MAX_GALAXIES];
  int input_merge_type[MAX_GALAXIES];
  problems += load_pass_galaxies(&in, ngal, halos, galaxies, input_merge_type);
  problems += load_fire(&in, name, ngal, galaxies);
  struct ModuleContext ctx;
  problems += make_context(name, &ctx, &halos[central], step, num_substeps);
  ctx.time = oracle_in_f64(&in, "call.halo_age");
  // SAGE26's centralgal is the FoF's Type 0 (B:393-400), which the pass finds itself
  if (mimic_find_fof_central_index(halos, ngal) != central) {
    fprintf(stderr, "oracle: %s: call.centralgal is not the workspace's Type 0\n", name);
    problems++;
  }
  if (problems != 0) {
    cleanup_modules();
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));
  memcpy(halos_before, halos, sizeof(halos_before));

  event_count = 0;
  action_count = 0;
  struct MimicSagePassRecord records[MAX_GALAXIES];
  if (sage26_resolve_mergers_and_disruption_pass(&ctx, halos, ngal, dispatch_merger_event,
                                                 records) != 0) {
    fprintf(stderr, "oracle: %s: the pass failed\n", name);
    problems++;
  }

  int expected_merge_type[MAX_GALAXIES] = {0, 0, 0, 0};
  for (int i = 0; i < ngal; i++) {
    expected_merge_type[i] = pass_merge_type(&records[i], input_merge_type[i]);
    // The clock the pass decremented; the rest of the galaxy as the transactions left it
    problems += oracle_check_f32(&out, oracle_galaxy_field(i, "MergTime"), galaxies[i].MergTime);
    before[i].MergTime = galaxies[i].MergTime;
    problems += check_halo_floats(&out, "", i, &halos[i]);
    problems += check_pass_record(&out, i, &records[i], 0);
  }
  problems += check_galaxies(name, &out, ngal, galaxies, before);
  problems += check_identities(&out, ngal, halos, halos_before, expected_merge_type);
  problems += check_pass_order(name, ngal, records);
  /* c09: MassLoading, each merger burst's wiring (dispatch_merger_event), and the oracle's
   * observations of SAGE26's pass under FIREmodeOn 0 and a replaced target redshift */
  if (fixture_fire) {
    problems += check_fire_mass_loading(&out, ngal, before, galaxies);
    problems += burst_replay_problems;
    problems += oracle_check_i32(&out, "intermediate.fire_mode", fixture_fire_mode_on);
    const int mode_sensitive = oracle_c09_observed(&out, "pass_fire_mode_sensitive");
    const int z_sensitive = oracle_c09_observed(&out, "pass_fire_redshift_sensitive");
    counts_pass_observed += mode_sensitive == 1 && z_sensitive == 1;
    counts_fire_pass++;
  }
  counts_pass++;

  cleanup_modules();
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

// ============================================================================
// JOINING, THEN THE PASS
// ============================================================================

/**
 * @brief Assert one joined or passed row against "<prefix>galaxy[<j>].*"
 *
 * The c06 block, the histories, the clock, the identity integers, the halo values, the
 * infall record and Cooling and Heating. On a row joining made an orphan, SAGE26 keeps
 * Len, QuasarModeBHaccretionMass, OutflowRate, Cooling and Heating (B:261, :273-276 run
 * on the main branch only), which the core zeros (Len, make_orphan) or init_repeat resets:
 * excluded there, by name, and counted. dT is SAGE26's -1 of B:237 until its physics loop
 * (B:483), which the pass never reads; mergeIntoID has no package member (B:545).
 *
 * @param expected_merge_type  The mergeType the port gives the row
 * @param newly_orphaned       The row became a Type 2 at this joining
 * @param offset               The workspace row of SAGE26's galaxy 0
 * @return Number of mismatches
 */
static int check_joined_row(struct oracle_fixture *out, const char *prefix, const int j,
                            const struct Halo *row, const int expected_merge_type,
                            const int newly_orphaned, const int offset) {
  struct GalaxyData *g = row->galaxy;
  int problems = 0;

  size_t count;
  const char *const *names = oracle_c06_block_names(&count);
  for (size_t k = 0; k < count; k++) {
    const char *field = oracle_prefixed_galaxy_field(prefix, j, names[k]);
    if (newly_orphaned && (strcmp(names[k], "QuasarModeBHaccretionMass") == 0 ||
                           strcmp(names[k], "OutflowRate") == 0)) {
      oracle_exclude(out, field);
      counts_orphan_exclusions++;
      continue;
    }
    problems += oracle_check_f32(out, field, *oracle_c06_galaxy_float(g, names[k]));
  }
  problems += oracle_c05_check_histories(out, prefix, j, g);
  problems +=
      oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, "MergTime"), g->MergTime);

  const int merge_type = oracle_in_i32(out, oracle_prefixed_galaxy_field(prefix, j, "mergeType"));
  if (merge_type != expected_merge_type) {
    fprintf(stderr, "oracle: %s: %sgalaxy[%d] mergeType %d, the port gives %d\n", out->path, prefix,
            j, merge_type, expected_merge_type);
    problems++;
  }
  const int sage26_type = oracle_in_i32(out, oracle_prefixed_galaxy_field(prefix, j, "Type"));
  const int expected_type = merge_type > 0 ? 3 : sage26_type;
  if (row->Type != expected_type) {
    fprintf(stderr, "oracle: %s: %sgalaxy[%d] Type %d, expected %d\n", out->path, prefix, j,
            row->Type, expected_type);
    problems++;
  }
  problems +=
      oracle_check_i32(out, oracle_prefixed_galaxy_field(prefix, j, "SnapNum"), row->SnapNum);
  problems +=
      oracle_check_i32(out, oracle_prefixed_galaxy_field(prefix, j, "HaloNr"), (int)row->HaloNr);
  problems += oracle_check_i32(out, oracle_prefixed_galaxy_field(prefix, j, "CentralGal"),
                               row->CentralHalo - offset);
  oracle_exclude(out, oracle_prefixed_galaxy_field(prefix, j, "mergeIntoID"));
  oracle_exclude(out, oracle_prefixed_galaxy_field(prefix, j, "dT"));
  if (newly_orphaned) {
    oracle_exclude(out, oracle_prefixed_galaxy_field(prefix, j, "Len"));
    counts_orphan_exclusions++;
  } else {
    problems += oracle_check_i32(out, oracle_prefixed_galaxy_field(prefix, j, "Len"), row->Len);
  }

  problems += check_halo_floats(out, prefix, j, row);
  problems += oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, "infallMvir"),
                               (float)row->infallMvir);
  problems += oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, "infallVvir"),
                               (float)row->infallVvir);
  problems += oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, "infallVmax"),
                               (float)row->infallVmax);
  problems += oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, "infallStellarMass"),
                               g->infallStellarMassState);
  problems += oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, j, "TimeOfInfall"),
                               g->TimeOfInfallState);

  const char *const doubles[] = {"Cooling", "Heating"};
  for (size_t k = 0; k < COUNT(doubles); k++) {
    const char *field = oracle_prefixed_galaxy_field(prefix, j, doubles[k]);
    if (newly_orphaned) {
      oracle_exclude(out, field);
      counts_orphan_exclusions++;
    } else {
      problems += oracle_check_f64(out, field, *oracle_galaxy_double(g, doubles[k]));
    }
  }
  return problems;
}

/** @brief Joining (the core's inheritance and the package's modules), then the pass */
static int check_join_and_pass(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "join_and_pass", &in, &out) != 0) {
    return 1;
  }
  int problems = setup_modules(&in);
  // The clock's cosmology and factor (oracle README "Flags and Parameters")
  MimicConfig.PartMass = oracle_in_f64(&in, "params.PartMass");
  MimicConfig.Hubble = oracle_in_f64(&in, "params.Hubble");
  MimicConfig.Omega = oracle_in_f64(&in, "params.Omega");
  MimicConfig.OmegaLambda = oracle_in_f64(&in, "params.OmegaLambda");
  if (oracle_set_model_param("MergerTimeFactor", oracle_in_f64(&in, "params.MergerTimeFactor")) !=
          0 ||
      sage26_initialise_merger_clock_init() != 0) {
    fprintf(stderr, "oracle: %s: the clock module failed to initialise\n", name);
    problems++;
  }

  struct GalaxyPool *pool = galaxy_pool_create(16);
  static struct oracle_join_result result;
  struct ModuleContext ctx;
  if (problems == 0) {
    problems += oracle_join(&in, pool, &result, &ctx, 1);
  }

  if (problems == 0) {
    const int start = result.start;
    const int retained = result.end - start;
    int newly_orphaned[ORACLE_JOIN_MAX_GALAXIES];
    problems += oracle_check_i32(&out, "intermediate.join.ngal", retained);
    for (int j = 0; j < retained; j++) {
      const struct Halo *row = &result.workspace[start + j];
      newly_orphaned[j] =
          (row->Type == 2 && (result.source_type[j] == 0 || result.source_type[j] == 1));
      problems += check_joined_row(&out, "intermediate.join.", j, row, 0, newly_orphaned[j], start);
    }

    // The pass at SAGE26's substep; centralgal is the joined central (B:393-400)
    const int fof_central = mimic_find_fof_central_index(result.workspace, result.end);
    if (fof_central - start != oracle_in_i32(&in, "call.centralgal")) {
      fprintf(stderr, "oracle: %s: call.centralgal is not the joined Type 0\n", name);
      problems++;
    }
    ctx.substep_number = oracle_in_i32(&in, "call.step");
    ctx.num_substeps = oracle_in_i32(&in, "call.effective_steps");
    ctx.central_galaxy = &result.workspace[fof_central];
    // The offset of mergeIntoID (B:545), SAGE26's output bookkeeping with no package member
    (void)oracle_in_i32(&in, "call.numgals");

    static struct GalaxyData before[ORACLE_JOIN_WORKSPACE_ROWS];
    static struct Halo halos_before[ORACLE_JOIN_WORKSPACE_ROWS];
    for (int i = 0; i < result.end; i++) {
      before[i] = *result.workspace[i].galaxy;
    }
    memcpy(halos_before, result.workspace, sizeof(halos_before));

    event_count = 0;
    action_count = 0;
    struct MimicSagePassRecord records[ORACLE_JOIN_WORKSPACE_ROWS];
    if (sage26_resolve_mergers_and_disruption_pass(&ctx, result.workspace, result.end,
                                                   dispatch_merger_event, records) != 0) {
      fprintf(stderr, "oracle: %s: the pass failed\n", name);
      problems++;
    }
    for (int j = 0; j < retained; j++) {
      struct Halo *row = &result.workspace[start + j];
      problems += check_joined_row(&out, "", j, row, pass_merge_type(&records[start + j], 0),
                                   newly_orphaned[j], start);
      before[start + j].MergTime = row->galaxy->MergTime;
      problems += check_rest_unchanged(name, j, &before[start + j], row->galaxy, 1);
      struct Halo expected = halos_before[start + j];
      expected.Type = row->Type;
      if (memcmp(&expected, row, sizeof(expected)) != 0) {
        fprintf(stderr, "oracle: %s: halo[%d] changed a field other than Type\n", name, j);
        problems++;
      }
      problems += check_pass_record(&out, j, &records[start + j], start);
    }
    problems += check_pass_order(name, result.end, records);
    counts_join_and_pass++;
  }
  cleanup_modules();
  sage26_initialise_merger_clock_cleanup();

  // SAGE26's joining locals (their effects are the asserted fields), and the inputs the
  // composition reads through the core and the modules
  consume_prefix(&out, "intermediate.join.progenitor[");
  consume_prefix(&in, "galaxy[");
  consume_prefix(&in, "halo[");
  consume_prefix(&in, "params.");
  consume_prefix(&in, "call.");

  galaxy_pool_destroy(pool);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Dispatch one fixture by its name */
static int check_fixture(const char *name) {
  /* c09: set by check_merge and check_pass for a *_fire routine */
  fixture_fire = 0;
  if (strncmp(name, "add_", 4) == 0) {
    return check_add(name);
  }
  if (strncmp(name, "mbb_", 4) == 0) {
    return check_mbb(name);
  }
  if (strncmp(name, "merge_", 6) == 0) {
    return check_merge(name);
  }
  if (strncmp(name, "dis_", 4) == 0) {
    return check_disrupt(name);
  }
  if (strncmp(name, "tr_pass_", 8) == 0) {
    return check_pass(name);
  }
  if (strncmp(name, "tr_jp_", 6) == 0) {
    return check_join_and_pass(name);
  }
  fprintf(stderr, "oracle: %s: no check for this fixture\n", name);
  return 1;
}

/**
 * @brief Run every listed fixture bit for bit
 * @return 1 if the manifest lists its declared count and every fixture is identical
 */
static int run_fixtures(void) {
  static char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
  int declared = -1;
  const int listed = oracle_manifest_fixtures(MODULE, names, &declared);
  int identical = 0;
  for (int i = 0; i < listed; i++) {
    const int problems = check_fixture(names[i]);
    if (problems == 0) {
      identical++;
      printf("\n  oracle: %s/%s bit-identical", MODULE, names[i]);
    } else {
      printf("\n  oracle: %s/%s DIFFERS (%d problems; details on stderr)", MODULE, names[i],
             problems);
    }
  }
  printf("\n  oracle: %s bit-identical %d of %d listed fixtures (manifest declares %d) ", MODULE,
         identical, listed, declared);
  return declared > 0 && listed == declared && identical == listed;
}

// ============================================================================
// TESTS
// ============================================================================

/**
 * @test    test_oracle_fixture_count
 * @brief   Every fixture fixtures.manifest lists for the module is present (platform independent)
 */
int test_oracle_fixture_count(void) {
  TEST_ASSERT(oracle_fixture_count_ok(MODULE),
              "fixtures.manifest must list its declared count and every listed fixture must exist");
  return TEST_PASS;
}

/**
 * @test    test_oracle_bit_identity
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its
 *          environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  counts_add = counts_mbb = counts_merge = counts_multi = 0;
  counts_add_sizes = 0;
  counts_merge_sizes = 0;
  memset(counts_merge_sizes_branch, 0, sizeof(counts_merge_sizes_branch));
  memset(counts_remnant_branch, 0, sizeof(counts_remnant_branch));
  memset(&remnant_tally, 0, sizeof(remnant_tally));
  counts_ratio_zero = counts_inert_radius = 0;
  counts_consumer_locals_validated = 0;
  counts_br06 = 0;
  counts_merge_bh_caps = 0;
  memset(&bh_h_tally, 0, sizeof(bh_h_tally));
  counts_disrupt = counts_pass = counts_join_and_pass = 0;
  counts_pass_mergers = counts_pass_disruptions = counts_pass_redirects = 0;
  counts_orphan_exclusions = 0;
  memset(&fire_tally, 0, sizeof(fire_tally));
  counts_fire_merge = counts_fire_pass = 0;
  counts_merger_wiring = counts_recheck_wiring = counts_pass_observed = 0;
  counts_mass_loading = 0;

  const int identical = run_fixtures();
  printf("\n  oracle: add_galaxies_together with BulgeSizeOn 3 %d", counts_add_sizes);
  printf("\n  oracle: deal_with_galaxy_merger with BulgeSizeOn 3 %d (major %d, minor onto a "
         "disk-dominated target %d, onto a spheroid-dominated one %d); remnant branches taken: "
         "M_tot <= 0 %d, g1 sum not positive %d, g2 sum not positive %d, R1 fallback %d, R2 "
         "fallback %d, both radii zero %d, E_final <= 0 %d; remnant and radius locals compared "
         "%d (validated %d, narrowing-limited %d, neither %d), absent %d",
         counts_merge_sizes, counts_merge_sizes_branch[0], counts_merge_sizes_branch[1],
         counts_merge_sizes_branch[2], counts_remnant_branch[0], counts_remnant_branch[1],
         counts_remnant_branch[2], counts_remnant_branch[3], counts_remnant_branch[4],
         counts_remnant_branch[5], counts_remnant_branch[6],
         remnant_tally.validated + remnant_tally.narrowing_limited + remnant_tally.not_validated,
         remnant_tally.validated, remnant_tally.narrowing_limited, remnant_tally.not_validated,
         remnant_tally.absent);
  printf("\n  oracle: add_galaxies_together %d, make_bulge_from_burst %d, "
         "deal_with_galaxy_merger %d (ratio 0: %d), disrupt_satellite_to_ICS %d, pass %d, "
         "joining then pass %d",
         counts_add, counts_mbb, counts_merge, counts_ratio_zero, counts_disrupt, counts_pass,
         counts_join_and_pass);
  printf("\n  oracle: the passes merged %d and disrupted %d satellites, redirected %d targets; "
         "%d passes with several mergers in one bin; new-orphan core-boundary exclusions %d",
         counts_pass_mergers, counts_pass_disruptions, counts_pass_redirects, counts_multi,
         counts_orphan_exclusions);
  printf("\n  oracle: carried values asserted %d (validated two-sided), binding %d, not validated "
         "%d, absent %d; remnant radius non-zero and inert %d; consumer locals validated by the "
         "oracle but not exposed %d ",
         tally.asserted, tally.binding, tally.not_validated, tally.absent, counts_inert_radius,
         counts_consumer_locals_validated);
  printf("\n  oracle: merge and pass fixtures with SFprescription 1 %d ", counts_br06);
  printf("\n  oracle: merger black-hole caps asserted through the consumer's shadow %d (max_h_bh "
         "validated %d, not validated %d, absent %d) ",
         counts_merge_bh_caps, bh_h_tally.validated, bh_h_tally.not_validated, bh_h_tally.absent);
  printf("\n  oracle: c09 deal_with_galaxy_merger %d and passes %d (FIREmodeOn 1 %d, 0 %d; FIRE "
         "values validated %d, not validated %d, absent %d; binding caps %d); merger-burst FIRE "
         "wiring asserted %d, recheck-burst %d; passes observed FIRE- and redshift-sensitive %d; "
         "MassLoading unchanged %d ",
         counts_fire_merge, counts_fire_pass, fire_tally.fire_on, fire_tally.fire_off,
         fire_tally.validated, fire_tally.not_validated, fire_tally.absent, fire_tally.binding,
         counts_merger_wiring, counts_recheck_wiring, counts_pass_observed, counts_mass_loading);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(counts_fire_merge >= 1 && counts_fire_pass >= 1 && counts_merger_wiring >= 2 &&
                  counts_recheck_wiring >= 1 && counts_pass_observed >= 1,
              "the c09 mergers and chained mergers must assert their bursts' FIRE wiring, and a "
              "recheck burst's");
  TEST_ASSERT(counts_br06 >= 1, "a merger fixture must run SFprescription 1");
  TEST_ASSERT(counts_merge_bh_caps >= 1,
              "an SFprescription 1 merger must assert its black-hole cap");
  TEST_ASSERT(counts_ratio_zero >= 1, "a ratio-0 merger must run through the transaction");
  TEST_ASSERT(counts_add_sizes >= 1, "an add_galaxies_together fixture must run BulgeSizeOn 3");
  TEST_ASSERT(counts_merge_sizes_branch[0] >= 1 && counts_merge_sizes_branch[1] >= 1 &&
                  counts_merge_sizes_branch[2] >= 1,
              "each BulgeSizeOn 3 assignment branch must run through the transaction");
  for (int k = 0; k < REMNANT_BRANCHES; k++) {
    TEST_ASSERT(counts_remnant_branch[k] >= 1, "every remnant-radius branch must be asserted");
  }
  TEST_ASSERT(counts_multi >= 1, "a pass with several mergers in one bin must be asserted");
  TEST_ASSERT(counts_disrupt >= 1 && counts_pass >= 1 && counts_join_and_pass >= 1,
              "the disruption, pass and joining fixtures must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_resolve_mergers_and_disruption SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  reset_config();
  init_memory_system(0);

  // The production run file's phases (models/sage26/input/sage26_mini-millennium.yaml)
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("satellite_mergers", MODULE, PROCESSING_MODE_FULL_HALO);
  test_phase_add("satellite_mergers", "sage26_quasar_mode", PROCESSING_MODE_PER_EVENT);
  test_phase_add("satellite_mergers", "sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  test_free_substep_phases();
  test_free_pre_timestep();
  check_memory_leaks();

  TEST_SUMMARY();
  return TEST_RESULT();
}
