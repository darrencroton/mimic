/**
 * @file    test_unit_sage_ffb_shared_helpers.c
 * @brief   Unit tests of the shared helpers: the FFB feedback budget
 *          (shared/sage_ffb_feedback_physics.h), the H I block (shared/sage_hi_physics.h) and the
 *          disk yield (shared/sage_metal_yield_physics.h)
 *
 * R = the staged SAGE26 source, S = R/src/model_starformation_and_feedback.c, X =
 * R/src/model_misc.h. Every expectation is derived independently of the helper under test: a
 * literal from inputs chosen so the result is exact, or SAGE26's expression written here in its
 * operand order with its lines cited, in separate statements where SAGE26 has them (so no
 * contraction can differ); never by calling the helper under test. Platform independent: libm's
 * pow, exp and log are called by the helper and by the expectation alike.
 *
 * FFB feedback budget (compute_sn_feedback_ffb, S:901-972):
 * - test_ffb_feedback_valid_capped: a valid scaling (Vvir 100 km/s, z 1) above 60 km/s: the
 *   division form of the velocity term, the eta cap binding, MassLoading written as the float
 *   eta, reheated = eta * stars, no renormalisation, E_FB below E_lift: no ejection
 * - test_ffb_feedback_valid_ejects: the same with the eta cap not binding: E_FB above E_lift ejects
 * - test_ffb_feedback_low_velocity_floor: Vvir 0.5 km/s: the velocity floored at 1 for the scaling
 *   (pow(1/60, -3.2)), the unfloored Vvir in the eta cap and the ejection, the coupling capped
 * - test_ffb_feedback_invalid_vvir: Vvir 0 and -5: MassLoading keeps its seeded value, reheated 0,
 *   no ejection, stars unchanged
 * - test_ffb_feedback_invalid_redshift: z -0.5 with Vvir 100: the same, although Vvir > 0
 * - test_ffb_feedback_renormalised: stars + reheated above the cold gas the caller passes: both
 *   scaled by ColdGas / (stars + reheated) (S:943-947), the ejection from the scaled values
 * - test_ffb_feedback_renorm_second_guard: cold gas -1, stars 0, an invalid scaling: the first
 *   guard holds and the second (stars + reheated > 0) does not, so nothing is scaled
 * H I block (S:1035-1046, S:1280-1292, S:130-152):
 * - test_hi_general_and_writes_only_h1: the ionisation cut's general expression; only H1gas written
 * - test_hi_paths: a disk below the cut (f_ion 1), non-positive inputs (f_ion 0), H2 above the
 *   cold hydrogen (atomicH floored at 0)
 * - test_hi_ffb_zero_h2: with H2gas +0 the block equals the plain multiply ColdGas * 0.74 (S:1285)
 * Disk yield (S:1092-1109, S:1353-1362):
 * - test_yield_above_floor_routing: the retained metals to the cold gas, the escaping metals to the
 *   central's selected metals by CGMrecipeOn and the CENTRAL's Regime, with retained and escaping
 *   amounts that differ (FracZleaveDisk 0.25), the central's float Mvir in the scale, the stars the
 *   argument (NewStellarMass not read or written)
 * - test_yield_below_floor: ColdGas at or below 1e-8: all of Yield * stars to the central's
 * selected metals, the cold metals untouched
 * - test_yield_fused_retained: the retained metals are one fused multiply-add (S:1097, S:1356) on
 *   an input (a cancelling negative metal reservoir) where the unfused form differs
 */

#include "../../../../tests/framework/test_framework.h"
#include "include/types.h"
#include "shared/sage_ffb_feedback_physics.h"
#include "shared/sage_hi_physics.h"
#include "shared/sage_metal_yield_physics.h"
#include "util/error.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Literal code constants for the budget tests: EtaSNcode 2 with EnergySNcode 1000 (the eta cap
 * binds at 100 km/s) or 1e5 (it does not) */
#define TEST_ETA_SN_CODE 2.0
#define TEST_ENERGY_SN_CODE_LOW 1000.0
#define TEST_ENERGY_SN_CODE_HIGH 1.0e5
#define TEST_EPSILON 2.9
#define TEST_EJECTION_EFFICIENCY 0.3
#define SEEDED_MASS_LOADING 123.25f

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }
static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// ============================================================================
// FFB FEEDBACK BUDGET
// ============================================================================

/** SAGE26's compute_sn_feedback_ffb with FIREmodeOn 1, written out (S:905-967, X:118-149) */
struct ffb_expected {
  double stars, reheated, ejected;
  float mass_loading;
};

static struct ffb_expected expected_ffb(const float vvir, const double z, const float cold_gas,
                                        const double stars_in, const float mass_loading_in,
                                        const double energy_sn) {
  struct ffb_expected e = {stars_in, 0.0, 0.0, mass_loading_in};
  double fire_scaling = 0.0;
  int valid = 0;
  const double vc = vvir;                            /* S:916 */
  if (vc > 0.0 && z >= 0.0) {                        /* S:917 */
    const double vc_floored = (vc < 1.0) ? 1.0 : vc; /* S:918 */
    const double z_term = pow(1.0 + z, 1.25);        /* S:919 */
    /* S:920-921; pow(x, -1.0) in the form SAGE26's object computes, 1.0 / x */
    const double v_term =
        (vc_floored < 60.0) ? pow(vc_floored / 60.0, -3.2) : 1.0 / (vc_floored / 60.0);
    fire_scaling = z_term * v_term; /* S:922 */
    valid = 1;
  }
  if (valid) {
    const double eta_raw = TEST_EPSILON * fire_scaling; /* S:931 */
    double eta = eta_raw;                               /* X:144-146: vvir > 0 here */
    if (vvir > 0.0) {
      const double esn = TEST_ETA_SN_CODE * energy_sn;                  /* X:147 */
      const double eta_max = 2.0 * esn / ((double)vvir * (double)vvir); /* X:148 */
      eta = (eta_raw > eta_max) ? eta_max : eta_raw;                    /* X:149 */
    }
    e.mass_loading = (float)eta; /* S:932 */
    e.reheated = eta * e.stars;  /* S:933 */
  }
  if ((e.stars + e.reheated) > cold_gas && (e.stars + e.reheated) > 0.0) { /* S:943 */
    const double fac = cold_gas / (e.stars + e.reheated);                  /* S:944 */
    e.stars *= fac;
    e.reheated *= fac;
  }
  if (vvir > 0.0 && valid) {                                   /* S:950-952 */
    double coupling = TEST_EJECTION_EFFICIENCY * fire_scaling; /* X:120 */
    if (coupling > 2.0) {
      coupling = 2.0; /* X:121-123 */
    }
    const double E_FB = coupling * 0.5 * e.stars * (TEST_ETA_SN_CODE * energy_sn); /* S:954-956 */
    const double E_lift = 0.5 * e.reheated * vc * vc;                              /* S:957 */
    e.ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;         /* S:958 */
  }
  if (e.ejected < 0.0) {
    e.ejected = 0.0; /* S:967 */
  }
  return e;
}

/** @brief Run the helper and compare every output bit for bit with the written-out budget */
static int run_and_compare(const float vvir, const double z, const float cold_gas,
                           const double stars_in, const double energy_sn,
                           struct MimicSageFfbFeedbackLocals *l, struct ffb_expected *got) {
  float mass_loading = SEEDED_MASS_LOADING;
  double stars = stars_in, reheated = -1.0, ejected = -1.0;
  mimic_sage_ffb_sn_feedback(vvir, z, cold_gas, TEST_EPSILON, TEST_EJECTION_EFFICIENCY,
                             TEST_ETA_SN_CODE, energy_sn, &stars, &reheated, &ejected,
                             &mass_loading, l);
  const struct ffb_expected e =
      expected_ffb(vvir, z, cold_gas, stars_in, SEEDED_MASS_LOADING, energy_sn);
  got->stars = stars;
  got->reheated = reheated;
  got->ejected = ejected;
  got->mass_loading = mass_loading;
  return same_f64(stars, e.stars) && same_f64(reheated, e.reheated) &&
         same_f64(ejected, e.ejected) && same_f32(mass_loading, e.mass_loading);
}

static int test_ffb_feedback_valid_capped(void) {
  struct MimicSageFfbFeedbackLocals l;
  struct ffb_expected got;
  TEST_ASSERT(run_and_compare(100.0f, 1.0, 1000.0f, 0.5, TEST_ENERGY_SN_CODE_LOW, &l, &got),
              "valid scaling at 100 km/s: every output equals SAGE26's expressions bit for bit");
  TEST_ASSERT(l.fire.scaling_valid == 1 && l.fire.high_velocity == 1,
              "the scaling is valid and takes the high-velocity term");
  TEST_ASSERT(l.mass_loading_written == 1, "MassLoading is written with a valid scaling");
  /* eta_max = 2 * 2000 / 100^2 = 0.4, below 2.9 * scaling */
  TEST_ASSERT(l.fire.eta_cap_binds == 1 && same_f64(l.fire.eta, 0.4) && got.mass_loading == 0.4f,
              "the eta cap binds at 0.4 and MassLoading is its float");
  TEST_ASSERT(same_f64(got.reheated, 0.4 * 0.5), "reheated = eta * stars");
  TEST_ASSERT(l.renorm_binds == 0 && same_f64(got.stars, 0.5), "no renormalisation");
  /* E_FB = 0.3 * scaling * 0.5 * 0.5 * 2000 is about 214, E_lift = 0.5 * 0.2 * 100^2 = 1000 */
  TEST_ASSERT(l.ejection_evaluated == 1 && l.fire.energy_ejects == 0 && same_f64(got.ejected, 0.0),
              "E_FB below E_lift ejects nothing");
  return TEST_PASS;
}

static int test_ffb_feedback_valid_ejects(void) {
  struct MimicSageFfbFeedbackLocals l;
  struct ffb_expected got;
  TEST_ASSERT(run_and_compare(100.0f, 1.0, 1000.0f, 0.5, TEST_ENERGY_SN_CODE_HIGH, &l, &got),
              "valid scaling, uncapped eta: every output equals SAGE26's expressions bit for bit");
  TEST_ASSERT(l.fire.eta_cap_binds == 0 && l.fire.coupling_capped == 0,
              "neither the eta cap (eta_max 40) nor the coupling cap binds");
  TEST_ASSERT(same_f32(got.mass_loading, (float)l.fire.eta) &&
                  got.mass_loading != SEEDED_MASS_LOADING,
              "MassLoading is the float of the uncapped eta");
  /* E_FB = 0.3 * scaling * 0.5 * 0.5 * 2e5 is about 21400, E_lift = 0.5 * 2.07 * 100^2 about 10300
   */
  TEST_ASSERT(l.fire.energy_ejects == 1 && got.ejected > 0.0, "E_FB above E_lift ejects");
  return TEST_PASS;
}

static int test_ffb_feedback_low_velocity_floor(void) {
  struct MimicSageFfbFeedbackLocals l;
  struct ffb_expected got;
  TEST_ASSERT(run_and_compare(0.5f, 2.0, 1000.0f, 0.25, TEST_ENERGY_SN_CODE_LOW, &l, &got),
              "valid scaling at 0.5 km/s: every output equals SAGE26's expressions bit for bit");
  TEST_ASSERT(l.fire.vc_floored == 1 && l.fire.high_velocity == 0,
              "the scaling's velocity is floored at 1 km/s and takes the -3.2 slope");
  TEST_ASSERT(l.fire.coupling_capped == 1 && same_f64(l.fire.coupling, 2.0),
              "the coupling is capped at 2 (X:121-123)");
  TEST_ASSERT(l.fire.eta_cap_vvir_nonpositive == 0 && same_f64(l.fire.eta_max, 2.0 * 2000.0 / 0.25),
              "the eta cap uses the unfloored Vvir 0.5 (X:148)");
  return TEST_PASS;
}

static int test_ffb_feedback_invalid_vvir(void) {
  const float velocities[2] = {0.0f, -5.0f};
  for (int k = 0; k < 2; k++) {
    struct MimicSageFfbFeedbackLocals l;
    struct ffb_expected got;
    TEST_ASSERT(
        run_and_compare(velocities[k], 1.0, 1000.0f, 0.5, TEST_ENERGY_SN_CODE_HIGH, &l, &got),
        "invalid scaling (Vvir <= 0): outputs equal SAGE26's expressions");
    TEST_ASSERT(same_f32(got.mass_loading, SEEDED_MASS_LOADING) && l.mass_loading_written == 0,
                "SAGE parity: an invalid scaling leaves MassLoading stale (S:929-934)");
    TEST_ASSERT(same_f64(got.reheated, 0.0) && same_f64(got.ejected, 0.0) &&
                    same_f64(got.stars, 0.5),
                "no reheating, no ejection, the stars unchanged");
    TEST_ASSERT(l.fire.scaling_valid == 0 && l.ejection_evaluated == 0, "no FIRE scaling");
  }
  return TEST_PASS;
}

static int test_ffb_feedback_invalid_redshift(void) {
  struct MimicSageFfbFeedbackLocals l;
  struct ffb_expected got;
  TEST_ASSERT(run_and_compare(100.0f, -0.5, 1000.0f, 0.5, TEST_ENERGY_SN_CODE_HIGH, &l, &got),
              "invalid scaling (z < 0, Vvir > 0): outputs equal SAGE26's expressions");
  TEST_ASSERT(same_f32(got.mass_loading, SEEDED_MASS_LOADING),
              "SAGE parity: z < 0 leaves MassLoading stale although Vvir > 0 (S:917)");
  TEST_ASSERT(same_f64(got.reheated, 0.0) && same_f64(got.ejected, 0.0),
              "no reheating and no ejection with a positive Vvir (S:952)");
  return TEST_PASS;
}

static int test_ffb_feedback_renormalised(void) {
  struct MimicSageFfbFeedbackLocals l;
  struct ffb_expected got;
  /* stars 1, eta 0.4 (capped, as above): stars + reheated = 1.4 > ColdGas 0.7 */
  TEST_ASSERT(run_and_compare(100.0f, 1.0, 0.7f, 1.0, TEST_ENERGY_SN_CODE_LOW, &l, &got),
              "renormalised: outputs equal SAGE26's expressions bit for bit");
  TEST_ASSERT(l.renorm_binds == 1 && same_f64(l.renorm_fac, (double)0.7f / (1.0 + 0.4)),
              "fac = ColdGas / (stars + reheated) (S:944)");
  TEST_ASSERT(same_f64(got.stars, 1.0 * l.renorm_fac) && same_f64(got.reheated, 0.4 * l.renorm_fac),
              "stars and reheated scaled by fac (S:945-946)");
  TEST_ASSERT(same_f64(l.stars_in, 1.0) && !same_f64(got.stars, 1.0),
              "the renormalised stars differ from the stars passed");
  return TEST_PASS;
}

static int test_ffb_feedback_renorm_second_guard(void) {
  struct MimicSageFfbFeedbackLocals l;
  struct ffb_expected got;
  TEST_ASSERT(run_and_compare(0.0f, 1.0, -1.0f, 0.0, TEST_ENERGY_SN_CODE_HIGH, &l, &got),
              "second guard: outputs equal SAGE26's expressions");
  TEST_ASSERT(l.renorm_binds == 0 && same_f64(got.stars, 0.0) && same_f64(got.reheated, 0.0),
              "stars + reheated = 0 > ColdGas -1, but not > 0: nothing scaled (S:943)");
  return TEST_PASS;
}

// ============================================================================
// H I BLOCK
// ============================================================================

/** SAGE26's ionized_gas_fraction (S:135-152), written out */
static double expected_f_ion(const double cold, const double rs, const double h) {
  if (cold <= 0.0 || rs <= 0.0 || 0.5 <= 0.0 || h <= 0.0) {
    return 0.0;
  }
  const double mgas = cold * 1.0e10 / h;
  const double rs_pc = rs * 1.0e6 / h;
  const double sigma0 = mgas / (2.0 * M_PI * rs_pc * rs_pc);
  if (sigma0 <= 0.5) {
    return 1.0;
  }
  const double x = log(sigma0 / 0.5);
  double f = (1.0 + x) * 0.5 / sigma0;
  if (f < 0.0) {
    f = 0.0;
  }
  if (f > 1.0) {
    f = 1.0;
  }
  return f;
}

static struct GalaxyData hi_galaxy(const float cold, const float h2, const float rs) {
  struct GalaxyData g;
  memset(&g, 0, sizeof(g));
  g.ColdGas = cold;
  g.H2gas = h2;
  g.DiskScaleRadius = rs;
  g.H1gas = -7.0f;
  g.StellarMass = 3.0f;
  return g;
}

static int test_hi_general_and_writes_only_h1(void) {
  /* ColdGas 1 (so ColdGas * 0.74 is exact, fused or not), H2 0.25, r_s 0.004 Mpc/h */
  struct GalaxyData g = hi_galaxy(1.0f, 0.25f, 0.004f);
  struct GalaxyData before = g;
  mimic_sage_calculate_h1gas(&g, 0.73);
  const double atomicH = 1.0 * 0.74 - 0.25; /* S:1036, exact */
  const double f_ion = expected_f_ion(1.0, (double)0.004f * 1.0, 0.73);
  TEST_ASSERT(f_ion > 0.0 && f_ion < 1.0, "the general expression (0 < f_ion < 1)");
  TEST_ASSERT(same_f32(g.H1gas, (float)(atomicH * (1.0 - f_ion))),
              "H1gas = atomicH * (1 - f_ion) narrowed to float (S:1044-1045)");
  before.H1gas = g.H1gas;
  TEST_ASSERT(memcmp(&before, &g, sizeof(g)) == 0, "only H1gas is written");
  return TEST_PASS;
}

static int test_hi_paths(void) {
  /* A disk entirely below the cut: tiny gas, large radius, f_ion 1, H1 0 */
  struct GalaxyData g = hi_galaxy(1.0e-6f, 0.0f, 1.0f);
  mimic_sage_calculate_h1gas(&g, 0.73);
  TEST_ASSERT(expected_f_ion(1.0e-6f, 1.0f, 0.73) == 1.0 && same_f32(g.H1gas, 0.0f),
              "sigma0 <= 0.5: fully ionised, H1gas 0");
  /* A non-positive radius: f_ion 0, H1 = atomicH = 0.5 * 0.74 - 0.125 = 0.245 */
  g = hi_galaxy(0.5f, 0.125f, 0.0f);
  mimic_sage_calculate_h1gas(&g, 0.73);
  TEST_ASSERT(same_f32(g.H1gas, (float)(0.5 * 0.74 - 0.125)), "r_s 0: f_ion 0, H1gas the atomic H");
  /* H2 above the cold hydrogen: atomicH floored at 0 (S:1037-1040) */
  g = hi_galaxy(1.0f, 0.875f, 0.004f);
  mimic_sage_calculate_h1gas(&g, 0.73);
  TEST_ASSERT(same_f32(g.H1gas, 0.0f) && !signbit(g.H1gas), "atomicH < 0 floored at +0");
  return TEST_PASS;
}

static int test_hi_ffb_zero_h2(void) {
  /* SAGE parity: on the FFB path H2gas is +0 and SAGE26's object multiplies (S:1285) */
  const float colds[4] = {0.3f, 1.7f, 12.345f, 3.0e-7f};
  for (int k = 0; k < 4; k++) {
    struct GalaxyData g = hi_galaxy(colds[k], 0.0f, 0.004f);
    mimic_sage_calculate_h1gas(&g, 0.73);
    double atomicH = (double)colds[k] * 0.74; /* S:1285 as the object computes it */
    if (atomicH < 0.0) {
      atomicH = 0.0;
    }
    const double f_ion = expected_f_ion(colds[k], (double)0.004f, 0.73);
    TEST_ASSERT(same_f32(g.H1gas, (float)(atomicH * (1.0 - f_ion))),
                "H2gas +0: H1gas from the plain multiply ColdGas * 0.74");
  }
  return TEST_PASS;
}

// ============================================================================
// DISK YIELD
// ============================================================================

static struct GalaxyData yield_galaxy(const float cold, const int regime) {
  struct GalaxyData g;
  memset(&g, 0, sizeof(g));
  g.ColdGas = cold;
  g.MetalsColdGas = 0.5f;
  g.MetalsHotGas = 0.25f;
  g.MetalsCGMgas = 0.125f;
  g.HotGas = 4.0f;
  g.CGMgas = 2.0f;
  g.Regime = regime;
  g.NewStellarMass = 99.0f;
  return g;
}

static int test_yield_above_floor_routing(void) {
  /* Yield 0.25, FracZleaveDisk 0.25, central Mvir 0 (exp(-0) = 1): FracZleaveDiskVal 0.25,
   * retained 0.25 * 0.75 * 1 = 0.1875, escaping 0.25 * 0.25 * 1 = 0.0625 (S:1094-1100) */
  static const struct {
    int cgm_recipe_on, regime, to_cgm;
  } runs[3] = {{1, 0, 1}, {1, 1, 0}, {0, 0, 0}};
  for (int k = 0; k < 3; k++) {
    struct GalaxyData gal = yield_galaxy(1.0f, 1 - runs[k].regime);
    struct GalaxyData central = yield_galaxy(1.0f, runs[k].regime);
    struct MimicSageDiskYieldLocals l;
    mimic_sage_apply_disk_yield(&gal, &central, 0.0f, 1.0, 0.25, 0.25, runs[k].cgm_recipe_on, &l);
    TEST_ASSERT(gal.MetalsColdGas == 0.5f + 0.1875f, "retained metals to the cold gas");
    TEST_ASSERT(gal.MetalsHotGas == 0.25f && gal.MetalsCGMgas == 0.125f,
                "the forming galaxy's own hot and CGM metals untouched");
    TEST_ASSERT(gal.NewStellarMass == 99.0f, "NewStellarMass neither read nor written");
    if (runs[k].to_cgm) {
      TEST_ASSERT(central.MetalsCGMgas == 0.125f + 0.0625f && central.MetalsHotGas == 0.25f,
                  "CGMrecipeOn 1, central Regime 0: escaping metals to the central's CGM metals");
    } else {
      TEST_ASSERT(central.MetalsHotGas == 0.25f + 0.0625f && central.MetalsCGMgas == 0.125f,
                  "escaping metals to the central's hot metals");
    }
    TEST_ASSERT(central.MetalsColdGas == 0.5f && central.HotGas == 4.0f && central.CGMgas == 2.0f,
                "the central's other fields untouched");
    TEST_ASSERT(l.coldgas_above_threshold == 1 && same_f64(l.frac_z_leave_disk_val, 0.25) &&
                    l.reservoir_cgm == runs[k].to_cgm,
                "locals: above the floor, FracZleaveDiskVal 0.25, the selection");
  }
  /* A positive central Mvir: FracZleaveDiskVal = FracZleaveDisk * exp(-1.0 * Mvir / 30.0) with the
   * float Mvir (S:1094) */
  struct GalaxyData gal = yield_galaxy(1.0f, 0);
  struct GalaxyData central = yield_galaxy(1.0f, 1);
  struct MimicSageDiskYieldLocals l;
  const float mvir = 12.3f;
  mimic_sage_apply_disk_yield(&gal, &central, mvir, 1.0, 0.25, 0.25, 1, &l);
  const double frac = 0.25 * exp(-1.0 * mvir / 30.0);
  const double escaping = 0.25 * frac * 1.0; /* S:1100 */
  TEST_ASSERT(same_f64(l.frac_z_leave_disk_val, frac), "the central's float Mvir in the scale");
  TEST_ASSERT(same_f32(central.MetalsHotGas, (float)((double)0.25f + escaping)),
              "escaping metals = Yield * FracZleaveDiskVal * stars, added as a completed double");
  return TEST_PASS;
}

static int test_yield_below_floor(void) {
  const float colds[2] = {0.0f, 1.0e-9f};
  for (int k = 0; k < 2; k++) {
    struct GalaxyData gal = yield_galaxy(colds[k], 0);
    struct GalaxyData central = yield_galaxy(1.0f, 0);
    struct MimicSageDiskYieldLocals l;
    mimic_sage_apply_disk_yield(&gal, &central, 0.0f, 2.0, 0.25, 0.25, 1, &l);
    TEST_ASSERT(gal.MetalsColdGas == 0.5f, "ColdGas <= 1e-8: the cold metals untouched");
    TEST_ASSERT(central.MetalsCGMgas == 0.125f + 0.5f && central.MetalsHotGas == 0.25f,
                "all metals Yield * stars = 0.5 to the central's selected (CGM) metals (S:1106)");
    TEST_ASSERT(l.coldgas_above_threshold == 0 && isnan(l.frac_z_leave_disk_val),
                "locals: below the floor, no FracZleaveDiskVal");
  }
  return TEST_PASS;
}

static int test_yield_fused_retained(void) {
  /* Yield 0.025, FracZleaveDisk 0, so the retained coefficient is 0.025. With stars = 0.1f / 0.025
   * the exact product is within a double rounding of the float 0.1f, and with MetalsColdGas -0.1f
   * (a constructed edge state: a negative metal reservoir) the sum cancels to that rounding, so the
   * fused MetalsColdGas + 0.025 * stars (one rounding) and the unfused one (the product rounded
   * first) differ after the float store */
  const double yield = 0.025;
  const double stars = (double)0.1f / yield;
  const float metals = -0.1f;
  const double retained = yield * (1.0 - 0.0);
  const float fused = (float)fma(retained, stars, (double)metals); /* S:1097, S:1356 */
  const double product = retained * stars;
  const float unfused = (float)((double)metals + product);
  TEST_ASSERT(!same_f32(fused, unfused), "the input separates the fused and unfused forms");
  struct GalaxyData gal = yield_galaxy(1.0f, 0);
  gal.MetalsColdGas = metals;
  struct GalaxyData central = yield_galaxy(1.0f, 0);
  mimic_sage_apply_disk_yield(&gal, &central, 0.0f, stars, yield, 0.0, 1, NULL);
  TEST_ASSERT(same_f32(gal.MetalsColdGas, fused),
              "SAGE parity: the retained metals are one fused multiply-add (S:1097, S:1356)");
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: FFB feedback, H I block and disk yield (shared helpers)\n");
  printf("============================================================\n");

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_ffb_feedback_valid_capped);
  TEST_RUN(test_ffb_feedback_valid_ejects);
  TEST_RUN(test_ffb_feedback_low_velocity_floor);
  TEST_RUN(test_ffb_feedback_invalid_vvir);
  TEST_RUN(test_ffb_feedback_invalid_redshift);
  TEST_RUN(test_ffb_feedback_renormalised);
  TEST_RUN(test_ffb_feedback_renorm_second_guard);
  TEST_RUN(test_hi_general_and_writes_only_h1);
  TEST_RUN(test_hi_paths);
  TEST_RUN(test_hi_ffb_zero_h2);
  TEST_RUN(test_yield_above_floor_routing);
  TEST_RUN(test_yield_below_floor);
  TEST_RUN(test_yield_fused_retained);

  TEST_SUMMARY();
  return TEST_RESULT();
}
