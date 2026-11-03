/**
 * @file    test_unit_sage26_set_disk_scale_radius_oracle.c
 * @brief   Bit-exact comparison of sage26_set_disk_scale_radius with SAGE26's get_disk_radius
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_set_disk_scale_radius (its
 * _tests/oracle/ directory) through the module's process() entry and asserts
 * bit-for-bit equality with the oracle output galaxy[0].DiskScaleRadius, the float
 * store of get_disk_radius's result (model_misc.c:203-228, core_build_model.c:297).
 * return.r_disk is the double before that store, which the package never keeps,
 * so it is checked only as consistent with the stored float.
 *
 * Core-owned inputs (Vvir, Rvir) and the simulation's halo Spin come from the
 * fixture. Outside the oracle environment (macOS arm64, Apple clang 21) the
 * bit-exact test reports SKIP.
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

extern int sage26_set_disk_scale_radius_init(void);
extern int sage26_set_disk_scale_radius_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage26_set_disk_scale_radius_cleanup(void);

static const char *MODULE = "sage26_set_disk_scale_radius";

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "get_disk_radius", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  reset_config();
  if (sage26_set_disk_scale_radius_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return 1;
  }

  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  halo.galaxy = &galaxy;
  halo.Type = oracle_in_i32(&in, oracle_galaxy_field(0, "Type"));
  /* SAGE26's float Vvir and Rvir, exactly widened to the core's doubles */
  halo.Vvir = oracle_in_f32(&in, oracle_galaxy_field(0, "Vvir"));
  halo.Rvir = oracle_in_f32(&in, oracle_galaxy_field(0, "Rvir"));
  for (int j = 0; j < 3; j++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "halo.Spin[%d]", j);
    halo.Spin[j] = oracle_in_f32(&in, field);
  }

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halo;

  if (sage26_set_disk_scale_radius_process(&ctx, &halo, 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  sage26_set_disk_scale_radius_cleanup();

  problems +=
      oracle_check_f32(&out, oracle_galaxy_field(0, "DiskScaleRadius"), galaxy.DiskScaleRadius);
  /* return.r_disk: the double before the float store, not package state */
  const float stored = (float)oracle_in_f64(&out, "return.r_disk");
  if (oracle_f32_to_bits(stored) != oracle_f32_to_bits(galaxy.DiskScaleRadius)) {
    fprintf(stderr, "oracle: %s: DiskScaleRadius is not the float store of return.r_disk\n", name);
    problems++;
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
  printf("Test Suite: sage26_set_disk_scale_radius SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
