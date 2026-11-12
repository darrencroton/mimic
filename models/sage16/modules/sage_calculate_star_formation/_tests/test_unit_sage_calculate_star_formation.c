/**
 * @file    test_unit_sage_calculate_star_formation.c
 * @brief   Unit tests for sage_calculate_star_formation module
 *
 * Validates: the Croton star-formation law (critical-mass threshold) and the module's guards.
 *
 * Test cases:
 *   - test_sf_calculation_above_threshold: the law against a hand-coded formula at two
 *     efficiencies (exact values, so the efficiency scaling is pinned)
 *   - test_sf_calculation_below_threshold: no stars below the critical mass
 *   - test_sf_calculation_at_threshold: the comparison at the critical mass is strict (just below
 *     and at it form no stars, one float step above does), on a threshold the float represents
 *     exactly
 *   - test_sf_calculation_zero_radius: zero DiskScaleRadius forms no stars
 *   - test_sf_calculation_zero_velocity: zero Vvir forms no stars
 *   - test_module_initialization: the module system initialises the SF and apply pipeline
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double SFR_EFFICIENCY;
extern double STAR_FORMING_DISK_FACTOR;

/* Stable string storage for pipeline config used by physics tests that call
 * sage_calculate_star_formation_init() directly.  The SF dependency check requires the
 * apply step to be visible in MimicConfig; static storage avoids the memory
 * system requirement. */
static char sf_pipeline_name0[] = "sage_calculate_star_formation";
static char sf_pipeline_name1[] = "sage_apply_star_formation_supernova";
static char sf_phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig sf_physics_pipeline[2];
static struct ModulePhaseConfig sf_substep_phase;

/* Module functions (extern declarations for direct testing) */
extern int sage_calculate_star_formation_init(void);
extern int sage_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage_calculate_star_formation_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Setup test galaxy with specified properties
 *
 * @param   halo        Halo structure to initialize
 * @param   gal         Galaxy structure to initialize
 * @param   cold_gas    Cold gas mass [1e10 Msun/h]
 * @param   disk_radius Disk scale radius [Mpc/h]
 * @param   vvir        Virial velocity [km/s]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal, double cold_gas,
                              double disk_radius, double vvir) {
  memset(halo, 0, sizeof(struct Halo));
  memset(gal, 0, sizeof(struct GalaxyData));

  halo->Type = 0;     /* Central galaxy */
  halo->Mvir = 100.0; /* 10^12 Msun/h */
  halo->Vvir = (float)vvir;
  halo->SnapNum = 63;
  halo->dT = 0.01; /* Default substep dt for tests (num_substeps=1) */
  halo->galaxy = gal;

  gal->ColdGas = (float)cold_gas;
  gal->DiskScaleRadius = (float)disk_radius;
  gal->NewStellarMass = 0.0; /* Should be calculated by module */
}

/**
 * @brief   Setup test parameters
 *
 * @param   efficiency  Star formation efficiency
 * @param   disk_factor Star forming disk factor
 */
static void setup_test_parameters(double efficiency, double disk_factor) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "SfrEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", efficiency);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "StarFormingDiskFactor");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", disk_factor);

  MimicConfig.NumModelParams = idx;

  /* The SF dependency check requires sage_apply_star_formation_supernova to be
   * visible in MimicConfig when sage_calculate_star_formation_init() is called directly. */
  sf_physics_pipeline[0].module_name = sf_pipeline_name0;
  sf_physics_pipeline[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  sf_physics_pipeline[1].module_name = sf_pipeline_name1;
  sf_physics_pipeline[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
  sf_substep_phase.name = sf_phase_name;
  sf_substep_phase.modules = sf_physics_pipeline;
  sf_substep_phase.num_modules = 2;
  MimicConfig.substep_phases = &sf_substep_phase;
  MimicConfig.num_substep_phases = 1;
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
 * @test    test_sf_calculation_above_threshold
 * @brief   The Croton law above the critical cold mass at two efficiencies
 *
 * Physics: strdot = SfrEfficiency * (ColdGas - cold_crit) / tdyn with cold_crit = 0.19 * Vvir *
 * reff (Kauffmann 1996) and tdyn = reff / Vvir, stars = strdot * dt. The hand-coded expectation
 * uses literal constants; two efficiencies pin the linear scaling in SfrEfficiency.
 */
int test_sf_calculation_above_threshold(void) {
  static const double efficiencies[] = {0.02, 0.05};
  for (size_t i = 0; i < sizeof(efficiencies) / sizeof(efficiencies[0]); i++) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    reset_config();

    const double efficiency = efficiencies[i];
    const double disk_factor = 3.0;
    setup_test_parameters(efficiency, disk_factor);

    int result = sage_calculate_star_formation_init();
    TEST_ASSERT(result == 0, "Module init should succeed");

    /* Create test galaxy with ColdGas above threshold */
    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 10.0;    /* 10^11 Msun/h */
    const double disk_radius = 0.05; /* 50 kpc/h */
    const double vvir = 200.0;       /* 200 km/s */
    setup_test_galaxy(&halo, &gal, cold_gas, disk_radius, vvir);

    /* Setup context */
    struct ModuleContext ctx;
    const double dt = 0.01; /* 10 Myr/h */
    setup_test_context(&ctx, dt);

    /* Calculate expected values (reproduce module logic) */
    const double reff = disk_factor * disk_radius;
    const double tdyn = reff / vvir;
    const double cold_crit = 0.19 * vvir * reff;
    const double expected_strdot =
        (cold_gas > cold_crit && tdyn > 0.0) ? efficiency * (cold_gas - cold_crit) / tdyn : 0.0;
    const double expected_stars = expected_strdot * dt;

    /* ===== EXECUTE ===== */
    result = sage_calculate_star_formation_process(&ctx, &halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Process function should succeed");
    TEST_ASSERT(gal.ColdGas > cold_crit, "ColdGas should be above threshold");
    TEST_ASSERT(gal.NewStellarMass > 0.0, "NewStellarMass should be positive");
    TEST_ASSERT_DOUBLE_EQUAL(gal.NewStellarMass, expected_stars, 1e-6,
                             "NewStellarMass should match expected value");

    /* ===== CLEANUP ===== */
    sage_calculate_star_formation_cleanup();
    check_memory_leaks();
  }

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_below_threshold
 * @brief   Test star formation when ColdGas is below critical threshold
 *
 * Expected: NewStellarMass = 0 (no star formation)
 * Validates: Critical threshold logic
 */
int test_sf_calculation_below_threshold(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  sage_calculate_star_formation_init();

  /* Create test galaxy with ColdGas below threshold */
  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 0.1;     /* Very low gas */
  const double disk_radius = 0.05; /* 50 kpc/h */
  const double vvir = 200.0;       /* 200 km/s */
  setup_test_galaxy(&halo, &gal, cold_gas, disk_radius, vvir);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* Calculate threshold */
  const double reff = disk_factor * disk_radius;
  const double cold_crit = 0.19 * vvir * reff;

  /* ===== EXECUTE ===== */
  sage_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(gal.ColdGas < cold_crit, "ColdGas should be below threshold");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero below threshold");

  /* ===== CLEANUP ===== */
  sage_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_at_threshold
 * @brief   The comparison at the critical cold mass is strict: ColdGas > cold_crit forms stars
 *
 * Vvir 100 km/s, DiskScaleRadius 0.125 and StarFormingDiskFactor 3 give reff = 0.375 and, since
 * 0.19 * 100 is exactly 19 in double, cold_crit = 7.125 exactly: a threshold the float ColdGas
 * represents. A galaxy one float step below it and one exactly at it form no stars; one float
 * step above forms the Croton law's stars for that excess, so a `>=` comparison fails the "at"
 * row and a `>` that drops the excess fails the "above" row.
 */
int test_sf_calculation_at_threshold(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  TEST_ASSERT(sage_calculate_star_formation_init() == 0, "Module init should succeed");

  const double vvir = 100.0;
  const double disk_radius = 0.125;
  const double dt = 0.01;
  const double reff = disk_factor * disk_radius;
  const float cold_crit = 7.125f;
  TEST_ASSERT(0.19 * vvir * reff == (double)cold_crit,
              "the threshold is exactly representable as a float");

  const struct {
    const char *where;
    float cold_gas;
  } rows[] = {
      {"one float step below", nextafterf(cold_crit, 0.0f)},
      {"at", cold_crit},
      {"one float step above", nextafterf(cold_crit, 100.0f)},
  };
  const double excess = (double)rows[2].cold_gas - (double)cold_crit;
  TEST_ASSERT(excess > 0.0 && (double)rows[0].cold_gas < (double)cold_crit,
              "the rows straddle the threshold");
  const double expected_above = efficiency * excess / (reff / vvir) * dt;

  /* ===== EXECUTE AND VALIDATE ===== */
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, rows[i].cold_gas, disk_radius, vvir);
    struct ModuleContext ctx;
    setup_test_context(&ctx, dt);

    TEST_ASSERT(sage_calculate_star_formation_process(&ctx, &halo, 1) == 0,
                "Process function should succeed");
    printf("\n  %s: NewStellarMass %.17g", rows[i].where, gal.NewStellarMass);
    if (i < 2) {
      TEST_ASSERT(gal.NewStellarMass == 0.0, "No stars form at or below the critical mass");
    } else {
      TEST_ASSERT(fabs(gal.NewStellarMass - expected_above) <= 1e-9 * expected_above,
                  "One float step above the critical mass forms stars for that excess");
    }
  }

  /* ===== CLEANUP ===== */
  sage_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_zero_radius
 * @brief   Test star formation when DiskScaleRadius is zero
 *
 * Expected: NewStellarMass = 0 (the tdyn > 0 guard)
 * Validates: Division by zero protection
 */
int test_sf_calculation_zero_radius(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  sage_calculate_star_formation_init();

  /* Create galaxy with zero disk radius */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 10.0, 0.0, 200.0); /* radius = 0 */

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle zero radius gracefully");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero with zero radius");

  /* ===== CLEANUP ===== */
  sage_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_zero_velocity
 * @brief   Test star formation when Vvir is zero
 *
 * Expected: NewStellarMass = 0: the dynamical time reff / Vvir is +inf, so the star formation
 * rate is exactly 0 through IEEE division, not NaN
 * Validates: Zero velocity gives zero stars
 */
int test_sf_calculation_zero_velocity(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  sage_calculate_star_formation_init();

  /* Create galaxy with zero virial velocity */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 10.0, 0.05, 0.0); /* vvir = 0 */

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle zero velocity gracefully");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero with zero velocity");

  /* ===== CLEANUP ===== */
  sage_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// MODULE INFRASTRUCTURE TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   The module system initialises the SF and apply pipeline, and cleans it up without leaks
 *
 * The apply step is initialised with it (the SF module requires it to follow).
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  /* SF requires sage_apply_star_formation_supernova in the pipeline */
  test_phase_add("galaxy_physics", "sage_calculate_star_formation", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters — use full set since apply step is also initialized */
  set_test_model_parameters();

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
 * Executes all sage_calculate_star_formation unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_calculate_star_formation Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Run physics calculation tests */
  TEST_RUN(test_sf_calculation_above_threshold);
  TEST_RUN(test_sf_calculation_below_threshold);
  TEST_RUN(test_sf_calculation_at_threshold);
  TEST_RUN(test_sf_calculation_zero_radius);
  TEST_RUN(test_sf_calculation_zero_velocity);

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
