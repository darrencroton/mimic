/**
 * @file    test_unit_sage_quasar_mode.c
 * @brief   Unit tests for sage_quasar_mode module
 *
 * Validates: black-hole growth (Kauffmann & Haehnelt 2000), its guards and the energy-driven
 * quasar wind, through both trigger channels (the disk instability's gas branch and the merger
 * event).
 *
 * The tables are exact: every expected accretion is a literal computed by hand from
 * BlackHoleGrowthRate * trigger / (1 + (280 / Vvir)^2) * ColdGas, never from the code under test.
 *
 * Test cases:
 *   - test_bh_growth_disk_instability: the instability channel grows the black hole by the exact
 *     accretion; cold gas, cold metals (metallicity preserved), the accretion tracker and the
 *     untouched trigger are asserted, and a wind too weak to eject leaves the other reservoirs
 *   - test_accretion_formula: rows over Vvir (below, at and above 280 km/s), growth rate and the
 *     cap at ColdGas (accretion above the cold gas), at QuasarModeEfficiency 0 so the wind leaves
 *     the rows alone
 *   - test_quasar_wind: ejecting the cold and hot gas, only the cold gas (the second test uses
 *     the original cold binding energy) or nothing, with exact reservoirs, through both channels
 *   - test_process_guards: NULL galaxy, ngal != 1 (an error, state untouched), ColdGas <= 0, a
 *     zero trigger and a zero or negative merger ratio (no growth), for both channels
 *   - test_agn_recipe_channels: AGNrecipe 0 grows no black hole in either channel, AGNrecipe 1,
 *     2 and 3 grow it by the exact accretion in the same setup
 *   - test_module_initialization: module_system_init accepts the module in its by-galaxy phase
 *
 * @author  Mimic Development Team
 * @date    2025-12-23
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
#include "module_system/generated/event_contracts.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double BLACK_HOLE_GROWTH_RATE;
extern double QUASAR_MODE_EFFICIENCY;

/* Module functions (extern declarations for direct testing) */
extern int sage_quasar_mode_init(void);
extern int sage_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_quasar_mode_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Setup test galaxy with specified properties
 *
 * @param   halo            Halo structure to initialize
 * @param   gal             Galaxy structure to initialize
 * @param   type            Halo type (0=central, 1=satellite, 2=orphan)
 * @param   mvir            Virial mass [1e10 Msun/h]
 * @param   vvir            Virial velocity [km/s]
 * @param   cold_gas        Cold gas mass [1e10 Msun/h]
 * @param   metals_cold     Metals in cold gas [1e10 Msun/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   metals_hot      Metals in hot gas [1e10 Msun/h]
 * @param   black_hole_mass Black hole mass [1e10 Msun/h]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal, int type, double mvir,
                              double vvir, double cold_gas, double metals_cold, double hot_gas,
                              double metals_hot, double black_hole_mass) {
  memset(halo, 0, sizeof(struct Halo));
  memset(gal, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Mvir = (float)mvir;
  halo->Vvir = (float)vvir;
  halo->SnapNum = 63;
  halo->galaxy = gal;

  gal->ColdGas = (float)cold_gas;
  gal->MetalsColdGas = (float)metals_cold;
  gal->HotGas = (float)hot_gas;
  gal->MetalsHotGas = (float)metals_hot;
  gal->BlackHoleMass = (float)black_hole_mass;
  gal->EjectedGas = 0.0;
  gal->MetalsEjectedGas = 0.0;
  gal->QuasarModeBHaccretionMass = 0.0;
  gal->UnstableDiskGasFraction = 0.0;
}

/**
 * @brief   Setup test parameters with an explicit AGNrecipe
 *
 * @param   growth_rate       Black hole growth rate
 * @param   quasar_efficiency Quasar mode efficiency
 * @param   agn_recipe        AGNrecipe (0=off, 1=empirical, 2=Bondi, 3=cold cloud)
 */
static void setup_test_parameters_agn(double growth_rate, double quasar_efficiency,
                                      int agn_recipe) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", growth_rate);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", quasar_efficiency);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "AGNrecipe");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", agn_recipe);

  MimicConfig.NumModelParams = idx;
}

/**
 * @brief   Setup test parameters with the shipped AGNrecipe (2, Bondi)
 *
 * @param   growth_rate       Black hole growth rate
 * @param   quasar_efficiency Quasar mode efficiency
 */
static void setup_test_parameters(double growth_rate, double quasar_efficiency) {
  setup_test_parameters_agn(growth_rate, quasar_efficiency, 2);
}

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx         Context to initialize
 */
static void setup_test_context(struct ModuleContext *ctx) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->substep_dt = 0.01;
  ctx->redshift = 0.0;
  ctx->time = 13.8;
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
  ctx->params = &MimicConfig;

  /* Set unit conversions (typical values) */
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5; /* 1 km/s */
  MimicConfig.Hubble_h = 0.73;
}

/** @brief A merger event of the given mass ratio, onto target 0 from source 1 */
static struct ModuleEvent merger_event(const double ratio) {
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 1,
                              .target_index = 0,
                              .value0 = ratio,
                              .value1 = 0.0};
  return event;
}

/** @brief Whether `got` is within `tol` of `want` */
static int is_close(const double got, const double want, const double tol) {
  return fabs(got - want) <= tol;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_bh_growth_disk_instability
 * @brief   The instability channel grows the black hole by the exact Kauffmann & Haehnelt
 *          accretion and moves exactly that mass out of the cold gas
 *
 * Hand calculation: BlackHoleGrowthRate 0.01, trigger 0.5, Vvir 300, ColdGas 10 give
 * 0.01 * 0.5 / (1 + (280 / 300)^2) * 10 = 0.02672209026128266. The cold metals fall by the
 * metallicity (0.2 / 10 = 0.02) times that, 0.2 - 0.02 * 0.02672209026128266 =
 * 0.19946555819477435, so the metallicity stays 0.02. The wind energy
 * 0.001 * 0.1 * 0.0267 * (C / UnitVelocity)^2 is about 2.4e5, below the cold binding energy
 * 0.5 * 9.97 * 300^2 of about 4.5e5, so nothing is ejected.
 *
 * The tolerances are the float storage of the reservoirs: about 1e-8 at the black-hole mass and
 * tracker (0.01 to 0.04), 1e-6 at ColdGas (10) and 1e-7 at the cold metals (0.2).
 *
 * Expected: the exact accretion in BlackHoleMass, ColdGas, MetalsColdGas and the tracker; the
 *           trigger unchanged
 * Validates: Disk instability trigger physics, conservation, metallicity preservation, tracking
 */
int test_bh_growth_disk_instability(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  int result = sage_quasar_mode_init();
  TEST_ASSERT(result == 0, "Module init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double want_accrete = 0.02672209026128266;
  const double initial_bh_mass = gal.BlackHoleMass;
  const double initial_cold_gas = gal.ColdGas;

  /* ===== EXECUTE ===== */
  result = sage_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process function should succeed");

  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass - initial_bh_mass, want_accrete, 1e-8,
                           "BlackHoleMass should grow by the Kauffmann & Haehnelt accretion");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, 10.0 - want_accrete, 1e-6,
                           "ColdGas should fall by exactly the accretion");
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass - initial_bh_mass, initial_cold_gas - gal.ColdGas,
                           1e-6, "Accreted mass should equal consumed cold gas");
  TEST_ASSERT_DOUBLE_EQUAL(gal.QuasarModeBHaccretionMass, want_accrete, 1e-8,
                           "QuasarModeBHaccretionMass should track the accretion");
  TEST_ASSERT_DOUBLE_EQUAL(gal.MetalsColdGas, 0.19946555819477435, 1e-7,
                           "MetalsColdGas should fall by the cold metallicity times the accretion");
  TEST_ASSERT_DOUBLE_EQUAL(gal.MetalsColdGas / gal.ColdGas, 0.02, 1e-7,
                           "The cold-gas metallicity should be preserved");
  TEST_ASSERT(gal.EjectedGas == 0.0f && gal.HotGas == 0.0f,
              "A wind below the cold binding energy ejects nothing");

  /* Trigger lifecycle is owned by clear modules, not quasar_mode */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "Disk-instability trigger should remain unchanged");

  /* ===== CLEANUP ===== */
  sage_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_accretion_formula
 * @brief   The accretion BlackHoleGrowthRate * trigger / (1 + (280 / Vvir)^2) * ColdGas and its
 *          cap at ColdGas, row by row
 *
 * QuasarModeEfficiency is 0, so the wind has no energy and ejects nothing: each row's ColdGas and
 * MetalsColdGas come from the growth alone. Every accretion is a hand-computed literal (ColdGas
 * 10, the rate and trigger shown):
 *   Vvir 300:  0.01 * 0.5 / (1 + (280 / 300)^2) * 10 = 0.02672209026128266
 *   Vvir 280:  0.01 * 0.5 / 2 * 10 = 0.025, exactly half of the unsuppressed rate
 *   Vvir 100:  0.01 * 0.5 / (1 + 2.8^2) * 10 = 0.005656108597285069 (low Vvir suppresses)
 *   Vvir 400:  0.01 * 0.5 / (1 + 0.7^2) * 10 = 0.03355704697986577
 *   rate 0.005 and 0.02 at Vvir 300: half and twice the first row
 *   cap:       0.5 * 4 / (1 + 0.28^2) * 10 = 18.5459940652819, above the ColdGas of 10, so the
 *              accretion is the whole 10 and the cold gas and cold metals reach 0
 * Cold metals fall by the cold metallicity 0.02 times the accretion: 0.2 * (1 - accretion / 10).
 *
 * The tolerances are the float storage of the reservoirs: 1e-5 relative at the accretion, 2e-6 at
 * ColdGas (10) and 1e-7 at the cold metals (0.2).
 *
 * Expected: every row's accretion, ColdGas and MetalsColdGas, and nothing ejected
 */
int test_accretion_formula(void) {
  const struct {
    const char *name;
    double rate, trigger, vvir;
    double want_accrete, want_metals, metals_tol;
  } rows[] = {
      {"Vvir 300", 0.01, 0.5, 300.0, 0.02672209026128266, 0.19946555819477435, 1e-7},
      {"Vvir 280", 0.01, 0.5, 280.0, 0.025, 0.1995, 1e-7},
      {"Vvir 100", 0.01, 0.5, 100.0, 0.005656108597285069, 0.1998868778280543, 1e-7},
      {"Vvir 400", 0.01, 0.5, 400.0, 0.03355704697986577, 0.19932885906040268, 1e-7},
      {"rate 0.005", 0.005, 0.5, 300.0, 0.01336104513064133, 0.19973277909738718, 1e-7},
      {"rate 0.02", 0.02, 0.5, 300.0, 0.05344418052256532, 0.19893111638954872, 1e-7},
      {"cap at ColdGas", 0.5, 4.0, 1000.0, 10.0, 0.0, 1e-6},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters(rows[r].rate, 0.0);
    TEST_ASSERT(sage_quasar_mode_init() == 0, "init must accept QuasarModeEfficiency 0");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, rows[r].vvir, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.UnstableDiskGasFraction = rows[r].trigger;
    struct ModuleContext ctx;
    setup_test_context(&ctx);
    const double initial_bh_mass = gal.BlackHoleMass;

    const int status = sage_quasar_mode_process(&ctx, &halo, 1);

    const double tol = 1e-5 * rows[r].want_accrete;
    const int row_ok = status == 0 &&
                       is_close(gal.QuasarModeBHaccretionMass, rows[r].want_accrete, tol) &&
                       is_close(gal.BlackHoleMass - initial_bh_mass, rows[r].want_accrete, tol) &&
                       is_close(gal.ColdGas, 10.0 - rows[r].want_accrete, 2e-6) &&
                       is_close(gal.MetalsColdGas, rows[r].want_metals, rows[r].metals_tol) &&
                       gal.EjectedGas == 0.0f;
    if (!row_ok) {
      printf("\n  %s: status %d, accretion %.9g (want %.9g), BlackHoleMass growth %.9g, ColdGas "
             "%.9g, MetalsColdGas %.9g (want %.9g), EjectedGas %.9g",
             rows[r].name, status, (double)gal.QuasarModeBHaccretionMass, rows[r].want_accrete,
             gal.BlackHoleMass - initial_bh_mass, (double)gal.ColdGas, (double)gal.MetalsColdGas,
             rows[r].want_metals, (double)gal.EjectedGas);
      ok = 0;
    }
    sage_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each row's accretion, cold gas and cold metals must match its hand calculation");
  return TEST_PASS;
}

/**
 * @test    test_quasar_wind
 * @brief   The wind ejects the cold gas when its energy exceeds the cold binding energy and the
 *          hot gas as well when it exceeds the cold plus hot binding energies, through both
 *          channels
 *
 * The growth makes every reservoir exact: BlackHoleGrowthRate 0.5, a trigger (or merger ratio)
 * of 0.5 and Vvir 280 give 0.5 * 0.5 / (1 + (280 / 280)^2) * 1 = 0.125 of ColdGas 1, so 0.875 of
 * cold gas is left (no cold metals). The cold binding energy is then exactly
 * 0.5 * 0.875 * 280 * 280 = 34300, and a hot reservoir of gas G binds 39200 * G. The wind energy
 * QuasarModeEfficiency * 0.1 * 0.125 * (C / UnitVelocity)^2 (C = 2.9979e10 cm/s, UnitVelocity
 * 1e5) is about 1.1234e6 for an efficiency of 1e-3 and 1.1234e4 for 1e-5. The test forms it from
 * that expression and asserts each row's side of both tests before asserting the state:
 *   eject both:  efficiency 1e-3, hot gas 1:     1.1234e6 > 34300 + 39200 = 73500
 *   cold only:   efficiency 1e-3, hot gas 28.2:  34300 < 1.1234e6 < 34300 + 1105440, while
 *                1.1234e6 > 1105440 alone (a wind that subtracted the cold ejection, or
 *                recomputed the cold energy after it, would also eject the hot gas)
 *   neither:     efficiency 1e-5, hot gas 1:     1.1234e4 < 34300
 *
 * Expected: BlackHoleMass and the tracker 0.125; ColdGas, HotGas, EjectedGas and the metals
 *           follow the row, exactly
 * Validates: Energy-driven wind physics, the original cold energy in the second test, the merger
 *            channel's growth
 */
int test_quasar_wind(void) {
  const struct {
    const char *name;
    double efficiency;
    float hot_gas, hot_metals;
    int ejects_cold, ejects_hot;
  } rows[] = {{"eject both", 1.0e-3, 1.0f, 0.125f, 1, 1},
              {"cold only", 1.0e-3, 28.2f, 0.25f, 1, 0},
              {"neither", 1.0e-5, 1.0f, 0.125f, 0, 0}};
  const double c_over_unit_velocity = 2.9979e10 / 1.0e5;
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const double e_wind =
        rows[r].efficiency * 0.1 * 0.125 * c_over_unit_velocity * c_over_unit_velocity;
    const double e_cold = 0.5 * 0.875 * 280.0 * 280.0;
    const double e_hot = 0.5 * rows[r].hot_gas * 280.0 * 280.0;
    TEST_ASSERT((e_wind > e_cold) == rows[r].ejects_cold, "the row's cold test outcome");
    TEST_ASSERT((e_wind > e_cold + e_hot) == rows[r].ejects_hot, "the row's hot test outcome");
    if (rows[r].ejects_cold && !rows[r].ejects_hot) {
      TEST_ASSERT(e_wind > e_hot, "only the original cold energy keeps the hot gas from ejecting");
    }

    for (int channel = 0; channel < 2; channel++) { /* 0 the instability, 1 the merger event */
      reset_config();
      setup_test_parameters(0.5, rows[r].efficiency);
      TEST_ASSERT(sage_quasar_mode_init() == 0, "init must succeed");

      struct Halo halo;
      struct GalaxyData gal;
      setup_test_galaxy(&halo, &gal, 1, 20.0, 280.0, 1.0, 0.0, rows[r].hot_gas, rows[r].hot_metals,
                        0.0);
      gal.EjectedGas = 0.5f;
      gal.MetalsEjectedGas = 0.0625f;

      struct ModuleContext ctx;
      setup_test_context(&ctx);
      struct ModuleEvent event = merger_event(0.5);
      if (channel == 1) {
        ctx.active_event = &event;
      } else {
        gal.UnstableDiskGasFraction = 0.5;
      }

      const int status = sage_quasar_mode_process(&ctx, &halo, 1);

      const float cold_out = rows[r].ejects_cold ? 0.0f : 0.875f;
      const float hot_out = rows[r].ejects_hot ? 0.0f : rows[r].hot_gas;
      const float hot_metals_out = rows[r].ejects_hot ? 0.0f : rows[r].hot_metals;
      const float ejected_out = 0.5f + (rows[r].ejects_cold ? 0.875f : 0.0f) +
                                (rows[r].ejects_hot ? rows[r].hot_gas : 0.0f);
      const float ejected_metals_out = 0.0625f + (rows[r].ejects_hot ? rows[r].hot_metals : 0.0f);
      const int row_ok = status == 0 && gal.BlackHoleMass == 0.125f &&
                         gal.QuasarModeBHaccretionMass == 0.125f && gal.ColdGas == cold_out &&
                         gal.MetalsColdGas == 0.0f && gal.HotGas == hot_out &&
                         gal.MetalsHotGas == hot_metals_out && gal.EjectedGas == ejected_out &&
                         gal.MetalsEjectedGas == ejected_metals_out;
      if (!row_ok) {
        printf("\n  %s, channel %d: status %d, BlackHoleMass %.9g, cold %.9g, hot %.9g/%.9g, "
               "ejected %.9g/%.9g",
               rows[r].name, channel, status, (double)gal.BlackHoleMass, (double)gal.ColdGas,
               (double)gal.HotGas, (double)gal.MetalsHotGas, (double)gal.EjectedGas,
               (double)gal.MetalsEjectedGas);
        ok = 0;
      }
      sage_quasar_mode_cleanup();
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the wind must eject exactly the reservoirs its two energy tests select");
  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_process_guards
 * @brief   The guards of both channels leave the galaxy untouched: a NULL galaxy, ngal != 1 (an
 *          error), ColdGas <= 0, a zero instability trigger and a zero or negative merger ratio
 *
 * Every row sets a trigger that would otherwise grow the black hole (0.5, or the row's value for
 * the trigger rows) on a galaxy with positive cold gas (or the row's ColdGas). Without a ColdGas
 * guard a negative ColdGas would shrink the black hole, so each row compares the whole GalaxyData
 * byte for byte after the call, not just the black-hole mass.
 *
 * Expected: status 0 (guard returns) or an error (ngal != 1); the first galaxy and, for ngal 2,
 *           the second are unchanged
 * Validates: process_by_galaxy and process_per_event guards
 */
int test_process_guards(void) {
  const struct {
    const char *name;
    int per_event, ngal, null_galaxy;
    float cold_gas;
    double trigger;
    int want_error;
  } rows[] = {
      {"by-galaxy, NULL galaxy", 0, 1, 1, 10.0f, 0.5, 0},
      {"per-event, NULL galaxy", 1, 1, 1, 10.0f, 0.5, 0},
      {"by-galaxy, ngal 2", 0, 2, 0, 10.0f, 0.5, 1},
      {"per-event, ngal 2", 1, 2, 0, 10.0f, 0.5, 1},
      {"by-galaxy, zero ColdGas", 0, 1, 0, 0.0f, 0.5, 0},
      {"by-galaxy, negative ColdGas", 0, 1, 0, -1.0f, 0.5, 0},
      {"per-event, zero ColdGas", 1, 1, 0, 0.0f, 0.5, 0},
      {"per-event, negative ColdGas", 1, 1, 0, -1.0f, 0.5, 0},
      {"by-galaxy, zero trigger", 0, 1, 0, 10.0f, 0.0, 0},
      {"per-event, zero ratio", 1, 1, 0, 10.0f, 0.0, 0},
      {"per-event, negative ratio", 1, 1, 0, 10.0f, -0.5, 0},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters(0.01, 0.001);
    TEST_ASSERT(sage_quasar_mode_init() == 0, "init must succeed");

    struct Halo halos[2];
    struct GalaxyData gals[2];
    setup_test_galaxy(&halos[0], &gals[0], 0, 100.0, 300.0, rows[r].cold_gas, 0.2, 0.0, 0.0, 0.01);
    setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 0.0, 0.0, 0.005);
    gals[0].UnstableDiskGasFraction = rows[r].trigger;
    gals[1].UnstableDiskGasFraction = rows[r].trigger;
    if (rows[r].null_galaxy) {
      halos[0].galaxy = NULL;
    }

    struct ModuleContext ctx;
    setup_test_context(&ctx);
    struct ModuleEvent event = merger_event(rows[r].trigger);
    if (rows[r].per_event) {
      ctx.active_event = &event;
    }

    struct GalaxyData before[2];
    memcpy(before, gals, sizeof(gals));
    const int status = sage_quasar_mode_process(&ctx, halos, rows[r].ngal);

    const int status_ok = rows[r].want_error ? status != 0 : status == 0;
    const int untouched = memcmp(before, gals, sizeof(gals)) == 0;
    if (!status_ok || !untouched) {
      printf("\n  %s: status %d (error wanted: %d), galaxies untouched: %d", rows[r].name, status,
             rows[r].want_error, untouched);
      ok = 0;
    }
    sage_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each guard must return as wanted and leave the galaxies untouched");
  return TEST_PASS;
}

/**
 * @test    test_agn_recipe_channels
 * @brief   AGNrecipe 0 grows no black hole in either channel, as SAGE gates grow_black_hole() on
 *          AGNrecipeOn after a merger and in the disk-instability check; AGNrecipe 1, 2 and 3
 *          grow it by the exact accretion in the same setup
 *
 * Hand calculation (BlackHoleGrowthRate 0.01, Vvir 300, ColdGas 10): a merger ratio of 0.3 gives
 * 0.01 * 0.3 / (1 + (280 / 300)^2) * 10 = 0.016033254156769594 and an instability trigger of 0.5
 * gives 0.02672209026128266. The AGNrecipe 1, 2 and 3 rows are the controls that the same input
 * grows a black hole, so the gate tests for 0 and not for "not 2".
 *
 * Expected: AGNrecipe 0: success; BlackHoleMass, ColdGas, MetalsColdGas and
 *           QuasarModeBHaccretionMass unchanged; otherwise the exact accretion
 * Validates: the AGNrecipe gate of both channels
 */
int test_agn_recipe_channels(void) {
  const struct {
    const char *name;
    int agn_recipe;
    int per_event;
    double want_accrete;
  } rows[] = {
      {"AGNrecipe 2, merger event", 2, 1, 0.016033254156769594},
      {"AGNrecipe 0, merger event", 0, 1, 0.0},
      {"AGNrecipe 3, merger event", 3, 1, 0.016033254156769594},
      {"AGNrecipe 2, disk instability", 2, 0, 0.02672209026128266},
      {"AGNrecipe 0, disk instability", 0, 0, 0.0},
      {"AGNrecipe 1, disk instability", 1, 0, 0.02672209026128266},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters_agn(0.01, 0.001, rows[r].agn_recipe);
    TEST_ASSERT(sage_quasar_mode_init() == 0, "init must accept AGNrecipe 0 to 3");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    struct ModuleContext ctx;
    setup_test_context(&ctx);
    struct ModuleEvent event = merger_event(0.3);
    if (rows[r].per_event) {
      ctx.active_event = &event;
    } else {
      gal.UnstableDiskGasFraction = 0.5;
    }
    const float initial_bh = gal.BlackHoleMass;
    const float initial_cold = gal.ColdGas;
    const float initial_metals = gal.MetalsColdGas;

    const int status = sage_quasar_mode_process(&ctx, &halo, 1);

    int row_ok = status == 0;
    if (rows[r].want_accrete == 0.0) {
      row_ok = row_ok && gal.BlackHoleMass == initial_bh && gal.ColdGas == initial_cold &&
               gal.MetalsColdGas == initial_metals && gal.QuasarModeBHaccretionMass == 0.0f;
    } else {
      const double tol = 1e-5 * rows[r].want_accrete;
      row_ok = row_ok && is_close(gal.QuasarModeBHaccretionMass, rows[r].want_accrete, tol) &&
               is_close(gal.BlackHoleMass - initial_bh, rows[r].want_accrete, tol) &&
               is_close(gal.ColdGas, initial_cold - rows[r].want_accrete, 2e-6);
    }
    if (!row_ok) {
      printf("\n  %s: status %d, BlackHoleMass growth %.9g, ColdGas %.9g, MetalsColdGas %.9g, "
             "tracker %.9g (accretion wanted %.9g)",
             rows[r].name, status, (double)gal.BlackHoleMass - initial_bh, (double)gal.ColdGas,
             (double)gal.MetalsColdGas, (double)gal.QuasarModeBHaccretionMass,
             rows[r].want_accrete);
      ok = 0;
    }
    sage_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "AGNrecipe 0 must grow no black hole and the others must grow it exactly");
  return TEST_PASS;
}

// ============================================================================
// MODULE LIFECYCLE
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

  test_phase_add("galaxy_physics", "sage_quasar_mode", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters */
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.01");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.001");
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

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_quasar_mode Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Growth physics, wind and edge cases */
  TEST_RUN(test_bh_growth_disk_instability);
  TEST_RUN(test_accretion_formula);
  TEST_RUN(test_quasar_wind);
  TEST_RUN(test_process_guards);
  TEST_RUN(test_agn_recipe_channels);

  /* Module lifecycle */
  TEST_RUN(test_module_initialization);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
