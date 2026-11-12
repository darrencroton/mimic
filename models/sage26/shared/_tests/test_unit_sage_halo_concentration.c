/**
 * @file    test_unit_sage_halo_concentration.c
 * @brief   Tests of shared/sage_halo_concentration.h (SAGE26's
 *          concentration_from_vmax_vvir, the ConcentrationOn 3 dispatch and the validator)
 *
 * Each expectation is derived independently of the helpers: a literal where the input makes the
 * result exact, or SAGE26's own loop written here (sage26_inline_inversion), never by calling a
 * helper under test.
 *
 * Test cases:
 *   - test_velocity_guards: Vvir +0, -0 and negative, and Vmax +0, -0 and negative, each on its
 *     own, return +0 with nothing formed
 *   - test_ratio_below_one: Vmax < Vvir returns +0 after forming ratio_sq
 *   - test_ratio_exactly_one: Vmax == Vvir returns exactly 1.0 after exactly 60 steps, c_lo stays
 *     at its bound 1.0 and c_hi ends at SAGE26's inline value (the double just above 1)
 *   - test_jump: the last ratio below f(2.5546875) returns exactly 1.0, the next SAGE26's inline
 *     result, 2.5546875 or more
 *   - test_tie_at_visited_midpoint: a ratio_sq equal to f at the visited midpoint 7.21875 (the
 *     strict f_mid < ratio_sq moves c_hi) gives SAGE26's inline result
 *   - test_typical_and_saturated_ratios: a table of Vmax / Vvir ratios, each equal to SAGE26's
 *     inline loop bit for bit: 1.5 (typical), 10 (saturates at exactly 200, c_hi at its bound)
 *     and a NaN Vmax (it passes the guards, so ratio_sq is NaN and the result is exactly 1.0)
 *   - test_dispatch_selects_by_type: Type 0 takes the current pair, 1, 2 and -1 the infall pair,
 *     each float widened exactly
 *   - test_get_halo_concentration: mode 0 returns 0 forming nothing; mode 3 inverts the selected
 *     pair (SAGE26's inline result); an unsupported mode (1, 2, -1 or 4) returns NaN
 *   - test_validator: 0 and 3 accepted; 1, 2, -1 and 4 refused naming ConcentrationOn
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_halo_concentration.h"

// ============================================================================
// SAGE26'S LOOP, WRITTEN INLINE
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define CONCENTRATION_TEST_NOINLINE __attribute__((noinline))
#else
#define CONCENTRATION_TEST_NOINLINE
#endif

/** The bracket and step count of the inline loop */
struct inline_result {
  double c_lo;
  double c_hi;
  int steps;
  int first_tie_step; /* the first step whose f_mid equals ratio_sq, -1 for none */
};

/**
 * @brief SAGE26's concentration_from_vmax_vvir, statement by statement
 *
 * Compared with the helper bit for bit, so it must run with the runtime libm: never inlined, and
 * its inputs pass through volatile locals, so log is evaluated at run time.
 */
static CONCENTRATION_TEST_NOINLINE double
sage26_inline_inversion(const double vmax_in, const double vvir_in, struct inline_result *r) {
  volatile double vmax_runtime = vmax_in;
  volatile double vvir_runtime = vvir_in;
  const double Vmax = vmax_runtime;
  const double Vvir = vvir_runtime;
  r->c_lo = NAN;
  r->c_hi = NAN;
  r->steps = 0;
  r->first_tie_step = -1;

  if (Vvir <= 0.0 || Vmax <= 0.0)
    return 0.0;
  const double ratio_sq = (Vmax / Vvir) * (Vmax / Vvir);
  const double A = 0.21621;
  double c_lo = 1.0, c_hi = 200.0;
  if (ratio_sq < 1.0)
    return 0.0;
  for (int iter = 0; iter < 60; iter++) {
    const double c_mid = 0.5 * (c_lo + c_hi);
    const double mu = log(1.0 + c_mid) - c_mid / (1.0 + c_mid);
    const double f_mid = A * c_mid / mu;
    if (f_mid == ratio_sq && r->first_tie_step < 0) {
      r->first_tie_step = iter;
    }
    if (f_mid < ratio_sq) {
      c_lo = c_mid;
    } else {
      c_hi = c_mid;
    }
    r->steps++;
  }
  r->c_lo = c_lo;
  r->c_hi = c_hi;
  return 0.5 * (c_lo + c_hi);
}

/** @brief The helper under test, its inputs passed through volatile locals as above */
static double helper(const double vmax_in, const double vvir_in,
                     struct MimicSageConcentrationLocals *locals) {
  volatile double vmax_runtime = vmax_in;
  volatile double vvir_runtime = vvir_in;
  return mimic_sage_concentration_from_vmax_vvir(vmax_runtime, vvir_runtime, locals);
}

/**
 * @brief Compare the helper with the inline loop bit for bit: return, final bracket, 60 steps
 * @return 0 when everything agrees; otherwise the number of disagreements (each printed)
 */
static int differs_from_inline(const double vmax, const double vvir, const char *what) {
  struct inline_result r;
  struct MimicSageConcentrationLocals l;
  const double expected = sage26_inline_inversion(vmax, vvir, &r);
  const double got = helper(vmax, vvir, &l);
  printf("\n  %s: Vmax %.17g Vvir %.17g -> %.17g (inline %.17g), bracket [%a, %a] (inline [%a, "
         "%a]), steps %d (inline %d), first tie step %d",
         what, vmax, vvir, got, expected, l.c_lo, l.c_hi, r.c_lo, r.c_hi, l.iterations, r.steps,
         r.first_tie_step);
  int problems = 0;
  problems += double_bits(got) != double_bits(expected);
  problems += l.iterations != 60 || r.steps != 60;
  problems += l.bracket_absent != 0;
  problems +=
      double_bits(l.c_lo) != double_bits(r.c_lo) || double_bits(l.c_hi) != double_bits(r.c_hi);
  return problems;
}

#define INLINE_MESSAGE                                                                             \
  "the helper must equal SAGE26's inline loop: return, final bracket and exactly 60 steps"

// ============================================================================
// THE INVERSION
// ============================================================================

/**
 * @test    test_velocity_guards
 * @brief   Each side of Vvir <= 0.0 || Vmax <= 0.0 on its own returns +0, forming nothing
 */
int test_velocity_guards(void) {
  const double pairs[6][2] = {{300.0, 0.0}, {300.0, -0.0}, {300.0, -150.0},
                              {0.0, 200.0}, {-0.0, 200.0}, {-300.0, 200.0}};
  for (int k = 0; k < 6; k++) {
    struct MimicSageConcentrationLocals l;
    const double c = helper(pairs[k][0], pairs[k][1], &l);
    printf("\n  Vmax %g Vvir %g -> %a (guard %d)", pairs[k][0], pairs[k][1], c, l.guard_returned);
    TEST_ASSERT(double_bits(c) == double_bits(0.0), "a non-positive velocity must return +0");
    TEST_ASSERT(l.guard_returned == 1 && l.below_one == -1, "the velocity guard must return");
    TEST_ASSERT(l.ratio_sq_absent == 1 && l.bracket_absent == 1 && l.iterations == 0,
                "nothing must be formed after the guard");
  }
  return TEST_PASS;
}

/**
 * @test    test_ratio_below_one
 * @brief   Vmax < Vvir (180 / 200): ratio_sq (0.9 * 0.9) is formed, is below 1, and returns +0
 */
int test_ratio_below_one(void) {
  struct MimicSageConcentrationLocals l;
  const double c = helper(180.0, 200.0, &l);
  printf("\n  Vmax 180 Vvir 200: %a, ratio_sq %.17g, below_one %d", c, l.ratio_sq, l.below_one);
  TEST_ASSERT(double_bits(c) == double_bits(0.0), "a ratio below 1 must return +0");
  TEST_ASSERT(l.guard_returned == 0 && l.below_one == 1, "the ratio test must return");
  TEST_ASSERT(l.ratio_sq_absent == 0 && l.ratio_sq == (0.9 * 0.9),
              "ratio_sq must be (180 / 200) squared");
  TEST_ASSERT(l.bracket_absent == 1 && l.iterations == 0, "the bisection must not run");
  return TEST_PASS;
}

/**
 * @test    test_ratio_exactly_one
 * @brief   Vmax == Vvir: every visited f_mid is at least 1, so c_lo stays 1.0, c_hi falls to the
 *          double just above 1 in exactly 60 steps (another step count ends elsewhere), and the
 *          return rounds to exactly 1.0
 */
int test_ratio_exactly_one(void) {
  const double vv[2] = {200.0, 1.0};
  for (int k = 0; k < 2; k++) {
    struct MimicSageConcentrationLocals l;
    const double c = helper(vv[k], vv[k], &l);
    printf("\n  Vmax = Vvir = %g: %a, ratio_sq %a, bracket [%a, %a], steps %d", vv[k], c,
           l.ratio_sq, l.c_lo, l.c_hi, l.iterations);
    TEST_ASSERT(c == 1.0, "Vmax == Vvir must return exactly 1.0");
    TEST_ASSERT(l.ratio_sq == 1.0 && l.below_one == 0, "ratio_sq must be exactly 1");
    TEST_ASSERT(l.c_lo == MIMIC_SAGE_CONCENTRATION_C_LO && l.c_lo == 1.0,
                "c_lo must stay at its bound 1.0");
    TEST_ASSERT(l.c_hi == nextafter(1.0, 2.0),
                "c_hi must end at the double just above 1 after exactly 60 steps");
    TEST_ASSERT(l.iterations == 60, "the bisection must take exactly 60 steps");
    TEST_ASSERT(differs_from_inline(vv[k], vv[k], "ratio 1") == 0, INLINE_MESSAGE);
  }
  return TEST_PASS;
}

/**
 * @test    test_jump
 * @brief   Either side of the jump at f(2.5546875), about 1.00503 (the non-monotonic quirk): with
 *          Vvir 1, the last Vmax returning exactly 1.0 and the next double, which returns SAGE26's
 *          inline result (2.5546875)
 */
int test_jump(void) {
  const double q_last = 1.0025106939461708;
  const double q_first = nextafter(q_last, 2.0);
  struct MimicSageConcentrationLocals l;
  const double below = helper(q_last, 1.0, &l);
  printf("\n  Vmax %.17g: %a (ratio_sq %.17g)", q_last, below, l.ratio_sq);
  TEST_ASSERT(below == 1.0, "the last ratio below the jump must return exactly 1.0");
  TEST_ASSERT(l.c_lo == 1.0, "c_lo must stay at its bound below the jump");
  const double above = helper(q_first, 1.0, &l);
  printf("\n  Vmax %.17g: %.17g (ratio_sq %.17g)", q_first, above, l.ratio_sq);
  TEST_ASSERT(above >= 2.5546875, "the first ratio above the jump must return 2.5546875 or more");
  TEST_ASSERT(differs_from_inline(q_last, 1.0, "below the jump") == 0, INLINE_MESSAGE);
  TEST_ASSERT(differs_from_inline(q_first, 1.0, "above the jump") == 0, INLINE_MESSAGE);
  return TEST_PASS;
}

/**
 * @test    test_tie_at_visited_midpoint
 * @brief   ratio_sq equal to f at the visited midpoint 7.21875 (the inline loop reports the tie):
 *          the strict f_mid < ratio_sq moves c_hi, and the result is SAGE26's inline one
 *          (7.2187499999999947; <= would give 7.21875)
 */
int test_tie_at_visited_midpoint(void) {
  TEST_ASSERT(differs_from_inline(1.1273365592411466, 1.0, "tie at 7.21875") == 0, INLINE_MESSAGE);
  return TEST_PASS;
}

/**
 * @test    test_typical_and_saturated_ratios
 * @brief   Each ratio equals SAGE26's inline loop bit for bit; the rows with a literal result
 *          also pin it
 *
 * Vmax / Vvir 1.5 is the typical path. Vmax / Vvir 10 (ratio_sq 100, far beyond f(200) ~ 10.04)
 * has no upper rejection: c_lo rises every step, c_hi stays at its bound 200 and the return is
 * exactly 200. A NaN Vmax passes both velocity guards (every comparison with NaN is false), so
 * ratio_sq is NaN, f_mid < ratio_sq never holds and c_hi falls onto c_lo = 1: exactly 1.0.
 */
int test_typical_and_saturated_ratios(void) {
  static const struct {
    const char *name;
    double vmax;
    double vvir;
    double ratio_sq;     /* the literal ratio_sq (NaN for a NaN ratio) */
    double exact_return; /* the literal result, or 0 when only the inline loop gives it */
  } rows[] = {
      {"typical ratio 1.5", 300.0, 200.0, 2.25, 0.0},
      {"ratio 10 saturates at 200", 2000.0, 200.0, 100.0, 200.0},
      {"a NaN Vmax passes the guards and returns 1.0", NAN, 200.0, NAN, 1.0},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    struct MimicSageConcentrationLocals l;
    const double c = helper(rows[i].vmax, rows[i].vvir, &l);
    printf("\n  %s: %.17g, bracket [%a, %a]", rows[i].name, c, l.c_lo, l.c_hi);
    TEST_ASSERT(isnan(rows[i].ratio_sq) ? isnan(l.ratio_sq) : l.ratio_sq == rows[i].ratio_sq,
                rows[i].name);
    if (rows[i].exact_return != 0.0) {
      TEST_ASSERT(c == rows[i].exact_return, rows[i].name);
    }
    if (rows[i].exact_return == MIMIC_SAGE_CONCENTRATION_C_HI) {
      TEST_ASSERT(l.c_hi == MIMIC_SAGE_CONCENTRATION_C_HI, "a saturated c_hi stays at its bound");
    }
    TEST_ASSERT(differs_from_inline(rows[i].vmax, rows[i].vvir, rows[i].name) == 0, INLINE_MESSAGE);
  }
  return TEST_PASS;
}

// ============================================================================
// THE DISPATCH AND THE VALIDATOR
// ============================================================================

/**
 * @test    test_dispatch_selects_by_type
 * @brief   Type 0 takes Vmax and Vvir; 1, 2 and -1 take infallVmax and infallVvir, each float
 *          widened exactly
 */
int test_dispatch_selects_by_type(void) {
  const float vmax = 220.3f, vvir = 180.7f, infall_vmax = 260.1f, infall_vvir = 199.9f;
  const int types[4] = {0, 1, 2, -1};
  for (int k = 0; k < 4; k++) {
    const struct MimicSageConcentrationPair p =
        mimic_sage_concentration_mode3_pair(types[k], vmax, vvir, infall_vmax, infall_vvir);
    printf("\n  Type %d: infall %d, pair %.9g / %.9g", types[k], p.infall, p.vmax, p.vvir);
    if (types[k] == 0) {
      TEST_ASSERT(p.infall == 0 && p.vmax == (double)vmax && p.vvir == (double)vvir,
                  "Type 0 must take the current float pair, widened");
    } else {
      TEST_ASSERT(p.infall == 1 && p.vmax == (double)infall_vmax && p.vvir == (double)infall_vvir,
                  "any other Type must take the infall float pair, widened");
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_get_halo_concentration
 * @brief   Mode 0 returns 0 forming nothing; mode 3 inverts the pair its Type selects, where the
 *          two pairs give different results; an unsupported mode returns NaN
 */
int test_get_halo_concentration(void) {
  struct MimicSageHaloConcentrationLocals l;
  /* current pair ratio 1.5, infall pair ratio below 1 */
  const float vmax = 300.0f, vvir = 200.0f, infall_vmax = 100.0f, infall_vvir = 200.0f;
  struct inline_result r;
  const double current = sage26_inline_inversion((double)vmax, (double)vvir, &r);

  double c = mimic_sage_get_halo_concentration(0, 0, vmax, vvir, infall_vmax, infall_vvir, &l);
  printf("\n  mode 0: %a, selected %d", c, l.selected_infall);
  TEST_ASSERT(double_bits(c) == double_bits(0.0) && l.selected_infall == -1 && l.pair_absent == 1,
              "mode 0 must return 0 with no pair selected");
  TEST_ASSERT(l.inversion.ratio_sq_absent == 1 && l.inversion.bracket_absent == 1,
              "mode 0 must form nothing");

  c = mimic_sage_get_halo_concentration(3, 0, vmax, vvir, infall_vmax, infall_vvir, &l);
  printf("\n  mode 3, Type 0: %.17g (inline %.17g)", c, current);
  TEST_ASSERT(double_bits(c) == double_bits(current) && l.selected_infall == 0,
              "Type 0 must invert the current pair");
  c = mimic_sage_get_halo_concentration(3, 1, vmax, vvir, infall_vmax, infall_vvir, &l);
  printf("\n  mode 3, Type 1: %a", c);
  TEST_ASSERT(double_bits(c) == double_bits(0.0) && l.selected_infall == 1 &&
                  l.inversion.below_one == 1,
              "Type 1 must invert the infall pair (ratio below 1)");
  c = mimic_sage_get_halo_concentration(3, 1, infall_vmax, infall_vvir, vmax, vvir, &l);
  TEST_ASSERT(double_bits(c) == double_bits(current),
              "Type 1 must invert the infall pair (ratio 1.5)");

  const int unsupported[4] = {1, 2, -1, 4};
  for (int k = 0; k < 4; k++) {
    c = mimic_sage_get_halo_concentration(unsupported[k], 0, vmax, vvir, infall_vmax, infall_vvir,
                                          &l);
    TEST_ASSERT(isnan(c), "an unsupported mode must return NaN");
  }
  return TEST_PASS;
}

/**
 * @test    test_validator
 * @brief   ConcentrationOn 0 and 3 accepted; 1, 2, -1 and 4 refused with an error naming it
 */
int test_validator(void) {
  TEST_ASSERT(mimic_sage_validate_concentration_on("test", 0) == 0, "0 must be accepted");
  TEST_ASSERT(mimic_sage_validate_concentration_on("test", 3) == 0, "3 must be accepted");
  const int refused[4] = {1, 2, -1, 4};
  for (int k = 0; k < 4; k++) {
    FILE *capture = tmpfile();
    FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
    const int status = mimic_sage_validate_concentration_on("test", refused[k]);
    const char *log = "";
    if (capture != NULL) {
      set_log_output(old_out);
      log = read_captured_log(capture);
      fclose(capture);
    }
    printf("\n  ConcentrationOn %d: %d", refused[k], status);
    TEST_ASSERT(status != 0, "an unsupported ConcentrationOn must be refused");
    TEST_ASSERT(strstr(log, "ConcentrationOn") != NULL, "the error must name ConcentrationOn");
  }
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: shared/sage_halo_concentration.h\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("\n%s--- The Inversion ---%s\n", BLUE, NC);
  TEST_RUN(test_velocity_guards);
  TEST_RUN(test_ratio_below_one);
  TEST_RUN(test_ratio_exactly_one);
  TEST_RUN(test_jump);
  TEST_RUN(test_tie_at_visited_midpoint);
  TEST_RUN(test_typical_and_saturated_ratios);

  printf("\n%s--- The Dispatch and the Validator ---%s\n", BLUE, NC);
  TEST_RUN(test_dispatch_selects_by_type);
  TEST_RUN(test_get_halo_concentration);
  TEST_RUN(test_validator);

  TEST_SUMMARY();
  return TEST_RESULT();
}
