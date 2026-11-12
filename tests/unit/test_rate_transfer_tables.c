/**
 * @file    test_rate_transfer_tables.c
 * @brief   Unit tests for the generated rate transfer tables and the rate fixture laws
 *
 * Validates, against the four rate fixture modules under src/module_system/
 * (test_rate_supply, test_rate_formation, test_rate_feedback and
 * test_rate_stripping):
 * - Each module's generated registration carries every declared transfer in
 *   declaration order, with its name, kind, reservoir table indices for both
 *   ends, recipient record and coefficient index constant; every declared
 *   observer; a reads_central flag that is false; and the rates callback bound
 *   with no FoF or snapshot callback
 * - The slaved source test_rate_feedback.yield resolves to the global index of
 *   test_rate_formation.star_formation, declared by another module
 * - The global transfer index orders transfers by module name, then
 *   declaration order: asserted as relative order only, never as absolute index
 *   values, so a rate module added later does not break the test
 * - Each fixture law, called directly on hand-built rows after its init() has
 *   read its parameters, returns the coefficients its README states
 *
 * The expected rows and coefficients below are written out by hand from the
 * fixtures' module_info.yaml and READMEs rather than derived from the
 * generator or the laws. Unit tests always run against test-build generated
 * code, so the fixtures are always compiled here.
 */

#include "../framework/test_framework.h"
#include "../../src/core/module_interface.h"
#include "../../src/include/generated/reservoir_table.h"
#include "../../src/include/globals.h"
#include "../../src/include/types.h"
#include "../../src/module_system/generated/rate_contracts.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Generated registrations of the rate fixtures (module_init.c gives rate modules external
 * linkage so these tables can be read here) */
extern struct Module test_rate_supply_module;
extern struct Module test_rate_formation_module;
extern struct Module test_rate_feedback_module;
extern struct Module test_rate_stripping_module;

/* The fixture lifecycle functions under test */
extern int test_rate_supply_init(void);
extern int test_rate_supply_rates(const struct RateContext *ctx, const struct Halo *halo,
                                  double *coefficients);
extern int test_rate_formation_init(void);
extern int test_rate_formation_rates(const struct RateContext *ctx, const struct Halo *halo,
                                     double *coefficients);
extern int test_rate_feedback_init(void);
extern int test_rate_feedback_rates(const struct RateContext *ctx, const struct Halo *halo,
                                    double *coefficients);
extern int test_rate_stripping_init(void);
extern int test_rate_stripping_rates(const struct RateContext *ctx, const struct Halo *halo,
                                     double *coefficients);

/** Relative tolerance for a law's coefficient against its hand-computed value */
#define LAW_RTOL 1e-15

/* ==========================================================================
 * Expected tables, from the fixtures' module_info.yaml
 * ========================================================================== */

/** One expected transfer descriptor, as a fixture declares it */
struct ExpectedTransfer {
  const char *name;
  int coefficient_index; /**< The transfer's generated coefficient index constant */
  enum RateTransferKind kind;
  int from; /**< Reservoir index symbol or RATE_TRANSFER_EXTERNAL */
  int to;   /**< Reservoir index symbol or RATE_TRANSFER_EXTERNAL */
  enum RateTransferRecord to_record;
  const char *slaved_to; /**< Declared master, or NULL */
};

static const struct ExpectedTransfer SUPPLY[] = {
    {"infall", TEST_RATE_SUPPLY_TRANSFER_INFALL, RATE_TRANSFER_SOURCE, RATE_TRANSFER_EXTERNAL,
     RESERVOIR_INDEX_RateFixtureHot, RATE_TRANSFER_TO_SELF, NULL},
    {"infall_tracer", TEST_RATE_SUPPLY_TRANSFER_INFALL_TRACER, RATE_TRANSFER_SOURCE,
     RATE_TRANSFER_EXTERNAL, RESERVOIR_INDEX_RateFixtureHotTracer, RATE_TRANSFER_TO_SELF, NULL},
    {"cooling", TEST_RATE_SUPPLY_TRANSFER_COOLING, RATE_TRANSFER_INTERNAL,
     RESERVOIR_INDEX_RateFixtureHot, RESERVOIR_INDEX_RateFixtureCold, RATE_TRANSFER_TO_SELF, NULL},
};

static const struct ExpectedTransfer FORMATION[] = {
    {"star_formation", TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION, RATE_TRANSFER_INTERNAL,
     RESERVOIR_INDEX_RateFixtureCold, RESERVOIR_INDEX_RateFixtureStars, RATE_TRANSFER_TO_SELF,
     NULL},
};

static const struct ExpectedTransfer FEEDBACK[] = {
    {"reheating", TEST_RATE_FEEDBACK_TRANSFER_REHEATING, RATE_TRANSFER_INTERNAL,
     RESERVOIR_INDEX_RateFixtureCold, RESERVOIR_INDEX_RateFixtureHot, RATE_TRANSFER_TO_SELF, NULL},
    {"yield", TEST_RATE_FEEDBACK_TRANSFER_YIELD, RATE_TRANSFER_SLAVED_SOURCE,
     RATE_TRANSFER_EXTERNAL, RESERVOIR_INDEX_RateFixtureColdTracer, RATE_TRANSFER_TO_SELF,
     "test_rate_formation.star_formation"},
};

static const struct ExpectedTransfer STRIPPING[] = {
    {"stripping", TEST_RATE_STRIPPING_TRANSFER_STRIPPING, RATE_TRANSFER_INTERNAL,
     RESERVOIR_INDEX_RateFixtureHot, RESERVOIR_INDEX_RateFixtureHot, RATE_TRANSFER_TO_CENTRAL,
     NULL},
    {"ejection", TEST_RATE_STRIPPING_TRANSFER_EJECTION, RATE_TRANSFER_SINK,
     RESERVOIR_INDEX_RateFixtureCold, RATE_TRANSFER_EXTERNAL, RATE_TRANSFER_TO_SELF, NULL},
};

#define COUNT_OF(array) ((int)(sizeof(array) / sizeof((array)[0])))

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/**
 * @brief   Check one fixture's generated registration against its declarations
 *
 * Covers the transfer table in declaration order, the bound callbacks, the
 * reads_central flag (false for every fixture) and the observer count; a
 * module's observers themselves are checked by the caller.
 */
static int check_module_table(const struct Module *module, const char *name,
                              const struct ExpectedTransfer *want, int count, int num_transfers,
                              int num_observers) {
  char msg[200];
  snprintf(msg, sizeof(msg), "%s: registration name", name);
  TEST_ASSERT_STRING_EQUAL(module->name, name, msg);
  snprintf(msg, sizeof(msg), "%s: the rates callback is bound", name);
  TEST_ASSERT(module->rates != NULL, msg);
  snprintf(msg, sizeof(msg), "%s: no FoF or snapshot callback", name);
  TEST_ASSERT(module->process == NULL && module->process_snapshot == NULL, msg);
  snprintf(msg, sizeof(msg), "%s: reads_central is false (the fixture does not declare it)", name);
  TEST_ASSERT(module->reads_central == false, msg);

  snprintf(msg, sizeof(msg), "%s: one descriptor per declared transfer", name);
  TEST_ASSERT_EQUAL(module->num_rate_transfers, count, msg);
  snprintf(msg, sizeof(msg), "%s: the generated count macro equals the declarations", name);
  TEST_ASSERT_EQUAL(num_transfers, count, msg);
  snprintf(msg, sizeof(msg), "%s: the transfer table is non-NULL", name);
  TEST_ASSERT(module->rate_transfers != NULL, msg);
  snprintf(msg, sizeof(msg), "%s: observer count", name);
  TEST_ASSERT_EQUAL(module->num_rate_observers, num_observers, msg);
  snprintf(msg, sizeof(msg), "%s: observer table is NULL exactly when there are none", name);
  TEST_ASSERT((module->rate_observers == NULL) == (num_observers == 0), msg);

  for (int i = 0; i < count; i++) {
    const struct RateTransfer *got = &module->rate_transfers[i];
    const struct ExpectedTransfer *exp = &want[i];
    snprintf(msg, sizeof(msg), "%s[%d]: name in declaration order", name, i);
    TEST_ASSERT_STRING_EQUAL(got->name, exp->name, msg);
    snprintf(msg, sizeof(msg), "%s.%s: coefficient index is the declaration position", name,
             exp->name);
    TEST_ASSERT_EQUAL(got->coefficient_index, i, msg);
    snprintf(msg, sizeof(msg), "%s.%s: coefficient index equals its generated constant", name,
             exp->name);
    TEST_ASSERT_EQUAL(got->coefficient_index, exp->coefficient_index, msg);
    snprintf(msg, sizeof(msg), "%s.%s: kind", name, exp->name);
    TEST_ASSERT_EQUAL(got->kind, exp->kind, msg);
    snprintf(msg, sizeof(msg), "%s.%s: from is the declared reservoir index", name, exp->name);
    TEST_ASSERT_EQUAL(got->from, exp->from, msg);
    snprintf(msg, sizeof(msg), "%s.%s: to is the declared reservoir index", name, exp->name);
    TEST_ASSERT_EQUAL(got->to, exp->to, msg);
    snprintf(msg, sizeof(msg), "%s.%s: to_record", name, exp->name);
    TEST_ASSERT_EQUAL(got->to_record, exp->to_record, msg);
    snprintf(msg, sizeof(msg), "%s.%s: global index lies in the global transfer index", name,
             exp->name);
    TEST_ASSERT(got->global_index >= 0 && got->global_index < MIMIC_RATE_TRANSFER_COUNT, msg);
    if (exp->slaved_to == NULL) {
      snprintf(msg, sizeof(msg), "%s.%s: not slaved", name, exp->name);
      TEST_ASSERT(got->slaved_master == -1 && got->slaved_to == NULL, msg);
    } else {
      snprintf(msg, sizeof(msg), "%s.%s: declared master", name, exp->name);
      TEST_ASSERT(got->slaved_to != NULL, msg);
      TEST_ASSERT_STRING_EQUAL(got->slaved_to, exp->slaved_to, msg);
    }
  }
  return TEST_PASS;
}

/** @brief Set the run-file model parameters a fixture's init() reads */
static void set_params(const char *const *names, const double *values, int count) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  for (int i = 0; i < count; i++) {
    snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", names[i]);
    snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%.17g", values[i]);
  }
  MimicConfig.NumModelParams = count;
}

/** @brief Build one hand-made row: a halo of the given Type and Mvir with a zeroed galaxy */
static void make_row(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir) {
  memset(halo, 0, sizeof(*halo));
  memset(galaxy, 0, sizeof(*galaxy));
  halo->Type = type;
  halo->Mvir = mvir;
  halo->galaxy = galaxy;
}

/** @brief A context with a NULL central, as every non-declaring module is given */
static struct RateContext make_context(void) {
  struct RateContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.snapshot_number = 40;
  ctx.redshift = 0.5;
  ctx.time = 1.25;
  ctx.central = NULL;
  ctx.params = &MimicConfig;
  return ctx;
}

/** @brief Check one coefficient against its hand-computed value to LAW_RTOL */
static int check_coefficient(double got, double want, const char *what) {
  TEST_ASSERT(isfinite(got) && got >= 0.0, what);
  TEST_ASSERT_DOUBLE_EQUAL(got, want, LAW_RTOL * fabs(want), what);
  return TEST_PASS;
}

/* ==========================================================================
 * Table tests
 * ========================================================================== */

/**
 * @test    test_supply_table
 * @brief   test_rate_supply: two sources (one into a tracer) and an internal transfer
 */
int test_supply_table(void) {
  return check_module_table(&test_rate_supply_module, "test_rate_supply", SUPPLY, COUNT_OF(SUPPLY),
                            TEST_RATE_SUPPLY_NUM_TRANSFERS, 0);
}

/**
 * @test    test_formation_table
 * @brief   test_rate_formation: one internal transfer and its observer
 */
int test_formation_table(void) {
  const struct Module *module = &test_rate_formation_module;
  if (check_module_table(module, "test_rate_formation", FORMATION, COUNT_OF(FORMATION),
                         TEST_RATE_FORMATION_NUM_TRANSFERS, 1) != TEST_PASS) {
    return TEST_FAIL;
  }
  TEST_ASSERT_EQUAL(module->rate_observers[0].property, RESERVOIR_INDEX_RateFixtureFormed,
                    "the observer accumulates into RateFixtureFormed");
  TEST_ASSERT_EQUAL(module->rate_observers[0].transfer, TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION,
                    "the observer observes star_formation");
  return TEST_PASS;
}

/**
 * @test    test_feedback_table
 * @brief   test_rate_feedback: an internal transfer and a slaved source into a tracer
 */
int test_feedback_table(void) {
  return check_module_table(&test_rate_feedback_module, "test_rate_feedback", FEEDBACK,
                            COUNT_OF(FEEDBACK), TEST_RATE_FEEDBACK_NUM_TRANSFERS, 0);
}

/**
 * @test    test_stripping_table
 * @brief   test_rate_stripping: a transfer onto the central's record and a sink
 */
int test_stripping_table(void) {
  return check_module_table(&test_rate_stripping_module, "test_rate_stripping", STRIPPING,
                            COUNT_OF(STRIPPING), TEST_RATE_STRIPPING_NUM_TRANSFERS, 0);
}

/**
 * @test    test_slaved_master_resolved_across_modules
 * @brief   The yield's master is the global index of another module's star_formation
 */
int test_slaved_master_resolved_across_modules(void) {
  const struct RateTransfer *yield =
      &test_rate_feedback_module.rate_transfers[TEST_RATE_FEEDBACK_TRANSFER_YIELD];
  const struct RateTransfer *star_formation =
      &test_rate_formation_module.rate_transfers[TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION];

  TEST_ASSERT_EQUAL(yield->slaved_master, star_formation->global_index,
                    "yield's slaved master is star_formation's global index");
  TEST_ASSERT(yield->slaved_master != yield->global_index, "a source is not its own master");
  return TEST_PASS;
}

/**
 * @test    test_global_order_by_module_then_declaration
 * @brief   Global indices follow (module name, declaration order), as relative order only
 *
 * A module's transfers are consecutive and ascending in declaration order, and
 * the four modules' blocks follow their names' order: feedback, formation,
 * stripping, supply. No absolute value is asserted, so a later rate module,
 * wherever its name sorts, leaves this test valid.
 */
int test_global_order_by_module_then_declaration(void) {
  const struct Module *by_name[] = {&test_rate_feedback_module, &test_rate_formation_module,
                                    &test_rate_stripping_module, &test_rate_supply_module};
  char msg[200];
  int previous_last = -1;
  for (int m = 0; m < COUNT_OF(by_name); m++) {
    const struct Module *module = by_name[m];
    const struct RateTransfer *transfers = module->rate_transfers;
    for (int i = 1; i < module->num_rate_transfers; i++) {
      snprintf(msg, sizeof(msg), "%s: transfer %d follows transfer %d directly", module->name, i,
               i - 1);
      TEST_ASSERT_EQUAL(transfers[i].global_index, transfers[i - 1].global_index + 1, msg);
    }
    snprintf(msg, sizeof(msg), "%s's transfers come after every earlier-named module's",
             module->name);
    TEST_ASSERT(transfers[0].global_index > previous_last, msg);
    previous_last = transfers[module->num_rate_transfers - 1].global_index;
  }
  TEST_ASSERT(MIMIC_RATE_TRANSFER_COUNT >= 8, "the global index holds at least the eight fixture "
                                              "transfers");
  return TEST_PASS;
}

/* ==========================================================================
 * Law tests: each fixture's law on hand-built rows
 * ========================================================================== */

/**
 * @test    test_supply_law
 * @brief   Infall on central rows only, tracer infall a fixed fraction of it, constant cooling
 *
 * Parameters: infall rate 0.5, tracer fraction 0.25, cooling 0.3.
 */
int test_supply_law(void) {
  static const char *const names[] = {"TestRateSupplyInfallRate", "TestRateSupplyTracerFraction",
                                      "TestRateSupplyCoolingRate"};
  static const double values[] = {0.5, 0.25, 0.3};
  set_params(names, values, COUNT_OF(names));
  TEST_ASSERT_EQUAL(test_rate_supply_init(), 0, "init reads the three parameters");

  /* {Type, Mvir} -> {infall, infall_tracer, cooling} */
  static const struct {
    int type;
    double mvir;
    double want[3];
  } rows[] = {
      {0, 12.0, {6.0, 1.5, 0.3}},  /* central: 0.5 * 12 and 0.25 of that */
      {0, 3.0, {1.5, 0.375, 0.3}}, /* central: 0.5 * 3 and 0.25 of that */
      {1, 12.0, {0.0, 0.0, 0.3}},  /* satellite: no infall */
      {2, 12.0, {0.0, 0.0, 0.3}},  /* orphan: no infall */
      {0, 0.0, {0.0, 0.0, 0.3}},   /* central without a virial mass */
  };
  struct RateContext ctx = make_context();
  char msg[200];
  for (int r = 0; r < COUNT_OF(rows); r++) {
    struct Halo halo;
    struct GalaxyData galaxy;
    make_row(&halo, &galaxy, rows[r].type, rows[r].mvir);
    double coefficients[TEST_RATE_SUPPLY_NUM_TRANSFERS] = {0.0};
    snprintf(msg, sizeof(msg), "row %d: the law succeeds", r);
    TEST_ASSERT_EQUAL(test_rate_supply_rates(&ctx, &halo, coefficients), 0, msg);
    for (int k = 0; k < TEST_RATE_SUPPLY_NUM_TRANSFERS; k++) {
      snprintf(msg, sizeof(msg), "row %d (Type %d, Mvir %g): %s", r, rows[r].type, rows[r].mvir,
               SUPPLY[k].name);
      if (check_coefficient(coefficients[k], rows[r].want[k], msg) != TEST_PASS) {
        return TEST_FAIL;
      }
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_formation_law
 * @brief   Zero at and below the threshold, ((Cold - threshold) / Cold) * efficiency above it
 *
 * Parameters: efficiency 2, threshold 1; then efficiency 1e308, threshold 2 and
 * Cold 4, where the bounded ratio formed first keeps the coefficient finite:
 * forming efficiency * (Cold - threshold) first gives 1e308 * 2 = inf.
 */
int test_formation_law(void) {
  static const char *const names[] = {"TestRateFormationEfficiency", "TestRateFormationThreshold"};
  static const double values[] = {2.0, 1.0};
  set_params(names, values, COUNT_OF(names));
  TEST_ASSERT_EQUAL(test_rate_formation_init(), 0, "init reads the two parameters");

  /* Cold -> star_formation */
  static const struct {
    double cold;
    double want;
  } rows[] = {
      {0.0, 0.0},             /* empty: below the threshold */
      {0.5, 0.0},             /* below the threshold */
      {1.0, 0.0},             /* at the threshold */
      {3.0, 4.0 / 3.0},       /* 2 * (3 - 1) / 3 */
      {5.0, 1.6},             /* 2 * (5 - 1) / 5 */
      {101.0, 200.0 / 101.0}, /* 2 * (101 - 1) / 101 */
  };
  struct RateContext ctx = make_context();
  char msg[200];
  for (int r = 0; r < COUNT_OF(rows); r++) {
    struct Halo halo;
    struct GalaxyData galaxy;
    make_row(&halo, &galaxy, r % 3, 10.0);
    galaxy.RateFixtureCold = rows[r].cold;
    double coefficients[TEST_RATE_FORMATION_NUM_TRANSFERS] = {0.0};
    snprintf(msg, sizeof(msg), "row %d: the law succeeds", r);
    TEST_ASSERT_EQUAL(test_rate_formation_rates(&ctx, &halo, coefficients), 0, msg);
    snprintf(msg, sizeof(msg), "row %d (Cold %g): star_formation", r, rows[r].cold);
    if (check_coefficient(coefficients[TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION], rows[r].want,
                          msg) != TEST_PASS) {
      return TEST_FAIL;
    }
  }

  /* A huge efficiency: Cold 4 above threshold 2 gives the ratio 2/4 = 1/2 exactly, so the
   * coefficient is 1e308 / 2 = 5e307 (halving is exact in binary). Multiplying first
   * would overflow: 1e308 * (4 - 2) = inf. */
  static const double huge_values[] = {1e308, 2.0};
  set_params(names, huge_values, COUNT_OF(names));
  TEST_ASSERT_EQUAL(test_rate_formation_init(), 0, "init accepts a finite efficiency of 1e308");
  struct Halo halo;
  struct GalaxyData galaxy;
  make_row(&halo, &galaxy, 0, 10.0);
  galaxy.RateFixtureCold = 4.0;
  double coefficients[TEST_RATE_FORMATION_NUM_TRANSFERS] = {0.0};
  TEST_ASSERT_EQUAL(test_rate_formation_rates(&ctx, &halo, coefficients), 0,
                    "the law succeeds with a huge efficiency");
  TEST_ASSERT(isfinite(coefficients[TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION]),
              "Cold 4, threshold 2, efficiency 1e308: star_formation is finite");
  TEST_ASSERT(coefficients[TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION] == 5e307,
              "Cold 4, threshold 2, efficiency 1e308: star_formation is exactly 5e307");
  return TEST_PASS;
}

/**
 * @test    test_feedback_law
 * @brief   Constant reheating and a constant dimensionless yield, whatever the row
 *
 * Parameters: reheating 0.4, yield 0.02.
 */
int test_feedback_law(void) {
  static const char *const names[] = {"TestRateFeedbackReheatRate", "TestRateFeedbackYield"};
  static const double values[] = {0.4, 0.02};
  set_params(names, values, COUNT_OF(names));
  TEST_ASSERT_EQUAL(test_rate_feedback_init(), 0, "init reads the two parameters");

  /* {Type, Cold} -> {reheating, yield} */
  static const struct {
    int type;
    double cold;
    double want[2];
  } rows[] = {
      {0, 0.0, {0.4, 0.02}},
      {1, 2.5, {0.4, 0.02}},
      {2, 7.0, {0.4, 0.02}},
  };
  struct RateContext ctx = make_context();
  char msg[200];
  for (int r = 0; r < COUNT_OF(rows); r++) {
    struct Halo halo;
    struct GalaxyData galaxy;
    make_row(&halo, &galaxy, rows[r].type, 10.0);
    galaxy.RateFixtureCold = rows[r].cold;
    double coefficients[TEST_RATE_FEEDBACK_NUM_TRANSFERS] = {0.0};
    snprintf(msg, sizeof(msg), "row %d: the law succeeds", r);
    TEST_ASSERT_EQUAL(test_rate_feedback_rates(&ctx, &halo, coefficients), 0, msg);
    for (int k = 0; k < TEST_RATE_FEEDBACK_NUM_TRANSFERS; k++) {
      snprintf(msg, sizeof(msg), "row %d (Type %d): %s", r, rows[r].type, FEEDBACK[k].name);
      if (check_coefficient(coefficients[k], rows[r].want[k], msg) != TEST_PASS) {
        return TEST_FAIL;
      }
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_stripping_law
 * @brief   Stripping zero on a Type 0 row and constant elsewhere; constant ejection
 *
 * Parameters: stripping 0.7, ejection 0.15.
 */
int test_stripping_law(void) {
  static const char *const names[] = {"TestRateStrippingRate", "TestRateStrippingEjectionRate"};
  static const double values[] = {0.7, 0.15};
  set_params(names, values, COUNT_OF(names));
  TEST_ASSERT_EQUAL(test_rate_stripping_init(), 0, "init reads the two parameters");

  /* Type -> {stripping, ejection} */
  static const struct {
    int type;
    double want[2];
  } rows[] = {
      {0, {0.0, 0.15}}, /* central: nothing to strip onto itself */
      {1, {0.7, 0.15}}, /* satellite */
      {2, {0.7, 0.15}}, /* orphan */
  };
  struct RateContext ctx = make_context();
  char msg[200];
  for (int r = 0; r < COUNT_OF(rows); r++) {
    struct Halo halo;
    struct GalaxyData galaxy;
    make_row(&halo, &galaxy, rows[r].type, 10.0);
    galaxy.RateFixtureHot = 3.0;
    galaxy.RateFixtureCold = 2.0;
    double coefficients[TEST_RATE_STRIPPING_NUM_TRANSFERS] = {0.0};
    snprintf(msg, sizeof(msg), "row %d: the law succeeds", r);
    TEST_ASSERT_EQUAL(test_rate_stripping_rates(&ctx, &halo, coefficients), 0, msg);
    for (int k = 0; k < TEST_RATE_STRIPPING_NUM_TRANSFERS; k++) {
      snprintf(msg, sizeof(msg), "row %d (Type %d): %s", r, rows[r].type, STRIPPING[k].name);
      if (check_coefficient(coefficients[k], rows[r].want[k], msg) != TEST_PASS) {
        return TEST_FAIL;
      }
    }
  }
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Rate Transfer Tables and Fixture Laws\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_supply_table);
  TEST_RUN(test_formation_table);
  TEST_RUN(test_feedback_table);
  TEST_RUN(test_stripping_table);
  TEST_RUN(test_slaved_master_resolved_across_modules);
  TEST_RUN(test_global_order_by_module_then_declaration);
  TEST_RUN(test_supply_law);
  TEST_RUN(test_formation_law);
  TEST_RUN(test_feedback_law);
  TEST_RUN(test_stripping_law);

  TEST_SUMMARY();
  return TEST_RESULT();
}
