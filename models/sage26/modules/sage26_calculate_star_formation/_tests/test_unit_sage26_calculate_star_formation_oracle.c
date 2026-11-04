/**
 * @file    test_unit_sage26_calculate_star_formation_oracle.c
 * @brief   Bit-exact comparison of the package's disk SF chain with SAGE26's
 * starformation_and_feedback
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_calculate_star_formation (its _tests/oracle/ directory): SAGE26's
 * starformation_and_feedback (R/src/model_starformation_and_feedback.c:995-1110) on
 * one galaxy, once (routine starformation_and_feedback) or for every substep of a
 * snapshot (routine starformation_and_feedback_sequence). The package splits that
 * routine over a chain of modules, so this chained test runs the chain in order,
 * through each module's process(), on every substep:
 *
 *   sage26_calculate_star_formation      sfr_croton06, the H I block, stars = strdot * dt
 *   sage26_calculate_supernova_feedback  compute_sn_feedback (renormalisation, ejection)
 *   sage26_apply_star_formation_supernova update_from_star_formation, the reheating clamp,
 *                                        update_from_feedback
 *   sage26_disk_instability              check_disk_instability (DiskInstabilityOn 1 only)
 *   sage26_apply_metal_enrichment        the disk yield
 *
 * It asserts bit for bit, on every galaxy of the fixture, the fourteen fields the
 * routine records (OutflowRate being the package's SupernovaOutflowRate) and
 * BlackHoleMass and QuasarModeBHaccretionMass, which the chain must leave as they
 * were. It asserts the values the package carries between modules (NewStellarMass
 * after star formation, NewStellarMass, SupernovaReheatedMass and SupernovaEjectedMass
 * after the feedback budget) wherever the oracle validates them two-sided. The apply
 * step's locals are not package state, so the test also replays that step through the
 * same shared commits (shared/sage_sf_feedback_physics.h) on a copy of the galaxies,
 * requires the replay to end in exactly the state process() produced, and asserts
 * the replay's metallicities, clamped reheated mass and update_from_feedback locals
 * where validated. It likewise replays the instability through its shared helper
 * (shared/sage_disk_instability_physics.h), requires the same end state as process(),
 * and asserts SAGE26's instability locals where validated. Where a clamp or cap binds
 * (the *_binds flags, instability_mcrit_capped) the recorded value must equal the
 * reservoir it binds to, which the replays supply.
 *
 * Scope of this slice (the oracle's records the package cannot yet meet are named
 * and not asserted):
 *
 * - The six per-substep histories, the step bin, SubstepsUsed and the writer's
 *   averages belong to the history substrate, which the package does not yet have.
 * - The instability (DiskInstabilityOn 1) runs sage26_disk_instability, SAGE26's
 *   check_disk_instability up to its black-hole and burst calls. The oracle admits a
 *   wrapper fixture only where that routine does not reach them: a stable disk, or an
 *   unstable disk with no cold gas after feedback, whose stellar transfer into
 *   BulgeMass, MetalsBulgeMass and InstabilityBulgeMass
 *   (R/src/model_disk_instability.c:77-79) is asserted here like every other field
 *   (instability_unstable_no_coldgas). The chain requires the module to hand on no
 *   unstable gas fraction. DiskInstabilityOn 0 fixtures (a labelled non-R0 value) do
 *   not run it.
 * - The SAGE26 locals the package never forms as named values (tdyn, cold_crit,
 *   strdot, atomicH, f_ion, the pre-renormalisation reheated mass, renorm_fac, the
 *   pre-floor ejected mass, FracZleaveDiskVal and the yield's escaping metals) reach
 *   the asserted fields and are not asserted themselves. The branch flags are read
 *   only to key the binding equalities.
 * - call.time (the substep midpoint) is read by no module of this chain on R0; the
 *   instability's c06 continuation, which reads it, is not reached. The parameters
 *   only that continuation reads (ThreshMajorMerger, BlackHoleGrowthRate,
 *   QuasarModeEfficiency, UnitVelocity_in_cm_per_s) are likewise not applied.
 *
 * The core-owned inputs are supplied as SAGE26 forms them: Vvir and Mvir widened
 * from SAGE26's floats, the row's dT = call.deltaT and num_substeps = effective_steps
 * (the module's dt is asserted equal to call.dt), G and Hubble_h, and SAGE26's R0 unit
 * factors, from which the feedback module derives EtaSNcode and EnergySNcode (both
 * asserted against the fixture through the shared helpers). StarFormingDiskFactor is
 * SAGE26's SF_DISK_RADIUS_FRAC, 3.0 (R/src/model_starformation_and_feedback.c:60), and
 * the same parameter is the instability's TOOMRE_DISK_FACTOR, also 3.0
 * (R/src/model_disk_instability.c:35).
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test
 * reports SKIP.
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
#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/metallicity.h"
#include "shared/sage_disk_instability_physics.h"
#include "shared/sage_sf_feedback_physics.h"
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
extern int sage26_apply_metal_enrichment_init(void);
extern int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);

static const char *MODULE = "sage26_calculate_star_formation";

#define MAX_GALAXIES 4
#define MAX_SUBSTEPS 64

/* SAGE26's SF_DISK_RADIUS_FRAC (R/src/model_starformation_and_feedback.c:60) and its
 * R0 unit factors (R/input/millennium.par:106-108) */
#define SAGE26_SF_DISK_RADIUS_FRAC 3.0
/* SAGE26's TOOMRE_DISK_FACTOR (R/src/model_disk_instability.c:35), the same value */
#define SAGE26_TOOMRE_DISK_FACTOR 3.0
#define SAGE26_UNIT_LENGTH_IN_CM 3.08568e+24
#define SAGE26_UNIT_MASS_IN_G 1.989e+43

/* The chain's modules, in substep order (static storage: no memory system needed) */
static char chain_names[5][64] = {"sage26_calculate_star_formation",
                                  "sage26_calculate_supernova_feedback",
                                  "sage26_apply_star_formation_supernova",
                                  "sage26_disk_instability", "sage26_apply_metal_enrichment"};
static char chain_phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig chain_modules[5];
static struct ModulePhaseConfig chain_phase;

static struct oracle_c05_carried_tally tally;

/** @brief Names of the branch flags every single-call output records */
static const char *const FLAGS[] = {"sf_vvir_nonpositive",
                                    "sf_above_critical",
                                    "stars_floored",
                                    "atomicH_floored",
                                    "f_ion_path",
                                    "renorm_active",
                                    "ejection_vvir_nonpositive",
                                    "ejected_floored",
                                    "instability_disk_positive",
                                    "instability_mcrit_capped",
                                    "instability_stars_transferred",
                                    "instability_gas_branch",
                                    "yield_coldgas_above_threshold"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief The galaxy fields the chain must leave as they were, beyond the fourteen */
static const char *const UNTOUCHED[] = {"BlackHoleMass", "QuasarModeBHaccretionMass"};

/** @brief The state of one fixture's chain */
struct chain {
  int ngal;
  int p;
  int centralgal;
  int effective_steps;
  int disk_instability_on;
  struct GalaxyData galaxies[MAX_GALAXIES];
  struct Halo halos[MAX_GALAXIES];
  struct ModuleContext ctx;
};

/** @brief What one substep of the chain carried and what the apply-step replay formed */
struct substep_record {
  double stars_pre_renorm;
  double stars;
  double reheated_mass;
  double ejected_mass;
  double metallicity_sf;
  double metallicity_fb;
  double reheated_mass_sf_clamped;
  float cold_gas_after_sf;
  struct MimicSageFeedbackLocals locals;
  struct MimicSageInstabilityLocals instability; /* NaN where not formed or not run */
};

/**
 * @brief Apply the fixture's flags and parameters, then initialise the chain's modules
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in, struct chain *c) {
  int problems = 0;
  reset_config();

  /* R0's fixed flags (oracle README "Flags and Parameters") */
  problems += oracle_c05_require_flag(in, "params.SFprescription", 0);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  problems += oracle_c05_require_flag(in, "params.FIREmodeOn", 0);
  problems += oracle_c05_require_flag(in, "params.EnhancedStarFormationOn", 0);
  problems += oracle_c05_require_flag(in, "params.BulgeSizeOn", 0);
  problems += oracle_c05_require_flag(in, "params.KarpovModeOn", 0);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  c->disk_instability_on = oracle_in_i32(in, "params.DiskInstabilityOn");
  if (c->disk_instability_on != 0 && c->disk_instability_on != 1) {
    fprintf(stderr, "oracle: %s: DiskInstabilityOn must be 0 or 1\n", in->path);
    problems++;
  }

  /* Core-owned constants and SAGE26's R0 unit factors (R/src/core_init.c:94-103) */
  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  MimicConfig.UnitLength_in_cm = SAGE26_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = SAGE26_UNIT_MASS_IN_G;
  /* Read only by the instability's c06 continuation (quasar wind), not reached here */
  MimicConfig.UnitVelocity_in_cm_per_s = oracle_in_f64(in, "params.UnitVelocity_in_cm_per_s");
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g *
                                  (MimicConfig.UnitLength_in_cm * MimicConfig.UnitLength_in_cm) /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);

  /* The feedback module derives EtaSNcode and EnergySNcode through these helpers */
  const double energy_sn_code =
      mimic_sage_energy_sn_code(MimicConfig.UnitEnergy_in_cgs, MimicConfig.Hubble_h);
  const double eta_sn_code =
      mimic_sage_eta_sn_code(MimicConfig.UnitMass_in_g, MimicConfig.Hubble_h);
  if (oracle_f64_to_bits(energy_sn_code) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EnergySNcode")) ||
      oracle_f64_to_bits(eta_sn_code) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.EtaSNcode"))) {
    fprintf(stderr, "oracle: %s: the package's EnergySNcode or EtaSNcode differs from SAGE26's\n",
            in->path);
    problems++;
  }

  /* The chain's parameters, at the fixture's bit patterns */
  const char *const params[] = {"SfrEfficiency",
                                "FeedbackReheatingEpsilon",
                                "FeedbackEjectionEfficiency",
                                "RecycleFraction",
                                "Yield",
                                "FracZleaveDisk"};
  for (size_t k = 0; k < COUNT(params); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", params[k]);
    if (oracle_set_model_param(params[k], oracle_in_f64(in, field)) != 0) {
      problems++;
    }
  }
  if (oracle_set_model_param("StarFormingDiskFactor", SAGE26_SF_DISK_RADIUS_FRAC) != 0) {
    problems++;
  }
  /* Read only by the instability's c06 continuation (black-hole growth, quasar wind,
   * burst), which no fixture of this chain reaches */
  (void)oracle_in_f64(in, "params.ThreshMajorMerger");
  (void)oracle_in_f64(in, "params.BlackHoleGrowthRate");
  (void)oracle_in_f64(in, "params.QuasarModeEfficiency");

  /* The substep phase the modules' init() dependency checks inspect */
  for (int m = 0; m < 5; m++) {
    chain_modules[m].module_name = chain_names[m];
    chain_modules[m].processing_mode = PROCESSING_MODE_BY_GALAXY;
  }
  chain_phase.name = chain_phase_name;
  chain_phase.modules = chain_modules;
  chain_phase.num_modules = 5;
  MimicConfig.substep_phases = &chain_phase;
  MimicConfig.num_substep_phases = 1;

  if (sage26_calculate_star_formation_init() != 0 ||
      sage26_calculate_supernova_feedback_init() != 0 ||
      sage26_apply_star_formation_supernova_init() != 0 || sage26_disk_instability_init() != 0 ||
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
  c->ngal = oracle_in_i32(in, "call.ngal");
  c->p = oracle_in_i32(in, "call.p");
  c->centralgal = oracle_in_i32(in, "call.centralgal");
  c->effective_steps = oracle_in_i32(in, "call.effective_steps");
  if (c->ngal < 1 || c->ngal > MAX_GALAXIES || c->p < 0 || c->p >= c->ngal || c->centralgal < 0 ||
      c->centralgal >= c->ngal || c->effective_steps < 1 || c->effective_steps > MAX_SUBSTEPS) {
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
    h->SnapNum = 0; /* read by no module of the chain once dT is positive */
    h->dT = deltaT; /* core-owned: SAGE26's deltaT of the forming galaxy's group */
    h->Type = oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    /* SAGE26's float halo values, exactly widened to the core's doubles */
    h->Mvir = oracle_in_f32(in, oracle_galaxy_field(i, "Mvir"));
    h->Vvir = oracle_in_f32(in, oracle_galaxy_field(i, "Vvir"));
    h->Vmax = oracle_in_f32(in, oracle_galaxy_field(i, "Vmax"));
    const char *const extra[] = {"DiskScaleRadius", "BlackHoleMass", "QuasarModeBHaccretionMass"};
    problems += oracle_c05_in_floats(in, i, g, extra, COUNT(extra));
    problems += oracle_c05_in_floats(in, i, g, written, count);
    /* The six histories: not package state before the history substrate is ported */
    oracle_c05_exclude_histories(in, "", i);

    /* Transport and diagnostic state the chain writes before it reads */
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

/** @brief Compare the fourteen fields and the two untouched ones of every galaxy */
static int check_end_state(struct oracle_fixture *out, struct chain *c) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < c->ngal; i++) {
    problems += oracle_c05_check_floats(out, "", i, &c->galaxies[i], written, count);
    problems += oracle_c05_check_floats(out, "", i, &c->galaxies[i], UNTOUCHED, COUNT(UNTOUCHED));
  }
  return problems;
}

/**
 * @brief Run one substep of the chain on galaxy p, recording what it carried
 * @return Number of problems
 */
static int run_substep(struct oracle_fixture *in, struct chain *c, const int step,
                       struct substep_record *r) {
  int problems = 0;
  c->ctx.substep_number = step;
  struct Halo *halo = &c->halos[c->p];
  struct GalaxyData *gal = &c->galaxies[c->p];

  /* The core-owned substep dt must be SAGE26's deltaT / effective_steps */
  double dt = 0.0;
  if (mimic_object_substep_dt(halo, &c->ctx, &dt) != MIMIC_OBJECT_TIME_OK ||
      oracle_f64_to_bits(dt) != oracle_f64_to_bits(oracle_in_f64(in, "call.dt"))) {
    fprintf(stderr, "oracle: %s: the substep dt differs from call.dt\n", in->path);
    problems++;
  }

  if (sage26_calculate_star_formation_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  r->stars_pre_renorm = gal->NewStellarMass;

  if (sage26_calculate_supernova_feedback_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  r->stars = gal->NewStellarMass;
  r->reheated_mass = gal->SupernovaReheatedMass;
  r->ejected_mass = gal->SupernovaEjectedMass;

  /* Replay the apply step through the shared commits on a copy, for its locals */
  struct GalaxyData replay[MAX_GALAXIES];
  memcpy(replay, c->galaxies, sizeof(replay));
  struct GalaxyData *rp = &replay[c->p];
  r->metallicity_sf = mimic_get_metallicity(rp->ColdGas, rp->MetalsColdGas);
  mimic_sage_update_from_star_formation(rp, r->stars, r->metallicity_sf,
                                        oracle_in_f64(in, "params.RecycleFraction"));
  r->cold_gas_after_sf = rp->ColdGas;
  r->metallicity_fb = mimic_get_metallicity(rp->ColdGas, rp->MetalsColdGas);
  r->reheated_mass_sf_clamped =
      r->reheated_mass > rp->ColdGas ? (double)rp->ColdGas : r->reheated_mass;
  mimic_sage_update_from_feedback(rp, &replay[c->centralgal], r->reheated_mass_sf_clamped,
                                  r->ejected_mass, r->metallicity_fb, &r->locals);

  if (sage26_apply_star_formation_supernova_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < c->ngal; i++) {
    for (size_t k = 0; k < count; k++) {
      if (oracle_f32_to_bits(*oracle_c05_galaxy_float(&replay[i], written[k])) !=
          oracle_f32_to_bits(*oracle_c05_galaxy_float(&c->galaxies[i], written[k]))) {
        fprintf(stderr,
                "oracle: %s: the apply-step replay differs from process() in galaxy[%d].%s\n",
                in->path, i, written[k]);
        problems++;
      }
    }
  }

  r->instability = (struct MimicSageInstabilityLocals){NAN, NAN, NAN, NAN, NAN, NAN};
  if (c->disk_instability_on) {
    /* Replay the instability through its shared helper on a copy, for its locals */
    struct GalaxyData instability_replay[MAX_GALAXIES];
    memcpy(instability_replay, c->galaxies, sizeof(instability_replay));
    struct Halo replay_halo = *halo;
    replay_halo.galaxy = &instability_replay[c->p];
    instability_replay[c->p].UnstableDiskGasFraction = mimic_sage_check_disk_instability(
        &replay_halo, &c->ctx, SAGE26_TOOMRE_DISK_FACTOR, &r->instability);

    if (sage26_disk_instability_process(&c->ctx, halo, 1) != 0) {
      problems++;
    }
    if (memcmp(instability_replay, c->galaxies, sizeof(struct GalaxyData) * (size_t)c->ngal) != 0) {
      fprintf(stderr, "oracle: %s: the instability replay differs from process()\n", in->path);
      problems++;
    }
    /* The oracle admits no wrapper fixture whose instability reaches the gas branch:
     * no unstable gas is handed on */
    if (gal->UnstableDiskGasFraction != 0.0) {
      fprintf(stderr, "oracle: %s: the instability handed on an unstable gas fraction\n", in->path);
      problems++;
    }
  }

  if (sage26_apply_metal_enrichment_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  return problems;
}

/**
 * @brief Assert one substep's carried values ("<prefix><name>") where validated
 * @param full  1 for a single-call record (every carried value), 0 for a sequence substep
 */
static int check_carried(struct oracle_fixture *out, const char *prefix,
                         const struct substep_record *r, const int full) {
  int problems = 0;
  problems += oracle_c05_check_carried(out, prefix, "stars", &r->stars, &tally);
  problems += oracle_c05_check_carried(out, prefix, "reheated_mass", &r->reheated_mass, &tally);
  problems += oracle_c05_check_carried(out, prefix, "ejected_mass", &r->ejected_mass, &tally);
  if (!full) {
    return problems;
  }

  /* Where a clamp or cap binds, its value equals the reservoir it binds to */
  problems += oracle_c05_check_binding(out, prefix, "reheat_clamp_sf_binds",
                                       "reheated_mass_sf_clamped", r->cold_gas_after_sf, &tally);
  problems += oracle_c05_check_binding(out, prefix, "reheat_clamp_fb_binds",
                                       "reheated_mass_fb_clamped", r->cold_gas_after_sf, &tally);
  problems += oracle_c05_check_binding(out, prefix, "ejected_cap_binds", "ejected_mass_capped",
                                       r->locals.hot_gas_at_cap, &tally);
  problems +=
      oracle_c05_check_binding(out, prefix, "metals_cap_binds", "metals_hot_to_eject_capped",
                               r->locals.metals_hot_gas_at_cap, &tally);
  /* Where Mcrit is capped it equals the disk mass (the flag itself is not asserted) */
  char capped[ORACLE_MAX_NAME];
  snprintf(capped, sizeof(capped), "%sinstability_mcrit_capped", prefix);
  if (oracle_in_i32(out, capped) == 1) {
    char mcrit[ORACLE_MAX_NAME];
    snprintf(mcrit, sizeof(mcrit), "%sMcrit", prefix);
    tally.binding++;
    problems += oracle_check_f64(out, mcrit, r->instability.diskmass);
  }

  const struct {
    const char *name;
    const double *actual; /* NULL: a SAGE26 local the package does not form or hold */
  } values[] = {
      {"tdyn", NULL},
      {"cold_crit", NULL},
      {"strdot", NULL},
      {"atomicH", NULL},
      {"f_ion", NULL},
      {"stars_pre_renorm", &r->stars_pre_renorm},
      {"reheated_mass_pre_renorm", NULL},
      {"renorm_fac", NULL},
      {"ejected_mass_pre_floor", NULL},
      {"metallicity_sf", &r->metallicity_sf},
      {"metallicity_fb", &r->metallicity_fb},
      {"reheated_mass_sf_clamped", &r->reheated_mass_sf_clamped},
      {"reheated_mass_fb_clamped", &r->locals.reheated_mass_clamped},
      {"ejected_mass_capped", &r->locals.ejected_mass_capped},
      {"metallicity_hot", &r->locals.metallicity_hot},
      {"metals_hot_to_eject", &r->locals.metals_hot_to_eject},
      {"metals_hot_to_eject_capped", &r->locals.metals_hot_to_eject_capped},
      /* The instability's locals, from the helper's replay */
      {"diskmass", &r->instability.diskmass},
      {"Mcrit", &r->instability.mcrit},
      {"unstable_gas", &r->instability.unstable_gas},
      {"unstable_stars", &r->instability.unstable_stars},
      {"instability_metallicity", &r->instability.metallicity},
      {"unstable_gas_fraction", &r->instability.unstable_gas_fraction},
      {"FracZleaveDiskVal", NULL},
      {"yield_metals_to_hot", NULL},
  };
  for (size_t k = 0; k < COUNT(values); k++) {
    problems += oracle_c05_check_carried(out, prefix, values[k].name, values[k].actual, &tally);
  }
  for (size_t k = 0; k < COUNT(FLAGS); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "%s%s", prefix, FLAGS[k]);
    oracle_exclude(out, field);
  }
  return problems;
}

/** @brief One starformation_and_feedback fixture: one call of the chain */
static int check_single(struct oracle_fixture *in, struct oracle_fixture *out, struct chain *c) {
  int problems = 0;
  const int step = oracle_in_i32(in, "call.step");
  /* The substep midpoint: read by no module of this chain on R0 (see the file comment) */
  (void)oracle_in_f64(in, "call.time");
  if (step < 0 || step >= c->effective_steps) {
    fprintf(stderr, "oracle: %s: call.step out of range\n", in->path);
    return problems + 1;
  }

  struct substep_record r;
  problems += run_substep(in, c, step, &r);
  problems += check_end_state(out, c);
  problems += check_carried(out, "intermediate.", &r, 1);

  /* The history substrate: the step bin and the six histories */
  oracle_exclude(out, "intermediate.step_bin");
  for (int i = 0; i < c->ngal; i++) {
    oracle_c05_exclude_histories(out, "", i);
  }
  return problems;
}

/** @brief One starformation_and_feedback_sequence fixture: every substep of the chain */
static int check_sequence(struct oracle_fixture *in, struct oracle_fixture *out, struct chain *c) {
  int problems = 0;
  /* The writer's units, read by its SFR averaging (the history substrate) */
  (void)oracle_in_f64(in, "params.UnitMass_in_g");
  (void)oracle_in_f64(in, "params.UnitTime_in_s");

  for (int step = 0; step < c->effective_steps; step++) {
    char field[ORACLE_MAX_NAME];
    /* The substep midpoint: read by no module of this chain on R0 */
    snprintf(field, sizeof(field), "call.substep[%d].time", step);
    (void)oracle_in_f64(in, field);

    struct substep_record r;
    problems += run_substep(in, c, step, &r);

    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.substep[%d].", step);
    problems += check_carried(out, prefix, &r, 0);
    snprintf(field, sizeof(field), "%sstep_bin", prefix);
    oracle_exclude(out, field); /* the history substrate */
  }
  problems += check_end_state(out, c);

  /* The history substrate: SubstepsUsed, the histories and the writer's averages */
  const char *const writer[] = {"sfr_rate_divisor",
                                "sfr_metallicity_divisor",
                                "SfrDiskZ_bins_skipped",
                                "SfrBulgeZ_bins_skipped",
                                "SfrDisk",
                                "SfrBulge",
                                "SfrDiskZ",
                                "SfrBulgeZ"};
  for (int i = 0; i < c->ngal; i++) {
    oracle_exclude(out, oracle_galaxy_field(i, "SubstepsUsed"));
    oracle_c05_exclude_histories(out, "", i);
    for (size_t k = 0; k < COUNT(writer); k++) {
      oracle_exclude(out, oracle_prefixed_galaxy_field("intermediate.writer.", i, writer[k]));
    }
  }
  return problems;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  const int sequence = strcmp(in.routine, "starformation_and_feedback_sequence") == 0;
  if (oracle_load_pair(MODULE, name,
                       sequence ? "starformation_and_feedback_sequence"
                                : "starformation_and_feedback",
                       &in, &out) != 0) {
    return 1;
  }

  static struct chain c;
  int problems = setup_parameters(&in, &c);
  problems += setup_galaxies(&in, &c);
  if (problems != 0) {
    return problems;
  }
  problems += sequence ? check_sequence(&in, &out, &c) : check_single(&in, &out, &c);

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
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit through the chain (SKIP
 *          outside the oracle environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  const int ok = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: carried values asserted %d (validated two-sided), not validated %d, "
         "not package values %d, absent %d; binding clamps and caps asserted equal to their "
         "reservoir %d ",
         tally.asserted, tally.not_validated, tally.not_carried, tally.absent, tally.binding);
  TEST_ASSERT(ok, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_calculate_star_formation chained SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
