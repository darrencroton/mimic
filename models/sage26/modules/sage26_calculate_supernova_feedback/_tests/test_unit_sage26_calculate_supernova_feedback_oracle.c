/**
 * @file    test_unit_sage26_calculate_supernova_feedback_oracle.c
 * @brief   Bit-exact comparison of the package's disk supernova feedback with SAGE26's
 *          compute_sn_feedback on the FIRE path (the FIRE fixtures of this group, all FIREmodeOn 1;
 *          the FIREmodeOn 0 twins are in sage26_calculate_star_formation's group)
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_calculate_supernova_feedback (its _tests/oracle/ directory), each a call of SAGE26's
 * public starformation_and_feedback (R/src/model_starformation_and_feedback.c:995-1110) with R3's
 * flags (routine starformation_and_feedback_fire, oracle README "The c09 Group"). SAGE26's
 * compute_sn_feedback is static, so the oracle records its values from a checked transcription
 * composed into the wrapper. This test runs the package's disk chain in order, through each
 * module's process():
 *
 *   sage26_calculate_star_formation      sfr_br06, the H I block, stars = strdot * dt
 *   sage26_calculate_supernova_feedback  compute_sn_feedback (the module under test)
 *   sage26_apply_star_formation_supernova update_from_star_formation, the reheating clamp,
 *                                        update_from_feedback (the central's hot gas)
 *   sage26_disk_instability              check_disk_instability (a stable disk: no gas branch)
 *   sage26_apply_metal_enrichment        the disk yield
 *
 * and asserts, bit for bit:
 *
 * - at the feedback module's boundary: the stars it receives (intermediate.stars_pre_renorm), the
 *   budgets it hands on (stars, reheated_mass after the renormalisation and, before it,
 *   reheated_mass_pre_renorm; ejected_mass before and after the floor) and its branch flags
 *   (renorm_active, ejection_vvir_nonpositive, ejected_floored), whatever their validation status,
 *   since the module forms each with SAGE26's expression; MassLoading (SAGE26's own end state); and
 *   the FIRE flags and values, from a replay of the module's FIRE budget through the shared helpers
 *   (shared/sage_fire_feedback_physics.h, sage26_oracle_fire.h) on the state the module received,
 *   which must end in exactly the budgets and MassLoading process() produced;
 * - the end state of every galaxy: the fourteen fields SAGE26 records (OutflowRate being the
 *   package's SupernovaOutflowRate), BlackHoleMass and QuasarModeBHaccretionMass, the four bulge
 *   size members (BulgeSizeOn 3), H2DepletionTime_Gyr, MassLoading and the sixty history elements.
 *
 * The oracle's other carried values (the apply step's, the instability's and the yield's locals and
 * the H2 values of the oracle README's "The c08 Group") are consumed without assertion: the chained
 * oracle test of sage26_calculate_star_formation asserts them on its fixtures, and here the end
 * state, which they determine, is asserted in full. call.time is read by no module of this chain.
 *
 * The core-owned inputs are supplied as SAGE26 forms them: Vvir and Mvir widened from SAGE26's
 * floats, each galaxy's stored SnapNum and the run's redshift table (ZZ of each stored snapshot,
 * every other entry NaN), the row's dT = call.deltaT and num_substeps = effective_steps, G,
 * Hubble_h and SAGE26's unit factors. Outside the oracle environment (macOS arm64, Apple clang 21)
 * the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
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
#include "modules/_tests/sage26_oracle_fire.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_h2.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_h2_physics.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"
#include "shared/time_parity.h"

extern int sage26_calculate_star_formation_init(void);
extern int sage26_calculate_star_formation_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_calculate_supernova_feedback_init(void);
extern int sage26_calculate_supernova_feedback_process(struct ModuleContext *ctx,
                                                       struct Halo *halos, int ngal);
extern int sage26_apply_star_formation_supernova_init(void);
extern int sage26_apply_star_formation_supernova_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_disk_instability_init(void);
extern int sage26_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_quasar_mode_init(void);
extern int sage26_starburst_feedback_init(void);
extern int sage26_apply_metal_enrichment_init(void);
extern int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);

static const char *MODULE = "sage26_calculate_supernova_feedback";

#define MAX_GALAXIES 4
#define MAX_SUBSTEPS 64

/* SAGE26's SF_DISK_RADIUS_FRAC (R/src/model_starformation_and_feedback.c:60), also
 * the instability's TOOMRE_DISK_FACTOR (R/src/model_disk_instability.c:35), and its
 * unit factors (input/millennium.par:106-107) */
#define SAGE26_SF_DISK_RADIUS_FRAC 3.0
#define SAGE26_UNIT_LENGTH_IN_CM 3.08568e+24
#define SAGE26_UNIT_MASS_IN_G 1.989e+43

/* The chain's modules, in substep order (static storage: no memory system needed); the quasar and
 * burst modules are configured for the instability's dependency check, never run (a stable disk) */
#define CHAIN_MODULES 7
static char chain_names[CHAIN_MODULES][64] = {"sage26_calculate_star_formation",
                                              "sage26_calculate_supernova_feedback",
                                              "sage26_apply_star_formation_supernova",
                                              "sage26_disk_instability",
                                              "sage26_quasar_mode",
                                              "sage26_starburst_feedback",
                                              "sage26_apply_metal_enrichment"};
static char chain_phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig chain_modules[CHAIN_MODULES];
static struct ModulePhaseConfig chain_phase;

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static struct oracle_c09_tally fire_tally;

/** @brief Counters of the feedback boundary's carried values, by validation status */
static struct {
  int validated;
  int not_validated;
  int intermediates_consumed; /* other oracle values consumed without assertion */
} boundary_tally;

/** @brief The galaxy fields the chain must leave as they were, beyond the fourteen */
static const char *const UNTOUCHED[] = {"BlackHoleMass", "QuasarModeBHaccretionMass"};

/** @brief The bulge-size members a BulgeSizeOn 3 fixture records */
static const char *const SIZES[] = {"MergerBulgeMass", "BulgeRadius", "MergerBulgeRadius",
                                    "InstabilityBulgeRadius"};

/** @brief The state of one fixture's chain */
struct chain {
  int ngal;
  int p;
  int centralgal;
  int effective_steps;
  int disk_instability_on;
  int bulge_size_on;
  int fire_mode_on;
  int snapnum[MAX_GALAXIES];
  struct oracle_c08_sf_config sf;
  struct GalaxyData galaxies[MAX_GALAXIES];
  struct Halo halos[MAX_GALAXIES];
  struct ModuleContext ctx;
};

/**
 * @brief Apply the fixture's flags and parameters, then initialise the chain's modules
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in, struct chain *c) {
  int problems = 0;
  reset_config();

  /* R3's flags (oracle README "The c09 Group"): SFprescription 1 with R2's H2 configuration,
   * FIREmodeOn 0 or 1 with each galaxy's stored snapshot and its redshift */
  problems += oracle_c08_read_sf_config(in, 0, &c->sf);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  c->ngal = oracle_in_i32(in, "call.ngal");
  if (c->ngal < 1 || c->ngal > MAX_GALAXIES) {
    fprintf(stderr, "oracle: %s: call.ngal out of range\n", in->path);
    return problems + 1;
  }
  problems += oracle_c09_read_fire_config(in, c->ngal, c->snapnum, &c->fire_mode_on);
  problems += oracle_c05_require_flag(in, "params.EnhancedStarFormationOn", 0);
  c->bulge_size_on = oracle_in_i32(in, "params.BulgeSizeOn");
  if (c->bulge_size_on != MIMIC_SAGE_BULGE_SIZE_OFF &&
      c->bulge_size_on != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    fprintf(stderr, "oracle: %s: BulgeSizeOn must be 0 or 3\n", in->path);
    problems++;
  }
  problems += oracle_c05_require_flag(in, "params.KarpovModeOn", 0);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  c->disk_instability_on = oracle_in_i32(in, "params.DiskInstabilityOn");
  if (c->disk_instability_on != 0 && c->disk_instability_on != 1) {
    fprintf(stderr, "oracle: %s: DiskInstabilityOn must be 0 or 1\n", in->path);
    problems++;
  }

  /* Core-owned constants and SAGE26's unit factors (R/src/core_init.c:94-103) */
  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  MimicConfig.UnitLength_in_cm = SAGE26_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = SAGE26_UNIT_MASS_IN_G;
  MimicConfig.UnitVelocity_in_cm_per_s = oracle_in_f64(in, "params.UnitVelocity_in_cm_per_s");
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g *
                                  (MimicConfig.UnitLength_in_cm * MimicConfig.UnitLength_in_cm) /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);

  /* The feedback module derives EtaSNcode and EnergySNcode through these helpers */
  if (oracle_f64_to_bits(
          mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h)) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EnergySNcode")) ||
      oracle_f64_to_bits(mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h)) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EtaSNcode"))) {
    fprintf(stderr, "oracle: %s: the package's EnergySNcode or EtaSNcode differs from SAGE26's\n",
            in->path);
    problems++;
  }

  const char *const params[] = {"SfrEfficiency",
                                "FeedbackReheatingEpsilon",
                                "FeedbackEjectionEfficiency",
                                "RecycleFraction",
                                "Yield",
                                "FracZleaveDisk"};
  for (size_t k = 0; k < COUNT(params); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", params[k]);
    problems += oracle_set_model_param(params[k], oracle_in_f64(in, field)) != 0;
  }
  problems += oracle_set_model_param("StarFormingDiskFactor", SAGE26_SF_DISK_RADIUS_FRAC) != 0;
  problems += oracle_set_model_param("BulgeSizeOn", (double)c->bulge_size_on) != 0;
  problems += oracle_c08_set_sf_parameters(&c->sf);
  /* Read only by the instability's continuation, which a stable disk never reaches */
  if (oracle_set_model_param("ThresholdMajorMerger",
                             oracle_in_f64(in, "params.ThreshMajorMerger")) != 0 ||
      oracle_set_model_param("BlackHoleGrowthRate",
                             oracle_in_f64(in, "params.BlackHoleGrowthRate")) != 0 ||
      oracle_set_model_param("QuasarModeEfficiency",
                             oracle_in_f64(in, "params.QuasarModeEfficiency")) != 0 ||
      oracle_set_model_param("StarburstColdGasOn", 1.0) != 0 ||
      /* CGMrecipeOn: required by the commits', yields', wind's and disruption's modules */
      oracle_set_model_param("CGMrecipeOn", 0.0) != 0 ||
      /* AGNrecipe: required by the wind's module, the fixture's AGNrecipeOn (checked above) */
      oracle_set_model_param("AGNrecipe", (double)oracle_in_i32(in, "params.AGNrecipeOn")) != 0) {
    problems++;
  }

  for (int m = 0; m < CHAIN_MODULES; m++) {
    chain_modules[m].module_name = chain_names[m];
    chain_modules[m].processing_mode = PROCESSING_MODE_BY_GALAXY;
  }
  chain_phase.name = chain_phase_name;
  chain_phase.modules = chain_modules;
  chain_phase.num_modules = CHAIN_MODULES;
  MimicConfig.substep_phases = &chain_phase;
  MimicConfig.num_substep_phases = 1;

  if (sage26_calculate_star_formation_init() != 0 ||
      sage26_calculate_supernova_feedback_init() != 0 ||
      sage26_apply_star_formation_supernova_init() != 0 || sage26_disk_instability_init() != 0 ||
      sage26_quasar_mode_init() != 0 || sage26_starburst_feedback_init() != 0 ||
      sage26_apply_metal_enrichment_init() != 0) {
    fprintf(stderr, "oracle: %s: a module init() failed\n", in->path);
    problems++;
  }
  return problems;
}

/**
 * @brief Build the galaxies, halos and context from the fixture
 * @return Number of problems
 */
static int setup_galaxies(struct oracle_fixture *in, struct chain *c) {
  int problems = 0;
  c->p = oracle_in_i32(in, "call.p");
  c->centralgal = oracle_in_i32(in, "call.centralgal");
  c->effective_steps = oracle_in_i32(in, "call.effective_steps");
  if (c->p < 0 || c->p >= c->ngal || c->centralgal < 0 || c->centralgal >= c->ngal ||
      c->effective_steps < 1 || c->effective_steps > MAX_SUBSTEPS) {
    fprintf(stderr, "oracle: %s: call indices or effective_steps out of range\n", in->path);
    return problems + 1;
  }
  const double deltaT = oracle_in_f64(in, "call.deltaT");

  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < c->ngal; i++) {
    struct GalaxyData *g = &c->galaxies[i];
    struct Halo *h = &c->halos[i];
    oracle_poison(g, sizeof(*g));
    oracle_poison(h, sizeof(*h));
    h->galaxy = g;
    h->HaloNr = i;
    h->SnapNum = c->snapnum[i]; /* the stored snapshot, the FIRE scaling's redshift index */
    h->dT = deltaT;
    h->Type = oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    h->Mvir = oracle_in_f32(in, oracle_galaxy_field(i, "Mvir"));
    h->Vvir = oracle_in_f32(in, oracle_galaxy_field(i, "Vvir"));
    h->Vmax = oracle_in_f32(in, oracle_galaxy_field(i, "Vmax"));
    const char *const extra[] = {"DiskScaleRadius", "BlackHoleMass", "QuasarModeBHaccretionMass"};
    problems += oracle_c05_in_floats(in, i, g, extra, COUNT(extra));
    problems += oracle_c05_in_floats(in, i, g, written, count);
    if (c->bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        *oracle_c06_galaxy_float(g, SIZES[k]) = oracle_in_f32(in, oracle_galaxy_field(i, SIZES[k]));
      }
    }
    if (c->sf.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
      g->H2DepletionTime_Gyr = oracle_in_f32(in, oracle_galaxy_field(i, "H2DepletionTime_Gyr"));
    }
    g->MassLoading = oracle_in_f32(in, oracle_galaxy_field(i, "MassLoading"));
    problems += oracle_c05_in_histories(in, i, g);

    g->NewStellarMass = 0.0;
    g->SupernovaReheatedMass = 0.0;
    g->SupernovaEjectedMass = 0.0;
    g->UnstableDiskGasFraction = 0.0;
    g->StarFormationRate = 0.0f;
  }

  memset(&c->ctx, 0, sizeof(c->ctx));
  c->ctx.params = &MimicConfig;
  c->ctx.central_galaxy = &c->halos[c->centralgal];
  c->ctx.num_substeps = c->effective_steps;
  return problems;
}

/**
 * @brief Compare one carried value of the feedback boundary bit for bit, whatever its validation
 *        status (the module forms it with SAGE26's expression), consuming its markers
 * @return Number of mismatches and marker errors
 */
static int check_boundary_value(struct oracle_fixture *out, const char *name, const double actual) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != (isnan(actual) ? 1 : 0)) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", isnan(actual) ? "absent" : "present");
    return 1;
  }
  if (absent) {
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  problems += oracle_check_i32(out, field, 1);
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s_validated disagrees with its two directions\n", out->path,
            name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  problems += oracle_check_f64(out, field, actual);
  if (validated) {
    boundary_tally.validated++;
  } else {
    boundary_tally.not_validated++;
  }
  return problems;
}

/** @brief Consume every oracle intermediate this test does not assert (stated in the file comment)
 */
static void consume_other_intermediates(struct oracle_fixture *out) {
  for (int i = 0; i < out->num_fields; i++) {
    if (!out->fields[i].consumed && strncmp(out->fields[i].name, "intermediate.", 13) == 0) {
      out->fields[i].consumed = 1;
      boundary_tally.intermediates_consumed++;
    }
  }
}

/** @brief Compare the end state of every galaxy */
static int check_end_state(struct oracle_fixture *out, struct chain *c) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < c->ngal; i++) {
    problems += oracle_c05_check_floats(out, "", i, &c->galaxies[i], written, count);
    problems += oracle_c05_check_floats(out, "", i, &c->galaxies[i], UNTOUCHED, COUNT(UNTOUCHED));
    if (c->bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        problems += oracle_check_f32(out, oracle_galaxy_field(i, SIZES[k]),
                                     *oracle_c06_galaxy_float(&c->galaxies[i], SIZES[k]));
      }
    }
    if (c->sf.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
      problems += oracle_check_f32(out, oracle_galaxy_field(i, "H2DepletionTime_Gyr"),
                                   c->galaxies[i].H2DepletionTime_Gyr);
    }
    problems +=
        oracle_check_f32(out, oracle_galaxy_field(i, "MassLoading"), c->galaxies[i].MassLoading);
    problems += oracle_c05_check_histories(out, "", i, &c->galaxies[i]);
  }
  return problems;
}

/** @brief One fixture: the chain on galaxy p for substep call.step */
static int run_fixture(struct oracle_fixture *in, struct oracle_fixture *out, struct chain *c) {
  int problems = 0;
  const int step = oracle_in_i32(in, "call.step");
  (void)oracle_in_f64(in, "call.time"); /* read by no module of this chain */
  if (step < 0 || step >= c->effective_steps) {
    fprintf(stderr, "oracle: %s: call.step out of range\n", in->path);
    return problems + 1;
  }
  c->ctx.substep_number = step;
  struct Halo *halo = &c->halos[c->p];
  struct GalaxyData *gal = &c->galaxies[c->p];

  double dt = 0.0;
  if (mimic_object_substep_dt(halo, &c->ctx, &dt) != MIMIC_OBJECT_TIME_OK ||
      oracle_f64_to_bits(dt) != oracle_f64_to_bits(oracle_in_f64(in, "call.dt"))) {
    fprintf(stderr, "oracle: %s: the substep dt differs from call.dt\n", in->path);
    problems++;
  }

  if (sage26_calculate_star_formation_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }

  /* The feedback boundary: what the module receives, its FIRE replay, then process() */
  const double stars_in = gal->NewStellarMass;
  const float vvir = (float)halo->Vvir;
  struct oracle_c09_sn_replay r;
  mimic_sage_fire_locals_init(&r.l);
  if (c->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    oracle_c09_replay_sn(stars_in, gal->ColdGas, vvir, MimicConfig.ZZ[halo->SnapNum],
                         oracle_in_f64(in, "params.FeedbackReheatingEpsilon"),
                         oracle_in_f64(in, "params.FeedbackEjectionEfficiency"),
                         oracle_in_f64(in, "params.EtaSNcode"),
                         oracle_in_f64(in, "params.EnergySNcode"), &r);
  }
  const float mass_loading_before = gal->MassLoading;
  if (sage26_calculate_supernova_feedback_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  if (c->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    if (oracle_f64_to_bits(r.stars) != oracle_f64_to_bits(gal->NewStellarMass) ||
        oracle_f64_to_bits(r.reheated) != oracle_f64_to_bits(gal->SupernovaReheatedMass) ||
        oracle_f64_to_bits(r.ejected) != oracle_f64_to_bits(gal->SupernovaEjectedMass) ||
        oracle_f32_to_bits(r.mass_loading) != oracle_f32_to_bits(gal->MassLoading)) {
      fprintf(stderr, "oracle: %s: the FIRE replay differs from the feedback module\n", in->path);
      problems++;
    }
  } else if (oracle_f32_to_bits(mass_loading_before) != oracle_f32_to_bits(gal->MassLoading)) {
    fprintf(stderr, "oracle: %s: FIREmodeOn 0 wrote MassLoading\n", in->path);
    problems++;
  }

  problems += check_boundary_value(out, "stars_pre_renorm", stars_in);
  problems += check_boundary_value(out, "stars", gal->NewStellarMass);
  problems += check_boundary_value(out, "reheated_mass", gal->SupernovaReheatedMass);
  problems += check_boundary_value(out, "ejected_mass", gal->SupernovaEjectedMass);
  if (c->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    /* The FIRE ejection is never negative, so the floor leaves it unchanged (S:880-882) */
    problems += check_boundary_value(out, "reheated_mass_pre_renorm", r.reheated_pre);
    problems += check_boundary_value(out, "ejected_mass_pre_floor", vvir > 0.0 ? r.ejected : NAN);
    problems += oracle_c06_check_flag(out, "renorm_active", r.renorm_active);
  }
  problems += oracle_c06_check_flag(out, "ejection_vvir_nonpositive", vvir > 0.0 ? 0 : 1);
  /* The FIRE ejection is +0.0 or a positive quotient, so the floor never binds on that path
   * (S:792, S:880-882) and the flag is asserted 0; with FIREmodeOn 0 a floored ejection is
   * legitimate and the flag is consumed without an assertion (below), the floored budget itself
   * asserted by the feedback boundary above */
  if (c->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    problems += oracle_c06_check_flag(out, "ejected_floored", 0);
  }
  problems +=
      oracle_c09_check_fire_record(out, c->fire_mode_on, &r.l, stars_in == 0.0, &fire_tally);

  /* The rest of the chain */
  if (sage26_apply_star_formation_supernova_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  if (c->disk_instability_on) {
    if (sage26_disk_instability_process(&c->ctx, halo, 1) != 0) {
      problems++;
    }
    if (gal->UnstableDiskGasFraction != 0.0) {
      fprintf(stderr, "oracle: %s: the instability handed on an unstable gas fraction\n", in->path);
      problems++;
    }
  }
  if (sage26_apply_metal_enrichment_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  problems += check_end_state(out, c);
  problems += oracle_check_i32(out, "intermediate.step_bin",
                               mimic_sage_sfr_step_bin(step, c->effective_steps));
  consume_other_intermediates(out);
  return problems;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, ORACLE_C09_ROUTINE, &in, &out) != 0) {
    return 1;
  }
  static struct chain c;
  int problems = setup_parameters(&in, &c);
  problems += setup_galaxies(&in, &c);
  if (problems != 0) {
    return problems;
  }
  problems += run_fixture(&in, &out, &c);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
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
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside the oracle
 *          environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&fire_tally, 0, sizeof(fire_tally));
  memset(&boundary_tally, 0, sizeof(boundary_tally));
  const int ok = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: c09 fixtures FIREmodeOn 1 %d, FIREmodeOn 0 %d; FIRE values compared bit for "
         "bit %d (continuation-checked %d, tail-checked %d; validated two-sided %d, "
         "narrowing-limited %d, not validated %d), absent %d; binding caps asserted %d ",
         fire_tally.fire_on, fire_tally.fire_off,
         fire_tally.validated + fire_tally.narrowing_limited + fire_tally.not_validated,
         fire_tally.continuation, fire_tally.tail, fire_tally.validated,
         fire_tally.narrowing_limited, fire_tally.not_validated, fire_tally.absent,
         fire_tally.binding);
  printf("\n  oracle: feedback boundary values compared bit for bit %d (validated two-sided %d, "
         "not validated %d); other oracle intermediates consumed unasserted %d ",
         boundary_tally.validated + boundary_tally.not_validated, boundary_tally.validated,
         boundary_tally.not_validated, boundary_tally.intermediates_consumed);
  TEST_ASSERT(ok, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(fire_tally.fire_on >= 1, "a FIREmodeOn 1 fixture must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_calculate_supernova_feedback SAGE26 oracle (bit-exact, FIRE)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
