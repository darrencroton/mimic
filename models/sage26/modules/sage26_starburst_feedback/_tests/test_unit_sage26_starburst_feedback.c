/**
 * @file    test_unit_sage26_starburst_feedback.c
 * @brief   Unit tests for sage26_starburst_feedback module
 *
 * Validates: the collisional starburst through the module's two channels (the disk-instability
 * burst, process_by_galaxy; the merger burst and its post-minor-merger recheck,
 * process_per_event), its feedback and yield arithmetic, the guards on bad state, and the
 * bulge star-formation history, FIRE, hydrogen and CGM routing wiring.
 *
 * Expected values are hand-computed from SAGE26's expressions (or written inline in the test
 * with SAGE26's operand order), never obtained by calling the module's own helpers. Reservoirs
 * are floats, so exact-arithmetic expectations are compared with close_to(): within 1e-6 of the
 * expected value's magnitude, or an absolute 1e-6 where that magnitude is below 1; bit-pattern
 * comparisons are exact.
 *
 * Test cases:
 *   - test_disk_instability_starburst: a disk-instability burst of four galaxies (feedback
 *     renormalised, ejection positive, the minor-burst yield branch, a metal-rich galaxy):
 *     every reservoir against a hand-computed value, mass and metal conservation, bulge growth
 *     equal to stellar growth through the instability channel, the trigger left untouched
 *   - test_burst_rates_use_full_interval: the burst's StarFormationRate divides by the host's
 *     full snapshot interval for both channels, independent of the substep count and of the
 *     merger event's dt; the outflow is the undivided reheated mass
 *   - test_merger_burst_bulge_channel: the merger burst credits MergerBulgeMass for a major
 *     merger or a spheroid-dominated target and InstabilityBulgeMass for a minor merger onto a
 *     disk-dominated target
 *   - test_per_event_recheck_black_hole_gating: the post-minor-merger recheck grows the black
 *     hole only with the quasar consumer configured, AGNrecipe 2 and a ratio strictly below
 *     ThresholdMajorMerger
 *   - test_per_event_zero_ratio_bursts: a zero-ratio merger still bursts and samples the bulge
 *     history
 *   - test_process_guards: process() on bad state (ngal != 1, NULL host or central galaxy, a
 *     non-positive event dt, invalid history bins, invalid and sentinel timesteps) returns the
 *     right status and writes nothing
 *   - test_no_burst_leaves_state: no trigger, or a trigger with no cold gas, leaves the galaxy
 *     bit for bit unchanged
 *   - test_zero_vvir: a burst on a zero-Vvir host forms stars and ejects nothing
 *   - test_module_initialization: module_system_init accepts the module's placement
 *   - test_instability_burst_samples_bulge_history: the disk-instability burst samples
 *     SfrBulge[bin] += stars / dt with the galaxy's substep dt, and accumulates the gas
 *   - test_merger_burst_samples_with_event_dt: the merger burst samples at the substep's
 *     bin with the satellite's substep dt carried in the event (value1)
 *   - test_post_merger_recheck_samples_with_event_dt: the post-merger instability burst
 *     samples the same bin with the same dt
 *   - test_consecutive_bursts_bulge_excess_grows: three consecutive instability bursts
 *     leave BulgeMass two floats above StellarMass, bit for bit as SAGE26's own routine
 *     does; pins the limit of the integration test's bulge bound
 *   - test_hydrogen_refresh_module_wiring: initialised with SFprescription 1 the module wires the
 *     prescription into the burst (the commits' caps and the hydrogen refresh run), and with
 *     SFprescription 0 it does not
 *   - test_hydrogen_refresh_invariants: the refresh overwrites a stale H1gas with the atomic
 *     remainder, caps H2gas only against the double budget, floors a negative remainder to 0,
 *     keeps a -0.0 H2gas and runs for a zero-star burst
 *   - test_hydrogen_cap_recheck_growth: the post-minor-merger recheck's black-hole growth
 *     receives the burst's SFprescription (its hydrogen-cap locals)
 *   - test_fire_burst_budget_independent: FIREmodeOn 1: the burst's FIRE budget against
 *     SAGE26's expressions written in the test, across the scaling's guards, floor and slopes
 *     and both caps, MassLoading left unwritten
 *   - test_fire_burst_on_versus_off: the same burst with FIREmodeOn 1 and 0 (the fixed-epsilon
 *     budget), neither writing MassLoading; the burst reads its host's float Vvir, stored
 *     SnapNum and that snapshot's redshift, not its FoF central's
 *   - test_fire_burst_wiring_by_channel: each module channel (the merger burst, the instability
 *     burst) bursts its host with the host's own Vvir and stored SnapNum
 *   - test_fire_burst_wiring_recheck: the post-minor-merger recheck's own burst takes the FIRE
 *     branch with the host's values
 *   - test_fire_burst_snapnum_refused: a stored SnapNum of -1 or the table size is
 *     refused through a burst, naming it and the table size, and the burst writes nothing
 *   - test_cgm_burst_module_channels: CGMrecipeOn 1 against 0 on the same input, through the
 *     module's two channels: the feedback and the escaping yield reach the FoF central's CGM
 *     pair (its Regime 0) or hot pair (Regime 1, or CGMrecipeOn 0); the burst host holds the
 *     other Regime; the unselected pair and the host's own pairs unchanged
 *   - test_cgm_burst_recheck_sites: the post-minor-merger recheck's own sites: its quasar wind
 *     selects by the burst HOST's Regime, its nested burst's feedback and yield by the FoF
 *     central's, as do the outer burst's
 *   - test_ffb_target_merger_continuation: a minor merger on a feedback-free-burst
 *     target (FFBRegime 1) grows its black hole (the quasar consumer), bursts and runs the
 *     post-merger instability recheck exactly as on the same target with FFBRegime 0: the two end
 *     states are bytewise equal but for FFBRegime, which each target keeps; the recheck is shown
 *     to act (bulge, black hole, accretion and cold gas differ from the run without it) and to
 *     conserve the baryons of the target and its central
 *
 * The module's AGNrecipe, FIREmodeOn and CGMrecipeOn refusals are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
 *
 * In the three hydrogen tests the remainder is formed with C's correctly rounded fma, as the
 * shared helper forms it. In the FIRE tests each expectation is SAGE26's expression written in
 * the test with its operand order and literal constants (sb_fire_expected), or the fixture's
 * input, never a call of the package's FIRE helpers.
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

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
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
 * The stellar and bulge metals carry the cold gas's metallicity (zero for a galaxy with no
 * cold gas).
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

  const double metallicity = cold_gas > 0.0 ? metals_cold / cold_gas : 0.0;
  gal->ColdGas = (float)cold_gas;
  gal->MetalsColdGas = (float)metals_cold;
  gal->StellarMass = (float)stellar_mass;
  gal->MetalsStellarMass = (float)(metallicity * stellar_mass);
  gal->BulgeMass = (float)bulge_mass;
  gal->MetalsBulgeMass = (float)(metallicity * bulge_mass);
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

  /* The required bulge-size mode: BulgeSizeOn 0 (no sizes) */
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "BulgeSizeOn");
  snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%d", 0);

  MimicConfig.NumModelParams = idx;
  /* The module's required SFprescription and StarburstColdGasOn */
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
    sage26_test_append_model_parameter("AGNrecipe", "2"); /* 0 or 2, read with the quasar */
  }
}

/** @brief Set the parameter list's AGNrecipe (the recheck parameters add it) to `value` */
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

/**
 * @brief Assert `cond`, naming the table row that failed in the failure message
 */
#define ROW_ASSERT(cond, row, what)                                                                \
  do {                                                                                             \
    char row_msg[192];                                                                             \
    snprintf(row_msg, sizeof(row_msg), "%s: %s", (row), (what));                                   \
    TEST_ASSERT((cond), row_msg);                                                                  \
  } while (0)

/**
 * @brief Whether a float reservoir equals its exact-arithmetic expectation
 *
 * The module narrows every reservoir to float (one part in 1.7e7), so a hand-computed expectation
 * is matched to one part in 1e6 of its value, and to 1e-6 absolute for a value below 1.
 */
static int close_to(const double actual, const double expected) {
  return fabs(actual - expected) <= 1e-6 * fmax(1.0, fabs(expected));
}

/** @brief A merger event from SAGE26's resolver with the given mass ratio and substep dt */
static struct ModuleEvent merger_event(const double mass_ratio, const double dt) {
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE26_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = mass_ratio,
                              .value1 = dt};
  return event;
}

/** @brief Set or replace a model parameter's value; NULL removes it */
static void set_model_param(const char *name, const char *value) {
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

// ============================================================================
// BULGE STAR-FORMATION HISTORY ACCESS
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

// ============================================================================
// THE MINOR MERGER SCENARIO
// ============================================================================

/**
 * What one minor-merger run configures: which consumers sit in the pipeline (the module reads
 * the pipeline at init to decide whether the post-merger recheck and its black-hole growth are
 * enabled) and the merger event it processes.
 */
struct minor_merger {
  int disk_instability;   /**< sage26_disk_instability by galaxy: the post-merger recheck */
  int quasar;             /**< sage26_quasar_mode per event: the recheck grows the black hole */
  int quasar_consumer;    /**< also run the quasar module's own merger event, before the burst */
  const char *agn_recipe; /**< AGNrecipe of the quasar parameters, NULL for the default 2 */
  double ratio;           /**< the event's mass ratio (value0) */
  double event_dt;        /**< the event's substep dt (value1) */
  int ffb_regime;         /**< the target's FFBRegime */
  int num_substeps;       /**< the context's substep count */
  int substep_number;     /**< the context's substep */
};

/** The default scenario: no consumers, a ratio of 0.2 (below ThresholdMajorMerger 0.3), one substep
 */
static const struct minor_merger MINOR_MERGER_DEFAULT = {
    .ratio = 0.2, .event_dt = 0.1, .num_substeps = 1};

/** The merger target's input state: a satellite whose disk is stable */
#define MINOR_TARGET_COLD 8.0
#define MINOR_TARGET_STELLAR 5.0
#define MINOR_TARGET_HOT 5.0
#define MINOR_TARGET_BLACK_HOLE 0.05
#define MINOR_CENTRAL_HOT 40.0

/**
 * @brief Run one merger event through sage26_starburst_feedback on a satellite target (cold gas
 *        8, stellar mass 5, hot gas 5, black hole 0.05) and its FoF central (hot gas 40)
 *
 * The module is initialised with the pipeline `cfg` describes and configured with
 * FeedbackReheatingEpsilon 1, no ejection, FracZleaveDisk 0 and ThresholdMajorMerger 0.3; the run
 * allocates through the caller's memory system.
 *
 * @param cfg          The scenario
 * @param target_out   Receives the target's end state
 * @param central_out  Receives the central's end state, may be NULL
 * @return 0 on success; non-zero if an init or a process call failed
 */
static int run_minor_merger(const struct minor_merger *cfg, struct GalaxyData *target_out,
                            struct GalaxyData *central_out) {
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  if (cfg->disk_instability || cfg->quasar) {
    setup_post_merger_recheck_parameters(cfg->quasar);
    if (cfg->agn_recipe != NULL) {
      set_agn_recipe(cfg->agn_recipe);
    }
  }
  if (cfg->disk_instability) {
    test_phase_add("galaxy_physics", "sage26_disk_instability", PROCESSING_MODE_BY_GALAXY);
  }
  if (cfg->quasar_consumer) {
    test_phase_add("satellite_mergers", "sage26_resolve_mergers_and_disruption",
                   PROCESSING_MODE_FULL_HALO);
  }
  if (cfg->quasar) {
    test_phase_add("satellite_mergers", "sage26_quasar_mode", PROCESSING_MODE_PER_EVENT);
  }
  if (cfg->quasar_consumer) {
    test_phase_add("satellite_mergers", "sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT);
  }
  MimicConfig.G = 43007.1;
  int status = (cfg->quasar_consumer && sage26_quasar_mode_init() != 0) ||
               sage26_starburst_feedback_init() != 0;

  struct Halo central, target;
  struct GalaxyData central_gal, target_gal;
  setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, MINOR_CENTRAL_HOT,
                    0.8);
  setup_test_galaxy(&target, &target_gal, 1, 30.0, 140.0, MINOR_TARGET_COLD, 0.16,
                    MINOR_TARGET_STELLAR, 1.0, MINOR_TARGET_HOT, 0.1);
  target.Vmax = 120.0;
  target_gal.DiskScaleRadius = 5.0;
  target_gal.BlackHoleMass = MINOR_TARGET_BLACK_HOLE;
  target_gal.FFBRegime = cfg->ffb_regime;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  ctx.num_substeps = cfg->num_substeps;
  ctx.substep_number = cfg->substep_number;
  struct ModuleEvent event = merger_event(cfg->ratio, cfg->event_dt);
  ctx.active_event = &event;
  if (status == 0 && cfg->quasar_consumer) {
    status = sage26_quasar_mode_process(&ctx, &target, 1) != 0;
  }
  if (status == 0) {
    status = sage26_starburst_feedback_process(&ctx, &target, 1) != 0;
  }
  *target_out = target_gal;
  if (central_out != NULL) {
    *central_out = central_gal;
  }

  sage26_starburst_feedback_cleanup();
  if (cfg->quasar_consumer) {
    sage26_quasar_mode_cleanup();
  }
  test_free_substep_phases();
  return status;
}

// ============================================================================
// BURST ARITHMETIC, RATES AND BULGE CHANNEL
// ============================================================================

/**
 * @test    test_disk_instability_starburst
 * @brief   A disk-instability burst (mode 1: eburst is the unstable fraction itself) of a central
 *          with ColdGas 10, Mvir 100, StellarMass 5, BulgeMass 1, HotGas 50 and MetalsHotGas 1,
 *          at FeedbackReheatingEpsilon 3, FeedbackEjectionEfficiency 0.5, RecycleFraction 0.43,
 *          Yield 0.03, FracZleaveDisk 0.5 and ThresholdMajorMerger 0.3
 *
 * Every expectation is worked by hand from SAGE26's recipe. With a trigger of 0.5, stars would be
 * 5 and the reheated mass 15, more than the 10 of cold gas, so both scale by 10 / 20: stars 2.5,
 * reheated 7.5. The cold gas, the stellar mass and the bulge lose or gain 0.57 * 2.5 = 1.425 (the
 * recycled fraction stays in the cold gas), the reheated gas goes to the central's hot gas with
 * the cold gas's metallicity, and a burst whose fraction is not below ThresholdMajorMerger sends
 * all of its yield, 0.03 * stars of metals, to the hot gas. A SAGE26 energy product
 * EtaSN * EnergySN of 251382.604 (in the test's units) makes the ejection
 * (0.5 * 251382.6 / Vvir^2 - 3) * stars, negative and so floored to 0 at Vvir 300.
 * Rows:
 *   - Vvir 300, trigger 0.5: renormalised, no ejection
 *   - Vvir 100, trigger 0.5: the ejection (0.5 * 251382.6 / 1e4 - 3) * 2.5 = 23.9228 leaves the
 *     central's hot gas with its metallicity 1.15 / 57.5
 *   - Vvir 300, trigger 0.2: below the threshold, stars 2 and reheated 6 need no renormalisation,
 *     and the yield takes the minor branch: FracZleaveDisk * exp(-Mvir / 30) = 0.0178371 of it
 *     escapes to the hot gas and the rest enriches the cold gas
 *   - Vvir 300, trigger 0.3, Z = 0.05: the metals scale with the cold gas's metallicity
 *
 * Each row must also conserve mass and metals, grow the bulge by what the stellar mass gains,
 * credit the instability channel only and leave the trigger untouched.
 */
int test_disk_instability_starburst(void) {
  const struct {
    const char *name;
    double vvir;
    double metals_cold; /* input */
    double trigger;
    double cold, metals_cold_out, stellar, metals_stellar, bulge, metals_bulge, hot, metals_hot,
        ejected, metals_ejected, outflow, sfr, instability_bulge; /* expected end state */
  } rows[] = {
      {"Vvir 300, renormalised, no ejection", 300.0, 0.2, 0.5, 1.075, 0.0215, 6.425, 0.1285, 2.425,
       0.0485, 57.5, 1.225, 0.0, 0.0, 7.5, 25.0, 1.425},
      {"Vvir 100, ejection", 100.0, 0.2, 0.5, 1.075, 0.0215, 6.425, 0.1285, 2.425, 0.0485,
       33.57717446, 0.746543489, 23.92282554, 0.478456511, 7.5, 25.0, 1.425},
      {"trigger 0.2, minor yield branch", 300.0, 0.2, 0.2, 2.86, 0.11612978, 6.14, 0.1228, 2.14,
       0.0428, 56.0, 1.12107022, 0.0, 0.0, 6.0, 20.0, 1.14},
      {"Z 0.05, trigger 0.3", 300.0, 0.5, 0.3, 1.075, 0.05375, 6.425, 0.32125, 2.425, 0.12125, 57.5,
       1.45, 0.0, 0.0, 7.5, 25.0, 1.425},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    ROW_ASSERT(sage26_starburst_feedback_init() == 0, name, "Module init should succeed");

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, rows[k].vvir, 10.0, rows[k].metals_cold, 5.0, 1.0,
                      50.0, 1.0);
    gal.UnstableDiskGasFraction = rows[k].trigger;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);

    const double mass_before = gal.ColdGas + gal.StellarMass + gal.HotGas + gal.EjectedGas;
    const double metals_before = gal.MetalsColdGas + gal.MetalsStellarMass + gal.MetalsHotGas;
    const double stellar_before = gal.StellarMass;
    const double bulge_before = gal.BulgeMass;

    ROW_ASSERT(sage26_starburst_feedback_process(&ctx, &halo, 1) == 0, name,
               "Process function should succeed");

    ROW_ASSERT(close_to(gal.ColdGas, rows[k].cold), name, "ColdGas");
    ROW_ASSERT(close_to(gal.MetalsColdGas, rows[k].metals_cold_out), name, "MetalsColdGas");
    ROW_ASSERT(close_to(gal.StellarMass, rows[k].stellar), name, "StellarMass");
    ROW_ASSERT(close_to(gal.MetalsStellarMass, rows[k].metals_stellar), name, "MetalsStellarMass");
    ROW_ASSERT(close_to(gal.BulgeMass, rows[k].bulge), name, "BulgeMass");
    ROW_ASSERT(close_to(gal.MetalsBulgeMass, rows[k].metals_bulge), name, "MetalsBulgeMass");
    ROW_ASSERT(close_to(gal.HotGas, rows[k].hot), name, "HotGas");
    ROW_ASSERT(close_to(gal.MetalsHotGas, rows[k].metals_hot), name, "MetalsHotGas");
    ROW_ASSERT(close_to(gal.EjectedGas, rows[k].ejected), name, "EjectedGas");
    ROW_ASSERT(close_to(gal.MetalsEjectedGas, rows[k].metals_ejected), name, "MetalsEjectedGas");
    ROW_ASSERT(close_to(gal.SupernovaOutflowRate, rows[k].outflow), name,
               "SupernovaOutflowRate is the undivided reheated mass");
    ROW_ASSERT(close_to(gal.StarFormationRate, rows[k].sfr), name,
               "StarFormationRate is stars / dT");
    ROW_ASSERT(close_to(gal.InstabilityBulgeMass, rows[k].instability_bulge), name,
               "InstabilityBulgeMass");
    ROW_ASSERT(gal.MergerBulgeMass == 0.0f, name,
               "A disk-instability burst credits no merger bulge");

    /* Conservation, independently of the expected end state */
    ROW_ASSERT(close_to(gal.ColdGas + gal.StellarMass + gal.HotGas + gal.EjectedGas, mass_before),
               name, "Total mass should be conserved");
    ROW_ASSERT(close_to(gal.MetalsColdGas + gal.MetalsStellarMass + gal.MetalsHotGas +
                            gal.MetalsEjectedGas,
                        metals_before + 0.03 * (gal.StellarMass - stellar_before) / 0.57),
               name, "Metals should be conserved apart from the yield of the stars formed");
    ROW_ASSERT(fabs((gal.BulgeMass - bulge_before) - (gal.StellarMass - stellar_before)) <= 1e-6,
               name, "Bulge growth should equal stellar mass growth");

    /* Trigger lifecycle is owned by the instability modules, not the burst */
    ROW_ASSERT(gal.UnstableDiskGasFraction == rows[k].trigger, name,
               "Disk-instability trigger should remain unchanged");

    sage26_starburst_feedback_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief One burst of the given channel, returning the host's end state
 *
 * The instability channel bursts a central whose UnstableDiskGasFraction is 0.5 over a halo
 * interval `host_dt` of `num_substeps` substeps. The merger channel bursts a satellite target
 * (halo interval `host_dt`) onto its central (halo interval `central_dt`) with an event of ratio
 * 0.3 and substep dt `event_dt`.
 */
static void run_rate_burst(const int merger, const double host_dt, const int num_substeps,
                           const double event_dt, const double central_dt,
                           struct GalaxyData *host_out) {
  reset_config();
  setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
  sage26_starburst_feedback_init();
  struct Halo central, host;
  struct GalaxyData central_gal, host_gal;
  struct ModuleContext ctx;
  struct ModuleEvent event = merger_event(0.3, event_dt);
  if (merger) {
    setup_test_galaxy(&central, &central_gal, 0, 120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
    central.dT = central_dt;
    setup_test_galaxy(&host, &host_gal, 1, 25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);
    setup_test_context(&ctx, &central);
    ctx.active_event = &event;
  } else {
    setup_test_galaxy(&host, &host_gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    host_gal.UnstableDiskGasFraction = 0.5;
    setup_test_context(&ctx, &host);
  }
  host.dT = host_dt;
  ctx.num_substeps = num_substeps;
  sage26_starburst_feedback_process(&ctx, &host, 1);
  *host_out = host_gal;
  sage26_starburst_feedback_cleanup();
}

/**
 * @test    test_burst_rates_use_full_interval
 * @brief   SAGE parity: both channels' bursts normalise StarFormationRate by the host's FULL
 *          snapshot interval (halo->dT), like the normal disk star-formation channel, so the
 *          reported rate is the snapshot mean: stars / dT, invariant to the substep count and
 *          independent of the merger event's substep dt (value1)
 *
 * The outflow is SAGE26's undivided OutflowRate += reheated mass (normalised once per halo by
 * sage26_finalize_cooling_heating), so it involves no interval at all: it is bit-identical
 * between two runs that differ only in their intervals.
 *
 * Rows: the instability burst (stars 2.5, reheated 7.5 after the renormalisation) at dT 0.2 with
 * one substep and with two; the merger burst of ratio 0.3 on a target with ColdGas 8 (stars
 * 0.56 * 0.3^0.7 * 8, reheated 3 * stars, no renormalisation) with target dT 0.9, event dt 0.1
 * against target dT 0.2, event dt 0.2 (and different central intervals).
 */
int test_burst_rates_use_full_interval(void) {
  const double merger_stars = 0.56 * pow(0.3, 0.7) * 8.0;
  const struct {
    const char *name;
    int merger;
    double stars, reheated;
    /* the two runs: host dT, substeps, event dt, central dT */
    double dt_a, event_dt_a, central_dt_a, dt_b, event_dt_b, central_dt_b;
    int substeps_a, substeps_b;
  } rows[] = {
      {"instability burst, 1 substep against 2", 0, 2.5, 7.5, 0.2, 0.0, 0.0, 0.2, 0.0, 0.0, 1, 2},
      {"merger burst, target dT 0.9 against 0.2", 1, merger_stars, 3.0 * merger_stars, 0.9, 0.1,
       0.05, 0.2, 0.2, 0.8, 1, 1},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    struct GalaxyData a, b;
    run_rate_burst(rows[k].merger, rows[k].dt_a, rows[k].substeps_a, rows[k].event_dt_a,
                   rows[k].central_dt_a, &a);
    run_rate_burst(rows[k].merger, rows[k].dt_b, rows[k].substeps_b, rows[k].event_dt_b,
                   rows[k].central_dt_b, &b);

    ROW_ASSERT(close_to(a.StarFormationRate, rows[k].stars / rows[k].dt_a), name,
               "StarFormationRate is stars / the full interval (first run)");
    ROW_ASSERT(close_to(b.StarFormationRate, rows[k].stars / rows[k].dt_b), name,
               "StarFormationRate is stars / the full interval (second run)");
    ROW_ASSERT(close_to(a.SupernovaOutflowRate, rows[k].reheated), name,
               "The outflow is the reheated mass");
    ROW_ASSERT(a.SupernovaOutflowRate == b.SupernovaOutflowRate, name,
               "The outflow is the undivided reheated mass, independent of any interval");
    ROW_ASSERT(a.StellarMass == b.StellarMass && a.ColdGas == b.ColdGas, name,
               "The masses are independent of the intervals");
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_merger_burst_bulge_channel
 * @brief   SAGE parity: the merger burst credits the locked stars to MergerBulgeMass for a major
 *          merger (ratio above ThresholdMajorMerger) or a spheroid-dominated target, and to
 *          InstabilityBulgeMass for a minor merger onto a disk-dominated target, the morphology
 *          the resolver carried with the event in MergerDiskDominated
 *
 * One credited channel gets (1 - RecycleFraction) of the burst's stars, 0.57 * 0.56 * ratio^0.7 *
 * ColdGas 10 (no renormalisation at FeedbackReheatingEpsilon 1); the other stays exactly 0. A
 * ratio equal to the threshold is not major.
 */
int test_merger_burst_bulge_channel(void) {
  const struct {
    const char *name;
    double ratio;
    int disk_dominated;
    int merger_channel;
  } rows[] = {
      {"major merger, disk-dominated target", 0.5, 1, 1},
      {"major merger, spheroid-dominated target", 0.5, 0, 1},
      {"minor merger, disk-dominated target", 0.2, 1, 0},
      {"minor merger, spheroid-dominated target", 0.2, 0, 1},
      {"ratio at the threshold, disk-dominated target", 0.3, 1, 0},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    reset_config();
    setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.5, 0.3);
    sage26_starburst_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
    gal.MergerDiskDominated = rows[k].disk_dominated;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);
    struct ModuleEvent event = merger_event(rows[k].ratio, 0.1);
    ctx.active_event = &event;

    ROW_ASSERT(sage26_starburst_feedback_process(&ctx, &halo, 1) == 0, name,
               "The merger burst should succeed");
    const double locked = (1.0 - 0.43) * 0.56 * pow(rows[k].ratio, 0.7) * 10.0;
    const float credited = rows[k].merger_channel ? gal.MergerBulgeMass : gal.InstabilityBulgeMass;
    const float other = rows[k].merger_channel ? gal.InstabilityBulgeMass : gal.MergerBulgeMass;
    ROW_ASSERT(close_to(credited, locked), name, "The selected channel gets the locked stars");
    ROW_ASSERT(other == 0.0f, name, "The other channel gets nothing");
    ROW_ASSERT(close_to(gal.BulgeMass, 1.0 + locked), name, "BulgeMass gets the locked stars");
    sage26_starburst_feedback_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// THE POST-MINOR-MERGER RECHECK
// ============================================================================

/**
 * @test    test_per_event_recheck_black_hole_gating
 * @brief   The post-minor-merger disk-instability recheck grows the black hole only when the
 *          quasar event consumer is configured, AGNrecipe is 2 (SAGE26's AGNrecipeOn 0 skips
 *          grow_black_hole in check_disk_instability) and the merger is minor (a ratio strictly
 *          below ThresholdMajorMerger: a major merger, and a ratio equal to the threshold, run no
 *          recheck)
 *
 * The scenario is a minor-merger target of black hole 0.05 with sage26_disk_instability
 * configured; the merger burst forms stars in every row, grown black hole or not.
 */
int test_per_event_recheck_black_hole_gating(void) {
  const struct {
    const char *name;
    int quasar;
    const char *agn_recipe;
    double ratio;
    int grows;
  } rows[] = {
      {"no quasar event consumer", 0, "2", 0.2, 0},
      {"quasar consumer, AGNrecipe 0", 1, "0", 0.2, 0},
      {"quasar consumer, AGNrecipe 2", 1, "2", 0.2, 1},
      {"major merger runs no recheck", 1, "2", 0.5, 0},
      {"ratio at the threshold runs no recheck", 1, "2", 0.3, 0},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    struct minor_merger cfg = MINOR_MERGER_DEFAULT;
    cfg.disk_instability = 1;
    cfg.quasar = rows[k].quasar;
    cfg.agn_recipe = rows[k].agn_recipe;
    cfg.ratio = rows[k].ratio;
    struct GalaxyData target;
    ROW_ASSERT(run_minor_merger(&cfg, &target, NULL) == 0, name, "init and process succeed");
    ROW_ASSERT(target.StellarMass > (float)MINOR_TARGET_STELLAR, name,
               "The merger burst forms stars");
    if (rows[k].grows) {
      ROW_ASSERT(target.BlackHoleMass > (float)MINOR_TARGET_BLACK_HOLE, name,
                 "The recheck grows the black hole");
      ROW_ASSERT(target.QuasarModeBHaccretionMass > 0.0f, name, "The recheck records accretion");
    } else {
      ROW_ASSERT(fabs(target.BlackHoleMass - MINOR_TARGET_BLACK_HOLE) <= 1e-6, name,
                 "The black hole is unchanged");
      ROW_ASSERT(target.QuasarModeBHaccretionMass == 0.0f, name, "No accretion is recorded");
    }
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_per_event_zero_ratio_bursts
 * @brief   A zero mass ratio merger event still runs SAGE26's burst
 *
 * SAGE26 bursts on every merger, a ratio of 0 included: the burst forms no stars
 * (eburst = 0.56 * 0^0.7 = 0), but still samples the host's cold gas and metals into its bulge
 * history.
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
              "A zero-ratio burst still samples ColdGas into SfrBulgeColdGas");
  TEST_ASSERT(*bulge_metals == expected_bulge_metals,
              "A zero-ratio burst still samples MetalsColdGas into SfrBulgeColdGasMetals");

  /* ===== CLEANUP ===== */
  sage26_starburst_feedback_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// GUARDS ON BAD STATE
// ============================================================================

/**
 * @test    test_process_guards
 * @brief   process() on bad state returns the right status and writes nothing: an ngal other than 1
 *          (-1), a NULL host or FoF-central galaxy (a silent 0), a merger event whose substep dt
 *          (value1) is not positive (-1), a substep outside the history's bins (-1), an invalid
 *          non-boundary dT (-1) and the boundary sentinel dT (a 0 no-op)
 *
 * Every row runs a satellite host with ColdGas 10 and a trigger of 0.5 (or a merger event of ratio
 * 0.3) beside its central; each error row must log its own message, which tells the guards that
 * return the same status apart, and the galaxies it was handed must come back bit for bit
 * unchanged.
 */
int test_process_guards(void) {
  const struct {
    const char *name;
    int event;           /* process_per_event (a merger event is active), else process_by_galaxy */
    int ngal;            /* the ngal argument */
    int null_host;       /* the host has no galaxy */
    int null_central;    /* the FoF central has no galaxy */
    int snapnum;         /* the host's stored SnapNum */
    double dt;           /* the host's dT */
    double event_dt;     /* the event's substep dt (value1) */
    int substep_number;  /* the context's substep, of one */
    int expected;        /* process()'s status */
    const char *message; /* the error the guard logs, NULL for a silent return */
  } rows[] = {
      {"by-galaxy: ngal 2", 0, 2, 0, 0, 63, 0.1, 0.0, 0, -1, "expects ngal=1"},
      {"by-galaxy: NULL host galaxy", 0, 1, 1, 0, 63, 0.1, 0.0, 0, 0, NULL},
      {"by-galaxy: NULL central galaxy", 0, 1, 0, 1, 63, 0.1, 0.0, 0, 0, NULL},
      {"by-galaxy: invalid non-boundary dT", 0, 1, 0, 0, 63, -1.0, 0.0, 0, -1,
       "Invalid disk-instability dt"},
      {"by-galaxy: boundary sentinel dT", 0, 1, 0, 0, -1, -1.0, 0.0, 0, 0, NULL},
      {"by-galaxy: substep outside the history", 0, 1, 0, 0, 63, 0.1, 0.0, 1, -1,
       "Invalid disk-instability SFR history bin"},
      {"event: ngal 2", 1, 2, 0, 0, 63, 0.1, 0.1, 0, -1, "(process_per_event) expects ngal=1"},
      {"event: NULL host galaxy", 1, 1, 1, 0, 63, 0.1, 0.1, 0, 0, NULL},
      {"event: NULL central galaxy", 1, 1, 0, 1, 63, 0.1, 0.1, 0, 0, NULL},
      {"event: zero substep dt", 1, 1, 0, 0, 63, 0.1, 0.0, 0, -1, "missing valid source dt"},
      {"event: negative substep dt", 1, 1, 0, 0, 63, 0.1, -0.1, 0, -1, "missing valid source dt"},
      {"event: substep outside the history", 1, 1, 0, 0, 63, 0.1, 0.1, 1, -1,
       "Invalid merger SFR history bin"},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage26_starburst_feedback_init();

    struct Halo central, halos[2];
    struct GalaxyData central_gal, gals[2];
    setup_test_galaxy(&central, &central_gal, 0, 120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
    setup_test_galaxy(&halos[0], &gals[0], 1, 50.0, 200.0, 10.0, 0.2, 2.0, 0.5, 10.0, 0.2);
    setup_test_galaxy(&halos[1], &gals[1], 1, 50.0, 200.0, 5.0, 0.1, 2.0, 0.5, 10.0, 0.2);
    halos[0].SnapNum = rows[k].snapnum;
    halos[0].dT = rows[k].dt;
    gals[0].UnstableDiskGasFraction = 0.5;
    if (rows[k].null_host) {
      halos[0].galaxy = NULL;
    }
    if (rows[k].null_central) {
      central.galaxy = NULL;
    }
    const struct GalaxyData host_before = gals[0], second_before = gals[1],
                            central_before = central_gal;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &central);
    ctx.substep_number = rows[k].substep_number;
    struct ModuleEvent event = merger_event(0.3, rows[k].event_dt);
    if (rows[k].event) {
      ctx.active_event = &event;
    }

    FILE *capture = tmpfile();
    FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
    const int result = sage26_starburst_feedback_process(&ctx, halos, rows[k].ngal);
    const char *log = "";
    if (capture != NULL) {
      set_log_output(old_out);
      log = read_captured_log(capture);
    }
    ROW_ASSERT(result == rows[k].expected, name, "process() returns the guard's status");
    ROW_ASSERT(rows[k].message == NULL || strstr(log, rows[k].message) != NULL, name,
               "The error names its own guard");
    ROW_ASSERT(memcmp(&host_before, &gals[0], sizeof(host_before)) == 0 &&
                   memcmp(&second_before, &gals[1], sizeof(second_before)) == 0 &&
                   memcmp(&central_before, &central_gal, sizeof(central_before)) == 0,
               name, "The guard writes nothing");
    if (capture != NULL) {
      fclose(capture);
    }
    sage26_starburst_feedback_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_no_burst_leaves_state
 * @brief   A galaxy with no trigger, and one with a trigger but no cold gas, come out of
 *          process() bit for bit unchanged (no star formation, no feedback, no yield, no bulge
 *          history sample), with finite metals
 *
 * The zero-cold-gas galaxy still passes the burst's early guards, so every assignment of the
 * burst runs on zero masses: the metals of a galaxy with no cold gas must stay 0, not become
 * 0 / 0.
 */
int test_no_burst_leaves_state(void) {
  const struct {
    const char *name;
    double cold_gas;
    double metals_cold;
    double trigger;
  } rows[] = {
      {"no trigger", 10.0, 0.2, 0.0},
      {"a trigger with no cold gas", 0.0, 0.0, 0.5},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage26_starburst_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, rows[k].cold_gas, rows[k].metals_cold, 5.0, 1.0,
                      50.0, 1.0);
    gal.UnstableDiskGasFraction = rows[k].trigger;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);
    const struct GalaxyData before = gal;

    ROW_ASSERT(sage26_starburst_feedback_process(&ctx, &halo, 1) == 0, name,
               "Process function should succeed");
    ROW_ASSERT(memcmp(&before, &gal, sizeof(before)) == 0, name, "The galaxy is unchanged");
    ROW_ASSERT(bulge_history_zero_outside(&gal, -1), name, "No bulge history bin is sampled");
    ROW_ASSERT(isfinite(gal.MetalsStellarMass) && isfinite(gal.MetalsBulgeMass) &&
                   isfinite(gal.MetalsHotGas) && isfinite(gal.MetalsEjectedGas),
               name, "The metals stay finite");

    sage26_starburst_feedback_cleanup();
  }
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
  /* The module's required SFprescription and StarburstColdGasOn */
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

// ============================================================================
// BULGE STAR-FORMATION HISTORY SAMPLING
// ============================================================================

/**
 * @test    test_instability_burst_samples_bulge_history
 * @brief   SfrBulge[bin] += stars / dt, SfrBulgeColdGas[bin] += ColdGas,
 *          SfrBulgeColdGasMetals[bin] += MetalsColdGas
 *
 * With 13 substeps, substep 5 maps to bin 3 ((5 * 10) / 13). The disk-instability
 * burst's dt is the galaxy's substep interval, dT / 13 (SAGE26's starformation_and_feedback dt
 * handed to collisional_starburst_recipe). The burst mass is formed here with the kernel's
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
 *          satellite's substep interval, which the
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
 * @test    test_post_merger_recheck_samples_with_event_dt
 * @brief   The post-merger instability burst samples the merger's bin with the merger's
 *          dt, as SAGE26's deal_with_galaxy_merger passes its dt and step to
 *          check_disk_instability and on to the burst
 *
 * A minor merger (the scenario of run_minor_merger, substep 4 of 10) with the recheck bursts
 * twice: the bin holds more rate and more sampled gas than the merger burst alone. Halving the
 * event dt doubles the bin's rate exactly, so both bursts divide by it.
 */
int test_post_merger_recheck_samples_with_event_dt(void) {
  init_memory_system(0);
  struct minor_merger cfg = MINOR_MERGER_DEFAULT;
  cfg.num_substeps = 10;
  cfg.substep_number = 4;
  struct GalaxyData merger_only, recheck, recheck_half;
  TEST_ASSERT(run_minor_merger(&cfg, &merger_only, NULL) == 0, "merger-only run should succeed");
  cfg.disk_instability = 1;
  TEST_ASSERT(run_minor_merger(&cfg, &recheck, NULL) == 0, "recheck run should succeed");
  cfg.event_dt = 0.05;
  TEST_ASSERT(run_minor_merger(&cfg, &recheck_half, NULL) == 0, "recheck run should succeed");

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

/**
 * @test    test_consecutive_bursts_bulge_excess_grows
 * @brief   Consecutive bursts can leave BulgeMass more than one float above StellarMass
 *
 * A burst adds the rounded double recycled_stars to BulgeMass while StellarMass takes the same
 * product through a differently rounded update, so each burst can move BulgeMass - StellarMass
 * by one float, and the excess can accumulate over consecutive bursts. SAGE26's own
 * collisional_starburst_recipe gives it with SAGE26's default parameters: ColdGas 1,
 * StellarMass = BulgeMass = 0x3f800001, three mode-1 bursts, each fed the previous output; the
 * expected bits below were produced by executing that routine. The shared burst transaction must
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

  /* SAGE26's default parameters, as exact bit patterns */
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
    TEST_ASSERT(stellar_bits == expected_stellar[k],
                "StellarMass must match SAGE26's own routine bit for bit");
    TEST_ASSERT(bulge_bits == expected_bulge[k],
                "BulgeMass must match SAGE26's own routine bit for bit");
  }
  TEST_ASSERT(gal.BulgeMass > nextafterf(gal.StellarMass, INFINITY),
              "three bursts leave BulgeMass two floats above StellarMass");
  return TEST_PASS;
}

/**
 * @brief The refresh's expected H1gas for a galaxy's final ColdGas and H2gas
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
 * @brief   The burst's hydrogen refresh is not a pair of caps
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
   * the float bound binds the refresh's cap (storing the same float), and the remainder
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
// FIREmodeOn: THE BURST'S FIRE BUDGET
// ============================================================================

/* The burst parameters the FIRE tests use: FeedbackReheatingEpsilon 2.9 and
 * FeedbackEjectionEfficiency 0.3, with SAGE26's EtaSNcode and EnergySNcode bit patterns */
#define FIRE_TEST_EPS 2.9
#define FIRE_TEST_EJ 0.3
#define FIRE_TEST_ETA_SN 68493150.684931502
#define FIRE_TEST_ENERGY_SN 0.0036701860231271991
#define FIRE_TEST_ESN (FIRE_TEST_ETA_SN * FIRE_TEST_ENERGY_SN)

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
 *        (including SAGE26's capped_eta_reheat and sn_energy_coupling): an expectation that calls
 *        no package helper
 *
 * SAGE parity: the high-velocity term is the division 1.0 / x, not pow(x, -1.0).
 *
 * @param vvir      The host's float Vvir
 * @param z         The redshift of the host's stored snapshot
 * @param stars_in  The stars before the renormalisation (eburst * gas, floored)
 * @param gas       The burst gas
 * @param esn       The product EtaSNcode * EnergySNcode the budget reads
 */
static struct sb_fire_budget sb_fire_expected(const float vvir, const double z,
                                              const double stars_in, const double gas,
                                              const double esn) {
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
  if (ejected < 0.0) {
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
 *          scaling's guards, floor and slopes and both caps
 *
 * A mode-1 burst (fraction 0.1) of one galaxy with ColdGas 1 and ample hot gas, at a host Vvir
 * below, at and above 1 and 60 km/s, zero and negative, and a stored-snapshot redshift of 0, 2.35,
 * 14.09 (both caps binding at 100 km/s) and -0.5, and at 30 km/s (the coupling capped at 2 while
 * the loading is not, so the cap decides the ejection): the transaction's stars, reheated and
 * ejected mass and the FIRE scaling must equal the inline expectation bit for bit, and MassLoading
 * must keep its stale value (no burst writes it).
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
    const struct sb_fire_budget e = sb_fire_expected(cases[k].vvir, cases[k].z, 0.1 * (double)1.0f,
                                                     (double)1.0f, FIRE_TEST_ESN);
    printf("\n  Vvir %.9g z %.4g: scaling %.17g reheated %.17g ejected %.17g", cases[k].vvir,
           cases[k].z, b.fire.l.scaling, b.reheated_mass, b.ejected_mass);
    TEST_ASSERT(status == 0, "the burst must run");
    TEST_ASSERT(b.fire.fire_mode == 1 && b.fire.snapnum == 40, "the FIRE branch, SnapNum 40");
    TEST_ASSERT(double_bits(b.fire.l.scaling) == double_bits(e.scaling),
                "the FIRE scaling must equal SAGE26's expression");
    TEST_ASSERT(double_bits(b.stars) == double_bits(e.stars) &&
                    double_bits(b.reheated_mass) == double_bits(e.reheated) &&
                    double_bits(b.ejected_mass) == double_bits(e.ejected),
                "stars, reheated and ejected mass must equal SAGE26's expressions bit for bit");
    TEST_ASSERT(gal.MassLoading == 7.25f, "a burst must not write MassLoading");
  }
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_on_versus_off
 * @brief   The same burst with FIREmodeOn 1 and 0: 1 is the inline FIRE budget, 0 the
 *          fixed-epsilon one (the float Vvir squared in float); neither
 *          writes MassLoading
 *
 * The host (Vvir 80 at stored SnapNum 40) and its FoF central (Vvir 300 at SnapNum 50) differ in
 * both, and the FIRE budget must be the host's: the record of the burst names the host's
 * SnapNum, float Vvir and that snapshot's redshift ZZ[40], and the central's budget would
 * differ.
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
      const struct sb_fire_budget e =
          sb_fire_expected(80.0f, MimicConfig.ZZ[40], stars, 1.0, FIRE_TEST_ESN);
      TEST_ASSERT(double_bits(b.reheated_mass) == double_bits(e.reheated) &&
                      double_bits(b.ejected_mass) == double_bits(e.ejected),
                  "FIREmodeOn 1: SAGE26's FIRE budget with the host's values");
      TEST_ASSERT(b.fire.fire_mode == 1 && b.fire.snapnum == 40 && b.fire.vvir == (double)80.0f,
                  "the burst read its host's SnapNum and Vvir");
      TEST_ASSERT(double_bits(b.fire.l.redshift) == double_bits(MimicConfig.ZZ[40]),
                  "the burst read ZZ[40], its host's stored snapshot");
      const struct sb_fire_budget central_e =
          sb_fire_expected(300.0f, MimicConfig.ZZ[50], stars, 1.0, FIRE_TEST_ESN);
      TEST_ASSERT(e.reheated != central_e.reheated, "the central's budget would differ");
    } else {
      const float v = 80.0f;
      const double e_reheated = FIRE_TEST_EPS * stars;
      double e_ejected =
          (FIRE_TEST_EJ * (FIRE_TEST_ETA_SN * FIRE_TEST_ENERGY_SN) / (v * v) - FIRE_TEST_EPS) *
          stars;
      if (e_ejected < 0.0) {
        e_ejected = 0.0;
      }
      TEST_ASSERT(b.fire.fire_mode == 0 && b.fire.snapnum == -1 && isnan(b.fire.l.scaling),
                  "FIREmodeOn 0 forms no FIRE value");
      TEST_ASSERT(double_bits(b.reheated_mass) == double_bits(e_reheated) &&
                      double_bits(b.ejected_mass) == double_bits(e_ejected),
                  "FIREmodeOn 0: the fixed-epsilon budget");
    }
  }
  TEST_ASSERT(reheated[0] != reheated[1] && ejected[0] != ejected[1],
              "the two modes must differ on this input");
  return TEST_PASS;
}

/**
 * @brief Initialise the module with the FIRE tests' burst parameters and FIREmodeOn `value`
 * @return The init status
 */
static int sb_fire_init(const char *value) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(FIRE_TEST_EPS, FIRE_TEST_EJ, 0.43, 0.025, 0.0, 0.3);
  set_model_param("FIREmodeOn", value);
  sb_fire_redshift_table();
  MimicConfig.G = 43.00707785642063;
  return sage26_starburst_feedback_init();
}

/** @brief The module's own EtaSNcode * EnergySNcode budget, for the module-level expectations */
static double sb_fire_module_esn(void) {
  return mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h) *
         mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
}

/**
 * @test    test_fire_burst_wiring_by_channel
 * @brief   Each module channel bursts its host with the host's own Vvir and stored SnapNum, not
 *          the FoF central's: the merger channel (process_per_event) the target, the instability
 *          channel (process_by_galaxy) the satellite. The host's SupernovaOutflowRate holds its
 *          FIRE reheated mass and the central's EjectedGas the ejection, as SAGE26's budget for
 *          the host
 */
int test_fire_burst_wiring_by_channel(void) {
  const struct {
    const char *name;
    int event;
  } rows[] = {{"merger channel", 1}, {"instability channel", 0}};

  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    ROW_ASSERT(sb_fire_init("1") == 0, name, "init must succeed with FIREmodeOn 1");
    struct Halo host, central;
    struct GalaxyData host_gal, central_gal;
    sb_fire_pair(&host, &host_gal, &central, &central_gal);
    struct ModuleContext ctx;
    setup_test_context(&ctx, &central);
    ctx.num_substeps = 10;
    struct ModuleEvent event = merger_event(0.2, 0.01);
    double eburst;
    if (rows[k].event) {
      ctx.active_event = &event;
      eburst = 0.56 * pow(0.2, 0.7);
    } else {
      host_gal.UnstableDiskGasFraction = 0.1;
      eburst = 0.1;
    }
    ROW_ASSERT(sage26_starburst_feedback_process(&ctx, &host, 1) == 0, name, "the channel bursts");
    const struct sb_fire_budget e = sb_fire_expected(
        80.0f, MimicConfig.ZZ[40], eburst * (double)1.0f, (double)1.0f, sb_fire_module_esn());
    const struct sb_fire_budget c = sb_fire_expected(
        300.0f, MimicConfig.ZZ[50], eburst * (double)1.0f, (double)1.0f, sb_fire_module_esn());
    ROW_ASSERT(host_gal.SupernovaOutflowRate == (float)e.reheated, name,
               "the host's outflow is its own FIRE reheated mass");
    ROW_ASSERT(central_gal.EjectedGas == (float)e.ejected, name,
               "the central's ejected gas is the host's FIRE ejection");
    ROW_ASSERT((float)c.reheated != (float)e.reheated, name, "the central's values would differ");
    ROW_ASSERT(host_gal.MassLoading == 7.25f && central_gal.MassLoading == 3.5f, name,
               "the burst writes no MassLoading");
    sage26_starburst_feedback_cleanup();
  }
  return TEST_PASS;
}

/**
 * @test    test_fire_burst_wiring_recheck
 * @brief   The post-minor-merger recheck's own burst takes the FIRE branch with the host's own
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
  TEST_ASSERT(double_bits(b.recheck_fire.l.redshift) == double_bits(MimicConfig.ZZ[40]),
              "the recheck's burst read ZZ[40], the host's stored snapshot");
  const struct sb_fire_budget e =
      sb_fire_expected(140.0f, MimicConfig.ZZ[40], 0.0, 0.0, FIRE_TEST_ESN);
  TEST_ASSERT(double_bits(b.recheck_fire.l.scaling) == double_bits(e.scaling),
              "the recheck burst's scaling is SAGE26's for the host");
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
      log = read_captured_log(capture);
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

  TEST_ASSERT(sb_fire_init("1") == 0, "init must succeed with FIREmodeOn 1");
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
// SAGE26's burst commits its feedback through update_from_feedback to the FoF CENTRAL, whose CGM
// pair it uses with CGMrecipeOn 1 and the central's Regime 0 and whose hot pair otherwise, and
// sends its escaping yield through add_metals_to_hot_reservoir to the same central's selected
// metals. The burst host is given the other Regime throughout. The parameters
// make the instability burst exact: FeedbackReheatingEpsilon 1, FeedbackEjectionEfficiency 0 (the
// ejection (0 - 1) * stars is floored to 0), RecycleFraction 0.5, Yield 0.5,
// FracZleaveDisk 1 with the central's Mvir 0 (FracZleaveDiskVal = 1 * exp(-0 / 30) = 1),
// ThreshMajorMerger 0.3, a host with ColdGas 1 and no cold metals:
//   instability (mode 1, fraction 0.25): eburst 0.25, stars 0.25, reheated 0.25,
//   no renormalisation (0.5 <= 1); the host keeps 1 - 0.125 - 0.25 = 0.625 of cold gas, so the
//   yield takes the retained branch with nothing retained and 0.5 * 1 * 0.25 = 0.125
//   escaping; the central's selected pair 2 / 0.25 -> 2.25 / 0.375.
//   merger (mode 0, ratio 0.5, a major merger: no recheck, and all the yield escapes):
//   eburst = 0.56 * pow(0.5, 0.7), formed at run time below, stars = eburst * 1,
//   reheated = stars; the selected pair -> (float)(2 + stars) / (float)(0.25 + 0.5 * stars).

/** @brief SAGE26's merger burst stars from ColdGas 1 */
static __attribute__((noinline)) double cgm_merger_burst_stars(double mass_ratio) {
  volatile double ratio = mass_ratio;
  volatile float cold_gas = 1.0f;
  return 0.56 * pow(ratio, 0.7) * cold_gas;
}

/** @brief Initialise with the exact-burst parameters and CGMrecipeOn `cgm` */
static int cgm_init(const char *cgm) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.5, 0.5, 1.0, 0.3);
  set_model_param("CGMrecipeOn", cgm);
  return sage26_starburst_feedback_init();
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
  const double merger_stars = cgm_merger_burst_stars(0.5);
  for (int channel = 0; channel < 2; channel++) { /* 0 the instability, 1 the merger event */
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
      char name[96];
      snprintf(name, sizeof(name), "%s, CGMrecipeOn %s, central Regime %d",
               channel == 0 ? "instability burst" : "merger burst", runs[r].cgm,
               runs[r].central_regime);
      ROW_ASSERT(cgm_init(runs[r].cgm) == 0, name, "init must accept CGMrecipeOn 0 and 1");
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
      struct ModuleEvent event = merger_event(0.5, 0.01);
      if (channel == 1) {
        ctx.active_event = &event;
      } else {
        host_gal.UnstableDiskGasFraction = 0.25;
      }
      ROW_ASSERT(sage26_starburst_feedback_process(&ctx, &host, 1) == 0, name,
                 "the burst must succeed");

      const float gas_out = channel == 0 ? 2.25f : (float)(2.0 + merger_stars);
      const float metals_out = channel == 0 ? 0.375f : (float)(0.25 + 0.5 * merger_stars);
      const float *sel_gas = runs[r].to_cgm ? &central_gal.CGMgas : &central_gal.HotGas;
      const float *sel_metals =
          runs[r].to_cgm ? &central_gal.MetalsCGMgas : &central_gal.MetalsHotGas;
      const float *oth_gas = runs[r].to_cgm ? &central_gal.HotGas : &central_gal.CGMgas;
      const float *oth_metals =
          runs[r].to_cgm ? &central_gal.MetalsHotGas : &central_gal.MetalsCGMgas;
      ROW_ASSERT(*sel_gas == gas_out && *sel_metals == metals_out, name,
                 "the central's selected pair receives the feedback and the yield");
      ROW_ASSERT(*oth_gas == 2.0f && *oth_metals == 0.25f, name,
                 "the central's unselected pair is unchanged");
      ROW_ASSERT(central_gal.EjectedGas == 0.0f && central_gal.ColdGas == 3.0f, name,
                 "the central's ejected and cold gas are unchanged");
      ROW_ASSERT(host_gal.HotGas == 5.0f && host_gal.MetalsHotGas == 0.5f &&
                     host_gal.CGMgas == 5.0f && host_gal.MetalsCGMgas == 0.5f &&
                     host_gal.EjectedGas == 0.0f,
                 name, "the host's own pairs and ejected gas are unchanged");
      ROW_ASSERT(channel == 1 || (host_gal.ColdGas == 0.625f && host_gal.MetalsColdGas == 0.0f &&
                                  host_gal.SupernovaOutflowRate == 0.25f),
                 name, "the instability burst leaves the host's cold gas and outflow as worked");
      sage26_starburst_feedback_cleanup();
    }
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_cgm_burst_recheck_sites
 * @brief   The recheck's wind selects by the host's Regime; every burst's feedback and yield, the
 *          nested instability burst's included, by the FoF central's
 *
 * The minor merger of recheck_growth_burst (ratio 0.2 below ThreshMajorMerger 0.3, a gas-unstable
 * host disk) runs the recheck, its black-hole growth and wind on the host and its nested mode-1
 * burst. Each galaxy's two
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
  for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
    char name[96];
    snprintf(name, sizeof(name), "CGMrecipeOn %d, central Regime %d, host Regime %d", runs[r].cgm,
             runs[r].central_regime, runs[r].host_regime);
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
    ROW_ASSERT(mimic_apply_collisional_starburst(0.2, &host, &central, 0, 1, 0.0, 0.0, 0, 0.01,
                                                 &ctx, &params, &b) == 0,
               name, "the burst must run");
    const int central_cgm = runs[r].cgm == 1 && runs[r].central_regime == 0;
    const int host_cgm = runs[r].cgm == 1 && runs[r].host_regime == 0;
    ROW_ASSERT(b.instability_recheck_ran == 1 && b.recheck_wind.reservoir_cgm != -1 &&
                   b.recheck_feedback_reservoir_cgm != -1 && b.recheck_yield_reservoir_cgm != -1,
               name, "the recheck must run its wind and its nested burst");
    ROW_ASSERT(b.feedback.reservoir_cgm == central_cgm && b.yield_reservoir_cgm == central_cgm,
               name, "the outer burst routes by the central's Regime");
    ROW_ASSERT(b.recheck_feedback_reservoir_cgm == central_cgm &&
                   b.recheck_yield_reservoir_cgm == central_cgm,
               name, "the nested burst routes by the central's Regime");
    ROW_ASSERT(b.recheck_wind.reservoir_cgm == host_cgm &&
                   isnan(host_cgm ? b.recheck_wind.hot_gas_energy : b.recheck_wind.cgm_gas_energy),
               name, "the recheck's wind routes by the host's Regime");
    const float central_unselected_gas = central_cgm ? central_gal.HotGas : central_gal.CGMgas;
    const float central_unselected_metals =
        central_cgm ? central_gal.MetalsHotGas : central_gal.MetalsCGMgas;
    const float host_unselected_gas = host_cgm ? gal.HotGas : gal.CGMgas;
    const float host_unselected_metals = host_cgm ? gal.MetalsHotGas : gal.MetalsCGMgas;
    /* both of the host's pairs entered as 5 / 0.1, both of the central's as 40 / 0.8 */
    ROW_ASSERT(central_unselected_gas == 40.0f && central_unselected_metals == 0.8f, name,
               "the central's unselected pair is unchanged");
    ROW_ASSERT(host_unselected_gas == 5.0f && host_unselected_metals == 0.1f, name,
               "the host's pair its wind does not select is unchanged");
  }
  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST TARGETS
// ============================================================================

/**
 * @test    test_ffb_target_merger_continuation
 * @brief   A merger on an FFB target bursts, grows its black hole and runs the post-merger recheck
 *          as on an ordinary target: SAGE26's merger routines do not read FFBRegime
 *          and only the FFB disk call skips the instability
 *
 * Both merger consumers run, sage26_quasar_mode then sage26_starburst_feedback, on a minor merger
 * (the scenario of run_minor_merger) with the recheck configured. The recheck is shown to act
 * against the same FFB target without it (more bulge, more black hole and accretion, less cold
 * gas, no fewer stars), and the baryons of the target and its central (cold and hot gas,
 * ejected gas, stars and black hole) are conserved across the whole merger.
 */
int test_ffb_target_merger_continuation(void) {
  init_memory_system(0);
  struct minor_merger cfg = MINOR_MERGER_DEFAULT;
  cfg.disk_instability = 1;
  cfg.quasar = 1;
  cfg.quasar_consumer = 1;
  cfg.ffb_regime = 1;
  struct GalaxyData ffb_target, ffb_central, normal_target, normal_central;
  TEST_ASSERT(run_minor_merger(&cfg, &ffb_target, &ffb_central) == 0,
              "the merger on the FFB target runs");
  cfg.ffb_regime = 0;
  TEST_ASSERT(run_minor_merger(&cfg, &normal_target, &normal_central) == 0,
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
  TEST_ASSERT(ffb_target.StellarMass > (float)MINOR_TARGET_STELLAR,
              "the burst formed stars on the FFB target");
  TEST_ASSERT(ffb_target.BlackHoleMass > (float)MINOR_TARGET_BLACK_HOLE,
              "the black hole grew on the FFB target");

  /* The baryons of the closed target-and-central system are conserved */
  const double before = MINOR_TARGET_COLD + MINOR_TARGET_STELLAR + MINOR_TARGET_HOT +
                        MINOR_TARGET_BLACK_HOLE + MINOR_CENTRAL_HOT;
  const double after = ffb_target.ColdGas + ffb_target.StellarMass + ffb_target.HotGas +
                       ffb_target.EjectedGas + ffb_target.BlackHoleMass + ffb_central.HotGas +
                       ffb_central.EjectedGas;
  TEST_ASSERT_DOUBLE_EQUAL(after, before, 1e-4,
                           "Total baryons (target+central) must be conserved across the merger");

  /* The recheck acted: without it the FFB target's end state differs */
  cfg.ffb_regime = 1;
  cfg.disk_instability = 0;
  struct GalaxyData no_recheck_target;
  TEST_ASSERT(run_minor_merger(&cfg, &no_recheck_target, NULL) == 0,
              "the merger without the recheck runs");
  TEST_ASSERT(ffb_target.BulgeMass > no_recheck_target.BulgeMass,
              "the recheck's stellar transfer ran on the FFB target");
  TEST_ASSERT(ffb_target.BlackHoleMass > no_recheck_target.BlackHoleMass,
              "the recheck's black-hole growth ran on the FFB target");
  TEST_ASSERT(ffb_target.QuasarModeBHaccretionMass > no_recheck_target.QuasarModeBHaccretionMass,
              "the recheck records extra black-hole accretion");
  TEST_ASSERT(ffb_target.ColdGas < no_recheck_target.ColdGas,
              "the recheck consumes or ejects more cold gas");
  TEST_ASSERT(ffb_target.StellarMass >= no_recheck_target.StellarMass,
              "the recheck does not reduce the stellar mass");
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

  /* The disk-instability burst */
  TEST_RUN(test_disk_instability_starburst);
  TEST_RUN(test_burst_rates_use_full_interval);
  TEST_RUN(test_merger_burst_bulge_channel);

  /* The post-minor-merger recheck */
  TEST_RUN(test_per_event_recheck_black_hole_gating);
  TEST_RUN(test_per_event_zero_ratio_bursts);

  /* Guards on bad state */
  TEST_RUN(test_process_guards);
  TEST_RUN(test_no_burst_leaves_state);
  TEST_RUN(test_zero_vvir);
  TEST_RUN(test_module_initialization);

  /* Bulge star-formation history */
  TEST_RUN(test_instability_burst_samples_bulge_history);
  TEST_RUN(test_merger_burst_samples_with_event_dt);
  TEST_RUN(test_post_merger_recheck_samples_with_event_dt);
  TEST_RUN(test_consecutive_bursts_bulge_excess_grows);

  /* Hydrogen */
  TEST_RUN(test_hydrogen_refresh_module_wiring);
  TEST_RUN(test_hydrogen_refresh_invariants);
  TEST_RUN(test_hydrogen_cap_recheck_growth);

  /* FIRE */
  TEST_RUN(test_fire_burst_budget_independent);
  TEST_RUN(test_fire_burst_on_versus_off);
  TEST_RUN(test_fire_burst_wiring_by_channel);
  TEST_RUN(test_fire_burst_wiring_recheck);
  TEST_RUN(test_fire_burst_snapnum_refused);

  /* CGM routing */
  TEST_RUN(test_cgm_burst_module_channels);
  TEST_RUN(test_cgm_burst_recheck_sites);

  /* Feedback-free-burst targets */
  TEST_RUN(test_ffb_target_merger_continuation);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
