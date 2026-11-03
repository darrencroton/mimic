/**
 * @file    test_unit_sage26_finalize_cooling_heating_oracle.c
 * @brief   Bit-exact comparison of sage26_finalize_cooling_heating with SAGE26's normalisation
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_finalize_cooling_heating (its _tests/oracle/ directory): the end-of-halo
 * normalisation of R/src/core_build_model.c:590-603, inv_deltaT = 1.0/deltaT with
 * deltaT from galaxy zero, applied to Cooling and Heating of every galaxy with
 * mergeType 0. evolve_galaxies is static and not in the oracle's link, so the
 * oracle records a transcription of those lines (oracle README).
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
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"

extern int sage26_finalize_cooling_heating_init(void);
extern int sage26_finalize_cooling_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_finalize_cooling_heating_cleanup(void);

static const char *MODULE = "sage26_finalize_cooling_heating";

#define MAX_GALAXIES 8

static const char *const STATE[] = {"Cooling", "Heating",     "tcool",
                                    "tff",     "RcoolToRvir", "CoolingRate"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "normalise_cooling_heating", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  reset_config();
  if (sage26_finalize_cooling_heating_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return 1;
  }

  const int ngal = oracle_in_i32(&in, "call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fprintf(stderr, "oracle: %s: call.ngal out of range\n", name);
    return problems + 1;
  }
  const double halo_age = oracle_in_f64(&in, "call.halo_age");

  struct GalaxyData galaxies[MAX_GALAXIES];
  struct Halo halos[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    oracle_poison(&galaxies[i], sizeof(galaxies[i]));
    oracle_poison(&halos[i], sizeof(halos[i]));
    halos[i].galaxy = &galaxies[i];
    halos[i].HaloNr = 0;
    halos[i].Type = oracle_in_i32(&in, oracle_galaxy_field(i, "Type"));
    halos[i].SnapNum = oracle_in_i32(&in, oracle_galaxy_field(i, "SnapNum"));

    /* SAGE26's mergeType > 0 is the package's Type 3 (oracle README) */
    const int merge_type = oracle_in_i32(&in, oracle_galaxy_field(i, "mergeType"));
    if ((merge_type > 0) != (halos[i].Type == 3)) {
      fprintf(stderr, "oracle: %s: galaxy[%d] mergeType %d disagrees with Type %d\n", name, i,
              merge_type, halos[i].Type);
      problems++;
    }

    /* The row's dT: SAGE26's deltaT expression for this galaxy (core-owned) */
    char age_field[ORACLE_MAX_NAME];
    snprintf(age_field, sizeof(age_field), "call.Age[%d]", halos[i].SnapNum);
    halos[i].dT = oracle_in_f64(&in, age_field) - halo_age;
    char delta_field[ORACLE_MAX_NAME];
    snprintf(delta_field, sizeof(delta_field), "intermediate.galaxy[%d].deltaT", i);
    problems += oracle_check_f64(&out, delta_field, halos[i].dT);

    const char *const floats[] = {"tcool", "tff", "RcoolToRvir", "CoolingRate"};
    oracle_in_galaxy_floats(&in, i, &galaxies[i], floats, COUNT(floats));
    galaxies[i].Cooling = oracle_in_f64(&in, oracle_galaxy_field(i, "Cooling"));
    galaxies[i].Heating = oracle_in_f64(&in, oracle_galaxy_field(i, "Heating"));
  }

  /* SAGE26's deltaT is galaxy zero's, the FoF's first row (core_build_model.c:590) */
  problems += oracle_check_f64(&out, "intermediate.deltaT", halos[0].dT);
  oracle_exclude(&out, "intermediate.inv_deltaT"); /* SAGE26 local; asserted through Cooling */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];

  if (sage26_finalize_cooling_heating_process(&ctx, halos, ngal) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_finalize_cooling_heating_cleanup();

  for (int i = 0; i < ngal; i++) {
    char normalised_field[ORACLE_MAX_NAME];
    snprintf(normalised_field, sizeof(normalised_field), "intermediate.galaxy[%d].normalised", i);
    problems += oracle_check_i32(&out, normalised_field, halos[i].Type != 3);
    problems += oracle_c04_check_fields(&out, "", i, &galaxies[i], STATE, COUNT(STATE));
  }

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
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
  TEST_ASSERT(oracle_all_bit_identical(MODULE, check_fixture),
              "every fixture must reproduce the oracle bit for bit");
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
