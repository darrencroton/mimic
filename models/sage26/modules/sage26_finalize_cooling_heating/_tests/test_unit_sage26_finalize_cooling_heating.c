/**
 * @file    test_unit_sage26_finalize_cooling_heating.c
 * @brief   Unit tests for sage26_finalize_cooling_heating
 *
 * Validates: the once-per-FoF normalisation of Cooling, Heating and SupernovaOutflowRate by the
 * first galaxy's interval (as SAGE26's model builder does), the package's boundary handling
 * (merged (Type 3) rows and rows without a galaxy are skipped, and a first galaxy without a
 * positive interval zeroes the three energies) and the written mdot rates.
 *
 * Test cases:
 *   - test_normalises_by_first_galaxy_interval: every live galaxy's Cooling, Heating and
 *     SupernovaOutflowRate use the first row's dT; the cooling diagnostics are not changed
 *   - test_merged_galaxies_untouched: Type 3 rows keep their energies
 *   - test_first_galaxy_merged_sets_interval: a merged first galaxy still sets deltaT
 *   - test_rows_without_galaxy_skipped: NULL-galaxy rows are skipped, also as the first row
 *   - test_no_positive_interval_zeroes: a first galaxy dT of -1, 0 or NaN zeroes live galaxies'
 *     Cooling, Heating and SupernovaOutflowRate
 *   - test_empty_input: NULL halos or ngal 0 is a no-op
 *   - test_mdot_writer_conversion: the output mdot rates are SAGE26's writer expression of the
 *     raw rates, left to right, for every live galaxy, with a positive first-row interval and
 *     with none (where a never-assigned, zero raw rate is written as 0)
 *
 * The module's placement (once, in post_timestep) is refused, with diagnostics, by
 * test_unit_sage_dependency_contracts.c.
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_finalize_cooling_heating_init(void);
extern int sage26_finalize_cooling_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_finalize_cooling_heating_cleanup(void);

#define NROWS 4

static struct GalaxyData galaxies[NROWS];
static struct Halo halos[NROWS];
static struct ModuleContext ctx;

/** @brief NROWS rows: Type 0 central then Type 1 satellites, each with its own dT and energies */
static void setup_rows(void) {
  memset(galaxies, 0, sizeof(galaxies));
  memset(halos, 0, sizeof(halos));
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 10;
  for (int i = 0; i < NROWS; i++) {
    halos[i].galaxy = &galaxies[i];
    halos[i].Type = (i == 0) ? 0 : 1;
    halos[i].SnapNum = 40 + i;
    halos[i].dT = 0.25 + 0.25 * i; /* differs per row: only row 0's may be used */
    galaxies[i].Cooling = 3.0 * (i + 1);
    galaxies[i].Heating = 1.5 * (i + 1);
    galaxies[i].SupernovaOutflowRate = 2.5f * (float)(i + 1);
  }
}

/**
 * @test    test_normalises_by_first_galaxy_interval
 * @brief   Cooling, Heating and SupernovaOutflowRate of every live galaxy are multiplied by
 *          1.0 / (first row's dT); the cooling diagnostics are not normalised
 */
int test_normalises_by_first_galaxy_interval(void) {
  setup_rows();
  for (int i = 0; i < NROWS; i++) {
    galaxies[i].tcool = -11.0f;
    galaxies[i].tff = -22.0f;
    galaxies[i].RcoolToRvir = -33.0f;
    galaxies[i].CoolingRate = -44.0f;
  }
  const double inv_deltaT = 1.0 / halos[0].dT;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  for (int i = 0; i < NROWS; i++) {
    double expected_cooling = 3.0 * (i + 1);
    double expected_heating = 1.5 * (i + 1);
    expected_cooling *= inv_deltaT;
    expected_heating *= inv_deltaT;
    /* The rate is a float: 2.5 * (i + 1) times 4 (row 0's dT is 0.25) is exact */
    const float expected_supernova = 10.0f * (float)(i + 1);
    TEST_ASSERT(galaxies[i].Cooling == expected_cooling,
                "Cooling must be the sum times 1.0/deltaT of the first galaxy, exactly");
    TEST_ASSERT(galaxies[i].Heating == expected_heating,
                "Heating must be the sum times 1.0/deltaT of the first galaxy, exactly");
    TEST_ASSERT(galaxies[i].SupernovaOutflowRate == expected_supernova,
                "SupernovaOutflowRate must be the sum times 1.0/deltaT of the first galaxy");
    TEST_ASSERT(galaxies[i].tcool == -11.0f && galaxies[i].tff == -22.0f &&
                    galaxies[i].RcoolToRvir == -33.0f && galaxies[i].CoolingRate == -44.0f,
                "diagnostics must be unchanged");
  }
  return TEST_PASS;
}

/**
 * @test    test_merged_galaxies_untouched
 * @brief   A row merged or disrupted this snapshot (Type 3) keeps its energies
 */
int test_merged_galaxies_untouched(void) {
  setup_rows();
  halos[2].Type = 3;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  TEST_ASSERT(galaxies[2].Cooling == 9.0, "merged galaxy's Cooling must be unchanged");
  TEST_ASSERT(galaxies[2].Heating == 4.5, "merged galaxy's Heating must be unchanged");
  TEST_ASSERT(galaxies[2].SupernovaOutflowRate == 7.5f,
              "merged galaxy's SupernovaOutflowRate must be unchanged");
  TEST_ASSERT(galaxies[1].Cooling == 6.0 * (1.0 / halos[0].dT), "live galaxy is normalised");
  return TEST_PASS;
}

/**
 * @test    test_first_galaxy_merged_sets_interval
 * @brief   SAGE26 takes deltaT from galaxy zero even when galaxy zero has merged
 */
int test_first_galaxy_merged_sets_interval(void) {
  setup_rows();
  halos[0].Type = 3;
  const double inv_deltaT = 1.0 / halos[0].dT;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  TEST_ASSERT(galaxies[0].Cooling == 3.0, "merged first galaxy keeps its Cooling");
  TEST_ASSERT(galaxies[1].Cooling == 6.0 * inv_deltaT,
              "a live galaxy is normalised by the merged first galaxy's interval");
  TEST_ASSERT(galaxies[3].Heating == 6.0 * inv_deltaT,
              "a live galaxy is normalised by the merged first galaxy's interval");
  return TEST_PASS;
}

/**
 * @test    test_rows_without_galaxy_skipped
 * @brief   Rows without a galaxy are skipped; the first row with a galaxy sets the interval
 */
int test_rows_without_galaxy_skipped(void) {
  setup_rows();
  halos[0].galaxy = NULL; /* the core can keep a row whose galaxy it discarded */
  halos[2].galaxy = NULL;
  const double inv_deltaT = 1.0 / halos[1].dT;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed with NULL-galaxy rows");
  TEST_ASSERT(galaxies[1].Cooling == 6.0 * inv_deltaT,
              "the first row with a galaxy sets the interval");
  TEST_ASSERT(galaxies[3].Cooling == 12.0 * inv_deltaT,
              "every live galaxy uses the first galaxy row's interval");
  TEST_ASSERT(galaxies[0].Cooling == 3.0 && galaxies[2].Cooling == 9.0,
              "galaxies detached from their rows are not touched");
  return TEST_PASS;
}

/**
 * @test    test_no_positive_interval_zeroes
 * @brief   Where the first galaxy has no positive interval (dT = -1 at birth, 0 or NaN), the live
 *          galaxies' Cooling, Heating and SupernovaOutflowRate are set to 0, not scaled by an
 *          infinite or NaN 1/dT; merged rows are left alone
 */
int test_no_positive_interval_zeroes(void) {
  const double first_dT[3] = {-1.0, 0.0, NAN};
  for (int r = 0; r < 3; r++) {
    setup_rows();
    halos[0].dT = first_dT[r];
    halos[3].Type = 3;

    TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
                "process should succeed");
    for (int i = 0; i < 3; i++) {
      TEST_ASSERT(galaxies[i].Cooling == 0.0 && galaxies[i].Heating == 0.0 &&
                      galaxies[i].SupernovaOutflowRate == 0.0f,
                  "live galaxies' energies are zeroed without a positive interval");
    }
    TEST_ASSERT(galaxies[3].Cooling == 12.0 && galaxies[3].Heating == 6.0 &&
                    galaxies[3].SupernovaOutflowRate == 10.0f,
                "merged galaxy is left alone");
  }
  return TEST_PASS;
}

/**
 * @test    test_empty_input
 * @brief   NULL halos or ngal 0 is a no-op that succeeds
 */
int test_empty_input(void) {
  setup_rows();
  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, NULL, 0) == 0, "NULL halos is a no-op");
  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, 0) == 0, "ngal 0 is a no-op");
  for (int i = 0; i < NROWS; i++) {
    TEST_ASSERT(galaxies[i].Cooling == 3.0 * (i + 1) && galaxies[i].Heating == 1.5 * (i + 1) &&
                    galaxies[i].SupernovaOutflowRate == 2.5f * (float)(i + 1),
                "ngal 0 must leave every row's energies unchanged");
  }
  return TEST_PASS;
}

// ============================================================================
// THE WRITTEN MDOT RATES
// ============================================================================

/** @brief A double the compiler cannot constant-fold (the expectation is evaluated at run time) */
static double opaque_double(const double value) {
  volatile double v = value;
  return v;
}

/*
 * A unit mass and two raw rates for which SAGE26's left-to-right conversion and the same
 * conversion regrouped as one constant factor round to different floats (found by an
 * exhaustive scratch search): UnitMass_in_g = 1.989e43 * (1 + 25e-6), UnitTime_in_s the
 * mini-Millennium 3.08568e24 / 1e5.
 */
#define T_UNIT_MASS 0x1.c8a9d5f481618p+143
#define T_UNIT_TIME 0x1.ac39601d1945fp+64
#define T_RAW_COOL_BITS 0x3fa9c5eau   /* 1.32635236 */
#define T_RAW_STREAM_BITS 0x3ff90024u /* 1.94531679 */

/** @brief SAGE26's writer expression, inline */
static float writer_value(const float raw) {
  return (float)(raw * opaque_double(T_UNIT_MASS) / opaque_double(T_UNIT_TIME) * 3.155e7 /
                 1.989e33);
}

/** @brief The same conversion regrouped as one constant factor (the form it must not take) */
static float grouped_value(const float raw) {
  return (float)(raw *
                 (opaque_double(T_UNIT_MASS) / opaque_double(T_UNIT_TIME) * 3.155e7 / 1.989e33));
}

/** @brief Rows with distinguishable raw rates and stale output values */
static void setup_mdot_rows(void) {
  setup_rows();
  MimicConfig.UnitMass_in_g = T_UNIT_MASS;
  MimicConfig.UnitTime_in_s = T_UNIT_TIME;
  for (int i = 0; i < NROWS; i++) {
    galaxies[i].mdot_cool = -77.0f;
    galaxies[i].mdot_stream = -88.0f;
  }
  galaxies[0].mdot_cool_raw = f32_from_bits(T_RAW_COOL_BITS);
  galaxies[0].mdot_stream_raw = f32_from_bits(T_RAW_STREAM_BITS);
  galaxies[1].mdot_cool_raw = 0.0f; /* never assigned: SAGE26's creation value */
  galaxies[1].mdot_stream_raw = 0.0f;
  galaxies[2].mdot_cool_raw = 3.0f;
  galaxies[2].mdot_stream_raw = 0.25f;
  galaxies[3].mdot_cool_raw = 7.0f;
  galaxies[3].mdot_stream_raw = 9.0f;
  halos[3].Type = 3; /* merged this snapshot: not finalised */
}

/**
 * @test    test_mdot_writer_conversion
 * @brief   mdot_cool and mdot_stream are SAGE26's writer conversion of the raw float rates, with
 *          a positive first-row interval and on the branch where there is none
 *
 * Expected: every live galaxy's outputs equal raw * UnitMass_in_g / UnitTime_in_s *
 * SEC_PER_YEAR / SOLAR_MASS evaluated left to right and narrowed (SAGE26's writer, with
 * 3.155e7 and 1.989e33); for row 0 that differs from the regrouped constant factor; a zero raw
 * rate is written 0 over the stale output; the merged row keeps its stale outputs. With the first
 * row's dT -1 the energies are zeroed and the conversion still runs.
 */
int test_mdot_writer_conversion(void) {
  TEST_ASSERT(writer_value(f32_from_bits(T_RAW_COOL_BITS)) !=
                      grouped_value(f32_from_bits(T_RAW_COOL_BITS)) &&
                  writer_value(f32_from_bits(T_RAW_STREAM_BITS)) !=
                      grouped_value(f32_from_bits(T_RAW_STREAM_BITS)),
              "precondition: the regrouped conversion rounds differently on row 0");

  static const struct {
    double first_dT; /* row 0's dT; the setup's own 0.25 is positive */
    double expected_cooling_row0;
  } rows[] = {
      {0.25, 12.0}, /* positive interval: the energies are normalised (3.0 / 0.25) */
      {-1.0, 0.0},  /* no positive interval: the energies are zeroed */
  };
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    char msg[160];
    setup_mdot_rows();
    halos[0].dT = rows[r].first_dT;
    snprintf(msg, sizeof(msg), "row %zu (first dT %g): process should succeed", r,
             rows[r].first_dT);
    TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0, msg);
    snprintf(msg, sizeof(msg), "row %zu (first dT %g): precondition: the expected branch ran", r,
             rows[r].first_dT);
    TEST_ASSERT(galaxies[0].Cooling == rows[r].expected_cooling_row0, msg);
    for (int i = 0; i < 3; i++) {
      snprintf(msg, sizeof(msg),
               "row %zu (first dT %g): galaxy %d's outputs are the writer conversion of its raw "
               "rates",
               r, rows[r].first_dT, i);
      TEST_ASSERT(galaxies[i].mdot_cool == writer_value(galaxies[i].mdot_cool_raw) &&
                      galaxies[i].mdot_stream == writer_value(galaxies[i].mdot_stream_raw),
                  msg);
    }
    snprintf(msg, sizeof(msg), "row %zu (first dT %g): a never-assigned rate is written 0", r,
             rows[r].first_dT);
    TEST_ASSERT(galaxies[1].mdot_cool == 0.0f && galaxies[1].mdot_stream == 0.0f, msg);
    snprintf(msg, sizeof(msg), "row %zu (first dT %g): a merged galaxy is not finalised", r,
             rows[r].first_dT);
    TEST_ASSERT(galaxies[3].mdot_cool == -77.0f && galaxies[3].mdot_stream == -88.0f, msg);
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_finalize_cooling_heating Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_normalises_by_first_galaxy_interval);
  TEST_RUN(test_merged_galaxies_untouched);
  TEST_RUN(test_first_galaxy_merged_sets_interval);
  TEST_RUN(test_rows_without_galaxy_skipped);
  TEST_RUN(test_no_positive_interval_zeroes);
  TEST_RUN(test_empty_input);
  TEST_RUN(test_mdot_writer_conversion);

  TEST_SUMMARY();
  return TEST_RESULT();
}
