/**
 * @file    test_unit_sage26_reionization_oracle.c
 * @brief   Bit-exact comparison of sage26_reionization with SAGE26's do_reionization
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_reionization
 * (models/sage26/modules/sage26_reionization/_tests/oracle/) through the module's
 * process() entry and asserts bit-for-bit equality with the oracle output:
 *
 * - intermediate.HaloBaryonFraction, the once-rounded reionization_modifier *
 *   BaryonFrac the package stores, from a pass with the fixture's BaryonFrac;
 * - return.reionization_modifier, from a second pass with GlobalBaryonFraction
 *   0.5: multiplying by a power of two is exact for these normal values, so the
 *   stored HaloBaryonFraction / 0.5 is the module's modifier, bit for bit.
 *
 * Core-owned inputs (Omega, OmegaLambda, Hubble, G, the redshift, the halo's
 * Mvir) are taken from the fixture; a0 and ar must equal the module's own
 * derivation from z0 = 8, zr = 7. Outside the oracle environment (macOS arm64,
 * Apple clang 21) the bit-exact test reports SKIP.
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

extern int sage26_reionization_init(void);
extern int sage26_reionization_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_reionization_cleanup(void);

static const char *MODULE = "sage26_reionization";

/** @brief Run the module once on the fixture's state with this GlobalBaryonFraction */
static int run_module(struct oracle_fixture *in, const double baryon_frac,
                      double *halo_baryon_fraction) {
  reset_config();
  MimicConfig.Omega = oracle_in_f64(in, "params.Omega");
  MimicConfig.OmegaLambda = oracle_in_f64(in, "params.OmegaLambda");
  MimicConfig.Hubble = oracle_in_f64(in, "params.Hubble");
  MimicConfig.G = oracle_in_f64(in, "params.G");
  if (oracle_set_model_param("GlobalBaryonFraction", baryon_frac) != 0 ||
      sage26_reionization_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", in->path);
    return -1;
  }

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  halo.galaxy = &galaxy;
  halo.Type = oracle_in_i32(in, oracle_galaxy_field(0, "Type"));
  /* SAGE26's float Mvir, exactly widened to the core's double */
  halo.Mvir = oracle_in_f32(in, oracle_galaxy_field(0, "Mvir"));

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.redshift = oracle_in_f64(in, "call.Zcurr");
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halo;

  const int status = sage26_reionization_process(&ctx, &halo, 1);
  sage26_reionization_cleanup();
  *halo_baryon_fraction = galaxy.HaloBaryonFraction;
  return status;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "do_reionization", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;

  /* The module derives a0 and ar from z0 = 8 and zr = 7 as SAGE26 does
   * (core_init.c:70-71); the fixture must use the same values. */
  if (oracle_f64_to_bits(oracle_in_f64(&in, "params.a0")) !=
          oracle_f64_to_bits(1.0 / (1.0 + 8.0)) ||
      oracle_f64_to_bits(oracle_in_f64(&in, "params.ar")) !=
          oracle_f64_to_bits(1.0 / (1.0 + 7.0))) {
    fprintf(stderr, "oracle: %s: params.a0/ar differ from the module's z0 = 8, zr = 7\n", name);
    problems++;
  }

  const double baryon_frac = oracle_in_f64(&in, "params.BaryonFrac");
  double halo_baryon_fraction = 0.0;
  if (run_module(&in, baryon_frac, &halo_baryon_fraction) != 0) {
    return problems + 1;
  }
  problems += oracle_check_f64(&out, "intermediate.HaloBaryonFraction", halo_baryon_fraction);

  /* Second pass: GlobalBaryonFraction 0.5 makes the stored product modifier * 0.5,
   * exact for a normal modifier, so dividing by 0.5 recovers the modifier exactly. */
  double half_product = 0.0;
  if (run_module(&in, 0.5, &half_product) != 0) {
    return problems + 1;
  }
  problems += oracle_check_f64(&out, "return.reionization_modifier", half_product / 0.5);

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
  printf("Test Suite: sage26_reionization SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
