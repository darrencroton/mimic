/**
 * @file    test_unit_sage26_calculate_cooling_budget_oracle.c
 * @brief   Bit-exact comparison of SAGE26's hot cooling chain and cooling-table interpolation
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_calculate_cooling_budget (its _tests/oracle/ directory) and asserts
 * bit-for-bit equality with the oracle output. The cooling tables are loaded once,
 * by the module's init(), for the whole run. Three kinds of fixture:
 *
 * - get_metaldependent_cooling_rate (interp_*): the inputs are (logTemp, logZ),
 *   not a galaxy, so the package's get_metaldependent_cooling_rate
 *   (cooling_tables.c) is called directly and its lambda asserted.
 *
 * - the c11 fixtures (cm_*, hs_*, cgm_*; oracle README "The c11 Group"):
 *   interpolate_clustering_mass, cooling_recipe_hot_streams (cooling_recipe_hot with
 *   CGMrecipeOn 1) and cooling_recipe_cgm, each run through the package's shared helpers
 *   (shared/sage_cooling_physics.h) directly, since no production module calls them yet
 *   (modules/_tests/sage26_oracle_cooling.h).
 *
 * - cooling_recipe_hot (hot_*), a chained test: the package splits SAGE26's
 *   cooling_recipe_hot (model_cooling_heating.c:135-314) and the
 *   cool_gas_onto_galaxy that follows it (core_build_model.c:508-509) across
 *   sage26_calculate_cooling_budget -> sage26_radio_mode_heating ->
 *   sage26_apply_cooling, so their process() entries run in that order on each
 *   fixture. After the budget step, before the radio module overwrites them, the
 *   carried post-cap CoolingGas and the return path (the early-return marker) are
 *   asserted, and x and Rcool where the fixture records them validated two-sided.
 *   After the radio step the suppressed mass is asserted, with the four reservoirs
 *   cool_gas_onto_galaxy changes as cooling_recipe_hot alone leaves them. After the
 *   apply step every other field of that state, and the whole sequence state.
 *
 * The snapshot interval is core-owned: the oracle's intermediate.deltaT
 * (Age[SnapNum] - halo_age) is supplied as the halo's dT and call.effective_steps
 * as the substep count; the modules' substep dt (shared/time_parity.h) is asserted
 * against intermediate.dt. Outside the oracle environment (macOS arm64, Apple
 * clang 21) the bit-exact test reports SKIP.
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
#include "cooling_tables.h"

extern int sage26_calculate_cooling_budget_init(void);
extern int sage26_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_cooling_budget_cleanup(void);
extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_radio_mode_heating_cleanup(void);
extern int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

static const char *MODULE = "sage26_calculate_cooling_budget";

/* The reservoirs cool_gas_onto_galaxy changes, and the rest of the c04 state */
static const char *const TRANSFERRED[] = {"HotGas", "MetalsHotGas", "ColdGas", "MetalsColdGas"};
static const char *const NOT_TRANSFERRED[] = {
    "BlackHoleMass", "r_heat", "Cooling", "Heating", "tcool", "tff", "RcoolToRvir", "CoolingRate"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief One interpolation fixture: the package's lambda at (logTemp, logZ) */
static int check_interpolation(const char *name, struct oracle_fixture *in,
                               struct oracle_fixture *out) {
  const double logTemp = oracle_in_f64(in, "call.logTemp");
  const double logZ = oracle_in_f64(in, "call.logZ");
  int problems =
      oracle_check_f64(out, "return.lambda", get_metaldependent_cooling_rate(logTemp, logZ));
  (void)name;
  return problems;
}

/**
 * @brief Assert a carried local (x or rcool) only where the oracle validated it two-sided
 *
 * The oracle records <local>_validated = 1 only when both neighbouring doubles
 * change an output of SAGE26's do_AGN_heating (oracle README); elsewhere the value
 * is not constrained by SAGE26's own code and is not asserted.
 */
static int check_carried_local(struct oracle_fixture *out, const char *local, const double actual) {
  char field[ORACLE_MAX_NAME];
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_absent", local);
  const int absent = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", local);
  (void)oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", local);
  (void)oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", local);
  const int validated = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s", local);
  if (absent) {
    return problems;
  }
  if (validated == 1) {
    problems += oracle_check_f64(out, field, actual);
  } else {
    oracle_exclude(out, field); /* not validated two-sided on this fixture: not asserted */
  }
  return problems;
}

/** @brief One cooling_recipe_hot fixture, run as the budget -> radio -> apply chain */
static int check_sequence(const char *name, struct oracle_fixture *in, struct oracle_fixture *out) {
  int problems = 0;

  reset_config();
  problems += oracle_c04_set_params(in, name);
  if (sage26_radio_mode_heating_init() != 0) {
    fprintf(stderr, "oracle: %s: radio module init failed\n", name);
    return problems + 1;
  }

  /* Age entries are core-owned; they reach the modules only as the halo's dT */
  (void)oracle_in_f64(in, "call.Age_galaxy_snap");
  (void)oracle_in_f64(in, "call.halo_age");

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c04_in_galaxy(in, 0, &halo, &galaxy);
  halo.HaloNr = 0;
  halo.SnapNum = 0;
  halo.dT = oracle_in_f64(out, "intermediate.deltaT");

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = oracle_in_i32(in, "call.effective_steps");
  ctx.central_galaxy = &halo;

  /* The modules' substep dt is SAGE26's deltaT / effective_steps */
  double dt = 0.0;
  if (mimic_object_substep_dt(&halo, &ctx, &dt) != MIMIC_OBJECT_TIME_OK) {
    fprintf(stderr, "oracle: %s: substep dt is not available\n", name);
    problems++;
  }
  problems += oracle_check_f64(out, "intermediate.dt", dt);

  /* ---- Step 1: the cooling budget ---- */
  if (sage26_calculate_cooling_budget_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: budget process() failed\n", name);
    problems++;
  }

  /* The early-return marker is the oracle's return path */
  problems += oracle_check_i32(out, "intermediate.return_path", galaxy.CoolingReturnPath);

  /* The carried post-cap mass; where SAGE26 never forms it the package carries 0 */
  if (oracle_in_i32(out, "intermediate.CoolingGas_postcap_absent")) {
    if (oracle_f64_to_bits(galaxy.CoolingGas) != oracle_f64_to_bits(0.0)) {
      fprintf(stderr, "oracle: %s: CoolingGas is %.17g where SAGE26 forms no post-cap mass\n", name,
              galaxy.CoolingGas);
      problems++;
    }
  } else {
    problems += oracle_check_f64(out, "intermediate.CoolingGas_postcap", galaxy.CoolingGas);
  }

  problems += check_carried_local(out, "x", galaxy.CoolingX);
  problems += check_carried_local(out, "rcool", galaxy.Rcool);

  /* SAGE26 locals the package does not carry; lambda is asserted by the
   * interpolation fixtures, and logTemp and logZ are its arguments. */
  const char *const uncarried[] = {"logTemp", "logZ", "lambda"};
  for (size_t k = 0; k < COUNT(uncarried); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s_absent", uncarried[k]);
    if (!oracle_in_i32(out, field)) {
      snprintf(field, sizeof(field), "intermediate.%s", uncarried[k]);
      oracle_exclude(out, field);
    }
  }

  /* SAGE26 calls do_AGN_heating with AGNrecipeOn > 0 and a positive post-cap mass
   * (model_cooling_heating.c:297); the radio module's gate is the same */
  const int entered = (galaxy.CoolingGas > 0.0) ? 1 : 0;
  problems += oracle_check_i32(out, "intermediate.do_AGN_heating_entered", entered);

  /* ---- Step 2: radio-mode heating ---- */
  if (sage26_radio_mode_heating_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: radio process() failed\n", name);
    problems++;
  }

  /* The mass cooling_recipe_hot returns, which cool_gas_onto_galaxy receives */
  problems += oracle_check_f64(out, "return.coolingGas", galaxy.CoolingGas);
  problems += oracle_check_f64(out, "intermediate.CoolingGas_suppressed", galaxy.CoolingGas);

  /* The reservoirs as cooling_recipe_hot alone leaves them (the tail, still to
   * run in the apply module, does not touch them) */
  problems += oracle_c04_check_fields(out, "", 0, &galaxy, TRANSFERRED, COUNT(TRANSFERRED));

  /* ---- Step 3: the tail and cool_gas_onto_galaxy ---- */
  if (sage26_apply_cooling_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: apply process() failed\n", name);
    problems++;
  }

  /* The rest of the state after cooling_recipe_hot alone: cool_gas_onto_galaxy
   * does not change these */
  problems += oracle_c04_check_fields(out, "", 0, &galaxy, NOT_TRANSFERRED, COUNT(NOT_TRANSFERRED));
  /* The state after the whole sequence (core_build_model.c:508-509) */
  problems += oracle_c04_check_galaxy(out, "intermediate.sequence.", 0, &galaxy);

  sage26_radio_mode_heating_cleanup();
  return problems;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;

  /* c11 fixtures (the clustering table, the CGMrecipeOn 1 hot recipe and CGM cooling) call the
   * shared helpers directly (modules/_tests/sage26_oracle_cooling.h) */
  int c11 = 0;
  const int c11_problems = oracle_c11_check_fixture(MODULE, name, &c11);
  if (c11) {
    return c11_problems;
  }

  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".out");
  if (oracle_load(path, &out) != 0) {
    return 1;
  }
  if (strcmp(in.routine, out.routine) != 0) {
    fprintf(stderr, "oracle: %s/%s: .in and .out name different routines\n", MODULE, name);
    return 1;
  }

  int problems = 0;
  if (strcmp(in.routine, "get_metaldependent_cooling_rate") == 0) {
    problems += check_interpolation(name, &in, &out);
  } else if (strcmp(in.routine, "cooling_recipe_hot") == 0) {
    problems += check_sequence(name, &in, &out);
  } else {
    fprintf(stderr, "oracle: %s/%s: unexpected routine %s\n", MODULE, name, in.routine);
    return 1;
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

  /* The cooling tables are loaded once for every fixture */
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
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
  printf("Test Suite: sage26_calculate_cooling_budget SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
