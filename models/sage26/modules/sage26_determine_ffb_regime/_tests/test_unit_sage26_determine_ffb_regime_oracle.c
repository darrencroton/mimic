/**
 * @file    test_unit_sage26_determine_ffb_regime_oracle.c
 * @brief   Bit-exact comparison of shared/sage_ffb_regime.h and sage26_determine_ffb_regime with
 *          SAGE26's calculate_ffb_threshold_mass, calculate_ffb_fraction and
 *          determine_and_store_ffb_regime
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_determine_ffb_regime
 * (models/sage26/modules/sage26_determine_ffb_regime/_tests/oracle/, written by oracle_c13):
 *
 * - thr_* fixtures (routine calculate_ffb_threshold_mass): each case's redshift and the
 *   fixture's Hubble_h into mimic_sage_ffb_threshold_mass;
 * - frac_* fixtures (routine calculate_ffb_fraction): each case's double Mvir and redshift into
 *   mimic_sage_ffb_fraction;
 * - cls_* fixtures (routine determine_and_store_ffb_regime): the module's process() with
 *   EnhancedStarFormationOn 1 and the fixture's Hubble_h, on halos built from the fixture's rows
 *   (SAGE26's float Mvir exactly widened to the core's double, MostBoundID as the i64 it is, a
 *   mergeType > 0 row as the package's Type 3 row, UniqueGalaxyID and the stored SnapNum
 *   poisoned so a read of either would surface), with ctx->redshift the fixture's Zcurr and
 *   ctx->snapshot_number its snapnum. The module's test seam only observes: every deviate is the
 *   production draw, SAGE26's own hash.
 *
 * Per case or row it asserts the return or the written FFBRegime, every recorded local (z_norm,
 * log_Mvir_ffb_code, the threshold Mvir_ffb, x, the fraction f_ffb, the draw and its 64 bits, and
 * the widened Mvir) bit for bit, the guard flag and the comparison's outcome and tie flags, and
 * checks each local's markers (present or absent as in the port, checked, of the recorded check
 * kind, `_validated` the conjunction of its two directions). It asserts the oracle's
 * pow_equals_exp10 is 1 at every threshold evaluation: the package writes pow(10.0, ...), which
 * this -O0 build calls, while SAGE26's object calls __exp10. In a classifier fixture it asserts
 * SAGE26's discarded-draw count equals the number of rows the package classified (one per row
 * not skipped; campaign decision D46) and that the oracle found the result independent of the
 * rand() queue; the queue itself is consumed unused (the product calls no rand()). Where the
 * fixture carries the R6 reference's written FFBRegime it asserts the package's equals it.
 * unfused_threshold_differs is consumed unasserted: an observation of the oracle, not SAGE26 or
 * package state. It prints the tallies quoted in the validation record.
 *
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/sage26_determine_ffb_regime/sage26_determine_ffb_regime.h"
#include "shared/sage_ffb_regime.h"

extern int sage26_determine_ffb_regime_init(void);
extern int sage26_determine_ffb_regime_process(struct ModuleContext *ctx, struct Halo *halos,
                                               int ngal);
extern int sage26_determine_ffb_regime_cleanup(void);

static const char *MODULE = "sage26_determine_ffb_regime";

#define MAX_CASES 32
#define MAX_ROWS 32
#define MAX_QUEUE 32
#define CHECK_CONTINUATION 1
#define CHECK_TAIL 2

/** Tallies over one test run (quoted in the validation record) */
struct c13_tally {
  int cases;          /* threshold and fraction cases */
  int rows;           /* classifier rows */
  int classified;     /* rows the package classified */
  int ffb;            /* rows written FFBRegime 1 */
  int reference_rows; /* rows asserted against the R6 reference's written value */
  int continuation;   /* locals present and continuation-checked */
  int tail;           /* locals present and tail-checked */
  int validated;      /* locals validated two-sided */
  int not_validated;  /* locals checked, not validated two-sided */
  int absent;         /* locals not formed on the path */
  int pow_checks;     /* threshold evaluations with pow(10.0, v) equal to __exp10(v) */
};

static struct c13_tally tally;

static char pre_timestep_name[] = "sage26_determine_ffb_regime";
static struct PhaseModuleConfig pre_timestep_entry[1];
/* The FFB transaction by galaxy in a substep phase, which the classifier requires at
 * EnhancedStarFormationOn 1 (c13 Slice 3) */
static char transaction_name[] = "sage26_ffb_star_formation";
static char substep_phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig substep_entry[1];
static struct ModulePhaseConfig substep_phase;

/**
 * @brief Check one recorded local: its markers, and its value bit for bit
 * @return Number of mismatches and marker errors
 */
static int check_local(struct oracle_fixture *out, const char *prefix, const char *name,
                       const int kind, const int absent_in_port, const double value) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "%s.%s_absent", prefix, name);
  const int absent = oracle_in_i32(out, field);
  if (absent != absent_in_port) {
    fprintf(stderr, "oracle: %s: %s.%s is %s in the oracle but %s in the port\n", out->path, prefix,
            name, absent ? "absent" : "present", absent_in_port ? "absent" : "present");
    return 1;
  }
  if (absent) {
    tally.absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "%s.%s_checked", prefix, name);
  if (oracle_in_i32(out, field) != 1) {
    fprintf(stderr, "oracle: %s: %s.%s is not marked checked\n", out->path, prefix, name);
    problems++;
  }
  snprintf(field, sizeof(field), "%s.%s_check_kind", prefix, name);
  if (oracle_in_i32(out, field) != kind) {
    fprintf(stderr, "oracle: %s: %s.%s is not of check kind %d\n", out->path, prefix, name, kind);
    problems++;
  }
  if (kind == CHECK_CONTINUATION) {
    tally.continuation++;
  } else {
    tally.tail++;
  }
  snprintf(field, sizeof(field), "%s.%s_validated_up", prefix, name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_validated_down", prefix, name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_validated", prefix, name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s.%s_validated disagrees with its two directions\n", out->path,
            prefix, name);
    problems++;
  }
  if (validated) {
    tally.validated++;
  } else {
    tally.not_validated++;
  }
  snprintf(field, sizeof(field), "%s.%s", prefix, name);
  problems += oracle_check_f64(out, field, value);
  return problems;
}

/** @brief Check a threshold evaluation's locals and observations under `prefix` */
static int check_threshold_locals(struct oracle_fixture *out, const char *prefix,
                                  const struct MimicSageFfbThresholdLocals *l) {
  char field[ORACLE_MAX_NAME];
  int problems = 0;
  problems += check_local(out, prefix, "z_norm", CHECK_TAIL, 0, l->z_norm);
  problems += check_local(out, prefix, "log_Mvir_ffb_code", CHECK_TAIL, 0, l->log_Mvir_ffb_code);
  /* The package writes pow(10.0, ...); SAGE26's object calls __exp10: they must agree here */
  snprintf(field, sizeof(field), "%s.pow_equals_exp10", prefix);
  problems += oracle_check_i32(out, field, 1);
  tally.pow_checks++;
  /* An observation of the oracle (the unfused log term's threshold), not SAGE26 or package state */
  snprintf(field, sizeof(field), "%s.unfused_threshold_differs", prefix);
  oracle_exclude(out, field);
  return problems;
}

/** @brief Check a fraction evaluation's locals (the threshold's included) under `prefix` */
static int check_fraction_locals(struct oracle_fixture *out, const char *prefix,
                                 const struct MimicSageFfbFractionLocals *l) {
  char field[ORACLE_MAX_NAME];
  int problems = check_threshold_locals(out, prefix, &l->threshold);
  problems += check_local(out, prefix, "Mvir_ffb", CHECK_TAIL, 0, l->Mvir_ffb);
  snprintf(field, sizeof(field), "%s.guard_returned", prefix);
  problems += oracle_check_i32(out, field, l->guard_returned);
  problems += check_local(out, prefix, "x", CHECK_TAIL, l->guard_returned, l->x);
  return problems;
}

/** @brief Read call.ncase, 1..MAX_CASES (0 on an error, reported) */
static int read_ncase(struct oracle_fixture *in, const char *name) {
  const int ncase = oracle_in_i32(in, "call.ncase");
  if (ncase < 1 || ncase > MAX_CASES) {
    fprintf(stderr, "oracle: %s: call.ncase %d outside 1..%d\n", name, ncase, MAX_CASES);
    return 0;
  }
  return ncase;
}

/** @brief Compare one calculate_ffb_threshold_mass fixture; returns mismatches and errors */
static int check_threshold_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "calculate_ffb_threshold_mass", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;
  const double hubble_h = oracle_in_f64(&in, "params.Hubble_h");
  const int ncase = read_ncase(&in, name);
  if (ncase == 0) {
    return 1;
  }
  for (int k = 0; k < ncase; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "case[%d].z", k);
    const double z = oracle_in_f64(&in, field);

    struct MimicSageFfbThresholdLocals l;
    const double threshold = mimic_sage_ffb_threshold_mass(z, hubble_h, &l);
    tally.cases++;

    snprintf(field, sizeof(field), "return.case[%d]", k);
    problems += oracle_check_f64(&out, field, threshold);
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    problems += check_threshold_locals(&out, prefix, &l);
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Read params.EnhancedStarFormationOn, which every c13 fixture holds at 1 */
static int require_enhanced_star_formation_on(struct oracle_fixture *in, const char *name) {
  const int value = oracle_in_i32(in, "params.EnhancedStarFormationOn");
  if (value != MIMIC_SAGE_FFB_LI24) {
    fprintf(stderr, "oracle: %s: params.EnhancedStarFormationOn %d, expected 1\n", name, value);
    return 1;
  }
  return 0;
}

/** @brief Compare one calculate_ffb_fraction fixture; returns mismatches and errors */
static int check_fraction_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "calculate_ffb_fraction", &in, &out) != 0) {
    return 1;
  }
  int problems = require_enhanced_star_formation_on(&in, name);
  const double hubble_h = oracle_in_f64(&in, "params.Hubble_h");
  const int ncase = read_ncase(&in, name);
  if (ncase == 0) {
    return 1;
  }
  for (int k = 0; k < ncase; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "case[%d].Mvir", k);
    const double mvir = oracle_in_f64(&in, field);
    snprintf(field, sizeof(field), "case[%d].z", k);
    const double z = oracle_in_f64(&in, field);

    struct MimicSageFfbFractionLocals l;
    const double f_ffb = mimic_sage_ffb_fraction(mvir, z, hubble_h, &l);
    tally.cases++;

    snprintf(field, sizeof(field), "return.case[%d]", k);
    problems += oracle_check_f64(&out, field, f_ffb);
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    problems += check_fraction_locals(&out, prefix, &l);
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** What the observing seam saw */
struct observed_rows {
  int observed[MAX_ROWS];
  int count;
  struct Sage26FfbRowTrace trace[MAX_ROWS];
  int problems;
};

static void observe_row(void *user, const struct Sage26FfbRowTrace *row) {
  struct observed_rows *s = user;
  if (row->row < 0 || row->row >= MAX_ROWS) {
    s->problems++;
    return;
  }
  s->observed[row->row] = 1;
  s->trace[row->row] = *row;
  s->count++;
}

/** @brief The i64 of the same two's-complement bits as a 64-bit pattern */
static long long bits_as_i64(const uint64_t bits) {
  long long value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief Compare one determine_and_store_ffb_regime fixture; returns mismatches and errors */
static int check_classifier_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "determine_and_store_ffb_regime", &in, &out) != 0) {
    return 1;
  }
  int problems = require_enhanced_star_formation_on(&in, name);

  /* Configuration: EnhancedStarFormationOn 1 (R6), the fixture's Hubble_h, the module alone in
   * pre_timestep and the FFB transaction by galaxy in a substep phase (the classifier's guard) */
  reset_config();
  pre_timestep_entry[0].module_name = pre_timestep_name;
  pre_timestep_entry[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep_entry[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep_entry;
  MimicConfig.num_pre_timestep = 1;
  substep_entry[0].module_name = transaction_name;
  substep_entry[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  substep_entry[0].resolved = NULL;
  substep_phase.name = substep_phase_name;
  substep_phase.modules = substep_entry;
  substep_phase.num_modules = 1;
  MimicConfig.substep_phases = &substep_phase;
  MimicConfig.num_substep_phases = 1;
  MimicConfig.Hubble_h = oracle_in_f64(&in, "params.Hubble_h");
  if (oracle_set_model_param("EnhancedStarFormationOn", 1.0) != 0 ||
      sage26_determine_ffb_regime_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", name);
    return problems + 1;
  }

  const double zcurr = oracle_in_f64(&in, "call.Zcurr");
  const int snapnum = oracle_in_i32(&in, "call.snapnum");
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
    halos[i].MostBoundID = oracle_in_i64(&in, oracle_galaxy_field(i, "MostBoundID"));
    galaxies[i].FFBRegime = oracle_in_i32(&in, oracle_galaxy_field(i, "FFBRegime"));
    galaxies[i].Regime = oracle_in_i32(&in, oracle_galaxy_field(i, "Regime"));
    /* UniqueGalaxyID and the stored SnapNum stay poisoned (-1): the classifier reads neither */
    skipped[i] = merge_type > 0;
    if (skipped[i] != (halos[i].Type == 3)) {
      fprintf(stderr,
              "oracle: %s: galaxy[%d] has Type %d and mergeType %d; the package skips exactly "
              "its Type 3 rows, SAGE26 exactly its mergeType > 0 rows\n",
              name, i, halos[i].Type, merge_type);
      problems++;
    }
  }
  /* SAGE26's discarded rand() values: the product calls no rand(), so the queue is unread */
  const int queue_length = oracle_in_i32(&in, "rand.queue_length");
  if (queue_length < 0 || queue_length > MAX_QUEUE) {
    fprintf(stderr, "oracle: %s: rand.queue_length outside 0..%d\n", name, MAX_QUEUE);
    return problems + 1;
  }
  for (int k = 0; k < queue_length; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "rand.queue[%d]", k);
    (void)oracle_in_i32(&in, field);
  }
  const int has_reference = oracle_in_i32(&in, "call.has_reference");
  int reference[MAX_ROWS];
  for (int i = 0; has_reference && i < ngal; i++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "reference.galaxy[%d].FFBRegime", i);
    reference[i] = oracle_in_i32(&in, field);
  }

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.redshift = zcurr;
  ctx.snapshot_number = snapnum;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];

  static struct observed_rows seen;
  memset(&seen, 0, sizeof(seen));
  const struct Sage26FfbSeam seam = {NULL, observe_row, &seen};
  sage26_determine_ffb_regime_set_test_seam(&seam);
  const int status = sage26_determine_ffb_regime_process(&ctx, halos, ngal);
  sage26_determine_ffb_regime_set_test_seam(NULL);
  sage26_determine_ffb_regime_cleanup();
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  MimicConfig.substep_phases = NULL;
  MimicConfig.num_substep_phases = 0;
  if (status != 0) {
    fprintf(stderr, "oracle: %s: process() returned %d\n", name, status);
    problems++;
  }
  problems += seen.problems;

  /* One discarded rand() per row SAGE26 does not skip (D46), the result independent of them */
  problems += oracle_check_i32(&out, "intermediate.draws_discarded", seen.count);
  problems += oracle_check_i32(&out, "intermediate.rand_queue_independent", 1);
  oracle_exclude(&out, "intermediate.RAND_MAX"); /* the oracle platform's, unused by the product */

  for (int i = 0; i < ngal; i++) {
    char field[ORACLE_MAX_NAME];
    const struct Sage26FfbRowTrace *t = &seen.trace[i];
    const int classified = seen.observed[i];
    tally.rows++;
    problems += oracle_check_i32(&out, oracle_galaxy_field(i, "FFBRegime"), galaxies[i].FFBRegime);
    if (has_reference) {
      snprintf(field, sizeof(field), "reference.galaxy[%d].FFBRegime", i);
      problems += oracle_check_i32(&out, field, reference[i]);
      if (galaxies[i].FFBRegime != reference[i]) {
        fprintf(stderr, "oracle: %s: galaxy[%d] FFBRegime %d, the R6 reference wrote %d\n", name, i,
                galaxies[i].FFBRegime, reference[i]);
        problems++;
      }
      tally.reference_rows++;
    }
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.galaxy[%d]", i);
#define ROW_FIELD(member) (snprintf(field, sizeof(field), "%s." member, prefix), field)
    problems += oracle_check_i32(&out, ROW_FIELD("skipped"), !classified);
    problems += oracle_check_i32(&out, ROW_FIELD("discarded_draw"), classified);
    if (!classified) {
      continue;
    }
    tally.classified++;
    tally.ffb += galaxies[i].FFBRegime == 1;
    if (t->ffb_regime != galaxies[i].FFBRegime || t->snapshot != snapnum ||
        oracle_f64_to_bits(t->redshift) != oracle_f64_to_bits(zcurr) ||
        t->most_bound_id != halos[i].MostBoundID) {
      fprintf(stderr, "oracle: %s: galaxy[%d]'s trace disagrees with its row or the context\n",
              name, i);
      problems++;
    }
    problems += oracle_check_i64(&out, ROW_FIELD("draw_bits"), bits_as_i64(t->draw_bits));
    problems += oracle_check_f64(&out, ROW_FIELD("Mvir"), t->mvir);
    problems += check_local(&out, prefix, "draw", CHECK_CONTINUATION, 0, t->draw);
    problems += check_fraction_locals(&out, prefix, &t->fraction);
    problems += check_local(&out, prefix, "f_ffb", CHECK_TAIL, 0, t->f_ffb);
    problems += oracle_check_i32(&out, ROW_FIELD("draw_below_f_ffb"), t->draw < t->f_ffb);
    problems += oracle_check_i32(&out, ROW_FIELD("draw_equals_f_ffb"),
                                 oracle_f64_to_bits(t->draw) == oracle_f64_to_bits(t->f_ffb));
#undef ROW_FIELD
  }

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Compare one fixture by its routine (the name's prefix) */
static int check_fixture(const char *name) {
  if (strncmp(name, "thr_", 4) == 0) {
    return check_threshold_fixture(name);
  }
  if (strncmp(name, "frac_", 5) == 0) {
    return check_fraction_fixture(name);
  }
  if (strncmp(name, "cls_", 4) == 0) {
    return check_classifier_fixture(name);
  }
  fprintf(stderr, "oracle: %s: a fixture name must start with thr_, frac_ or cls_\n", name);
  return 1;
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
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle c13 (%s): %d threshold/fraction cases, %d classifier rows (%d classified, "
         "%d FFB, %d against the R6 reference); locals continuation-checked %d, tail-checked %d; "
         "validated two-sided %d, not validated %d; absent %d; pow == __exp10 at %d threshold "
         "evaluations ",
         MODULE, tally.cases, tally.rows, tally.classified, tally.ffb, tally.reference_rows,
         tally.continuation, tally.tail, tally.validated, tally.not_validated, tally.absent,
         tally.pow_checks);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(tally.ffb >= 1 && tally.classified > tally.ffb,
              "both outcomes of the classification must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_determine_ffb_regime SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
