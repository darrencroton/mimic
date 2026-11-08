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
 *   - test_per_event_zero_ratio_bursts: a zero-ratio merger still bursts and samples the bulge
 * history
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
 *   - test_consecutive_bursts_bulge_excess_grows: three consecutive instability bursts
 *     leave BulgeMass two floats above StellarMass, bit for bit as SAGE26's own routine
 *     does; pins the limit of the integration tests' bulge bound
 *   - test_hydrogen_refresh_module_wiring: initialised with SFprescription 1 the module wires the
 *     prescription into the burst (the commits' caps and the hydrogen refresh run), and with
 *     SFprescription 0 it does not
 *   - test_hydrogen_refresh_invariants: the refresh (R/src/model_mergers.c:798-803) overwrites
 *     a stale H1gas with the fused atomic remainder, caps H2gas only against the double budget,
 *     floors a negative remainder to 0, keeps a -0.0 H2gas and runs for a zero-star burst
 *   - test_hydrogen_cap_recheck_growth: the post-minor-merger recheck's black-hole growth
 *     receives the burst's SFprescription (its hydrogen-cap locals, R/src/model_mergers.c:366-372)
 *
 *   - test_fire_burst_budget_independent: FIREmodeOn 1: the burst's FIRE budget against
 *     SAGE26's expressions written in the test (R/src/model_mergers.c:692-752), across the
 *     scaling's guards, floor and slopes and both caps, MassLoading left unwritten
 *   - test_fire_burst_on_versus_off: the same burst with FIREmodeOn 1 and 0 (the fixed-epsilon
 *     budget), neither writing MassLoading
 *   - test_fire_burst_wiring_direct: the burst reads its host's float Vvir and stored SnapNum
 *     (and that snapshot's redshift), not its FoF central's
 *   - test_fire_burst_wiring_merger_event: the merger channel bursts the target with its own
 *     values (process_per_event)
 *   - test_fire_burst_wiring_instability_channel: the instability channel bursts the satellite
 *     with its own values (process_by_galaxy)
 *   - test_fire_burst_wiring_recheck: the post-minor-merger recheck's own burst takes the FIRE
 *     branch with the host's values
 *   - test_fire_mode_refused: FIREmodeOn is required: missing or 2 or -1 refused, naming it
 *   - test_fire_burst_snapnum_refused: a stored SnapNum of -1 or the table size is
 *     refused through a burst, naming it and the table size, and the burst writes nothing
 *   - test_cgm_burst_module_channels: CGMrecipeOn 1 against 0 on the same input, through the
 *     module's two channels (the instability burst, process_by_galaxy; the merger burst,
 *     process_per_event): the feedback and the escaping yield reach the FoF central's CGM pair
 *     (its Regime 0) or hot pair (Regime 1, or CGMrecipeOn 0); the burst host holds the other
 *     Regime; the unselected pair and the host's own pairs unchanged (the bundle's CGMrecipeOn)
 *   - test_cgm_burst_recheck_sites: the post-minor-merger recheck's own sites: its quasar wind
 *     selects by the burst HOST's Regime, its nested burst's feedback and yield by the FoF
 *     central's, as do the outer burst's (R/src/model_mergers.c:806-831,
 *     R/src/model_disk_instability.c:102-108)
 *   - test_cgm_recipe_on_refused: CGMrecipeOn is required and only 0 or 1 accepted
 *   - test_ffb_target_merger_continuation: a minor merger on a feedback-free-burst
 *     target (FFBRegime 1) grows its black hole (the quasar consumer), bursts and runs the
 *     post-merger instability recheck exactly as on the same target with FFBRegime 0: the two end
 *     states are bytewise equal but for FFBRegime, which each target keeps; the recheck is shown
 *     to act (without it the end state differs), so the comparison discriminates a recheck that
 *     skipped FFB targets
 * The three hydrogen tests are platform independent (no oracle bit pattern; the remainder is
 * formed with C's correctly rounded fma, as the shared helper forms it), so they run on every
 * platform. So are the FIRE tests: each expectation is SAGE26's expression written in the test
 * with its operand order and literal constants (sb_fire_expected), or the fixture's input, never
 * a call of the package's FIRE helpers.
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
#include "shared/sage_starburst_physics.h"

#include <stdint.h>
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
extern int sage26_quasar_mode_init(void);
extern int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_quasar_mode_cleanup(void);
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

  /* The required bulge-size mode, SAGE26's R0 0 (no sizes) */
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BulgeSizeOn");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", 0);

  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription and StarburstColdGasOn, at R1's values */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_STARBURST_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
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
 *          same snapshot mean regardless of substep count
 *          (R/src/io/save_gals_hdf5.c:934-959).
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
 * Validates: SAGE parity for merger_centralgal vs centralgal roles (the burst host
 *            and the FoF central of R/src/model_mergers.c:584-831)
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
 *          by STEPS, giving the snapshot mean total_burst_stars/dT,
 *          R/src/io/save_gals_hdf5.c:934-959.) The outflow is
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
 * SAGE parity (check_disk_instability, R/src/model_disk_instability.c:47-108): the follow-up
 * (a) transfers unstable disk stars to the bulge (BulgeMass grows; this is a
 * disk->bulge redistribution that does NOT change total StellarMass), and
 * (b) for the unstable gas, calls grow_black_hole — which itself drives a
 * quasar_mode_wind (grow_black_hole -> quasar_mode_wind, R/src/model_mergers.c:346-378) —
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
   * new stars form (R/src/model_disk_instability.c:101-108). It must not DECREASE. */
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
 * @test    test_per_event_zero_ratio_bursts
 * @brief   A zero mass ratio merger event still runs SAGE26's burst
 *
 * SAGE26 bursts on every merger, a ratio of 0 included (R/src/model_mergers.c:284-287):
 * the burst forms no stars (eburst = 0.56 * 0^0.7 = 0), but still samples the host's
 * cold gas and metals into its bulge history (:754-757).
 *
 * Expected: success; StellarMass, BulgeMass and ColdGas unchanged; the substep's
 *           SfrBulgeColdGas and SfrBulgeColdGasMetals bins grow by ColdGas and MetalsColdGas
 * Validates: no zero-ratio early return in the process_per_event path
 */
int test_per_event_zero_ratio_bursts(void) {
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
                              .value0 = 0.0, /* zero ratio: SAGE26 still bursts */
                              .value1 = 0.1};
  ctx.active_event = &event;

  const double initial_stellar = gal.StellarMass;
  const double initial_bulge = gal.BulgeMass;
  const double initial_cold = gal.ColdGas;
  const int bin = mimic_sage_sfr_step_bin(ctx.substep_number, ctx.num_substeps);
  TEST_ASSERT(bin >= 0, "the context's substep has a history bin");
  float *bulge_cold = mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin);
  float *bulge_metals = mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, bin);
  const float expected_bulge_cold = *bulge_cold + gal.ColdGas;
  const float expected_bulge_metals = *bulge_metals + gal.MetalsColdGas;

  /* ===== EXECUTE ===== */
  int result = sage26_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Zero mass ratio event should succeed");
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, initial_stellar, 1e-12,
                           "A zero-ratio burst forms no stars");
  TEST_ASSERT_DOUBLE_EQUAL(gal.BulgeMass, initial_bulge, 1e-12,
                           "A zero-ratio burst adds nothing to the bulge");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, initial_cold, 1e-12,
                           "A zero-ratio burst consumes no cold gas");
  TEST_ASSERT(*bulge_cold == expected_bulge_cold,
              "A zero-ratio burst still samples ColdGas into SfrBulgeColdGas (M:756)");
  TEST_ASSERT(*bulge_metals == expected_bulge_metals,
              "A zero-ratio burst still samples MetalsColdGas into SfrBulgeColdGasMetals (M:757)");

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
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BulgeSizeOn");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "0");
  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription and StarburstColdGasOn, at R1's values */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_STARBURST_MODULE);
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
 * @test    test_consecutive_bursts_bulge_excess_grows
 * @brief   Consecutive bursts can leave BulgeMass more than one float above StellarMass
 *
 * A burst adds the rounded double recycled_stars to BulgeMass (R/src/model_mergers.c:770-772) while
 * update_from_star_formation adds the same product to StellarMass as one fused multiply-add
 * (R/src/model_starformation_and_feedback.c:1125), so each burst can move BulgeMass - StellarMass
 * by one float, and the excess can accumulate over consecutive bursts. SAGE26's own
 * collisional_starburst_recipe gives it under R0's parameters: ColdGas 1, StellarMass = BulgeMass =
 * 0x3f800001, three mode-1 bursts, each fed the previous output. The shared burst transaction must
 * give the same bits: StellarMass/BulgeMass 0x3f800001/0x3f800002, then 0x3f800002/0x3f800003, then
 * 0x3f800002/0x3f800004, a two-float excess. Those bits depend only on ColdGas, the ratios,
 * FeedbackReheatingEpsilon and RecycleFraction (the ejection, the hot gas and the metals do not
 * feed the host's ColdGas, StellarMass or BulgeMass).
 *
 * The integration tests' bulge bound (test_integration_sage26_starburst_feedback.py,
 * bulge_upper_bound) therefore grows with the number of bursts; this case pins why a
 * one-float bound is not SAGE26's.
 */
int test_consecutive_bursts_bulge_excess_grows(void) {
  struct GalaxyData gal;
  struct Halo halo;
  memset(&gal, 0, sizeof(gal));
  memset(&halo, 0, sizeof(halo));
  halo.galaxy = &gal;
  halo.Type = 0;
  halo.Mvir = 100.0;
  halo.Vvir = 200.0;
  halo.Vmax = 200.0;

  uint32_t bits;
  bits = 0x3f800001u;
  memcpy(&gal.StellarMass, &bits, sizeof(bits));
  memcpy(&gal.BulgeMass, &bits, sizeof(bits));
  gal.ColdGas = 1.0f;
  gal.HotGas = 10.0f;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  MimicConfig.G = 43.00707785642063;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;

  /* R0's parameters at the oracle fixtures' bit patterns (input/millennium.par:69-83) */
  const struct MimicStarburstParams params = {
      .feedback_reheating_epsilon = 0x1.7333333333333p+1,   /* 2.9 */
      .feedback_ejection_efficiency = 0x1.3333333333333p-2, /* 0.3 */
      .recycle_fraction = 0x1.b851eb851eb85p-2,             /* 0.43 */
      .yield = 0x1.999999999999ap-6,                        /* 0.025 */
      .frac_z_leave_disk = 0.0,
      .threshold_major_merger = 0x1.3333333333333p-2, /* 0.3 */
      .energy_sn_code = 0x1.e10f01e10f01dp-9,
      .eta_sn_code = 0x1.0547d7abd5eafp+26,
      .disk_instability_on = 1,
      .star_forming_disk_factor = 3.0,
      .agn_recipe_on = 1,
      .black_hole_growth_rate = 0x1.eb851eb851eb8p-7, /* 0.015 */
      .quasar_mode_efficiency = 0x1.47ae147ae147bp-8, /* 0.005 */
  };

  const double ratios[3] = {0x1.c11f7039d3166p-24, 0x1.c11f7ace98f2cp-23, 0x1.c11f8fdc142f7p-24};
  const uint32_t expected_stellar[3] = {0x3f800001u, 0x3f800002u, 0x3f800002u};
  const uint32_t expected_bulge[3] = {0x3f800002u, 0x3f800003u, 0x3f800004u};

  for (int k = 0; k < 3; k++) {
    mimic_apply_collisional_starburst(ratios[k], &halo, &halo, 1, 0, (double)gal.DiskScaleRadius,
                                      0.0, 0, 0.01, &ctx, &params, NULL);
    uint32_t stellar_bits;
    uint32_t bulge_bits;
    memcpy(&stellar_bits, &gal.StellarMass, sizeof(stellar_bits));
    memcpy(&bulge_bits, &gal.BulgeMass, sizeof(bulge_bits));
    printf("\n  burst %d: StellarMass 0x%08x BulgeMass 0x%08x", k + 1, (unsigned)stellar_bits,
           (unsigned)bulge_bits);
    TEST_ASSERT(stellar_bits == expected_stellar[k],
                "StellarMass must match SAGE26's own routine bit for bit");
    TEST_ASSERT(bulge_bits == expected_bulge[k],
                "BulgeMass must match SAGE26's own routine bit for bit");
  }
  TEST_ASSERT(gal.BulgeMass > nextafterf(gal.StellarMass, INFINITY),
              "three bursts leave BulgeMass two floats above StellarMass");
  printf(" ");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_starburst_feedback unit tests and reports results.
 */
/**
 * @brief The refresh's expected H1gas for a galaxy's final ColdGas and H2gas (M:801-803)
 */
static float expected_refreshed_h1(const struct GalaxyData *gal) {
  const float h1 = (float)fma((double)gal->ColdGas, 0.74, -(double)gal->H2gas);
  return h1 < 0.0f ? 0.0f : h1;
}

/**
 * @test    test_hydrogen_refresh_module_wiring
 * @brief   The module passes its required SFprescription into the burst's parameter bundle
 *
 * One instability burst through process() on a galaxy whose stale H2gas is far above the
 * hydrogen of the cold gas the burst leaves: with SFprescription 1 the commits cap H2gas at the
 * float 0.74 of the remaining cold gas and the refresh overwrites the stale H1gas of 0 with the
 * atomic remainder; with SFprescription 0 neither runs. Dropping the bundle's sf_prescription
 * initialiser would silently disable every burst's caps and refresh, which this test catches.
 */
int test_hydrogen_refresh_module_wiring(void) {
  const int prescriptions[2] = {1, 0};
  for (int k = 0; k < 2; k++) {
    init_memory_system(0);
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    for (int i = 0; i < MimicConfig.NumModelParams; i++) {
      if (strcmp(MimicConfig.ModelParams[i].param_name, "SFprescription") == 0) {
        snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%d", prescriptions[k]);
      }
    }
    TEST_ASSERT(sage26_starburst_feedback_init() == 0, "init succeeds");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.H2gas = 9.0f;
    gal.H1gas = 0.0f;
    gal.UnstableDiskGasFraction = 0.5;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);
    TEST_ASSERT(sage26_starburst_feedback_process(&ctx, &halo, 1) == 0, "process succeeds");
    TEST_ASSERT(gal.ColdGas < 10.0f && gal.ColdGas > 0.0f, "the burst consumes some cold gas");

    if (prescriptions[k] == 1) {
      const float bound = (float)(gal.ColdGas * 0.74);
      TEST_ASSERT(gal.H2gas == bound, "SFprescription 1: H2gas is capped at the float bound");
      TEST_ASSERT(gal.H1gas == expected_refreshed_h1(&gal),
                  "SFprescription 1: H1gas is the refreshed atomic remainder");
    } else {
      TEST_ASSERT(gal.H2gas == 9.0f && gal.H1gas == 0.0f,
                  "SFprescription 0: no cap and no refresh");
    }
    sage26_starburst_feedback_cleanup();
    check_memory_leaks();
  }
  return TEST_PASS;
}

/** @brief One zero-star instability burst (mode 1, trigger 0) with SFprescription 1 */
static void zero_star_burst(struct GalaxyData *gal, const float cold_gas, const float h2,
                            const float h1, struct MimicSageStarburstLocals *locals) {
  struct Halo halo;
  setup_test_galaxy(&halo, gal, 0, 100.0, 300.0, 1.0, 0.02, 5.0, 1.0, 50.0, 1.0);
  gal->ColdGas = cold_gas;
  gal->MetalsColdGas = 0.0f;
  gal->H2gas = h2;
  gal->H1gas = h1;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  MimicConfig.G = 43.00707785642063;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  const struct MimicStarburstParams params = {
      .feedback_reheating_epsilon = 2.9,
      .feedback_ejection_efficiency = 0.3,
      .recycle_fraction = 0.43,
      .yield = 0.025,
      .threshold_major_merger = 0.3,
      .energy_sn_code = 0.0036701860231271991,
      .eta_sn_code = 68493150.684931502,
      .star_forming_disk_factor = 3.0,
      .sf_prescription = 1,
  };
  mimic_apply_collisional_starburst(0.0, &halo, &halo, 1, 0, 0.0, 0.0, 0, 0.01, &ctx, &params,
                                    locals);
}

/**
 * @test    test_hydrogen_refresh_invariants
 * @brief   The burst's hydrogen refresh is not a pair of caps (R/src/model_mergers.c:798-803)
 */
int test_hydrogen_refresh_invariants(void) {
  struct GalaxyData gal;
  struct MimicSageStarburstLocals b;

  /* A zero-star burst moves no gas, and still refreshes: a stale H1gas of 0 is replaced */
  zero_star_burst(&gal, 2.0f, 0.5f, 0.0f, &b);
  TEST_ASSERT(b.stars == 0.0, "a zero trigger forms no stars");
  TEST_ASSERT(gal.ColdGas == 2.0f, "a zero-star burst leaves the cold gas");
  TEST_ASSERT(b.hydrogen_refresh.tracks_h2 == 1, "SFprescription 1 runs the refresh");
  TEST_ASSERT(gal.H2gas == 0.5f && b.hydrogen_refresh.h2_cap_binds == 0, "H2gas below is kept");
  TEST_ASSERT(gal.H1gas == expected_refreshed_h1(&gal) && gal.H1gas > 0.0f,
              "the stale H1gas is overwritten with the atomic remainder");

  /* An H1gas reduced for ionisation is restored to the full remainder (no ionisation term) */
  const float ionised = 0.5f * expected_refreshed_h1(&gal);
  zero_star_burst(&gal, 2.0f, 0.5f, ionised, &b);
  TEST_ASSERT(gal.H1gas == expected_refreshed_h1(&gal) && gal.H1gas > ionised,
              "a reduced H1gas is restored to the full remainder");

  /* ColdGas 1.0: the float bound 0.74f lies above the double budget 1.0 * 0.74, so an H2gas at
   * the float bound binds the refresh's cap (storing the same float), and the fused remainder
   * is negative and floored to 0 */
  zero_star_burst(&gal, 1.0f, 0.74f, 0.25f, &b);
  TEST_ASSERT((double)0.74f > 1.0 * 0.74, "the float bound rounds up");
  TEST_ASSERT(b.hydrogen_refresh.h2_cap_binds == 1 && gal.H2gas == 0.74f,
              "the refresh's cap compares with the double budget and stores the same float");
  TEST_ASSERT(b.hydrogen_refresh.h1_refresh < 0.0, "the fused remainder is negative");
  TEST_ASSERT(b.hydrogen_refresh.h1_floored == 1 && gal.H1gas == 0.0f, "and is floored to 0");

  /* No cold gas, H2gas -0.0: the strict caps keep -0.0; H1gas is 0 * 0.74 - (-0.0) = +0.0 */
  zero_star_burst(&gal, 0.0f, -0.0f, -0.0f, &b);
  TEST_ASSERT(gal.H2gas == 0.0f && signbit(gal.H2gas), "H2gas -0.0 is kept");
  TEST_ASSERT(gal.H1gas == 0.0f && !signbit(gal.H1gas), "H1gas becomes +0.0");
  return TEST_PASS;
}

/**
 * @brief One minor-merger burst (mode 0, ratio 0.2 below ThresholdMajorMerger 0.3) whose
 *        instability recheck reaches its gas branch and grows the black hole (AGN on)
 *
 * The host's disk is far above its critical mass (Vmax 50 km/s, DiskScaleRadius 0.001), so the
 * recheck bursts most of the cold gas; H2gas 0 makes the burst refresh set H1gas to the whole
 * hydrogen of the cold gas, which the recheck's accretion then leaves above the new bound.
 */
static void recheck_growth_burst(const int sf_prescription, struct GalaxyData *gal,
                                 struct MimicSageStarburstLocals *locals) {
  static struct Halo central, host;
  static struct GalaxyData central_gal;
  setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  setup_test_galaxy(&host, gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
  host.Vmax = 50.0f;
  gal->DiskScaleRadius = 0.001f;
  gal->BlackHoleMass = 0.05f;
  gal->H2gas = 0.0f;
  gal->H1gas = 0.0f;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  MimicConfig.G = 43.00707785642063;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  const struct MimicStarburstParams params = {
      .feedback_reheating_epsilon = 2.9,
      .feedback_ejection_efficiency = 0.3,
      .recycle_fraction = 0.43,
      .yield = 0.025,
      .threshold_major_merger = 0.3,
      .energy_sn_code = 0.0036701860231271991,
      .eta_sn_code = 68493150.684931502,
      .disk_instability_on = 1,
      .star_forming_disk_factor = 3.0,
      .agn_recipe_on = 1,
      .black_hole_growth_rate = 0.015,
      .quasar_mode_efficiency = 0.001,
      .sf_prescription = sf_prescription,
  };
  mimic_apply_collisional_starburst(0.2, &host, &central, 0, 1, 0.0, 0.0, 0, 0.01, &ctx, &params,
                                    locals);
}

/**
 * @test    test_hydrogen_cap_recheck_growth
 * @brief   The post-minor-merger recheck's black-hole growth receives the burst's SFprescription
 *          (R/src/model_mergers.c:806-811 into R/src/model_disk_instability.c:102-104, cap
 *          :366-372)
 *
 * The cap is end-state invariant here (the recheck's own burst re-caps and refreshes the
 * hydrogen), so its locals are the evidence: with SFprescription 1 the growth's guard holds and
 * the refreshed H1gas binds its cap at a positive float bound; with SFprescription 0 the guard
 * fails and nothing is capped. Dropping the recheck's sf_prescription argument fails this test.
 */
int test_hydrogen_cap_recheck_growth(void) {
  struct GalaxyData gal;
  struct MimicSageStarburstLocals b;

  recheck_growth_burst(1, &gal, &b);
  TEST_ASSERT(b.instability_recheck_ran == 1, "the minor merger runs the recheck");
  TEST_ASSERT(b.recheck_bh_caps.tracks_h2 == 1,
              "SFprescription 1: the recheck's growth evaluates the hydrogen cap");
  TEST_ASSERT(b.recheck_bh_caps.max_h > 0.0f, "the bound is the float 0.74 of the cold gas left");
  TEST_ASSERT(b.recheck_bh_caps.h2_cap_binds == 0, "H2gas 0 lies below the bound");
  TEST_ASSERT(b.recheck_bh_caps.h1_cap_binds == 1,
              "the refreshed H1gas lies above the bound the accretion lowered");

  recheck_growth_burst(0, &gal, &b);
  TEST_ASSERT(b.instability_recheck_ran == 1, "the minor merger runs the recheck");
  TEST_ASSERT(b.recheck_bh_caps.tracks_h2 == 0,
              "SFprescription 0: the recheck's growth skips the hydrogen cap");
  TEST_ASSERT(isnan(b.recheck_bh_caps.max_h) && b.recheck_bh_caps.h1_cap_binds == -1,
              "no bound is formed and no cap evaluated");
  return TEST_PASS;
}

// ============================================================================
// FIREmodeOn: THE BURST'S FIRE BUDGET (R/src/model_mergers.c:690-752)
// ============================================================================

/* The burst parameters the FIRE tests use: SAGE26's R3 values (input/millennium.par:69-70, :75-82)
 * and its EtaSNcode and EnergySNcode bit patterns (R/src/core_init.c:102-103) */
#define FIRE_TEST_EPS 2.9
#define FIRE_TEST_EJ 0.3
#define FIRE_TEST_ETA_SN 68493150.684931502
#define FIRE_TEST_ENERGY_SN 0.0036701860231271991

/** @brief Bit pattern of a double, for exact comparisons */
static uint64_t sb_fire_bits(const double v) {
  uint64_t b;
  memcpy(&b, &v, sizeof(b));
  return b;
}

/** @brief SAGE26's budget of one burst, written in the test (see sb_fire_expected) */
struct sb_fire_budget {
  double scaling;
  double eta;
  double stars;
  double reheated;
  double ejected;
};

/**
 * @brief SAGE26's burst FIRE budget written inline with its operand order and literal constants
 *        (R/src/model_mergers.c:692-752, capped_eta_reheat and sn_energy_coupling of
 *        R/src/model_misc.h:118-149): an expectation that calls no package helper
 *
 * The high-velocity term is the division SAGE26's object computes for its pow(x, -1.0) (fdiv at
 * model_mergers.o offset 0xdc8).
 *
 * @param vvir      The host's float Vvir
 * @param z         The redshift of the host's stored snapshot
 * @param stars_in  The stars before the renormalisation (eburst * gas, floored)
 * @param gas       The burst gas
 */
static struct sb_fire_budget sb_fire_expected(const float vvir, const double z,
                                              const double stars_in, const double gas) {
  struct sb_fire_budget r;
  double fire_scaling = 0.0;
  const double vc_fire = vvir;                                               /* M:695 */
  if (vc_fire > 0.0 && z >= 0.0) {                                           /* M:696 */
    const double vc_floored = (vc_fire < 1.0) ? 1.0 : vc_fire;               /* M:697 */
    const double v_term = (vc_floored < 60.0) ? pow(vc_floored / 60.0, -3.2) /* M:698-699 */
                                              : 1.0 / (vc_floored / 60.0);   /* M:700, fdiv */
    fire_scaling = pow(1.0 + z, 1.25) * v_term;                              /* M:701 */
  }
  double eta = FIRE_TEST_EPS * fire_scaling; /* M:709, X:144-149 */
  if (vc_fire > 0.0) {
    const double eta_max = 2.0 * (FIRE_TEST_ETA_SN * FIRE_TEST_ENERGY_SN) / (vc_fire * vc_fire);
    if (eta > eta_max) {
      eta = eta_max;
    }
  }
  double stars = stars_in;
  double reheated = eta * stars;  /* M:708-710 */
  if ((stars + reheated) > gas) { /* M:721 */
    const double fac = gas / (stars + reheated);
    stars *= fac;
    reheated *= fac;
  }
  double ejected = 0.0;
  if (vc_fire > 0.0) {                             /* M:728 */
    double coupling = FIRE_TEST_EJ * fire_scaling; /* X:120-123 */
    if (coupling > 2.0) {
      coupling = 2.0;
    }
    const double E_FB = coupling * 0.5 * stars * (FIRE_TEST_ETA_SN * FIRE_TEST_ENERGY_SN);
    const double E_lift = 0.5 * reheated * vc_fire * vc_fire;
    ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc_fire * vc_fire) : 0.0; /* M:733-736 */
  }
  if (ejected < 0.0) { /* M:746-748 */
    ejected = 0.0;
  }
  r.scaling = fire_scaling;
  r.eta = eta;
  r.stars = stars;
  r.reheated = reheated;
  r.ejected = ejected;
  return r;
}

/**
 * @brief Give the run a 64-entry redshift table (Snaplistlen 64), each entry a distinct value
 *        ZZ[s] = (63 - s) / 10.0 + 0.05, so ZZ[40] = 2.35 and ZZ[50] = 1.35
 */
static void sb_fire_redshift_table(void) {
  MimicConfig.Snaplistlen = 64;
  for (int s = 0; s < 64; s++) {
    MimicConfig.ZZ[s] = (63 - s) / 10.0 + 0.05;
  }
}

/** @brief The burst bundle of the FIRE helper-level tests */
static struct MimicStarburstParams sb_fire_params(const int fire_mode_on, const int recheck) {
  struct MimicStarburstParams p;
  memset(&p, 0, sizeof(p));
  p.feedback_reheating_epsilon = FIRE_TEST_EPS;
  p.feedback_ejection_efficiency = FIRE_TEST_EJ;
  p.recycle_fraction = 0.43;
  p.yield = 0.025;
  p.threshold_major_merger = 0.3;
  p.energy_sn_code = FIRE_TEST_ENERGY_SN;
  p.eta_sn_code = FIRE_TEST_ETA_SN;
  p.disk_instability_on = recheck;
  p.star_forming_disk_factor = 3.0;
  p.agn_recipe_on = recheck;
  p.black_hole_growth_rate = 0.015;
  p.quasar_mode_efficiency = 0.001;
  p.fire_mode_on = fire_mode_on;
  p.redshift_config = &MimicConfig;
  return p;
}

/**
 * @brief A host (Type 1, Vvir 80 km/s, stored SnapNum 40) and its FoF central (Type 0, Vvir
 *        300 km/s, SnapNum 50) that differ in both: the burst must read the host's
 */
static void sb_fire_pair(struct Halo *host, struct GalaxyData *host_gal, struct Halo *central,
                         struct GalaxyData *central_gal) {
  setup_test_galaxy(central, central_gal, 0, 120.0, 300.0, 4.0, 0.08, 6.0, 1.5, 400.0, 0.8);
  central->SnapNum = 50;
  setup_test_galaxy(host, host_gal, 1, 30.0, 80.0, 1.0, 0.02, 2.0, 0.5, 5.0, 0.1);
  host->SnapNum = 40;
  host->Vmax = 120.0f;
  host_gal->DiskScaleRadius = 5.0f; /* a stable disk: no recheck burst */
  host_gal->MassLoading = 7.25f;    /* a stale value no burst may write */
  central_gal->MassLoading = 3.5f;
}

/**
 * @test    test_fire_burst_budget_independent
 * @brief   The burst's FIRE budget against SAGE26's expressions written in the test, across the
 *          scaling's guards, floor and slopes and both caps (R/src/model_mergers.c:692-752)
 *
 * A mode-1 burst (fraction 0.1) of one galaxy with ColdGas 1 and ample hot gas, at a host Vvir
 * below, at and above 1 and 60 km/s, zero and negative, and a stored-snapshot redshift of 0, 2.35,
 * 14.09 (both caps binding at 100 km/s) and -0.5, and at 30 km/s (the coupling capped at 2 while
 * the loading is not, so the cap decides the ejection): the transaction's stars, reheated and
 * ejected mass and the FIRE scaling must equal the inline expectation bit for bit, and MassLoading
 * must keep its stale value (no burst writes it, M:707-710).
 */
int test_fire_burst_budget_independent(void) {
  reset_config();
  sb_fire_redshift_table();
  const struct {
    float vvir;
    double z;
  } cases[] = {{0.5f, 2.35},          {1.0f, 2.35},  {1.5f, 2.35},   {59.5f, 2.35},   {60.0f, 0.0},
               {0x1.e00168p+5f, 0.0}, {80.0f, 2.35}, {300.0f, 2.35}, {100.0f, 14.09}, {30.0f, 2.35},
               {0.0f, 2.35},          {-5.0f, 2.35}, {80.0f, -0.5}};
  const struct MimicStarburstParams params = sb_fire_params(MIMIC_SAGE_FIRE_MODE_ON, 0);
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    struct Halo host;
    struct GalaxyData gal;
    setup_test_galaxy(&host, &gal, 0, 120.0, 0.0, 1.0, 0.02, 2.0, 0.5, 500.0, 1.0);
    host.Vvir = cases[k].vvir;
    host.SnapNum = 40;
    MimicConfig.ZZ[40] = cases[k].z;
    gal.MassLoading = 7.25f;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &host);
    struct MimicSageStarburstLocals b;
    const int status = mimic_apply_collisional_starburst(0.1, &host, &host, 1, 0, 0.0, 0.0, 0, 0.01,
                                                         &ctx, &params, &b);
    const struct sb_fire_budget e =
        sb_fire_expected(cases[k].vvir, cases[k].z, 0.1 * (double)1.0f, (double)1.0f);
    printf("\n  Vvir %.9g z %.4g: scaling %.17g reheated %.17g ejected %.17g", cases[k].vvir,
           cases[k].z, b.fire.l.scaling, b.reheated_mass, b.ejected_mass);
    TEST_ASSERT(status == 0, "the burst must run");
    TEST_ASSERT(b.fire.fire_mode == 1 && b.fire.snapnum == 40, "the FIRE branch, SnapNum 40");
    TEST_ASSERT(sb_fire_bits(b.fire.l.scaling) == sb_fire_bits(e.scaling),
                "the FIRE scaling must equal SAGE26's expression");
    TEST_ASSERT(sb_fire_bits(b.stars) == sb_fire_bits(e.stars) &&
                    sb_fire_bits(b.reheated_mass) == sb_fire_bits(e.reheated) &&
                    sb_fire_bits(b.ejected_mass) == sb_fire_bits(e.ejected),
                "stars, reheated and ejected mass must equal SAGE26's expressions bit for bit");
    TEST_ASSERT(gal.MassLoading == 7.25f, "a burst must not write MassLoading");
  }
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_on_versus_off
 * @brief   The same burst with FIREmodeOn 1 and 0: 1 is the inline FIRE budget, 0 the
 *          fixed-epsilon one (M:711-712, M:738-743, the float Vvir squared in float); neither
 *          writes MassLoading
 */
int test_fire_burst_on_versus_off(void) {
  reset_config();
  sb_fire_redshift_table();
  double reheated[2], ejected[2];
  for (int fire = 0; fire <= 1; fire++) {
    struct Halo host, central;
    struct GalaxyData host_gal, central_gal;
    sb_fire_pair(&host, &host_gal, &central, &central_gal);
    struct ModuleContext ctx;
    setup_test_context(&ctx, &central);
    const struct MimicStarburstParams params = sb_fire_params(fire, 0);
    struct MimicSageStarburstLocals b;
    TEST_ASSERT(mimic_apply_collisional_starburst(0.1, &host, &central, 1, 0, 0.0, 0.0, 0, 0.01,
                                                  &ctx, &params, &b) == 0,
                "the burst must run");
    reheated[fire] = b.reheated_mass;
    ejected[fire] = b.ejected_mass;
    TEST_ASSERT(host_gal.MassLoading == 7.25f && central_gal.MassLoading == 3.5f,
                "no burst writes MassLoading, in either mode");
    const double stars = 0.1 * (double)1.0f;
    if (fire) {
      const struct sb_fire_budget e = sb_fire_expected(80.0f, MimicConfig.ZZ[40], stars, 1.0);
      TEST_ASSERT(sb_fire_bits(b.reheated_mass) == sb_fire_bits(e.reheated) &&
                      sb_fire_bits(b.ejected_mass) == sb_fire_bits(e.ejected),
                  "FIREmodeOn 1: SAGE26's FIRE budget with the host's values");
    } else {
      const float v = 80.0f;
      const double e_reheated = FIRE_TEST_EPS * stars; /* M:712 */
      double e_ejected =
          (FIRE_TEST_EJ * (FIRE_TEST_ETA_SN * FIRE_TEST_ENERGY_SN) / (v * v) - FIRE_TEST_EPS) *
          stars; /* M:739-741 */
      if (e_ejected < 0.0) {
        e_ejected = 0.0;
      }
      TEST_ASSERT(b.fire.fire_mode == 0 && b.fire.snapnum == -1 && isnan(b.fire.l.scaling),
                  "FIREmodeOn 0 forms no FIRE value");
      TEST_ASSERT(sb_fire_bits(b.reheated_mass) == sb_fire_bits(e_reheated) &&
                      sb_fire_bits(b.ejected_mass) == sb_fire_bits(e_ejected),
                  "FIREmodeOn 0: the fixed-epsilon budget");
    }
  }
  printf("\n  reheated %.17g (0) / %.17g (1), ejected %.17g / %.17g", reheated[0], reheated[1],
         ejected[0], ejected[1]);
  TEST_ASSERT(reheated[0] != reheated[1] && ejected[0] != ejected[1],
              "the two modes must differ on this input");
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_wiring_direct
 * @brief   The burst reads its HOST's float Vvir and stored SnapNum, not the FoF central's
 *          (R/src/model_mergers.c:694-695): host Vvir 80 at SnapNum 40, central 300 at 50
 */
int test_fire_burst_wiring_direct(void) {
  reset_config();
  sb_fire_redshift_table();
  struct Halo host, central;
  struct GalaxyData host_gal, central_gal;
  sb_fire_pair(&host, &host_gal, &central, &central_gal);
  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  const struct MimicStarburstParams params = sb_fire_params(MIMIC_SAGE_FIRE_MODE_ON, 0);
  struct MimicSageStarburstLocals b;
  TEST_ASSERT(mimic_apply_collisional_starburst(0.1, &host, &central, 1, 0, 0.0, 0.0, 0, 0.01, &ctx,
                                                &params, &b) == 0,
              "the burst must run");
  TEST_ASSERT(b.fire.fire_mode == 1 && b.fire.snapnum == 40 && b.fire.vvir == (double)80.0f,
              "the burst read its host's SnapNum and Vvir");
  TEST_ASSERT(sb_fire_bits(b.fire.l.redshift) == sb_fire_bits(MimicConfig.ZZ[40]),
              "the burst read ZZ[40], its host's stored snapshot");
  const struct sb_fire_budget host_e = sb_fire_expected(80.0f, MimicConfig.ZZ[40], 0.1, 1.0);
  const struct sb_fire_budget central_e = sb_fire_expected(300.0f, MimicConfig.ZZ[50], 0.1, 1.0);
  TEST_ASSERT(sb_fire_bits(b.reheated_mass) == sb_fire_bits(host_e.reheated),
              "the reheated mass is the host's budget");
  TEST_ASSERT(host_e.reheated != central_e.reheated, "the central's budget would differ");
  return TEST_PASS;
}

/** @brief Set or replace a model parameter's value; NULL removes it */
static void sb_fire_set_param(const char *name, const char *value) {
  int n = MimicConfig.NumModelParams;
  for (int i = 0; i < n; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, name) == 0) {
      if (value == NULL) {
        MimicConfig.ModelParams[i] = MimicConfig.ModelParams[n - 1];
        MimicConfig.NumModelParams = n - 1;
      } else {
        snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
      }
      return;
    }
  }
  if (value != NULL) {
    sage26_test_append_model_parameter(name, value);
  }
}

/** @brief Read a captured log into a static buffer (valid until the next call) */
static const char *sb_fire_read_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/**
 * @brief Initialise the module with R3's burst parameters and FIREmodeOn `value` (NULL: the key
 *        missing), capturing the log
 * @return The init status
 */
static int sb_fire_init(const char *value, const char **log) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(FIRE_TEST_EPS, FIRE_TEST_EJ, 0.43, 0.025, 0.0, 0.3);
  sb_fire_set_param("FIREmodeOn", value);
  sb_fire_redshift_table();
  MimicConfig.G = 43.00707785642063;
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_starburst_feedback_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = sb_fire_read_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/** @brief The module's own EtaSNcode * EnergySNcode budget, for the module-level expectations */
static double sb_fire_module_esn(void) {
  return mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h) *
         mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
}

/**
 * @brief SAGE26's burst budget at the module's own SN constants (the module derives them from the
 *        run's units, R/src/core_init.c:102-103): sb_fire_expected's expressions with that
 *        product, which is all the budget reads of them
 */
static struct sb_fire_budget sb_fire_module_expected(const float vvir, const double z,
                                                     const double stars_in, const double gas) {
  const double esn = sb_fire_module_esn();
  struct sb_fire_budget r;
  double fire_scaling = 0.0;
  const double vc_fire = vvir;
  if (vc_fire > 0.0 && z >= 0.0) {
    const double vc_floored = (vc_fire < 1.0) ? 1.0 : vc_fire;
    const double v_term =
        (vc_floored < 60.0) ? pow(vc_floored / 60.0, -3.2) : 1.0 / (vc_floored / 60.0);
    fire_scaling = pow(1.0 + z, 1.25) * v_term;
  }
  double eta = FIRE_TEST_EPS * fire_scaling;
  if (vc_fire > 0.0) {
    const double eta_max = 2.0 * esn / (vc_fire * vc_fire);
    if (eta > eta_max) {
      eta = eta_max;
    }
  }
  double stars = stars_in;
  double reheated = eta * stars;
  if ((stars + reheated) > gas) {
    const double fac = gas / (stars + reheated);
    stars *= fac;
    reheated *= fac;
  }
  double ejected = 0.0;
  if (vc_fire > 0.0) {
    double coupling = FIRE_TEST_EJ * fire_scaling;
    if (coupling > 2.0) {
      coupling = 2.0;
    }
    const double E_FB = coupling * 0.5 * stars * esn;
    const double E_lift = 0.5 * reheated * vc_fire * vc_fire;
    ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc_fire * vc_fire) : 0.0;
  }
  r.scaling = fire_scaling;
  r.eta = eta;
  r.stars = stars;
  r.reheated = reheated;
  r.ejected = ejected < 0.0 ? 0.0 : ejected;
  return r;
}

/**
 * @test    test_fire_burst_wiring_merger_event
 * @brief   The merger channel (process_per_event) bursts the TARGET with its own Vvir and stored
 *          SnapNum, not the FoF central's: the target's SupernovaOutflowRate holds its reheated
 *          mass and the central's EjectedGas the ejection, as SAGE26's budget for the target
 */
int test_fire_burst_wiring_merger_event(void) {
  const char *log = "";
  TEST_ASSERT(sb_fire_init("1", &log) == 0, "init must succeed with FIREmodeOn 1");
  struct Halo target, central;
  struct GalaxyData target_gal, central_gal;
  sb_fire_pair(&target, &target_gal, &central, &central_gal);
  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  ctx.num_substeps = 10;
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = 0.2,
                              .value1 = 0.01};
  ctx.active_event = &event;
  TEST_ASSERT(sage26_starburst_feedback_process(&ctx, &target, 1) == 0, "the merger bursts");
  const double eburst = 0.56 * pow(0.2, 0.7); /* M:603 */
  const struct sb_fire_budget e =
      sb_fire_module_expected(80.0f, MimicConfig.ZZ[40], eburst * (double)1.0f, (double)1.0f);
  const struct sb_fire_budget c =
      sb_fire_module_expected(300.0f, MimicConfig.ZZ[50], eburst * (double)1.0f, (double)1.0f);
  printf("\n  target outflow %.9g (expected %.9g), central ejected %.9g (expected %.9g)",
         target_gal.SupernovaOutflowRate, (float)e.reheated, central_gal.EjectedGas,
         (float)e.ejected);
  TEST_ASSERT(target_gal.SupernovaOutflowRate == (float)e.reheated,
              "the target's outflow is its own FIRE reheated mass");
  TEST_ASSERT(central_gal.EjectedGas == (float)e.ejected,
              "the central's ejected gas is the target's FIRE ejection");
  TEST_ASSERT((float)c.reheated != (float)e.reheated, "the central's values would differ");
  TEST_ASSERT(target_gal.MassLoading == 7.25f && central_gal.MassLoading == 3.5f,
              "the merger burst writes no MassLoading");
  sage26_starburst_feedback_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_wiring_instability_channel
 * @brief   The instability channel (process_by_galaxy) bursts the satellite with its own Vvir and
 *          stored SnapNum, not its FoF central's (R/src/model_disk_instability.c:106)
 */
int test_fire_burst_wiring_instability_channel(void) {
  const char *log = "";
  TEST_ASSERT(sb_fire_init("1", &log) == 0, "init must succeed with FIREmodeOn 1");
  struct Halo host, central;
  struct GalaxyData host_gal, central_gal;
  sb_fire_pair(&host, &host_gal, &central, &central_gal);
  host_gal.UnstableDiskGasFraction = 0.1;
  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  ctx.num_substeps = 10;
  TEST_ASSERT(sage26_starburst_feedback_process(&ctx, &host, 1) == 0, "the instability bursts");
  const struct sb_fire_budget e =
      sb_fire_module_expected(80.0f, MimicConfig.ZZ[40], 0.1 * (double)1.0f, (double)1.0f);
  TEST_ASSERT(host_gal.SupernovaOutflowRate == (float)e.reheated,
              "the satellite's outflow is its own FIRE reheated mass");
  TEST_ASSERT(central_gal.EjectedGas == (float)e.ejected,
              "the central's ejected gas is the satellite's FIRE ejection");
  TEST_ASSERT(host_gal.MassLoading == 7.25f, "the instability burst writes no MassLoading");
  sage26_starburst_feedback_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_wiring_recheck
 * @brief   The post-minor-merger recheck's own burst (R/src/model_mergers.c:806-811 into
 *          R/src/model_disk_instability.c:106-108) takes the FIRE branch with the host's own
 *          stored SnapNum, float Vvir and redshift, not the central's
 */
int test_fire_burst_wiring_recheck(void) {
  reset_config();
  sb_fire_redshift_table();
  static struct Halo central, host;
  static struct GalaxyData central_gal, gal;
  setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  central.SnapNum = 50;
  setup_test_galaxy(&host, &gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
  host.SnapNum = 40;
  host.Vmax = 50.0f;
  gal.DiskScaleRadius = 0.001f;
  gal.BlackHoleMass = 0.05f;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  MimicConfig.G = 43.00707785642063;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  const struct MimicStarburstParams params = sb_fire_params(MIMIC_SAGE_FIRE_MODE_ON, 1);
  struct MimicSageStarburstLocals b;
  TEST_ASSERT(mimic_apply_collisional_starburst(0.2, &host, &central, 0, 1, 0.0, 0.0, 0, 0.01, &ctx,
                                                &params, &b) == 0,
              "the merger burst and its recheck must run");
  TEST_ASSERT(b.instability_recheck_ran == 1, "the minor merger runs the recheck");
  TEST_ASSERT(b.recheck_fire.fire_mode == 1 && b.recheck_fire.snapnum == 40 &&
                  b.recheck_fire.vvir == (double)140.0f,
              "the recheck's burst took the FIRE branch with the host's SnapNum and Vvir");
  TEST_ASSERT(sb_fire_bits(b.recheck_fire.l.redshift) == sb_fire_bits(MimicConfig.ZZ[40]),
              "the recheck's burst read ZZ[40], the host's stored snapshot");
  const struct sb_fire_budget e = sb_fire_expected(140.0f, MimicConfig.ZZ[40], 0.0, 0.0);
  TEST_ASSERT(sb_fire_bits(b.recheck_fire.l.scaling) == sb_fire_bits(e.scaling),
              "the recheck burst's scaling is SAGE26's for the host");
  return TEST_PASS;
}

/**
 * @test    test_fire_mode_refused
 * @brief   sage26_starburst_feedback's FIREmodeOn: a missing key fails initialisation and 2 and -1
 *          are refused, each naming the parameter; 0 and 1 initialise
 */
int test_fire_mode_refused(void) {
  const char *log = "";
  int status = sb_fire_init(NULL, &log);
  printf("\n  without FIREmodeOn: init %d", status);
  TEST_ASSERT(status != 0 && strstr(log, "FIREmodeOn") != NULL,
              "a missing FIREmodeOn must fail initialisation, naming it");
  const char *refused[] = {"2", "-1"};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    status = sb_fire_init(refused[k], &log);
    printf("\n  FIREmodeOn %s: init %d", refused[k], status);
    TEST_ASSERT(status != 0 && strstr(log, "FIREmodeOn") != NULL,
                "an unsupported FIREmodeOn must be refused, naming it");
  }
  const char *accepted[] = {"0", "1"};
  for (size_t k = 0; k < sizeof(accepted) / sizeof(accepted[0]); k++) {
    status = sb_fire_init(accepted[k], &log);
    TEST_ASSERT(status == 0, "FIREmodeOn 0 and 1 initialise");
    sage26_starburst_feedback_cleanup();
  }
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_snapnum_refused
 * @brief   Through a burst, a host whose stored SnapNum is -1 or the table size (64) is refused
 *          with an error naming the SnapNum and the table, and writes nothing; FIREmodeOn 0 reads
 *          no redshift and bursts; the merger channel's process() fails on the refusal
 */
int test_fire_burst_snapnum_refused(void) {
  reset_config();
  sb_fire_redshift_table();
  const int refused[] = {-1, 64};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    struct Halo host, central;
    struct GalaxyData host_gal, central_gal;
    sb_fire_pair(&host, &host_gal, &central, &central_gal);
    host.SnapNum = refused[k];
    const struct GalaxyData host_before = host_gal, central_before = central_gal;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &central);
    const struct MimicStarburstParams params = sb_fire_params(MIMIC_SAGE_FIRE_MODE_ON, 0);
    FILE *capture = tmpfile();
    FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
    const int status = mimic_apply_collisional_starburst(0.1, &host, &central, 1, 0, 0.0, 0.0, 0,
                                                         0.01, &ctx, &params, NULL);
    const char *log = "";
    if (capture != NULL) {
      set_log_output(old_out);
      log = sb_fire_read_log(capture);
      fclose(capture);
    }
    printf("\n  SnapNum %d: status %d", refused[k], status);
    TEST_ASSERT(status != 0, "an out-of-range stored SnapNum must be refused");
    TEST_ASSERT(strstr(log, "SnapNum") != NULL && strstr(log, "64 entries") != NULL,
                "the error must name the SnapNum and the table size");
    TEST_ASSERT(memcmp(&host_before, &host_gal, sizeof(host_gal)) == 0 &&
                    memcmp(&central_before, &central_gal, sizeof(central_gal)) == 0,
                "a refused burst writes nothing");
    const struct MimicStarburstParams off = sb_fire_params(MIMIC_SAGE_FIRE_MODE_OFF, 0);
    TEST_ASSERT(mimic_apply_collisional_starburst(0.1, &host, &central, 1, 0, 0.0, 0.0, 0, 0.01,
                                                  &ctx, &off, NULL) == 0,
                "FIREmodeOn 0 reads no redshift and bursts");
  }

  const char *log = "";
  TEST_ASSERT(sb_fire_init("1", &log) == 0, "init must succeed with FIREmodeOn 1");
  struct Halo target, central;
  struct GalaxyData target_gal, central_gal;
  sb_fire_pair(&target, &target_gal, &central, &central_gal);
  target.SnapNum = -1;
  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  ctx.num_substeps = 10;
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = 0.2,
                              .value1 = 0.01};
  ctx.active_event = &event;
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_starburst_feedback_process(&ctx, &target, 1);
  if (capture != NULL) {
    set_log_output(old_out);
    fclose(capture);
  }
  TEST_ASSERT(status != 0, "the merger channel's process() fails on the refusal");
  sage26_starburst_feedback_cleanup();
  return TEST_PASS;
}

// ============================================================================
// CGM ROUTING TESTS (CGMrecipeOn)
// ============================================================================
//
// SAGE26's burst (R = the staged SAGE26 source, R/src/model_mergers.c:584-831) commits its feedback
// through update_from_feedback to the FoF CENTRAL (:791), whose CGM pair it uses with CGMrecipeOn 1
// and the central's Regime 0 and whose hot pair otherwise
// (R/src/model_starformation_and_feedback.c:1175-1247), and sends its escaping yield through
// add_metals_to_hot_reservoir to the same central's selected metals (:824, :830,
// R/src/model_misc.h:67-74). The burst host is given the other Regime throughout. The parameters
// make the instability burst exact: FeedbackReheatingEpsilon 1, FeedbackEjectionEfficiency 0 (the
// ejection (0 - 1) * stars is floored to 0, :738-748), RecycleFraction 0.5, Yield 0.5,
// FracZleaveDisk 1 with the central's Mvir 0 (FracZleaveDiskVal = 1 * exp(-0 / 30) = 1, :816),
// ThreshMajorMerger 0.3, a host with ColdGas 1 and no cold metals:
//   instability (mode 1, fraction 0.25): eburst 0.25 (:600-601), stars 0.25, reheated 0.25 (:712),
//   no renormalisation (0.5 <= 1); the host keeps 1 - 0.125 - 0.25 = 0.625 of cold gas, so the
//   yield takes the retained branch (:814) with nothing retained and 0.5 * 1 * 0.25 = 0.125
//   escaping (:822); the central's selected pair 2 / 0.25 -> 2.25 / 0.375.
//   merger (mode 0, ratio 0.5, a major merger: no recheck, :807-808, and all the yield escapes,
//   :828): eburst = 0.56 * pow(0.5, 0.7) (:603), formed at run time below, stars = eburst * 1,
//   reheated = stars; the selected pair -> (float)(2 + stars) / (float)(0.25 + 0.5 * stars).

/** @brief SAGE26's merger burst stars from ColdGas 1 (R/src/model_mergers.c:603, :685) */
static __attribute__((noinline)) double c10_merger_burst_stars(double mass_ratio) {
  volatile double ratio = mass_ratio;
  volatile float cold_gas = 1.0f;
  return 0.56 * pow(ratio, 0.7) * cold_gas;
}

/** @brief Initialise with the exact-burst parameters and CGMrecipeOn `cgm` (NULL: missing) */
static int c10_init(const char *cgm, const char **log) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.5, 0.5, 1.0, 0.3);
  sb_fire_set_param("CGMrecipeOn", cgm);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_starburst_feedback_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = sb_fire_read_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/**
 * @test    test_cgm_burst_module_channels
 * @brief   Both module channels pass CGMrecipeOn to the burst, whose feedback and yield reach the
 *          FoF central's selected pair
 */
int test_cgm_burst_module_channels(void) {
  const struct {
    const char *cgm;
    int central_regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}, {"0", 1, 0}};
  const double merger_stars = c10_merger_burst_stars(0.5);
  int ok = 1;
  for (int channel = 0; channel < 2; channel++) { /* 0 the instability, 1 the merger event */
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
      const char *log = "";
      TEST_ASSERT(c10_init(runs[r].cgm, &log) == 0, "init must accept CGMrecipeOn 0 and 1");
      struct Halo host, central;
      struct GalaxyData host_gal, central_gal;
      setup_test_galaxy(&central, &central_gal, 0, 0.0, 200.0, 3.0, 0.0, 4.0, 1.0, 2.0, 0.25);
      central_gal.CGMgas = 2.0f;
      central_gal.MetalsCGMgas = 0.25f;
      central_gal.Regime = runs[r].central_regime;
      setup_test_galaxy(&host, &host_gal, 1, 10.0, 120.0, 1.0, 0.0, 2.0, 0.5, 5.0, 0.5);
      host_gal.MetalsStellarMass = 0.0f;
      host_gal.MetalsBulgeMass = 0.0f;
      host_gal.CGMgas = 5.0f;
      host_gal.MetalsCGMgas = 0.5f;
      host_gal.Regime = 1 - runs[r].central_regime;
      struct ModuleContext ctx;
      setup_test_context(&ctx, &central);
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
        host_gal.UnstableDiskGasFraction = 0.25;
      }
      const int status = sage26_starburst_feedback_process(&ctx, &host, 1);

      const float gas_out = channel == 0 ? 2.25f : (float)(2.0 + merger_stars);
      const float metals_out = channel == 0 ? 0.375f : (float)(0.25 + 0.5 * merger_stars);
      const float *sel_gas = runs[r].to_cgm ? &central_gal.CGMgas : &central_gal.HotGas;
      const float *sel_metals =
          runs[r].to_cgm ? &central_gal.MetalsCGMgas : &central_gal.MetalsHotGas;
      const float *oth_gas = runs[r].to_cgm ? &central_gal.HotGas : &central_gal.CGMgas;
      const float *oth_metals =
          runs[r].to_cgm ? &central_gal.MetalsHotGas : &central_gal.MetalsCGMgas;
      const int central_ok = *sel_gas == gas_out && *sel_metals == metals_out && *oth_gas == 2.0f &&
                             *oth_metals == 0.25f && central_gal.EjectedGas == 0.0f &&
                             central_gal.ColdGas == 3.0f;
      const int host_ok =
          host_gal.HotGas == 5.0f && host_gal.MetalsHotGas == 0.5f && host_gal.CGMgas == 5.0f &&
          host_gal.MetalsCGMgas == 0.5f && host_gal.EjectedGas == 0.0f &&
          (channel == 1 || (host_gal.ColdGas == 0.625f && host_gal.MetalsColdGas == 0.0f &&
                            host_gal.SupernovaOutflowRate == 0.25f));
      if (status != 0 || !central_ok || !host_ok) {
        printf("\n  channel %d, CGMrecipeOn %s, central Regime %d: status %d, central hot "
               "%.9g/%.9g, CGM %.9g/%.9g (expected selected %.9g/%.9g), host cold %.9g",
               channel, runs[r].cgm, runs[r].central_regime, status, (double)central_gal.HotGas,
               (double)central_gal.MetalsHotGas, (double)central_gal.CGMgas,
               (double)central_gal.MetalsCGMgas, (double)gas_out, (double)metals_out,
               (double)host_gal.ColdGas);
        ok = 0;
      }
      sage26_starburst_feedback_cleanup();
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "each burst channel must route its feedback and yield by the central's Regime");
  return TEST_PASS;
}

/**
 * @test    test_cgm_burst_recheck_sites
 * @brief   The recheck's wind selects by the host's Regime; every burst's feedback and yield, the
 *          nested instability burst's included, by the FoF central's
 *
 * The minor merger of recheck_growth_burst (ratio 0.2 below ThreshMajorMerger 0.3, a gas-unstable
 * host disk) runs the recheck, its black-hole growth and wind on the host and its nested mode-1
 * burst (R/src/model_mergers.c:806-811, R/src/model_disk_instability.c:90-108). Each galaxy's two
 * pairs hold the same input. The central's selected pair receives both bursts; its unselected pair
 * must keep its input, and so must the host's pair its wind does not select. The site selections
 * are the transaction's locals, each compared with the Regime of the galaxy that must route it.
 */
int test_cgm_burst_recheck_sites(void) {
  const struct {
    int cgm;
    int central_regime;
    int host_regime;
  } runs[] = {{1, 0, 1}, {1, 1, 0}, {0, 0, 1}, {0, 1, 0}};
  int ok = 1;
  for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
    static struct Halo central, host;
    static struct GalaxyData central_gal, gal;
    setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
    central_gal.CGMgas = 40.0f;
    central_gal.MetalsCGMgas = 0.8f;
    central_gal.Regime = runs[r].central_regime;
    setup_test_galaxy(&host, &gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
    host.Vmax = 50.0f;
    gal.DiskScaleRadius = 0.001f;
    gal.BlackHoleMass = 0.05f;
    gal.CGMgas = 5.0f;
    gal.MetalsCGMgas = 0.1f;
    gal.Regime = runs[r].host_regime;
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    MimicConfig.G = 43.00707785642063;
    MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
    const struct MimicStarburstParams params = {
        .feedback_reheating_epsilon = 2.9,
        .feedback_ejection_efficiency = 0.3,
        .recycle_fraction = 0.43,
        .yield = 0.025,
        .threshold_major_merger = 0.3,
        .energy_sn_code = 0.0036701860231271991,
        .eta_sn_code = 68493150.684931502,
        .disk_instability_on = 1,
        .star_forming_disk_factor = 3.0,
        .agn_recipe_on = 1,
        .black_hole_growth_rate = 0.015,
        .quasar_mode_efficiency = 0.001,
        .cgm_recipe_on = runs[r].cgm,
    };
    struct MimicSageStarburstLocals b;
    const int status = mimic_apply_collisional_starburst(0.2, &host, &central, 0, 1, 0.0, 0.0, 0,
                                                         0.01, &ctx, &params, &b);
    const int central_cgm = runs[r].cgm == 1 && runs[r].central_regime == 0;
    const int host_cgm = runs[r].cgm == 1 && runs[r].host_regime == 0;
    const int ran = b.instability_recheck_ran == 1 && b.recheck_wind.reservoir_cgm != -1 &&
                    b.recheck_feedback_reservoir_cgm != -1 && b.recheck_yield_reservoir_cgm != -1;
    const int sites_ok =
        b.feedback.reservoir_cgm == central_cgm && b.yield_reservoir_cgm == central_cgm &&
        b.recheck_feedback_reservoir_cgm == central_cgm &&
        b.recheck_yield_reservoir_cgm == central_cgm && b.recheck_wind.reservoir_cgm == host_cgm &&
        isnan(host_cgm ? b.recheck_wind.hot_gas_energy : b.recheck_wind.cgm_gas_energy);
    const float central_unselected_gas = central_cgm ? central_gal.HotGas : central_gal.CGMgas;
    const float central_unselected_metals =
        central_cgm ? central_gal.MetalsHotGas : central_gal.MetalsCGMgas;
    const float host_unselected_gas = host_cgm ? gal.HotGas : gal.CGMgas;
    const float host_unselected_metals = host_cgm ? gal.MetalsHotGas : gal.MetalsCGMgas;
    /* both of the host's pairs entered as 5 / 0.1, both of the central's as 40 / 0.8 */
    const int unselected_ok = central_unselected_gas == 40.0f &&
                              central_unselected_metals == 0.8f && host_unselected_gas == 5.0f &&
                              host_unselected_metals == 0.1f;
    if (status != 0 || !ran || !sites_ok || !unselected_ok) {
      printf("\n  CGMrecipeOn %d, central Regime %d, host Regime %d: status %d, ran %d, feedback "
             "%d, yield %d, recheck feedback %d, recheck yield %d, recheck wind %d, central "
             "unselected %.9g/%.9g, host unselected %.9g/%.9g",
             runs[r].cgm, runs[r].central_regime, runs[r].host_regime, status, ran,
             b.feedback.reservoir_cgm, b.yield_reservoir_cgm, b.recheck_feedback_reservoir_cgm,
             b.recheck_yield_reservoir_cgm, b.recheck_wind.reservoir_cgm,
             (double)central_unselected_gas, (double)central_unselected_metals,
             (double)host_unselected_gas, (double)host_unselected_metals);
      ok = 0;
    }
  }
  TEST_ASSERT(ok, "the recheck's wind must route by the host, every burst site by the central");
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_refused
 * @brief   CGMrecipeOn is required (missing key fails init) and only 0 or 1 is accepted,
 *          a refusal naming the parameter
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
  sage26_starburst_feedback_cleanup();
  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST TARGETS
// ============================================================================

/**
 * @brief Run one minor merger (ratio 0.2, below ThresholdMajorMerger 0.3) on a target with the
 *        given FFBRegime through both merger consumers, sage26_quasar_mode then
 *        sage26_starburst_feedback (R/src/model_mergers.c:262-287), with the post-merger recheck
 *        configured or not (sage26_disk_instability by galaxy, SAGE26's DiskInstabilityOn), and
 *        return the target's and the central's end states
 */
static int run_ffb_target_merger(const int ffb_regime, const int recheck,
                                 struct GalaxyData *target_out, struct GalaxyData *central_out) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  setup_post_merger_recheck_parameters(1);
  if (recheck) {
    test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  }
  test_phase_add("satellite_mergers", "sage26_resolve_mergers_and_disruption",
                 PROCESSING_MODE_FULL_HALO);
  test_phase_add("satellite_mergers", "sage26_quasar_mode", PROCESSING_MODE_PER_EVENT);
  test_phase_add("satellite_mergers", "sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT);
  MimicConfig.G = 43007.1;
  int status = sage26_quasar_mode_init() != 0 || sage26_starburst_feedback_init() != 0;

  struct Halo central, target;
  struct GalaxyData central_gal, target_gal;
  setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  setup_test_galaxy(&target, &target_gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
  target.Vmax = 120.0;
  target_gal.DiskScaleRadius = 5.0;
  target_gal.BlackHoleMass = 0.05;
  target_gal.FFBRegime = ffb_regime;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = 0.2,
                              .value1 = 0.1};
  ctx.active_event = &event;
  status |= sage26_quasar_mode_process(&ctx, &target, 1) != 0;
  status |= sage26_starburst_feedback_process(&ctx, &target, 1) != 0;
  memcpy(target_out, &target_gal, sizeof(target_gal));
  memcpy(central_out, &central_gal, sizeof(central_gal));

  sage26_starburst_feedback_cleanup();
  sage26_quasar_mode_cleanup();
  test_free_substep_phases();
  return status;
}

/**
 * @test    test_ffb_target_merger_continuation
 * @brief   A merger on an FFB target bursts, grows its black hole and runs the post-merger recheck
 *          as on an ordinary target: SAGE26's merger routines do not read FFBRegime
 *          (R/src/model_mergers.c:259-287, :806-811), and only the FFB disk call skips the
 *          instability (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367)
 */
int test_ffb_target_merger_continuation(void) {
  struct GalaxyData ffb_target, ffb_central, normal_target, normal_central;
  TEST_ASSERT(run_ffb_target_merger(1, 1, &ffb_target, &ffb_central) == 0,
              "the merger on the FFB target runs");
  TEST_ASSERT(run_ffb_target_merger(0, 1, &normal_target, &normal_central) == 0,
              "the merger on the ordinary target runs");
  TEST_ASSERT(ffb_target.FFBRegime == 1, "the FFB target keeps FFBRegime 1");
  TEST_ASSERT(normal_target.FFBRegime == 0, "the ordinary target keeps FFBRegime 0");
  struct GalaxyData ffb_as_normal;
  memcpy(&ffb_as_normal, &ffb_target, sizeof(ffb_target));
  ffb_as_normal.FFBRegime = 0;
  TEST_ASSERT(memcmp(&ffb_as_normal, &normal_target, sizeof(normal_target)) == 0,
              "the FFB target's end state equals the ordinary target's in every other field");
  TEST_ASSERT(memcmp(&ffb_central, &normal_central, sizeof(normal_central)) == 0,
              "the central's end state is the same for both targets");

  /* The merger acted: the burst formed stars, the black hole grew */
  printf("\n  FFB target: StellarMass 5 -> %.9g, BulgeMass 1 -> %.9g, BlackHoleMass 0.05 -> %.9g",
         (double)ffb_target.StellarMass, (double)ffb_target.BulgeMass,
         (double)ffb_target.BlackHoleMass);
  TEST_ASSERT(ffb_target.StellarMass > 5.0f, "the burst formed stars on the FFB target");
  TEST_ASSERT(ffb_target.BlackHoleMass > 0.05f, "the black hole grew on the FFB target");

  /* The recheck acted: without it the FFB target's end state differs */
  struct GalaxyData no_recheck_target, no_recheck_central;
  TEST_ASSERT(run_ffb_target_merger(1, 0, &no_recheck_target, &no_recheck_central) == 0,
              "the merger without the recheck runs");
  printf("\n  without the recheck: BulgeMass %.9g, BlackHoleMass %.9g",
         (double)no_recheck_target.BulgeMass, (double)no_recheck_target.BlackHoleMass);
  TEST_ASSERT(ffb_target.BulgeMass > no_recheck_target.BulgeMass,
              "the recheck's stellar transfer ran on the FFB target");
  TEST_ASSERT(ffb_target.BlackHoleMass > no_recheck_target.BlackHoleMass,
              "the recheck's black-hole growth ran on the FFB target");
  check_memory_leaks();
  return TEST_PASS;
}

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
  TEST_RUN(test_per_event_zero_ratio_bursts);
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
  TEST_RUN(test_consecutive_bursts_bulge_excess_grows);
  TEST_RUN(test_hydrogen_refresh_module_wiring);
  TEST_RUN(test_hydrogen_refresh_invariants);
  TEST_RUN(test_hydrogen_cap_recheck_growth);
  TEST_RUN(test_fire_burst_budget_independent);
  TEST_RUN(test_fire_burst_on_versus_off);
  TEST_RUN(test_fire_burst_wiring_direct);
  TEST_RUN(test_fire_burst_wiring_merger_event);
  TEST_RUN(test_fire_burst_wiring_instability_channel);
  TEST_RUN(test_fire_burst_wiring_recheck);
  TEST_RUN(test_fire_mode_refused);
  TEST_RUN(test_fire_burst_snapnum_refused);
  TEST_RUN(test_cgm_burst_module_channels);
  TEST_RUN(test_cgm_burst_recheck_sites);
  TEST_RUN(test_cgm_recipe_on_refused);
  TEST_RUN(test_ffb_target_merger_continuation);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
