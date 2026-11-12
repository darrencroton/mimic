/**
 * @file    test_unit_sage26_quasar_mode.c
 * @brief   Unit tests for sage26_quasar_mode module
 *
 * Validates: black-hole growth (Kauffmann & Haehnelt 2000), its guards and the quasar-mode wind of
 * SAGE26's grow_black_hole, through both trigger channels (the disk instability's gas branch and
 * the merger event).
 *
 * Most cases run the module at SFprescription 0 and CGMrecipeOn 0. The tables are exact: every
 * expected accretion is a literal computed by hand from BlackHoleGrowthRate * trigger /
 * (1 + (280 / Vvir)^2) * ColdGas, never from the code under test.
 *
 * Test cases:
 *   - test_bh_growth_disk_instability: the instability channel grows the black hole by the exact
 *     accretion; cold gas, cold metals (metallicity preserved), the accretion tracker and the
 *     untouched trigger are asserted, and a wind too weak to eject leaves the other reservoirs
 *   - test_accretion_formula: rows over Vvir (below, at and above 280 km/s), growth rate, the cap
 *     at ColdGas (accretion above the cold gas) and a negative cold-metal reservoir floored at 0
 *     after a real accretion, at QuasarModeEfficiency 0 so the wind leaves the rows alone
 *   - test_per_event_zero_ratio_accretes_nothing: a zero-ratio merger runs grow_black_hole, which
 *     still floors a negative MetalsColdGas
 *   - test_no_triggers: a zero instability trigger does not run grow_black_hole (it leaves a
 *     negative MetalsColdGas alone)
 *   - test_process_guards: NULL galaxy, ngal != 1 (an error, state untouched) and ColdGas <= 0 (no
 *     growth), for the instability channel and the merger event
 *   - test_hydrogen_cap_independent_strict: SFprescription 1 caps H2gas and H1gas each on its own,
 *     strictly, at the float 0.74 of the cold gas the accretion leaves
 *   - test_hydrogen_cap_off_prescription_zero: SFprescription 0 leaves H2gas and H1gas alone
 *   - test_hydrogen_cap_then_wind_no_refresh: the cap precedes the wind, which refreshes nothing
 *   - test_hydrogen_cap_module_wiring: each channel passes the module's SFprescription to the
 *     growth (0 leaves the hydrogen alone, 1 caps it)
 *   - test_cgm_wind_energy_tests: CGMrecipeOn 1 against 0 on the same input, through both channels
 *     (the instability and the merger event): the wind's cold test and its second test on the
 *     WIND HOST's CGM pair (host Regime 0) or hot pair (Regime 1, or CGMrecipeOn 0), ejecting both,
 *     only the cold gas (the original cold binding energy in the second test) or nothing; the FoF
 *     central always holds the other Regime and is untouched; the unselected pair unchanged
 *   - test_agn_recipe_channels: AGNrecipe 0 grows no black hole in either channel, AGNrecipe 2
 *     grows it by the exact accretion in the same setup
 *   - test_module_initialization: module_system_init accepts the module in its by-galaxy phase
 *
 * The module's CGMrecipeOn and AGNrecipe refusals are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
 *
 * The hydrogen-cap cases assert the cap's invariants from the package's own float arithmetic.
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
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_agn_physics.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double BLACK_HOLE_GROWTH_RATE;
extern double QUASAR_MODE_EFFICIENCY;

/* Module functions (extern declarations for direct testing) */
extern int sage26_quasar_mode_init(void);
extern int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_quasar_mode_cleanup(void);

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
 * @brief   Setup test parameters
 *
 * @param   growth_rate       Black hole growth rate
 * @param   quasar_efficiency Quasar mode efficiency
 */
static void setup_test_parameters(double growth_rate, double quasar_efficiency) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", growth_rate);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", quasar_efficiency);

  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_QUASAR_MODULE);
  add_test_cgm_recipe_parameter();                      /* CGMrecipeOn: 0, required */
  sage26_test_append_model_parameter("AGNrecipe", "2"); /* required: 0 or 2 */
}

/** @brief Set the parameter list's `name` (setup_test_parameters adds it) to `value` */
static void set_model_parameter(const char *name, const char *value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, name) == 0) {
      snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
    }
  }
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
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
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
  int result = sage26_quasar_mode_init();
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
  result = sage26_quasar_mode_process(&ctx, &halo, 1);

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
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_accretion_formula
 * @brief   The accretion BlackHoleGrowthRate * trigger / (1 + (280 / Vvir)^2) * ColdGas, its cap
 *          at ColdGas and the cold-metal floor, row by row
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
 *              accretion is the whole 10, the cold gas and cold metals reach 0
 *   floor:     a cold-metal reservoir of -1e-9 (float cancellation) has metallicity 0, so the
 *              debit leaves it negative and the floor sets it to exactly 0
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
    float metals_cold;
    double want_accrete, want_metals, metals_tol;
  } rows[] = {
      {"Vvir 300", 0.01, 0.5, 300.0, 0.2f, 0.02672209026128266, 0.19946555819477435, 1e-7},
      {"Vvir 280", 0.01, 0.5, 280.0, 0.2f, 0.025, 0.1995, 1e-7},
      {"Vvir 100", 0.01, 0.5, 100.0, 0.2f, 0.005656108597285069, 0.1998868778280543, 1e-7},
      {"Vvir 400", 0.01, 0.5, 400.0, 0.2f, 0.03355704697986577, 0.19932885906040268, 1e-7},
      {"rate 0.005", 0.005, 0.5, 300.0, 0.2f, 0.01336104513064133, 0.19973277909738718, 1e-7},
      {"rate 0.02", 0.02, 0.5, 300.0, 0.2f, 0.05344418052256532, 0.19893111638954872, 1e-7},
      {"cap at ColdGas", 0.5, 4.0, 1000.0, 0.2f, 10.0, 0.0, 1e-6},
      {"metals floor", 0.01, 0.5, 300.0, -1.0e-9f, 0.02672209026128266, 0.0, 0.0},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters(rows[r].rate, 0.0);
    TEST_ASSERT(sage26_quasar_mode_init() == 0, "init must accept QuasarModeEfficiency 0");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, rows[r].vvir, 10.0, rows[r].metals_cold, 0.0, 0.0,
                      0.01);
    gal.UnstableDiskGasFraction = rows[r].trigger;
    struct ModuleContext ctx;
    setup_test_context(&ctx);
    const double initial_bh_mass = gal.BlackHoleMass;

    const int status = sage26_quasar_mode_process(&ctx, &halo, 1);

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
    sage26_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each row's accretion, cold gas and cold metals must match its hand calculation");
  return TEST_PASS;
}

/**
 * @test    test_per_event_zero_ratio_accretes_nothing
 * @brief   A zero merger ratio event runs grow_black_hole, which accretes nothing
 *
 * SAGE26 grows the black hole on every merger, a ratio of 0 included; with ratio 0 the
 * accretion BlackHoleGrowthRate * ratio * ColdGas / (...) is 0, so the wind has no energy. The
 * call still floors a negative MetalsColdGas at zero, which tiny negative metal reservoirs
 * reach: the one effect that distinguishes running grow_black_hole from skipping it.
 *
 * Expected: success; BlackHoleMass, ColdGas and QuasarModeBHaccretionMass unchanged;
 *           a negative MetalsColdGas floored to exactly 0
 * Validates: a zero-ratio merger event runs grow_black_hole as SAGE26 does
 */
int test_per_event_zero_ratio_accretes_nothing(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 1,
                              .target_index = 0,
                              .value0 = 0.0, /* zero ratio: grow_black_hole still runs */
                              .value1 = 0.0};
  ctx.active_event = &event;

  /* A tiny negative cold-metal reservoir, as float cancellation can leave */
  gal.MetalsColdGas = -1.0e-9f;
  const double initial_bh = gal.BlackHoleMass;
  const double initial_cold = gal.ColdGas;
  const double initial_accretion = gal.QuasarModeBHaccretionMass;

  /* ===== EXECUTE ===== */
  int result = sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Zero merger ratio event should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-12,
                           "A zero-ratio merger accretes nothing onto the black hole");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12,
                           "A zero-ratio merger takes no cold gas");
  TEST_ASSERT_DOUBLE_EQUAL(gal.QuasarModeBHaccretionMass, initial_accretion, 1e-12,
                           "A zero-ratio merger records no accretion");
  TEST_ASSERT(gal.MetalsColdGas == 0.0f,
              "grow_black_hole floors a negative MetalsColdGas at 0 even at ratio 0");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_no_triggers
 * @brief   A zero instability trigger does not run grow_black_hole
 *
 * Unlike a zero-ratio merger event (test_per_event_zero_ratio_accretes_nothing), the by-galaxy
 * path acts only on a positive UnstableDiskGasFraction. A negative MetalsColdGas, which
 * grow_black_hole would floor at 0, tells a skipped call from a call that accretes nothing.
 *
 * Expected: BlackHoleMass, ColdGas, QuasarModeBHaccretionMass and the negative MetalsColdGas
 *           unchanged
 * Validates: Trigger requirement
 */
int test_no_triggers(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.MetalsColdGas = -1.0e-9f; /* a trigger of 0 (the setup's) must leave it negative */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  int result = sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process function should succeed without a trigger");
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                           "BH mass unchanged without triggers");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10, "Cold gas unchanged without triggers");
  TEST_ASSERT(gal.QuasarModeBHaccretionMass == 0.0f, "No accretion recorded without triggers");
  TEST_ASSERT(
      gal.MetalsColdGas == -1.0e-9f,
      "grow_black_hole is not called without a trigger, so the cold metals are not floored");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_process_guards
 * @brief   The guards of both channels leave the galaxy untouched: a NULL galaxy, ngal != 1 (an
 *          error) and ColdGas <= 0
 *
 * Every row sets a positive trigger (0.5 on the instability, a ratio of 0.5 on the merger event)
 * on a galaxy that would otherwise grow its black hole. Without a ColdGas guard a negative
 * ColdGas would shrink the black hole, so those rows compare the whole GalaxyData byte for byte
 * after the call, not just the black-hole mass.
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
    int want_error;
  } rows[] = {
      {"by-galaxy, NULL galaxy", 0, 1, 1, 10.0f, 0},
      {"per-event, NULL galaxy", 1, 1, 1, 10.0f, 0},
      {"by-galaxy, ngal 2", 0, 2, 0, 10.0f, 1},
      {"per-event, ngal 2", 1, 2, 0, 10.0f, 1},
      {"by-galaxy, zero ColdGas", 0, 1, 0, 0.0f, 0},
      {"by-galaxy, negative ColdGas", 0, 1, 0, -1.0f, 0},
      {"per-event, zero ColdGas", 1, 1, 0, 0.0f, 0},
      {"per-event, negative ColdGas", 1, 1, 0, -1.0f, 0},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters(0.01, 0.001);
    TEST_ASSERT(sage26_quasar_mode_init() == 0, "init must succeed");

    struct Halo halos[2];
    struct GalaxyData gals[2];
    setup_test_galaxy(&halos[0], &gals[0], 0, 100.0, 300.0, rows[r].cold_gas, 0.2, 0.0, 0.0, 0.01);
    setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 0.0, 0.0, 0.005);
    gals[0].UnstableDiskGasFraction = 0.5;
    gals[1].UnstableDiskGasFraction = 0.5;
    if (rows[r].null_galaxy) {
      halos[0].galaxy = NULL;
    }

    struct ModuleContext ctx;
    setup_test_context(&ctx);
    struct ModuleEvent event = merger_event(0.5);
    if (rows[r].per_event) {
      ctx.active_event = &event;
    }

    struct GalaxyData before[2];
    memcpy(before, gals, sizeof(gals));
    const int status = sage26_quasar_mode_process(&ctx, halos, rows[r].ngal);

    const int status_ok = rows[r].want_error ? status != 0 : status == 0;
    const int untouched = memcmp(before, gals, sizeof(gals)) == 0;
    if (!status_ok || !untouched) {
      printf("\n  %s: status %d (error wanted: %d), galaxies untouched: %d", rows[r].name, status,
             rows[r].want_error, untouched);
      ok = 0;
    }
    sage26_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each guard must return as wanted and leave the galaxies untouched");
  return TEST_PASS;
}

// ============================================================================
// HYDROGEN CAP (SFprescription 1)
// ============================================================================

/** @brief The float bound SAGE26 forms from the cold gas the accretion leaves */
static float hydrogen_bound(const float cold_gas_after) {
  return (float)((cold_gas_after > 0.0f) ? cold_gas_after * 0.74 : 0.0f);
}

/**
 * @brief Grow the black hole of a fresh galaxy with the given H2gas and H1gas
 * @return The cold gas the accretion left
 */
static float grow_with_hydrogen(struct GalaxyData *gal, const double vvir, const float h2,
                                const float h1, const int sf_prescription,
                                struct MimicSageBlackHoleLocals *locals) {
  struct Halo halo;
  setup_test_galaxy(&halo, gal, 0, 100.0, vvir, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal->H2gas = h2;
  gal->H1gas = h1;
  struct ModuleContext ctx;
  setup_test_context(&ctx);
  (void)mimic_apply_black_hole_growth(&halo, 0.5, 0.01, 0.001, &ctx, sf_prescription,
                                      MIMIC_SAGE_CGM_RECIPE_OFF, locals);
  return gal->ColdGas;
}

/**
 * @test    test_hydrogen_cap_independent_strict
 * @brief   With SFprescription 1, H2gas and H1gas are each capped, strictly and on its own, at the
 *          float bound of the remaining cold gas; never their sum
 */
int test_hydrogen_cap_independent_strict(void) {
  struct GalaxyData gal;
  struct MimicSageBlackHoleLocals bh;
  /* The bound depends only on the accretion, which does not read the hydrogen */
  const float cold_after = grow_with_hydrogen(&gal, 300.0, 0.0f, 0.0f, 1, &bh);
  const float bound = hydrogen_bound(cold_after);
  TEST_ASSERT(bh.hydrogen_caps.tracks_h2 == 1, "SFprescription 1 runs the cap");
  TEST_ASSERT(bh.hydrogen_caps.max_h == bound, "the recorded bound is the float 0.74 * ColdGas");
  TEST_ASSERT(bh.wind.ejects_cold == 0, "this galaxy's wind keeps its cold gas");

  const float above = nextafterf(bound, INFINITY);
  const float below = nextafterf(bound, -INFINITY);
  const struct {
    float h2, h1;
    float want_h2, want_h1;
    int binds_h2, binds_h1;
  } cases[] = {
      {above, below, bound, below, 1, 0},
      {below, above, below, bound, 0, 1},
      {bound, bound, bound, bound, 0, 0},
      {above, above, bound, bound, 1, 1},
      {above, bound, bound, bound, 1, 0},
      {bound, above, bound, bound, 0, 1},
      /* each below the bound, their sum above it: nothing changes */
      {0.6f * bound, 0.6f * bound, 0.6f * bound, 0.6f * bound, 0, 0},
  };
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    (void)grow_with_hydrogen(&gal, 300.0, cases[k].h2, cases[k].h1, 1, &bh);
    TEST_ASSERT(gal.H2gas == cases[k].want_h2, "H2gas is capped strictly at the bound");
    TEST_ASSERT(gal.H1gas == cases[k].want_h1, "H1gas is capped strictly at the bound");
    TEST_ASSERT(bh.hydrogen_caps.h2_cap_binds == cases[k].binds_h2, "the H2 cap's flag");
    TEST_ASSERT(bh.hydrogen_caps.h1_cap_binds == cases[k].binds_h1, "the H1 cap's flag");
  }
  return TEST_PASS;
}

/**
 * @test    test_hydrogen_cap_off_prescription_zero
 * @brief   With SFprescription 0 the guard (SFprescription != 0 && != 2) is false: stale hydrogen
 *          above the bound is left alone
 */
int test_hydrogen_cap_off_prescription_zero(void) {
  struct GalaxyData gal;
  struct MimicSageBlackHoleLocals bh;
  (void)grow_with_hydrogen(&gal, 300.0, 9.0f, 9.0f, 0, &bh);
  TEST_ASSERT(bh.hydrogen_caps.tracks_h2 == 0, "SFprescription 0 skips the cap");
  TEST_ASSERT(isnan(bh.hydrogen_caps.max_h), "no bound is formed");
  TEST_ASSERT(gal.H2gas == 9.0f && gal.H1gas == 9.0f, "the hydrogen is unchanged");
  return TEST_PASS;
}

/**
 * @test    test_hydrogen_cap_then_wind_no_refresh
 * @brief   The cap runs before the quasar wind; a wind that ejects the whole cold gas
 *          leaves the capped H2gas and H1gas behind (no refresh after it)
 */
int test_hydrogen_cap_then_wind_no_refresh(void) {
  struct GalaxyData gal;
  struct MimicSageBlackHoleLocals bh;
  /* A low Vvir: little accretion, and a cold binding energy the wind exceeds */
  (void)grow_with_hydrogen(&gal, 50.0, 10.0f, 10.0f, 1, &bh);
  TEST_ASSERT(bh.wind.ejects_cold == 1, "the wind ejects the cold gas");
  TEST_ASSERT(gal.ColdGas == 0.0f, "no cold gas is left");
  TEST_ASSERT(bh.hydrogen_caps.h2_cap_binds == 1 && bh.hydrogen_caps.h1_cap_binds == 1,
              "both caps bound before the wind");
  TEST_ASSERT(gal.H2gas == bh.hydrogen_caps.max_h && gal.H1gas == bh.hydrogen_caps.max_h,
              "the hydrogen keeps the bound of the cold gas before the wind");
  TEST_ASSERT(gal.H2gas > 0.0f, "the wind does not refresh the hydrogen");
  return TEST_PASS;
}

/**
 * @test    test_hydrogen_cap_module_wiring
 * @brief   Initialised with SFprescription 0 or 1, each channel (the instability and the merger
 *          event) passes it on to the growth: 1 caps the hydrogen, 0 leaves it alone
 *
 * The growth (BlackHoleGrowthRate 0.01, trigger or ratio 0.5, Vvir 300, ColdGas 10) leaves
 * ColdGas about 9.97, so the float bound is about 7.38. The instability row holds H2gas above it
 * and H1gas below it (that one must be kept); the merger row holds both above it. A 0 passed
 * to the growth in place of the module's SFprescription would leave the capped fields above the
 * bound.
 *
 * Expected: SFprescription 1: each field above the bound is capped to it and a field below is
 *           kept; SFprescription 0: both unchanged
 * Validates: the SFprescription argument of both channels' calls
 */
int test_hydrogen_cap_module_wiring(void) {
  const struct {
    const char *name;
    int per_event, sf_prescription;
    float h2, h1;
    int h2_above_bound, h1_above_bound;
  } rows[] = {
      {"instability, SFprescription 1", 0, 1, 9.0f, 1.0f, 1, 0},
      {"instability, SFprescription 0", 0, 0, 9.0f, 1.0f, 1, 0},
      {"merger event, SFprescription 1", 1, 1, 7.5f, 7.6f, 1, 1},
      {"merger event, SFprescription 0", 1, 0, 7.5f, 7.6f, 1, 1},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters(0.01, 0.001);
    char value[8];
    snprintf(value, sizeof(value), "%d", rows[r].sf_prescription);
    set_model_parameter("SFprescription", value);
    TEST_ASSERT(sage26_quasar_mode_init() == 0, "init with SFprescription 0 or 1 succeeds");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.H2gas = rows[r].h2;
    gal.H1gas = rows[r].h1;
    struct ModuleContext ctx;
    setup_test_context(&ctx);
    struct ModuleEvent event = merger_event(0.5);
    if (rows[r].per_event) {
      ctx.active_event = &event;
    } else {
      gal.UnstableDiskGasFraction = 0.5;
    }

    const int status = sage26_quasar_mode_process(&ctx, &halo, 1);

    const float bound = hydrogen_bound(gal.ColdGas);
    const int accreted = gal.ColdGas < 10.0f && gal.ColdGas > 0.0f;
    const int binds_as_designed = (rows[r].h2 > bound) == rows[r].h2_above_bound &&
                                  (rows[r].h1 > bound) == rows[r].h1_above_bound;
    const float want_h2 = (rows[r].sf_prescription == 1 && rows[r].h2 > bound) ? bound : rows[r].h2;
    const float want_h1 = (rows[r].sf_prescription == 1 && rows[r].h1 > bound) ? bound : rows[r].h1;
    if (status != 0 || !accreted || !binds_as_designed || gal.H2gas != want_h2 ||
        gal.H1gas != want_h1) {
      printf("\n  %s: status %d, ColdGas %.9g, bound %.9g, H2gas %.9g (want %.9g), H1gas %.9g "
             "(want %.9g)",
             rows[r].name, status, (double)gal.ColdGas, (double)bound, (double)gal.H2gas,
             (double)want_h2, (double)gal.H1gas, (double)want_h1);
      ok = 0;
    }
    sage26_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each channel must pass its SFprescription to the hydrogen cap");
  return TEST_PASS;
}

// ============================================================================
// CGM ROUTING TESTS (CGMrecipeOn)
// ============================================================================
//
// SAGE26's quasar_mode_wind ejects the WIND HOST's cold gas when the wind energy exceeds the cold
// binding energy, then tests the wind energy against the ORIGINAL cold binding energy plus the
// binding energy of the host's CGM pair when CGMrecipeOn is 1 and the host's Regime is 0, and of
// its hot pair otherwise, ejecting that whole pair when it is exceeded. The host is the galaxy
// that grows the black hole (the unstable galaxy or the merger target); the FoF central in ctx is
// given the other Regime, so a wind that read the central's regime would select the other pair.
// The host's two pairs hold the same input, so the unselected pair must keep it.
//
// The growth makes every reservoir exact: BlackHoleGrowthRate 0.5, a trigger of 0.5 and Vvir 280
// give BHaccrete = 0.5 * 0.5 / (1 + (280 / 280)^2) * ColdGas = 0.125 of ColdGas 1, so
// 0.875 of cold gas is left (no cold metals). The cold binding energy is then exactly
// 0.5 * 0.875 * 280 * 280 = 34300, and a pair of gas G binds 39200 * G. The
// wind energy QuasarModeEfficiency * 0.1 * 0.125 * (C / UnitVelocity)^2 (C = 2.9979e10,
// UnitVelocity 1e5) is about 1.1234e6 for an efficiency of 1e-3 and 1.1234e4
// for 1e-5; the test forms it from SAGE26's expression at run time and asserts each case's side
// of both tests before asserting the state:
//   eject both:     efficiency 1e-3, pairs 1 / 0.125:     1.1234e6 > 34300 + 39200 = 73500
//   cold only:      efficiency 1e-3, pairs 28.2 / 0.25:   34300 < 1.1234e6 < 34300 + 1105440, while
//                   1.1234e6 > 1105440 alone (a wind that subtracted the cold ejection, or
//                   recomputed the cold energy after it, would also eject the pair)
//   neither:        efficiency 1e-5, pairs 1 / 0.125:     1.1234e4 < 34300

/** @brief SAGE26's wind energy, from run-time inputs */
static __attribute__((noinline)) double cgm_quasar_energy(double efficiency, double bh_accrete) {
  volatile double eff = efficiency;
  volatile double acc = bh_accrete;
  volatile double c = 2.9979e10; /* SAGE26's C, in cm/s */
  volatile double unit_velocity = 1.0e5;
  return eff * 0.1 * acc * (c / unit_velocity) * (c / unit_velocity);
}

/** @brief SAGE26's binding energy 0.5 * gas * Vvir * Vvir with float members */
static __attribute__((noinline)) double cgm_binding_energy(float gas, float vvir) {
  volatile float g = gas;
  volatile float v = vvir;
  return 0.5 * g * v * v;
}

/** @brief Initialise with growth rate 0.5, `efficiency` and CGMrecipeOn `cgm` */
static int cgm_init(const double efficiency, const char *cgm) {
  reset_config();
  setup_test_parameters(0.5, efficiency);
  set_test_cgm_recipe_on(cgm);
  return sage26_quasar_mode_init();
}

/**
 * @test    test_cgm_wind_energy_tests
 * @brief   The wind's two tests select the wind host's pair, through both channels
 */
int test_cgm_wind_energy_tests(void) {
  const struct {
    const char *cgm;
    int host_regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}, {"0", 1, 0}};
  const struct {
    const char *name;
    double efficiency;
    float pair_gas, pair_metals;
    int ejects_cold, ejects_pair;
  } cases[] = {{"eject both", 1.0e-3, 1.0f, 0.125f, 1, 1},
               {"cold only", 1.0e-3, 28.2f, 0.25f, 1, 0},
               {"neither", 1.0e-5, 1.0f, 0.125f, 0, 0}};
  int ok = 1;
  init_memory_system(0);
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    /* The case's side of both tests, from SAGE26's expressions */
    const double e_wind = cgm_quasar_energy(cases[c].efficiency, 0.125);
    const double e_cold = cgm_binding_energy(0.875f, 280.0f);
    const double e_pair = cgm_binding_energy(cases[c].pair_gas, 280.0f);
    TEST_ASSERT((e_wind > e_cold) == cases[c].ejects_cold, "the case's cold test outcome");
    TEST_ASSERT((e_wind > e_cold + e_pair) == cases[c].ejects_pair, "the case's second outcome");
    if (cases[c].ejects_cold && !cases[c].ejects_pair) {
      TEST_ASSERT(e_wind > e_pair, "only the original cold energy keeps the pair from ejecting");
    }
    for (int channel = 0; channel < 2; channel++) { /* 0 the instability, 1 the merger event */
      for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        TEST_ASSERT(cgm_init(cases[c].efficiency, runs[r].cgm) == 0,
                    "init must accept CGMrecipeOn 0 and 1");
        struct Halo host, central;
        struct GalaxyData host_gal, central_gal;
        setup_test_galaxy(&central, &central_gal, 0, 100.0, 300.0, 2.0, 0.0, 3.0, 0.375, 0.0);
        central_gal.CGMgas = 3.0f;
        central_gal.MetalsCGMgas = 0.375f;
        central_gal.Regime = 1 - runs[r].host_regime;
        setup_test_galaxy(&host, &host_gal, 1, 20.0, 280.0, 1.0, 0.0, cases[c].pair_gas,
                          cases[c].pair_metals, 0.0);
        host_gal.CGMgas = cases[c].pair_gas;
        host_gal.MetalsCGMgas = cases[c].pair_metals;
        host_gal.Regime = runs[r].host_regime;
        host_gal.EjectedGas = 0.5f;
        host_gal.MetalsEjectedGas = 0.0625f;

        struct ModuleContext ctx;
        setup_test_context(&ctx);
        ctx.central_galaxy = &central;
        struct ModuleEvent event = {.producer_module_id =
                                        MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                                    .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                                    .source_index = 2,
                                    .target_index = 1,
                                    .value0 = 0.5,
                                    .value1 = 0.01};
        if (channel == 1) {
          ctx.active_event = &event;
        } else {
          host_gal.UnstableDiskGasFraction = 0.5;
        }
        const int status = sage26_quasar_mode_process(&ctx, &host, 1);

        const float *sel_gas = runs[r].to_cgm ? &host_gal.CGMgas : &host_gal.HotGas;
        const float *sel_metals = runs[r].to_cgm ? &host_gal.MetalsCGMgas : &host_gal.MetalsHotGas;
        const float *oth_gas = runs[r].to_cgm ? &host_gal.HotGas : &host_gal.CGMgas;
        const float *oth_metals = runs[r].to_cgm ? &host_gal.MetalsHotGas : &host_gal.MetalsCGMgas;
        const float cold_out = cases[c].ejects_cold ? 0.0f : 0.875f;
        const float pair_out = cases[c].ejects_pair ? 0.0f : cases[c].pair_gas;
        const float pair_metals_out = cases[c].ejects_pair ? 0.0f : cases[c].pair_metals;
        const float ejected_out = 0.5f + (cases[c].ejects_cold ? 0.875f : 0.0f) +
                                  (cases[c].ejects_pair ? cases[c].pair_gas : 0.0f);
        const float ejected_metals_out =
            0.0625f + (cases[c].ejects_pair ? cases[c].pair_metals : 0.0f);
        const int host_ok = host_gal.BlackHoleMass == 0.125f && host_gal.ColdGas == cold_out &&
                            *sel_gas == pair_out && *sel_metals == pair_metals_out &&
                            *oth_gas == cases[c].pair_gas && *oth_metals == cases[c].pair_metals &&
                            host_gal.EjectedGas == ejected_out &&
                            host_gal.MetalsEjectedGas == ejected_metals_out &&
                            host_gal.QuasarModeBHaccretionMass == 0.125f;
        const int central_ok = central_gal.HotGas == 3.0f && central_gal.MetalsHotGas == 0.375f &&
                               central_gal.CGMgas == 3.0f && central_gal.MetalsCGMgas == 0.375f &&
                               central_gal.ColdGas == 2.0f && central_gal.EjectedGas == 0.0f;
        if (status != 0 || !host_ok || !central_ok) {
          printf("\n  %s, channel %d, CGMrecipeOn %s, host Regime %d: status %d, cold %.9g, hot "
                 "%.9g/%.9g, CGM %.9g/%.9g, ejected %.9g/%.9g",
                 cases[c].name, channel, runs[r].cgm, runs[r].host_regime, status,
                 (double)host_gal.ColdGas, (double)host_gal.HotGas, (double)host_gal.MetalsHotGas,
                 (double)host_gal.CGMgas, (double)host_gal.MetalsCGMgas,
                 (double)host_gal.EjectedGas, (double)host_gal.MetalsEjectedGas);
          ok = 0;
        }
        sage26_quasar_mode_cleanup();
      }
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the wind's second test must eject exactly the wind host's selected pair");
  return TEST_PASS;
}

/**
 * @test    test_agn_recipe_channels
 * @brief   AGNrecipe 0 grows no black hole in either channel, as SAGE26's AGNrecipeOn 0 skips
 *          grow_black_hole after a merger and in the instability's gas branch; AGNrecipe 2 grows
 *          it by the exact accretion in the same setup
 *
 * Hand calculation (BlackHoleGrowthRate 0.01, Vvir 300, ColdGas 10): a merger ratio of 0.3 gives
 * 0.01 * 0.3 / (1 + (280 / 300)^2) * 10 = 0.016033254156769594 and an instability trigger of 0.5
 * gives 0.02672209026128266. The AGNrecipe 2 rows are the controls that the same input grows a
 * black hole.
 *
 * Expected: AGNrecipe 0: success; BlackHoleMass, ColdGas, MetalsColdGas and
 *           QuasarModeBHaccretionMass unchanged; AGNrecipe 2: the exact accretion
 * Validates: the AGNrecipe gate of both channels
 */
int test_agn_recipe_channels(void) {
  const struct {
    const char *name;
    const char *agn_recipe;
    int per_event;
    double want_accrete;
  } rows[] = {
      {"AGNrecipe 2, merger event", "2", 1, 0.016033254156769594},
      {"AGNrecipe 0, merger event", "0", 1, 0.0},
      {"AGNrecipe 2, disk instability", "2", 0, 0.02672209026128266},
      {"AGNrecipe 0, disk instability", "0", 0, 0.0},
  };
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    reset_config();
    setup_test_parameters(0.01, 0.001);
    set_model_parameter("AGNrecipe", rows[r].agn_recipe);
    TEST_ASSERT(sage26_quasar_mode_init() == 0, "init must accept AGNrecipe 0 and 2");

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

    const int status = sage26_quasar_mode_process(&ctx, &halo, 1);

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
    sage26_quasar_mode_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "AGNrecipe 0 must grow no black hole and AGNrecipe 2 must grow it exactly");
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

  test_phase_add("galaxy_physics", "sage26_quasar_mode", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters */
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.01");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.001");
  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_QUASAR_MODULE);
  add_test_cgm_recipe_parameter();                      /* CGMrecipeOn: 0, required */
  sage26_test_append_model_parameter("AGNrecipe", "2"); /* required: 0 or 2 */

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
  printf("Test Suite: sage26_quasar_mode Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Growth physics and edge cases */
  TEST_RUN(test_bh_growth_disk_instability);
  TEST_RUN(test_accretion_formula);
  TEST_RUN(test_per_event_zero_ratio_accretes_nothing);
  TEST_RUN(test_no_triggers);
  TEST_RUN(test_process_guards);

  /* Hydrogen cap */
  TEST_RUN(test_hydrogen_cap_independent_strict);
  TEST_RUN(test_hydrogen_cap_off_prescription_zero);
  TEST_RUN(test_hydrogen_cap_then_wind_no_refresh);
  TEST_RUN(test_hydrogen_cap_module_wiring);

  /* Wind pair selection and parameter refusals */
  TEST_RUN(test_cgm_wind_energy_tests);
  TEST_RUN(test_agn_recipe_channels);

  /* Module lifecycle */
  TEST_RUN(test_module_initialization);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
