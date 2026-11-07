/**
 * @file    test_unit_sage26_apply_star_formation_supernova.c
 * @brief   Unit tests for sage26_apply_star_formation_supernova module
 *
 * Validates: Physics update logic, gas transfers, metal enrichment, conservation, edge cases
 *
 * This test validates the sage26_apply_star_formation_supernova module physics:
 * - Star formation updates (gas depletion, stellar mass growth, recycling)
 * - Supernova reheating (cold→hot transfer to central)
 * - Supernova ejection (hot→ejected transfer from central, limiting)
 * - Metal enrichment (yield production, FracZleaveDisk scaling)
 * - Temporary property cleanup (NewStellarMass, etc. zeroed after processing)
 * - Conservation laws (mass, metals)
 * - Edge cases (zero stars, zero gas, ejection exceeds hot gas)
 * - Module lifecycle and memory safety
 *
 * Test cases:
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *   - test_star_formation_with_recycling: Gas depletion and stellar mass growth
 *   - test_star_formation_metal_transfer: Metal conservation during SF
 *   - test_star_formation_rate_accumulation: SFR calculation
 *   - test_reheating_gas_transfer: Cold→hot transfer to central
 *   - test_reheating_metal_transfer: Metals move with reheated gas
 *   - test_reheating_outflow_rate: Undivided outflow accumulation (SAGE26 OutflowRate)
 *   - test_ejection_gas_transfer: Hot→ejected transfer from central
 *   - test_ejection_limiting: Ejection capped at available hot gas
 *   - test_ejection_metal_transfer: Metals move with ejected gas
 *   - test_metal_yield_production: New metals from yield
 *   - test_metal_disk_vs_halo_split: FracZleaveDisk scaling
 *   - test_metal_enrichment_zero_cold_gas: All metals to hot halo when ColdGas=0
 *   - test_zero_stars_no_updates: A zero event leaves positive reservoirs unchanged
 *   - test_zero_event_caps_negative_hot_metals: A zero event still runs the metal cap
 *   - test_sf_commit_fused_rounding: the shared SF commit rounds as SAGE26's fused build
 *   - test_sf_commit_hydrogen_caps: SFprescription 1's post-SF H2/H1 caps (S:1131-1135)
 *   - test_feedback_commit_hydrogen_caps: SFprescription 1's post-reheating H2/H1 caps on the
 *     reheated galaxy only (S:1169-1173)
 *   - test_ejection_exceeds_hot_gas: Ejection limited to HotGas
 *   - test_satellite_reheats_to_central: Satellites reheat to central's hot halo
 *   - test_temporary_properties_reset: Cleanup after processing
 *   - test_mass_conservation: Total baryons unchanged
 *   - test_metal_conservation: Total metals conserved with yield production
 *   - test_disk_history_sampled_every_call: SfrDisk[bin] += stars / dt and the cold-gas
 *     samples on every call over a 13-substep snapshot, zero-star calls included
 *   - test_disk_history_zero_stars: a zero-star call still assigns the gas samples
 *   - test_disk_history_without_central: the Mimic-only no-central path still samples
 *   - test_disk_history_initial_boundary: no interval (snapshot-zero birth) samples nothing
 *   - test_cgm_feedback_destination: CGMrecipeOn 1 against 0 on the same input: the feedback into
 *     the FoF central's CGM pair (central Regime 0) or hot pair (Regime 1, or CGMrecipeOn 0), the
 *     ejection cap and the metal cap binding and not, the unselected pair and the satellite's own
 *     pairs unchanged; the satellite's Regime is always the other value (the commit routes by the
 *     FoF central's regime, S:1175-1247)
 *   - test_cgm_recipe_on_refused: CGMrecipeOn is required and only 0 or 1 accepted (D33)
 *   - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central
 *     bytewise unchanged, its history included, also in an interval the time guard refuses (the
 *     bypass precedes the history sample and the guard); the same row with FFBRegime 0 is sampled
 *     and committed, its reheated gas to the central (c13 Slice 3)
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
#include "shared/metallicity.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (set by setup helpers) */
static double test_recycle_fraction = 0.43;
static double test_yield = 0.025;
static double test_frac_z_leave_disk = 0.0;

/* Module functions (extern declarations for direct testing) */
extern int sage26_apply_star_formation_supernova_init(void);
extern int sage26_apply_star_formation_supernova_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_apply_star_formation_supernova_cleanup(void);

/* Downstream metal-yield module (SAGE ordering parity: the disk-SF yield is
 * applied after the disk-instability chain by sage26_apply_metal_enrichment;
 * pipeline tests below run both modules in sequence) */
extern int sage26_apply_metal_enrichment_init(void);
extern int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage26_apply_metal_enrichment_cleanup(void);

// ============================================================================
// TEST FIXTURES AND HELPERS
// ============================================================================

/**
 * @brief   Setup module parameters for testing
 *
 * @param   recycle     RecycleFraction [0,1]
 * @param   yield       Metal yield [0,1]
 * @param   frac_z      FracZleaveDisk [0,1]
 */
static void setup_test_parameters(double recycle, double yield, double frac_z) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", recycle);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", yield);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", frac_z);

  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription, at R1's value */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_APPLY_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */

  /* Update test variables */
  test_recycle_fraction = recycle;
  test_yield = yield;
  test_frac_z_leave_disk = frac_z;
}

/**
 * @brief   Setup module context for testing
 *
 * @param   ctx             ModuleContext to initialize
 * @param   central         Pointer to central galaxy halo
 * @param   dt              Time step
 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central, double dt) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->central_galaxy = central;
  ctx->substep_dt = dt;
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
 * @param   type            Halo type (0=central, 1=satellite, 2=orphan)
 * @param   mvir            Virial mass (1e10 Msun/h)
 * @param   dt              Time step (Gyr) for rate calculations
 * @param   cold_gas        Cold gas mass (1e10 Msun/h)
 * @param   hot_gas         Hot gas mass (1e10 Msun/h)
 * @param   ejected_gas     Ejected gas mass (1e10 Msun/h)
 * @param   stellar_mass    Stellar mass (1e10 Msun/h)
 * @param   metals_cold     Metals in cold gas (1e10 Msun/h)
 * @param   metals_hot      Metals in hot gas (1e10 Msun/h)
 * @param   metals_ejected  Metals in ejected gas (1e10 Msun/h)
 * @param   metals_stellar  Metals in stellar mass (1e10 Msun/h)
 * @param   new_stars       NewStellarMass (calculated by previous module)
 * @param   reheated_mass   SupernovaReheatedMass (calculated by previous module)
 * @param   ejected_mass    SupernovaEjectedMass (calculated by previous module)
 */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir,
                            double dt, double cold_gas, double hot_gas, double ejected_gas,
                            double stellar_mass, double metals_cold, double metals_hot,
                            double metals_ejected, double metals_stellar, double new_stars,
                            double reheated_mass, double ejected_mass) {
  memset(halo, 0, sizeof(struct Halo));
  memset(galaxy, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Mvir = mvir;
  halo->Vvir = 150.0; /* km/s */
  halo->SnapNum = 63;
  halo->dT = dt;
  halo->galaxy = galaxy;

  galaxy->ColdGas = cold_gas;
  galaxy->HotGas = hot_gas;
  galaxy->EjectedGas = ejected_gas;
  galaxy->StellarMass = stellar_mass;
  galaxy->MetalsColdGas = metals_cold;
  galaxy->MetalsHotGas = metals_hot;
  galaxy->MetalsEjectedGas = metals_ejected;
  galaxy->MetalsStellarMass = metals_stellar;
  galaxy->NewStellarMass = new_stars;
  galaxy->SupernovaReheatedMass = reheated_mass;
  galaxy->SupernovaEjectedMass = ejected_mass;
  galaxy->StarFormationRate = 0.0;
  galaxy->SupernovaOutflowRate = 0.0;
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors or leaks
 * Validates: Module lifecycle management
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

  /* Configure sage26_apply_star_formation_supernova module in galaxy_physics */
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  /* the module's required SFprescription */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_APPLY_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */

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
 * @brief   Test that module doesn't leak memory during normal operation
 *
 * Expected: No memory leaks after init, cleanup cycle
 * Validates: Memory management in module
 */
int test_memory_safety(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  /* the module's required SFprescription */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_APPLY_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */

  /* ===== EXECUTE ===== */
  int result = module_system_init();
  TEST_ASSERT(result == 0, "Module initialization should succeed");

  /* ===== VALIDATE ===== */
  /* Module initialized successfully without memory leaks */

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// STAR FORMATION UPDATE TESTS
// ============================================================================

/**
 * @test    test_star_formation_with_recycling
 * @brief   Test star formation updates with recycling fraction
 *
 * Physics: ColdGas -= (1 - RecycleFraction) * NewStellarMass
 *          StellarMass += (1 - RecycleFraction) * NewStellarMass
 *
 * Expected: Gas depletion and stellar mass growth respect recycling fraction
 * Validates: Recycling fraction correctly applied
 */
int test_star_formation_with_recycling(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0); /* RecycleFraction=0.43, no yield */

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central */
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0,
                  20.0,               /* ColdGas, HotGas, EjectedGas, StellarMass */
                  0.0, 0.0, 0.0, 0.0, /* Metals */
                  0.0, 0.0, 0.0); /* NewStellarMass, SupernovaReheatedMass, SupernovaEjectedMass */

  /* Setup test galaxy: 10 ColdGas, form 1.0 new stars */
  double initial_cold = 10.0;
  double initial_stellar = 20.0;
  double new_stars = 1.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, 50.0, 0.0, initial_stellar,
                  0.0, 0.0, 0.0, 0.0, new_stars, 0.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Expected: Gas consumed = (1 - RecycleFraction) * stars = (1 - 0.43) * 1.0 = 0.57 */
  double expected_gas_consumed = (1.0 - test_recycle_fraction) * new_stars;
  double expected_cold = initial_cold - expected_gas_consumed;
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, expected_cold, 1e-6,
                           "ColdGas should decrease by (1 - RecycleFraction) * NewStellarMass");

  /* Expected: Stellar mass gain = (1 - RecycleFraction) * stars = 0.57 */
  double expected_stellar = initial_stellar + expected_gas_consumed;
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StellarMass, expected_stellar, 1e-6,
                           "StellarMass should increase by (1 - RecycleFraction) * NewStellarMass");

  return TEST_PASS;
}

/**
 * @test    test_star_formation_metal_transfer
 * @brief   Test that metals move from cold gas to stellar mass during star formation
 *
 * Physics: MetalsColdGas -= metallicity * (1 - RecycleFraction) * NewStellarMass
 *          MetalsStellarMass += metallicity * (1 - RecycleFraction) * NewStellarMass
 *
 * Expected: Metals conserved (transferred, not destroyed)
 * Validates: Metal conservation during star formation
 */
int test_star_formation_metal_transfer(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  /* Setup with metallicity = 0.02 (solar) */
  double initial_cold = 10.0;
  double initial_metals_cold = 0.2; /* Z = 0.2/10.0 = 0.02 */
  double initial_metals_stellar = 0.3;
  double new_stars = 1.0;
  double metallicity = initial_metals_cold / initial_cold;

  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, 50.0, 0.0, 20.0,
                  initial_metals_cold, 0.0, 0.0, initial_metals_stellar, new_stars, 0.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Expected metal transfer */
  double metal_transferred = metallicity * (1.0 - test_recycle_fraction) * new_stars;
  double expected_metals_cold = initial_metals_cold - metal_transferred;
  double expected_metals_stellar = initial_metals_stellar + metal_transferred;

  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsColdGas, expected_metals_cold, 1e-6,
                           "MetalsColdGas should decrease");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsStellarMass, expected_metals_stellar, 1e-6,
                           "MetalsStellarMass should increase by same amount");

  return TEST_PASS;
}

/**
 * @test    test_star_formation_rate_accumulation
 * @brief   Test that StarFormationRate is calculated correctly
 *
 * Physics: StarFormationRate += NewStellarMass / dT
 *
 * Expected: SFR in code units (1e10 Msun/h) / (978 Gyr/h)
 * Validates: SFR accumulation
 */
int test_star_formation_rate_accumulation(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  double dt = 0.1;        /* 978 Gyr/h */
  double new_stars = 1.0; /* 1e10 Msun/h */
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, dt, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0, 0.0,
                  new_stars, 0.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, dt);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Expected SFR = stars / dt = 1.0 / 0.1 = 10.0 */
  double expected_sfr = new_stars / dt;
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StarFormationRate, expected_sfr, 1e-6,
                           "StarFormationRate should equal NewStellarMass / dT");

  return TEST_PASS;
}

// ============================================================================
// SUPERNOVA REHEATING TESTS
// ============================================================================

/**
 * @test    test_reheating_gas_transfer
 * @brief   Test that reheating transfers gas from galaxy's cold to central's hot
 *
 * Physics: Galaxy's ColdGas -= SupernovaReheatedMass
 *          Central's HotGas += SupernovaReheatedMass
 *
 * Expected: Gas moved from galaxy to central (satellites too)
 * Validates: Reheating gas transfer logic
 */
int test_reheating_gas_transfer(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central */
  double central_hot_initial = 50.0;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, central_hot_initial, 0.0,
                  20.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

  /* Setup test galaxy: 10 ColdGas, form 1.0 stars, reheat 0.5 */
  double initial_cold = 10.0;
  double new_stars = 1.0;
  double reheated_mass = 0.5;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, new_stars, reheated_mass, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Galaxy's cold gas should decrease by reheated mass (plus SF depletion) */
  double sf_depletion = (1.0 - test_recycle_fraction) * new_stars;
  double expected_cold = initial_cold - sf_depletion - reheated_mass;
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, expected_cold, 1e-6,
                           "Galaxy ColdGas should decrease by SF + reheating");

  /* Central's hot gas should increase by reheated mass */
  double expected_central_hot = central_hot_initial + reheated_mass;
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, expected_central_hot, 1e-6,
                           "Central HotGas should increase by reheated mass");

  return TEST_PASS;
}

/**
 * @test    test_reheating_metal_transfer
 * @brief   Test that metals move with reheated gas
 *
 * Physics: Metals transferred at galaxy's cold gas metallicity
 *
 * Expected: Metals conserved during reheating transfer
 * Validates: Metal transfer during reheating
 */
int test_reheating_metal_transfer(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  double central_metals_hot_initial = 0.5;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0,
                  central_metals_hot_initial, 0.0, 0.0, 0.0, 0.0, 0.0);

  /* Setup with metallicity = 0.02 */
  double initial_cold = 10.0;
  double initial_metals_cold = 0.2; /* Z = 0.02 */
  double new_stars = 1.0;
  double reheated_mass = 0.5;
  double metallicity_before_sf = initial_metals_cold / initial_cold;

  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, 50.0, 0.0, 20.0,
                  initial_metals_cold, 0.0, 0.0, 0.0, new_stars, reheated_mass, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Metallicity is recomputed after SF, so we need to calculate it */
  double sf_gas_consumed = (1.0 - test_recycle_fraction) * new_stars;
  double sf_metals_consumed = metallicity_before_sf * sf_gas_consumed;
  double cold_gas_after_sf = initial_cold - sf_gas_consumed;
  double metals_cold_after_sf = initial_metals_cold - sf_metals_consumed;
  double metallicity_after_sf =
      (cold_gas_after_sf > 0) ? (metals_cold_after_sf / cold_gas_after_sf) : 0.0;

  /* Expected metal transfer during reheating */
  double metal_transferred_reheat = metallicity_after_sf * reheated_mass;
  double expected_metals_cold = metals_cold_after_sf - metal_transferred_reheat;
  double expected_central_metals_hot = central_metals_hot_initial + metal_transferred_reheat;

  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsColdGas, expected_metals_cold, 1e-6,
                           "Galaxy MetalsColdGas should decrease by reheated amount");
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsHotGas, expected_central_metals_hot, 1e-6,
                           "Central MetalsHotGas should increase by same amount");

  return TEST_PASS;
}

/**
 * @test    test_reheating_outflow_rate
 * @brief   Test that SupernovaOutflowRate accumulates the undivided reheated mass
 *
 * Physics: SupernovaOutflowRate += reheated mass, a float accumulation of SAGE26's
 * OutflowRate (R/src/model_starformation_and_feedback.c:1249). The division by the
 * interval happens once per halo in sage26_finalize_cooling_heating.
 *
 * Expected: the reheated mass itself, in code mass units
 * Validates: Outflow accumulation
 */
int test_reheating_outflow_rate(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  double dt = 0.1;            /* 978 Gyr/h */
  double reheated_mass = 0.5; /* 1e10 Msun/h */
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, dt, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0, 0.0,
                  1.0, reheated_mass, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, dt);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Expected: the undivided reheated mass, narrowed to the float accumulator */
  const float expected_outflow = (float)(0.0 + reheated_mass);
  TEST_ASSERT(test_galaxy.SupernovaOutflowRate == expected_outflow,
              "SupernovaOutflowRate should equal the undivided SupernovaReheatedMass");

  return TEST_PASS;
}

// ============================================================================
// SUPERNOVA EJECTION TESTS
// ============================================================================

/**
 * @test    test_ejection_gas_transfer
 * @brief   Test that ejection transfers gas from central's hot to central's ejected
 *
 * Physics: Central's HotGas -= SupernovaEjectedMass
 *          Central's EjectedGas += SupernovaEjectedMass
 *
 * Expected: Gas moved within central halo
 * Validates: Ejection gas transfer logic
 */
int test_ejection_gas_transfer(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central with plenty of hot gas */
  double central_hot_initial = 50.0;
  double central_ejected_initial = 5.0;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, central_hot_initial,
                  central_ejected_initial, 20.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

  /* Galaxy forms stars, reheats, and ejects */
  double new_stars = 1.0;
  double reheated_mass = 0.5;
  double ejected_mass = 0.3;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0,
                  0.0, new_stars, reheated_mass, ejected_mass);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Central's hot gas: gain from reheating, lose to ejection */
  double expected_central_hot = central_hot_initial + reheated_mass - ejected_mass;
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, expected_central_hot, 1e-6,
                           "Central HotGas should gain from reheating and lose to ejection");

  /* Central's ejected gas should increase */
  double expected_central_ejected = central_ejected_initial + ejected_mass;
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.EjectedGas, expected_central_ejected, 1e-6,
                           "Central EjectedGas should increase by ejected mass");

  return TEST_PASS;
}

/**
 * @test    test_ejection_limiting
 * @brief   Test that ejection is limited to available hot gas
 *
 * Physics: If SupernovaEjectedMass > Central's HotGas, ejection = HotGas
 *
 * Expected: Ejection capped at available hot gas
 * Validates: Ejection limiting logic
 */
int test_ejection_limiting(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central with limited hot gas (less than requested ejection) */
  double central_hot_initial = 0.2; /* Only 0.2 available */
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, central_hot_initial, 0.0,
                  20.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

  /* Galaxy wants to eject 0.5, but central only has 0.2 */
  double requested_ejection = 0.5;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0,
                  0.0, 1.0, 0.0, requested_ejection);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Central's hot gas should be completely depleted */
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, 0.0, 1e-6,
                           "Central HotGas should be zero (all ejected)");

  /* Central's ejected gas should increase by only what was available */
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.EjectedGas, central_hot_initial, 1e-6,
                           "Ejection should be limited to available HotGas");

  return TEST_PASS;
}

/**
 * @test    test_ejection_metal_transfer
 * @brief   Test that metals move with ejected gas
 *
 * Physics: Metals transferred at central's hot gas metallicity
 *
 * Expected: Metals conserved during ejection transfer
 * Validates: Metal transfer during ejection
 */
int test_ejection_metal_transfer(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central with hot gas metallicity = 0.01 */
  double central_hot_initial = 50.0;
  double central_metals_hot_initial = 0.5; /* Z = 0.5/50 = 0.01 */
  double central_metals_ejected_initial = 0.1;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, central_hot_initial, 5.0,
                  20.0, 0.0, central_metals_hot_initial, central_metals_ejected_initial, 0.0, 0.0,
                  0.0, 0.0);

  double ejected_mass = 0.3;
  double metallicity_hot = central_metals_hot_initial / central_hot_initial;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0,
                  0.0, 1.0, 0.0, ejected_mass);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Expected metal transfer */
  double metal_transferred = metallicity_hot * ejected_mass;
  double expected_metals_hot = central_metals_hot_initial - metal_transferred;
  double expected_metals_ejected = central_metals_ejected_initial + metal_transferred;

  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsHotGas, expected_metals_hot, 1e-6,
                           "Central MetalsHotGas should decrease");
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsEjectedGas, expected_metals_ejected, 1e-6,
                           "Central MetalsEjectedGas should increase by same amount");

  return TEST_PASS;
}

// ============================================================================
// METAL ENRICHMENT TESTS
// ============================================================================

/**
 * @test    test_metal_yield_production
 * @brief   Test that new metals are produced from stellar yields
 *
 * Physics: Total metals increase by Yield * NewStellarMass
 *
 * Expected: New metals created (not conserved - stellar nucleosynthesis)
 * Validates: Metal yield production
 */
int test_metal_yield_production(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.025, 0.0); /* Yield = 0.025 */

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0,
              "Metal enrichment module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  /* Galaxy with plenty of cold gas and forms 1.0 new stars */
  double initial_metals_cold = 0.2;
  double initial_metals_hot = 0.5;
  double new_stars = 1.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0,
                  initial_metals_cold, initial_metals_hot, 0.0, 0.0, new_stars, 0.0, 0.0);

  /* Calculate total metals before */
  double total_metals_before = initial_metals_cold + initial_metals_hot;

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  /* Run the full SF/SN apply -> metal enrichment pipeline (SAGE ordering) */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Module process should succeed");
  result = sage26_apply_metal_enrichment_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Metal enrichment process should succeed");

  /* Calculate total metals after (all reservoirs) */
  double total_metals_after = test_galaxy.MetalsColdGas + test_galaxy.MetalsHotGas +
                              test_galaxy.MetalsEjectedGas + test_galaxy.MetalsStellarMass;

  /* Expected new metals = Yield * NewStellarMass = 0.025 * 1.0 = 0.025 */
  double expected_new_metals = test_yield * new_stars;
  double expected_total_metals = total_metals_before + expected_new_metals;

  TEST_ASSERT_DOUBLE_EQUAL(total_metals_after, expected_total_metals, 1e-6,
                           "Total metals should increase by Yield * NewStellarMass");

  return TEST_PASS;
}

/**
 * @test    test_metal_disk_vs_halo_split
 * @brief   Test that metal enrichment splits between disk and hot halo
 *
 * Physics: FracZleaveDiskVal = FracZleaveDisk * exp(-Mvir/30)
 *          Disk gets: Yield * (1 - FracZleaveDiskVal) * NewStellarMass
 *          Halo gets: Yield * FracZleaveDiskVal * NewStellarMass
 *
 * Expected: Low mass halos retain more metals in disk
 * Validates: FracZleaveDisk scaling with halo mass
 */
int test_metal_disk_vs_halo_split(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.025, 0.5); /* Yield=0.025, FracZleaveDisk=0.5 */

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0,
              "Metal enrichment module initialization should succeed");

  struct Halo test_halo;
  struct GalaxyData test_galaxy;

  /* Setup test galaxy as a central (processes itself) */
  double mvir = 100.0; /* 10^12 Msun/h */
  double initial_metals_cold = 0.2;
  double initial_metals_hot = 0.5;
  double new_stars = 1.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, mvir, 0.1, 10.0, 50.0, 0.0, 20.0,
                  initial_metals_cold, initial_metals_hot, 0.0, 0.0, new_stars, 0.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &test_halo, 0.1); /* Central points to itself */

  /* Store initial metals */
  double metals_cold_initial = initial_metals_cold;
  double metals_hot_initial = initial_metals_hot;

  /* ===== EXECUTE ===== */
  /* Run the full SF/SN apply -> metal enrichment pipeline (SAGE ordering) */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Module process should succeed");
  result = sage26_apply_metal_enrichment_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Metal enrichment process should succeed");

  /* Calculate expected split */
  double frac_z_leave_disk_val = test_frac_z_leave_disk * exp(-1.0 * mvir / 30.0);
  double metals_to_disk = test_yield * (1.0 - frac_z_leave_disk_val) * new_stars;
  double metals_to_halo = test_yield * frac_z_leave_disk_val * new_stars;

  /* Account for SF metal transfer and yield production */
  double metallicity_before_sf = metals_cold_initial / 10.0;
  double metals_from_sf_transfer =
      metallicity_before_sf * (1.0 - test_recycle_fraction) * new_stars;
  double expected_metals_cold = metals_cold_initial - metals_from_sf_transfer + metals_to_disk;
  double expected_metals_hot = metals_hot_initial + metals_to_halo;

  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsColdGas, expected_metals_cold, 1e-5,
                           "MetalsColdGas should include yield production to disk");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.MetalsHotGas, expected_metals_hot, 1e-5,
                           "MetalsHotGas should include yield production to halo");

  return TEST_PASS;
}

/**
 * @test    test_metal_enrichment_zero_cold_gas
 * @brief   Test metal enrichment when ColdGas = 0 (all metals go to hot halo)
 *
 * Physics: If ColdGas <= EPSILON_SMALL, all new metals go to central's hot halo
 *
 * Expected: All yield metals added to hot halo
 * Validates: Zero cold gas edge case
 */
int test_metal_enrichment_zero_cold_gas(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.025, 0.5);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0,
              "Metal enrichment module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  double initial_central_metals_hot = 0.5;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0,
                  initial_central_metals_hot, 0.0, 0.0, 0.0, 0.0, 0.0);

  /* Galaxy with exactly enough cold gas for SF, will have ~0 after depletion */
  double new_stars = 1.0;
  double gas_consumed = (1.0 - test_recycle_fraction) * new_stars;
  double initial_cold = gas_consumed; /* Exactly consumed by SF */
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, new_stars, 0.0, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  /* Run the full SF/SN apply -> metal enrichment pipeline (SAGE ordering) */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Module process should succeed");
  result = sage26_apply_metal_enrichment_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Metal enrichment process should succeed");

  /* All yield metals should go to central's hot halo */
  double expected_new_metals = test_yield * new_stars;
  double expected_central_metals_hot = initial_central_metals_hot + expected_new_metals;

  TEST_ASSERT(test_galaxy.ColdGas < 1e-6, "ColdGas should be ~0 after SF consumption");
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.MetalsHotGas, expected_central_metals_hot, 1e-6,
                           "All yield metals should go to central's hot halo when ColdGas=0");

  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_stars_no_updates
 * @brief   Test that a zero event leaves positive reservoirs unchanged
 *
 * SAGE26 commits star formation and feedback on every call, also with zero masses
 * (R/src/model_starformation_and_feedback.c:1063-1085), so the apply step runs both
 * commits; with zero masses and non-negative reservoirs nothing moves.
 *
 * Expected: All properties unchanged except temporary properties zeroed
 * Validates: The zero-mass commit path
 */
int test_zero_stars_no_updates(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.025, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 5.0, 20.0, 0.0, 0.5,
                  0.1, 0.0, 0.0, 0.0, 0.0);

  /* Galaxy with zero star formation */
  double initial_cold = 10.0;
  double initial_hot = 50.0;
  double initial_ejected = 5.0;
  double initial_stellar = 20.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, initial_cold, initial_hot,
                  initial_ejected, initial_stellar, 0.2, 0.5, 0.1, 0.3, 0.0, 0.0,
                  0.0); /* NewStellarMass = 0 */

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* All properties should be unchanged */
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.ColdGas, initial_cold, 1e-6, "ColdGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.HotGas, initial_hot, 1e-6, "HotGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.EjectedGas, initial_ejected, 1e-6,
                           "EjectedGas should be unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.StellarMass, initial_stellar, 1e-6,
                           "StellarMass should be unchanged");

  /* Temporary properties should be zeroed */
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, 0.0, 1e-6,
                           "NewStellarMass should be zeroed");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, 0.0, 1e-6,
                           "SupernovaReheatedMass should be zeroed");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-6,
                           "SupernovaEjectedMass should be zeroed");

  return TEST_PASS;
}

/**
 * @test    test_zero_event_caps_negative_hot_metals
 * @brief   A zero event still runs update_from_feedback's metal cap
 *
 * SAGE26 calls update_from_feedback on every substep, also with no stars
 * (R/src/model_starformation_and_feedback.c:1063-1085). With zero ejected mass the
 * metal transfer metallicityHot * 0 = 0 exceeds a negative MetalsHotGas, so the cap
 * (:1237-1240) moves the negative hot metals into the ejected reservoir and leaves
 * MetalsHotGas at zero. A module that returned early on zero stars would not.
 * No oracle wrapper fixture has both zero stars and negative hot metals.
 */
int test_zero_event_caps_negative_hot_metals(void) {
  setup_test_parameters(0.43, 0.025, 0.0);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0,
              "Module initialization should succeed");

  struct Halo test_halo;
  struct GalaxyData test_galaxy;
  const double negative_hot_metals = -1.0e-9;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 5.0, 20.0, 0.2,
                  negative_hot_metals, 0.1, 0.3, 0.0, 0.0, 0.0);
  const float ejected_metals_before = test_galaxy.MetalsEjectedGas;
  const float hot_metals_before = test_galaxy.MetalsHotGas;

  struct ModuleContext ctx;
  setup_module_context(&ctx, &test_halo, 0.1);

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1) == 0,
              "Module process should succeed");
  TEST_ASSERT(test_galaxy.MetalsHotGas == 0.0f,
              "the zero event's capped transfer empties the negative hot metals");
  TEST_ASSERT(test_galaxy.MetalsEjectedGas == (float)(ejected_metals_before + hot_metals_before),
              "the negative hot metals move into the ejected reservoir");
  TEST_ASSERT(test_galaxy.HotGas == 50.0f && test_galaxy.ColdGas == 10.0f,
              "no gas moves in a zero event");
  return TEST_PASS;
}

/** @brief A float from its IEEE-754 bit pattern */
static float f32_from_bits(const uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief A double from its IEEE-754 bit pattern */
static double f64_from_bits(const uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief The IEEE-754 bit pattern of a float */
static uint32_t f32_bits(const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/**
 * @test    test_sf_commit_fused_rounding
 * @brief   The shared SF commit rounds each update once, as SAGE26's build fuses them
 *
 * SAGE26's compiled update_from_star_formation (R/src/model_starformation_and_feedback.c
 * :1123-1126) applies two fmla.2d: [ColdGas, StellarMass] += [-(1-R), (1-R)] * stars
 * and [MetalsColdGas, MetalsStellarMass] += [-(Z(1-R)), Z(1-R)] * stars, one rounding
 * per lane. No oracle fixture distinguishes that from separate roundings, so these
 * cases pin it. Every expected bit pattern was produced by executing SAGE26's own
 * compiled update_from_star_formation: the function-level oracle's object for
 * model_starformation_and_feedback.c, built by _tests/oracle/build.sh with the pinned
 * toolchain (oracle README "Oracle Environment"), linked into a small caller that
 * passes SAGE26's get_metallicity of the cold gas. The inputs were chosen so that each
 * exact result lies near a float rounding midpoint, where a fused and an unfused
 * commit round to different floats: case 0 differs in StellarMass, cases 1 and 2 in
 * ColdGas and StellarMass, case 3 in MetalsColdGas and MetalsStellarMass. R is 0.43.
 */
int test_sf_commit_fused_rounding(void) {
  static const struct {
    uint32_t in[4]; /* ColdGas, StellarMass, MetalsColdGas, MetalsStellarMass */
    uint64_t stars;
    uint64_t metallicity;
    uint32_t sage26_out[4]; /* SAGE26's update_from_star_formation, executed */
  } cases[] = {
      {{0x3f800000, 0x3dcccccd, 0x00000000, 0x00000000},
       0x3fc1f707f9435e4fULL,
       0x0000000000000000ULL,
       {0x3f6b851b, 0x3e3851fb, 0x00000000, 0x00000000}},
      {{0x40400000, 0x40200000, 0x3d3851ec, 0x3d19999a},
       0x3fe0d79398b3a630ULL,
       0x3f8eb85200000000ULL,
       {0x402ccccd, 0x40333333, 0x3d25e355, 0x3d2c0831}},
      {{0x40400000, 0x40200000, 0x3d3851ec, 0x3d19999a},
       0x3fdc11f7e50d794aULL,
       0x3f8eb85200000000ULL,
       {0x402fffff, 0x40300001, 0x3d28f5c3, 0x3d28f5c3}},
      {{0x40400000, 0x40200000, 0x3d3851ec, 0x3d19999a},
       0x3fdc11f7ee68caf6ULL,
       0x3f8eb85200000000ULL,
       {0x402fffff, 0x40300001, 0x3d28f5c3, 0x3d28f5c3}},
  };
  const double recycle_fraction = f64_from_bits(0x3fdb851eb851eb85ULL); /* 0.43 */

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));
    gal.ColdGas = f32_from_bits(cases[c].in[0]);
    gal.StellarMass = f32_from_bits(cases[c].in[1]);
    gal.MetalsColdGas = f32_from_bits(cases[c].in[2]);
    gal.MetalsStellarMass = f32_from_bits(cases[c].in[3]);
    const double metallicity = mimic_get_metallicity(gal.ColdGas, gal.MetalsColdGas);
    TEST_ASSERT(metallicity == f64_from_bits(cases[c].metallicity),
                "the cold-gas metallicity must equal SAGE26's get_metallicity");

    /* SFprescription 0: SAGE26's R0 and R1 commit, with no hydrogen cap */
    mimic_sage_update_from_star_formation(&gal, f64_from_bits(cases[c].stars), metallicity,
                                          recycle_fraction, MIMIC_SAGE_SF_PRESCRIPTION_CROTON,
                                          NULL);

    TEST_ASSERT(f32_bits(gal.ColdGas) == cases[c].sage26_out[0],
                "ColdGas must round as SAGE26's fused update (S:1123)");
    TEST_ASSERT(f32_bits(gal.StellarMass) == cases[c].sage26_out[1],
                "StellarMass must round as SAGE26's fused update (S:1125)");
    TEST_ASSERT(f32_bits(gal.MetalsColdGas) == cases[c].sage26_out[2],
                "MetalsColdGas must round as SAGE26's fused update (S:1124)");
    TEST_ASSERT(f32_bits(gal.MetalsStellarMass) == cases[c].sage26_out[3],
                "MetalsStellarMass must round as SAGE26's fused update (S:1126)");
  }
  return TEST_PASS;
}

// ============================================================================
// HYDROGEN CAPS OF THE SHARED COMMITS (SFPRESCRIPTION 1)
// ============================================================================
//
// Platform-independent invariants of the two hydrogen caps SAGE26 runs for a prescription that
// tracks H2: update_from_star_formation's after its four reservoir stores (S:1131-1135) and
// update_from_feedback's after the cold-gas and metal debits (S:1169-1173). Each bound is the
// float max_h = (ColdGas > 0.0f) ? ColdGas * 0.74 : 0.0f of the cold gas the commit leaves,
// formed here as the commit forms it (one double product, narrowed: no operation a compiler can
// contract). They carry no oracle fixture and no reference-build bit pattern; the bit-for-bit
// parity with SAGE26 is the oracle test's
// (test_unit_sage26_apply_star_formation_supernova_oracle.c).

/** @brief The float hydrogen bound of a cold-gas reservoir, as both commits form it */
static float hydrogen_bound(const float cold_gas) {
  return (float)((cold_gas > 0.0f) ? cold_gas * 0.74 : 0.0f);
}

/** @brief A star-formation commit of a galaxy with the given cold gas, H2gas and H1gas */
static void commit_star_formation(struct GalaxyData *gal, const float cold_gas, const float h2,
                                  const float h1, const double stars, const int sf_prescription,
                                  struct MimicSageHydrogenCapLocals *caps) {
  memset(gal, 0, sizeof(*gal));
  gal->ColdGas = cold_gas;
  gal->StellarMass = 1.0f;
  gal->H2gas = h2;
  gal->H1gas = h1;
  mimic_sage_update_from_star_formation(gal, stars, 0.0, 0.43, sf_prescription, caps);
}

/**
 * @test    test_sf_commit_hydrogen_caps
 * @brief   update_from_star_formation at SFprescription 1 caps H2gas and H1gas independently and
 *          strictly at max_h of the cold gas it leaves, never their sum (S:1131-1135)
 */
int test_sf_commit_hydrogen_caps(void) {
  struct GalaxyData gal;
  struct MimicSageHydrogenCapLocals caps;
  const float bound = hydrogen_bound(1.0f); /* a zero-star event leaves ColdGas 1.0 */
  const float above = nextafterf(bound, INFINITY);
  const float below = nextafterf(bound, 0.0f);

  /* H2 above (capped to the bound), H1 at the bound (unchanged) */
  commit_star_formation(&gal, 1.0f, above, bound, 0.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(caps.tracks_h2 == 1 && caps.max_h == bound, "the bound is max_h of the cold gas");
  TEST_ASSERT(gal.H2gas == bound && caps.h2_cap_binds == 1, "H2gas above the bound is capped");
  TEST_ASSERT(gal.H1gas == bound && caps.h1_cap_binds == 0, "H1gas at the bound is unchanged");

  /* H2 at the bound (unchanged), H1 above (capped) */
  commit_star_formation(&gal, 1.0f, bound, above, 0.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(gal.H2gas == bound && caps.h2_cap_binds == 0, "H2gas at the bound is unchanged");
  TEST_ASSERT(gal.H1gas == bound && caps.h1_cap_binds == 1, "H1gas above the bound is capped");

  /* Both below the bound (unchanged) */
  commit_star_formation(&gal, 1.0f, below, below, 0.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(gal.H2gas == below && gal.H1gas == below, "H2gas and H1gas below are unchanged");

  /* Both below, their sum above: the caps never act on the sum */
  commit_star_formation(&gal, 1.0f, 0.5f, 0.5f, 0.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(0.5f + 0.5f > bound, "the sum is above the bound");
  TEST_ASSERT(gal.H2gas == 0.5f && gal.H1gas == 0.5f, "the caps never act on the sum");

  /* The bound is the cold gas the stars leave, not the cold gas before them */
  commit_star_formation(&gal, 2.0f, 1.4f, 0.1f, 0.5, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(gal.ColdGas < 2.0f, "the stars deplete the cold gas");
  TEST_ASSERT(caps.max_h == hydrogen_bound(gal.ColdGas), "the bound uses the remaining cold gas");
  TEST_ASSERT(1.4f < hydrogen_bound(2.0f) && gal.H2gas == hydrogen_bound(gal.ColdGas),
              "H2gas within the hydrogen before SF is capped to the hydrogen after it");
  TEST_ASSERT(gal.H1gas == 0.1f, "H1gas below the remaining hydrogen is unchanged");

  /* No cold gas: the bound is +0.0 */
  commit_star_formation(&gal, 0.0f, 0.1f, 0.2f, 0.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(caps.max_h == 0.0f && !signbit(caps.max_h), "with no cold gas the bound is +0.0");
  TEST_ASSERT(gal.H2gas == 0.0f && !signbit(gal.H2gas), "H2gas is capped to +0.0");
  TEST_ASSERT(gal.H1gas == 0.0f && !signbit(gal.H1gas), "H1gas is capped to +0.0");

  /* The caps are strict: a -0.0 H2gas or H1gas at a +0.0 bound is kept as -0.0 */
  commit_star_formation(&gal, 0.0f, -0.0f, -0.0f, 0.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &caps);
  TEST_ASSERT(gal.H2gas == 0.0f && signbit(gal.H2gas), "a -0.0 H2gas at a +0.0 bound is kept");
  TEST_ASSERT(gal.H1gas == 0.0f && signbit(gal.H1gas), "a -0.0 H1gas at a +0.0 bound is kept");

  /* SFprescription 0: no cap, H2gas and H1gas not written */
  commit_star_formation(&gal, 1.0f, 5.0f, 6.0f, 0.5, MIMIC_SAGE_SF_PRESCRIPTION_CROTON, &caps);
  TEST_ASSERT(caps.tracks_h2 == 0, "SFprescription 0 does not track H2");
  TEST_ASSERT(gal.H2gas == 5.0f && gal.H1gas == 6.0f,
              "at SFprescription 0 the commit writes neither H2gas nor H1gas");
  return TEST_PASS;
}

/** @brief A feedback commit of a galaxy (reheating into a separate central) */
static void commit_feedback(struct GalaxyData *gal, struct GalaxyData *central,
                            const float cold_gas, const float h2, const float h1,
                            const double reheated, const int sf_prescription,
                            struct MimicSageFeedbackLocals *locals) {
  memset(gal, 0, sizeof(*gal));
  memset(central, 0, sizeof(*central));
  gal->ColdGas = cold_gas;
  gal->H2gas = h2;
  gal->H1gas = h1;
  /* The central holds far more H2 and H1 than any bound here: a cap on it would show */
  central->HotGas = 10.0f;
  central->H2gas = 50.0f;
  central->H1gas = 60.0f;
  mimic_sage_update_from_feedback(gal, central, reheated, 0.0, 0.0, sf_prescription,
                                  MIMIC_SAGE_CGM_RECIPE_OFF, locals);
}

/**
 * @test    test_feedback_commit_hydrogen_caps
 * @brief   update_from_feedback at SFprescription 1 caps the reheated galaxy's H2gas and H1gas
 *          independently and strictly at max_h of its cold gas after the debit, never their sum,
 *          and never the central's (S:1169-1173)
 */
int test_feedback_commit_hydrogen_caps(void) {
  struct GalaxyData gal;
  struct GalaxyData central;
  struct MimicSageFeedbackLocals locals;
  const float bound = hydrogen_bound(0.75f); /* 1.0 - 0.25 reheated, exact in float */
  const float above = nextafterf(bound, INFINITY);
  const float below = nextafterf(bound, 0.0f);

  /* H2 above (capped), H1 at the bound (unchanged); the central untouched */
  commit_feedback(&gal, &central, 1.0f, above, bound, 0.25, MIMIC_SAGE_SF_PRESCRIPTION_BR06,
                  &locals);
  TEST_ASSERT(gal.ColdGas == 0.75f, "the reheated gas leaves the cold gas");
  TEST_ASSERT(locals.hydrogen_caps.max_h == bound, "the bound is max_h of the debited cold gas");
  TEST_ASSERT(gal.H2gas == bound && locals.hydrogen_caps.h2_cap_binds == 1,
              "H2gas above the bound is capped");
  TEST_ASSERT(gal.H1gas == bound && locals.hydrogen_caps.h1_cap_binds == 0,
              "H1gas at the bound is unchanged");
  TEST_ASSERT(central.HotGas == 10.25f, "the reheated gas reaches the central's hot gas");
  TEST_ASSERT(central.H2gas == 50.0f && central.H1gas == 60.0f,
              "the cap acts on the reheated galaxy only, not the central");

  /* H2 at the bound (unchanged), H1 above (capped) */
  commit_feedback(&gal, &central, 1.0f, bound, above, 0.25, MIMIC_SAGE_SF_PRESCRIPTION_BR06,
                  &locals);
  TEST_ASSERT(gal.H2gas == bound && locals.hydrogen_caps.h2_cap_binds == 0,
              "H2gas at the bound is unchanged");
  TEST_ASSERT(gal.H1gas == bound && locals.hydrogen_caps.h1_cap_binds == 1,
              "H1gas above the bound is capped");

  /* Both below (unchanged) */
  commit_feedback(&gal, &central, 1.0f, below, below, 0.25, MIMIC_SAGE_SF_PRESCRIPTION_BR06,
                  &locals);
  TEST_ASSERT(gal.H2gas == below && gal.H1gas == below, "H2gas and H1gas below are unchanged");

  /* Both below, their sum above: the caps never act on the sum */
  commit_feedback(&gal, &central, 1.0f, 0.4f, 0.4f, 0.25, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &locals);
  TEST_ASSERT(0.4f + 0.4f > bound, "the sum is above the bound");
  TEST_ASSERT(gal.H2gas == 0.4f && gal.H1gas == 0.4f, "the caps never act on the sum");

  /* All the cold gas reheated (the clamp): the bound is +0.0 */
  commit_feedback(&gal, &central, 0.5f, 0.1f, 0.2f, 1.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06, &locals);
  TEST_ASSERT(gal.ColdGas == 0.0f, "the clamped reheating empties the cold gas");
  TEST_ASSERT(locals.hydrogen_caps.max_h == 0.0f && !signbit(locals.hydrogen_caps.max_h),
              "with no cold gas the bound is +0.0");
  TEST_ASSERT(gal.H2gas == 0.0f && !signbit(gal.H2gas), "H2gas is capped to +0.0");
  TEST_ASSERT(gal.H1gas == 0.0f && !signbit(gal.H1gas), "H1gas is capped to +0.0");
  TEST_ASSERT(central.H2gas == 50.0f && central.H1gas == 60.0f,
              "the central's H2gas and H1gas are untouched");

  /* The caps are strict: a -0.0 H2gas or H1gas at a +0.0 bound is kept as -0.0 */
  commit_feedback(&gal, &central, 0.5f, -0.0f, -0.0f, 1.0, MIMIC_SAGE_SF_PRESCRIPTION_BR06,
                  &locals);
  TEST_ASSERT(gal.H2gas == 0.0f && signbit(gal.H2gas), "a -0.0 H2gas at a +0.0 bound is kept");
  TEST_ASSERT(gal.H1gas == 0.0f && signbit(gal.H1gas), "a -0.0 H1gas at a +0.0 bound is kept");

  /* SFprescription 0: no cap, H2gas and H1gas not written */
  commit_feedback(&gal, &central, 1.0f, 5.0f, 6.0f, 0.25, MIMIC_SAGE_SF_PRESCRIPTION_CROTON,
                  &locals);
  TEST_ASSERT(locals.hydrogen_caps.tracks_h2 == 0, "SFprescription 0 does not track H2");
  TEST_ASSERT(gal.H2gas == 5.0f && gal.H1gas == 6.0f,
              "at SFprescription 0 the commit writes neither H2gas nor H1gas");
  return TEST_PASS;
}

/**
 * @test    test_satellite_reheats_to_central
 * @brief   Test that satellites reheat gas to central's hot halo (not their own)
 *
 * Physics: Satellites are stripped, so reheated gas goes to central
 *
 * Expected: Satellite's cold gas decreases, central's hot gas increases
 * Validates: Satellite vs central behavior
 */
int test_satellite_reheats_to_central(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo satellite_halo, central_halo;
  struct GalaxyData satellite_galaxy, central_galaxy;

  /* Setup central */
  double central_hot_initial = 50.0;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, central_hot_initial, 0.0,
                  20.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

  /* Setup satellite (Type=1) */
  double satellite_cold_initial = 5.0;
  double satellite_hot_initial =
      10.0; /* Satellite has own hot gas, but reheating goes to central */
  double new_stars = 0.5;
  double reheated_mass = 0.3;
  setup_test_halo(&satellite_halo, &satellite_galaxy, 1, 20.0, 0.1, /* Type=1 (satellite) */
                  satellite_cold_initial, satellite_hot_initial, 0.0, 5.0, 0.0, 0.0, 0.0, 0.0,
                  new_stars, reheated_mass, 0.0);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &satellite_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Satellite's cold gas should decrease */
  double sf_depletion = (1.0 - test_recycle_fraction) * new_stars;
  double expected_satellite_cold = satellite_cold_initial - sf_depletion - reheated_mass;
  TEST_ASSERT_DOUBLE_EQUAL(satellite_galaxy.ColdGas, expected_satellite_cold, 1e-6,
                           "Satellite ColdGas should decrease by SF + reheating");

  /* Satellite's hot gas should be unchanged (reheating goes to central) */
  TEST_ASSERT_DOUBLE_EQUAL(satellite_galaxy.HotGas, satellite_hot_initial, 1e-6,
                           "Satellite HotGas should be unchanged");

  /* Central's hot gas should increase */
  double expected_central_hot = central_hot_initial + reheated_mass;
  TEST_ASSERT_DOUBLE_EQUAL(central_galaxy.HotGas, expected_central_hot, 1e-6,
                           "Central HotGas should increase by satellite's reheated mass");

  return TEST_PASS;
}

// ============================================================================
// CLEANUP AND CONSERVATION TESTS
// ============================================================================

/**
 * @test    test_temporary_properties_reset
 * @brief   Test that temporary properties are zeroed after processing
 *
 * Physics: NewStellarMass, SupernovaReheatedMass, SupernovaEjectedMass must be zeroed
 *
 * Expected: All temporary properties = 0 after processing
 * Validates: Cleanup logic prevents accumulation across timesteps
 */
int test_temporary_properties_reset(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0);

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0,
              "Metal enrichment module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  /* Galaxy with non-zero temporary properties */
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0,
                  0.0, 1.0, 0.5, 0.3); /* Non-zero temporary values */

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* SN transport properties are consumed by the apply step; NewStellarMass is
   * intentionally preserved for sage26_apply_metal_enrichment (SAGE ordering) */
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, 1.0, 1e-6,
                           "NewStellarMass should be preserved for the enrichment module");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaReheatedMass, 0.0, 1e-6,
                           "SupernovaReheatedMass should be zeroed after processing");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.SupernovaEjectedMass, 0.0, 1e-6,
                           "SupernovaEjectedMass should be zeroed after processing");

  /* The enrichment module consumes NewStellarMass */
  result = sage26_apply_metal_enrichment_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Metal enrichment process should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(test_galaxy.NewStellarMass, 0.0, 1e-6,
                           "NewStellarMass should be zeroed after metal enrichment");

  return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Test that total baryonic mass is conserved (excluding yield production)
 *
 * Physics: (ColdGas + HotGas + EjectedGas + StellarMass)_after =
 *          (ColdGas + HotGas + EjectedGas + StellarMass)_before
 *
 * Expected: Total baryons unchanged (recycling preserves mass)
 * Validates: Mass conservation
 */
int test_mass_conservation(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.0, 0.0); /* Yield=0 to test pure conservation */

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central */
  double central_cold_initial = 10.0;
  double central_hot_initial = 50.0;
  double central_ejected_initial = 5.0;
  double central_stellar_initial = 20.0;
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, central_cold_initial,
                  central_hot_initial, central_ejected_initial, central_stellar_initial, 0.0, 0.0,
                  0.0, 0.0, 0.0, 0.0, 0.0);

  /* Setup test galaxy */
  double galaxy_cold_initial = 10.0;
  double galaxy_hot_initial = 50.0;
  double galaxy_ejected_initial = 5.0;
  double galaxy_stellar_initial = 20.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, galaxy_cold_initial, galaxy_hot_initial,
                  galaxy_ejected_initial, galaxy_stellar_initial, 0.0, 0.0, 0.0, 0.0, 1.0, 0.5,
                  0.3);

  /* Calculate total baryons before (galaxy + central since transfers happen) */
  double total_before =
      (galaxy_cold_initial + galaxy_hot_initial + galaxy_ejected_initial + galaxy_stellar_initial) +
      (central_cold_initial + central_hot_initial + central_ejected_initial +
       central_stellar_initial);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module process should succeed");

  /* Calculate total baryons after */
  double total_after = (test_galaxy.ColdGas + test_galaxy.HotGas + test_galaxy.EjectedGas +
                        test_galaxy.StellarMass) +
                       (central_galaxy.ColdGas + central_galaxy.HotGas + central_galaxy.EjectedGas +
                        central_galaxy.StellarMass);

  TEST_ASSERT_DOUBLE_EQUAL(total_after, total_before, 1e-5,
                           "Total baryonic mass should be conserved");

  return TEST_PASS;
}

/**
 * @test    test_metal_conservation
 * @brief   Test that total metals are conserved (except for new yield production)
 *
 * Physics: Total_metals_after = Total_metals_before + Yield * NewStellarMass
 *
 * Expected: Metals conserved with yield production accounted for
 * Validates: Metal conservation with stellar nucleosynthesis
 */
int test_metal_conservation(void) {
  /* ===== SETUP ===== */
  setup_test_parameters(0.43, 0.025, 0.0); /* Yield=0.025 */

  int init_result = sage26_apply_star_formation_supernova_init();
  TEST_ASSERT(init_result == 0, "Module initialization should succeed");
  TEST_ASSERT(sage26_apply_metal_enrichment_init() == 0,
              "Metal enrichment module initialization should succeed");

  struct Halo test_halo, central_halo;
  struct GalaxyData test_galaxy, central_galaxy;

  /* Setup central */
  setup_test_halo(&central_halo, &central_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 5.0, 20.0, 0.1, 0.5,
                  0.05, 0.3, 0.0, 0.0, 0.0);

  /* Setup test galaxy */
  double new_stars = 1.0;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 5.0, 20.0, 0.2, 0.5, 0.1,
                  0.4, new_stars, 0.5, 0.3);

  /* Calculate total metals before (all reservoirs, galaxy + central) */
  double total_metals_before = (test_galaxy.MetalsColdGas + test_galaxy.MetalsHotGas +
                                test_galaxy.MetalsEjectedGas + test_galaxy.MetalsStellarMass) +
                               (central_galaxy.MetalsColdGas + central_galaxy.MetalsHotGas +
                                central_galaxy.MetalsEjectedGas + central_galaxy.MetalsStellarMass);

  struct ModuleContext ctx;
  setup_module_context(&ctx, &central_halo, 0.1);

  /* ===== EXECUTE ===== */
  /* Run the full SF/SN apply -> metal enrichment pipeline (SAGE ordering) */
  int result = sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1);
  TEST_ASSERT(result == 0, "Module process should succeed");
  result = sage26_apply_metal_enrichment_process(&ctx, &test_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Metal enrichment process should succeed");

  /* Calculate total metals after */
  double total_metals_after = (test_galaxy.MetalsColdGas + test_galaxy.MetalsHotGas +
                               test_galaxy.MetalsEjectedGas + test_galaxy.MetalsStellarMass) +
                              (central_galaxy.MetalsColdGas + central_galaxy.MetalsHotGas +
                               central_galaxy.MetalsEjectedGas + central_galaxy.MetalsStellarMass);

  /* Expected new metals from yield */
  double expected_new_metals = test_yield * new_stars;
  double expected_total_metals = total_metals_before + expected_new_metals;

  TEST_ASSERT_DOUBLE_EQUAL(total_metals_after, expected_total_metals, 1e-5,
                           "Total metals should equal initial + yield production");

  return TEST_PASS;
}

// ============================================================================
// DISK STAR-FORMATION HISTORY SAMPLING
// ============================================================================

/** @brief Element (history, bin) of a galaxy */
static float history(struct GalaxyData *gal, const enum MimicSageSfrHistory h, const int bin) {
  return *mimic_sage_sfr_history(gal, h, bin);
}

/**
 * @test    test_disk_history_sampled_every_call
 * @brief   Every call samples the disk history at its substep's bin, zero stars included
 *
 * SAGE26 samples on every starformation_and_feedback call, after the feedback
 * renormalisation and before any reservoir update
 * (R/src/model_starformation_and_feedback.c:1053-1060):
 *   SfrDisk[bin] += stars / dt; SfrDiskColdGas[bin] = ColdGas;
 *   SfrDiskColdGasMetals[bin] = MetalsColdGas.
 * With 13 substeps, bins 0, 3 and 6 are shared by two substeps (bin (step * 10) / 13).
 * Every third substep forms no stars. The expected rate is formed with SAGE26's
 * expression in float storage; the gas samples are the reservoirs before the call's
 * commit, the last substep of a bin winning.
 */
int test_disk_history_sampled_every_call(void) {
  reset_config();
  setup_test_parameters(0.43, 0.025, 0.0);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 100.0, 0.13, 10.0, 50.0, 5.0, 20.0, 0.2, 0.5, 0.1, 0.3, 0.0, 0.0,
                  0.0);
  struct ModuleContext ctx;
  setup_module_context(&ctx, &halo, 0.01);
  ctx.num_substeps = 13;
  const double dt = halo.dT / 13.0;

  struct GalaxyData expected;
  memcpy(&expected, &gal, sizeof(expected));
  for (int step = 0; step < 13; step++) {
    ctx.substep_number = step;
    const double stars = (step % 3 == 2) ? 0.0 : 0.01 * (step + 1);
    gal.NewStellarMass = stars;
    gal.SupernovaReheatedMass = 0.0;
    gal.SupernovaEjectedMass = 0.0;
    const int bin = (step * 10) / 13;
    *mimic_sage_sfr_history(&expected, MIMIC_SFR_HISTORY_DISK, bin) += stars / dt;
    *mimic_sage_sfr_history(&expected, MIMIC_SFR_HISTORY_DISK_COLD_GAS, bin) = gal.ColdGas;
    *mimic_sage_sfr_history(&expected, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, bin) =
        gal.MetalsColdGas;

    TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
                "process should succeed");
    for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
      for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
        TEST_ASSERT(history(&gal, (enum MimicSageSfrHistory)h, k) ==
                        history(&expected, (enum MimicSageSfrHistory)h, k),
                    "every history element equals SAGE26's sampling after every substep");
      }
    }
  }
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK, 6) > 0.0f, "a shared bin sums its rates");
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_disk_history_zero_stars
 * @brief   A zero-star call adds 0 to the rate and still assigns both gas samples
 */
int test_disk_history_zero_stars(void) {
  reset_config();
  setup_test_parameters(0.43, 0.025, 0.0);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 100.0, 0.1, 3.0, 50.0, 5.0, 20.0, 0.06, 0.5, 0.1, 0.3, 0.0, 0.0,
                  0.0);
  *mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_DISK, 4) = 7.0f;
  *mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 4) = 99.0f;
  *mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, 4) = 98.0f;
  struct ModuleContext ctx;
  setup_module_context(&ctx, &halo, 0.01);
  ctx.num_substeps = 10;
  ctx.substep_number = 4;

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
              "process should succeed");
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK, 4) == 7.0f, "zero stars add nothing");
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 4) == 3.0f,
              "the cold gas is sampled on a zero-star call");
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, 4) == 0.06f,
              "the cold metals are sampled on a zero-star call");
  for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
    if (k != 4) {
      TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, k) == 0.0f,
                  "only the substep's bin is sampled");
    }
  }
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_disk_history_without_central
 * @brief   With no FoF central (a Mimic-only path) the history is sampled, nothing committed
 */
int test_disk_history_without_central(void) {
  reset_config();
  setup_test_parameters(0.43, 0.025, 0.0);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 1, 10.0, 0.1, 2.0, 0.0, 0.0, 1.0, 0.02, 0.0, 0.0, 0.01, 0.5, 0.1,
                  0.0);
  struct ModuleContext ctx;
  setup_module_context(&ctx, NULL, 0.01);
  ctx.num_substeps = 10;
  ctx.substep_number = 9;

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
              "process should succeed");
  float rate = 0.0f;
  rate += 0.5 / (0.1 / 10.0);
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK, 9) == rate, "the rate is sampled");
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 9) == 2.0f, "the gas is sampled");
  TEST_ASSERT(gal.ColdGas == 2.0f, "nothing is committed without a central");
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_disk_history_initial_boundary
 * @brief   Without a positive interval (snapshot-zero birth) there is no dt: nothing sampled
 */
int test_disk_history_initial_boundary(void) {
  reset_config();
  setup_test_parameters(0.43, 0.025, 0.0);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 10.0, -1.0, 2.0, 1.0, 0.0, 1.0, 0.02, 0.0, 0.0, 0.01, 0.0, 0.0,
                  0.0);
  halo.SnapNum = -1;
  struct ModuleContext ctx;
  setup_module_context(&ctx, &halo, 0.0);
  ctx.num_substeps = 10;

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
              "the boundary is not an error");
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
      TEST_ASSERT(history(&gal, (enum MimicSageSfrHistory)h, k) == 0.0f, "nothing is sampled");
    }
  }

  /* A non-positive interval off the boundary is an inconsistent state */
  halo.SnapNum = 40;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) != 0,
              "an invalid interval is an error");
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER

// ============================================================================

// ============================================================================
// CGM ROUTING TESTS (c10, CGMrecipeOn)
// ============================================================================
//
// SAGE26's update_from_feedback (R = the staged SAGE26 source,
// R/src/model_starformation_and_feedback.c:1146-1251) credits the reheated gas to, caps the
// ejection against and ejects from the FoF CENTRAL's CGM pair when CGMrecipeOn is 1 and the
// central's Regime is 0 (:1175-1199), and its hot pair otherwise (:1200-1223, :1224-1247). The
// forming galaxy's own Regime is never read. Each case below gives the central's two pairs the SAME
// input values, so the selected pair ends at the case's literal values and the unselected one keeps
// its input; the forming satellite always holds the other Regime. Every value is a short binary
// fraction, so every expectation is an exact literal:
//
//   satellite ColdGas 4, MetalsColdGas 0.5 (Z 0.125), no stars, reheated 1 -> ColdGas 3,
//   MetalsColdGas 0.375, outflow 1; the central's ejected pair 0.5 / 0.0625.
//   A (no cap binds): pairs 2 / 0.25, ejected 0.5 -> selected 3 / 0.375 after the reheating,
//     metallicity 0.125, -> 2.5 / 0.3125; ejected pair 1 / 0.125.
//   B (the gas cap binds, :1184-1186 / :1208-1210 / :1232-1234): ejected 5 -> capped to the
//     selected 3 after the reheating -> 0 / 0; ejected pair 3.5 / 0.4375.
//   C (the metal cap binds, :1189-1192 / :1213-1216 / :1237-1240): pairs 2 / -0.5 -> 3 / -0.375
//     after the reheating; metallicity 0 (get_metallicity of negative metals), the transfer 0 is
//     above -0.375, so it is capped to -0.375 -> 2.5 / 0; ejected pair 1 / -0.3125. A constructed
//     edge state (negative metals arise from float cancellation, as test_zero_event_caps_negative_
//     hot_metals uses for the hot pair).

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

/** @brief Initialise the module with CGMrecipeOn `value` (NULL: missing), capturing the log */
static int c10_init(const char *value, const char **log) {
  setup_test_parameters(0.5, 0.0, 0.0);
  c10_set_cgm_recipe_on(value);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_apply_star_formation_supernova_init();
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
 * @test    test_cgm_feedback_destination
 * @brief   The disk commit's feedback goes to the FoF central's pair its Regime selects
 */
int test_cgm_feedback_destination(void) {
  const struct {
    const char *cgm;
    int central_regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}, {"0", 1, 0}};
  const struct {
    const char *name;
    float pair_gas, pair_metals; /* both central pairs' input */
    double ejected;              /* SupernovaEjectedMass */
    float gas, metals;           /* the selected pair after the call */
    float ejected_gas, ejected_metals;
  } cases[] = {{"A no cap", 2.0f, 0.25f, 0.5, 2.5f, 0.3125f, 1.0f, 0.125f},
               {"B gas cap", 2.0f, 0.25f, 5.0, 0.0f, 0.0f, 3.5f, 0.4375f},
               {"C metal cap", 2.0f, -0.5f, 0.5, 2.5f, 0.0f, 1.0f, -0.3125f}};
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      const char *log = "";
      TEST_ASSERT(c10_init(runs[r].cgm, &log) == 0, "init must accept CGMrecipeOn 0 and 1");

      struct Halo central_halo, satellite_halo;
      struct GalaxyData central, satellite;
      setup_test_halo(&central_halo, &central, 0, 100.0, 0.1, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0625,
                      0.0, 0.0, 0.0, 0.0);
      central.Regime = runs[r].central_regime;
      central.HotGas = cases[c].pair_gas;
      central.MetalsHotGas = cases[c].pair_metals;
      central.CGMgas = cases[c].pair_gas;
      central.MetalsCGMgas = cases[c].pair_metals;
      setup_test_halo(&satellite_halo, &satellite, 1, 20.0, 0.1, 4.0, 7.0, 0.0, 1.0, 0.5, 0.75, 0.0,
                      0.0, 0.0, 1.0, cases[c].ejected);
      satellite.Regime = 1 - runs[r].central_regime;
      satellite.CGMgas = 6.0f;
      satellite.MetalsCGMgas = 0.5f;

      struct ModuleContext ctx;
      setup_module_context(&ctx, &central_halo, 0.1);
      const int status = sage26_apply_star_formation_supernova_process(&ctx, &satellite_halo, 1);

      const float *sel_gas = runs[r].to_cgm ? &central.CGMgas : &central.HotGas;
      const float *sel_metals = runs[r].to_cgm ? &central.MetalsCGMgas : &central.MetalsHotGas;
      const float *oth_gas = runs[r].to_cgm ? &central.HotGas : &central.CGMgas;
      const float *oth_metals = runs[r].to_cgm ? &central.MetalsHotGas : &central.MetalsCGMgas;
      const int selected_ok = *sel_gas == cases[c].gas && *sel_metals == cases[c].metals;
      const int unselected_ok =
          *oth_gas == cases[c].pair_gas && *oth_metals == cases[c].pair_metals;
      const int ejected_ok = central.EjectedGas == cases[c].ejected_gas &&
                             central.MetalsEjectedGas == cases[c].ejected_metals;
      const int satellite_ok = satellite.ColdGas == 3.0f && satellite.MetalsColdGas == 0.375f &&
                               satellite.SupernovaOutflowRate == 1.0f && satellite.HotGas == 7.0f &&
                               satellite.MetalsHotGas == 0.75f && satellite.CGMgas == 6.0f &&
                               satellite.MetalsCGMgas == 0.5f && satellite.EjectedGas == 0.0f;
      if (status != 0 || !selected_ok || !unselected_ok || !ejected_ok || !satellite_ok) {
        printf("\n  CGMrecipeOn %s, central Regime %d, case %s: status %d, hot %.9g/%.9g, CGM "
               "%.9g/%.9g, ejected %.9g/%.9g, satellite cold %.9g/%.9g",
               runs[r].cgm, runs[r].central_regime, cases[c].name, status, (double)central.HotGas,
               (double)central.MetalsHotGas, (double)central.CGMgas, (double)central.MetalsCGMgas,
               (double)central.EjectedGas, (double)central.MetalsEjectedGas,
               (double)satellite.ColdGas, (double)satellite.MetalsColdGas);
        ok = 0;
      }
      sage26_apply_star_formation_supernova_cleanup();
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the feedback must reach exactly the FoF central's selected pair");
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_refused
 * @brief   CGMrecipeOn is required (missing key fails init) and only 0 or 1 is accepted, a refusal
 *          naming the parameter (campaign decision D33)
 */
int test_cgm_recipe_on_refused(void) {
  const char *log = "";
  TEST_ASSERT(c10_init(NULL, &log) != 0, "init must fail when CGMrecipeOn is missing");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the missing-key error must name CGMrecipeOn");
  TEST_ASSERT(c10_init("2", &log) != 0, "init must refuse CGMrecipeOn 2");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init("-1", &log) != 0, "init must refuse CGMrecipeOn -1");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the refusal must name CGMrecipeOn");
  TEST_ASSERT(c10_init("1", &log) == 0, "init must accept CGMrecipeOn 1");
  TEST_ASSERT(c10_init("0", &log) == 0, "init must accept CGMrecipeOn 0");
  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST BYPASS (c13 Slice 3)
// ============================================================================

/**
 * @test    test_ffb_row_bypassed
 * @brief   An FFBRegime 1 row is not processed; the same row with FFBRegime 0 is
 *
 * SAGE26's starformation_and_feedback hands an FFBRegime 1 row to starformation_ffb and returns
 * before its ordinary history sample and commits (R/src/model_starformation_and_feedback.c
 * :1004-1008). The row is a satellite with seeded transport fields and its own FoF central; with
 * FFBRegime 1 both galaxy structs are compared byte for byte with their inputs, in a valid
 * interval and in one the time guard refuses on an ordinary row (dT 0 with a stored snapshot).
 * With FFBRegime 0 the expectations are literals: SfrDisk[0] += 1.0 / 0.1 (:1057), the locked
 * (1 - 0.43) * 1.0 leaves the cold gas (:1121-1124), then the reheated 2.0 (:1166), which the
 * central's hot gas gains before 0.5 is ejected from it (CGMrecipeOn 0, :1203-1222).
 */
int test_ffb_row_bypassed(void) {
  reset_config();
  setup_test_parameters(0.43, 0.0, 0.0);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "Module init should succeed");

  struct Halo central, row;
  struct GalaxyData central_gal, gal;
  setup_test_halo(&central, &central_gal, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0, 0.0,
                  0.0, 0.0, 0.0);
  setup_test_halo(&row, &gal, 1, 10.0, 0.1, 10.0, 5.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 2.0, 0.5);
  gal.FFBRegime = 1;
  struct ModuleContext ctx;
  setup_module_context(&ctx, &central, 0.1);

  struct GalaxyData row_in, central_in;
  memcpy(&row_in, &gal, sizeof(gal));
  memcpy(&central_in, &central_gal, sizeof(central_gal));

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) == 0,
              "process succeeds on an FFB row");
  TEST_ASSERT(memcmp(&gal, &row_in, sizeof(gal)) == 0,
              "FFBRegime 1: every field of the row is unchanged, its history included");
  TEST_ASSERT(memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: every field of its central is unchanged");

  row.dT = 0.0;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) == 0 &&
                  memcmp(&gal, &row_in, sizeof(gal)) == 0 &&
                  memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: returns before the history sample's time guard, writing nothing");
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) != 0,
              "the time guard refuses that interval on an ordinary row");
  row.dT = 0.1;

  memcpy(&gal, &row_in, sizeof(gal));
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) == 0,
              "process succeeds on an ordinary row");
  printf("\n  FFBRegime 0: SfrDisk[0] %.9g, ColdGas %.9g, StellarMass %.9g, central HotGas %.9g, "
         "central EjectedGas %.9g",
         (double)gal.SfrDiskBin0, (double)gal.ColdGas, (double)gal.StellarMass,
         (double)central_gal.HotGas, (double)central_gal.EjectedGas);
  TEST_ASSERT_DOUBLE_EQUAL(gal.SfrDiskBin0, 10.0, 1e-5, "FFBRegime 0: the history is sampled");
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, 2.57, 1e-5, "FFBRegime 0: the locked stars");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, 10.0 - 0.57 - 2.0, 1e-5,
                           "FFBRegime 0: the locked stars and the reheated gas leave the cold gas");
  TEST_ASSERT_DOUBLE_EQUAL(
      central_gal.HotGas, 50.0 + 2.0 - 0.5, 1e-5,
      "FFBRegime 0: the central's hot gas gains the reheated, loses the ejected");
  TEST_ASSERT_DOUBLE_EQUAL(central_gal.EjectedGas, 0.5, 1e-7, "FFBRegime 0: the ejection");
  TEST_ASSERT(gal.HotGas == 5.0f, "FFBRegime 0: the row's own hot gas is not the destination");
  TEST_ASSERT(gal.SupernovaReheatedMass == 0.0 && gal.SupernovaEjectedMass == 0.0,
              "FFBRegime 0: the consumed transport fields are zeroed");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_apply_star_formation_supernova unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_apply_star_formation_supernova Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Run software quality tests */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);

  /* Run star formation update tests */
  TEST_RUN(test_star_formation_with_recycling);
  TEST_RUN(test_star_formation_metal_transfer);
  TEST_RUN(test_star_formation_rate_accumulation);

  /* Run supernova reheating tests */
  TEST_RUN(test_reheating_gas_transfer);
  TEST_RUN(test_reheating_metal_transfer);
  TEST_RUN(test_reheating_outflow_rate);

  /* Run supernova ejection tests */
  TEST_RUN(test_ejection_gas_transfer);
  TEST_RUN(test_ejection_limiting);
  TEST_RUN(test_ejection_metal_transfer);

  /* Run metal enrichment tests */
  TEST_RUN(test_metal_yield_production);
  TEST_RUN(test_metal_disk_vs_halo_split);
  TEST_RUN(test_metal_enrichment_zero_cold_gas);

  /* Run edge case tests */
  TEST_RUN(test_zero_stars_no_updates);
  TEST_RUN(test_zero_event_caps_negative_hot_metals);
  TEST_RUN(test_sf_commit_fused_rounding);
  TEST_RUN(test_sf_commit_hydrogen_caps);
  TEST_RUN(test_feedback_commit_hydrogen_caps);
  TEST_RUN(test_satellite_reheats_to_central);

  /* Run cleanup and conservation tests */
  TEST_RUN(test_temporary_properties_reset);
  TEST_RUN(test_mass_conservation);
  TEST_RUN(test_metal_conservation);

  /* Run disk star-formation history sampling tests */
  TEST_RUN(test_disk_history_sampled_every_call);
  TEST_RUN(test_disk_history_zero_stars);
  TEST_RUN(test_disk_history_without_central);
  TEST_RUN(test_disk_history_initial_boundary);
  TEST_RUN(test_cgm_feedback_destination);
  TEST_RUN(test_cgm_recipe_on_refused);

  /* The feedback-free-burst bypass (c13 Slice 3) */
  TEST_RUN(test_ffb_row_bypassed);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
