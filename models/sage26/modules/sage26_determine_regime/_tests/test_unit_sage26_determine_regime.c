/**
 * @file    test_unit_sage26_determine_regime.c
 * @brief   Behavioural tests of sage26_determine_regime
 *
 * Each runs the module's process() through its test seam (sage26_determine_regime.h): the seam
 * supplies the deviates and reports which rows requested one and each row's values. Every
 * expectation is derived independently of the module: literal inputs, or SAGE26's own expressions
 * written here with their operand order (the shock mass, the mass ratio, the sigmoid and the
 * comparison), never by calling the module's helpers.
 *
 * Software quality:
 * - test_module_lifecycle: module_system_init accepts the module in pre_timestep, cleanup, no leaks
 *
 * The module's parameter refusals are in test_unit_sage26_parameter_refusals.c, its placement
 * refusals in test_unit_sage_dependency_contracts.c.
 *
 * Behaviour:
 * - test_cgm_recipe_off_writes_nothing: CGMrecipeOn 0 writes nothing and requests no deviate
 * - test_mvir_zero_and_negative_no_deviate: Mvir +0, -0 and negative give Regime 0, no deviate
 * - test_mvir_nan_requests_deviate: a NaN Mvir requests a deviate and gives Regime 0
 * - test_ratio_above_at_below: mass ratios 2, ~1 and 1/2, the exact ratio 1 (Hubble_h 0.5: the
 *   tie) and a row in the initial-boundary state (stored SnapNum -1, dT 0), each against the
 *   deviates one step below, equal to and one step above SAGE26's sigmoid value (expected Regime
 *   1, 0, 0)
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

/* The mini-Millennium Hubble_h and SAGE26's default MShockMsun */
#define TEST_HUBBLE_H 0.73
#define TEST_MSHOCK_MSUN 6.0e11

// ============================================================================
// FIXTURES
// ============================================================================

/* The module's pre_timestep entry */
static char regime_name[] = "sage26_determine_regime";
static struct PhaseModuleConfig pre_timestep_entries[1];

/**
 * @brief Configure the module's run: pre_timestep entry and the two required parameters, MShockMsun
 *        at the run files' 6.0e11
 *
 * @param cgm       CGMrecipeOn's run-file text
 * @param hubble_h  The run's Hubble_h
 */
static void configure(const char *cgm, const double hubble_h) {
  reset_config();
  MimicConfig.Hubble_h = hubble_h;
  pre_timestep_entries[0].module_name = regime_name;
  pre_timestep_entries[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  pre_timestep_entries[0].resolved = NULL;
  MimicConfig.pre_timestep = pre_timestep_entries;
  MimicConfig.num_pre_timestep = 1;
  sage26_test_append_model_parameter("CGMrecipeOn", cgm);
  sage26_test_append_model_parameter("MShockMsun", "6.0e11");
}

/** @brief Configure the run for the given CGMrecipeOn and initialise the module */
static int init_ok(const char *cgm, const double hubble_h) {
  configure(cgm, hubble_h);
  return sage26_determine_regime_init();
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

/**
 * @brief SAGE26's sigmoid for a float Mvir, written as SAGE26 writes it
 *
 * MSUN_TO_CODE_MASS(MShockMsun, Hubble_h) = ((x) * (h) / 1.0e10), the ratio of the float Mvir to
 * it, x = log10(ratio) / delta_log_M with delta_log_M 0.1, hot_fraction = 1.0 / (1.0 + exp(-x)).
 *
 * The tests compare the module's result exactly and at one-ulp boundaries, so this expectation is
 * computed at run time by the same libm: the helper is never inlined and its inputs pass through
 * volatile locals, so the compiler cannot fold log10 and exp.
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
 * @brief   module_system_init accepts the module in pre_timestep with CGMrecipeOn 0 (SAGE26's
 *          default) and MShockMsun 6.0e11; cleanup leaves no leaks
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

// ============================================================================
// BEHAVIOURAL TESTS
// ============================================================================

/**
 * @test    test_cgm_recipe_off_writes_nothing
 * @brief   With CGMrecipeOn 0 the module writes nothing and requests no deviate: SAGE26 skips the
 *          classification
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
 * @brief   Rows of mass ratio 2, the float nearest 1 and 1/2, the exact ratio 1 (Hubble_h 0.5) and
 *          a row in the initial-boundary state (stored SnapNum -1, dT 0), each against the deviate
 *          one step below, equal to and one step above SAGE26's sigmoid value: Regime 1, 0 and 0
 *          (the comparison is a strict u < hot_fraction)
 */
int test_ratio_above_at_below(void) {
  /* Mshock = 6e11 * h / 1e10: 43.8 at h 0.73 and exactly 30 at h 0.5. The exact_tie row has the
   * ratio exactly 1 (Mvir 30), x exactly 0 and hot_fraction exactly 0.5: the tie u == hot_fraction
   * that no rand() value reaches */
  static const struct {
    float mvir;
    double hubble_h;
    int snap_num; /* the row's stored SnapNum; the classifier reads neither it nor dT */
    double dT;
    int snapshot; /* ctx->snapshot_number */
    int exact_tie;
  } rows[] = {
      {87.6f, TEST_HUBBLE_H, 62, 100.0, 63, 0}, /* ratio 2 */
      {43.8f, TEST_HUBBLE_H, 62, 100.0, 63, 0}, /* ratio ~1 */
      {21.9f, TEST_HUBBLE_H, 62, 100.0, 63, 0}, /* ratio 1/2 */
      {30.0f, 0.5, 62, 100.0, 63, 1},           /* ratio exactly 1 */
      {87.6f, TEST_HUBBLE_H, -1, 0.0, 0, 0},    /* initial-boundary state, ratio 2 */
  };
  const double direction[3] = {0.0, NAN, 1.0}; /* NAN: the deviate equal to hot_fraction */
  const int expected_regime[3] = {1, 0, 0};

  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    double expected_mshock = 0.0;
    const double p =
        sage26_hot_fraction(rows[r].mvir, TEST_MSHOCK_MSUN, rows[r].hubble_h, &expected_mshock);
    for (int d = 0; d < 3; d++) {
      char msg[160];
      TEST_ASSERT(init_ok("1", rows[r].hubble_h) == 0, "init must succeed");
      struct Halo halos[1];
      struct GalaxyData gals[1];
      make_row(&halos[0], &gals[0], 0, (double)rows[r].mvir, 41, -1);
      halos[0].SnapNum = rows[r].snap_num;
      halos[0].dT = rows[r].dT;
      struct recording_seam s;
      memset(&s, 0, sizeof(s));
      s.deviate_for_row[0] = isnan(direction[d]) ? p : nextafter(p, direction[d]);
      snprintf(msg, sizeof(msg), "row %zu, deviate %d: process must succeed", r, d);
      TEST_ASSERT(run_with_seam(halos, 1, &s, rows[r].snapshot) == 0, msg);
      printf("\n  Mvir %.9g (h %.2f): hot_fraction %.17g, deviate %.17g -> Regime %d",
             (double)rows[r].mvir, rows[r].hubble_h, p, s.deviate_for_row[0], gals[0].Regime);
      snprintf(msg, sizeof(msg), "row %zu, deviate %d: a positive Mvir must request one deviate", r,
               d);
      TEST_ASSERT(s.requests == 1, msg);
      snprintf(msg, sizeof(msg), "row %zu, deviate %d: Mshock must be MShockMsun * Hubble_h / 1e10",
               r, d);
      TEST_ASSERT(double_bits(s.trace[0].mshock) == double_bits(expected_mshock), msg);
      snprintf(msg, sizeof(msg), "row %zu, deviate %d: hot_fraction must be SAGE26's sigmoid", r,
               d);
      TEST_ASSERT(double_bits(s.trace[0].hot_fraction) == double_bits(p), msg);
      if (rows[r].exact_tie) {
        TEST_ASSERT(s.trace[0].mshock == 30.0 && s.trace[0].mass_ratio == 1.0,
                    "the tie row: Mshock must be 30 and the ratio exactly 1");
        TEST_ASSERT(s.trace[0].x == 0.0 && s.trace[0].hot_fraction == 0.5,
                    "the tie row: x must be 0 and hot_fraction exactly 0.5");
      }
      snprintf(msg, sizeof(msg),
               "row %zu, deviate %d: Regime must be 1 exactly when the deviate is below "
               "hot_fraction",
               r, d);
      TEST_ASSERT(gals[0].Regime == expected_regime[d], msg);
    }
  }
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

  printf("\n%s--- Classifier Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_cgm_recipe_off_writes_nothing);
  TEST_RUN(test_mvir_zero_and_negative_no_deviate);
  TEST_RUN(test_mvir_nan_requests_deviate);
  TEST_RUN(test_ratio_above_at_below);
  TEST_RUN(test_every_type_and_skipped_rows);
  TEST_RUN(test_production_keyed_draw);

  TEST_SUMMARY();
  return TEST_RESULT();
}
