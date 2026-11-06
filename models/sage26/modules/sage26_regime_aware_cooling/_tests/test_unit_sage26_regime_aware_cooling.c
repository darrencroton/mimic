/**
 * @file    test_unit_sage26_regime_aware_cooling.c
 * @brief   Platform-independent tests of sage26_regime_aware_cooling, SAGE26's regime-aware
 *          cooling transaction (C = R/src/model_cooling_heating.c:420-506)
 *
 * Every expectation is derived independently of the transaction and the shared helpers it calls:
 * a literal chosen so the result is exact, or SAGE26's expression written inline with its operand
 * order and its line cited. The oracle test (test_unit_sage26_regime_aware_cooling_oracle.c)
 * covers the same transaction bit for bit on macOS arm64 only; these run everywhere.
 *
 * Test cases:
 *   - test_cgm_recipe_off_writes_nothing: CGMrecipeOn 0 leaves the halo and galaxy untouched
 *   - test_mdot_reset_and_rate_overwrite: the raw mdot rates reset (C:433-434) and CoolingRate
 *     overwritten (C:498-502) on a call that cools nothing
 *   - test_stream_rates_stored_raw: the streams' float stores reach mdot_cool_raw and
 *     mdot_stream_raw (C:282-283)
 *   - test_hot_agn_before_cgm_cooling: the hot AGN's heating radius is what the CGM AGN reads
 *   - test_cgm_applied_before_hot: CGM then hot application, each capped (C:475-496)
 *   - test_hot_apply_fresh_cap: the hot application is capped to the post-AGN HotGas
 *   - test_cgm_apply_after_cgm_agn: with the CGM AGN on, the CGM application moves the CGM pair
 *     as that AGN left it, with its own metallicity (C:475-483 after C:630-680)
 *   - test_equality_metals: no "move all metals" branch at equality; metals above the
 *     proportional split (C:479-483, :491-495)
 *   - test_diagnostics_reset_survival_restoration: every diagnostic path (C:436-469)
 *   - test_initial_boundary_matches_classic: no positive interval: what the classic trio writes
 *   - test_d35_refusal_with_positive_interval: a stored SnapNum outside ZZ is refused
 *   - test_parameter_refusals: CGMrecipeOn and MShockMsun (D33)
 *   - test_guard_*: the CGMrecipeOn 1 configuration guard of this module and the budget
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/sage26_calculate_cooling_budget/cooling_tables.h"
#include "modules/sage26_regime_aware_cooling/sage26_regime_aware_cooling.h"
#include "shared/sage_constants.h"

extern int sage26_calculate_cooling_budget_init(void);
extern int sage26_calculate_cooling_budget_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_cooling_budget_cleanup(void);
extern int sage26_radio_mode_heating_init(void);
extern int sage26_radio_mode_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_regime_aware_cooling_init(void);
extern int sage26_regime_aware_cooling_process(struct ModuleContext *ctx, struct Halo *halos,
                                               int ngal);
extern int sage26_regime_aware_cooling_cleanup(void);

/* The run units (mini-Millennium) every test sets */
#define T_G 43.007077856420629
#define T_HUBBLE 0.73
#define T_UNIT_TIME 3.08568e19
#define T_UNIT_DENSITY 6.7698980144406312e-31
#define T_UNIT_ENERGY 1.989e53

static const struct Sage26RegimeAwareCoolingConfig AGN_OFF = {1, 6.0e11, {0, 0.08}};
static const struct Sage26RegimeAwareCoolingConfig AGN_BONDI = {1, 6.0e11, {2, 0.08}};

// ============================================================================
// FIXTURES
// ============================================================================

/** @brief The run's units, Millennium cosmology and a redshift table with only ZZ[40] = 0 */
static void set_run_units(void) {
  MimicConfig.G = T_G;
  MimicConfig.Hubble_h = T_HUBBLE;
  MimicConfig.UnitTime_in_s = T_UNIT_TIME;
  MimicConfig.UnitDensity_in_cgs = T_UNIT_DENSITY;
  MimicConfig.UnitEnergy_in_cgs = T_UNIT_ENERGY;
  MimicConfig.Omega = 0.25;
  MimicConfig.Snaplistlen = 64;
  for (int k = 0; k < ABSOLUTEMAXSNAPS; k++) {
    MimicConfig.ZZ[k] = NAN; /* a read of another snapshot surfaces as NaN */
  }
  MimicConfig.ZZ[40] = 0.0;
}

/** @brief Load the cooling tables once (the budget's init with CGMrecipeOn 0), then the units */
static int load_tables(void) {
  reset_config();
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required (D33) */
  const int status = sage26_calculate_cooling_budget_init();
  set_run_units();
  return status;
}

/**
 * @brief A central with both reservoirs: Mvir 100 above the shock mass 43.8 and z = 0, so the
 *        streams' cutoff sets f_stream = 0 exactly (C:263-265); every written field holds a
 *        sentinel no path writes
 */
static void setup_galaxy(struct Halo *halo, struct GalaxyData *gal, const int regime) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  halo->galaxy = gal;
  halo->Type = 0;
  halo->SnapNum = 40; /* the stored snapshot: ZZ[40] = 0 */
  halo->dT = 0.01;
  halo->Mvir = 100.0;
  halo->Rvir = 0.2;
  halo->Vvir = 150.0;
  gal->Regime = regime;
  gal->HotGas = 10.0f;
  gal->MetalsHotGas = 0.2f;
  gal->CGMgas = 5.0f;
  gal->MetalsCGMgas = 0.05f;
  gal->ColdGas = 1.0f;
  gal->MetalsColdGas = 0.01f;
  gal->BlackHoleMass = 0.0f;
  gal->Rheat = 0.0f;
  gal->Cooling = 2.0;
  gal->Heating = 3.0;
  gal->tcool = -11.0f;
  gal->tff = -22.0f;
  gal->RcoolToRvir = -33.0f;
  gal->CoolingRate = -44.0f;
  gal->mdot_cool_raw = -77.0f;
  gal->mdot_stream_raw = -88.0f;
}

/**
 * @brief A float the compiler cannot constant-fold, so an inline expected expression is evaluated
 *        at run time and contracted (fused) as the module's own expression is; a folded constant
 *        expression is evaluated unfused
 */
static float opaque(const float value) {
  volatile float v = value;
  return v;
}

/** @brief SAGE26's float CoolingRate of a mass (C:499-502), inline */
static float expected_rate(const double mass, const double dt) {
  return (dt > 0.0) ? (float)((mass / dt) * 1.0e10 / T_HUBBLE * 3.155e16 / T_UNIT_TIME) : 0.0f;
}

/** @brief The hot recipe's tcool diagnostic (C:142, :145-146), inline */
static float expected_hot_tcool(const float rvir, const float vvir) {
  const double tcool = rvir / vvir; /* C:142: a float division stored in a double */
  return (float)((tcool * 1.0e10 / T_HUBBLE * 3.155e16 / T_UNIT_TIME));
}

/** @brief The hot recipe's uncapped rcool (C:142-173), inline */
static double expected_hot_rcool(const float hot, const float zhot, const float rvir,
                                 const float vvir) {
  const double tcool = rvir / vvir;                                         /* C:142 */
  const double temp = 35.9 * vvir * vvir;                                   /* C:150 */
  const double logZ = log10(zhot / hot);                                    /* C:154 */
  const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ); /* C:157 */
  double x = PROTONMASS * BOLTZMANN * temp / lambda;                        /* C:163 */
  x /= (T_UNIT_DENSITY * T_UNIT_TIME);                                      /* C:164 */
  const double rho_rcool = x / tcool * (1.5 * 0.59);                        /* C:165 */
  const double rho0 = hot / (4 * M_PI * rvir);                              /* C:172 */
  return sqrt(rho0 / rho_rcool);                                            /* C:173 */
}

/** @brief The CGM recipe's tcool and tff diagnostics (C:322-357), inline */
static void expected_cgm_diagnostics(const float cgm, const float zcgm, const float mvir,
                                     const float rvir, const float vvir, float *tcool_out,
                                     float *tff_out) {
  const double temp = 35.9 * vvir * vvir;                                                /* C:322 */
  const double lambda = get_metaldependent_cooling_rate(log10(temp), log10(zcgm / cgm)); /* :329 */
  double x = PROTONMASS * BOLTZMANN * temp / lambda;                                     /* C:332 */
  x /= (T_UNIT_DENSITY * T_UNIT_TIME);                                                   /* C:333 */
  const double alpha = 1.4, r0 = 0.1 * rvir, ratio = 10.0;                           /* C:336-338 */
  const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);                /* C:341 */
  const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha); /* C:342 */
  const double rho0 = cgm / (4.0 * M_PI * r0 * r0 * r0 * I_M);                       /* C:345 */
  const double rho_eff = rho0 * (I_cool / I_M);                                      /* C:346 */
  const double tcool = (x / rho_eff) * (1.5 * 0.59);                                 /* C:349 */
  const double g_accel = T_G * mvir / (rvir * rvir);                                 /* C:352 */
  const double tff = sqrt(2.0 * rvir / g_accel);                                     /* C:353 */
  *tcool_out = (float)(tcool * T_UNIT_TIME / 3.155e16);                              /* C:356 */
  *tff_out = (float)(tff * T_UNIT_TIME / 3.155e16);                                  /* C:357 */
}

/** @brief The transaction's model parameters (all four keys it reads) */
static void set_transaction_parameters(const char *cgm_recipe_on, const char *mshock) {
  if (cgm_recipe_on != NULL) {
    sage26_test_append_model_parameter("CGMrecipeOn", cgm_recipe_on);
  }
  if (mshock != NULL) {
    sage26_test_append_model_parameter("MShockMsun", mshock);
  }
  sage26_test_append_model_parameter("AGNrecipe", "0");
  sage26_test_append_model_parameter("RadioModeEfficiency", "0.08");
}

/** @brief The run files' cooling phase: the classic trio, then the transaction */
static void add_run_file_cooling_phase(const int with_transaction) {
  test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_radio_mode_heating", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("galaxy_physics", "sage26_apply_cooling", PROCESSING_MODE_BY_GALAXY);
  if (with_transaction) {
    test_phase_add("galaxy_physics", "sage26_regime_aware_cooling", PROCESSING_MODE_BY_GALAXY);
  }
}

/** @brief A module context for one galaxy with ten substeps */
static void setup_ctx(struct ModuleContext *ctx, struct Halo *halo) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->params = &MimicConfig;
  ctx->num_substeps = 10;
  ctx->central_galaxy = halo;
}

// ============================================================================
// THE TRANSACTION
// ============================================================================

/**
 * @test    test_cgm_recipe_off_writes_nothing
 * @brief   With CGMrecipeOn 0 SAGE26 calls cooling_recipe_hot (C:128-131): the module is inert
 *
 * Expected: process() returns 0 and every byte of the halo and the galaxy is unchanged, for both
 * regimes and both reservoirs present.
 */
int test_cgm_recipe_off_writes_nothing(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  set_transaction_parameters("0", "6.0e11");
  TEST_ASSERT(sage26_regime_aware_cooling_init() == 0, "init succeeds with CGMrecipeOn 0");
  for (int regime = -1; regime <= 1; regime++) {
    struct Halo halo, before_halo;
    struct GalaxyData gal, before_gal;
    struct ModuleContext ctx;
    setup_galaxy(&halo, &gal, regime);
    setup_ctx(&ctx, &halo);
    memcpy(&before_halo, &halo, sizeof(halo)); /* padding included, for memcmp */
    memcpy(&before_gal, &gal, sizeof(gal));
    TEST_ASSERT(sage26_regime_aware_cooling_process(&ctx, &halo, 1) == 0, "process succeeds");
    TEST_ASSERT(memcmp(&gal, &before_gal, sizeof(gal)) == 0, "no galaxy byte changes");
    TEST_ASSERT(memcmp(&halo, &before_halo, sizeof(halo)) == 0, "no halo byte changes");
  }
  sage26_regime_aware_cooling_cleanup();
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_mdot_reset_and_rate_overwrite
 * @brief   A Regime 0 galaxy without CGM gas: the mdot reset (C:433-434), the diagnostic reset
 *          (C:443) and the final CoolingRate of zero total (C:498-502)
 *
 * Expected: mdot_cool_raw and mdot_stream_raw 0; tcool 0, tff -1, RcoolToRvir -1; CoolingRate
 * overwritten from -44 to 0; no mass moves and Cooling and Heating are untouched.
 */
int test_mdot_reset_and_rate_overwrite(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 0);
  gal.CGMgas = 0.0f;
  gal.MetalsCGMgas = 0.0f;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, 1.0e-3, NULL) == 0,
              "the transaction succeeds");
  TEST_ASSERT(gal.mdot_cool_raw == 0.0f && gal.mdot_stream_raw == 0.0f, "the raw rates reset");
  TEST_ASSERT(gal.tcool == 0.0f && gal.tff == -1.0f && gal.RcoolToRvir == -1.0f,
              "reset_cgm_diagnostics writes exactly 0, -1, -1 (C:413-418)");
  TEST_ASSERT(gal.CoolingRate == 0.0f, "CoolingRate is overwritten with the zero total");
  TEST_ASSERT(gal.ColdGas == 1.0f && gal.HotGas == 10.0f && gal.Cooling == 2.0 &&
                  gal.Heating == 3.0,
              "no mass or energy moves");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_stream_rates_stored_raw
 * @brief   The hot recipe's float stores reach the raw state (C:282-283; campaign decision D39)
 *
 * Expected: with the cutoff (Mvir 100 above M_shock = 6e11 * 0.73 / 1e10 = 43.8, z = 0, so
 * 3 M_*(0) >> M_shock) f_stream is exactly 0, so mdot_stream_raw is 0 and mdot_cool_raw is
 * SAGE26's float hot_halo_cooling / dt (C:279-282) with rcool written inline (C:142-173).
 */
int test_stream_rates_stored_raw(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 1);
  gal.CGMgas = 0.0f;
  gal.MetalsCGMgas = 0.0f;
  const double dt = 1.0e-5;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0,
              "the transaction succeeds");

  const float hot = 10.0f, zhot = 0.2f, rvir = 0.2f, vvir = 150.0f;
  const double tcool = rvir / vvir;                               /* C:142 */
  const double rcool = expected_hot_rcool(hot, zhot, rvir, vvir); /* C:173 */
  const double f_stream = 0.0;                                    /* C:263-265 */
  const double cold_stream_cooling = f_stream * hot / tcool * dt; /* C:277 */
  const double hot_halo_cooling =
      (1.0 - f_stream) * (hot / rvir) * (rcool / (2.0 * tcool)) * dt; /* C:279-280 */
  TEST_ASSERT(gal.mdot_cool_raw == (float)(hot_halo_cooling / dt), "mdot_cool_raw (C:282)");
  TEST_ASSERT(gal.mdot_stream_raw == (float)(cold_stream_cooling / dt) &&
                  gal.mdot_stream_raw == 0.0f,
              "mdot_stream_raw (C:283)");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_hot_agn_before_cgm_cooling
 * @brief   The CGM AGN reads the heating radius the hot AGN changed (C:451-461)
 *
 * A hot-regime central with dense hot gas and a large black hole: the hot recipe's rcool exceeds
 * Rvir, the hot AGN's heating is capped to the cooling mass, so its ratchet sets Rheat = rcool, and
 * the CGMrecipeOn 1 cap sets Rheat = Rvir (C:612-616, :623-624). The CGM recipe then pins rcool to
 * Rvir (C:360). Its AGN gate sees the positive post-cap mass (C:372-374), so the CGM AGN is called;
 * its suppression then zeroes the mass, as Rheat >= rcool (C:638-643); the accretion's heating cap
 * then binds against the zero mass and sets the accretion to 0 / AGNcoeff = 0 (C:568-571), so the
 * CGM pair is not drawn from, and nothing is applied (C:475). The pair is not touched (the oracle's
 * rac_regime1_hot_rheat_saturates_cgm records cgm.agn_entered 1, heating_capped 1, AGNaccreted 0).
 *
 * Expected: RcoolToRvir > 1 (the precondition), Rheat equal to the float Rvir, CGMgas and
 * MetalsCGMgas exactly as they were, the black hole grown. Run in the other order the CGM AGN
 * would read Rheat 0 and cool CGM gas.
 */
int test_hot_agn_before_cgm_cooling(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 1);
  gal.HotGas = 50.0f;
  gal.MetalsHotGas = 0.5f;
  gal.CGMgas = 1.0f;
  gal.MetalsCGMgas = 0.01f;
  gal.BlackHoleMass = 1.0f;
  gal.Rheat = 0.0f;

  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_BONDI, 1.0e-3, NULL) == 0,
              "the transaction succeeds");
  TEST_ASSERT(gal.RcoolToRvir > 1.0f, "precondition: the hot rcool exceeds Rvir");
  TEST_ASSERT(gal.Rheat == 0.2f, "the hot AGN set Rheat to the float Rvir");
  TEST_ASSERT(gal.CGMgas == 1.0f && gal.MetalsCGMgas == 0.01f,
              "the CGM AGN read that Rheat: no CGM gas cooled or accreted");
  TEST_ASSERT(gal.BlackHoleMass > 1.0f, "the hot AGN grew the black hole");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_cgm_applied_before_hot
 * @brief   CGM is applied before hot (C:475-496), each capped to its reservoir
 *
 * With AGN off and a long dt, each recipe caps its mass to its whole reservoir (C:288-292,
 * :366-370), so the applied masses are the literals CGMgas = 2^-24 and HotGas = 2^-23. With
 * ColdGas 1, adding the CGM mass first rounds 1 + 2^-24 to 1 (a tie to even), then
 * 1 + 2^-23; the other order gives 1 + 2^-23 then the tie 1 + 2^-23 + 2^-24, rounded to
 * 1 + 2^-22.
 *
 * Expected: ColdGas = (1 + 2^-24) + 2^-23 in float, both reservoirs 0, CoolingRate the total.
 */
int test_cgm_applied_before_hot(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 1);
  const float cgm = 0x1p-24f, hot = 0x1p-23f;
  gal.CGMgas = cgm;
  gal.MetalsCGMgas = 0.0f;
  gal.HotGas = hot;
  gal.MetalsHotGas = 0.0f;
  gal.ColdGas = 1.0f;
  gal.MetalsColdGas = 0.0f;
  const double dt = 1.0e12;
  struct Sage26RegimeAwareCoolingTrace t;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, &t) == 0,
              "the transaction succeeds");
  TEST_ASSERT(t.hot.cap == MIMIC_SAGE_CAP_RESERVOIR && t.cgm.cap == MIMIC_SAGE_CAP_RESERVOIR,
              "precondition: each recipe caps its mass to its reservoir");

  float cold = opaque(1.0f); /* C:480 then C:492 */
  cold += (double)cgm;
  cold += (double)hot;
  float other_order = opaque(1.0f);
  other_order += (double)hot;
  other_order += (double)cgm;
  TEST_ASSERT(cold != other_order, "precondition: the two orders round differently");
  TEST_ASSERT(gal.ColdGas == cold, "the CGM mass is added before the hot mass");
  TEST_ASSERT(gal.CGMgas == 0.0f && gal.HotGas == 0.0f, "both reservoirs are consumed");
  TEST_ASSERT(gal.CoolingRate == expected_rate((double)cgm + (double)hot, dt),
              "CoolingRate of the total (C:498-502)");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_hot_apply_fresh_cap
 * @brief   The hot application caps to the HotGas the hot AGN left (C:488-490)
 *
 * With a long dt the hot recipe caps its mass to HotGas and the hot AGN (Bondi, Rheat 0, so no
 * suppression) then accretes from HotGas without re-capping the returned mass (C:599-626): the
 * returned mass exceeds the post-AGN HotGas.
 *
 * Expected: HotGas exactly 0 after the application (capped to the post-AGN reservoir, not the
 * pre-AGN one, which would leave it negative), the black hole grown.
 */
int test_hot_apply_fresh_cap(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 1);
  gal.CGMgas = 0.0f;
  gal.MetalsCGMgas = 0.0f;
  gal.BlackHoleMass = 0.1f;
  struct Sage26RegimeAwareCoolingTrace t;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_BONDI, 1.0, &t) == 0,
              "the transaction succeeds");
  TEST_ASSERT(t.hot.cap == MIMIC_SAGE_CAP_RESERVOIR && t.hot_agn.accreted > 0.0,
              "precondition: the recipe capped to HotGas and the AGN accreted from it");
  TEST_ASSERT(gal.HotGas == 0.0f, "the application emptied the post-AGN HotGas exactly");
  TEST_ASSERT(gal.BlackHoleMass > 0.1f, "the black hole grew");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_cgm_apply_after_cgm_agn
 * @brief   The CGM application uses the CGM pair as the CGM AGN left it (C:475-483 after
 *          C:630-680)
 *
 * A Regime 0 central with CGMgas 5 and metals 0.3 (metallicity 0.06, not a fixed fraction of the
 * gas moved), no hot gas, Rheat 0 (no suppression), a black hole of 0.1 and AGNrecipe 2 (Bondi),
 * dt 1e12. The CGM recipe caps its mass to CGMgas (C:366-367); the Bondi and Eddington rates over
 * that dt exceed the reservoir, so the accretion is capped to it (C:559-560) and then by the
 * heating cap to mass / AGNcoeff (C:562-571), a few 1e-6 of the gas, drawn from CGMgas and its
 * metals at their metallicity (C:653-656); the returned mass is re-capped to the post-accretion
 * CGMgas (C:675-677), so the application moves exactly the post-AGN pair, with the post-AGN
 * metallicity (C:479-483), leaving CGMgas 0 and a metal residual of rounding size.
 *
 * Expected: ColdGas, MetalsColdGas, CGMgas, MetalsCGMgas, BlackHoleMass, Rheat and CoolingRate as
 * SAGE26's statements written inline give them (every input read through opaque(), so the inline
 * fused metal statements contract as the module's do). Preconditions assert that the pre-AGN
 * metallicity, the hot pair's metallicity (0: no hot gas) and the pre-AGN CGMgas would each give a
 * different end state.
 */
int test_cgm_apply_after_cgm_agn(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 0);
  gal.HotGas = 0.0f;
  gal.MetalsHotGas = 0.0f;
  gal.CGMgas = 5.0f;
  gal.MetalsCGMgas = 0.3f;
  gal.ColdGas = 1.0f;
  gal.MetalsColdGas = 0.01f;
  gal.BlackHoleMass = 0.1f;
  gal.Rheat = 0.0f;
  const double dt = 1.0e12;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_BONDI, dt, NULL) == 0,
              "the transaction succeeds");

  /* SAGE26's float inputs (R/src/core_allvars.h:106-221), read at run time */
  const float mvir = opaque(100.0f), rvir = opaque(0.2f), vvir = opaque(150.0f);
  float cgm = opaque(5.0f), zcgm = opaque(0.3f), bh = opaque(0.1f), rheat = opaque(0.0f);
  float cold = opaque(1.0f), zcold = opaque(0.01f);
  const double efficiency = 0.08;

  /* cooling_recipe_cgm up to the AGN step (C:322-370) */
  const double temp = 35.9 * vvir * vvir;                                                /* C:322 */
  const double lambda = get_metaldependent_cooling_rate(log10(temp), log10(zcgm / cgm)); /* :329 */
  double x = PROTONMASS * BOLTZMANN * temp / lambda;                                     /* C:332 */
  x /= (T_UNIT_DENSITY * T_UNIT_TIME);                                                   /* C:333 */
  const double alpha = 1.4, r0 = 0.1 * rvir, ratio = 10.0;                           /* C:336-338 */
  const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);                /* C:341 */
  const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha); /* C:342 */
  const double rho0 = cgm / (4.0 * M_PI * r0 * r0 * r0 * I_M);                       /* C:345 */
  const double rho_eff = rho0 * (I_cool / I_M);                                      /* C:346 */
  const double tcool = (x / rho_eff) * (1.5 * 0.59);                                 /* C:349 */
  const double g_accel = T_G * mvir / (rvir * rvir);                                 /* C:352 */
  const double tff = sqrt(2.0 * rvir / g_accel);                                     /* C:353 */
  const double rcool = rvir;                                                         /* C:360 */
  double cooling = (cgm / (tcool + tff)) * dt;                                       /* C:364 */
  TEST_ASSERT(cooling > cgm, "precondition: the CGM recipe caps its mass to CGMgas");
  cooling = cgm; /* C:366-367 */

  /* do_AGN_heating_cgm (C:635-680) with agn_accretion_compute's Bondi branch (C:526-576) */
  if (rheat < rcool && !(rheat >= 0.99 * rcool)) { /* C:638-643 */
    cooling = (1.0 - rheat / rcool) * cooling;
  } else {
    cooling = 0.0;
  }
  const double bh_mass = bh;
  double rate = (2.5 * M_PI * T_G) * (0.375 * 0.6 * x) * bh_mass * efficiency; /* C:540 */
  const double edd = (1.3e38 * bh_mass * 1e10 / T_HUBBLE) / (T_UNIT_ENERGY / T_UNIT_TIME) /
                     (0.1 * 9.0e10); /* C:553-556 */
  if (rate > edd)                    /* C:557 */
    rate = edd;
  double accreted = rate * dt; /* C:559 */
  if (accreted > cgm)          /* C:560 */
    accreted = cgm;
  double heating = 0.0;
  const double vvir_d = vvir;
  if (vvir_d > 0.0) { /* C:562-572 */
    const double coeff = (1.34e5 / vvir_d) * (1.34e5 / vvir_d);
    heating = coeff * accreted;
    if (heating > cooling && coeff > 0.0) {
      accreted = cooling / coeff;
      heating = cooling;
    }
  }
  const double z_pre = (cgm > 0.0 && zcgm > 0.0) ? fmin((double)zcgm / cgm, 1.0) : 0.0; /* :653 */
  const float cgm_pre = cgm;
  bh += accreted;                       /* C:654 */
  cgm -= accreted;                      /* C:655 */
  zcgm -= z_pre * accreted;             /* C:656 */
  if (rheat < rcool && cooling > 0.0) { /* C:658-662 */
    const double rheat_new = (heating / cooling) * rcool;
    if (rheat_new > rheat)
      rheat = rheat_new;
  }
  if (rheat > rvir) /* C:663-664 */
    rheat = rvir;
  if (cooling > cgm) /* C:675-677 */
    cooling = cgm;
  TEST_ASSERT(accreted > 0.0 && cgm < cgm_pre && cooling == (double)cgm,
              "precondition: the AGN drew CGM gas and the returned mass is the post-AGN CGMgas");

  /* the CGM application (C:475-483) */
  if (cooling > cgm) /* C:476-478 */
    cooling = cgm;
  const double z_post = (cgm > 0.0 && zcgm > 0.0) ? fmin((double)zcgm / cgm, 1.0) : 0.0; /* :479 */
  const float zcgm_post_agn = zcgm;
  cold += cooling;           /* C:480 */
  zcold += z_post * cooling; /* C:481 */
  cgm -= cooling;            /* C:482 */
  zcgm -= z_post * cooling;  /* C:483 */

  /* What the pre-AGN metallicity would leave (mutants a and c) */
  float zcgm_with_pre_z = zcgm_post_agn;
  zcgm_with_pre_z -= z_pre * cooling;
  TEST_ASSERT(zcgm_with_pre_z != zcgm, "precondition: the pre-AGN metallicity leaves other metals");
  TEST_ASSERT(z_post > 0.0, "precondition: the hot pair's metallicity (0) credits other metals");

  TEST_ASSERT(gal.CGMgas == cgm && cgm == 0.0f, "the application empties the post-AGN CGMgas");
  TEST_ASSERT(gal.MetalsCGMgas == zcgm, "the CGM metals debited at the post-AGN metallicity");
  TEST_ASSERT(gal.ColdGas == cold, "ColdGas gains the post-AGN CGM mass (C:480)");
  TEST_ASSERT(gal.MetalsColdGas == zcold, "MetalsColdGas gains the CGM pair's metals (C:481)");
  TEST_ASSERT(gal.BlackHoleMass == bh && gal.Rheat == rheat, "the CGM AGN's accretion and Rheat");
  TEST_ASSERT(gal.CoolingRate == expected_rate(cooling, dt), "CoolingRate of the applied mass");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_equality_metals
 * @brief   The CGM-on application has no "move all metals on equality" branch (C:475-496)
 *
 * With AGN off and a long dt both masses equal their whole reservoirs. HotGas 0.7 carries metals
 * 0.2, a metallicity not exact in double, so metallicity * HotGas differs from MetalsHotGas and
 * the debit leaves a residual (the classic transfer, C:700-704, would zero it and credit all
 * 0.2). CGMgas 0.25 carries metals 0.4, above the gas, so the metallicity caps at 1
 * (get_metallicity, R/src/model_misc.c:427-437) and only 0.25 of the metals moves.
 *
 * Expected: every reservoir as SAGE26's statements written inline give, with the compound
 * assignments' types (C:479-483, :491-495), evaluated at run time (opaque()) so that the inline
 * metal credits and debits contract as the module's do.
 */
int test_equality_metals(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  setup_galaxy(&halo, &gal, 1);
  const float hot = 0.7f, zhot = 0.2f, cgm = 0.25f, zcgm = 0.4f;
  gal.HotGas = hot;
  gal.MetalsHotGas = zhot;
  gal.CGMgas = cgm;
  gal.MetalsCGMgas = zcgm;
  gal.ColdGas = 1.0f;
  gal.MetalsColdGas = 0.01f;
  struct Sage26RegimeAwareCoolingTrace t;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, 1.0e12, &t) == 0,
              "the transaction succeeds");
  TEST_ASSERT(t.hot.cap == MIMIC_SAGE_CAP_RESERVOIR && t.cgm.cap == MIMIC_SAGE_CAP_RESERVOIR,
              "precondition: each mass equals its whole reservoir");

  const double z_cgm = 1.0; /* get_metallicity: 0.4 / 0.25 >= 1 caps at 1 */
  const double z_hot = (double)opaque(zhot) / (double)opaque(hot);
  float cold = opaque(1.0f), metals_cold = opaque(0.01f), metals_cgm = opaque(zcgm),
        metals_hot = opaque(zhot);
  cold += (double)cgm;                /* C:480 */
  metals_cold += z_cgm * (double)cgm; /* C:481 */
  metals_cgm -= z_cgm * (double)cgm;  /* C:483 */
  cold += (double)hot;                /* C:492 */
  metals_cold += z_hot * (double)hot; /* C:493 */
  metals_hot -= z_hot * (double)hot;  /* C:495 */
  TEST_ASSERT(metals_hot != 0.0f, "precondition: the hot debit leaves a residual");
  TEST_ASSERT(gal.HotGas == 0.0f && gal.CGMgas == 0.0f, "both reservoirs are consumed");
  TEST_ASSERT(gal.MetalsHotGas == metals_hot, "the hot metals keep the residual (no equality "
                                              "branch)");
  TEST_ASSERT(gal.MetalsCGMgas == metals_cgm && metals_cgm > 0.0f,
              "the CGM metals above the gas stay behind");
  TEST_ASSERT(gal.ColdGas == cold && gal.MetalsColdGas == metals_cold,
              "the cold credits, CGM then hot");
  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_diagnostics_reset_survival_restoration
 * @brief   Every diagnostic path of the wrapper (C:436-469, reset C:413-418)
 *
 * Expected, case by case:
 *   A  Regime 0, no CGM gas: reset (tcool 0, tff -1, RcoolToRvir -1)
 *   B  Regime 0, CGM gas, Vvir 0 (the CGM guard fails): the stale values survive
 *   C  Regime 0, CGM gas with a NaN cooling function (lambda > 0 fails): they survive
 *   D  Regime 1, hot gas and CGM gas: the hot tcool restored over the CGM value, tff -1, the hot
 *      RcoolToRvir
 *   E  Regime 1, no hot gas, CGM gas: the CGM tcool and tff, RcoolToRvir untouched
 *   F  Regime 1, hot gas with Vvir 0 (hot diagnostics invalid), no CGM gas: reset
 *   G  Regime 1, hot gas, no CGM gas: no reset; the hot tcool, tff -1, the hot RcoolToRvir
 *   H  Regime 1, no hot gas, CGM gas, Vvir 0: no reset; the stale values survive
 */
int test_diagnostics_reset_survival_restoration(void) {
  TEST_ASSERT(load_tables() == 0, "cooling tables load");
  struct Halo halo;
  struct GalaxyData gal;
  const double dt = 1.0e-4;
  const float hot_tcool = expected_hot_tcool(0.2f, 150.0f);
  const float hot_ratio = (float)(expected_hot_rcool(10.0f, 0.2f, 0.2f, 150.0f) / 0.2f); /* :175 */
  float cgm_tcool, cgm_tff;
  expected_cgm_diagnostics(5.0f, 0.05f, 100.0f, 0.2f, 150.0f, &cgm_tcool, &cgm_tff);

  /* A */
  setup_galaxy(&halo, &gal, 0);
  gal.CGMgas = 0.0f;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "A");
  TEST_ASSERT(gal.tcool == 0.0f && gal.tff == -1.0f && gal.RcoolToRvir == -1.0f, "A: reset");

  /* B */
  setup_galaxy(&halo, &gal, 0);
  halo.Vvir = 0.0;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "B");
  TEST_ASSERT(gal.tcool == -11.0f && gal.tff == -22.0f && gal.RcoolToRvir == -33.0f,
              "B: a failed Vvir guard does not reset");
  TEST_ASSERT(gal.CoolingRate == 0.0f, "B: CoolingRate is overwritten");

  /* C */
  setup_galaxy(&halo, &gal, 0);
  gal.CGMgas = INFINITY;
  gal.MetalsCGMgas = INFINITY;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "C");
  TEST_ASSERT(gal.tcool == -11.0f && gal.tff == -22.0f && gal.RcoolToRvir == -33.0f,
              "C: a failed lambda guard does not reset");

  /* D */
  setup_galaxy(&halo, &gal, 1);
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "D");
  TEST_ASSERT(cgm_tcool != hot_tcool, "D: precondition, the CGM and hot values differ");
  TEST_ASSERT(gal.tcool == hot_tcool && gal.tff == -1.0f && gal.RcoolToRvir == hot_ratio,
              "D: the saved hot tcool is restored with tff -1 after CGM cooling");

  /* E */
  setup_galaxy(&halo, &gal, 1);
  gal.HotGas = 0.0f;
  gal.MetalsHotGas = 0.0f;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "E");
  TEST_ASSERT(gal.tcool == cgm_tcool && gal.tff == cgm_tff && gal.RcoolToRvir == -33.0f,
              "E: the CGM diagnostics, RcoolToRvir not written");

  /* F */
  setup_galaxy(&halo, &gal, 1);
  halo.Vvir = 0.0;
  gal.CGMgas = 0.0f;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "F");
  TEST_ASSERT(gal.tcool == 0.0f && gal.tff == -1.0f && gal.RcoolToRvir == -1.0f,
              "F: invalid hot diagnostics and no CGM gas reset");

  /* G */
  setup_galaxy(&halo, &gal, 1);
  gal.CGMgas = 0.0f;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "G");
  TEST_ASSERT(gal.tcool == hot_tcool && gal.tff == -1.0f && gal.RcoolToRvir == hot_ratio,
              "G: valid hot diagnostics are not reset");

  /* H */
  setup_galaxy(&halo, &gal, 1);
  halo.Vvir = 0.0;
  gal.HotGas = 0.0f;
  TEST_ASSERT(sage26_regime_aware_cooling_run(&halo, &MimicConfig, &AGN_OFF, dt, NULL) == 0, "H");
  TEST_ASSERT(gal.tcool == -11.0f && gal.tff == -22.0f && gal.RcoolToRvir == -33.0f,
              "H: CGM gas with a failed guard in the hot regime does not reset");

  sage26_calculate_cooling_budget_cleanup();
  return TEST_PASS;
}

// ============================================================================
// THE MODULE: BOUNDARY, REFUSALS AND THE CONFIGURATION GUARD
// ============================================================================

/** @brief Initialise the budget, radio and transaction with CGMrecipeOn 1 (the run's phases) */
static int init_cgm_on_modules(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  test_pre_timestep_add("sage26_determine_regime", PROCESSING_MODE_FULL_HALO);
  add_run_file_cooling_phase(1);
  set_transaction_parameters("1", "6.0e11");
  int status = sage26_calculate_cooling_budget_init();
  status |= sage26_radio_mode_heating_init();
  status |= sage26_regime_aware_cooling_init();
  set_run_units();
  return status;
}

static void free_phases(void) {
  test_free_substep_phases();
  test_free_pre_timestep();
}

/**
 * @test    test_initial_boundary_matches_classic
 * @brief   No positive interval (stored SnapNum -1, dT -1; MIMIC_OBJECT_TIME_SKIP_INITIAL)
 *
 * SAGE26 is undefined there (Age[-1], R/src/core_build_model.c:479). The classic trio with
 * CGMrecipeOn 0 writes only its no-cooling transport there; with CGMrecipeOn 1 the budget, radio,
 * apply and transaction together must write exactly the same, and read no table: Snaplistlen 0
 * makes any ZZ read a D35 refusal.
 *
 * Expected: every galaxy byte equal between the two paths, and every process() returns 0.
 */
int test_initial_boundary_matches_classic(void) {
  struct Halo halo_off, halo_on;
  struct GalaxyData gal_off, gal_on;
  struct ModuleContext ctx_off, ctx_on;

  /* CGMrecipeOn 0: the classic trio */
  TEST_ASSERT(load_tables() == 0, "cooling tables load (CGMrecipeOn 0)");
  sage26_test_append_model_parameter("AGNrecipe", "2");
  sage26_test_append_model_parameter("RadioModeEfficiency", "0.08");
  TEST_ASSERT(sage26_radio_mode_heating_init() == 0, "radio init");
  MimicConfig.Snaplistlen = 0;
  setup_galaxy(&halo_off, &gal_off, 1);
  halo_off.SnapNum = -1;
  halo_off.dT = -1.0;
  setup_ctx(&ctx_off, &halo_off);
  TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx_off, &halo_off, 1) == 0, "budget");
  TEST_ASSERT(sage26_radio_mode_heating_process(&ctx_off, &halo_off, 1) == 0, "radio");
  TEST_ASSERT(sage26_apply_cooling_process(&ctx_off, &halo_off, 1) == 0, "apply");
  sage26_calculate_cooling_budget_cleanup();

  /* CGMrecipeOn 1: the same three, then the transaction */
  TEST_ASSERT(init_cgm_on_modules() == 0, "CGMrecipeOn 1 modules initialise");
  MimicConfig.Snaplistlen = 0;
  setup_galaxy(&halo_on, &gal_on, 1);
  halo_on.SnapNum = -1;
  halo_on.dT = -1.0;
  setup_ctx(&ctx_on, &halo_on);
  TEST_ASSERT(sage26_calculate_cooling_budget_process(&ctx_on, &halo_on, 1) == 0, "budget");
  TEST_ASSERT(sage26_radio_mode_heating_process(&ctx_on, &halo_on, 1) == 0, "radio");
  TEST_ASSERT(sage26_apply_cooling_process(&ctx_on, &halo_on, 1) == 0, "apply");
  TEST_ASSERT(sage26_regime_aware_cooling_process(&ctx_on, &halo_on, 1) == 0,
              "the transaction reads no table and succeeds");

  TEST_ASSERT(gal_on.CoolingReturnPath == SAGE_COOLING_PATH_NOT_EVALUATED &&
                  gal_on.CoolingGas == 0.0 && gal_on.CoolingX == 0.0 && gal_on.Rcool == 0.0,
              "the no-cooling transport");
  TEST_ASSERT(memcmp(&gal_on, &gal_off, sizeof(gal_on)) == 0,
              "CGMrecipeOn 1 writes exactly what the classic trio writes");

  sage26_regime_aware_cooling_cleanup();
  sage26_calculate_cooling_budget_cleanup();
  free_phases();
  return TEST_PASS;
}

/**
 * @test    test_d35_refusal_with_positive_interval
 * @brief   A hot-regime galaxy whose stored SnapNum is outside ZZ is refused (campaign decision
 *          D35), with dT > 0
 *
 * Expected: process() returns -1 for the stored SnapNum 64 (Snaplistlen 64), where SAGE26 reads
 * ZZ out of bounds (C:205); the same galaxy at SnapNum 40 succeeds.
 */
int test_d35_refusal_with_positive_interval(void) {
  struct Halo halo;
  struct GalaxyData gal;
  struct ModuleContext ctx;
  TEST_ASSERT(init_cgm_on_modules() == 0, "CGMrecipeOn 1 modules initialise");
  setup_galaxy(&halo, &gal, 1);
  halo.SnapNum = 64;
  setup_ctx(&ctx, &halo);
  TEST_ASSERT(halo.dT > 0.0, "a positive interval");
  TEST_ASSERT(sage26_regime_aware_cooling_process(&ctx, &halo, 1) == -1,
              "the stored SnapNum 64 is refused");
  setup_galaxy(&halo, &gal, 1);
  setup_ctx(&ctx, &halo);
  TEST_ASSERT(sage26_regime_aware_cooling_process(&ctx, &halo, 1) == 0, "SnapNum 40 is accepted");
  sage26_regime_aware_cooling_cleanup();
  sage26_calculate_cooling_budget_cleanup();
  free_phases();
  return TEST_PASS;
}

/**
 * @test    test_parameter_refusals
 * @brief   CGMrecipeOn and MShockMsun are required and validated (campaign decision D33)
 *
 * Expected: init() fails without CGMrecipeOn, with CGMrecipeOn 2, without MShockMsun, and with
 * MShockMsun 0, -1, nan or inf; it succeeds with CGMrecipeOn 0 and MShockMsun 6.0e11.
 */
int test_parameter_refusals(void) {
  reset_config();
  set_transaction_parameters(NULL, "6.0e11");
  TEST_ASSERT(sage26_regime_aware_cooling_init() != 0, "a missing CGMrecipeOn is refused");
  reset_config();
  set_transaction_parameters("2", "6.0e11");
  TEST_ASSERT(sage26_regime_aware_cooling_init() != 0, "CGMrecipeOn 2 is refused");
  reset_config();
  set_transaction_parameters("0", NULL);
  TEST_ASSERT(sage26_regime_aware_cooling_init() != 0, "a missing MShockMsun is refused");
  const char *const bad[] = {"0", "-1.0", "nan", "inf"};
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    reset_config();
    set_transaction_parameters("0", bad[k]);
    TEST_ASSERT(sage26_regime_aware_cooling_init() != 0, "a non-positive or non-finite MShockMsun");
  }
  reset_config();
  set_transaction_parameters("0", "6.0e11");
  TEST_ASSERT(sage26_regime_aware_cooling_init() == 0, "the run files' values are accepted");
  sage26_regime_aware_cooling_cleanup();
  return TEST_PASS;
}

/**
 * @brief Run module_system_init() on a phase configuration, capturing the log
 *
 * @param cgm_on           CGMrecipeOn
 * @param with_classifier  sage26_determine_regime in pre_timestep
 * @param layout           0 no transaction, 1 the run files' order, 2 the transaction before apply
 * @param log_out          Receives the captured log (static buffer)
 * @return module_system_init()'s result
 */
static int init_configuration(const int cgm_on, const int with_classifier, const int layout,
                              char *log_out, const size_t log_len) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  MimicConfig.SubSteps = 1;
  if (with_classifier) {
    test_pre_timestep_add("sage26_determine_regime", PROCESSING_MODE_FULL_HALO);
  }
  if (layout == 2) {
    test_phase_add("galaxy_physics", "sage26_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY);
    test_phase_add("galaxy_physics", "sage26_radio_mode_heating", PROCESSING_MODE_BY_GALAXY);
    test_phase_add("galaxy_physics", "sage26_regime_aware_cooling", PROCESSING_MODE_BY_GALAXY);
    test_phase_add("galaxy_physics", "sage26_apply_cooling", PROCESSING_MODE_BY_GALAXY);
  } else {
    add_run_file_cooling_phase(layout == 1);
  }
  set_test_model_parameters();
  sage26_test_append_model_parameter("CGMrecipeOn", cgm_on ? "1" : "0");
  sage26_test_append_model_parameter("MShockMsun", "6.0e11");

  FILE *capture = tmpfile();
  FILE *old_out = set_log_output(capture);
  const int result = module_system_init();
  set_log_output(old_out);
  rewind(capture);
  const size_t n = fread(log_out, 1, log_len - 1, capture);
  log_out[n] = '\0';
  fclose(capture);
  if (result == 0) {
    module_system_cleanup();
  }
  free_phases();
  check_memory_leaks();
  return result;
}

/**
 * @test    test_guard_missing_transaction
 * @brief   CGMrecipeOn 1 without sage26_regime_aware_cooling: the budget refuses, naming it
 */
int test_guard_missing_transaction(void) {
  static char log[8192];
  TEST_ASSERT(init_configuration(1, 1, 0, log, sizeof(log)) != 0, "initialisation fails");
  TEST_ASSERT(strstr(log, "requires sage26_regime_aware_cooling") != NULL,
              "the missing transaction is named");
  return TEST_PASS;
}

/**
 * @test    test_guard_missing_classifier
 * @brief   CGMrecipeOn 1 without sage26_determine_regime: the budget refuses, naming it
 */
int test_guard_missing_classifier(void) {
  static char log[8192];
  TEST_ASSERT(init_configuration(1, 0, 1, log, sizeof(log)) != 0, "initialisation fails");
  TEST_ASSERT(strstr(log, "requires sage26_determine_regime") != NULL,
              "the missing classifier is named");
  return TEST_PASS;
}

/**
 * @test    test_guard_misplaced_transaction
 * @brief   CGMrecipeOn 1 with the transaction before sage26_apply_cooling: it refuses
 */
int test_guard_misplaced_transaction(void) {
  static char log[8192];
  TEST_ASSERT(init_configuration(1, 1, 2, log, sizeof(log)) != 0, "initialisation fails");
  TEST_ASSERT(strstr(log, "must run immediately after sage26_apply_cooling") != NULL,
              "the misplacement is named");
  return TEST_PASS;
}

/**
 * @test    test_guard_complete_configuration
 * @brief   CGMrecipeOn 1 with the classifier and the run files' cooling phase initialises
 */
int test_guard_complete_configuration(void) {
  static char log[8192];
  TEST_ASSERT(init_configuration(1, 1, 1, log, sizeof(log)) == 0, "initialisation succeeds");
  return TEST_PASS;
}

/**
 * @test    test_guard_inactive_with_cgm_off
 * @brief   With CGMrecipeOn 0 none of the checks applies
 *
 * Expected: the run files' configuration initialises with and without the transaction, and so
 * do the misplaced transaction and a configuration without the classifier.
 */
int test_guard_inactive_with_cgm_off(void) {
  static char log[8192];
  TEST_ASSERT(init_configuration(0, 1, 1, log, sizeof(log)) == 0, "with the transaction");
  TEST_ASSERT(init_configuration(0, 1, 0, log, sizeof(log)) == 0, "without the transaction");
  TEST_ASSERT(init_configuration(0, 1, 2, log, sizeof(log)) == 0, "the transaction misplaced");
  TEST_ASSERT(init_configuration(0, 0, 1, log, sizeof(log)) == 0, "without the classifier");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_regime_aware_cooling\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  TEST_RUN(test_cgm_recipe_off_writes_nothing);
  TEST_RUN(test_mdot_reset_and_rate_overwrite);
  TEST_RUN(test_stream_rates_stored_raw);
  TEST_RUN(test_hot_agn_before_cgm_cooling);
  TEST_RUN(test_cgm_applied_before_hot);
  TEST_RUN(test_hot_apply_fresh_cap);
  TEST_RUN(test_cgm_apply_after_cgm_agn);
  TEST_RUN(test_equality_metals);
  TEST_RUN(test_diagnostics_reset_survival_restoration);
  TEST_RUN(test_initial_boundary_matches_classic);
  TEST_RUN(test_d35_refusal_with_positive_interval);
  TEST_RUN(test_parameter_refusals);
  TEST_RUN(test_guard_missing_transaction);
  TEST_RUN(test_guard_missing_classifier);
  TEST_RUN(test_guard_misplaced_transaction);
  TEST_RUN(test_guard_complete_configuration);
  TEST_RUN(test_guard_inactive_with_cgm_off);

  TEST_SUMMARY();
  return TEST_RESULT();
}
