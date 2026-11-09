/**
 * @file    test_unit_sage26_quasar_mode.c
 * @brief   Unit tests for sage26_quasar_mode module
 *
 * Validates: Quasar-mode AGN physics, BH growth, energy-driven winds, edge cases
 *
 * This test validates the sage26_quasar_mode module physics:
 * - Black hole growth from disk instability (Kauffmann & Haehnelt 2000)
 * - Vvir suppression at low masses (< 280 km/s)
 * - Mass conservation during accretion
 * - Metallicity preservation
 * - Quasar-mode wind ejection (cold and hot gas)
 * - Energy-driven wind physics
 * - Accretion mass tracking
 * - Edge cases (zero values, orphans, NULL galaxies, triggers)
 * - Parameter sensitivity
 *
 * Test cases:
 *   - test_bh_growth_disk_instability: BH grows from disk instability
 *   - test_bh_growth_merger: By-galaxy path ignores merger flags
 *   - test_bh_growth_both_triggers: Disk trigger processed while merger trigger preserved
 *   - test_bh_growth_per_event_merger: Merger event processed in process_per_event mode
 *   - test_per_event_zero_ratio_accretes_nothing: a zero-ratio merger runs grow_black_hole
 *   - test_vvir_suppression: Low Vvir suppresses accretion
 *   - test_mass_conservation_accretion: Mass conserved
 *   - test_metallicity_preservation_accretion: Metallicity preserved
 *   - test_quasar_wind_cold_ejection: Cold gas ejection
 *   - test_quasar_wind_hot_ejection: Hot gas ejection
 *   - test_quasar_wind_no_ejection: No ejection when energy low
 *   - test_accretion_mass_tracking: QuasarModeBHaccretionMass tracks total
 *   - test_zero_cold_gas: No accretion with zero cold gas
 *   - test_zero_efficiency_factor: No accretion with zero efficiency
 *   - test_no_triggers: No processing without triggers
 *   - test_orphan_galaxy_skipped: Type 2 orphans skipped
 *   - test_null_galaxy: NULL galaxy handled
 *   - test_triggers_preserved: Trigger lifecycle owned by clear modules
 *   - test_invalid_ngal: Error when ngal != 1
 *   - test_parameter_sensitivity_growth_rate: Growth rate parameter
 *   - test_parameter_sensitivity_quasar_efficiency: Efficiency parameter
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *   - test_hydrogen_cap_independent_strict: SFprescription 1 caps H2gas and H1gas each on its own,
 *     strictly, at the float 0.74 of the cold gas the accretion leaves
 *   - test_hydrogen_cap_off_prescription_zero: SFprescription 0 leaves H2gas and H1gas alone
 *   - test_hydrogen_cap_then_wind_no_refresh: the cap precedes the wind, which refreshes nothing
 *   - test_hydrogen_cap_module_wiring: the module passes its SFprescription 1 to the growth
 *   - test_hydrogen_cap_module_wiring_per_event: the merger channel passes it too
 *   - test_cgm_wind_energy_tests: CGMrecipeOn 1 against 0 on the same input, through both channels
 *     (the instability and the merger event): the wind's cold test and its second test on the
 *     WIND HOST's CGM pair (host Regime 0) or hot pair (Regime 1, or CGMrecipeOn 0), ejecting both,
 *     only the cold gas (the original cold binding energy in the second test) or nothing; the FoF
 *     central always holds the other Regime and is untouched; the unselected pair unchanged
 *     (R/src/model_mergers.c:390-441)
 *   - test_cgm_recipe_on_refused: CGMrecipeOn is required and only 0 or 1 accepted
 *   - test_agn_recipe_off_merger_event: AGNrecipe 0 grows no black hole after a merger
 *     (R/src/model_mergers.c:262-264)
 *   - test_agn_recipe_off_disk_instability: AGNrecipe 0 grows no black hole in the instability's
 *     gas branch (R/src/model_disk_instability.c:101-103)
 *   - test_agn_recipe_refused: the module loads AGNrecipe through the package's validator, which
 *     refuses 1
 *
 * The five hydrogen-cap tests (R/src/model_mergers.c:366-372) are platform independent: they
 * assert the cap's invariants from the package's own float arithmetic, with no oracle bit
 * pattern, so they run on every platform (the bit-exact oracle test reports SKIP off macOS
 * arm64 / Apple clang 21).
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
  /* The module's required SFprescription, at R1's value */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_QUASAR_MODULE);
  add_test_cgm_recipe_parameter();                      /* CGMrecipeOn: 0, required */
  sage26_test_append_model_parameter("AGNrecipe", "2"); /* required: 0 or 2 */
}

/** @brief Set the parameter list's AGNrecipe (setup_test_parameters adds it) to `value` */
static void set_agn_recipe(const char *value) {
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "AGNrecipe") == 0) {
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

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_bh_growth_disk_instability
 * @brief   Test BH growth from disk instability trigger
 *
 * Expected: BH mass increases, cold gas decreases, accretion tracked
 * Validates: Disk instability trigger physics
 */
int test_bh_growth_disk_instability(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  const double growth_rate = 0.01;
  const double quasar_eff = 0.001;
  setup_test_parameters(growth_rate, quasar_eff);

  int result = sage26_quasar_mode_init();
  TEST_ASSERT(result == 0, "Module init should succeed");

  /* Create massive central galaxy */
  struct Halo halo;
  struct GalaxyData gal;
  const double mvir = 100.0; /* 10^12 Msun/h */
  const double vvir = 300.0; /* Well above 280 km/s threshold */
  const double cold_gas = 10.0;
  const double metals_cold = 0.2; /* Z = 0.02 */
  const double black_hole_mass = 0.01;

  setup_test_galaxy(&halo, &gal, 0, mvir, vvir, cold_gas, metals_cold, 0.0, 0.0, black_hole_mass);

  /* Set disk instability trigger */
  gal.UnstableDiskGasFraction = 0.5; /* Disk instability efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh_mass = gal.BlackHoleMass;
  const double initial_cold_gas = gal.ColdGas;
  const double initial_accretion = gal.QuasarModeBHaccretionMass;

  /* ===== EXECUTE ===== */
  result = sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process function should succeed");

  /* BH should have grown */
  TEST_ASSERT(gal.BlackHoleMass > initial_bh_mass, "BH mass should increase from disk instability");

  /* Cold gas should decrease */
  TEST_ASSERT(gal.ColdGas < initial_cold_gas, "Cold gas should decrease from BH accretion");

  /* Mass conservation */
  const double accreted = gal.BlackHoleMass - initial_bh_mass;
  const double consumed = initial_cold_gas - gal.ColdGas;
  TEST_ASSERT_DOUBLE_EQUAL(accreted, consumed, 1e-6,
                           "Accreted mass should equal consumed cold gas");

  /* Accretion tracking */
  TEST_ASSERT_DOUBLE_EQUAL(gal.QuasarModeBHaccretionMass - initial_accretion, accreted, 1e-6,
                           "QuasarModeBHaccretionMass should track accretion");

  /* Trigger lifecycle is owned by clear modules, not quasar_mode */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "Disk-instability trigger should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_bh_growth_merger
 * @brief   Test merger trigger is ignored in quasar mode module
 *
 * Expected: BH mass unchanged (merger channel handled inline in merge module)
 * Validates: Channel separation between galaxy_physics and satellite_mergers trigger paths
 */
int test_bh_growth_merger(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

  /* Set merger trigger */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh_mass = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh_mass, 1e-10,
                           "BH mass should be unchanged when only merger trigger is set");

  /* Trigger lifecycle is owned by clear modules, not quasar_mode */

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_bh_growth_both_triggers
 * @brief   Test BH growth when both triggers present
 *
 * Expected: Disk trigger is processed; merger trigger is preserved/ignored here
 * Validates: Disk-instability-only behavior in quasar module
 */
int test_bh_growth_both_triggers(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);

  /* Set both triggers */
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh_mass = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(gal.BlackHoleMass > initial_bh_mass,
              "BH mass should increase from disk-instability trigger");

  /* Trigger lifecycle is owned by clear modules, not quasar_mode */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "Disk-instability trigger should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_bh_growth_per_event_merger
 * @brief   Test BH growth from merger event in process_per_event path
 *
 * Expected: BH mass increases and cold gas decreases
 * Validates: process_per_event merger channel behavior
 */
int test_bh_growth_per_event_merger(void) {
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
                              .value0 = 0.3,
                              .value1 = 0.0};
  ctx.active_event = &event;

  const double initial_bh = gal.BlackHoleMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  int result = sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Per-event merger processing should succeed");
  TEST_ASSERT(gal.BlackHoleMass > initial_bh, "BH mass should increase from merger event");
  TEST_ASSERT(gal.ColdGas < initial_cold, "Cold gas should decrease from merger event");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_zero_ratio_accretes_nothing
 * @brief   A zero merger ratio event runs grow_black_hole, which accretes nothing
 *
 * SAGE26 grows the black hole on every merger, a ratio of 0 included
 * (R/src/model_mergers.c:262-264); with ratio 0 the accretion BlackHoleGrowthRate * ratio
 * * ColdGas / (...) is 0 (:351-352), so the wind has no energy. The call still floors a
 * negative MetalsColdGas at zero (:363-365), which R0's tiny negative metal reservoirs
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

  /* A tiny negative cold-metal reservoir, as float cancellation leaves on R0 */
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
              "grow_black_hole floors a negative MetalsColdGas at 0 even at ratio 0 (M:363-365)");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_vvir_suppression
 * @brief   Test that low Vvir suppresses BH accretion
 *
 * Expected: Less accretion at low Vvir (< 280 km/s)
 * Validates: Kauffmann & Haehnelt (2000) Vvir suppression
 */
int test_vvir_suppression(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  /* Test 1: Low Vvir (100 km/s << 280 km/s) */
  struct Halo halo_low;
  struct GalaxyData gal_low;
  setup_test_galaxy(&halo_low, &gal_low, 0, 10.0, 100.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal_low.UnstableDiskGasFraction = 0.5;

  const double initial_bh_low = gal_low.BlackHoleMass;
  sage26_quasar_mode_process(&ctx, &halo_low, 1);
  const double accreted_low = gal_low.BlackHoleMass - initial_bh_low;

  /* Test 2: High Vvir (400 km/s >> 280 km/s) */
  struct Halo halo_high;
  struct GalaxyData gal_high;
  setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 400.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal_high.UnstableDiskGasFraction = 0.5;

  const double initial_bh_high = gal_high.BlackHoleMass;
  sage26_quasar_mode_process(&ctx, &halo_high, 1);
  const double accreted_high = gal_high.BlackHoleMass - initial_bh_high;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(accreted_high > accreted_low,
              "High Vvir should produce more accretion than low Vvir");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_mass_conservation_accretion
 * @brief   Test mass conservation during BH accretion
 *
 * Expected: Mass from cold gas equals mass added to BH
 * Validates: Mass conservation
 */
int test_mass_conservation_accretion(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;
  const double initial_cold = gal.ColdGas;
  const double initial_total = initial_bh + initial_cold;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  const double final_total = gal.BlackHoleMass + gal.ColdGas;
  TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, 1e-6,
                           "Total mass (BH + cold gas) should be conserved");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_metallicity_preservation_accretion
 * @brief   Test metallicity preservation during BH accretion
 *
 * Expected: Cold gas metallicity preserved after accretion
 * Validates: Metallicity handling
 */
int test_metallicity_preservation_accretion(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 10.0;
  const double metals_cold = 0.5; /* Z = 0.05 */
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, cold_gas, metals_cold, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_metallicity = metals_cold / cold_gas;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  if (gal.ColdGas > 0.0) {
    const double final_metallicity = gal.MetalsColdGas / gal.ColdGas;
    TEST_ASSERT_DOUBLE_EQUAL(final_metallicity, initial_metallicity, 1e-6,
                             "Cold gas metallicity should be preserved");
  }

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_quasar_wind_cold_ejection
 * @brief   Test quasar wind ejects cold gas when energy sufficient
 *
 * Expected: Cold gas transferred to ejected reservoir
 * Validates: Energy-driven wind physics
 */
int test_quasar_wind_cold_ejection(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Use high efficiency to ensure wind */
  setup_test_parameters(0.05, 1.0);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 1.0; /* Small cold gas mass */
  const double vvir = 100.0;   /* Low Vvir = low binding energy */
  setup_test_galaxy(&halo, &gal, 0, 10.0, vvir, cold_gas, 0.02, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.8; /* High efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_ejected = gal.EjectedGas;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Regression for the C_CGS / UnitVelocity_in_cm_per_s unit contract: with
   * C_KM_S this fixture only accretes a small amount onto the BH and leaves
   * EjectedGas unchanged. The corrected SAGE-energy wind ejects the remaining
   * cold reservoir. */
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, 0.0, 1e-6,
                           "Sufficient quasar energy should eject all remaining cold gas");
  TEST_ASSERT(gal.EjectedGas > initial_ejected + 0.9,
              "Quasar wind should transfer cold gas to EjectedGas");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_quasar_wind_hot_ejection
 * @brief   Test quasar wind can eject both cold and hot gas
 *
 * Expected: Both cold and hot gas ejected when energy very high
 * Validates: Strong wind physics
 */
int test_quasar_wind_hot_ejection(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Very high efficiency to ensure strong wind */
  setup_test_parameters(0.1, 1.0);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 1.0;
  const double hot_gas = 1.0;
  const double vvir = 80.0; /* Very low Vvir */
  setup_test_galaxy(&halo, &gal, 0, 5.0, vvir, cold_gas, 0.02, hot_gas, 0.02, 0.01);
  gal.UnstableDiskGasFraction = 0.9;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_ejected = gal.EjectedGas;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, 0.0, 1e-6, "Strong quasar wind should eject all cold gas");
  TEST_ASSERT_DOUBLE_EQUAL(gal.HotGas, 0.0, 1e-6, "Strong quasar wind should eject all hot gas");
  TEST_ASSERT(gal.EjectedGas > initial_ejected + cold_gas + 0.9 * hot_gas,
              "Strong quasar wind should transfer cold and hot gas to EjectedGas");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_quasar_wind_no_ejection
 * @brief   Test no ejection when quasar energy below binding energy
 *
 * Expected: No gas ejected, only BH growth
 * Validates: Energy threshold logic
 */
int test_quasar_wind_no_ejection(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Low efficiency = low wind energy */
  setup_test_parameters(0.001, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 10.0;
  const double vvir = 300.0; /* High Vvir = high binding energy */
  setup_test_galaxy(&halo, &gal, 0, 100.0, vvir, cold_gas, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.1; /* Low efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_ejected = gal.EjectedGas;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Ejected gas should be unchanged (no wind) */
  TEST_ASSERT_DOUBLE_EQUAL(gal.EjectedGas, initial_ejected, 1e-6,
                           "No ejection when energy below binding energy");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_accretion_mass_tracking
 * @brief   Test QuasarModeBHaccretionMass tracks total accretion
 *
 * Expected: Accumulator equals total BH growth
 * Validates: Accretion tracking
 */
int test_accretion_mass_tracking(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;
  const double initial_tracker = gal.QuasarModeBHaccretionMass;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  const double bh_growth = gal.BlackHoleMass - initial_bh;
  const double tracker_growth = gal.QuasarModeBHaccretionMass - initial_tracker;
  TEST_ASSERT_DOUBLE_EQUAL(tracker_growth, bh_growth, 1e-6,
                           "QuasarModeBHaccretionMass should track BH growth");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_cold_gas
 * @brief   Test no accretion when ColdGas = 0
 *
 * Expected: BH mass unchanged
 * Validates: Zero cold gas handling
 */
int test_zero_cold_gas(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 0.0, 0.0, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                           "BH mass should not change when ColdGas = 0");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_efficiency_factor
 * @brief   Test no accretion when efficiency factor = 0
 *
 * Expected: BH mass unchanged
 * Validates: Zero efficiency handling
 */
int test_zero_efficiency_factor(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.0; /* Zero efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                           "BH mass should not change when efficiency = 0");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_no_triggers
 * @brief   Test no processing when both triggers = 0
 *
 * Expected: All properties unchanged
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
  /* No triggers set */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                           "BH mass unchanged without triggers");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10, "Cold gas unchanged without triggers");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_orphan_galaxy_skipped
 * @brief   Test Type 2 orphans are not processed
 *
 * Expected: No changes to galaxy properties
 * Validates: Orphan galaxy handling (module doesn't explicitly check Type,
 *            but orphans have no triggers set by upstream modules)
 */
int test_orphan_galaxy_skipped(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 2, 0.0, 0.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  /* Orphans don't have triggers set by upstream modules */

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh = gal.BlackHoleMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.BlackHoleMass, initial_bh, 1e-10,
                           "Orphan BH mass should not change");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10, "Orphan cold gas should not change");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_null_galaxy
 * @brief   Test NULL galaxy handled gracefully
 *
 * Expected: Function returns success
 * Validates: NULL pointer safety
 */
int test_null_galaxy(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.Type = 0;
  halo.galaxy = NULL;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  /* ===== EXECUTE ===== */
  int result = sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_triggers_preserved
 * @brief   Test triggers are preserved after processing
 *
 * Expected: Trigger flags remain unchanged after processing
 * Validates: Trigger lifecycle ownership by dedicated clear modules
 */
int test_triggers_preserved(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "UnstableDiskGasFraction should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_invalid_ngal
 * @brief   Test error when ngal != 1
 *
 * Expected: Function returns error
 * Validates: process_by_galaxy mode enforcement
 */
int test_invalid_ngal(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halos[2];
  struct GalaxyData gals[2];
  setup_test_galaxy(&halos[0], &gals[0], 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 0.0, 0.0, 0.005);

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  /* ===== EXECUTE ===== */
  int result = sage26_quasar_mode_process(&ctx, halos, 2);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result != 0, "Should return error when ngal != 1");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_growth_rate
 * @brief   Test BlackHoleGrowthRate parameter affects results
 *
 * Expected: Higher growth rate produces more BH accretion
 * Validates: Parameter loading and physics
 */
int test_parameter_sensitivity_growth_rate(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test with low growth rate */
  reset_config();
  setup_test_parameters(0.005, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo_low;
  struct GalaxyData gal_low;
  setup_test_galaxy(&halo_low, &gal_low, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal_low.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_bh_low = gal_low.BlackHoleMass;
  sage26_quasar_mode_process(&ctx, &halo_low, 1);
  const double accreted_low = gal_low.BlackHoleMass - initial_bh_low;

  sage26_quasar_mode_cleanup();

  /* Test with high growth rate */
  reset_config();
  setup_test_parameters(0.02, 0.001); /* 4x higher */
  sage26_quasar_mode_init();

  struct Halo halo_high;
  struct GalaxyData gal_high;
  setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal_high.UnstableDiskGasFraction = 0.5;

  const double initial_bh_high = gal_high.BlackHoleMass;
  sage26_quasar_mode_process(&ctx, &halo_high, 1);
  const double accreted_high = gal_high.BlackHoleMass - initial_bh_high;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(accreted_high > accreted_low, "Higher growth rate should produce more accretion");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity_quasar_efficiency
 * @brief   Test QuasarModeEfficiency parameter affects wind strength
 *
 * Expected: Higher efficiency produces stronger winds
 * Validates: Parameter loading and wind physics
 */
int test_parameter_sensitivity_quasar_efficiency(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test with low efficiency */
  reset_config();
  setup_test_parameters(0.05, 0.0001);
  sage26_quasar_mode_init();

  struct Halo halo_low;
  struct GalaxyData gal_low;
  setup_test_galaxy(&halo_low, &gal_low, 0, 10.0, 100.0, 2.0, 0.04, 2.0, 0.04, 0.01);
  gal_low.UnstableDiskGasFraction = 0.8;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  const double initial_ejected_low = gal_low.EjectedGas;
  sage26_quasar_mode_process(&ctx, &halo_low, 1);
  const double ejected_low = gal_low.EjectedGas - initial_ejected_low;

  sage26_quasar_mode_cleanup();

  /* Test with high efficiency */
  reset_config();
  setup_test_parameters(0.05, 1.0); /* Much higher */
  sage26_quasar_mode_init();

  struct Halo halo_high;
  struct GalaxyData gal_high;
  setup_test_galaxy(&halo_high, &gal_high, 0, 10.0, 100.0, 2.0, 0.04, 2.0, 0.04, 0.01);
  gal_high.UnstableDiskGasFraction = 0.8;

  const double initial_ejected_high = gal_high.EjectedGas;
  sage26_quasar_mode_process(&ctx, &halo_high, 1);
  const double ejected_high = gal_high.EjectedGas - initial_ejected_high;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(ejected_high >= ejected_low,
              "Higher efficiency should produce at least as much ejection");

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
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

  test_phase_add("galaxy_physics", "sage26_quasar_mode", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters */
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BlackHoleGrowthRate");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.01");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "QuasarModeEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.001");
  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription, at R1's value */
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

/**
 * @test    test_memory_safety
 * @brief   Test module doesn't leak memory during operation
 *
 * Expected: No memory leaks after init/process/cleanup cycle
 * Validates: Memory management
 */
int test_memory_safety(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(0.01, 0.001);
  sage26_quasar_mode_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx);

  /* ===== EXECUTE ===== */
  sage26_quasar_mode_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* check_memory_leaks() will catch any leaks */

  /* ===== CLEANUP ===== */
  sage26_quasar_mode_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage26_quasar_mode unit tests and reports results.
 */
// ============================================================================
// HYDROGEN CAP (SFprescription 1, R/src/model_mergers.c:366-372)
// ============================================================================

/** @brief The float bound SAGE26 forms from the cold gas the accretion leaves (:367-368) */
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
 * @brief   The cap runs before the quasar wind (:366-374); a wind that ejects the whole cold gas
 *          leaves the capped H2gas and H1gas behind (no refresh after it, :397-403)
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
 * @brief   Initialised with SFprescription 1, the module's disk-instability channel caps the
 *          hydrogen (the module passes its required parameter to the shared growth)
 */
int test_hydrogen_cap_module_wiring(void) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(0.01, 0.001);
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "SFprescription") == 0) {
      snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "1");
    }
  }
  TEST_ASSERT(sage26_quasar_mode_init() == 0, "init with SFprescription 1 succeeds");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.H2gas = 9.0f;
  gal.H1gas = 1.0f;
  gal.UnstableDiskGasFraction = 0.5;
  struct ModuleContext ctx;
  setup_test_context(&ctx);
  TEST_ASSERT(sage26_quasar_mode_process(&ctx, &halo, 1) == 0, "process succeeds");
  const float bound = hydrogen_bound(gal.ColdGas);
  TEST_ASSERT(gal.H2gas == bound, "H2gas above the bound is capped to it");
  TEST_ASSERT(gal.H1gas == 1.0f, "H1gas below the bound is kept");

  sage26_quasar_mode_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_hydrogen_cap_module_wiring_per_event
 * @brief   Initialised with SFprescription 1, the module's merger channel (a merger event,
 *          process_per_event) caps the hydrogen: the per-event growth passes the module's
 *          required parameter on as the instability channel's does (R/src/model_mergers.c:262-264,
 *          :366-372)
 *
 * Expected: H2gas above the float bound is capped to it, H1gas above it is capped to it, and
 *           H1gas or H2gas below it is kept; with the same galaxy at SFprescription 0 both stay
 * Validates: the per-event call's SFprescription argument (a 0 there leaves both fields above
 *            the bound)
 */
int test_hydrogen_cap_module_wiring_per_event(void) {
  for (int sf_prescription = 0; sf_prescription <= 1; sf_prescription++) {
    init_memory_system(0);
    reset_config();
    setup_test_parameters(0.01, 0.001);
    for (int i = 0; i < MimicConfig.NumModelParams; i++) {
      if (strcmp(MimicConfig.ModelParams[i].param_name, "SFprescription") == 0) {
        snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%d", sf_prescription);
      }
    }
    TEST_ASSERT(sage26_quasar_mode_init() == 0, "init with SFprescription 0 or 1 succeeds");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
    gal.H2gas = 7.5f;
    gal.H1gas = 7.6f;
    struct ModuleContext ctx;
    setup_test_context(&ctx);
    struct ModuleEvent event = {.producer_module_id =
                                    MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                                .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                                .source_index = 1,
                                .target_index = 0,
                                .value0 = 0.5,
                                .value1 = 0.0};
    ctx.active_event = &event;
    TEST_ASSERT(sage26_quasar_mode_process(&ctx, &halo, 1) == 0, "per-event process succeeds");
    TEST_ASSERT(gal.ColdGas < 10.0f && gal.ColdGas > 0.0f,
                "the merger accreted part of the cold gas and the wind kept the rest");

    const float bound = hydrogen_bound(gal.ColdGas);
    TEST_ASSERT(bound < 7.5f, "the bound lies below both fields' input values");
    if (sf_prescription == 1) {
      TEST_ASSERT(gal.H2gas == bound, "SFprescription 1: H2gas above the bound is capped to it");
      TEST_ASSERT(gal.H1gas == bound, "SFprescription 1: H1gas above the bound is capped to it");
    } else {
      TEST_ASSERT(gal.H2gas == 7.5f && gal.H1gas == 7.6f,
                  "SFprescription 0: H2gas and H1gas are left alone");
    }

    sage26_quasar_mode_cleanup();
    check_memory_leaks();
  }
  return TEST_PASS;
}

// ============================================================================
// CGM ROUTING TESTS (CGMrecipeOn)
// ============================================================================
//
// SAGE26's quasar_mode_wind (R = the staged SAGE26 source, R/src/model_mergers.c:390-441) ejects
// the WIND HOST's cold gas when the wind energy exceeds the cold binding energy (:394-403), then
// tests the wind energy against the ORIGINAL cold binding energy plus the binding energy of the
// host's CGM pair when CGMrecipeOn is 1 and the host's Regime is 0 (:406-417), and of its hot pair
// otherwise (:418-428, :430-440), ejecting that whole pair when it is exceeded. The host is the
// galaxy that grows the black hole (the unstable galaxy or the merger target); the FoF central in
// ctx is given the other Regime, so a wind that read the central's regime would select the other
// pair. The host's two pairs hold the same input, so the unselected pair must keep it.
//
// The growth makes every reservoir exact: BlackHoleGrowthRate 0.5, a trigger of 0.5 and Vvir 280
// give BHaccrete = 0.5 * 0.5 / (1 + (280 / 280)^2) * ColdGas = 0.125 of ColdGas 1 (:351-352), so
// 0.875 of cold gas is left (no cold metals). The cold binding energy is then exactly
// 0.5 * 0.875 * 280 * 280 = 34300 (:394), and a pair of gas G binds 39200 * G (:409, :420). The
// wind energy QuasarModeEfficiency * 0.1 * 0.125 * (C / UnitVelocity)^2 (:393, C = 2.9979e10,
// R/src/macros.h:30, UnitVelocity 1e5) is about 1.1234e6 for an efficiency of 1e-3 and 1.1234e4
// for 1e-5; the test forms it from SAGE26's expression at run time and asserts each case's side
// of both tests before asserting the state:
//   eject both:     efficiency 1e-3, pairs 1 / 0.125:     1.1234e6 > 34300 + 39200 = 73500
//   cold only:      efficiency 1e-3, pairs 28.2 / 0.25:   34300 < 1.1234e6 < 34300 + 1105440, while
//                   1.1234e6 > 1105440 alone (a wind that subtracted the cold ejection, or
//                   recomputed the cold energy after it, would also eject the pair)
//   neither:        efficiency 1e-5, pairs 1 / 0.125:     1.1234e4 < 34300

/** @brief SAGE26's wind energy (R/src/model_mergers.c:393), from run-time inputs */
static __attribute__((noinline)) double c10_quasar_energy(double efficiency, double bh_accrete) {
  volatile double eff = efficiency;
  volatile double acc = bh_accrete;
  volatile double c = 2.9979e10; /* SAGE26's C (R/src/macros.h:30) */
  volatile double unit_velocity = 1.0e5;
  return eff * 0.1 * acc * (c / unit_velocity) * (c / unit_velocity);
}

/** @brief SAGE26's binding energy 0.5 * gas * Vvir * Vvir with float members (:394, :409, :420) */
static __attribute__((noinline)) double c10_binding_energy(float gas, float vvir) {
  volatile float g = gas;
  volatile float v = vvir;
  return 0.5 * g * v * v;
}

/** @brief Set the parameter list's CGMrecipeOn to `value`, or remove it for NULL */
static void c10_set_cgm_recipe_on(const char *value) {
  int n = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "CGMrecipeOn") != 0) {
      MimicConfig.ModelParams[n++] = MimicConfig.ModelParams[i];
    }
  }
  if (value != NULL) {
    snprintf(MimicConfig.ModelParams[n].param_name, MAX_STRING_LEN, "%s", "CGMrecipeOn");
    snprintf(MimicConfig.ModelParams[n].value, MAX_STRING_LEN, "%s", value);
    n++;
  }
  MimicConfig.NumModelParams = n;
}

/** @brief Read a captured log stream into a static buffer (valid until the next call) */
static const char *c10_read_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/** @brief Initialise with growth rate 0.5, `efficiency` and CGMrecipeOn `cgm` (NULL: missing) */
static int c10_init(const double efficiency, const char *cgm, const char **log) {
  reset_config();
  setup_test_parameters(0.5, efficiency);
  c10_set_cgm_recipe_on(cgm);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_quasar_mode_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = c10_read_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
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
    /* The case's side of both tests, from SAGE26's expressions (:393-394, :409/:420, :411/:422) */
    const double e_wind = c10_quasar_energy(cases[c].efficiency, 0.125);
    const double e_cold = c10_binding_energy(0.875f, 280.0f);
    const double e_pair = c10_binding_energy(cases[c].pair_gas, 280.0f);
    TEST_ASSERT((e_wind > e_cold) == cases[c].ejects_cold, "the case's cold test outcome");
    TEST_ASSERT((e_wind > e_cold + e_pair) == cases[c].ejects_pair, "the case's second outcome");
    if (cases[c].ejects_cold && !cases[c].ejects_pair) {
      TEST_ASSERT(e_wind > e_pair, "only the original cold energy keeps the pair from ejecting");
    }
    for (int channel = 0; channel < 2; channel++) { /* 0 the instability, 1 the merger event */
      for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        const char *log = "";
        TEST_ASSERT(c10_init(cases[c].efficiency, runs[r].cgm, &log) == 0,
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
 * @test    test_cgm_recipe_on_refused
 * @brief   CGMrecipeOn is required (missing key fails init) and only 0 or 1 is accepted,
 *          a refusal naming the parameter
 */
int test_cgm_recipe_on_refused(void) {
  const char *log = "";
  TEST_ASSERT(c10_init(1.0e-3, NULL, &log) != 0, "init must fail when CGMrecipeOn is missing");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the missing-key error must name CGMrecipeOn");
  TEST_ASSERT(c10_init(1.0e-3, "2", &log) != 0, "init must refuse CGMrecipeOn 2");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init(1.0e-3, "-1", &log) != 0, "init must refuse CGMrecipeOn -1");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init(1.0e-3, "1", &log) == 0, "init must accept CGMrecipeOn 1");
  TEST_ASSERT(c10_init(1.0e-3, "0", &log) == 0, "init must accept CGMrecipeOn 0");
  return TEST_PASS;
}

/**
 * @test    test_agn_recipe_off_merger_event
 * @brief   AGNrecipe 0: a merger event grows no black hole, as SAGE26's AGNrecipeOn 0 skips
 *          grow_black_hole after the merger (R/src/model_mergers.c:262-264)
 *
 * Expected: success; BlackHoleMass, ColdGas, MetalsColdGas and QuasarModeBHaccretionMass
 *           unchanged for a merger that grows the black hole at AGNrecipe 2
 */
int test_agn_recipe_off_merger_event(void) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(0.01, 0.001);
  set_agn_recipe("0");
  TEST_ASSERT(sage26_quasar_mode_init() == 0, "init with AGNrecipe 0 succeeds");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  struct ModuleContext ctx;
  setup_test_context(&ctx);
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 1,
                              .target_index = 0,
                              .value0 = 0.3, /* as test_bh_growth_per_event_merger, which grows */
                              .value1 = 0.0};
  ctx.active_event = &event;
  const float initial_bh = gal.BlackHoleMass;
  const float initial_cold = gal.ColdGas;
  const float initial_metals = gal.MetalsColdGas;
  const float initial_accretion = gal.QuasarModeBHaccretionMass;

  TEST_ASSERT(sage26_quasar_mode_process(&ctx, &halo, 1) == 0, "process succeeds");
  TEST_ASSERT(gal.BlackHoleMass == initial_bh, "AGNrecipe 0: the merger grows no black hole");
  TEST_ASSERT(gal.ColdGas == initial_cold, "AGNrecipe 0: the merger takes no cold gas");
  TEST_ASSERT(gal.MetalsColdGas == initial_metals, "AGNrecipe 0: the cold metals are unchanged");
  TEST_ASSERT(gal.QuasarModeBHaccretionMass == initial_accretion,
              "AGNrecipe 0: the merger records no accretion");

  sage26_quasar_mode_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_agn_recipe_off_disk_instability
 * @brief   AGNrecipe 0: the instability's gas branch grows no black hole, as SAGE26's
 *          AGNrecipeOn 0 skips grow_black_hole there (R/src/model_disk_instability.c:101-103)
 *
 * Expected: success; BlackHoleMass, ColdGas, MetalsColdGas and QuasarModeBHaccretionMass
 *           unchanged for an instability trigger that grows the black hole at AGNrecipe 2
 */
int test_agn_recipe_off_disk_instability(void) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(0.01, 0.001);
  set_agn_recipe("0");
  TEST_ASSERT(sage26_quasar_mode_init() == 0, "init with AGNrecipe 0 succeeds");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 0.0, 0.0, 0.01);
  gal.UnstableDiskGasFraction = 0.5; /* as test_bh_growth_disk_instability, which grows */
  struct ModuleContext ctx;
  setup_test_context(&ctx);
  const float initial_bh = gal.BlackHoleMass;
  const float initial_cold = gal.ColdGas;
  const float initial_metals = gal.MetalsColdGas;
  const float initial_accretion = gal.QuasarModeBHaccretionMass;

  TEST_ASSERT(sage26_quasar_mode_process(&ctx, &halo, 1) == 0, "process succeeds");
  TEST_ASSERT(gal.BlackHoleMass == initial_bh, "AGNrecipe 0: the instability grows no black hole");
  TEST_ASSERT(gal.ColdGas == initial_cold, "AGNrecipe 0: the instability takes no cold gas");
  TEST_ASSERT(gal.MetalsColdGas == initial_metals, "AGNrecipe 0: the cold metals are unchanged");
  TEST_ASSERT(gal.QuasarModeBHaccretionMass == initial_accretion,
              "AGNrecipe 0: the instability records no accretion");

  sage26_quasar_mode_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_agn_recipe_refused
 * @brief   The module loads AGNrecipe through the package's validator (shared/sage_agn_recipe.h):
 *          1 is refused with an error naming the parameter, and a missing key fails init
 */
int test_agn_recipe_refused(void) {
  init_memory_system(0);
  const char *log = "";
  reset_config();
  setup_test_parameters(0.01, 0.001);
  set_agn_recipe("1");
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_quasar_mode_init();
  if (capture != NULL) {
    set_log_output(old_out);
    log = c10_read_log(capture);
    fclose(capture);
  }
  TEST_ASSERT(status != 0, "init must refuse AGNrecipe 1");
  TEST_ASSERT(strstr(log, "AGNrecipe") != NULL, "the refusal must name AGNrecipe");

  reset_config();
  setup_test_parameters(0.01, 0.001);
  int n = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "AGNrecipe") != 0) {
      MimicConfig.ModelParams[n++] = MimicConfig.ModelParams[i];
    }
  }
  MimicConfig.NumModelParams = n;
  TEST_ASSERT(sage26_quasar_mode_init() != 0, "init must fail when AGNrecipe is missing");

  sage26_quasar_mode_cleanup();
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

  /* Run physics calculation tests */
  TEST_RUN(test_bh_growth_disk_instability);
  TEST_RUN(test_bh_growth_merger);
  TEST_RUN(test_bh_growth_both_triggers);
  TEST_RUN(test_bh_growth_per_event_merger);
  TEST_RUN(test_per_event_zero_ratio_accretes_nothing);
  TEST_RUN(test_vvir_suppression);
  TEST_RUN(test_mass_conservation_accretion);
  TEST_RUN(test_metallicity_preservation_accretion);
  TEST_RUN(test_quasar_wind_cold_ejection);
  TEST_RUN(test_quasar_wind_hot_ejection);
  TEST_RUN(test_quasar_wind_no_ejection);
  TEST_RUN(test_accretion_mass_tracking);

  /* Run edge case tests */
  TEST_RUN(test_zero_cold_gas);
  TEST_RUN(test_zero_efficiency_factor);
  TEST_RUN(test_no_triggers);
  TEST_RUN(test_orphan_galaxy_skipped);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_triggers_preserved);
  TEST_RUN(test_invalid_ngal);

  /* Run parameter sensitivity tests */
  TEST_RUN(test_parameter_sensitivity_growth_rate);
  TEST_RUN(test_parameter_sensitivity_quasar_efficiency);

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);
  TEST_RUN(test_hydrogen_cap_independent_strict);
  TEST_RUN(test_hydrogen_cap_off_prescription_zero);
  TEST_RUN(test_hydrogen_cap_then_wind_no_refresh);
  TEST_RUN(test_hydrogen_cap_module_wiring);
  TEST_RUN(test_hydrogen_cap_module_wiring_per_event);
  TEST_RUN(test_cgm_wind_energy_tests);
  TEST_RUN(test_cgm_recipe_on_refused);
  TEST_RUN(test_agn_recipe_off_merger_event);
  TEST_RUN(test_agn_recipe_off_disk_instability);
  TEST_RUN(test_agn_recipe_refused);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
