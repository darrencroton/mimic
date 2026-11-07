/**
 * @file    test_unit_sage_ffb_regime.c
 * @brief   Platform-independent tests of shared/sage_ffb_regime.h, SAGE26's FFB threshold,
 *          fraction and SplitMix64 draw, and the EnhancedStarFormationOn validator
 *
 * Every expectation is derived independently of the helpers: the draw's reference vectors (computed
 * by an independent transcription; SAGE26's own values, from the oracle, confirm them), literal
 * inputs whose result is exact, or SAGE26's expressions written in this file with their operand
 * order (R/src/model_regimes.c:309-312 and :330-339; R/ is the staged SAGE26 source root), never by
 * calling the helper under test.
 *
 * - test_draw_vectors: the 64 hashed bits and the deviate, bit for bit, of the eight reference
 *   vectors (the first is SplitMix64's published first output for seed 0) and of two negative
 *   snapshots, whose (uint32_t) conversion the result depends on
 * - test_deviate_endpoints: the deviate of the hashed bits 0 is 2^-54 and of all ones exactly 1.0
 *   (SAGE26's + 0.5 with no endpoint correction, :158)
 * - test_threshold_inline: the threshold and its locals at literal redshifts equal SAGE26's
 *   expression written here; z 9 gives z_norm exactly 1
 * - test_fraction_guards: each guard gives exactly 0 on its own: Mvir +0, -0, negative and -inf,
 *   a threshold of 0 (z 1e54), and Mvir +0 against a NaN threshold (z -2); a NaN Mvir passes the
 *   guards (NaN), an infinite one gives exactly 1
 * - test_fraction_half: Mvir equal to the threshold gives exactly 0.5
 * - test_fraction_inline: the fraction across the sigmoid equals SAGE26's expression written here
 * - test_validator: EnhancedStarFormationOn 0 and 1 are accepted; 2, 3 and -1 refused, naming it
 */

#include "../../../../tests/framework/test_framework.h"
#include "include/globals.h"
#include "util/error.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shared/sage_ffb_regime.h"

/* Test framework counters (each test translation unit owns its own) */
static int passed = 0;
static int failed = 0;

/* R6's Hubble_h (input/millennium.par:40) */
#define TEST_HUBBLE_H 0.73

static uint64_t bits_of(const double v) {
  uint64_t b;
  memcpy(&b, &v, sizeof(b));
  return b;
}

#if defined(__GNUC__) || defined(__clang__)
#define FFB_TEST_NOINLINE __attribute__((noinline))
#else
#define FFB_TEST_NOINLINE
#endif

/**
 * @brief SAGE26's calculate_ffb_threshold_mass written here (R/src/model_regimes.c:308-312),
 *        its log term fused as SAGE26's object computes it (model_regimes.o 0x908: fmadd of
 *        log10(z_norm), -6.2 and log10(h) + 0.8)
 *
 * Never inlined, its inputs through volatile locals, so the compiler cannot fold the libm calls
 * at compile time with another rounding than the runtime libm's.
 */
static FFB_TEST_NOINLINE double sage26_threshold(const double z_in, const double h_in,
                                                 double *z_norm_out, double *log_out) {
  volatile double z_runtime = z_in;
  volatile double h_runtime = h_in;
  const double z = z_runtime;
  const double h = h_runtime;
  const double z_norm = (1.0 + z) / 10.0;
  const double log_Mvir_ffb_code = fma(-6.2, log10(z_norm), 0.8 + log10(h));
  *z_norm_out = z_norm;
  *log_out = log_Mvir_ffb_code;
  return pow(10.0, log_Mvir_ffb_code);
}

/** @brief SAGE26's calculate_ffb_fraction written here (:330-339), for a positive Mvir */
static FFB_TEST_NOINLINE double sage26_fraction(const double mvir_in, const double z,
                                                const double h) {
  volatile double mvir_runtime = mvir_in;
  const double Mvir = mvir_runtime;
  double z_norm, log_code;
  const double Mvir_ffb = sage26_threshold(z, h, &z_norm, &log_code);
  if (Mvir <= 0.0 || Mvir_ffb <= 0.0) {
    return 0.0;
  }
  const double delta_log_M = 0.15;
  const double x = log10(Mvir / Mvir_ffb) / delta_log_M;
  return 1.0 / (1.0 + exp(-x));
}

/**
 * @test    test_draw_vectors
 * @brief   halo_uniform's bits and deviate (R/src/model_regimes.c:151-159), bit for bit
 */
int test_draw_vectors(void) {
  const struct {
    long long id;
    int snap;
    uint64_t bits;
    uint64_t deviate;
  } vectors[] = {
      /* The draw's reference vectors */
      {0LL, 0, 0xe220a8397b1dcdafULL, 0x3fec4415072f63baULL},
      {1LL, 0, 0x910a2dec89025cc1ULL, 0x3fe22145bd91204cULL},
      {0LL, 63, 0xe255b237b8bb18fbULL, 0x3fec4ab646f71764ULL},
      {-1LL, 12, 0x6092f2d2b1aad496ULL, 0x3fd824bcb4ac6ab5ULL},
      {INT64_MIN, 5, 0x4d7e6a268a67c5ffULL, 0x3fd35f9a89a299f1ULL},
      {4611686018427400249LL, 27, 0x6552cb8b30f23ce3ULL, 0x3fd954b2e2cc3c8fULL},
      {4896973LL, 12, 0x7218b70ad3eb71d1ULL, 0x3fdc862dc2b4faddULL},
      {9391591LL, 16, 0x14bdcaf7d17309fbULL, 0x3fb4bdcaf7d1730cULL},
      /* Negative snapshots: (uint32_t)-1 is 4294967295, not the sign-extended 64-bit -1;
       * SAGE26's own bits for both (the oracle's cls_negative_snapshot_probe) */
      {4896973LL, -1, 0xe8efcc1adad0340eULL, 0x3fed1df9835b5a06ULL},
      {5LL, -1, 0x956865b57d4c09a2ULL, 0x3fe2ad0cb6afa982ULL},
  };
  for (size_t k = 0; k < sizeof(vectors) / sizeof(vectors[0]); k++) {
    const uint64_t bits = mimic_sage_ffb_halo_uniform_bits(vectors[k].id, vectors[k].snap);
    const double deviate = mimic_sage_ffb_halo_uniform(vectors[k].id, vectors[k].snap);
    printf("\n  (%lld, %d): bits 0x%016llx, deviate 0x%016llx", vectors[k].id, vectors[k].snap,
           (unsigned long long)bits, (unsigned long long)bits_of(deviate));
    TEST_ASSERT(bits == vectors[k].bits, "the hashed bits must equal the frozen vector");
    TEST_ASSERT(bits_of(deviate) == vectors[k].deviate, "the deviate must equal the frozen vector");
    TEST_ASSERT(bits_of(mimic_sage_ffb_uniform_from_bits(bits)) == vectors[k].deviate,
                "the deviate from the bits must equal the composed draw");
  }
  return TEST_PASS;
}

/**
 * @test    test_deviate_endpoints
 * @brief   ((double)(x >> 11) + 0.5) * 2^-53 (R/src/model_regimes.c:158): 2^-54 for x 0, and
 *          exactly 1.0 for x all ones (2^53 - 1 + 0.5 rounds to 2^53: no endpoint correction)
 */
int test_deviate_endpoints(void) {
  const double low = mimic_sage_ffb_uniform_from_bits(0);
  const double high = mimic_sage_ffb_uniform_from_bits(UINT64_MAX);
  const double next = mimic_sage_ffb_uniform_from_bits(1ULL << 11);
  printf("\n  deviate(0) %a, deviate(1 << 11) %a, deviate(all ones) %a", low, next, high);
  TEST_ASSERT(low == 0x1.0p-54, "the deviate of 0 must be 0.5 * 2^-53");
  TEST_ASSERT(next == 0x1.8p-53, "the deviate of 1 << 11 must be 1.5 * 2^-53");
  TEST_ASSERT(high == 1.0, "the deviate of all ones must round to exactly 1.0");
  return TEST_PASS;
}

/**
 * @test    test_threshold_inline
 * @brief   The threshold and its locals equal SAGE26's expression written here at literal
 *          redshifts (R/src/model_regimes.c:309-312); z 9 gives z_norm 1 and log10 0, so the log
 *          is exactly 0.8 + log10(h)
 */
int test_threshold_inline(void) {
  const double redshifts[] = {0.0, 5.0, 7.0, 9.0, 12.5, 30.0};
  for (size_t k = 0; k < sizeof(redshifts) / sizeof(redshifts[0]); k++) {
    double z_norm, log_code;
    const double expected = sage26_threshold(redshifts[k], TEST_HUBBLE_H, &z_norm, &log_code);
    struct MimicSageFfbThresholdLocals l;
    const double got = mimic_sage_ffb_threshold_mass(redshifts[k], TEST_HUBBLE_H, &l);
    printf("\n  z %g: threshold %.17g (SAGE26's expression %.17g)", redshifts[k], got, expected);
    TEST_ASSERT(bits_of(got) == bits_of(expected), "the threshold must be SAGE26's");
    TEST_ASSERT(bits_of(l.z_norm) == bits_of(z_norm), "z_norm must be (1 + z) / 10");
    TEST_ASSERT(bits_of(l.log_Mvir_ffb_code) == bits_of(log_code),
                "log_Mvir_ffb_code must be SAGE26's fused log term");
  }
  struct MimicSageFfbThresholdLocals l;
  (void)mimic_sage_ffb_threshold_mass(9.0, TEST_HUBBLE_H, &l);
  volatile double h = TEST_HUBBLE_H;
  TEST_ASSERT(l.z_norm == 1.0 && l.log_Mvir_ffb_code == 0.8 + log10(h),
              "at z 9 the log must be exactly 0.8 + log10(h)");
  return TEST_PASS;
}

/**
 * @test    test_fraction_guards
 * @brief   Each guard of R/src/model_regimes.c:332 gives exactly 0 on its own; NaN passes them
 */
int test_fraction_guards(void) {
  const struct {
    double mvir;
    double z;
    const char *label;
  } guarded[] = {
      {0.0, 7.0, "Mvir +0"},
      {-0.0, 7.0, "Mvir -0"},
      {-1.0, 7.0, "Mvir -1"},
      {-INFINITY, 7.0, "Mvir -inf"},
      {1.0, 1e54, "a threshold of 0 (z 1e54)"},
      {0.0, -2.0, "Mvir +0 against a NaN threshold (z -2)"},
  };
  for (size_t k = 0; k < sizeof(guarded) / sizeof(guarded[0]); k++) {
    struct MimicSageFfbFractionLocals l;
    const double f = mimic_sage_ffb_fraction(guarded[k].mvir, guarded[k].z, TEST_HUBBLE_H, &l);
    printf("\n  %s: fraction %a, threshold %g, guard %d", guarded[k].label, f, l.Mvir_ffb,
           l.guard_returned);
    TEST_ASSERT(bits_of(f) == bits_of(0.0), "a guarded fraction must be exactly +0");
    TEST_ASSERT(l.guard_returned == 1 && isnan(l.x), "the guard must return before x");
  }
  struct MimicSageFfbFractionLocals l;
  TEST_ASSERT(isnan(mimic_sage_ffb_fraction(NAN, 7.0, TEST_HUBBLE_H, &l)) && l.guard_returned == 0,
              "a NaN Mvir must pass the guards and give a NaN fraction");
  TEST_ASSERT(isnan(mimic_sage_ffb_fraction(1.0, -2.0, TEST_HUBBLE_H, &l)) &&
                  l.guard_returned == 0 && isnan(l.Mvir_ffb),
              "a NaN threshold must pass the guard and give a NaN fraction");
  TEST_ASSERT(mimic_sage_ffb_fraction(INFINITY, 7.0, TEST_HUBBLE_H, &l) == 1.0,
              "an infinite Mvir must give exactly 1");
  TEST_ASSERT(mimic_sage_ffb_fraction(1.0, -1.0, TEST_HUBBLE_H, &l) == 0.0 &&
                  l.guard_returned == 0 && l.Mvir_ffb == INFINITY,
              "an infinite threshold (z -1) must give exactly 0 through x = -inf, not the guard");
  return TEST_PASS;
}

/**
 * @test    test_fraction_half
 * @brief   Mvir equal to the threshold: log10(1) / 0.15 = 0, so the fraction is exactly 0.5
 */
int test_fraction_half(void) {
  const double redshifts[] = {7.0, 9.0, 12.5};
  for (size_t k = 0; k < sizeof(redshifts) / sizeof(redshifts[0]); k++) {
    double z_norm, log_code;
    const double threshold = sage26_threshold(redshifts[k], TEST_HUBBLE_H, &z_norm, &log_code);
    struct MimicSageFfbFractionLocals l;
    const double f = mimic_sage_ffb_fraction(threshold, redshifts[k], TEST_HUBBLE_H, &l);
    printf("\n  z %g: Mvir = threshold %.17g -> fraction %a, x %a", redshifts[k], threshold, f,
           l.x);
    TEST_ASSERT(l.x == 0.0 && f == 0.5, "Mvir equal to the threshold must give exactly 0.5");
  }
  return TEST_PASS;
}

/**
 * @test    test_fraction_inline
 * @brief   The fraction across the sigmoid equals SAGE26's expression written here
 *          (R/src/model_regimes.c:330-339)
 */
int test_fraction_inline(void) {
  const double z = 7.0;
  double z_norm, log_code;
  const double threshold = sage26_threshold(z, TEST_HUBBLE_H, &z_norm, &log_code);
  const double factors[] = {0.01, 0.3, 0.8, 0.97, 1.03, 1.25, 3.0, 100.0};
  for (size_t k = 0; k < sizeof(factors) / sizeof(factors[0]); k++) {
    const double mvir = (double)(float)(threshold * factors[k]);
    const double expected = sage26_fraction(mvir, z, TEST_HUBBLE_H);
    struct MimicSageFfbFractionLocals l;
    const double f = mimic_sage_ffb_fraction(mvir, z, TEST_HUBBLE_H, &l);
    printf("\n  Mvir %.9g (threshold x %g): fraction %.17g (SAGE26's expression %.17g)", mvir,
           factors[k], f, expected);
    TEST_ASSERT(bits_of(f) == bits_of(expected), "the fraction must be SAGE26's");
    TEST_ASSERT(bits_of(l.Mvir_ffb) == bits_of(threshold), "the threshold must be SAGE26's");
  }
  return TEST_PASS;
}

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[4096];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/**
 * @test    test_validator
 * @brief   EnhancedStarFormationOn 0 and 1 are accepted; 2 (BK25, not ported), 3 and -1 refused,
 *          naming the parameter
 */
int test_validator(void) {
  TEST_ASSERT(mimic_sage_validate_enhanced_star_formation_on("test", 0) == 0, "0 is supported");
  TEST_ASSERT(mimic_sage_validate_enhanced_star_formation_on("test", 1) == 0, "1 is supported");
  const int refused[] = {2, 3, -1};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    FILE *capture = tmpfile();
    FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
    const int status = mimic_sage_validate_enhanced_star_formation_on("test", refused[k]);
    const char *log = "";
    if (capture != NULL) {
      set_log_output(old_out);
      log = read_captured_log(capture);
      fclose(capture);
    }
    printf("\n  EnhancedStarFormationOn %d: %d", refused[k], status);
    TEST_ASSERT(status != 0, "an unsupported value must be refused");
    TEST_ASSERT(strstr(log, "EnhancedStarFormationOn") != NULL, "the refusal must name it");
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: shared/sage_ffb_regime.h\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_draw_vectors);
  TEST_RUN(test_deviate_endpoints);
  TEST_RUN(test_threshold_inline);
  TEST_RUN(test_fraction_guards);
  TEST_RUN(test_fraction_half);
  TEST_RUN(test_fraction_inline);
  TEST_RUN(test_validator);

  TEST_SUMMARY();
  return TEST_RESULT();
}
