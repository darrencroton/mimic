/**
 * @file    test_reservoir_table.c
 * @brief   Unit tests for the generated reservoir table
 *
 * Validates: in a test build, reservoir_table.h ends with the seven rate fixture
 * properties of src/module_system/test_fixture/test_properties.yaml (a test build
 * appends them after the model package's own properties), in their YAML order,
 * each with the kind, parent, total and struct GalaxyData offset its `reservoir:`
 * mapping declares, and the two conserved totals by name.
 *
 * The expected rows below are written out by hand from the fixture YAML rather
 * than derived from the generator, so a generator that reorders entries, drops
 * one, resolves a parent or total to the wrong index, or emits a wrong offset
 * fails here. Rows are located by their RESERVOIR_INDEX_* symbols, parents are
 * asserted by symbol and totals by name, so a model package that declares
 * reservoirs of its own, which come first in the table and name totals first,
 * does not break the test. Unit tests always run against test-build generated code.
 */

#include "../framework/test_framework.h"
#include "../../src/include/generated/reservoir_table.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

#define FIXTURE_MASS_TOTAL "rate_fixture_mass"
#define FIXTURE_TRACER_TOTAL "rate_fixture_tracer"

/** One expected table row, as the fixture YAML declares it */
struct ExpectedReservoir {
  const char *name;
  int index;     /**< The entry's generated index symbol */
  size_t offset; /**< offsetof(struct GalaxyData, <name>), computed here */
  enum ReservoirKind kind;
  int parent;        /**< Index symbol of a tracer's parent, else -1 */
  const char *total; /**< Name of the entry's conserved total, NULL for none */
};

static const struct ExpectedReservoir EXPECTED[] = {
    {"RateFixtureHot", RESERVOIR_INDEX_RateFixtureHot, offsetof(struct GalaxyData, RateFixtureHot),
     RESERVOIR_KIND_RESERVOIR, -1, FIXTURE_MASS_TOTAL},
    {"RateFixtureCold", RESERVOIR_INDEX_RateFixtureCold,
     offsetof(struct GalaxyData, RateFixtureCold), RESERVOIR_KIND_RESERVOIR, -1,
     FIXTURE_MASS_TOTAL},
    {"RateFixtureStars", RESERVOIR_INDEX_RateFixtureStars,
     offsetof(struct GalaxyData, RateFixtureStars), RESERVOIR_KIND_RESERVOIR, -1,
     FIXTURE_MASS_TOTAL},
    {"RateFixtureHotTracer", RESERVOIR_INDEX_RateFixtureHotTracer,
     offsetof(struct GalaxyData, RateFixtureHotTracer), RESERVOIR_KIND_TRACER,
     RESERVOIR_INDEX_RateFixtureHot, FIXTURE_TRACER_TOTAL},
    {"RateFixtureColdTracer", RESERVOIR_INDEX_RateFixtureColdTracer,
     offsetof(struct GalaxyData, RateFixtureColdTracer), RESERVOIR_KIND_TRACER,
     RESERVOIR_INDEX_RateFixtureCold, FIXTURE_TRACER_TOTAL},
    {"RateFixtureStarsTracer", RESERVOIR_INDEX_RateFixtureStarsTracer,
     offsetof(struct GalaxyData, RateFixtureStarsTracer), RESERVOIR_KIND_TRACER,
     RESERVOIR_INDEX_RateFixtureStars, FIXTURE_TRACER_TOTAL},
    {"RateFixtureFormed", RESERVOIR_INDEX_RateFixtureFormed,
     offsetof(struct GalaxyData, RateFixtureFormed), RESERVOIR_KIND_OBSERVER, -1, NULL},
};

#define EXPECTED_COUNT ((int)(sizeof(EXPECTED) / sizeof(EXPECTED[0])))

/**
 * @test    test_fixture_entries_in_yaml_order
 * @brief   The table ends with the seven fixture entries, each as declared
 */
int test_fixture_entries_in_yaml_order(void) {
  TEST_ASSERT(MIMIC_RESERVOIR_COUNT >= EXPECTED_COUNT,
              "the test-build table holds at least the seven fixture entries");
  TEST_ASSERT_EQUAL(RESERVOIR_INDEX_RateFixtureHot, MIMIC_RESERVOIR_COUNT - EXPECTED_COUNT,
                    "the fixture entries are the tail of the table");

  const struct ReservoirEntry *table = mimic_reservoir_table();
  TEST_ASSERT(table != NULL, "a non-empty table has a non-NULL accessor");
  const char *const *total_names = mimic_reservoir_total_names();

  char msg[160];
  for (int i = 0; i < EXPECTED_COUNT; i++) {
    const struct ExpectedReservoir *want = &EXPECTED[i];
    const int row = RESERVOIR_INDEX_RateFixtureHot + i;
    const struct ReservoirEntry *got = &table[want->index];

    snprintf(msg, sizeof(msg), "row %d (%s): index symbol is the fixture's %dth row", row,
             want->name, i);
    TEST_ASSERT_EQUAL(want->index, row, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): name", row, want->name);
    TEST_ASSERT_STRING_EQUAL(got->name, want->name, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): offset equals offsetof(struct GalaxyData)", row,
             want->name);
    TEST_ASSERT_EQUAL(got->offset, want->offset, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): kind", row, want->name);
    TEST_ASSERT_EQUAL(got->kind, want->kind, msg);
    snprintf(msg, sizeof(msg), "row %d (%s): parent index", row, want->name);
    TEST_ASSERT_EQUAL(got->parent, want->parent, msg);

    if (want->total == NULL) {
      snprintf(msg, sizeof(msg), "row %d (%s): no total", row, want->name);
      TEST_ASSERT_EQUAL(got->total, -1, msg);
    } else {
      snprintf(msg, sizeof(msg), "row %d (%s): total index is in range", row, want->name);
      TEST_ASSERT(got->total >= 0 && got->total < MIMIC_RESERVOIR_TOTAL_COUNT, msg);
      snprintf(msg, sizeof(msg), "row %d (%s): total name", row, want->name);
      TEST_ASSERT_STRING_EQUAL(total_names[got->total], want->total, msg);
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_fixture_totals_by_name
 * @brief   rate_fixture_mass and rate_fixture_tracer are declared totals, in that order
 */
int test_fixture_totals_by_name(void) {
  TEST_ASSERT(MIMIC_RESERVOIR_TOTAL_COUNT >= 2, "the fixture declares at least two totals");

  const char *const *names = mimic_reservoir_total_names();
  TEST_ASSERT(names != NULL, "a build with totals has a non-NULL total-name accessor");

  int mass = -1;
  int tracer = -1;
  for (int i = 0; i < MIMIC_RESERVOIR_TOTAL_COUNT; i++) {
    if (strcmp(names[i], FIXTURE_MASS_TOTAL) == 0) {
      mass = i;
    } else if (strcmp(names[i], FIXTURE_TRACER_TOTAL) == 0) {
      tracer = i;
    }
  }
  TEST_ASSERT(mass >= 0, "the reservoirs' total is declared");
  TEST_ASSERT(tracer >= 0, "the tracers' total is declared");
  TEST_ASSERT(mass < tracer, "the reservoirs' total is named before the tracers' total");
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
