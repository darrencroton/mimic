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
 *   sage26_quasar_mode                   the instability's grow_black_hole (SFprescription 1
 *                                        gas-unstable fixtures only, see below)
 *   sage26_starburst_feedback            the instability's collisional_starburst_recipe (the
 *                                        same fixtures)
 *   sage26_apply_metal_enrichment        the disk yield
 *
 * and, after the last substep of a sequence fixture, the post_timestep finaliser
 *
 *   sage26_finalize_sfr_histories        SubstepsUsed (R/src/core_build_model.c:466-468)
 *                                        and the writer's averaging of the histories
 *                                        (R/src/io/save_gals_hdf5.c:934-959)
 *
 * It asserts bit for bit, on every galaxy of the fixture, the fourteen fields the
 * routine records (OutflowRate being the package's SupernovaOutflowRate) and
 * BlackHoleMass and QuasarModeBHaccretionMass, which the chain must leave as they
 * were, and all sixty elements of the six per-substep histories (SfrDisk, SfrBulge,
 * SfrDiskColdGas, SfrDiskColdGasMetals, SfrBulgeColdGas, SfrBulgeColdGasMetals; the
 * package's <history>Bin<k> properties, reached through shared/sage_sfr_history.h),
 * which the apply step samples at the substep's bin (S:1058-1060): after the call of a
 * single-call fixture, and after the last substep of a sequence fixture. It asserts
 * the step bin of every substep, and on sequence fixtures each galaxy's SubstepsUsed
 * and the writer's divisors, skipped-bin counts and four averages, which are the
 * package's SfrDisk, SfrBulge, SfrDiskZ and SfrBulgeZ outputs. It asserts the values
 * the package carries between modules (NewStellarMass
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
 * Scope (the oracle's records the package cannot yet meet are named and not asserted):
 *
 * - The instability (DiskInstabilityOn 1) runs sage26_disk_instability, SAGE26's
 *   check_disk_instability up to its black-hole and burst calls. With SFprescription 0 the
 *   oracle admits a wrapper fixture only where that routine does not reach them: a stable
 *   disk, or an unstable disk with no cold gas after feedback, whose stellar transfer into
 *   BulgeMass, MetalsBulgeMass and InstabilityBulgeMass
 *   (R/src/model_disk_instability.c:77-79) is asserted here like every other field
 *   (instability_unstable_no_coldgas), and the chain requires the module to hand on no
 *   unstable gas fraction. With SFprescription 1 the continuation runs (below).
 *   DiskInstabilityOn 0 fixtures (a labelled non-R0 value) do not run it.
 * - The SAGE26 locals the package never forms as named values (tdyn, cold_crit,
 *   strdot, atomicH, f_ion, the pre-renormalisation reheated mass, renorm_fac, the
 *   pre-floor ejected mass, FracZleaveDiskVal and the yield's escaping metals) reach
 *   the asserted fields and are not asserted themselves. The branch flags are read
 *   only to key the binding equalities.
 * - call.time (the substep midpoint) is read by no module of this chain: the instability's
 *   merger-family continuation passes it to a mode-1 burst, which does not read it. The parameters
 *   only that continuation reads (ThreshMajorMerger, BlackHoleGrowthRate, QuasarModeEfficiency,
 *   UnitVelocity_in_cm_per_s) are applied to the quasar and burst modules and reach the state only
 *   on the SFprescription 1 gas-unstable fixtures.
 *
 * The core-owned inputs are supplied as SAGE26 forms them: Vvir and Mvir widened
 * from SAGE26's floats, the row's dT = call.deltaT and num_substeps = effective_steps
 * (the module's dt is asserted equal to call.dt), G and Hubble_h, and SAGE26's R0 unit
 * factors, from which the feedback module derives EtaSNcode and EnergySNcode (both
 * asserted against the fixture through the shared helpers) and with which the
 * finaliser converts the averaged rates (UnitMass_in_g and UnitTime_in_s, asserted
 * equal to the sequence fixtures' writer units). StarFormingDiskFactor is
 * SAGE26's SF_DISK_RADIUS_FRAC, 3.0 (R/src/model_starformation_and_feedback.c:60), and
 * the same parameter is the instability's TOOMRE_DISK_FACTOR, also 3.0
 * (R/src/model_disk_instability.c:35).
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test
 * reports SKIP.
 *
 * The gas-unstable continuation at SFprescription 1. The oracle admits a prescription-1 wrapper
 * fixture whose disk is gas-unstable after feedback (R/src/model_starformation_and_feedback.c:1088
 * into R/src/model_disk_instability.c:90): the instability hands on its unstable gas fraction, and
 * the chain runs sage26_quasar_mode and then sage26_starburst_feedback (process_by_galaxy) on the
 * galaxy, as the production phase does, so the black-hole growth caps the hydrogen
 * (R/src/model_mergers.c:366-372) and the burst caps and refreshes it (S:1131-1135, S:1169-1173,
 * R/src/model_mergers.c:798-803). The test asserts the galaxy's state after sage26_quasar_mode
 * against the oracle's state after SAGE26's own grow_black_hole (intermediate.after_bh.*, the
 * hydrogen fields after the cap) and the end state after the burst and the yield, H2gas and H1gas
 * after the refresh among the fourteen fields. With SFprescription 0 the chain still requires no
 * unstable gas to be handed on. The two modules are initialised for every fixture, with the
 * fixture's ThreshMajorMerger, BlackHoleGrowthRate and QuasarModeEfficiency
 * and StarburstColdGasOn 1.
 *
 * SFprescription 1 (the fixtures, R2's flags: BR06 with radial integration and BulgeSizeOn 3): the
 * chain runs the module's BR06 branch, and the test also asserts, on every galaxy, the four
 * bulge-size members BulgeSizeOn 3 records and H2DepletionTime_Gyr, which must come out as it went
 * in. It asserts the SFprescription 1 values the oracle records, bit for bit whatever their
 * validation status: H2gas and H1gas as the SF step hands them on (package state between modules),
 * the commits' cap bounds max_h_sf and max_h_fb (from the apply-step replay's kernel locals), and
 * sfr_br06's rs_pc, the radial H2 before its cap and the cap's double budget, from a replay of the
 * BR06 branch through the shared H2 helper (shared/sage_h2_physics.h) on a copy, which must end in
 * the H2gas process() produced. The SFprescription 1 branch flags are asserted against
 * the same replays.
 *
 * The module also owns the fixtures of the two public H2 routines SAGE26's BR06 branch calls
 * (oracle_c08): calculate_molecular_fraction_BR06 and
 * calculate_molecular_fraction_radial_integration, each called directly. For those the test
 * calls the package's shared helpers (mimic_sage_br06_molecular_fraction,
 * mimic_sage_h2_radial_integration) and asserts the return, H2gas and H2DepletionTime_Gyr, the
 * branch flags, and every local and annulus the oracle records, bit for bit.
 *
 * FIREmodeOn (the FIRE fixtures, routine starformation_and_feedback_fire, R3's flags): each
 * galaxy's stored SnapNum and its redshift ZZ[SnapNum] are applied to the halos and the run's
 * redshift table, MassLoading is set on every galaxy (a stale value) and asserted on every galaxy
 * after the call (written by the FIRE path, unchanged with FIREmodeOn 0), and the feedback module's
 * FIRE budget is replayed through the shared helpers (shared/sage_fire_feedback_physics.h) on the
 * state it received, which must end in the budgets and MassLoading process() produced; the FIRE
 * branch flags and values the oracle records are asserted against that replay bit for bit
 * (sage26_oracle_fire.h). Every earlier fixture is read as before, with the feedback module's
 * required FIREmodeOn set to the fixture's 0. The oracle also admits a FIREmodeOn 1 fixture whose
 * disk is gas-unstable after the feedback (sf_fire_*_unstable*): the continuation runs as at
 * SFprescription 1, with the burst module reading the fixture's FIREmodeOn and the redshift table;
 * the test also asserts the disk MassLoading the feedback module wrote, unchanged through the
 * black-hole growth (intermediate.after_bh.*) and the burst (R/src/model_mergers.c:707-710), and
 * the instability burst's FIRE wiring through a replay of the burst module's call on copies,
 * required to end in process()'s state: the forming galaxy's own stored SnapNum, float Vvir and
 * that snapshot's redshift, never its FoF central's. The oracle's observations of SAGE26's burst
 * (intermediate.nested_burst_fire_*_sensitive) are read and counted.
 *
 * CGMrecipeOn (the CGMrecipeOn 1 cgm_sf_* fixtures, routine starformation_and_feedback_fire with
 * R4's flags, oracle README "The c10 Group, Part 3"): the fixture's CGMrecipeOn is the run's (every
 * module of the chain that reads it), each galaxy's Regime and CGM pair are set from the fixture
 * and asserted after the call (Regime unchanged), and every routing site of the disk path is
 * asserted against the oracle's selection AND against the expected galaxy's input Regime (the
 * site's wiring): the disk commit's (the apply-step replay's kernel locals) and the disk yield's (a
 * replay of the enrichment step through the shared routing helper on copies, required to end in
 * process()'s state) by the FoF central's Regime; on gas-unstable fixtures the instability's wind
 * (a replay of the quasar module's growth on copies, required to end in process()'s state, its
 * locals compared with the oracle's wind values and energy tests) by the wind host's, the forming
 * galaxy's, Regime; and the instability burst's commit and yield (the FIRE burst replay, its bundle
 * carrying the fixture's CGMrecipeOn) by the FoF central's. The central's pair its Regime does not
 * select and the forming galaxy's pair its own Regime does not select (the wind's) are asserted
 * unchanged against the input, and the oracle's observations of SAGE26's nested growth and burst
 * (intermediate.instability_*_regime_sensitive) are read and counted.
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
#include "shared/metallicity.h"
#include "shared/sage_agn_physics.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"
#include "shared/sage_disk_instability_physics.h"
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
extern int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_starburst_feedback_init(void);
extern int sage26_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);
extern int sage26_apply_metal_enrichment_init(void);
extern int sage26_apply_metal_enrichment_process(struct ModuleContext *ctx, struct Halo *halos,
                                                 int ngal);
extern int sage26_finalize_sfr_histories_process(struct ModuleContext *ctx, struct Halo *halos,
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

static struct oracle_c05_carried_tally tally;
static struct oracle_c08_local_tally h2_tally;
static struct oracle_c09_tally fire_tally;
/* SFprescription 1 gas-unstable continuations run through the quasar and burst modules */
static int continuations_br06 = 0;
/* Gas-unstable FIRE fixtures, their instability-burst FIRE wiring asserted, SAGE26's burst observed
 * FIRE- and redshift-sensitive, and the disk MassLoading kept */
static int fire_continuations = 0;
static int fire_wiring_asserted = 0;
static int fire_nested_observed = 0;
static int fire_mass_loading_kept = 0;
/* CGMrecipeOn 1 fixtures; their routing sites asserted against the oracle and the expected galaxy's
 * Regime (the disk commit, the disk yield, the instability's wind and its burst's commit and
 * yield); the unselected pairs asserted unchanged; the wind values compared; the observations
 */
static int cgm_fixtures = 0;
static int cgm_sites_asserted = 0;
static int cgm_sites_cgm = 0;
static int cgm_wind_sites = 0;
static int cgm_burst_sites = 0;
static int cgm_unselected_pairs = 0;
static int cgm_wind_values = 0;
static int cgm_wind_values_validated = 0;
static int cgm_observed_host_sensitive = 0;
static int cgm_observed_other_insensitive = 0;

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

/** @brief The CGM pair a CGMrecipeOn 1 fixture records (oracle README "The c10 Group, Part 3") */
static const char *const CGM_PAIR[] = {"CGMgas", "MetalsCGMgas"};

/** @brief The bulge-size members a BulgeSizeOn 3 fixture records (oracle README "The c07 Group") */
static const char *const SIZES[] = {"MergerBulgeMass", "BulgeRadius", "MergerBulgeRadius",
                                    "InstabilityBulgeRadius"};

/** @brief The state of one fixture's chain */
struct chain {
  int ngal;
  int p;
  int centralgal;
  int effective_steps;
  int disk_instability_on;
  int bulge_size_on;                     /* 0 or 3 */
  struct oracle_c08_sf_config sf;        /* SFprescription 0 or 1 and the H2 configuration */
  int fire;                              /* a starformation_and_feedback_fire fixture */
  int fire_mode_on;                      /* its FIREmodeOn (0 for every earlier fixture) */
  int snapnum[MAX_GALAXIES];             /* each galaxy's stored SnapNum */
  int cgm_recipe_on;                     /* CGMrecipeOn, 0 for every earlier fixture */
  struct GalaxyData input[MAX_GALAXIES]; /* the galaxies as the fixture set them */
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
  /* SFprescription 1: the BR06 branch's replay and the values the SF step hands on, NaN (and the
   * flags -1) where the branch does not form them */
  float rs_pc;
  float h2_radial;
  double h2_budget;
  float h2_sf;
  float h1_sf;
  /* SFprescription 1 gas-unstable continuation: galaxy p after sage26_quasar_mode */
  int after_bh_present;
  struct GalaxyData after_bh;
  /* The MassLoading the feedback module wrote, before the continuation */
  float mass_loading_sn;
  int rs_nonpositive;
  int radial_returned_early;
  int h2_cap_binds;
  int sf_from_h2;
  struct MimicSageHydrogenCapLocals caps_sf;
  /* The feedback module's FIRE budget replayed through the shared helpers (FIREmodeOn 1;
   * initialised locals for FIREmodeOn 0) */
  struct oracle_c09_sn_replay fire;
  /* The disk yield's selection (the enrichment replay), the instability's wind (the quasar module's
   * replay, reservoir_cgm -1 where it does not run) and its burst's two selections (-1 where the
   * burst does not run) */
  int yield_reservoir_cgm;
  struct MimicSageQuasarWindLocals wind;
  int burst_feedback_reservoir_cgm;
  int burst_yield_reservoir_cgm;
};

/**
 * @brief Apply the fixture's flags and parameters, then initialise the chain's modules
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in, struct chain *c) {
  int problems = 0;
  reset_config();

  /* R0's fixed flags (oracle README "Flags and Parameters"); SFprescription 0, or 1 with R2's H2
   * configuration; BulgeSizeOn 0 or 3 (R1 onward) */
  problems += oracle_c08_read_sf_config(in, 0, &c->sf);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  /* CGMrecipeOn: 0 for every earlier fixture; 0 or 1 for a starformation_and_feedback_fire fixture
   * (1 for the cgm_sf_* fixtures, R4's flags), the run's for every module */
  c->cgm_recipe_on = oracle_in_i32(in, "params.CGMrecipeOn");
  if (c->cgm_recipe_on != MIMIC_SAGE_CGM_RECIPE_OFF &&
      !(c->fire && c->cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON)) {
    fprintf(stderr, "oracle: %s: params.CGMrecipeOn must be 0, or 1 for %s\n", in->path,
            ORACLE_C09_ROUTINE);
    problems++;
  }
  /* FIREmodeOn: 0 for every earlier fixture; 0 or 1 with each galaxy's stored snapshot and its
   * redshift for a FIRE fixture (applied to the run's redshift table) */
  if (c->fire) {
    const int ngal = oracle_in_i32(in, "call.ngal");
    if (ngal < 1 || ngal > MAX_GALAXIES) {
      fprintf(stderr, "oracle: %s: call.ngal out of range\n", in->path);
      return problems + 1;
    }
    problems += oracle_c09_read_fire_config(in, ngal, c->snapnum, &c->fire_mode_on);
  } else {
    c->fire_mode_on = MIMIC_SAGE_FIRE_MODE_OFF;
    problems += oracle_c05_require_flag(in, "params.FIREmodeOn", 0);
    /* The feedback module's required FIREmodeOn, the fixture's 0 */
    problems += oracle_set_model_param("FIREmodeOn", 0.0) != 0;
  }
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

  /* Core-owned constants and SAGE26's R0 unit factors (R/src/core_init.c:94-103) */
  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  MimicConfig.UnitLength_in_cm = SAGE26_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = SAGE26_UNIT_MASS_IN_G;
  /* Read only by the instability's merger-family continuation (quasar wind), not reached here */
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
  /* sage26_disk_instability's required BulgeSizeOn, the fixture's (checked above) */
  if (oracle_set_model_param("BulgeSizeOn", (double)c->bulge_size_on) != 0) {
    problems++;
  }
  /* The SF module's SFprescription and H2 integration parameters and the apply step's
   * SFprescription, the fixture's (the H2 parameters at R2's values for SFprescription 0) */
  problems += oracle_c08_set_sf_parameters(&c->sf);
  /* Read only by the instability's merger-family continuation (black-hole growth, quasar wind,
   * burst), which the SFprescription 1 gas-unstable fixtures reach (SAGE26's ThreshMajorMerger is
   * the package's ThresholdMajorMerger), and the burst module's StarburstColdGasOn 1 */
  if (oracle_set_model_param("ThresholdMajorMerger",
                             oracle_in_f64(in, "params.ThreshMajorMerger")) != 0 ||
      oracle_set_model_param("BlackHoleGrowthRate",
                             oracle_in_f64(in, "params.BlackHoleGrowthRate")) != 0 ||
      oracle_set_model_param("QuasarModeEfficiency",
                             oracle_in_f64(in, "params.QuasarModeEfficiency")) != 0 ||
      oracle_set_model_param("StarburstColdGasOn", 1.0) != 0 ||
      /* CGMrecipeOn: required by the commits', yields', wind's and disruption's
       * modules, the fixture's */
      oracle_set_model_param("CGMrecipeOn", (double)c->cgm_recipe_on) != 0) {
    problems++;
  }

  /* The substep phase the modules' init() dependency checks inspect */
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
    /* The stored snapshot: the FIRE scaling's redshift index; read by no other module of the chain
     * once dT is positive */
    h->SnapNum = c->fire ? c->snapnum[i] : 0;
    h->dT = deltaT; /* core-owned: SAGE26's deltaT of the forming galaxy's group */
    h->Type = oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    /* SAGE26's float halo values, exactly widened to the core's doubles */
    h->Mvir = oracle_in_f32(in, oracle_galaxy_field(i, "Mvir"));
    h->Vvir = oracle_in_f32(in, oracle_galaxy_field(i, "Vvir"));
    h->Vmax = oracle_in_f32(in, oracle_galaxy_field(i, "Vmax"));
    const char *const extra[] = {"DiskScaleRadius", "BlackHoleMass", "QuasarModeBHaccretionMass"};
    problems += oracle_c05_in_floats(in, i, g, extra, COUNT(extra));
    problems += oracle_c05_in_floats(in, i, g, written, count);
    /* BulgeSizeOn 3: the four size members (oracle README "The c07 Group") */
    if (c->bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        *oracle_c06_galaxy_float(g, SIZES[k]) = oracle_in_f32(in, oracle_galaxy_field(i, SIZES[k]));
      }
    }
    /* SFprescription 1: the K13 depletion time, which the chain must leave as it was */
    if (c->sf.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
      g->H2DepletionTime_Gyr = oracle_in_f32(in, oracle_galaxy_field(i, "H2DepletionTime_Gyr"));
    }
    /* The stale MassLoading the FIRE path overwrites (and FIREmodeOn 0 leaves) */
    if (c->fire) {
      g->MassLoading = oracle_in_f32(in, oracle_galaxy_field(i, "MassLoading"));
    }
    /* Regime and the CGM pair, which CGMrecipeOn 1 routes by and into */
    if (c->cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
      g->Regime = oracle_in_i32(in, oracle_galaxy_field(i, "Regime"));
      problems += oracle_c05_in_floats(in, i, g, CGM_PAIR, COUNT(CGM_PAIR));
    }
    /* The six histories, every element set explicitly (oracle README "Input Fields") */
    problems += oracle_c05_in_histories(in, i, g);

    /* Transport and diagnostic state the chain writes before it reads */
    g->NewStellarMass = 0.0;
    g->SupernovaReheatedMass = 0.0;
    g->SupernovaEjectedMass = 0.0;
    g->UnstableDiskGasFraction = 0.0;
    g->StarFormationRate = 0.0f;
  }

  memcpy(c->input, c->galaxies, sizeof(c->input));
  memset(&c->ctx, 0, sizeof(c->ctx));
  c->ctx.params = &MimicConfig;
  c->ctx.central_galaxy = &c->halos[c->centralgal];
  c->ctx.num_substeps = c->effective_steps;
  return problems;
}

/**
 * @brief Compare the fourteen fields, the two untouched ones and the sixty history
 *        elements of every galaxy
 */
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
    if (c->fire) {
      problems +=
          oracle_check_f32(out, oracle_galaxy_field(i, "MassLoading"), c->galaxies[i].MassLoading);
    }
    if (c->cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
      problems += oracle_check_i32(out, oracle_galaxy_field(i, "Regime"), c->galaxies[i].Regime);
      problems += oracle_c05_check_floats(out, "", i, &c->galaxies[i], CGM_PAIR, COUNT(CGM_PAIR));
    }
    problems += oracle_c05_check_histories(out, "", i, &c->galaxies[i]);
  }
  return problems;
}

/**
 * @brief Replay the module's BR06 branch (sfr_br06, R/src/model_starformation_and_feedback.c:
 *        193-252) on a copy of the galaxy, through the shared H2 helper, for its values
 *
 * The values are formed with sfr_br06's own expressions: the float h and rs_pc (:214-215), the
 * radial integration (:222), the double cap budget ColdGas * 0.74 (:239) and tdyn (:212).
 * Leaves NaN (and -1 flags) where the branch does not form them.
 */
static void replay_br06(const struct chain *c, const struct GalaxyData *gal, const float vvir,
                        struct substep_record *r) {
  r->rs_pc = NAN;
  r->h2_radial = NAN;
  r->h2_budget = NAN;
  r->h2_sf = NAN;
  r->rs_nonpositive = -1;
  r->radial_returned_early = -1;
  r->h2_cap_binds = -1;
  r->sf_from_h2 = -1;
  if (vvir <= 0.0) {
    return;
  }
  const double tdyn = SAGE26_SF_DISK_RADIUS_FRAC * gal->DiskScaleRadius / vvir;
  const float h = (float)MimicConfig.Hubble_h;
  r->rs_pc = (float)(gal->DiskScaleRadius * 1.0e6 / h);
  r->rs_nonpositive = r->rs_pc <= 0.0;
  if (r->rs_nonpositive) {
    return;
  }
  struct GalaxyData copy = *gal;
  struct MimicSageH2RadialLocals radial;
  (void)mimic_sage_h2_radial_integration(&copy, MimicConfig.Hubble_h, c->sf.h2_radial_nbins,
                                         c->sf.h2_radial_rmax_factor, &radial);
  r->radial_returned_early = radial.returned_early;
  r->h2_radial = copy.H2gas;
  r->h2_budget = copy.ColdGas * SAGE_HYDROGEN_MASS_FRAC;
  r->h2_cap_binds = copy.H2gas > r->h2_budget;
  if (r->h2_cap_binds) {
    copy.H2gas = (float)r->h2_budget;
  }
  r->h2_sf = copy.H2gas;
  r->sf_from_h2 = copy.H2gas > 0.0 && tdyn > 0.0;
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

  const int br06 = c->sf.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06;
  if (br06) {
    replay_br06(c, gal, (float)halo->Vvir, r);
  }

  if (sage26_calculate_star_formation_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  r->stars_pre_renorm = gal->NewStellarMass;
  /* What the SF step hands on: H2gas (where the BR06 branch formed it) and H1gas */
  r->h1_sf = br06 ? gal->H1gas : NAN;
  if (br06 && !isnan(r->h2_sf)) {
    if (oracle_f32_to_bits(r->h2_sf) != oracle_f32_to_bits(gal->H2gas)) {
      fprintf(stderr, "oracle: %s: the BR06 replay's H2gas differs from process()\n", in->path);
      problems++;
    }
    r->h2_sf = gal->H2gas;
  }

  /* Replay the feedback module's FIRE budget on the state it receives, before process() */
  mimic_sage_fire_locals_init(&r->fire.l);
  if (c->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    oracle_c09_replay_sn(
        gal->NewStellarMass, gal->ColdGas, (float)halo->Vvir, MimicConfig.ZZ[halo->SnapNum],
        oracle_in_f64(in, "params.FeedbackReheatingEpsilon"),
        oracle_in_f64(in, "params.FeedbackEjectionEfficiency"),
        oracle_in_f64(in, "params.EtaSNcode"), oracle_in_f64(in, "params.EnergySNcode"), &r->fire);
  }
  const float mass_loading_before = gal->MassLoading;

  if (sage26_calculate_supernova_feedback_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  r->stars = gal->NewStellarMass;
  if (c->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    if (oracle_f64_to_bits(r->fire.stars) != oracle_f64_to_bits(gal->NewStellarMass) ||
        oracle_f64_to_bits(r->fire.reheated) != oracle_f64_to_bits(gal->SupernovaReheatedMass) ||
        oracle_f64_to_bits(r->fire.ejected) != oracle_f64_to_bits(gal->SupernovaEjectedMass) ||
        oracle_f32_to_bits(r->fire.mass_loading) != oracle_f32_to_bits(gal->MassLoading)) {
      fprintf(stderr, "oracle: %s: the FIRE replay differs from the feedback module\n", in->path);
      problems++;
    }
  } else if (c->fire &&
             oracle_f32_to_bits(mass_loading_before) != oracle_f32_to_bits(gal->MassLoading)) {
    fprintf(stderr, "oracle: %s: FIREmodeOn 0 wrote MassLoading\n", in->path);
    problems++;
  }
  r->reheated_mass = gal->SupernovaReheatedMass;
  r->ejected_mass = gal->SupernovaEjectedMass;
  r->mass_loading_sn = gal->MassLoading;

  /* Replay the apply step through the shared commits on a copy, for its locals */
  struct GalaxyData replay[MAX_GALAXIES];
  memcpy(replay, c->galaxies, sizeof(replay));
  struct GalaxyData *rp = &replay[c->p];
  r->metallicity_sf = mimic_get_metallicity(rp->ColdGas, rp->MetalsColdGas);
  mimic_sage_update_from_star_formation(rp, r->stars, r->metallicity_sf,
                                        oracle_in_f64(in, "params.RecycleFraction"),
                                        c->sf.sf_prescription, &r->caps_sf);
  r->cold_gas_after_sf = rp->ColdGas;
  r->metallicity_fb = mimic_get_metallicity(rp->ColdGas, rp->MetalsColdGas);
  r->reheated_mass_sf_clamped =
      r->reheated_mass > rp->ColdGas ? (double)rp->ColdGas : r->reheated_mass;
  mimic_sage_update_from_feedback(rp, &replay[c->centralgal], r->reheated_mass_sf_clamped,
                                  r->ejected_mass, r->metallicity_fb, c->sf.sf_prescription,
                                  c->cgm_recipe_on, &r->locals);

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
  r->after_bh_present = 0;
  mimic_sage_quasar_wind_locals_reset(&r->wind);
  r->burst_feedback_reservoir_cgm = -1;
  r->burst_yield_reservoir_cgm = -1;
  if (c->disk_instability_on) {
    /* Replay the instability through its shared helper on a copy, for its locals */
    struct GalaxyData instability_replay[MAX_GALAXIES];
    memcpy(instability_replay, c->galaxies, sizeof(instability_replay));
    struct Halo replay_halo = *halo;
    replay_halo.galaxy = &instability_replay[c->p];
    const struct MimicSageBulgeSize sizes = {c->bulge_size_on, MimicConfig.Hubble_h};
    instability_replay[c->p].UnstableDiskGasFraction = mimic_sage_check_disk_instability_with_sizes(
        &replay_halo, &c->ctx, SAGE26_TOOMRE_DISK_FACTOR, &sizes, &r->instability);

    if (sage26_disk_instability_process(&c->ctx, halo, 1) != 0) {
      problems++;
    }
    if (memcmp(instability_replay, c->galaxies, sizeof(struct GalaxyData) * (size_t)c->ngal) != 0) {
      fprintf(stderr, "oracle: %s: the instability replay differs from process()\n", in->path);
      problems++;
    }
    /* SFprescription 0: the oracle admits no wrapper fixture whose instability reaches the gas
     * branch, so no unstable gas is handed on. SFprescription 1: the continuation runs, the
     * black-hole growth and then the burst, as the production phase orders them */
    if (gal->UnstableDiskGasFraction != 0.0) {
      if (!br06) {
        fprintf(stderr, "oracle: %s: the instability handed on an unstable gas fraction\n",
                in->path);
        problems++;
      } else {
        /* The quasar module's growth and wind replayed on copies, for the wind's locals */
        struct GalaxyData bh_replay[MAX_GALAXIES];
        memcpy(bh_replay, c->galaxies, sizeof(bh_replay));
        struct Halo bh_halo = *halo;
        bh_halo.galaxy = &bh_replay[c->p];
        struct MimicSageBlackHoleLocals bh;
        (void)mimic_apply_black_hole_growth(&bh_halo, gal->UnstableDiskGasFraction,
                                            oracle_in_f64(in, "params.BlackHoleGrowthRate"),
                                            oracle_in_f64(in, "params.QuasarModeEfficiency"),
                                            &c->ctx, c->sf.sf_prescription, c->cgm_recipe_on, &bh);
        if (sage26_quasar_mode_process(&c->ctx, halo, 1) != 0) {
          problems++;
        }
        if (memcmp(bh_replay, c->galaxies, sizeof(struct GalaxyData) * (size_t)c->ngal) != 0) {
          fprintf(stderr, "oracle: %s: the black-hole growth replay differs from process()\n",
                  in->path);
          problems++;
        }
        r->wind = bh.wind;
        r->after_bh = *gal;
        r->after_bh_present = 1;
        /* The burst module's call replayed on copies, for its FIRE record */
        static struct oracle_c09_burst_replay burst_replay;
        if (c->fire) {
          const struct MimicSageBulgeSize burst_sizes = {c->bulge_size_on, MimicConfig.Hubble_h};
          struct MimicStarburstParams bundle;
          oracle_c09_burst_bundle(in, &burst_sizes, c->sf.sf_prescription, c->fire_mode_on, 0,
                                  &bundle);
          /* The module's bundle carries the run's CGMrecipeOn to the burst's routing */
          bundle.cgm_recipe_on = c->cgm_recipe_on;
          oracle_c09_replay_instability_burst(&burst_replay, c->galaxies, c->halos, c->ngal, c->p,
                                              c->centralgal, &c->ctx, &bundle);
        }
        if (sage26_starburst_feedback_process(&c->ctx, halo, 1) != 0) {
          problems++;
        }
        if (c->fire) {
          /* The instability burst's wiring: the forming galaxy's own stored SnapNum, float Vvir
           * and redshift (the fixture's), never its FoF central's */
          problems +=
              oracle_c09_replay_matches(in->path, "instability burst", &burst_replay, c->galaxies);
          problems += oracle_c09_check_burst_wiring(in->path, "instability burst",
                                                    &burst_replay.locals.fire, c->fire_mode_on,
                                                    c->snapnum[c->p], (float)halo->Vvir);
          fire_wiring_asserted++;
          r->burst_feedback_reservoir_cgm = burst_replay.locals.feedback.reservoir_cgm;
          r->burst_yield_reservoir_cgm = burst_replay.locals.yield_reservoir_cgm;
        }
        continuations_br06++;
      }
    }
  }

  /* The enrichment step replayed on copies through the shared routing helper, as the module forms
   * it (S:1093-1108), for its selection; it must end in process()'s state */
  struct GalaxyData yield_replay[MAX_GALAXIES];
  memcpy(yield_replay, c->galaxies, sizeof(yield_replay));
  {
    struct GalaxyData *yg = &yield_replay[c->p];
    struct GalaxyData *yc = &yield_replay[c->centralgal];
    const double stars = yg->NewStellarMass;
    const double yield = oracle_in_f64(in, "params.Yield");
    yg->NewStellarMass = 0.0;
    double escaping;
    if (yg->ColdGas > SAGE_COLD_GAS_YIELD_THRESHOLD) {
      const float central_mvir = (float)c->halos[c->centralgal].Mvir;
      const double frac = oracle_in_f64(in, "params.FracZleaveDisk") *
                          exp(-1.0 * central_mvir / SAGE_METAL_EJECTION_MVIR_SCALE);
      yg->MetalsColdGas = (float)fma(yield * (1.0 - frac), stars, (double)yg->MetalsColdGas);
      escaping = yield * frac * stars;
    } else {
      escaping = yield * stars;
    }
    r->yield_reservoir_cgm = mimic_sage_cgm_reservoir_selected(c->cgm_recipe_on, yc->Regime);
    mimic_sage_add_metals_to_hot_reservoir(yc, c->cgm_recipe_on, escaping);
  }
  if (sage26_apply_metal_enrichment_process(&c->ctx, halo, 1) != 0) {
    problems++;
  }
  if (memcmp(yield_replay, c->galaxies, sizeof(struct GalaxyData) * (size_t)c->ngal) != 0) {
    fprintf(stderr, "oracle: %s: the enrichment replay differs from process()\n", in->path);
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
                                       r->locals.selected_gas_at_cap, &tally);
  problems +=
      oracle_c05_check_binding(out, prefix, "metals_cap_binds", "metals_hot_to_eject_capped",
                               r->locals.selected_metals_at_cap, &tally);
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
      {"metallicity_hot", &r->locals.metallicity_selected},
      {"metals_hot_to_eject", &r->locals.metals_to_eject},
      {"metals_hot_to_eject_capped", &r->locals.metals_to_eject_capped},
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

/**
 * @brief Assert the record of one SFprescription 1 call (oracle README "The c08 Group")
 *
 * The BR06 branch's flags and values from the replay, the H2gas and H1gas the SF step handed
 * on, and the two commits' caps from the apply-step replay's kernel locals, each compared bit
 * for bit; where a cap binds, its bound equals the field it set.
 */
static int check_h2_record(struct oracle_fixture *out, const struct substep_record *r) {
  int problems = 0;
  const struct MimicSageHydrogenCapLocals *fb = &r->locals.hydrogen_caps;
  problems += oracle_c06_check_flag(out, "br06_rs_nonpositive", r->rs_nonpositive);
  problems += oracle_c06_check_flag(out, "br06_radial_returned_early", r->radial_returned_early);
  problems += oracle_c06_check_flag(out, "br06_h2_cap_binds", r->h2_cap_binds);
  problems += oracle_c06_check_flag(out, "br06_sf_from_h2", r->sf_from_h2);
  problems += oracle_c06_check_flag(out, "hydrogen_caps_sf_tracked", r->caps_sf.tracks_h2);
  problems += oracle_c06_check_flag(out, "h2_cap_sf_binds", r->caps_sf.h2_cap_binds);
  problems += oracle_c06_check_flag(out, "h1_cap_sf_binds", r->caps_sf.h1_cap_binds);
  problems += oracle_c06_check_flag(out, "hydrogen_caps_fb_tracked", fb->tracks_h2);
  problems += oracle_c06_check_flag(out, "h2_cap_fb_binds", fb->h2_cap_binds);
  problems += oracle_c06_check_flag(out, "h1_cap_fb_binds", fb->h1_cap_binds);

  problems += oracle_c08_check_local(out, "rs_pc", r->rs_pc, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "H2gas_radial", r->h2_radial, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "h2_budget", r->h2_budget, 0, 1, &h2_tally);
  problems += oracle_c08_check_local(out, "H2gas_sf", r->h2_sf, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "H1gas_sf", r->h1_sf, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "max_h_sf", r->caps_sf.max_h, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "max_h_fb", fb->max_h, 1, 0, &h2_tally);
  /* The BR06 cap's binding value is the budget narrowed into H2gas */
  if (r->h2_cap_binds == 1) {
    h2_tally.binding++;
    problems += oracle_f32_to_bits(r->h2_sf) != oracle_f32_to_bits((float)r->h2_budget);
  }
  return problems;
}

/**
 * @brief Assert the gas-unstable continuation's state after the black-hole growth
 *
 * The oracle records galaxy p after SAGE26's own grow_black_hole (its hydrogen cap and wind)
 * and before the burst only where the gas branch ran; the chain must have run the quasar
 * module exactly there. The fourteen written fields, the size members, H2DepletionTime_Gyr and
 * the two black-hole fields are compared bit for bit.
 */
static int check_after_bh(struct oracle_fixture *out, const struct chain *c,
                          const struct substep_record *r) {
  const char *probe = oracle_prefixed_galaxy_field("intermediate.after_bh.", c->p, "ColdGas");
  if (oracle_has(out, probe) != r->after_bh_present) {
    fprintf(stderr, "oracle: %s: the oracle %s a state after the black-hole growth, the chain %s\n",
            out->path, r->after_bh_present ? "records no" : "records", "disagrees");
    return 1;
  }
  if (!r->after_bh_present) {
    return 0;
  }
  int problems = 0;
  struct GalaxyData g = r->after_bh;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  problems += oracle_c05_check_floats(out, "intermediate.after_bh.", c->p, &g, written, count);
  problems +=
      oracle_c05_check_floats(out, "intermediate.after_bh.", c->p, &g, UNTOUCHED, COUNT(UNTOUCHED));
  if (c->bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
    for (size_t k = 0; k < COUNT(SIZES); k++) {
      problems += oracle_check_f32(
          out, oracle_prefixed_galaxy_field("intermediate.after_bh.", c->p, SIZES[k]),
          *oracle_c06_galaxy_float(&g, SIZES[k]));
    }
  }
  problems += oracle_check_f32(
      out, oracle_prefixed_galaxy_field("intermediate.after_bh.", c->p, "H2DepletionTime_Gyr"),
      g.H2DepletionTime_Gyr);
  /* The MassLoading the disk path wrote, which the growth leaves */
  if (c->fire) {
    problems += oracle_c09_check_mass_loading(out, "intermediate.after_bh.", c->p,
                                              r->mass_loading_sn, g.MassLoading);
  }
  /* The wind host's Regime and CGM pair after its wind */
  if (c->cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
    problems += oracle_check_i32(
        out, oracle_prefixed_galaxy_field("intermediate.after_bh.", c->p, "Regime"), g.Regime);
    problems +=
        oracle_c05_check_floats(out, "intermediate.after_bh.", c->p, &g, CGM_PAIR, COUNT(CGM_PAIR));
  }
  return problems;
}

/**
 * @brief Compare one routing site's selection: the oracle's flag against the package's, and the
 *        package's against the routing galaxy's input Regime (1 exactly when it is 0)
 * @param expected_regime  The input Regime of the galaxy that must route this site, or -1 where
 *                         the site does not run (the package's selection must then be -1)
 * @return Number of mismatches
 */
static int check_site(struct oracle_fixture *out, const char *flag, const int actual,
                      const int expected_regime) {
  int problems = oracle_c06_check_flag(out, flag, actual);
  const int expected = expected_regime < 0 ? -1 : expected_regime == 0;
  if (actual != expected) {
    fprintf(stderr, "oracle: %s: %s routed by the wrong galaxy's Regime (%d, expected %d)\n",
            out->path, flag, actual, expected);
    problems++;
  }
  if (actual >= 0) {
    cgm_sites_asserted++;
    cgm_sites_cgm += actual;
  }
  return problems;
}

/**
 * @brief Compare one wind value: the oracle's tail-checked markers and its value bit for bit with
 *        the package's kernel local, NaN exactly where the oracle records it absent
 * @return Number of mismatches and marker errors
 */
static int check_wind_value(struct oracle_fixture *out, const char *name, const double actual) {
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
  problems += oracle_in_i32(out, field) != 1;
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  problems += oracle_in_i32(out, field) != ORACLE_C10_TAIL;
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  problems += validated != (up && down);
  snprintf(field, sizeof(field), "intermediate.%s", name);
  problems += oracle_check_f64(out, field, actual);
  cgm_wind_values++;
  cgm_wind_values_validated += validated;
  return problems;
}

/**
 * @brief The record of one CGMrecipeOn 1 call: every routing site's selection against the oracle
 *        and the expected galaxy's input Regime, the wind's tests and values, the unselected pairs
 *        unchanged, and the oracle's observations of SAGE26's nested routines counted
 * @return Number of mismatches
 */
static int check_cgm_record(struct oracle_fixture *out, const struct chain *c,
                            const struct substep_record *r) {
  int problems = oracle_check_i32(out, "intermediate.cgm_recipe_on", MIMIC_SAGE_CGM_RECIPE_ON);
  const int central_regime = c->input[c->centralgal].Regime;
  const int host_regime = c->input[c->p].Regime;
  const int continued = r->after_bh_present;
  /* The disk commit and the disk yield: the FoF central's Regime (S:1175-1176, S:1102, S:1108) */
  problems += check_site(out, "feedback_reservoir_cgm", r->locals.reservoir_cgm, central_regime);
  problems += check_site(out, "yield_reservoir_cgm", r->yield_reservoir_cgm, central_regime);
  /* The instability's wind: the wind host's, the forming galaxy's (D:103, M:406-407) */
  problems += check_site(out, "instability_wind_reservoir_cgm", r->wind.reservoir_cgm,
                         continued ? host_regime : -1);
  problems += oracle_c06_check_flag(out, "instability_wind_ejects_cold", r->wind.ejects_cold);
  problems +=
      oracle_c06_check_flag(out, "instability_wind_ejects_selected", r->wind.ejects_selected);
  /* The instability burst's commit and yield: the FoF central's (M:791, M:824, M:830) */
  problems += check_site(out, "instability_burst_feedback_reservoir_cgm",
                         r->burst_feedback_reservoir_cgm, continued ? central_regime : -1);
  problems += check_site(out, "instability_burst_yield_reservoir_cgm", r->burst_yield_reservoir_cgm,
                         continued ? central_regime : -1);
  cgm_wind_sites += continued;
  cgm_burst_sites += continued;
  problems += check_wind_value(out, "instability_wind_quasar_energy", r->wind.quasar_energy);
  problems += check_wind_value(out, "instability_wind_cold_gas_energy", r->wind.cold_gas_energy);
  problems += check_wind_value(out, "instability_wind_cgm_gas_energy", r->wind.cgm_gas_energy);
  problems += check_wind_value(out, "instability_wind_hot_gas_energy", r->wind.hot_gas_energy);

  /* The pair each galaxy's own Regime does not select is never written on this path: the central's
   * by the commits and yields, the wind host's by its wind; any other galaxy keeps both */
  for (int i = 0; i < c->ngal; i++) {
    struct GalaxyData end = c->galaxies[i];
    if (i == c->centralgal || i == c->p) {
      const int selects_cgm = c->input[i].Regime == 0;
      problems += oracle_c10_check_unselected(out->path, i, &c->input[i], &end,
                                              oracle_c10_pair(!selects_cgm), 2);
      cgm_unselected_pairs++;
    } else {
      problems +=
          oracle_c10_check_unselected(out->path, i, &c->input[i], &end, oracle_c10_pair(0), 2);
      problems +=
          oracle_c10_check_unselected(out->path, i, &c->input[i], &end, oracle_c10_pair(1), 2);
      cgm_unselected_pairs += 2;
    }
  }

  /* SAGE26's nested growth and burst observed (counted, not port values) */
  if (continued) {
    cgm_observed_host_sensitive +=
        oracle_c09_observed(out, "instability_wind_host_regime_sensitive") == 1;
    cgm_observed_host_sensitive +=
        oracle_c09_observed(out, "instability_burst_central_regime_sensitive") == 1;
    cgm_observed_other_insensitive +=
        oracle_c09_observed(out, "instability_wind_central_regime_sensitive") == 0;
    cgm_observed_other_insensitive +=
        oracle_c09_observed(out, "instability_burst_host_regime_sensitive") == 0;
  }
  cgm_fixtures++;
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
  if (c->sf.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    problems += check_h2_record(out, &r);
    problems += check_after_bh(out, c, &r);
  }
  /* The FIRE record against the replay; with no stars the loading's values reach the state only
   * through MassLoading (narrowing-limited) */
  if (c->fire) {
    problems += oracle_c09_check_fire_record(out, c->fire_mode_on, &r.fire.l,
                                             r.stars_pre_renorm == 0.0, &fire_tally);
    /* The gas-unstable continuation's burst leaves the MassLoading the disk path wrote
     * (R/src/model_mergers.c:707-710); SAGE26's burst observed */
    if (r.after_bh_present) {
      const float end = c->galaxies[c->p].MassLoading;
      if (oracle_f32_to_bits(end) != oracle_f32_to_bits(r.mass_loading_sn)) {
        fprintf(stderr, "oracle: %s: the continuation's burst changed MassLoading\n", in->path);
        problems++;
      }
      fire_mass_loading_kept++;
      fire_continuations++;
      const int mode_sensitive = oracle_c09_observed(out, "nested_burst_fire_mode_sensitive");
      const int z_sensitive = oracle_c09_observed(out, "nested_burst_fire_redshift_sensitive");
      fire_nested_observed += mode_sensitive == 1 && z_sensitive == 1;
    }
  }

  /* CGMrecipeOn 1's routing record */
  if (c->cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
    problems += check_cgm_record(out, c, &r);
  }

  /* The bin the apply step sampled (R/src/core_build_model.c:514-515) */
  problems += oracle_check_i32(out, "intermediate.step_bin",
                               mimic_sage_sfr_step_bin(step, c->effective_steps));
  return problems;
}

/** @brief One starformation_and_feedback_sequence fixture: every substep of the chain */
static int check_sequence(struct oracle_fixture *in, struct oracle_fixture *out, struct chain *c) {
  int problems = 0;
  /* The writer's units: the run's, which the finaliser reads from MimicConfig */
  if (oracle_f64_to_bits(MimicConfig.UnitMass_in_g) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.UnitMass_in_g")) ||
      oracle_f64_to_bits(MimicConfig.UnitTime_in_s) !=
          oracle_f64_to_bits(oracle_in_f64(in, "params.UnitTime_in_s"))) {
    fprintf(stderr,
            "oracle: %s: the run's UnitMass_in_g or UnitTime_in_s differs from the "
            "writer's\n",
            in->path);
    problems++;
  }

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
    problems += oracle_check_i32(out, field, mimic_sage_sfr_step_bin(step, c->effective_steps));
  }

  /* After the last substep, the post_timestep finaliser on the whole group: SubstepsUsed
   * on every galaxy and the writer's averages into the four output scalars */
  if (sage26_finalize_sfr_histories_process(&c->ctx, c->halos, c->ngal) != 0) {
    problems++;
  }
  problems += check_end_state(out, c);
  for (int i = 0; i < c->ngal; i++) {
    problems +=
        oracle_check_i32(out, oracle_galaxy_field(i, "SubstepsUsed"), c->galaxies[i].SubstepsUsed);
    /* The writer's divisors and skipped-bin counts are the helper's locals, which
     * process() does not keep: recompute them on a copy, which must also reproduce
     * process()'s four outputs */
    struct GalaxyData copy = c->galaxies[i];
    struct MimicSageSfrAverageLocals locals;
    mimic_sage_average_sfr_histories(&copy, MimicConfig.UnitMass_in_g, MimicConfig.UnitTime_in_s,
                                     &locals);
    if (oracle_f32_to_bits(copy.SfrDisk) != oracle_f32_to_bits(c->galaxies[i].SfrDisk) ||
        oracle_f32_to_bits(copy.SfrBulge) != oracle_f32_to_bits(c->galaxies[i].SfrBulge) ||
        oracle_f32_to_bits(copy.SfrDiskZ) != oracle_f32_to_bits(c->galaxies[i].SfrDiskZ) ||
        oracle_f32_to_bits(copy.SfrBulgeZ) != oracle_f32_to_bits(c->galaxies[i].SfrBulgeZ)) {
      fprintf(stderr, "oracle: %s: the averaging helper differs from process()\n", in->path);
      problems++;
    }
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.writer.galaxy[%d].", i);
    problems += oracle_c05_check_writer(out, prefix, &c->galaxies[i], &locals);
  }
  return problems;
}

/** @brief One calculate_molecular_fraction_BR06 fixture, called directly (oracle_c08) */
static int check_br06(struct oracle_fixture *in, struct oracle_fixture *out) {
  const float gas = oracle_in_f32(in, "call.gas_surface_density_msun_pc2");
  const float stars = oracle_in_f32(in, "call.stellar_surface_density_msun_pc2");
  const float rs = oracle_in_f32(in, "call.disk_scale_length_pc");

  struct MimicSageBr06Locals l;
  const float f_mol = mimic_sage_br06_molecular_fraction(gas, stars, rs, &l);

  int problems = oracle_check_f32(out, "return.f_mol", f_mol);
  problems += oracle_c06_check_flag(out, "scale_length_nonpositive", l.scale_length_nonpositive);
  problems += oracle_c06_check_flag(out, "pressure_branch", l.pressure_branch);
  problems += oracle_c06_check_flag(out, "stellar_floor_binds", l.stellar_floor_binds);
  problems += oracle_c06_check_flag(out, "pressure_nonpositive", l.pressure_nonpositive);
  problems += oracle_c08_check_local(out, "log_h_star", l.log_h_star, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "h_star_pc", l.h_star_pc, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "effective_sigma_stars", l.effective_sigma_stars, 1, 0,
                                     &h2_tally);
  problems += oracle_c08_check_local(out, "pressure", l.pressure, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "pressure_ratio", l.pressure_ratio, 1, 0, &h2_tally);
  problems += oracle_c08_check_local(out, "R_mol", l.R_mol, 1, 0, &h2_tally);
  /* f_mol reaches the float return only through its narrowing */
  problems += oracle_c08_check_local(out, "f_mol", l.f_mol, 0, 1, &h2_tally);
  return problems;
}

/**
 * @brief One calculate_molecular_fraction_radial_integration fixture, called directly as
 *        sfr_br06 calls it (oracle_c08)
 */
static int check_radial(struct oracle_fixture *in, struct oracle_fixture *out) {
  struct oracle_c08_sf_config cfg;
  int problems = oracle_c08_read_sf_config(in, 1, &cfg);
  if (cfg.sf_prescription != MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    fprintf(stderr, "oracle: %s: the radial integration is supported only with SFprescription 1\n",
            in->path);
    return problems + 1;
  }
  const double hubble_h = oracle_in_f64(in, "params.Hubble_h");

  struct GalaxyData gal;
  oracle_poison(&gal, sizeof(gal));
  gal.DiskScaleRadius = oracle_in_f32(in, "galaxy[0].DiskScaleRadius");
  gal.ColdGas = oracle_in_f32(in, "galaxy[0].ColdGas");
  gal.MetalsColdGas = oracle_in_f32(in, "galaxy[0].MetalsColdGas");
  gal.StellarMass = oracle_in_f32(in, "galaxy[0].StellarMass");
  gal.BulgeMass = oracle_in_f32(in, "galaxy[0].BulgeMass");
  gal.H2gas = oracle_in_f32(in, "galaxy[0].H2gas");
  gal.H2DepletionTime_Gyr = oracle_in_f32(in, "galaxy[0].H2DepletionTime_Gyr");
  struct GalaxyData before = gal;

  static struct MimicSageH2RadialLocals l;
  const float h2 = mimic_sage_h2_radial_integration(&gal, hubble_h, cfg.h2_radial_nbins,
                                                    cfg.h2_radial_rmax_factor, &l);

  /* The helper writes H2gas and nothing else of the galaxy (R/src/model_h2_chemistry.c:288) */
  before.H2gas = gal.H2gas;
  if (memcmp(&before, &gal, sizeof(gal)) != 0) {
    fprintf(stderr, "oracle: %s: the radial integration wrote a member other than H2gas\n",
            in->path);
    problems++;
  }
  problems += oracle_check_f32(out, "return.H2_code_units", h2);
  problems += oracle_check_f32(out, "galaxy[0].H2gas", gal.H2gas);
  problems += oracle_check_f32(out, "galaxy[0].H2DepletionTime_Gyr", gal.H2DepletionTime_Gyr);
  problems += oracle_c06_check_flag(out, "returned_early", l.returned_early);
  problems += oracle_c06_check_flag(out, "disk_stars_positive", l.disk_stars_positive);

  const struct {
    const char *name;
    float value;
  } scalars[] = {{"h", l.h},
                 {"rs_pc", l.rs_pc},
                 {"M_gas_total", l.M_gas_total},
                 {"M_disk_star", l.M_disk_star},
                 {"sigma_gas_0", l.sigma_gas_0},
                 {"sigma_star_0", l.sigma_star_0},
                 {"R_MAX", l.R_MAX},
                 {"dr", l.dr},
                 {"log_h_star", l.log_h_star},
                 {"h_star_br06", l.h_star_br06}};
  for (size_t k = 0; k < COUNT(scalars); k++) {
    problems += oracle_c08_check_local(out, scalars[k].name, scalars[k].value, 1, 0, &h2_tally);
  }
  for (int i = 0; i < MIMIC_SAGE_H2_RADIAL_NBINS; i++) {
    const struct MimicSageH2RadialBin *b = &l.bin[i];
    char name[ORACLE_MAX_NAME];
    snprintf(name, sizeof(name), "bin[%d].skipped", i);
    problems += oracle_c06_check_flag(out, name, b->skipped);
    snprintf(name, sizeof(name), "bin[%d].stellar_floor_binds", i);
    problems += oracle_c06_check_flag(out, name, b->stellar_floor_binds);
    const struct {
      const char *name;
      float value;
    } bin_values[] = {{"r", b->r},
                      {"exp_factor", b->exp_factor},
                      {"sigma_gas_r", b->sigma_gas_r},
                      {"sigma_star_r", b->sigma_star_r},
                      {"f_mol_r", b->f_mol_r},
                      {"dM_H2", b->dM_H2},
                      {"M_H2_total", b->M_H2_total}};
    for (size_t k = 0; k < COUNT(bin_values); k++) {
      snprintf(name, sizeof(name), "bin[%d].%s", i, bin_values[k].name);
      problems += oracle_c08_check_local(out, name, bin_values[k].value, 1, 0, &h2_tally);
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
  /* The two public H2 routines, called directly (oracle_c08) */
  static const char *const H2_ROUTINES[] = {"calculate_molecular_fraction_BR06",
                                            "calculate_molecular_fraction_radial_integration"};
  for (size_t k = 0; k < COUNT(H2_ROUTINES); k++) {
    if (strcmp(in.routine, H2_ROUTINES[k]) == 0) {
      if (oracle_load_pair(MODULE, name, H2_ROUTINES[k], &in, &out) != 0) {
        return 1;
      }
      int problems = k == 0 ? check_br06(&in, &out) : check_radial(&in, &out);
      problems += oracle_report_unconsumed(&in);
      problems += oracle_report_unconsumed(&out);
      return problems;
    }
  }
  const int sequence = strcmp(in.routine, "starformation_and_feedback_sequence") == 0;
  const int fire = strcmp(in.routine, ORACLE_C09_ROUTINE) == 0;
  if (oracle_load_pair(MODULE, name,
                       sequence ? "starformation_and_feedback_sequence"
                       : fire   ? ORACLE_C09_ROUTINE
                                : "starformation_and_feedback",
                       &in, &out) != 0) {
    return 1;
  }

  static struct chain c;
  c.fire = fire;
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
  memset(&h2_tally, 0, sizeof(h2_tally));
  memset(&fire_tally, 0, sizeof(fire_tally));
  continuations_br06 = 0;
  fire_continuations = 0;
  fire_wiring_asserted = 0;
  fire_nested_observed = 0;
  fire_mass_loading_kept = 0;
  cgm_fixtures = 0;
  cgm_sites_asserted = 0;
  cgm_sites_cgm = 0;
  cgm_wind_sites = 0;
  cgm_burst_sites = 0;
  cgm_unselected_pairs = 0;
  cgm_wind_values = 0;
  cgm_wind_values_validated = 0;
  cgm_observed_host_sensitive = 0;
  cgm_observed_other_insensitive = 0;
  const int ok = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: carried values asserted %d (validated two-sided), not validated %d, "
         "not package values %d, absent %d; binding clamps and caps asserted equal to their "
         "reservoir %d ",
         tally.asserted, tally.not_validated, tally.not_carried, tally.absent, tally.binding);
  printf("\n  oracle: c08 locals and values compared bit for bit %d (validated two-sided %d, "
         "narrowing-limited %d, not validated %d), absent %d; binding caps asserted %d ",
         h2_tally.validated + h2_tally.narrowing_limited + h2_tally.not_validated,
         h2_tally.validated, h2_tally.narrowing_limited, h2_tally.not_validated, h2_tally.absent,
         h2_tally.binding);
  printf("\n  oracle: SFprescription 1 gas-unstable continuations through the quasar and burst "
         "modules %d ",
         continuations_br06);
  printf("\n  oracle: c09 fixtures FIREmodeOn 1 %d, FIREmodeOn 0 %d; FIRE values compared bit for "
         "bit %d (continuation-checked %d, tail-checked %d; validated two-sided %d, "
         "narrowing-limited %d, not validated %d), absent %d; binding caps asserted %d ",
         fire_tally.fire_on, fire_tally.fire_off,
         fire_tally.validated + fire_tally.narrowing_limited + fire_tally.not_validated,
         fire_tally.continuation, fire_tally.tail, fire_tally.validated,
         fire_tally.narrowing_limited, fire_tally.not_validated, fire_tally.absent,
         fire_tally.binding);
  printf("\n  oracle: c09 gas-unstable continuations %d: instability-burst FIRE wiring asserted %d "
         "(SAGE26's burst observed FIRE- and redshift-sensitive on %d), the disk MassLoading kept "
         "through the burst %d ",
         fire_continuations, fire_wiring_asserted, fire_nested_observed, fire_mass_loading_kept);
  printf(
      "\n  oracle: c10 CGMrecipeOn 1 fixtures %d: routing sites asserted against the oracle and "
      "the expected galaxy's Regime %d (CGM pair %d, hot pair %d), of which instability winds "
      "%d and instability bursts %d (commit and yield each); unselected pairs asserted unchanged "
      "%d; wind values compared bit for bit %d (validated two-sided %d); SAGE26's nested "
      "routines observed routing by the expected galaxy %d, insensitive to the other's Regime %d ",
      cgm_fixtures, cgm_sites_asserted, cgm_sites_cgm, cgm_sites_asserted - cgm_sites_cgm,
      cgm_wind_sites, cgm_burst_sites, cgm_unselected_pairs, cgm_wind_values,
      cgm_wind_values_validated, cgm_observed_host_sensitive, cgm_observed_other_insensitive);
  TEST_ASSERT(ok, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(cgm_wind_sites >= 1 && cgm_sites_cgm >= 1 && cgm_sites_asserted - cgm_sites_cgm >= 1,
              "CGMrecipeOn 1 fixtures selecting each pair, with an instability continuation, must "
              "be asserted");
  TEST_ASSERT(fire_continuations >= 1 && fire_wiring_asserted == fire_continuations &&
                  fire_nested_observed >= 1 && fire_mass_loading_kept == fire_continuations,
              "every c09 gas-unstable continuation must have its burst's FIRE wiring asserted and "
              "keep the disk MassLoading, and SAGE26's burst must be observed FIRE-sensitive");
  TEST_ASSERT(continuations_br06 >= 1,
              "an SFprescription 1 gas-unstable continuation must be asserted");
  TEST_ASSERT(fire_tally.fire_on >= 1 && fire_tally.fire_off >= 1,
              "c09 fixtures with FIREmodeOn 1 and 0 must be asserted");
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
