/**
 * @file    test_unit_sage_starburst_feedback.c
 * @brief   Unit tests for sage_starburst_feedback module
 *
 * Validates: the collisional starburst through the module's two channels (the disk-instability
 * burst, process_by_galaxy; the merger burst and its post-minor-merger recheck,
 * process_per_event), its feedback and yield arithmetic, and the guards on bad state.
 *
 * Expected values are hand-computed from SAGE's recipe, never obtained by calling the module's
 * own helpers. Reservoirs are floats, so exact-arithmetic expectations are compared at one part
 * in a million of the value (close_to()).
 *
 * Test cases:
 *   - test_disk_instability_starburst: a disk-instability burst of four galaxies (feedback
 *     renormalised, ejection positive, the minor-burst yield branch, a metal-rich galaxy):
 *     every reservoir against a hand-computed value, mass and metal conservation, bulge growth
 *     equal to stellar growth, the trigger left untouched
 *   - test_burst_rates_use_full_interval: the burst's StarFormationRate and
 *     SupernovaOutflowRate divide the stars and the reheated mass by the host's full snapshot
 *     interval for both channels, independent of the substep count and of the merger event's dt
 *   - test_per_event_merger_feedback_destination: a merger burst forms stars, bulge and outflow
 *     on its target and sends the reheated gas, and with it the yield, to the FoF central's hot
 *     gas, not the target's
 *   - test_per_event_recheck_black_hole_gating: the post-minor-merger recheck grows the black
 *     hole only with the quasar consumer configured and a ratio strictly below
 *     ThresholdMajorMerger
 *   - test_per_event_recheck_agn_recipe_zero_keeps_black_hole: AGNrecipe 0 stops the follow-up's
 *     black-hole growth, as in SAGE, with the quasar module present; at AGNrecipe 2 the recheck
 *     records accretion, consumes cold gas and conserves the baryons of the target and its central
 *   - test_process_guards: process() on bad state (ngal != 1, NULL host or central galaxy, a zero
 *     mass ratio, a non-positive event dt, invalid and sentinel timesteps) returns the right
 *     status and writes nothing
 *   - test_no_burst_leaves_state: no trigger, or a trigger with no cold gas, leaves the galaxy
 *     bit for bit unchanged
 *   - test_zero_vvir: a burst on a zero-Vvir host forms stars and ejects nothing
 *   - test_module_initialization: module_system_init accepts the module's placement
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
extern int sage_starburst_feedback_init(void);
extern int sage_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage_starburst_feedback_cleanup(void);

// ============================================================================
// TEST FIXTURES
// ============================================================================

/**
 * @brief   Initialize global unit conversion constants
 *
 * CRITICAL: The module uses global unit variables (MimicConfig.UnitEnergy_in_cgs,
 * MimicConfig.UnitMass_in_g) to convert physical constants to code units. These must be initialized
 * before calling sage_starburst_feedback_init() or the SN code-unit constants will be garbage.
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

  MimicConfig.NumModelParams = idx;
}

static void append_model_param(const char *param_name, double value) {
  const int idx = MimicConfig.NumModelParams;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "%s", param_name);
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%.6f", value);
  MimicConfig.NumModelParams = idx + 1;
}

/* AGNrecipe is an integer key: the loader parses it strictly, so "%.6f" would be refused. */
static void append_model_param_int(const char *param_name, int value) {
  const int idx = MimicConfig.NumModelParams;
  snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "%s", param_name);
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%d", value);
  MimicConfig.NumModelParams = idx + 1;
}

/* The module loads AGNrecipe only with the recheck active and the quasar module present. */
static void setup_post_merger_recheck_parameters_agn(int include_quasar, int agn_recipe) {
  append_model_param("StarFormingDiskFactor", 3.0);
  if (include_quasar) {
    append_model_param("BlackHoleGrowthRate", 0.02);
    append_model_param("QuasarModeEfficiency", 0.001);
    append_model_param_int("AGNrecipe", agn_recipe);
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

/** @brief A merger event from SAGE's resolver with the given mass ratio and substep dt */
static struct ModuleEvent merger_event(const double mass_ratio, const double dt) {
  struct ModuleEvent event = {.producer_module_id = MODULE_ID_SAGE_RESOLVE_MERGERS_AND_DISRUPTION,
                              .event_id = SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                              .source_index = 2,
                              .target_index = 1,
                              .value0 = mass_ratio,
                              .value1 = dt};
  return event;
}

// ============================================================================
// THE MINOR MERGER SCENARIO
// ============================================================================

/** The merger target's input stellar mass and black hole */
#define MINOR_TARGET_STELLAR 5.0
#define MINOR_TARGET_BLACK_HOLE 0.05

/**
 * What one minor-merger run configures: which consumers sit in the pipeline (the module reads
 * the pipeline at init to decide whether the post-merger recheck and its black-hole growth are
 * enabled) and the merger event it processes.
 */
struct minor_merger {
  int disk_instability; /**< sage_disk_instability by galaxy: the post-merger recheck */
  int quasar;           /**< sage_quasar_mode per event: the recheck grows the black hole */
  int agn_recipe;       /**< AGNrecipe supplied with the quasar parameters */
  double ratio;         /**< the event's mass ratio (value0) */
  double event_dt;      /**< the event's substep dt (value1) */
};

/** The default scenario: no consumers, AGNrecipe 2, a ratio of 0.2 (below ThresholdMajorMerger) */
static const struct minor_merger MINOR_MERGER_DEFAULT = {
    .agn_recipe = 2, .ratio = 0.2, .event_dt = 0.1};

/**
 * @brief Run one merger event through sage_starburst_feedback on a satellite target (cold gas 8,
 *        stellar mass 5, hot gas 5, black hole 0.05) beside its FoF central
 *
 * The module is initialised with the pipeline `cfg` describes and configured with
 * FeedbackReheatingEpsilon 1, no ejection, FracZleaveDisk 0 and ThresholdMajorMerger 0.3; the
 * run allocates through the caller's memory system.
 *
 * @param cfg          The scenario
 * @param target_out   Receives the target's end state
 * @param central_out  Receives the central's end state, may be NULL
 * @return 0 on success; non-zero if init or process failed
 */
static int run_minor_merger(const struct minor_merger *cfg, struct GalaxyData *target_out,
                            struct GalaxyData *central_out) {
  reset_config();
  setup_test_parameters(1.0, 0.0, 0.43, 0.03, 0.0, 0.3);
  if (cfg->disk_instability || cfg->quasar) {
    setup_post_merger_recheck_parameters_agn(cfg->quasar, cfg->agn_recipe);
  }
  if (cfg->disk_instability) {
    test_phase_add("galaxy_physics", "sage_disk_instability", PROCESSING_MODE_BY_GALAXY);
  }
  if (cfg->quasar) {
    test_phase_add("satellite_mergers", "sage_quasar_mode", PROCESSING_MODE_PER_EVENT);
  }
  MimicConfig.G = 43007.1;
  int status = sage_starburst_feedback_init();
  if (status == 0) {
    struct Halo central, target;
    struct GalaxyData central_gal, target_gal;
    setup_test_galaxy(&central, &central_gal, 0, 120.0, 220.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
    setup_test_galaxy(&target, &target_gal, 1, 30.0, 140.0, 8.0, 0.16, 5.0, 1.0, 5.0, 0.1);
    target.Vmax = 120.0;
    target_gal.DiskScaleRadius = 5.0;
    target_gal.BlackHoleMass = MINOR_TARGET_BLACK_HOLE;

    struct ModuleContext ctx;
    setup_test_context(&ctx, &central);
    struct ModuleEvent event = merger_event(cfg->ratio, cfg->event_dt);
    ctx.active_event = &event;
    status = sage_starburst_feedback_process(&ctx, &target, 1);
    *target_out = target_gal;
    if (central_out != NULL) {
      *central_out = central_gal;
    }
  }
  sage_starburst_feedback_cleanup();
  test_free_substep_phases();
  return status;
}

// ============================================================================
// BURST ARITHMETIC, RATES AND FEEDBACK DESTINATION
// ============================================================================

/**
 * @test    test_disk_instability_starburst
 * @brief   A disk-instability burst (mode 1: eburst is the unstable fraction itself) of a central
 *          with ColdGas 10, Mvir 100, StellarMass 5, BulgeMass 1, HotGas 50 and MetalsHotGas 1,
 *          at FeedbackReheatingEpsilon 3, FeedbackEjectionEfficiency 0.5, RecycleFraction 0.43,
 *          Yield 0.03, FracZleaveDisk 0.5 and ThresholdMajorMerger 0.3
 *
 * Every expectation is worked by hand from SAGE's recipe. With a trigger of 0.5, stars would be
 * 5 and the reheated mass 15, more than the 10 of cold gas, so both scale by 10 / 20: stars 2.5,
 * reheated 7.5. The cold gas, the stellar mass and the bulge lose or gain 0.57 * 2.5 = 1.425 (the
 * recycled fraction stays in the cold gas), the reheated gas goes to the central's hot gas with
 * the cold gas's metallicity, and a burst whose fraction is not below ThresholdMajorMerger sends
 * all of its yield, 0.03 * stars of metals, to the hot gas. A SAGE energy product
 * EtaSN * EnergySN of 251382.604 (in the test's units) makes the ejection
 * (0.5 * 251382.6 / Vvir^2 - 3) * stars, negative and so floored to 0 at Vvir 300. The outflow
 * rate and the star formation rate divide the reheated mass and the stars by dT = 0.1.
 * Rows:
 *   - Vvir 300, trigger 0.5: renormalised, no ejection
 *   - Vvir 100, trigger 0.5: the ejection (0.5 * 251382.6 / 1e4 - 3) * 2.5 = 23.9228 leaves the
 *     central's hot gas with its metallicity 1.15 / 57.5
 *   - Vvir 300, trigger 0.2: below the threshold, stars 2 and reheated 6 need no renormalisation,
 *     and the yield takes the minor branch: FracZleaveDisk * exp(-Mvir / 30) = 0.0178371 of it
 *     escapes to the hot gas and the rest enriches the cold gas
 *   - Vvir 300, trigger 0.3, Z = 0.05: the metals scale with the cold gas's metallicity
 *
 * Each row must also conserve mass and metals, grow the bulge by what the stellar mass gains
 * and leave the trigger untouched.
 */
int test_disk_instability_starburst(void) {
  const struct {
    const char *name;
    double vvir;
    double metals_cold; /* input */
    double trigger;
    double cold, metals_cold_out, stellar, metals_stellar, bulge, metals_bulge, hot, metals_hot,
        ejected, metals_ejected, outflow, sfr; /* expected end state */
  } rows[] = {
      {"Vvir 300, renormalised, no ejection", 300.0, 0.2, 0.5, 1.075, 0.0215, 6.425, 0.1285, 2.425,
       0.0485, 57.5, 1.225, 0.0, 0.0, 75.0, 25.0},
      {"Vvir 100, ejection", 100.0, 0.2, 0.5, 1.075, 0.0215, 6.425, 0.1285, 2.425, 0.0485,
       33.57717446, 0.746543489, 23.92282554, 0.478456511, 75.0, 25.0},
      {"trigger 0.2, minor yield branch", 300.0, 0.2, 0.2, 2.86, 0.11612978, 6.14, 0.1228, 2.14,
       0.0428, 56.0, 1.12107022, 0.0, 0.0, 60.0, 20.0},
      {"Z 0.05, trigger 0.3", 300.0, 0.5, 0.3, 1.075, 0.05375, 6.425, 0.32125, 2.425, 0.12125, 57.5,
       1.45, 0.0, 0.0, 75.0, 25.0},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    ROW_ASSERT(sage_starburst_feedback_init() == 0, name, "Module init should succeed");

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

    ROW_ASSERT(sage_starburst_feedback_process(&ctx, &halo, 1) == 0, name,
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
               "SupernovaOutflowRate is the reheated mass / dT");
    ROW_ASSERT(close_to(gal.StarFormationRate, rows[k].sfr), name,
               "StarFormationRate is stars / dT");

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

    sage_starburst_feedback_cleanup();
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
  sage_starburst_feedback_init();
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
  sage_starburst_feedback_process(&ctx, &host, 1);
  *host_out = host_gal;
  sage_starburst_feedback_cleanup();
}

/**
 * @test    test_burst_rates_use_full_interval
 * @brief   SAGE parity: both channels' bursts normalise their rates by the host's FULL snapshot
 *          interval (halo->dT), like the normal disk star-formation channel, so the reported
 *          StarFormationRate and SupernovaOutflowRate are the snapshot means stars / dT and
 *          reheated mass / dT: invariant to the substep count and independent of the merger
 *          event's substep dt (value1)
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
    /* the two runs: host dT, event dt, central dT, substeps */
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
    ROW_ASSERT(close_to(a.SupernovaOutflowRate, rows[k].reheated / rows[k].dt_a), name,
               "SupernovaOutflowRate is the reheated mass / the full interval (first run)");
    ROW_ASSERT(close_to(b.SupernovaOutflowRate, rows[k].reheated / rows[k].dt_b), name,
               "SupernovaOutflowRate is the reheated mass / the full interval (second run)");
    ROW_ASSERT(a.StellarMass == b.StellarMass && a.ColdGas == b.ColdGas, name,
               "The masses are independent of the intervals");
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_per_event_merger_feedback_destination
 * @brief   A merger burst forms stars on its TARGET and credits them to its bulge, but deposits
 *          the reheated gas, and with it the escaping yield, in the FoF central's hot gas
 *          (SAGE's merger_centralgal against centralgal), leaving the target's own hot gas alone
 *
 * A ratio-0.3 merger (equal to ThresholdMajorMerger, so the yield all escapes) onto a target with
 * ColdGas 8, MetalsColdGas 0.16, StellarMass 2, BulgeMass 0.5 and HotGas 5, with no ejection:
 * stars = 0.56 * 0.3^0.7 * 8 = 1.92869 (no renormalisation: 4 * stars < 8), reheated
 * 3 * stars = 5.78608. The target keeps 8 - 0.57 * stars - reheated = 1.11457 of cold gas and
 * gains 0.57 * stars of stars and bulge; the central, with HotGas 40, gains the reheated mass and
 * its metals (0.8 + Z * reheated + 0.03 * stars).
 */
int test_per_event_merger_feedback_destination(void) {
  init_memory_system(0);
  reset_config();
  setup_test_parameters(3.0, 0.0, 0.43, 0.03, 0.5, 0.3);
  sage_starburst_feedback_init();

  struct Halo central, target;
  struct GalaxyData central_gal, target_gal;
  setup_test_galaxy(&central, &central_gal, 0, 120.0, 260.0, 4.0, 0.08, 6.0, 1.5, 40.0, 0.8);
  setup_test_galaxy(&target, &target_gal, 1, 25.0, 180.0, 8.0, 0.16, 2.0, 0.5, 5.0, 0.1);
  struct ModuleContext ctx;
  setup_test_context(&ctx, &central);
  struct ModuleEvent event = merger_event(0.3, 0.1);
  ctx.active_event = &event;

  TEST_ASSERT(sage_starburst_feedback_process(&ctx, &target, 1) == 0,
              "Per-event merger processing should succeed");

  TEST_ASSERT(close_to(target_gal.ColdGas, 1.114569350), "target ColdGas");
  TEST_ASSERT(close_to(target_gal.MetalsColdGas, 0.022291387), "target MetalsColdGas");
  TEST_ASSERT(close_to(target_gal.StellarMass, 3.099354473), "target StellarMass");
  TEST_ASSERT(close_to(target_gal.BulgeMass, 1.599354473), "target BulgeMass");
  TEST_ASSERT(close_to(target_gal.MetalsStellarMass, 0.061987089), "target MetalsStellarMass");
  TEST_ASSERT(close_to(target_gal.MetalsBulgeMass, 0.031987089), "target MetalsBulgeMass");
  TEST_ASSERT(close_to(target_gal.StarFormationRate, 19.28692059), "target StarFormationRate");
  TEST_ASSERT(close_to(target_gal.SupernovaOutflowRate, 57.86076176), "target outflow");
  TEST_ASSERT(target_gal.HotGas == 5.0f && target_gal.MetalsHotGas == 0.1f &&
                  target_gal.EjectedGas == 0.0f,
              "the target's own hot and ejected gas receive nothing");

  TEST_ASSERT(close_to(central_gal.HotGas, 45.78607618), "central HotGas gets the reheated gas");
  TEST_ASSERT(close_to(central_gal.MetalsHotGas, 0.973582285),
              "central MetalsHotGas gets the reheated metals and the yield");
  TEST_ASSERT(central_gal.EjectedGas == 0.0f, "no ejection at FeedbackEjectionEfficiency 0");
  TEST_ASSERT(central_gal.ColdGas == 4.0f && central_gal.StellarMass == 6.0f &&
                  central_gal.BulgeMass == 1.5f,
              "the central forms no stars");

  sage_starburst_feedback_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// THE POST-MINOR-MERGER RECHECK
// ============================================================================

/**
 * @test    test_per_event_recheck_black_hole_gating
 * @brief   The post-minor-merger disk-instability recheck grows the black hole only when the
 *          quasar event consumer is configured and the merger is minor (a ratio strictly below
 *          ThresholdMajorMerger: a major merger, and a ratio equal to the threshold, run no
 *          recheck)
 *
 * The scenario is a minor-merger target of black hole 0.05 with sage_disk_instability
 * configured; the merger burst forms stars in every row, grown black hole or not.
 */
int test_per_event_recheck_black_hole_gating(void) {
  const struct {
    const char *name;
    int quasar;
    double ratio;
    int grows;
  } rows[] = {
      {"no quasar event consumer", 0, 0.2, 0},
      {"quasar consumer, minor merger", 1, 0.2, 1},
      {"major merger runs no recheck", 1, 0.5, 0},
      {"ratio at the threshold runs no recheck", 1, 0.3, 0},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    struct minor_merger cfg = MINOR_MERGER_DEFAULT;
    cfg.disk_instability = 1;
    cfg.quasar = rows[k].quasar;
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
 * @test    test_per_event_recheck_agn_recipe_zero_keeps_black_hole
 * @brief   AGNrecipe 0 stops the post-minor-merger recheck's black-hole growth
 *
 * SAGE gates grow_black_hole() on AGNrecipeOn in check_disk_instability
 * (sage-model/src/model_disk_instability.c:67-68). With the recheck active and
 * sage_quasar_mode present, AGNrecipe 2 grows the black hole
 * (test_per_event_recheck_black_hole_gating); AGNrecipe 0 must leave the
 * black hole as if the quasar module were absent, while the recheck's starburst still runs.
 *
 * Expected: BlackHoleMass and QuasarModeBHaccretionMass unchanged; every other reservoir
 * equal to the run without the quasar module; bulge above the run with no recheck. The
 * AGNrecipe 2 run (the positive control) must also conserve the baryons and consume cold gas.
 */
int test_per_event_recheck_agn_recipe_zero_keeps_black_hole(void) {
  init_memory_system(0);

  struct GalaxyData no_recheck;
  struct GalaxyData no_quasar;
  struct GalaxyData agn_zero;
  struct GalaxyData agn_on;
  struct GalaxyData agn_on_central;

  struct minor_merger cfg = MINOR_MERGER_DEFAULT;
  TEST_ASSERT(run_minor_merger(&cfg, &no_recheck, NULL) == 0,
              "Run without the recheck should succeed");
  cfg.disk_instability = 1;
  TEST_ASSERT(run_minor_merger(&cfg, &no_quasar, NULL) == 0,
              "Run without the quasar module should succeed");
  cfg.quasar = 1;
  cfg.agn_recipe = 0;
  TEST_ASSERT(run_minor_merger(&cfg, &agn_zero, NULL) == 0,
              "Init and process with AGNrecipe 0 should succeed");
  cfg.agn_recipe = 2;
  TEST_ASSERT(run_minor_merger(&cfg, &agn_on, &agn_on_central) == 0,
              "Run with AGNrecipe 2 should succeed");

  TEST_ASSERT_DOUBLE_EQUAL(agn_zero.BlackHoleMass, 0.05, 1e-6,
                           "BH mass should be unchanged by the recheck at AGNrecipe 0");
  TEST_ASSERT_DOUBLE_EQUAL(agn_zero.QuasarModeBHaccretionMass, 0.0, 1e-6,
                           "No BH accretion should be tallied by the recheck at AGNrecipe 0");
  TEST_ASSERT_DOUBLE_EQUAL(agn_zero.ColdGas, no_quasar.ColdGas, 1e-9,
                           "AGNrecipe 0 with the quasar module should match its absence (ColdGas)");
  TEST_ASSERT_DOUBLE_EQUAL(agn_zero.BulgeMass, no_quasar.BulgeMass, 1e-9,
                           "AGNrecipe 0 with the quasar module should match its absence (Bulge)");
  TEST_ASSERT_DOUBLE_EQUAL(agn_zero.StellarMass, no_quasar.StellarMass, 1e-9,
                           "AGNrecipe 0 with the quasar module should match its absence (Stars)");
  TEST_ASSERT(agn_zero.BulgeMass > no_recheck.BulgeMass,
              "The recheck's starburst should still run at AGNrecipe 0");
  TEST_ASSERT(agn_on.BlackHoleMass > 0.05, "Positive control: AGNrecipe 2 should grow the BH");

  /* The recheck at AGNrecipe 2, against the merger without it: it records the accretion, consumes
   * or ejects more cold gas and reduces no stars, and the baryons of the target and its central
   * (cold, hot and ejected gas, stars and black hole) are conserved across the merger */
  TEST_ASSERT(agn_on.QuasarModeBHaccretionMass > no_recheck.QuasarModeBHaccretionMass,
              "The recheck should record extra BH accretion");
  TEST_ASSERT(agn_on.ColdGas < no_recheck.ColdGas,
              "The recheck should consume or eject more cold gas");
  TEST_ASSERT(agn_on.StellarMass >= no_recheck.StellarMass,
              "The recheck should not reduce the stellar mass");
  const double before = 8.0 + MINOR_TARGET_STELLAR + 5.0 + MINOR_TARGET_BLACK_HOLE + 40.0;
  const double after = agn_on.ColdGas + agn_on.StellarMass + agn_on.HotGas + agn_on.EjectedGas +
                       agn_on.BlackHoleMass + agn_on_central.HotGas + agn_on_central.EjectedGas;
  TEST_ASSERT_DOUBLE_EQUAL(after, before, 1e-4,
                           "Total baryons (target+central) must be conserved across the merger");

  check_memory_leaks();

  return TEST_PASS;
}

// ============================================================================
// GUARDS ON BAD STATE
// ============================================================================

/**
 * @test    test_process_guards
 * @brief   process() on bad state returns the right status and writes nothing: an ngal other than 1
 *          (-1), a NULL host or FoF-central galaxy (a silent 0), a merger event of zero mass ratio
 *          (a 0 no-op, decided before the event's dt is looked at), a merger event whose substep dt
 *          (value1) is not positive (-1), an invalid non-boundary dT (-1) and the boundary
 *          sentinel dT (a 0 no-op)
 *
 * Every row runs a satellite host with ColdGas 10 and a trigger of 0.5 (or a merger event) beside
 * its central; each error row must log its own message, which tells the guards that return the
 * same status apart, and the galaxies it was handed must come back bit for bit unchanged.
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
    double ratio;        /* the event's mass ratio (value0) */
    double event_dt;     /* the event's substep dt (value1) */
    int expected;        /* process()'s status */
    const char *message; /* the error the guard logs, NULL for a silent return */
  } rows[] = {
      {"by-galaxy: ngal 2", 0, 2, 0, 0, 63, 0.1, 0.0, 0.0, -1, "expects ngal=1"},
      {"by-galaxy: NULL host galaxy", 0, 1, 1, 0, 63, 0.1, 0.0, 0.0, 0, NULL},
      {"by-galaxy: NULL central galaxy", 0, 1, 0, 1, 63, 0.1, 0.0, 0.0, 0, NULL},
      {"by-galaxy: invalid non-boundary dT", 0, 1, 0, 0, 63, -1.0, 0.0, 0.0, -1,
       "Invalid disk-instability dt"},
      {"by-galaxy: boundary sentinel dT", 0, 1, 0, 0, -1, -1.0, 0.0, 0.0, 0, NULL},
      {"event: ngal 2", 1, 2, 0, 0, 63, 0.1, 0.3, 0.1, -1, "(process_per_event) expects ngal=1"},
      {"event: NULL host galaxy", 1, 1, 1, 0, 63, 0.1, 0.3, 0.1, 0, NULL},
      {"event: NULL central galaxy", 1, 1, 0, 1, 63, 0.1, 0.3, 0.1, 0, NULL},
      {"event: zero ratio is a no-op even with no dt", 1, 1, 0, 0, 63, 0.1, 0.0, 0.0, 0, NULL},
      {"event: zero substep dt", 1, 1, 0, 0, 63, 0.1, 0.3, 0.0, -1, "missing valid source dt"},
      {"event: negative substep dt", 1, 1, 0, 0, 63, 0.1, 0.3, -0.1, -1, "missing valid source dt"},
  };

  init_memory_system(0);
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    const char *name = rows[k].name;
    reset_config();
    setup_test_parameters(3.0, 0.5, 0.43, 0.03, 0.5, 0.3);
    sage_starburst_feedback_init();

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
    struct ModuleEvent event = merger_event(rows[k].ratio, rows[k].event_dt);
    if (rows[k].event) {
      ctx.active_event = &event;
    }

    FILE *capture = tmpfile();
    FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
    const int result = sage_starburst_feedback_process(&ctx, halos, rows[k].ngal);
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
    sage_starburst_feedback_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_no_burst_leaves_state
 * @brief   A galaxy with no trigger, and one with a trigger but no cold gas, come out of
 *          process() bit for bit unchanged (no star formation, no feedback, no yield), with
 *          finite metals
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
    sage_starburst_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0, 100.0, 300.0, rows[k].cold_gas, rows[k].metals_cold, 5.0, 1.0,
                      50.0, 1.0);
    gal.UnstableDiskGasFraction = rows[k].trigger;
    struct ModuleContext ctx;
    setup_test_context(&ctx, &halo);
    const struct GalaxyData before = gal;

    ROW_ASSERT(sage_starburst_feedback_process(&ctx, &halo, 1) == 0, name,
               "Process function should succeed");
    ROW_ASSERT(memcmp(&before, &gal, sizeof(before)) == 0, name, "The galaxy is unchanged");
    ROW_ASSERT(isfinite(gal.MetalsStellarMass) && isfinite(gal.MetalsBulgeMass) &&
                   isfinite(gal.MetalsHotGas) && isfinite(gal.MetalsEjectedGas),
               name, "The metals stay finite");

    sage_starburst_feedback_cleanup();
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
  sage_starburst_feedback_init();

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_galaxy(&halo, &gal, 0, 100.0, 0.0, 10.0, 0.2, 5.0, 1.0, 50.0, 1.0);
  gal.UnstableDiskGasFraction = 0.5;

  struct ModuleContext ctx;
  setup_test_context(&ctx, &halo);

  const double initial_stellar = gal.StellarMass;
  const double initial_ejected = gal.EjectedGas;

  /* ===== EXECUTE ===== */
  sage_starburst_feedback_process(&ctx, &halo, 1);

  /* ===== VALIDATE ===== */
  /* Stars should still form */
  TEST_ASSERT(gal.StellarMass > initial_stellar, "Stars should form even with Vvir = 0");

  /* No ejection when Vvir = 0 */
  TEST_ASSERT_DOUBLE_EQUAL(gal.EjectedGas, initial_ejected, 1e-10, "No ejection when Vvir = 0");

  /* ===== CLEANUP ===== */
  sage_starburst_feedback_cleanup();
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

  test_phase_add("galaxy_physics", "sage_starburst_feedback", PROCESSING_MODE_BY_GALAXY);
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

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage_starburst_feedback Unit Tests\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* The burst: arithmetic, rates and feedback destination */
  TEST_RUN(test_disk_instability_starburst);
  TEST_RUN(test_burst_rates_use_full_interval);
  TEST_RUN(test_per_event_merger_feedback_destination);

  /* The post-minor-merger recheck */
  TEST_RUN(test_per_event_recheck_black_hole_gating);
  TEST_RUN(test_per_event_recheck_agn_recipe_zero_keeps_black_hole);

  /* Guards on bad state */
  TEST_RUN(test_process_guards);
  TEST_RUN(test_no_burst_leaves_state);
  TEST_RUN(test_zero_vvir);

  /* Infrastructure */
  TEST_RUN(test_module_initialization);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
