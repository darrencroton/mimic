/**
 * @file    sage26_oracle_fire.h
 * @brief   Oracle-test helpers for the FIRE supernova feedback (routine
 *          starformation_and_feedback_fire)
 *
 * Adds, beside the shared fixture reader sage26_oracle_fixture.h (which stays as it is), what the
 * oracle tests of the FIRE fixtures need (oracle README "The c09 Group"):
 *
 * - oracle_c09_read_fire_config(): a fixture's params.FIREmodeOn (0 or 1), each galaxy's stored
 *   SnapNum and the redshift params.ZZ[<s>] of each distinct one, applied to MimicConfig's ZZ
 *   table (every other entry NaN, so no unread redshift can be used) and to the package's required
 *   FIREmodeOn parameter;
 * - oracle_c09_replay_sn(): the SN module's FIRE budget (sage26_calculate_supernova_feedback) on
 *   the state it receives, through the shared helpers with their locals, so a test can compare
 *   the locals with the oracle's and the budgets with process();
 * - oracle_c09_check_fire_record(): the FIRE branch flags and values, each value's markers checked
 *   (oracle README "The c06 Group", with its check kind, and "The c07 Group", with its narrowing
 *   limitation) and compared bit for bit whatever its validation status; a binding cap's value is
 *   asserted equal to its bound.
 * - the burst (routines collisional_starburst_recipe_fire, deal_with_galaxy_merger_fire,
 *   merger_pass_fire, check_disk_instability_fire and the gas-unstable
 *   starformation_and_feedback_fire): oracle_c09_check_burst_wiring(), the per-path wiring
 *   assertion that a burst took the FIRE branch with its host's own stored SnapNum, float
 *   Vvir and the fixture's redshift of that snapshot; oracle_c09_read_mass_loading() and
 *   oracle_c09_check_mass_loading(), the invariant MassLoading of a burst's galaxies; and
 *   oracle_c09_observed(), an observation of the oracle's (intermediate.*_sensitive).
 *
 * Test-only: included by module oracle tests, never by package code.
 */

#ifndef SAGE26_ORACLE_FIRE_H
#define SAGE26_ORACLE_FIRE_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "include/globals.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_hydrogen.h"
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_merger_physics.h"
#include "shared/sage_starburst_physics.h"
#include "shared/sage_sfr_history.h"
#include "shared/time_parity.h"

/** @brief The routine of the FIRE fixtures */
#define ORACLE_C09_ROUTINE "starformation_and_feedback_fire"

/** @brief The redshift table the FIRE tests give the run: mini-Millennium's 64 snapshots */
#define ORACLE_C09_SNAPLISTLEN 64

/**
 * @brief Read a FIRE fixture's FIREmodeOn, stored snapshots and their redshifts
 *
 * FIREmodeOn must be 0 or 1 (the package's required parameter is set to it). Each galaxy's
 * galaxy[i].SnapNum is stored in snapnum[i]; it must lie in the table; params.ZZ[<s>] is read
 * once per distinct snapshot into MimicConfig.ZZ, the rest of the table is NaN and
 * MimicConfig.Snaplistlen is ORACLE_C09_SNAPLISTLEN.
 *
 * @return Number of problems
 */
static inline int oracle_c09_read_fire_config(struct oracle_fixture *in, const int ngal,
                                              int *snapnum, int *fire_mode_on) {
  int problems = 0;
  *fire_mode_on = oracle_in_i32(in, "params.FIREmodeOn");
  if (*fire_mode_on != MIMIC_SAGE_FIRE_MODE_OFF && *fire_mode_on != MIMIC_SAGE_FIRE_MODE_ON) {
    fprintf(stderr, "oracle: %s: params.FIREmodeOn %d is neither 0 nor 1\n", in->path,
            *fire_mode_on);
    problems++;
  }
  for (int s = 0; s < ABSOLUTEMAXSNAPS; s++) {
    MimicConfig.ZZ[s] = NAN;
  }
  MimicConfig.Snaplistlen = ORACLE_C09_SNAPLISTLEN;
  for (int i = 0; i < ngal; i++) {
    snapnum[i] = oracle_in_i32(in, oracle_galaxy_field(i, "SnapNum"));
    if (snapnum[i] < 0 || snapnum[i] >= ORACLE_C09_SNAPLISTLEN) {
      fprintf(stderr, "oracle: %s: galaxy[%d].SnapNum %d is outside the table\n", in->path, i,
              snapnum[i]);
      return problems + 1;
    }
  }
  for (int i = 0; i < ngal; i++) {
    int first = 1;
    for (int j = 0; j < i; j++) {
      first = first && snapnum[j] != snapnum[i];
    }
    if (first) {
      char field[ORACLE_MAX_NAME];
      snprintf(field, sizeof(field), "params.ZZ[%d]", snapnum[i]);
      MimicConfig.ZZ[snapnum[i]] = oracle_in_f64(in, field);
    }
  }
  problems += oracle_set_model_param("FIREmodeOn", (double)*fire_mode_on) != 0;
  return problems;
}

/** @brief One replay of the SN module's budget on the state compute_sn_feedback receives */
struct oracle_c09_sn_replay {
  double stars;                 /**< after the renormalisation */
  double reheated_pre;          /**< before the renormalisation */
  double reheated;              /**< after the renormalisation */
  double ejected;               /**< after the floor */
  float mass_loading;           /**< the float MassLoading the FIRE path stores (FIREmodeOn 1) */
  int renorm_active;            /**< the renormalisation ran (S:775) */
  struct MimicSageFireLocals l; /**< the FIRE locals (NAN and -1 for FIREmodeOn 0) */
};

/**
 * @brief Replay sage26_calculate_supernova_feedback's FIREmodeOn 1 budget through the shared
 *        helpers, in the module's (SAGE26's, S:739-792) order, with their locals
 *
 * @param stars_in   NewStellarMass as star formation leaves it (S:1048-1051)
 * @param cold_gas   The forming galaxy's ColdGas (read by the renormalisation, S:775-776)
 * @param vvir       Its own Vvir as SAGE26's float
 * @param redshift   ZZ[SnapNum] of its stored snapshot
 */
static inline void oracle_c09_replay_sn(const double stars_in, const float cold_gas,
                                        const float vvir, const double redshift,
                                        const double epsilon, const double ejection_efficiency,
                                        const double eta_sn_code, const double energy_sn_code,
                                        struct oracle_c09_sn_replay *r) {
  mimic_sage_fire_locals_init(&r->l);
  double stars = stars_in;
  const double fire_scaling = mimic_sage_fire_scaling(vvir, redshift, &r->l);
  const double eta = mimic_sage_capped_eta_reheat(epsilon * fire_scaling, vvir, eta_sn_code,
                                                  energy_sn_code, &r->l);
  r->mass_loading = (float)eta;
  double reheated = eta * stars;
  r->reheated_pre = reheated;
  r->renorm_active = (stars + reheated) > cold_gas && (stars + reheated) > 0.0;
  if (r->renorm_active) {
    const double fac = cold_gas / (stars + reheated);
    stars *= fac;
    reheated *= fac;
  }
  double ejected = 0.0;
  if (vvir > 0.0) {
    const double coupling = mimic_sage_sn_energy_coupling(fire_scaling, ejection_efficiency, &r->l);
    ejected = mimic_sage_fire_energy_ejection(coupling, stars, reheated, vvir, eta_sn_code,
                                              energy_sn_code, &r->l);
  }
  if (ejected < 0.0) {
    ejected = 0.0;
  }
  r->stars = stars;
  r->reheated = reheated;
  r->ejected = ejected;
}

/** @brief Counters of the FIRE assertions a test made, for its report */
struct oracle_c09_tally {
  int validated;         /**< validated two-sided */
  int narrowing_limited; /**< its only path is the MassLoading narrowing: not validated, by name */
  int not_validated;     /**< neither (a one-ulp change is absorbed by the float stores) */
  int absent;            /**< the path does not form it (and the port agrees) */
  int binding;           /**< a binding cap's value asserted equal to its bound */
  int continuation;      /**< present and continuation-checked */
  int tail;              /**< present and tail-checked */
  int fire_on;           /**< fixtures with FIREmodeOn 1 */
  int fire_off;          /**< fixtures with FIREmodeOn 0 (MassLoading asserted unchanged) */
};

/**
 * @brief Check one FIRE value: markers, check kind, narrowing limitation, value bit for bit
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c09_check_value(struct oracle_fixture *out, const char *name,
                                         const double actual, const int kind,
                                         const int narrowing_limited,
                                         struct oracle_c09_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != (isnan(actual) ? 1 : 0)) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", isnan(actual) ? "absent" : "present");
    return 1;
  }
  if (absent) {
    tally->absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  problems += oracle_check_i32(out, field, 1);
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  problems += oracle_check_i32(out, field, kind);
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s_validated disagrees with its two directions\n", out->path,
            name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s_narrowing_limited", name);
  problems += oracle_check_i32(out, field, narrowing_limited);
  if (narrowing_limited && validated) {
    fprintf(stderr, "oracle: %s: %s is narrowing-limited but validated two-sided\n", out->path,
            name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  problems += oracle_check_f64(out, field, actual);
  if (kind == 1) {
    tally->continuation++;
  } else {
    tally->tail++;
  }
  if (validated) {
    tally->validated++;
  } else if (narrowing_limited) {
    tally->narrowing_limited++;
  } else {
    tally->not_validated++;
  }
  return problems;
}

/**
 * @brief Check the FIRE record of one starformation_and_feedback_fire call against the port
 *
 * @param fire_mode_on  The fixture's FIREmodeOn (intermediate.fire_mode)
 * @param l             The port's FIRE locals (oracle_c09_replay_sn, or initialised for 0)
 * @param zero_stars    1 if no stars formed (stars_pre_renorm 0): the loading's values then
 *                      reach the state only through MassLoading (narrowing-limited)
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c09_check_fire_record(struct oracle_fixture *out, const int fire_mode_on,
                                               const struct MimicSageFireLocals *l,
                                               const int zero_stars,
                                               struct oracle_c09_tally *tally) {
  int problems = oracle_check_i32(out, "intermediate.fire_mode", fire_mode_on);
  const struct {
    const char *name;
    int value;
  } flags[] = {{"fire_scaling_valid", l->scaling_valid},
               {"fire_vc_floored", l->vc_floored},
               {"fire_high_velocity", l->high_velocity},
               {"eta_cap_vvir_nonpositive", l->eta_cap_vvir_nonpositive},
               {"eta_cap_binds", l->eta_cap_binds},
               {"coupling_capped", l->coupling_capped},
               {"fire_energy_ejects", l->energy_ejects}};
  for (size_t k = 0; k < sizeof(flags) / sizeof(flags[0]); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s", flags[k].name);
    problems += oracle_check_i32(out, field, flags[k].value);
  }
  /* Check kinds (oracle README "The c09 Group"): fire_scaling and eta_raw are passed to SAGE26's
   * sn_energy_coupling and capped_eta_reheat (continuation, 1); the rest are tail-checked (2) */
  const struct {
    const char *name;
    double value;
    int kind;
    int via_mass_loading;
  } values[] = {{"fire_redshift", l->redshift, 2, 1},
                {"fire_v_term", l->v_term, 2, 1},
                {"fire_scaling", l->scaling, 1, 1},
                {"eta_raw", l->eta_raw, 1, 1},
                {"eta_max", l->eta_max, 2, 1},
                {"eta", l->eta, 2, 1},
                {"eps_eff", l->eps_eff, 2, 0},
                {"coupling", l->coupling, 2, 0},
                {"E_FB", l->e_fb, 2, 0},
                {"E_lift", l->e_lift, 2, 0}};
  for (size_t k = 0; k < sizeof(values) / sizeof(values[0]); k++) {
    problems += oracle_c09_check_value(out, values[k].name, values[k].value, values[k].kind,
                                       values[k].via_mass_loading && zero_stars, tally);
  }
  /* Where a cap binds its value equals the bound (the flags were compared above) */
  if (l->eta_cap_binds == 1) {
    tally->binding++;
    problems += oracle_f64_to_bits(l->eta) != oracle_f64_to_bits(l->eta_max);
  }
  if (l->coupling_capped == 1) {
    tally->binding++;
    problems +=
        oracle_f64_to_bits(l->coupling) != oracle_f64_to_bits(MIMIC_SAGE_MAX_SN_ENERGY_COUPLING);
  }
  if (fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    tally->fire_on++;
  } else {
    tally->fire_off++;
  }
  return problems;
}

// ============================================================================
// The burst's FIRE budget (R/src/model_mergers.c:690-752)
// ============================================================================

/** @brief The routines that carry the burst's FIRE budget */
#define ORACLE_C09_BURST_ROUTINE "collisional_starburst_recipe_fire"
#define ORACLE_C09_MERGER_ROUTINE "deal_with_galaxy_merger_fire"
#define ORACLE_C09_PASS_ROUTINE "merger_pass_fire"
#define ORACLE_C09_INSTABILITY_ROUTINE "check_disk_instability_fire"

/**
 * @brief The per-path FIRE wiring assertion: the burst a path ran took the branch of the fixture's
 *        FIREmodeOn and, with FIREmodeOn 1, read its host's own stored SnapNum, its float Vvir and
 *        the redshift of that snapshot
 *
 * Each expectation is the fixture's input, independent of the port: host_snapnum and host_vvir are
 * the host's galaxy[<i>].SnapNum and Vvir, and the redshift is MimicConfig.ZZ[host_snapnum], the
 * fixture's params.ZZ entry (oracle_c09_read_fire_config). A full end-state comparison may not show
 * which galaxy's values a burst received; this record does.
 *
 * @param site  The path, for the message ("merger burst", "recheck burst", ...)
 * @return Number of mismatches
 */
static inline int oracle_c09_check_burst_wiring(const char *path, const char *site,
                                                const struct MimicSageBurstFireRecord *r,
                                                const int fire_mode_on, const int host_snapnum,
                                                const float host_vvir) {
  int problems = 0;
  if (r->fire_mode != fire_mode_on) {
    fprintf(stderr, "oracle: %s: the %s took FIREmodeOn %d, the fixture's is %d\n", path, site,
            r->fire_mode, fire_mode_on);
    return 1;
  }
  if (fire_mode_on != MIMIC_SAGE_FIRE_MODE_ON) {
    return 0;
  }
  if (r->snapnum != host_snapnum) {
    fprintf(stderr, "oracle: %s: the %s read SnapNum %d, its host's is %d\n", path, site,
            r->snapnum, host_snapnum);
    problems++;
  }
  if (oracle_f64_to_bits(r->vvir) != oracle_f64_to_bits((double)host_vvir)) {
    fprintf(stderr, "oracle: %s: the %s read Vvir %.9g, its host's float is %.9g\n", path, site,
            r->vvir, (double)host_vvir);
    problems++;
  }
  if (host_snapnum < 0 || host_snapnum >= ORACLE_C09_SNAPLISTLEN ||
      oracle_f64_to_bits(r->l.redshift) != oracle_f64_to_bits(MimicConfig.ZZ[host_snapnum])) {
    fprintf(stderr, "oracle: %s: the %s read redshift %.17g, not its host's ZZ[%d]\n", path, site,
            r->l.redshift, host_snapnum);
    problems++;
  }
  return problems;
}

/**
 * @brief The merger family's flags of a FIRE burst fixture: oracle_c08_require_merger_flags
 *        with FIREmodeOn left to oracle_c09_read_fire_config (0 or 1; SAGE26's
 *        input/millennium.par:55 sets 1)
 * @return Number of problems
 */
static inline int oracle_c09_require_merger_flags(struct oracle_fixture *in,
                                                  struct MimicSageBulgeSize *sizes,
                                                  int *sf_prescription) {
  struct oracle_c08_sf_config cfg;
  int problems = oracle_c08_read_sf_config(in, 0, &cfg);
  *sf_prescription = cfg.sf_prescription;
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  problems += oracle_c07_read_sizes(in, sizes);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);
  return problems;
}

/** @brief Each galaxy's input MassLoading (the invariant field of a burst fixture) */
static inline void oracle_c09_read_mass_loading(struct oracle_fixture *in, const int ngal,
                                                struct GalaxyData *galaxies) {
  for (int i = 0; i < ngal; i++) {
    galaxies[i].MassLoading = oracle_in_f32(in, oracle_galaxy_field(i, "MassLoading"));
  }
}

/**
 * @brief A recorded galaxy's MassLoading against the oracle's and its own input: a burst never
 *        writes it (R/src/model_mergers.c:707-710)
 * @return Number of mismatches
 */
static inline int oracle_c09_check_mass_loading(struct oracle_fixture *out, const char *prefix,
                                                const int index, const float input,
                                                const float actual) {
  int problems =
      oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, index, "MassLoading"), actual);
  if (oracle_f32_to_bits(actual) != oracle_f32_to_bits(input)) {
    fprintf(stderr, "oracle: %s: galaxy[%d].MassLoading changed from its input\n", out->path,
            index);
    problems++;
  }
  return problems;
}

/** @brief An oracle observation (an i32 -1, 0 or 1), consumed */
static inline int oracle_c09_observed(struct oracle_fixture *out, const char *name) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  return oracle_in_i32(out, field);
}

/**
 * @brief The recheck's or instability's own burst against the outer burst of the same host: the
 *        path-invariant FIRE values (the scaling, the raw and capped loading and the coupling
 *        depend only on the host's Vvir and redshift and the parameters)
 * @return Number of mismatches
 */
static inline int oracle_c09_check_same_host_budget(const char *path, const char *site,
                                                    const struct MimicSageFireLocals *nested,
                                                    const struct MimicSageFireLocals *outer) {
  const double pairs[][2] = {{nested->scaling, outer->scaling},
                             {nested->eta_raw, outer->eta_raw},
                             {nested->eta, outer->eta},
                             {nested->coupling, outer->coupling}};
  int problems = 0;
  for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
    if (oracle_f64_to_bits(pairs[k][0]) != oracle_f64_to_bits(pairs[k][1])) {
      fprintf(stderr, "oracle: %s: the %s's FIRE value %zu differs from its host's\n", path, site,
              k);
      problems++;
    }
  }
  return problems;
}

/** @brief The most galaxies a replay copies */
#define ORACLE_C09_REPLAY_GALAXIES 4

/**
 * @brief A replay of sage26_starburst_feedback's call of the shared transaction, on copies of the
 *        galaxies and halos, with the transaction's locals (the replay must end in the state
 *        process() produced, so its locals are process()'s)
 */
struct oracle_c09_burst_replay {
  struct GalaxyData galaxies[ORACLE_C09_REPLAY_GALAXIES];
  struct Halo halos[ORACLE_C09_REPLAY_GALAXIES];
  struct MimicSageStarburstLocals locals;
  int ngal;
  int status; /* the transaction's return value */
};

/**
 * @brief The parameter bundle sage26_starburst_feedback's process() builds, from a fixture's
 *        parameters (SAGE26's names) and the test's configuration
 *
 * recheck_on stands for the module's configured post-minor-merger recheck and its black-hole
 * growth (sage26_disk_instability and the quasar event consumer in the run); a mode-1 burst
 * never reads them.
 */
static inline void oracle_c09_burst_bundle(struct oracle_fixture *in,
                                           const struct MimicSageBulgeSize *sizes,
                                           const int sf_prescription, const int fire_mode_on,
                                           const int recheck_on, struct MimicStarburstParams *p) {
  memset(p, 0, sizeof(*p));
  p->feedback_reheating_epsilon = oracle_in_f64(in, "params.FeedbackReheatingEpsilon");
  p->feedback_ejection_efficiency = oracle_in_f64(in, "params.FeedbackEjectionEfficiency");
  p->recycle_fraction = oracle_in_f64(in, "params.RecycleFraction");
  p->yield = oracle_in_f64(in, "params.Yield");
  p->frac_z_leave_disk = oracle_in_f64(in, "params.FracZleaveDisk");
  p->threshold_major_merger = oracle_in_f64(in, "params.ThreshMajorMerger");
  p->energy_sn_code = oracle_in_f64(in, "params.EnergySNcode");
  p->eta_sn_code = oracle_in_f64(in, "params.EtaSNcode");
  p->disk_instability_on = recheck_on;
  p->star_forming_disk_factor =
      3.0; /* SAGE26's TOOMRE_DISK_FACTOR, R/src/model_disk_instability.c:35 */
  p->agn_recipe_on = recheck_on;
  p->black_hole_growth_rate = recheck_on ? oracle_in_f64(in, "params.BlackHoleGrowthRate") : 0.0;
  p->quasar_mode_efficiency = recheck_on ? oracle_in_f64(in, "params.QuasarModeEfficiency") : 0.0;
  p->bulge_size = *sizes;
  p->sf_prescription = sf_prescription;
  p->fire_mode_on = fire_mode_on;
  p->redshift_config = &MimicConfig;
}

/** @brief Copy the galaxies and halos, each copied halo pointing at its copied galaxy */
static inline void oracle_c09_replay_copy(struct oracle_c09_burst_replay *r,
                                          const struct GalaxyData *galaxies,
                                          const struct Halo *halos, const int ngal) {
  r->ngal = ngal;
  memcpy(r->galaxies, galaxies, sizeof(struct GalaxyData) * (size_t)ngal);
  memcpy(r->halos, halos, sizeof(struct Halo) * (size_t)ngal);
  for (int i = 0; i < ngal; i++) {
    r->halos[i].galaxy = &r->galaxies[i];
  }
  mimic_sage_starburst_locals_reset(&r->locals);
  r->status = -1;
}

/**
 * @brief Replay the instability channel's burst (process_by_galaxy, mode 1, the instability
 *        channel, the galaxy's current DiskScaleRadius as old_disk_radius) as the module calls it,
 *        its FoF central the galaxy itself for a Type 0 and centralgal otherwise
 */
static inline void oracle_c09_replay_instability_burst(struct oracle_c09_burst_replay *r,
                                                       const struct GalaxyData *galaxies,
                                                       const struct Halo *halos, const int ngal,
                                                       const int host, const int centralgal,
                                                       const struct ModuleContext *ctx,
                                                       const struct MimicStarburstParams *p) {
  oracle_c09_replay_copy(r, galaxies, halos, ngal);
  const int central = r->halos[host].Type == 0 ? host : centralgal;
  double sub_dt = 0.0;
  (void)mimic_object_substep_dt(&r->halos[host], ctx, &sub_dt);
  const int bin = mimic_sage_sfr_step_bin(ctx->substep_number, ctx->num_substeps);
  const struct GalaxyData *g = &r->galaxies[host];
  r->status = mimic_apply_collisional_starburst(
      g->UnstableDiskGasFraction, &r->halos[host], &r->halos[central], 1, 0,
      (double)g->DiskScaleRadius, r->halos[host].dT, bin, sub_dt, ctx, p, &r->locals);
}

/**
 * @brief Replay the merger channel's burst (process_per_event, mode 0) as the module calls it: the
 *        event's ratio and substep interval, the channel from the target's pre-merger morphology,
 *        the target's DiskScaleRadius as old_disk_radius
 */
static inline void oracle_c09_replay_merger_burst(struct oracle_c09_burst_replay *r,
                                                  const struct GalaxyData *galaxies,
                                                  const struct Halo *halos, const int ngal,
                                                  const int target, const int centralgal,
                                                  const struct ModuleContext *ctx,
                                                  const struct MimicStarburstParams *p) {
  oracle_c09_replay_copy(r, galaxies, halos, ngal);
  const struct ModuleEvent *event = ctx->active_event;
  const int bin = mimic_sage_sfr_step_bin(ctx->substep_number, ctx->num_substeps);
  const struct GalaxyData *g = &r->galaxies[target];
  const int channel = mimic_sage_merger_burst_channel(event->value0, p->threshold_major_merger,
                                                      g->MergerDiskDominated);
  r->status = mimic_apply_collisional_starburst(
      event->value0, &r->halos[target], &r->halos[centralgal], 0, channel,
      (double)g->DiskScaleRadius, r->halos[target].dT, bin, event->value1, ctx, p, &r->locals);
}

/**
 * @brief Require the replay to have ended in process()'s state, every member of every galaxy
 * @return Number of mismatches
 */
static inline int oracle_c09_replay_matches(const char *path, const char *site,
                                            const struct oracle_c09_burst_replay *r,
                                            const struct GalaxyData *galaxies) {
  if (r->status != 0) {
    fprintf(stderr, "oracle: %s: the %s replay refused its host\n", path, site);
    return 1;
  }
  if (memcmp(r->galaxies, galaxies, sizeof(struct GalaxyData) * (size_t)r->ngal) != 0) {
    fprintf(stderr, "oracle: %s: the %s replay differs from process()\n", path, site);
    return 1;
  }
  return 0;
}

#endif /* SAGE26_ORACLE_FIRE_H */
