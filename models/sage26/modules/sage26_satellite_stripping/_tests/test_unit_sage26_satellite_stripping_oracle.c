/**
 * @file    test_unit_sage26_satellite_stripping_oracle.c
 * @brief   Bit-exact comparison of sage26_satellite_stripping with SAGE26's strip_from_satellite
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_satellite_stripping (its
 * _tests/oracle/ directory) through the module's process() entry on the fixture's
 * satellite (call.satellite), with the fixture's central as the FoF central, and
 * asserts bit-for-bit equality with the oracle output: the hot and CGM reservoirs
 * of every galaxy after the call-site gate and strip_from_satellite
 * (core_build_model.c:493-498, model_infall.c:225-273).
 *
 * The satellite's HaloBaryonFraction is supplied as the package stores it: the
 * oracle's intermediate.HaloBaryonFraction, SAGE26's once-rounded
 * reionization_modifier * BaryonFrac (sage26_reionization is tested on its own
 * fixtures). intermediate.reionization_modifier is SAGE26's local, never formed
 * separately by the package, so it is not asserted. When the call-site gate does
 * not call strip_from_satellite (intermediate.strip_from_satellite_called 0, e.g.
 * a Type 2 orphan), the recorded modifier and product come from the oracle's
 * pre-gate evaluation and are not SAGE26 state (oracle README); they are still
 * fed to the module, which must leave every reservoir bit-identical to its input,
 * checked against the inputs as well as against the oracle output. Outside the
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

extern int sage26_satellite_stripping_init(void);
extern int sage26_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos,
                                              int ngal);
extern int sage26_satellite_stripping_cleanup(void);

#define MAX_GALAXIES 8

static const char *MODULE = "sage26_satellite_stripping";

/* Reservoirs strip_from_satellite reads (oracle_c03.c run_strip_from_satellite) */
static const char *const INPUT_RESERVOIRS[] = {
    "StellarMass",  "ColdGas",     "HotGas",        "MetalsHotGas", "CGMgas",
    "MetalsCGMgas", "EjectedMass", "BlackHoleMass", "ICS",
};

/* Reservoirs the oracle records after the call */
static const char *const OUTPUT_RESERVOIRS[] = {"HotGas", "MetalsHotGas", "CGMgas", "MetalsCGMgas"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "strip_from_satellite", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  if (oracle_in_i32(&in, "params.ReionizationOn") != 1 ||
      oracle_in_i32(&in, "params.CGMrecipeOn") != 0) {
    fprintf(stderr, "oracle: %s: not an R0 fixture (ReionizationOn 1, CGMrecipeOn 0)\n", name);
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
      sage26_satellite_stripping_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return problems + 1;
  }

  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int centralgal = oracle_in_i32(&in, "call.centralgal");
  const int satellite = oracle_in_i32(&in, "call.satellite");
  if (ngal < 1 || ngal > MAX_GALAXIES || centralgal < 0 || centralgal >= ngal || satellite < 0 ||
      satellite >= ngal || satellite == centralgal) {
    fprintf(stderr, "oracle: %s: call indices out of range\n", name);
    sage26_satellite_stripping_cleanup();
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
  }

  /* Every galaxy's state before the call, for the not-called check below */
  struct GalaxyData before[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    before[i] = galaxies[i];
  }

  /* HaloBaryonFraction as the package stores it (see the file comment) */
  galaxies[satellite].HaloBaryonFraction = oracle_in_f64(&out, "intermediate.HaloBaryonFraction");
  oracle_exclude(&out, "intermediate.reionization_modifier"); /* SAGE26 local, not package state */

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = oracle_in_f64(&in, "call.Zcurr");
  ctx.params = &MimicConfig;
  ctx.num_substeps = oracle_in_i32(&in, "call.effective_steps");
  ctx.central_galaxy = &halos[centralgal];

  if (sage26_satellite_stripping_process(&ctx, &halos[satellite], 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_satellite_stripping_cleanup();

  /* When SAGE26's call-site gate does not call strip_from_satellite
   * (core_build_model.c:493-498), the module must visibly do nothing: every
   * galaxy's reservoirs stay bit-identical to their inputs. */
  if (oracle_in_i32(&out, "intermediate.strip_from_satellite_called") == 0) {
    for (int i = 0; i < ngal; i++) {
      problems += oracle_check_galaxy_unchanged(&out, i, &before[i], &galaxies[i],
                                                OUTPUT_RESERVOIRS, COUNT(OUTPUT_RESERVOIRS));
    }
  }
  for (int i = 0; i < ngal; i++) {
    problems += oracle_check_galaxy_floats(&out, i, &galaxies[i], OUTPUT_RESERVOIRS,
                                           COUNT(OUTPUT_RESERVOIRS));
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
  printf("Test Suite: sage26_satellite_stripping SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
