/**
 * @file    test_unit_sage26_determine_regime_oracle.c
 * @brief   Bit-exact comparison of sage26_determine_regime with SAGE26's determine_and_store_regime
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_determine_regime
 * (models/sage26/modules/sage26_determine_regime/_tests/oracle/, written by oracle_c10) through
 * the module's process() entry with CGMrecipeOn 1 and the fixture's MShockMsun and Hubble_h,
 * and asserts bit-for-bit equality with the oracle output.
 *
 * SAGE26 draws its deviate from libc rand() (R/src/model_regimes.c:94); the oracle supplied an
 * explicit integer queue through its test-only rand(). The module's test seam
 * (sage26_determine_regime.h) stands in for the production keyed draw here: its k-th deviate
 * request returns (double)rand.queue[k] / (double)RAND_MAX with the oracle platform's RAND_MAX
 * (intermediate.RAND_MAX), exactly SAGE26's expression, so the package receives the deviates
 * SAGE26 received, in request order. The seam also reports which rows requested one. Per row
 * the test asserts Regime, whether the row was skipped, whether it requested a deviate and
 * which queue entry it took, and SAGE26's locals Mshock, mass_ratio, x, hot_fraction and the
 * deviate, plus the comparison's outcome (deviate below, or equal to, hot_fraction); and the
 * number of draws. The production keyed draw is tested separately
 * (shared/_tests/test_unit_sage_keyed_draw.c), never through these fixtures.
 *
 * A fixture row that SAGE26 skips (mergeType > 0) is a Type 3 row on the Mimic side, the
 * package's mark of a merged or disrupted row; a fixture whose skipped rows are not Type 3, or
 * whose Type 3 rows are not skipped, is refused as unrepresentable. Outside the oracle
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
#include "modules/sage26_determine_regime/sage26_determine_regime.h"

extern int sage26_determine_regime_init(void);
extern int sage26_determine_regime_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_determine_regime_cleanup(void);

static const char *MODULE = "sage26_determine_regime";

#define MAX_ROWS 32
#define MAX_QUEUE 32

/* The module's one pre_timestep entry, which its init() requires */
static char pre_timestep_name[] = "sage26_determine_regime";
static struct PhaseModuleConfig pre_timestep_entry[1];

/** The seam's state for one fixture: the queue and what the module requested */
struct queue_seam {
  int queue[MAX_QUEUE];
  int queue_length;
  double rand_max;
  int requests;                /* deviates requested so far */
  int request_index[MAX_ROWS]; /* queue entry each row took, -1 without one */
  int observed[MAX_ROWS];      /* 1 when the row was classified */
  struct Sage26RegimeRowTrace trace[MAX_ROWS];
  int problems;
};

/** @brief Request k gets SAGE26's deviate of queue entry k (R/src/model_regimes.c:94) */
static double queue_deviate(void *user, const struct Sage26RegimeRowTrace *row) {
  struct queue_seam *s = user;
  if (row->row < 0 || row->row >= MAX_ROWS || s->requests >= s->queue_length) {
    fprintf(stderr, "oracle: row %d requested deviate %d of a queue of %d\n", row->row,
            s->requests + 1, s->queue_length);
    s->problems++;
    return NAN;
  }
  s->request_index[row->row] = s->requests;
  return (double)s->queue[s->requests++] / s->rand_max;
}

static void queue_observe(void *user, const struct Sage26RegimeRowTrace *row) {
  struct queue_seam *s = user;
  if (row->row < 0 || row->row >= MAX_ROWS) {
    s->problems++;
    return;
  }
  s->observed[row->row] = 1;
  s->trace[row->row] = *row;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "determine_and_store_regime", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;

  /* Configuration: CGMrecipeOn 1 (R4) and the fixture's MShockMsun and Hubble_h */
  reset_config();
  pre_timestep_entry[0].module_name = pre_timestep_name;
  pre_timestep_entry[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep_entry[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep_entry;
  MimicConfig.num_pre_timestep = 1;
  MimicConfig.Hubble_h = oracle_in_f64(&in, "params.Hubble_h");
  if (oracle_set_model_param("CGMrecipeOn", 1.0) != 0 ||
      oracle_set_model_param("MShockMsun", oracle_in_f64(&in, "params.MShockMsun")) != 0 ||
      sage26_determine_regime_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return problems + 1;
  }

  /* The rows: SAGE26's float Mvir exactly widened to the core's double */
  const int ngal = oracle_in_i32(&in, "call.ngal");
  if (ngal < 1 || ngal > MAX_ROWS) {
    fprintf(stderr, "oracle: %s: call.ngal %d outside 1..%d\n", name, ngal, MAX_ROWS);
    return problems + 1;
  }
  static struct Halo halos[MAX_ROWS];
  static struct GalaxyData galaxies[MAX_ROWS];
  int skipped[MAX_ROWS];
  for (int i = 0; i < ngal; i++) {
    oracle_poison(&halos[i], sizeof(halos[i]));
    oracle_poison(&galaxies[i], sizeof(galaxies[i]));
    halos[i].galaxy = &galaxies[i];
    halos[i].Type = oracle_in_i32(&in, oracle_galaxy_field(i, "Type"));
    const int merge_type = oracle_in_i32(&in, oracle_galaxy_field(i, "mergeType"));
    halos[i].Mvir = oracle_in_f32(&in, oracle_galaxy_field(i, "Mvir"));
    halos[i].UniqueGalaxyID = 1000 + i; /* unread: the seam supplies every deviate */
    galaxies[i].Regime = oracle_in_i32(&in, oracle_galaxy_field(i, "Regime"));
    skipped[i] = merge_type > 0;
    if (skipped[i] != (halos[i].Type == 3)) {
      fprintf(stderr,
              "oracle: %s: galaxy[%d] has Type %d and mergeType %d; the package skips exactly "
              "its Type 3 rows, SAGE26 exactly its mergeType > 0 rows\n",
              name, i, halos[i].Type, merge_type);
      problems++;
    }
  }

  static struct queue_seam seam_state;
  memset(&seam_state, 0, sizeof(seam_state));
  seam_state.queue_length = oracle_in_i32(&in, "rand.queue_length");
  if (seam_state.queue_length < 0 || seam_state.queue_length > MAX_QUEUE) {
    fprintf(stderr, "oracle: %s: rand.queue_length outside 0..%d\n", name, MAX_QUEUE);
    return problems + 1;
  }
  for (int k = 0; k < seam_state.queue_length; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "rand.queue[%d]", k);
    seam_state.queue[k] = oracle_in_i32(&in, field);
  }
  /* The oracle platform's RAND_MAX, as SAGE26 divided by it */
  seam_state.rand_max = (double)oracle_in_i32(&out, "intermediate.RAND_MAX");
  for (int i = 0; i < MAX_ROWS; i++) {
    seam_state.request_index[i] = -1;
  }

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.snapshot_number = 63; /* unread: the seam supplies every deviate */
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];

  const struct Sage26RegimeSeam seam = {queue_deviate, queue_observe, &seam_state};
  sage26_determine_regime_set_test_seam(&seam);
  const int status = sage26_determine_regime_process(&ctx, halos, ngal);
  sage26_determine_regime_set_test_seam(NULL);
  sage26_determine_regime_cleanup();
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  if (status != 0) {
    fprintf(stderr, "oracle: %s: process() returned %d\n", name, status);
    problems++;
  }
  problems += seam_state.problems;

  problems += oracle_check_i32(&out, "intermediate.draws_consumed", seam_state.requests);
  for (int i = 0; i < ngal; i++) {
    char field[ORACLE_MAX_NAME];
    const struct Sage26RegimeRowTrace *t = &seam_state.trace[i];
    const int classified = seam_state.observed[i];
    problems += oracle_check_i32(&out, oracle_galaxy_field(i, "Regime"), galaxies[i].Regime);
#define ROW_FIELD(member)                                                                          \
  (snprintf(field, sizeof(field), "intermediate.galaxy[%d]." member, i), field)
    problems += oracle_check_i32(&out, ROW_FIELD("skipped"), !classified);
    problems += oracle_check_i32(&out, ROW_FIELD("drew"), classified ? t->drew : 0);
    problems += oracle_check_i32(&out, ROW_FIELD("draw_index"), seam_state.request_index[i]);
    if (!classified) {
      continue;
    }
    if (t->regime != galaxies[i].Regime) {
      fprintf(stderr, "oracle: %s: galaxy[%d] trace regime %d, stored %d\n", name, i, t->regime,
              galaxies[i].Regime);
      problems++;
    }
    problems += oracle_check_f64(&out, ROW_FIELD("Mshock"), t->mshock);
    problems += oracle_check_f64(&out, ROW_FIELD("mass_ratio"), t->mass_ratio);
    if (!t->drew) {
      continue;
    }
    problems += oracle_check_f64(&out, ROW_FIELD("x"), t->x);
    problems += oracle_check_f64(&out, ROW_FIELD("hot_fraction"), t->hot_fraction);
    problems += oracle_check_f64(&out, ROW_FIELD("deviate"), t->deviate);
    problems += oracle_check_i32(&out, ROW_FIELD("deviate_below_hot_fraction"),
                                 t->deviate < t->hot_fraction);
    problems +=
        oracle_check_i32(&out, ROW_FIELD("deviate_equals_hot_fraction"),
                         oracle_f64_to_bits(t->deviate) == oracle_f64_to_bits(t->hot_fraction));
#undef ROW_FIELD
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
  TEST_ASSERT(oracle_all_bit_identical(MODULE, check_fixture),
              "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_determine_regime SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
