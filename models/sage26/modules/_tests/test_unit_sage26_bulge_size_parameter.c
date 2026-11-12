/**
 * @file    test_unit_sage26_bulge_size_parameter.c
 * @brief   What BulgeSizeOn selects: the mode-0 size helpers and the disk-radius module's
 *          configuration requirement in mode 3
 *
 * BulgeSizeOn is a required package parameter, 0 (no sizes, SAGE26's default) or 3 (the Tonini
 * sizes); modes 1 and 2 of SAGE26 are not supported. The refusal of a missing key and of every
 * unsupported value, and the acceptance of 0 and 3, by every module that reads it are in
 * test_unit_sage26_parameter_refusals.c. The mode-0 helpers keep SAGE26's BulgeSizeOn 0
 * behaviour: get_bulge_radius zeroes the three radii and returns 0, and
 * update_instability_bulge_radius returns at once, stale radii included.
 *
 * Test cases:
 *   - test_mode0_helpers: mode 0 zeroes in get_bulge_radius and returns in the update
 *   - test_mode3_disk_radius_needs_configuration: the disk-radius module refuses mode 3
 *     without the run configuration (Hubble_h), and runs mode 0 without it
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_bulge_size_physics.h"

extern int sage26_set_disk_scale_radius_init(void);
extern int sage26_set_disk_scale_radius_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage26_set_disk_scale_radius_cleanup(void);

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/**
 * @test    test_mode0_helpers
 * @brief   BulgeSizeOn 0: get_bulge_radius zeroes and returns 0, the update returns at once
 */
int test_mode0_helpers(void) {
  const struct MimicSageBulgeSize r0 = {MIMIC_SAGE_BULGE_SIZE_OFF, 0.73};
  struct GalaxyData g;
  memset(&g, 0, sizeof(g));
  g.MergerBulgeMass = 0.5f;
  g.InstabilityBulgeMass = 0.25f;
  g.DiskScaleRadius = 0.004f;
  g.BulgeRadius = 0.0005f;
  g.MergerBulgeRadius = 0.0007f;
  g.InstabilityBulgeRadius = 0.0003f;

  struct GalaxyData updated = g;
  struct MimicSageInstabilityRadiusLocals ul;
  mimic_sage_update_instability_bulge_radius(&updated, 0.0625, 0.004, &r0, &ul);
  TEST_ASSERT(memcmp(&updated, &g, sizeof(g)) == 0,
              "mode 0: the update must leave every member, the stale radii included, as it was");
  TEST_ASSERT(ul.returned_early == 1 && isnan(ul.R_new), "mode 0: the update returns at once");

  struct MimicSageBulgeRadiusLocals bl;
  const double r = mimic_sage_get_bulge_radius(&g, &r0, &bl);
  TEST_ASSERT(r == 0.0 && g.BulgeRadius == 0.0f && g.MergerBulgeRadius == 0.0f &&
                  g.InstabilityBulgeRadius == 0.0f,
              "mode 0: get_bulge_radius zeroes the three radii and returns 0");
  TEST_ASSERT(g.MergerBulgeMass == 0.5f && g.InstabilityBulgeMass == 0.25f,
              "mode 0: get_bulge_radius leaves the channel masses");
  TEST_ASSERT(isnan(bl.M_total) && bl.total_nonpositive == -1,
              "mode 0: get_bulge_radius forms no local");
  return TEST_PASS;
}

/**
 * @test    test_mode3_disk_radius_needs_configuration
 * @brief   The disk-radius module refuses BulgeSizeOn 3 without the run configuration
 *          (Hubble_h) and runs BulgeSizeOn 0 without it
 */
int test_mode3_disk_radius_needs_configuration(void) {
  const char *const values[] = {"3", "0"};
  for (size_t v = 0; v < COUNT(values); v++) {
    reset_config();
    sage26_test_append_model_parameter("BulgeSizeOn", values[v]);
    TEST_ASSERT(sage26_set_disk_scale_radius_init() == 0, "init must accept 0 and 3");
    struct GalaxyData g;
    memset(&g, 0, sizeof(g));
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));
    halo.galaxy = &g;
    halo.Type = 0;
    halo.Vvir = 200.0;
    halo.Rvir = 0.2;
    halo.Spin[0] = 1.0f;
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = NULL;
    const int status = sage26_set_disk_scale_radius_process(&ctx, &halo, 1);
    printf("\n  BulgeSizeOn %s without the run configuration: process %d", values[v], status);
    if (v == 0) {
      TEST_ASSERT(status != 0, "mode 3 must refuse a context without the run configuration");
    } else {
      TEST_ASSERT(status == 0, "mode 0 reads no Hubble_h and must run without it");
    }
    sage26_set_disk_scale_radius_cleanup();
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE26 BulgeSizeOn mode-0 size helpers\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  TEST_RUN(test_mode0_helpers);
  TEST_RUN(test_mode3_disk_radius_needs_configuration);

  check_memory_leaks();
  TEST_SUMMARY();
  return TEST_RESULT();
}
