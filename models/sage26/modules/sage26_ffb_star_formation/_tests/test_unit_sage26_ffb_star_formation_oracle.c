/**
 * @file    test_unit_sage26_ffb_star_formation_oracle.c
 * @brief   Bit-exact comparison of sage26_ffb_star_formation with SAGE26's starformation_ffb and
 *          the wrapper's FFB dispatch
 *
 * Runs every fixture fixtures.manifest lists for sage26_ffb_star_formation (written by the
 * SAGE26 function-level oracle, oracle_c13; README "The c13 Group, Part 2: FFB Star Formation"):
 * routines starformation_ffb (S:1262-1368) and starformation_and_feedback_ffb (the wrapper
 * starformation_and_feedback, S:995-1110, at EnhancedStarFormationOn 1 on an FFBRegime 1 row,
 * which returns through starformation_ffb at S:1004-1008). S = the staged SAGE26 source's
 * src/model_starformation_and_feedback.c.
 *
 * For each fixture the module is initialised with the fixture's flags and parameters (every
 * required key set explicitly, EnhancedStarFormationOn 1, init asserted to succeed), the row and
 * its FoF central are built as halos (SAGE26's float halo values exactly widened, every other
 * member poisoned), the run's redshift table holds only the forming galaxy's stored snapshot's
 * redshift (every other entry NaN), and process() runs on the forming row with the substep and
 * interval the fixture gives (num_substeps = effective_steps, dT = deltaT, so the module's dt is
 * asserted equal to call.dt). The test then asserts, bit for bit:
 *
 * - every recorded float of every galaxy (the cold, stellar, bulge, hot, CGM and ejected
 *   reservoirs, the bulge radii, H1gas, H2gas, H2DepletionTime_Gyr, OutflowRate, MassLoading, the
 *   black hole and all sixty history elements), its Regime and FFBRegime;
 * - the transport fields NewStellarMass, SupernovaReheatedMass, SupernovaEjectedMass and the
 *   instability trigger UnstableDiskGasFraction keep their seeded values; the package's own
 *   StarFormationRate diagnostic (not a SAGE26 output) is the stars formed over dT;
 * - the history bin, every branch flag, and every carried value with its markers against the
 *   module's test observer (sage26_ffb_star_formation.h), including the reheated mass the module
 *   passes into the shared feedback commit (reheated_mass_commit, the wiring assertion on the
 *   clamp at S:1346, which the commit repeats);
 * - the shared helpers called directly: the H I block's atomic hydrogen and ionised fraction
 *   (shared/sage_hi_physics.h) from the fixture's cold gas and radius, and the FFB feedback budget
 *   (shared/sage_ffb_feedback_physics.h) from the oracle's stars formed and cold gas after the
 *   star-formation commit, against the recorded renormalised stars, masses and MassLoading;
 * - for the wrapper fixtures, the oracle's check that the wrapper's end state is
 *   starformation_ffb's, and the instability observation recomputed from the package's end state.
 *
 * The oracle's observations (recomputed_metallicity_changes_state,
 * yield_with_formed_stars_changes_state, metallicity_post_commit and its _differs flag) are not
 * package or SAGE26 state and are consumed unasserted; f_ion_path is not reported by the shared
 * helper and is consumed unasserted (the helper's f_ion is compared bit for bit instead).
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-identity test reports SKIP.
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/sage26_ffb_star_formation/sage26_ffb_star_formation.h"
#include "shared/sage_ffb_feedback_physics.h"
#include "shared/sage_hi_physics.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"
#include "shared/time_parity.h"

extern int sage26_ffb_star_formation_init(void);
extern int sage26_ffb_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_ffb_star_formation_cleanup(void);

static const char *MODULE = "sage26_ffb_star_formation";

#define MAX_GALAXIES 4
#define SNAPLISTLEN 64
#define CHECK_CONTINUATION 1
#define CHECK_TAIL 2
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* SAGE26's R0 unit factors (R/input/millennium.par:103-107) and its SF_DISK_RADIUS_FRAC (S:60) */
#define SAGE26_UNIT_LENGTH_IN_CM 3.08568e+24
#define SAGE26_UNIT_MASS_IN_G 1.989e+43
#define SAGE26_UNIT_VELOCITY_IN_CM_PER_S 100000.0
#define SAGE26_SF_DISK_RADIUS_FRAC 3.0

/* Distinctive seeds of the fields the transaction must not write */
#define SEED_NEW_STELLAR_MASS 0.375
#define SEED_REHEATED 0.625
#define SEED_EJECTED 0.875
#define SEED_UNSTABLE_FRACTION 0.3125

/** Tallies over one run (quoted in the validation record) */
struct ffb_tally {
  int fixtures;
  int wrapper;
  int continuation; /* present and continuation-checked */
  int tail;         /* present and tail-checked */
  int validated;    /* validated two-sided */
  int narrowing_limited;
  int not_validated;
  int absent;
  int binding; /* binding clamps and caps asserted equal to their bound */
  int fields;  /* end-state fields compared */
  int direct_helper_calls;
};

static struct ffb_tally tally;

static char module_name[] = "sage26_ffb_star_formation";
static char phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig phase_entry[1];
static struct ModulePhaseConfig phase;

/** The package float member of a SAGE26 struct GALAXY name (README "Field Names") */
static float *member(struct GalaxyData *g, const char *name) {
  const struct {
    const char *sage26;
    float *address;
  } table[] = {{"ColdGas", &g->ColdGas},
               {"MetalsColdGas", &g->MetalsColdGas},
               {"StellarMass", &g->StellarMass},
               {"MetalsStellarMass", &g->MetalsStellarMass},
               {"BulgeMass", &g->BulgeMass},
               {"MetalsBulgeMass", &g->MetalsBulgeMass},
               {"InstabilityBulgeMass", &g->InstabilityBulgeMass},
               {"MergerBulgeMass", &g->MergerBulgeMass},
               {"BulgeRadius", &g->BulgeRadius},
               {"MergerBulgeRadius", &g->MergerBulgeRadius},
               {"InstabilityBulgeRadius", &g->InstabilityBulgeRadius},
               {"HotGas", &g->HotGas},
               {"MetalsHotGas", &g->MetalsHotGas},
               {"CGMgas", &g->CGMgas},
               {"MetalsCGMgas", &g->MetalsCGMgas},
               {"EjectedMass", &g->EjectedGas},
               {"MetalsEjectedMass", &g->MetalsEjectedGas},
               {"H1gas", &g->H1gas},
               {"H2gas", &g->H2gas},
               {"H2DepletionTime_Gyr", &g->H2DepletionTime_Gyr},
               {"OutflowRate", &g->SupernovaOutflowRate},
               {"MassLoading", &g->MassLoading},
               {"BlackHoleMass", &g->BlackHoleMass},
               {"QuasarModeBHaccretionMass", &g->QuasarModeBHaccretionMass},
               {"DiskScaleRadius", &g->DiskScaleRadius}};
  for (size_t k = 0; k < COUNT(table); k++) {
    if (strcmp(table[k].sage26, name) == 0) {
      return table[k].address;
    }
  }
  return NULL;
}

static const char *const STATE[] = {"ColdGas",
                                    "MetalsColdGas",
                                    "StellarMass",
                                    "MetalsStellarMass",
                                    "BulgeMass",
                                    "MetalsBulgeMass",
                                    "InstabilityBulgeMass",
                                    "MergerBulgeMass",
                                    "BulgeRadius",
                                    "MergerBulgeRadius",
                                    "InstabilityBulgeRadius",
                                    "HotGas",
                                    "MetalsHotGas",
                                    "CGMgas",
                                    "MetalsCGMgas",
                                    "EjectedMass",
                                    "MetalsEjectedMass",
                                    "H1gas",
                                    "H2gas",
                                    "H2DepletionTime_Gyr",
                                    "OutflowRate",
                                    "MassLoading",
                                    "BlackHoleMass",
                                    "QuasarModeBHaccretionMass"};

static const struct {
  const char *name;
  enum MimicSageSfrHistory history;
} HISTORIES[] = {{"SfrDisk", MIMIC_SFR_HISTORY_DISK},
                 {"SfrDiskColdGas", MIMIC_SFR_HISTORY_DISK_COLD_GAS},
                 {"SfrDiskColdGasMetals", MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS},
                 {"SfrBulge", MIMIC_SFR_HISTORY_BULGE},
                 {"SfrBulgeColdGas", MIMIC_SFR_HISTORY_BULGE_COLD_GAS},
                 {"SfrBulgeColdGasMetals", MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS}};

static const char *gfield(const int i, const char *name) { return oracle_galaxy_field(i, name); }

static const char *hfield(const int i, const char *name, const int k) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "galaxy[%d].%s[%d]", i, name, k);
  return buffer;
}

/** The observer's record of the one call */
static struct Sage26FfbStarFormationTrace observed;
static int observed_calls;

static void observer(void *user, const struct Sage26FfbStarFormationTrace *trace) {
  (void)user;
  observed = *trace;
  observed_calls++;
}

/** @brief Check one carried value: its presence, markers, check kind and value bit for bit */
static int check_value(struct oracle_fixture *out, const char *name, const int present,
                       const double value, const int kind) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != !present) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", present ? "present" : "absent");
    return 1;
  }
  if (absent) {
    tally.absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  problems += oracle_check_i32(out, field, 1);
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  problems += oracle_check_i32(out, field, kind);
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_narrowing_limited", name);
  const int narrowing = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down && !narrowing)) {
    fprintf(stderr, "oracle: %s: %s_validated disagrees with its markers\n", out->path, name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  problems += oracle_check_f64(out, field, value);
  if (kind == CHECK_CONTINUATION) {
    tally.continuation++;
  } else {
    tally.tail++;
  }
  if (validated) {
    tally.validated++;
  } else if (narrowing) {
    tally.narrowing_limited++;
  } else {
    tally.not_validated++;
  }
  return problems;
}

static int check_flag(struct oracle_fixture *out, const char *name, const int value) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  return oracle_check_i32(out, field, value);
}

/** @brief Apply the fixture's flags and parameters, then initialise the module */
static int setup_parameters(struct oracle_fixture *in, const int wrapper) {
  int problems = 0;
  reset_config();
  phase_entry[0].module_name = module_name;
  phase_entry[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  phase_entry[0].resolved = NULL;
  phase.name = phase_name;
  phase.modules = phase_entry;
  phase.num_modules = 1;
  MimicConfig.substep_phases = &phase;
  MimicConfig.num_substep_phases = 1;

  /* SupernovaRecipeOn 1 is the package's only recipe; SaveFullSFH 0 (off on every rung) */
  problems += oracle_in_i32(in, "params.SupernovaRecipeOn") != 1;
  problems += oracle_in_i32(in, "params.SaveFullSFH") != 0;
  if (wrapper) {
    problems += oracle_in_i32(in, "params.EnhancedStarFormationOn") != 1;
    MimicConfig.G = oracle_in_f64(in, "params.G");
  }

  /* SAGE26's R0 unit factors, from which the module derives EtaSNcode and EnergySNcode */
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  MimicConfig.UnitLength_in_cm = SAGE26_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = SAGE26_UNIT_MASS_IN_G;
  MimicConfig.UnitVelocity_in_cm_per_s = SAGE26_UNIT_VELOCITY_IN_CM_PER_S;
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g *
                                  (MimicConfig.UnitLength_in_cm * MimicConfig.UnitLength_in_cm) /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  if (oracle_f64_to_bits(
          mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h)) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EnergySNcode")) ||
      oracle_f64_to_bits(mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h)) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EtaSNcode"))) {
    fprintf(stderr, "oracle: %s: the package's EnergySNcode or EtaSNcode differs from SAGE26's\n",
            in->path);
    problems++;
  }

  /* Every key the module reads, set explicitly (campaign decision D33) */
  problems += oracle_set_model_param("EnhancedStarFormationOn", 1.0) != 0;
  problems += oracle_set_model_param("StarFormingDiskFactor", SAGE26_SF_DISK_RADIUS_FRAC) != 0;
  const char *const ints[] = {"SFprescription", "CGMrecipeOn", "FIREmodeOn"};
  for (size_t k = 0; k < COUNT(ints); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", ints[k]);
    problems += oracle_set_model_param(ints[k], (double)oracle_in_i32(in, field)) != 0;
  }
  const char *const doubles[] = {
      "EnhancedSFEfficiency",       "RecycleFraction", "FeedbackReheatingEpsilon",
      "FeedbackEjectionEfficiency", "Yield",           "FracZleaveDisk"};
  for (size_t k = 0; k < COUNT(doubles); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", doubles[k]);
    problems += oracle_set_model_param(doubles[k], oracle_in_f64(in, field)) != 0;
  }
  if (sage26_ffb_star_formation_init() != 0) {
    fprintf(stderr, "oracle: %s: sage26_ffb_star_formation_init() failed\n", in->path);
    problems++;
  }
  return problems;
}

static int check_fixture(const char *name) {
  /* The wrapper fixtures are named ffbw_* (README "The c13 Group, Part 2") */
  const int wrapper = strncmp(name, "ffbw_", 5) == 0;
  struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name,
                       wrapper ? "starformation_and_feedback_ffb" : "starformation_ffb", &in,
                       &out) != 0) {
    return 1;
  }
  int problems = setup_parameters(&in, wrapper);

  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int p = oracle_in_i32(&in, "call.p");
  const int centralgal = oracle_in_i32(&in, "call.centralgal");
  const int step = oracle_in_i32(&in, "call.step");
  const int effective_steps = oracle_in_i32(&in, "call.effective_steps");
  const double deltaT = oracle_in_f64(&in, "call.deltaT");
  const double call_dt = oracle_in_f64(&in, "call.dt");
  if (wrapper) {
    (void)oracle_in_f64(&in, "call.time"); /* not read on the FFB path (S:1004-1008) */
  }
  if (ngal < 1 || ngal > MAX_GALAXIES || p < 0 || p >= ngal || centralgal < 0 ||
      centralgal >= ngal) {
    fprintf(stderr, "oracle: %s: call indices out of range\n", in.path);
    return problems + 1;
  }

  static struct GalaxyData galaxies[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES];
  float vmax[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *g = &galaxies[i];
    struct Halo *h = &halos[i];
    oracle_poison(g, sizeof(*g));
    oracle_poison(h, sizeof(*h));
    h->galaxy = g;
    h->HaloNr = i;
    h->Type = oracle_in_i32(&in, gfield(i, "Type"));
    h->SnapNum = oracle_in_i32(&in, gfield(i, "SnapNum"));
    h->dT = deltaT; /* core-owned: SAGE26's deltaT of the group */
    g->Regime = oracle_in_i32(&in, gfield(i, "Regime"));
    g->FFBRegime = oracle_in_i32(&in, gfield(i, "FFBRegime"));
    /* SAGE26's float halo values, exactly widened */
    h->Mvir = oracle_in_f32(&in, gfield(i, "Mvir"));
    h->Vvir = oracle_in_f32(&in, gfield(i, "Vvir"));
    vmax[i] = oracle_in_f32(&in, gfield(i, "Vmax"));
    h->Vmax = vmax[i];
    g->DiskScaleRadius = oracle_in_f32(&in, gfield(i, "DiskScaleRadius"));
    for (size_t m = 0; m < COUNT(STATE); m++) {
      *member(g, STATE[m]) = oracle_in_f32(&in, gfield(i, STATE[m]));
    }
    for (size_t m = 0; m < COUNT(HISTORIES); m++) {
      for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
        *mimic_sage_sfr_history(g, HISTORIES[m].history, k) =
            oracle_in_f32(&in, hfield(i, HISTORIES[m].name, k));
      }
    }
    g->NewStellarMass = SEED_NEW_STELLAR_MASS;
    g->SupernovaReheatedMass = SEED_REHEATED;
    g->SupernovaEjectedMass = SEED_EJECTED;
    g->UnstableDiskGasFraction = SEED_UNSTABLE_FRACTION;
    g->StarFormationRate = 0.0f;
  }
  /* The run's redshift table: only the forming galaxy's stored snapshot's entry (S:915) */
  for (int s = 0; s < ABSOLUTEMAXSNAPS; s++) {
    MimicConfig.ZZ[s] = NAN;
  }
  MimicConfig.Snaplistlen = SNAPLISTLEN;
  char zz_field[ORACLE_MAX_NAME];
  snprintf(zz_field, sizeof(zz_field), "params.ZZ[%d]", halos[p].SnapNum);
  if (halos[p].SnapNum < 0 || halos[p].SnapNum >= SNAPLISTLEN) {
    fprintf(stderr, "oracle: %s: the stored SnapNum is outside the table\n", in.path);
    return problems + 1;
  }
  MimicConfig.ZZ[halos[p].SnapNum] = oracle_in_f64(&in, zz_field);
  const double redshift = MimicConfig.ZZ[halos[p].SnapNum];

  struct GalaxyData input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* The shared H I block's pieces, called directly on the input (H2gas zeroed, S:1280-1291) */
  const double atomicH_raw = fma((double)input[p].ColdGas, SAGE_HYDROGEN_MASS_FRAC, -0.0);
  const double atomicH = atomicH_raw < 0.0 ? 0.0 : atomicH_raw;
  const double f_ion = mimic_sage_ionized_gas_fraction(
      input[p].ColdGas, mimic_sage_atomic_disk_radius(input[p].DiskScaleRadius),
      MimicConfig.Hubble_h, MIMIC_SAGE_SIGMA_HI_CRIT);
  tally.direct_helper_calls++;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halos[centralgal];
  ctx.num_substeps = effective_steps;
  ctx.substep_number = step;

  observed_calls = 0;
  sage26_ffb_star_formation_set_test_observer(observer, NULL);
  const int status = sage26_ffb_star_formation_process(&ctx, &halos[p], 1);
  sage26_ffb_star_formation_set_test_observer(NULL, NULL);
  if (status != 0 || observed_calls != 1 || observed.path != SAGE26_FFB_PATH_COMPLETE) {
    fprintf(stderr, "oracle: %s: process() returned %d, observed %d calls, path %d\n", in.path,
            status, observed_calls, (int)observed.path);
    return problems + 1;
  }
  const struct Sage26FfbStarFormationTrace *t = &observed;
  if (oracle_f64_to_bits(t->dt) != oracle_f64_to_bits(call_dt)) {
    fprintf(stderr, "oracle: %s: the module's dt differs from call.dt\n", in.path);
    problems++;
  }
  if (oracle_f64_to_bits(t->redshift) != oracle_f64_to_bits(redshift)) {
    fprintf(stderr, "oracle: %s: the module read another redshift\n", in.path);
    problems++;
  }

  /* The end state of every galaxy */
  problems += oracle_check_i32(&out, "intermediate.step_bin", t->bin);
  for (int i = 0; i < ngal; i++) {
    for (size_t m = 0; m < COUNT(STATE); m++) {
      problems += oracle_check_f32(&out, gfield(i, STATE[m]), *member(&galaxies[i], STATE[m]));
      tally.fields++;
    }
    for (size_t m = 0; m < COUNT(HISTORIES); m++) {
      for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
        problems +=
            oracle_check_f32(&out, hfield(i, HISTORIES[m].name, k),
                             *mimic_sage_sfr_history(&galaxies[i], HISTORIES[m].history, k));
        tally.fields++;
      }
    }
    problems += oracle_check_i32(&out, gfield(i, "Regime"), galaxies[i].Regime);
    problems += oracle_check_i32(&out, gfield(i, "FFBRegime"), galaxies[i].FFBRegime);
    /* No transport field and no instability trigger is written */
    if (galaxies[i].NewStellarMass != SEED_NEW_STELLAR_MASS ||
        galaxies[i].SupernovaReheatedMass != SEED_REHEATED ||
        galaxies[i].SupernovaEjectedMass != SEED_EJECTED ||
        galaxies[i].UnstableDiskGasFraction != SEED_UNSTABLE_FRACTION) {
      fprintf(stderr, "oracle: %s: galaxy[%d]'s transport fields changed\n", in.path, i);
      problems++;
    }
    /* The package's StarFormationRate diagnostic: the forming galaxy's stars formed over dT */
    const float sfr_expected =
        (i == p) ? (float)(0.0f + oracle_in_f64(&out, "intermediate.stars") / deltaT) : 0.0f;
    if (oracle_f32_to_bits(galaxies[i].StarFormationRate) != oracle_f32_to_bits(sfr_expected)) {
      fprintf(stderr, "oracle: %s: galaxy[%d].StarFormationRate %.9g, expected %.9g\n", in.path, i,
              (double)galaxies[i].StarFormationRate, (double)sfr_expected);
      problems++;
    }
  }

  /* The branch flags */
  const struct MimicSageFfbFeedbackLocals *fb = &t->feedback;
  const struct MimicSageFireLocals *fire = &fb->fire;
  const int energy_equal = fb->ejection_evaluated ? fire->e_fb == fire->e_lift : -1;
  const struct {
    const char *name;
    int value;
  } flags[] = {{"nonfinite_guard", t->nonfinite_guard},
               {"tdyn_gas_positive", t->tdyn_gas_positive},
               {"strdot_rejected", t->strdot_rejected},
               {"stars_cap_binds", t->stars_cap_binds},
               {"stars_rejected", t->stars_rejected},
               {"atomicH_floored", atomicH_raw < 0.0},
               {"sf_caps_tracked", t->sf_caps.tracks_h2},
               {"sf_h2_cap_binds", t->sf_caps.h2_cap_binds},
               {"sf_h1_cap_binds", t->sf_caps.h1_cap_binds},
               {"fire_scaling_valid", fire->scaling_valid},
               {"fire_vc_floored", fire->vc_floored},
               {"fire_high_velocity", fire->high_velocity},
               {"eta_cap_binds", fire->eta_cap_binds},
               {"renorm_active", fb->renorm_binds},
               {"ejection_evaluated", fb->ejection_evaluated},
               {"coupling_capped", fire->coupling_capped},
               {"energy_ejects", fire->energy_ejects},
               {"energy_equal", energy_equal},
               {"ejected_floored", fb->ejected_floored},
               {"reheat_clamp_ffb_binds", t->reheat_clamp_binds},
               {"uff_reheat_clamp_binds", t->commit.reheat_clamp_binds},
               {"feedback_reservoir_cgm", t->commit.reservoir_cgm},
               {"fb_caps_tracked", t->commit.hydrogen_caps.tracks_h2},
               {"fb_h2_cap_binds", t->commit.hydrogen_caps.h2_cap_binds},
               {"fb_h1_cap_binds", t->commit.hydrogen_caps.h1_cap_binds},
               {"ejected_cap_binds", t->commit.ejected_cap_binds},
               {"metals_cap_binds", t->commit.metals_cap_binds},
               {"yield_coldgas_above_threshold", t->yield.coldgas_above_threshold},
               {"yield_reservoir_cgm", t->yield.reservoir_cgm}};
  for (size_t k = 0; k < COUNT(flags); k++) {
    problems += check_flag(&out, flags[k].name, flags[k].value);
  }
  oracle_exclude(&out, "intermediate.f_ion_path"); /* the shared helper reports no path */

  /* The carried values, in statement order */
  const int valid = fire->scaling_valid == 1;
  const int evaluated = fb->ejection_evaluated == 1;
  const int above = t->yield.coldgas_above_threshold == 1;
  const int formed = t->tdyn_gas_positive == 1;
  const struct {
    const char *name;
    int present;
    double value;
    int kind;
  } values[] = {
      {"reff", 1, t->reff, CHECK_TAIL},
      {"tdyn", 1, t->tdyn, CHECK_TAIL},
      {"atomicH", 1, atomicH, CHECK_TAIL},
      {"f_ion", 1, f_ion, CHECK_TAIL},
      {"strdot", formed, t->strdot, CHECK_TAIL},
      {"stars", 1, t->stars_formed, CHECK_CONTINUATION},
      {"metallicity", 1, t->metallicity, CHECK_CONTINUATION},
      {"fire_scaling", valid, fire->scaling, CHECK_CONTINUATION},
      {"eta_raw", valid, fire->eta_raw, CHECK_CONTINUATION},
      {"eta_reheat", valid, fire->eta, CHECK_TAIL},
      {"reheated_mass_pre_renorm", 1, fb->reheated_pre_renorm, CHECK_TAIL},
      {"renorm_fac", fb->renorm_binds, fb->renorm_fac, CHECK_TAIL},
      {"stars_renorm", 1, fb->stars_out, CHECK_TAIL},
      {"reheated_mass_renorm", 1, fb->reheated_out, CHECK_TAIL},
      {"coupling", evaluated, fire->coupling, CHECK_TAIL},
      {"E_FB", evaluated, fire->e_fb, CHECK_TAIL},
      {"E_lift", evaluated, fire->e_lift, CHECK_TAIL},
      {"ejected_mass", 1, t->ejected_to_commit, CHECK_CONTINUATION},
      /* The wiring assertion on the clamp at S:1346: the reheated mass the module passes into the
       * shared feedback commit, which clamps it again (S:1153-1156), so no end-state comparison
       * can show this clamp ran */
      {"reheated_mass_commit", 1, t->reheated_to_commit, CHECK_CONTINUATION},
      {"uff_reheated_mass_clamped", 1, t->commit.reheated_mass_clamped, CHECK_TAIL},
      {"uff_ejected_mass_capped", 1, t->commit.ejected_mass_capped, CHECK_TAIL},
      {"uff_metallicity_selected", 1, t->commit.metallicity_selected, CHECK_TAIL},
      {"uff_metals_to_eject", 1, t->commit.metals_to_eject, CHECK_TAIL},
      {"uff_metals_to_eject_capped", 1, t->commit.metals_to_eject_capped, CHECK_TAIL},
      {"FracZleaveDiskVal", above, t->yield.frac_z_leave_disk_val, CHECK_TAIL},
      {"yield_metals_to_hot", 1, t->yield.metals_to_hot, CHECK_CONTINUATION},
  };
  for (size_t k = 0; k < COUNT(values); k++) {
    problems +=
        check_value(&out, values[k].name, values[k].present, values[k].value, values[k].kind);
  }
  /* Binding clamps and caps: each value equals its bound */
  if (t->stars_cap_binds == 1) {
    tally.binding++;
    problems += oracle_f64_to_bits(t->stars_formed) != oracle_f64_to_bits((double)input[p].ColdGas);
  }
  if (t->reheat_clamp_binds == 1) {
    /* The clamp's bound is the cold gas the star-formation commit left (S:1346) */
    tally.binding++;
    problems +=
        oracle_f64_to_bits(t->reheated_to_commit) !=
        oracle_f64_to_bits((double)oracle_in_f32(&out, "intermediate.coldgas_after_sf_commit"));
  }
  if (fire->eta_cap_binds == 1) {
    tally.binding++;
    problems += oracle_f64_to_bits(fire->eta) != oracle_f64_to_bits(fire->eta_max);
  }
  if (fire->coupling_capped == 1) {
    tally.binding++;
    problems +=
        oracle_f64_to_bits(fire->coupling) != oracle_f64_to_bits(MIMIC_SAGE_MAX_SN_ENERGY_COUPLING);
  }
  if (t->commit.ejected_cap_binds == 1) {
    tally.binding++;
    problems += oracle_f64_to_bits(t->commit.ejected_mass_capped) !=
                oracle_f64_to_bits((double)t->commit.selected_gas_at_cap);
  }
  if (t->commit.metals_cap_binds == 1) {
    tally.binding++;
    problems += oracle_f64_to_bits(t->commit.metals_to_eject_capped) !=
                oracle_f64_to_bits((double)t->commit.selected_metals_at_cap);
  }

  /* The shared FFB feedback budget, called directly from the oracle's stars formed and cold gas
   * after the star-formation commit */
  {
    const float coldgas_after = oracle_in_f32(&out, "intermediate.coldgas_after_sf_commit");
    double stars = oracle_in_f64(&out, "intermediate.stars");
    double reheated = NAN;
    double ejected = NAN;
    float mass_loading = input[p].MassLoading;
    const double energy_sn_code =
        mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
    const double eta_sn_code =
        mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);
    mimic_sage_ffb_sn_feedback((float)halos[p].Vvir, redshift, coldgas_after,
                               oracle_in_f64(&in, "params.FeedbackReheatingEpsilon"),
                               oracle_in_f64(&in, "params.FeedbackEjectionEfficiency"), eta_sn_code,
                               energy_sn_code, &stars, &reheated, &ejected, &mass_loading, NULL);
    tally.direct_helper_calls++;
    if (oracle_f64_to_bits(stars) != oracle_f64_to_bits(fb->stars_out) ||
        oracle_f64_to_bits(reheated) != oracle_f64_to_bits(fb->reheated_out) ||
        oracle_f64_to_bits(ejected) != oracle_f64_to_bits(t->ejected_to_commit) ||
        oracle_f32_to_bits(mass_loading) != oracle_f32_to_bits(galaxies[p].MassLoading)) {
      fprintf(stderr, "oracle: %s: the FFB feedback helper called directly disagrees\n", in.path);
      problems++;
    }
  }

  /* Observations of the oracle's, not package or SAGE26 state */
  oracle_exclude(&out, "intermediate.metallicity_post_commit");
  oracle_exclude(&out, "intermediate.metallicity_post_commit_differs");
  oracle_exclude(&out, "intermediate.recomputed_metallicity_changes_state");
  oracle_exclude(&out, "intermediate.yield_with_formed_stars_changes_state");
  if (wrapper) {
    tally.wrapper++;
    problems += oracle_check_i32(&out, "intermediate.wrapper_equals_starformation_ffb", 1);
    /* check_disk_instability's criterion (R/src/model_disk_instability.c:54-57) on the end state */
    const struct GalaxyData *g = &galaxies[p];
    const double diskmass = g->ColdGas + (g->StellarMass - g->BulgeMass);
    const double Mcrit = vmax[p] * vmax[p] * (3.0 * g->DiskScaleRadius) / MimicConfig.G;
    problems += oracle_check_i32(&out, "intermediate.disk_unstable_after_call",
                                 diskmass > 0.0 && Mcrit < diskmass);
  }

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  sage26_ffb_star_formation_cleanup();
  tally.fixtures++;
  return problems;
}

/**
 * @test    test_oracle_fixture_count
 * @brief   Every fixture fixtures.manifest lists for the module is present (platform independent)
 */
int test_oracle_fixture_count(void) {
  TEST_ASSERT(oracle_fixture_count_ok(MODULE),
              "fixtures.manifest must list its declared count and every listed fixture must exist");
  return TEST_PASS;
}

/**
 * @test    test_oracle_bit_identity
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit through process() (SKIP outside
 *          the oracle environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  const int ok = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: fixtures %d (wrapper dispatch %d); end-state fields compared %d; carried "
         "values compared bit for bit %d (continuation-checked %d, tail-checked %d; validated "
         "two-sided %d, narrowing-limited %d, not validated %d), absent %d; binding clamps and "
         "caps asserted equal to their bound %d; shared helpers called directly %d ",
         tally.fixtures, tally.wrapper, tally.fields, tally.continuation + tally.tail,
         tally.continuation, tally.tail, tally.validated, tally.narrowing_limited,
         tally.not_validated, tally.absent, tally.binding, tally.direct_helper_calls);
  TEST_ASSERT(ok, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(tally.wrapper >= 1, "the wrapper's FFB dispatch must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_ffb_star_formation SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
