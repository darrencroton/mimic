/**
 * @file    test_unit_sage_set_disk_scale_radius.c
 * @brief   Unit tests for sage_set_disk_scale_radius module
 *
 * Validates: the disk radius formula and its guards, the Type 0 filter, and module placement
 *
 * The module sets DiskScaleRadius = (lambda / 1.414) * Rvir with the Bullock spin parameter
 * lambda = |J| / (1.414 * Vvir * Rvir) (Mo, Mao & White 1998), which reduces to
 * |J| / (1.414^2 * Vvir). Expected values are hand-computed literals of that closed form, not a
 * copy of the module's arithmetic. SAGE parity: SAGE uses the truncated literal 1.414, so a
 * full-precision sqrt(2) changes every row by 3e-4, which the 1e-6 relative tolerance (the float
 * store's precision) detects.
 *
 * Test cases:
 *   - test_disk_radius_formula: formula rows (normal, high and low spin, zero spin) and the
 *     Vvir = 0, Rvir = 0 and below-EPSILON_SMALL Vvir and Rvir guards
 *   - test_disk_radius_type_filter: Types 1, 2 and 3 keep their radius; in a mixed array only the
 *     Type 0 halo is processed
 *   - test_disk_radius_null_galaxy: a halo without a galaxy is skipped without ending the loop
 *   - test_module_initialization: module_system_init() accepts the module in pre_timestep
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "include/constants.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/** @brief Relative tolerance on DiskScaleRadius: the float store is good to 6e-8 */
#define RADIUS_RTOL 1e-6

/* Module functions (extern declarations for direct testing) */
extern int sage_set_disk_scale_radius_init(void);
extern int sage_set_disk_scale_radius_process(struct ModuleContext *ctx, struct Halo *halos,
                                              int ngal);
extern int sage_set_disk_scale_radius_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Setup test galaxy with specified properties
 *
 * @param   halo        Halo structure to initialize
 * @param   gal         Galaxy structure to initialize
 * @param   spin_x      Spin x-component [km/s * Mpc/h]
 * @param   spin_y      Spin y-component [km/s * Mpc/h]
 * @param   spin_z      Spin z-component [km/s * Mpc/h]
 * @param   vvir        Virial velocity [km/s]
 * @param   rvir        Virial radius [Mpc/h]
 * @param   type        Halo type (0=central, 1=satellite, 2=orphan)
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal, float spin_x, float spin_y,
                              float spin_z, float vvir, float rvir, int type) {
  memset(halo, 0, sizeof(struct Halo));
  memset(gal, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Spin[0] = spin_x;
  halo->Spin[1] = spin_y;
  halo->Spin[2] = spin_z;
  halo->Vvir = vvir;
  halo->Rvir = rvir;
  halo->Mvir = 100.0; /* 10^12 Msun/h */
  halo->SnapNum = 63;
  halo->galaxy = gal;

  gal->DiskScaleRadius = -1.0; /* Sentinel value to check if modified */
}

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx         Context to initialize
 * @param   dt          Time step
 */
static void setup_test_context(struct ModuleContext *ctx, double dt) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->substep_dt = dt;
  ctx->redshift = 0.0;
  ctx->time = 13.8;
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
  ctx->params = &MimicConfig;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_disk_radius_formula
 * @brief   DiskScaleRadius = |J| / (1.414^2 * Vvir) for Type 0, with the SAGE guards
 *
 * Expected: each row's literal. Vvir or Rvir at or below EPSILON_SMALL (1e-10) takes the
 *           fallback 0.1 * Rvir (zero for Rvir = 0), so a tiny positive Vvir and Rvir of 1e-12
 *           still fall back.
 * Validates: Core physics calculation, the 1.414 literal, and the fallback guards
 */
int test_disk_radius_formula(void) {
  static const struct {
    const char *label;
    float spin[3]; /* km/s * Mpc/h */
    float vvir;    /* km/s */
    float rvir;    /* Mpc/h */
    double expected;
  } rows[] = {
      {"normal halo", {100.0f, 150.0f, 200.0f}, 200.0f, 0.2f, 0.673348952275},
      {"high spin", {3.0f, 4.0f, 0.0f}, 200.0f, 0.2f, 0.0125037761404},
      {"low spin", {0.3f, 0.4f, 0.0f}, 200.0f, 0.2f, 0.00125037764385},
      {"zero spin", {0.0f, 0.0f, 0.0f}, 200.0f, 0.2f, 0.0},
      {"zero Vvir takes the 0.1 * Rvir fallback", {100.0f, 100.0f, 100.0f}, 0.0f, 0.2f, 0.02},
      {"zero Rvir gives zero", {100.0f, 100.0f, 100.0f}, 200.0f, 0.0f, 0.0},
      {"Vvir and Rvir below EPSILON_SMALL take the fallback",
       {100.0f, 100.0f, 100.0f},
       1e-12f,
       1e-12f,
       1e-13},
  };

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();
    TEST_ASSERT(sage_set_disk_scale_radius_init() == 0, "Module init should succeed");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, rows[i].spin[0], rows[i].spin[1], rows[i].spin[2], rows[i].vvir,
                      rows[i].rvir, 0);
    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage_set_disk_scale_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    char message[256];
    snprintf(message, sizeof(message), "Process should succeed (%s)", rows[i].label);
    TEST_ASSERT(result == 0, message);
    snprintf(message, sizeof(message), "DiskScaleRadius must match the expected value (%s)",
             rows[i].label);
    TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, rows[i].expected,
                             RADIUS_RTOL * rows[i].expected, message);

    /* ===== CLEANUP ===== */
    sage_set_disk_scale_radius_cleanup();
    check_memory_leaks();
  }

  return TEST_PASS;
}

/**
 * @test    test_disk_radius_type_filter
 * @brief   SAGE parity: only the FoF central (Type 0) is recomputed
 *
 * Expected: a Type 1, 2 or 3 halo keeps its inherited DiskScaleRadius (the -1 sentinel); in an
 *           array with a Type 0 halo between a satellite and an orphan only the Type 0 halo is
 *           processed
 * Validates: Type filtering, the Type 1 freeze contract and the loop indexing
 */
int test_disk_radius_type_filter(void) {
  static const struct {
    int type;
    const char *label;
  } rows[] = {{1, "Type 1 satellite"}, {2, "Type 2 orphan"}, {3, "Type 3"}};

  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();
  TEST_ASSERT(sage_set_disk_scale_radius_init() == 0, "Module init should succeed");
  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, rows[i].type);

    /* ===== EXECUTE ===== */
    int result = sage_set_disk_scale_radius_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    char message[256];
    snprintf(message, sizeof(message), "Process should succeed (%s)", rows[i].label);
    TEST_ASSERT(result == 0, message);
    snprintf(message, sizeof(message), "%s should keep its inherited DiskScaleRadius",
             rows[i].label);
    TEST_ASSERT(gal.DiskScaleRadius == -1.0f, message);
  }

  /* Mixed array: the Type 0 halo sits between a satellite and an orphan */
  struct Halo halos[3];
  struct GalaxyData gals[3];
  setup_test_galaxy(&halos[0], &gals[0], 300.0f, 400.0f, 0.0f, 200.0f, 0.2f, 1);
  setup_test_galaxy(&halos[1], &gals[1], 100.0f, 150.0f, 200.0f, 200.0f, 0.2f, 0);
  setup_test_galaxy(&halos[2], &gals[2], 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, 2);
  TEST_ASSERT(sage_set_disk_scale_radius_process(&ctx, halos, 3) == 0,
              "Process should succeed for the array");
  TEST_ASSERT(gals[0].DiskScaleRadius == -1.0f, "The Type 1 halo before the central is unchanged");
  TEST_ASSERT_DOUBLE_EQUAL((double)gals[1].DiskScaleRadius, 0.673348952275,
                           RADIUS_RTOL * 0.673348952275, "The Type 0 halo should be processed");
  TEST_ASSERT(gals[2].DiskScaleRadius == -1.0f, "The Type 2 halo after the central is unchanged");

  /* ===== CLEANUP ===== */
  sage_set_disk_scale_radius_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_disk_radius_null_galaxy
 * @brief   A halo without a galaxy is skipped and the rest of the array is still processed
 *
 * Expected: the galaxy-less Type 0 halo stays without a galaxy; the Type 0 halo after it receives
 *           its disk radius
 * Validates: Null pointer safety without ending the loop
 */
int test_disk_radius_null_galaxy(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();
  TEST_ASSERT(sage_set_disk_scale_radius_init() == 0, "Module init should succeed");

  struct Halo halos[2];
  struct GalaxyData gal;
  setup_test_galaxy(&halos[0], &gal, 100.0f, 100.0f, 100.0f, 200.0f, 0.2f, 0);
  halos[0].galaxy = NULL;
  setup_test_galaxy(&halos[1], &gal, 100.0f, 150.0f, 200.0f, 200.0f, 0.2f, 0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage_set_disk_scale_radius_process(&ctx, halos, 2);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle null galaxy gracefully");
  TEST_ASSERT(halos[0].galaxy == NULL, "Galaxy pointer is null");
  TEST_ASSERT_DOUBLE_EQUAL((double)gal.DiskScaleRadius, 0.673348952275,
                           RADIUS_RTOL * 0.673348952275,
                           "The halo after the galaxy-less one should still be processed");

  /* ===== CLEANUP ===== */
  sage_set_disk_scale_radius_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// MODULE INFRASTRUCTURE TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors
 * Validates: Module lifecycle management
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  MimicConfig.pre_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.pre_timestep[0].module_name = strdup("sage_set_disk_scale_radius");
  MimicConfig.pre_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.num_pre_timestep = 1;
  MimicConfig.SubSteps = 1;

  /* No parameters needed for this module */
  MimicConfig.NumModelParams = 0;

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_set_disk_scale_radius unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_set_disk_scale_radius Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_disk_radius_formula);
  TEST_RUN(test_disk_radius_type_filter);
  TEST_RUN(test_disk_radius_null_galaxy);
  TEST_RUN(test_module_initialization);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
