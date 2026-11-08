/**
 * @file    test_unit_sage26_reincorporation_oracle.c
 * @brief   Bit-exact comparison of sage26_reincorporation with SAGE26's reincorporate_gas
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_reincorporation (its
 * _tests/oracle/ directory) through the module's process() entry and asserts bit-for-bit equality
 * with the oracle output: the central's ejected and hot reservoirs after reincorporate_gas
 * (R/src/model_reincorporation.c:44-66), and for a CGMrecipeOn 1 fixture also its CGM pair. A
 * CGMrecipeOn 1 fixture sets the central's Regime and CGM pair and the module's CGMrecipeOn 1; the
 * test asserts the oracle's destination (intermediate.reservoir_cgm) against the input's
 * CGMrecipeOn 1 && Regime 0, the unselected pair unchanged, and each recorded local's markers, with
 * reincorporated equal to the ejected gas where the cap binds (oracle README, "The c10 Group").
 *
 * The snapshot interval is core-owned: the oracle's intermediate.deltaT
 * (Age[SnapNum] - halo_age, core_build_model.c:479) is supplied as the halo's dT
 * and call.effective_steps as the substep count. The module's own substep dt
 * (shared/time_parity.h) is asserted against intermediate.dt. Where the oracle
 * records that the SAGE26 call-site gate (ReIncorporationFactor > 0,
 * core_build_model.c:490) did not call reincorporate_gas
 * (intermediate.reincorporate_gas_called 0), the module must leave the reservoirs
 * bit-identical to their inputs. Outside the oracle environment (macOS arm64,
 * Apple clang 21) the bit-exact test reports SKIP.
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
#include "shared/time_parity.h"

extern int sage26_reincorporation_init(void);
extern int sage26_reincorporation_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_reincorporation_cleanup(void);

static const char *MODULE = "sage26_reincorporation";

static const char *const RESERVOIRS[] = {"EjectedMass", "MetalsEjectedMass", "HotGas",
                                         "MetalsHotGas"};

/* The tallies of the CGMrecipeOn 1 locals over one run of the bit-exact test */
static struct oracle_c10_tally c10_tally;

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "reincorporate_gas", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  const int cgm_recipe_on = oracle_in_i32(&in, "params.CGMrecipeOn");
  if (cgm_recipe_on != 0 && cgm_recipe_on != 1) {
    fprintf(stderr, "oracle: %s: CGMrecipeOn must be 0 (R0) or 1 (c10)\n", name);
    problems++;
  }

  const double factor = oracle_in_f64(&in, "params.ReIncorporationFactor");
  reset_config();
  if (oracle_set_model_param("ReIncorporationFactor", factor) != 0 ||
      oracle_set_model_param("CGMrecipeOn", cgm_recipe_on) != 0 ||
      sage26_reincorporation_init() != 0) {
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
  halo.galaxy = &galaxy;
  halo.Type = oracle_in_i32(&in, oracle_galaxy_field(0, "Type"));
  halo.HaloNr = 0;
  halo.SnapNum = 0;
  /* SAGE26's float Vvir and Rvir, exactly widened to the core's doubles */
  halo.Vvir = oracle_in_f32(&in, oracle_galaxy_field(0, "Vvir"));
  halo.Rvir = oracle_in_f32(&in, oracle_galaxy_field(0, "Rvir"));
  halo.dT = oracle_in_f64(&out, "intermediate.deltaT");
  oracle_in_galaxy_floats(&in, 0, &galaxy, RESERVOIRS, COUNT(RESERVOIRS));
  if (cgm_recipe_on == 1) {
    galaxy.Regime = oracle_in_i32(&in, oracle_galaxy_field(0, "Regime"));
    oracle_in_galaxy_floats(&in, 0, &galaxy, oracle_c10_pair(1), 2);
  }

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
  const struct GalaxyData before = galaxy;

  if (sage26_reincorporation_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_reincorporation_cleanup();

  /* When SAGE26's call-site gate (ReIncorporationFactor > 0,
   * core_build_model.c:490) does not call reincorporate_gas, the module must
   * visibly do nothing: the reservoirs stay bit-identical to their inputs. */
  if (oracle_in_i32(&out, "intermediate.reincorporate_gas_called") == 0) {
    problems +=
        oracle_check_galaxy_unchanged(&out, 0, &before, &galaxy, RESERVOIRS, COUNT(RESERVOIRS));
  }

  problems += oracle_check_galaxy_floats(&out, 0, &galaxy, RESERVOIRS, COUNT(RESERVOIRS));

  if (cgm_recipe_on == 1) {
    problems += oracle_check_galaxy_floats(&out, 0, &galaxy, oracle_c10_pair(1), 2);
    const int guards_passed = oracle_in_i32(&out, "intermediate.guards_passed");
    /* SAGE26's destination, independently of the package: CGMrecipeOn 1 && Regime 0 (X:78),
     * evaluated only when the guards pass (G:48) */
    const int to_cgm = (before.Regime == 0);
    problems +=
        oracle_check_i32(&out, "intermediate.reservoir_cgm", guards_passed == 1 ? to_cgm : -1);
    if (guards_passed == 1) {
      problems +=
          oracle_c10_check_unselected(out.path, 0, &before, &galaxy, oracle_c10_pair(!to_cgm), 2);
    } else {
      problems +=
          oracle_c10_check_unselected(out.path, 0, &before, &galaxy, RESERVOIRS, COUNT(RESERVOIRS));
      problems += oracle_c10_check_unselected(out.path, 0, &before, &galaxy, oracle_c10_pair(1), 2);
    }
    const int cap_binds = oracle_in_i32(&out, "intermediate.reincorporation_cap_binds") == 1;
    problems += oracle_c10_check_local(&out, "Vcrit", ORACLE_C10_TAIL, 0, 0, 0, 0.0f, &c10_tally);
    problems += oracle_c10_check_local(&out, "reincorporated_pre_cap", ORACLE_C10_TAIL, 0, 0, 0,
                                       0.0f, &c10_tally);
    problems += oracle_c10_check_local(&out, "reincorporated", ORACLE_C10_CONTINUATION, 0, 0,
                                       cap_binds, before.EjectedGas, &c10_tally);
    problems +=
        oracle_c10_check_local(&out, "metallicity", ORACLE_C10_TAIL, 0, 0, 0, 0.0f, &c10_tally);
    problems += oracle_c10_check_local(&out, "metals_reincorporated", ORACLE_C10_CONTINUATION, 0, 0,
                                       0, 0.0f, &c10_tally);
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
  memset(&c10_tally, 0, sizeof(c10_tally));
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  oracle_c10_print_tally(MODULE, &c10_tally);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_reincorporation SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
