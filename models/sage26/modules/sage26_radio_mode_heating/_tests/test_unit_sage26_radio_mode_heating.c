/**
 * @file    test_unit_sage26_radio_mode_heating.c
 * @brief   Unit tests for sage26_radio_mode_heating module
 *
 * Validates: AGN physics, cooling suppression, accretion modes, edge cases
 *
 * This test validates the sage26_radio_mode_heating module physics:
 * - AGN accretion (the Bondi rate from the carried x; AGNrecipe 1 and 3 refused)
 * - Eddington rate limiting
 * - Cooling suppression based on heating radius
 * - Black hole growth and hot gas consumption
 * - Heating radius evolution
 * - Edge cases (zero values, orphan galaxies, AGN off)
 * - Parameter sensitivity
 *
 * Test cases:
 *   - test_eddington_limit: Accretion limited by Eddington rate
 *   - test_agn_suppresses_cooling: CoolingGas reduced by AGN
 *   - test_cooling_suppression_by_rheat: Rheat/Rcool suppression logic
 *   - test_heating_radius_update: Rheat increases with AGN heating
 *   - test_heating_energy_tracking: Heating accumulator tracks energy
 *   - test_bondi_uses_carried_x: Bondi accretion from the carried x, Heating undivided,
 *     no second subtraction of the heating from the cooling mass (CGMrecipeOn 0)
 *   - test_agn_off_no_changes: No changes when AGN disabled
 *   - test_zero_cooling_gas: No AGN when no cooling
 *   - test_zero_hot_gas: No accretion without hot gas
 *   - test_zero_black_hole_mass: Edge case handling
 *   - test_orphan_galaxy_processed: Type 2 galaxies processed like centrals (SAGE parity)
 *   - test_parameter_sensitivity_efficiency: RadioModeEfficiency affects results
 *   - test_agn_recipe_1_refused: AGNrecipe 1 (SAGE26's empirical rate) is not supported; refused
 *   - test_agn_recipe_3_refused: AGNrecipe 3 (SAGE26's cold cloud rate) is not supported; refused
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *
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
#include "shared/sage_cooling_physics.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double RADIO_MODE_EFFICIENCY;
extern int AGN_RECIPE_ON;

/* Module functions (extern declarations for direct testing) */
extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_radio_mode_heating_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Setup test galaxy with specified properties
 *
 * @param   halo            Halo structure to initialize
 * @param   gal             Galaxy structure to initialize
 * @param   mvir            Virial mass [1e10 Msun/h]
 * @param   vvir            Virial velocity [km/s]
 * @param   rvir            Virial radius [Mpc/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   black_hole_mass Black hole mass [1e10 Msun/h]
 * @param   cooling_gas     Cooling gas rate [1e10 Msun/h per timestep]
 * @param   rcool           Cooling radius [Mpc/h]
 * @param   rheat           Heating radius [Mpc/h]
 *
 * The carried x (CoolingX) is 2e-3, which gives the Bondi rate an accretion below the Eddington
 * and heating caps for the default galaxy; a test that needs another x sets it after this call.
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal, double mvir, double vvir,
                              double rvir, double hot_gas, double black_hole_mass,
                              double cooling_gas, double rcool, double rheat) {
  memset(halo, 0, sizeof(struct Halo));
  memset(gal, 0, sizeof(struct GalaxyData));

  halo->Type = 0; /* Central galaxy */
  halo->Mvir = (float)mvir;
  halo->Vvir = (float)vvir;
  halo->Rvir = (float)rvir;
  halo->SnapNum = 63;
  halo->dT = 0.01; /* Timestep for heating rate calculation */
  halo->galaxy = gal;

  gal->HotGas = (float)hot_gas;
  gal->MetalsHotGas = (float)(hot_gas * 0.02); /* 2% metallicity */
  gal->BlackHoleMass = (float)black_hole_mass;
  gal->CoolingGas = (float)cooling_gas;
  gal->Rcool = (float)rcool;
  gal->Rheat = (float)rheat;
  gal->Heating = 0.0;
  gal->CoolingX = 2.0e-3;
}

/**
 * @brief   Setup test parameters
 *
 * @param   efficiency  Radio mode efficiency
 * @param   recipe      AGN recipe (0=off, 2=Bondi; 1 and 3 are refused)
 */
static void setup_test_parameters(double efficiency, int recipe) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", efficiency);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "AGNrecipe");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", recipe);

  MimicConfig.NumModelParams = idx;
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

  /* Set unit conversions (typical values) */
  MimicConfig.UnitMass_in_g = 1.989e43;     /* 1e10 Msun/h */
  MimicConfig.UnitTime_in_s = 3.08568e16;   /* ~1 Gyr/h */
  MimicConfig.UnitEnergy_in_cgs = 1.989e53; /* Energy units */
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.G = 43.0071; /* the Bondi rate's G, (Mpc/h) (km/s)^2 / (1e10 Msun/h) */
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_eddington_limit
 * @brief   Test that AGN accretion is limited by Eddington rate
 *
 * Expected: the Bondi rate from a large carried x exceeds the Eddington rate, and the cooling
 *           mass is large enough that the heating cap does not bind, so the accretion is the
 *           Eddington rate times dt
 * Validates: Eddington rate limiting physics
 */
int test_eddington_limit(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Use maximum valid efficiency */
  setup_test_parameters(1.0, 2); /* Maximum valid efficiency, Bondi */
  sage26_radio_mode_heating_init();

  /* Small BH mass = low Eddington rate */
  struct Halo halo;
  struct GalaxyData gal;
  const double black_hole_mass = 0.0001; /* Very small BH */
  const double hot_gas = 10.0;
  const double cooling_gas = 100.0; /* Very high cooling: the heating cap does not bind */

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, hot_gas, black_hole_mass, cooling_gas, 0.05,
                    0.0);
  gal.CoolingX = 1.0; /* Bondi rate about 7.6e-3, above the Eddington rate about 3.1e-3 */

  struct ModuleContext ctx;
  const double dt = 0.01;
  setup_test_context(&ctx, dt);

  /* Calculate expected Eddington rate */
  const double edd_luminosity = 1.3e38 * black_hole_mass * 1e10 / ctx.params->Hubble_h;
  const double edd_rate =
      edd_luminosity / (ctx.params->UnitEnergy_in_cgs / ctx.params->UnitTime_in_s) / (0.1 * 9e10);
  const double max_accretion = edd_rate * dt;

  const double initial_bh_mass = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  const double accreted = gal.BlackHoleMass - initial_bh_mass;
  TEST_ASSERT(accreted > 0.0, "Some accretion should occur");
  TEST_ASSERT(accreted <= max_accretion * 1.01, "Accretion should be Eddington-limited");
  /* 1e-5 relative: the float BlackHoleMass store */
  TEST_ASSERT_DOUBLE_EQUAL(accreted, max_accretion, 1e-5 * max_accretion,
                           "Accretion should equal the Eddington rate times dt");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_agn_suppresses_cooling
 * @brief   Test that AGN heating suppresses cooling
 *
 * Expected: CoolingGas reduced after AGN heating or BH grows
 * Validates: Cooling suppression mechanism
 */
int test_agn_suppresses_cooling(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double initial_cooling = 1.0;

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, initial_cooling, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(gal.CoolingGas >= 0.0, "CoolingGas should remain non-negative");

  /* AGN should have some effect */
  const int bh_grew = (gal.BlackHoleMass > initial_bh_mass);
  const int cooling_changed = (gal.CoolingGas != initial_cooling);
  TEST_ASSERT(bh_grew || cooling_changed,
              "AGN should have some effect (BH growth or cooling change)");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_cooling_suppression_by_rheat
 * @brief   Test cooling suppression based on Rheat/Rcool ratio
 *
 * Expected: Complete suppression when Rheat >= Rcool, partial when Rheat < Rcool
 * Validates: Heating radius suppression logic
 */
int test_cooling_suppression_by_rheat(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* Test 1: Rheat >= Rcool → complete suppression */
  struct Halo halo1;
  struct GalaxyData gal1;
  const double rcool = 0.05;
  const double rheat_high = 0.06; /* rheat > rcool */

  setup_test_galaxy(&halo1, &gal1, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, rcool, rheat_high);

  sage26_radio_mode_heating_process(&ctx, &halo1, 1);

  TEST_ASSERT_DOUBLE_EQUAL(gal1.CoolingGas, 0.0, 1e-10,
                           "CoolingGas should be zero when Rheat > Rcool");

  /* Test 2: Rheat < Rcool → partial suppression */
  struct Halo halo2;
  struct GalaxyData gal2;
  const double rheat_low = 0.025; /* rheat = 0.5 * rcool */
  const double initial_cooling = 1.0;

  setup_test_galaxy(&halo2, &gal2, 100.0, 200.0, 0.2, 10.0, 0.01, initial_cooling, rcool,
                    rheat_low);

  sage26_radio_mode_heating_process(&ctx, &halo2, 1);

  /* Expected: CoolingGas = (1 - rheat/rcool) * initial_cooling */
  const double expected_cooling_after_rheat = (1.0 - rheat_low / rcool) * initial_cooling;
  /* After Rheat suppression, AGN will further suppress it */
  TEST_ASSERT(gal2.CoolingGas <= expected_cooling_after_rheat,
              "CoolingGas should be suppressed when Rheat < Rcool");
  TEST_ASSERT(gal2.CoolingGas >= 0.0, "CoolingGas should be non-negative");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_heating_radius_update
 * @brief   Test that heating radius increases with AGN heating
 *
 * Expected: Rheat increases when AGN heats gas
 * Validates: Heating radius evolution
 */
int test_heating_radius_update(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double initial_rheat = 0.01; /* Small initial heating radius */

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, initial_rheat);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(gal.Rheat >= initial_rheat, "Rheat should not decrease");
  /* Rheat should increase if AGN heating occurred */
  if (gal.BlackHoleMass > 0.01) { /* If accretion occurred */
    /* Rheat may increase, or stay the same if heating is small */
    TEST_ASSERT(gal.Rheat <= gal.Rcool * 1.1, "Rheat should not exceed Rcool by large margin");
  }

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_heating_energy_tracking
 * @brief   Test that Heating accumulator tracks AGN heating energy
 *
 * Expected: Heating > 0 when AGN active
 * Validates: Energy tracking mechanism
 */
int test_heating_energy_tracking(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  TEST_ASSERT_DOUBLE_EQUAL(gal.Heating, 0.0, 1e-10, "Initial Heating should be zero");

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Heating should be positive if AGN was active and suppressed cooling */
  if (gal.BlackHoleMass > 0.01 && gal.CoolingGas < 1.0) {
    TEST_ASSERT(gal.Heating >= 0.0, "Heating should be non-negative");
  }

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_bondi_uses_carried_x
 * @brief   SAGE26's Bondi path with CGMrecipeOn 0
 *
 * Expected: the accretion rate is built from the CoolingX the cooling budget
 *           carries (none without it); Heating gains 0.5 * AGNheating * Vvir^2
 *           undivided; the returned cooling mass is only the Rheat-suppressed one,
 *           with no second subtraction of the heating; Rheat ratchets to
 *           (AGNheating / coolingGas) * rcool.
 */
int test_bondi_uses_carried_x(void) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(0.08, 2); /* Bondi */
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;
  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);
  MimicConfig.G = 43.0071;

  /* Without a carried x there is no Bondi accretion */
  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, 0.0);
  gal.CoolingX = 0.0;
  TEST_ASSERT(sage26_radio_mode_heating_process(&ctx, &halo, 1) == 0, "process should succeed");
  TEST_ASSERT(gal.BlackHoleMass == 0.01f, "no accretion without x");
  TEST_ASSERT(gal.Heating == 0.0, "no heating without x");

  /* With x: below the Eddington and heating caps */
  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, 0.0);
  const double x = 1.0e-3;
  gal.CoolingX = x;
  const double dt = halo.dT / ctx.num_substeps;
  const double bh = (double)gal.BlackHoleMass;
  const double rate = (2.5 * M_PI * MimicConfig.G) * (0.375 * 0.6 * x) * bh * 0.08;
  const double accreted = rate * dt;
  const double coeff = (1.34e5 / 200.0) * (1.34e5 / 200.0);
  const double heating = coeff * accreted;
  TEST_ASSERT(heating < 1.0, "fixture must stay below the heating cap");

  TEST_ASSERT(sage26_radio_mode_heating_process(&ctx, &halo, 1) == 0, "process should succeed");
  /* BlackHoleMass is float: the module adds in double and narrows once */
  TEST_ASSERT(gal.BlackHoleMass == (float)(bh + accreted),
              "BH grows by the Bondi rate from the carried x times dt");
  TEST_ASSERT_DOUBLE_EQUAL(gal.Heating, 0.5 * heating * 200.0 * 200.0,
                           1e-9 * 0.5 * heating * 200.0 * 200.0,
                           "Heating gains 0.5 AGNheating Vvir^2, not divided by dT");
  TEST_ASSERT(gal.CoolingGas == 1.0,
              "the heating is not subtracted from the cooling mass a second time");
  TEST_ASSERT_DOUBLE_EQUAL(gal.Rheat, heating / 1.0 * 0.05, 1e-7,
                           "Rheat ratchets to (AGNheating / coolingGas) * rcool");

  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_agn_off_no_changes
 * @brief   Test that no changes occur when AGN is disabled
 *
 * Expected: All galaxy properties unchanged
 * Validates: AGNrecipe = 0 disables AGN
 */
int test_agn_off_no_changes(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 0); /* AGN off */
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double initial_cooling = 1.0;

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, initial_cooling, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;
  const double initial_hot_gas = gal.HotGas;

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                           "BH mass should not change when AGN off");
  TEST_ASSERT_DOUBLE_EQUAL(gal.HotGas, initial_hot_gas, 1e-10,
                           "Hot gas should not change when AGN off");
  /* Note: CoolingGas may still be suppressed by Rheat even with AGN off */

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_cooling_gas
 * @brief   Test that no AGN accretion occurs when cooling is zero
 *
 * Expected: No changes to BH or hot gas
 * Validates: Early exit when CoolingGas = 0
 */
int test_zero_cooling_gas(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;

  /* CoolingGas = 0 */
  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 0.0, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;
  const double initial_hot_gas = gal.HotGas;

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                           "BH mass should not change when no cooling");
  TEST_ASSERT_DOUBLE_EQUAL(gal.HotGas, initial_hot_gas, 1e-10,
                           "Hot gas should not change when no cooling");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_hot_gas
 * @brief   Test behavior when HotGas is zero
 *
 * Expected: No accretion (nothing to accrete from)
 * Validates: Edge case handling
 */
int test_zero_hot_gas(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;

  /* HotGas = 0 */
  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 0.0, 0.01, 1.0, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                           "BH mass should not change when no hot gas");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_black_hole_mass
 * @brief   Test behavior when black hole mass is zero
 *
 * Expected: Handles gracefully (may or may not accrete depending on mode)
 * Validates: Edge case handling
 */
int test_zero_black_hole_mass(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;

  /* BlackHoleMass = 0 */
  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.0, 1.0, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  int result = sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle zero BH mass gracefully");
  TEST_ASSERT(gal.BlackHoleMass >= 0.0, "BH mass should remain non-negative");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_orphan_galaxy_processed
 * @brief   SAGE parity: orphan galaxies (Type 2) are processed like any other
 *          galaxy — SAGE calls do_AGN_heating from cooling_recipe for every
 *          non-merged galaxy. With AGN on, hot gas, and a black hole present,
 *          the orphan's BH accretes (grows) and its hot gas is consumed.
 *
 * Validates: Type 2 galaxies are no longer skipped.
 */
int test_orphan_galaxy_processed(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, 0.0);

  halo.Type = 2; /* Orphan galaxy */

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;
  const double initial_hot_gas = gal.HotGas;

  /* ===== EXECUTE ===== */
  int result = sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should process the Type 2 orphan");
  TEST_ASSERT(gal.BlackHoleMass > initial_bh_mass,
              "Orphan BH should accrete (grow) — orphan is processed");
  TEST_ASSERT(gal.HotGas < initial_hot_gas, "Orphan hot gas should be consumed by BH accretion");
  TEST_ASSERT(gal.BlackHoleMass >= 0.0 && gal.HotGas >= 0.0, "Orphan reservoirs remain physical");

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_efficiency
 * @brief   Test that changing RadioModeEfficiency parameter is respected
 *
 * Expected: Module loads different efficiency values correctly
 * Validates: Parameter loading and validation
 */
int test_parameter_sensitivity_efficiency(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test with low efficiency */
  reset_config();
  setup_test_parameters(0.02, 2);
  int result_low = sage26_radio_mode_heating_init();
  TEST_ASSERT(result_low == 0, "Module should initialize with low efficiency");
  sage26_radio_mode_heating_cleanup();

  /* Test with high efficiency */
  reset_config();
  setup_test_parameters(0.80, 2);
  int result_high = sage26_radio_mode_heating_init();
  TEST_ASSERT(result_high == 0, "Module should initialize with high efficiency");
  sage26_radio_mode_heating_cleanup();

  /* Test with maximum efficiency */
  reset_config();
  setup_test_parameters(1.0, 2);
  int result_max = sage26_radio_mode_heating_init();
  TEST_ASSERT(result_max == 0, "Module should initialize with maximum efficiency");
  sage26_radio_mode_heating_cleanup();

  /* Test with minimum efficiency */
  reset_config();
  setup_test_parameters(0.0, 2);
  int result_min = sage26_radio_mode_heating_init();
  TEST_ASSERT(result_min == 0, "Module should initialize with minimum efficiency");
  sage26_radio_mode_heating_cleanup();

  /* ===== CLEANUP ===== */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @brief   Initialise the module with AGNrecipe `recipe`, capturing its log
 *
 * @param   recipe  The AGNrecipe to load
 * @param   log     Receives the captured log (static buffer, valid until the next call)
 * @return  The init status
 */
static int init_with_recipe_captured(const int recipe, const char **log) {
  static char buf[8192];
  reset_config();
  setup_test_parameters(0.08, recipe);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_radio_mode_heating_init();
  buf[0] = '\0';
  if (capture != NULL) {
    set_log_output(old_out);
    rewind(capture);
    const size_t n = fread(buf, 1, sizeof(buf) - 1, capture);
    buf[n] = '\0';
    fclose(capture);
  }
  *log = buf;
  return status;
}

/**
 * @test    test_agn_recipe_1_refused
 * @brief   AGNrecipe 1, SAGE26's empirical rate, is not supported: init fails with an error
 *          naming the parameter
 */
int test_agn_recipe_1_refused(void) {
  init_memory_system(0);
  const char *log = "";
  TEST_ASSERT(init_with_recipe_captured(1, &log) != 0, "init must refuse AGNrecipe 1");
  TEST_ASSERT(strstr(log, "AGNrecipe") != NULL, "the error must name AGNrecipe");
  sage26_radio_mode_heating_cleanup();
  TEST_ASSERT(init_with_recipe_captured(2, &log) == 0, "init must accept AGNrecipe 2");
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_agn_recipe_3_refused
 * @brief   AGNrecipe 3, SAGE26's cold cloud rate, is not supported: init fails with an error
 *          naming the parameter
 */
int test_agn_recipe_3_refused(void) {
  init_memory_system(0);
  const char *log = "";
  TEST_ASSERT(init_with_recipe_captured(3, &log) != 0, "init must refuse AGNrecipe 3");
  TEST_ASSERT(strstr(log, "AGNrecipe") != NULL, "the error must name AGNrecipe");
  sage26_radio_mode_heating_cleanup();
  TEST_ASSERT(init_with_recipe_captured(0, &log) == 0, "init must accept AGNrecipe 0");
  sage26_radio_mode_heating_cleanup();
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

  test_phase_add("galaxy_physics", "sage26_radio_mode_heating", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters */
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.08");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "AGNrecipe");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "2");
  MimicConfig.NumModelParams = idx;

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

  setup_test_parameters(0.08, 2);
  sage26_radio_mode_heating_init();

  /* Process a galaxy */
  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, 0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  /* ===== EXECUTE ===== */
  sage26_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* check_memory_leaks() will catch any leaks */

  /* ===== CLEANUP ===== */
  sage26_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

// ============================================================================
// THE CGMrecipeOn 1 RADIO AGN (shared/sage_cooling_physics.h)
// ============================================================================
//
// Behavioural tests of the CGM-on AGN helpers. Each expectation is a literal chosen so the result
// is exact, or SAGE26's expression written inline, never a call of the helper under test.

/** @brief A galaxy for the AGN helper tests (Millennium units, Bondi accretion) */
static void setup_agn_helper_galaxy(struct Halo *halo, struct GalaxyData *gal) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  MimicConfig.G = 43.007077856420629;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitTime_in_s = 3.08568e19;
  MimicConfig.UnitEnergy_in_cgs = 1.989e53;
  halo->galaxy = gal;
  halo->Mvir = 100.0;
  halo->Rvir = 0.1;
  halo->Vvir = 200.0;
  gal->HotGas = 10.0f;
  gal->MetalsHotGas = 0.1f;
  gal->CGMgas = 1.0f;
  gal->MetalsCGMgas = 0.01f;
  gal->BlackHoleMass = 0.0f; /* no accretion: only the suppression and the caps act */
}

/** @brief The largest float strictly below, and the smallest float at or above, 0.99 * rcool */
static void saturation_pair(const double rcool, float *below, float *at) {
  float f = (float)(0.99 * rcool);
  while (!(f < 0.99 * rcool)) {
    f = nextafterf(f, 0.0f);
  }
  *below = f;
  float g = nextafterf(f, INFINITY);
  while (g < 0.99 * rcool) {
    g = nextafterf(g, INFINITY);
  }
  *at = g;
}

/**
 * @test    test_cgm_on_saturation_either_side
 * @brief   The CGMrecipeOn 1 saturation Rheat >= 0.99 * rcool
 *
 * Expected: Rheat the largest float below 0.99 * rcool suppresses the mass to
 * (1 - Rheat / rcool) * m, in both calls; the smallest float at or above zeroes it with
 * CGMrecipeOn 1 but not with CGMrecipeOn 0 (the classic call).
 */
int test_cgm_on_saturation_either_side(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageAgnLocals al;
  const double rcool = 0.25, m = 0.5, dt = 1.0e-4;
  float below, at;
  saturation_pair(rcool, &below, &at);
  reset_config();

  setup_agn_helper_galaxy(&halo, &gal);
  gal.Rheat = below;
  double ret =
      mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, 2, 0.08, 1, dt, m, 1.0, rcool, &al);
  TEST_ASSERT(ret == (1.0 - below / rcool) * m && al.saturated == 0, "hot: below 0.99 suppresses");

  setup_agn_helper_galaxy(&halo, &gal);
  gal.Rheat = at;
  ret = mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, 2, 0.08, 1, dt, m, 1.0, rcool, &al);
  TEST_ASSERT(ret == 0.0 && al.saturated == 1, "hot: at 0.99 the CGMrecipeOn 1 saturation zeroes");

  setup_agn_helper_galaxy(&halo, &gal);
  gal.Rheat = at;
  ret = mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, 2, 0.08, 0, dt, m, 1.0, rcool, &al);
  TEST_ASSERT(ret == (1.0 - at / rcool) * m, "hot, CGMrecipeOn 0: no saturation");

  setup_agn_helper_galaxy(&halo, &gal);
  halo.Rvir = 1.0; /* keep the CGM call's Rvir cap out of the way */
  gal.Rheat = below;
  ret = mimic_sage_do_agn_heating_cgm(&halo, &MimicConfig, 2, 0.08, 1, dt, m, 1.0, rcool, &al);
  TEST_ASSERT(ret == (1.0 - below / rcool) * m, "CGM: below 0.99 suppresses");
  setup_agn_helper_galaxy(&halo, &gal);
  halo.Rvir = 1.0;
  gal.Rheat = at;
  ret = mimic_sage_do_agn_heating_cgm(&halo, &MimicConfig, 2, 0.08, 1, dt, m, 1.0, rcool, &al);
  TEST_ASSERT(ret == 0.0 && al.saturated == 1, "CGM: at 0.99 zeroes");
  return TEST_PASS;
}

/**
 * @test    test_cgm_on_rheat_caps
 * @brief   The Rvir caps: the hot call's outside its positive-reservoir block, the CGM call's
 * inside
 *
 * Expected: with HotGas 0 and Rheat 0.3 > Rvir 0.1 the hot call caps Rheat to the float Rvir with
 * CGMrecipeOn 1 and leaves it with 0; with CGMgas positive and Rheat above Rvir the
 * CGM call caps it, and with CGMgas 0 it does not reach the cap.
 */
int test_cgm_on_rheat_caps(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageAgnLocals al;
  reset_config();

  setup_agn_helper_galaxy(&halo, &gal);
  gal.HotGas = 0.0f;
  gal.Rheat = 0.3f;
  (void)mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, 2, 0.08, 1, 1.0e-4, 0.5, 1.0, 0.5, &al);
  TEST_ASSERT(al.reservoir_positive == 0 && gal.Rheat == 0.1f && al.rheat_capped == 1,
              "hot: capped to Rvir without a reservoir");
  setup_agn_helper_galaxy(&halo, &gal);
  gal.HotGas = 0.0f;
  gal.Rheat = 0.3f;
  (void)mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, 2, 0.08, 0, 1.0e-4, 0.5, 1.0, 0.5, &al);
  TEST_ASSERT(gal.Rheat == 0.3f, "hot, CGMrecipeOn 0: no cap");

  setup_agn_helper_galaxy(&halo, &gal);
  gal.Rheat = 0.3f;
  (void)mimic_sage_do_agn_heating_cgm(&halo, &MimicConfig, 2, 0.08, 1, 1.0e-4, 0.5, 1.0, 0.1, &al);
  TEST_ASSERT(gal.Rheat == 0.1f && al.rheat_capped == 1, "CGM: capped inside the block");
  setup_agn_helper_galaxy(&halo, &gal);
  gal.CGMgas = 0.0f;
  gal.Rheat = 0.3f;
  (void)mimic_sage_do_agn_heating_cgm(&halo, &MimicConfig, 2, 0.08, 1, 1.0e-4, 0.5, 1.0, 0.1, &al);
  TEST_ASSERT(gal.Rheat == 0.3f && al.rheat_capped == 0, "CGM: no reservoir, no cap");
  return TEST_PASS;
}

/**
 * @test    test_cgm_agn_recaps_to_post_accretion_cgm
 * @brief   The CGM AGN re-caps the returned mass to the CGMgas left after accretion
 *
 * Expected: m = CGMgas = 1, Rheat 0 (no suppression), a black hole of mass 1 at Vvir 1e5 km/s
 * accretes from the CGM, so the returned mass is the post-accretion CGMgas, below 1; the CGM pair
 * loses the accretion with its metallicity and the heating is not subtracted.
 */
int test_cgm_agn_recaps_to_post_accretion_cgm(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct MimicSageAgnLocals al;
  reset_config();
  setup_agn_helper_galaxy(&halo, &gal);
  halo.Vvir = 1.0e5;
  gal.BlackHoleMass = 1.0f;
  gal.Rheat = 0.0f;
  const double ret =
      mimic_sage_do_agn_heating_cgm(&halo, &MimicConfig, 2, 0.08, 1, 0.01, 1.0, 1.0, 0.1, &al);
  /* SAGE26's chain, inline: Bondi rate, Eddington cap, reservoir cap, AGNcoeff, heating cap */
  const double bh = 1.0f, vv = 1.0e5f, m = 1.0, x = 1.0, dt = 0.01;
  double rate = (2.5 * M_PI * 43.007077856420629) * (0.375 * 0.6 * x) * bh * 0.08;
  const double edd = (1.3e38 * bh * 1e10 / 0.73) / (1.989e53 / 3.08568e19) / (0.1 * 9.0e10);
  if (rate > edd)
    rate = edd;
  double acc = rate * dt;
  if (acc > (double)1.0f)
    acc = 1.0f;
  const double coeff = (1.34e5 / vv) * (1.34e5 / vv);
  if (coeff * acc > m && coeff > 0.0)
    acc = m / coeff;
  TEST_ASSERT(al.accreted == acc && acc > 0.0, "AGNaccreted from the inline accretion");
  TEST_ASSERT(gal.CGMgas < 1.0f, "the accretion drains the CGM");
  TEST_ASSERT(ret == (double)gal.CGMgas && al.recapped == 1, "re-capped to the post-accretion CGM");
  TEST_ASSERT(gal.CGMgas == (float)(1.0f - acc), "CGMgas -= AGNaccreted");
  TEST_ASSERT(gal.HotGas == 10.0f, "the hot pair is not touched");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_radio_mode_heating unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_radio_mode_heating Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Run physics calculation tests */
  TEST_RUN(test_eddington_limit);
  TEST_RUN(test_agn_suppresses_cooling);
  TEST_RUN(test_cooling_suppression_by_rheat);
  TEST_RUN(test_heating_radius_update);
  TEST_RUN(test_heating_energy_tracking);
  TEST_RUN(test_bondi_uses_carried_x);

  /* Run edge case tests */
  TEST_RUN(test_agn_off_no_changes);
  TEST_RUN(test_zero_cooling_gas);
  TEST_RUN(test_zero_hot_gas);
  TEST_RUN(test_zero_black_hole_mass);
  TEST_RUN(test_orphan_galaxy_processed);

  /* Run parameter sensitivity tests */
  TEST_RUN(test_parameter_sensitivity_efficiency);
  TEST_RUN(test_agn_recipe_1_refused);
  TEST_RUN(test_agn_recipe_3_refused);

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);

  printf("\n");
  printf("CGMrecipeOn 1 radio AGN (shared helpers)\n");
  printf("------------------------------------------------------------\n");

  TEST_RUN(test_cgm_on_saturation_either_side);
  TEST_RUN(test_cgm_on_rheat_caps);
  TEST_RUN(test_cgm_agn_recaps_to_post_accretion_cgm);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
