/**
 * @file    test_unit_sage_radio_mode_heating.c
 * @brief   Unit tests for sage_radio_mode_heating module
 *
 * Validates: the radio-mode AGN call under each accretion recipe, its caps and guards
 *
 * Expected values are hand-derived literals or SAGE's expressions written inline, never a call of
 * the code under test.
 *
 * Test cases:
 *   - test_cold_cloud_agn_rate_below_threshold: no accretion when the black hole is below the cold
 *     cloud threshold
 *   - test_eddington_limit: the accretion is the Eddington rate times dt when the empirical rate
 *     exceeds it
 *   - test_radio_accretion_rows: a table over the empirical, Bondi and cold cloud rates where no
 *     cap binds, the heating and reservoir caps, the hot-gas metals debit, the Rheat suppression
 *     (partial, complete, held ratchet), the efficiency, AGNrecipe 0 and the zero cooling, hot gas
 *     and black hole guards, each asserting the black hole, hot pair, cooling mass, Rheat and
 *     Heating
 *   - test_orphan_galaxy_processed: Type 2 galaxies are processed like centrals (SAGE parity)
 *   - test_module_initialization: module_system_init accepts the module's placement
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
#include <string.h>
#include <math.h>
#include <float.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double RADIO_MODE_EFFICIENCY;
extern int AGN_RECIPE_ON;

/* Module functions (extern declarations for direct testing) */
extern int sage_radio_mode_heating_init(void);
extern int sage_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_radio_mode_heating_cleanup(void);

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
}

/**
 * @brief   Setup test parameters
 *
 * @param   efficiency  Radio mode efficiency
 * @param   recipe      AGN recipe (0=off, 1=empirical, 2=Bondi, 3=cold cloud)
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
  MimicConfig.G = 43.0071;                       /* the Bondi rate's G */
  MimicConfig.UnitDensity_in_cgs = 6.769911e-22; /* the Bondi rate's x */
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_cold_cloud_agn_rate_below_threshold
 * @brief   Test cold cloud AGN accretion when BH mass below threshold
 *
 * Expected: No AGN accretion: the black hole stays as it was
 * Validates: Cold cloud threshold logic (BH > 1e-4 Mvir (Rcool / Rvir)^3 triggers it)
 */
int test_cold_cloud_agn_rate_below_threshold(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.08, 3); /* Cold cloud mode */
  sage_radio_mode_heating_init();

  /* Setup galaxy where BH is below cold cloud threshold */
  struct Halo halo;
  struct GalaxyData gal;
  const double mvir = 100.0;
  const double vvir = 200.0;
  const double rvir = 0.2;
  const double rcool = 0.05;
  const double hot_gas = 10.0;
  const double cooling_gas = 1.0;

  /* Calculate threshold and set BH mass below it */
  const double threshold = 0.0001 * mvir * pow(rcool / rvir, 3.0);
  const double black_hole_mass = threshold * 0.5; /* Below threshold */

  setup_test_galaxy(&halo, &gal, mvir, vvir, rvir, hot_gas, black_hole_mass, cooling_gas, rcool,
                    0.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                           "BH mass should not change when below threshold");

  /* ===== CLEANUP ===== */
  sage_radio_mode_heating_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_eddington_limit
 * @brief   Test that AGN accretion is limited by Eddington rate
 *
 * Expected: Accretion limited even with high cooling rate
 * Validates: Eddington rate limiting physics
 */
int test_eddington_limit(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Use maximum valid efficiency */
  setup_test_parameters(1.0, 1); /* Maximum valid efficiency */
  sage_radio_mode_heating_init();

  /* Small BH mass = low Eddington rate */
  struct Halo halo;
  struct GalaxyData gal;
  const double black_hole_mass = 0.0001; /* Very small BH */
  const double hot_gas = 10.0;
  const double cooling_gas = 10.0; /* Very high cooling */

  /* Vvir 400: the empirical rate (8 times that at 200 km/s, 7.8e-3) is 2.5 times the Eddington
   * rate (3.1e-3), and the heating (3.4 of the cooling mass 10) does not cap the accretion */
  setup_test_galaxy(&halo, &gal, 100.0, 400.0, 0.2, hot_gas, black_hole_mass, cooling_gas, 0.05,
                    0.0);

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
  sage_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  const double accreted = gal.BlackHoleMass - initial_bh_mass;
  TEST_ASSERT(accreted > 0.0, "Some accretion should occur");
  TEST_ASSERT(accreted <= max_accretion * 1.01, "Accretion should be Eddington-limited");
  /* 1e-5 relative: the float BlackHoleMass store */
  TEST_ASSERT_DOUBLE_EQUAL(accreted, max_accretion, 1e-5 * max_accretion,
                           "Accretion should equal the Eddington rate times dt");

  /* ===== CLEANUP ===== */
  sage_radio_mode_heating_cleanup();
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
 * @brief One radio-mode call and the galaxy it must leave
 *
 * The substep is dt = 0.01 (also the halo's dT), Rcool is 0.05, and the hot gas carries a
 * metallicity of 0.02. The expectations are hand-derived literals or expressions of SAGE's chain:
 * the accretion rate of the recipe (below), the Eddington cap, the cap at the hot gas, the heating
 * coefficient (1.34e5 / Vvir)^2 (448900 at Vvir 200), the cap of the heating to the suppressed
 * cooling mass, the suppression (1 - Rheat / Rcool) CoolingGas, the Rheat ratchet
 * (AGNheating / suppressed) Rcool and Heating += 0.5 AGNheating Vvir^2 / dT.
 *
 * The rates, with the test's units (UnitMass 1.989e43 g, UnitTime 3.08568e16 s, so a unit
 * conversion of 10.22465):
 *   empirical (1): eff / 10.22465 (BH / 0.01) (Vvir / 200)^3 ((HotGas / Mvir) / 0.1), without the
 *                  hot fraction when Mvir is 0
 *   Bondi (2):     (2.5 pi G)(0.375 * 0.6 x) BH eff, x = proton mass k_B (35.9 Vvir^2) / Lambda
 *                  / (UnitDensity UnitTime), with G 43.0071 and UnitDensity 6.769911e-22
 *   cold cloud (3): 1e-4 CoolingGas / dt when BH > 1e-4 Mvir (Rcool / Rvir)^3 (Mvir 100, Rcool
 *                  0.05, Rvir 0.2 give a threshold of 1.5625e-4), else 0
 */
struct accretion_row {
  const char *name;
  /* the call */
  int recipe;        /**< AGNrecipe */
  double efficiency; /**< RadioModeEfficiency */
  double mvir;
  double vvir; /**< Vvir [km/s] */
  double hot_gas;
  double black_hole_mass;
  double cooling_gas;
  double rheat;
  double lambda; /**< the carried cooling function (Bondi only) */
  /* the galaxy afterwards */
  double accreted;      /**< black hole growth, and the hot gas and its metals drawn */
  double cooling_after; /**< CoolingGas */
  double rheat_after;
  double heating_after; /**< Heating, divided by dT */
};

/**
 * @test    test_radio_accretion_rows
 * @brief   The three accretion recipes, the caps, the ratchet, Heating, the metals debit, the
 *          Rheat suppression and the guards, one row per input, each asserted against the whole
 *          galaxy
 *
 * Validates: the Bondi, empirical and cold cloud rates at values where no cap binds (so the rate
 *            is observed), and each cap and guard where it does
 */
int test_radio_accretion_rows(void) {
  static const struct accretion_row rows[] = {
      /* Empirical rate 7.8242e-5 with a black hole of 1e-4 (far below the Eddington rate), so
       * accreted = 7.8242e-7 and AGNheating = 448900 accreted = 0.35123, below the cooling mass
       * 1.0; Rheat ratchets to 0.35123 / 1.0 * 0.05 */
      {"empirical_below_caps", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.0, 0.0, 7.824228012e-7,
       1.0, 0.0175614804, 702459.1909},
      /* The rate goes as Vvir^3 (1.5^3 = 3.375 times) and the heating coefficient as Vvir^-2 */
      {"empirical_vvir_300", 1, 0.08, 100.0, 300.0, 10.0, 1.0e-4, 1.0, 0.0, 0.0, 2.640676954e-6,
       1.0, 0.0263422206, 2370799.769},
      /* Half the hot fraction halves the rate */
      {"empirical_half_hot_fraction", 1, 0.08, 100.0, 200.0, 5.0, 1.0e-4, 1.0, 0.0, 0.0,
       3.912114006e-7, 1.0, 0.008780740201, 351229.5954},
      /* Mvir 0 drops the hot fraction: the rate of the first row at half its hot gas */
      {"empirical_mvir_zero", 1, 0.08, 0.0, 200.0, 5.0, 1.0e-4, 1.0, 0.0, 0.0, 7.824228012e-7, 1.0,
       0.0175614804, 702459.1909},
      /* The heating cap: a black hole of 0.01 would heat 1.0 and more than the cooling mass 1.0,
       * so accreted falls to 1.0 / 448900, the heating to 1.0 and Rheat to Rcool */
      {"empirical_heating_cap", 1, 0.08, 100.0, 200.0, 10.0, 0.01, 1.0, 0.0, 0.0, 1.0 / 448900.0,
       1.0, 0.05, 2000000.0},
      /* Bondi at Lambda 1e-26: x = 1.58e-3 and a rate of 9.6512e-5 */
      {"bondi_below_caps", 2, 0.08, 100.0, 200.0, 10.0, 0.01, 1.0, 0.0, 1.0e-26, 9.651239478e-7,
       1.0, 0.02166220732, 866488.2803},
      /* Cold cloud above the threshold, at Vvir 2000 so that the heating (0.4489 of the cooling
       * mass) is not capped: accreted = 1e-4 CoolingGas whatever dt */
      {"cold_cloud_above", 3, 0.08, 100.0, 2000.0, 10.0, 0.01, 1.0, 0.0, 0.0, 1.0e-4, 1.0,
       0.02244500071, 89780000.0},
      /* The reservoir cap: only 5e-5 of hot gas against 1e-4 of accretion, so the hot gas and
       * its metals are emptied */
      {"cold_cloud_reservoir_cap", 3, 0.08, 100.0, 2000.0, 5.0e-5, 0.01, 1.0, 0.0, 0.0,
       4.999999874e-5, 1.0, 0.01122249942, 44889998.87},
      /* The metals debit: 1e-3 of accretion from hot gas at metallicity 0.02 lowers
       * MetalsHotGas by 2e-5 (a float step at 0.2 is 1.5e-8) */
      {"metals_debit", 3, 0.08, 100.0, 2000.0, 10.0, 0.01, 10.0, 0.0, 0.0, 1.0e-3, 10.0,
       0.02244500071, 897800000.0},
      /* Rheat 0.01 suppresses the mass to 0.8 and the ratchet (0.35123 / 0.8 * 0.05) raises Rheat
       * past 0.01; Rheat 0.025 halves the mass; with a mass of 100 suppressed to 50 the ratchet
       * value is below the Rheat held, which stays */
      {"rheat_raised", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.01, 0.0, 7.824228012e-7, 0.8,
       0.0219518505, 702459.1909},
      {"rheat_half", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.025, 0.0, 7.824228012e-7, 0.5,
       0.03512296081, 702459.1909},
      {"rheat_held", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 100.0, 0.025, 0.0, 7.824228012e-7, 50.0,
       0.025, 702459.1909},
      /* Rheat at and above Rcool suppresses the cooling mass completely: nothing is left to
       * heat, so the heating cap leaves no accretion */
      {"rheat_equals_rcool", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.05, 0.0, 0.0, 0.0, 0.05,
       0.0},
      {"rheat_above_rcool", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.06, 0.0, 0.0, 0.0, 0.06,
       0.0},
      /* The accretion is linear in the efficiency (the 0.08 rows above are the middle) */
      {"efficiency_half", 1, 0.04, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.0, 0.0, 3.912114006e-7, 1.0,
       0.008780740201, 351229.5954},
      {"efficiency_double", 1, 0.16, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.0, 0.0, 1.564845602e-6, 1.0,
       0.03512296081, 1404918.382},
      {"efficiency_zero", 1, 0.0, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0},
      /* AGNrecipe 0 returns before touching anything: the mass is not suppressed (0.6 would be
       * the suppressed value at Rheat 0.02), Rheat stays and nothing accretes */
      {"agn_off", 0, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 1.0, 0.02, 0.0, 0.0, 1.0, 0.02, 0.0},
      /* The guards: no cooling mass leaves the galaxy as it was; no hot gas or no black hole
       * accretes nothing, but the mass is still suppressed (1 - 0.02 / 0.05 = 0.6) and Rheat is
       * not ratcheted */
      {"zero_cooling", 1, 0.08, 100.0, 200.0, 10.0, 1.0e-4, 0.0, 0.02, 0.0, 0.0, 0.0, 0.02, 0.0},
      {"zero_hot_gas", 1, 0.08, 100.0, 200.0, 0.0, 1.0e-4, 1.0, 0.02, 0.0, 0.0, 0.6, 0.02, 0.0},
      {"zero_black_hole", 1, 0.08, 100.0, 200.0, 10.0, 0.0, 1.0, 0.02, 0.0, 0.0, 0.6, 0.02, 0.0},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const struct accretion_row *r = &rows[k];
    reset_config();
    setup_test_parameters(r->efficiency, r->recipe);
    TEST_ASSERT(sage_radio_mode_heating_init() == 0, "init must accept the row's parameters");

    struct Halo halo;
    struct GalaxyData gal;
    struct ModuleContext ctx;
    setup_test_galaxy(&halo, &gal, r->mvir, r->vvir, 0.2, r->hot_gas, r->black_hole_mass,
                      r->cooling_gas, 0.05, r->rheat);
    gal.CoolingLambda = r->lambda;
    setup_test_context(&ctx, 0.01);

    const double hot0 = gal.HotGas, metals0 = gal.MetalsHotGas, bh0 = gal.BlackHoleMass;
    ROW_ASSERT(sage_radio_mode_heating_process(&ctx, &halo, 1) == 0, r, "process succeeds");

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

    sage_radio_mode_heating_cleanup();
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

  setup_test_parameters(0.08, 1);
  sage_radio_mode_heating_init();

  struct Halo halo;
  struct GalaxyData gal;

  setup_test_galaxy(&halo, &gal, 100.0, 200.0, 0.2, 10.0, 0.01, 1.0, 0.05, 0.0);

  halo.Type = 2; /* Orphan galaxy */

  struct ModuleContext ctx;
  setup_test_context(&ctx, 0.01);

  const double initial_bh_mass = gal.BlackHoleMass;
  const double initial_hot_gas = gal.HotGas;

  /* ===== EXECUTE ===== */
  int result = sage_radio_mode_heating_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module should process the Type 2 orphan");
  TEST_ASSERT(gal.BlackHoleMass > initial_bh_mass,
              "Orphan BH should accrete (grow) — orphan is processed");
  TEST_ASSERT(gal.HotGas < initial_hot_gas, "Orphan hot gas should be consumed by BH accretion");
  TEST_ASSERT(gal.BlackHoleMass >= 0.0 && gal.HotGas >= 0.0, "Orphan reservoirs remain physical");

  /* ===== CLEANUP ===== */
  sage_radio_mode_heating_cleanup();
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

  test_phase_add("galaxy_physics", "sage_radio_mode_heating", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters */
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RadioModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.08");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "AGNrecipe");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "1");
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
 * @brief   Main test runner
 *
 * Executes all sage_radio_mode_heating unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_radio_mode_heating Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Run physics calculation tests */
  TEST_RUN(test_cold_cloud_agn_rate_below_threshold);
  TEST_RUN(test_eddington_limit);
  TEST_RUN(test_radio_accretion_rows);

  /* Run edge case tests */
  TEST_RUN(test_orphan_galaxy_processed);

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
