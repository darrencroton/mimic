/**
 * @file    test_unit_sage_apply_metal_enrichment.c
 * @brief   Unit tests for sage_apply_metal_enrichment module
 *
 * Validates the instantaneous-recycling disk-SF metal yield in isolation using
 * minimal mocks. The module runs process_by_galaxy (one galaxy at a time, ngal=1)
 * and ejects metals into the FOF central supplied through ctx->central_galaxy, so
 * physics tests process a satellite (&halos[1], 1) with a distinct central. Its ordering
 * refusals (after the SF/SN apply step and after starburst) are in
 * models/sage16/modules/_tests/test_unit_sage_dependency_contracts.c.
 *
 * Test cases:
 *   - test_physics_cold_gas_above_threshold: yield split between disk and ejected reservoirs by
 *     the Krumholz & Dekel factor of a non-zero FracZleaveDisk, and NewStellarMass is consumed
 *   - test_physics_cold_gas_threshold_branches: the retained or all-metals branch by ColdGas
 *     against the 1e-8 threshold, with ColdGas 0, at the boundary and just above it
 *   - test_zero_new_stellar_mass: no metals added when there are no new stars
 *   - test_invalid_inputs: a NULL galaxy, a missing FOF central and ngal != 1 leave the state
 *     unchanged, the last with an error
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "shared/sage_constants.h"
#include "util/error.h"
#include "util/memory.h"

/* Shared SAGE16 test fixture boilerplate (counters, config reset, parameters) */
#include "modules/_tests/sage_test_fixtures.h"

extern int sage_apply_metal_enrichment_init(void);
extern int sage_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                               int ngal);
extern int sage_apply_metal_enrichment_cleanup(void);

#define FLOAT_EQ(a, b, epsilon) (fabs((double)(a) - (double)(b)) < (epsilon))

/* SAGE test defaults (mirror sage_test_fixtures.h set_test_model_parameters) */
#define TEST_YIELD 0.03
#define TEST_FRAC_Z_LEAVE_DISK 0.3

static struct ModuleContext create_test_context(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.params = &MimicConfig;
  return ctx;
}

static struct Halo create_test_halo(int type, double mvir, struct GalaxyData *galaxy) {
  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.Type = type;
  halo.Mvir = mvir;
  halo.SnapNum = 63;
  halo.galaxy = galaxy;
  return halo;
}

/**
 * @test    test_physics_cold_gas_above_threshold
 * @brief   Above the cold-gas threshold the yield splits between disk cold gas and the
 *          central hot halo, scaled by the Krumholz & Dekel ejection factor.
 */
int test_physics_cold_gas_above_threshold(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  TEST_ASSERT(sage_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal); /* Mvir=30 → exp(-1) ejection scale */

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0; /* above SAGE_COLD_GAS_YIELD_THRESHOLD */
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  ctx.central_galaxy = &central;
  struct Halo halos[2] = {central, satellite};
  /* halos[] copies the structs, but galaxy pointers still reference cen_gal/sat_gal. */
  ctx.central_galaxy = &halos[0];

  const double stars = 2.0;
  const double frac_z_leave = TEST_FRAC_Z_LEAVE_DISK * exp(-1.0); /* Mvir 30 over the scale 30 */
  const double expected_cold = TEST_YIELD * (1.0 - frac_z_leave) * stars;
  const double expected_hot = TEST_YIELD * frac_z_leave * stars;

  int result = sage_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, expected_cold, 1e-6),
              "disk metals should receive the retained yield");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, expected_hot, 1e-6),
              "central hot metals should receive the ejected yield");
  TEST_ASSERT(halos[1].galaxy->NewStellarMass == 0.0,
              "NewStellarMass is the per-substep transport field and must be consumed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_physics_cold_gas_threshold_branches
 * @brief   ColdGas above 1e-8 keeps part of the yield in the disk and sends the rest to the
 *          central hot halo; at or below it the whole yield goes to the central hot halo
 *
 * The central's Mvir is 0, so the Krumholz & Dekel factor is exactly FracZleaveDisk (0.3);
 * 2 stars at Yield 0.03 give 0.06 of metals, of which 0.042 stay in the disk when the cold gas is
 * above the threshold. float(1e-8) is 9.99999994e-9, below the double threshold 1e-8, so it is the
 * all-metals branch; the next float, 1.00000008e-8, is above it. The metals fields are float32,
 * hence the 1e-6 tolerance, far below the 0.018 that separates the branches.
 */
int test_physics_cold_gas_threshold_branches(void) {
  const struct {
    const char *name;
    float cold_gas;
    double disk_metals; /* the satellite's MetalsColdGas after the call */
    double hot_metals;  /* the central's MetalsHotGas after the call */
  } rows[] = {
      {"ColdGas 0", 0.0f, 0.0, 0.06},
      {"ColdGas at the 1e-8 threshold", 1.0e-8f, 0.0, 0.06},
      {"ColdGas just above the 1e-8 threshold", 1.00000008e-8f, 0.042, 0.018},
  };
  int ok = 1;
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  TEST_ASSERT(sage_apply_metal_enrichment_init() == 0, "init should succeed");
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    struct ModuleContext ctx = create_test_context();
    struct GalaxyData cen_gal, sat_gal;
    memset(&cen_gal, 0, sizeof(cen_gal));
    memset(&sat_gal, 0, sizeof(sat_gal));
    sat_gal.ColdGas = rows[k].cold_gas;
    sat_gal.NewStellarMass = 2.0;
    struct Halo halos[2] = {create_test_halo(0, 0.0, &cen_gal),
                            create_test_halo(1, 10.0, &sat_gal)};
    ctx.central_galaxy = &halos[0];

    const int status = sage_apply_metal_enrichment_process(&ctx, &halos[1], 1);

    if (status != 0 || !FLOAT_EQ(sat_gal.MetalsColdGas, rows[k].disk_metals, 1e-6) ||
        !FLOAT_EQ(cen_gal.MetalsHotGas, rows[k].hot_metals, 1e-6)) {
      printf("\n  row '%s': status %d, disk metals %.9g (expected %.9g), central hot metals %.9g "
             "(expected %.9g)",
             rows[k].name, status, (double)sat_gal.MetalsColdGas, rows[k].disk_metals,
             (double)cen_gal.MetalsHotGas, rows[k].hot_metals);
      ok = 0;
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the yield must split by the ColdGas threshold in every row");
  return TEST_PASS;
}

/**
 * @test    test_zero_new_stellar_mass
 * @brief   No new stars means no metals are produced.
 */
int test_zero_new_stellar_mass(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  TEST_ASSERT(sage_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);

  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 0.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  int result = sage_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, 0.0, 1e-9), "no disk metals when no stars");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, 0.0, 1e-9), "no hot metals when no stars");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_invalid_inputs
 * @brief   A halo with no galaxy and a missing FOF central are no-ops that leave every field
 *          unchanged, and process_by_galaxy's ngal != 1 is an error that does the same
 *
 * The missing-central row keeps NewStellarMass unconsumed: the module returns before consuming
 * it, so no yield is lost or applied.
 */
int test_invalid_inputs(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  TEST_ASSERT(sage_apply_metal_enrichment_init() == 0, "init should succeed");

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);
  struct GalaxyData central_in;
  memcpy(&central_in, &cen_gal, sizeof(cen_gal));

  /* No galaxy on the processed halo */
  struct Halo galaxyless = create_test_halo(1, 10.0, NULL);
  ctx.central_galaxy = &central;
  TEST_ASSERT(sage_apply_metal_enrichment_process(&ctx, &galaxyless, 1) == 0,
              "NULL galaxy should be a graceful no-op");
  TEST_ASSERT(memcmp(&cen_gal, &central_in, sizeof(cen_gal)) == 0,
              "NULL galaxy: the central is unchanged");

  /* No FOF central */
  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);
  struct GalaxyData sat_in;
  memcpy(&sat_in, &sat_gal, sizeof(sat_gal));
  ctx.central_galaxy = NULL;
  TEST_ASSERT(sage_apply_metal_enrichment_process(&ctx, &satellite, 1) == 0,
              "missing central should be a graceful no-op");
  TEST_ASSERT(memcmp(&sat_gal, &sat_in, sizeof(sat_gal)) == 0,
              "missing central: every field of the row is unchanged, NewStellarMass included");

  /* ngal != 1 */
  struct Halo pair[2] = {create_test_halo(1, 10.0, &sat_gal), create_test_halo(0, 30.0, &cen_gal)};
  ctx.central_galaxy = &pair[1];
  TEST_ASSERT(sage_apply_metal_enrichment_process(&ctx, pair, 2) == -1,
              "ngal != 1 should return an error");
  TEST_ASSERT(memcmp(&sat_gal, &sat_in, sizeof(sat_gal)) == 0 &&
                  memcmp(&cen_gal, &central_in, sizeof(cen_gal)) == 0,
              "ngal != 1: no galaxy is changed");
  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_apply_metal_enrichment Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_physics_cold_gas_above_threshold);
  TEST_RUN(test_physics_cold_gas_threshold_branches);
  TEST_RUN(test_zero_new_stellar_mass);
  TEST_RUN(test_invalid_inputs);

  TEST_SUMMARY();
  return TEST_RESULT();
}
