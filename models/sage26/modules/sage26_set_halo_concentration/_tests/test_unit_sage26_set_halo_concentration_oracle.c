/**
 * @file    test_unit_sage26_set_halo_concentration_oracle.c
 * @brief   Bit-exact comparison of shared/sage_halo_concentration.h with SAGE26's
 *          concentration_from_vmax_vvir and get_halo_concentration
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_set_halo_concentration
 * (models/sage26/modules/sage26_set_halo_concentration/_tests/oracle/, written by oracle_c12)
 * through the shared helpers the module calls, directly:
 *
 * - inv_* fixtures (routine concentration_from_vmax_vvir): each case's double Vmax and Vvir into
 *   mimic_sage_concentration_from_vmax_vvir;
 * - ghc_* fixtures (routine get_halo_concentration): the fixture's ConcentrationOn and each
 *   galaxy's Type (its Type at the call) and four float velocities into
 *   mimic_sage_get_halo_concentration.
 *
 * Per case it asserts the double return, the float store SAGE26's joining makes of it (where the
 * fixture records one: ConcentrationOn > 0), the guard and ratio flags, the step count, the
 * selected pair and every recorded local (ratio_sq, the final c_lo and c_hi, the selected vmax and
 * vvir) bit for bit, which the helpers report; checks each local's markers (present or absent as
 * in the port, checked, of the recorded check kind, `_validated` the conjunction of its two
 * directions); asserts each bracket end recorded at its initial bound equal to that bound; and
 * prints the tallies quoted in the validation record. The redshift input is consumed unused:
 * neither mode reads it, in SAGE26 or the package. The transcription's tie observations (`ties`,
 * `first_tie_step`) are consumed unasserted: they are not SAGE26 or package state. The module's
 * own row selection (new Type 0, PreviousType 0 or 1) is the platform-independent unit test's;
 * its composition with SAGE26's joining is a later slice's.
 *
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/sage_halo_concentration.h"

static const char *MODULE = "sage26_set_halo_concentration";

#define MAX_CASES 24
#define CHECK_CONTINUATION 1
#define CHECK_TAIL 2

/** Tallies of the recorded locals over one test run (quoted in the validation record) */
struct c12_tally {
  int cases;
  int continuation;      /* present and continuation-checked */
  int tail;              /* present and tail-checked */
  int validated;         /* validated two-sided, a float store changing in a direction */
  int narrowing_limited; /* validated two-sided through the double return only */
  int not_validated;     /* checked, not validated two-sided */
  int binding;           /* a bracket end at its initial bound, asserted equal to it */
  int absent;            /* not formed on the case's path */
};

static struct c12_tally tally;

/**
 * @brief Check one recorded local: its markers, and its value bit for bit
 * @return Number of mismatches and marker errors
 */
static int check_local(struct oracle_fixture *out, const char *prefix, const char *name,
                       const int kind, const int absent_in_port, const double value) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "%s.%s_absent", prefix, name);
  const int absent = oracle_in_i32(out, field);
  if (absent != absent_in_port) {
    fprintf(stderr, "oracle: %s: %s.%s is %s in the oracle but %s in the port\n", out->path, prefix,
            name, absent ? "absent" : "present", absent_in_port ? "absent" : "present");
    return 1;
  }
  if (absent) {
    tally.absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "%s.%s_checked", prefix, name);
  if (oracle_in_i32(out, field) != 1) {
    fprintf(stderr, "oracle: %s: %s.%s is not marked checked\n", out->path, prefix, name);
    problems++;
  }
  snprintf(field, sizeof(field), "%s.%s_check_kind", prefix, name);
  if (oracle_in_i32(out, field) != kind) {
    fprintf(stderr, "oracle: %s: %s.%s is not of check kind %d\n", out->path, prefix, name, kind);
    problems++;
  }
  if (kind == CHECK_CONTINUATION) {
    tally.continuation++;
  } else {
    tally.tail++;
  }
  snprintf(field, sizeof(field), "%s.%s_validated_up", prefix, name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_validated_down", prefix, name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_validated", prefix, name);
  const int validated = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_store_changes_up", prefix, name);
  const int store_up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_store_changes_down", prefix, name);
  const int store_down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_narrowing_limited", prefix, name);
  const int narrowing = oracle_in_i32(out, field);
  if (validated != (up && down) || narrowing != (validated && !store_up && !store_down) ||
      (store_up && !up) || (store_down && !down)) {
    fprintf(stderr, "oracle: %s: %s.%s's validation markers disagree with each other\n", out->path,
            prefix, name);
    problems++;
  }
  if (narrowing) {
    tally.narrowing_limited++;
  } else if (validated) {
    tally.validated++;
  } else {
    tally.not_validated++;
  }
  snprintf(field, sizeof(field), "%s.%s", prefix, name);
  problems += oracle_check_f64(out, field, value);
  return problems;
}

/**
 * @brief Check an inversion's flags, step count, locals and bound markers under `prefix`
 * @return Number of mismatches and marker errors
 */
static int check_inversion(struct oracle_fixture *out, const char *prefix,
                           const struct MimicSageConcentrationLocals *l) {
  char field[ORACLE_MAX_NAME];
  int problems = 0;
  snprintf(field, sizeof(field), "%s.guard_returned", prefix);
  problems += oracle_check_i32(out, field, l->guard_returned);
  snprintf(field, sizeof(field), "%s.below_one", prefix);
  problems += oracle_check_i32(out, field, l->below_one);
  snprintf(field, sizeof(field), "%s.iterations", prefix);
  problems += oracle_check_i32(out, field, l->iterations);
  problems += check_local(out, prefix, "ratio_sq", CHECK_TAIL, l->ratio_sq_absent, l->ratio_sq);
  problems += check_local(out, prefix, "c_lo", CHECK_TAIL, l->bracket_absent, l->c_lo);
  problems += check_local(out, prefix, "c_hi", CHECK_TAIL, l->bracket_absent, l->c_hi);

  /* A bracket end that never moved: the bound binds, and the port's value must equal it */
  snprintf(field, sizeof(field), "%s.c_lo_at_initial_bound", prefix);
  const int lo_bound = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.c_hi_at_initial_bound", prefix);
  const int hi_bound = oracle_in_i32(out, field);
  if (l->bracket_absent) {
    problems += (lo_bound != -1) + (hi_bound != -1);
  } else {
    if (lo_bound != (l->c_lo == MIMIC_SAGE_CONCENTRATION_C_LO) ||
        hi_bound != (l->c_hi == MIMIC_SAGE_CONCENTRATION_C_HI)) {
      fprintf(stderr, "oracle: %s: %s's bound markers disagree with the port's bracket\n",
              out->path, prefix);
      problems++;
    }
    if (lo_bound == 1) {
      tally.binding++;
      problems += oracle_f64_to_bits(l->c_lo) != oracle_f64_to_bits(MIMIC_SAGE_CONCENTRATION_C_LO);
    }
    if (hi_bound == 1) {
      tally.binding++;
      problems += oracle_f64_to_bits(l->c_hi) != oracle_f64_to_bits(MIMIC_SAGE_CONCENTRATION_C_HI);
    }
  }
  /* The transcription's tie observations: not SAGE26 or package state */
  snprintf(field, sizeof(field), "%s.ties", prefix);
  oracle_exclude(out, field);
  snprintf(field, sizeof(field), "%s.first_tie_step", prefix);
  oracle_exclude(out, field);
  return problems;
}

/** @brief Compare one concentration_from_vmax_vvir fixture; returns mismatches and errors */
static int check_inversion_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "concentration_from_vmax_vvir", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;
  const int ncase = oracle_in_i32(&in, "call.ncase");
  if (ncase < 1 || ncase > MAX_CASES) {
    fprintf(stderr, "oracle: %s: call.ncase %d outside 1..%d\n", name, ncase, MAX_CASES);
    return 1;
  }
  for (int k = 0; k < ncase; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "case[%d].Vmax", k);
    const double vmax = oracle_in_f64(&in, field);
    snprintf(field, sizeof(field), "case[%d].Vvir", k);
    const double vvir = oracle_in_f64(&in, field);

    struct MimicSageConcentrationLocals l;
    const double concentration = mimic_sage_concentration_from_vmax_vvir(vmax, vvir, &l);
    tally.cases++;

    snprintf(field, sizeof(field), "return.case[%d]", k);
    problems += oracle_check_f64(&out, field, concentration);
    snprintf(field, sizeof(field), "output.case[%d].Concentration", k);
    problems += oracle_check_f32(&out, field, (float)concentration);
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    problems += check_inversion(&out, prefix, &l);
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Compare one get_halo_concentration fixture; returns mismatches and errors */
static int check_dispatch_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "get_halo_concentration", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;
  const int concentration_on = oracle_in_i32(&in, "params.ConcentrationOn");
  if (mimic_sage_validate_concentration_on(MODULE, concentration_on) != 0) {
    return 1;
  }
  (void)oracle_in_f64(&in, "call.z"); /* read by neither mode, in SAGE26 or the package */
  const int ncase = oracle_in_i32(&in, "call.ncase");
  if (ncase < 1 || ncase > MAX_CASES) {
    fprintf(stderr, "oracle: %s: call.ncase %d outside 1..%d\n", name, ncase, MAX_CASES);
    return 1;
  }
  /* SAGE26's joining stores the result only under ConcentrationOn > 0 (B:291) */
  const int store_made = concentration_on > 0;
  problems += oracle_check_i32(&out, "intermediate.store_made", store_made);

  for (int k = 0; k < ncase; k++) {
    const int type = oracle_in_i32(&in, oracle_galaxy_field(k, "Type"));
    const float vmax = oracle_in_f32(&in, oracle_galaxy_field(k, "Vmax"));
    const float vvir = oracle_in_f32(&in, oracle_galaxy_field(k, "Vvir"));
    const float infall_vmax = oracle_in_f32(&in, oracle_galaxy_field(k, "infallVmax"));
    const float infall_vvir = oracle_in_f32(&in, oracle_galaxy_field(k, "infallVvir"));

    struct MimicSageHaloConcentrationLocals l;
    const double concentration = mimic_sage_get_halo_concentration(
        concentration_on, type, vmax, vvir, infall_vmax, infall_vvir, &l);
    tally.cases++;

    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "return.case[%d]", k);
    problems += oracle_check_f64(&out, field, concentration);
    if (store_made) {
      snprintf(field, sizeof(field), "output.case[%d].Concentration", k);
      problems += oracle_check_f32(&out, field, (float)concentration);
    }
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    snprintf(field, sizeof(field), "%s.selected_infall", prefix);
    problems += oracle_check_i32(&out, field, l.selected_infall);
    problems += check_local(&out, prefix, "vmax", CHECK_CONTINUATION, l.pair_absent, l.pair.vmax);
    problems += check_local(&out, prefix, "vvir", CHECK_CONTINUATION, l.pair_absent, l.pair.vvir);
    problems += check_inversion(&out, prefix, &l.inversion);
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Compare one fixture by its routine (the name's prefix) */
static int check_fixture(const char *name) {
  if (strncmp(name, "inv_", 4) == 0) {
    return check_inversion_fixture(name);
  }
  if (strncmp(name, "ghc_", 4) == 0) {
    return check_dispatch_fixture(name);
  }
  fprintf(stderr, "oracle: %s: a fixture name must start with inv_ or ghc_\n", name);
  return 1;
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
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle c12 (%s): %d cases; locals continuation-checked %d, tail-checked %d; "
         "validated two-sided %d, narrowing-limited %d, not validated %d; binding bracket bounds "
         "%d; absent %d ",
         MODULE, tally.cases, tally.continuation, tally.tail, tally.validated,
         tally.narrowing_limited, tally.not_validated, tally.binding, tally.absent);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_set_halo_concentration SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
