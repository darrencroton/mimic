/**
 * @file    test_unit_sage26_radio_mode_heating.c
 * @brief   Unit tests for sage26_radio_mode_heating module
 *
 * Validates: the classic (CGMrecipeOn 0) radio-mode AGN call and the CGMrecipeOn 1 radio AGN
 * helpers
 *
 * On the default CGMrecipeOn 1 pipeline this module writes nothing (its cooling budget carries no
 * cooling mass), so these tests are the only protection of the classic path. Expected values are
 * hand-derived literals or SAGE26's expressions written inline, never a call of the code under
 * test.
 *
 * Test cases:
 *   - test_eddington_limit: the accretion is the Eddington rate times dt when Bondi exceeds it
 *   - test_bondi_uses_carried_x: Bondi accretion from the carried x, Heating undivided, no
 *     second subtraction of the heating from the cooling mass, no accretion without x
 *   - test_radio_accretion_rows: a table over the Rheat suppression (partial, complete, held
 *     ratchet), the heating cap, the reservoir cap, the hot-gas metals debit, Vvir 0, the
 *     efficiency, AGNrecipe 0 and the zero cooling, hot gas and black hole guards, each asserting
 *     the black hole, hot pair, cooling mass, Rheat and Heating
 *   - test_orphan_galaxy_processed: Type 2 galaxies are processed like centrals (SAGE parity)
 *   - test_module_initialization: module_system_init accepts the module's placement
 *   - test_cgm_on_saturation_either_side: the CGMrecipeOn 1 saturation Rheat >= 0.99 Rcool
 *   - test_cgm_on_rheat_caps: the Rvir caps of the hot and CGM calls
 *   - test_cgm_agn_recaps_to_post_accretion_cgm: the CGM call re-caps to the post-accretion CGM
 *
 * The AGNrecipe refusals (1 and 3) are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
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
#include <float.h>

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
// THE ACCRETION, HEATING AND GUARD TABLE
// ============================================================================

/** @brief Fails the case naming the table row and the quantity that missed */
#define ROW_ASSERT(cond, row, what)                                                                \
  do {                                                                                             \
    char row_msg[160];                                                                             \
    snprintf(row_msg, sizeof(row_msg), "row '%s': %s", (row)->name, (what));                       \
    TEST_ASSERT(cond, row_msg);                                                                    \
  } while (0)

/**
 * @brief One radio-mode call (CGMrecipeOn 0) and the galaxy it must leave
 *
 * Every galaxy has Mvir 100, Rvir 0.2 and an Rcool of 0.05; the substep is dt = 0.01 and the
 * Bondi coefficient is (2.5 pi G)(0.375 * 0.6 x) BH eff with G = 43.0071. The hot gas carries a
 * metallicity of 0.02. The expectations are hand-derived literals or expressions of SAGE26's
 * chain: Bondi rate below the Eddington rate, accreted = rate dt capped by the hot gas, the
 * heating coefficient (1.34e5 / Vvir)^2 = 448900 at Vvir 200, the cap of the heating to the
 * suppressed cooling mass, the suppression (1 - Rheat / Rcool) CoolingGas, the Rheat ratchet
 * (AGNheating / suppressed) Rcool and Heating += 0.5 AGNheating Vvir^2.
 */
struct accretion_row {
  const char *name;
  /* the call */
  double efficiency; /**< RadioModeEfficiency */
  int recipe;        /**< AGNrecipe */
  double vvir;       /**< Vvir [km/s] */
  double hot_gas;
  double black_hole_mass;
  double cooling_gas;
  double x; /**< the carried CoolingX */
  double rheat;
  /* the galaxy afterwards */
  double accreted;      /**< black hole growth, and the hot gas and its metals drawn */
  double cooling_after; /**< CoolingGas */
  double rheat_after;
  double heating_after; /**< Heating, undivided */
};

/**
 * @test    test_radio_accretion_rows
 * @brief   The suppression, accretion, caps, ratchet, Heating, metals debit and guards, one row
 *          per input, each asserted against the whole galaxy
 *
 * Validates: SAGE26's classic (CGMrecipeOn 0) radio-mode AGN call, which the default pipeline
 *            never runs because its cooling budget carries no cooling mass there
 */
int test_radio_accretion_rows(void) {
  /* Bondi accreted mass at x 1e-3, black hole 0.01, efficiency 0.08, over dt 0.01: 6.07999e-7 */
  static const struct accretion_row rows[] = {
      /* Below every cap, no earlier heating: AGNheating = 448900 * accreted = 0.27293, and Rheat
       * ratchets to 0.27293 / 1.0 * 0.05 */
      {"bondi_below_caps", 0.08, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.0, 6.079985388e-7, 1.0,
       0.01364652719, 5458.610881},
      /* Rheat 0.01 suppresses the mass to 0.8; twice the x doubles the accretion and the ratchet
       * (0.54586 / 0.8 * 0.05) raises Rheat past 0.01 */
      {"rheat_raised", 0.08, 2, 200.0, 10.0, 0.01, 1.0, 2.0e-3, 0.01, 1.215997078e-6, 0.8,
       0.03411631659, 10917.22176},
      /* Rheat 0.025 is half of Rcool: the mass is halved, and the ratchet (0.27293 / 0.5 * 0.05)
       * raises Rheat */
      {"rheat_half", 0.08, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.025, 6.079985388e-7, 0.5,
       0.02729305439, 5458.610881},
      /* A large cooling mass, 100, suppressed to 50: the ratchet value 0.54586 / 50 * 0.05 is
       * below the Rheat already held, which stays; the Heating still accumulates */
      {"rheat_held", 0.08, 2, 200.0, 10.0, 0.01, 100.0, 2.0e-3, 0.025, 1.215997078e-6, 50.0, 0.025,
       10917.22176},
      /* Rheat at and above Rcool suppresses the cooling mass completely: nothing is left to heat,
       * so the heating cap leaves no accretion */
      {"rheat_equals_rcool", 0.08, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.05, 0.0, 0.0, 0.05, 0.0},
      {"rheat_above_rcool", 0.08, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.06, 0.0, 0.0, 0.06, 0.0},
      /* The heating cap: 0.125 of cooling mass would be exceeded by the 0.27293 of heating, so
       * the accretion falls to 0.125 / 448900, the heating to 0.125 (Heating 0.5 0.125 200^2)
       * and Rheat to Rcool */
      {"heating_cap", 0.08, 2, 200.0, 10.0, 0.01, 0.125, 1.0e-3, 0.0, 0.125 / 448900.0, 0.125, 0.05,
       2500.0},
      /* The reservoir cap: only 1e-7 of hot gas against 6.08e-7 of accretion, so the black hole
       * takes the float 1e-7, the reservoir and its metals are emptied, and AGNheating =
       * 448900 * 1e-7 = 0.04489 */
      {"reservoir_cap", 0.08, 2, 200.0, 1.0e-7, 0.01, 1.0, 1.0e-3, 0.0, 1.000000012e-7, 1.0,
       0.002244499978, 897.8000105},
      /* The metals debit: a black hole of 1 accretes 6.08e-5 of hot gas at metallicity 0.02, so
       * MetalsHotGas falls by 1.216e-6 (a float step at 0.02 is 1.9e-9) */
      {"metals_debit", 0.08, 2, 200.0, 1.0, 1.0, 100.0, 1.0e-3, 0.0, 6.079985524e-5, 100.0,
       0.01364652812, 545861.1003},
      /* Vvir 0 forms no heating coefficient, so the accretion is not heating-capped and nothing
       * is heated */
      {"vvir_zero", 0.08, 2, 0.0, 10.0, 0.01, 1.0, 1.0e-3, 0.0, 6.079985388e-7, 1.0, 0.0, 0.0},
      /* The accretion is linear in the efficiency (the 0.08 rows above are the middle) */
      {"efficiency_half", 0.04, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.0, 3.039992694e-7, 1.0,
       0.006823263597, 2729.305441},
      {"efficiency_double", 0.16, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.0, 1.215997078e-6, 1.0,
       0.02729305439, 10917.22176},
      {"efficiency_zero", 0.0, 2, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.0, 0.0, 1.0, 0.0, 0.0},
      /* AGNrecipe 0 returns before touching anything: the mass is not suppressed (0.6 would be
       * the suppressed value at Rheat 0.02), Rheat stays and nothing accretes */
      {"agn_off", 0.08, 0, 200.0, 10.0, 0.01, 1.0, 1.0e-3, 0.02, 0.0, 1.0, 0.02, 0.0},
      /* The guards: no cooling mass leaves the galaxy as it was; no hot gas or no black hole
       * accretes nothing, but the mass is still suppressed (1 - 0.02 / 0.05 = 0.6) and Rheat is
       * not ratcheted */
      {"zero_cooling", 0.08, 2, 200.0, 10.0, 0.01, 0.0, 1.0e-3, 0.02, 0.0, 0.0, 0.02, 0.0},
      {"zero_hot_gas", 0.08, 2, 200.0, 0.0, 0.01, 1.0, 1.0e-3, 0.02, 0.0, 0.6, 0.02, 0.0},
      {"zero_black_hole", 0.08, 2, 200.0, 10.0, 0.0, 1.0, 1.0e-3, 0.02, 0.0, 0.6, 0.02, 0.0},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const struct accretion_row *r = &rows[k];
    reset_config();
    setup_test_parameters(r->efficiency, r->recipe);
    TEST_ASSERT(sage26_radio_mode_heating_init() == 0, "init must accept the row's parameters");

    struct Halo halo;
    struct GalaxyData gal;
    struct ModuleContext ctx;
    setup_test_galaxy(&halo, &gal, 100.0, r->vvir, 0.2, r->hot_gas, r->black_hole_mass,
                      r->cooling_gas, 0.05, r->rheat);
    gal.CoolingX = r->x;
    setup_test_context(&ctx, 0.01);

    const double hot0 = gal.HotGas, metals0 = gal.MetalsHotGas, bh0 = gal.BlackHoleMass;
    ROW_ASSERT(sage26_radio_mode_heating_process(&ctx, &halo, 1) == 0, r, "process succeeds");

    /* The reservoirs are floats: a tolerance of two float steps of the value the row starts from */
    const double acc = r->accreted;
    const double hot_expected = hot0 - acc;
    const double metals_expected = hot0 > 0.0 ? metals0 - (metals0 / hot0) * acc : metals0;
    ROW_ASSERT(fabs((gal.BlackHoleMass - bh0) - acc) <= 2.0 * FLT_EPSILON * (bh0 + acc), r,
               "black hole growth");
    ROW_ASSERT(fabs(gal.HotGas - hot_expected) <= 2.0 * FLT_EPSILON * hot0, r, "hot gas drawn");
    ROW_ASSERT(fabs(gal.MetalsHotGas - metals_expected) <= 2.0 * FLT_EPSILON * metals0, r,
               "hot gas metals drawn at its metallicity");
    ROW_ASSERT(fabs(gal.CoolingGas - r->cooling_after) <= 2.0 * FLT_EPSILON * r->cooling_after, r,
               "cooling mass");
    ROW_ASSERT(fabs(gal.Rheat - r->rheat_after) <= 2.0 * FLT_EPSILON * r->rheat_after, r,
               "heating radius");
    ROW_ASSERT(fabs(gal.Heating - r->heating_after) <= 1.0e-6 * r->heating_after, r,
               "Heating energy");

    sage26_radio_mode_heating_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_orphan_galaxy_processed
 * @brief   SAGE parity: orphan galaxies (Type 2) are processed like any other
 *          galaxy — SAGE calls do_AGN_heating from cooling_recipe for every
 *          non-merged galaxy. With AGN on, hot gas, and a black hole present,
 *          the orphan's BH accretes (grows) and its hot gas is consumed.
 *
 * Validates: Type 2 galaxies are processed, not skipped.
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
  TEST_RUN(test_bondi_uses_carried_x);
  TEST_RUN(test_radio_accretion_rows);

  /* Run edge case tests */
  TEST_RUN(test_orphan_galaxy_processed);

  /* Run refusal tests */

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);

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
