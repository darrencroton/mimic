/**
 * @file    test_unit_sage26_apply_infall_oracle.c
 * @brief   Bit-exact comparison of sage26_apply_infall with SAGE26's add_infall_to_hot
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_apply_infall (its
 * _tests/oracle/ directory) through the module's process() entry and asserts bit-for-bit equality
 * with the oracle output: the central's ejected and hot reservoirs after add_infall_to_hot
 * (R/src/model_infall.c:351-414), and for a CGMrecipeOn 1 fixture also its CGM pair. A CGMrecipeOn
 * 1 fixture sets the central's Regime and CGM pair and the module's CGMrecipeOn 1; the test
 * asserts the oracle's reservoir selection (intermediate.reservoir_cgm) against the input's
 * CGMrecipeOn 1 && Regime 0, the unselected pair unchanged, and each recorded local's markers
 * (oracle README, "The c10 Group").
 *
 * The snapshot budget call.InfallingGas is supplied as the package's InfallingGas
 * and call.effective_steps as the substep count; the module forms the per-substep
 * share itself, as SAGE26's call site does (core_build_model.c:488). That share,
 * intermediate.infallingGas_per_step, is a local the package never stores, so it is
 * not asserted directly; every reservoir it changes is. Outside the oracle
 * environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
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

extern int sage26_apply_infall_init(void);
extern int sage26_apply_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_apply_infall_cleanup(void);

static const char *MODULE = "sage26_apply_infall";

static const char *const RESERVOIRS[] = {"EjectedMass", "MetalsEjectedMass", "HotGas",
                                         "MetalsHotGas"};

/* The tallies of the CGMrecipeOn 1 locals over one run of the bit-exact test */
static struct oracle_c10_tally c10_tally;

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "add_infall_to_hot", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  const int cgm_recipe_on = oracle_in_i32(&in, "params.CGMrecipeOn");
  if (cgm_recipe_on != 0 && cgm_recipe_on != 1) {
    fprintf(stderr, "oracle: %s: CGMrecipeOn must be 0 (R0) or 1 (c10)\n", name);
    problems++;
  }

  /* init() requires the budget module in pre_timestep */
  static struct PhaseModuleConfig pre_timestep[1];
  reset_config();
  pre_timestep[0].module_name = (char *)"sage26_prepare_infall_budget";
  pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep;
  MimicConfig.num_pre_timestep = 1;
  if (oracle_set_model_param("CGMrecipeOn", cgm_recipe_on) != 0 ||
      sage26_apply_infall_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    MimicConfig.pre_timestep = NULL;
    return problems + 1;
  }

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  halo.galaxy = &galaxy;
  halo.Type = oracle_in_i32(&in, oracle_galaxy_field(0, "Type"));
  halo.Mvir = 0.0; /* read only by a debug log */
  oracle_in_galaxy_floats(&in, 0, &galaxy, RESERVOIRS, COUNT(RESERVOIRS));
  galaxy.InfallingGas = oracle_in_f64(&in, "call.InfallingGas");
  if (cgm_recipe_on == 1) {
    galaxy.Regime = oracle_in_i32(&in, oracle_galaxy_field(0, "Regime"));
    oracle_in_galaxy_floats(&in, 0, &galaxy, oracle_c10_pair(1), 2);
  }
  const struct GalaxyData before = galaxy;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = oracle_in_i32(&in, "call.effective_steps");
  ctx.central_galaxy = &halo;

  if (sage26_apply_infall_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_apply_infall_cleanup();
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;

  oracle_exclude(&out, "intermediate.infallingGas_per_step"); /* package local, not state */
  problems += oracle_check_galaxy_floats(&out, 0, &galaxy, RESERVOIRS, COUNT(RESERVOIRS));

  if (cgm_recipe_on == 1) {
    problems += oracle_check_galaxy_floats(&out, 0, &galaxy, oracle_c10_pair(1), 2);
    /* SAGE26's selection, independently of the package: CGMrecipeOn 1 && Regime 0 (I:371-372) */
    const int to_cgm = (before.Regime == 0);
    problems += oracle_check_i32(&out, "intermediate.reservoir_cgm", to_cgm);
    problems +=
        oracle_c10_check_unselected(out.path, 0, &before, &galaxy, oracle_c10_pair(!to_cgm), 2);
    /* Branch flags of the transcription: the reservoirs above show each outcome */
    const char *const flags[] = {"ejected_withdrawal_ran",      "ejected_metals_floor_binds",
                                 "ejected_exhausted",           "selected_metals_debit_ran",
                                 "selected_metals_floor_binds", "selected_gas_cleared"};
    for (size_t k = 0; k < COUNT(flags); k++) {
      char field[ORACLE_MAX_NAME];
      snprintf(field, sizeof(field), "intermediate.%s", flags[k]);
      (void)oracle_in_i32(&out, field);
    }
    problems += oracle_c10_check_local(&out, "infallingGas_per_step", ORACLE_C10_CONTINUATION, 0, 1,
                                       0, 0.0f, &c10_tally);
    problems += oracle_c10_check_local(&out, "infallingGas_after_ejected", ORACLE_C10_TAIL, 0, 0, 0,
                                       0.0f, &c10_tally);
    problems += oracle_c10_check_local(&out, "metallicity_ejected", ORACLE_C10_TAIL, 1, 0, 0, 0.0f,
                                       &c10_tally);
    problems += oracle_c10_check_local(&out, "metallicity_selected", ORACLE_C10_TAIL, 1, 0, 0, 0.0f,
                                       &c10_tally);
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
  printf("Test Suite: sage26_apply_infall SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
