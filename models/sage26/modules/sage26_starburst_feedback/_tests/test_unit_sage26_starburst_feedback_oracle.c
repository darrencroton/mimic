/**
 * @file    test_unit_sage26_starburst_feedback_oracle.c
 * @brief   Bit-exact comparison of the collisional burst transaction with SAGE26's
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_starburst_feedback (its
 * _tests/oracle/ directory): SAGE26's collisional_starburst_recipe (R/src/model_mergers.c:584-831),
 * in both modes, with R0's flags, BulgeSizeOn 0 (R0) or 3 (the burst_s3_* fixtures, R1,
 * with the fixture's Hubble_h). The test calls the shared transaction the module calls,
 * mimic_apply_collisional_starburst (shared/sage_starburst_physics.h), directly, with the
 * fixture's inputs standing in for the module's trigger: call.mass_ratio (the merger event's ratio
 * or the instability's fraction), call.mode, call.burst_to_merger_bulge (the channel SAGE26's
 * caller selects from the pre-transfer morphology), call.merger_centralgal (the burst host),
 * call.centralgal (its FoF central), call.step (the history bin), call.dt (the substep interval)
 * and call.old_disk_radius (the disk radius SAGE26's caller saved, which the instability channel's
 * radius update reads in mode 3, :783-784). The post-minor-merger instability recheck runs
 * inside the transaction, as it does in SAGE26 (:806-811), with SAGE26's TOOMRE_DISK_FACTOR 3.0
 * (R/src/model_disk_instability.c:35), DiskInstabilityOn and AGNrecipeOn from the fixture.
 *
 * Asserted bit for bit: the thirty floats of every galaxy's recorded block and the host's sixty
 * history elements; every other member of every galaxy unchanged (the FoF central's histories
 * included, and the package's StarFormationRate diagnostic, which this test leaves off with rate_dt
 * 0); every branch flag the fixture records; the presence of every carried value; and each carried
 * value where the oracle validates it two-sided or a clamp or floor binds it. Checked values that
 * are not validated are recorded, not asserted (the rule of the oracle README's "The c05 Group":
 * the float stores absorb a one-ulp change of most doubles).
 *
 * Not applied, named: call.time (the event time, which reaches no statement on R0 or R1: the
 * recheck passes it on to a burst that does not read it). call.old_disk_radius is applied; with
 * BulgeSizeOn 0 it is read only by update_instability_bulge_radius, which returns at once
 * (R/src/model_misc.c:395), and the bulge radii must stay unchanged. params.G is applied (the
 * recheck's Mcrit). The observation intermediate.burst_instability_recheck_changed_state is not a
 * port value (it records whether SAGE26's own recheck changed the state); the end state asserts its
 * effect. SFprescription 1 (the burst_r2_* fixtures, R2's flags): the bundle carries the fixture's
 * SFprescription, so the commits cap the hydrogen (S:1131-1135, S:1169-1173), the burst refreshes
 * it after the feedback (M:798-803) and the recheck's black-hole growth caps it (M:366-372); the
 * test also asserts the burst's hydrogen record bit for bit (sage26_oracle_hydrogen.h). The
 * recheck's black-hole cap is end-state invariant (its burst re-caps and refreshes the hydrogen):
 * the oracle's observation of SAGE26's own recheck growth (its capped flags and, where a cap binds,
 * the bound it stored) is asserted against the port's recheck cap locals. The recheck's own burst
 * records no locals; the end state asserts its effect. FIREmodeOn (the FIRE fixtures, routine
 * collisional_starburst_recipe_fire, R3's flags): the bundle carries the fixture's FIREmodeOn and
 * the redshift table its params.ZZ entries fill (sage26_oracle_fire.h), and the test also asserts
 * bit for bit: every recorded galaxy's MassLoading, equal to its input (a burst never writes it,
 * R/src/model_mergers.c:707-710); the burst's FIRE flags and values (oracle_c09_check_fire_record,
 * check kinds and markers included); the burst's wiring (it took the FIREmodeOn branch with the
 * host's own stored SnapNum, float Vvir and that snapshot's redshift, the fixture's, never the FoF
 * central's or a satellite's); and, where the oracle observes that the post-minor-merger recheck's
 * own burst took the FIRE branch (intermediate.burst_recheck_fire_*_sensitive 1), the port's record
 * of that nested burst: the same wiring and the host's path-invariant scaling, loadings and
 * coupling. CGMrecipeOn 1 (the burst_cgm_* fixtures, routine collisional_starburst_recipe_fire with
 * R4's flags; oracle README "The c10 Group, Part 3"): the bundle carries the fixture's CGMrecipeOn
 * and every galaxy's Regime is set from the fixture. The test also asserts each routing site
 * against the oracle's selection AND against the expected galaxy's input Regime: the burst's
 * commit (feedback.reservoir_cgm) and yield (yield_reservoir_cgm) by the FoF central's; the
 * post-minor-merger recheck's wind (recheck_wind, its two tests and its four energies bit for bit)
 * by the burst HOST's; the recheck's own burst's commit and yield (recheck_feedback_reservoir_cgm,
 * recheck_yield_reservoir_cgm) by the FoF central's. It asserts the pair the FoF central's
 * Regime does not select, the pair the host's Regime does not select (the host differs from the
 * central) and both pairs of any other galaxy unchanged against the input, and counts the oracle's
 * observations of SAGE26's recheck (intermediate.recheck_*_regime_sensitive). Outside the oracle
 * environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_cgm.h"
#include "modules/_tests/sage26_oracle_fire.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_hydrogen.h"
#include "modules/_tests/sage26_oracle_sizes.h"
#include "shared/sage_starburst_physics.h"

static const char *MODULE = "sage26_starburst_feedback";
static const char *ROUTINE = "collisional_starburst_recipe";

#define MAX_GALAXIES 4

/* SAGE26's TOOMRE_DISK_FACTOR (R/src/model_disk_instability.c:35), the recheck's factor */
#define SAGE26_TOOMRE_DISK_FACTOR 3.0

/* Carried-value check kinds (oracle README "Carried Values, Check Kinds") */
#define KIND_CONTINUATION 1
#define KIND_TAIL 2

static struct oracle_c06_carried_tally tally;
static int ratio_zero_fixtures = 0;
/* Fixtures run with BulgeSizeOn 3 */
static int sizes_fixtures = 0;
/* Fixtures run with SFprescription 1, and their hydrogen values */
static int br06_fixtures = 0;
static struct oracle_c08_local_tally h_tally;
/* SFprescription 1 bursts whose recheck grew the black hole, and the binding recheck caps whose
 * bound was asserted (oracle_c08_check_recheck_bh) */
static int recheck_growth_fixtures = 0;
static int recheck_binding = 0;
/* The FIRE fixtures' values, and the per-path wiring asserted (the burst, and the recheck's own
 * burst where the oracle observes it took the FIRE branch) */
static struct oracle_c09_tally fire_tally;
static int burst_wiring_asserted = 0;
static int recheck_wiring_asserted = 0;
static int mass_loading_asserted = 0;
/* The CGMrecipeOn 1 fixtures' assertions; the burst commits routed to each of the central's pairs;
 * the recheck winds and recheck bursts asserted */
static struct oracle_c10_site_tally cgm_tally;
static int cgm_commit_cgm_pair = 0, cgm_commit_hot_pair = 0;
static int cgm_recheck_winds = 0, cgm_recheck_bursts = 0;

/**
 * @brief Read the fixture's flags and parameters into the burst's parameter bundle
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in, struct MimicStarburstParams *p,
                            const int fire) {
  /* SFprescription 0 or 1 (with the H2 configuration for 1), BulgeSizeOn 0 or 3 with Hubble_h for 3
   * (the other flags R0's, R1's and R2's; FIREmodeOn 0 or 1 for a FIRE fixture, read by
   * oracle_c09_read_fire_config; CGMrecipeOn 0, or 0 or 1 for a FIRE routine) */
  struct MimicSageBulgeSize sizes;
  int sf_prescription = 0;
  int cgm_recipe_on = 0;
  int fire_mode_unused = 0;
  int problems = oracle_c10_require_merger_flags(in, &sizes, &sf_prescription, fire, fire,
                                                 &cgm_recipe_on, &fire_mode_unused);
  reset_config();
  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.UnitVelocity_in_cm_per_s = oracle_in_f64(in, "params.UnitVelocity_in_cm_per_s");

  memset(p, 0, sizeof(*p));
  p->feedback_reheating_epsilon = oracle_in_f64(in, "params.FeedbackReheatingEpsilon");
  p->feedback_ejection_efficiency = oracle_in_f64(in, "params.FeedbackEjectionEfficiency");
  p->recycle_fraction = oracle_in_f64(in, "params.RecycleFraction");
  p->yield = oracle_in_f64(in, "params.Yield");
  p->frac_z_leave_disk = oracle_in_f64(in, "params.FracZleaveDisk");
  p->threshold_major_merger = oracle_in_f64(in, "params.ThreshMajorMerger");
  p->energy_sn_code = oracle_in_f64(in, "params.EnergySNcode");
  p->eta_sn_code = oracle_in_f64(in, "params.EtaSNcode");
  /* R0's DiskInstabilityOn 1 and AGNrecipeOn 2, required above */
  p->disk_instability_on = 1;
  p->star_forming_disk_factor = SAGE26_TOOMRE_DISK_FACTOR;
  p->agn_recipe_on = 1;
  p->black_hole_growth_rate = oracle_in_f64(in, "params.BlackHoleGrowthRate");
  p->quasar_mode_efficiency = oracle_in_f64(in, "params.QuasarModeEfficiency");
  p->bulge_size = sizes;
  p->sf_prescription = sf_prescription;
  /* The module's bundle carries the run's CGMrecipeOn (sage26_starburst_feedback.c) */
  p->cgm_recipe_on = cgm_recipe_on;
  return problems;
}

/**
 * @brief Require every member of a non-host galaxy outside its thirty block floats unchanged
 *
 * Covers the FoF central's histories, which the routine does not write and the oracle
 * does not record for it.
 *
 * @return 1 if another member changed, else 0
 */
static int check_non_host_unchanged(const char *path, const int index,
                                    const struct GalaxyData *before, struct GalaxyData *after) {
  struct GalaxyData expected = *before;
  size_t count;
  const char *const *names = oracle_c06_block_names(&count);
  for (size_t k = 0; k < count; k++) {
    *oracle_c06_galaxy_float(&expected, names[k]) = *oracle_c06_galaxy_float(after, names[k]);
  }
  if (memcmp(&expected, after, sizeof(expected)) != 0) {
    fprintf(stderr, "oracle: %s: galaxy[%d] changed a member outside its recorded block\n", path,
            index);
    return 1;
  }
  return 0;
}

/** @brief Assert the burst's branch flags; returns the number of mismatches */
static int check_flags(struct oracle_fixture *out, const struct MimicSageStarburstLocals *b) {
  int problems = 0;
  problems += oracle_c06_check_flag(out, "burst_gas_floored", b->gas_floored);
  problems += oracle_c06_check_flag(out, "burst_stars_floored", b->stars_floored);
  problems += oracle_c06_check_flag(out, "burst_renorm_active", b->renorm_active);
  problems +=
      oracle_c06_check_flag(out, "burst_ejection_vvir_nonpositive", b->ejection_vvir_nonpositive);
  problems += oracle_c06_check_flag(out, "burst_ejected_floored", b->ejected_floored);
  problems += oracle_c06_check_flag(out, "reheat_clamp_fb_binds", b->feedback.reheat_clamp_binds);
  problems += oracle_c06_check_flag(out, "ejected_cap_binds", b->feedback.ejected_cap_binds);
  problems += oracle_c06_check_flag(out, "metals_cap_binds", b->feedback.metals_cap_binds);
  problems +=
      oracle_c06_check_flag(out, "burst_instability_recheck_ran", b->instability_recheck_ran);
  problems += oracle_c06_check_flag(out, "burst_yield_minor_branch", b->yield_minor_branch);
  /* An observation of SAGE26's own recheck, not a port value: the end state asserts it */
  oracle_exclude(out, "intermediate.burst_instability_recheck_changed_state");
  return problems;
}

/** @brief Assert the burst's carried values; returns the number of problems */
static int check_carried(struct oracle_fixture *out, const struct MimicSageStarburstLocals *b) {
  const struct MimicSageFeedbackLocals *f = &b->feedback;
  int problems = 0;
  problems += oracle_c06_check_carried(out, "eburst", KIND_TAIL, b->eburst, 0, &tally);
  /* A floor binds its value to exactly zero (:683, :686, :746-748) */
  problems += oracle_c06_check_carried(out, "gas_for_starburst", KIND_TAIL, b->gas_for_starburst,
                                       b->gas_floored == 1, &tally);
  problems += oracle_c06_check_carried(out, "stars_pre_renorm", KIND_CONTINUATION,
                                       b->stars_pre_renorm, b->stars_floored == 1, &tally);
  problems += oracle_c06_check_carried(out, "reheated_mass_pre_renorm", KIND_CONTINUATION,
                                       b->reheated_mass_pre_renorm, 0, &tally);
  problems += oracle_c06_check_carried(out, "renorm_fac", KIND_TAIL, b->renorm_fac, 0, &tally);
  problems += oracle_c06_check_carried(out, "stars", KIND_CONTINUATION, b->stars, 0, &tally);
  problems += oracle_c06_check_carried(out, "reheated_mass", KIND_CONTINUATION, b->reheated_mass, 0,
                                       &tally);
  problems += oracle_c06_check_carried(out, "ejected_mass_pre_floor", KIND_CONTINUATION,
                                       b->ejected_mass_pre_floor, 0, &tally);
  problems += oracle_c06_check_carried(out, "ejected_mass", KIND_CONTINUATION, b->ejected_mass,
                                       b->ejected_floored == 1, &tally);
  problems += oracle_c06_check_carried(out, "metallicity_sf", KIND_CONTINUATION, b->metallicity_sf,
                                       0, &tally);
  problems += oracle_c06_check_carried(out, "recycled_stars", KIND_CONTINUATION, b->recycled_stars,
                                       0, &tally);
  problems += oracle_c06_check_carried(out, "metallicity_fb", KIND_CONTINUATION, b->metallicity_fb,
                                       0, &tally);
  /* The feedback's clamp and caps bind their values to the reservoir (S:1153-1240) */
  problems +=
      oracle_c06_check_carried(out, "reheated_mass_fb_clamped", KIND_TAIL, f->reheated_mass_clamped,
                               f->reheat_clamp_binds == 1, &tally);
  problems += oracle_c06_check_carried(out, "ejected_mass_capped", KIND_TAIL,
                                       f->ejected_mass_capped, f->ejected_cap_binds == 1, &tally);
  problems += oracle_c06_check_carried(out, "metallicity_hot", KIND_TAIL, f->metallicity_selected,
                                       0, &tally);
  problems += oracle_c06_check_carried(out, "metals_hot_to_eject", KIND_TAIL, f->metals_to_eject, 0,
                                       &tally);
  problems += oracle_c06_check_carried(out, "metals_hot_to_eject_capped", KIND_TAIL,
                                       f->metals_to_eject_capped, f->metals_cap_binds == 1, &tally);
  problems += oracle_c06_check_carried(out, "FracZleaveDiskVal", KIND_TAIL,
                                       b->frac_z_leave_disk_val, 0, &tally);
  problems += oracle_c06_check_carried(out, "yield_metals_to_hot", KIND_CONTINUATION,
                                       b->yield_metals_to_hot, 0, &tally);
  return problems;
}

/**
 * @brief The FIRE record, the burst's wiring and the recheck burst's, and MassLoading
 * @return Number of mismatches
 */
static int check_fire(struct oracle_fixture *out, const int fire_mode_on, const int ngal,
                      const int host, const struct MimicSageStarburstLocals *b,
                      const struct GalaxyData *before, const struct GalaxyData *galaxies,
                      const struct Halo *halos) {
  int problems = oracle_c09_check_fire_record(out, fire_mode_on, &b->fire.l, 0, &fire_tally);
  problems += oracle_c09_check_burst_wiring(out->path, "burst", &b->fire, fire_mode_on,
                                            halos[host].SnapNum, (float)halos[host].Vvir);
  burst_wiring_asserted++;
  const int mode_sensitive = oracle_c09_observed(out, "burst_recheck_fire_mode_sensitive");
  const int z_sensitive = oracle_c09_observed(out, "burst_recheck_fire_redshift_sensitive");
  if (mode_sensitive == 1 || z_sensitive == 1) {
    /* SAGE26's recheck burst took the FIRE branch with the host's redshift: the port's must */
    problems +=
        oracle_c09_check_burst_wiring(out->path, "recheck burst", &b->recheck_fire, fire_mode_on,
                                      halos[host].SnapNum, (float)halos[host].Vvir);
    problems += oracle_c09_check_same_host_budget(out->path, "recheck burst", &b->recheck_fire.l,
                                                  &b->fire.l);
    recheck_wiring_asserted++;
  }
  /* Every galaxy of a FIRE burst fixture is recorded, an uninvolved satellite included */
  for (int i = 0; i < ngal; i++) {
    problems +=
        oracle_c09_check_mass_loading(out, "", i, before[i].MassLoading, galaxies[i].MassLoading);
    mass_loading_asserted++;
  }
  return problems;
}

/**
 * @brief The CGMrecipeOn 1 record of one burst: each routing site against the oracle's selection
 *        and the expected galaxy's input Regime, the recheck wind's tests and values, the
 *        unselected pairs unchanged against the input, and the observations counted
 * @return Number of mismatches
 */
static int check_cgm(const char *name, struct oracle_fixture *out, const int ngal, const int host,
                     const int central, const struct MimicSageStarburstLocals *b,
                     const struct GalaxyData *before, struct GalaxyData *galaxies) {
  int problems = oracle_check_i32(out, "intermediate.cgm_recipe_on", MIMIC_SAGE_CGM_RECIPE_ON);
  const int central_regime = before[central].Regime;
  const int host_regime = before[host].Regime;
  /* The burst's commit and yield: the FoF central's (M:791, S:1175-1176; M:824, M:830) */
  problems += oracle_c10_check_site(out, "burst_feedback_reservoir_cgm", b->feedback.reservoir_cgm,
                                    central_regime, &cgm_tally);
  problems += oracle_c10_check_site(out, "burst_yield_reservoir_cgm", b->yield_reservoir_cgm,
                                    central_regime, &cgm_tally);
  cgm_commit_cgm_pair += b->feedback.reservoir_cgm == 1;
  cgm_commit_hot_pair += b->feedback.reservoir_cgm == 0;
  /* The recheck (M:806-811): the oracle's gas branch is the package's recheck burst running */
  const int gas_branch = oracle_in_i32(out, "intermediate.recheck_gas_branch");
  const int package_gas_branch =
      b->instability_recheck_ran == 1 ? (b->recheck_feedback_reservoir_cgm != -1) : -1;
  if (gas_branch != package_gas_branch) {
    fprintf(stderr, "oracle: %s: the recheck's gas branch is %d in the oracle, %d in the port\n",
            name, gas_branch, package_gas_branch);
    problems++;
  }
  /* Its wind by the HOST's Regime (D:103, M:406-407), its burst by the FoF central's (D:106) */
  problems +=
      oracle_c10_check_site(out, "recheck_wind_reservoir_cgm", b->recheck_wind.reservoir_cgm,
                            gas_branch == 1 ? host_regime : -1, &cgm_tally);
  problems += oracle_c06_check_flag(out, "recheck_wind_ejects_cold", b->recheck_wind.ejects_cold);
  problems +=
      oracle_c06_check_flag(out, "recheck_wind_ejects_selected", b->recheck_wind.ejects_selected);
  problems += oracle_c10_check_site(out, "recheck_feedback_reservoir_cgm",
                                    b->recheck_feedback_reservoir_cgm,
                                    gas_branch == 1 ? central_regime : -1, &cgm_tally);
  problems +=
      oracle_c10_check_site(out, "recheck_yield_reservoir_cgm", b->recheck_yield_reservoir_cgm,
                            gas_branch == 1 ? central_regime : -1, &cgm_tally);
  const struct {
    const char *name;
    double actual;
  } values[] = {{"recheck_wind_quasar_energy", b->recheck_wind.quasar_energy},
                {"recheck_wind_cold_gas_energy", b->recheck_wind.cold_gas_energy},
                {"recheck_wind_cgm_gas_energy", b->recheck_wind.cgm_gas_energy},
                {"recheck_wind_hot_gas_energy", b->recheck_wind.hot_gas_energy}};
  for (size_t k = 0; k < sizeof(values) / sizeof(values[0]); k++) {
    problems += oracle_c10_check_value(out, values[k].name, values[k].actual, &cgm_tally);
  }
  if (gas_branch == 1) {
    cgm_recheck_winds++;
    cgm_recheck_bursts++;
    const int distinct = host != central;
    (void)oracle_c10_observed(out, "recheck_wind_host_regime_sensitive", 1, &cgm_tally);
    (void)oracle_c10_observed(out, "recheck_burst_central_regime_sensitive", 1, &cgm_tally);
    if (distinct) {
      (void)oracle_c10_observed(out, "recheck_wind_central_regime_sensitive", 0, &cgm_tally);
      (void)oracle_c10_observed(out, "recheck_burst_host_regime_sensitive", 0, &cgm_tally);
    } else {
      oracle_exclude(out, "intermediate.recheck_wind_central_regime_sensitive");
      oracle_exclude(out, "intermediate.recheck_burst_host_regime_sensitive");
    }
  }
  problems += oracle_c10_check_regimes(out, ngal, galaxies);
  /* The pairs no site of this path writes: the central's unselected pair (the commits and yields
   * select by its Regime), the host's unselected pair when it is not the central (only its own
   * recheck wind writes its pairs, by its Regime), both pairs of any other galaxy */
  for (int i = 0; i < ngal; i++) {
    if (i == central || (i == host && host != central)) {
      problems += oracle_c10_check_unselected(name, i, &before[i], &galaxies[i],
                                              oracle_c10_pair(before[i].Regime != 0), 2);
      cgm_tally.unselected++;
    } else if (i != host) {
      for (int pair = 0; pair <= 1; pair++) {
        problems += oracle_c10_check_unselected(name, i, &before[i], &galaxies[i],
                                                oracle_c10_pair(pair), 2);
        cgm_tally.unselected++;
      }
    }
  }
  cgm_tally.fixtures++;
  return problems;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  const int fire = strcmp(in.routine, ORACLE_C09_BURST_ROUTINE) == 0;
  if (oracle_load_pair(MODULE, name, fire ? ORACLE_C09_BURST_ROUTINE : ROUTINE, &in, &out) != 0) {
    return 1;
  }

  struct MimicStarburstParams params;
  int problems = setup_parameters(&in, &params, fire);
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int host = oracle_in_i32(&in, "call.merger_centralgal");
  const int central = oracle_in_i32(&in, "call.centralgal");
  if (ngal < 1 || ngal > MAX_GALAXIES || host < 0 || host >= ngal || central < 0 ||
      central >= ngal) {
    fprintf(stderr,
            "oracle: %s: call.ngal, call.merger_centralgal or call.centralgal out of "
            "range\n",
            name);
    return problems + 1;
  }
  const double mass_ratio = oracle_in_f64(&in, "call.mass_ratio");
  const int mode = oracle_in_i32(&in, "call.mode");
  const int burst_to_merger_bulge = oracle_in_i32(&in, "call.burst_to_merger_bulge");
  const int step = oracle_in_i32(&in, "call.step");
  const double dt = oracle_in_f64(&in, "call.dt");
  const double old_disk_radius = oracle_in_f64(&in, "call.old_disk_radius");
  /* Unread on R0 and R1 (see the file comment) */
  (void)oracle_in_f64(&in, "call.time");

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    problems += oracle_c06_in_galaxy(&in, i, &halos[i], &galaxies[i]);
  }
  /* FIREmodeOn, the stored snapshots' redshifts (the bundle's table) and MassLoading */
  int fire_mode_on = MIMIC_SAGE_FIRE_MODE_OFF;
  if (fire) {
    int snapnum[MAX_GALAXIES];
    problems += oracle_c09_read_fire_config(&in, ngal, snapnum, &fire_mode_on);
    oracle_c09_read_mass_loading(&in, ngal, galaxies);
    params.fire_mode_on = fire_mode_on;
    params.redshift_config = &MimicConfig;
  }
  /* Every galaxy's Regime, which the CGMrecipeOn 1 routing reads */
  if (params.cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
    problems += oracle_c10_read_regimes(&in, ngal, galaxies);
  }
  if (problems != 0) {
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));
  if (mode == 0 && oracle_f64_to_bits(mass_ratio) == oracle_f64_to_bits(0.0)) {
    ratio_zero_fixtures++;
  }

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halos[central];

  /* rate_dt 0: the package's StarFormationRate diagnostic is left off and must not change */
  struct MimicSageStarburstLocals locals;
  if (mimic_apply_collisional_starburst(mass_ratio, &halos[host], &halos[central], mode,
                                        burst_to_merger_bulge, old_disk_radius, 0.0, step, dt, &ctx,
                                        &params, &locals) != 0) {
    fprintf(stderr, "oracle: %s: the burst refused its host\n", name);
    return problems + 1;
  }
  if (params.bulge_size.bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
    sizes_fixtures++;
  }
  if (params.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    br06_fixtures++;
  }

  for (int i = 0; i < ngal; i++) {
    problems += oracle_c06_check_galaxy(&out, "", i, &galaxies[i], i == host);
    if (i == host) {
      problems += oracle_c06_check_rest_unchanged(name, i, &before[i], &galaxies[i]);
    } else {
      problems += check_non_host_unchanged(name, i, &before[i], &galaxies[i]);
    }
  }
  problems += check_flags(&out, &locals);
  problems += check_carried(&out, &locals);
  problems += oracle_c08_check_burst_hydrogen(&out, params.sf_prescription, &locals, &h_tally);
  if (params.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06 &&
      locals.recheck_bh_caps.tracks_h2 != -1) {
    recheck_growth_fixtures++;
  }
  problems += oracle_c08_check_recheck_bh(&out, params.sf_prescription, &locals.recheck_bh_caps,
                                          &recheck_binding);
  if (fire) {
    problems += check_fire(&out, fire_mode_on, ngal, host, &locals, before, galaxies, halos);
  }
  if (params.cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
    problems += check_cgm(name, &out, ngal, host, central, &locals, before, galaxies);
  }

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

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
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  ratio_zero_fixtures = 0;
  sizes_fixtures = 0;
  br06_fixtures = 0;
  memset(&h_tally, 0, sizeof(h_tally));
  recheck_growth_fixtures = 0;
  recheck_binding = 0;
  memset(&fire_tally, 0, sizeof(fire_tally));
  burst_wiring_asserted = 0;
  recheck_wiring_asserted = 0;
  mass_loading_asserted = 0;
  memset(&cgm_tally, 0, sizeof(cgm_tally));
  cgm_commit_cgm_pair = 0;
  cgm_commit_hot_pair = 0;
  cgm_recheck_winds = 0;
  cgm_recheck_bursts = 0;
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: carried values asserted %d (validated two-sided), binding %d, "
         "not validated %d, absent %d; ratio-0 merger bursts through the direct call %d ",
         tally.asserted, tally.binding, tally.not_validated, tally.absent, ratio_zero_fixtures);
  printf("\n  oracle: bursts with BulgeSizeOn 3 %d ", sizes_fixtures);
  printf("\n  oracle: bursts with SFprescription 1 %d; c08 values validated %d, "
         "narrowing-limited %d, not validated %d, absent %d ",
         br06_fixtures, h_tally.validated, h_tally.narrowing_limited, h_tally.not_validated,
         h_tally.absent);
  printf("\n  oracle: SFprescription 1 rechecks that grew the black hole %d, their binding "
         "caps asserted at the bound %d ",
         recheck_growth_fixtures, recheck_binding);
  printf("\n  oracle: c09 bursts with FIREmodeOn 1 %d, with 0 %d; c09 values validated %d, "
         "not validated %d, absent %d (continuation %d, tail %d), binding caps %d ",
         fire_tally.fire_on, fire_tally.fire_off, fire_tally.validated, fire_tally.not_validated,
         fire_tally.absent, fire_tally.continuation, fire_tally.tail, fire_tally.binding);
  printf("\n  oracle: c09 wiring asserted: bursts %d, recheck bursts %d; MassLoading unchanged %d ",
         burst_wiring_asserted, recheck_wiring_asserted, mass_loading_asserted);
  oracle_c10_print_site_tally(&cgm_tally);
  printf("\n  oracle: c10 burst commits into the central's CGM pair %d, its hot pair %d; recheck "
         "winds asserted %d, recheck bursts %d ",
         cgm_commit_cgm_pair, cgm_commit_hot_pair, cgm_recheck_winds, cgm_recheck_bursts);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(cgm_commit_cgm_pair >= 1 && cgm_commit_hot_pair >= 1 && cgm_recheck_winds >= 1,
              "CGMrecipeOn 1 bursts committing to each pair and a recheck wind must be asserted");
  TEST_ASSERT(fire_tally.fire_on >= 1 && fire_tally.fire_off >= 1,
              "the c09 bursts must run FIREmodeOn 1 and its 0 twin");
  TEST_ASSERT(recheck_wiring_asserted >= 1,
              "a recheck burst's FIRE wiring must be asserted where the oracle observes it");
  TEST_ASSERT(sizes_fixtures >= 1, "a burst fixture must run BulgeSizeOn 3");
  TEST_ASSERT(br06_fixtures >= 1, "a burst fixture must run SFprescription 1");
  TEST_ASSERT(recheck_growth_fixtures >= 1 && recheck_binding >= 1,
              "an SFprescription 1 recheck must grow the black hole with a binding cap");
  TEST_ASSERT(ratio_zero_fixtures >= 1, "the ratio-0 merger burst fixture must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_starburst_feedback SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
