/**
 * @file    test_unit_sage_keyed_draw.c
 * @brief   Unit tests of the package's keyed uniform draw (shared/sage_keyed_draw.h, D1)
 *
 * The keyed draw replaces SAGE26's libc draw in the CGM regime classifier
 * (R/src/model_regimes.c:94; campaign decision D1). Its key contract is frozen by the c10 node
 * plan, which also gives eight reference vectors computed by the Campaign Manager with an
 * independent Python implementation of the frozen text. These tests are platform independent
 * (integer arithmetic and exact double conversions only):
 *
 * - test_reference_vectors: all eight vectors, `bits` and the deviate, bit for bit;
 * - test_deviate_strictly_inside_unit_interval: the deviate of the extreme bit patterns
 *   (2^-53 and 1 - 2^-53) and of a sweep of keys lies strictly inside (0, 1);
 * - test_pure_function_of_inputs: repeated calls and a reordered sequence of calls give the
 *   same values;
 * - test_each_input_changes_the_draw: a change in any one of the four inputs (UniqueGalaxyID,
 *   snapshot, purpose salt, ordinal) changes the bits and the deviate;
 * - test_cgm_wrapper_is_the_contract: the classifier's wrapper is the CGM purpose and ordinal 0.
 */

#include "../../../../tests/framework/test_framework.h"
#include "shared/sage_keyed_draw.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** One frozen reference vector of the campaign rules (c10 node plan, keyed draw) */
struct draw_vector {
  int64_t galaxy_id;
  int snapshot;
  uint64_t ordinal;
  uint64_t bits;  /* the expected keyed bits */
  double deviate; /* the expected deviate, as the plan's hexadecimal double */
};

static const struct draw_vector VECTORS[] = {
    {0, 0, 0, UINT64_C(0x600f3d65cab0d697), 0x1.803cf5972ac36p-2},
    {1, 63, 0, UINT64_C(0x0916b00c4a9b61fe), 0x1.22d60189536d0p-5},
    {INT64_C(1000000000123), 37, 0, UINT64_C(0x060f98fcec722e1f), 0x1.83e63f3b1c8a0p-6},
    {-5, 8, 0, UINT64_C(0x9c1fad9e65e48c21), 0x1.383f5b3ccbc91p-1},
    {INT64_C(1000000000123), 38, 0, UINT64_C(0x64309bb56590e1f3), 0x1.90c26ed59643ap-2},
    {INT64_C(1000000000124), 37, 0, UINT64_C(0x5f00daf2f53b6350), 0x1.7c036bcbd4edap-2},
    {INT64_C(1000000000123), 37, 1, UINT64_C(0x54c26ced2ceb1f4a), 0x1.5309b3b4b3ac6p-2},
    {INT64_C(9223372036854775807), 63, 0, UINT64_C(0x94aaf0f9d5a99164), 0x1.2955e1f3ab533p-1},
};

#define NUM_VECTORS ((int)(sizeof(VECTORS) / sizeof(VECTORS[0])))

/** @brief Bit pattern of a double, for exact comparisons */
static uint64_t double_bits(const double v) {
  uint64_t b;
  memcpy(&b, &v, sizeof(b));
  return b;
}

/**
 * @test    test_reference_vectors
 * @brief   The eight frozen reference vectors, `bits` and deviate, bit for bit
 */
int test_reference_vectors(void) {
  int mismatches = 0;
  for (int i = 0; i < NUM_VECTORS; i++) {
    const struct draw_vector *v = &VECTORS[i];
    const uint64_t bits =
        mimic_sage_keyed_bits(v->galaxy_id, v->snapshot, MIMIC_SAGE_DRAW_PURPOSE_CGM, v->ordinal);
    const double u = mimic_sage_keyed_uniform(v->galaxy_id, v->snapshot,
                                              MIMIC_SAGE_DRAW_PURPOSE_CGM, v->ordinal);
    const int ok = bits == v->bits && double_bits(u) == double_bits(v->deviate);
    printf("\n  (%lld, %d, %llu) -> bits 0x%016llx deviate %a (expected 0x%016llx %a) %s",
           (long long)v->galaxy_id, v->snapshot, (unsigned long long)v->ordinal,
           (unsigned long long)bits, u, (unsigned long long)v->bits, v->deviate,
           ok ? "ok" : "MISMATCH");
    mismatches += !ok;
  }
  TEST_ASSERT(mismatches == 0, "every reference vector must be reproduced bit for bit");
  return TEST_PASS;
}

/**
 * @test    test_deviate_strictly_inside_unit_interval
 * @brief   The deviate lies strictly inside (0, 1): the extreme bit patterns and a key sweep
 */
int test_deviate_strictly_inside_unit_interval(void) {
  /* The extremes of the mapping: all-zero bits give 2^-53, all-one bits 1 - 2^-53 */
  const double lowest = mimic_sage_keyed_uniform_from_bits(UINT64_C(0));
  const double highest = mimic_sage_keyed_uniform_from_bits(UINT64_MAX);
  printf("\n  extremes: %a, %a", lowest, highest);
  TEST_ASSERT(double_bits(lowest) == double_bits(0x1.0p-53), "all-zero bits must give 2^-53");
  TEST_ASSERT(double_bits(highest) == double_bits(1.0 - 0x1.0p-53),
              "all-one bits must give 1 - 2^-53");
  TEST_ASSERT(lowest > 0.0 && highest < 1.0, "the extremes must lie strictly inside (0, 1)");

  /* The low 12 bits are discarded: every pattern of them gives the same deviate */
  TEST_ASSERT(double_bits(mimic_sage_keyed_uniform_from_bits(UINT64_C(0xfff))) ==
                  double_bits(lowest),
              "the low 12 bits must not enter the deviate");

  /* A sweep of keys: IDs of both signs, every snapshot 0-63, two ordinals */
  long long count = 0;
  double min_u = 1.0;
  double max_u = 0.0;
  for (int64_t id = -2000; id <= 2000; id += 7) {
    for (int snap = 0; snap < 64; snap++) {
      for (uint64_t ordinal = 0; ordinal < 2; ordinal++) {
        const double u = mimic_sage_keyed_uniform(id * INT64_C(1000000007), snap,
                                                  MIMIC_SAGE_DRAW_PURPOSE_CGM, ordinal);
        TEST_ASSERT(u > 0.0 && u < 1.0 && isfinite(u), "every deviate must lie inside (0, 1)");
        min_u = u < min_u ? u : min_u;
        max_u = u > max_u ? u : max_u;
        count++;
      }
    }
  }
  printf("\n  %lld swept deviates in [%.17g, %.17g]", count, min_u, max_u);
  return TEST_PASS;
}

/**
 * @test    test_pure_function_of_inputs
 * @brief   Repeated calls and a reordered sequence of calls give the same values
 */
int test_pure_function_of_inputs(void) {
  enum { N = 64 };
  int64_t ids[N];
  int snaps[N];
  uint64_t forward[N];
  for (int i = 0; i < N; i++) {
    ids[i] = (int64_t)i * INT64_C(1000000000) + 17 * i - 300;
    snaps[i] = (i * 13) % 64;
    forward[i] = mimic_sage_keyed_bits(ids[i], snaps[i], MIMIC_SAGE_DRAW_PURPOSE_CGM, 0);
  }

  /* The same sequence again, then the sequence backwards, then an interleaved order */
  for (int i = 0; i < N; i++) {
    TEST_ASSERT(mimic_sage_keyed_bits(ids[i], snaps[i], MIMIC_SAGE_DRAW_PURPOSE_CGM, 0) ==
                    forward[i],
                "a repeated call must give the same bits");
  }
  for (int i = N - 1; i >= 0; i--) {
    TEST_ASSERT(mimic_sage_keyed_bits(ids[i], snaps[i], MIMIC_SAGE_DRAW_PURPOSE_CGM, 0) ==
                    forward[i],
                "a reversed sequence of calls must give the same bits");
  }
  for (int i = 0; i < N; i++) {
    const int j = (i * 37 + 11) % N; /* 37 is coprime to 64: a permutation */
    /* An unrelated draw between the calls must not disturb them (no hidden state) */
    (void)mimic_sage_keyed_uniform(-1, 99, MIMIC_SAGE_DRAW_PURPOSE_CGM, 5);
    TEST_ASSERT(mimic_sage_cgm_regime_deviate(ids[j], snaps[j]) ==
                    mimic_sage_keyed_uniform_from_bits(forward[j]),
                "a permuted sequence of calls must give the same deviates");
  }
  printf("\n  %d keys reproduced in forward, reversed and permuted order", N);
  return TEST_PASS;
}

/**
 * @test    test_each_input_changes_the_draw
 * @brief   A change in any one input (ID, snapshot, purpose salt, ordinal) changes the draw
 */
int test_each_input_changes_the_draw(void) {
  const int64_t id = INT64_C(1000000000123);
  const int snap = 37;
  const uint64_t purpose = MIMIC_SAGE_DRAW_PURPOSE_CGM;
  const uint64_t base = mimic_sage_keyed_bits(id, snap, purpose, 0);
  const double base_u = mimic_sage_keyed_uniform_from_bits(base);

  const uint64_t other_id = mimic_sage_keyed_bits(id + 1, snap, purpose, 0);
  const uint64_t other_negative_id = mimic_sage_keyed_bits(-id, snap, purpose, 0);
  const uint64_t other_snap = mimic_sage_keyed_bits(id, snap + 1, purpose, 0);
  const uint64_t other_purpose = mimic_sage_keyed_bits(id, snap, purpose ^ UINT64_C(1), 0);
  const uint64_t other_ordinal = mimic_sage_keyed_bits(id, snap, purpose, 1);
  printf("\n  base 0x%016llx; id+1 0x%016llx; -id 0x%016llx; snap+1 0x%016llx; purpose^1 "
         "0x%016llx; ordinal 1 0x%016llx",
         (unsigned long long)base, (unsigned long long)other_id,
         (unsigned long long)other_negative_id, (unsigned long long)other_snap,
         (unsigned long long)other_purpose, (unsigned long long)other_ordinal);

  TEST_ASSERT(other_id != base && mimic_sage_keyed_uniform_from_bits(other_id) != base_u,
              "a different UniqueGalaxyID must change the draw");
  TEST_ASSERT(other_negative_id != base &&
                  mimic_sage_keyed_uniform_from_bits(other_negative_id) != base_u,
              "a negated UniqueGalaxyID must change the draw");
  TEST_ASSERT(other_snap != base && mimic_sage_keyed_uniform_from_bits(other_snap) != base_u,
              "a different snapshot must change the draw");
  TEST_ASSERT(other_purpose != base && mimic_sage_keyed_uniform_from_bits(other_purpose) != base_u,
              "a different purpose salt must change the draw");
  TEST_ASSERT(other_ordinal != base && mimic_sage_keyed_uniform_from_bits(other_ordinal) != base_u,
              "a different ordinal must change the draw");

  /* Neighbouring IDs, snapshots and ordinals over a range: every one changes the draw */
  int collisions = 0;
  for (int k = 1; k <= 1000; k++) {
    collisions += mimic_sage_keyed_bits(id + k, snap, purpose, 0) == base;
    collisions += mimic_sage_keyed_bits(id, snap + k, purpose, 0) == base;
    collisions += mimic_sage_keyed_bits(id, snap, purpose, (uint64_t)k) == base;
  }
  TEST_ASSERT(collisions == 0, "no neighbouring key may reproduce the base draw");
  return TEST_PASS;
}

/**
 * @test    test_cgm_wrapper_is_the_contract
 * @brief   The classifier's deviate is the CGM purpose with ordinal 0 (the vectors' purpose)
 */
int test_cgm_wrapper_is_the_contract(void) {
  TEST_ASSERT(MIMIC_SAGE_DRAW_ORDINAL_CGM == 0, "the classifier's ordinal must be 0");
  for (int i = 0; i < NUM_VECTORS; i++) {
    const struct draw_vector *v = &VECTORS[i];
    if (v->ordinal != 0) {
      continue;
    }
    const double u = mimic_sage_cgm_regime_deviate(v->galaxy_id, v->snapshot);
    TEST_ASSERT(double_bits(u) == double_bits(v->deviate),
                "the CGM wrapper must reproduce each ordinal-0 vector's deviate");
  }
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: Keyed uniform draw (sage_keyed_draw.h, D1)\n");
  printf("============================================================\n");

  TEST_RUN(test_reference_vectors);
  TEST_RUN(test_deviate_strictly_inside_unit_interval);
  TEST_RUN(test_pure_function_of_inputs);
  TEST_RUN(test_each_input_changes_the_draw);
  TEST_RUN(test_cgm_wrapper_is_the_contract);

  TEST_SUMMARY();

  return TEST_RESULT();
}
