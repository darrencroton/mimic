/**
 * @file    test_unit_sage_calculate_supernova_feedback.c
 * @brief   Unit tests for sage_calculate_supernova_feedback module
 *
 * Validates: Physics calculations, renormalization, edge cases, parameter handling
 *
 * Software quality tests:
 * - test_module_initialization: the module system initialises the SN calculator and the apply
 *   step
 *
 * Physics tests:
 * - test_fixed_epsilon_budgets: reheating (epsilon * stars), no renormalisation with ample cold
 *   gas, and ejection from the FoF central's Vvir for a standard galaxy, epsilon 0, no stars, a
 *   very high central Vvir (negative ejection floored at 0) and a central Vvir of 0 (ejection
 *   skipped, reheating still applied)
 * - test_renormalization_triggered: stars and reheating scaled together when they exceed ColdGas
 * - test_ejection_calculation: a satellite ejects according to the central's Vvir, not its own
 * - test_null_galaxy_pointer: a NULL galaxy is returned from early
 *
 * Reference: Croton et al. (2006, 2016) - SAGE model
 *
 * @author  Mimic Development Team
 * @date    2025-12-18 (Refactored for comprehensive physics validation)
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
#include "module_system/physical_constants.h"

#include <stdio.h>
#include <string.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (set by setup helpers) */
static double test_epsilon = 3.0; /* FeedbackReheatingEpsilon */
static double test_eta = 0.3;     /* FeedbackEjectionEfficiency */

/* Minimal pipeline config for physics tests that call sage_calculate_supernova_feedback_init()
 * directly.  The dependency check requires the apply step to be visible in
 * MimicConfig; we use static storage to avoid needing the memory system. */
static char sn_pipeline_name0[] = "sage_calculate_supernova_feedback";
static char sn_pipeline_name1[] = "sage_apply_star_formation_supernova";
static char sn_phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig sn_physics_pipeline[2];
static struct ModulePhaseConfig sn_substep_phase;

/* Physics constants (computed during init) */
static double energy_sn_code_test;
static double eta_sn_code_test;

// ============================================================================
// TEST FIXTURES AND HELPERS
// ============================================================================

/**
 * @brief   Setup module context for testing
 *
 * Creates a minimal ModuleContext with required fields for supernova feedback.
 *
 * @param   ctx             ModuleContext to initialize
 * @param   central         Pointer to central galaxy halo
 * @param   substep_dt      Time step for substep
 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central,
                                 double substep_dt) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->central_galaxy = central;
  ctx->substep_dt = substep_dt;
  ctx->params = &MimicConfig;
  ctx->redshift = 0.0;
  ctx->time = 13.8; /* Gyr */
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
}

/**
 * @brief   Setup test halo and galaxy with specified properties
 *
 * @param   halo            Halo to initialize
 * @param   galaxy          Galaxy to initialize
 * @param   type            Halo type (0=central, 1=satellite)
 * @param   mvir            Virial mass (1e10 Msun/h)
 * @param   vvir            Virial velocity (km/s)
 * @param   cold_gas        Cold gas mass (1e10 Msun/h)
 * @param   new_stars       Stellar mass formed this timestep (1e10 Msun/h)
 */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir,
                            double vvir, double cold_gas, double new_stars) {
  memset(halo, 0, sizeof(struct Halo));
  memset(galaxy, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Mvir = mvir;
  halo->Vvir = vvir;
  halo->SnapNum = 63;
  halo->galaxy = galaxy;

  galaxy->ColdGas = cold_gas;
  galaxy->NewStellarMass = new_stars;
  galaxy->SupernovaReheatedMass = 0.0;
  galaxy->SupernovaEjectedMass = 0.0;
}

/**
 * @brief   Initialize test physics constants
 *
 * Computes the SN code-unit constants the same way the module does.
 * Also sets global unit conversion variables required by the module.
 */
static void init_test_constants(void) {
  /* Set minimal config for unit conversion */
  MimicConfig.Hubble_h = 0.73;

  /* CRITICAL: Initialize global unit conversion variables */
  /* These are required by the module init function */
  MimicConfig.UnitLength_in_cm = 3.08568e24;    /* 1 Mpc in cm */
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5; /* 1 km/s in cm/s */
  MimicConfig.UnitMass_in_g = 1.989e43;         /* 1e10 Msun in g */

  /* Derived units */
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);

  /* Unit conversions (from module initialization) */
  energy_sn_code_test = ENERGY_SN / MimicConfig.UnitEnergy_in_cgs * MimicConfig.Hubble_h;
  eta_sn_code_test = ETA_SN * (MimicConfig.UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;

  /* Minimal pipeline config: the SN dependency check requires the apply step
   * to be visible in MimicConfig even when init is called directly. */
  sn_physics_pipeline[0].module_name = sn_pipeline_name0;
  sn_physics_pipeline[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  sn_physics_pipeline[1].module_name = sn_pipeline_name1;
  sn_physics_pipeline[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
  sn_substep_phase.name = sn_phase_name;
  sn_substep_phase.modules = sn_physics_pipeline;
  sn_substep_phase.num_modules = 2;
  MimicConfig.substep_phases = &sn_substep_phase;
  MimicConfig.num_substep_phases = 1;
}

/**
 * @brief   Call module init function directly
 *
 * @return  0 on success, non-zero on failure
 */
extern int sage_calculate_supernova_feedback_init(void);

/**
 * @brief   Call module process function directly (bypassing module system)
 *
 * @param   ctx             Module context
 * @param   halos           Array of halos (ngal=1 for by_galaxy mode)
 * @param   ngal            Number of galaxies (must be 1)
 * @return  0 on success, non-zero on failure
 */
extern int sage_calculate_supernova_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                                     int ngal);

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   The module system initialises the SN calculator and the apply step, and cleans up
 *          without leaks
 *
 * The apply step is initialised with the calculator (it requires the calculator to precede it).
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set up minimal cosmology configuration */
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  /* Configure sage_calculate_supernova_feedback module in galaxy_physics */
  /* SN requires sage_apply_star_formation_supernova in the pipeline */
  test_phase_add("galaxy_physics", "sage_calculate_supernova_feedback", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
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
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @brief   Overwrite the value of a model parameter set by set_test_model_parameters()
 */
static void set_test_parameter_value(const char *name, double value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, name) == 0) {
      snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%.17g", value);
      return;
    }
  }
}

/**
 * @brief   Initialise the module with the given FeedbackReheatingEpsilon and
 *          FeedbackEjectionEfficiency
 *
 * @return  The module's init result
 */
static int init_feedback_module(double epsilon, double eta) {
  init_test_constants();
  set_test_model_parameters();
  set_test_parameter_value("FeedbackReheatingEpsilon", epsilon);
  set_test_parameter_value("FeedbackEjectionEfficiency", eta);
  return sage_calculate_supernova_feedback_init();
}

/**
 * @brief   SAGE's ejected mass, written out
 *
 * ejected = (eta * (EtaSNcode * EnergySNcode) / Vvir^2 - epsilon) * stars, floored at 0, where
 * Vvir is the FoF central's virial velocity (the potential every member ejects into), and a
 * non-positive central Vvir ejects nothing.
 *
 * @param   epsilon         FeedbackReheatingEpsilon
 * @param   eta             FeedbackEjectionEfficiency
 * @param   central_vvir    The FoF central's Vvir [km/s]
 * @param   stars           Stars formed [1e10 Msun/h]
 * @return  The expected ejected mass [1e10 Msun/h]
 */
static double expected_ejection(double epsilon, double eta, double central_vvir, double stars) {
  if (!(central_vvir > 0.0)) {
    return 0.0;
  }
  const double ejected =
      (eta * (eta_sn_code_test * energy_sn_code_test) / (central_vvir * central_vvir) - epsilon) *
      stars;
  return ejected < 0.0 ? 0.0 : ejected;
}

/**
 * @test    test_fixed_epsilon_budgets
 * @brief   With ample cold gas: reheated mass is epsilon * stars, stars are not renormalised, and
 *          ejected mass follows the central-Vvir formula (floored at 0)
 *
 * Rows: a standard galaxy, epsilon 0 (no reheating, ejection of the full energy term), no stars
 * (nothing reheated or ejected), a very high central Vvir (the energy term falls below epsilon,
 * so the negative ejection is floored at 0), and a central Vvir of 0 (reheating still occurs,
 * ejection is skipped). `ejects` states whether the row's expected ejection is positive, so a row
 * cannot drift into vacuity.
 */
int test_fixed_epsilon_budgets(void) {
  static const struct {
    const char *name;
    double epsilon;
    double eta;
    double central_vvir;
    double stars;
    int ejects;
  } rows[] = {
      {"standard galaxy", 3.0, 0.3, 150.0, 1.0, 1},
      {"epsilon 0", 0.0, 0.3, 150.0, 1.0, 1},
      {"no stars", 3.0, 0.3, 150.0, 0.0, 0},
      {"negative ejection floored", 3.0, 0.3, 1000.0, 1.0, 0},
      {"central Vvir 0", 3.0, 0.3, 0.0, 1.0, 0},
  };
  const double cold_gas = 100.0; /* far more than stars + reheating needs */

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    printf("\n  row: %s", rows[i].name);
    TEST_ASSERT(init_feedback_module(rows[i].epsilon, rows[i].eta) == 0,
                "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, rows[i].central_vvir, cold_gas, 0.0);
    /* A satellite whose own Vvir (100 km/s) the ejection must not use */
    setup_test_halo(&test_halo, &test_galaxy, 1, 10.0, 100.0, cold_gas, rows[i].stars);
    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    TEST_ASSERT(sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1) == 0,
                "Module process should succeed");

    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, rows[i].epsilon * rows[i].stars,
                             1e-12, "Reheated mass should equal epsilon * stars");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, rows[i].stars, 1e-12,
                             "NewStellarMass should not change when gas is sufficient");
    const double expected_ejected =
        expected_ejection(rows[i].epsilon, rows[i].eta, rows[i].central_vvir, rows[i].stars);
    TEST_ASSERT(rows[i].ejects ? expected_ejected > 0.0 : expected_ejected == 0.0,
                "the row is as constructed");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, expected_ejected, 1e-10,
                             "Ejected mass should match the central-Vvir formula");
  }
  return TEST_PASS;
}

/**
 * @test    test_renormalization_triggered
 * @brief   Test renormalization when stars + reheating > ColdGas
 *
 * Physics: When (stars + reheated) > ColdGas, both are scaled by ColdGas / (stars + reheated)
 *
 * Expected: Both stars and reheating scaled down proportionally
 * Validates: Renormalization logic preserves mass conservation
 */
int test_renormalization_triggered(void) {
  /* ===== SETUP ===== */
  init_test_constants();
  set_test_model_parameters();

  int init_result = sage_calculate_supernova_feedback_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo;
  struct GalaxyData test_galaxy;
  struct Halo central_halo;
  struct GalaxyData central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

  /* Setup: Not enough cold gas for stars + reheating */
  double new_stars = 1.0; /* 1e10 Msun/h */
  double cold_gas = 3.0;  /* 3e10 Msun/h */
  /* Without renorm: reheated = 3.0 * 1.0 = 3.0, total = 4.0 > 3.0 (cold_gas) */
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 150.0, cold_gas, new_stars);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Calculate expected scaling */
  double initial_reheated = test_epsilon * new_stars;  /* 3.0 */
  double initial_total = new_stars + initial_reheated; /* 4.0 */
  double scale_factor = cold_gas / initial_total;      /* 3.0 / 4.0 = 0.75 */

  double expected_stars = new_stars * scale_factor;           /* 1.0 * 0.75 = 0.75 */
  double expected_reheated = initial_reheated * scale_factor; /* 3.0 * 0.75 = 2.25 */

  /* Validate renormalization */
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, expected_stars, 1e-6,
                           "NewStellarMass should be renormalized");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, expected_reheated, 1e-6,
                           "Reheated mass should be renormalized");

  /* Validate mass conservation: stars + reheated should not exceed cold gas */
  double total_used = test_galaxy.NewStellarMass + test_galaxy.SupernovaReheatedMass;
  TEST_ASSERT(total_used <= cold_gas + 1e-6, "Stars + reheating should not exceed cold gas");

  return TEST_PASS;
}

/**
 * @test    test_ejection_calculation
 * @brief   Ejection mass of a satellite is set by the central's Vvir, not its own
 *
 * Physics: ejected = (eta * EnergySN / Vvir^2 - epsilon) * stars, with the FoF central's Vvir
 * (100 km/s: an energy term of 7.5 against epsilon 3, so 4.5 stars-worth ejected). The
 * satellite's own Vvir (200 km/s) would give an energy term of 1.9, below epsilon, and no
 * ejection at all, so using the wrong Vvir fails.
 */
int test_ejection_calculation(void) {
  /* ===== SETUP ===== */
  init_test_constants();
  set_test_model_parameters();

  int init_result = sage_calculate_supernova_feedback_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo;
  struct GalaxyData test_galaxy;
  struct Halo central_halo;
  struct GalaxyData central_galaxy;

  /* Setup central with the Vvir the ejection must use */
  const double central_vvir = 100.0; /* km/s */
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, central_vvir, 100.0, 0.0);

  /* Setup test galaxy: a satellite with a deeper potential of its own */
  const double new_stars = 1.0;
  const double own_vvir = 200.0; /* km/s */
  setup_test_halo(&test_halo, &test_galaxy, 1, 10.0, own_vvir, 100.0, new_stars);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  const double expected_ejected =
      expected_ejection(test_epsilon, test_eta, central_vvir, new_stars);
  TEST_ASSERT(expected_ejected > 0.0, "the central's potential ejects gas");
  TEST_ASSERT(expected_ejection(test_epsilon, test_eta, own_vvir, new_stars) == 0.0,
              "the satellite's own potential would eject none");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, expected_ejected, 1e-10,
                           "Ejected mass should match the central-Vvir formula");

  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_null_galaxy_pointer
 * @brief   Test edge case: galaxy pointer is NULL
 *
 * Expected: Module returns early without crash
 * Validates: NULL pointer handling
 */
int test_null_galaxy_pointer(void) {
  /* ===== SETUP ===== */
  init_test_constants();

  struct Halo test_halo;
  struct Halo central_halo;
  struct GalaxyData central_galaxy;

  memset(&test_halo, 0, sizeof(test_halo));
  test_halo.galaxy = NULL; /* NULL galaxy */

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 150.0, 100.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should handle NULL galaxy gracefully");

  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage_calculate_supernova_feedback tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_calculate_supernova_feedback Module\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Software quality tests */
  printf("\n%s--- Software Quality Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_module_initialization);

  /* Physics calculation tests */
  printf("\n%s--- Physics Calculation Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_fixed_epsilon_budgets);
  TEST_RUN(test_renormalization_triggered);
  TEST_RUN(test_ejection_calculation);

  /* Edge case tests */
  printf("\n%s--- Edge Case Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_null_galaxy_pointer);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
