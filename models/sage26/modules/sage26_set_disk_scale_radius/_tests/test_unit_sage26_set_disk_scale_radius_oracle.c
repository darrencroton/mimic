/**
 * @file    test_unit_sage26_set_disk_scale_radius_oracle.c
 * @brief   Bit-exact comparison of sage26_set_disk_scale_radius with SAGE26's get_disk_radius,
 *          get_bulge_radius and the central's synchronisation
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_set_disk_scale_radius (its
 * _tests/oracle/ directory), dispatching on the fixture's routine:
 *
 * - get_disk_radius: through the module's process() with BulgeSizeOn 0 (R0), asserting
 *   galaxy[0].DiskScaleRadius, the float store of get_disk_radius's result (model_misc.c:203-228,
 *   core_build_model.c:297); return.r_disk is the double before that store, which the package never
 *   keeps, so it is checked only as consistent with the stored float. The input carries no bulge
 *   sizes: the synchronisation that follows zeroes the radii in mode 0, which this
 *   fixture's output does not record;
 * - get_bulge_radius (gbr_*): the shared helper mimic_sage_get_bulge_radius
 *   (shared/sage_bulge_size_physics.h, model_misc.c:239-377) called directly with the fixture's
 *   BulgeSizeOn and Hubble_h, asserting the double return, the six-member size block, every other
 *   galaxy member unchanged, the five branch flags and every local the oracle records;
 * - central_disk_and_bulge_radius (tr_sync_*): the oracle's transcription of
 *   core_build_model.c:297-298, the disk radius and then get_bulge_radius on the FoF central,
 *   through the module's process() with the fixture's BulgeSizeOn and Hubble_h, asserting the size
 *   block (the new DiskScaleRadius and the synchronised radii) and every other member unchanged;
 *   return.r_disk is checked as in the get_disk_radius case.
 *
 * Core-owned inputs (Vvir, Rvir, Hubble_h) and the simulation's halo Spin come from the
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
#include "modules/_tests/sage26_oracle_sizes.h"
#include "shared/sage_bulge_size_physics.h"

extern int sage26_set_disk_scale_radius_init(void);
extern int sage26_set_disk_scale_radius_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage26_set_disk_scale_radius_cleanup(void);

static const char *MODULE = "sage26_set_disk_scale_radius";

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static int disk_fixtures = 0, bulge_fixtures = 0, sync_fixtures = 0;
static struct oracle_c07_local_tally local_tally;

/** @brief Initialise the module with a BulgeSizeOn and Hubble_h; returns the number of problems */
static int init_module(const char *name, const struct MimicSageBulgeSize *sizes) {
  reset_config();
  MimicConfig.Hubble_h = sizes->hubble_h;
  if (oracle_set_model_param("BulgeSizeOn", (double)sizes->bulge_size_on) != 0 ||
      sage26_set_disk_scale_radius_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return 1;
  }
  return 0;
}

/** @brief get_disk_radius through process(); returns the number of mismatches and errors */
static int check_disk_radius(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "get_disk_radius", &in, &out) != 0) {
    return 1;
  }

  int problems = 0;
  /* SAGE26's R0 BulgeSizeOn 0, the module's required parameter: get_disk_radius fixtures carry
   * no size configuration (oracle README "Routines") */
  const struct MimicSageBulgeSize r0_sizes = {MIMIC_SAGE_BULGE_SIZE_OFF, 0.0};
  if (init_module(name, &r0_sizes) != 0) {
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

  disk_fixtures++;
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief get_bulge_radius through the shared helper; returns mismatches and errors */
static int check_bulge_radius(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "get_bulge_radius", &in, &out) != 0) {
    return 1;
  }
  struct MimicSageBulgeSize sizes;
  int problems = oracle_c07_read_sizes(&in, &sizes);
  if (oracle_in_i32(&in, "call.ngal") != 1 || oracle_in_i32(&in, "call.p") != 0) {
    fprintf(stderr, "oracle: %s: call.ngal must be 1 and call.p 0\n", name);
    problems++;
  }
  struct GalaxyData galaxy, before;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_c07_in_size_block(&in, 0, &galaxy);
  before = galaxy;

  struct MimicSageBulgeRadiusLocals l;
  const double r_bulge = mimic_sage_get_bulge_radius(&galaxy, &sizes, &l);

  problems += oracle_check_f64(&out, "return.R_bulge", r_bulge);
  problems += oracle_c07_check_size_block(&out, 0, &galaxy);
  struct GalaxyData expected = before;
  expected.BulgeRadius = galaxy.BulgeRadius;
  expected.MergerBulgeRadius = galaxy.MergerBulgeRadius;
  expected.InstabilityBulgeRadius = galaxy.InstabilityBulgeRadius;
  if (memcmp(&expected, &galaxy, sizeof(expected)) != 0) {
    fprintf(stderr, "oracle: %s: get_bulge_radius changed a member other than the three radii\n",
            name);
    problems++;
  }
  problems += oracle_check_i32(&out, "intermediate.total_nonpositive", l.total_nonpositive);
  problems += oracle_check_i32(&out, "intermediate.merger_mass_zero", l.merger_mass_zero);
  problems += oracle_check_i32(&out, "intermediate.instability_mass_zero", l.instability_mass_zero);
  problems += oracle_check_i32(&out, "intermediate.merger_fallback", l.merger_fallback);
  problems += oracle_check_i32(&out, "intermediate.instability_fallback", l.instability_fallback);
  const struct {
    const char *name;
    double value;
  } values[] = {{"M_merger", l.M_merger},
                {"M_instability", l.M_instability},
                {"M_total", l.M_total},
                {"M_merger_sun", l.M_merger_sun},
                {"log_R_merger_kpc", l.log_R_merger_kpc},
                {"R_merger", l.R_merger},
                {"R_disc", l.R_disc},
                {"M_inst_sun", l.M_inst_sun},
                {"log_R_inst_kpc", l.log_R_inst_kpc},
                {"R_instability", l.R_instability},
                {"R_bulge", l.R_bulge}};
  for (size_t k = 0; k < COUNT(values); k++) {
    problems += oracle_c07_check_local(&out, values[k].name, values[k].value, 0, &local_tally);
  }
  bulge_fixtures++;
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/**
 * @brief The central's synchronisation (core_build_model.c:297-298) through process()
 * @return Number of mismatches and errors
 */
static int check_sync(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "central_disk_and_bulge_radius", &in, &out) != 0) {
    return 1;
  }
  struct MimicSageBulgeSize sizes;
  int problems = oracle_c07_read_sizes(&in, &sizes);
  if (oracle_in_i32(&in, "call.ngal") != 1 || oracle_in_i32(&in, "call.p") != 0) {
    fprintf(stderr, "oracle: %s: call.ngal must be 1 and call.p 0\n", name);
    problems++;
  }
  problems += init_module(name, &sizes);

  struct GalaxyData galaxy, before;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  halo.galaxy = &galaxy;
  halo.Type = oracle_in_i32(&in, oracle_galaxy_field(0, "Type"));
  halo.Vvir = oracle_in_f32(&in, oracle_galaxy_field(0, "Vvir"));
  halo.Rvir = oracle_in_f32(&in, oracle_galaxy_field(0, "Rvir"));
  for (int j = 0; j < 3; j++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "halo.Spin[%d]", j);
    halo.Spin[j] = oracle_in_f32(&in, field);
  }
  oracle_c07_in_size_block(&in, 0, &galaxy);
  before = galaxy;

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

  problems += oracle_c07_check_size_block(&out, 0, &galaxy);
  const float stored = (float)oracle_in_f64(&out, "return.r_disk");
  if (oracle_f32_to_bits(stored) != oracle_f32_to_bits(galaxy.DiskScaleRadius)) {
    fprintf(stderr, "oracle: %s: DiskScaleRadius is not the float store of return.r_disk\n", name);
    problems++;
  }
  struct GalaxyData expected = before;
  expected.DiskScaleRadius = galaxy.DiskScaleRadius;
  expected.BulgeRadius = galaxy.BulgeRadius;
  expected.MergerBulgeRadius = galaxy.MergerBulgeRadius;
  expected.InstabilityBulgeRadius = galaxy.InstabilityBulgeRadius;
  if (memcmp(&expected, &galaxy, sizeof(expected)) != 0) {
    fprintf(stderr,
            "oracle: %s: the synchronisation changed a member other than the disk and "
            "bulge radii\n",
            name);
    problems++;
  }
  sync_fixtures++;
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Dispatch one fixture by its name's routine prefix */
static int check_fixture(const char *name) {
  if (strncmp(name, "gbr_", 4) == 0) {
    return check_bulge_radius(name);
  }
  if (strncmp(name, "tr_sync_", 8) == 0) {
    return check_sync(name);
  }
  return check_disk_radius(name);
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
  disk_fixtures = bulge_fixtures = sync_fixtures = 0;
  memset(&local_tally, 0, sizeof(local_tally));
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: get_disk_radius %d, get_bulge_radius %d (locals validated %d, "
         "narrowing-limited %d, not validated %d, absent %d), central synchronisation %d ",
         disk_fixtures, bulge_fixtures, local_tally.validated, local_tally.narrowing_limited,
         local_tally.not_validated, local_tally.absent, sync_fixtures);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(disk_fixtures >= 1 && bulge_fixtures >= 1 && sync_fixtures >= 1,
              "every routine of the module's fixtures must be asserted");
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
