/**
 * @file    test_reservoir_table.c
 * @brief   Unit tests for the generated reservoir table
 *
 * Validates: in a test build, reservoir_table.h holds exactly the seven rate
 * fixture properties of src/module_system/test_fixture/test_properties.yaml, in
 * their YAML order, each with the kind, parent index, total index and struct
 * GalaxyData offset its `reservoir:` mapping declares, and the two conserved
 * totals by name.
 *
 * The expected rows below are written out by hand from the fixture YAML rather
 * than derived from the generator, so a generator that reorders entries, drops
 * one, resolves a parent or total to the wrong index, or emits a wrong offset
 * fails here. Unit tests always run against test-build generated code.
 */

#include "../framework/test_framework.h"
#include "../../src/include/generated/reservoir_table.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** One expected table row, as the fixture YAML declares it */
struct ExpectedReservoir {
  const char *name;
  int index;     /**< The entry's generated index symbol */
  size_t offset; /**< offsetof(struct GalaxyData, <name>), computed here */
  enum ReservoirKind kind;
  int parent; /**< Table index of a tracer's parent, else -1 */
  int total;  /**< 0 = rate_fixture_mass, 1 = rate_fixture_tracer, -1 = none */
};

static const struct ExpectedReservoir EXPECTED[] = {
    {"RateFixtureHot", RESERVOIR_INDEX_RateFixtureHot, offsetof(struct GalaxyData, RateFixtureHot),
     RESERVOIR_KIND_RESERVOIR, -1, 0},
    {"RateFixtureCold", RESERVOIR_INDEX_RateFixtureCold,
     offsetof(struct GalaxyData, RateFixtureCold), RESERVOIR_KIND_RESERVOIR, -1, 0},
    {"RateFixtureStars", RESERVOIR_INDEX_RateFixtureStars,
     offsetof(struct GalaxyData, RateFixtureStars), RESERVOIR_KIND_RESERVOIR, -1, 0},
    {"RateFixtureHotTracer", RESERVOIR_INDEX_RateFixtureHotTracer,
     offsetof(struct GalaxyData, RateFixtureHotTracer), RESERVOIR_KIND_TRACER, 0, 1},
    {"RateFixtureColdTracer", RESERVOIR_INDEX_RateFixtureColdTracer,
     offsetof(struct GalaxyData, RateFixtureColdTracer), RESERVOIR_KIND_TRACER, 1, 1},
    {"RateFixtureStarsTracer", RESERVOIR_INDEX_RateFixtureStarsTracer,
     offsetof(struct GalaxyData, RateFixtureStarsTracer), RESERVOIR_KIND_TRACER, 2, 1},
    {"RateFixtureFormed", RESERVOIR_INDEX_RateFixtureFormed,
     offsetof(struct GalaxyData, RateFixtureFormed), RESERVOIR_KIND_OBSERVER, -1, -1},
};

#define EXPECTED_COUNT ((int)(sizeof(EXPECTED) / sizeof(EXPECTED[0])))

/**
 * @test    test_fixture_entries_in_yaml_order
 * @brief   The table holds exactly the seven fixture entries, each as declared
 */
int test_fixture_entries_in_yaml_order(void) {
  TEST_ASSERT_EQUAL(MIMIC_RESERVOIR_COUNT, EXPECTED_COUNT,
                    "the test-build table holds exactly the seven fixture entries");

  const struct ReservoirEntry *table = mimic_reservoir_table();
  TEST_ASSERT(table != NULL, "a non-empty table has a non-NULL accessor");

  char msg[160];
  for (int i = 0; i < EXPECTED_COUNT; i++) {
    const struct ExpectedReservoir *want = &EXPECTED[i];
    const struct ReservoirEntry *got = &table[i];

    snprintf(msg, sizeof(msg), "row %d (%s): index symbol equals the row", i, want->name);
    TEST_ASSERT_EQUAL(want->index, i, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): name", i, want->name);
    TEST_ASSERT_STRING_EQUAL(got->name, want->name, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): offset equals offsetof(struct GalaxyData)", i,
             want->name);
    TEST_ASSERT_EQUAL(got->offset, want->offset, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): kind", i, want->name);
    TEST_ASSERT_EQUAL(got->kind, want->kind, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): parent index", i, want->name);
    TEST_ASSERT_EQUAL(got->parent, want->parent, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): total index", i, want->name);
    TEST_ASSERT_EQUAL(got->total, want->total, msg);
  }
  return TEST_PASS;
}

/**
 * @test    test_fixture_totals_by_name
 * @brief   The two conserved totals are rate_fixture_mass and rate_fixture_tracer, in that order
 */
int test_fixture_totals_by_name(void) {
  TEST_ASSERT_EQUAL(MIMIC_RESERVOIR_TOTAL_COUNT, 2, "the fixture declares two totals");

  const char *const *names = mimic_reservoir_total_names();
  TEST_ASSERT(names != NULL, "a build with totals has a non-NULL total-name accessor");
  TEST_ASSERT_STRING_EQUAL(names[0], "rate_fixture_mass", "total 0 is the reservoirs' total");
  TEST_ASSERT_STRING_EQUAL(names[1], "rate_fixture_tracer", "total 1 is the tracers' total");
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Generated Reservoir Table\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_fixture_entries_in_yaml_order);
  TEST_RUN(test_fixture_totals_by_name);

  TEST_SUMMARY();
  return TEST_RESULT();
}
