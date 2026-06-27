/**
 * @file    test_galaxy_id_encoding.c
 * @brief   Unit tests for UniqueGalaxyID encoding helper boundaries.
 */

#include "../framework/test_framework.h"

#include "galaxy_id.h"
#include "globals.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static int passed = 0, failed = 0;

/**
 * @test    test_max_forest_count
 * @brief   Reports the largest run-scoped forest count that cannot overflow the
 * two-term UniqueGalaxyID encoding.
 */
int test_max_forest_count(void) {
  const int64_t expected = (LLONG_MAX - (TREE_MUL_FAC - 1LL)) / TREE_MUL_FAC;

  TEST_ASSERT(mimic_unique_galaxy_id_max_forests() == expected,
              "maximum forest count should reserve space for the largest halo index");
  TEST_ASSERT(expected > 0, "maximum forest count should be positive");

  return TEST_PASS;
}

/**
 * @test    test_total_forest_validation_boundaries
 * @brief   Accepts zero and the capacity limit, then rejects negative and over-limit
 * total forest counts.
 */
int test_total_forest_validation_boundaries(void) {
  const int64_t max_forests = mimic_unique_galaxy_id_max_forests();

  TEST_ASSERT(mimic_unique_galaxy_id_total_forests_valid(0), "zero total forests should be valid");
  TEST_ASSERT(mimic_unique_galaxy_id_total_forests_valid(max_forests),
              "maximum total forest count should be valid");
  TEST_ASSERT(!mimic_unique_galaxy_id_total_forests_valid(max_forests + 1),
              "one forest over the limit should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_total_forests_valid(-1),
              "negative total forest count should be invalid");

  return TEST_PASS;
}

/**
 * @test    test_component_validation_boundaries
 * @brief   Enforces non-negative components, the maximum in-forest halo index, and the
 * maximum usable forest index.
 */
int test_component_validation_boundaries(void) {
  const int64_t max_forests = mimic_unique_galaxy_id_max_forests();

  TEST_ASSERT(mimic_unique_galaxy_id_components_valid(0, 0),
              "zero halo and forest components should be valid");
  TEST_ASSERT(mimic_unique_galaxy_id_components_valid(TREE_MUL_FAC - 1, max_forests - 1),
              "highest halo and forest indices should be valid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(TREE_MUL_FAC, 0),
              "halonr equal to TREE_MUL_FAC should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(-1, 0), "negative halonr should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(0, max_forests),
              "forest index equal to the maximum forest count should be invalid");
  TEST_ASSERT(!mimic_unique_galaxy_id_components_valid(0, -1),
              "negative forest index should be invalid");

  return TEST_PASS;
}

/**
 * @test    test_encoding_formula
 * @brief   Encodes valid components as halonr + TREE_MUL_FAC * (forestnr_global + 1),
 * reserving zero as a pure sentinel.
 */
int test_encoding_formula(void) {
  const int64_t max_forests = mimic_unique_galaxy_id_max_forests();
  const int64_t max_valid_id = max_forests * TREE_MUL_FAC + TREE_MUL_FAC - 1;

  TEST_ASSERT(mimic_encode_unique_galaxy_id(0, 0) == TREE_MUL_FAC,
              "first real galaxy should not encode to the zero sentinel");
  TEST_ASSERT(mimic_encode_unique_galaxy_id(123, 456) == 457 * TREE_MUL_FAC + 123,
              "non-zero components should use the one-based forest formula");
  TEST_ASSERT(mimic_encode_unique_galaxy_id(TREE_MUL_FAC - 1, max_forests - 1) == max_valid_id,
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
  TEST_RUN(test_total_forest_validation_boundaries);
  TEST_RUN(test_component_validation_boundaries);
  TEST_RUN(test_encoding_formula);
  TEST_RUN(test_global_forest_offset_default);

  TEST_SUMMARY();
  return TEST_RESULT();
}
