/**
 * @file    test_unit_sage26_ffb_star_formation.c
 * @brief   Behavioural tests of sage26_ffb_star_formation
 *
 * Every expectation is derived independently of the module and of the shared helpers it calls:
 * either a literal from inputs chosen so the result is exact, or SAGE26's starformation_ffb
 * written out in this file (sage26_ffb_written_out, with compute_sn_feedback_ffb,
 * update_from_star_formation, update_from_feedback, ionized_gas_fraction, get_metallicity,
 * capped_eta_reheat and sn_energy_coupling), statement by statement with SAGE26's operand order.
 * The written-out transaction never calls the package's code. A test runs the module's process()
 * and compares the forming row and its FoF central with the written-out end state byte for byte
 * (memcmp of the whole galaxy struct, so a field the module must not write, such as the
 * instability state or a transport field, is compared too), and asserts the named behaviour with
 * literals or the observer (sage26_ffb_star_formation.h). Where a test targets a misordering, it
 * also shows that its input discriminates: the written-out transaction run with that misordering
 * (a "variant") gives a different end state.
 *
 * The module's parameter refusals, its placement and the cross-module dispatch guards it applies at
 * EnhancedStarFormationOn 1 are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c and test_unit_sage_dependency_contracts.c.
 *
 * Behaviour (EnhancedStarFormationOn 1 unless stated):
 * - test_off_writes_nothing: nothing is written at EnhancedStarFormationOn 0, nor on an FFBRegime 0
 *   row at 1
 * - test_initial_boundary_and_refusal: the initial-boundary state writes nothing; an out-of-range
 *   stored SnapNum with dT > 0 is refused, naming it
 * - test_process_refusals: more than one row, and a substep outside the substep count (no history
 *   bin), are refused, naming what they refuse, with nothing written
 * - test_stars_formed_and_renormalised: the committed stellar mass and the history sample use the
 *   stars formed, the yield the renormalised stars; H2gas is zeroed and H1gas is the H I block's
 *   under the prescription-1 caps
 * - test_pre_commit_metallicity: the feedback uses the metallicity before the commit
 * - test_invalid_fire_scaling: Vvir 0 or z < 0 gives no reheating or ejection and leaves
 *   MassLoading as it was
 * - test_guards_and_tdyn: the NaN/infinity guards, a zero dynamical time and a row with no cold
 *   gas form no stars
 * - test_satellite_routing: a Type 1 row's reheated gas and metals go to its FoF central's pair,
 *   selected by the CENTRAL's Regime
 * - test_reheat_clamp_wiring: the reheated mass is clamped to the remaining cold gas before the
 *   feedback commit
 * - test_sf_prescription_zero_probe: SFprescription 0 (the commits' caps off)
 * - test_no_central: with no FoF central only H2gas, H1gas and the history are written
 * - test_chain_leaves_trigger: the rest of the by-galaxy chain, initialised through
 *   module_system_init over the run files' configuration, leaves an FFB satellite with an unstable
 *   disk, and its central, as the transaction left them
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

/* The FFB configuration's values */
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
static char ordinary_sf_name[] = "sage26_calculate_star_formation";
static char classifier_name[] = "sage26_determine_ffb_regime";
static struct PhaseModuleConfig substep_entries[2];
static struct PhaseModuleConfig classifier_entries[1];
static struct ModulePhaseConfig substep_phases[1];

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }
static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static void set_entry(struct PhaseModuleConfig *entry, char *name, const enum ProcessingMode mode) {
  entry->module_name = name;
  entry->processing_mode = mode;
  entry->resolved = NULL;
}

/**
 * @brief The module once by galaxy in a substep phase, followed by sage26_calculate_star_formation
 *        by galaxy, and sage26_determine_ffb_regime in pre_timestep: the dispatch the cross-module
 *        guards require at EnhancedStarFormationOn 1
 */
static void set_pipeline(void) {
  set_entry(&substep_entries[0], module_name, PROCESSING_MODE_BY_GALAXY);
  set_entry(&substep_entries[1], ordinary_sf_name, PROCESSING_MODE_BY_GALAXY);
  substep_phases[0].name = phase_name;
  substep_phases[0].modules = substep_entries;
  substep_phases[0].num_modules = 2;
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

static const struct test_params FFB_PARAMS = {"1", "0.2", "1", "1", "1", "0.0"};

/**
 * @brief Configure the run, the parameters and the complete dispatch, and initialise the module
 * @return The init status
 */
static int init_with(const struct test_params *p) {
  reset_config();
  set_run();
  set_pipeline();
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
  return sage26_ffb_star_formation_init();
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

/** SAGE26's get_metallicity, written out */
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

/** SAGE26's ionized_gas_fraction, written out */
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

/** The hydrogen cap of the commits, written out */
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

/** SAGE26's compute_sn_feedback_ffb, written out; returns the reheated and ejected masses */
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
 *  reheated-mass clamp bound, from its last call */
static double written_out_reheated_commit;
static int written_out_clamp_binds;

/**
 * @brief SAGE26's starformation_ffb on g with its FoF central c (c may be g), plus
 *        the package's StarFormationRate diagnostic, run with the given variant
 *
 * SAGE parity: SAGE26's multiply-adds are written with fma(), as SAGE26 computes them.
 */
static void sage26_ffb_written_out(struct GalaxyData *g, struct GalaxyData *c, const struct tx *in,
                                   const int variant) {
  const double reff = TEST_SF_DISK_FACTOR * g->DiskScaleRadius;
  const double tdyn = (reff > 0.0 && in->vvir > 0.0) ? reff / in->vvir : 0.0;
  g->H2gas = 0.0;
  {
    double atomicH = (double)g->ColdGas * 0.74;
    if (atomicH < 0.0) {
      atomicH = 0.0;
    }
    const double f_ion = f_ion_of(g->ColdGas, 1.0 * g->DiskScaleRadius, TEST_HUBBLE_H);
    atomicH *= (1.0 - f_ion);
    g->H1gas = atomicH;
  }
  const double gas_for_sf = g->ColdGas;
  double stars;
  if (isnan(g->ColdGas) || isinf(g->ColdGas) || isnan(in->vvir) || isinf(in->vvir) || isnan(reff) ||
      isinf(reff) || isnan(tdyn) || isinf(tdyn)) {
    stars = 0.0;
  } else if (tdyn > 0.0 && gas_for_sf > 0.0) {
    const double strdot = in->efficiency * gas_for_sf / tdyn;
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
    *history(g, 0, in->bin) += commit_stars / in->dt;
  }
  *history(g, 1, in->bin) = g->ColdGas;
  *history(g, 2, in->bin) = g->MetalsColdGas;

  double metallicity = metallicity_of(g->ColdGas, g->MetalsColdGas);
  {
    /* update_from_star_formation */
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
    budget(in, g->ColdGas, &fb_stars, &reheated, &ejected, &g->MassLoading);
  }
  if (variant & VAR_RECOMPUTE_Z) {
    metallicity = metallicity_of(g->ColdGas, g->MetalsColdGas);
  }
  written_out_clamp_binds = reheated > g->ColdGas;
  if (reheated > g->ColdGas) {
    reheated = g->ColdGas;
  }
  written_out_reheated_commit = reheated;

  /* update_from_feedback */
  struct GalaxyData *dest = (variant & VAR_FEEDBACK_TO_ROW) ? g : c;
  if (reheated > g->ColdGas) {
    reheated = g->ColdGas;
  }
  g->ColdGas -= reheated;
  g->MetalsColdGas = (float)fma(-metallicity, reheated, (double)g->MetalsColdGas);
  cap_hydrogen(g, in->sf_prescription);
  const int to_cgm = in->cgm_recipe_on == 1 && dest->Regime == 0;
  float *gas = to_cgm ? &dest->CGMgas : &dest->HotGas;
  float *metals = to_cgm ? &dest->MetalsCGMgas : &dest->MetalsHotGas;
  *gas += reheated;
  *metals = (float)fma(metallicity, reheated, (double)*metals);
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
  g->SupernovaOutflowRate += reheated;

  /* the yield */
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

/** @brief Run process() on `ngal` rows (the first's central `central`, NULL for none) */
static int run_rows(struct Halo *rows, const int ngal, struct Halo *central, const int num_substeps,
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
  const int status = sage26_ffb_star_formation_process(&ctx, rows, ngal);
  sage26_ffb_star_formation_set_test_observer(NULL, NULL);
  return status;
}

/** @brief Run process() on row (its central `central`, NULL for none) at the test substep */
static int run_at(struct Halo *row, struct Halo *central, const int num_substeps,
                  const int substep) {
  return run_rows(row, 1, central, num_substeps, substep);
}

/** @brief As run_rows(), capturing the log (valid until the next capture) in *log */
static int run_rows_logged(struct Halo *rows, const int ngal, struct Halo *central,
                           const int num_substeps, const int substep, const char **log) {
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = run_rows(rows, ngal, central, num_substeps, substep);
  *log = "";
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  }
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
// THE RUN FILES' CHAIN
// ============================================================================

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

/**
 * @brief Configure the run files' pre_timestep classifier and by-galaxy chain with every key the
 *        modules read, at EnhancedStarFormationOn 1, and run module_system_init, capturing the
 *        log; the caller cleans up with chain_cleanup()
 * @return The module_system_init status
 */
static int chain_init(const char **log) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  set_run();
  test_pre_timestep_add(classifier_name, PROCESSING_MODE_FULL_HALO);
  for (int k = 0; k < CHAIN_LENGTH; k++) {
    test_phase_add(phase_name, CHAIN[k], PROCESSING_MODE_BY_GALAXY);
  }
  MimicConfig.SubSteps = TEST_NUM_SUBSTEPS;
  set_test_model_parameters();
  sage26_test_append_model_parameter("EnhancedStarFormationOn", "1");
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

// ============================================================================
// BEHAVIOUR
// ============================================================================

/**
 * @test    test_off_writes_nothing
 * @brief   An FFBRegime 1 row is not written at EnhancedStarFormationOn 0, and an FFBRegime 0 row
 *          is not written at 1: the whole galaxy is unchanged and the observer is not called
 */
int test_off_writes_nothing(void) {
  static const struct test_params off = {"0", "0.2", "1", "1", "1", "0.0"};
  const struct {
    const char *label;
    const struct test_params *params;
    int ffb_regime;
  } rows[] = {{"EnhancedStarFormationOn 0, FFBRegime 1", &off, 1},
              {"EnhancedStarFormationOn 1, FFBRegime 0", &FFB_PARAMS, 0}};
  for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
    TEST_ASSERT(init_with(rows[k].params) == 0, "init");
    struct Halo row;
    struct GalaxyData gal;
    make_row(&row, &gal, 0, 80.0);
    gal.FFBRegime = rows[k].ffb_regime;
    const struct GalaxyData before = gal;
    TEST_ASSERT(run(&row, &row) == 0, "process succeeds");
    printf("\n  %s: observer called %d times", rows[k].label, observed_count);
    TEST_ASSERT(galaxies_equal(&gal, &before) && observed_count == 0,
                "a row the module does not take is not written");
  }
  return TEST_PASS;
}

int test_initial_boundary_and_refusal(void) {
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
  struct Halo row;
  struct GalaxyData gal;
  make_row(&row, &gal, 0, 80.0);
  row.SnapNum = -1;
  row.dT = 0.0;
  struct GalaxyData before = gal;
  TEST_ASSERT(run(&row, &row) == 0, "the initial-boundary state returns 0");
  TEST_ASSERT(galaxies_equal(&gal, &before) && observed.path == SAGE26_FFB_PATH_INITIAL_BOUNDARY,
              "the initial-boundary state writes nothing");
  TEST_ASSERT(unformed_budget_and_commit(&observed) && nan_f64(observed.dt) &&
                  nan_f64(observed.stars_formed),
              "the observed trace holds the not-formed sentinels (NAN, -1)");

  make_row(&row, &gal, 0, 80.0);
  row.SnapNum = 64; /* outside ZZ's 64 entries, with dT > 0 */
  before = gal;
  const char *log = "";
  const int status = run_rows_logged(&row, 1, &row, TEST_NUM_SUBSTEPS, TEST_SUBSTEP, &log);
  TEST_ASSERT(status != 0, "an out-of-range stored SnapNum with dT > 0 is refused");
  TEST_ASSERT(strstr(log, "SnapNum = 64") != NULL, "the refusal names the SnapNum");
  TEST_ASSERT(galaxies_equal(&gal, &before), "nothing is written before the refusal");
  return TEST_PASS;
}

/**
 * @test    test_process_refusals
 * @brief   Two rows (process_by_galaxy takes one) and a substep outside [0, num_substeps) (no
 *          history bin) are refused, naming what they refuse, with nothing written to either row
 */
int test_process_refusals(void) {
  static const struct {
    const char *label;
    int ngal, substep;
    const char *named;
  } refused[] = {{"two rows", 2, TEST_SUBSTEP, "ngal=1, got 2"},
                 {"substep at the substep count", 1, TEST_NUM_SUBSTEPS, "history bin"},
                 {"negative substep", 1, -1, "history bin"}};
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    struct Halo rows[2];
    struct GalaxyData gals[2];
    make_row(&rows[0], &gals[0], 0, 80.0);
    make_row(&rows[1], &gals[1], 0, 80.0);
    const struct GalaxyData before[2] = {gals[0], gals[1]};
    const char *log = "";
    const int status = run_rows_logged(rows, refused[k].ngal, &rows[0], TEST_NUM_SUBSTEPS,
                                       refused[k].substep, &log);
    printf("\n  %s: status %d", refused[k].label, status);
    TEST_ASSERT(status != 0, "the module must refuse");
    TEST_ASSERT(strstr(log, refused[k].named) != NULL, "the error must name what it refuses");
    TEST_ASSERT(galaxies_equal(&gals[0], &before[0]) && galaxies_equal(&gals[1], &before[1]),
                "nothing is written before the refusal");
  }
  return TEST_PASS;
}

int test_stars_formed_and_renormalised(void) {
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
  struct Halo row;
  struct GalaxyData gal, expected;
  /* Vvir 2 km/s (a large loading) and a short tdyn = 3 * 0.004 / 2 = 0.006: strdot * dt =
   * 0.2 * 1 / 0.006 * 0.125 > ColdGas 1, so the stars are capped at exactly 1 */
  make_row(&row, &gal, 0, 2.0);
  gal.H1gas = 9.0f;
  const struct GalaxyData input = gal;
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "the end state equals SAGE26's written-out starformation_ffb byte for byte");
  TEST_ASSERT(observed.stars_cap_binds == 1 && same_f64(observed.stars_formed, 1.0),
              "the stars are capped at ColdGas = 1");
  TEST_ASSERT(same_f32(gal.StellarMass, (float)((double)2.5f + (1 - TEST_RECYCLE))),
              "the committed stellar mass gains (1 - R) * the stars formed (1)");
  TEST_ASSERT(same_f32(*history(&gal, 0, TEST_BIN), (float)(0.125 + 1.0 / 0.125)),
              "the history samples the stars formed over dt: SfrDisk += 1 / 0.125");
  TEST_ASSERT(same_f32(*history(&gal, 1, TEST_BIN), 1.0f) &&
                  same_f32(*history(&gal, 2, TEST_BIN), 0.0234567f),
              "the history samples the cold gas before any commit");
  TEST_ASSERT(observed.feedback.renorm_binds == 1 && observed.yield_stars < 1.0 &&
                  same_f64(observed.yield_stars, observed.feedback.stars_out),
              "the budget renormalises the stars, and the yield receives the renormalised stars");
  /* The hydrogen: H2gas is zeroed, H2DepletionTime_Gyr is not written, H1gas is the H I block of
   * the input cold gas, before the commits' caps, then capped at the hydrogen of what remains */
  TEST_ASSERT(same_f32(gal.H2gas, 0.0f) && !signbit(gal.H2gas), "H2gas is zeroed");
  TEST_ASSERT(same_f32(gal.H2DepletionTime_Gyr, 1.5f), "H2DepletionTime_Gyr is not written");
  const double f_ion = f_ion_of(1.0f, 0.004f, TEST_HUBBLE_H);
  const float h1_block = (float)((1.0 * 0.74) * (1.0 - f_ion));
  const float max_h = (float)((double)gal.ColdGas * 0.74);
  TEST_ASSERT(same_f32(gal.H1gas, h1_block < max_h ? h1_block : max_h),
              "H1gas is the H I block's, capped at the hydrogen of the remaining cold gas");
  TEST_ASSERT(observed.sf_caps.tracks_h2 == 1 && observed.sf_caps.h1_cap_binds == 1,
              "the prescription-1 cap binds on H1gas after the commit");
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
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
  struct Halo row;
  struct GalaxyData gal, expected;
  make_row(&row, &gal, 0, 30.0);
  gal.ColdGas = 0.7f;
  gal.MetalsColdGas = 0.0123f;
  /* The input discriminates: the feedback with the metallicity recomputed after the commit gives
   * a different end state */
  const struct GalaxyData input = gal;
  const struct GalaxyData plain = variant_of(&input, &row, VAR_NONE);
  const struct GalaxyData recomputed = variant_of(&input, &row, VAR_RECOMPUTE_Z);
  TEST_ASSERT(!galaxies_equal(&plain, &recomputed),
              "the recomputed metallicity would change the end state");
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "the end state equals SAGE26's, with the pre-commit metallicity");
  TEST_ASSERT(observed.commit.reheated_mass_clamped > 0.0,
              "the feedback commit moves gas with that metallicity");
  return TEST_PASS;
}

int test_invalid_fire_scaling(void) {
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
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
                "SAGE parity: an invalid FIRE scaling leaves MassLoading stale");
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

int test_guards_and_tdyn(void) {
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
  struct Halo row;
  struct GalaxyData gal, expected;
  /* NaN ColdGas, infinite Vvir, NaN radius: stars 0 */
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
  /* tdyn 0 from a zero radius, a zero Vvir and a negative radius (reff > 0.0 false) */
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
  /* A positive tdyn but no cold gas (all consumed): no stars, and the yield's metals all go to the
   * central because the cold gas is below the threshold */
  make_row(&row, &gal, 0, 80.0);
  gal.ColdGas = 0.0f;
  TEST_ASSERT(central_matches(&row, 1, 1, 0.0, &expected),
              "no cold gas: the end state equals SAGE26's written-out transaction");
  TEST_ASSERT(observed.tdyn > 0.0 && observed.tdyn_gas_positive == 0 &&
                  same_f64(observed.stars_formed, 0.0) &&
                  observed.yield.coldgas_above_threshold == 0,
              "tdyn is positive, no gas forms no stars, and the cold gas is below the threshold");
  return TEST_PASS;
}

int test_satellite_routing(void) {
  static const struct {
    const char *cgm;
    int central_regime;
    int to_cgm;
  } runs[3] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}};
  for (int k = 0; k < 3; k++) {
    struct test_params p = FFB_PARAMS;
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
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
  /* An in-domain state where the reheated-mass clamp binds with positive cold gas. A satellite
   * with its own Vvir 1e-10 at a constructed stored-snapshot redshift 1e8 has a loading near
   * 1e16, so the renormalised reheated mass rounds above the cold gas the star-formation commit
   * left */
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
              "the input binds the reheated-mass clamp with positive cold gas");

  const int status = run_at(&row, &central, 10, 3);
  MimicConfig.ZZ[16] = (63 - 16) / 10.0;
  TEST_ASSERT(status == 0, "process succeeds");
  TEST_ASSERT(galaxies_equal(&gal, &exp_row) && galaxies_equal(&cgal, &exp_central),
              "row and central equal SAGE26's written-out transaction byte for byte");
  TEST_ASSERT(observed.reheat_clamp_binds == 1 &&
                  observed.reheated_from_budget > observed.reheated_to_commit,
              "the clamp binds and lowers the budget's reheated mass");
  TEST_ASSERT(same_f64(observed.reheated_to_commit, written_out_reheated_commit),
              "wiring: the reheated mass passed to the feedback commit is the remaining "
              "ColdGas");
  return TEST_PASS;
}

int test_sf_prescription_zero_probe(void) {
  struct test_params p = FFB_PARAMS;
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
  TEST_ASSERT(init_with(&FFB_PARAMS) == 0, "init with the FFB configuration");
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

/** SAGE26's check_disk_instability criterion */
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
 * runs no instability check, so neither grow_black_hole nor the instability burst runs.
 * The modules are initialised by module_system_init over the run files' configuration at
 * EnhancedStarFormationOn 1, then each process() runs on the FFB satellite in the run files'
 * order. The trigger starts at its per-snapshot reset value 0 (init_repeat); a positively seeded
 * trigger would survive the write-free path and is not this case.
 */
int test_chain_leaves_trigger(void) {
  const char *log = "";
  const int status = chain_init(&log);
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

  TEST_RUN(test_off_writes_nothing);
  TEST_RUN(test_initial_boundary_and_refusal);
  TEST_RUN(test_process_refusals);
  TEST_RUN(test_stars_formed_and_renormalised);
  TEST_RUN(test_pre_commit_metallicity);
  TEST_RUN(test_invalid_fire_scaling);
  TEST_RUN(test_guards_and_tdyn);
  TEST_RUN(test_satellite_routing);
  TEST_RUN(test_reheat_clamp_wiring);
  TEST_RUN(test_sf_prescription_zero_probe);
  TEST_RUN(test_no_central);
  TEST_RUN(test_chain_leaves_trigger);

  TEST_SUMMARY();
  return TEST_RESULT();
}
