/**
 * @file    test_galaxy_id_encoding.c
 * @brief   Unit tests for UniqueGalaxyID encoding helper boundaries.
 *
 * The helpers take the run's configured forest multiplier explicitly, so every
 * test below names the multiplier it exercises. Two properties are pinned beyond
 * the per-helper boundaries: the unified bound expression (INT64_MAX / M - 1) at
 * several multipliers, and encoder equivalence with the pre-configurable formula
 * at the default multiplier.
 */

#include "../framework/test_framework.h"

#include "constants.h"
#include "galaxy_id.h"
#include "globals.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static int passed = 0, failed = 0;

/** The default forest multiplier, and the second value the tests encode with. */
#define DEFAULT_MULTIPLIER ((int64_t)TREE_MUL_FAC)
#define TEN_BILLION (10000000000LL)

/**
 * @test    test_max_forest_count
 * @brief   Reports the largest run-scoped forest count that cannot overflow the
 * two-term UniqueGalaxyID encoding, at the default multiplier.
 */
int test_max_forest_count(void) {
  /* The pre-slice expression, kept as an equivalence pin: it agrees with the
     unified INT64_MAX/M - 1 bound at the default multiplier, so unifying the two
     bound expressions in the codebase changed no accepted forest count here. */
  const int64_t expected = (LLONG_MAX - (DEFAULT_MULTIPLIER - 1LL)) / DEFAULT_MULTIPLIER;

  TEST_ASSERT(mimic_unique_galaxy_id_max_forests(DEFAULT_MULTIPLIER) == expected,
              "maximum forest count should reserve space for the largest halo index");
  TEST_ASSERT(expected > 0, "maximum forest count should be positive");

  return TEST_PASS;
}

/**
 * @test    test_unified_bound_across_multipliers
 * @brief   Pins max_forests == INT64_MAX / M - 1 exactly at M = 1, 2, 10^9 and
 * 10^10, and checks arithmetically that the extreme accepted components encode
 * without overflowing int64.
 *
 * M = 1 and M = 2 divide 2^63, which is exactly where the unified snapshot-form
 * bound is one stricter than the retired (LLONG_MAX - (M-1))/M form -- so those
 * two cases fail if the old expression ever returns.
 */
int test_unified_bound_across_multipliers(void) {
  const int64_t multipliers[4] = {1LL, 2LL, DEFAULT_MULTIPLIER, TEN_BILLION};

  for (int i = 0; i < 4; i++) {
    const int64_t m = multipliers[i];
    const int64_t max_forests = mimic_unique_galaxy_id_max_forests(m);

    TEST_ASSERT(max_forests == INT64_MAX / m - 1,
                "max_forests should be INT64_MAX / multiplier - 1 at every multiplier");
    TEST_ASSERT(max_forests > 0, "max_forests should be positive at every tested multiplier");

    /* Extreme accepted components: the largest halonr (m - 1) in the largest
       usable forest (max_forests - 1). */
    const int64_t max_halonr = m - 1;
    const int64_t max_forest_index = max_forests - 1;
    TEST_ASSERT(mimic_unique_galaxy_id_components_valid(m, max_halonr, max_forest_index),
                "the extreme components should be accepted by the component validator");

    /* Overflow headroom, computed independently of the helper and without
       overflowing: encoding those components yields m * max_forests + (m - 1),
       so it fits int64 iff max_forests <= (INT64_MAX - (m - 1)) / m. At M = 10^9
       the two sides are equal, so this is a tight check, not a loose one. */
    TEST_ASSERT(max_forests <= (INT64_MAX - max_halonr) / m,
                "the extreme accepted components must encode without overflowing int64");

    const int64_t encoded = mimic_encode_unique_galaxy_id(m, max_halonr, max_forest_index);
    TEST_ASSERT(encoded == m * (INT64_MAX / m) - 1,
                "the extreme accepted components should encode to m * (INT64_MAX / m) - 1");
    TEST_ASSERT(encoded > 0, "the extreme encoding should stay inside the positive int64 range");
    /* Round-trip: the encoding must be decomposable back into its components,
       which is what the output provenance attribute promises a reader. */
    TEST_ASSERT(encoded % m == max_halonr && encoded / m - 1 == max_forest_index,
                "the extreme encoding should decompose back into its own components");

    /* The first unrepresentable component pair in each direction is refused. */
    TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(m, max_halonr, max_forests),
                "a forest index at the bound should be rejected");
    TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(m, m, max_forest_index),
                "a halonr equal to the multiplier should be rejected");
  }

  return TEST_PASS;
}

/**
 * @test    test_encoder_equivalence_at_default_multiplier
 * @brief   At the default multiplier the encoder reproduces the pre-slice formula
 * halonr + 10^9 * (forestnr_global + 1) exactly.
 *
 * Every galaxy output written before the multiplier became configurable used that
 * formula, so this is the assertion that keeps historical ids valid.
 */
int test_encoder_equivalence_at_default_multiplier(void) {
  TEST_ASSERT(DEFAULT_MULTIPLIER == 1000000000LL, "the default multiplier should still be 10^9");

  const int64_t max_forests = mimic_unique_galaxy_id_max_forests(DEFAULT_MULTIPLIER);
  const int64_t halonrs[5] = {0, 1, 123, DEFAULT_MULTIPLIER / 2, DEFAULT_MULTIPLIER - 1};
  const int64_t forests[5] = {0, 1, 456, max_forests / 2, max_forests - 1};

  for (int h = 0; h < 5; h++) {
    for (int f = 0; f < 5; f++) {
      const int64_t reference = halonrs[h] + 1000000000LL * (forests[f] + 1LL);
      TEST_ASSERT(mimic_encode_unique_galaxy_id(DEFAULT_MULTIPLIER, halonrs[h], forests[f]) ==
                      reference,
                  "the default-multiplier encoding should match the pre-slice formula");
    }
  }

  /* A configured multiplier must actually change the encoding, or "honours the
     configured value" would be indistinguishable from ignoring it. */
  TEST_ASSERT(mimic_encode_unique_galaxy_id(TEN_BILLION, 0, 0) == TEN_BILLION,
              "a 10^10 multiplier should encode the first galaxy at 10^10, not 10^9");
  TEST_ASSERT(mimic_encode_unique_galaxy_id(TEN_BILLION, 123, 456) == 457LL * TEN_BILLION + 123LL,
              "a 10^10 multiplier should scale the forest term by 10^10");

  return TEST_PASS;
}

/**
 * @test    test_total_forest_validation_boundaries
 * @brief   Accepts zero and the capacity limit, then rejects negative and over-limit
 * total forest counts.
 */
int test_total_forest_validation_boundaries(void) {
  const int64_t max_forests = mimic_unique_galaxy_id_max_forests(DEFAULT_MULTIPLIER);

  TEST_ASSERT(mimic_unique_galaxy_id_total_forests_valid(DEFAULT_MULTIPLIER, 0),
              "zero total forests should be valid");
  TEST_ASSERT(mimic_unique_galaxy_id_total_forests_valid(DEFAULT_MULTIPLIER, max_forests),
              "maximum total forest count should be valid");
  TEST_ASSERT(!mimic_unique_galaxy_id_total_forests_valid(DEFAULT_MULTIPLIER, max_forests + 1),
              "one forest over the limit should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_total_forests_valid(DEFAULT_MULTIPLIER, -1),
              "negative total forest count should be invalid");

  /* The limit tracks the configured multiplier: a larger multiplier leaves room
     for FEWER forests, so the default's limit is rejected at 10^10 while the
     10^10 limit is itself accepted there. */
  const int64_t max_forests_ten_billion = mimic_unique_galaxy_id_max_forests(TEN_BILLION);
  TEST_ASSERT(max_forests_ten_billion < max_forests,
              "a ten-fold larger multiplier should leave room for fewer forests");
  TEST_ASSERT(mimic_unique_galaxy_id_total_forests_valid(TEN_BILLION, max_forests_ten_billion),
              "the 10^10 limit should be valid under the 10^10 multiplier");
  TEST_ASSERT(!mimic_unique_galaxy_id_total_forests_valid(TEN_BILLION, max_forests),
              "the default limit should be rejected under the 10^10 multiplier");

  return TEST_PASS;
}

/**
 * @test    test_component_validation_boundaries
 * @brief   Enforces non-negative components, the maximum in-forest halo index, and the
 * maximum usable forest index.
 */
int test_component_validation_boundaries(void) {
  const int64_t max_forests = mimic_unique_galaxy_id_max_forests(DEFAULT_MULTIPLIER);

  TEST_ASSERT(mimic_unique_galaxy_id_components_valid(DEFAULT_MULTIPLIER, 0, 0),
              "zero halo and forest components should be valid");
  TEST_ASSERT(mimic_unique_galaxy_id_components_valid(DEFAULT_MULTIPLIER, DEFAULT_MULTIPLIER - 1,
                                                      max_forests - 1),
              "highest halo and forest indices should be valid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(DEFAULT_MULTIPLIER, DEFAULT_MULTIPLIER, 0),
              "halonr equal to the multiplier should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(DEFAULT_MULTIPLIER, -1, 0),
              "negative halonr should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(DEFAULT_MULTIPLIER, 0, max_forests),
              "forest index equal to the maximum forest count should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(DEFAULT_MULTIPLIER, 0, -1),
              "negative forest index should be invalid");

  /* A halo index the default multiplier cannot represent becomes representable
     under a larger configured multiplier -- the whole point of configuring it. */
  TEST_ASSERT(mimic_unique_galaxy_id_components_valid(TEN_BILLION, DEFAULT_MULTIPLIER, 0),
              "a 10^9 halo index should fit a 10^10 multiplier");

  return TEST_PASS;
}

/**
 * @test    test_encoding_formula
 * @brief   Encodes valid components as halonr + multiplier * (forestnr_global + 1),
 * reserving zero as a pure sentinel.
 */
int test_encoding_formula(void) {
  const int64_t max_forests = mimic_unique_galaxy_id_max_forests(DEFAULT_MULTIPLIER);
  const int64_t max_valid_id = max_forests * DEFAULT_MULTIPLIER + DEFAULT_MULTIPLIER - 1;

  TEST_ASSERT(mimic_encode_unique_galaxy_id(DEFAULT_MULTIPLIER, 0, 0) == DEFAULT_MULTIPLIER,
              "first real galaxy should not encode to the zero sentinel");
  TEST_ASSERT(mimic_encode_unique_galaxy_id(DEFAULT_MULTIPLIER, 123, 456) ==
                  457 * DEFAULT_MULTIPLIER + 123,
              "non-zero components should use the one-based forest formula");
  TEST_ASSERT(mimic_encode_unique_galaxy_id(DEFAULT_MULTIPLIER, DEFAULT_MULTIPLIER - 1,
                                            max_forests - 1) == max_valid_id,
              "highest valid components should encode to the highest valid ID");

  return TEST_PASS;
}

/**
 * @test    test_global_forest_offset_default
 * @brief   New runtime state is available and starts at zero in the current slice.
 */
int test_global_forest_offset_default(void) {
  TEST_ASSERT(GlobalForestOffset == 0, "GlobalForestOffset should default to zero");

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: UniqueGalaxyID Encoding\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_max_forest_count);
  TEST_RUN(test_unified_bound_across_multipliers);
  TEST_RUN(test_encoder_equivalence_at_default_multiplier);
  TEST_RUN(test_total_forest_validation_boundaries);
  TEST_RUN(test_component_validation_boundaries);
  TEST_RUN(test_encoding_formula);
  TEST_RUN(test_global_forest_offset_default);

  TEST_SUMMARY();
  return TEST_RESULT();
}
