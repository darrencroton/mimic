/**
 * @file    test_unit_sage26_ffb_star_formation.c
 * @brief   Platform-independent behavioural tests of sage26_ffb_star_formation
 *
 * The oracle test (test_unit_sage26_ffb_star_formation_oracle.c) skips off macOS arm64, so these
 * tests are the transaction's guard on every other platform. R = the staged SAGE26 source, S =
 * R/src/model_starformation_and_feedback.c, X = R/src/model_misc.h.
 *
 * Every expectation is derived independently of the module and of the shared helpers it calls:
 * either a literal from inputs chosen so the result is exact, or SAGE26's starformation_ffb
 * written out in this file (sage26_ffb_written_out: S:1270-1362 with compute_sn_feedback_ffb
 * S:905-967, update_from_star_formation S:1121-1135, update_from_feedback S:1150-1249,
 * ionized_gas_fraction S:135-152, get_metallicity, capped_eta_reheat X:141-149 and
 * sn_energy_coupling X:118-124), statement by statement with SAGE26's operand order and the forms
 * SAGE26's object computes (its fused multiply-adds as fma(), the high-velocity pow as a division),
 * cited line by line. The written-out transaction never calls the package's code. A test runs the
 * module's process() and compares the forming row and its FoF central with the written-out end
 * state byte for byte (memcmp of the whole galaxy struct, so a field the module must not write,
 * such as the instability state or a transport field, is compared too), and asserts the named
 * behaviour with literals or the observer (sage26_ffb_star_formation.h). Where a test targets a
 * misordering, it also shows that its input discriminates: the written-out transaction run with
 * that misordering (a "variant") gives a different end state.
 *
 * Software quality:
 * - test_module_lifecycle: registration, module_system_init in a substep phase, cleanup, no leaks
 * - test_parameters_refused: each missing key (EnhancedStarFormationOn, EnhancedSFEfficiency),
 *   EnhancedStarFormationOn 2, EnhancedSFEfficiency negative, above 1 and NaN, and FIREmodeOn 0 at
 *   EnhancedStarFormationOn 1 are refused naming what they refuse; FIREmodeOn 0 is accepted at 0
 * - test_placement_refused: pre_timestep, post_timestep, twice, and process_full_halo are refused
 *   naming the module; once by galaxy in a substep phase is accepted
 * - test_dispatch_guards: at EnhancedStarFormationOn 1, init refuses, naming the missing or
 *   misplaced module, without sage26_determine_ffb_regime in pre_timestep, and with the transaction
 *   not immediately before sage26_calculate_star_formation (absent, after it, separated from it, it
 *   as process_full_halo, or in another substep phase); the complete configuration initialises; at
 *   EnhancedStarFormationOn 0 every one of those configurations initialises
 * - test_chain_initialises: module_system_init over the run files' pre_timestep classifier and
 *   by-galaxy star-formation chain at EnhancedStarFormationOn 1 succeeds; without the transaction,
 *   without the classifier and with the transaction after sage26_calculate_star_formation it fails
 *   naming the module; at 0 each of those succeeds
 *
 * Behaviour (EnhancedStarFormationOn 1 unless stated):
 * - test_off_writes_nothing: EnhancedStarFormationOn 0 writes nothing on an FFBRegime 1 row
 * - test_non_ffb_row_not_written: an FFBRegime 0 row is not written
 * - test_initial_boundary_and_refusal: the initial-boundary state writes nothing and observes a
 * trace whose every value is its not-formed sentinel; an out-of-range stored SnapNum with dT > 0 is
 *   refused, naming it, with nothing written
 * - test_stars_formed_and_renormalised: the committed stellar mass and the history sample use the
 *   stars formed, the yield the renormalised stars (they differ: the cap at ColdGas binds and the
 *   renormalisation binds)
 * - test_pre_commit_metallicity: the feedback uses the metallicity before the commit, on an input
 *   where the recomputed one changes the end state
 * - test_invalid_fire_scaling: Vvir 0 and, separately, z < 0 with Vvir > 0 leave a seeded
 *   MassLoading and give no reheating or ejection
 * - test_hydrogen: H2gas is zeroed and H1gas is the H I block's; H2DepletionTime_Gyr is not
 *   written; the prescription-1 caps bind
 * - test_guards_and_tdyn: the NaN/infinity guards (ColdGas, Vvir, DiskScaleRadius), tdyn 0 from a
 *   zero radius, a zero Vvir and a negative radius (observed tdyn +0)
 * - test_satellite_routing: a Type 1 row's reheated gas and metals go to its FoF central's pair by
 *   the CENTRAL's Regime (CGMrecipeOn 1 Regime 0 and 1; CGMrecipeOn 0), with metals that are not a
 *   fixed fraction of the gas moved; the row's own hot and CGM pairs untouched
 * - test_no_instability_no_transport: a seeded unstable disk and seeded transport fields are left
 *   as they were
 * - test_reheat_clamp_wiring: on an in-domain state where the clamp of S:1346 binds with positive
 *   cold gas (the inputs of the oracle fixture ffb_reheat_clamp_binds), the end state equals
 *   SAGE26's and the reheated mass passed to the feedback commit (the observer) is the written-out
 *   clamped value; the commit clamps the same argument again, so only this wiring assertion shows
 *   the earlier clamp ran
 * - test_reheat_clamp_negative_gas_probe: an out-of-domain probe (ColdGas -1, where SAGE26 aborts)
 *   asserting the wiring value alone
 * - test_sf_prescription_zero_probe: SFprescription 0 (the commits' caps off) matches SAGE26
 * - test_no_central: with no FoF central H2gas, H1gas and the history are written, nothing else;
 *   the observed budget and commit locals hold their not-formed sentinels
 * - test_chain_leaves_trigger: after the transaction, the rest of the by-galaxy chain in the run
 *   files' order (the five bypassed modules, sage26_quasar_mode and sage26_starburst_feedback by
 *   galaxy) leaves an FFB satellite whose disk is unstable by its masses and radii (SAGE26's
 *   criterion written out, R/src/model_disk_instability.c:54-57), and its central, bytewise as the
 *   transaction left them: UnstableDiskGasFraction keeps its reset value 0 and the quasar and burst
 *   entries write nothing. The same state with FFBRegime 0 sets a positive trigger, so
 *   the input discriminates.
 *
 * Every test that initialises the module at EnhancedStarFormationOn 1 configures the complete
 * dispatch the cross-module guards require (sage26_determine_ffb_regime in pre_timestep, the
 * transaction immediately before sage26_calculate_star_formation).
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "module_system/physical_constants.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/sage26_ffb_star_formation/sage26_ffb_star_formation.h"

extern int sage26_ffb_star_formation_init(void);
extern int sage26_ffb_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_ffb_star_formation_cleanup(void);

/* R6's values */
#define TEST_HUBBLE_H 0.73
#define TEST_RECYCLE 0.43
#define TEST_EPSILON 2.9
#define TEST_EJECTION 0.3
#define TEST_YIELD 0.025
#define TEST_SF_DISK_FACTOR 3.0

/* A substep: dT 0.5 over 4 substeps (dt 0.125), substep 1, history bin (1 * 10) / 4 = 2 */
#define TEST_DT_SNAP 0.5
#define TEST_NUM_SUBSTEPS 4
#define TEST_SUBSTEP 1
#define TEST_BIN 2

#define SEEDED_MASS_LOADING 123.25f

// ============================================================================
// CONFIGURATION
// ============================================================================

static char module_name[] = "sage26_ffb_star_formation";
static char phase_name[] = "galaxy_physics";
static char other_phase_name[] = "later_physics";
static char ordinary_sf_name[] = "sage26_calculate_star_formation";
static char apply_name[] = "sage26_apply_star_formation_supernova";
static char classifier_name[] = "sage26_determine_ffb_regime";
static struct PhaseModuleConfig substep_entries[4];
static struct PhaseModuleConfig other_entries[1];
static struct PhaseModuleConfig fixed_entries[1];
static struct PhaseModuleConfig classifier_entries[1];
static struct ModulePhaseConfig substep_phases[2];

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }
static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

static void set_entry(struct PhaseModuleConfig *entry, char *name, const enum ProcessingMode mode) {
  entry->module_name = name;
  entry->processing_mode = mode;
  entry->resolved = NULL;
}

/**
 * @brief The module `count` times (1 or 2, mode as given) in a substep phase, followed by
 *        sage26_calculate_star_formation by galaxy, and sage26_determine_ffb_regime in
 *        pre_timestep: the dispatch the cross-module guards require at EnhancedStarFormationOn 1
 */
static void set_substep(const int count, const enum ProcessingMode mode) {
  for (int i = 0; i < count; i++) {
    set_entry(&substep_entries[i], module_name, mode);
  }
  set_entry(&substep_entries[count], ordinary_sf_name, PROCESSING_MODE_BY_GALAXY);
  substep_phases[0].name = phase_name;
  substep_phases[0].modules = substep_entries;
  substep_phases[0].num_modules = count + 1;
  MimicConfig.substep_phases = substep_phases;
  MimicConfig.num_substep_phases = 1;
  set_entry(&classifier_entries[0], classifier_name, PROCESSING_MODE_FULL_HALO);
  MimicConfig.pre_timestep = classifier_entries;
  MimicConfig.num_pre_timestep = 1;
}

/** @brief The run's units, Hubble_h and a 64-entry redshift table ZZ[s] = (63 - s) / 10 */
static void set_run(void) {
  MimicConfig.Hubble_h = TEST_HUBBLE_H;
  MimicConfig.G = 43.0071; /* (km/s)^2 Mpc / 1e10 Msun: an instability check would run */
  MimicConfig.UnitLength_in_cm = 3.08568e24;
  MimicConfig.UnitMass_in_g = 1.989e43;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  MimicConfig.Snaplistlen = 64;
  for (int s = 0; s < 64; s++) {
    MimicConfig.ZZ[s] = (63 - s) / 10.0;
  }
}

/** The parameters a test initialises with; NULL leaves a key out */
struct test_params {
  const char *enhanced_on, *efficiency, *sf_prescription, *cgm_recipe_on, *fire_mode_on,
      *frac_z_leave;
};

static const struct test_params R6_PARAMS = {"1", "0.2", "1", "1", "1", "0.0"};

/**
 * @brief Configure the run, the parameters and the placement (`arrange`, or once by galaxy), and
 *        initialise, capturing the log
 * @return The init status
 */
static int init_captured(const struct test_params *p, void (*arrange)(void), const char **log) {
  reset_config();
  set_run();
  set_substep(1, PROCESSING_MODE_BY_GALAXY);
  if (p->enhanced_on != NULL) {
    sage26_test_append_model_parameter("EnhancedStarFormationOn", p->enhanced_on);
  }
  if (p->efficiency != NULL) {
    sage26_test_append_model_parameter("EnhancedSFEfficiency", p->efficiency);
  }
  sage26_test_append_model_parameter("StarFormingDiskFactor", "3.0");
  sage26_test_append_model_parameter("SFprescription", p->sf_prescription);
  sage26_test_append_model_parameter("RecycleFraction", "0.43");
  sage26_test_append_model_parameter("CGMrecipeOn", p->cgm_recipe_on);
  sage26_test_append_model_parameter("FIREmodeOn", p->fire_mode_on);
  sage26_test_append_model_parameter("FeedbackReheatingEpsilon", "2.9");
  sage26_test_append_model_parameter("FeedbackEjectionEfficiency", "0.3");
  sage26_test_append_model_parameter("Yield", "0.025");
  sage26_test_append_model_parameter("FracZleaveDisk", p->frac_z_leave);
  if (arrange != NULL) {
    arrange();
  }
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_ffb_star_formation_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

static int init_with(const struct test_params *p) {
  const char *log = "";
  return init_captured(p, NULL, &log);
}

// ============================================================================
// SAGE26's starformation_ffb, WRITTEN OUT
// ============================================================================

/** @brief The history element (bin 0-9) of SfrDisk (0), SfrDiskColdGas (1), SfrDiskColdGasMetals
 * (2) */
static float *history(struct GalaxyData *g, const int which, const int bin) {
#define SAGE26_TEST_BINS(H)                                                                        \
  switch (bin) {                                                                                   \
  case 0:                                                                                          \
    return &g->H##Bin0;                                                                            \
  case 1:                                                                                          \
    return &g->H##Bin1;                                                                            \
  case 2:                                                                                          \
    return &g->H##Bin2;                                                                            \
  case 3:                                                                                          \
    return &g->H##Bin3;                                                                            \
  case 4:                                                                                          \
    return &g->H##Bin4;                                                                            \
  case 5:                                                                                          \
    return &g->H##Bin5;                                                                            \
  case 6:                                                                                          \
    return &g->H##Bin6;                                                                            \
  case 7:                                                                                          \
    return &g->H##Bin7;                                                                            \
  case 8:                                                                                          \
    return &g->H##Bin8;                                                                            \
  default:                                                                                         \
    return &g->H##Bin9;                                                                            \
  }
  if (which == 0) {
    SAGE26_TEST_BINS(SfrDisk)
  } else if (which == 1) {
    SAGE26_TEST_BINS(SfrDiskColdGas)
  }
  SAGE26_TEST_BINS(SfrDiskColdGasMetals)
#undef SAGE26_TEST_BINS
}

/** get_metallicity (R/src/model_misc.c), written out */
static double metallicity_of(const double gas, const double metals) {
  double z = 0.0;
  if (gas > 0.0 && metals > 0.0) {
    z = metals / gas;
    if (z >= 1.0) {
      z = 1.0;
    }
  }
  return z;
}

/** ionized_gas_fraction (S:135-152), written out */
static double f_ion_of(const double cold, const double rs, const double h) {
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

/** The hydrogen cap of the commits (S:1131-1135, S:1169-1173), written out */
static void cap_hydrogen(struct GalaxyData *g, const int sf_prescription) {
  if (sf_prescription != 0 && sf_prescription != 2) {
    const float max_h = (g->ColdGas > 0.0f) ? g->ColdGas * 0.74 : 0.0f;
    if (g->H2gas > max_h) {
      g->H2gas = max_h;
    }
    if (g->H1gas > max_h) {
      g->H1gas = max_h;
    }
  }
}

/** Misorderings the written-out transaction can run with, to show an input discriminates */
enum variant {
  VAR_NONE = 0,
  VAR_RECOMPUTE_Z = 1,          /* the feedback with the metallicity recomputed after the commit */
  VAR_YIELD_FORMED = 2,         /* the yield from the stars formed */
  VAR_HISTORY_RENORM = 4,       /* the history from the renormalised stars */
  VAR_RENORM_BEFORE_COMMIT = 8, /* the budget against the pre-commit gas, the commit renormalised */
  VAR_FEEDBACK_TO_ROW = 16      /* the feedback credited to the forming row, not its central */
};

/** The inputs of one written-out call */
struct tx {
  float vvir;
  double z, dt, dT, efficiency, eta_sn, energy_sn, frac_z_leave;
  float central_mvir;
  int bin, sf_prescription, cgm_recipe_on;
};

/** compute_sn_feedback_ffb (S:905-967), written out; returns the reheated and ejected masses */
static void budget(const struct tx *in, const float cold_gas, double *stars, double *reheated,
                   double *ejected, float *mass_loading) {
  *reheated = 0.0;
  *ejected = 0.0;
  double fire_scaling = 0.0;
  int valid = 0;
  const double vc = in->vvir;
  if (vc > 0.0 && in->z >= 0.0) {
    const double vc_floored = (vc < 1.0) ? 1.0 : vc;
    const double z_term = pow(1.0 + in->z, 1.25);
    const double v_term =
        (vc_floored < 60.0) ? pow(vc_floored / 60.0, -3.2) : 1.0 / (vc_floored / 60.0);
    fire_scaling = z_term * v_term;
    valid = 1;
  }
  if (valid) {
    double eta = TEST_EPSILON * fire_scaling;
    const double eta_max = 2.0 * (in->eta_sn * in->energy_sn) / (vc * vc);
    eta = (eta > eta_max) ? eta_max : eta;
    *mass_loading = (float)eta;
    *reheated = eta * *stars;
  }
  if ((*stars + *reheated) > cold_gas && (*stars + *reheated) > 0.0) {
    const double fac = cold_gas / (*stars + *reheated);
    *stars *= fac;
    *reheated *= fac;
  }
  if (in->vvir > 0.0 && valid) {
    double coupling = TEST_EJECTION * fire_scaling;
    coupling = (coupling > 2.0) ? 2.0 : coupling;
    const double E_FB = coupling * 0.5 * *stars * (in->eta_sn * in->energy_sn);
    const double E_lift = 0.5 * *reheated * vc * vc;
    *ejected = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;
  }
  if (*ejected < 0.0) {
    *ejected = 0.0;
  }
}

/** The written-out transaction's reheated mass passed to update_from_feedback, and whether the
 *  clamp of S:1346 bound, from its last call */
static double written_out_reheated_commit;
static int written_out_clamp_binds;

/**
 * @brief SAGE26's starformation_ffb (S:1270-1362) on g with its FoF central c (c may be g), plus
 *        the package's StarFormationRate diagnostic, run with the given variant
 */
static void sage26_ffb_written_out(struct GalaxyData *g, struct GalaxyData *c, const struct tx *in,
                                   const int variant) {
  const double reff = TEST_SF_DISK_FACTOR * g->DiskScaleRadius;               /* S:1273 */
  const double tdyn = (reff > 0.0 && in->vvir > 0.0) ? reff / in->vvir : 0.0; /* S:1274 */
  g->H2gas = 0.0;                                                             /* S:1280 */
  {
    double atomicH = (double)g->ColdGas * 0.74; /* S:1285, the object's plain multiply (H2 +0) */
    if (atomicH < 0.0) {
      atomicH = 0.0;
    }
    const double f_ion = f_ion_of(g->ColdGas, 1.0 * g->DiskScaleRadius, TEST_HUBBLE_H);
    atomicH *= (1.0 - f_ion);
    g->H1gas = atomicH; /* S:1291 */
  }
  const double gas_for_sf = g->ColdGas; /* S:1297 */
  double stars;
  if (isnan(g->ColdGas) || isinf(g->ColdGas) || isnan(in->vvir) || isinf(in->vvir) || isnan(reff) ||
      isinf(reff) || isnan(tdyn) || isinf(tdyn)) {
    stars = 0.0;
  } else if (tdyn > 0.0 && gas_for_sf > 0.0) {
    const double strdot = in->efficiency * gas_for_sf / tdyn; /* S:1309 */
    if (isnan(strdot) || isinf(strdot) || strdot < 0.0) {
      stars = 0.0;
    } else {
      stars = strdot * in->dt;
      if (stars > g->ColdGas) {
        stars = g->ColdGas;
      }
      if (isnan(stars) || isinf(stars) || stars < 0.0) {
        stars = 0.0;
      }
    }
  } else {
    stars = 0.0;
  }

  double fb_stars = stars, reheated = 0.0, ejected = 0.0;
  double commit_stars = stars;
  if (variant & VAR_RENORM_BEFORE_COMMIT) {
    budget(in, g->ColdGas, &fb_stars, &reheated, &ejected, &g->MassLoading);
    commit_stars = fb_stars;
  }
  if (variant & VAR_HISTORY_RENORM) {
    double s = stars, r = 0.0, e = 0.0;
    float ml = 0.0f;
    const float cold_after = (float)fma(-(1 - TEST_RECYCLE), stars, (double)g->ColdGas);
    budget(in, cold_after, &s, &r, &e, &ml);
    *history(g, 0, in->bin) += s / in->dt;
  } else {
    *history(g, 0, in->bin) += commit_stars / in->dt; /* S:1325 */
  }
  *history(g, 1, in->bin) = g->ColdGas;       /* S:1326 */
  *history(g, 2, in->bin) = g->MetalsColdGas; /* S:1327 */

  double metallicity = metallicity_of(g->ColdGas, g->MetalsColdGas); /* S:1329 */
  {
    /* update_from_star_formation (S:1121-1135), its four updates fused as the object has them */
    const double locked = 1 - TEST_RECYCLE;
    const double metals_fraction = metallicity * locked;
    g->ColdGas = (float)fma(-locked, commit_stars, (double)g->ColdGas);
    g->StellarMass = (float)fma(locked, commit_stars, (double)g->StellarMass);
    g->MetalsColdGas = (float)fma(-metals_fraction, commit_stars, (double)g->MetalsColdGas);
    g->MetalsStellarMass = (float)fma(metals_fraction, commit_stars, (double)g->MetalsStellarMass);
    cap_hydrogen(g, in->sf_prescription);
  }
  g->StarFormationRate += commit_stars / in->dT; /* the package's diagnostic */

  if (!(variant & VAR_RENORM_BEFORE_COMMIT)) {
    budget(in, g->ColdGas, &fb_stars, &reheated, &ejected, &g->MassLoading); /* S:1344 */
  }
  if (variant & VAR_RECOMPUTE_Z) {
    metallicity = metallicity_of(g->ColdGas, g->MetalsColdGas);
  }
  written_out_clamp_binds = reheated > g->ColdGas;
  if (reheated > g->ColdGas) {
    reheated = g->ColdGas; /* S:1346 */
  }
  written_out_reheated_commit = reheated;

  /* update_from_feedback (S:1150-1249) */
  struct GalaxyData *dest = (variant & VAR_FEEDBACK_TO_ROW) ? g : c;
  if (reheated > g->ColdGas) {
    reheated = g->ColdGas;
  }
  g->ColdGas -= reheated;
  g->MetalsColdGas = (float)fma(-metallicity, reheated, (double)g->MetalsColdGas); /* S:1168 */
  cap_hydrogen(g, in->sf_prescription);
  const int to_cgm = in->cgm_recipe_on == 1 && dest->Regime == 0; /* S:1175-1176 */
  float *gas = to_cgm ? &dest->CGMgas : &dest->HotGas;
  float *metals = to_cgm ? &dest->MetalsCGMgas : &dest->MetalsHotGas;
  *gas += reheated;
  *metals = (float)fma(metallicity, reheated, (double)*metals); /* S:1181, :1205, :1229 */
  if (ejected > *gas) {
    ejected = *gas;
  }
  const double z_sel = metallicity_of(*gas, *metals);
  double metals_to_eject = z_sel * ejected;
  if (metals_to_eject > *metals) {
    metals_to_eject = *metals;
  }
  *gas -= ejected;
  *metals -= metals_to_eject;
  dest->EjectedGas += ejected;
  dest->MetalsEjectedGas += metals_to_eject;
  g->SupernovaOutflowRate += reheated; /* S:1249 */

  /* the yield (S:1353-1362) */
  const double ystars = (variant & VAR_YIELD_FORMED) ? stars : fb_stars;
  const int yield_cgm = in->cgm_recipe_on == 1 && c->Regime == 0;
  if (g->ColdGas > 1.0e-8) {
    const double frac = in->frac_z_leave * exp(-1.0 * in->central_mvir / 30.0);
    g->MetalsColdGas = (float)fma(TEST_YIELD * (1.0 - frac), ystars, (double)g->MetalsColdGas);
    const double leaving = TEST_YIELD * frac * ystars;
    if (yield_cgm) {
      c->MetalsCGMgas += leaving;
    } else {
      c->MetalsHotGas += leaving;
    }
  } else {
    const double all_metals = TEST_YIELD * ystars;
    if (yield_cgm) {
      c->MetalsCGMgas += all_metals;
    } else {
      c->MetalsHotGas += all_metals;
    }
  }
}

// ============================================================================
// ROWS, CONTEXT AND RUNS
// ============================================================================

/** @brief An FFB disk: Type, own Vvir, Mvir, stored SnapNum 40 (z 2.3), and seeded reservoirs */
static void make_row(struct Halo *halo, struct GalaxyData *gal, const int type, const double vvir) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  halo->galaxy = gal;
  halo->Type = type;
  halo->Vvir = vvir;
  halo->Vmax = 30.0f;
  halo->Mvir = 15.5;
  halo->SnapNum = 40;
  halo->dT = TEST_DT_SNAP;
  halo->HaloNr = 7;
  gal->FFBRegime = 1;
  gal->Regime = 1;
  gal->ColdGas = 1.0f;
  gal->MetalsColdGas = 0.0234567f;
  gal->StellarMass = 2.5f;
  gal->MetalsStellarMass = 0.05f;
  gal->HotGas = 6.0f;
  gal->MetalsHotGas = 0.0371f;
  gal->CGMgas = 3.0f;
  gal->MetalsCGMgas = 0.0123f;
  gal->EjectedGas = 1.25f;
  gal->MetalsEjectedGas = 0.0071f;
  gal->DiskScaleRadius = 0.004f;
  gal->H2gas = 0.5f;
  gal->H1gas = 0.125f;
  gal->MassLoading = SEEDED_MASS_LOADING;
  gal->H2DepletionTime_Gyr = 1.5f;
  gal->SupernovaOutflowRate = 0.375f;
  gal->StarFormationRate = 0.25f;
  /* A seeded instability and transport state the transaction must not touch */
  gal->BulgeMass = 0.75f;
  gal->MetalsBulgeMass = 0.015f;
  gal->InstabilityBulgeMass = 0.5f;
  gal->BulgeRadius = 0.002f;
  gal->MergerBulgeRadius = 0.001f;
  gal->InstabilityBulgeRadius = 0.003f;
  gal->UnstableDiskGasFraction = 0.0;
  gal->NewStellarMass = 7.0;
  gal->SupernovaReheatedMass = 8.0;
  gal->SupernovaEjectedMass = 9.0;
  for (int b = 0; b < 10; b++) {
    *history(gal, 0, b) = 0.0625f * (float)b;
    *history(gal, 1, b) = 0.5f;
    *history(gal, 2, b) = 0.01f;
  }
}

/** What the observer saw */
static struct Sage26FfbStarFormationTrace observed;
static int observed_count;

static void record(void *user, const struct Sage26FfbStarFormationTrace *t) {
  (void)user;
  observed = *t;
  observed_count++;
}

/** @brief Run process() on row (its central `central`, NULL for none) at the test substep */
static int run_at(struct Halo *row, struct Halo *central, const int num_substeps,
                  const int substep) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = num_substeps;
  ctx.substep_number = substep;
  ctx.central_galaxy = central;
  observed_count = 0;
  memset(&observed, 0x5a, sizeof(observed)); /* no value of an earlier call can survive */
  sage26_ffb_star_formation_set_test_observer(record, NULL);
  const int status = sage26_ffb_star_formation_process(&ctx, row, 1);
  sage26_ffb_star_formation_set_test_observer(NULL, NULL);
  return status;
}

static int run(struct Halo *row, struct Halo *central) {
  return run_at(row, central, TEST_NUM_SUBSTEPS, TEST_SUBSTEP);
}

static int nan_f64(const double v) { return isnan(v); }
static int nan_f32(const float v) { return isnan(v); }

/**
 * @brief Whether the observed budget and feedback-commit locals all hold their "not formed"
 *        sentinels (NAN for values, -1 for flags, sage26_ffb_star_formation.h)
 */
static int unformed_budget_and_commit(const struct Sage26FfbStarFormationTrace *t) {
  const struct MimicSageFfbFeedbackLocals *f = &t->feedback;
  const struct MimicSageFeedbackLocals *c = &t->commit;
  const int budget =
      nan_f64(f->stars_in) && f->mass_loading_written == -1 && nan_f64(f->reheated_pre_renorm) &&
      f->renorm_binds == -1 && nan_f64(f->renorm_fac) && nan_f64(f->stars_out) &&
      nan_f64(f->reheated_out) && f->ejection_evaluated == -1 && nan_f64(f->ejected_pre_floor) &&
      f->ejected_floored == -1 && nan_f64(f->ejected_out) && nan_f64(f->fire.redshift) &&
      f->fire.scaling_valid == -1 && nan_f64(f->fire.scaling) && nan_f64(f->fire.eta) &&
      nan_f64(f->fire.e_fb) && nan_f64(f->fire.e_lift);
  const int commit = nan_f64(c->reheated_mass_clamped) && nan_f64(c->ejected_mass_capped) &&
                     nan_f64(c->metallicity_selected) && nan_f64(c->metals_to_eject) &&
                     nan_f64(c->metals_to_eject_capped) && nan_f32(c->selected_gas_at_cap) &&
                     nan_f32(c->selected_metals_at_cap) && c->reservoir_cgm == -1 &&
                     c->reheat_clamp_binds == -1 && c->ejected_cap_binds == -1 &&
                     c->metals_cap_binds == -1 && c->hydrogen_caps.tracks_h2 == -1 &&
                     nan_f32(c->hydrogen_caps.max_h) && c->hydrogen_caps.h2_cap_binds == -1 &&
                     c->hydrogen_caps.h1_cap_binds == -1;
  const int others = nan_f64(t->reheated_from_budget) && t->reheat_clamp_binds == -1 &&
                     nan_f64(t->reheated_to_commit) && nan_f64(t->yield_stars) &&
                     t->yield.coldgas_above_threshold == -1 && t->sf_caps.tracks_h2 == -1;
  return budget && commit && others;
}

/** @brief The written-out call's inputs for a row with central mass `central_mvir` */
static struct tx tx_for(const struct Halo *row, const double central_mvir, const int sfp,
                        const int cgm, const double frac_z) {
  struct tx in;
  in.vvir = (float)row->Vvir;
  in.z = MimicConfig.ZZ[row->SnapNum];
  in.dT = row->dT;
  in.dt = row->dT / TEST_NUM_SUBSTEPS;
  in.efficiency = 0.2;
  in.eta_sn = ETA_SN * (MimicConfig.UnitMass_in_g / SOLAR_MASS) / MimicConfig.Hubble_h;
  in.energy_sn = ENERGY_SN / MimicConfig.UnitEnergy_in_cgs * MimicConfig.Hubble_h;
  in.frac_z_leave = frac_z;
  in.central_mvir = (float)central_mvir;
  in.bin = TEST_BIN;
  in.sf_prescription = sfp;
  in.cgm_recipe_on = cgm;
  return in;
}

static int galaxies_equal(const struct GalaxyData *a, const struct GalaxyData *b) {
  return memcmp(a, b, sizeof(*a)) == 0;
}

/**
 * @brief Run the module on a central row (the row its own FoF central) and compare with SAGE26's
 *        written-out transaction; `expected` receives the written-out end state
 */
static int central_matches(struct Halo *row, const int sfp, const int cgm, const double frac_z,
                           struct GalaxyData *expected) {
  *expected = *row->galaxy;
  const struct tx in = tx_for(row, row->Mvir, sfp, cgm, frac_z);
  sage26_ffb_written_out(expected, expected, &in, VAR_NONE);
  if (run(row, row) != 0) {
    return 0;
  }
  return galaxies_equal(row->galaxy, expected);
}

/** @brief The written-out end state of a central row under a variant */
static struct GalaxyData variant_of(const struct GalaxyData *input, const struct Halo *row,
                                    const int variant) {
  struct GalaxyData g = *input;
  const struct tx in = tx_for(row, row->Mvir, 1, 1, 0.0);
  sage26_ffb_written_out(&g, &g, &in, variant);
  return g;
}

// ============================================================================
// SOFTWARE QUALITY
// ============================================================================

int test_module_lifecycle(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  set_run();
  test_phase_add(phase_name, module_name, PROCESSING_MODE_BY_GALAXY);
  set_test_model_parameters();
  sage26_test_append_model_parameter("EnhancedStarFormationOn", "0");
  sage26_test_append_model_parameter("EnhancedSFEfficiency", "0.2");
  sage26_test_append_model_parameter("SFprescription", "1");
  sage26_test_append_model_parameter("CGMrecipeOn", "1");
  sage26_test_append_model_parameter("FIREmodeOn", "1");
  const int result = module_system_init();
  TEST_ASSERT(result == 0, "module system initialisation must succeed");
  module_system_cleanup();
  test_free_substep_phases();
  check_memory_leaks();
  return TEST_PASS;
}

int test_parameters_refused(void) {
  static const struct {
    struct test_params p;
    const char *named;
  } refused[] = {
      {{NULL, "0.2", "1", "1", "1", "0.0"}, "EnhancedStarFormationOn"},
      {{"1", NULL, "1", "1", "1", "0.0"}, "EnhancedSFEfficiency"},
      {{"2", "0.2", "1", "1", "1", "0.0"}, "EnhancedStarFormationOn"},
      {{"1", "-0.1", "1", "1", "1", "0.0"}, "EnhancedSFEfficiency"},
      {{"1", "1.5", "1", "1", "1", "0.0"}, "EnhancedSFEfficiency"},
      {{"1", "nan", "1", "1", "1", "0.0"}, "EnhancedSFEfficiency"},
      {{"1", "0.2", "1", "1", "0", "0.0"}, "FIREmodeOn"},
  };
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    const char *log = "";
    const int status = init_captured(&refused[k].p, NULL, &log);
    printf("\n  refusal %zu (%s): init %d", k, refused[k].named, status);
    TEST_ASSERT(status != 0, "init must refuse the configuration");
    TEST_ASSERT(strstr(log, refused[k].named) != NULL, "the error must name what it refuses");
  }
  const struct test_params fire_off_at_zero = {"0", "0.2", "1", "1", "0", "0.0"};
  TEST_ASSERT(init_with(&fire_off_at_zero) == 0,
              "FIREmodeOn 0 is accepted at EnhancedStarFormationOn 0");
  const struct test_params bounds = {"1", "1.0", "1", "1", "1", "0.0"};
  TEST_ASSERT(init_with(&bounds) == 0, "EnhancedSFEfficiency 1.0 is accepted");
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "R6's configuration initialises");
  return TEST_PASS;
}

static void arrange_pre_timestep(void) {
  MimicConfig.num_substep_phases = 0;
  fixed_entries[0].module_name = module_name;
  fixed_entries[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  fixed_entries[0].resolved = NULL;
  MimicConfig.pre_timestep = fixed_entries;
  MimicConfig.num_pre_timestep = 1;
}
static void arrange_post_timestep(void) {
  MimicConfig.num_substep_phases = 0;
  fixed_entries[0].module_name = module_name;
  fixed_entries[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  fixed_entries[0].resolved = NULL;
  MimicConfig.post_timestep = fixed_entries;
  MimicConfig.num_post_timestep = 1;
}
static void arrange_twice(void) { set_substep(2, PROCESSING_MODE_BY_GALAXY); }
static void arrange_full_halo(void) { set_substep(1, PROCESSING_MODE_FULL_HALO); }

int test_placement_refused(void) {
  void (*const arrangements[4])(void) = {arrange_pre_timestep, arrange_post_timestep, arrange_twice,
                                         arrange_full_halo};
  const char *labels[4] = {"pre_timestep", "post_timestep", "twice", "process_full_halo"};
  for (int k = 0; k < 4; k++) {
    const char *log = "";
    const int status = init_captured(&R6_PARAMS, arrangements[k], &log);
    printf("\n  placement %s: init %d", labels[k], status);
    TEST_ASSERT(status != 0, "a misplacement must be refused");
    TEST_ASSERT(strstr(log, module_name) != NULL, "the error must name the module");
  }
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "once by galaxy in a substep phase is accepted");
  return TEST_PASS;
}

/* The dispatch configurations the cross-module guards refuse at EnhancedStarFormationOn 1, each
 * arranged after init_captured() has set the complete one */
static void arrange_no_classifier(void) { MimicConfig.num_pre_timestep = 0; }
static void arrange_no_ordinary_sf(void) { substep_phases[0].num_modules = 1; }
static void arrange_after_ordinary_sf(void) {
  set_entry(&substep_entries[0], ordinary_sf_name, PROCESSING_MODE_BY_GALAXY);
  set_entry(&substep_entries[1], module_name, PROCESSING_MODE_BY_GALAXY);
}
static void arrange_separated(void) {
  set_entry(&substep_entries[1], apply_name, PROCESSING_MODE_BY_GALAXY);
  set_entry(&substep_entries[2], ordinary_sf_name, PROCESSING_MODE_BY_GALAXY);
  substep_phases[0].num_modules = 3;
}
static void arrange_ordinary_sf_full_halo(void) {
  substep_entries[1].processing_mode = PROCESSING_MODE_FULL_HALO;
}
static void arrange_other_phase(void) {
  substep_phases[0].num_modules = 1;
  set_entry(&other_entries[0], ordinary_sf_name, PROCESSING_MODE_BY_GALAXY);
  substep_phases[1].name = other_phase_name;
  substep_phases[1].modules = other_entries;
  substep_phases[1].num_modules = 1;
  MimicConfig.num_substep_phases = 2;
}

/**
 * @test    test_dispatch_guards
 * @brief   At EnhancedStarFormationOn 1 the transaction refuses, naming the module, a
 *          configuration without the classifier in pre_timestep or not immediately before
 *          sage26_calculate_star_formation (process_by_galaxy, same
 *          substep phase); at 0 it accepts each
 */
int test_dispatch_guards(void) {
  const struct {
    const char *label;
    void (*arrange)(void);
    const char *named;
  } refused[] = {
      {"no sage26_determine_ffb_regime", arrange_no_classifier, classifier_name},
      {"no sage26_calculate_star_formation", arrange_no_ordinary_sf, ordinary_sf_name},
      {"after sage26_calculate_star_formation", arrange_after_ordinary_sf, ordinary_sf_name},
      {"separated from it", arrange_separated, ordinary_sf_name},
      {"it as process_full_halo", arrange_ordinary_sf_full_halo, ordinary_sf_name},
      {"it in another substep phase", arrange_other_phase, ordinary_sf_name},
  };
  const struct test_params off = {"0", "0.2", "1", "1", "1", "0.0"};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    const char *log = "";
    int status = init_captured(&R6_PARAMS, refused[k].arrange, &log);
    printf("\n  %s (EnhancedStarFormationOn 1): init %d", refused[k].label, status);
    TEST_ASSERT(status != 0, "init must refuse the incomplete dispatch");
    TEST_ASSERT(strstr(log, refused[k].named) != NULL,
                "the error must name the missing or misplaced module");
    TEST_ASSERT(strstr(log, module_name) != NULL, "the error must name the transaction");
    status = init_captured(&off, refused[k].arrange, &log);
    printf("; at 0: init %d", status);
    TEST_ASSERT(status == 0, "at EnhancedStarFormationOn 0 no cross-module guard applies");
  }
  const char *log = "";
  TEST_ASSERT(init_captured(&R6_PARAMS, NULL, &log) == 0,
              "the complete dispatch initialises at EnhancedStarFormationOn 1");
  return TEST_PASS;
}

/** The run files' by-galaxy star-formation chain, in their order */
static const char *const CHAIN[] = {"sage26_ffb_star_formation",
                                    "sage26_calculate_star_formation",
                                    "sage26_calculate_supernova_feedback",
                                    "sage26_apply_star_formation_supernova",
                                    "sage26_disk_instability",
                                    "sage26_quasar_mode",
                                    "sage26_starburst_feedback",
                                    "sage26_apply_metal_enrichment"};
#define CHAIN_LENGTH ((int)(sizeof(CHAIN) / sizeof(CHAIN[0])))

/** How a chain configuration departs from the run files' */
enum chain_variant { CHAIN_COMPLETE, CHAIN_NO_TRANSACTION, CHAIN_NO_CLASSIFIER, CHAIN_SWAPPED };

/**
 * @brief Configure the run files' pre_timestep classifier and by-galaxy chain (as `variant`
 *        changes them) with every key the modules read, and run module_system_init, capturing
 *        the log; the caller cleans up with chain_cleanup()
 * @return The module_system_init status
 */
static int chain_init(const enum chain_variant variant, const char *enhanced_on, const char **log) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  set_run();
  if (variant != CHAIN_NO_CLASSIFIER) {
    test_pre_timestep_add(classifier_name, PROCESSING_MODE_FULL_HALO);
  }
  for (int k = 0; k < CHAIN_LENGTH; k++) {
    int m = k;
    if (variant == CHAIN_SWAPPED && k < 2) {
      m = 1 - k; /* sage26_calculate_star_formation, then the transaction */
    }
    if (variant == CHAIN_NO_TRANSACTION && m == 0) {
      continue;
    }
    test_phase_add(phase_name, CHAIN[m], PROCESSING_MODE_BY_GALAXY);
  }
  MimicConfig.SubSteps = TEST_NUM_SUBSTEPS;
  set_test_model_parameters();
  sage26_test_append_model_parameter("EnhancedStarFormationOn", enhanced_on);
  sage26_test_append_model_parameter("EnhancedSFEfficiency", "0.2");
  sage26_test_append_model_parameter("SFprescription", "1");
  sage26_test_append_model_parameter("CGMrecipeOn", "1");
  sage26_test_append_model_parameter("FIREmodeOn", "1");
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_SF_MODULE |
                                      SAGE26_TEST_KEYS_STARBURST_MODULE);
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = module_system_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

static void chain_cleanup(const int init_status) {
  if (init_status == 0) {
    module_system_cleanup();
  }
  test_free_substep_phases();
  test_free_pre_timestep();
  check_memory_leaks();
}

/**
 * @test    test_chain_initialises
 * @brief   module_system_init over the run files' classifier and chain: the complete configuration
 *          initialises at EnhancedStarFormationOn 1; without the transaction, without the
 *          classifier, or with the transaction after sage26_calculate_star_formation it fails
 *          naming the missing or misplaced module; at 0 each initialises
 */
int test_chain_initialises(void) {
  const char *log = "";
  int status = chain_init(CHAIN_COMPLETE, "1", &log);
  printf("\n  complete chain at 1: init %d", status);
  TEST_ASSERT(status == 0, "the run files' complete configuration initialises at 1");
  chain_cleanup(status);
  const struct {
    enum chain_variant variant;
    const char *label;
    const char *named;
  } refused[] = {
      {CHAIN_NO_TRANSACTION, "without the transaction", module_name},
      {CHAIN_NO_CLASSIFIER, "without the classifier", classifier_name},
      {CHAIN_SWAPPED, "transaction after sage26_calculate_star_formation", ordinary_sf_name}};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    status = chain_init(refused[k].variant, "1", &log);
    printf("\n  %s at 1: init %d", refused[k].label, status);
    TEST_ASSERT(status != 0, "module_system_init must refuse the incomplete dispatch");
    TEST_ASSERT(strstr(log, refused[k].named) != NULL,
                "the error must name the missing or misplaced module");
    chain_cleanup(status);
    status = chain_init(refused[k].variant, "0", &log);
    printf("; at 0: init %d", status);
    TEST_ASSERT(status == 0, "at EnhancedStarFormationOn 0 no cross-module guard applies");
    chain_cleanup(status);
  }
  return TEST_PASS;
}

// ============================================================================
// BEHAVIOUR
// ============================================================================

int test_off_writes_nothing(void) {
  const struct test_params off = {"0", "0.2", "1", "1", "1", "0.0"};
  TEST_ASSERT(init_with(&off) == 0, "init at EnhancedStarFormationOn 0");
  struct Halo row;
  struct GalaxyData gal;
  make_row(&row, &gal, 0, 80.0);
  const struct GalaxyData before = gal;
  TEST_ASSERT(run(&row, &row) == 0, "process succeeds");
  TEST_ASSERT(galaxies_equal(&gal, &before) && observed_count == 0,
              "EnhancedStarFormationOn 0: an FFBRegime 1 row is not written");
  return TEST_PASS;
}

int test_non_ffb_row_not_written(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal;
  make_row(&row, &gal, 0, 80.0);
  gal.FFBRegime = 0;
  const struct GalaxyData before = gal;
  TEST_ASSERT(run(&row, &row) == 0, "process succeeds");
  TEST_ASSERT(galaxies_equal(&gal, &before) && observed_count == 0,
              "an FFBRegime 0 row is not written (S:1004)");
  return TEST_PASS;
}

int test_initial_boundary_and_refusal(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal;
  make_row(&row, &gal, 0, 80.0);
  row.SnapNum = -1;
  row.dT = 0.0;
  struct GalaxyData before = gal;
  TEST_ASSERT(run(&row, &row) == 0, "the initial-boundary state returns 0");
  TEST_ASSERT(galaxies_equal(&gal, &before) && observed.path == SAGE26_FFB_PATH_INITIAL_BOUNDARY,
              "the initial-boundary state writes nothing (D37)");
  TEST_ASSERT(unformed_budget_and_commit(&observed) && nan_f64(observed.dt) &&
                  nan_f64(observed.stars_formed),
              "the observed trace holds the not-formed sentinels (NAN, -1)");

  make_row(&row, &gal, 0, 80.0);
  row.SnapNum = 64; /* outside ZZ's 64 entries, with dT > 0 */
  before = gal;
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = run(&row, &row);
  const char *log = "";
  if (capture != NULL) {
    set_log_output(old_out);
    log = read_captured_log(capture);
    fclose(capture);
  }
  TEST_ASSERT(status != 0, "an out-of-range stored SnapNum with dT > 0 is refused (D35)");
  TEST_ASSERT(strstr(log, "SnapNum = 64") != NULL, "the refusal names the SnapNum");
  TEST_ASSERT(galaxies_equal(&gal, &before), "nothing is written before the refusal");
  return TEST_PASS;
}

int test_stars_formed_and_renormalised(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal, expected;
  /* Vvir 2 km/s (a large loading) and a short tdyn = 3 * 0.004 / 2 = 0.006: strdot * dt =
   * 0.2 * 1 / 0.006 * 0.125 > ColdGas 1, so the stars are capped at exactly 1 */
  make_row(&row, &gal, 0, 2.0);
  const struct GalaxyData input = gal;
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "the end state equals SAGE26's written-out starformation_ffb byte for byte");
  TEST_ASSERT(observed.stars_cap_binds == 1 && same_f64(observed.stars_formed, 1.0),
              "the stars are capped at ColdGas = 1 (S:1315)");
  TEST_ASSERT(same_f32(gal.StellarMass, (float)((double)2.5f + (1 - TEST_RECYCLE))),
              "the committed stellar mass gains (1 - R) * the stars formed (1) (S:1330)");
  TEST_ASSERT(same_f32(*history(&gal, 0, TEST_BIN), (float)(0.125 + 1.0 / 0.125)),
              "the history samples the stars formed over dt: SfrDisk += 1 / 0.125 (S:1325)");
  TEST_ASSERT(same_f32(*history(&gal, 1, TEST_BIN), 1.0f) &&
                  same_f32(*history(&gal, 2, TEST_BIN), 0.0234567f),
              "the history samples the cold gas before any commit (S:1326-1327)");
  TEST_ASSERT(observed.feedback.renorm_binds == 1 && observed.yield_stars < 1.0 &&
                  same_f64(observed.yield_stars, observed.feedback.stars_out),
              "the budget renormalises the stars, and the yield receives the renormalised stars");
  /* The input discriminates each misordering */
  struct GalaxyData v = variant_of(&input, &row, VAR_YIELD_FORMED);
  TEST_ASSERT(!galaxies_equal(&v, &expected), "the yield from the stars formed would differ");
  v = variant_of(&input, &row, VAR_HISTORY_RENORM);
  TEST_ASSERT(!galaxies_equal(&v, &expected), "a history of the renormalised stars would differ");
  v = variant_of(&input, &row, VAR_RENORM_BEFORE_COMMIT);
  TEST_ASSERT(!galaxies_equal(&v, &expected), "the renormalisation before the commit would differ");
  return TEST_PASS;
}

int test_pre_commit_metallicity(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  /* Search the cold metals for an input whose recomputed metallicity changes the end state */
  struct Halo row;
  struct GalaxyData gal, expected;
  int found = 0;
  for (int k = 0; k < 200 && !found; k++) {
    make_row(&row, &gal, 0, 30.0);
    gal.ColdGas = 0.7f;
    gal.MetalsColdGas = 0.0123f + 1.0e-5f * (float)k;
    const struct GalaxyData input = gal;
    const struct GalaxyData plain = variant_of(&input, &row, VAR_NONE);
    const struct GalaxyData recomputed = variant_of(&input, &row, VAR_RECOMPUTE_Z);
    found = !galaxies_equal(&plain, &recomputed);
  }
  TEST_ASSERT(found, "an input where the recomputed metallicity changes the end state exists");
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "the end state equals SAGE26's, with the pre-commit metallicity (S:1329, S:1348)");
  TEST_ASSERT(observed.commit.reheated_mass_clamped > 0.0,
              "the feedback commit moves gas with that metallicity");
  return TEST_PASS;
}

int test_invalid_fire_scaling(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  for (int k = 0; k < 2; k++) {
    struct Halo row;
    struct GalaxyData gal, expected;
    if (k == 0) {
      make_row(&row, &gal, 0, 0.0); /* Vvir 0 */
    } else {
      make_row(&row, &gal, 0, 80.0);
      row.SnapNum = 10;
      MimicConfig.ZZ[10] = -0.5; /* z < 0 with Vvir > 0 */
    }
    TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
                "the end state equals SAGE26's written-out transaction");
    TEST_ASSERT(same_f32(gal.MassLoading, SEEDED_MASS_LOADING),
                "SAGE parity: an invalid FIRE scaling leaves MassLoading stale (S:929-934)");
    TEST_ASSERT(same_f64(observed.reheated_to_commit, 0.0) &&
                    same_f64(observed.ejected_to_commit, 0.0),
                "no reheating and no ejection");
    TEST_ASSERT(same_f32(gal.HotGas, 6.0f) && same_f32(gal.EjectedGas, 1.25f) &&
                    same_f32(gal.SupernovaOutflowRate, 0.375f),
                "the hot and ejected gas and the outflow unchanged");
    MimicConfig.ZZ[10] = (63 - 10) / 10.0;
  }
  return TEST_PASS;
}

int test_hydrogen(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal, expected;
  make_row(&row, &gal, 0, 80.0);
  gal.H1gas = 9.0f;
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "the end state equals SAGE26's written-out transaction");
  TEST_ASSERT(same_f32(gal.H2gas, 0.0f) && !signbit(gal.H2gas), "H2gas is zeroed (S:1280)");
  TEST_ASSERT(same_f32(gal.H2DepletionTime_Gyr, 1.5f), "H2DepletionTime_Gyr is not written");
  /* The H I block from the input cold gas, before the commits' caps */
  const double f_ion = f_ion_of(1.0f, 0.004f, TEST_HUBBLE_H);
  const float h1_block = (float)((1.0 * 0.74) * (1.0 - f_ion));
  const float max_h = (float)((double)gal.ColdGas * 0.74);
  TEST_ASSERT(same_f32(gal.H1gas, h1_block < max_h ? h1_block : max_h),
              "H1gas is the H I block's, capped at the hydrogen of the remaining cold gas");
  TEST_ASSERT(observed.sf_caps.tracks_h2 == 1 && observed.sf_caps.h1_cap_binds == 1,
              "the prescription-1 cap binds on H1gas after the commit (S:1131-1135)");
  return TEST_PASS;
}

int test_guards_and_tdyn(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal, expected;
  /* NaN ColdGas, infinite Vvir, NaN radius: stars 0 (S:1303-1306) */
  for (int k = 0; k < 3; k++) {
    make_row(&row, &gal, 0, 80.0);
    if (k == 0) {
      gal.ColdGas = NAN;
    } else if (k == 1) {
      row.Vvir = INFINITY;
    } else {
      gal.DiskScaleRadius = NAN;
    }
    TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
                "a guarded input: the end state equals SAGE26's written-out transaction");
    TEST_ASSERT(observed.nonfinite_guard == 1 && same_f64(observed.stars_formed, 0.0),
                "the NaN/infinity guard forms no stars");
  }
  /* tdyn 0 from a zero radius, a zero Vvir and a negative radius (reff > 0.0 false, S:1274) */
  const float radii[3] = {0.0f, 0.004f, -0.5f};
  const double velocities[3] = {80.0, 0.0, 80.0};
  for (int k = 0; k < 3; k++) {
    make_row(&row, &gal, 0, velocities[k]);
    gal.DiskScaleRadius = radii[k];
    TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
                "tdyn 0: the end state equals SAGE26's written-out transaction");
    TEST_ASSERT(same_f64(observed.tdyn, 0.0) && observed.tdyn_gas_positive == 0 &&
                    same_f64(observed.stars_formed, 0.0),
                "tdyn is +0 and no stars form");
  }
  return TEST_PASS;
}

int test_satellite_routing(void) {
  static const struct {
    const char *cgm;
    int central_regime;
    int to_cgm;
  } runs[3] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}};
  for (int k = 0; k < 3; k++) {
    struct test_params p = R6_PARAMS;
    p.cgm_recipe_on = runs[k].cgm;
    TEST_ASSERT(init_with(&p) == 0, "init");
    struct Halo row, central;
    struct GalaxyData gal, cgal;
    make_row(&row, &gal, 1, 30.0);
    gal.Regime = 1 - runs[k].central_regime;
    make_row(&central, &cgal, 0, 200.0);
    central.Mvir = 80.0;
    cgal.FFBRegime = 0;
    cgal.Regime = runs[k].central_regime;
    /* metals not a fixed fraction: the central's selected pairs are metal-rich, the row metal-poor
     */
    cgal.MetalsHotGas = 0.9f;
    cgal.MetalsCGMgas = 0.6f;
    gal.MetalsColdGas = 0.0017f;
    struct GalaxyData exp_row = gal, exp_central = cgal;
    const struct tx in = tx_for(&row, central.Mvir, 1, runs[k].cgm[0] - '0', 0.0);
    sage26_ffb_written_out(&exp_row, &exp_central, &in, VAR_NONE);
    struct GalaxyData to_row = gal, to_row_central = cgal;
    sage26_ffb_written_out(&to_row, &to_row_central, &in, VAR_FEEDBACK_TO_ROW);
    TEST_ASSERT(!galaxies_equal(&to_row, &exp_row), "the input discriminates the destination");
    const struct GalaxyData row_before = gal;
    TEST_ASSERT(run(&row, &central) == 0, "process succeeds");
    TEST_ASSERT(galaxies_equal(&gal, &exp_row) && galaxies_equal(&cgal, &exp_central),
                "row and central equal SAGE26's written-out transaction byte for byte");
    TEST_ASSERT(same_f32(gal.HotGas, row_before.HotGas) &&
                    same_f32(gal.MetalsHotGas, row_before.MetalsHotGas) &&
                    same_f32(gal.CGMgas, row_before.CGMgas) &&
                    same_f32(gal.MetalsCGMgas, row_before.MetalsCGMgas),
                "the forming row's own hot and CGM pairs are untouched");
    TEST_ASSERT(observed.commit.reservoir_cgm == runs[k].to_cgm &&
                    observed.yield.reservoir_cgm == runs[k].to_cgm,
                "the CENTRAL's Regime selects the feedback's and the yield's pair");
    const float before_gas = runs[k].to_cgm ? 3.0f : 6.0f;
    const float after_gas = runs[k].to_cgm ? cgal.CGMgas : cgal.HotGas;
    TEST_ASSERT(same_f32(after_gas,
                         (float)((double)(float)((double)before_gas + observed.reheated_to_commit) -
                                 observed.commit.ejected_mass_capped)),
                "the selected gas gains the reheated mass and loses the ejection");
  }
  return TEST_PASS;
}

int test_no_instability_no_transport(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal, expected;
  /* An unstable disk by its masses and radius: the Mo, Mao & White (1998) critical mass
   * Vmax^2 * 3 * DiskScaleRadius / G = 900 * 0.012 / 43.0071, about 0.25, against a disk of
   * ColdGas 1 + StellarMass 2.5 - BulgeMass 0.75 = 2.75 (R/src/model_disk_instability.c:54-58) */
  TEST_ASSERT(900.0 * (3.0 * (double)0.004f) / 43.0071 < 1.0 + (2.5 - 0.75),
              "the seeded disk is unstable");
  make_row(&row, &gal, 0, 80.0);
  gal.UnstableDiskGasFraction = 0.0;
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "the end state equals SAGE26's written-out transaction (which has no instability)");
  TEST_ASSERT(same_f32(gal.BulgeMass, 0.75f) && same_f32(gal.MetalsBulgeMass, 0.015f) &&
                  same_f32(gal.InstabilityBulgeMass, 0.5f) && same_f32(gal.BulgeRadius, 0.002f) &&
                  same_f32(gal.MergerBulgeRadius, 0.001f) &&
                  same_f32(gal.InstabilityBulgeRadius, 0.003f) &&
                  same_f64(gal.UnstableDiskGasFraction, 0.0),
              "no instability field is written (S:1365-1367)");
  TEST_ASSERT(same_f64(gal.NewStellarMass, 7.0) && same_f64(gal.SupernovaReheatedMass, 8.0) &&
                  same_f64(gal.SupernovaEjectedMass, 9.0),
              "no transport field is written");
  return TEST_PASS;
}

/** @brief A float from its IEEE-754 bit pattern */
static float f32_bits(const uint32_t bits) {
  float v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

/** @brief A double from its IEEE-754 bit pattern */
static double f64_bits(const uint64_t bits) {
  double v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

int test_reheat_clamp_wiring(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  /* An in-domain state where the clamp of S:1346 binds with positive cold gas: the inputs of the
   * oracle fixture ffb_reheat_clamp_binds, as literals. A satellite with its own Vvir 1e-10 at a
   * constructed stored-snapshot redshift 1e8 has a loading near 1e16, so the renormalised
   * reheated mass (S:943-947) rounds above the cold gas the star-formation commit left */
  struct Halo row, central;
  struct GalaxyData gal, cgal;
  make_row(&central, &cgal, 0, 0.0);
  central.Vvir = f32_bits(0x434610f4u); /* 198.066223 */
  central.Mvir = f32_bits(0x41725c66u); /* 15.1475582 */
  central.SnapNum = 16;
  central.dT = f64_bits(0x3f0db75a67b0d059ull);
  cgal.FFBRegime = 0;
  cgal.Regime = 0;
  cgal.CGMgas = f32_bits(0x3dddfef8u);
  cgal.MetalsCGMgas = f32_bits(0x39b58f05u);
  cgal.EjectedGas = f32_bits(0x39fef7ddu);
  cgal.MetalsEjectedGas = f32_bits(0x35d08660u);
  make_row(&row, &gal, 1, f32_bits(0x2edbe6ffu)); /* Vvir 1.00000001e-10 */
  row.SnapNum = 16;
  row.dT = f64_bits(0x3f0db75a67b0d059ull); /* 5.667919773380608e-05 */
  gal.Regime = 0;
  gal.DiskScaleRadius = f32_bits(0x398ca070u);   /* 0.000268224161 */
  gal.ColdGas = f32_bits(0x3fa072b0u);           /* 1.25349998 */
  gal.MetalsColdGas = f32_bits(0x3943accau);     /* 0.000186610181 */
  gal.StellarMass = f32_bits(0x3f5675b9u);       /* 0.837733805 */
  gal.MetalsStellarMass = f32_bits(0x3b289430u); /* 0.00257230923 */
  gal.CGMgas = f32_bits(0x3fcd161bu);            /* 1.60223711 */
  gal.MetalsCGMgas = f32_bits(0x3b3a67e7u);      /* 0.00284432783 */
  gal.H1gas = f32_bits(0x3b9c9812u);
  gal.H2gas = f32_bits(0x3a83126fu);
  gal.MassLoading = f32_bits(0x4120bc7fu);
  MimicConfig.ZZ[16] = 1.0e8; /* the constructed redshift */

  /* SAGE26's transaction written out on the same inputs: 10 substeps, substep 3 (bin 3) */
  struct GalaxyData exp_row = gal, exp_central = cgal;
  struct tx in = tx_for(&row, central.Mvir, 1, 1, 0.0);
  in.dt = row.dT / 10;
  in.bin = 3;
  sage26_ffb_written_out(&exp_row, &exp_central, &in, VAR_NONE);
  TEST_ASSERT(written_out_clamp_binds == 1 && written_out_reheated_commit > 0.0,
              "the input binds the clamp of S:1346 with positive cold gas (in SAGE26's domain)");

  const int status = run_at(&row, &central, 10, 3);
  MimicConfig.ZZ[16] = (63 - 16) / 10.0;
  TEST_ASSERT(status == 0, "process succeeds");
  TEST_ASSERT(galaxies_equal(&gal, &exp_row) && galaxies_equal(&cgal, &exp_central),
              "row and central equal SAGE26's written-out transaction byte for byte");
  TEST_ASSERT(observed.reheat_clamp_binds == 1 &&
                  observed.reheated_from_budget > observed.reheated_to_commit,
              "the clamp of S:1346 binds and lowers the budget's reheated mass");
  TEST_ASSERT(same_f64(observed.reheated_to_commit, written_out_reheated_commit),
              "wiring: the reheated mass passed to the feedback commit is the remaining ColdGas "
              "(S:1346)");
  return TEST_PASS;
}

int test_reheat_clamp_negative_gas_probe(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  /* OUT OF SAGE26's DOMAIN, a labelled probe of the wiring only: with ColdGas -1 no stars form, the
   * budget's reheated mass 0 exceeds the cold gas, and the clamp of S:1346 passes -1 to the
   * commit, on which SAGE26's update_from_feedback aborts (S:1158-1160). No end state is asserted
   */
  struct Halo row;
  struct GalaxyData gal;
  make_row(&row, &gal, 0, 80.0);
  gal.ColdGas = -1.0f;
  TEST_ASSERT(run(&row, &row) == 0, "process returns");
  TEST_ASSERT(observed.reheat_clamp_binds == 1 && same_f64(observed.reheated_to_commit, -1.0),
              "wiring: the clamped value -1 (the cold gas) is passed to the commit");
  return TEST_PASS;
}

int test_sf_prescription_zero_probe(void) {
  struct test_params p = R6_PARAMS;
  p.sf_prescription = "0";
  TEST_ASSERT(init_with(&p) == 0, "init with SFprescription 0");
  struct Halo row;
  struct GalaxyData gal, expected;
  make_row(&row, &gal, 0, 80.0);
  gal.H1gas = 9.0f;
  TEST_ASSERT(central_matches(&row, 0, 1, 0.0, &expected),
              "SFprescription 0: the end state equals SAGE26's written-out transaction");
  TEST_ASSERT(observed.sf_caps.tracks_h2 == 0, "the commits' caps are off");
  return TEST_PASS;
}

int test_no_central(void) {
  TEST_ASSERT(init_with(&R6_PARAMS) == 0, "init at R6");
  struct Halo row;
  struct GalaxyData gal;
  make_row(&row, &gal, 0, 80.0);
  const struct GalaxyData before = gal;
  TEST_ASSERT(run(&row, NULL) == 0, "process succeeds without a FoF central");
  TEST_ASSERT(observed.path == SAGE26_FFB_PATH_NO_CENTRAL, "the no-central path");
  TEST_ASSERT(unformed_budget_and_commit(&observed) && nan_f64(observed.metallicity),
              "the observed budget and commit locals hold the not-formed sentinels (NAN, -1)");
  struct GalaxyData expected = before;
  expected.H2gas = gal.H2gas;
  expected.H1gas = gal.H1gas;
  *history(&expected, 0, TEST_BIN) = *history(&gal, 0, TEST_BIN);
  *history(&expected, 1, TEST_BIN) = *history(&gal, 1, TEST_BIN);
  *history(&expected, 2, TEST_BIN) = *history(&gal, 2, TEST_BIN);
  TEST_ASSERT(galaxies_equal(&gal, &expected) && same_f32(gal.H2gas, 0.0f) &&
                  same_f32(*history(&gal, 1, TEST_BIN), 1.0f),
              "only H2gas, H1gas and the history sample are written; nothing is committed");
  return TEST_PASS;
}

/* The chain's other modules, called directly after the transaction */
extern int sage26_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_supernova_feedback_process(struct ModuleContext *ctx,
                                                       struct Halo *halos, int ngal);
extern int sage26_apply_star_formation_supernova_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);

/** SAGE26's check_disk_instability criterion (R/src/model_disk_instability.c:54-57) */
static int disk_unstable(const struct GalaxyData *g, const float vmax) {
  const double diskmass = g->ColdGas + (g->StellarMass - g->BulgeMass);
  const double Mcrit = vmax * vmax * (3.0 * g->DiskScaleRadius) / MimicConfig.G;
  return diskmass > 0.0 && Mcrit < diskmass;
}

/**
 * @test    test_chain_leaves_trigger
 * @brief   After the transaction, the rest of the by-galaxy chain writes nothing on an FFB row with
 *          an unstable disk: UnstableDiskGasFraction keeps its reset value 0, and the quasar and
 *          burst entries write nothing
 *
 * SAGE26's FFB dispatch returns from starformation_and_feedback after starformation_ffb, which
 * runs no instability check (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367), so
 * neither grow_black_hole nor the instability burst runs (R/src/model_disk_instability.c:90-108).
 * The modules are initialised by module_system_init over the run files' configuration at
 * EnhancedStarFormationOn 1, then each process() runs on the FFB satellite in the run files'
 * order. The trigger starts at its per-snapshot reset value 0 (init_repeat); a positively seeded
 * trigger would survive the write-free path and is not this case.
 */
int test_chain_leaves_trigger(void) {
  const char *log = "";
  const int status = chain_init(CHAIN_COMPLETE, "1", &log);
  TEST_ASSERT(status == 0, "the complete chain initialises at EnhancedStarFormationOn 1");

  struct Halo central, row;
  struct GalaxyData central_gal, gal;
  make_row(&central, &central_gal, 0, 120.0);
  central_gal.FFBRegime = 0;
  make_row(&row, &gal, 1, 80.0);
  TEST_ASSERT(gal.UnstableDiskGasFraction == 0.0, "the trigger starts at its reset value 0");
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = TEST_NUM_SUBSTEPS;
  ctx.substep_number = TEST_SUBSTEP;
  ctx.central_galaxy = &central;

  TEST_ASSERT(sage26_ffb_star_formation_process(&ctx, &row, 1) == 0, "the transaction runs");
  struct GalaxyData row_tx, central_tx;
  memcpy(&row_tx, &gal, sizeof(gal));
  memcpy(&central_tx, &central_gal, sizeof(central_gal));
  TEST_ASSERT(row_tx.StellarMass > 2.5f, "the transaction formed stars on the FFB row");
  TEST_ASSERT(disk_unstable(&gal, (float)row.Vmax),
              "the FFB row's disk is unstable by its masses and radii after the transaction");

  int (*const rest[])(struct ModuleContext *, struct Halo *,
                      int) = {sage26_calculate_star_formation_process,
                              sage26_calculate_supernova_feedback_process,
                              sage26_apply_star_formation_supernova_process,
                              sage26_disk_instability_process,
                              sage26_quasar_mode_process,
                              sage26_starburst_feedback_process,
                              sage26_apply_metal_enrichment_process};
  for (size_t k = 0; k < sizeof(rest) / sizeof(rest[0]); k++) {
    struct GalaxyData row_before, central_before;
    memcpy(&row_before, &gal, sizeof(gal));
    memcpy(&central_before, &central_gal, sizeof(central_gal));
    TEST_ASSERT(rest[k](&ctx, &row, 1) == 0, "each chain module's process succeeds");
    printf("\n  %s: row %s, central %s", CHAIN[k + 1],
           memcmp(&gal, &row_before, sizeof(gal)) == 0 ? "unchanged" : "CHANGED",
           memcmp(&central_gal, &central_before, sizeof(central_gal)) == 0 ? "unchanged"
                                                                           : "CHANGED");
    if (k == 4 || k == 5) {
      TEST_ASSERT(memcmp(&gal, &row_before, sizeof(gal)) == 0 &&
                      memcmp(&central_gal, &central_before, sizeof(central_gal)) == 0,
                  "the by-galaxy quasar and burst entries write nothing on the FFB row");
    }
  }
  TEST_ASSERT(gal.UnstableDiskGasFraction == 0.0,
              "UnstableDiskGasFraction keeps its reset value 0 on the FFB row");
  TEST_ASSERT(memcmp(&gal, &row_tx, sizeof(gal)) == 0 &&
                  memcmp(&central_gal, &central_tx, sizeof(central_gal)) == 0,
              "the chain after the transaction leaves the row and its central as it left them");

  /* The input discriminates: the same post-transaction state on an ordinary row is unstable */
  memcpy(&gal, &row_tx, sizeof(gal));
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_disk_instability_process(&ctx, &row, 1) == 0 &&
                  gal.UnstableDiskGasFraction > 0.0,
              "with FFBRegime 0 the same state sets a positive trigger");

  chain_cleanup(status);
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: sage26_ffb_star_formation\n");
  printf("============================================================\n");

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_module_lifecycle);
  TEST_RUN(test_parameters_refused);
  TEST_RUN(test_placement_refused);
  TEST_RUN(test_dispatch_guards);
  TEST_RUN(test_chain_initialises);
  TEST_RUN(test_off_writes_nothing);
  TEST_RUN(test_non_ffb_row_not_written);
  TEST_RUN(test_initial_boundary_and_refusal);
  TEST_RUN(test_stars_formed_and_renormalised);
  TEST_RUN(test_pre_commit_metallicity);
  TEST_RUN(test_invalid_fire_scaling);
  TEST_RUN(test_hydrogen);
  TEST_RUN(test_guards_and_tdyn);
  TEST_RUN(test_satellite_routing);
  TEST_RUN(test_no_instability_no_transport);
  TEST_RUN(test_reheat_clamp_wiring);
  TEST_RUN(test_reheat_clamp_negative_gas_probe);
  TEST_RUN(test_sf_prescription_zero_probe);
  TEST_RUN(test_no_central);
  TEST_RUN(test_chain_leaves_trigger);

  TEST_SUMMARY();
  return TEST_RESULT();
}
