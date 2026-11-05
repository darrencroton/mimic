/**
 * @file    test_unit_sage26_disk_instability_oracle.c
 * @brief   Bit-exact comparison of sage26_disk_instability with SAGE26's check_disk_instability
 *          and update_instability_bulge_radius
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_disk_instability
 * (its _tests/oracle/ directory), dispatching on the fixture's routine:
 *
 * check_disk_instability (R/src/model_disk_instability.c:47-108) on galaxy call.p, with R0's
 * flags and BulgeSizeOn 0 (R0) or 3 (the c07 di_s3_* fixtures, R1.par:109). The test
 * runs the module's process() on that galaxy and asserts bit for bit, on every galaxy
 * of the fixture, the fourteen fields the c05 oracle records (OutflowRate being the
 * package's SupernovaOutflowRate) and BlackHoleMass and QuasarModeBHaccretionMass, and,
 * with BulgeSizeOn 3, MergerBulgeMass and the three bulge radii, which the oracle then
 * records too (the instability's radius update, D:83, R/src/model_misc.c:385-424):
 *
 * - intermediate.state_is_post_transfer 0 (a stable disk, a non-positive disk mass, or
 *   an unstable disk with no cold gas): SAGE26's own end state, which is every field
 *   check_disk_instability writes on these paths (BulgeMass, MetalsBulgeMass and
 *   InstabilityBulgeMass) and every other recorded field unchanged.
 * - intermediate.state_is_post_transfer 1 (a gas-unstable disk): the state after the
 *   stellar transfer (:54-84) and before grow_black_hole and collisional_starburst_recipe
 *   (:102-108), from the oracle's transcription, which the oracle checks against
 *   SAGE26's own routine by composing it with SAGE26's continuation. The test then runs
 *   the package's continuation of the gas branch end to end, as a run configures it:
 *   the module hands the fraction on through UnstableDiskGasFraction, sage26_quasar_mode
 *   grows the black hole with its quasar wind and sage26_starburst_feedback bursts it
 *   (both process_by_galaxy, in that order, R/src/model_disk_instability.c:101-108).
 *   SAGE26's full end state, intermediate.c06.*, is asserted bit for bit on every galaxy:
 *   the sixteen recorded fields and the sixty history elements.
 *
 * The module must also leave every other member of every galaxy as it was. With BulgeSizeOn 0
 * that includes the bulge radii (update_instability_bulge_radius returns at once,
 * R/src/model_misc.c:395): the test sets them to non-zero values and compares the whole
 * struct, apart from the four members the module writes. With BulgeSizeOn 3 the radii are
 * the fixture's and are asserted against the oracle; every other member is compared.
 *
 * Carried values. The shared helper reports SAGE26's locals
 * (mimic_sage_check_disk_instability); its replay on a copy must end in exactly the
 * state process() produced. Each local's presence must match the oracle's _absent
 * marker. diskmass, Mcrit, unstable_gas, unstable_stars and instability_metallicity are
 * asserted where the oracle validates them two-sided; where Mcrit is capped
 * (instability_mcrit_capped) it must equal diskmass. unstable_gas_fraction, the value
 * handed to sage26_quasar_mode and sage26_starburst_feedback through
 * UnstableDiskGasFraction, is asserted bit for bit wherever the gas branch is taken: it
 * pins the oracle's checked transcription of :101. No fixture validates it two-sided
 * (SAGE26's float stores downstream absorb a one-ulp change), so the assertion is
 * against the checked value, not a two-sided validated one. Where the gas branch is not
 * taken the module must hand on 0.
 *
 * The continuation's inputs come from the fixture: call.step_bin is the substep's history
 * bin (ten substeps, substep call.step_bin) and call.dt the substep interval (each halo's
 * dT is the double whose tenth is call.dt bit for bit); the c06 parameters, Mvir and Vvir
 * are applied, and the package's EnergySNcode and EtaSNcode, derived from SAGE26's R0
 * unit factors, must equal the fixture's. Every galaxy member the chain does not write
 * must be unchanged; the package's StarFormationRate diagnostic, which the burst writes
 * and SAGE26 does not hold, is not compared.
 *
 * Not asserted, each named: the branch flags (read only to key the Mcrit binding); the
 * post_transfer.* validation markers (read and required consistent); the six per-substep
 * histories of the instability's own recorded state, which check_disk_instability does
 * not write before its burst call (required unchanged by the whole-struct check);
 * call.time, which the R0 continuation does not read; and SfrEfficiency, which no
 * statement of the routine or its continuation reads.
 *
 * update_instability_bulge_radius (R/src/model_misc.c:385-424, uibr_*, oracle_c07): the
 * shared helper mimic_sage_update_instability_bulge_radius (shared/sage_bulge_size_physics.h)
 * called directly with the fixture's call.delta_mass and call.old_disk_radius. Asserted bit
 * for bit: the six-member size block, every other galaxy member unchanged, the two branch
 * flags and every local the oracle records, each named by the oracle as narrowing-limited
 * (its only path to the state is one float store, so it is validated in neither direction).
 *
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports
 * SKIP.
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
#include "modules/_tests/sage26_oracle_sizes.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_disk_instability_physics.h"
#include "shared/sage_sf_feedback_physics.h"

extern int sage26_disk_instability_init(void);
extern int sage26_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_quasar_mode_init(void);
extern int sage26_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_starburst_feedback_init(void);
extern int sage26_starburst_feedback_process(struct ModuleContext *ctx, struct Halo *halos,
                                             int ngal);

static const char *MODULE = "sage26_disk_instability";

#define MAX_GALAXIES 4
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* SAGE26's TOOMRE_DISK_FACTOR (R/src/model_disk_instability.c:35) */
#define SAGE26_TOOMRE_DISK_FACTOR 3.0

/* SAGE26's R0 unit factors (R0.par:105-107), from which the burst module derives
 * EnergySNcode and EtaSNcode (R/src/core_init.c:102-103) */
#define SAGE26_UNIT_LENGTH_IN_CM 3.08568e+24
#define SAGE26_UNIT_MASS_IN_G 1.989e+43

/* The continuation's substeps: the fixture's step_bin is substep step_bin of ten */
#define CHAIN_SUBSTEPS 10

/* Non-zero bulge radii the module must leave as they were (Mpc/h) */
#define TEST_BULGE_RADIUS 0.25f
#define TEST_MERGER_BULGE_RADIUS 0.125f
#define TEST_INSTABILITY_BULGE_RADIUS 0.0625f

static struct oracle_c05_carried_tally tally;
static int handed_fraction_asserted = 0;
static int end_to_end_asserted = 0;
static int sizes_asserted = 0;
static int update_fixtures = 0;
static struct oracle_c07_local_tally local_tally;

/** @brief The members BulgeSizeOn 3 adds to every c05 galaxy (oracle README "The c07 Group") */
static const char *const SIZES[] = {"MergerBulgeMass", "BulgeRadius", "MergerBulgeRadius",
                                    "InstabilityBulgeRadius"};

/** @brief The galaxy fields the routine records beyond the fourteen, unchanged before c06 */
static const char *const UNTOUCHED[] = {"BlackHoleMass", "QuasarModeBHaccretionMass"};

/** @brief The branch flags every output records */
static const char *const FLAGS[] = {"instability_disk_positive", "instability_mcrit_capped",
                                    "instability_stars_transferred", "instability_gas_branch"};

/** @brief The fields whose post-transfer value the oracle validates on gas-unstable fixtures:
 *         the three masses, and the three radii with BulgeSizeOn 3 */
static const char *const POST_TRANSFER[] = {
    "BulgeMass",   "MetalsBulgeMass",   "InstabilityBulgeMass",
    "BulgeRadius", "MergerBulgeRadius", "InstabilityBulgeRadius"};
#define POST_TRANSFER_R0 3

/**
 * @brief Apply the fixture's flags and parameters, then initialise the module
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in, int *bulge_size_on) {
  int problems = 0;
  reset_config();

  /* R0's fixed flags (oracle README "Flags and Parameters") */
  problems += oracle_c05_require_flag(in, "params.SFprescription", 0);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  problems += oracle_c05_require_flag(in, "params.FIREmodeOn", 0);
  problems += oracle_c05_require_flag(in, "params.EnhancedStarFormationOn", 0);
  /* BulgeSizeOn 0 (R0) or 3 (R1.par:109); c05 fixtures carry Hubble_h in both modes */
  *bulge_size_on = oracle_in_i32(in, "params.BulgeSizeOn");
  if (*bulge_size_on != MIMIC_SAGE_BULGE_SIZE_OFF &&
      *bulge_size_on != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    fprintf(stderr, "oracle: %s: params.BulgeSizeOn %d is neither 0 nor 3\n", in->path,
            *bulge_size_on);
    problems++;
  }
  problems += oracle_c05_require_flag(in, "params.KarpovModeOn", 0);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);

  /* The core-owned gravitational constant, at SAGE26's bit pattern (R/src/core_init.c:96) */
  MimicConfig.G = oracle_in_f64(in, "params.G");
  /* Read by the routine only in update_instability_bulge_radius past its BulgeSizeOn 3 test
   * (R/src/model_misc.c:395-398), which R0 never passes; the burst module derives its
   * supernova constants from it */
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");

  /* SAGE26's R0 unit factors, for the quasar wind and the burst's supernova constants */
  MimicConfig.UnitLength_in_cm = SAGE26_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = SAGE26_UNIT_MASS_IN_G;
  MimicConfig.UnitVelocity_in_cm_per_s = oracle_in_f64(in, "params.UnitVelocity_in_cm_per_s");
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g *
                                  (MimicConfig.UnitLength_in_cm * MimicConfig.UnitLength_in_cm) /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
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

  /* Read by no statement of the routine or its continuation on R0 */
  (void)oracle_in_f64(in, "params.SfrEfficiency");

  /* The continuation's parameters, at the fixture's bit patterns; SAGE26's ThreshMajorMerger
   * is the package's ThresholdMajorMerger */
  const struct {
    const char *sage26;
    const char *package;
  } continuation[] = {{"FeedbackReheatingEpsilon", "FeedbackReheatingEpsilon"},
                      {"FeedbackEjectionEfficiency", "FeedbackEjectionEfficiency"},
                      {"RecycleFraction", "RecycleFraction"},
                      {"Yield", "Yield"},
                      {"FracZleaveDisk", "FracZleaveDisk"},
                      {"ThreshMajorMerger", "ThresholdMajorMerger"},
                      {"BlackHoleGrowthRate", "BlackHoleGrowthRate"},
                      {"QuasarModeEfficiency", "QuasarModeEfficiency"}};
  for (size_t k = 0; k < COUNT(continuation); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", continuation[k].sage26);
    if (oracle_set_model_param(continuation[k].package, oracle_in_f64(in, field)) != 0) {
      problems++;
    }
  }

  if (oracle_set_model_param("StarFormingDiskFactor", SAGE26_TOOMRE_DISK_FACTOR) != 0 ||
      oracle_set_model_param("BulgeSizeOn", (double)*bulge_size_on) != 0) {
    problems++;
  }
  /* sage26_starburst_feedback's required SFprescription and StarburstColdGasOn, at R1's values
   * (0, the commits' hydrogen caps off, as these fixtures' SFprescription 0; 1, cold-gas bursts) */
  if (oracle_set_model_param("SFprescription", 0.0) != 0 ||
      oracle_set_model_param("StarburstColdGasOn", 1.0) != 0) {
    problems++;
  }
  if (sage26_disk_instability_init() != 0) {
    fprintf(stderr, "oracle: %s: sage26_disk_instability_init() failed\n", in->path);
    problems++;
  }
  if (sage26_quasar_mode_init() != 0 || sage26_starburst_feedback_init() != 0) {
    fprintf(stderr, "oracle: %s: the continuation's modules failed to initialise\n", in->path);
    problems++;
  }
  return problems;
}

/**
 * @brief The halo dT whose tenth is the fixture's substep interval bit for bit
 *
 * The burst module forms its dt as halo->dT / num_substeps (shared/time_parity.h); SAGE26's
 * dt is deltaT / effective_steps (R/src/core_build_model.c:516). Searches the doubles
 * nearest call.dt * 10.
 *
 * @return 0 and the interval in `dT`, or 1 if no nearby double divides to call.dt exactly
 */
static int chain_halo_dT(const double dt, double *dT) {
  double candidate = dt * CHAIN_SUBSTEPS;
  for (int k = 0; k < 4; k++) {
    candidate = nextafter(candidate, -INFINITY);
  }
  for (int k = 0; k < 9; k++) {
    if (oracle_f64_to_bits(candidate / (double)CHAIN_SUBSTEPS) == oracle_f64_to_bits(dt)) {
      *dT = candidate;
      return 0;
    }
    candidate = nextafter(candidate, INFINITY);
  }
  return 1;
}

/**
 * @brief Build the galaxies and their halos from the fixture
 * @return Number of problems
 */
static int setup_galaxies(struct oracle_fixture *in, const int ngal, const int sizes_on,
                          struct GalaxyData galaxies[MAX_GALAXIES],
                          struct Halo halos[MAX_GALAXIES]) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *g = &galaxies[i];
    struct Halo *h = &halos[i];
    oracle_poison(g, sizeof(*g));
    oracle_poison(h, sizeof(*h));
    h->galaxy = g;
    h->HaloNr = i;
    h->Type = oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    /* SAGE26's float Vmax: the package's halo Vmax is the same float */
    h->Vmax = oracle_in_f32(in, oracle_galaxy_field(i, "Vmax"));
    /* Read only by the continuation: the quasar wind's and the burst's ejection Vvir and
     * the yield's FoF-central Mvir, SAGE26's floats exactly widened */
    h->Mvir = oracle_in_f32(in, oracle_galaxy_field(i, "Mvir"));
    h->Vvir = oracle_in_f32(in, oracle_galaxy_field(i, "Vvir"));

    const char *const extra[] = {"DiskScaleRadius", "BlackHoleMass", "QuasarModeBHaccretionMass"};
    problems += oracle_c05_in_floats(in, i, g, extra, COUNT(extra));
    problems += oracle_c05_in_floats(in, i, g, written, count);
    /* The six histories: written only by the continuation's burst */
    problems += oracle_c05_in_histories(in, i, g);

    if (sizes_on) {
      /* BulgeSizeOn 3: the merger channel and the three radii are the fixture's */
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        *oracle_c06_galaxy_float(g, SIZES[k]) = oracle_in_f32(in, oracle_galaxy_field(i, SIZES[k]));
      }
    } else {
      g->BulgeRadius = TEST_BULGE_RADIUS;
      g->MergerBulgeRadius = TEST_MERGER_BULGE_RADIUS;
      g->InstabilityBulgeRadius = TEST_INSTABILITY_BULGE_RADIUS;
    }
    g->UnstableDiskGasFraction = 0.0;
  }
  return problems;
}

/**
 * @brief Require every galaxy member the module does not write to be bit-identical to before
 * @return Number of galaxies with a changed member
 */
static int check_rest_unchanged(const char *path, const int ngal, const int sizes_on,
                                const struct GalaxyData before[MAX_GALAXIES],
                                const struct GalaxyData after[MAX_GALAXIES]) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData expected = before[i];
    expected.BulgeMass = after[i].BulgeMass;
    expected.MetalsBulgeMass = after[i].MetalsBulgeMass;
    expected.InstabilityBulgeMass = after[i].InstabilityBulgeMass;
    expected.UnstableDiskGasFraction = after[i].UnstableDiskGasFraction;
    if (sizes_on) {
      /* BulgeSizeOn 3: the radius update writes the three radii (asserted separately) */
      expected.BulgeRadius = after[i].BulgeRadius;
      expected.MergerBulgeRadius = after[i].MergerBulgeRadius;
      expected.InstabilityBulgeRadius = after[i].InstabilityBulgeRadius;
    }
    if (memcmp(&expected, &after[i], sizeof(expected)) != 0) {
      fprintf(stderr,
              "oracle: %s: galaxy[%d] changed a member other than BulgeMass, MetalsBulgeMass, "
              "InstabilityBulgeMass and UnstableDiskGasFraction\n",
              path, i);
      problems++;
    }
  }
  return problems;
}

/**
 * @brief Assert one of SAGE26's instability locals where validated, its presence always
 * @return Number of mismatches and marker errors
 */
static int check_local(struct oracle_fixture *out, const char *name, const double *actual) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != (isnan(*actual) ? 1 : 0)) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", isnan(*actual) ? "absent" : "present");
    return 1;
  }
  return oracle_c05_check_carried(out, "intermediate.", name, actual, &tally);
}

/**
 * @brief Assert the handed unstable_gas_fraction against the checked transcription
 *
 * Present (the gas branch is taken): the oracle's value, checked by composition and
 * validated two-sided on no fixture, must equal both the helper's local and the
 * UnstableDiskGasFraction the module hands on, bit for bit. Absent: the module hands
 * on 0.
 *
 * @return Number of mismatches and marker errors
 */
static int check_handed_fraction(struct oracle_fixture *out, const double local,
                                 const double handed) {
  int problems = 0;
  const int absent = oracle_in_i32(out, "intermediate.unstable_gas_fraction_absent");
  if (absent) {
    if (!isnan(local) || oracle_f64_to_bits(handed) != oracle_f64_to_bits(0.0)) {
      fprintf(stderr, "oracle: %s: no gas branch, yet the port formed or handed on a fraction\n",
              out->path);
      problems++;
    }
    return problems;
  }
  if (oracle_in_i32(out, "intermediate.unstable_gas_fraction_checked") != 1) {
    fprintf(stderr, "oracle: %s: unstable_gas_fraction is not marked checked\n", out->path);
    problems++;
  }
  const int up = oracle_in_i32(out, "intermediate.unstable_gas_fraction_validated_up");
  const int down = oracle_in_i32(out, "intermediate.unstable_gas_fraction_validated_down");
  if (oracle_in_i32(out, "intermediate.unstable_gas_fraction_validated") != (up && down)) {
    fprintf(stderr, "oracle: %s: unstable_gas_fraction_validated disagrees with its directions\n",
            out->path);
    problems++;
  }
  problems += oracle_check_f64(out, "intermediate.unstable_gas_fraction", local);
  problems += oracle_check_f64(out, "intermediate.unstable_gas_fraction", handed);
  handed_fraction_asserted++;
  return problems;
}

/**
 * @brief Read the post-transfer validation markers, requiring each to be checked and consistent
 * @return Number of marker errors
 */
static int check_post_transfer_markers(struct oracle_fixture *out, const int state_post,
                                       const int sizes_on) {
  int problems = 0;
  const int absent = oracle_in_i32(out, "intermediate.post_transfer_absent");
  if (absent != !state_post) {
    fprintf(stderr, "oracle: %s: post_transfer_absent disagrees with state_is_post_transfer\n",
            out->path);
    return 1;
  }
  if (absent) {
    return 0;
  }
  const size_t post_count = sizes_on ? COUNT(POST_TRANSFER) : POST_TRANSFER_R0;
  for (size_t k = 0; k < post_count; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.post_transfer.%s_checked", POST_TRANSFER[k]);
    problems += oracle_in_i32(out, field) != 1;
    snprintf(field, sizeof(field), "intermediate.post_transfer.%s_validated_up", POST_TRANSFER[k]);
    const int up = oracle_in_i32(out, field);
    snprintf(field, sizeof(field), "intermediate.post_transfer.%s_validated_down",
             POST_TRANSFER[k]);
    const int down = oracle_in_i32(out, field);
    snprintf(field, sizeof(field), "intermediate.post_transfer.%s_validated", POST_TRANSFER[k]);
    problems += oracle_in_i32(out, field) != (up && down);
  }
  if (problems != 0) {
    fprintf(stderr, "oracle: %s: a post_transfer marker is unchecked or inconsistent\n", out->path);
  }
  return problems;
}

/**
 * @brief Run the gas branch's continuation and assert SAGE26's full end state
 *
 * sage26_quasar_mode and then sage26_starburst_feedback (process_by_galaxy) on galaxy p,
 * which read the fraction the module handed on, as SAGE26's check_disk_instability calls
 * grow_black_hole and then collisional_starburst_recipe (R/src/model_disk_instability.c:
 * 101-108). Asserts intermediate.c06.* on every galaxy and every other member unchanged,
 * apart from the package's StarFormationRate diagnostic.
 *
 * @return Number of mismatches and errors
 */
static int check_end_to_end(const char *name, struct oracle_fixture *out, struct ModuleContext *ctx,
                            const int ngal, const int p, const int sizes_on,
                            struct GalaxyData galaxies[MAX_GALAXIES],
                            struct Halo halos[MAX_GALAXIES]) {
  int problems = 0;
  static struct GalaxyData post_transfer[MAX_GALAXIES];
  memcpy(post_transfer, galaxies, sizeof(post_transfer));

  if (sage26_quasar_mode_process(ctx, &halos[p], 1) != 0 ||
      sage26_starburst_feedback_process(ctx, &halos[p], 1) != 0) {
    fprintf(stderr, "oracle: %s: the continuation's process() failed\n", name);
    problems++;
  }

  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    problems += oracle_c05_check_floats(out, "intermediate.c06.", i, &galaxies[i], written, count);
    problems += oracle_c05_check_floats(out, "intermediate.c06.", i, &galaxies[i], UNTOUCHED,
                                        COUNT(UNTOUCHED));
    problems += oracle_c05_check_histories(out, "intermediate.c06.", i, &galaxies[i]);
    if (sizes_on) {
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        problems +=
            oracle_check_f32(out, oracle_prefixed_galaxy_field("intermediate.c06.", i, SIZES[k]),
                             *oracle_c06_galaxy_float(&galaxies[i], SIZES[k]));
      }
    }

    struct GalaxyData expected = post_transfer[i];
    for (size_t k = 0; k < count; k++) {
      *oracle_c05_galaxy_float(&expected, written[k]) =
          *oracle_c05_galaxy_float(&galaxies[i], written[k]);
    }
    for (size_t k = 0; k < COUNT(UNTOUCHED); k++) {
      *oracle_c05_galaxy_float(&expected, UNTOUCHED[k]) =
          *oracle_c05_galaxy_float(&galaxies[i], UNTOUCHED[k]);
    }
    for (size_t h = 0; h < (size_t)MIMIC_SFR_HISTORY_COUNT; h++) {
      for (int k = 0; k < ORACLE_C05_STEPS; k++) {
        *oracle_c05_history_element(&expected, h, k) =
            *oracle_c05_history_element(&galaxies[i], h, k);
      }
    }
    if (sizes_on) {
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        *oracle_c06_galaxy_float(&expected, SIZES[k]) =
            *oracle_c06_galaxy_float(&galaxies[i], SIZES[k]);
      }
    }
    /* The package's diagnostic, which SAGE26 does not hold */
    expected.StarFormationRate = galaxies[i].StarFormationRate;
    if (memcmp(&expected, &galaxies[i], sizeof(expected)) != 0) {
      fprintf(stderr,
              "oracle: %s: the continuation changed a member of galaxy[%d] SAGE26's "
              "does not write\n",
              name, i);
      problems++;
    }
  }
  end_to_end_asserted++;
  return problems;
}

/** @brief check_disk_instability through process(); returns the number of mismatches and errors */
static int check_instability(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "check_disk_instability", &in, &out) != 0) {
    return 1;
  }

  int bulge_size_on = MIMIC_SAGE_BULGE_SIZE_OFF;
  int problems = setup_parameters(&in, &bulge_size_on);
  const int sizes_on = bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI;
  const struct MimicSageBulgeSize sizes = {bulge_size_on, MimicConfig.Hubble_h};
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int p = oracle_in_i32(&in, "call.p");
  const int centralgal = oracle_in_i32(&in, "call.centralgal");
  if (ngal < 1 || ngal > MAX_GALAXIES || p < 0 || p >= ngal || centralgal < 0 ||
      centralgal >= ngal) {
    fprintf(stderr, "oracle: %s: call.ngal, call.p or call.centralgal out of range\n", name);
    return problems + 1;
  }
  /* The continuation's history bin and substep interval; the event time reaches no R0
   * statement of the continuation */
  (void)oracle_in_f64(&in, "call.time");
  const double dt = oracle_in_f64(&in, "call.dt");
  const int step_bin = oracle_in_i32(&in, "call.step_bin");
  double halo_dT = 0.0;
  if (step_bin < 0 || step_bin >= CHAIN_SUBSTEPS || chain_halo_dT(dt, &halo_dT) != 0) {
    fprintf(stderr, "oracle: %s: no substep of ten reproduces call.step_bin and call.dt\n", name);
    return problems + 1;
  }

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES], replay[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES], replay_halos[MAX_GALAXIES];
  problems += setup_galaxies(&in, ngal, sizes_on, galaxies, halos);
  if (problems != 0) {
    return problems;
  }
  for (int i = 0; i < ngal; i++) {
    halos[i].dT = halo_dT;
  }
  memcpy(before, galaxies, sizeof(before));

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halos[centralgal];
  ctx.num_substeps = CHAIN_SUBSTEPS;
  ctx.substep_number = step_bin;

  /* The shared helper on a copy, for SAGE26's locals */
  memcpy(replay, galaxies, sizeof(replay));
  memcpy(replay_halos, halos, sizeof(replay_halos));
  for (int i = 0; i < ngal; i++) {
    replay_halos[i].galaxy = &replay[i];
  }
  struct MimicSageInstabilityLocals locals;
  const double replay_fraction = mimic_sage_check_disk_instability_with_sizes(
      &replay_halos[p], &ctx, SAGE26_TOOMRE_DISK_FACTOR, &sizes, &locals);

  /* The module itself */
  if (sage26_disk_instability_process(&ctx, &halos[p], 1) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", name);
    problems++;
  }
  replay[p].UnstableDiskGasFraction = replay_fraction;
  if (memcmp(replay, galaxies, sizeof(struct GalaxyData) * (size_t)ngal) != 0) {
    fprintf(stderr, "oracle: %s: the helper's replay differs from process()\n", name);
    problems++;
  }

  /* The recorded state: SAGE26's end state, or the checked post-transfer state */
  const int state_post = oracle_in_i32(&out, "intermediate.state_is_post_transfer");
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    problems += oracle_c05_check_floats(&out, "", i, &galaxies[i], written, count);
    problems += oracle_c05_check_floats(&out, "", i, &galaxies[i], UNTOUCHED, COUNT(UNTOUCHED));
    oracle_c05_exclude_histories(&out, "", i);
    if (sizes_on) {
      for (size_t k = 0; k < COUNT(SIZES); k++) {
        problems += oracle_check_f32(&out, oracle_galaxy_field(i, SIZES[k]),
                                     *oracle_c06_galaxy_float(&galaxies[i], SIZES[k]));
      }
    }
  }
  if (sizes_on) {
    sizes_asserted++;
  }
  problems += check_rest_unchanged(name, ngal, sizes_on, before, galaxies);
  problems += check_post_transfer_markers(&out, state_post, sizes_on);

  /* SAGE26's locals */
  problems += check_local(&out, "diskmass", &locals.diskmass);
  problems += check_local(&out, "Mcrit", &locals.mcrit);
  problems += check_local(&out, "unstable_gas", &locals.unstable_gas);
  problems += check_local(&out, "unstable_stars", &locals.unstable_stars);
  problems += check_local(&out, "instability_metallicity", &locals.metallicity);
  problems += check_handed_fraction(&out, locals.unstable_gas_fraction,
                                    galaxies[p].UnstableDiskGasFraction);

  /* Where Mcrit is capped it equals the disk mass (the flag itself is not asserted) */
  if (oracle_in_i32(&out, "intermediate.instability_mcrit_capped") == 1) {
    tally.binding++;
    problems += oracle_check_f64(&out, "intermediate.Mcrit", locals.diskmass);
  }
  for (size_t k = 0; k < COUNT(FLAGS); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "intermediate.%s", FLAGS[k]);
    oracle_exclude(&out, field);
  }

  /* SAGE26's full end state after the gas branch's continuation, through the package's
   * continuation as a run configures it */
  if (state_post) {
    problems += check_end_to_end(name, &out, &ctx, ngal, p, sizes_on, galaxies, halos);
  }

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/**
 * @brief update_instability_bulge_radius (R/src/model_misc.c:385-424) through the shared helper
 * @return Number of mismatches and errors
 */
static int check_update(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "update_instability_bulge_radius", &in, &out) != 0) {
    return 1;
  }
  struct MimicSageBulgeSize sizes;
  int problems = oracle_c07_read_sizes(&in, &sizes);
  if (oracle_in_i32(&in, "call.ngal") != 1 || oracle_in_i32(&in, "call.p") != 0) {
    fprintf(stderr, "oracle: %s: call.ngal must be 1 and call.p 0\n", name);
    problems++;
  }
  const double delta_mass = oracle_in_f64(&in, "call.delta_mass");
  const double old_disk_radius = oracle_in_f64(&in, "call.old_disk_radius");

  struct GalaxyData galaxy, before;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_c07_in_size_block(&in, 0, &galaxy);
  before = galaxy;

  struct MimicSageInstabilityRadiusLocals l;
  mimic_sage_update_instability_bulge_radius(&galaxy, delta_mass, old_disk_radius, &sizes, &l);

  problems += oracle_c07_check_size_block(&out, 0, &galaxy);
  struct GalaxyData expected = before;
  expected.BulgeRadius = galaxy.BulgeRadius;
  expected.MergerBulgeRadius = galaxy.MergerBulgeRadius;
  expected.InstabilityBulgeRadius = galaxy.InstabilityBulgeRadius;
  if (memcmp(&expected, &galaxy, sizeof(expected)) != 0) {
    fprintf(stderr, "oracle: %s: the update changed a member other than the three radii\n", name);
    problems++;
  }
  problems += oracle_check_i32(&out, "intermediate.returned_early", l.returned_early);
  problems += oracle_check_i32(&out, "intermediate.incremental", l.incremental);
  const struct {
    const char *name;
    double value;
  } values[] = {{"M_old", l.M_old},
                {"R_old", l.R_old},
                {"R_disc_kpc", l.R_disc_kpc},
                {"R_new_contribution_kpc", l.R_new_contribution_kpc},
                {"R_new_contribution", l.R_new_contribution},
                {"R_old_kpc", l.R_old_kpc},
                {"M_new", l.M_new},
                {"R_new_kpc", l.R_new_kpc},
                {"R_new", l.R_new}};
  for (size_t k = 0; k < COUNT(values); k++) {
    problems += oracle_c07_check_local(&out, values[k].name, values[k].value, 1, &local_tally);
  }
  update_fixtures++;

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Dispatch one fixture by its routine */
static int check_fixture(const char *name) {
  if (strncmp(name, "uibr_", 5) == 0) {
    return check_update(name);
  }
  return check_instability(name);
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
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  handed_fraction_asserted = 0;
  end_to_end_asserted = 0;
  sizes_asserted = 0;
  update_fixtures = 0;
  memset(&local_tally, 0, sizeof(local_tally));
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: locals asserted %d (validated two-sided), not validated %d, absent %d; "
         "Mcrit caps asserted equal to the disk mass %d; handed unstable_gas_fraction asserted "
         "against the checked transcription %d; gas-unstable fixtures asserted end to end "
         "through the quasar and burst modules %d ",
         tally.asserted, tally.not_validated, tally.absent, tally.binding, handed_fraction_asserted,
         end_to_end_asserted);
  printf(
      "\n  oracle: check_disk_instability with BulgeSizeOn 3 %d; update_instability_bulge_radius "
      "%d, its locals validated %d, narrowing-limited %d, not validated %d, absent %d ",
      sizes_asserted, update_fixtures, local_tally.validated, local_tally.narrowing_limited,
      local_tally.not_validated, local_tally.absent);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(end_to_end_asserted == 9,
              "the nine gas-unstable fixtures (six R0, three BulgeSizeOn 3) must be asserted end "
              "to end");
  TEST_ASSERT(sizes_asserted >= 1 && update_fixtures >= 1,
              "the BulgeSizeOn 3 instability and the direct radius update must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_disk_instability SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
