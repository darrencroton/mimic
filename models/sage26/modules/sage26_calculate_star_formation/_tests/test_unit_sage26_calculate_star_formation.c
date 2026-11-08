/**
 * @file    test_unit_sage26_calculate_star_formation.c
 * @brief   Unit tests for sage26_calculate_star_formation module
 *
 * Validates: Physics calculation logic, edge cases, parameter handling
 *
 * This test validates the sage26_calculate_star_formation module physics:
 * - Star formation calculation (Kennicutt-Schmidt with critical threshold)
 * - Critical threshold logic (above/below/at threshold)
 * - Edge cases (zero radius, zero velocity, negative values)
 * - Parameter sensitivity (efficiency affects results)
 * - Module lifecycle and memory safety
 *
 * Test cases:
 *   - test_sf_calculation_above_threshold: Normal star formation
 *   - test_sf_calculation_below_threshold: No SF when below threshold
 *   - test_sf_calculation_at_threshold: Boundary condition
 *   - test_sf_calculation_zero_radius: Edge case handling
 *   - test_sf_calculation_zero_velocity: Edge case handling
 *   - test_sf_parameter_sensitivity: Efficiency changes results
 *   - test_br06_vvir_nonpositive: SFprescription 1, own Vvir <= 0 gives H2gas 0 and no stars
 *   - test_br06_disk_radius_nonpositive: SFprescription 1, DiskScaleRadius <= 0 likewise
 *   - test_br06_coldgas_zero_clears_stale_h2: SFprescription 1, the cap clears a stale H2gas
 *   - test_br06_h2_within_hydrogen_and_sf_law: SFprescription 1, H2gas <= ColdGas * 0.74, the
 *     H2 star-formation law, H2DepletionTime_Gyr unchanged
 *   - test_br06_no_critical_mass_threshold: SFprescription 1 forms stars below the Croton
 *     critical mass
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *   - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central
 *     bytewise unchanged, also in a state the time guard refuses (the bypass precedes it); the
 *     same row with FFBRegime 0 forms SAGE26's sfr_croton06 stars, written inline
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
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double SFR_EFFICIENCY;
extern double STAR_FORMING_DISK_FACTOR;

/* Stable string storage for pipeline config used by physics tests that call
 * sage26_calculate_star_formation_init() directly.  The SF dependency check requires the
 * apply step to be visible in MimicConfig; static storage avoids the memory
 * system requirement. */
static char sf_pipeline_name0[] = "sage26_calculate_star_formation";
static char sf_pipeline_name1[] = "sage26_apply_star_formation_supernova";
static char sf_phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig sf_physics_pipeline[2];
static struct ModulePhaseConfig sf_substep_phase;

/* Module functions (extern declarations for direct testing) */
extern int sage26_calculate_star_formation_init(void);
extern int sage26_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_star_formation_cleanup(void);

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
 * @brief   Setup test parameters for a given SFprescription
 *
 * @param   efficiency      Star formation efficiency
 * @param   disk_factor     Star forming disk factor
 * @param   sf_prescription SFprescription: 0 (R1, the Croton law) or 1 (R2, BR06)
 */
static void setup_test_parameters_for_prescription(double efficiency, double disk_factor,
                                                   int sf_prescription) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "SfrEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", efficiency);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "StarFormingDiskFactor");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", disk_factor);

  MimicConfig.NumModelParams = idx;
  /* A non-default prescription goes in first: the fixture helper below adds only absent keys */
  if (sf_prescription != 0) {
    char value[MAX_STRING_LEN];
    snprintf(value, sizeof(value), "%d", sf_prescription);
    sage26_test_append_model_parameter("SFprescription", value);
  }
  /* The module's required SFprescription and H2 integration parameters, at R1's values */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_SF_MODULE);

  /* The SF dependency check requires sage26_apply_star_formation_supernova to be
   * visible in MimicConfig when sage26_calculate_star_formation_init() is called directly. */
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
 * @brief   Setup test parameters at SFprescription 0 (R1, the Croton law)
 *
 * @param   efficiency  Star formation efficiency
 * @param   disk_factor Star forming disk factor
 */
static void setup_test_parameters(double efficiency, double disk_factor) {
  setup_test_parameters_for_prescription(efficiency, disk_factor, 0);
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
 * @brief   Test star formation when ColdGas is above critical threshold
 *
 * Expected: NewStellarMass > 0, follows SF equation
 * Validates: Core physics calculation
 */
int test_sf_calculation_above_threshold(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Initialize module parameters */
  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  int result = sage26_calculate_star_formation_init();
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
  result = sage26_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process function should succeed");
  TEST_ASSERT(gal.ColdGas > cold_crit, "ColdGas should be above threshold");
  TEST_ASSERT(gal.NewStellarMass > 0.0, "NewStellarMass should be positive");
  TEST_ASSERT_DOUBLE_EQUAL(gal.NewStellarMass, expected_stars, 1e-6,
                           "NewStellarMass should match expected value");

  /* ===== CLEANUP ===== */
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

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

  sage26_calculate_star_formation_init();

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
  sage26_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(gal.ColdGas < cold_crit, "ColdGas should be below threshold");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero below threshold");

  /* ===== CLEANUP ===== */
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_at_threshold
 * @brief   Test star formation when ColdGas is exactly at critical threshold
 *
 * Expected: NewStellarMass = 0 (boundary condition)
 * Validates: Boundary condition handling
 */
int test_sf_calculation_at_threshold(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  sage26_calculate_star_formation_init();

  /* Setup galaxy parameters */
  const double disk_radius = 0.05; /* 50 kpc/h */
  const double vvir = 200.0;       /* 200 km/s */
  const double reff = disk_factor * disk_radius;
  const double cold_crit = 0.19 * vvir * reff;

  /* Set ColdGas exactly at threshold */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, cold_crit, disk_radius, vvir);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  sage26_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, cold_crit, 1e-6, "ColdGas should be at threshold");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero at threshold");

  /* ===== CLEANUP ===== */
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_zero_radius
 * @brief   Test star formation when DiskScaleRadius is zero
 *
 * Expected: NewStellarMass = 0 (handles edge case gracefully)
 * Validates: Division by zero protection
 */
int test_sf_calculation_zero_radius(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  sage26_calculate_star_formation_init();

  /* Create galaxy with zero disk radius */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 10.0, 0.0, 200.0); /* radius = 0 */

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage26_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle zero radius gracefully");
  TEST_ASSERT(gal.DiskScaleRadius == 0.0, "DiskScaleRadius is zero");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero with zero radius");

  /* ===== CLEANUP ===== */
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_calculation_zero_velocity
 * @brief   Test star formation when Vvir is zero
 *
 * Expected: NewStellarMass = 0 (handles edge case gracefully)
 * Validates: Division by zero protection
 */
int test_sf_calculation_zero_velocity(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double efficiency = 0.02;
  const double disk_factor = 3.0;
  setup_test_parameters(efficiency, disk_factor);

  sage26_calculate_star_formation_init();

  /* Create galaxy with zero virial velocity */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 10.0, 0.05, 0.0); /* vvir = 0 */

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage26_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle zero velocity gracefully");
  TEST_ASSERT(halo.Vvir == 0.0, "Vvir is zero");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "NewStellarMass should be zero with zero velocity");

  /* ===== CLEANUP ===== */
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_sf_parameter_sensitivity
 * @brief   Test that changing SfrEfficiency changes star formation
 *
 * Expected: Higher efficiency → more stars
 * Validates: Parameter sensitivity
 */
int test_sf_parameter_sensitivity(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test with low efficiency */
  reset_config();
  setup_test_parameters(0.01, 3.0);
  sage26_calculate_star_formation_init();

  struct Halo halo1;
  struct GalaxyData gal1;
  setup_test_galaxy(&halo1, &gal1, 10.0, 0.05, 200.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  sage26_calculate_star_formation_process(&ctx, &halo1, 1);
  const double stars_low_eff = gal1.NewStellarMass;
  sage26_calculate_star_formation_cleanup();

  /* Test with high efficiency */
  reset_config();
  setup_test_parameters(0.05, 3.0); /* 5x higher efficiency */
  sage26_calculate_star_formation_init();

  struct Halo halo2;
  struct GalaxyData gal2;
  setup_test_galaxy(&halo2, &gal2, 10.0, 0.05, 200.0); /* Same galaxy */

  sage26_calculate_star_formation_process(&ctx, &halo2, 1);
  const double stars_high_eff = gal2.NewStellarMass;
  sage26_calculate_star_formation_cleanup();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(stars_low_eff > 0.0, "Low efficiency should produce stars");
  TEST_ASSERT(stars_high_eff > 0.0, "High efficiency should produce stars");
  TEST_ASSERT(stars_high_eff > stars_low_eff, "Higher efficiency should produce more stars");

  /* Should scale approximately linearly (5x efficiency → ~5x stars) */
  const double ratio = stars_high_eff / stars_low_eff;
  TEST_ASSERT(ratio > 4.5 && ratio < 5.5, "Stars should scale linearly with efficiency");

  /* ===== CLEANUP ===== */
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// SFPRESCRIPTION 1 (BR06) TESTS
// ============================================================================
//
// Platform-independent invariants of SAGE26's sfr_br06 (R/src/model_starformation_and_feedback.c
// :193-252, R = the staged SAGE26 source) as the module ports it. They carry no oracle fixture and
// no reference-build bit pattern, so they run wherever the unit tier runs; the bit-for-bit parity
// with SAGE26 is the oracle test's (test_unit_sage26_calculate_star_formation_oracle.c).

/* Hubble_h of the BR06 tests (Millennium's), read through ctx->params */
#define BR06_TEST_HUBBLE_H 0.73

/* The H2DepletionTime_Gyr every BR06 test galaxy starts with: sfr_br06 must leave it as it is,
 * because it passes no SFR-output pointer to the radial integration (:222) */
#define BR06_TEST_DEPLETION_TIME (-1.0f)

/**
 * @brief   Initialise the module at SFprescription 1, R2's BR06 law with radial integration
 *
 * @param   efficiency  Star formation efficiency
 * @return  The module's init result
 */
static int init_br06_module(double efficiency) {
  init_memory_system(0);
  reset_config();
  MimicConfig.Hubble_h = BR06_TEST_HUBBLE_H;
  setup_test_parameters_for_prescription(efficiency, 3.0, 1);
  return sage26_calculate_star_formation_init();
}

/**
 * @brief   Setup a BR06 test galaxy: a disk with stars, a bulge and no prior H2 history
 *
 * @param   halo        Halo structure to initialize
 * @param   gal         Galaxy structure to initialize
 * @param   cold_gas    Cold gas mass [1e10 Msun/h]
 * @param   disk_radius Disk scale radius [Mpc/h]
 * @param   vvir        The galaxy's own virial velocity [km/s]
 * @param   stale_h2    H2gas before the call [1e10 Msun/h]
 */
static void setup_br06_galaxy(struct Halo *halo, struct GalaxyData *gal, double cold_gas,
                              double disk_radius, double vvir, float stale_h2) {
  setup_test_galaxy(halo, gal, cold_gas, disk_radius, vvir);
  gal->StellarMass = 2.0f; /* the radial integration reads the disk stars, */
  gal->BulgeMass = 0.5f;   /* StellarMass - BulgeMass */
  gal->H2gas = stale_h2;
  gal->H1gas = 0.0f;
  gal->H2DepletionTime_Gyr = BR06_TEST_DEPLETION_TIME;
}

/**
 * @test    test_br06_vvir_nonpositive
 * @brief   A non-positive own Vvir sets H2gas to 0 and forms no stars (:208-210)
 */
int test_br06_vvir_nonpositive(void) {
  static const double vvirs[] = {0.0, -50.0};
  for (size_t i = 0; i < sizeof(vvirs) / sizeof(vvirs[0]); i++) {
    TEST_ASSERT(init_br06_module(0.02) == 0, "Module init at SFprescription 1 should succeed");

    struct Halo halo;
    struct GalaxyData gal;
    setup_br06_galaxy(&halo, &gal, 1.0, 0.003, vvirs[i], 0.05f);
    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &halo, 1) == 0,
                "Process function should succeed");
    TEST_ASSERT(gal.H2gas == 0.0f, "A non-positive own Vvir sets H2gas to 0");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "A non-positive own Vvir forms no stars");
    TEST_ASSERT(gal.H2DepletionTime_Gyr == BR06_TEST_DEPLETION_TIME,
                "H2DepletionTime_Gyr is not written");

    sage26_calculate_star_formation_cleanup();
    check_memory_leaks();
  }
  return TEST_PASS;
}

/**
 * @test    test_br06_disk_radius_nonpositive
 * @brief   A non-positive disk scale radius (rs_pc <= 0) sets H2gas to 0 and forms no stars
 *          (:216-218)
 */
int test_br06_disk_radius_nonpositive(void) {
  static const double radii[] = {0.0, -0.003};
  for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); i++) {
    TEST_ASSERT(init_br06_module(0.02) == 0, "Module init at SFprescription 1 should succeed");

    struct Halo halo;
    struct GalaxyData gal;
    setup_br06_galaxy(&halo, &gal, 1.0, radii[i], 200.0, 0.05f);
    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &halo, 1) == 0,
                "Process function should succeed");
    TEST_ASSERT(gal.H2gas == 0.0f, "A non-positive disk scale radius sets H2gas to 0");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "A non-positive disk scale radius forms no stars");
    TEST_ASSERT(gal.H2DepletionTime_Gyr == BR06_TEST_DEPLETION_TIME,
                "H2DepletionTime_Gyr is not written");

    sage26_calculate_star_formation_cleanup();
    check_memory_leaks();
  }
  return TEST_PASS;
}

/**
 * @test    test_br06_coldgas_zero_clears_stale_h2
 * @brief   With no cold gas the radial integration returns early without writing H2gas
 *          (R/src/model_h2_chemistry.c:141-143); the ColdGas * 0.74 cap then clears the stale
 *          value (:239-242), and no stars form
 */
int test_br06_coldgas_zero_clears_stale_h2(void) {
  TEST_ASSERT(init_br06_module(0.02) == 0, "Module init at SFprescription 1 should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_br06_galaxy(&halo, &gal, 0.0, 0.003, 200.0, 0.1f);
  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &halo, 1) == 0,
              "Process function should succeed");
  TEST_ASSERT(gal.H2gas == 0.0f, "The cap clears a stale H2gas when the cold gas is gone");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "No H2 forms no stars");
  TEST_ASSERT(gal.H2DepletionTime_Gyr == BR06_TEST_DEPLETION_TIME,
              "H2DepletionTime_Gyr is not written");

  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_br06_h2_within_hydrogen_and_sf_law
 * @brief   After process() H2gas is positive and at most the double ColdGas * 0.74, stars follow
 *          strdot = SfrEfficiency * H2gas / tdyn (:244-248), and H2DepletionTime_Gyr is unchanged
 *
 * The expected stellar mass repeats the module's double operations in SAGE26's order (products
 * and quotients only, which no compiler contracts), so the comparison is exact.
 */
int test_br06_h2_within_hydrogen_and_sf_law(void) {
  static const struct {
    double cold_gas;    /* 1e10 Msun/h */
    double disk_radius; /* Mpc/h */
  } cases[] = {{0.01, 0.002}, {0.2, 0.003}, {1.0, 0.003}, {10.0, 0.005}, {5.0, 0.001}};
  const double efficiency = 0.02;
  const double vvir = 200.0;

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    TEST_ASSERT(init_br06_module(efficiency) == 0,
                "Module init at SFprescription 1 should succeed");

    struct Halo halo;
    struct GalaxyData gal;
    setup_br06_galaxy(&halo, &gal, cases[i].cold_gas, cases[i].disk_radius, vvir, 0.0f);
    struct ModuleContext ctx;
    setup_test_context(&ctx, 0.01);

    TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &halo, 1) == 0,
                "Process function should succeed");
    TEST_ASSERT(gal.H2gas > 0.0f, "A gas-rich disk has molecular gas");
    TEST_ASSERT((double)gal.H2gas <= (double)gal.ColdGas * 0.74,
                "H2gas never exceeds the hydrogen of the cold gas");
    TEST_ASSERT(gal.H2DepletionTime_Gyr == BR06_TEST_DEPLETION_TIME,
                "H2DepletionTime_Gyr is not written");

    const double reff = 3.0 * gal.DiskScaleRadius;
    const double tdyn = reff / (float)halo.Vvir;
    const double expected_stars = efficiency * gal.H2gas / tdyn * ((double)halo.dT / 1.0);
    TEST_ASSERT(gal.NewStellarMass > 0.0, "Molecular gas forms stars");
    TEST_ASSERT(gal.NewStellarMass == expected_stars,
                "Stars form at SfrEfficiency * H2gas / tdyn over the substep");

    sage26_calculate_star_formation_cleanup();
    check_memory_leaks();
  }
  return TEST_PASS;
}

/**
 * @test    test_br06_no_critical_mass_threshold
 * @brief   SFprescription 1 has no Croton critical mass: a disk below it still forms stars from
 *          its H2, where SFprescription 0 forms none
 */
int test_br06_no_critical_mass_threshold(void) {
  const double cold_gas = 0.2;
  const double disk_radius = 0.003;
  const double vvir = 200.0;
  const double cold_crit = 0.19 * vvir * (3.0 * disk_radius);

  /* SFprescription 1 */
  TEST_ASSERT(init_br06_module(0.02) == 0, "Module init at SFprescription 1 should succeed");
  struct Halo halo;
  struct GalaxyData gal;
  setup_br06_galaxy(&halo, &gal, cold_gas, disk_radius, vvir, 0.0f);
  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);
  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &halo, 1) == 0,
              "Process function should succeed");
  TEST_ASSERT(gal.ColdGas < cold_crit, "The disk is below the Croton critical mass");
  TEST_ASSERT(gal.H2gas > 0.0f, "The disk has molecular gas");
  TEST_ASSERT(gal.NewStellarMass > 0.0, "BR06 forms stars below the Croton critical mass");
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

  /* SFprescription 0, the same disk */
  init_memory_system(0);
  reset_config();
  MimicConfig.Hubble_h = BR06_TEST_HUBBLE_H;
  setup_test_parameters(0.02, 3.0);
  TEST_ASSERT(sage26_calculate_star_formation_init() == 0,
              "Module init at SFprescription 0 should succeed");
  setup_br06_galaxy(&halo, &gal, cold_gas, disk_radius, vvir, 0.05f);
  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &halo, 1) == 0,
              "Process function should succeed");
  TEST_ASSERT(gal.NewStellarMass == 0.0, "The Croton law forms no stars below its critical mass");
  TEST_ASSERT(gal.H2gas == 0.05f, "SFprescription 0 leaves H2gas as it was");
  sage26_calculate_star_formation_cleanup();
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

  /* SF requires sage26_apply_star_formation_supernova in the pipeline */
  test_phase_add("galaxy_physics", "sage26_calculate_star_formation", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters — use full set since apply step is also initialized */
  set_test_model_parameters();
  /* The module's required SFprescription and H2 parameters (and the apply step's SFprescription) */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_SF_MODULE);
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

/**
 * @test    test_memory_safety
 * @brief   Test that module doesn't leak memory during operation
 *
 * Expected: No memory leaks after init/process/cleanup cycle
 * Validates: Memory management
 */
int test_memory_safety(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.02, 3.0);
  sage26_calculate_star_formation_init();

  /* Process a galaxy */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 10.0, 0.05, 200.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  sage26_calculate_star_formation_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* check_memory_leaks() will catch any leaks */

  /* ===== CLEANUP ===== */
  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();

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
 * before its ordinary chain (R/src/model_starformation_and_feedback.c:1004-1008). The row is a
 * satellite with its own FoF central; with FFBRegime 1 both galaxy structs are compared byte for
 * byte with their inputs, in a valid interval and in one the time guard refuses (dT 0 with a
 * stored snapshot, which errors on an ordinary row), so the bypass precedes the guard. With
 * FFBRegime 0 the row forms SAGE26's sfr_croton06 stars (R/src/model_starformation_and_feedback.c
 * :160-184, :1048-1051), written here with SAGE26's operand order, never by calling the module.
 */
int test_ffb_row_bypassed(void) {
  init_memory_system(0);
  reset_config();
  const double efficiency = 0.02;
  setup_test_parameters(efficiency, 3.0);
  TEST_ASSERT(sage26_calculate_star_formation_init() == 0, "Module init should succeed");

  struct Halo central, row;
  struct GalaxyData central_gal, gal;
  setup_test_galaxy(&central, &central_gal, 4.0, 0.02, 250.0);
  setup_test_galaxy(&row, &gal, 10.0, 0.05, 200.0);
  row.Type = 1;
  gal.FFBRegime = 1;
  gal.NewStellarMass = 7.0; /* a seed the module would overwrite */
  gal.H1gas = 0.375f;       /* likewise (the H I block) */
  gal.H2gas = 0.25f;
  central_gal.NewStellarMass = 5.0;
  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);
  ctx.central_galaxy = &central;

  struct GalaxyData row_in, central_in;
  memcpy(&row_in, &gal, sizeof(gal));
  memcpy(&central_in, &central_gal, sizeof(central_gal));

  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &row, 1) == 0,
              "process succeeds on an FFB row");
  TEST_ASSERT(memcmp(&gal, &row_in, sizeof(gal)) == 0,
              "FFBRegime 1: every field of the row is unchanged");
  TEST_ASSERT(memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: every field of its central is unchanged");

  /* The time guard refuses dT 0 with a stored snapshot on an ordinary row; an FFB row returns
   * before it */
  row.dT = 0.0;
  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &row, 1) == 0 &&
                  memcmp(&gal, &row_in, sizeof(gal)) == 0 &&
                  memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: returns before the time guard, writing nothing");
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &row, 1) != 0,
              "the time guard refuses that interval on an ordinary row");
  row.dT = 0.01;

  /* The same row with FFBRegime 0: SAGE26's sfr_croton06 (:168-178) and stars = strdot * dt */
  memcpy(&gal, &row_in, sizeof(gal));
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_calculate_star_formation_process(&ctx, &row, 1) == 0,
              "process succeeds on an ordinary row");
  const float vvir = 200.0f;
  const double reff = 3.0 * (double)row_in.DiskScaleRadius;               /* :168 */
  const double tdyn = reff / vvir;                                        /* :173 */
  const double cold_crit = 0.19 * vvir * reff;                            /* :176 */
  const double strdot = efficiency * (row_in.ColdGas - cold_crit) / tdyn; /* :178 */
  const double expected_stars = strdot * 0.01;                            /* :1048 */
  printf("\n  FFBRegime 0: NewStellarMass %.17g, expected %.17g", gal.NewStellarMass,
         expected_stars);
  TEST_ASSERT(fabs(gal.NewStellarMass - expected_stars) <= 1e-12 * expected_stars,
              "FFBRegime 0: the row forms SAGE26's stars");
  TEST_ASSERT(gal.H1gas != row_in.H1gas, "FFBRegime 0: the H I block runs");
  TEST_ASSERT(gal.FFBRegime == 0, "FFBRegime is not written");

  sage26_calculate_star_formation_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage26_calculate_star_formation unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_calculate_star_formation Unit Tests\n");
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
  TEST_RUN(test_sf_parameter_sensitivity);

  /* Run SFprescription 1 (BR06) tests */
  TEST_RUN(test_br06_vvir_nonpositive);
  TEST_RUN(test_br06_disk_radius_nonpositive);
  TEST_RUN(test_br06_coldgas_zero_clears_stale_h2);
  TEST_RUN(test_br06_h2_within_hydrogen_and_sf_law);
  TEST_RUN(test_br06_no_critical_mass_threshold);

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);

  /* The feedback-free-burst bypass */
  TEST_RUN(test_ffb_row_bypassed);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
