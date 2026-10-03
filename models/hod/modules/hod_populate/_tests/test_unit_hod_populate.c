/**
 * @file    test_unit_hod_populate.c
 * @brief   Unit tests for the hod_populate module and the HOD random-number header
 *
 * Validates:
 * - hod_random.h: same key and index give the same value, different indices,
 *   keys, seeds, snapshots and host IDs give different values; uniforms lie
 *   strictly inside (0, 1) including at the extreme bit patterns; Poisson
 *   inversion against hand cases, the overflow signal and a mean whose
 *   exp(-lambda) underflows
 * - the hand-checkable spot values of the occupation law, the concentration,
 *   the NFW inverse, the periodic wrap and the physical-to-comoving offset;
 *   small concentrations against a 60-digit reference, and refusal of a
 *   degenerate concentration
 * - init(): the configuration guards (exactly one post_timestep entry, none in
 *   pre_timestep or a substep phase) and every parameter's domain, including
 *   the strings the strict parser accepts and the range checks would pass
 * - process(), driven through the registered dispatch (execute_module_pipeline()
 *   over hand-built FoF workspaces): Type 2 retirement, the ghost reset, the
 *   central gating, created satellites placed exactly as the helpers predict,
 *   the output-snapshot gate against MimicConfig, the identity-radix errors,
 *   placement at a tiny concentration normalisation,
 *   bitwise repeat identity, and invariance under row and FoF permutation
 * - statistics over 40,000 independently keyed hosts (central frequency,
 *   satellite count mean and variance, radial CDF, velocity variance), each
 *   bound derived where it is asserted
 * - process_snapshot(): the audit line against a hand sum, the per-bin lines,
 *   silence on non-output snapshots, no property writes and no net allocation
 *
 * Statistical bounds are fixed-seed: every draw is a pure function of its key,
 * so each statistic is one deterministic number. The bounds are stated at four
 * standard errors (or a DKW bound at tail probability 1e-6) so that a correct
 * generator passes for essentially any seed and a mis-specified law fails.
 */

#include "framework/test_framework.h"
#include "core/fof_workspace.h"
#include "core/galaxy_pool.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "framework/test_phase_config.h"
#include "include/constants.h"
#include "include/globals.h"
#include "include/types.h"
#include "io/vertical/reader.h"
#include "util/error.h"
#include "util/memory.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared HOD fixture boilerplate (counters, config reset, default parameters) */
#include "modules/_tests/hod_test_fixtures.h"

/* Model-private RNG and the module's helpers */
#include "modules/hod_populate/hod_populate.h"
#include "shared/hod_random.h"

extern int hod_populate_init(void);
extern int hod_populate_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int hod_populate_process_snapshot(const struct SnapshotContext *ctx,
                                         const struct Halo *halos, int64_t count);
extern int hod_populate_cleanup(void);

/**
 * Hosts in every statistical test: twice the 20,000 the HOD plan sets as a minimum, so the
 * central frequency's 4-SE band is +-0.01 and the radial and velocity tests see about
 * 215,000 satellites at 1e14 Msun/h
 */
#define STAT_HOSTS 40000

/** Synthetic identity space: unit = snapshot, HaloNr below TEST_ROWS_PER_UNIT */
#define TEST_ROWS_PER_UNIT 1000
#define TEST_UNITS 100

/** Largest workspace any dispatch test builds */
#define MAX_ROWS 8

/** Shipped default parameters as numbers, for the helper-level oracles */
static const struct HodParameters default_params = {
    12.02, 0.26, 11.38, 13.31, 1.06, 1, 5.71, 12.301030, -0.084, -0.47, HOD_TEST_BOX_SIZE,
};

/* ============================================================================
 * LOG CAPTURE
 * ============================================================================ */

static FILE *log_file = NULL;
static char log_text[1 << 16];

/** @brief Route every log line at INFO and above (and VERBOSE lines if asked) into a file */
static void capture_log(int verbose) {
  log_file = tmpfile();
  initialize_error_handling(LOG_LEVEL_INFO, log_file);
  set_verbose_format(verbose);
}

/** @brief Stop capturing and return what was logged */
static const char *captured_log(void) {
  log_text[0] = '\0';
  if (log_file != NULL) {
    fflush(log_file);
    rewind(log_file);
    const size_t n = fread(log_text, 1, sizeof(log_text) - 1, log_file);
    log_text[n] = '\0';
    fclose(log_file);
    log_file = NULL;
  }
  set_verbose_format(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  return log_text;
}

/* ============================================================================
 * WORKSPACE AND PIPELINE FIXTURES
 * ============================================================================ */

/** One synthetic workspace row; every row gets a galaxy unless no_galaxy is set */
struct RowSpec {
  int type;
  long long id;
  long long halonr;
  double mvir;
  float pos[3];
  float vel[3];
  bool no_galaxy;
};

static struct FoFWorkspace workspace;
static struct GalaxyPool *pool = NULL;
static struct ModuleContext context;

/**
 * @brief   Build a FoF workspace from row specs, the host at @p central_index
 *
 * Every row has the host's UniqueCentralGalaxyID, Rvir 0.25 and Vvir 200, and
 * HODGhost 0 so the reset is observable. The identity space has unit =
 * snapshot, so created IDs decode to (snapshot, host HaloNr, ordinal).
 */
static void build_workspace(const struct RowSpec *rows, int n, int central_index, int snapshot,
                            double redshift) {
  memset(&workspace, 0, sizeof(workspace));
  pool = galaxy_pool_create(64);
  workspace.halos = mymalloc_cat((size_t)n * sizeof(struct Halo), MEM_HALOS);
  memset(workspace.halos, 0, (size_t)n * sizeof(struct Halo));
  workspace.count = n;
  workspace.capacity = n;
  workspace.pool = pool;
  workspace.identity = (struct RecordIdentitySpace){
      .unit = snapshot, .rows_per_unit = TEST_ROWS_PER_UNIT, .fits = true, .units = TEST_UNITS};
  workspace.base_count = n;

  for (int i = 0; i < n; i++) {
    struct Halo *h = &workspace.halos[i];
    h->Type = rows[i].type;
    h->UniqueGalaxyID = rows[i].id;
    h->UniqueCentralGalaxyID = rows[central_index].id;
    h->HaloNr = rows[i].halonr;
    h->SnapNum = snapshot;
    h->Mvir = rows[i].mvir;
    h->Len = 100;
    h->Rvir = 0.25;
    h->Vvir = 200.0;
    h->Vmax = 220.0f;
    for (int j = 0; j < 3; j++) {
      h->Pos[j] = rows[i].pos[j];
      h->Vel[j] = rows[i].vel[j];
    }
    if (!rows[i].no_galaxy) {
      h->galaxy = galaxy_pool_alloc(pool);
      init_galaxy_defaults(h->galaxy);
      h->galaxy->HODGhost = 0;
    }
  }

  memset(&context, 0, sizeof(context));
  context.snapshot_number = snapshot;
  context.redshift = redshift;
  context.num_substeps = 1;
  context.central_index = central_index;
  context.central_galaxy = &workspace.halos[central_index];
  context.params = &MimicConfig;
}

static void free_workspace(void) {
  fof_workspace_destroy(&workspace);
  if (pool != NULL) {
    galaxy_pool_destroy(pool);
    pool = NULL;
  }
}

static void add_post_timestep(const char *module_name, enum ProcessingMode mode) {
  if (MimicConfig.post_timestep == NULL) {
    MimicConfig.post_timestep =
        mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.num_post_timestep = 0;
  }
  const int i = MimicConfig.num_post_timestep++;
  MimicConfig.post_timestep[i].module_name = strdup(module_name);
  MimicConfig.post_timestep[i].processing_mode = mode;
  MimicConfig.post_timestep[i].resolved = NULL;
}

/** @brief Default parameters, the module in post_timestep (and post_snapshot if asked) */
static void configure_run(bool with_audit) {
  set_hod_test_parameters(NULL, NULL);
  ensure_modules_registered();
  MimicConfig.ProcessingOrder =
      with_audit ? INPUT_PROCESSING_ORDER_HORIZONTAL : INPUT_PROCESSING_ORDER_VERTICAL;
  add_post_timestep("hod_populate", PROCESSING_MODE_FULL_HALO);
  if (with_audit) {
    test_post_snapshot_add("hod_populate", PROCESSING_MODE_SNAPSHOT);
  }
}

/** @brief Release everything a dispatch case holds; safe to repeat */
static int release_case(void) {
  if (log_file != NULL) {
    (void)captured_log();
  }
  const int rc = module_system_cleanup();
  free_workspace();
  module_release_record_creation_scratch();
  return rc;
}

/** @brief Mass in Msun/h of a workspace row */
static double row_mass(const struct Halo *h) { return h->Mvir * HOD_MASS_UNIT_MSUN; }

/** @brief The helper-level draw the module must reproduce for a host at a snapshot */
static void expected_occupation(const struct Halo *host, int snapshot, uint64_t *key,
                                struct HodOccupation *occupation) {
  *key = hod_random_key(default_params.seed, snapshot, host->UniqueGalaxyID);
  hod_populate_draw_occupation(&default_params, *key, row_mass(host), occupation);
}

/** @brief Find a host ID whose occupation at (mass, snapshot) satisfies @p want */
static long long find_host_id(double mass, int snapshot, long long first_id,
                              bool (*want)(const struct HodOccupation *)) {
  for (long long id = first_id; id < first_id + 100000; id++) {
    struct HodOccupation occupation;
    hod_populate_draw_occupation(&default_params, hod_random_key(default_params.seed, snapshot, id),
                                 mass, &occupation);
    if (want(&occupation)) {
      return id;
    }
  }
  return -1;
}

static bool central_absent(const struct HodOccupation *o) { return !o->central; }

static bool too_many_satellites(const struct HodOccupation *o) {
  return o->num_satellites > HOD_MAX_SATELLITES;
}

/* ============================================================================
 * RANDOM NUMBERS (hod_random.h)
 * ============================================================================ */

/**
 * @test   test_random_determinism
 * @brief  Same key and index repeat bitwise; indices, keys and every key component differ
 */
int test_random_determinism(void) {
  const uint64_t key = hod_random_key(1, 49, 123456789LL);
  TEST_ASSERT(key == hod_random_key(1, 49, 123456789LL), "the key is a pure function");
  for (uint64_t i = 0; i < 1000; i++) {
    TEST_ASSERT(hod_random_bits(key, i) == hod_random_bits(key, i), "same index repeats bitwise");
    TEST_ASSERT(hod_random_uniform(key, i) == hod_random_uniform(key, i),
                "same uniform repeats bitwise");
    TEST_ASSERT(hod_random_gaussian(key, i) == hod_random_gaussian(key, i),
                "same Gaussian repeats bitwise");
  }

  /* 64-bit outputs over 4096 indices: a collision would be a defect (probability ~5e-13). */
  static uint64_t seen[4096];
  for (uint64_t i = 0; i < 4096; i++) {
    seen[i] = hod_random_bits(key, i);
    for (uint64_t j = 0; j < i; j++) {
      if (seen[j] == seen[i]) {
        TEST_ASSERT(0, "different indices of one stream must give different values");
      }
    }
  }

  TEST_ASSERT(hod_random_key(2, 49, 123456789LL) != key, "the seed changes the key");
  TEST_ASSERT(hod_random_key(1, 48, 123456789LL) != key, "the snapshot changes the key");
  TEST_ASSERT(hod_random_key(1, 49, 123456790LL) != key, "the host ID changes the key");
  TEST_ASSERT(hod_random_key(1, 49, -123456789LL) != key, "a negative host ID is distinct");
  TEST_ASSERT(hod_random_bits(hod_random_key(2, 49, 123456789LL), 0) != hod_random_bits(key, 0),
              "different keys give different draws at the same index");
  return TEST_PASS;
}

/**
 * @test   test_random_open_interval
 * @brief  Uniforms are strictly inside (0, 1), at the extreme bit patterns and over 1e6 draws
 */
int test_random_open_interval(void) {
  const double lowest = hod_random_uniform_from_bits(0);
  const double highest = hod_random_uniform_from_bits(UINT64_MAX);
  TEST_ASSERT(lowest == 0x1.0p-53, "all-zero bits map to 2^-53");
  TEST_ASSERT(highest == 1.0 - 0x1.0p-53, "all-one bits map to 1 - 2^-53");
  TEST_ASSERT(lowest > 0.0 && highest < 1.0, "the extremes are inside (0, 1)");
  TEST_ASSERT(isfinite(log(lowest)) && isfinite(log(1.0 - highest)),
              "logarithms of the extremes are finite");

  const uint64_t key = hod_random_key(7, 3, 42);
  for (uint64_t i = 0; i < 1000000; i++) {
    const double u = hod_random_uniform(key, i);
    if (!(u > 0.0 && u < 1.0)) {
      TEST_ASSERT(0, "every uniform must lie strictly inside (0, 1)");
    }
  }
  return TEST_PASS;
}

/**
 * @test   test_random_poisson_inversion
 * @brief  Inversion against hand cases, the overflow signal and an underflowing exp(-lambda)
 */
int test_random_poisson_inversion(void) {
  TEST_ASSERT(hod_random_poisson(0.0, 0.999, 1024) == 0, "lambda 0 always gives 0");

  /* lambda = 1: P(0) = e^-1 = 0.367879, P(<=1) = 2 e^-1 = 0.735759, P(<=2) = 2.5 e^-1. */
  const double e1 = exp(-1.0);
  TEST_ASSERT(hod_random_poisson(1.0, e1 - 1e-9, 1024) == 0, "u just below P(0) gives 0");
  TEST_ASSERT(hod_random_poisson(1.0, e1 + 1e-9, 1024) == 1, "u just above P(0) gives 1");
  TEST_ASSERT(hod_random_poisson(1.0, 2.0 * e1 + 1e-9, 1024) == 2, "u above P(<=1) gives 2");
  TEST_ASSERT(hod_random_poisson(1.0, 2.5 * e1 - 1e-9, 1024) == 2, "u below P(<=2) gives 2");

  /* The cap: lambda = 3 needs k = 3 for u = 0.6 (P(<=2) = 0.4232, P(<=3) = 0.6472). */
  TEST_ASSERT(hod_random_poisson(3.0, 0.6, 3) == 3, "a count equal to the cap is returned");
  TEST_ASSERT(hod_random_poisson(3.0, 0.6, 2) == 3, "a count above the cap returns cap + 1");

  /* lambda = 800: exp(-800) underflows to 0, yet the median is within one of 800. */
  const int median = hod_random_poisson(800.0, 0.5, 1024);
  TEST_ASSERT(median >= 799 && median <= 800, "log-space terms give the median of Poisson(800)");

  /* u within rounding of 1 terminates on the exhausted tail instead of running to the cap. */
  const int tail = hod_random_poisson(5.0, 1.0 - 0x1.0p-53, 1024);
  TEST_ASSERT(tail > 15 && tail < 60, "an extreme u stops in the far tail, not at the cap");
  return TEST_PASS;
}

/* ============================================================================
 * SPOT VALUES
 * ============================================================================ */

/**
 * @test   test_occupation_spot_values
 * @brief  <Ncen> and lambda at the contract's masses for the Mr < -20 parameters
 */
int test_occupation_spot_values(void) {
  const struct HodParameters *p = &default_params;
  TEST_ASSERT(fabs(hod_populate_mean_ncen(p, pow(10.0, 12.02)) - 0.5) <= 1e-12,
              "<Ncen>(10^12.02) = 0.5");
  TEST_ASSERT(fabs(hod_populate_mean_ncen(p, 1e14) - 1.0) <= 1e-12, "<Ncen>(1e14) = 1");
  TEST_ASSERT(hod_populate_mean_ncen(p, 0.0) == 0.0, "a zero mass has no central");
  TEST_ASSERT(fabs(hod_populate_lambda(p, 1e13) - 0.457322) <= 1e-6, "lambda(1e13) = 0.457322");
  TEST_ASSERT(fabs(hod_populate_lambda(p, 1e14) - 5.373959) <= 1e-6, "lambda(1e14) = 5.373959");
  TEST_ASSERT(fabs(hod_populate_lambda(p, 1e15) - 61.842859) <= 1e-6, "lambda(1e15) = 61.842859");
  TEST_ASSERT(hod_populate_lambda(p, pow(10.0, 11.38)) == 0.0, "lambda is 0 at M = 10^HODLogM0");
  TEST_ASSERT(hod_populate_lambda(p, 1e11) == 0.0, "lambda is 0 below 10^HODLogM0");
  return TEST_PASS;
}

/**
 * @test   test_concentration_spot_values
 * @brief  Duffy et al. (2008) concentration at the contract's masses and redshifts
 */
int test_concentration_spot_values(void) {
  const struct HodParameters *p = &default_params;
  TEST_ASSERT(fabs(hod_populate_concentration(p, 2e12, 0.0) - 5.71) <= 1e-6, "c(2e12, 0) = 5.71");
  TEST_ASSERT(fabs(hod_populate_concentration(p, 1e14, 0.0) - 4.110765) <= 1e-6,
              "c(1e14, 0) = 4.110765");
  TEST_ASSERT(fabs(hod_populate_concentration(p, 1e12, 1.0) - 4.369568) <= 1e-6,
              "c(1e12, 1) = 4.369568");
  return TEST_PASS;
}

/**
 * @test   test_nfw_inverse_spot_values
 * @brief  x(u, c) at the contract's points to 1e-9, and the residual tolerance everywhere
 */
int test_nfw_inverse_spot_values(void) {
  double x = 0.0;
  TEST_ASSERT(hod_populate_nfw_inverse(0.5, 5.0, &x) == 0 && fabs(x - 2.2166041757) <= 1e-9,
              "x(0.5, 5) = 2.2166041757");
  TEST_ASSERT(hod_populate_nfw_inverse(0.1, 10.0, &x) == 0 && fabs(x - 0.8223966449) <= 1e-9,
              "x(0.1, 10) = 0.8223966449");
  TEST_ASSERT(hod_populate_nfw_inverse(0.9, 5.71, &x) == 0 && fabs(x - 4.9207968140) <= 1e-9,
              "x(0.9, 5.71) = 4.9207968140");

  static const double us[] = {0x1.0p-53, 1e-6, 0.25, 0.75, 0.999999, 1.0 - 0x1.0p-53};
  static const double cs[] = {0.5, 4.110765, 30.0};
  for (size_t i = 0; i < sizeof(us) / sizeof(us[0]); i++) {
    for (size_t k = 0; k < sizeof(cs) / sizeof(cs[0]); k++) {
      TEST_ASSERT(hod_populate_nfw_inverse(us[i], cs[k], &x) == 0, "the inverse converges");
      const double residual = (log1p(x) - x / (1.0 + x)) / (log1p(cs[k]) - cs[k] / (1.0 + cs[k]));
      TEST_ASSERT(x >= 0.0 && x <= cs[k], "x stays in [0, c]");
      TEST_ASSERT(fabs(residual - us[i]) <= 1e-12, "|m(x)/m(c) - u| <= 1e-12");
    }
  }
  return TEST_PASS;
}

/** One small-concentration reference: u, c and x/c from an independent 60-digit computation */
struct NfwReference {
  double u;
  double c;
  double x_over_c;
};

/**
 * @test   test_nfw_small_concentration
 * @brief  Small concentrations solve to the 60-digit reference; degenerate inputs are refused
 *
 * References: Python decimal at 60 significant digits, m(x) = ln(1 + x) - x/(1 + x)
 * with exact decimal ln, bisection on [0, c] for 400 halvings (bracket below 1e-120 c):
 *   c = 1e-6: x/c = 0.31622762186508335 (u 0.1), 0.70710664311541649 (0.5), 0.94868326559499083
 * (0.9) c = 1e-3: x/c = 0.31608370353845688, 0.70696876639232642, 0.94865085153072204 c = 0.1:  x/c
 * = 0.30264840236102647, 0.69383592092457724, 0.94552429531711990 (the small-c limit x/c -> sqrt(u)
 * = 0.316228, 0.707107, 0.948683 is visible at c = 1e-6). The bisection stops at |m(x)/m(c) - u| <=
 * 1e-12 and dF/d(x/c) is about 2 (x/c) >= 0.6 here, so x/c is within ~2e-12 of the reference; 1e-10
 * is asserted. Directly evaluated, log1p(c) - c/(1 + c) loses all its digits near c = 1e-6 and the
 * old bisection stalled with residual ~3e-10.
 */
int test_nfw_small_concentration(void) {
  static const struct NfwReference refs[] = {
      {0.1, 1e-6, 0.31622762186508335}, {0.5, 1e-6, 0.70710664311541649},
      {0.9, 1e-6, 0.94868326559499083}, {0.1, 1e-3, 0.31608370353845688},
      {0.5, 1e-3, 0.70696876639232642}, {0.9, 1e-3, 0.94865085153072204},
      {0.1, 0.1, 0.30264840236102647},  {0.5, 0.1, 0.69383592092457724},
      {0.9, 0.1, 0.94552429531711990},
  };
  for (size_t k = 0; k < sizeof(refs) / sizeof(refs[0]); k++) {
    double x = -1.0;
    TEST_ASSERT(hod_populate_nfw_inverse(refs[k].u, refs[k].c, &x) == 0,
                "a small concentration converges");
    TEST_ASSERT(fabs(x / refs[k].c - refs[k].x_over_c) <= 1e-10,
                "x/c matches the 60-digit reference to 1e-10");
    TEST_ASSERT(fabs(hod_populate_nfw_fraction(x, refs[k].c) - refs[k].u) <= 1e-12,
                "the stable fraction meets the tolerance at the solution");
  }

  /* The fraction is continuous across the series switch at 0.1 (both forms agree there). */
  TEST_ASSERT(fabs(hod_populate_nfw_fraction(0.0999999999, 0.2) -
                   hod_populate_nfw_fraction(0.1000000001, 0.2)) <= 1e-8,
              "the series and direct forms agree at the switch");
  TEST_ASSERT(hod_populate_nfw_fraction(0.0, 1e-6) == 0.0, "the fraction is 0 at x = 0");
  TEST_ASSERT(fabs(hod_populate_nfw_fraction(1e-300, 1e-300) - 1.0) <= 1e-15,
              "a concentration far below double resolution of 1 neither cancels nor underflows");

  static const double degenerate_c[] = {0.0, -1.0, NAN, INFINITY, -INFINITY};
  for (size_t k = 0; k < sizeof(degenerate_c) / sizeof(degenerate_c[0]); k++) {
    double x = -7.0;
    TEST_ASSERT(hod_populate_nfw_inverse(0.5, degenerate_c[k], &x) != 0,
                "a concentration that is not finite and positive is refused");
    TEST_ASSERT(x == -7.0, "a refused inverse writes no radius");
  }
  double x = -7.0;
  TEST_ASSERT(hod_populate_nfw_inverse(NAN, 5.0, &x) != 0 && x == -7.0, "a NaN u is refused");
  TEST_ASSERT(hod_populate_nfw_inverse(1.5, 5.0, &x) != 0 && x == -7.0, "u > 1 is refused");

  struct HodSatellite sat;
  TEST_ASSERT(hod_populate_draw_satellite(hod_random_key(1, 5, 9), 0, 0.0, 0.3, 200.0, 0.0, &sat) !=
                  0,
              "a satellite draw at a degenerate concentration fails");
  TEST_ASSERT(sat.u_radius > 0.0 && sat.u_radius < 1.0, "and reports its radius uniform");
  return TEST_PASS;
}

/**
 * @test   test_position_wrap
 * @brief  Exactly BoxSize wraps to 0; -1e-9 wraps to BoxSize - 1e-9; stored floats stay inside
 */
int test_position_wrap(void) {
  const double box = HOD_TEST_BOX_SIZE;
  TEST_ASSERT(hod_populate_wrap(box, box) == 0.0, "a component exactly at BoxSize wraps to 0");
  TEST_ASSERT(hod_populate_wrap(2.0 * box, box) == 0.0, "a multiple of BoxSize wraps to 0");
  TEST_ASSERT(hod_populate_wrap(0.0, box) == 0.0, "0 stays 0");
  TEST_ASSERT(hod_populate_wrap(37.5, box) == 37.5, "an inside component is unchanged");
  TEST_ASSERT(hod_populate_wrap(box + 2.5, box) == 2.5, "an overshoot wraps by one box");
  TEST_ASSERT(hod_populate_wrap(-2.5, box) == box - 2.5, "an undershoot wraps by one box");

  const double wrapped = hod_populate_wrap(-1e-9, box);
  TEST_ASSERT(wrapped < box, "the wrapped double is below BoxSize");
  TEST_ASSERT(fabs(wrapped - (box - 1e-9)) <= 1e-12, "-1e-9 wraps to BoxSize - 1e-9 in double");
  const float stored = hod_populate_store_position(wrapped, box);
  const float one_ulp_below = nextafterf((float)box, 0.0f);
  TEST_ASSERT((double)stored < box, "the stored float is below BoxSize");
  TEST_ASSERT(stored == one_ulp_below, "it is the float nearest BoxSize - 1e-9 from below");
  TEST_ASSERT(fabs((double)stored - (box - 1e-9)) <= (double)(box - one_ulp_below),
              "the stored value is within one float rounding of BoxSize - 1e-9");
  TEST_ASSERT(hod_populate_store_position(0.0, box) == 0.0f, "0 stores as 0");
  return TEST_PASS;
}

/**
 * @test   test_comoving_offset
 * @brief  At z = 1 the comoving offset is twice the physical radius; at z = 0 they are equal
 */
int test_comoving_offset(void) {
  const uint64_t key = hod_random_key(1, 10, 777);
  const double c = hod_populate_concentration(&default_params, 1e14, 1.0);
  for (int s = 0; s < 16; s++) {
    struct HodSatellite at_z1;
    struct HodSatellite at_z0;
    TEST_ASSERT(hod_populate_draw_satellite(key, s, c, 0.3, 300.0, 1.0, &at_z1) == 0 &&
                    hod_populate_draw_satellite(key, s, c, 0.3, 300.0, 0.0, &at_z0) == 0,
                "the draws succeed");
    const double norm = sqrt(at_z1.offset[0] * at_z1.offset[0] + at_z1.offset[1] * at_z1.offset[1] +
                             at_z1.offset[2] * at_z1.offset[2]);
    TEST_ASSERT(at_z1.r_com == 2.0 * at_z1.r_phys, "at z = 1, r_com = 2 r_phys exactly");
    TEST_ASSERT(fabs(norm - at_z1.r_com) <= 1e-12 * (1.0 + at_z1.r_com),
                "the offset vector's length is r_com");
    TEST_ASSERT(at_z0.r_com == at_z0.r_phys, "at z = 0, r_com = r_phys");
    TEST_ASSERT(fabs(at_z1.r_phys - 0.3 * at_z1.x / c) <= 1e-15, "r_phys = Rvir x / c");
    TEST_ASSERT(at_z1.r_phys <= 0.3 * (1.0 + 1e-15), "a satellite lies inside Rvir");
  }
  return TEST_PASS;
}

/* ============================================================================
 * INIT: CONFIGURATION AND PARAMETERS
 * ============================================================================ */

/**
 * @test   test_init_configuration
 * @brief  One post_timestep process_full_halo entry, no other FoF entry; post_snapshot, if present,
 * has the audit
 */
int test_init_configuration(void) {
  ensure_modules_registered();

  /* Vertical style: post_timestep only, no post_snapshot phase. */
  configure_run(false);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "post_timestep alone is a valid configuration");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");

  /* Horizontal style: post_timestep plus the audit. */
  configure_run(true);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "post_timestep plus post_snapshot is valid");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");

  /* Absent from post_timestep (direct init: nothing else is configured). */
  set_hod_test_parameters(NULL, NULL);
  TEST_ASSERT(hod_populate_init() != 0, "init fails without a post_timestep entry");

  /* Only in post_snapshot. */
  set_hod_test_parameters(NULL, NULL);
  test_post_snapshot_add("hod_populate", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(module_system_init() != 0, "the audit alone is rejected");
  module_system_cleanup();

  /* post_timestep with the wrong mode. */
  set_hod_test_parameters(NULL, NULL);
  add_post_timestep("hod_populate", PROCESSING_MODE_BY_GALAXY);
  TEST_ASSERT(module_system_init() != 0, "post_timestep as process_by_galaxy is rejected");
  module_system_cleanup();

  /* Twice in post_timestep. */
  configure_run(false);
  add_post_timestep("hod_populate", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "two post_timestep entries are rejected");
  module_system_cleanup();

  /* Also in another FoF phase: an extra entry would retire and redraw the FoF step again. */
  configure_run(false);
  test_pre_timestep_add("hod_populate", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a pre_timestep entry as well is rejected");
  module_system_cleanup();

  configure_run(false);
  test_phase_add("galaxy_physics", "hod_populate", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a substep-phase entry as well is rejected");
  module_system_cleanup();

  /* post_snapshot configured without this module (direct init: the registry would reject
     an unknown module name before init() runs). */
  configure_run(false);
  test_post_snapshot_add("some_other_snapshot_module", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(hod_populate_init() != 0, "post_snapshot without the audit is rejected");
  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Run init() with every default except one parameter; returns its result */
static int init_with(const char *name, const char *value) {
  set_hod_test_parameters(name, value);
  add_post_timestep("hod_populate", PROCESSING_MODE_FULL_HALO);
  const int rc = hod_populate_init();
  hod_populate_cleanup();
  module_system_cleanup(); /* frees the phase configuration */
  return rc;
}

/**
 * @test   test_init_parameter_domain
 * @brief  Non-finite, malformed, missing and out-of-range values fail; boundary values pass
 */
int test_init_parameter_domain(void) {
  ensure_modules_registered();
  TEST_ASSERT_EQUAL(init_with(NULL, NULL), 0, "the shipped defaults initialise");

  static const char *const doubles[] = {
      "HODLogMmin", "HODSigmaLogM",     "HODLogM0", "HODLogM1", "HODAlpha",
      "HODConcA",   "HODConcLogMpivot", "HODConcB", "HODConcC",
  };
  static const char *const malformed[] = {"nan", "inf", "-inf", "1e400", "1,0", "abc", ""};
  for (size_t k = 0; k < sizeof(doubles) / sizeof(doubles[0]); k++) {
    for (size_t m = 0; m < sizeof(malformed) / sizeof(malformed[0]); m++) {
      if (init_with(doubles[k], malformed[m]) == 0) {
        fprintf(stderr, "%s='%s' was accepted\n", doubles[k], malformed[m]);
        TEST_ASSERT(0, "every non-finite or malformed double must fail init");
      }
    }
    TEST_ASSERT(init_with(doubles[k], NULL) != 0, "a missing double parameter fails init");
  }
  for (int k = 0; k < 10; k++) {
    TEST_ASSERT(init_with(hod_parameter_names[k], NULL) != 0, "every parameter is required");
  }

  TEST_ASSERT(init_with("HODSigmaLogM", "0") != 0, "HODSigmaLogM = 0 is rejected");
  TEST_ASSERT(init_with("HODSigmaLogM", "-0.1") != 0, "HODSigmaLogM < 0 is rejected");
  TEST_ASSERT(init_with("HODSigmaLogM", "1e-6") == 0, "a small positive HODSigmaLogM passes");
  TEST_ASSERT(init_with("HODAlpha", "-0.01") != 0, "HODAlpha < 0 is rejected");
  TEST_ASSERT(init_with("HODAlpha", "0") == 0, "HODAlpha = 0 passes");
  TEST_ASSERT(init_with("HODSeed", "-1") != 0, "HODSeed < 0 is rejected");
  TEST_ASSERT(init_with("HODSeed", "0") == 0, "HODSeed = 0 passes");
  TEST_ASSERT(init_with("HODSeed", "1.5") != 0, "a non-integer HODSeed is rejected");
  TEST_ASSERT(init_with("HODSeed", "abc") != 0, "a malformed HODSeed is rejected");
  TEST_ASSERT(init_with("HODConcA", "0") != 0, "HODConcA = 0 is rejected");
  TEST_ASSERT(init_with("HODConcA", "-5.71") != 0, "HODConcA < 0 is rejected");
  TEST_ASSERT(init_with("HODLogM1", "400") != 0, "a log mass whose power overflows is rejected");
  TEST_ASSERT(init_with("HODLogM0", "-400") != 0, "a log mass whose power underflows is rejected");

  static const double boxes[] = {0.0, -100.0, NAN, INFINITY, 1e200};
  for (size_t k = 0; k < sizeof(boxes) / sizeof(boxes[0]); k++) {
    set_hod_test_parameters(NULL, NULL);
    MimicConfig.BoxSize = boxes[k];
    add_post_timestep("hod_populate", PROCESSING_MODE_FULL_HALO);
    TEST_ASSERT(hod_populate_init() != 0,
                "a non-positive, non-finite or cube-overflowing box fails");
    module_system_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * PROCESS: VALIDATION, LIFECYCLE AND GATING
 * ============================================================================ */

/** Host at index 0 with M = 1e15 Msun/h (<Ncen> = 1), a Type 1 row, an inherited Type 2, a Type 3
 */
static const struct RowSpec mixed_rows[] = {
    {0, 5001, 3, 1.0e5, {10.0f, 20.0f, 30.0f}, {100.0f, -50.0f, 25.0f}, false},
    {1, 5002, 4, 30.0, {10.5f, 20.5f, 30.5f}, {10.0f, 0.0f, 0.0f}, false},
    {2, 5003, 5, 0.0, {11.0f, 21.0f, 31.0f}, {0.0f, 5.0f, 0.0f}, false},
    {3, 5004, 6, 0.0, {12.0f, 22.0f, 32.0f}, {0.0f, 0.0f, 5.0f}, false},
};

/** @brief Whether the workspace still holds exactly the rows build_workspace() wrote */
static bool workspace_untouched(const struct RowSpec *rows, int n) {
  if (workspace.count != n) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    if (workspace.halos[i].Type != rows[i].type) {
      return false;
    }
    if (workspace.halos[i].galaxy != NULL && workspace.halos[i].galaxy->HODGhost != 0) {
      return false;
    }
  }
  return true;
}

/**
 * @test   test_process_input_validation
 * @brief  Bad calls fail before any write: no workspace, bad central, missing galaxy, bad mass
 */
int test_process_input_validation(void) {
  configure_run(false);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");

  build_workspace(mixed_rows, 4, 0, 5, 0.0);
  TEST_ASSERT(hod_populate_process(NULL, workspace.halos, 4) != 0, "a NULL context fails");
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 0) != 0, "ngal = 0 fails");
  TEST_ASSERT(hod_populate_process(&context, NULL, 4) != 0, "a NULL workspace fails");
  context.central_index = 4;
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "a central index past the workspace fails");
  context.central_index = -1;
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "a negative central index fails");
  context.central_index = 1;
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "a Type 1 row at central_index fails");
  TEST_ASSERT(workspace_untouched(mixed_rows, 4), "failed calls write nothing");
  free_workspace();

  struct RowSpec rows[4];
  memcpy(rows, mixed_rows, sizeof(rows));
  rows[0].no_galaxy = true;
  build_workspace(rows, 4, 0, 5, 0.0);
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "a host without a galaxy fails");
  free_workspace();

  memcpy(rows, mixed_rows, sizeof(rows));
  rows[1].no_galaxy = true;
  build_workspace(rows, 4, 0, 5, 0.0);
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "a live row without a galaxy fails");
  TEST_ASSERT(workspace.halos[2].Type == 2, "the inherited orphan is not retired by a failure");
  free_workspace();

  memcpy(rows, mixed_rows, sizeof(rows));
  rows[0].mvir = NAN;
  build_workspace(rows, 4, 0, 5, 0.0);
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "a non-finite host mass fails on an output snapshot");
  TEST_ASSERT(workspace_untouched(rows, 4), "and writes nothing");
  free_workspace();

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  build_workspace(mixed_rows, 4, 0, 5, 0.0);
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 4) != 0,
              "process fails before (or after) init");
  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @brief Check every created row of the workspace against the helper-level draw
 *
 * Rows [base, count) must be Type 2 sample members with the host's
 * UniqueCentralGalaxyID, a created ID decoding to (snapshot, host HaloNr,
 * ordinal), and exactly the helper's wrapped position and velocity.
 */
static int check_created_rows(const struct Halo *host_before, int snapshot, double redshift) {
  uint64_t key = 0;
  struct HodOccupation occupation;
  expected_occupation(host_before, snapshot, &key, &occupation);
  TEST_ASSERT_EQUAL(workspace.count - workspace.base_count, (int64_t)occupation.num_satellites,
                    "the module creates exactly the drawn number of satellites");
  const double c = hod_populate_concentration(&default_params, row_mass(host_before), redshift);
  for (int64_t r = workspace.base_count; r < workspace.count; r++) {
    const struct Halo *row = &workspace.halos[r];
    const int s = (int)(r - workspace.base_count);
    struct HodSatellite sat;
    TEST_ASSERT(hod_populate_draw_satellite(key, s, c, host_before->Rvir, host_before->Vvir,
                                            redshift, &sat) == 0,
                "the helper-level draw succeeds");
    TEST_ASSERT(row->Type == 2, "a satellite is a Type 2 row");
    TEST_ASSERT(row->galaxy != NULL && row->galaxy->HODGhost == 0, "a satellite is in the sample");
    TEST_ASSERT(row->UniqueCentralGalaxyID == host_before->UniqueGalaxyID,
                "a satellite's UniqueCentralGalaxyID is its host's");
    const int64_t k = -row->UniqueGalaxyID - 1;
    TEST_ASSERT(row->UniqueGalaxyID < 0, "a satellite has a created (negative) ID");
    TEST_ASSERT(k % MAX_CREATED_RECORDS_PER_HOST == s, "ordinals count satellites in order");
    TEST_ASSERT((k / MAX_CREATED_RECORDS_PER_HOST) % TEST_ROWS_PER_UNIT == host_before->HaloNr,
                "the created ID names the host's HaloNr");
    TEST_ASSERT((k / MAX_CREATED_RECORDS_PER_HOST) / TEST_ROWS_PER_UNIT == snapshot,
                "the created ID names the snapshot's unit");
    for (int j = 0; j < 3; j++) {
      const double wrapped =
          hod_populate_wrap((double)host_before->Pos[j] + sat.offset[j], HOD_TEST_BOX_SIZE);
      TEST_ASSERT(row->Pos[j] == hod_populate_store_position(wrapped, HOD_TEST_BOX_SIZE),
                  "the position is the host's plus the comoving offset, wrapped");
      TEST_ASSERT(row->Pos[j] >= 0.0f && (double)row->Pos[j] < HOD_TEST_BOX_SIZE,
                  "the position is inside [0, BoxSize)");
      TEST_ASSERT(row->Vel[j] == (float)((double)host_before->Vel[j] + sat.velocity[j]),
                  "the velocity is the host's plus the Gaussian offset");
    }
  }
  return TEST_PASS;
}

/**
 * @test   test_process_lifecycle
 * @brief  Retire Type 2, reset ghosts, keep Type 1 scaffold, flag the central, create satellites
 */
int test_process_lifecycle(void) {
  configure_run(false);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(mixed_rows, 4, 0, 5, 0.5);
  const struct Halo host_before = workspace.halos[0];

  execute_module_pipeline(&context, &workspace);

  TEST_ASSERT(workspace.halos[0].Type == 0 && workspace.halos[0].galaxy->HODGhost == 0,
              "a present central (<Ncen> = 1) is a sample member");
  TEST_ASSERT(workspace.halos[1].Type == 1 && workspace.halos[1].galaxy->HODGhost == 1,
              "a Type 1 row stays scaffold");
  TEST_ASSERT(workspace.halos[2].Type == 3 && workspace.halos[2].galaxy->HODGhost == 1,
              "an inherited Type 2 row is retired to Type 3 and reset");
  TEST_ASSERT(workspace.halos[3].Type == 3 && workspace.halos[3].galaxy->HODGhost == 1,
              "a Type 3 row stays retired and is reset");
  TEST_ASSERT(workspace.count > workspace.base_count, "M = 1e15 creates satellites");
  TEST_ASSERT(check_created_rows(&host_before, 5, 0.5) == TEST_PASS,
              "created rows match the helper-level draw");

  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_absent_central_creates_nothing
 * @brief  Central gating: a host whose central is drawn absent keeps HODGhost = 1 and gets no
 * satellite
 */
int test_absent_central_creates_nothing(void) {
  /* At M = 10^12.5, lambda = 0.13 > 0 but <Ncen> = 0.9907 < 1: find a key whose u0 >= <Ncen>. */
  const double mass = pow(10.0, 12.5);
  TEST_ASSERT(hod_populate_lambda(&default_params, mass) > 0.0, "the mass has a satellite mean");
  const long long id = find_host_id(mass, 5, 700000, central_absent);
  TEST_ASSERT(id > 0, "some host draws no central");

  configure_run(false);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  struct RowSpec rows[2] = {
      {0, id, 1, mass / HOD_MASS_UNIT_MSUN, {50.0f, 50.0f, 50.0f}, {0.0f, 0.0f, 0.0f}, false},
      {2, id + 1, 2, 0.0, {50.0f, 50.0f, 50.0f}, {0.0f, 0.0f, 0.0f}, false},
  };
  build_workspace(rows, 2, 0, 5, 0.0);
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT(workspace.halos[0].galaxy->HODGhost == 1, "an absent central stays scaffold");
  TEST_ASSERT(workspace.halos[1].Type == 3, "the orphan is still retired");
  TEST_ASSERT_EQUAL(workspace.count, 2, "no satellite is created without a central");
  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_output_snapshot_gating
 * @brief  A list naming one snapshot draws only there; an empty list draws everywhere
 */
int test_output_snapshot_gating(void) {
  configure_run(false);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 7;
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");

  build_workspace(mixed_rows, 4, 0, 5, 0.0);
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT(workspace.halos[2].Type == 3, "a non-output snapshot still retires Type 2");
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT(workspace.halos[i].galaxy->HODGhost == 1,
                "a non-output snapshot resets every row, the certain central included");
  }
  TEST_ASSERT_EQUAL(workspace.count, 4, "a non-output snapshot creates nothing");
  free_workspace();

  build_workspace(mixed_rows, 4, 0, 7, 0.0);
  const struct Halo host_at_7 = workspace.halos[0];
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT(workspace.halos[0].galaxy->HODGhost == 0, "the listed snapshot draws the central");
  TEST_ASSERT(workspace.count > 4, "the listed snapshot creates satellites");
  TEST_ASSERT(check_created_rows(&host_at_7, 7, 0.0) == TEST_PASS, "with the expected draws");
  free_workspace();

  MimicConfig.NOUT = 0; /* an empty output.snapshot_list: every snapshot is an output */
  build_workspace(mixed_rows, 4, 0, 5, 0.0);
  const struct Halo host_at_5 = workspace.halos[0];
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT(workspace.halos[0].galaxy->HODGhost == 0, "an empty list draws at snapshot 5");
  TEST_ASSERT(check_created_rows(&host_at_5, 5, 0.0) == TEST_PASS, "with the expected draws");

  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_satellite_limit_errors
 * @brief  lambda >= 1024 and a drawn Nsat > 1024 fail with the host ID and lambda, writing nothing
 */
int test_satellite_limit_errors(void) {
  configure_run(false);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");

  /* M = 1e18 Msun/h: lambda = (1e18 - 10^11.38)/10^13.31)^1.06 = 93626.7. */
  struct RowSpec rows[2] = {
      {0, 880001, 1, 1.0e8, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, false},
      {2, 880002, 2, 0.0, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, false},
  };
  build_workspace(rows, 2, 0, 5, 0.0);
  capture_log(0);
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 2) != 0, "lambda >= 1024 fails");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "UniqueGalaxyID 880001") != NULL, "the message names the host ID");
  TEST_ASSERT(strstr(log, "lambda=93626.7") != NULL, "the message carries lambda");
  TEST_ASSERT(workspace_untouched(rows, 2), "a refused host writes nothing");
  free_workspace();

  /* lambda = 1020 < 1024: about 45% of keys draw Nsat > 1024 (Poisson(1020) has sigma 32). */
  const double mass =
      pow(10.0, 13.31) * pow(1020.0, 1.0 / 1.06) + pow(10.0, 11.38); /* lambda(mass) = 1020 */
  TEST_ASSERT(fabs(hod_populate_lambda(&default_params, mass) - 1020.0) < 1e-6,
              "the chosen mass has lambda = 1020");
  const long long id = find_host_id(mass, 5, 900000, too_many_satellites);
  TEST_ASSERT(id > 0, "some host draws more than 1024 satellites");
  rows[0].id = id;
  rows[0].mvir = mass / HOD_MASS_UNIT_MSUN;
  build_workspace(rows, 2, 0, 5, 0.0);
  capture_log(0);
  TEST_ASSERT(hod_populate_process(&context, workspace.halos, 2) != 0, "a drawn Nsat > 1024 fails");
  log = captured_log();
  char expected_id[64];
  snprintf(expected_id, sizeof(expected_id), "UniqueGalaxyID %lld", id);
  TEST_ASSERT(strstr(log, expected_id) != NULL, "the message names the host ID");
  TEST_ASSERT(strstr(log, "lambda=1020") != NULL, "the message carries lambda");
  TEST_ASSERT(workspace_untouched(rows, 2), "a refused draw writes nothing");

  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_small_concentration_placement
 * @brief  With HODConcA = 1e-6 (and no mass or redshift slope) satellites are not placed at the
 * host
 *
 * c = 1e-6 for every host, so x/c is about sqrt(u) and each satellite sits at a
 * physical radius of order Rvir (0.25 here), not at zero; positions equal the
 * helper-level draw at that concentration.
 */
int test_small_concentration_placement(void) {
  configure_run(false);
  snprintf(MimicConfig.ModelParams[6].value, MAX_STRING_LEN, "%s", "1e-6"); /* HODConcA */
  snprintf(MimicConfig.ModelParams[8].value, MAX_STRING_LEN, "%s", "0");    /* HODConcB */
  snprintf(MimicConfig.ModelParams[9].value, MAX_STRING_LEN, "%s", "0");    /* HODConcC */
  TEST_ASSERT_EQUAL(module_system_init(), 0, "a tiny concentration normalisation initialises");
  build_workspace(mixed_rows, 4, 0, 5, 0.0);
  const struct Halo host = workspace.halos[0];
  execute_module_pipeline(&context, &workspace);

  struct HodParameters p = default_params;
  p.conc_a = 1e-6;
  p.conc_b = 0.0;
  p.conc_c = 0.0;
  const uint64_t key = hod_random_key(p.seed, 5, host.UniqueGalaxyID);
  struct HodOccupation occupation;
  hod_populate_draw_occupation(&p, key, row_mass(&host), &occupation);
  TEST_ASSERT_EQUAL(workspace.count - workspace.base_count, (int64_t)occupation.num_satellites,
                    "every drawn satellite is created");
  TEST_ASSERT(occupation.num_satellites > 0, "M = 1e15 has satellites");
  const double c = hod_populate_concentration(&p, row_mass(&host), 0.0);
  TEST_ASSERT(fabs(c - 1e-6) <= 1e-18, "the concentration is 1e-6");
  for (int64_t r = workspace.base_count; r < workspace.count; r++) {
    const int s = (int)(r - workspace.base_count);
    struct HodSatellite sat;
    TEST_ASSERT(hod_populate_draw_satellite(key, s, c, host.Rvir, host.Vvir, 0.0, &sat) == 0,
                "the helper draw at c = 1e-6 succeeds");
    TEST_ASSERT(sat.r_phys > 0.01 * host.Rvir && sat.r_phys <= host.Rvir,
                "the radius is of order Rvir, not collapsed onto the host");
    for (int j = 0; j < 3; j++) {
      const double wrapped =
          hod_populate_wrap((double)host.Pos[j] + sat.offset[j], HOD_TEST_BOX_SIZE);
      TEST_ASSERT(workspace.halos[r].Pos[j] ==
                      hod_populate_store_position(wrapped, HOD_TEST_BOX_SIZE),
                  "the created position is the helper's");
    }
  }
  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * DETERMINISM AND PERMUTATION INVARIANCE
 * ============================================================================ */

/** Created rows of one FoF, copied out (halo with its galaxy pointer cleared, and the galaxy) */
struct CreatedSet {
  int count;
  struct Halo halos[256];
  struct GalaxyData galaxies[256];
};

static void copy_created(struct CreatedSet *set) {
  set->count = (int)(workspace.count - workspace.base_count);
  for (int s = 0; s < set->count && s < 256; s++) {
    set->halos[s] = workspace.halos[workspace.base_count + s];
    set->galaxies[s] = *set->halos[s].galaxy;
    set->halos[s].galaxy = NULL;
  }
}

/** Two FoFs: A (host 6001, M = 1e15) and B (host 7001, M = 3e14), each with scaffold rows */
static const struct RowSpec fof_a[] = {
    {0, 6001, 11, 1.0e5, {99.9f, 0.05f, 50.0f}, {300.0f, -20.0f, 5.0f}, false},
    {1, 6002, 12, 40.0, {99.0f, 0.10f, 50.1f}, {0.0f, 0.0f, 0.0f}, false},
    {2, 6003, 13, 0.0, {98.0f, 0.20f, 50.2f}, {0.0f, 0.0f, 0.0f}, false},
};
static const struct RowSpec fof_b[] = {
    {0, 7001, 21, 3.0e4, {5.0f, 95.0f, 0.01f}, {-80.0f, 60.0f, 15.0f}, false},
    {2, 7002, 22, 0.0, {5.1f, 95.1f, 0.02f}, {0.0f, 0.0f, 0.0f}, false},
};

/** @brief Run one FoF through the pipeline and copy out its created rows */
static void run_fof(const struct RowSpec *rows, int n, int central_index, struct CreatedSet *out) {
  build_workspace(rows, n, central_index, 9, 0.25);
  execute_module_pipeline(&context, &workspace);
  copy_created(out);
  free_workspace();
}

/** @brief Whether two created sets are bitwise identical (halo-side and galaxy) */
static bool sets_identical(const struct CreatedSet *a, const struct CreatedSet *b) {
  return a->count == b->count &&
         memcmp(a->halos, b->halos, (size_t)a->count * sizeof(a->halos[0])) == 0 &&
         memcmp(a->galaxies, b->galaxies, (size_t)a->count * sizeof(a->galaxies[0])) == 0;
}

/** @brief Whether two created sets carry the same draws: count, IDs, Pos, Vel bits, flags */
static bool draws_identical(const struct CreatedSet *a, const struct CreatedSet *b) {
  if (a->count != b->count) {
    return false;
  }
  for (int s = 0; s < a->count; s++) {
    if (a->halos[s].UniqueGalaxyID != b->halos[s].UniqueGalaxyID ||
        a->halos[s].Type != b->halos[s].Type ||
        memcmp(a->halos[s].Pos, b->halos[s].Pos, sizeof(a->halos[s].Pos)) != 0 ||
        memcmp(a->halos[s].Vel, b->halos[s].Vel, sizeof(a->halos[s].Vel)) != 0 ||
        a->galaxies[s].HODGhost != b->galaxies[s].HODGhost) {
      return false;
    }
  }
  return true;
}

/**
 * @test   test_determinism_and_permutation
 * @brief  Repeats are bitwise identical; row order within a FoF and FoF order change no draw
 */
int test_determinism_and_permutation(void) {
  static struct CreatedSet a_first;
  static struct CreatedSet b_second;
  static struct CreatedSet a_repeat;
  static struct CreatedSet b_first;
  static struct CreatedSet a_second;
  static struct CreatedSet a_permuted;
  static struct CreatedSet b_permuted;

  configure_run(false);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");

  run_fof(fof_a, 3, 0, &a_first);
  run_fof(fof_b, 2, 0, &b_second);
  TEST_ASSERT(a_first.count > 0 && b_second.count > 0, "both hosts create satellites");
  TEST_ASSERT(a_first.count < 256 && b_second.count < 256, "the copies hold every row");

  run_fof(fof_a, 3, 0, &a_repeat);
  TEST_ASSERT(sets_identical(&a_first, &a_repeat), "a repeat is bitwise identical");

  /* FoF order B then A. */
  run_fof(fof_b, 2, 0, &b_first);
  run_fof(fof_a, 3, 0, &a_second);
  TEST_ASSERT(sets_identical(&a_first, &a_second), "FoF order does not change A's rows");
  TEST_ASSERT(sets_identical(&b_second, &b_first), "FoF order does not change B's rows");

  /* Rows permuted within each FoF: the host moves to the end. */
  const struct RowSpec a_rows[] = {fof_a[2], fof_a[1], fof_a[0]};
  const struct RowSpec b_rows[] = {fof_b[1], fof_b[0]};
  run_fof(a_rows, 3, 2, &a_permuted);
  run_fof(b_rows, 2, 1, &b_permuted);
  TEST_ASSERT(draws_identical(&a_first, &a_permuted), "row order does not change A's draws");
  TEST_ASSERT(draws_identical(&b_second, &b_permuted), "row order does not change B's draws");

  /* The seed is part of the key. */
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  configure_run(false);
  snprintf(MimicConfig.ModelParams[5].value, MAX_STRING_LEN, "%s", "2"); /* HODSeed */
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises with seed 2");
  static struct CreatedSet a_seed2;
  run_fof(fof_a, 3, 0, &a_seed2);
  TEST_ASSERT(!draws_identical(&a_first, &a_seed2), "another seed changes the draws");

  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * STATISTICS OVER INDEPENDENTLY KEYED HOSTS
 * ============================================================================ */

/** @brief Key of synthetic host i (distinct UniqueGalaxyID per host, snapshot 49, seed 1) */
static uint64_t stat_key(int i) { return hod_random_key(1, 49, 1000000LL + i); }

/**
 * @test   test_statistics_central_frequency
 * @brief  At M = 10^12.02, the central frequency is 0.5 within 4 binomial standard errors
 *
 * Each host's central is a Bernoulli(p) trial with p = <Ncen>(10^12.02) = 0.5,
 * so the frequency over N hosts has standard error sqrt(p (1 - p) / N) =
 * 0.5 / sqrt(N) (0.0025 for N = 40,000). Gating is checked on the same hosts:
 * no host without a central has a satellite.
 */
int test_statistics_central_frequency(void) {
  const double mass = pow(10.0, 12.02);
  int centrals = 0;
  for (int i = 0; i < STAT_HOSTS; i++) {
    struct HodOccupation o;
    hod_populate_draw_occupation(&default_params, stat_key(i), mass, &o);
    if (o.central) {
      centrals++;
    } else if (o.num_satellites != 0) {
      TEST_ASSERT(0, "a host without a central must have no satellite");
    }
  }
  const double frequency = (double)centrals / STAT_HOSTS;
  const double se = sqrt(0.25 / STAT_HOSTS);
  printf("[central frequency %.5f, |dev| = %.2f SE] ", frequency, fabs(frequency - 0.5) / se);
  TEST_ASSERT(fabs(frequency - 0.5) <= 4.0 * se, "central frequency within 4 SE of 0.5");
  return TEST_PASS;
}

/**
 * @test   test_statistics_satellite_counts
 * @brief  At M = 1e14, Nsat's mean and variance agree with lambda = 5.373959 within 4 SE
 *
 * <Ncen>(1e14) = 1 to 1e-12, so every host has a central and Nsat is
 * Poisson(lambda) unconditionally. Mean: SE = sqrt(lambda / N). Sample variance
 * s^2: Var(s^2) ~ (mu_4 - sigma^4) / N with the Poisson central fourth moment
 * mu_4 = lambda + 3 lambda^2 and sigma^4 = lambda^2, so SE = sqrt((lambda +
 * 2 lambda^2) / N) (0.0164 and 0.0562 for N = 40,000).
 */
int test_statistics_satellite_counts(void) {
  const double lambda = 5.373959;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int i = 0; i < STAT_HOSTS; i++) {
    struct HodOccupation o;
    hod_populate_draw_occupation(&default_params, stat_key(i), 1e14, &o);
    TEST_ASSERT(o.central, "every host has a central at <Ncen> = 1");
    sum += o.num_satellites;
    sum_sq += (double)o.num_satellites * o.num_satellites;
  }
  const double mean = sum / STAT_HOSTS;
  const double variance = (sum_sq - STAT_HOSTS * mean * mean) / (STAT_HOSTS - 1);
  const double se_mean = sqrt(lambda / STAT_HOSTS);
  const double se_variance = sqrt((lambda + 2.0 * lambda * lambda) / STAT_HOSTS);
  printf("[mean %.4f (%.2f SE), variance %.4f (%.2f SE)] ", mean, fabs(mean - lambda) / se_mean,
         variance, fabs(variance - lambda) / se_variance);
  TEST_ASSERT(fabs(mean - lambda) <= 4.0 * se_mean, "satellite mean within 4 SE of lambda");
  TEST_ASSERT(fabs(variance - lambda) <= 4.0 * se_variance,
              "satellite variance within 4 SE of lambda");
  return TEST_PASS;
}

/** @brief The test's own NFW enclosed-mass shape, the forward profile (c here is ~4, no
 * cancellation) */
static double forward_nfw_mass(double x) { return log1p(x) - x / (1.0 + x); }

/**
 * @test   test_statistics_radial_profile
 * @brief  The satellite radial CDF at M = 1e14 follows the forward NFW profile (DKW bound)
 *
 * Every satellite of the 40,000 hosts (about 215,000) is one independent draw
 * of x = r_phys c / Rvir. The Dvoretzky-Kiefer-Wolfowitz inequality bounds the
 * empirical CDF F_n of n i.i.d. draws uniformly in x: P(sup |F_n - F| > eps) <=
 * 2 exp(-2 n eps^2), so eps = sqrt(ln(2 / 1e-6) / (2 n)) (about 0.0058 for
 * n = 215,000) is exceeded anywhere with probability below 1e-6, which covers
 * every point checked below at once. Three assertions, each proving one thing:
 * - Fixed grid x_k = k c / 10, k = 1..9, against the test's own forward profile
 *   F(x) = m(x)/m(c): the drawn radii follow the NFW profile. Neither the grid
 *   nor F involves hod_populate_nfw_inverse(), so this holds only if the
 *   module's inverse (and the radius uniform) are both right.
 * - Quantile points x_q = hod_populate_nfw_inverse(q, c), q = 0.05 ... 0.95,
 *   against q: the radius uniforms are uniform. For any monotone inverse,
 *   x <= x_q exactly when u <= q, so this says nothing about the inverse itself
 *   (test_nfw_inverse_spot_values pins that), only about the uniforms.
 * - The same quantile points against the forward profile m(x_q)/m(c): the
 *   inverse's outputs sit where the forward profile puts them, linking the two.
 */
int test_statistics_radial_profile(void) {
  const double c = hod_populate_concentration(&default_params, 1e14, 0.0);
  const double rvir = 1.0;
  const double m_c = forward_nfw_mass(c);
  double grid[9];
  double grid_cdf[9];
  for (int k = 0; k < 9; k++) {
    grid[k] = c * (k + 1) / 10.0;
    grid_cdf[k] = forward_nfw_mass(grid[k]) / m_c;
  }
  double q_level[10];
  double x_q[10];
  double x_q_cdf[10];
  for (int q = 0; q < 10; q++) {
    q_level[q] = 0.05 + 0.1 * q;
    TEST_ASSERT(hod_populate_nfw_inverse(q_level[q], c, &x_q[q]) == 0, "the inverse converges");
    x_q_cdf[q] = forward_nfw_mass(x_q[q]) / m_c;
  }

  int64_t n = 0;
  int64_t below_grid[9] = {0};
  int64_t below_q[10] = {0};
  for (int i = 0; i < STAT_HOSTS; i++) {
    const uint64_t key = stat_key(i);
    struct HodOccupation o;
    hod_populate_draw_occupation(&default_params, key, 1e14, &o);
    for (int s = 0; s < o.num_satellites; s++) {
      struct HodSatellite sat;
      TEST_ASSERT(hod_populate_draw_satellite(key, s, c, rvir, 200.0, 0.0, &sat) == 0,
                  "every satellite draw succeeds");
      const double x = sat.r_phys * c / rvir;
      for (int k = 0; k < 9; k++) {
        below_grid[k] += (x <= grid[k]);
      }
      for (int q = 0; q < 10; q++) {
        below_q[q] += (x <= x_q[q]);
      }
      n++;
    }
  }

  const double eps = sqrt(log(2.0 / 1e-6) / (2.0 * (double)n));
  double worst_grid = 0.0;
  for (int k = 0; k < 9; k++) {
    worst_grid = fmax(worst_grid, fabs((double)below_grid[k] / (double)n - grid_cdf[k]));
  }
  double worst_uniform = 0.0;
  double worst_linked = 0.0;
  for (int q = 0; q < 10; q++) {
    const double empirical = (double)below_q[q] / (double)n;
    worst_uniform = fmax(worst_uniform, fabs(empirical - q_level[q]));
    worst_linked = fmax(worst_linked, fabs(empirical - x_q_cdf[q]));
  }
  printf("[n = %lld, max |F_n - F| grid %.5f, quantiles vs q %.5f, vs forward %.5f, bound %.5f] ",
         (long long)n, worst_grid, worst_uniform, worst_linked, eps);
  TEST_ASSERT(n > 150000, "about 215,000 satellites were drawn");
  TEST_ASSERT(worst_grid <= eps,
              "on a fixed grid the radial CDF follows the forward profile within the DKW bound");
  TEST_ASSERT(worst_uniform <= eps, "at the inverse's quantile points the CDF is q (uniforms)");
  TEST_ASSERT(worst_linked <= eps,
              "at the inverse's quantile points the CDF follows the forward profile");
  return TEST_PASS;
}

/**
 * @test   test_statistics_velocity_dispersion
 * @brief  The one-dimensional velocity-offset variance is Vvir^2 / 2 within 4 SE
 *
 * Each component of each satellite is an independent N(0, sigma^2) draw with
 * sigma^2 = Vvir^2 / 2 (20,000 (km/s)^2 for Vvir = 200). For n Gaussian draws
 * the sample variance has SE = sigma^2 sqrt(2 / (n - 1)).
 */
int test_statistics_velocity_dispersion(void) {
  const double vvir = 200.0;
  const double c = hod_populate_concentration(&default_params, 1e14, 0.0);
  int64_t n = 0;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int i = 0; i < STAT_HOSTS; i++) {
    const uint64_t key = stat_key(i);
    struct HodOccupation o;
    hod_populate_draw_occupation(&default_params, key, 1e14, &o);
    for (int s = 0; s < o.num_satellites; s++) {
      struct HodSatellite sat;
      TEST_ASSERT(hod_populate_draw_satellite(key, s, c, 1.0, vvir, 0.0, &sat) == 0,
                  "every satellite draw succeeds");
      for (int j = 0; j < 3; j++) {
        sum += sat.velocity[j];
        sum_sq += sat.velocity[j] * sat.velocity[j];
        n++;
      }
    }
  }
  const double sigma_sq = vvir * vvir / 2.0;
  const double mean = sum / (double)n;
  const double variance = (sum_sq - (double)n * mean * mean) / (double)(n - 1);
  const double se = sigma_sq * sqrt(2.0 / (double)(n - 1));
  printf("[n = %lld, variance %.1f (%.2f SE)] ", (long long)n, variance,
         fabs(variance - sigma_sq) / se);
  TEST_ASSERT(fabs(variance - sigma_sq) <= 4.0 * se, "velocity variance within 4 SE of Vvir^2/2");
  return TEST_PASS;
}

/* ============================================================================
 * SNAPSHOT AUDIT
 * ============================================================================ */

static struct Halo audit_halos[MAX_ROWS];
static struct GalaxyData audit_galaxies[MAX_ROWS];

static void set_audit_row(int i, int type, long long id, long long central_id, double mvir,
                          int ghost) {
  memset(&audit_halos[i], 0, sizeof(audit_halos[i]));
  memset(&audit_galaxies[i], 0, sizeof(audit_galaxies[i]));
  audit_halos[i].Type = type;
  audit_halos[i].UniqueGalaxyID = id;
  audit_halos[i].UniqueCentralGalaxyID = central_id;
  audit_halos[i].Mvir = mvir;
  audit_halos[i].galaxy = &audit_galaxies[i];
  audit_galaxies[i].HODGhost = ghost;
}

/**
 * Hand population: host A (M = 1e15, central present, three satellites), host
 * B (M = 10^12.02, central absent, one Type 1 scaffold row), host C (M = 0) and
 * host D (M = 1e14, central present, no satellite).
 *   expected per host <Ncen> (1 + lambda): A 62.842859, B 0.5 x 1.0325706 =
 *   0.5162853, C 0, D 6.373959; total 69.733103 over V = 1e6 (Mpc/h)^3
 *   -> n_gal expected 6.973310e-05; satellites alone 61.842859 + 0.0162853 +
 *   5.373959 = 67.233103 -> f_sat expected 0.964149
 *   realised: A, D and three satellites = 5 -> 5.000000e-06; f_sat 3/5 = 0.6
 */
static int build_audit_population(void) {
  set_audit_row(0, 0, 101, 101, 1.0e5, 0);
  set_audit_row(1, 2, -11, 101, 0.0, 0);
  set_audit_row(2, 2, -12, 101, 0.0, 0);
  set_audit_row(3, 2, -13, 101, 0.0, 0);
  set_audit_row(4, 0, 201, 201, pow(10.0, 2.02), 1);
  set_audit_row(5, 1, 202, 201, 50.0, 1);
  set_audit_row(6, 0, 301, 301, 0.0, 1);
  set_audit_row(7, 0, 401, 401, 1.0e4, 0);
  return 8;
}

/**
 * @test   test_snapshot_audit
 * @brief  The audit line and bins match a hand sum; non-output snapshots are silent; nothing
 * written
 */
int test_snapshot_audit(void) {
  configure_run(true);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 49;
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises with the audit");

  const int n = build_audit_population();
  static struct Halo halos_before[MAX_ROWS];
  static struct GalaxyData galaxies_before[MAX_ROWS];
  memcpy(halos_before, audit_halos, sizeof(audit_halos));
  memcpy(galaxies_before, audit_galaxies, sizeof(audit_galaxies));
  const size_t utility_before = memory_category_bytes(MEM_UTILITY);

  struct SnapshotContext ctx = {49, 0.0, 0.0, &MimicConfig};
  capture_log(1);
  TEST_ASSERT_EQUAL(hod_populate_process_snapshot(&ctx, audit_halos, n), 0, "the audit succeeds");
  const char *log = captured_log();
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before,
              "the audit leaves no net MEM_UTILITY allocation");
  TEST_ASSERT(memcmp(halos_before, audit_halos, sizeof(audit_halos)) == 0 &&
                  memcmp(galaxies_before, audit_galaxies, sizeof(audit_galaxies)) == 0,
              "the audit writes nothing");

  TEST_ASSERT(strstr(log, "HOD audit z=0.0000 hosts=4 n_gal expected=") != NULL,
              "one summary line in the documented format");
  double n_expected = 0.0;
  double n_realised = 0.0;
  double fsat_expected = 0.0;
  double fsat_realised = 0.0;
  const char *summary = strstr(log, "n_gal expected=");
  TEST_ASSERT(summary != NULL &&
                  sscanf(summary, "n_gal expected=%lf realised=%lf f_sat expected=%lf realised=%lf",
                         &n_expected, &n_realised, &fsat_expected, &fsat_realised) == 4,
              "the summary carries both densities and both satellite fractions");
  TEST_ASSERT(fabs(n_expected - 6.973310e-05) <= 1e-11, "n_gal expected matches the hand sum");
  TEST_ASSERT(fabs(n_realised - 5.0e-06) <= 1e-12, "n_gal realised is 5 / V");
  TEST_ASSERT(fabs(fsat_expected - 0.964149) <= 1e-6, "f_sat expected matches the hand sum");
  TEST_ASSERT(fabs(fsat_realised - 0.6) <= 1e-6, "f_sat realised is 3 / 5");
  TEST_ASSERT_EQUAL(strstr(strstr(log, "HOD audit z=") + 1, "HOD audit z=") == NULL, 1,
                    "exactly one summary line");

  TEST_ASSERT(
      strstr(log, "log10M=[12.00, 12.20) hosts=1 <N> expected=0.516285 realised=0.000000") != NULL,
      "the 10^12.02 bin holds host B: half a central expected, none realised");
  TEST_ASSERT(
      strstr(log, "log10M=[14.00, 14.20) hosts=1 <N> expected=6.373959 realised=1.000000") != NULL,
      "the 1e14 bin holds host D: one central realised");
  TEST_ASSERT(
      strstr(log, "log10M=[15.00, 15.20) hosts=1 <N> expected=62.842859 realised=4.000000") != NULL,
      "the 1e15 bin holds host A and its three satellites");

  /* A non-output snapshot returns 0 without logging. */
  ctx.snapshot_number = 48;
  capture_log(1);
  TEST_ASSERT_EQUAL(hod_populate_process_snapshot(&ctx, audit_halos, n), 0,
                    "a non-output snapshot succeeds");
  log = captured_log();
  TEST_ASSERT(strstr(log, "HOD audit") == NULL, "and logs nothing");

  /* An empty population is a valid call. */
  ctx.snapshot_number = 49;
  TEST_ASSERT_EQUAL(hod_populate_process_snapshot(&ctx, NULL, 0), 0, "an empty population audits");

  /* A sample satellite whose host is missing breaks the contract and fails the audit. */
  set_audit_row(3, 2, -13, 999, 0.0, 0);
  TEST_ASSERT(hod_populate_process_snapshot(&ctx, audit_halos, n) != 0,
              "an unattributable sample satellite fails the audit");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before,
              "a failed audit releases its scratch");

  TEST_ASSERT_EQUAL(release_case(), 0, "release succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * MAIN
 * ============================================================================ */

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: hod_populate (HOD occupation, placement, audit, RNG)\n");
  printf("============================================================\n");
  printf("%s", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_random_determinism);
  TEST_RUN(test_random_open_interval);
  TEST_RUN(test_random_poisson_inversion);
  TEST_RUN(test_occupation_spot_values);
  TEST_RUN(test_concentration_spot_values);
  TEST_RUN(test_nfw_inverse_spot_values);
  TEST_RUN(test_nfw_small_concentration);
  TEST_RUN(test_position_wrap);
  TEST_RUN(test_comoving_offset);
  TEST_RUN(test_init_configuration);
  TEST_RUN(test_init_parameter_domain);
  TEST_RUN(test_process_input_validation);
  TEST_RUN(test_process_lifecycle);
  TEST_RUN(test_absent_central_creates_nothing);
  TEST_RUN(test_output_snapshot_gating);
  TEST_RUN(test_satellite_limit_errors);
  TEST_RUN(test_small_concentration_placement);
  TEST_RUN(test_determinism_and_permutation);
  TEST_RUN(test_statistics_central_frequency);
  TEST_RUN(test_statistics_satellite_counts);
  TEST_RUN(test_statistics_radial_profile);
  TEST_RUN(test_statistics_velocity_dispersion);
  TEST_RUN(test_snapshot_audit);

  TEST_SUMMARY();
  return TEST_RESULT();
}
