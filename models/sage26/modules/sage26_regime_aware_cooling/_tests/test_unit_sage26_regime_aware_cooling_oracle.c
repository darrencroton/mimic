/**
 * @file    test_unit_sage26_regime_aware_cooling_oracle.c
 * @brief   Bit-exact comparison of the regime-aware cooling transaction with SAGE26's
 *          cooling_recipe_regime_aware (C = R/src/model_cooling_heating.c:420-506)
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_regime_aware_cooling (its
 * _tests/oracle/ directory). Each fixture is a direct call of SAGE26's public
 * cooling_recipe_regime_aware with CGMrecipeOn 1 (oracle README, "The c11 Group"); the test runs
 * the module's transaction, sage26_regime_aware_cooling_run(), on the fixture's galaxy and
 * asserts bit for bit:
 *
 * - the return value (total_cooling) and every recorded galaxy field (the sixteen of that group).
 *   SAGE26 holds its mdot rates in the galaxy's mdot_cool and mdot_stream; the transaction keeps
 *   them in mdot_cool_raw and mdot_stream_raw, so the harness seeds the raw fields with the
 *   fixture's input mdot values and compares them where SAGE26's fields are recorded;
 * - the wrapper's flags (the branch taken, the reset, the restoration, the application caps), the
 *   saved float hot_tcool and its locals (each returned and applied mass, both metallicities, the
 *   total), and every flag and local of the nested hot and CGM calls (intermediate.hot.* and
 *   intermediate.cgm.*), from the transaction's trace;
 * - where an application cap binds, the applied mass equal to the reservoir it was capped to; the
 *   recipes' own binding caps likewise.
 *
 * Every local's check and validation markers are consumed and tallied (oracle_c11_report). The
 * cooling tables are loaded once by sage26_calculate_cooling_budget's init(). Outside the oracle
 * environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_cooling.h"
#include "modules/sage26_regime_aware_cooling/sage26_regime_aware_cooling.h"
#include "shared/sage_constants.h"

extern int sage26_calculate_cooling_budget_init(void);
extern int sage26_calculate_cooling_budget_cleanup(void);

static const char *MODULE = "sage26_regime_aware_cooling";

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const char *const AGN_FLAG_NAMES[] = {
    "agn_suppressed",   "agn_saturated",  "agn_reservoir_positive", "eddington_capped",
    "accretion_capped", "heating_capped", "ratchet_raised",         "rheat_capped"};

/** @brief "<prefix><name>" in a static buffer */
static const char *pre(const char *prefix, const char *name) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "%s%s", prefix, name);
  return buffer;
}

static int check_local(struct oracle_fixture *out, const char *prefix, const char *name,
                       const double value) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "%s%s", prefix, name);
  return oracle_c11_check_local(out, field, value);
}

static int check_flag(struct oracle_fixture *out, const char *prefix, const char *name,
                      const int value) {
  return oracle_c11_check_flag(out, pre(prefix, name), value);
}

static int flag_of(struct oracle_fixture *out, const char *prefix, const char *name) {
  return oracle_c11_flag(out, pre(prefix, name));
}

static void exclude_flag(struct oracle_fixture *out, const char *prefix, const char *name) {
  oracle_c11_exclude_flag(out, pre(prefix, name));
}

/** @brief Every AGN flag of a nested call that did not run is -1 */
static int check_agn_not_run(struct oracle_fixture *out, const char *prefix) {
  int p = 0;
  for (size_t k = 0; k < COUNT(AGN_FLAG_NAMES); k++) {
    p += check_flag(out, prefix, AGN_FLAG_NAMES[k], -1);
  }
  return p;
}

/** @brief The locals and flags of one nested AGN call (as oracle_c11_check_agn, prefixed) */
static int check_agn(const char *fixture, struct oracle_fixture *out, const char *prefix,
                     const struct MimicSageAgnLocals *al, const double reservoir_before) {
  int p = 0;
  p += check_flag(out, prefix, "agn_suppressed", al->suppressed);
  p += check_flag(out, prefix, "agn_saturated", al->saturated);
  p += check_flag(out, prefix, "agn_reservoir_positive", al->reservoir_positive);
  p += check_local(out, prefix, "CoolingGas_suppressed", al->coolingGas_suppressed);
  p += check_flag(out, prefix, "ratchet_raised", al->ratchet_raised);
  if (al->reservoir_positive) {
    p += check_local(out, prefix, "AGNaccreted", al->accreted);
    p += check_local(out, prefix, "AGNheating", al->heating);
    if (flag_of(out, prefix, "accretion_capped") == 1 &&
        flag_of(out, prefix, "heating_capped") == 0) {
      p += oracle_c11_check_binding(fixture, "AGNaccreted", al->accreted, reservoir_before);
    }
    if (flag_of(out, prefix, "heating_capped") == 1) {
      p += oracle_c11_check_binding(fixture, "AGNheating", al->heating, al->coolingGas_suppressed);
    }
    /* inside agn_accretion_compute: their effect is in the recorded state */
    exclude_flag(out, prefix, "accretion_capped");
    exclude_flag(out, prefix, "heating_capped");
  } else {
    p += check_flag(out, prefix, "accretion_capped", -1);
    p += check_flag(out, prefix, "heating_capped", -1);
  }
  exclude_flag(out, prefix, "eddington_capped");
  p += check_flag(out, prefix, "rheat_capped", al->rheat_capped);
  return p;
}

/** @brief The post-cap mass a recipe's cap left, from its precap mass and cap outcome */
static double postcap_of(const int cap, const double precap, const double reservoir) {
  if (cap == MIMIC_SAGE_CAP_RESERVOIR) {
    return reservoir;
  }
  return (cap == MIMIC_SAGE_CAP_FLOOR) ? 0.0 : precap;
}

/** @brief The nested hot call (intermediate.hot.*) */
static int check_hot(const char *fixture, struct oracle_fixture *out,
                     const struct Sage26RegimeAwareCoolingTrace *t,
                     const struct MimicSageAgnConfig *agn, const double hot_in) {
  const char *P = "hot.";
  const struct MimicSageHotCoolingLocals *l = &t->hot;
  const struct MimicSageColdStreamLocals *s = &l->stream;
  int p = check_flag(out, P, "return_path", l->path);
  const char *const stream_flags[] = {
      "ratio_nonpositive",    "exponent_capped_high", "exponent_capped_low", "stream_cutoff",
      "f_stream_capped_high", "f_stream_floored",     "reservoir_cap"};
  if (l->path != SAGE_COOLING_PATH_TAIL) {
    /* the outer guard (the lambda and rho_rcool returns are not constructible, oracle README) */
    for (size_t k = 0; k < COUNT(stream_flags); k++) {
      p += check_flag(out, P, stream_flags[k], -1);
    }
    p += check_flag(out, P, "agn_entered", -1);
    p += check_agn_not_run(out, P);
    p += check_local(out, P, "tcool", l->tcool);
    p += check_local(out, P, "lambda", l->lambda);
    return p;
  }
  p += check_local(out, P, "tcool", l->tcool);
  p += check_local(out, P, "lambda", l->lambda);
  p += check_local(out, P, "x", l->x);
  p += check_local(out, P, "rcool", l->rcool);
  p += check_local(out, P, "z", s->z);
  p += check_local(out, P, "M_shock", s->M_shock);
  p += check_local(out, P, "mass_ratio", s->mass_ratio);
  p += check_local(out, P, "log_mstar", s->log_mstar);
  p += check_local(out, P, "Mstar", s->Mstar);
  p += check_local(out, P, "fMstar", s->fMstar);
  p += check_local(out, P, "ratio", s->ratio);
  p += check_flag(out, P, "ratio_nonpositive", s->ratio_nonpositive);
  if (!s->ratio_nonpositive) {
    p += check_local(out, P, "exponent", s->exponent);
    p += check_flag(out, P, "exponent_capped_high", s->exponent_capped_high);
    p += check_flag(out, P, "exponent_capped_low", s->exponent_capped_low);
  } else {
    p += check_flag(out, P, "exponent_capped_high", -1);
    p += check_flag(out, P, "exponent_capped_low", -1);
  }
  p += check_local(out, P, "f_stream_criterion", s->f_stream_criterion);
  p += check_flag(out, P, "stream_cutoff", s->cutoff);
  exclude_flag(out, P, "f_stream_capped_high"); /* not reported by the helper */
  exclude_flag(out, P, "f_stream_floored");
  p += check_local(out, P, "f_stream", s->f_stream);
  p += check_local(out, P, "cold_stream_cooling", s->cold_stream_cooling);
  p += check_local(out, P, "hot_halo_cooling", s->hot_halo_cooling);
  p += check_local(out, P, "CoolingGas_precap", l->cooling_precap);
  p += check_flag(out, P, "reservoir_cap", l->cap);
  const double postcap = postcap_of(l->cap, l->cooling_precap, hot_in);
  p += check_local(out, P, "CoolingGas_postcap", postcap);
  if (l->cap == MIMIC_SAGE_CAP_RESERVOIR) {
    p += oracle_c11_check_binding(fixture, "hot CoolingGas_postcap", postcap, hot_in);
  }
  const int agn_entered = (agn->recipe > 0 && postcap > 0.0) ? 1 : 0;
  p += check_flag(out, P, "agn_entered", agn_entered);
  if ((t->hot_agn.suppressed != -1) != agn_entered) { /* the trace agrees on the AGN call */
    fprintf(stderr, "oracle: %s: the hot AGN call disagrees with agn_entered\n", fixture);
    p++;
  }
  if (agn_entered) {
    p += check_agn(fixture, out, P, &t->hot_agn, hot_in);
  } else {
    p += check_agn_not_run(out, P);
  }
  return p;
}

/** @brief The nested CGM call (intermediate.cgm.*) */
static int check_cgm(const char *fixture, struct oracle_fixture *out,
                     const struct Sage26RegimeAwareCoolingTrace *t,
                     const struct MimicSageAgnConfig *agn, const double cgm_in) {
  const char *P = "cgm.";
  const struct MimicSageCgmCoolingLocals *l = &t->cgm;
  int p = 0;
  p += check_flag(out, P, "cgm_entered", l->entered);
  p += check_flag(out, P, "lambda_positive", l->lambda_positive);
  if (l->entered) {
    p += check_local(out, P, "lambda", l->lambda);
  }
  int agn_entered = -1;
  int recapped = -1;
  if (l->lambda_positive == 1) {
    p += check_local(out, P, "x", l->x);
    p += check_local(out, P, "rho0", l->rho0);
    p += check_local(out, P, "rho_eff", l->rho_eff);
    p += check_local(out, P, "tcool", l->tcool);
    p += check_local(out, P, "g_accel", l->g_accel);
    p += check_local(out, P, "tff", l->tff);
    p += check_local(out, P, "rcool", l->rcool);
    p += check_local(out, P, "CoolingGas_precap", l->cooling_precap);
    p += check_flag(out, P, "reservoir_cap", l->cap);
    const double postcap = postcap_of(l->cap, l->cooling_precap, cgm_in);
    p += check_local(out, P, "CoolingGas_postcap", postcap);
    if (l->cap == MIMIC_SAGE_CAP_RESERVOIR) {
      p += oracle_c11_check_binding(fixture, "cgm CoolingGas_postcap", postcap, cgm_in);
    }
    agn_entered = (agn->recipe > 0 && postcap > 0.0) ? 1 : 0;
    if (agn_entered) {
      p += check_agn(fixture, out, P, &t->cgm_agn, cgm_in);
      recapped = t->cgm_agn.reservoir_positive ? t->cgm_agn.recapped : 0;
    }
  } else {
    p += check_flag(out, P, "reservoir_cap", -1);
  }
  p += check_flag(out, P, "agn_entered", agn_entered);
  p += check_flag(out, P, "recapped", recapped);
  if (agn_entered != 1) {
    p += check_agn_not_run(out, P);
  }
  return p;
}

/** @brief One cooling_recipe_regime_aware fixture */
static int check_wrapper(const char *fixture, struct oracle_fixture *in,
                         struct oracle_fixture *out) {
  reset_config();
  const struct MimicSageAgnConfig agn = oracle_c11_set_params(in);
  MimicConfig.Omega = oracle_in_f64(in, "params.Omega");
  const double mshock = oracle_in_f64(in, "params.MShockMsun");
  MimicConfig.Snaplistlen = oracle_in_i32(in, "params.Snaplistlen");
  for (int k = 0; k < ABSOLUTEMAXSNAPS; k++) {
    MimicConfig.ZZ[k] = NAN; /* a read of another snapshot surfaces as NaN */
  }
  const int current = oracle_in_i32(in, "call.current_SnapNum");
  const double zz_stored = oracle_in_f64(in, "params.ZZ_stored");
  const double zz_current = oracle_in_f64(in, "params.ZZ_current");

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c11_in_galaxy(in, &halo, &galaxy);
  galaxy.Regime = oracle_in_i32(in, "galaxy[0].Regime");
  /* SAGE26's mdot members are the package's raw rates */
  galaxy.mdot_cool_raw = galaxy.mdot_cool;
  galaxy.mdot_stream_raw = galaxy.mdot_stream;
  MimicConfig.ZZ[halo.SnapNum] = zz_stored;
  MimicConfig.ZZ[current] = zz_current;
  double dt = 0.0;
  int p = oracle_c11_dt(in, out, &halo, &dt);
  const double hot_in = galaxy.HotGas;
  const double cgm_in = galaxy.CGMgas; /* no hot step changes the CGM pair before its call */

  const struct Sage26RegimeAwareCoolingConfig cfg = {1, mshock, agn};
  struct Sage26RegimeAwareCoolingTrace t;
  if (sage26_regime_aware_cooling_run(&halo, &MimicConfig, &cfg, dt, &t) != 0) {
    fprintf(stderr, "oracle: %s: the transaction refused the fixture\n", fixture);
    return p + 1;
  }

  /* SAGE26's galaxy fields where the package keeps the raw rates */
  galaxy.mdot_cool = galaxy.mdot_cool_raw;
  galaxy.mdot_stream = galaxy.mdot_stream_raw;
  p += oracle_check_f64(out, "return.total_cooling", t.total_cooling);
  p += oracle_c11_check_state(out, &galaxy);

  p += check_flag(out, "", "cgm_regime", t.cgm_regime);
  p += check_flag(out, "", "hot_called", t.hot_called);
  p += check_flag(out, "", "hot_diagnostics_valid", t.hot_diagnostics_valid);
  p += check_flag(out, "", "cgm_called", t.cgm_called);
  p += check_flag(out, "", "reset_called", t.reset_called);
  p += check_flag(out, "", "restored", t.restored);
  p += check_flag(out, "", "cgm_apply", t.cgm_apply);
  p += check_flag(out, "", "hot_apply", t.hot_apply);
  p += oracle_check_f32(out, "intermediate.hot_tcool", t.hot_tcool);

  if (t.hot_called) {
    p += check_hot(fixture, out, &t, &agn, hot_in);
    p += check_local(out, "", "hot_cooling_returned", t.hot_returned);
  }
  if (t.cgm_called) {
    p += check_cgm(fixture, out, &t, &agn, cgm_in);
    p += check_local(out, "", "cgm_cooling_returned", t.cgm_returned);
  }
  if (t.cgm_apply != SAGE26_RAC_APPLY_NOT_REACHED) {
    p += check_local(out, "", "cgm_cooling_applied", t.cgm_applied);
    p += check_local(out, "", "cgm_metallicity", t.cgm_metallicity);
    if (t.cgm_apply == SAGE26_RAC_APPLY_CAPPED) {
      p += oracle_c11_check_binding(fixture, "cgm_cooling_applied", t.cgm_applied, t.cgm_reservoir);
    }
  }
  if (t.hot_apply != SAGE26_RAC_APPLY_NOT_REACHED) {
    p += check_local(out, "", "hot_cooling_applied", t.hot_applied);
    p += check_local(out, "", "hot_metallicity", t.hot_metallicity);
    if (t.hot_apply == SAGE26_RAC_APPLY_CAPPED) {
      p += oracle_c11_check_binding(fixture, "hot_cooling_applied", t.hot_applied, t.hot_reservoir);
    }
  }
  p += check_local(out, "", "total_cooling", t.total_cooling);
  return p;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "cooling_recipe_regime_aware", &in, &out) != 0) {
    return 1;
  }
  int problems = check_wrapper(name, &in, &out);
  oracle_c11_counts.fixtures++;
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  if (problems != 0) {
    fprintf(stderr, "oracle: %s/%s: %d mismatches\n", MODULE, name, problems);
  }
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
  /* The cooling tables are loaded once for every fixture (the budget's init, CGMrecipeOn 0) */
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "cooling tables must load");
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  sage26_calculate_cooling_budget_cleanup();
  oracle_c11_report(MODULE);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_regime_aware_cooling SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
