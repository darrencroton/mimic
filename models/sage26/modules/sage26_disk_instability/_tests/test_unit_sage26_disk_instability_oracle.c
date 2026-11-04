/**
 * @file    test_unit_sage26_disk_instability_oracle.c
 * @brief   Bit-exact comparison of sage26_disk_instability with SAGE26's check_disk_instability
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_disk_instability
 * (its _tests/oracle/ directory): SAGE26's check_disk_instability
 * (R/src/model_disk_instability.c:47-108) on galaxy call.p, with R0's flags. The test
 * runs the module's process() on that galaxy and asserts bit for bit, on every galaxy
 * of the fixture, the fourteen fields the c05 oracle records (OutflowRate being the
 * package's SupernovaOutflowRate) and BlackHoleMass and QuasarModeBHaccretionMass:
 *
 * - intermediate.state_is_post_transfer 0 (a stable disk, a non-positive disk mass, or
 *   an unstable disk with no cold gas): SAGE26's own end state, which is every field
 *   check_disk_instability writes on these paths (BulgeMass, MetalsBulgeMass and
 *   InstabilityBulgeMass) and every other recorded field unchanged.
 * - intermediate.state_is_post_transfer 1 (a gas-unstable disk): the state after the
 *   stellar transfer (:54-84) and before grow_black_hole and collisional_starburst_recipe
 *   (:102-108), from the oracle's transcription, which the oracle checks against
 *   SAGE26's own routine by composing it with SAGE26's continuation. The continuation
 *   is the merger and burst port's; SAGE26's full end state, intermediate.c06.*, is
 *   recorded for that port and is not asserted here.
 *
 * The module must also leave every other member of every galaxy as it was, bulge radii
 * included (on R0, BulgeSizeOn 0, update_instability_bulge_radius returns at once,
 * R/src/model_misc.c:395): the test sets the radii to non-zero values and compares the
 * whole struct, apart from the four members the module writes.
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
 * Not asserted, each named: the branch flags (read only to key the Mcrit binding);
 * the post_transfer.* validation markers (read and required consistent); the six
 * per-substep histories, which check_disk_instability does not write before its burst
 * call and the package does not yet hold; and call.time, call.dt, call.step_bin,
 * Mvir, Vvir and the c06 parameters, read only by the continuation. Outside the oracle
 * environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
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
#include "shared/sage_disk_instability_physics.h"

extern int sage26_disk_instability_init(void);
extern int sage26_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

static const char *MODULE = "sage26_disk_instability";

#define MAX_GALAXIES 4
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* SAGE26's TOOMRE_DISK_FACTOR (R/src/model_disk_instability.c:35) */
#define SAGE26_TOOMRE_DISK_FACTOR 3.0

/* Non-zero bulge radii the module must leave as they were (Mpc/h) */
#define TEST_BULGE_RADIUS 0.25f
#define TEST_MERGER_BULGE_RADIUS 0.125f
#define TEST_INSTABILITY_BULGE_RADIUS 0.0625f

static struct oracle_c05_carried_tally tally;
static int handed_fraction_asserted = 0;

/** @brief The galaxy fields the routine records beyond the fourteen, unchanged before c06 */
static const char *const UNTOUCHED[] = {"BlackHoleMass", "QuasarModeBHaccretionMass"};

/** @brief The branch flags every output records */
static const char *const FLAGS[] = {"instability_disk_positive", "instability_mcrit_capped",
                                    "instability_stars_transferred", "instability_gas_branch"};

/** @brief The fields whose post-transfer value the oracle validates on gas-unstable fixtures */
static const char *const POST_TRANSFER[] = {"BulgeMass", "MetalsBulgeMass", "InstabilityBulgeMass"};

/**
 * @brief Apply the fixture's flags and parameters, then initialise the module
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in) {
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
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);

  /* The core-owned gravitational constant, at SAGE26's bit pattern (R/src/core_init.c:96) */
  MimicConfig.G = oracle_in_f64(in, "params.G");
  /* Read by SAGE26 only in update_instability_bulge_radius past its BulgeSizeOn 3 test
   * (R/src/model_misc.c:395-397), which R0 never passes */
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");

  /* Read only by the gas branch's continuation (grow_black_hole, quasar_mode_wind,
   * collisional_starburst_recipe), which belongs to the merger and burst port */
  const char *const continuation[] = {"SfrEfficiency",
                                      "FeedbackReheatingEpsilon",
                                      "FeedbackEjectionEfficiency",
                                      "EtaSNcode",
                                      "EnergySNcode",
                                      "RecycleFraction",
                                      "Yield",
                                      "FracZleaveDisk",
                                      "ThreshMajorMerger",
                                      "BlackHoleGrowthRate",
                                      "QuasarModeEfficiency",
                                      "UnitVelocity_in_cm_per_s"};
  for (size_t k = 0; k < COUNT(continuation); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", continuation[k]);
    (void)oracle_in_f64(in, field);
  }

  if (oracle_set_model_param("StarFormingDiskFactor", SAGE26_TOOMRE_DISK_FACTOR) != 0) {
    problems++;
  }
  if (sage26_disk_instability_init() != 0) {
    fprintf(stderr, "oracle: %s: sage26_disk_instability_init() failed\n", in->path);
    problems++;
  }
  return problems;
}

/**
 * @brief Build the galaxies and their halos from the fixture
 * @return Number of problems
 */
static int setup_galaxies(struct oracle_fixture *in, const int ngal,
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
    /* Read only by the continuation (the burst's feedback and the quasar wind) */
    (void)oracle_in_f32(in, oracle_galaxy_field(i, "Mvir"));
    (void)oracle_in_f32(in, oracle_galaxy_field(i, "Vvir"));

    const char *const extra[] = {"DiskScaleRadius", "BlackHoleMass", "QuasarModeBHaccretionMass"};
    problems += oracle_c05_in_floats(in, i, g, extra, COUNT(extra));
    problems += oracle_c05_in_floats(in, i, g, written, count);
    /* The six histories: written only by the burst (c06's), not yet package state */
    oracle_c05_exclude_histories(in, "", i);

    g->BulgeRadius = TEST_BULGE_RADIUS;
    g->MergerBulgeRadius = TEST_MERGER_BULGE_RADIUS;
    g->InstabilityBulgeRadius = TEST_INSTABILITY_BULGE_RADIUS;
    g->UnstableDiskGasFraction = 0.0;
  }
  return problems;
}

/**
 * @brief Require every galaxy member the module does not write to be bit-identical to before
 * @return Number of galaxies with a changed member
 */
static int check_rest_unchanged(const char *path, const int ngal,
                                const struct GalaxyData before[MAX_GALAXIES],
                                const struct GalaxyData after[MAX_GALAXIES]) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData expected = before[i];
    expected.BulgeMass = after[i].BulgeMass;
    expected.MetalsBulgeMass = after[i].MetalsBulgeMass;
    expected.InstabilityBulgeMass = after[i].InstabilityBulgeMass;
    expected.UnstableDiskGasFraction = after[i].UnstableDiskGasFraction;
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
static int check_post_transfer_markers(struct oracle_fixture *out, const int state_post) {
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
  for (size_t k = 0; k < COUNT(POST_TRANSFER); k++) {
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

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "check_disk_instability", &in, &out) != 0) {
    return 1;
  }

  int problems = setup_parameters(&in);
  const int ngal = oracle_in_i32(&in, "call.ngal");
  const int p = oracle_in_i32(&in, "call.p");
  const int centralgal = oracle_in_i32(&in, "call.centralgal");
  if (ngal < 1 || ngal > MAX_GALAXIES || p < 0 || p >= ngal || centralgal < 0 ||
      centralgal >= ngal) {
    fprintf(stderr, "oracle: %s: call.ngal, call.p or call.centralgal out of range\n", name);
    return problems + 1;
  }
  /* Read only by the continuation (the burst's history bin, interval and event time) */
  (void)oracle_in_f64(&in, "call.time");
  (void)oracle_in_f64(&in, "call.dt");
  (void)oracle_in_i32(&in, "call.step_bin");

  static struct GalaxyData galaxies[MAX_GALAXIES], before[MAX_GALAXIES], replay[MAX_GALAXIES];
  static struct Halo halos[MAX_GALAXIES], replay_halos[MAX_GALAXIES];
  problems += setup_galaxies(&in, ngal, galaxies, halos);
  if (problems != 0) {
    return problems;
  }
  memcpy(before, galaxies, sizeof(before));

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halos[centralgal];

  /* The shared helper on a copy, for SAGE26's locals */
  memcpy(replay, galaxies, sizeof(replay));
  memcpy(replay_halos, halos, sizeof(replay_halos));
  for (int i = 0; i < ngal; i++) {
    replay_halos[i].galaxy = &replay[i];
  }
  struct MimicSageInstabilityLocals locals;
  const double replay_fraction =
      mimic_sage_check_disk_instability(&replay_halos[p], &ctx, SAGE26_TOOMRE_DISK_FACTOR, &locals);

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
  }
  problems += check_rest_unchanged(name, ngal, before, galaxies);
  problems += check_post_transfer_markers(&out, state_post);

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

  /* SAGE26's full end state after the gas branch's continuation: the merger and burst
   * port's, recorded for it, not a c05 assertion */
  if (state_post) {
    for (int i = 0; i < ngal; i++) {
      for (size_t k = 0; k < count; k++) {
        oracle_exclude(&out, oracle_prefixed_galaxy_field("intermediate.c06.", i, written[k]));
      }
      for (size_t k = 0; k < COUNT(UNTOUCHED); k++) {
        oracle_exclude(&out, oracle_prefixed_galaxy_field("intermediate.c06.", i, UNTOUCHED[k]));
      }
      oracle_c05_exclude_histories(&out, "intermediate.c06.", i);
    }
  }

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
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  handed_fraction_asserted = 0;
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: locals asserted %d (validated two-sided), not validated %d, absent %d; "
         "Mcrit caps asserted equal to the disk mass %d; handed unstable_gas_fraction asserted "
         "against the checked transcription %d ",
         tally.asserted, tally.not_validated, tally.absent, tally.binding,
         handed_fraction_asserted);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
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
