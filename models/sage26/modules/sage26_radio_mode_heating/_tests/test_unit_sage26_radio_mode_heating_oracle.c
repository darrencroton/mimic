/**
 * @file    test_unit_sage26_radio_mode_heating_oracle.c
 * @brief   Bit-exact comparison of sage26_radio_mode_heating with SAGE26's do_AGN_heating
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_radio_mode_heating (its
 * _tests/oracle/ directory): SAGE26's do_AGN_heating (model_cooling_heating.c:587-626, with the
 * Bondi branch of agn_accretion_compute, :526-575) called on its own with a positive cooling mass,
 * x and rcool. The fixture's call.coolingGas, call.x and call.rcool are the carried CoolingGas,
 * CoolingX and Rcool the cooling budget would hand the module; the module runs through its
 * process() entry and the test asserts, bit for bit, the returned (suppressed) mass, which the
 * module writes back into CoolingGas, and BlackHoleMass, HotGas, MetalsHotGas, r_heat (the
 * package's Rheat) and Heating. The other six fields of the cooling state, which do_AGN_heating
 * does not write, are asserted unchanged as the oracle records them.
 *
 * The CGM cooling fixtures (agn_cgmon_*, cgm_agn_*; oracle README "The c11 Group") are the CGM-on
 * radio AGN: SAGE26's do_AGN_heating with CGMrecipeOn 1, and the static do_AGN_heating_cgm reached
 * through cooling_recipe_cgm. They run through the package's shared helpers
 * (shared/sage_cooling_physics.h) directly, since no production module calls them yet
 * (modules/_tests/sage26_oracle_cooling.h); the cooling tables are loaded for them.
 *
 * The snapshot interval is core-owned: the oracle's intermediate.deltaT is
 * supplied as the halo's dT and call.effective_steps as the substep count; the
 * module's substep dt (shared/time_parity.h) is asserted against intermediate.dt.
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test
 * reports SKIP.
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
#include "modules/_tests/sage26_oracle_cooling.h"
#include "shared/sage_constants.h"
#include "shared/time_parity.h"

extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_radio_mode_heating_cleanup(void);
extern int sage26_calculate_cooling_budget_init(void);
extern int sage26_calculate_cooling_budget_cleanup(void);

static const char *MODULE = "sage26_radio_mode_heating";

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;

  /* CGM cooling fixtures (the CGM-on radio AGN: do_AGN_heating with CGMrecipeOn 1, and the static
   * do_AGN_heating_cgm through cooling_recipe_cgm) call the shared
   * helpers directly (modules/_tests/sage26_oracle_cooling.h) */
  int c11 = 0;
  const int c11_problems = oracle_c11_check_fixture(MODULE, name, &c11);
  if (c11) {
    return c11_problems;
  }

  if (oracle_load_pair(MODULE, name, "do_AGN_heating", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  reset_config();
  problems += oracle_c04_set_params(&in, name);
  if (sage26_radio_mode_heating_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return problems + 1;
  }

  /* Age entries are core-owned; they reach the module only as the halo's dT */
  (void)oracle_in_f64(&in, "call.Age_galaxy_snap");
  (void)oracle_in_f64(&in, "call.halo_age");

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c04_in_galaxy(&in, 0, &halo, &galaxy);
  halo.HaloNr = 0;
  halo.SnapNum = 0;
  halo.dT = oracle_in_f64(&out, "intermediate.deltaT");

  /* The cooling budget's carried state: the post-cap mass, x and rcool */
  galaxy.CoolingGas = oracle_in_f64(&in, "call.coolingGas");
  galaxy.CoolingX = oracle_in_f64(&in, "call.x");
  galaxy.Rcool = oracle_in_f64(&in, "call.rcool");
  galaxy.CoolingReturnPath = SAGE_COOLING_PATH_TAIL;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = oracle_in_i32(&in, "call.effective_steps");
  ctx.central_galaxy = &halo;

  /* The module's substep dt is SAGE26's deltaT / effective_steps */
  double dt = 0.0;
  if (mimic_object_substep_dt(&halo, &ctx, &dt) != MIMIC_OBJECT_TIME_OK) {
    fprintf(stderr, "oracle: %s: substep dt is not available\n", name);
    problems++;
  }
  problems += oracle_check_f64(&out, "intermediate.dt", dt);

  if (sage26_radio_mode_heating_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_radio_mode_heating_cleanup();

  /* The returned (suppressed) mass is written back into CoolingGas */
  problems += oracle_check_f64(&out, "return.coolingGas", galaxy.CoolingGas);
  problems += oracle_c04_check_galaxy(&out, "", 0, &galaxy);

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

  /* The cooling_recipe_cgm fixtures read the cooling tables: loaded once for every fixture */
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
  TEST_ASSERT(sage26_calculate_cooling_budget_init() == 0, "cooling tables must load");
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  sage26_calculate_cooling_budget_cleanup();
  oracle_c11_report(MODULE);

  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_radio_mode_heating SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
