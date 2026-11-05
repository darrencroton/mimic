/**
 * @file    sage26_oracle_fire.h
 * @brief   Oracle-test helpers for the c09 FIRE supernova feedback (routine
 *          starformation_and_feedback_fire)
 *
 * Adds, beside the shared fixture reader sage26_oracle_fixture.h (which stays as it is), what the
 * oracle tests of the c09 fixtures need (oracle README "The c09 Group"):
 *
 * - oracle_c09_read_fire_config(): a fixture's params.FIREmodeOn (0 or 1), each galaxy's stored
 *   SnapNum and the redshift params.ZZ[<s>] of each distinct one, applied to MimicConfig's ZZ
 *   table (every other entry NaN, so no unread redshift can be used) and to the package's required
 *   FIREmodeOn parameter;
 * - oracle_c09_replay_sn(): the SN module's FIRE budget (sage26_calculate_supernova_feedback) on
 *   the state it receives, through the shared helpers with their locals, so a test can compare
 *   the locals with the oracle's and the budgets with process();
 * - oracle_c09_check_fire_record(): the c09 branch flags and values, each value's markers checked
 *   (c06's, with its check kind, and c07's narrowing limitation) and compared bit for bit
 *   whatever its validation status; a binding cap's value is asserted equal to its bound.
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
#include "shared/sage_fire_feedback_physics.h"

/** @brief The routine of the c09 fixtures */
#define ORACLE_C09_ROUTINE "starformation_and_feedback_fire"

/** @brief The redshift table the c09 tests give the run: mini-Millennium's 64 snapshots */
#define ORACLE_C09_SNAPLISTLEN 64

/**
 * @brief Read a c09 fixture's FIREmodeOn, stored snapshots and their redshifts
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

/** @brief Counters of the c09 assertions a test made, for its report */
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
 * @brief Check one c09 value: markers, check kind, narrowing limitation, value bit for bit
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
 * @brief Check the c09 record of one starformation_and_feedback_fire call against the port
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

#endif /* SAGE26_ORACLE_FIRE_H */
