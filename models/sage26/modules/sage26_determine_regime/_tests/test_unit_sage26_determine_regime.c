/**
 * @file    test_unit_sage26_determine_regime.c
 * @brief   Platform-independent behavioural tests of sage26_determine_regime
 *
 * The oracle test (test_unit_sage26_determine_regime_oracle.c) skips off macOS arm64, so these
 * tests are the classifier's guard on every other platform. Each runs the module's process()
 * through its test seam (sage26_determine_regime.h): the seam supplies the deviates and reports
 * which rows requested one and each row's values. Every expectation is derived independently of
 * the module: literal inputs, or SAGE26's own expressions written here with their operand order
 * (R/src/macros.h:47 for the shock mass, R/src/model_regimes.c:74-95 for the ratio, sigmoid and
 * comparison), never by calling the module's helpers. R/ is the staged SAGE26 source root.
 *
 * Software quality:
 * - test_module_lifecycle: registration, module_system_init with the module in pre_timestep,
 *   cleanup, no leaks
 * - test_cgm_recipe_on_missing_refused, test_cgm_recipe_on_values: the required CGMrecipeOn:
 *   missing fails init; 2, -1 and 3 are refused naming it; 0, 1 accepted
 * - test_mshock_missing_refused, test_mshock_values: the required MShockMsun: missing fails init;
 *   0, negative, inf and nan are refused naming it; 6.0e11 accepted
 * - test_placement_refused: outside pre_timestep, twice, or after the infall budget is refused
 *
 * Behaviour:
 * - test_cgm_recipe_off_writes_nothing: CGMrecipeOn 0 writes nothing and requests no deviate
 * - test_mvir_zero_and_negative_no_deviate: Mvir +0, -0 and negative give Regime 0, no deviate
 * - test_mvir_nan_requests_deviate: a NaN Mvir requests a deviate and gives Regime 0
 * - test_ratio_above_at_below: mass ratios 2, ~1 and 1/2 against the deviates one step below,
 *   equal to and one step above SAGE26's sigmoid value (expected Regime 1, 0, 0)
 * - test_ratio_one_tie: the exact mass ratio 1 (hot_fraction 0.5) with the deviates below, equal
 *   to (the tie: Regime 0) and above 0.5
 * - test_initial_boundary_row: a row in the initial-boundary state (stored SnapNum -1, dT 0) is
 *   classified without error
 * - test_every_type_and_skipped_rows: Types 0, 1 and 2 are classified in row order; Type 3 and
 *   galaxy-less rows are skipped, keeping their Regime; no previous regime is reused
 * - test_production_keyed_draw: without a seam, the production keyed draw of the row's
 *   UniqueGalaxyID and ctx->snapshot_number decides, checked against reference deviates written
 *   here as literals
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/sage26_determine_regime/sage26_determine_regime.h"

extern int sage26_determine_regime_init(void);
extern int sage26_determine_regime_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sage26_determine_regime_cleanup(void);

#define MAX_ROWS 8

/* R4's Hubble_h (input/millennium.par:40) and SAGE26's
 * default MShockMsun (core_read_parameter_file.c:114) */
#define TEST_HUBBLE_H 0.73
#define TEST_MSHOCK_MSUN 6.0e11

// ============================================================================
// FIXTURES
// ============================================================================

/* The module's pre_timestep entry, and the infall budget's for the ordering test */
static char regime_name[] = "sage26_determine_regime";
static char budget_name[] = "sage26_prepare_infall_budget";
static struct PhaseModuleConfig pre_timestep_entries[3];
static struct PhaseModuleConfig post_timestep_entries[1];

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/**
 * @brief Configure and initialise the module, capturing the log
 *
 * @param cgm       CGMrecipeOn's run-file text, or NULL for a missing key
 * @param mshock    MShockMsun's run-file text, or NULL for a missing key
 * @param hubble_h  The run's Hubble_h
 * @param log       Receives the captured log
 * @return The init status
 */
static int init_captured(const char *cgm, const char *mshock, const double hubble_h,
                         const char **log) {
  reset_config();
  MimicConfig.Hubble_h = hubble_h;
  pre_timestep_entries[0].module_name = regime_name;
  pre_timestep_entries[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep_entries[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep_entries;
  MimicConfig.num_pre_timestep = 1;
  if (cgm != NULL) {
    sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  }
  if (mshock != NULL) {
    sage26_test_append_model_parameter("MShockMsun", mshock);
  }
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_determine_regime_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/** @brief init_captured() with R4's values for the given CGMrecipeOn, asserting success */
static int init_ok(const char *cgm, const double hubble_h) {
  const char *log = "";
  return init_captured(cgm, "6.0e11", hubble_h, &log);
}

/** What the seam saw: deviates to hand out per row, and the requests and rows it was told of */
struct recording_seam {
  double deviate_for_row[MAX_ROWS];
  int requests;
  int request_row[MAX_ROWS]; /* the row of each request, in request order */
  int observed;
  struct Sage26RegimeRowTrace trace[MAX_ROWS];
  int observed_row[MAX_ROWS];
};

static double recording_deviate(void *user, const struct Sage26RegimeRowTrace *row) {
  struct recording_seam *s = user;
  if (s->requests < MAX_ROWS) {
    s->request_row[s->requests] = row->row;
  }
  s->requests++;
  return (row->row >= 0 && row->row < MAX_ROWS) ? s->deviate_for_row[row->row] : NAN;
}

static void recording_observe(void *user, const struct Sage26RegimeRowTrace *row) {
  struct recording_seam *s = user;
  if (row->row >= 0 && row->row < MAX_ROWS) {
    s->trace[row->row] = *row;
    s->observed_row[row->row] = 1;
  }
  s->observed++;
}

/** @brief A row with a galaxy: Type, core Mvir, UniqueGalaxyID and an input Regime */
static void make_row(struct Halo *halo, struct GalaxyData *gal, const int type, const double mvir,
                     const long long id, const int regime) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  halo->galaxy = gal;
  halo->Type = type;
  halo->Mvir = mvir;
  halo->UniqueGalaxyID = id;
  halo->SnapNum = 62;
  halo->dT = 100.0;
  gal->Regime = regime;
}

/** @brief Run process() on the rows with the recording seam installed; returns its status */
static int run_with_seam(struct Halo *halos, const int ngal, struct recording_seam *s,
                         const int snapshot) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.snapshot_number = snapshot;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];
  const struct Sage26RegimeSeam seam = {recording_deviate, recording_observe, s};
  sage26_determine_regime_set_test_seam(&seam);
  const int status = sage26_determine_regime_process(&ctx, halos, ngal);
  sage26_determine_regime_set_test_seam(NULL);
  return status;
}

/** @brief Bit pattern of a double, for exact comparisons */
static uint64_t bits_of(const double v) {
  uint64_t b;
  memcpy(&b, &v, sizeof(b));
  return b;
}

/**
 * @brief SAGE26's sigmoid for a float Mvir, written as SAGE26 writes it
 *
 * MSUN_TO_CODE_MASS(MShockMsun, Hubble_h) = ((x) * (h) / 1.0e10) (R/src/macros.h:47), the ratio
 * of the float Mvir to it (R/src/model_regimes.c:74), x = log10(ratio) / delta_log_M with
 * delta_log_M 0.1 (:82, :85), hot_fraction = 1.0 / (1.0 + exp(-x)) (:89).
 *
 * The tests assert the module's runtime result bit for bit and at one-ulp boundaries, so this
 * expectation must also be computed at run time by the same libm. The callers pass compile-time
 * constants, and an optimising compiler could inline the helper and fold log10 and exp at
 * compile time (GCC folds with MPFR, correctly rounded, which can differ from the runtime libm by
 * one ulp). The helper is therefore never inlined, and its inputs pass through volatile locals the
 * compiler cannot treat as constants.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SAGE26_REGIME_TEST_NOINLINE __attribute__((noinline))
#else
#define SAGE26_REGIME_TEST_NOINLINE
#endif
static SAGE26_REGIME_TEST_NOINLINE double sage26_hot_fraction(const float mvir_in,
                                                              const double mshock_msun_in,
                                                              const double hubble_h_in,
                                                              double *mshock_out) {
  volatile float mvir_runtime = mvir_in;
  volatile double mshock_msun_runtime = mshock_msun_in;
  volatile double hubble_h_runtime = hubble_h_in;
  const float mvir = mvir_runtime;
  const double mshock_msun = mshock_msun_runtime;
  const double hubble_h = hubble_h_runtime;

  const double Mshock = ((mshock_msun) * (hubble_h) / 1.0e10);
  const double mass_ratio = mvir / Mshock;
  const double delta_log_M = 0.1;
  const double x = log10(mass_ratio) / delta_log_M;
  const double hot_fraction = 1.0 / (1.0 + exp(-x));
  if (mshock_out != NULL) {
    *mshock_out = Mshock;
  }
  return hot_fraction;
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_lifecycle
 * @brief   The module registers, initialises through module_system_init in pre_timestep with
 *          CGMrecipeOn 0 (SAGE26's R0-R3 value), MShockMsun 6.0e11, cleans up, no leaks
 */
int test_module_lifecycle(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  MimicConfig.Hubble_h = TEST_HUBBLE_H;
  test_pre_timestep_add("sage26_determine_regime", PROCESSING_MODE_FULL_HALO);
  set_test_model_parameters();
  sage26_test_append_model_parameter("CGMrecipeOn", "0");
  sage26_test_append_model_parameter("MShockMsun", "6.0e11");

  const int result = module_system_init();
  TEST_ASSERT(result == 0, "module system initialisation must succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_missing_refused
 * @brief   Init fails without CGMrecipeOn, naming it: a required parameter with no default
 */
int test_cgm_recipe_on_missing_refused(void) {
  const char *log = "";
  const int status = init_captured(NULL, "6.0e11", TEST_HUBBLE_H, &log);
  printf("\n  without CGMrecipeOn: init %d", status);
  TEST_ASSERT(status != 0, "init must fail when CGMrecipeOn is missing");
  TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the error must name CGMrecipeOn");
  return TEST_PASS;
}

/**
 * @test    test_cgm_recipe_on_values
 * @brief   CGMrecipeOn 2, -1 and 3 are refused, naming the parameter; 0 and 1 initialise
 */
int test_cgm_recipe_on_values(void) {
  const char *refused[] = {"2", "-1", "3"};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    const char *log = "";
    const int status = init_captured(refused[k], "6.0e11", TEST_HUBBLE_H, &log);
    printf("\n  CGMrecipeOn %s: init %d", refused[k], status);
    TEST_ASSERT(status != 0, "init must refuse an unsupported CGMrecipeOn");
    TEST_ASSERT(strstr(log, "CGMrecipeOn") != NULL, "the error must name CGMrecipeOn");
  }
  const char *accepted[] = {"0", "1"};
  for (size_t k = 0; k < sizeof(accepted) / sizeof(accepted[0]); k++) {
    const char *log = "";
    const int status = init_captured(accepted[k], "6.0e11", TEST_HUBBLE_H, &log);
    printf("\n  CGMrecipeOn %s: init %d", accepted[k], status);
    TEST_ASSERT(status == 0, "init must accept CGMrecipeOn 0 and 1");
  }
  return TEST_PASS;
}

/**
 * @test    test_mshock_missing_refused
 * @brief   Init fails without MShockMsun, naming it: a required parameter with no default
 */
int test_mshock_missing_refused(void) {
  const char *log = "";
  const int status = init_captured("1", NULL, TEST_HUBBLE_H, &log);
  printf("\n  without MShockMsun: init %d", status);
  TEST_ASSERT(status != 0, "init must fail when MShockMsun is missing");
  TEST_ASSERT(strstr(log, "MShockMsun") != NULL, "the error must name MShockMsun");
  return TEST_PASS;
}

/**
 * @test    test_mshock_values
 * @brief   MShockMsun 0, -0, negative, inf, -inf and nan are refused, naming the parameter;
 *          6.0e11 initialises (with CGMrecipeOn 0 and 1)
 */
int test_mshock_values(void) {
  const char *refused[] = {"0", "-0.0", "-6.0e11", "inf", "-inf", "nan"};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    for (int cgm = 0; cgm <= 1; cgm++) {
      const char *log = "";
      const int status = init_captured(cgm ? "1" : "0", refused[k], TEST_HUBBLE_H, &log);
      printf("\n  MShockMsun %s (CGMrecipeOn %d): init %d", refused[k], cgm, status);
      TEST_ASSERT(status != 0, "init must refuse a non-positive or non-finite MShockMsun");
      TEST_ASSERT(strstr(log, "MShockMsun") != NULL, "the error must name MShockMsun");
    }
  }
  TEST_ASSERT(init_ok("0", TEST_HUBBLE_H) == 0, "MShockMsun 6.0e11 must initialise (CGM off)");
  TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "MShockMsun 6.0e11 must initialise (CGM on)");
  return TEST_PASS;
}

/**
 * @test    test_placement_refused
 * @brief   The module must be configured once, in pre_timestep, before the infall budget
 */
int test_placement_refused(void) {
  const char *log = "";

  /* Not in pre_timestep at all */
  TEST_ASSERT(init_captured("1", "6.0e11", TEST_HUBBLE_H, &log) == 0, "baseline must initialise");
  MimicConfig.num_pre_timestep = 0;
  TEST_ASSERT(sage26_determine_regime_init() != 0, "init must refuse a missing pre_timestep entry");

  /* Twice in pre_timestep */
  TEST_ASSERT(init_captured("1", "6.0e11", TEST_HUBBLE_H, &log) == 0, "baseline must initialise");
  pre_timestep_entries[1] = pre_timestep_entries[0];
  MimicConfig.num_pre_timestep = 2;
  TEST_ASSERT(sage26_determine_regime_init() != 0, "init must refuse two pre_timestep entries");

  /* Also in post_timestep */
  TEST_ASSERT(init_captured("1", "6.0e11", TEST_HUBBLE_H, &log) == 0, "baseline must initialise");
  post_timestep_entries[0] = pre_timestep_entries[0];
  MimicConfig.post_timestep = post_timestep_entries;
  MimicConfig.num_post_timestep = 1;
  TEST_ASSERT(sage26_determine_regime_init() != 0, "init must refuse a post_timestep entry");
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;

  /* After the infall budget, then before it */
  TEST_ASSERT(init_captured("1", "6.0e11", TEST_HUBBLE_H, &log) == 0, "baseline must initialise");
  pre_timestep_entries[1] = pre_timestep_entries[0];
  pre_timestep_entries[0].module_name = budget_name;
  MimicConfig.num_pre_timestep = 2;
  TEST_ASSERT(sage26_determine_regime_init() != 0,
              "init must refuse running after sage26_prepare_infall_budget");
  pre_timestep_entries[0].module_name = regime_name;
  pre_timestep_entries[1].module_name = budget_name;
  TEST_ASSERT(sage26_determine_regime_init() == 0,
              "init must accept running before sage26_prepare_infall_budget");
  return TEST_PASS;
}

// ============================================================================
// BEHAVIOURAL TESTS
// ============================================================================

/**
 * @test    test_cgm_recipe_off_writes_nothing
 * @brief   With CGMrecipeOn 0 the module writes nothing and requests no deviate: SAGE26 skips the
 *          call (R/src/core_build_model.c:410)
 */
int test_cgm_recipe_off_writes_nothing(void) {
  TEST_ASSERT(init_ok("0", TEST_HUBBLE_H) == 0, "init must succeed");
  struct Halo halos[4];
  struct GalaxyData gals[4];
  make_row(&halos[0], &gals[0], 0, 100.0, 11, -1);
  make_row(&halos[1], &gals[1], 1, 10.0, 12, 0);
  make_row(&halos[2], &gals[2], 2, 0.0, 13, 1);
  make_row(&halos[3], &gals[3], 0, NAN, 14, -1);
  struct Halo halos_before[4];
  struct GalaxyData gals_before[4];
  memcpy(halos_before, halos, sizeof(halos));
  memcpy(gals_before, gals, sizeof(gals));

  struct recording_seam s;
  memset(&s, 0, sizeof(s));
  const int status = run_with_seam(halos, 4, &s, 63);
  printf("\n  CGMrecipeOn 0: status %d, deviates requested %d, rows reported %d", status,
         s.requests, s.observed);
  TEST_ASSERT(status == 0, "process must succeed");
  TEST_ASSERT(s.requests == 0, "CGMrecipeOn 0 must request no deviate");
  TEST_ASSERT(s.observed == 0, "CGMrecipeOn 0 must classify no row");
  TEST_ASSERT(memcmp(gals, gals_before, sizeof(gals)) == 0, "CGMrecipeOn 0 must write nothing");
  TEST_ASSERT(memcmp(halos, halos_before, sizeof(halos)) == 0,
              "CGMrecipeOn 0 must leave the rows unchanged");
  return TEST_PASS;
}

/**
 * @test    test_mvir_zero_and_negative_no_deviate
 * @brief   Mvir +0, -0 and negative give mass_ratio <= 0.0: Regime 0 and no deviate requested
 *          (R/src/model_regimes.c:77-78)
 */
int test_mvir_zero_and_negative_no_deviate(void) {
  TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "init must succeed");
  struct Halo halos[3];
  struct GalaxyData gals[3];
  make_row(&halos[0], &gals[0], 0, 0.0, 21, 1);
  make_row(&halos[1], &gals[1], 0, -0.0, 22, 1);
  make_row(&halos[2], &gals[2], 1, -5.0, 23, -1);

  struct recording_seam s;
  memset(&s, 0, sizeof(s));
  for (int i = 0; i < 3; i++) {
    s.deviate_for_row[i] = 0x1.0p-53; /* would give Regime 1 for any positive hot_fraction */
  }
  TEST_ASSERT(run_with_seam(halos, 3, &s, 63) == 0, "process must succeed");
  printf("\n  Mvir 0, -0, -5: Regime %d %d %d, deviates requested %d", gals[0].Regime,
         gals[1].Regime, gals[2].Regime, s.requests);
  TEST_ASSERT(s.requests == 0, "a zero or negative Mvir must request no deviate");
  TEST_ASSERT(s.observed == 3, "every row must be classified");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(gals[i].Regime == 0, "a zero or negative Mvir must give Regime 0");
    TEST_ASSERT(s.trace[i].drew == 0, "the seam must report no draw");
  }
  return TEST_PASS;
}

/**
 * @test    test_mvir_nan_requests_deviate
 * @brief   A NaN Mvir gives a NaN mass_ratio, which is not <= 0.0, so it requests a deviate; every
 *          ordered comparison with the NaN hot_fraction is false, so Regime is 0
 */
int test_mvir_nan_requests_deviate(void) {
  TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "init must succeed");
  struct Halo halos[1];
  struct GalaxyData gals[1];
  make_row(&halos[0], &gals[0], 0, NAN, 31, 1);

  struct recording_seam s;
  memset(&s, 0, sizeof(s));
  s.deviate_for_row[0] = 0x1.0p-53;
  TEST_ASSERT(run_with_seam(halos, 1, &s, 63) == 0, "process must succeed");
  printf("\n  Mvir NaN: Regime %d, deviates requested %d", gals[0].Regime, s.requests);
  TEST_ASSERT(s.requests == 1, "a NaN Mvir must request one deviate");
  TEST_ASSERT(s.trace[0].drew == 1, "the seam must report the draw");
  TEST_ASSERT(isnan(s.trace[0].mass_ratio) && isnan(s.trace[0].hot_fraction),
              "the ratio and sigmoid value must be NaN");
  TEST_ASSERT(gals[0].Regime == 0, "a NaN Mvir must give Regime 0");
  return TEST_PASS;
}

/**
 * @test    test_ratio_above_at_below
 * @brief   Mass ratios 2, the float nearest 1 and 1/2, each against the deviate one step below,
 *          equal to and one step above SAGE26's sigmoid value: Regime 1, 0 and 0
 *          (R/src/model_regimes.c:95, a strict u < hot_fraction)
 */
int test_ratio_above_at_below(void) {
  /* 87.6, 43.8 and 21.9 against Mshock = 6e11 * 0.73 / 1e10 = 43.8 */
  const float mvir[3] = {87.6f, 43.8f, 21.9f};
  const double direction[3] = {0.0, NAN, 1.0}; /* NAN: the deviate equal to hot_fraction */
  const int expected_regime[3] = {1, 0, 0};

  for (int r = 0; r < 3; r++) {
    double expected_mshock = 0.0;
    const double p =
        sage26_hot_fraction(mvir[r], TEST_MSHOCK_MSUN, TEST_HUBBLE_H, &expected_mshock);
    for (int d = 0; d < 3; d++) {
      TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "init must succeed");
      struct Halo halos[1];
      struct GalaxyData gals[1];
      make_row(&halos[0], &gals[0], 0, (double)mvir[r], 41, -1);
      struct recording_seam s;
      memset(&s, 0, sizeof(s));
      s.deviate_for_row[0] = isnan(direction[d]) ? p : nextafter(p, direction[d]);
      TEST_ASSERT(run_with_seam(halos, 1, &s, 63) == 0, "process must succeed");
      printf("\n  Mvir %.9g: hot_fraction %.17g, deviate %.17g -> Regime %d", (double)mvir[r], p,
             s.deviate_for_row[0], gals[0].Regime);
      TEST_ASSERT(s.requests == 1, "a positive Mvir must request one deviate");
      TEST_ASSERT(bits_of(s.trace[0].mshock) == bits_of(expected_mshock),
                  "Mshock must be MShockMsun * Hubble_h / 1.0e10 (R/src/macros.h:47)");
      TEST_ASSERT(bits_of(s.trace[0].hot_fraction) == bits_of(p),
                  "hot_fraction must be SAGE26's sigmoid (R/src/model_regimes.c:74-89)");
      TEST_ASSERT(gals[0].Regime == expected_regime[d],
                  "Regime must be 1 exactly when the deviate is below hot_fraction");
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_ratio_one_tie
 * @brief   Mass ratio exactly 1 (Hubble_h 0.5, Mshock 30, Mvir 30): hot_fraction is exactly 0.5,
 *          and the deviate 0.5 itself gives Regime 0 (the tie no rand() value reaches)
 */
int test_ratio_one_tie(void) {
  const double deviates[3] = {nextafter(0.5, 0.0), 0.5, nextafter(0.5, 1.0)};
  const int expected_regime[3] = {1, 0, 0};
  for (int d = 0; d < 3; d++) {
    TEST_ASSERT(init_ok("1", 0.5) == 0, "init must succeed");
    struct Halo halos[1];
    struct GalaxyData gals[1];
    make_row(&halos[0], &gals[0], 0, 30.0, 51, -1);
    struct recording_seam s;
    memset(&s, 0, sizeof(s));
    s.deviate_for_row[0] = deviates[d];
    TEST_ASSERT(run_with_seam(halos, 1, &s, 63) == 0, "process must succeed");
    printf("\n  ratio 1: hot_fraction %a, deviate %a -> Regime %d", s.trace[0].hot_fraction,
           deviates[d], gals[0].Regime);
    TEST_ASSERT(s.trace[0].mshock == 30.0 && s.trace[0].mass_ratio == 1.0,
                "Mshock must be 30 and the ratio exactly 1");
    TEST_ASSERT(s.trace[0].x == 0.0 && s.trace[0].hot_fraction == 0.5,
                "x must be 0 and hot_fraction exactly 0.5");
    TEST_ASSERT(gals[0].Regime == expected_regime[d],
                "the tie u == hot_fraction must give Regime 0, a smaller deviate Regime 1");
  }
  return TEST_PASS;
}

/**
 * @test    test_initial_boundary_row
 * @brief   A row in the package's initial-boundary state (stored SnapNum -1, dT 0) is
 *          classified like any other: the classifier reads no snapshot-indexed table
 *          (R/src/model_regimes.c:63-100)
 */
int test_initial_boundary_row(void) {
  TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "init must succeed");
  struct Halo halos[1];
  struct GalaxyData gals[1];
  make_row(&halos[0], &gals[0], 0, 87.6, 61, -1);
  halos[0].SnapNum = -1;
  halos[0].dT = 0.0;
  const double p = sage26_hot_fraction(87.6f, TEST_MSHOCK_MSUN, TEST_HUBBLE_H, NULL);
  struct recording_seam s;
  memset(&s, 0, sizeof(s));
  s.deviate_for_row[0] = nextafter(p, 0.0);
  const int status = run_with_seam(halos, 1, &s, 0);
  printf("\n  SnapNum -1, dT 0: status %d, Regime %d", status, gals[0].Regime);
  TEST_ASSERT(status == 0, "the initial-boundary row must be classified without error");
  TEST_ASSERT(s.requests == 1 && gals[0].Regime == 1, "the row must be classified");
  return TEST_PASS;
}

/**
 * @test    test_every_type_and_skipped_rows
 * @brief   Types 0, 1 and 2 are classified in row order with no Type restriction; Type 3 rows
 *          (SAGE26's mergeType > 0) and rows without a galaxy are skipped and keep their Regime;
 *          the input Regime is overwritten, never reused
 */
int test_every_type_and_skipped_rows(void) {
  TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "init must succeed");
  struct Halo halos[6];
  struct GalaxyData gals[6];
  make_row(&halos[0], &gals[0], 0, 87.6, 71, 0); /* central: deviate below -> 1 */
  make_row(&halos[1], &gals[1], 3, 87.6, 72, 7); /* merged: skipped, keeps 7 */
  make_row(&halos[2], &gals[2], 1, 21.9, 73, 1); /* satellite: deviate above -> 0 */
  make_row(&halos[3], &gals[3], 2, 0.0, 74, 1);  /* orphan, Mvir 0: no draw -> 0 */
  make_row(&halos[4], &gals[4], 2, 87.6, 75, 0); /* Type 2 with a mass: deviate below -> 1 */
  make_row(&halos[5], &gals[5], 0, 87.6, 76, 5); /* no galaxy: skipped */
  halos[5].galaxy = NULL;

  const double p_above = sage26_hot_fraction(87.6f, TEST_MSHOCK_MSUN, TEST_HUBBLE_H, NULL);
  const double p_below = sage26_hot_fraction(21.9f, TEST_MSHOCK_MSUN, TEST_HUBBLE_H, NULL);
  struct recording_seam s;
  memset(&s, 0, sizeof(s));
  s.deviate_for_row[0] = nextafter(p_above, 0.0);
  s.deviate_for_row[1] = 0x1.0p-53;
  s.deviate_for_row[2] = nextafter(p_below, 1.0);
  s.deviate_for_row[4] = nextafter(p_above, 0.0);
  TEST_ASSERT(run_with_seam(halos, 6, &s, 63) == 0, "process must succeed");
  printf("\n  Regime %d %d %d %d %d; deviates requested %d (rows %d %d %d)", gals[0].Regime,
         gals[1].Regime, gals[2].Regime, gals[3].Regime, gals[4].Regime, s.requests,
         s.request_row[0], s.request_row[1], s.request_row[2]);
  TEST_ASSERT(s.requests == 3, "rows 0, 2 and 4 must each request one deviate");
  TEST_ASSERT(s.request_row[0] == 0 && s.request_row[1] == 2 && s.request_row[2] == 4,
              "deviates must be requested in row order");
  TEST_ASSERT(s.observed == 4 && !s.observed_row[1] && !s.observed_row[5],
              "the Type 3 and galaxy-less rows must not be classified");
  TEST_ASSERT(gals[0].Regime == 1 && gals[2].Regime == 0 && gals[3].Regime == 0 &&
                  gals[4].Regime == 1,
              "Types 0, 1 and 2 must be classified by their own deviate");
  TEST_ASSERT(gals[1].Regime == 7 && gals[5].Regime == 5, "skipped rows must keep their Regime");
  return TEST_PASS;
}

/**
 * @test    test_production_keyed_draw
 * @brief   Without a seam, the row's UniqueGalaxyID and ctx->snapshot_number key the deviate: with
 *          the reference deviates, one row flips between snapshots 37 and 38 and two IDs at
 *          snapshot 37 fall either side of the same hot_fraction
 */
int test_production_keyed_draw(void) {
  /* The reference vectors of the keyed draw: (ID, snapshot, ordinal 0) -> deviate */
  const double u_123_37 = 0x1.83e63f3b1c8a0p-6; /* (1000000000123, 37) ~0.0237 */
  const double u_123_38 = 0x1.90c26ed59643ap-2; /* (1000000000123, 38) ~0.391 */
  const double u_124_37 = 0x1.7c036bcbd4edap-2; /* (1000000000124, 37) ~0.371 */
  const float mvir = 31.8f;                     /* hot_fraction ~0.2 against Mshock 43.8 */
  const double p = sage26_hot_fraction(mvir, TEST_MSHOCK_MSUN, TEST_HUBBLE_H, NULL);
  printf("\n  hot_fraction %.17g; deviates %.17g %.17g %.17g", p, u_123_37, u_123_38, u_124_37);
  TEST_ASSERT(u_123_37 < p && !(u_123_38 < p) && !(u_124_37 < p),
              "the chosen mass must separate the reference deviates");

  const int snapshots[2] = {37, 38};
  const int expected_123[2] = {1, 0};
  for (int k = 0; k < 2; k++) {
    TEST_ASSERT(init_ok("1", TEST_HUBBLE_H) == 0, "init must succeed");
    struct Halo halos[2];
    struct GalaxyData gals[2];
    make_row(&halos[0], &gals[0], 0, (double)mvir, 1000000000123LL, -1);
    make_row(&halos[1], &gals[1], 1, (double)mvir, 1000000000124LL, -1);
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = &MimicConfig;
    ctx.snapshot_number = snapshots[k];
    ctx.num_substeps = 1;
    ctx.central_galaxy = &halos[0];
    sage26_determine_regime_set_test_seam(NULL);
    TEST_ASSERT(sage26_determine_regime_process(&ctx, halos, 2) == 0, "process must succeed");
    printf("\n  snapshot %d: Regime %d (ID ...123), %d (ID ...124)", snapshots[k], gals[0].Regime,
           gals[1].Regime);
    TEST_ASSERT(gals[0].Regime == expected_123[k],
                "ID 1000000000123 must be hot at snapshot 37 and CGM at 38");
    TEST_ASSERT(gals[1].Regime == 0, "ID 1000000000124 at snapshot 37 must be CGM");
  }
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_determine_regime Module\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("\n%s--- Software Quality Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_module_lifecycle);
  TEST_RUN(test_cgm_recipe_on_missing_refused);
  TEST_RUN(test_cgm_recipe_on_values);
  TEST_RUN(test_mshock_missing_refused);
  TEST_RUN(test_mshock_values);
  TEST_RUN(test_placement_refused);

  printf("\n%s--- Classifier Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_cgm_recipe_off_writes_nothing);
  TEST_RUN(test_mvir_zero_and_negative_no_deviate);
  TEST_RUN(test_mvir_nan_requests_deviate);
  TEST_RUN(test_ratio_above_at_below);
  TEST_RUN(test_ratio_one_tie);
  TEST_RUN(test_initial_boundary_row);
  TEST_RUN(test_every_type_and_skipped_rows);
  TEST_RUN(test_production_keyed_draw);

  TEST_SUMMARY();
  return TEST_RESULT();
}
