/**
 * @file    test_unit_sage26_finalize_cooling_heating_oracle.c
 * @brief   Bit-exact comparison of sage26_finalize_cooling_heating with SAGE26's normalisation
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_finalize_cooling_heating (its _tests/oracle/ directory): the end-of-halo
 * normalisation of R/src/core_build_model.c:590-604, inv_deltaT = 1.0/deltaT with
 * deltaT from galaxy zero, applied to Cooling, Heating and OutflowRate of every
 * galaxy with mergeType 0. evolve_galaxies is static and not in the oracle's link,
 * so the oracle records a transcription of those lines (oracle README). The
 * fixtures name one of two routines, and the test dispatches on that line:
 *
 * - normalise_cooling_heating (the c04 fixtures): Cooling and Heating, and the four
 *   cooling diagnostics left as they were;
 * - writer_mdot_conversion (the c11 fixtures, campaign decision D39): SAGE26's writer
 *   conversion of its raw float mdot rates (R/src/io/save_gals_hdf5.c:996-997), transcribed in
 *   the oracle; the test runs process() on one live galaxy holding the raw rates in
 *   mdot_cool_raw and mdot_stream_raw, with a positive interval and again without one, and
 *   asserts the written mdot_cool and mdot_stream bit for bit on both paths. Where the oracle
 *   records that the conversion regrouped as one constant factor rounds differently, the test
 *   also asserts that the package does not compute the regrouped value;
 * - normalise_outflow_rate (the c05 fixtures): Cooling, Heating and the float
 *   OutflowRate (the package's SupernovaOutflowRate), in SAGE26's statement order.
 *   Their intermediate.writer.galaxy[i].OutflowRate is SAGE26's float output value
 *   (R/src/io/save_gals_hdf5.c:995, a left-to-right unit conversion). Mimic converts
 *   SupernovaOutflowRate in its generated output code with one combined factor, a
 *   core-owned difference; the test reports on how many galaxies the two agree and
 *   does not assert it.
 *
 * Each fixture galaxy becomes one workspace row in the fixture's order. Its dT is
 * core-owned: the test forms SAGE26's deltaT expression Age[SnapNum] - halo_age
 * from the fixture's Age entries, asserts it against the oracle's per-galaxy
 * intermediate.galaxy[i].deltaT, and supplies it as the row's dT. A merged galaxy
 * (mergeType > 0) is Type 3, the package's convention for a galaxy merged or
 * disrupted this snapshot. The test asserts Cooling and Heating of every galaxy bit
 * for bit, and that the four cooling diagnostics are left as they were. Outside the
 * oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "module_system/physical_constants.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"

extern int sage26_finalize_cooling_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);

static const char *MODULE = "sage26_finalize_cooling_heating";

#define MAX_GALAXIES 8

static const char *const STATE[] = {"Cooling", "Heating",     "tcool",
                                    "tff",     "RcoolToRvir", "CoolingRate"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* Writer-conversion agreement over the normalise_outflow_rate fixtures (reported only) */
static int writer_conversion_galaxies = 0;
static int writer_conversion_agree = 0;

/**
 * @brief Compare one fixture of either routine; returns the number of mismatches and errors
 *
 * @param outflow  1 for a normalise_outflow_rate fixture, 0 for normalise_cooling_heating
 */
static int check_normalisation(const char *name, struct oracle_fixture *in,
                               struct oracle_fixture *out, const int outflow) {
  int problems = 0;
  /* process() holds no init state; init() only checks the module's pipeline
   * placement, which a bare oracle test does not configure (the unit test covers it) */
  reset_config();

  const int ngal = oracle_in_i32(in, "call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fprintf(stderr, "oracle: %s: call.ngal out of range\n", name);
    return problems + 1;
  }
  const double halo_age = oracle_in_f64(in, "call.halo_age");
  double unit_mass_in_g = 0.0;
  double unit_time_in_s = 0.0;
  if (outflow) {
    /* The writer's units, read only by the reported output-conversion comparison */
    unit_mass_in_g = oracle_in_f64(in, "params.UnitMass_in_g");
    unit_time_in_s = oracle_in_f64(in, "params.UnitTime_in_s");
  }

  struct GalaxyData galaxies[MAX_GALAXIES];
  struct Halo halos[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    oracle_poison(&galaxies[i], sizeof(galaxies[i]));
    oracle_poison(&halos[i], sizeof(halos[i]));
    halos[i].galaxy = &galaxies[i];
    halos[i].HaloNr = 0;
    halos[i].Type = oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    halos[i].SnapNum = oracle_in_i32(in, oracle_galaxy_field(i, "SnapNum"));

    /* SAGE26's mergeType > 0 is the package's Type 3 (oracle README) */
    const int merge_type = oracle_in_i32(in, oracle_galaxy_field(i, "mergeType"));
    if ((merge_type > 0) != (halos[i].Type == 3)) {
      fprintf(stderr, "oracle: %s: galaxy[%d] mergeType %d disagrees with Type %d\n", name, i,
              merge_type, halos[i].Type);
      problems++;
    }

    /* The row's dT: SAGE26's deltaT expression for this galaxy (core-owned) */
    char age_field[ORACLE_MAX_NAME];
    snprintf(age_field, sizeof(age_field), "call.Age[%d]", halos[i].SnapNum);
    halos[i].dT = oracle_in_f64(in, age_field) - halo_age;
    char delta_field[ORACLE_MAX_NAME];
    snprintf(delta_field, sizeof(delta_field), "intermediate.galaxy[%d].deltaT", i);
    problems += oracle_check_f64(out, delta_field, halos[i].dT);

    if (outflow) {
      galaxies[i].SupernovaOutflowRate = oracle_in_f32(in, oracle_galaxy_field(i, "OutflowRate"));
    } else {
      const char *const floats[] = {"tcool", "tff", "RcoolToRvir", "CoolingRate"};
      oracle_in_galaxy_floats(in, i, &galaxies[i], floats, COUNT(floats));
    }
    galaxies[i].Cooling = oracle_in_f64(in, oracle_galaxy_field(i, "Cooling"));
    galaxies[i].Heating = oracle_in_f64(in, oracle_galaxy_field(i, "Heating"));
  }

  /* SAGE26's deltaT is galaxy zero's, the FoF's first row (core_build_model.c:590) */
  problems += oracle_check_f64(out, "intermediate.deltaT", halos[0].dT);
  oracle_exclude(out, "intermediate.inv_deltaT"); /* SAGE26 local; asserted through Cooling */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];

  if (sage26_finalize_cooling_heating_process(&ctx, halos, ngal) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  for (int i = 0; i < ngal; i++) {
    char normalised_field[ORACLE_MAX_NAME];
    snprintf(normalised_field, sizeof(normalised_field), "intermediate.galaxy[%d].normalised", i);
    problems += oracle_check_i32(out, normalised_field, halos[i].Type != 3);
    if (!outflow) {
      problems += oracle_c04_check_fields(out, "", i, &galaxies[i], STATE, COUNT(STATE));
      continue;
    }
    const char *const energies[] = {"Cooling", "Heating"};
    problems += oracle_c04_check_fields(out, "", i, &galaxies[i], energies, COUNT(energies));
    problems += oracle_check_f32(out, oracle_galaxy_field(i, "OutflowRate"),
                                 galaxies[i].SupernovaOutflowRate);

    /* Core-owned: the generated output conversion (one combined factor) against the
     * writer's left-to-right conversion (save_gals_hdf5.c:995); reported, not asserted */
    const char *writer_field =
        oracle_prefixed_galaxy_field("intermediate.writer.", i, "OutflowRate");
    if (oracle_has(out, writer_field)) {
      const struct oracle_field *writer = oracle_take(out, writer_field, ORACLE_F32);
      float generated = galaxies[i].SupernovaOutflowRate;
      generated *= unit_mass_in_g / unit_time_in_s * SEC_PER_YEAR / SOLAR_MASS;
      writer_conversion_galaxies++;
      if (writer != NULL && (uint32_t)writer->bits == oracle_f32_to_bits(generated)) {
        writer_conversion_agree++;
      }
    }
  }

  problems += oracle_report_unconsumed(in);
  problems += oracle_report_unconsumed(out);
  return problems;
}

/* writer_mdot_conversion fixtures checked */
static int writer_mdot_fixtures = 0;

/**
 * @brief One writer_mdot_conversion fixture: process() writes SAGE26's written float rates
 *
 * The raw rates are SAGE26's float members; the units are the writer's. The galaxy is finalised
 * once with a positive interval and once without one (the non-positive-interval branch), and
 * both must write the oracle's values.
 */
static int check_writer_mdot(const char *name, struct oracle_fixture *in,
                             struct oracle_fixture *out) {
  reset_config();
  MimicConfig.UnitMass_in_g = oracle_in_f64(in, "params.UnitMass_in_g");
  MimicConfig.UnitTime_in_s = oracle_in_f64(in, "params.UnitTime_in_s");
  const float raw_cool = oracle_in_f32(in, "galaxy[0].mdot_cool");
  const float raw_stream = oracle_in_f32(in, "galaxy[0].mdot_stream");
  int problems = 0;
  float written[2][2];
  for (int branch = 0; branch < 2; branch++) {
    struct GalaxyData galaxy;
    struct Halo halo;
    struct ModuleContext ctx;
    oracle_poison(&galaxy, sizeof(galaxy));
    oracle_poison(&halo, sizeof(halo));
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    halo.galaxy = &galaxy;
    halo.Type = 0;
    halo.dT = (branch == 0) ? 1.0 : -1.0; /* a positive interval, then none */
    galaxy.Cooling = 0.0;
    galaxy.Heating = 0.0;
    galaxy.SupernovaOutflowRate = 0.0f;
    galaxy.mdot_cool_raw = raw_cool;
    galaxy.mdot_stream_raw = raw_stream;
    if (sage26_finalize_cooling_heating_process(&ctx, &halo, 1) != 0) {
      fprintf(stderr, "oracle: %s: process() failed\n", name);
      return problems + 1;
    }
    written[branch][0] = galaxy.mdot_cool;
    written[branch][1] = galaxy.mdot_stream;
  }
  problems += oracle_check_f32(out, "output.mdot_cool", written[0][0]);
  problems += oracle_check_f32(out, "output.mdot_stream", written[0][1]);
  if (memcmp(written[0], written[1], sizeof(written[0])) != 0) {
    fprintf(stderr, "oracle: %s: the non-positive-interval branch writes different rates\n", name);
    problems++;
  }
  const char *const which[2] = {"mdot_cool", "mdot_stream"};
  for (int k = 0; k < 2; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.grouped_%s", which[k]);
    const float grouped = oracle_in_f32(out, field);
    snprintf(field, sizeof(field), "intermediate.grouped_differs_%s", which[k]);
    const int differs = oracle_in_i32(out, field);
    if (differs && memcmp(&grouped, &written[0][k], sizeof(grouped)) == 0) {
      fprintf(stderr, "oracle: %s: the package computes the regrouped %s conversion\n", name,
              which[k]);
      problems++;
    }
  }
  writer_mdot_fixtures++;
  problems += oracle_report_unconsumed(in);
  problems += oracle_report_unconsumed(out);
  return problems;
}

/** @brief Compare one fixture; dispatches on its routine line */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  if (strcmp(in.routine, "writer_mdot_conversion") == 0) {
    if (oracle_load_pair(MODULE, name, "writer_mdot_conversion", &in, &out) != 0) {
      return 1;
    }
    return check_writer_mdot(name, &in, &out);
  }
  const int outflow = strcmp(in.routine, "normalise_outflow_rate") == 0;
  if (oracle_load_pair(MODULE, name,
                       outflow ? "normalise_outflow_rate" : "normalise_cooling_heating", &in,
                       &out) != 0) {
    return 1;
  }
  return check_normalisation(name, &in, &out, outflow);
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
  writer_conversion_galaxies = 0;
  writer_conversion_agree = 0;
  writer_mdot_fixtures = 0;
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: c11 writer_mdot_conversion fixtures checked bit for bit on both branches: %d",
         writer_mdot_fixtures);
  printf("\n  oracle: core boundary (not asserted): the generated SupernovaOutflowRate output "
         "conversion equals SAGE26's writer value on %d of %d galaxies ",
         writer_conversion_agree, writer_conversion_galaxies);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_finalize_cooling_heating SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
