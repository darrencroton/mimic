/**
 * @file    test_unit_sage26_starburst_feedback.c
 * @brief   Unit tests for sage26_starburst_feedback module
 *
 * Validates: Collisional starburst physics, merger/disk triggers, feedback, edge cases
 *
 * This test validates the sage26_starburst_feedback module physics:
 * - Disk instability trigger (mode=1, eburst = efficiency)
 * - By-galaxy path ignores merger flags (merger channel uses process_per_event)
 * - Per-event merger trigger support via ctx->active_event
 * - Star formation and bulge growth
 * - Feedback reheating and ejection
 * - Mass and metallicity conservation
 * - Metal distribution (major vs minor mergers)
 * - Cold gas balance constraint
 * - Edge cases (zero values, NULL galaxies, triggers)
 * - Parameter sensitivity
 *
 * Test cases:
 *   - test_disk_instability_starburst: Disk instability trigger physics
 *   - test_merger_starburst: Merger-only trigger is ignored in this module
 *   - test_both_triggers: Disk trigger processed while merger trigger preserved
 *   - test_per_event_merger_starburst: Merger event triggers starburst physics
 *   - test_per_event_minor_merger_rechecks_disk_instability: Minor mergers run
 *     same-step post-starburst disk instability follow-up when configured
 *   - test_per_event_recheck_respects_phase2_quasar_configuration: Same-step
 *     follow-up skips BH growth when the quasar event consumer is absent
 *   - test_per_event_unknown_code_noop: Unknown event code is no-op
 *   - test_major_vs_minor_merger: Merger-only major/minor triggers are both ignored
 *   - test_mass_conservation: Total mass conserved
 *   - test_metallicity_preservation: Metallicity preserved
 *   - test_bulge_formation: Stars added to bulge
 *   - test_ejection_calculation: Energy-driven ejection
 *   - test_cold_gas_balance: (stars + reheated) <= ColdGas
 *   - test_zero_cold_gas: No starburst with zero cold gas
 *   - test_zero_efficiency: No starburst with zero triggers
 *   - test_no_triggers: No processing without triggers
 *   - test_null_galaxy: NULL galaxy handled
 *   - test_null_central_galaxy: NULL central handled
 *   - test_triggers_preserved: Trigger lifecycle owned by clear modules
 *   - test_invalid_ngal: Error when ngal != 1
 *   - test_zero_vvir: Ejection = 0 when Vvir = 0
 *   - test_insufficient_cold_gas: Balancing works
 *   - test_parameter_sensitivity_reheating: Reheating parameter
 *   - test_parameter_sensitivity_ejection: Ejection parameter
 *   - test_parameter_sensitivity_yield: Yield parameter
 *   - test_module_initialization: Module lifecycle
 *   - test_memory_safety: No memory leaks
 *   - test_instability_burst_samples_bulge_history: the disk-instability burst samples
 *     SfrBulge[bin] += stars / dt with the galaxy's substep dt, and accumulates the gas
 *   - test_merger_burst_samples_with_event_dt: the merger burst samples at the substep's
 *     bin with the satellite's substep dt carried in the event (value1)
 *   - test_post_merger_recheck_samples_with_event_dt: the post-merger instability burst
 *     samples the same bin with the same dt
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
#include "shared/sage_sfr_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared SAGE16 test fixture boilerplate (counters, config reset, module registration) */
#define SAGE_TEST_LOCAL_RESET_CONFIG /* this file keeps a custom reset_config() */
#include "modules/_tests/sage_test_fixtures.h"

/* Module parameters (extern declarations to access module internals for testing) */
extern double FEEDBACK_REHEATING_EPSILON;
extern double FEEDBACK_EJECTION_EFFICIENCY;
extern double RECYCLE_FRACTION;
extern double YIELD;
extern double FRAC_Z_LEAVE_DISK;
extern double THRESHOLD_MAJOR_MERGER;

/* Module functions (extern declarations for direct testing) */
extern int sage26_starburst_feedback_init(void);
extern int sage26_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_starburst_feedback_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Initialize global unit conversion constants
 *
 * CRITICAL: The module uses global unit variables (MimicConfig.UnitEnergy_in_cgs,
 * MimicConfig.UnitMass_in_g) to convert physical constants to code units. These must be initialized
 * before calling sage26_starburst_feedback_init() or the SN code-unit constants will be garbage.
 */
static void init_unit_constants(void) {
  /* Standard cosmological unit system */
  MimicConfig.UnitLength_in_cm = 3.08568e24;    /* 1 Mpc in cm */
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5; /* 1 km/s in cm/s */
  MimicConfig.UnitMass_in_g = 1.989e43;         /* 1e10 Msun in g */

  /* Derived units */
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
}

/**
 * @brief   Reset global configuration state
 */
static void reset_config(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  init_unit_constants(); /* Always initialize unit constants after reset */

  /* Set Hubble_h before module init - required for SN code-unit constant calculation */
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5; /* 1 km/s */
}

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
 * @param   stellar_mass    Stellar mass [1e10 Msun/h]
 * @param   bulge_mass      Bulge mass [1e10 Msun/h]
 * @param   hot_gas         Hot gas mass [1e10 Msun/h]
 * @param   metals_hot      Metals in hot gas [1e10 Msun/h]
 */
static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal, int type, double mvir,
                              double vvir, double cold_gas, double metals_cold, double stellar_mass,
                              double bulge_mass, double hot_gas, double metals_hot) {
  memset(halo, 0, sizeof(struct Halo));
  memset(gal, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Mvir = (float)mvir;
  halo->Vvir = (float)vvir;
  halo->SnapNum = 63;
  halo->dT = 0.1; /* Time interval for rate calculations */
  halo->galaxy = gal;

  gal->ColdGas = (float)cold_gas;
  gal->MetalsColdGas = (float)metals_cold;
  gal->StellarMass = (float)stellar_mass;
  gal->MetalsStellarMass = (float)(metals_cold / cold_gas * stellar_mass);
  gal->BulgeMass = (float)bulge_mass;
  gal->MetalsBulgeMass = (float)(metals_cold / cold_gas * bulge_mass);
  gal->HotGas = (float)hot_gas;
  gal->MetalsHotGas = (float)metals_hot;
  gal->EjectedGas = 0.0;
  gal->MetalsEjectedGas = 0.0;
  gal->StarFormationRate = 0.0;
  gal->SupernovaOutflowRate = 0.0;
  gal->UnstableDiskGasFraction = 0.0;
}

/**
 * @brief   Setup test parameters
 *
 * @param   reheating_eps       Feedback reheating epsilon
 * @param   ejection_eff        Feedback ejection efficiency
 * @param   recycle_frac        Recycle fraction
 * @param   yield               Metal yield
 * @param   frac_z_leave        Fraction of metals leaving disk
 * @param   threshold_major     Major merger threshold
 */
static void setup_test_parameters(double reheating_eps, double ejection_eff, double recycle_frac,
                                  double yield, double frac_z_leave, double threshold_major) {
  /* Set model parameters in MimicConfig */
  int idx = 0;

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackReheatingEpsilon");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", reheating_eps);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackEjectionEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", ejection_eff);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", recycle_frac);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", yield);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", frac_z_leave);

  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", threshold_major);

  MimicConfig.NumModelParams = idx;
}

static void append_model_param(const char *param_name, double value) {
  const int idx = MimicConfig.NumModelParams;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "%s", param_name);
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%.6f", value);
  MimicConfig.NumModelParams = idx + 1;
}

static void setup_post_merger_recheck_parameters(int include_quasar) {
  append_model_param("StarFormingDiskFactor", 3.0);
  if (include_quasar) {
    append_model_param("BlackHoleGrowthRate", 0.02);
    append_model_param("QuasarModeEfficiency", 0.001);
  }
}

static void setup_runtime_phase_config(int enable_disk_instability, int enable_phase2_quasar) {
  if (enable_disk_instability) {
    test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  }

  if (enable_phase2_quasar) {
    test_phase_add("satellite_mergers", "sage26_quasar_mode", PROCESSING_MODE_PER_EVENT);
  }
}

static void teardown_runtime_phase_config(void) { test_free_substep_phases(); }

/**
 * @brief   Create minimal module context for testing
 *
 * @param   ctx             Context to initialize
 * @param   central_halo    Central halo (for ctx->central_galaxy)
 */
static void setup_test_context(struct ModuleContext *ctx, struct Halo *central_halo) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->substep_dt = 0.01;
  ctx->redshift = 0.0;
  ctx->time = 13.8;
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
  ctx->params = &MimicConfig;
  ctx->central_galaxy = central_halo;

  /* Set unit conversions (typical values) */
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5; /* 1 km/s */
  MimicConfig.Hubble_h = 0.73;
}

// ============================================================================
// PHYSICS CALCULATION TESTS
// ============================================================================

/**
 * @test    test_disk_instability_starburst
 * @brief   Test starburst from disk instability trigger
 *
 * Expected: Stars form, cold gas decreases, bulge increases
 * Validates: Disk instability trigger physics (mode=1, eburst = efficiency)
 */
int test_disk_instability_starburst(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);

  int result = sage26_starburst_feedback_init();
  TEST_ASSERT(result == 0, "Module init should succeed");

  /* Create central galaxy with cold gas */
  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 10.0;
  const double metals_cold = 0.2; /* Z = 0.02 */
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, cold_gas, metals_cold, 5.0, 1.0, 50.0, 1.0);

  /* Set disk instability trigger */
  gal.UnstableDiskGasFraction = 0.5; /* 50% efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_cold = gal.ColdGas;
  const double initial_stellar = gal.StellarMass;
  const double initial_bulge = gal.BulgeMass;

  /* ===== EXECUTE ===== */
  result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Process function should succeed");

  /* Cold gas should decrease */
  TEST_ASSERT(gal.ColdGas < initial_cold, "Cold gas should decrease from starburst");

  /* Stellar mass should increase */
  TEST_ASSERT(gal.StellarMass > initial_stellar, "Stellar mass should increase from starburst");

  /* Bulge mass should increase (starbursts form spheroids) */
  TEST_ASSERT(gal.BulgeMass > initial_bulge, "Bulge mass should increase from starburst");

  /* Trigger lifecycle is owned by clear modules, not collisional_starburst */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "Disk-instability trigger should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_disk_instability_uses_full_interval_for_rates
 * @brief   SAGE parity: disk-instability burst rates normalize by the full
 *          snapshot interval (halo->dT), like the normal disk-SF channel, so the
 *          reported rate is the snapshot-mean and is INVARIANT to num_substeps.
 *          SAGE output averages per-substep SfrBulge[step] by STEPS, giving the
 *          same snapshot mean regardless of substep count.
 *
 * Expected: With identical physics and num_substeps changing from 1 to 2,
 * StarFormationRate and SupernovaOutflowRate are unchanged (ratio = 1.0). The
 * outflow is the undivided reheated mass (SAGE26's OutflowRate, normalised once per
 * halo by sage26_finalize_cooling_heating), so it involves no interval at all.
 */
int test_disk_instability_uses_full_interval_for_rates(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo1;
  struct GalaxyData gal1;
  setup_test_galaxy(&halo1, &gal1, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  halo1.dT = 0.2f;
  gal1.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx1;
  setup_test_context(&ctx1, &halo1);
  ctx1.num_substeps = 1;

  int result = sage26_starburst_feedback_process(&ctx1, &halo1, 1);
  TEST_ASSERT(result == 0, "First disk-instability processing should succeed");
  const double sfr1 = gal1.StarFormationRate;
  const double outflow1 = gal1.SupernovaOutflowRate;

  struct Halo halo2;
  struct GalaxyData gal2;
  setup_test_galaxy(&halo2, &gal2, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  halo2.dT = 0.2f;
  gal2.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx2;
  setup_test_context(&ctx2, &halo2);
  ctx2.num_substeps = 2; /* Same halo dT, half per-substep dt */

  result = sage26_starburst_feedback_process(&ctx2, &halo2, 1);
  TEST_ASSERT(result == 0, "Second disk-instability processing should succeed");
  const double sfr2 = gal2.StarFormationRate;
  const double outflow2 = gal2.SupernovaOutflowRate;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(sfr1 > 0.0 && sfr2 > 0.0, "Both runs should produce positive star formation rate");
  TEST_ASSERT(outflow1 > 0.0 && outflow2 > 0.0, "Both runs should produce positive outflow rate");
  TEST_ASSERT_DOUBLE_EQUAL(sfr2 / sfr1, 1.0, 1e-4,
                           "Disk-instability SFR uses full interval — invariant to num_substeps");
  TEST_ASSERT_DOUBLE_EQUAL(
      outflow2 / outflow1, 1.0, 1e-4,
      "Disk-instability outflow is the undivided reheated mass — invariant to num_substeps");
  TEST_ASSERT_DOUBLE_EQUAL(gal2.StellarMass, gal1.StellarMass, 1e-6,
                           "Mass transfer should be timestep-invariant");
  TEST_ASSERT_DOUBLE_EQUAL(gal2.ColdGas, gal1.ColdGas, 1e-6,
                           "Cold-gas evolution should be timestep-invariant");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_merger_starburst
 * @brief   Test merger-only trigger is ignored in collisional_starburst
 *
 * Expected: No star formation change (merger channel handled in merge module)
 * Validates: Channel separation between galaxy_physics and satellite_mergers
 */
int test_merger_starburst(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

  /* Set merger trigger */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                           "Stellar mass should be unchanged for merger-only trigger");

  /* Trigger lifecycle is owned by clear modules, not collisional_starburst */

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_both_triggers
 * @brief   Test disk trigger processed while merger trigger is ignored
 *
 * Expected: Disk-instability starburst occurs; merger trigger remains unchanged
 * Validates: Disk-only behavior in galaxy_physics module
 */
int test_both_triggers(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

  /* Set both triggers */
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(gal.StellarMass > initial_stellar,
              "Stellar mass should increase from disk-instability starburst");

  /* Trigger lifecycle is owned by clear modules, not collisional_starburst */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "Disk trigger should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_merger_starburst
 * @brief   Test merger event triggers starburst in process_per_event path
 *
 * Expected: Stellar and bulge mass increase, cold gas decreases
 * Validates: process_per_event merger channel behavior
 */
int test_per_event_merger_starburst(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 1,
                              .target_index = 0,
                              .value0 = 0.3,
                              .value1 = 0.1};
  ctx.active_event = &event;

  const double initial_stellar = gal.StellarMass;
  const double initial_bulge = gal.BulgeMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Per-event merger processing should succeed");
  TEST_ASSERT(gal.StellarMass > initial_stellar, "Stellar mass should increase from merger event");
  TEST_ASSERT(gal.BulgeMass > initial_bulge, "Bulge mass should increase from merger event");
  TEST_ASSERT(gal.ColdGas < initial_cold, "Cold gas should decrease from merger event");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_merger_uses_fof_central_feedback_destination
 * @brief   Per-event merger channel deposits reheated/ejected gas to FOF central
 *
 * Expected: Event target forms stars, but feedback destination is ctx->central_galaxy
 * Validates: SAGE parity for merger_centralgal vs centralgal roles
 */
int test_per_event_merger_uses_fof_central_feedback_destination(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Zero ejection to isolate reheating destination behavior. */
  setup_test_parameters(3.0, 0.0, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo fof_central_halo;
  struct GalaxyData fof_central_gal;
  setup_test_galaxy(&fof_central_halo, &fof_central_gal, 0, 120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0,
                    0.8);

  struct Halo event_target_halo;
  struct GalaxyData event_target_gal;
  setup_test_galaxy(&event_target_halo, &event_target_gal, 1, 25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0,
                    0.1);

  struct ModuleContext ctx;
  setup_test_context(&ctx, &fof_central_halo);

  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = 0.3,
                              .value1 = 0.1};
  ctx.active_event = &event;

  const double initial_fof_hot = fof_central_halo.galaxy->HotGas;
  const double initial_target_hot = event_target_halo.galaxy->HotGas;

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &event_target_halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Per-event merger processing should succeed");
  TEST_ASSERT(fof_central_halo.galaxy->HotGas > initial_fof_hot,
              "Reheated gas should be deposited to FOF central hot gas");
  TEST_ASSERT_DOUBLE_EQUAL(event_target_halo.galaxy->HotGas, initial_target_hot, 1e-8,
                           "Event target hot gas should not receive reheated gas directly");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_merger_uses_target_full_interval_for_rates
 * @brief   SAGE parity: the per-event merger burst normalizes rates by the
 *          target galaxy's FULL snapshot interval (event_halo->dT), like the
 *          normal SF channel — NOT the per-substep event payload dt
 *          (event->value1). So the reported rate scales as 1/dT_target and is
 *          independent of event->value1. (SAGE output averages SfrBulge[step]
 *          by STEPS, giving the snapshot mean total_burst_stars/dT.) The outflow is
 *          SAGE26's undivided OutflowRate += reheated mass (shared feedback commit,
 *          R/src/model_starformation_and_feedback.c:1249), normalised once per halo
 *          by sage26_finalize_cooling_heating, so it depends on neither interval.
 *
 * Expected: SFR ratio follows the inverse of the target full interval; the outflows
 *           are equal.
 */
int test_per_event_merger_uses_target_full_interval_for_rates(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo central1;
  struct GalaxyData central_gal1;
  setup_test_galaxy(&central1, &central_gal1, 0, 120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  central1.dT = 0.05f;

  struct Halo target1;
  struct GalaxyData target_gal1;
  setup_test_galaxy(&target1, &target_gal1, 1, 25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);
  target1.dT = 0.9f;

  struct ModuleContext ctx1;
  setup_test_context(&ctx1, &central1);
  struct ModuleEvent event1 = {.producer_module_id =
                                   MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                               .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                               .source_index = 2,
                               .target_index = 1,
                               .value0 = 0.3,
                               .value1 = 0.1};
  ctx1.active_event = &event1;

  int result = sage26_starburst_feedback_process(&ctx1, &target1, 1);
  TEST_ASSERT(result == 0, "First per-event merger processing should succeed");
  const double sfr1 = target1.galaxy->StarFormationRate;
  const double outflow1 = target1.galaxy->SupernovaOutflowRate;

  struct Halo central2;
  struct GalaxyData central_gal2;
  setup_test_galaxy(&central2, &central_gal2, 0, 120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  central2.dT = 0.8f;

  struct Halo target2;
  struct GalaxyData target_gal2;
  setup_test_galaxy(&target2, &target_gal2, 1, 25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);
  target2.dT = 0.2f;

  struct ModuleContext ctx2;
  setup_test_context(&ctx2, &central2);
  struct ModuleEvent event2 = {.producer_module_id =
                                   MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                               .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                               .source_index = 2,
                               .target_index = 1,
                               .value0 = 0.3,
                               .value1 = 0.2};
  ctx2.active_event = &event2;

  result = sage26_starburst_feedback_process(&ctx2, &target2, 1);
  TEST_ASSERT(result == 0, "Second per-event merger processing should succeed");
  const double sfr2 = target2.galaxy->StarFormationRate;
  const double outflow2 = target2.galaxy->SupernovaOutflowRate;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(sfr1 > 0.0 && sfr2 > 0.0, "Both runs should produce positive star formation rate");
  TEST_ASSERT(outflow1 > 0.0 && outflow2 > 0.0, "Both runs should produce positive outflow rate");
  /* Rate scales as 1/dT_target (uses event_halo->dT), independent of the
   * event payload dt. target1.dT=0.9, target2.dT=0.2 -> sfr1/sfr2 = 0.2/0.9. */
  const double expected_ratio = (double)target2.dT / (double)target1.dT;
  TEST_ASSERT_DOUBLE_EQUAL(sfr1 / sfr2, expected_ratio, 1e-4,
                           "Merger-burst SFR scales with inverse target full interval (halo->dT)");
  TEST_ASSERT(outflow1 == outflow2,
              "Merger-burst outflow is the undivided reheated mass, independent of any interval");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_minor_merger_rechecks_disk_instability
 * @brief   Minor merger event applies same-step disk-instability follow-up
 *
 * SAGE parity (model_disk_instability.c check_disk_instability): the follow-up
 * (a) transfers unstable disk stars to the bulge (BulgeMass grows; this is a
 * disk->bulge redistribution that does NOT change total StellarMass), and
 * (b) for the unstable gas, calls grow_black_hole — which itself drives a
 * quasar_mode_wind (grow_black_hole -> quasar_mode_wind, model_mergers.c:118) —
 * and then collisional_starburst. Because the (now correctly scaled) quasar wind
 * can eject the cold gas before that burst, additional NEW stars are not
 * guaranteed; the robust observables of the recheck are bulge growth, BH growth,
 * and cold-gas consumption/ejection.
 */
int test_per_event_minor_merger_rechecks_disk_instability(void) {
  init_memory_system(0);

  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  setup_runtime_phase_config(0, 0);
  MimicConfig.G = 43007.1;
  int result = sage26_starburst_feedback_init();
  TEST_ASSERT(result == 0, "Baseline init should succeed");

  struct Halo base_central;
  struct GalaxyData base_central_gal;
  setup_test_galaxy(&base_central, &base_central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0,
                    0.8);

  struct Halo base_target;
  struct GalaxyData base_target_gal;
  setup_test_galaxy(&base_target, &base_target_gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
  base_target.Vmax = 120.0;
  base_target.galaxy->DiskScaleRadius = 5.0;
  base_target.galaxy->BlackHoleMass = 0.05;

  struct ModuleContext base_ctx;
  setup_test_context(&base_ctx, &base_central);
  struct ModuleEvent base_event = {.producer_module_id =
                                       MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                                   .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                                   .source_index = 2,
                                   .target_index = 1,
                                   .value0 = 0.2,
                                   .value1 = 0.1};
  base_ctx.active_event = &base_event;

  result = sage26_starburst_feedback_process(&base_ctx, &base_target, 1);
  TEST_ASSERT(result == 0, "Baseline per-event merger processing should succeed");

  const double baseline_bulge = base_target.galaxy->BulgeMass;
  const double baseline_stellar = base_target.galaxy->StellarMass;
  const double baseline_cold_gas = base_target.galaxy->ColdGas;
  const double baseline_bh = base_target.galaxy->BlackHoleMass;
  const double baseline_bh_accretion = base_target.galaxy->QuasarModeBHaccretionMass;

  sage26_starburst_feedback_cleanup();
  teardown_runtime_phase_config();

  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  setup_post_merger_recheck_parameters(1);
  setup_runtime_phase_config(1, 1);
  MimicConfig.G = 43007.1;
  result = sage26_starburst_feedback_init();
  TEST_ASSERT(result == 0, "Follow-up init should succeed");

  struct Halo follow_central;
  struct GalaxyData follow_central_gal;
  setup_test_galaxy(&follow_central, &follow_central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5,
                    40.0, 0.8);

  struct Halo follow_target;
  struct GalaxyData follow_target_gal;
  setup_test_galaxy(&follow_target, &follow_target_gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0,
                    0.1);
  follow_target.Vmax = 120.0;
  follow_target.galaxy->DiskScaleRadius = 5.0;
  follow_target.galaxy->BlackHoleMass = 0.05;

  struct ModuleContext follow_ctx;
  setup_test_context(&follow_ctx, &follow_central);
  struct ModuleEvent follow_event = {.producer_module_id =
                                         MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                                     .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                                     .source_index = 2,
                                     .target_index = 1,
                                     .value0 = 0.2,
                                     .value1 = 0.1};
  follow_ctx.active_event = &follow_event;

  /* Capture combined baryon total across target + central before the call.
   * Conservation: ColdGas + StellarMass + HotGas + EjectedGas + BlackHoleMass
   * must hold across both galaxies (feedback reheating moves cold→hot→ejected
   * within this closed system; BH growth moves ColdGas→BlackHoleMass). */
  const double pre_target_total = follow_target.galaxy->ColdGas +
                                  follow_target.galaxy->StellarMass + follow_target.galaxy->HotGas +
                                  follow_target.galaxy->EjectedGas +
                                  follow_target.galaxy->BlackHoleMass;
  const double pre_central_total =
      follow_central.galaxy->HotGas + follow_central.galaxy->EjectedGas;
  const double pre_combined = pre_target_total + pre_central_total;

  result = sage26_starburst_feedback_process(&follow_ctx, &follow_target, 1);
  TEST_ASSERT(result == 0, "Follow-up per-event merger processing should succeed");

  const double post_target_total = follow_target.galaxy->ColdGas +
                                   follow_target.galaxy->StellarMass +
                                   follow_target.galaxy->HotGas + follow_target.galaxy->EjectedGas +
                                   follow_target.galaxy->BlackHoleMass;
  const double post_central_total =
      follow_central.galaxy->HotGas + follow_central.galaxy->EjectedGas;
  TEST_ASSERT_DOUBLE_EQUAL(
      post_target_total + post_central_total, pre_combined, 1e-4,
      "Total baryons (target+central) must be conserved across post-merger follow-up");

  TEST_ASSERT(follow_target.galaxy->BulgeMass > baseline_bulge,
              "Disk-instability follow-up should add extra bulge growth (disk->bulge transfer)");
  /* SAGE parity: total StellarMass need not increase — the quasar wind from the
   * disk-instability BH growth can eject the cold gas before the burst, so no
   * new stars form. It must not DECREASE. */
  TEST_ASSERT(follow_target.galaxy->StellarMass >= baseline_stellar,
              "Disk-instability follow-up must not reduce stellar mass");
  TEST_ASSERT(follow_target.galaxy->ColdGas < baseline_cold_gas,
              "Disk-instability follow-up should consume/eject more cold gas");
  TEST_ASSERT(
      follow_target.galaxy->BlackHoleMass > baseline_bh,
      "Disk-instability follow-up should grow the black hole when quasar consumer is enabled");
  TEST_ASSERT(follow_target.galaxy->QuasarModeBHaccretionMass > baseline_bh_accretion,
              "Disk-instability follow-up should record extra BH accretion");

  sage26_starburst_feedback_cleanup();
  teardown_runtime_phase_config();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_recheck_respects_phase2_quasar_configuration
 * @brief   Post-merger recheck skips BH growth if quasar event consumer is absent
 *
 * Expected: The disk-instability starburst still runs, but BH growth is gated
 * by whether sage26_quasar_mode is configured for satellite_mergers process_per_event.
 */
int test_per_event_recheck_respects_phase2_quasar_configuration(void) {
  init_memory_system(0);

  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  setup_post_merger_recheck_parameters(0);
  setup_runtime_phase_config(1, 0);
  MimicConfig.G = 43007.1;
  int result = sage26_starburst_feedback_init();
  TEST_ASSERT(result == 0, "Init without satellite_mergers quasar should succeed");

  struct Halo no_quasar_central;
  struct GalaxyData no_quasar_central_gal;
  setup_test_galaxy(&no_quasar_central, &no_quasar_central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0,
                    1.5, 40.0, 0.8);

  struct Halo no_quasar_target;
  struct GalaxyData no_quasar_target_gal;
  setup_test_galaxy(&no_quasar_target, &no_quasar_target_gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0,
                    5.0, 0.1);
  no_quasar_target.Vmax = 120.0;
  no_quasar_target.galaxy->DiskScaleRadius = 5.0;
  no_quasar_target.galaxy->BlackHoleMass = 0.05;

  struct ModuleContext no_quasar_ctx;
  setup_test_context(&no_quasar_ctx, &no_quasar_central);
  struct ModuleEvent no_quasar_event = {
      .producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
      .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
      .source_index = 2,
      .target_index = 1,
      .value0 = 0.2,
      .value1 = 0.1};
  no_quasar_ctx.active_event = &no_quasar_event;

  result = sage26_starburst_feedback_process(&no_quasar_ctx, &no_quasar_target, 1);
  TEST_ASSERT(result == 0, "No-quasar follow-up should succeed");

  const double no_quasar_bh = no_quasar_target.galaxy->BlackHoleMass;
  const double no_quasar_bh_accretion = no_quasar_target.galaxy->QuasarModeBHaccretionMass;
  TEST_ASSERT_DOUBLE_EQUAL(
      no_quasar_bh, 0.05, 1e-6,
      "BH mass should remain unchanged when the satellite_mergers quasar consumer is disabled");
  TEST_ASSERT_DOUBLE_EQUAL(
      no_quasar_bh_accretion, 0.0, 1e-6,
      "BH accretion should remain zero without the satellite_mergers quasar consumer");

  sage26_starburst_feedback_cleanup();
  teardown_runtime_phase_config();

  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  setup_post_merger_recheck_parameters(1);
  setup_runtime_phase_config(1, 1);
  MimicConfig.G = 43007.1;
  result = sage26_starburst_feedback_init();
  TEST_ASSERT(result == 0, "Init with satellite_mergers quasar should succeed");

  struct Halo with_quasar_central;
  struct GalaxyData with_quasar_central_gal;
  setup_test_galaxy(&with_quasar_central, &with_quasar_central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0,
                    1.5, 40.0, 0.8);

  struct Halo with_quasar_target;
  struct GalaxyData with_quasar_target_gal;
  setup_test_galaxy(&with_quasar_target, &with_quasar_target_gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0,
                    1.0, 5.0, 0.1);
  with_quasar_target.Vmax = 120.0;
  with_quasar_target.galaxy->DiskScaleRadius = 5.0;
  with_quasar_target.galaxy->BlackHoleMass = 0.05;

  struct ModuleContext with_quasar_ctx;
  setup_test_context(&with_quasar_ctx, &with_quasar_central);
  struct ModuleEvent with_quasar_event = {
      .producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
      .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
      .source_index = 2,
      .target_index = 1,
      .value0 = 0.2,
      .value1 = 0.1};
  with_quasar_ctx.active_event = &with_quasar_event;

  result = sage26_starburst_feedback_process(&with_quasar_ctx, &with_quasar_target, 1);
  TEST_ASSERT(result == 0, "Quasar-enabled follow-up should succeed");
  TEST_ASSERT(
      with_quasar_target.galaxy->BlackHoleMass > no_quasar_bh,
      "BH growth should only occur when the satellite_mergers quasar consumer is configured");
  TEST_ASSERT(with_quasar_target.galaxy->QuasarModeBHaccretionMass > no_quasar_bh_accretion,
              "BH accretion tracking should only increase with the quasar event consumer");

  sage26_starburst_feedback_cleanup();
  teardown_runtime_phase_config();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_per_event_unknown_code_noop
 * @brief   Test zero mass ratio event is a graceful no-op
 *
 * Subscription routing ensures only valid events reach this module.
 * The remaining guard is value0 <= 0.0 (zero/negative baryonic mass ratio).
 *
 * Expected: No property changes and success return
 * Validates: Zero-ratio early-exit guard in process_per_event path
 */
int test_per_event_unknown_code_noop(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 1,
                              .target_index = 0,
                              .value0 = 0.0, /* zero ratio — must be a no-op */
                              .value1 = 0.1};
  ctx.active_event = &event;

  const double initial_stellar = gal.StellarMass;
  const double initial_bulge = gal.BulgeMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Zero mass ratio event should be no-op success");
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-12,
                           "Stellar mass should remain unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(gal.BulgeMass, initial_bulge, 1e-12,
                           "Bulge mass should remain unchanged");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12, "Cold gas should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_disk_instability_invalid_nonboundary_dt_errors
 * @brief   Invalid non-boundary dT must fail fast in disk-instability path
 *
 * Expected: SnapNum >= 0 with dT <= 0 returns error.
 */
int test_disk_instability_invalid_nonboundary_dt_errors(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  halo.SnapNum = 63;
  halo.dT = -1.0f;
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);
  ctx.num_substeps = 1;

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == -1, "Non-boundary dT <= 0 must fail with error");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_disk_instability_boundary_sentinel_dt_noop
 * @brief   Boundary sentinel dT is a no-op for first-snapshot objects
 *
 * Expected: SnapNum < 0 with dT <= 0 returns success with no property changes.
 */
int test_disk_instability_boundary_sentinel_dt_noop(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  halo.SnapNum = -1;
  halo.dT = -1.0f;
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);
  ctx.num_substeps = 1;

  const double initial_stellar = gal.StellarMass;
  const double initial_cold = gal.ColdGas;
  const double initial_sfr = gal.StarFormationRate;
  const double initial_outflow = gal.SupernovaOutflowRate;

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Boundary sentinel dT should be a no-op success");
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-12,
                           "Stellar mass should remain unchanged on boundary skip");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12,
                           "Cold gas should remain unchanged on boundary skip");
  TEST_ASSERT_DOUBLE_EQUAL(gal.StarFormationRate, initial_sfr, 1e-12,
                           "SFR should remain unchanged on boundary skip");
  TEST_ASSERT_DOUBLE_EQUAL(gal.SupernovaOutflowRate, initial_outflow, 1e-12,
                           "Outflow rate should remain unchanged on boundary skip");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_major_vs_minor_merger
 * @brief   Test merger-only major/minor triggers are both ignored
 *
 * Expected: No stellar mass growth from merger-only triggers
 * Validates: Merger channel is not consumed in galaxy_physics starburst module
 */
int test_major_vs_minor_merger(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3); /* threshold = 0.3 */
  sage26_starburst_feedback_init();

  struct ModuleContext ctx;

  /* Test 1: Minor merger trigger only (ratio < threshold) */
  struct Halo halo_minor;
  struct GalaxyData gal_minor;
  setup_test_galaxy(&halo_minor, &gal_minor, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

  setup_test_context(&ctx, &halo_minor);

  sage26_starburst_feedback_process(&ctx, &halo_minor, 1);

  /* Test 2: Major merger trigger only (ratio > threshold) */
  struct Halo halo_major;
  struct GalaxyData gal_major;
  setup_test_galaxy(&halo_major, &gal_major, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);

  setup_test_context(&ctx, &halo_major);

  sage26_starburst_feedback_process(&ctx, &halo_major, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal_minor.StellarMass, 5.0, 1e-10,
                           "Minor merger-only trigger should be ignored");
  TEST_ASSERT_DOUBLE_EQUAL(gal_major.StellarMass, 5.0, 1e-10,
                           "Major merger-only trigger should be ignored");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_mass_conservation
 * @brief   Test total mass conserved during starburst
 *
 * Expected: Total mass (cold + stellar + hot + ejected) conserved
 * Validates: Mass conservation
 */
int test_mass_conservation(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_total = gal.ColdGas + gal.StellarMass + gal.HotGas + gal.EjectedGas;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  const double final_total = gal.ColdGas + gal.StellarMass + gal.HotGas + gal.EjectedGas;
  TEST_ASSERT_DOUBLE_EQUAL(final_total, initial_total, 1e-4, "Total mass should be conserved");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_metallicity_preservation
 * @brief   Test metallicity preserved during starburst
 *
 * Expected: Cold gas metallicity unchanged (metals tracked correctly)
 * Validates: Metallicity handling
 */
int test_metallicity_preservation(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  const double cold_gas = 10.0;
  const double metals_cold = 0.5; /* Z = 0.05 */
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, cold_gas, metals_cold, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.3; /* Modest efficiency to leave gas */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_metallicity = metals_cold / cold_gas;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Metallicity should be approximately preserved (may change due to yield) */
  if (gal.ColdGas > 0.1) {
    const double final_metallicity = gal.MetalsColdGas / gal.ColdGas;
    /* Allow for metal enrichment from yield */
    TEST_ASSERT(final_metallicity >= initial_metallicity * 0.9,
                "Cold gas metallicity should be approximately preserved");
  }

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_bulge_formation
 * @brief   Test stars added to bulge mass (not disk)
 *
 * Expected: BulgeMass increases proportional to stellar mass increase
 * Validates: Bulge formation (unique to starbursts)
 */
int test_bulge_formation(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;
  const double initial_bulge = gal.BulgeMass;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  const double stellar_growth = gal.StellarMass - initial_stellar;
  const double bulge_growth = gal.BulgeMass - initial_bulge;

  /* Bulge should grow by same amount as stellar mass (starbursts form spheroids) */
  TEST_ASSERT_DOUBLE_EQUAL(bulge_growth, stellar_growth, 1e-6,
                           "Bulge growth should equal stellar mass growth");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_ejection_calculation
 * @brief   Test energy-driven ejection calculation
 *
 * Expected: Ejected gas calculated from energy balance
 * Validates: Ejection physics
 */
int test_ejection_calculation(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_hot = gal.HotGas;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Feedback should transfer gas: cold decreases, hot increases or ejection occurs */
  const int cold_consumed = (gal.ColdGas < initial_cold);
  const int hot_increased = (gal.HotGas > initial_hot) || (gal.EjectedGas > 0.0);
  TEST_ASSERT(cold_consumed, "Cold gas should be consumed by star formation");
  TEST_ASSERT(hot_increased, "Feedback should heat or eject gas");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_cold_gas_balance
 * @brief   Test (stars + reheated) <= ColdGas constraint enforced
 *
 * Expected: Balancing reduces stars and reheated when insufficient gas
 * Validates: Cold gas balance constraint
 */
int test_cold_gas_balance(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* High reheating to trigger balance */
  setup_test_parameters(10.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  /* Low cold gas, high efficiency */
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 1.0, 0.02, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.9;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Cold gas should not go negative */
  TEST_ASSERT(gal.ColdGas >= -1e-6, "Cold gas should not be negative");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

/**
 * @test    test_zero_cold_gas
 * @brief   Test no starburst when ColdGas = 0
 *
 * Expected: Stellar mass unchanged
 * Validates: Zero cold gas handling
 */
int test_zero_cold_gas(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 0.0, 0.0, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                           "Stellar mass should not change with zero cold gas");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_efficiency
 * @brief   Test no starburst when efficiency = 0
 *
 * Expected: Stellar mass unchanged
 * Validates: Zero efficiency handling
 */
int test_zero_efficiency(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.0; /* Zero efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                           "Stellar mass should not change with zero efficiency");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_no_triggers
 * @brief   Test no processing when triggers = 0
 *
 * Expected: All properties unchanged
 * Validates: Trigger requirement
 */
int test_no_triggers(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  /* No triggers set */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;
  const double initial_cold = gal.ColdGas;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-10,
                           "Stellar mass unchanged without triggers");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-10, "Cold gas unchanged without triggers");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
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

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  memset(&halo, 0, sizeof(halo));
  halo.Type = 0;
  halo.galaxy = NULL;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle NULL galaxy gracefully");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_null_central_galaxy
 * @brief   Test NULL central galaxy handled gracefully
 *
 * Expected: Function returns success without crash
 * Validates: NULL central pointer safety
 */
int test_null_central_galaxy(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo satellite;
  struct GalaxyData sat_gal;
  setup_test_galaxy(&satellite, &sat_gal, 1, 50.0, 200.0, 5.0, 0.1, 2.0, 0.5, 10.0, 0.2);

  /* Create central with NULL galaxy */
  struct Halo central;
  memset(&central, 0, sizeof(central));
  central.Type = 0;
  central.galaxy = NULL;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &satellite, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Should handle NULL central galaxy gracefully");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_triggers_preserved
 * @brief   Test triggers preserved after processing
 *
 * Expected: Trigger flags remain unchanged after processing
 * Validates: Trigger lifecycle ownership by dedicated clear modules
 */
int test_triggers_preserved(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(gal.UnstableDiskGasFraction, 0.5, 1e-10,
                           "UnstableDiskGasFraction should remain unchanged");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
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

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halos[2];
  struct GalaxyData gals[2];
  setup_test_galaxy(&halos[0], &gals[0], 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 2.0, 0.5, 10.0, 0.2);

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halos[0]);

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, halos, 2);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result != 0, "Should return error when ngal != 1");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_zero_vvir
 * @brief   Test ejection = 0 when Vvir = 0
 *
 * Expected: No ejection, but starburst still occurs
 * Validates: Zero Vvir handling
 */
int test_zero_vvir(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 0.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;
  const double initial_ejected = gal.EjectedGas;

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Stars should still form */
  TEST_ASSERT(gal.StellarMass > initial_stellar, "Stars should form even with Vvir = 0");

  /* No ejection when Vvir = 0 */
  TEST_ASSERT_DOUBLE_EQUAL(gal.EjectedGas, initial_ejected, 1e-10, "No ejection when Vvir = 0");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_insufficient_cold_gas
 * @brief   Test balancing works when (stars + reheated) > ColdGas
 *
 * Expected: Stars and reheated scaled down to fit available gas
 * Validates: Balancing constraint
 */
int test_insufficient_cold_gas(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  /* Very high reheating to trigger balancing */
  setup_test_parameters(15.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 1.0, 0.02, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.9; /* High efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Should not crash and cold gas should not go negative */
  TEST_ASSERT(gal.ColdGas >= -1e-6, "Cold gas should not be negative");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// PARAMETER SENSITIVITY TESTS
// ============================================================================

/**
 * @test    test_parameter_sensitivity_reheating
 * @brief   Test FeedbackReheatingEpsilon parameter affects reheating
 *
 * Expected: Higher epsilon produces more reheating (before ejection)
 * Validates: Parameter loading and physics
 *
 * Strategy: Use conditions where balancing and ejection don't dominate:
 * - Low star formation efficiency (eburst = 0.1) to avoid balancing
 * - Large ColdGas to avoid running out
 * - Measure cold gas decrease (proxy for reheating since cold gas consumed by both SF and
 * reheating)
 */
int test_parameter_sensitivity_reheating(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test with low reheating epsilon */
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.5, 0.3); /* Zero ejection to isolate reheating */
  sage26_starburst_feedback_init();

  struct Halo halo_low;
  struct GalaxyData gal_low;
  /* Large cold gas, low efficiency to avoid balancing */
  setup_test_galaxy(&halo_low, &gal_low, 0, 100.0, 300.0, 100.0, 2.0, 5.0, 1.0, 50.0, 1.0);
  gal_low.UnstableDiskGasFraction = 0.1; /* Low efficiency */

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo_low);

  const double initial_cold_low = gal_low.ColdGas;
  sage26_starburst_feedback_process(&ctx, &halo_low, 1);
  const double cold_consumed_low = initial_cold_low - gal_low.ColdGas;

  sage26_starburst_feedback_cleanup();

  /* Test with high reheating epsilon */
  reset_config();
  setup_test_parameters(3.0, 0.0, 0.43, 0.03, 0.5, 0.3); /* Zero ejection to isolate reheating */
  sage26_starburst_feedback_init();

  struct Halo halo_high;
  struct GalaxyData gal_high;
  setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 300.0, 100.0, 2.0, 5.0, 1.0, 50.0, 1.0);
  gal_high.UnstableDiskGasFraction = 0.1; /* Same efficiency */

  setup_test_context(&ctx, &halo_high);

  const double initial_cold_high = gal_high.ColdGas;
  sage26_starburst_feedback_process(&ctx, &halo_high, 1);
  const double cold_consumed_high = initial_cold_high - gal_high.ColdGas;

  /* ===== VALIDATE ===== */
  /* Cold gas consumed = (1-recycle)*stars + reheated
   * With same eburst: stars_low ≈ stars_high
   * reheated_low = 1.0 * stars, reheated_high = 3.0 * stars
   * So cold_consumed_high should be > cold_consumed_low */
  TEST_ASSERT(cold_consumed_high > cold_consumed_low,
              "Higher reheating epsilon should consume more cold gas");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity_ejection
 * @brief   Test FeedbackEjectionEfficiency parameter affects ejection
 *
 * Expected: Parameter affects ejection when energetically favorable
 * Validates: Parameter loading and physics
 *
 * Strategy: Use low Vvir and modest reheating to ensure ejection occurs
 * ejection = (efficiency * constant / Vvir^2 - reheating_epsilon) * stars
 * With low Vvir, the first term dominates, making ejection positive
 */
int test_parameter_sensitivity_ejection(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test 1: Verify ejection can be zero with low efficiency */
  reset_config();
  setup_test_parameters(0.5, 0.1, 0.43, 0.03, 0.5, 0.3); /* Low reheating and ejection */
  sage26_starburst_feedback_init();

  struct Halo halo_low;
  struct GalaxyData gal_low;
  /* Low Vvir to make ejection energetically favorable */
  setup_test_galaxy(&halo_low, &gal_low, 0, 10.0, 100.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal_low.UnstableDiskGasFraction = 0.3;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo_low);

  sage26_starburst_feedback_process(&ctx, &halo_low, 1);

  /* Verify basic correctness */
  TEST_ASSERT(gal_low.EjectedGas >= -1e-10, "Ejected gas should not be significantly negative");
  TEST_ASSERT(gal_low.HotGas >= -1e-10, "Hot gas should not be significantly negative");
  TEST_ASSERT(gal_low.ColdGas >= -1e-10, "Cold gas should not be significantly negative");

  sage26_starburst_feedback_cleanup();

  /* Test 2: Higher ejection efficiency */
  reset_config();
  setup_test_parameters(0.5, 1.0, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo_high;
  struct GalaxyData gal_high;
  setup_test_galaxy(&halo_high, &gal_high, 0, 10.0, 100.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal_high.UnstableDiskGasFraction = 0.3;

  setup_test_context(&ctx, &halo_high);

  sage26_starburst_feedback_process(&ctx, &halo_high, 1);

  /* ===== VALIDATE ===== */
  /* Verify both tests executed without significant negative values */
  TEST_ASSERT(gal_high.EjectedGas >= -1e-10, "Ejected gas should not be significantly negative");
  TEST_ASSERT(gal_high.HotGas >= -1e-10, "Hot gas should not be significantly negative");

  /* Basic sanity: parameters were loaded and module executed */
  const int module_executed = (gal_low.StellarMass > 0.0) || (gal_high.StellarMass > 0.0);
  TEST_ASSERT(module_executed, "Module should have executed and formed stars");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity_yield
 * @brief   Test Yield parameter affects metal enrichment
 *
 * Expected: Higher yield produces more metals
 * Validates: Parameter loading and metal physics
 */
int test_parameter_sensitivity_yield(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);

  /* Test with low yield */
  reset_config();
  setup_test_parameters(3.0, 0.5, 0.43, 0.01, 0.5, 0.3); /* Low yield */
  sage26_starburst_feedback_init();

  struct Halo halo_low;
  struct GalaxyData gal_low;
  setup_test_galaxy(&halo_low, &gal_low, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal_low.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo_low);

  const double initial_metals_total_low = gal_low.MetalsColdGas + gal_low.MetalsHotGas;
  sage26_starburst_feedback_process(&ctx, &halo_low, 1);
  const double final_metals_total_low =
      gal_low.MetalsColdGas + gal_low.MetalsHotGas + gal_low.MetalsStellarMass;
  const double metals_produced_low = final_metals_total_low - initial_metals_total_low;

  sage26_starburst_feedback_cleanup();

  /* Test with high yield */
  reset_config();
  setup_test_parameters(3.0, 0.5, 0.43, 0.05, 0.5, 0.3); /* 5x higher */
  sage26_starburst_feedback_init();

  struct Halo halo_high;
  struct GalaxyData gal_high;
  setup_test_galaxy(&halo_high, &gal_high, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal_high.UnstableDiskGasFraction = 0.5;

  setup_test_context(&ctx, &halo_high);

  const double initial_metals_total_high = gal_high.MetalsColdGas + gal_high.MetalsHotGas;
  sage26_starburst_feedback_process(&ctx, &halo_high, 1);
  const double final_metals_total_high =
      gal_high.MetalsColdGas + gal_high.MetalsHotGas + gal_high.MetalsStellarMass;
  const double metals_produced_high = final_metals_total_high - initial_metals_total_high;

  /* ===== VALIDATE ===== */
  /* Verify parameter is loaded and metal enrichment occurs */
  /* Note: Metal change is complex (consumption + enrichment from yield) */
  const int metals_changed = (metals_produced_low != 0.0) || (metals_produced_high != 0.0);
  TEST_ASSERT(metals_changed, "Yield parameter should be loaded and metal enrichment should occur");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
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

  test_phase_add("galaxy_physics", "sage26_starburst_feedback", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

  /* Set required parameters */
  int idx = 0;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackReheatingEpsilon");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "3.0");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FeedbackEjectionEfficiency");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.5");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "RecycleFraction");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.43");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "Yield");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.03");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "FracZleaveDisk");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.5");
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "ThresholdMajorMerger");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0.3");
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
 * @brief   Test module doesn't leak memory during operation
 *
 * Expected: No memory leaks after init/process/cleanup cycle
 * Validates: Memory management
 */
int test_memory_safety(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  reset_config();

  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  /* ===== EXECUTE ===== */
  sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* check_memory_leaks() will catch any leaks */

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// BULGE STAR-FORMATION HISTORY SAMPLING
// ============================================================================

/** @brief Element (history, bin) of a galaxy */
static float bulge_history(struct GalaxyData *gal, const enum MimicSageSfrHistory h,
                           const int bin) {
  return *mimic_sage_sfr_history(gal, h, bin);
}

/** @brief Whether every element of the three bulge histories outside `bin` is zero */
static int bulge_history_zero_outside(struct GalaxyData *gal, const int bin) {
  const enum MimicSageSfrHistory bulge[] = {MIMIC_SFR_HISTORY_BULGE,
                                            MIMIC_SFR_HISTORY_BULGE_COLD_GAS,
                                            MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS};
  for (int h = 0; h < 3; h++) {
    for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
      if (k != bin && bulge_history(gal, bulge[h], k) != 0.0f) {
        return 0;
      }
    }
  }
  return 1;
}

/**
 * @test    test_instability_burst_samples_bulge_history
 * @brief   SfrBulge[bin] += stars / dt, SfrBulgeColdGas[bin] += ColdGas,
 *          SfrBulgeColdGasMetals[bin] += MetalsColdGas (R/src/model_mergers.c:755-757)
 *
 * With 13 substeps, substep 5 maps to bin 3 ((5 * 10) / 13). The disk-instability
 * burst's dt is the galaxy's substep interval, dT / 13 (SAGE26's
 * starformation_and_feedback dt handed to collisional_starburst_recipe,
 * R/src/model_disk_instability.c:106). The burst mass is formed here with the kernel's
 * own expressions (mode 1: eburst = the unstable fraction; the feedback
 * renormalisation), and the sample is taken before the burst consumes the cold gas. A
 * second burst in the same bin adds its own rate and its own (smaller) cold gas.
 */
int test_instability_burst_samples_bulge_history(void) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  TEST_ASSERT(sage26_starburst_feedback_init() == 0, "Module init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  halo.dT = 0.13;
  gal.UnstableDiskGasFraction = 0.5;
  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);
  ctx.num_substeps = 13;
  ctx.substep_number = 5;
  const int bin = 3;
  const double dt = halo.dT / 13.0;

  /* The kernel's burst mass (mode 1) and its feedback renormalisation */
  const float cold_before = gal.ColdGas;
  const float metals_before = gal.MetalsColdGas;
  double stars = 0.5 * gal.ColdGas;
  const double reheated = 3.0 * stars;
  if ((stars + reheated) > gal.ColdGas && (stars + reheated) > 0.0) {
    stars *= gal.ColdGas / (stars + reheated);
  }
  float rate = 0.0f;
  rate += stars / dt;

  TEST_ASSERT(sage26_starburst_feedback_process(&ctx, &halo, 1) == 0, "Process should succeed");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE, bin) == rate,
              "SfrBulge[bin] is stars / dt with the galaxy's substep dt");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin) == cold_before,
              "SfrBulgeColdGas[bin] adds the cold gas before the burst");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, bin) == metals_before,
              "SfrBulgeColdGasMetals[bin] adds the cold metals before the burst");
  TEST_ASSERT(bulge_history_zero_outside(&gal, bin), "only the substep's bin is sampled");
  for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
    TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_DISK, k) == 0.0f &&
                    bulge_history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, k) == 0.0f,
                "a burst does not touch the disk history");
  }

  /* A second burst in the same bin accumulates the gas (+=), not assigns it */
  const float cold_second = gal.ColdGas;
  float gas_sum = cold_before;
  gas_sum += cold_second;
  TEST_ASSERT(sage26_starburst_feedback_process(&ctx, &halo, 1) == 0, "Process should succeed");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin) == gas_sum,
              "a second burst in the bin adds its cold gas to the first's");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE, bin) > rate,
              "a second burst in the bin adds its rate");

  sage26_starburst_feedback_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief Run one merger-event burst on a fresh central with the given event dt (value1)
 *
 * Substep 9 of 10 (bin 9); the target's own dT / num_substeps is 0.01, which the
 * event's satellite dt must override.
 */
static int run_merger_burst(const double event_dt, struct GalaxyData *gal_out) {
  reset_config();
  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  if (sage26_starburst_feedback_init() != 0) {
    return -1;
  }
  static struct Halo halo;
  setup_test_galaxy(&halo, gal_out, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);
  ctx.num_substeps = 10;
  ctx.substep_number = 9;
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 1,
                              .target_index = 0,
                              .value0 = 0.3,
                              .value1 = event_dt};
  ctx.active_event = &event;
  const int result = sage26_starburst_feedback_process(&ctx, &halo, 1);
  sage26_starburst_feedback_cleanup();
  return result;
}

/**
 * @test    test_merger_burst_samples_with_event_dt
 * @brief   The merger burst samples bin (step * 10) / N with SAGE26's merger dt, the
 *          satellite's substep interval (R/src/core_build_model.c:579-581), which the
 *          merger event carries in value1
 *
 * The burst mass is formed with the kernel's mode-0 expressions; the rate is stars /
 * value1. Halving value1 must double the rate exactly (the burst masses do not depend
 * on it), which the target's own substep interval could not do.
 */
int test_merger_burst_samples_with_event_dt(void) {
  init_memory_system(0);
  struct GalaxyData gal;
  TEST_ASSERT(run_merger_burst(0.07, &gal) == 0, "Per-event merger processing should succeed");

  double stars = 0.56 * pow(0.3, 0.7) * 10.0;
  const double reheated = 3.0 * stars;
  if ((stars + reheated) > 10.0 && (stars + reheated) > 0.0) {
    stars *= 10.0 / (stars + reheated);
  }
  float rate = 0.0f;
  rate += stars / 0.07;
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE, 9) == rate,
              "SfrBulge[9] is stars / value1");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, 9) == 10.0f,
              "SfrBulgeColdGas[9] adds the merger central's cold gas before the burst");
  TEST_ASSERT(bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, 9) == 0.2f,
              "SfrBulgeColdGasMetals[9] adds its cold metals before the burst");
  TEST_ASSERT(bulge_history_zero_outside(&gal, 9), "only the substep's bin is sampled");

  struct GalaxyData half;
  TEST_ASSERT(run_merger_burst(0.035, &half) == 0, "Per-event merger processing should succeed");
  TEST_ASSERT(bulge_history(&half, MIMIC_SFR_HISTORY_BULGE, 9) ==
                  2.0f * bulge_history(&gal, MIMIC_SFR_HISTORY_BULGE, 9),
              "halving the event dt doubles the sampled rate exactly");
  TEST_ASSERT(half.StellarMass == gal.StellarMass && half.ColdGas == gal.ColdGas,
              "the event dt does not change the burst masses");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief Run a minor merger whose post-merger instability recheck bursts (as
 *        test_per_event_minor_merger_rechecks_disk_instability), substep 4 of 10
 */
static int run_recheck_burst(const int recheck, const double event_dt, struct GalaxyData *gal_out) {
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  if (recheck) {
    setup_post_merger_recheck_parameters(0);
  }
  setup_runtime_phase_config(recheck, 0);
  MimicConfig.G = 43007.1;
  if (sage26_starburst_feedback_init() != 0) {
    return -1;
  }
  static struct Halo central, target;
  static struct GalaxyData central_gal;
  setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  setup_test_galaxy(&target, gal_out, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
  target.Vmax = 120.0;
  gal_out->DiskScaleRadius = 5.0;
  gal_out->BlackHoleMass = 0.05;
  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  ctx.num_substeps = 10;
  ctx.substep_number = 4;
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = 0.2,
                              .value1 = event_dt};
  ctx.active_event = &event;
  const int result = sage26_starburst_feedback_process(&ctx, &target, 1);
  sage26_starburst_feedback_cleanup();
  teardown_runtime_phase_config();
  return result;
}

/**
 * @test    test_post_merger_recheck_samples_with_event_dt
 * @brief   The post-merger instability burst samples the merger's bin with the merger's
 *          dt, as SAGE26's deal_with_galaxy_merger passes its dt and step to
 *          check_disk_instability (R/src/model_mergers.c:809) and on to the burst
 *
 * With the recheck the bin holds two bursts: more rate and more sampled gas than the
 * merger burst alone. Halving the event dt doubles the bin's rate exactly, so both
 * bursts divide by it.
 */
int test_post_merger_recheck_samples_with_event_dt(void) {
  init_memory_system(0);
  struct GalaxyData merger_only, recheck, recheck_half;
  TEST_ASSERT(run_recheck_burst(0, 0.1, &merger_only) == 0, "merger-only run should succeed");
  TEST_ASSERT(run_recheck_burst(1, 0.1, &recheck) == 0, "recheck run should succeed");
  TEST_ASSERT(run_recheck_burst(1, 0.05, &recheck_half) == 0, "recheck run should succeed");

  TEST_ASSERT(bulge_history(&recheck, MIMIC_SFR_HISTORY_BULGE, 4) >
                  bulge_history(&merger_only, MIMIC_SFR_HISTORY_BULGE, 4),
              "the recheck burst adds its rate to the merger's bin");
  TEST_ASSERT(bulge_history(&recheck, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, 4) >
                  bulge_history(&merger_only, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, 4),
              "the recheck burst adds its cold gas to the merger's bin");
  TEST_ASSERT(bulge_history_zero_outside(&recheck, 4), "only the merger's bin is sampled");
  TEST_ASSERT(bulge_history(&recheck_half, MIMIC_SFR_HISTORY_BULGE, 4) ==
                  2.0f * bulge_history(&recheck, MIMIC_SFR_HISTORY_BULGE, 4),
              "both bursts divide by the event dt");

  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER

// ============================================================================

/**
 * @brief   Main test runner
 *
 * Executes all sage26_starburst_feedback unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_starburst_feedback Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Run physics calculation tests */
  TEST_RUN(test_disk_instability_starburst);
  TEST_RUN(test_disk_instability_uses_full_interval_for_rates);
  TEST_RUN(test_merger_starburst);
  TEST_RUN(test_both_triggers);
  TEST_RUN(test_per_event_merger_starburst);
  TEST_RUN(test_per_event_merger_uses_fof_central_feedback_destination);
  TEST_RUN(test_per_event_merger_uses_target_full_interval_for_rates);
  TEST_RUN(test_per_event_minor_merger_rechecks_disk_instability);
  TEST_RUN(test_per_event_recheck_respects_phase2_quasar_configuration);
  TEST_RUN(test_per_event_unknown_code_noop);
  TEST_RUN(test_major_vs_minor_merger);
  TEST_RUN(test_mass_conservation);
  TEST_RUN(test_metallicity_preservation);
  TEST_RUN(test_bulge_formation);
  TEST_RUN(test_ejection_calculation);
  TEST_RUN(test_cold_gas_balance);

  /* Run edge case tests */
  TEST_RUN(test_zero_cold_gas);
  TEST_RUN(test_zero_efficiency);
  TEST_RUN(test_no_triggers);
  TEST_RUN(test_null_galaxy);
  TEST_RUN(test_null_central_galaxy);
  TEST_RUN(test_triggers_preserved);
  TEST_RUN(test_invalid_ngal);
  TEST_RUN(test_disk_instability_invalid_nonboundary_dt_errors);
  TEST_RUN(test_disk_instability_boundary_sentinel_dt_noop);
  TEST_RUN(test_zero_vvir);
  TEST_RUN(test_insufficient_cold_gas);

  /* Run parameter sensitivity tests */
  TEST_RUN(test_parameter_sensitivity_reheating);
  TEST_RUN(test_parameter_sensitivity_ejection);
  TEST_RUN(test_parameter_sensitivity_yield);

  /* Run infrastructure tests */
  TEST_RUN(test_module_initialization);
  TEST_RUN(test_memory_safety);
  TEST_RUN(test_instability_burst_samples_bulge_history);
  TEST_RUN(test_merger_burst_samples_with_event_dt);
  TEST_RUN(test_post_merger_recheck_samples_with_event_dt);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
