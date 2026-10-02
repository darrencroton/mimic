/**
 * @file    test_unit_sham_global_rank.c
 * @brief   Unit tests for the sham_global_rank snapshot module
 *
 * Validates: parameter loading and domain checks (including the strings the
 * strict parser accepts and the range macros pass), the configuration guards,
 * an independent hand-solvable oracle with an exact tie, permutation and repeat
 * invariance in exact float bytes, the global (cross-FoF) nature of the rank,
 * peak tracking and its float storage bound, Type 2 inclusion, the reset of
 * every stellar field, all-or-nothing failure on malformed entries, empty and
 * all-ineligible populations, scratch release and no allocation growth, and a
 * numerical edge battery against an independent 60-digit Decimal reference.
 *
 * Float tolerance policy. Same-build repeats and permutations must match in
 * exact float bytes. Independent formula checks (a direct double evaluation of
 * 8 / (r + 0.5), or the Decimal reference) allow at most two float ULPs: the
 * module evaluates four double logarithms and one exponential, each within a
 * few double ULPs, so its double mass is within ~1e-13 relative of the exact
 * value for every case here (|ln M| < 120, bracket terms < 1e3), far below one
 * float ULP (~6e-8 relative, and 2^-149 absolute for subnormals); the float
 * rounding of two values that close can differ by one ULP only when the exact
 * value sits next to a rounding midpoint. Two ULPs bounds that with margin. It
 * is a bound for these cases, not a universal error bound.
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared SHAM test fixture boilerplate (counters, config reset, legacy parameters) */
#include "modules/_tests/sham_test_fixtures.h"

/* Module under test: lifecycle and the rank helper declared in its header */
#include "sham_global_rank.h"
extern int sham_global_rank_init(void);
extern int sham_global_rank_process_snapshot(const struct SnapshotContext *ctx,
                                             const struct Halo *halos, int64_t count);
extern int sham_global_rank_cleanup(void);

/** Largest synthetic population used by any test */
#define MAX_POPULATION 1024

/** Independent-oracle bound in float ULPs (see the file header) */
#define MAX_ORACLE_ULPS 2

/* ============================================================================
 * FIXTURES
 * ============================================================================ */

static struct Halo halos[MAX_POPULATION];
static struct GalaxyData galaxies[MAX_POPULATION];
static struct Halo halos_before[MAX_POPULATION];
static struct GalaxyData galaxies_before[MAX_POPULATION];

/** Append one module parameter as a string, exactly as the run-file parser stores it */
static void add_parameter(const char *name, const char *value) {
  int i = MimicConfig.NumModelParams++;
  snprintf(MimicConfig.ModelParams[i].param_name, MAX_STRING_LEN, "%s", name);
  snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%s", value);
}

/** Reset the configuration to one run with the three global parameters and a box size */
static void configure_run(double box_size, const char *mass_scale, const char *number_density,
                          const char *slope) {
  reset_config();
  MimicConfig.BoxSize = box_size;
  if (mass_scale != NULL) {
    add_parameter("ShamGlobalMassScale", mass_scale);
  }
  if (number_density != NULL) {
    add_parameter("ShamGlobalNumberDensity", number_density);
  }
  if (slope != NULL) {
    add_parameter("ShamGlobalSlope", slope);
  }
}

/** Run init() with this module configured once in post_snapshot; frees the phase after */
static int init_configured(double box_size, const char *mass_scale, const char *number_density,
                           const char *slope) {
  configure_run(box_size, mass_scale, number_density, slope);
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  const int rc = sham_global_rank_init();
  test_free_post_snapshot();
  return rc;
}

/** Build entry i: everything not set here stays zeroed, including all peaks */
static void set_entry(int64_t i, int type, long long id, float vmax, double mvir, int fof) {
  memset(&halos[i], 0, sizeof(halos[i]));
  memset(&galaxies[i], 0, sizeof(galaxies[i]));
  halos[i].Type = type;
  halos[i].UniqueGalaxyID = id;
  halos[i].Vmax = vmax;
  halos[i].Mvir = mvir;
  halos[i].CentralHalo = fof; /* FoF label only; the module never reads it */
  halos[i].galaxy = &galaxies[i];
}

static void snapshot_population(int64_t count) {
  memcpy(halos_before, halos, (size_t)count * sizeof(halos[0]));
  memcpy(galaxies_before, galaxies, (size_t)count * sizeof(galaxies[0]));
}

/** Whether halos and galaxies are byte-identical to the last snapshot_population() */
static int population_unchanged(int64_t count) {
  return memcmp(halos_before, halos, (size_t)count * sizeof(halos[0])) == 0 &&
         memcmp(galaxies_before, galaxies, (size_t)count * sizeof(galaxies[0])) == 0;
}

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/** ULP distance between two nonnegative finite floats */
static int64_t ulp_distance(float a, float b) {
  return llabs((long long)float_bits(a) - (long long)float_bits(b));
}

static int64_t find_id(int64_t count, long long id) {
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].UniqueGalaxyID == id) {
      return i;
    }
  }
  return -1;
}

/** Oracle target with n0 * BoxSize^3 = 1 exactly in logarithms, so M_r = M0 / (r + 0.5) */
static const struct ShamGlobalRankTarget unit_volume_target = {8.0, 1.0, 1.0, 1.0};

/** Fisher-Yates shuffle of halos/galaxies [0, count) by a fixed LCG; repoints galaxies */
static void shuffle_population(int64_t count, uint64_t seed) {
  for (int64_t i = count - 1; i > 0; i--) {
    seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    const int64_t j = (int64_t)((seed >> 33) % (uint64_t)(i + 1));
    struct Halo halo = halos[i];
    halos[i] = halos[j];
    halos[j] = halo;
    struct GalaxyData galaxy = galaxies[i];
    galaxies[i] = galaxies[j];
    galaxies[j] = galaxy;
  }
  for (int64_t i = 0; i < count; i++) {
    halos[i].galaxy = &galaxies[i];
  }
}

/**
 * Population used by the invariance tests: four FoF labels, Types 0/1/2, exact
 * Vpeak ties inside and across FoFs, zero-peak (ineligible) entries, and Type 2
 * entries whose inherited peak differs from their (unused) Vmax.
 */
static int64_t build_mixed_population(void) {
  int64_t n = 0;
  for (int k = 0; k < 48; k++) {
    const int type = (k % 5 == 3) ? 2 : (k % 3 == 1 ? 1 : 0);
    const long long id = 1000LL + 37LL * ((k * 11) % 48); /* unique, not sorted by k */
    const float vmax = (k % 7 == 0) ? 0.0f : (float)(100 + (k % 9) * 10); /* many ties */
    set_entry(n, type, id, vmax, 10.0 + k, k % 4);
    if (type == 2) {
      galaxies[n].ShamVpeak = (k % 2 == 0) ? (float)(120 + (k % 4) * 10) : 0.0f;
      galaxies[n].ShamMpeak = 5.0f;
      halos[n].Vmax = 999.0f;
    }
    n++;
  }
  return n;
}

/** Independent rank of entry i by brute force over the updated peaks in galaxies[] */
static int64_t brute_force_rank(int64_t count, int64_t i) {
  int64_t rank = 0;
  for (int64_t j = 0; j < count; j++) {
    if (j == i || !(galaxies[j].ShamVpeak > 0.0f)) {
      continue;
    }
    if (galaxies[j].ShamVpeak > galaxies[i].ShamVpeak ||
        (galaxies[j].ShamVpeak == galaxies[i].ShamVpeak &&
         halos[j].UniqueGalaxyID < halos[i].UniqueGalaxyID)) {
      rank++;
    }
  }
  return rank;
}

/* ============================================================================
 * PARAMETERS AND CONFIGURATION
 * ============================================================================ */

/**
 * @test   test_init_requires_single_post_snapshot_configuration
 * @brief  init() succeeds only when configured once in post_snapshot as process_snapshot
 */
int test_init_requires_single_post_snapshot_configuration(void) {
  init_memory_system(0);

  configure_run(1.0, "8", "1", "1");
  TEST_ASSERT(sham_global_rank_init() != 0, "init must fail when not configured in post_snapshot");

  TEST_ASSERT(init_configured(1.0, "8", "1", "1") == 0, "init should succeed when configured");
  TEST_ASSERT(sham_global_rank_cleanup() == 0, "cleanup should succeed");

  configure_run(1.0, "8", "1", "1");
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(sham_global_rank_init() != 0, "init must fail when configured twice");
  test_free_post_snapshot();

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_legacy_sham_rejected_anywhere
 * @brief  Startup fails when sham_assign_stellar_mass is also configured in any phase
 */
int test_legacy_sham_rejected_anywhere(void) {
  init_memory_system(0);
  ensure_modules_registered();

  /* Directly: the legacy module in pre_timestep makes init() fail. */
  configure_run(1.0, "8", "1", "1");
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  test_pre_timestep_add("sham_assign_stellar_mass", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sham_global_rank_init() != 0, "init must reject the legacy module in pre_timestep");
  test_free_post_snapshot();
  test_free_pre_timestep();

  /* Through module_system_init(): the legacy module is valid and inits first, then ours fails. */
  reset_config();
  set_test_model_parameters();
  MimicConfig.BoxSize = 1.0;
  add_parameter("ShamGlobalMassScale", "8");
  add_parameter("ShamGlobalNumberDensity", "1");
  add_parameter("ShamGlobalSlope", "1");
  MimicConfig.SubSteps = 1;
  test_phase_add("galaxy_physics", "sham_assign_stellar_mass", PROCESSING_MODE_FULL_HALO);
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(module_system_init() != 0,
              "module_system_init must fail with both SHAM prescriptions configured");
  module_system_cleanup();

  /* Control: the same run without the legacy module initializes. */
  reset_config();
  set_test_model_parameters();
  MimicConfig.BoxSize = 1.0;
  add_parameter("ShamGlobalMassScale", "8");
  add_parameter("ShamGlobalNumberDensity", "1");
  add_parameter("ShamGlobalSlope", "1");
  MimicConfig.SubSteps = 1;
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(module_system_init() == 0, "module_system_init should succeed with only ours");
  module_system_cleanup();

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_mass_scale_parameter_domain
 * @brief  Malformed, non-finite and out-of-range M0 strings fail; 1e5 and 100000 agree bitwise
 */
int test_mass_scale_parameter_domain(void) {
  init_memory_system(0);

  static const char *const rejected[] = {
      "nan", "inf", "infinity", "-inf", "1e400", "1,0", "abc", "", "0", "-1", "100000.0000001",
  };
  for (size_t k = 0; k < sizeof(rejected) / sizeof(rejected[0]); k++) {
    if (init_configured(1.0, rejected[k], "0.5", "1") == 0) {
      fprintf(stderr, "ShamGlobalMassScale='%s' was accepted\n", rejected[k]);
      TEST_ASSERT(0, "every malformed, non-finite or out-of-range mass scale must fail init");
    }
  }
  TEST_ASSERT(init_configured(1.0, NULL, "0.5", "1") != 0, "a missing mass scale must fail init");

  /* 1e5 and 100000 are the endpoint M0; with n0 * V = 0.5, rank 0 receives M0 itself. */
  uint32_t bits[2];
  static const char *const endpoint[] = {"1e5", "100000"};
  for (int k = 0; k < 2; k++) {
    TEST_ASSERT(init_configured(1.0, endpoint[k], "0.5", "1") == 0, "endpoint M0 must be valid");
    set_entry(0, 0, 1, 200.0f, 1.0, 0);
    struct SnapshotContext ctx = {0, 0.0, 0.0, &MimicConfig};
    TEST_ASSERT(sham_global_rank_process_snapshot(&ctx, halos, 1) == 0, "endpoint must succeed");
    bits[k] = float_bits(galaxies[0].StellarMass);
    TEST_ASSERT(sham_global_rank_cleanup() == 0, "cleanup should succeed");
  }
  TEST_ASSERT(bits[0] == bits[1], "1e5 and 100000 must give identical bits");
  TEST_ASSERT(bits[0] == float_bits(100000.0f), "the endpoint must store exactly 100000.0f");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_density_slope_and_box_domains
 * @brief  n0, alpha and BoxSize outside their domains (or non-finite) fail init
 */
int test_density_slope_and_box_domains(void) {
  init_memory_system(0);

  static const char *const bad_density[] = {"nan", "inf", "9.99e-13", "1000.0001", "0", "x"};
  for (size_t k = 0; k < sizeof(bad_density) / sizeof(bad_density[0]); k++) {
    TEST_ASSERT(init_configured(1.0, "8", bad_density[k], "1") != 0,
                "an out-of-domain number density must fail init");
  }
  TEST_ASSERT(init_configured(1.0, "8", NULL, "1") != 0, "a missing density must fail init");
  TEST_ASSERT(init_configured(1.0, "8", "1e-12", "1") == 0, "n0 = 1e-12 is inside [1e-12, 1e3]");
  TEST_ASSERT(init_configured(1.0, "8", "1e3", "1") == 0, "n0 = 1e3 is inside [1e-12, 1e3]");

  static const char *const bad_slope[] = {"nan", "-inf", "0.0999", "10.0001", "0", "1 "};
  for (size_t k = 0; k < sizeof(bad_slope) / sizeof(bad_slope[0]); k++) {
    TEST_ASSERT(init_configured(1.0, "8", "1", bad_slope[k]) != 0,
                "an out-of-domain slope must fail init");
  }
  TEST_ASSERT(init_configured(1.0, "8", "1", NULL) != 0, "a missing slope must fail init");
  TEST_ASSERT(init_configured(1.0, "8", "1", "0.1") == 0, "alpha = 0.1 is inside [0.1, 10]");
  TEST_ASSERT(init_configured(1.0, "8", "1", "10") == 0, "alpha = 10 is inside [0.1, 10]");

  static const double bad_box[] = {0.0, -1.0, NAN, INFINITY, 1.0e103, 1.0e-110};
  for (size_t k = 0; k < sizeof(bad_box) / sizeof(bad_box[0]); k++) {
    TEST_ASSERT(init_configured(bad_box[k], "8", "1", "1") != 0,
                "a non-finite, non-positive or cube-overflowing/underflowing BoxSize must fail");
  }
  TEST_ASSERT(init_configured(1.0e-105, "8", "1", "1") == 0,
              "BoxSize = 1e-105 has a subnormal but positive cube and is valid");
  TEST_ASSERT(init_configured(5.6e102, "8", "1", "1") == 0,
              "BoxSize = 5.6e102 has a finite cube and is valid");

  sham_global_rank_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_process_snapshot_requires_init
 * @brief  The callback refuses to run before init() or without a context
 */
int test_process_snapshot_requires_init(void) {
  init_memory_system(0);
  sham_global_rank_cleanup();
  set_entry(0, 0, 1, 200.0f, 1.0, 0);
  struct SnapshotContext ctx = {0, 0.0, 0.0, &MimicConfig};
  TEST_ASSERT(sham_global_rank_process_snapshot(&ctx, halos, 1) != 0,
              "process_snapshot must fail before init");

  TEST_ASSERT(init_configured(1.0, "8", "1", "1") == 0, "init should succeed");
  TEST_ASSERT(sham_global_rank_process_snapshot(NULL, halos, 1) != 0,
              "process_snapshot must fail without a context");
  TEST_ASSERT(sham_global_rank_cleanup() == 0, "cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * RANKING
 * ============================================================================ */

/**
 * @test   test_hand_solvable_oracle
 * @brief  alpha=1, n0*V=1, M0=8: IDs 80/7/42 get 16, 16/3, 16/5 (ID 7 wins the tie)
 *
 * Catches a reversed tie key (ID 42 would get 16/3) and r + 1 in place of
 * r + 0.5 (ID 80 would get 8). Checked for two (n0, BoxSize) pairs with
 * n0 * BoxSize^3 = 1, including the example's 1e-6 and 100.
 */
int test_hand_solvable_oracle(void) {
  init_memory_system(0);

  static const struct ShamGlobalRankTarget targets[] = {{8.0, 1.0, 1.0, 1.0},
                                                        {8.0, 1.0e-6, 1.0, 100.0}};
  for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
    set_entry(0, 0, 42, 100.0f, 1.0, 2);
    set_entry(1, 1, 80, 200.0f, 1.0, 0);
    set_entry(2, 0, 7, 100.0f, 1.0, 1);
    TEST_ASSERT(sham_global_rank_assign(&targets[t], halos, 3) == 0, "assignment should succeed");

    const float expected_80 = 16.0f;
    const float expected_7 = (float)(16.0 / 3.0);
    const float expected_42 = (float)(16.0 / 5.0);
    TEST_ASSERT(ulp_distance(galaxies[1].StellarMass, expected_80) <= MAX_ORACLE_ULPS,
                "ID 80 (rank 0) must receive 16");
    TEST_ASSERT(ulp_distance(galaxies[2].StellarMass, expected_7) <= MAX_ORACLE_ULPS,
                "ID 7 (rank 1, lower ID wins the tie) must receive 16/3");
    TEST_ASSERT(ulp_distance(galaxies[0].StellarMass, expected_42) <= MAX_ORACLE_ULPS,
                "ID 42 (rank 2) must receive 16/5");
    TEST_ASSERT(galaxies[2].StellarMass > galaxies[0].StellarMass,
                "the tie must be broken by ascending UniqueGalaxyID");
    for (int i = 0; i < 3; i++) {
      TEST_ASSERT(float_bits(galaxies[i].ShamStellarMassNoScatter) ==
                      float_bits(galaxies[i].StellarMass),
                  "both stellar-mass fields receive the same float");
    }
  }
  /* n0 = 1, BoxSize = 1 gives a bracket of exactly ln(r + 0.5): rank 0 is exactly 16. */
  set_entry(0, 0, 80, 200.0f, 1.0, 0);
  TEST_ASSERT(sham_global_rank_assign(&targets[0], halos, 1) == 0, "assignment should succeed");
  TEST_ASSERT(galaxies[0].StellarMass == 16.0f, "rank 0 receives exactly 16, not 8 (r + 1)");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_ranks_match_independent_oracle
 * @brief  Every entry of a mixed population gets 8 / (rank + 0.5) for its brute-force rank
 */
int test_ranks_match_independent_oracle(void) {
  init_memory_system(0);
  const int64_t n = build_mixed_population();
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, n) == 0,
              "assignment should succeed");

  int64_t eligible = 0;
  for (int64_t i = 0; i < n; i++) {
    if (!(galaxies[i].ShamVpeak > 0.0f)) {
      TEST_ASSERT(galaxies[i].StellarMass == 0.0f, "a zero-peak entry must stay at zero");
      continue;
    }
    eligible++;
    const float expected = (float)(8.0 / ((double)brute_force_rank(n, i) + 0.5));
    if (ulp_distance(galaxies[i].StellarMass, expected) > MAX_ORACLE_ULPS) {
      fprintf(stderr, "ID %lld: got %.9g expected %.9g\n", halos[i].UniqueGalaxyID,
              (double)galaxies[i].StellarMass, (double)expected);
      TEST_ASSERT(0, "every eligible entry must match the independent rank oracle");
    }
  }
  TEST_ASSERT(eligible > 0 && eligible < n, "the population must mix eligible and zero-peak");

  check_memory_leaks();
  return TEST_PASS;
}

/** Per-ID StellarMass bits of the mixed population after one assignment of a given order */
static int mixed_population_bits(uint64_t seed, int64_t *n_out, long long *ids, uint32_t *bits) {
  const int64_t n = build_mixed_population();
  if (seed != 0) {
    shuffle_population(n, seed);
  }
  if (sham_global_rank_assign(&unit_volume_target, halos, n) != 0) {
    return -1;
  }
  /* Report in ascending-ID order so different population orders compare entry by entry. */
  int64_t written = 0;
  for (long long id = 0; written < n; id++) {
    const int64_t i = find_id(n, id);
    if (i >= 0) {
      ids[written] = id;
      bits[written] = float_bits(galaxies[i].StellarMass);
      written++;
    }
  }
  *n_out = n;
  return 0;
}

/**
 * @test   test_permutation_and_repeat_are_bitwise_identical
 * @brief  Repeats and permutations of the population give identical per-ID float bytes
 */
int test_permutation_and_repeat_are_bitwise_identical(void) {
  init_memory_system(0);
  static long long ref_ids[MAX_POPULATION];
  static uint32_t ref_bits[MAX_POPULATION];
  static long long ids[MAX_POPULATION];
  static uint32_t bits[MAX_POPULATION];
  int64_t ref_n;
  int64_t n;

  TEST_ASSERT(mixed_population_bits(0, &ref_n, ref_ids, ref_bits) == 0, "reference assignment");
  TEST_ASSERT(mixed_population_bits(0, &n, ids, bits) == 0, "repeat assignment");
  TEST_ASSERT(n == ref_n && memcmp(bits, ref_bits, (size_t)n * sizeof(bits[0])) == 0,
              "repeating the callback must give identical bits");

  static const uint64_t seeds[] = {1, 2, 3, 0x5eed, 0xfeedbeef, 77777};
  for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
    TEST_ASSERT(mixed_population_bits(seeds[s], &n, ids, bits) == 0, "permuted assignment");
    TEST_ASSERT(n == ref_n && memcmp(ids, ref_ids, (size_t)n * sizeof(ids[0])) == 0,
                "a permutation keeps the ID set");
    TEST_ASSERT(memcmp(bits, ref_bits, (size_t)n * sizeof(bits[0])) == 0,
                "permuting the population must preserve every per-ID result bitwise");
  }

  /* Reversal moves every FoF group and every tie partner. */
  const int64_t m = build_mixed_population();
  for (int64_t i = 0; i < m / 2; i++) {
    struct Halo halo = halos[i];
    halos[i] = halos[m - 1 - i];
    halos[m - 1 - i] = halo;
    struct GalaxyData galaxy = galaxies[i];
    galaxies[i] = galaxies[m - 1 - i];
    galaxies[m - 1 - i] = galaxy;
  }
  for (int64_t i = 0; i < m; i++) {
    halos[i].galaxy = &galaxies[i];
  }
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, m) == 0, "reversed assignment");
  for (int64_t k = 0; k < ref_n; k++) {
    const int64_t i = find_id(m, ref_ids[k]);
    TEST_ASSERT(i >= 0 && float_bits(galaxies[i].StellarMass) == ref_bits[k],
                "reversing the population must preserve every per-ID result bitwise");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_rank_is_global_across_fofs
 * @brief  A higher-proxy galaxy added in a different FoF lowers every other rank
 */
int test_rank_is_global_across_fofs(void) {
  init_memory_system(0);

  set_entry(0, 0, 11, 150.0f, 1.0, 0); /* FoF A */
  set_entry(1, 1, 12, 120.0f, 1.0, 0); /* FoF A */
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 2) == 0, "FoF A alone");
  const float alone_11 = galaxies[0].StellarMass;
  const float alone_12 = galaxies[1].StellarMass;
  TEST_ASSERT(alone_11 == 16.0f, "ID 11 is rank 0 on its own");

  set_entry(0, 0, 11, 150.0f, 1.0, 0);
  set_entry(1, 1, 12, 120.0f, 1.0, 0);
  set_entry(2, 0, 99, 300.0f, 1.0, 1); /* FoF B, higher proxy */
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 3) == 0, "with FoF B");
  TEST_ASSERT(galaxies[2].StellarMass == 16.0f, "the FoF B galaxy takes rank 0");
  TEST_ASSERT(float_bits(galaxies[0].StellarMass) == float_bits(alone_12),
              "ID 11 moves to rank 1 and receives what rank 1 received before");
  TEST_ASSERT(galaxies[1].StellarMass < alone_12, "ID 12 moves down to rank 2");
  TEST_ASSERT(ulp_distance(galaxies[1].StellarMass, (float)(16.0 / 5.0)) <= MAX_ORACLE_ULPS,
              "ID 12 at rank 2 receives 16/5");

  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * PEAKS, TYPES AND WRITES
 * ============================================================================ */

/**
 * @test   test_peak_tracking_and_type2_inclusion
 * @brief  Type 0/1 peaks ratchet up only; Type 2 keeps inherited peaks and is ranked
 */
int test_peak_tracking_and_type2_inclusion(void) {
  init_memory_system(0);

  set_entry(0, 0, 1, 120.0f, 30.0, 0);
  galaxies[0].ShamVpeak = 150.0f;
  galaxies[0].ShamMpeak = 20.0f;
  set_entry(1, 1, 2, 180.0f, 10.0, 0);
  galaxies[1].ShamVpeak = 170.0f;
  galaxies[1].ShamMpeak = 40.0f;
  set_entry(2, 2, 3, 900.0f, 500.0, 1); /* orphan: its halo proxies are not consumed */
  galaxies[2].ShamVpeak = 175.0f;
  galaxies[2].ShamMpeak = 25.0f;
  galaxies[2].ShamOrphanAge = 321.0f;
  snapshot_population(3);

  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 3) == 0, "assign should work");
  TEST_ASSERT(galaxies[0].ShamVpeak == 150.0f, "Vpeak must not decrease below its peak");
  TEST_ASSERT(galaxies[0].ShamMpeak == 30.0f, "Mpeak must rise to Mvir");
  TEST_ASSERT(galaxies[1].ShamVpeak == 180.0f, "Vpeak must rise to Vmax");
  TEST_ASSERT(galaxies[1].ShamMpeak == 40.0f, "Mpeak must not decrease below its peak");
  TEST_ASSERT(galaxies[2].ShamVpeak == 175.0f && galaxies[2].ShamMpeak == 25.0f,
              "Type 2 keeps its inherited peaks");
  TEST_ASSERT(galaxies[2].ShamOrphanAge == 321.0f, "ShamOrphanAge is left unchanged");
  TEST_ASSERT(memcmp(halos_before, halos, 3 * sizeof(halos[0])) == 0,
              "no halo field (Type included) may change");

  /* Ranks by updated Vpeak 180 (ID 2), 175 (ID 3, orphan), 150 (ID 1). */
  TEST_ASSERT(galaxies[1].StellarMass == 16.0f, "ID 2 is rank 0");
  TEST_ASSERT(ulp_distance(galaxies[2].StellarMass, (float)(16.0 / 3.0)) <= MAX_ORACLE_ULPS,
              "the Type 2 orphan is ranked (rank 1)");
  TEST_ASSERT(ulp_distance(galaxies[0].StellarMass, (float)(16.0 / 5.0)) <= MAX_ORACLE_ULPS,
              "ID 1 is rank 2");

  /* Peaks persist: a second callback with lower proxies keeps them and the same ranks. */
  const float first_masses[3] = {galaxies[0].StellarMass, galaxies[1].StellarMass,
                                 galaxies[2].StellarMass};
  halos[0].Vmax = 50.0f;
  halos[0].Mvir = 1.0;
  halos[1].Vmax = 60.0f;
  halos[1].Mvir = 1.0;
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 3) == 0, "second callback");
  TEST_ASSERT(galaxies[0].ShamVpeak == 150.0f && galaxies[1].ShamVpeak == 180.0f &&
                  galaxies[0].ShamMpeak == 30.0f && galaxies[1].ShamMpeak == 40.0f,
              "peaks persist across callbacks");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(float_bits(galaxies[i].StellarMass) == float_bits(first_masses[i]),
                "unchanged peaks reproduce the same masses bitwise");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_stellar_fields_reset_for_every_entry
 * @brief  All seven stellar fields reset; eligible entries then get M_r in both mass fields
 */
int test_stellar_fields_reset_for_every_entry(void) {
  init_memory_system(0);

  set_entry(0, 0, 5, 200.0f, 1.0, 0); /* eligible */
  set_entry(1, 1, 6, 0.0f, 1.0, 0);   /* zero peak: ineligible */
  for (int i = 0; i < 2; i++) {
    galaxies[i].StellarMass = 3.0f;
    galaxies[i].ShamStellarMassNoScatter = 3.0f;
    galaxies[i].ShamScatterDex = 0.3f;
    galaxies[i].BulgeMass = 1.0f;
    galaxies[i].MetalsStellarMass = 0.1f;
    galaxies[i].MetalsBulgeMass = 0.05f;
    galaxies[i].StarFormationRate = 7.0f;
    galaxies[i].ShamOrphanAge = 12.0f;
  }
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 2) == 0, "assign should work");
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT(galaxies[i].ShamScatterDex == 0.0f && galaxies[i].BulgeMass == 0.0f &&
                    galaxies[i].MetalsStellarMass == 0.0f && galaxies[i].MetalsBulgeMass == 0.0f &&
                    galaxies[i].StarFormationRate == 0.0f,
                "scatter, bulge, metals and SFR are reset for every entry");
    TEST_ASSERT(galaxies[i].ShamOrphanAge == 12.0f, "ShamOrphanAge is never written");
  }
  TEST_ASSERT(galaxies[0].StellarMass == 16.0f && galaxies[0].ShamStellarMassNoScatter == 16.0f,
              "the eligible entry gets M_r in both stellar-mass fields");
  TEST_ASSERT(galaxies[1].StellarMass == 0.0f && galaxies[1].ShamStellarMassNoScatter == 0.0f,
              "the ineligible entry stays at zero");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_mpeak_float_storage_bound
 * @brief  Mvir = FLT_MAX is stored exactly; Mvir = 1e39 fails without any write
 */
int test_mpeak_float_storage_bound(void) {
  init_memory_system(0);

  set_entry(0, 0, 1, 200.0f, (double)FLT_MAX, 0);
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 1) == 0,
              "Mvir = FLT_MAX is within float storage");
  TEST_ASSERT(galaxies[0].ShamMpeak == FLT_MAX, "Mvir = FLT_MAX is stored exactly");

  set_entry(0, 0, 1, 200.0f, 1.0e39, 0);
  set_entry(1, 0, 2, 0.0f, 1.0, 1);
  galaxies[1].StellarMass = 4.0f;
  snapshot_population(2);
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 2) != 0,
              "a peak mass above FLT_MAX must fail the snapshot");
  TEST_ASSERT(population_unchanged(2), "a failed snapshot writes nothing");

  /* An ineligible (zero-Vmax) entry with an unstorable Mvir fails too. */
  set_entry(0, 1, 3, 0.0f, 1.0e39, 0);
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 1) != 0,
              "an ineligible entry's unstorable peak still fails");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_malformed_entries_fail_without_writes
 * @brief  Every framework or proxy violation fails the whole call before any write
 */
int test_malformed_entries_fail_without_writes(void) {
  init_memory_system(0);

  enum {
    CASE_NULL_GALAXY,
    CASE_TYPE_3,
    CASE_TYPE_NEGATIVE,
    CASE_ZERO_ID,
    CASE_NEGATIVE_ID,
    CASE_DUPLICATE_ELIGIBLE,
    CASE_DUPLICATE_INELIGIBLE,
    CASE_VMAX_NAN,
    CASE_VMAX_NEGATIVE,
    CASE_MVIR_INF,
    CASE_MVIR_NEGATIVE,
    CASE_VPEAK_NAN,
    CASE_VPEAK_INF_TYPE2,
    CASE_MPEAK_NEGATIVE_TYPE2,
    CASE_INELIGIBLE_MVIR_NAN,
    NUM_CASES
  };
  for (int c = 0; c < NUM_CASES; c++) {
    /* A valid three-entry population with one ineligible entry, then one defect. */
    set_entry(0, 0, 10, 200.0f, 5.0, 0);
    set_entry(1, 2, 20, 0.0f, 0.0, 0);
    galaxies[1].ShamVpeak = 150.0f;
    set_entry(2, 1, 30, 0.0f, 2.0, 1);
    for (int i = 0; i < 3; i++) {
      galaxies[i].StellarMass = 9.0f;
    }
    switch (c) {
    case CASE_NULL_GALAXY:
      halos[2].galaxy = NULL;
      break;
    case CASE_TYPE_3:
      halos[2].Type = 3;
      break;
    case CASE_TYPE_NEGATIVE:
      halos[0].Type = -1;
      break;
    case CASE_ZERO_ID:
      halos[2].UniqueGalaxyID = 0;
      break;
    case CASE_NEGATIVE_ID:
      halos[1].UniqueGalaxyID = -5;
      break;
    case CASE_DUPLICATE_ELIGIBLE:
      halos[1].UniqueGalaxyID = 10;
      break;
    case CASE_DUPLICATE_INELIGIBLE:
      halos[2].UniqueGalaxyID = 20;
      galaxies[1].ShamVpeak = 0.0f;
      break;
    case CASE_VMAX_NAN:
      halos[0].Vmax = NAN;
      break;
    case CASE_VMAX_NEGATIVE:
      halos[2].Vmax = -1.0f;
      break;
    case CASE_MVIR_INF:
      halos[0].Mvir = INFINITY;
      break;
    case CASE_MVIR_NEGATIVE:
      halos[2].Mvir = -1.0;
      break;
    case CASE_VPEAK_NAN:
      galaxies[0].ShamVpeak = NAN;
      break;
    case CASE_VPEAK_INF_TYPE2:
      galaxies[1].ShamVpeak = INFINITY;
      break;
    case CASE_MPEAK_NEGATIVE_TYPE2:
      galaxies[1].ShamMpeak = -2.0f;
      break;
    case CASE_INELIGIBLE_MVIR_NAN:
      halos[2].Mvir = NAN;
      break;
    default:
      break;
    }
    snapshot_population(3);
    const int64_t before = (int64_t)memory_category_bytes(MEM_UTILITY);
    if (sham_global_rank_assign(&unit_volume_target, halos, 3) == 0) {
      fprintf(stderr, "malformed case %d was accepted\n", c);
      TEST_ASSERT(0, "every malformed population must fail");
    }
    if (!population_unchanged(3)) {
      fprintf(stderr, "malformed case %d wrote to the population\n", c);
      TEST_ASSERT(0, "a failed call must write nothing");
    }
    TEST_ASSERT((int64_t)memory_category_bytes(MEM_UTILITY) == before,
                "scratch must be released on a returned failure");
  }

  /* Control: the valid population itself succeeds. */
  set_entry(0, 0, 10, 200.0f, 5.0, 0);
  set_entry(1, 2, 20, 0.0f, 0.0, 0);
  galaxies[1].ShamVpeak = 150.0f;
  set_entry(2, 1, 30, 0.0f, 2.0, 1);
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 3) == 0,
              "the unmodified population is valid");

  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, NULL, 3) != 0,
              "a NULL population with a positive count fails");
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, -1) != 0,
              "a negative count fails");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_empty_and_all_ineligible_populations
 * @brief  Empty succeeds without allocating; all-ineligible updates peaks and resets only
 */
int test_empty_and_all_ineligible_populations(void) {
  init_memory_system(0);

  const size_t before = memory_category_bytes(MEM_UTILITY);
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, NULL, 0) == 0,
              "an empty population with a NULL pointer succeeds");
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 0) == 0,
              "an empty population with a non-NULL pointer succeeds");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == before, "an empty population allocates");

  /* Type 0 with zero Vmax but positive Mvir, and a Type 2 with zero inherited peak. */
  set_entry(0, 0, 3, 0.0f, 12.5, 0);
  set_entry(1, 2, 4, 50.0f, 99.0, 1);
  for (int i = 0; i < 2; i++) {
    galaxies[i].StellarMass = 2.0f;
    galaxies[i].ShamStellarMassNoScatter = 2.0f;
  }
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 2) == 0,
              "an all-ineligible population succeeds");
  TEST_ASSERT(galaxies[0].ShamMpeak == 12.5f, "peaks are still updated");
  TEST_ASSERT(galaxies[1].ShamVpeak == 0.0f, "Type 2 keeps its zero inherited peak");
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT(galaxies[i].StellarMass == 0.0f && galaxies[i].ShamStellarMassNoScatter == 0.0f,
                "ineligible entries are reset to zero");
  }
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == before, "scratch is released");

  /* Uniqueness still applies to all-ineligible populations. */
  set_entry(0, 0, 3, 0.0f, 1.0, 0);
  set_entry(1, 1, 3, 0.0f, 1.0, 0);
  TEST_ASSERT(sham_global_rank_assign(&unit_volume_target, halos, 2) != 0,
              "duplicate IDs fail even when nothing is eligible");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_no_allocation_growth_across_snapshots
 * @brief  Repeated callbacks leave tracked memory exactly where it started
 */
int test_no_allocation_growth_across_snapshots(void) {
  init_memory_system(0);
  TEST_ASSERT(init_configured(1.0, "8", "1", "1") == 0, "init should succeed");

  const size_t before = memory_category_bytes(MEM_UTILITY);
  struct SnapshotContext ctx = {0, 0.0, 0.0, &MimicConfig};
  for (int snap = 0; snap < 64; snap++) {
    const int64_t n = (snap % 4 == 0) ? 0 : 1 + (snap * 97) % (MAX_POPULATION - 1);
    for (int64_t i = 0; i < n; i++) {
      set_entry(i, (int)(i % 3), 1 + i, (float)(1 + (i * 31) % 400), 1.0 + (double)i, 0);
      galaxies[i].ShamVpeak = (i % 3 == 2) ? 99.0f : 0.0f;
    }
    ctx.snapshot_number = snap;
    TEST_ASSERT(sham_global_rank_process_snapshot(&ctx, n > 0 ? halos : NULL, n) == 0,
                "every snapshot should succeed");
    TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == before,
                "no allocation may outlive or accumulate across callbacks");
  }

  TEST_ASSERT(sham_global_rank_cleanup() == 0, "cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_registered_snapshot_dispatch
 * @brief  Generated registration binds process_snapshot; the core dispatches it
 */
int test_registered_snapshot_dispatch(void) {
  init_memory_system(0);
  ensure_modules_registered();

  configure_run(1.0, "8", "1", "1");
  MimicConfig.SubSteps = 1;
  test_post_snapshot_add("sham_global_rank", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(module_system_init() == 0, "module_system_init should succeed");

  set_entry(0, 0, 80, 200.0f, 1.0, 0);
  set_entry(1, 1, 7, 100.0f, 1.0, 1);
  struct SnapshotContext ctx = {3, 0.5, 1.0, &MimicConfig};
  execute_post_snapshot(&ctx, halos, 2); /* fatal on a non-zero return */
  TEST_ASSERT(galaxies[0].StellarMass == 16.0f, "the dispatched callback assigns rank 0");
  TEST_ASSERT(ulp_distance(galaxies[1].StellarMass, (float)(16.0 / 3.0)) <= MAX_ORACLE_ULPS,
              "the dispatched callback assigns rank 1");
  execute_post_snapshot(&ctx, NULL, 0);

  TEST_ASSERT(module_system_cleanup() == 0, "module_system_cleanup should succeed");
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * NUMERICAL EDGE BATTERY
 * ============================================================================ */

/**
 * One acceptance-contract edge case and its independent 60-digit Decimal
 * reference, generated by _tests/sham_global_rank_reference.py (which
 * test_integration_sham_global_rank.py checks against this table): whether the
 * exact-real mass is assignable, and the bits of its correctly rounded float32.
 */
struct EdgeCase {
  const char *name;
  const char *mass_scale;
  const char *number_density;
  const char *slope;
  double box_size;
  int expect_pass;
  uint32_t reference_bits;
};

/* SHAM_DECIMAL_REFERENCE_BEGIN */
static const struct EdgeCase edge_cases[] = {
    {"endpoint", "100000", "0.5", "1", 1.0, 1, 0x47C35000u},
    {"endpoint_density_above", "100000", "0.5000000000005", "1", 1.0, 0, 0u},
    {"tiny_volume", "1e5", "1e-12", "10", 1.0e-105, 1, 0x11878AA6u},
    {"huge_volume_too_massive", "1", "1e3", "0.1", 5.6e102, 0, 0u},
    {"huge_volume", "1e-30", "1e3", "10", 5.6e102, 1, 0x41646427u},
    {"subnormal", "1e-40", "1e-12", "10", 1.0, 1, 0x000012DAu},
    {"rounds_to_zero", "1e-46", "1e3", "10", 1.0, 0, 0u},
};
/* SHAM_DECIMAL_REFERENCE_END */

/**
 * @test   test_numerical_edge_battery
 * @brief  Success/failure exactly, masses within two ULPs of the Decimal reference
 */
int test_numerical_edge_battery(void) {
  init_memory_system(0);

  for (size_t k = 0; k < sizeof(edge_cases) / sizeof(edge_cases[0]); k++) {
    const struct EdgeCase *edge = &edge_cases[k];
    TEST_ASSERT(
        init_configured(edge->box_size, edge->mass_scale, edge->number_density, edge->slope) == 0,
        "every edge case has in-domain parameters");
    struct SnapshotContext ctx = {(int)k, 0.0, 0.0, &MimicConfig};
    uint32_t bits[2] = {0, 0};
    for (int repeat = 0; repeat < 2; repeat++) {
      set_entry(0, 0, 1, 200.0f, 1.0, 0);
      galaxies[0].StellarMass = 7.0f;
      snapshot_population(1);
      const int rc = sham_global_rank_process_snapshot(&ctx, halos, 1);
      if ((rc == 0) != (edge->expect_pass != 0)) {
        fprintf(stderr, "edge case %s: rc=%d, reference expects %s\n", edge->name, rc,
                edge->expect_pass ? "success" : "failure");
        TEST_ASSERT(0, "success/failure must match the reference exactly");
      }
      if (!edge->expect_pass) {
        TEST_ASSERT(population_unchanged(1), "a failed edge case writes nothing");
        break;
      }
      bits[repeat] = float_bits(galaxies[0].StellarMass);
      const int64_t ulps = llabs((long long)bits[repeat] - (long long)edge->reference_bits);
      if (ulps > MAX_ORACLE_ULPS) {
        fprintf(stderr, "edge case %s: bits 0x%08X, reference 0x%08X (%lld ULPs)\n", edge->name,
                bits[repeat], edge->reference_bits, (long long)ulps);
        TEST_ASSERT(0, "an edge mass must be within two float ULPs of the Decimal reference");
      }
    }
    if (edge->expect_pass) {
      TEST_ASSERT(bits[0] == bits[1], "a repeated edge case is bitwise identical");
    }
    TEST_ASSERT(sham_global_rank_cleanup() == 0, "cleanup should succeed");
  }

  /* The endpoint stores exactly 100000.0f, and the subnormal case stores a subnormal. */
  set_entry(0, 0, 1, 200.0f, 1.0, 0);
  TEST_ASSERT(
      sham_global_rank_assign(&(struct ShamGlobalRankTarget){1.0e5, 0.5, 1.0, 1.0}, halos, 1) == 0,
      "the endpoint is accepted");
  TEST_ASSERT(galaxies[0].StellarMass == 100000.0f, "the endpoint stores exactly 100000.0f");
  set_entry(0, 0, 1, 200.0f, 1.0, 0);
  TEST_ASSERT(sham_global_rank_assign(&(struct ShamGlobalRankTarget){1.0e-40, 1.0e-12, 10.0, 1.0},
                                      halos, 1) == 0,
              "the subnormal case is accepted");
  TEST_ASSERT(fpclassify(galaxies[0].StellarMass) == FP_SUBNORMAL,
              "the subnormal mass is stored as a subnormal, not flushed or clipped");

  check_memory_leaks();
  return TEST_PASS;
}

/** Mutant: rank density and its ratio to n0 materialized as doubles */
static double materialized_density_mass(double m0, double n0, double alpha, double box) {
  const double volume = box * box * box;
  const double rank_density = 0.5 / volume;
  return m0 * pow(rank_density / n0, -1.0 / alpha);
}

/** Mutant: n0 * volume materialized as one double */
static double materialized_count_mass(double m0, double n0, double alpha, double box) {
  const double volume = box * box * box;
  return m0 * pow(0.5 / (n0 * volume), -1.0 / alpha);
}

/** Whether a mutant's rank-0 mass would pass the reference within the oracle bound */
static int mutant_matches(double mass, uint32_t reference_bits) {
  const float stored = (float)mass;
  return isfinite(stored) && stored > 0.0f &&
         llabs((long long)float_bits(stored) - (long long)reference_bits) <= MAX_ORACLE_ULPS;
}

/**
 * @test   test_edge_battery_detects_materialized_density
 * @brief  The tiny- and huge-volume cases reject implementations that form n_r or n0 * V
 */
int test_edge_battery_detects_materialized_density(void) {
  const struct EdgeCase *tiny = &edge_cases[2];
  const struct EdgeCase *huge = &edge_cases[4];
  TEST_ASSERT(strcmp(tiny->name, "tiny_volume") == 0 && strcmp(huge->name, "huge_volume") == 0,
              "edge table order");

  TEST_ASSERT(!mutant_matches(materialized_density_mass(1.0e5, 1.0e-12, 10.0, 1.0e-105),
                              tiny->reference_bits),
              "tiny volume must catch a materialized rank density");
  TEST_ASSERT(!mutant_matches(materialized_count_mass(1.0e5, 1.0e-12, 10.0, 1.0e-105),
                              tiny->reference_bits),
              "tiny volume must catch a materialized n0 * V");
  TEST_ASSERT(
      !mutant_matches(materialized_count_mass(1.0e-30, 1.0e3, 10.0, 5.6e102), huge->reference_bits),
      "huge volume must catch a materialized n0 * V");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sham_global_rank\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_init_requires_single_post_snapshot_configuration);
  TEST_RUN(test_legacy_sham_rejected_anywhere);
  TEST_RUN(test_mass_scale_parameter_domain);
  TEST_RUN(test_density_slope_and_box_domains);
  TEST_RUN(test_process_snapshot_requires_init);
  TEST_RUN(test_hand_solvable_oracle);
  TEST_RUN(test_ranks_match_independent_oracle);
  TEST_RUN(test_permutation_and_repeat_are_bitwise_identical);
  TEST_RUN(test_rank_is_global_across_fofs);
  TEST_RUN(test_peak_tracking_and_type2_inclusion);
  TEST_RUN(test_stellar_fields_reset_for_every_entry);
  TEST_RUN(test_mpeak_float_storage_bound);
  TEST_RUN(test_malformed_entries_fail_without_writes);
  TEST_RUN(test_empty_and_all_ineligible_populations);
  TEST_RUN(test_no_allocation_growth_across_snapshots);
  TEST_RUN(test_registered_snapshot_dispatch);
  TEST_RUN(test_numerical_edge_battery);
  TEST_RUN(test_edge_battery_detects_materialized_density);

  TEST_SUMMARY();
  return TEST_RESULT();
}
