/**
 * @file    test_unit_sage26_apply_cooling_oracle.c
 * @brief   Bit-exact comparison of sage26_apply_cooling with SAGE26's cool_gas_onto_galaxy
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_apply_cooling
 * (its _tests/oracle/ directory): SAGE26's cool_gas_onto_galaxy
 * (model_cooling_heating.c:689-705) called on its own with a cooling mass. The
 * fixture's call.coolingGas is the AGN-suppressed CoolingGas the module receives;
 * the module runs through its process() entry and the test asserts, bit for bit,
 * HotGas, MetalsHotGas, ColdGas and MetalsColdGas, and that BlackHoleMass, r_heat
 * (Rheat), Heating, tcool, tff and RcoolToRvir are left as the oracle records them.
 *
 * The module's process() also runs the tail of cooling_recipe_hot (:301-313),
 * which writes Cooling and CoolingRate and which a standalone cool_gas_onto_galaxy
 * call does not reach. Those two fields are therefore not asserted here; the
 * sequence fixtures of sage26_calculate_cooling_budget's oracle test assert them
 * through the whole budget -> radio -> apply chain. The tail needs a positive
 * substep interval and the unit factors, which these fixtures do not carry (the
 * routine reads none of them): the test supplies dT 1 with one substep and SAGE26's
 * R0 Hubble_h and UnitTime_in_s, and the normal-tail return path. Outside the
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
#include "shared/sage_constants.h"

extern int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

static const char *MODULE = "sage26_apply_cooling";

/* The c04 state fields cool_gas_onto_galaxy may change or must leave as they are */
static const char *const ASSERTED[] = {"HotGas",        "MetalsHotGas", "ColdGas", "MetalsColdGas",
                                       "BlackHoleMass", "r_heat",       "Heating", "tcool",
                                       "tff",           "RcoolToRvir"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* SAGE26's R0 Hubble_h (R/input/millennium.par:40) and UnitTime_in_s (L / V,
 * R/src/core_init.c:94), only so the tail the module also runs is finite */
#define R0_HUBBLE_H 0.73
#define R0_UNIT_TIME_IN_S 3.08568e19

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "cool_gas_onto_galaxy", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  reset_config();
  MimicConfig.Hubble_h = R0_HUBBLE_H;
  MimicConfig.UnitTime_in_s = R0_UNIT_TIME_IN_S;

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c04_in_galaxy(&in, 0, &halo, &galaxy);
  halo.HaloNr = 0;
  halo.SnapNum = 0;
  halo.dT = 1.0;

  /* The AGN-suppressed mass the module receives, on the normal tail */
  galaxy.CoolingGas = oracle_in_f64(&in, "call.coolingGas");
  galaxy.CoolingReturnPath = SAGE_COOLING_PATH_TAIL;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halo;

  if (sage26_apply_cooling_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }

  problems += oracle_c04_check_fields(&out, "", 0, &galaxy, ASSERTED, COUNT(ASSERTED));
  /* Written by the cooling_recipe_hot tail, not by cool_gas_onto_galaxy (see above) */
  oracle_exclude(&out, "galaxy[0].Cooling");
  oracle_exclude(&out, "galaxy[0].CoolingRate");

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
  printf("Test Suite: sage26_apply_cooling SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
