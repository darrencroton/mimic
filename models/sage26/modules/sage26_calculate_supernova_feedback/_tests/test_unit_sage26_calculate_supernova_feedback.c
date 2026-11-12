/**
 * @file    test_unit_sage26_calculate_supernova_feedback.c
 * @brief   Unit tests for sage26_calculate_supernova_feedback module
 *
 * Validates: Physics calculations, renormalization, edge cases, parameter handling
 *
 * Software quality tests:
 * - test_module_initialization: the module system initialises the SN calculator and the apply
 *   step, and cleans up without leaks
 *
 * Physics tests (FIREmodeOn 0, the fixed-epsilon feedback):
 * - test_fixed_epsilon_budgets: reheating (epsilon * stars), no renormalisation with ample cold
 *   gas, and ejection from the forming galaxy's own Vvir, as in SAGE26, for a standard galaxy,
 *   epsilon 0, no stars, a very high Vvir (negative ejection floored at 0) and an own Vvir of 0
 * - test_renormalization_triggered: stars and reheating scaled together when they exceed ColdGas
 * - test_ejection_calculation: a satellite's ejection from its own Vvir, squared in float, on a
 *   Vvir a float holds exactly and one it does not
 * - test_null_galaxy_pointer: a NULL galaxy is returned from early
 *
 * FIREmodeOn (its refused values are in test_unit_sage26_parameter_refusals.c). Each FIRE helper
 * and the module's FIREmodeOn 1 branch is asserted against an expectation derived without the
 * helpers: literal inputs whose results are exact, or SAGE26's own expressions written in the test
 * with its operand order and literal constants:
 * - test_fire_on_versus_off: the same galaxy with FIREmodeOn 0 and 1: 0 keeps MassLoading and the
 *   fixed-epsilon budgets; 1 writes MassLoading = (float)eta and reheats eta * stars
 * - test_fire_scaling_branches: the scaling's guards, floor and slopes
 * - test_fire_helpers_independent_expectations: each FIRE helper and the module's FIRE branch
 *   against the independently derived expectations
 * - test_fire_high_velocity_division: the high-velocity term is 1.0 / x as SAGE26 computes it,
 *   not libm pow(x, -1.0)
 * - test_fire_reheating_cap_and_mass_loading: the reheating cap above, below and without a
 *   positive velocity, and MassLoading written before the renormalisation, also with no stars
 * - test_fire_energy_ejection: the energy coupling's cap, the energy-based ejection, the
 *   narrowing of the double halo Vvir to SAGE26's float, and no ejection for Vvir <= 0
 * - test_fire_stored_snapshot_refused: a stored SnapNum of -1 or of the table size reaching the
 *   FIRE helper is an error naming the SnapNum and the table size, and writes nothing
 * - test_fire_initial_boundary_skipped: the initial-boundary state (stored SnapNum -1, dT 0) is
 *   skipped before the redshift lookup and writes nothing
 *
 * Feedback-free bursts:
 * - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central bytewise
 *   unchanged, with FIREmodeOn 0 and with FIREmodeOn 1 on a stored SnapNum the redshift lookup
 *   refuses (the bypass precedes the lookup and the time guard); the same row with FFBRegime 0
 *   gets SAGE26's fixed-epsilon reheating and the refusal with FIREmodeOn 1
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
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_sf_feedback_physics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (set by setup helpers) */
static double test_epsilon = 3.0; /* FeedbackReheatingEpsilon */
static double test_eta = 0.3;     /* FeedbackEjectionEfficiency */

/* Minimal pipeline config for physics tests that call sage26_calculate_supernova_feedback_init()
 * directly.  The dependency check requires the apply step to be visible in
 * MimicConfig; we use static storage to avoid needing the memory system. */
static char sn_pipeline_name0[] = "sage26_calculate_supernova_feedback";
static char sn_pipeline_name1[] = "sage26_apply_star_formation_supernova";
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
extern int sage26_calculate_supernova_feedback_init(void);

/**
 * @brief   Call module process function directly (bypassing module system)
 *
 * @param   ctx             Module context
 * @param   halos           Array of halos (ngal=1 for by_galaxy mode)
 * @param   ngal            Number of galaxies (must be 1)
 * @return  0 on success, non-zero on failure
 */
extern int sage26_calculate_supernova_feedback_process(struct ModuleContext *ctx,
                                                       struct Halo *halos, int ngal);

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   The module system initialises the SN calculator and the apply step, and cleans up
 *          without leaks
 *
 * The apply step is initialised with the calculator (it requires the calculator to precede it),
 * so this is also the unit tier's run of the apply step's init() in an SN-only pipeline.
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

  /* Configure sage26_calculate_supernova_feedback module in galaxy_physics */
  /* SN requires sage26_apply_star_formation_supernova in the pipeline */
  test_phase_add("galaxy_physics", "sage26_calculate_supernova_feedback",
                 PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  /* The module's required FIREmodeOn */
  sage26_test_append_model_parameter("FIREmodeOn", "0");
  /* sage26_apply_star_formation_supernova's required SFprescription */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_APPLY_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

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
 * @brief   Initialise the module at FIREmodeOn 0 with the given FeedbackReheatingEpsilon and
 *          FeedbackEjectionEfficiency
 *
 * @return  The module's init result
 */
static int init_fixed_epsilon_module(double epsilon, double eta) {
  init_test_constants();
  set_test_model_parameters();
  set_test_parameter_value("FeedbackReheatingEpsilon", epsilon);
  set_test_parameter_value("FeedbackEjectionEfficiency", eta);
  /* The module's required FIREmodeOn */
  sage26_test_append_model_parameter("FIREmodeOn", "0");
  return sage26_calculate_supernova_feedback_init();
}

/**
 * @brief   SAGE26's FIREmodeOn 0 ejected mass, written out
 *
 * ejected = (eta * (EtaSNcode * EnergySNcode) / Vvir^2 - epsilon) * stars, floored at 0, where
 * Vvir is the forming galaxy's own virial velocity narrowed to float and squared in float, and a
 * non-positive Vvir ejects nothing.
 *
 * @param   epsilon     FeedbackReheatingEpsilon
 * @param   eta         FeedbackEjectionEfficiency
 * @param   own_vvir    The forming galaxy's Vvir [km/s]
 * @param   stars       Stars formed [1e10 Msun/h]
 * @return  The expected ejected mass [1e10 Msun/h]
 */
static double expected_fixed_epsilon_ejection(double epsilon, double eta, double own_vvir,
                                              double stars) {
  const float vvir = (float)own_vvir;
  if (!(vvir > 0.0f)) {
    return 0.0;
  }
  const float vvir_sq = vvir * vvir;
  const double ejected =
      (eta * (eta_sn_code_test * energy_sn_code_test) / vvir_sq - epsilon) * stars;
  return ejected < 0.0 ? 0.0 : ejected;
}

/**
 * @test    test_fixed_epsilon_budgets
 * @brief   FIREmodeOn 0 with ample cold gas: reheated mass is epsilon * stars, stars are not
 *          renormalised, and ejected mass follows the own-Vvir formula (floored at 0)
 *
 * Rows: a standard galaxy, epsilon 0 (no reheating, ejection of the full energy term), no stars
 * (nothing reheated or ejected), a very high Vvir (the energy term falls below epsilon, so the
 * negative ejection is floored at 0), and an own Vvir of 0 (an orphan: reheating still occurs,
 * ejection is skipped whatever the central's Vvir). `ejects` states whether the row's expected
 * ejection is positive, so a row cannot drift into vacuity.
 */
int test_fixed_epsilon_budgets(void) {
  static const struct {
    const char *name;
    double epsilon;
    double eta;
    int type;
    double own_vvir;
    double stars;
    int ejects;
  } rows[] = {
      {"standard galaxy", 3.0, 0.3, 0, 150.0, 1.0, 1},
      {"epsilon 0", 0.0, 0.3, 0, 150.0, 1.0, 1},
      {"no stars", 3.0, 0.3, 0, 150.0, 0.0, 0},
      {"negative ejection floored", 3.0, 0.3, 0, 1000.0, 1.0, 0},
      {"own Vvir 0", 3.0, 0.3, 2, 0.0, 1.0, 0},
  };
  const double cold_gas = 100.0; /* far more than stars + reheating needs */
  const double central_vvir = 200.0;

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    printf("\n  row: %s", rows[i].name);
    TEST_ASSERT(init_fixed_epsilon_module(rows[i].epsilon, rows[i].eta) == 0,
                "Module initialization should succeed");

    struct Halo test_halo, central_halo;
    struct GalaxyData test_galaxy, central_galaxy;
    /* A central with a Vvir the ejection must not use */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, central_vvir, cold_gas, 0.0);
    setup_test_halo(&test_halo, &test_galaxy, rows[i].type, 100.0, rows[i].own_vvir, cold_gas,
                    rows[i].stars);
    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    TEST_ASSERT(sage26_calculate_supernova_feedback_process(&ctx, &test_halo, 1) == 0,
                "Module process should succeed");

    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, rows[i].epsilon * rows[i].stars,
                             1e-12, "Reheated mass should equal epsilon * stars");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, rows[i].stars, 1e-12,
                             "NewStellarMass should not change when gas is sufficient");
    const double expected_ejected = expected_fixed_epsilon_ejection(
        rows[i].epsilon, rows[i].eta, rows[i].own_vvir, rows[i].stars);
    TEST_ASSERT(rows[i].ejects ? expected_ejected > 0.0 : expected_ejected == 0.0,
                "the row is as constructed");
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, expected_ejected, 1e-10,
                             "Ejected mass should match the own-Vvir formula");
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
  /* The module's required FIREmodeOn */
  sage26_test_append_model_parameter("FIREmodeOn", "0");

  int init_result = sage26_calculate_supernova_feedback_init();
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
  int result = sage26_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

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
 * @brief   Ejection mass of a satellite from its own Vvir, squared in float as SAGE26 does
 *
 * Physics: ejected = (eta * EnergySN / Vvir^2 - epsilon) * stars, with the forming galaxy's own
 * Vvir narrowed to float and squared in float; the central's Vvir (200 km/s) is not used.
 *
 * Rows: a Vvir a float holds exactly (100 km/s), and one it does not (137.3 km/s), where the
 * float square differs from the double square by about 2e-7 in the ejected mass, far above the
 * 1e-10 tolerance, so squaring in double would fail.
 */
int test_ejection_calculation(void) {
  static const double own_vvirs[] = {100.0, 137.3};
  const double new_stars = 1.0;

  for (size_t i = 0; i < sizeof(own_vvirs) / sizeof(own_vvirs[0]); i++) {
    printf("\n  own Vvir %g", own_vvirs[i]);
    /* ===== SETUP ===== */
    TEST_ASSERT(init_fixed_epsilon_module(test_epsilon, test_eta) == 0,
                "Module initialization should succeed");

    struct Halo test_halo;
    struct GalaxyData test_galaxy;
    struct Halo central_halo;
    struct GalaxyData central_galaxy;

    /* Setup central with a Vvir the ejection must not use */
    setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 200.0, 100.0, 0.0);

    /* Setup test galaxy (a satellite) with its own Vvir */
    setup_test_halo(&test_halo, &test_galaxy, 1, 10.0, own_vvirs[i], 100.0, new_stars);

    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, 0.01);

    /* ===== EXECUTE ===== */
    int result = sage26_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

    /* ===== VALIDATE ===== */
    TEST_ASSERT(result == 0, "Module process should succeed");

    const double expected_ejected =
        expected_fixed_epsilon_ejection(test_epsilon, test_eta, own_vvirs[i], new_stars);
    TEST_ASSERT(expected_ejected > 0.0, "the satellite ejects gas");
    /* The same expression with the Vvir squared in double: the row must be able to tell them
     * apart where the float Vvir is not exact */
    const double v_float = (double)(float)own_vvirs[i];
    const double double_square =
        (test_eta * (eta_sn_code_test * energy_sn_code_test) / (v_float * v_float) - test_epsilon) *
        new_stars;
    if (own_vvirs[i] != v_float) {
      TEST_ASSERT(fabs(double_square - expected_ejected) > 1e-8,
                  "squaring in float differs measurably from squaring in double on this Vvir");
    }
    TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, expected_ejected, 1e-10,
                             "Ejected mass should match the formula");
  }

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
  int result = sage26_calculate_supernova_feedback_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should handle NULL galaxy gracefully");

  return TEST_PASS;
}

// ============================================================================
// FIREmodeOn TESTS (the FIRE scaling, capped loading and energy-based ejection)
// ============================================================================

/* SAGE parity: the high-velocity term is 1.0 / x, not libm pow(x, -1.0). This Vvir gives
 * x = (double)Vvir / 60 = 0x1.0000cp+0, where the two differ in the last bit; the expected bits
 * are those of 1.0 / x, SAGE26's value */
#define FIRE_TEST_VVIR_DIVISION_SEPARATOR 0x1.e00168p+5f
#define FIRE_TEST_V_TERM_DIVISION_BITS 0x3fefffe80011fff3ULL

/**
 * @brief Configure and initialise the module with FIREmodeOn `value`; give the run a 64-entry
 *        redshift table (ZZ[s] = (63 - s) / 10, so ZZ[63] = 0)
 * @return The init status
 */
static int fire_init(const char *value) {
  init_test_constants();
  set_test_model_parameters();
  sage26_test_append_model_parameter("FIREmodeOn", value);
  MimicConfig.Snaplistlen = 64;
  for (int s = 0; s < 64; s++) {
    MimicConfig.ZZ[s] = (63 - s) / 10.0;
  }
  return sage26_calculate_supernova_feedback_init();
}

/** @brief Run process() on one galaxy, capturing the log; returns its status */
static int fire_process_captured(struct Halo *halo, const char **log) {
  struct ModuleContext ctx;
  setup_module_context(&ctx, halo, 0.01);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_calculate_supernova_feedback_process(&ctx, halo, 1);
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/** @brief SAGE26's EtaSNcode and EnergySNcode, as the module derives them */
static double fire_eta_sn(void) {
  return mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);
}
static double fire_energy_sn(void) {
  return mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
}

/**
 * @test    test_fire_on_versus_off
 * @brief   The same galaxy with FIREmodeOn 0 and 1: 0 keeps MassLoading and the fixed-epsilon
 *          budgets; 1 writes MassLoading = (float)eta and reheats eta * stars
 */
int test_fire_on_versus_off(void) {
  const double stars = 0.01;
  const double cold_gas = 10.0;
  const float vvir = 80.0f;
  const int snap = 32; /* redshift ZZ[32] = 3.1 in this table */
  double reheated[2], ejected[2];
  float loading[2];
  for (int fire = 0; fire <= 1; fire++) {
    const char *log = "";
    TEST_ASSERT(fire_init(fire ? "1" : "0") == 0, "init must succeed");
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_halo(&halo, &gal, 1, 10.0, vvir, cold_gas, stars);
    halo.SnapNum = snap;
    gal.MassLoading = 7.5f; /* stale */
    TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
    reheated[fire] = gal.SupernovaReheatedMass;
    ejected[fire] = gal.SupernovaEjectedMass;
    loading[fire] = gal.MassLoading;
  }
  printf("\n  FIREmodeOn 0: reheated %.17g ejected %.17g MassLoading %.9g", reheated[0], ejected[0],
         (double)loading[0]);
  printf("\n  FIREmodeOn 1: reheated %.17g ejected %.17g MassLoading %.9g", reheated[1], ejected[1],
         (double)loading[1]);
  TEST_ASSERT(loading[0] == 7.5f, "FIREmodeOn 0 must leave MassLoading as it was");
  TEST_ASSERT(double_bits(reheated[0]) == double_bits(test_epsilon * stars),
              "FIREmodeOn 0 reheats FeedbackReheatingEpsilon * stars");

  /* FIREmodeOn 1, the expectation written as SAGE26's own expressions with literal constants (never
   * the package helpers): 80 km/s takes the slope -1.0, which SAGE26 evaluates as 1.0 / x;
   * redshift 3.1; 0.3 * scaling < 2 and the loading below its cap (checked below) */
  const double vc = 80.0;
  const double scaling = pow(1.0 + MimicConfig.ZZ[snap], 1.25) * (1.0 / (vc / 60.0));
  const double eta = test_epsilon * scaling;
  TEST_ASSERT(eta < 2.0 * (eta_sn_code_test * energy_sn_code_test) / (vc * vc),
              "the loading is below its cap on this input");
  const double coupling = test_eta * scaling; /* below 2 */
  TEST_ASSERT(coupling < 2.0, "the coupling is below its cap on this input");
  const double E_FB = coupling * 0.5 * stars * (eta_sn_code_test * energy_sn_code_test);
  const double E_lift = 0.5 * (eta * stars) * vc * vc;
  const double expect_ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;
  TEST_ASSERT(loading[1] == (float)eta, "FIREmodeOn 1 must write MassLoading = (float)eta");
  TEST_ASSERT(double_bits(reheated[1]) == double_bits(eta * stars),
              "FIREmodeOn 1 reheats eta * stars");
  TEST_ASSERT(double_bits(ejected[1]) == double_bits(expect_ejected),
              "FIREmodeOn 1 ejects the energy-based remainder");
  TEST_ASSERT(reheated[0] != reheated[1], "the two modes must differ on this input");
  return TEST_PASS;
}

/**
 * @test    test_fire_scaling_branches
 * @brief   The scaling's guards, floor and slopes
 */
int test_fire_scaling_branches(void) {
  struct MimicSageFireLocals a, b;
  /* Non-positive velocity or negative redshift: 0, not valid */
  mimic_sage_fire_locals_init(&a);
  TEST_ASSERT(mimic_sage_fire_scaling(0.0f, 1.0, &a) == 0.0 && a.scaling_valid == 0,
              "Vvir 0 gives scaling 0");
  TEST_ASSERT(mimic_sage_fire_scaling(-50.0f, 1.0, &a) == 0.0 && a.scaling_valid == 0,
              "a negative Vvir gives scaling 0");
  TEST_ASSERT(mimic_sage_fire_scaling(80.0f, -0.01, &a) == 0.0 && a.scaling_valid == 0,
              "a negative redshift gives scaling 0");
  /* -0.0 >= 0.0: valid, and pow(1.0 + -0.0, 1.25) is exactly 1 */
  mimic_sage_fire_locals_init(&a);
  const double s_neg_zero = mimic_sage_fire_scaling(80.0f, -0.0, &a);
  TEST_ASSERT(a.scaling_valid == 1 && double_bits(s_neg_zero) == double_bits(a.v_term),
              "redshift -0.0 is valid and scales by exactly 1");
  /* Redshift 0: the scaling is the velocity term */
  mimic_sage_fire_locals_init(&a);
  TEST_ASSERT(double_bits(mimic_sage_fire_scaling(80.0f, 0.0, &a)) == double_bits(a.v_term),
              "at redshift 0 the scaling is the velocity term");
  /* The 1 km/s floor: 0.5 km/s scales as 1 km/s */
  mimic_sage_fire_locals_init(&a);
  mimic_sage_fire_locals_init(&b);
  const double s_half = mimic_sage_fire_scaling(0.5f, 2.0, &a);
  const double s_one = mimic_sage_fire_scaling(1.0f, 2.0, &b);
  TEST_ASSERT(a.vc_floored == 1 && b.vc_floored == 0, "below 1 km/s is floored, 1 km/s is not");
  TEST_ASSERT(double_bits(s_half) == double_bits(s_one), "the floored velocity is 1 km/s");
  /* The slopes: -3.2 below 60 km/s, -1.0 at and above (60 is the high branch, term exactly 1) */
  mimic_sage_fire_locals_init(&a);
  mimic_sage_fire_scaling(nextafterf(60.0f, 0.0f), 0.0, &a);
  TEST_ASSERT(a.high_velocity == 0 && a.v_term > 1.0, "just below 60 km/s takes the slope -3.2");
  mimic_sage_fire_locals_init(&a);
  mimic_sage_fire_scaling(60.0f, 0.0, &a);
  TEST_ASSERT(a.high_velocity == 1 && a.v_term == 1.0, "60 km/s takes the slope -1.0, term 1");
  mimic_sage_fire_locals_init(&a);
  mimic_sage_fire_scaling(120.0f, 0.0, &a);
  TEST_ASSERT(a.high_velocity == 1 && a.v_term == 0.5, "120 km/s: (120 / 60)^-1 = 0.5 exactly");

  /* Independent expectations: literal results, or SAGE26's own expression written here with its
   * literal constants, never the helper */
  mimic_sage_fire_locals_init(&a);
  TEST_ASSERT(mimic_sage_fire_scaling(120.0f, 0.0, &a) == 0.5,
              "120 km/s at redshift 0: scaling (120 / 60)^-1 = 0.5 exactly");
  mimic_sage_fire_locals_init(&a);
  const double s_z15 = mimic_sage_fire_scaling(120.0f, 15.0, &a);
  TEST_ASSERT(double_bits(s_z15) == double_bits(pow(1.0 + 15.0, 1.25) * (1.0 / (120.0 / 60.0))),
              "redshift 15: SAGE26's pow(1 + z, 1.25) * v_term");
  printf("\n  scaling(120 km/s, z 15) = %.17g (16 = 32 * 0.5 where pow(16, 1.25) is exact)", s_z15);
  mimic_sage_fire_locals_init(&a);
  const double s_30 = mimic_sage_fire_scaling(30.0f, 0.0, &a);
  TEST_ASSERT(double_bits(s_30) == double_bits(pow(1.0 + 0.0, 1.25) * pow(30.0 / 60.0, -3.2)),
              "30 km/s: SAGE26's slope pow(v / 60, -3.2)");
  mimic_sage_fire_locals_init(&a);
  const double s_floor = mimic_sage_fire_scaling(0.5f, 0.0, &a);
  TEST_ASSERT(double_bits(s_floor) == double_bits(pow(1.0 + 0.0, 1.25) * pow(1.0 / 60.0, -3.2)),
              "0.5 km/s: the velocity floored at 1 km/s with the slope -3.2");
  return TEST_PASS;
}

/**
 * @test    test_fire_helpers_independent_expectations
 * @brief   Each FIRE helper and the module's FIRE branch against expectations derived without the
 *          helpers: literal inputs whose results are exact, or SAGE26's own expressions written
 *          here with its operand order
 */
int test_fire_helpers_independent_expectations(void) {
  struct MimicSageFireLocals l;
  /* capped_eta_reheat with EtaSNcode * EnergySNcode = 1000 at 10 km/s: eta_max = 2 * 1000 / 100 =
   * 20; 1000 is capped to 20, 5 is not, and vvir 0 returns eta_raw */
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_capped_eta_reheat(1000.0, 10.0, 1000.0, 1.0, &l) == 20.0 &&
                  l.eta_max == 20.0 && l.eta_cap_binds == 1,
              "the loading cap binds at 2 * esn / v^2 = 20");
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_capped_eta_reheat(5.0, 10.0, 1000.0, 1.0, &l) == 5.0 &&
                  l.eta_cap_binds == 0,
              "a loading below the cap is unchanged");
  /* sn_energy_coupling: 0.25 * 4 = 1 is below the cap, 0.25 * 40 = 10 is capped at 2 */
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_sn_energy_coupling(4.0, 0.25, &l) == 1.0 && l.coupling_capped == 0,
              "a coupling of 1 is not capped");
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_sn_energy_coupling(40.0, 0.25, &l) == 2.0 && l.coupling_capped == 1,
              "a coupling of 10 is capped at 2");
  /* The energies: coupling 2, stars 1, reheated 1, v 10, EtaSNcode * EnergySNcode 1000:
   * E_FB = 2 * 0.5 * 1 * 1000 = 1000, E_lift = 0.5 * 1 * 10 * 10 = 50, ejection 950 / 50 = 19 */
  mimic_sage_fire_locals_init(&l);
  const double ej = mimic_sage_fire_energy_ejection(2.0, 1.0, 1.0, 10.0, 1000.0, 1.0, &l);
  printf("\n  E_FB %.17g E_lift %.17g ejected %.17g", l.e_fb, l.e_lift, ej);
  TEST_ASSERT(l.e_fb == 1000.0 && l.e_lift == 50.0 && ej == 19.0,
              "E_FB 1000, E_lift 50 and the ejection 19");

  /* process() with FIREmodeOn 1, the cap not binding: Vvir 120 km/s at redshift 0 (ZZ[63] = 0),
   * stars 1, ColdGas 100. Scaling 0.5, eta = 3 * 0.5 = 1.5 (below 2 * esn / 120^2), no
   * renormalisation, coupling 0.3 * 0.5 below 2; the ejection is SAGE26's energy balance, written
   * here */
  const double esn = eta_sn_code_test * energy_sn_code_test;
  const char *log = "";
  TEST_ASSERT(fire_init("1") == 0, "init must succeed");
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 100.0, 120.0, 100.0, 1.0);
  halo.SnapNum = 63;
  gal.MassLoading = 7.5f;
  TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
  {
    const double vc = 120.0, stars = 1.0, eta = 1.5, reheated = 1.5;
    const double coupling = test_eta * 0.5;
    const double E_FB = coupling * 0.5 * stars * esn;
    const double E_lift = 0.5 * reheated * vc * vc;
    const double ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;
    printf("\n  process (120 km/s, z 0): MassLoading %.9g reheated %.17g ejected %.17g (expected "
           "%.17g) stars %.17g",
           (double)gal.MassLoading, gal.SupernovaReheatedMass, gal.SupernovaEjectedMass, ejected,
           gal.NewStellarMass);
    TEST_ASSERT(eta < 2.0 * esn / (vc * vc) && E_FB > E_lift, "the case is as constructed");
    TEST_ASSERT(gal.MassLoading == 1.5f, "MassLoading = (float)(3 * 0.5) = 1.5");
    TEST_ASSERT(gal.SupernovaReheatedMass == 1.5, "reheated mass eta * stars = 1.5");
    TEST_ASSERT(gal.NewStellarMass == 1.0, "no renormalisation: NewStellarMass stays 1");
    TEST_ASSERT(double_bits(gal.SupernovaEjectedMass) == double_bits(ejected),
                "the ejection is SAGE26's (E_FB - E_lift) / (0.5 v^2)");
  }

  /* process() with FIREmodeOn 1, both caps binding: Vvir 120 km/s at redshift 15 (pow(16, 1.25) =
   * 32), so scaling 16, eta_raw 48 above 2 * esn / 120^2 and the coupling 0.3 * 16 above 2 */
  MimicConfig.ZZ[10] = 15.0;
  setup_test_halo(&halo, &gal, 0, 100.0, 120.0, 100.0, 1.0);
  halo.SnapNum = 10;
  gal.MassLoading = 7.5f;
  TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
  {
    const double vc = 120.0, stars = 1.0;
    const double eta_max = 2.0 * esn / (vc * vc);
    const double reheated = eta_max * stars;
    const double E_FB = 2.0 * 0.5 * stars * esn; /* the capped coupling */
    const double E_lift = 0.5 * reheated * vc * vc;
    const double ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;
    printf("\n  process (120 km/s, z 15): MassLoading %.9g reheated %.17g ejected %.17g "
           "(expected %.17g)",
           (double)gal.MassLoading, gal.SupernovaReheatedMass, gal.SupernovaEjectedMass, ejected);
    TEST_ASSERT(48.0 > eta_max && test_eta * 16.0 > 2.0, "the case is as constructed");
    TEST_ASSERT(gal.MassLoading == (float)eta_max, "MassLoading is the capped loading");
    TEST_ASSERT(double_bits(gal.SupernovaReheatedMass) == double_bits(reheated),
                "the reheated mass is the capped loading times the stars");
    TEST_ASSERT(gal.NewStellarMass == 1.0, "no renormalisation: NewStellarMass stays 1");
    TEST_ASSERT(double_bits(gal.SupernovaEjectedMass) == double_bits(ejected),
                "the ejection with both caps binding");
  }
  return TEST_PASS;
}

/**
 * @test    test_fire_high_velocity_division
 * @brief   The high-velocity term is 1.0 / x as SAGE26 computes it, not libm pow(x, -1.0),
 *          which differs for this Vvir on some platforms
 */
int test_fire_high_velocity_division(void) {
  const float vvir = FIRE_TEST_VVIR_DIVISION_SEPARATOR;
  struct MimicSageFireLocals l;
  mimic_sage_fire_locals_init(&l);
  mimic_sage_fire_scaling(vvir, 0.0, &l);
  const double x = (double)vvir / 60.0;
  double (*volatile libm_pow)(double, double) = pow;
  const double by_pow = libm_pow(x, -1.0);
  printf("\n  Vvir %a: v_term 0x%016llx, 1.0 / x 0x%016llx, libm pow(x, -1.0) 0x%016llx",
         (double)vvir, (unsigned long long)double_bits(l.v_term),
         (unsigned long long)double_bits(1.0 / x), (unsigned long long)double_bits(by_pow));
  TEST_ASSERT(double_bits(l.v_term) == FIRE_TEST_V_TERM_DIVISION_BITS,
              "the high-velocity term must be SAGE26's division result");
#if defined(__APPLE__) && defined(__aarch64__)
  TEST_ASSERT(double_bits(by_pow) != FIRE_TEST_V_TERM_DIVISION_BITS,
              "on macOS arm64 libm pow(x, -1.0) differs here, so the division form is separated");
#endif
  return TEST_PASS;
}

/**
 * @test    test_fire_reheating_cap_and_mass_loading
 * @brief   capped_eta_reheat above, below and without a positive velocity, and
 *          MassLoading written before the renormalisation, also with no stars
 */
int test_fire_reheating_cap_and_mass_loading(void) {
  const double esn = fire_eta_sn() * fire_energy_sn();
  struct MimicSageFireLocals l;
  /* 2 km/s: the raw loading exceeds 2 * esn / v^2, which binds */
  mimic_sage_fire_locals_init(&l);
  const double s_low = mimic_sage_fire_scaling(2.0f, 0.0, &l);
  const double eta_low =
      mimic_sage_capped_eta_reheat(test_epsilon * s_low, 2.0, fire_eta_sn(), fire_energy_sn(), &l);
  TEST_ASSERT(l.eta_cap_binds == 1 && double_bits(eta_low) == double_bits(2.0 * esn / (2.0 * 2.0)),
              "at 2 km/s the loading is capped at 2 * esn / v^2");
  /* 300 km/s at redshift 0: below the cap, unchanged */
  mimic_sage_fire_locals_init(&l);
  const double s_high = mimic_sage_fire_scaling(300.0f, 0.0, &l);
  const double eta_high = mimic_sage_capped_eta_reheat(test_epsilon * s_high, 300.0, fire_eta_sn(),
                                                       fire_energy_sn(), &l);
  /* Expected: SAGE26's eta_raw = epsilon * (pow(1 + z, 1.25) * (1.0 / (v / 60))), written here,
   * not taken from the scaling helper */
  TEST_ASSERT(l.eta_cap_binds == 0 &&
                  double_bits(eta_high) ==
                      double_bits(test_epsilon * (pow(1.0 + 0.0, 1.25) * (1.0 / (300.0 / 60.0)))) &&
                  double_bits(s_high) == double_bits(pow(1.0 + 0.0, 1.25) * (1.0 / (300.0 / 60.0))),
              "at 300 km/s the loading is not capped");
  /* A non-positive velocity returns the raw loading unchanged */
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_capped_eta_reheat(5.0, 0.0, fire_eta_sn(), fire_energy_sn(), &l) == 5.0 &&
                  l.eta_cap_vvir_nonpositive == 1 && isnan(l.eta_max),
              "vvir <= 0 returns eta_raw unchanged");

  /* No stars: MassLoading still becomes (float)eta, the budgets stay 0 */
  const char *log = "";
  TEST_ASSERT(fire_init("1") == 0, "init must succeed");
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 100.0, 80.0, 0.0, 0.0);
  halo.SnapNum = 63;
  gal.MassLoading = 7.5f;
  TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
  /* 80 km/s at redshift 0: scaling (80 / 60)^-1 = 0.75 exactly, eta = 3 * 0.75 = 2.25 (uncapped) */
  printf("\n  zero stars: MassLoading %.9g (expected 2.25)", (double)gal.MassLoading);
  TEST_ASSERT(gal.MassLoading == 2.25f, "a zero-star call writes MassLoading = (float)eta = 2.25");
  TEST_ASSERT(gal.SupernovaReheatedMass == 0.0 && gal.SupernovaEjectedMass == 0.0,
              "no stars: nothing reheated or ejected");

  /* The renormalisation scales stars and reheated mass after MassLoading is written */
  setup_test_halo(&halo, &gal, 0, 100.0, 10.0, 0.05, 0.01);
  halo.SnapNum = 63;
  TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
  /* Expected: SAGE26's eta_raw at 10 km/s and redshift 0, uncapped */
  const double eta10 = test_epsilon * (pow(1.0 + 0.0, 1.25) * pow(10.0 / 60.0, -3.2));
  TEST_ASSERT(eta10 < 2.0 * esn / (10.0 * 10.0), "the loading is below its cap on this input");
  /* ColdGas is a float: the factor divides its widened value */
  const double fac = (double)gal.ColdGas / (0.01 + eta10 * 0.01);
  TEST_ASSERT(gal.MassLoading == (float)eta10, "MassLoading is the loading before renormalisation");
  TEST_ASSERT(double_bits(gal.NewStellarMass) == double_bits(0.01 * fac) &&
                  double_bits(gal.SupernovaReheatedMass) == double_bits(eta10 * 0.01 * fac),
              "stars and reheated mass are renormalised together");
  return TEST_PASS;
}

/**
 * @test    test_fire_energy_ejection
 * @brief   sn_energy_coupling's cap, the energy-based ejection, the
 *          narrowing of the double halo Vvir to SAGE26's float, and no ejection for Vvir <= 0
 */
int test_fire_energy_ejection(void) {
  struct MimicSageFireLocals l;
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_sn_energy_coupling(100.0, test_eta, &l) == 2.0 && l.coupling_capped == 1,
              "the coupling is capped at 2");
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(double_bits(mimic_sage_sn_energy_coupling(1.0, test_eta, &l)) ==
                      double_bits(test_eta) &&
                  l.coupling_capped == 0,
              "below the cap the coupling is FeedbackEjectionEfficiency * scaling");
  /* E_FB above, below and equal to E_lift */
  const double esn_eta = fire_eta_sn(), esn_energy = fire_energy_sn();
  mimic_sage_fire_locals_init(&l);
  const double ej = mimic_sage_fire_energy_ejection(2.0, 1.0, 1.0, 10.0, esn_eta, esn_energy, &l);
  {
    /* SAGE26's energy balance written here */
    const double E_FB = 2.0 * 0.5 * 1.0 * (esn_eta * esn_energy);
    const double E_lift = 0.5 * 1.0 * 10.0 * 10.0;
    TEST_ASSERT(l.energy_ejects == 1 && double_bits(l.e_fb) == double_bits(E_FB) &&
                    l.e_lift == 50.0 &&
                    double_bits(ej) == double_bits((E_FB - E_lift) / (0.5 * 10.0 * 10.0)),
                "E_FB > E_lift ejects (E_FB - E_lift) / (0.5 v^2)");
  }
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_fire_energy_ejection(0.1, 1.0, 100.0, 300.0, esn_eta, esn_energy, &l) ==
                      0.0 &&
                  l.energy_ejects == 0,
              "E_FB < E_lift ejects nothing");
  mimic_sage_fire_locals_init(&l);
  TEST_ASSERT(mimic_sage_fire_energy_ejection(0.0, 1.0, 0.0, 300.0, esn_eta, esn_energy, &l) ==
                      0.0 &&
                  l.energy_ejects == 0,
              "E_FB == E_lift (both 0) ejects nothing");

  /* process(): the double halo Vvir is narrowed to SAGE26's float before every FIRE use */
  const char *log = "";
  TEST_ASSERT(fire_init("1") == 0, "init must succeed");
  struct Halo halo;
  struct GalaxyData gal;
  const double vvir_double = 80.000001; /* not a float: narrows to 80.0000000f */
  setup_test_halo(&halo, &gal, 0, 100.0, vvir_double, 10.0, 0.01);
  halo.SnapNum = 40;
  TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
  const float vf = (float)vvir_double;
  /* Expected: SAGE26's FIRE expressions written here with the float Vvir widened (80.0 exactly) and
   * ZZ[40] = 2.3; the loading and the coupling are below their caps on this input */
  const double vc = vf;
  const double s = pow(1.0 + MimicConfig.ZZ[40], 1.25) * (1.0 / (vc / 60.0));
  const double eta = test_epsilon * s;
  const double c = test_eta * s;
  TEST_ASSERT(vc == 80.0 && eta < 2.0 * (esn_eta * esn_energy) / (vc * vc) && c < 2.0,
              "the case is as constructed");
  const double E_FB = c * 0.5 * 0.01 * (esn_eta * esn_energy);
  const double E_lift = 0.5 * (eta * 0.01) * vc * vc;
  const double expect = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;
  printf("\n  Vvir %.9g narrowed to %.9g: ejected %.17g", vvir_double, (double)vf,
         gal.SupernovaEjectedMass);
  TEST_ASSERT(double_bits(gal.SupernovaEjectedMass) == double_bits(expect),
              "the ejection uses the float Vvir widened to double");

  /* A non-positive own Vvir ejects nothing and the scaling is 0 */
  setup_test_halo(&halo, &gal, 0, 100.0, 0.0, 10.0, 0.01);
  halo.SnapNum = 40;
  gal.MassLoading = 7.5f;
  TEST_ASSERT(fire_process_captured(&halo, &log) == 0, "process must succeed");
  TEST_ASSERT(gal.SupernovaEjectedMass == 0.0 && gal.SupernovaReheatedMass == 0.0 &&
                  gal.MassLoading == 0.0f,
              "Vvir 0: scaling 0, MassLoading 0, nothing reheated or ejected");
  return TEST_PASS;
}

/**
 * @test    test_fire_stored_snapshot_refused
 * @brief   A stored SnapNum of -1 or of the table size reaching the FIRE helper fails with an error
 *          naming the SnapNum and the table size, and writes nothing; FIREmodeOn 0 never reads it.
 *          Each halo has dT > 0, so SnapNum -1 is not the initial-boundary state the guard skips.
 */
int test_fire_stored_snapshot_refused(void) {
  const int bad[] = {-1, 64};
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    const char *log = "";
    TEST_ASSERT(fire_init("1") == 0, "init must succeed");
    struct Halo halo;
    struct GalaxyData gal;
    setup_test_halo(&halo, &gal, 0, 100.0, 80.0, 10.0, 0.01);
    halo.SnapNum = bad[k];
    /* A positive interval, so SnapNum -1 is not the initial-boundary state the time guard
     * skips and reaches the refusal */
    halo.dT = 0.1;
    gal.MassLoading = 7.5f;
    const struct GalaxyData before = gal;
    const int status = fire_process_captured(&halo, &log);
    char expect[64];
    snprintf(expect, sizeof(expect), "SnapNum = %d", bad[k]);
    printf("\n  SnapNum %d: process %d, log: %.160s", bad[k], status, log);
    TEST_ASSERT(status != 0, "process must refuse a stored SnapNum outside the redshift table");
    TEST_ASSERT(strstr(log, expect) != NULL, "the error must name the SnapNum");
    TEST_ASSERT(strstr(log, "64 entries") != NULL, "the error must name the table size");
    TEST_ASSERT(memcmp(&before, &gal, sizeof(gal)) == 0, "a refused galaxy is not written");

    TEST_ASSERT(fire_init("0") == 0, "init must succeed");
    setup_test_halo(&halo, &gal, 0, 100.0, 80.0, 10.0, 0.01);
    halo.SnapNum = bad[k];
    TEST_ASSERT(fire_process_captured(&halo, &log) == 0,
                "FIREmodeOn 0 never reads the redshift, so the same galaxy is processed");
  }
  return TEST_PASS;
}

/**
 * @test    test_fire_initial_boundary_skipped
 * @brief   With FIREmodeOn 1, a galaxy in the initial-boundary state (stored SnapNum -1, dT 0:
 *          MIMIC_OBJECT_TIME_SKIP_INITIAL) is skipped before the ZZ[SnapNum] read: process()
 *          succeeds, logs no refusal, and writes nothing (NewStellarMass, SupernovaReheatedMass,
 *          SupernovaEjectedMass and MassLoading keep their inputs, every other member too)
 */
int test_fire_initial_boundary_skipped(void) {
  const char *log = "";
  TEST_ASSERT(fire_init("1") == 0, "init must succeed");
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 100.0, 80.0, 10.0, 0.01);
  halo.SnapNum = -1;
  halo.dT = 0.0;
  gal.SupernovaReheatedMass = 0.125;
  gal.SupernovaEjectedMass = 0.25;
  gal.MassLoading = 7.5f;
  const struct GalaxyData before = gal;
  const int status = fire_process_captured(&halo, &log);
  printf("\n  SnapNum -1, dT 0: process %d", status);
  TEST_ASSERT(status == 0, "the initial-boundary state is skipped, not refused");
  TEST_ASSERT(strstr(log, "SnapNum") == NULL, "no refusal is logged");
  TEST_ASSERT(gal.NewStellarMass == 0.01 && gal.SupernovaReheatedMass == 0.125 &&
                  gal.SupernovaEjectedMass == 0.25 && gal.MassLoading == 7.5f,
              "the module's outputs and MassLoading keep their inputs");
  TEST_ASSERT(memcmp(&before, &gal, sizeof(gal)) == 0, "no member is written");
  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST BYPASS
// ============================================================================

/**
 * @test    test_ffb_row_bypassed
 * @brief   An FFBRegime 1 row is not processed; the same row with FFBRegime 0 is
 *
 * SAGE26's starformation_and_feedback hands an FFBRegime 1 row to starformation_ffb and returns
 * before compute_sn_feedback. The row is a
 * satellite with its own FoF central, both compared byte for byte with their inputs. With
 * FIREmodeOn 0 the FFBRegime 0 row's reheated mass is FeedbackReheatingEpsilon * stars,
 * 3.0 * 1.0 exactly, with no renormalisation (ample cold gas). With FIREmodeOn 1 a stored
 * SnapNum outside the redshift table is refused on an ordinary row and passed over,
 * writing nothing, on an FFB row.
 */
int test_ffb_row_bypassed(void) {
  TEST_ASSERT(fire_init("0") == 0, "init with FIREmodeOn 0 must succeed");

  struct Halo central, row;
  struct GalaxyData central_gal, gal;
  setup_test_halo(&central, &central_gal, 0, 100.0, 150.0, 20.0, 0.0);
  setup_test_halo(&row, &gal, 1, 10.0, 80.0, 100.0, 1.0);
  row.dT = 0.1;
  gal.FFBRegime = 1;
  gal.SupernovaReheatedMass = 0.625; /* seeds the module would overwrite */
  gal.SupernovaEjectedMass = 0.875;
  gal.MassLoading = 12.25f;
  struct ModuleContext ctx;
  setup_module_context(&ctx, &central, 0.01);

  struct GalaxyData row_in, central_in;
  memcpy(&row_in, &gal, sizeof(gal));
  memcpy(&central_in, &central_gal, sizeof(central_gal));

  TEST_ASSERT(sage26_calculate_supernova_feedback_process(&ctx, &row, 1) == 0,
              "process succeeds on an FFB row");
  TEST_ASSERT(memcmp(&gal, &row_in, sizeof(gal)) == 0,
              "FFBRegime 1: every field of the row is unchanged");
  TEST_ASSERT(memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: every field of its central is unchanged");

  /* The same row with FFBRegime 0: SAGE26's fixed-epsilon budget */
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_calculate_supernova_feedback_process(&ctx, &row, 1) == 0,
              "process succeeds on an ordinary row");
  printf("\n  FFBRegime 0: reheated %.17g, stars %.17g", gal.SupernovaReheatedMass,
         gal.NewStellarMass);
  TEST_ASSERT(gal.SupernovaReheatedMass == 3.0 && gal.NewStellarMass == 1.0,
              "FFBRegime 0: reheated = FeedbackReheatingEpsilon * stars, stars kept");
  TEST_ASSERT(gal.SupernovaEjectedMass != 0.875, "FFBRegime 0: the ejection is written");
  TEST_ASSERT(memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "the budget step never writes the central");

  /* FIREmodeOn 1: a stored SnapNum outside the table, with a positive interval */
  TEST_ASSERT(fire_init("1") == 0, "init with FIREmodeOn 1 must succeed");
  memcpy(&gal, &row_in, sizeof(gal));
  row.SnapNum = 64;
  TEST_ASSERT(
      sage26_calculate_supernova_feedback_process(&ctx, &row, 1) == 0 &&
          memcmp(&gal, &row_in, sizeof(gal)) == 0 &&
          memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
      "FFBRegime 1: returns before the time guard and the redshift lookup, writing nothing");
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_calculate_supernova_feedback_process(&ctx, &row, 1) != 0,
              "FFBRegime 0: the same stored SnapNum is refused");
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage26_calculate_supernova_feedback tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_calculate_supernova_feedback Module\n");
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

  /* FIREmodeOn tests */
  printf("\n%s--- FIREmodeOn Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_fire_on_versus_off);
  TEST_RUN(test_fire_scaling_branches);
  TEST_RUN(test_fire_helpers_independent_expectations);
  TEST_RUN(test_fire_high_velocity_division);
  TEST_RUN(test_fire_reheating_cap_and_mass_loading);
  TEST_RUN(test_fire_energy_ejection);
  TEST_RUN(test_fire_stored_snapshot_refused);
  TEST_RUN(test_fire_initial_boundary_skipped);

  /* The feedback-free-burst bypass */
  TEST_RUN(test_ffb_row_bypassed);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
