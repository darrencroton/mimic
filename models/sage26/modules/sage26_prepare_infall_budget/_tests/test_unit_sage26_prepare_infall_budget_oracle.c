/**
 * @file    test_unit_sage26_prepare_infall_budget_oracle.c
 * @brief   Bit-exact comparison of sage26_prepare_infall_budget with SAGE26's infall_recipe
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_prepare_infall_budget (its _tests/oracle/ directory) through the module's
 * process() entry and asserts bit-for-bit equality with the oracle output:
 * return.InfallingGas (the package's InfallingGas) and, for every galaxy, the
 * ejected, ICS (with its three history fields), CGM and hot reservoirs.
 *
 * The central's HaloBaryonFraction is supplied as the package stores it: the
 * oracle's intermediate.HaloBaryonFraction, SAGE26's once-rounded
 * reionization_modifier * BaryonFrac (sage26_reionization is tested on its own
 * fixtures). intermediate.reionization_modifier is SAGE26's local, which the
 * package never forms separately, so it is not asserted. Core-owned inputs (each
 * halo's Mvir, the redshift) come from the fixture. Outside the oracle environment
 * (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * A CGMrecipeOn 1 fixture (c10) also sets every galaxy's Regime and the module's CGMrecipeOn 1;
 * the test asserts the oracle's migration direction (intermediate.central_regime_cgm) against
 * the central's input Regime 0, every satellite's hot and CGM pairs unchanged (satellites keep
 * their CGM and are not consolidated), and consumes the CGM safety-check flags, whose outcomes
 * the asserted reservoirs show (oracle README, "The c10 Group").
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"

extern int sage26_prepare_infall_budget_init(void);
extern int sage26_prepare_infall_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage26_prepare_infall_budget_cleanup(void);

#define MAX_GALAXIES 8

static const char *MODULE = "sage26_prepare_infall_budget";

/* Reservoirs infall_recipe reads (oracle_c03.c run_infall_recipe) */
static const char *const INPUT_RESERVOIRS[] = {
    "StellarMass",  "BlackHoleMass",     "ColdGas",     "HotGas",     "MetalsHotGas",
    "EjectedMass",  "MetalsEjectedMass", "ICS",         "MetalsICS",  "CGMgas",
    "MetalsCGMgas", "ICS_disrupt",       "ICS_accrete", "ICS_sum_mt",
};

/* Reservoirs the oracle records after the call */
static const char *const OUTPUT_RESERVOIRS[] = {
    "EjectedMass",  "MetalsEjectedMass", "ICS",    "MetalsICS",    "ICS_disrupt",
    "ICS_accrete",  "ICS_sum_mt",        "CGMgas", "MetalsCGMgas", "HotGas",
    "MetalsHotGas",
};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "infall_recipe", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;

  /* R0 or c10: reionization on (its result arrives as HaloBaryonFraction), CGM recipe 0 or 1 */
  const int cgm_recipe_on = oracle_in_i32(&in, "params.CGMrecipeOn");
  if (oracle_in_i32(&in, "params.ReionizationOn") != 1 ||
      (cgm_recipe_on != 0 && cgm_recipe_on != 1)) {
    fprintf(stderr,
            "oracle: %s: not an R0 (CGMrecipeOn 0) or c10 (CGMrecipeOn 1) fixture with "
            "ReionizationOn 1\n",
            name);
    problems++;
  }
  /* The reionization inputs are applied by sage26_reionization, upstream; here they
   * arrive only through intermediate.HaloBaryonFraction. */
  const char *const reionization_inputs[] = {"params.a0",          "params.ar",     "params.Omega",
                                             "params.OmegaLambda", "params.Hubble", "params.G"};
  for (size_t k = 0; k < COUNT(reionization_inputs); k++) {
    (void)oracle_in_f64(&in, reionization_inputs[k]);
  }

  reset_config();
  if (oracle_set_model_param("GlobalBaryonFraction", oracle_in_f64(&in, "params.BaryonFrac")) !=
          0 ||
      oracle_set_model_param("CGMrecipeOn", cgm_recipe_on) != 0 ||
      sage26_prepare_infall_budget_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return problems + 1;
  }

  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int centralgal = oracle_in_i32(&in, "call.centralgal");
  if (ngal < 1 || ngal > MAX_GALAXIES || centralgal < 0 || centralgal >= ngal) {
    fprintf(stderr, "oracle: %s: call.ngal / call.centralgal out of range\n", name);
    sage26_prepare_infall_budget_cleanup();
    return problems + 1;
  }

  struct GalaxyData galaxies[MAX_GALAXIES];
  struct Halo halos[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    oracle_poison(&galaxies[i], sizeof(galaxies[i]));
    oracle_poison(&halos[i], sizeof(halos[i]));
    halos[i].galaxy = &galaxies[i];
    halos[i].Type = oracle_in_i32(&in, oracle_galaxy_field(i, "Type"));
    /* SAGE26's float Mvir, exactly widened to the core's double */
    halos[i].Mvir = oracle_in_f32(&in, oracle_galaxy_field(i, "Mvir"));
    oracle_in_galaxy_floats(&in, i, &galaxies[i], INPUT_RESERVOIRS, COUNT(INPUT_RESERVOIRS));
    if (cgm_recipe_on == 1) {
      galaxies[i].Regime = oracle_in_i32(&in, oracle_galaxy_field(i, "Regime"));
    }
  }
  struct GalaxyData before[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    before[i] = galaxies[i];
  }

  /* The package finds the FoF central as the first Type 0; the fixture's must be it */
  int first_type0 = -1;
  for (int i = 0; i < ngal && first_type0 == -1; i++) {
    first_type0 = (halos[i].Type == 0) ? i : -1;
  }
  if (first_type0 != centralgal) {
    fprintf(stderr, "oracle: %s: call.centralgal is not the first Type 0 galaxy\n", name);
    problems++;
  }

  /* HaloBaryonFraction as the package stores it (see the file comment) */
  const double halo_baryon_fraction = oracle_in_f64(&out, "intermediate.HaloBaryonFraction");
  galaxies[centralgal].HaloBaryonFraction = halo_baryon_fraction;
  oracle_exclude(&out, "intermediate.reionization_modifier"); /* SAGE26 local, not package state */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = oracle_in_f64(&in, "call.Zcurr");
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[centralgal];

  if (sage26_prepare_infall_budget_process(&ctx, halos, ngal) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_prepare_infall_budget_cleanup();

  problems += oracle_check_f64(&out, "return.InfallingGas", galaxies[centralgal].InfallingGas);
  if (oracle_f64_to_bits(galaxies[centralgal].HaloBaryonFraction) !=
      oracle_f64_to_bits(halo_baryon_fraction)) {
    fprintf(stderr, "oracle: %s: the budget changed the central's HaloBaryonFraction\n", name);
    problems++;
  }
  for (int i = 0; i < ngal; i++) {
    problems += oracle_check_galaxy_floats(&out, i, &galaxies[i], OUTPUT_RESERVOIRS,
                                           COUNT(OUTPUT_RESERVOIRS));
  }

  if (cgm_recipe_on == 1) {
    /* SAGE26's migration direction, independently of the package (I:164) */
    problems +=
        oracle_check_i32(&out, "intermediate.central_regime_cgm", before[centralgal].Regime == 0);
    /* Satellites keep their CGM (I:115-117) and their hot gas (never consolidated) */
    for (int i = 0; i < ngal; i++) {
      if (i != centralgal) {
        problems += oracle_c10_check_unselected(out.path, i, &before[i], &galaxies[i],
                                                oracle_c10_pair(1), 2);
        problems += oracle_c10_check_unselected(out.path, i, &before[i], &galaxies[i],
                                                oracle_c10_pair(0), 2);
      }
    }
    /* The CGM safety checks' flags (I:184-193): the asserted CGM pair shows each outcome */
    (void)oracle_in_i32(&out, "intermediate.cgm_metals_cap_binds");
    (void)oracle_in_i32(&out, "intermediate.cgm_gas_negative_clears");
    (void)oracle_in_i32(&out, "intermediate.cgm_metals_floor_binds");
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
  printf("Test Suite: sage26_prepare_infall_budget SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
