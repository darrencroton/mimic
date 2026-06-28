/**
 * @file    test_unit_sage_apply_metal_enrichment.c
 * @brief   Unit tests for sage_apply_metal_enrichment module
 *
 * Validates the instantaneous-recycling disk-SF metal yield in isolation using
 * minimal mocks. The module runs process_by_galaxy (one galaxy at a time, ngal=1)
 * and ejects metals into the FOF central supplied through ctx->central_galaxy, so
 * physics tests process a satellite (&halos[1], 1) with a distinct central.
 *
 * Test cases:
 *   - test_module_registration: module registers without error
 *   - test_init_valid: init/cleanup lifecycle succeeds with valid parameters
 *   - test_init_rejects_sf_after: init fails when the SF/SN apply step is ordered after it
 *   - test_init_rejects_starburst_after: init fails when starburst is ordered after it
 *   - test_physics_cold_gas_above_threshold: yield split between disk and ejected reservoirs
 *   - test_physics_cold_gas_below_threshold: full yield ejected to the central hot halo
 *   - test_consumes_new_stellar_mass: NewStellarMass is zeroed after application
 *   - test_zero_new_stellar_mass: no metals added when there are no new stars
 *   - test_null_galaxy: NULL galaxy handled gracefully
 *   - test_null_central: missing FOF central handled gracefully
 *   - test_invalid_ngal: error when ngal != 1
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"

#include "core/module_interface.h"
#include "core/module_registry.h"
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
 * @test    test_module_registration
 * @brief   Module registers correctly
 */
int test_module_registration(void) {
  reset_config();
  ensure_modules_registered();
  return TEST_PASS;
}

/**
 * @test    test_init_valid
 * @brief   init/cleanup lifecycle succeeds with valid parameters and no predecessors
 */
int test_init_valid(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();

  /* No SF/SN or starburst modules configured, so the ordering checks are skipped. */
  TEST_ASSERT(sage_apply_metal_enrichment_init() == 0, "init should succeed with valid parameters");

  sage_apply_metal_enrichment_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_init_rejects_sf_after
 * @brief   init fails when sage_apply_star_formation_supernova is ordered after this module
 */
int test_init_rejects_sf_after(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();

  /* Wrong order: the SF/SN apply step must precede the yield. */
  test_phase_add("galaxy_physics", "sage_apply_metal_enrichment", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);

  TEST_ASSERT(sage_apply_metal_enrichment_init() == -1,
              "init should reject SF/SN apply step ordered after the yield");

  test_free_substep_phases();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_init_rejects_starburst_after
 * @brief   init fails when sage_starburst_feedback is ordered after this module
 */
int test_init_rejects_starburst_after(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();

  /* Wrong order: SAGE adds the disk yield after the disk-instability starburst. */
  test_phase_add("galaxy_physics", "sage_apply_metal_enrichment", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage_starburst_feedback", PROCESSING_MODE_BY_GALAXY);

  TEST_ASSERT(sage_apply_metal_enrichment_init() == -1,
              "init should reject starburst ordered after the yield");

  test_free_substep_phases();
  check_memory_leaks();
  return TEST_PASS;
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
  const double frac_z_leave = TEST_FRAC_Z_LEAVE_DISK * exp(-30.0 / SAGE_METAL_EJECTION_MVIR_SCALE);
  const double expected_cold = TEST_YIELD * (1.0 - frac_z_leave) * stars;
  const double expected_hot = TEST_YIELD * frac_z_leave * stars;

  int result = sage_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, expected_cold, 1e-6),
              "disk metals should receive the retained yield");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, expected_hot, 1e-6),
              "central hot metals should receive the ejected yield");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_physics_cold_gas_below_threshold
 * @brief   At/below the cold-gas threshold the full yield is ejected to the central hot halo.
 */
int test_physics_cold_gas_below_threshold(void) {
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
  sat_gal.ColdGas = 0.0; /* at/below SAGE_COLD_GAS_YIELD_THRESHOLD */
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  const double expected_hot = TEST_YIELD * 2.0;

  int result = sage_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(result == 0, "process should succeed");
  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->MetalsColdGas, 0.0, 1e-9),
              "disk metals should be untouched below the cold-gas threshold");
  TEST_ASSERT(FLOAT_EQ(halos[0].galaxy->MetalsHotGas, expected_hot, 1e-6),
              "central hot metals should receive the full yield");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_consumes_new_stellar_mass
 * @brief   NewStellarMass is the per-substep transport field and must be zeroed after use.
 */
int test_consumes_new_stellar_mass(void) {
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
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  struct Halo halos[2] = {central, satellite};
  ctx.central_galaxy = &halos[0];

  sage_apply_metal_enrichment_process(&ctx, &halos[1], 1);

  TEST_ASSERT(FLOAT_EQ(halos[1].galaxy->NewStellarMass, 0.0, 1e-12),
              "NewStellarMass should be consumed (zeroed)");
  check_memory_leaks();
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
 * @test    test_null_galaxy
 * @brief   A halo with no galaxy is a no-op.
 */
int test_null_galaxy(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  sage_apply_metal_enrichment_init();

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData cen_gal;
  memset(&cen_gal, 0, sizeof(cen_gal));
  struct Halo central = create_test_halo(0, 30.0, &cen_gal);
  struct Halo galaxyless = create_test_halo(1, 10.0, NULL);

  ctx.central_galaxy = &central;
  int result = sage_apply_metal_enrichment_process(&ctx, &galaxyless, 1);

  TEST_ASSERT(result == 0, "NULL galaxy should be a graceful no-op");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_null_central
 * @brief   A missing FOF central (no ejection destination) is a no-op.
 */
int test_null_central(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  sage_apply_metal_enrichment_init();

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData sat_gal;
  memset(&sat_gal, 0, sizeof(sat_gal));
  sat_gal.ColdGas = 5.0;
  sat_gal.NewStellarMass = 2.0;
  struct Halo satellite = create_test_halo(1, 10.0, &sat_gal);

  ctx.central_galaxy = NULL;
  int result = sage_apply_metal_enrichment_process(&ctx, &satellite, 1);

  TEST_ASSERT(result == 0, "missing central should be a graceful no-op");
  TEST_ASSERT(FLOAT_EQ(satellite.galaxy->MetalsColdGas, 0.0, 1e-9),
              "no metals applied without an ejection destination");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_invalid_ngal
 * @brief   process_by_galaxy expects ngal=1; anything else is an error.
 */
int test_invalid_ngal(void) {
  reset_config();
  init_memory_system(0);
  set_test_model_parameters();
  sage_apply_metal_enrichment_init();

  struct ModuleContext ctx = create_test_context();
  struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  struct Halo halo = create_test_halo(0, 30.0, &gal);
  ctx.central_galaxy = &halo;

  TEST_ASSERT(sage_apply_metal_enrichment_process(&ctx, &halo, 2) == -1,
              "ngal != 1 should return an error");
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

  TEST_RUN(test_module_registration);
  TEST_RUN(test_init_valid);
  TEST_RUN(test_init_rejects_sf_after);
  TEST_RUN(test_init_rejects_starburst_after);
  TEST_RUN(test_physics_cold_gas_above_threshold);
  TEST_RUN(test_physics_cold_gas_below_threshold);
  TEST_RUN(test_consumes_new_stellar_mass);
  TEST_RUN(test_zero_new_stellar_mass);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_null_central);
  TEST_RUN(test_invalid_ngal);

  TEST_SUMMARY();
  return TEST_RESULT();
}
