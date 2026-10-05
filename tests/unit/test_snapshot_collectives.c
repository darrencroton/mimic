/**
 * @file    test_snapshot_collectives.c
 * @brief   Unit tests for the snapshot collectives and the snapshot_distribution startup refusal
 *
 * Runs in the non-MPI unit build, so every collective takes its serial
 * identity path (the MPI sample-sort is proven by the distributed gate's MPI
 * control test). Checks:
 * - module_snapshot_rank() equals a brute-force rank (count the keys that
 *   precede each key under value descending, id ascending) on seeded random
 *   keys drawn from a small value set so ties are common, on all-equal values,
 *   and on infinities and signed zeros; a duplicate id and a NaN value return
 *   -1 and leave the output untouched; `count == 0` succeeds; invalid arguments
 *   are refused
 * - the reductions leave their inputs unchanged and module_snapshot_any()
 *   returns `flag != 0`
 * - the callback gate, entered through hand-built probe modules driven by the
 *   registry (module_system_init(), execute_module_pipeline(),
 *   execute_post_snapshot(), module_system_cleanup()), never a test-only
 *   setter: every collective except module_snapshot_is_root_task() returns -1
 *   from init(), a process_full_halo call, a process_by_galaxy call and
 *   cleanup(), and succeeds from a process_snapshot call and with no callback
 *   running; module_snapshot_is_root_task() returns 1 in every state
 * - with NTask = 2 set by hand, module_system_init() (through
 *   validate_post_snapshot_entries()) refuses a serial_only module configured
 *   under post_snapshot, naming the module and NTask, and accepts a collective
 *   one; with NTask unset the serial_only module is accepted
 */

#include "framework/test_framework.h"
#include "core/fof_workspace.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "core/snapshot_collectives.h"
#include "framework/test_phase_config.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Shared core-test fixtures (config reset) */
#include "framework/core_test_fixtures.h"

#define MAX_KEYS 96

/* ==========================================================================
 * Brute-force oracle and key generation
 * ========================================================================== */

/** @brief Whether key a precedes key b: larger value first, ties by smaller id */
static bool oracle_precedes(const struct SnapshotRankKey *a, const struct SnapshotRankKey *b) {
  if (a->value != b->value) {
    return a->value > b->value;
  }
  return a->id < b->id;
}

/** @brief rank[i] = number of keys preceding keys[i], counted pair by pair */
static void oracle_rank(const struct SnapshotRankKey *keys, int64_t count, int64_t *rank) {
  for (int64_t i = 0; i < count; i++) {
    rank[i] = 0;
    for (int64_t j = 0; j < count; j++) {
      if (j != i && oracle_precedes(&keys[j], &keys[i])) {
        rank[i]++;
      }
    }
  }
}

/** @brief Deterministic xorshift64 generator, so failures reproduce */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng_next(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/**
 * @brief   Fill @p count keys with values from a @p distinct-element set and unique ids
 *
 * Ids are a shuffled run of distinct values spread across negative and positive
 * int64, so neither sorted input order nor small ids are assumed.
 */
static void random_keys(struct SnapshotRankKey *keys, int64_t count, int distinct) {
  for (int64_t i = 0; i < count; i++) {
    keys[i].value = 0.5 * (double)(rng_next() % (uint64_t)distinct) - 1.0;
    keys[i].id = (i - count / 2) * 1000003LL;
  }
  for (int64_t i = count - 1; i > 0; i--) {
    const int64_t j = (int64_t)(rng_next() % (uint64_t)(i + 1));
    const int64_t id = keys[i].id;
    keys[i].id = keys[j].id;
    keys[j].id = id;
  }
}

/** @brief Rank @p keys through module_snapshot_rank() and compare with the oracle */
static bool rank_matches_oracle(const struct SnapshotRankKey *keys, int64_t count) {
  int64_t got[MAX_KEYS];
  int64_t want[MAX_KEYS];
  if (module_snapshot_rank(NULL, keys, count, got) != 0) {
    return false;
  }
  oracle_rank(keys, count, want);
  return memcmp(got, want, (size_t)count * sizeof(int64_t)) == 0;
}

/* ==========================================================================
 * Rank
 * ========================================================================== */

/**
 * @test    test_rank_matches_brute_force
 * @brief   The serial rank equals the brute-force rank on keys with ties
 */
static int test_rank_matches_brute_force(void) {
  struct SnapshotRankKey keys[MAX_KEYS];
  const size_t start_bytes = memory_category_bytes(MEM_UTILITY);

  int trials = 0;
  for (int64_t count = 1; count <= MAX_KEYS; count += 5) {
    for (int distinct = 1; distinct <= 9; distinct += 4) {
      for (int repeat = 0; repeat < 4; repeat++) {
        random_keys(keys, count, distinct);
        TEST_ASSERT(rank_matches_oracle(keys, count), "random keys rank as the oracle does");
        trials++;
      }
    }
  }
  TEST_ASSERT(trials > 200, "the random sweep ran");

  /* All values equal: the rank is the id order alone. */
  random_keys(keys, MAX_KEYS, 1);
  for (int i = 0; i < MAX_KEYS; i++) {
    keys[i].value = 3.25;
  }
  TEST_ASSERT(rank_matches_oracle(keys, MAX_KEYS), "all-equal values rank by ascending id");
  int64_t ranks[MAX_KEYS];
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, keys, MAX_KEYS, ranks), 0, "rank succeeds");
  for (int i = 0; i < MAX_KEYS; i++) {
    const int64_t expected = keys[i].id / 1000003LL + MAX_KEYS / 2;
    TEST_ASSERT_EQUAL(ranks[i], expected, "all-equal values: rank is the id's position");
  }

  /* Infinities and signed zeros take their ordinary order; -0.0 ties 0.0. */
  const struct SnapshotRankKey special[] = {{0.0, 7}, {-INFINITY, 1}, {INFINITY, 9}, {-0.0, 3},
                                            {1.0, 2}, {-1.0, 8},      {INFINITY, 4}};
  const int64_t nspecial = (int64_t)(sizeof(special) / sizeof(special[0]));
  TEST_ASSERT(rank_matches_oracle(special, nspecial), "infinities and signed zeros rank");
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, special, nspecial, ranks), 0, "rank succeeds");
  TEST_ASSERT_EQUAL(ranks[3], 3, "-0.0 (id 3) precedes 0.0 (id 7) on the id tie-break");
  TEST_ASSERT_EQUAL(ranks[0], 4, "0.0 (id 7) follows -0.0");
  TEST_ASSERT_EQUAL(ranks[6], 0, "+inf with the smaller id ranks first");

  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_UTILITY), start_bytes, "scratch is freed");
  return TEST_PASS;
}

/**
 * @test    test_rank_rejects_invalid_keys
 * @brief   A duplicate id or a NaN value returns -1 and writes no rank
 */
static int test_rank_rejects_invalid_keys(void) {
  const size_t start_bytes = memory_category_bytes(MEM_UTILITY);
  int64_t ranks[4] = {-7, -7, -7, -7};

  const struct SnapshotRankKey duplicate[] = {{1.0, 10}, {2.0, 11}, {3.0, 10}, {4.0, 12}};
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, duplicate, 4, ranks), -1,
                    "a duplicate id (with different values) is refused");

  const struct SnapshotRankKey same_key[] = {{1.0, 10}, {1.0, 10}};
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, same_key, 2, ranks), -1,
                    "a repeated identical key is refused");

  const struct SnapshotRankKey with_nan[] = {{1.0, 1}, {NAN, 2}, {3.0, 3}};
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, with_nan, 3, ranks), -1, "a NaN is refused");

  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_EQUAL(ranks[i], -7, "a refused rank leaves the output untouched");
  }

  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, duplicate, -1, ranks), -1, "count < 0 refused");
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, NULL, 2, ranks), -1, "NULL keys refused");
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, duplicate, 2, NULL), -1, "NULL ranks refused");

  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_UTILITY), start_bytes, "scratch is freed");
  return TEST_PASS;
}

/**
 * @test    test_rank_of_no_keys
 * @brief   count == 0 succeeds, with or without arrays
 */
static int test_rank_of_no_keys(void) {
  const struct SnapshotRankKey key = {1.0, 1};
  int64_t rank = -7;
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, NULL, 0, NULL), 0, "empty NULL arrays succeed");
  TEST_ASSERT_EQUAL(module_snapshot_rank(NULL, &key, 0, &rank), 0, "empty arrays succeed");
  TEST_ASSERT_EQUAL(rank, -7, "nothing is written");
  return TEST_PASS;
}

/* ==========================================================================
 * Reductions and predicates
 * ========================================================================== */

/**
 * @test    test_reductions_and_any_are_identities
 * @brief   Serially the reductions return their inputs and any() is flag != 0
 */
static int test_reductions_and_any_are_identities(void) {
  int64_t ints[3] = {-5, 0, INT64_MAX};
  TEST_ASSERT_EQUAL(module_snapshot_sum_i64(NULL, ints, 3), 0, "sum_i64 succeeds");
  TEST_ASSERT(ints[0] == -5 && ints[1] == 0 && ints[2] == INT64_MAX, "sum_i64 is the identity");

  double doubles[3] = {-1.5, 0.1, 1e300};
  TEST_ASSERT_EQUAL(module_snapshot_sum_f64(NULL, doubles, 3), 0, "sum_f64 succeeds");
  TEST_ASSERT(doubles[0] == -1.5 && doubles[1] == 0.1 && doubles[2] == 1e300,
              "sum_f64 is the identity");

  double minima[2] = {-2.0, 4.0};
  double maxima[2] = {3.0, -INFINITY};
  TEST_ASSERT_EQUAL(module_snapshot_min_max_f64(NULL, minima, maxima, 2), 0, "min_max succeeds");
  TEST_ASSERT(minima[0] == -2.0 && minima[1] == 4.0, "minima are the identity");
  TEST_ASSERT(maxima[0] == 3.0 && maxima[1] == -INFINITY, "maxima are the identity");

  TEST_ASSERT_EQUAL(module_snapshot_sum_i64(NULL, NULL, 0), 0, "n == 0 succeeds");
  TEST_ASSERT_EQUAL(module_snapshot_sum_f64(NULL, NULL, 0), 0, "n == 0 succeeds");
  TEST_ASSERT_EQUAL(module_snapshot_min_max_f64(NULL, NULL, NULL, 0), 0, "n == 0 succeeds");
  TEST_ASSERT_EQUAL(module_snapshot_sum_i64(NULL, ints, -1), -1, "n < 0 refused");
  TEST_ASSERT_EQUAL(module_snapshot_sum_f64(NULL, NULL, 1), -1, "NULL values refused");
  TEST_ASSERT_EQUAL(module_snapshot_min_max_f64(NULL, minima, NULL, 1), -1, "NULL maxima refused");

  TEST_ASSERT_EQUAL(module_snapshot_any(NULL, 0), 0, "any(0) is 0");
  TEST_ASSERT_EQUAL(module_snapshot_any(NULL, 1), 1, "any(1) is 1");
  TEST_ASSERT_EQUAL(module_snapshot_any(NULL, -3), 1, "any(non-zero) is 1");

  TEST_ASSERT_EQUAL(module_snapshot_is_root_task(), 1, "a serial run is the root task");
  return TEST_PASS;
}

/* ==========================================================================
 * Callback gate through probe modules
 * ========================================================================== */

/** Return codes of one round of every collective, in a fixed order */
enum { CALL_RANK, CALL_SUM_I64, CALL_SUM_F64, CALL_MIN_MAX, CALL_ANY, CALL_ROOT, CALL_COUNT };

struct CollectiveRound {
  bool ran;
  int rc[CALL_COUNT];
};

/** @brief Call every collective once on small valid inputs, recording each return */
static void call_every_collective(struct CollectiveRound *round) {
  const struct SnapshotRankKey keys[2] = {{1.0, 1}, {2.0, 2}};
  int64_t ranks[2];
  int64_t ints[1] = {1};
  double doubles[1] = {1.0};
  double minima[1] = {1.0};
  double maxima[1] = {1.0};
  round->ran = true;
  round->rc[CALL_RANK] = module_snapshot_rank(NULL, keys, 2, ranks);
  round->rc[CALL_SUM_I64] = module_snapshot_sum_i64(NULL, ints, 1);
  round->rc[CALL_SUM_F64] = module_snapshot_sum_f64(NULL, doubles, 1);
  round->rc[CALL_MIN_MAX] = module_snapshot_min_max_f64(NULL, minima, maxima, 1);
  round->rc[CALL_ANY] = module_snapshot_any(NULL, 0); /* 0 when allowed, -1 when refused */
  round->rc[CALL_ROOT] = module_snapshot_is_root_task();
}

static struct CollectiveRound from_init;
static struct CollectiveRound from_full_halo;
static struct CollectiveRound from_by_galaxy;
static struct CollectiveRound from_snapshot;
static struct CollectiveRound from_cleanup;

static int gate_probe_init(void) {
  call_every_collective(&from_init);
  return 0;
}

static int gate_probe_cleanup(void) {
  call_every_collective(&from_cleanup);
  return 0;
}

static int gate_probe_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ctx;
  (void)halos;
  /* Full-halo and by-galaxy calls both pass one row here; the callback kind
   * the registry is running tells them apart. */
  call_every_collective(module_registry_running_callback(NULL) == RUNNING_CALLBACK_BY_GALAXY
                            ? &from_by_galaxy
                            : &from_full_halo);
  return ngal == 1 ? 0 : 1;
}

static int gate_probe_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                       int64_t count) {
  (void)ctx;
  (void)halos;
  (void)count;
  call_every_collective(&from_snapshot);
  return 0;
}

static int probe_noop(void) { return 0; }

static int probe_noop_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                               int64_t count) {
  (void)ctx;
  (void)halos;
  (void)count;
  return 0;
}

static const enum ProcessingMode gate_modes[] = {
    PROCESSING_MODE_FULL_HALO, PROCESSING_MODE_BY_GALAXY, PROCESSING_MODE_SNAPSHOT};
static const enum ProcessingMode snapshot_modes[] = {PROCESSING_MODE_SNAPSHOT};

/** Advertises FoF and snapshot modes; records every collective's return in each callback */
static struct Module gate_probe_module = {.name = "sc_gate_probe",
                                          .init = gate_probe_init,
                                          .process = gate_probe_process,
                                          .process_snapshot = gate_probe_process_snapshot,
                                          .cleanup = gate_probe_cleanup,
                                          .supported_processing_modes = gate_modes,
                                          .num_supported_modes = 3};

/** Snapshot module left at the default (zero) distribution: serial_only */
static struct Module serial_probe_module = {.name = "sc_serial_probe",
                                            .init = probe_noop,
                                            .process_snapshot = probe_noop_snapshot,
                                            .cleanup = probe_noop,
                                            .supported_processing_modes = snapshot_modes,
                                            .num_supported_modes = 1};

/** Snapshot module declaring snapshot_distribution: collective */
static struct Module collective_probe_module = {.name = "sc_collective_probe",
                                                .init = probe_noop,
                                                .process_snapshot = probe_noop_snapshot,
                                                .cleanup = probe_noop,
                                                .supported_processing_modes = snapshot_modes,
                                                .num_supported_modes = 1,
                                                .snapshot_distribution =
                                                    SNAPSHOT_DISTRIBUTION_COLLECTIVE};

static void ensure_probes_registered(void) {
  static bool registered = false;
  if (!registered) {
    module_registry_add(&gate_probe_module);
    module_registry_add(&serial_probe_module);
    module_registry_add(&collective_probe_module);
    registered = true;
  }
}

/** @brief Add a post_timestep entry (the phase helpers cover pre_timestep and post_snapshot) */
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

/* ---- log capture ---- */

static FILE *log_file = NULL;
static char log_text[1 << 16];

/** @brief Route every log line at INFO and above into a temporary file */
static void capture_log(void) {
  log_file = tmpfile();
  initialize_error_handling(LOG_LEVEL_INFO, log_file);
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
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  return log_text;
}

/** @brief Release a gate or refusal case; safe after a failed assertion */
static int release_case(void) {
  if (log_file != NULL) {
    (void)captured_log();
  }
  NTask = 0;
  return module_system_cleanup();
}

/** @brief Whether every collective but is_root_task returned @p expected and is_root_task 1 */
static bool round_is(const struct CollectiveRound *round, int expected) {
  if (!round->ran || round->rc[CALL_ROOT] != 1) {
    return false;
  }
  for (int c = 0; c < CALL_ROOT; c++) {
    if (round->rc[c] != expected) {
      return false;
    }
  }
  return true;
}

/** @brief Body of test_collectives_refused_outside_snapshot_dispatch */
static int gate_body(void) {
  reset_config();
  ensure_probes_registered();
  memset(&from_init, 0, sizeof(from_init));
  memset(&from_full_halo, 0, sizeof(from_full_halo));
  memset(&from_by_galaxy, 0, sizeof(from_by_galaxy));
  memset(&from_snapshot, 0, sizeof(from_snapshot));
  memset(&from_cleanup, 0, sizeof(from_cleanup));
  MimicConfig.SubSteps = 1;
  test_pre_timestep_add("sc_gate_probe", PROCESSING_MODE_FULL_HALO);
  add_post_timestep("sc_gate_probe", PROCESSING_MODE_BY_GALAXY);
  test_post_snapshot_add("sc_gate_probe", PROCESSING_MODE_SNAPSHOT);

  /* No callback running: everything is allowed. */
  struct CollectiveRound direct;
  memset(&direct, 0, sizeof(direct));
  call_every_collective(&direct);
  TEST_ASSERT(round_is(&direct, 0), "with no callback running every collective succeeds");

  /* One Type 0 row with a galaxy, so both the full-halo and by-galaxy passes run. */
  static struct GalaxyData galaxy;
  static struct Halo row;
  memset(&galaxy, 0, sizeof(galaxy));
  memset(&row, 0, sizeof(row));
  row.Type = 0;
  row.galaxy = &galaxy;
  struct FoFWorkspace ws;
  memset(&ws, 0, sizeof(ws));
  ws.halos = &row;
  ws.count = 1;
  ws.capacity = 1;
  ws.base_count = 1;
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.num_substeps = 1;
  ctx.central_galaxy = &row;
  ctx.params = &MimicConfig;

  capture_log();
  TEST_ASSERT_EQUAL(module_system_init(), 0, "the probe pipeline initialises");
  execute_module_pipeline(&ctx, &ws);
  struct SnapshotContext snapshot_ctx = {.snapshot_number = 5, .params = &MimicConfig};
  execute_post_snapshot(&snapshot_ctx, &row, 1);
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "the probe pipeline cleans up");
  const char *log = captured_log();

  TEST_ASSERT(round_is(&from_init, -1), "init(): refused, is_root_task still 1");
  TEST_ASSERT(round_is(&from_full_halo, -1), "full-halo callback: refused, is_root_task 1");
  TEST_ASSERT(round_is(&from_by_galaxy, -1), "by-galaxy callback: refused, is_root_task 1");
  TEST_ASSERT(round_is(&from_cleanup, -1), "cleanup(): refused, is_root_task still 1");
  TEST_ASSERT(round_is(&from_snapshot, 0), "process_snapshot callback: every collective succeeds");
  TEST_ASSERT(strstr(log, "module_snapshot_rank refused for module 'sc_gate_probe': called from a "
                          "process_full_halo callback") != NULL,
              "the full-halo refusal names the function, the module and the callback kind");
  TEST_ASSERT(strstr(log, "module_snapshot_any refused for module 'sc_gate_probe': called from "
                          "init()") != NULL,
              "the init refusal names the function, the module and init()");

  /* Back outside every callback: allowed again. */
  memset(&direct, 0, sizeof(direct));
  call_every_collective(&direct);
  TEST_ASSERT(round_is(&direct, 0), "after the pipeline every collective succeeds again");
  return TEST_PASS;
}

/**
 * @test    test_collectives_refused_outside_snapshot_dispatch
 * @brief   The gate refuses init/full-halo/by-galaxy/cleanup and allows snapshot and none
 */
static int test_collectives_refused_outside_snapshot_dispatch(void) {
  const int result = gate_body();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return result;
}

/* ==========================================================================
 * Startup refusal of serial_only snapshot modules under NTask > 1
 * ========================================================================== */

/** @brief Configure one post_snapshot entry and initialise the module system */
static int init_with_post_snapshot(const char *module_name) {
  reset_config();
  ensure_probes_registered();
  MimicConfig.SubSteps = 1;
  test_post_snapshot_add(module_name, PROCESSING_MODE_SNAPSHOT);
  return module_system_init();
}

/** @brief Body of test_post_snapshot_refuses_serial_only_under_multiple_tasks */
static int refusal_body(void) {
  NTask = 2;
  capture_log();
  const int serial_rc = init_with_post_snapshot("sc_serial_probe");
  const char *log = captured_log();
  TEST_ASSERT_EQUAL(serial_rc, -1, "a serial_only post_snapshot module is refused at NTask = 2");
  TEST_ASSERT(strstr(log, "Module 'sc_serial_probe' is snapshot_distribution: serial_only") != NULL,
              "the refusal names the module");
  TEST_ASSERT(strstr(log, "NTask = 2") != NULL, "the refusal names NTask");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup after the refusal succeeds");

  TEST_ASSERT_EQUAL(init_with_post_snapshot("sc_collective_probe"), 0,
                    "a collective post_snapshot module is accepted at NTask = 2");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");

  NTask = 0;
  TEST_ASSERT_EQUAL(init_with_post_snapshot("sc_serial_probe"), 0,
                    "a serial_only module is accepted in a serial run");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  NTask = 1;
  TEST_ASSERT_EQUAL(init_with_post_snapshot("sc_serial_probe"), 0,
                    "a serial_only module is accepted at NTask = 1");
  return TEST_PASS;
}

/**
 * @test    test_post_snapshot_refuses_serial_only_under_multiple_tasks
 * @brief   validate_post_snapshot_entries() refuses serial_only and accepts collective at NTask 2
 */
static int test_post_snapshot_refuses_serial_only_under_multiple_tasks(void) {
  const int result = refusal_body();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return result;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: snapshot collectives and snapshot_distribution\n");
  printf("============================================================\n");
  printf("%s", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_rank_matches_brute_force);
  TEST_RUN(test_rank_rejects_invalid_keys);
  TEST_RUN(test_rank_of_no_keys);
  TEST_RUN(test_reductions_and_any_are_identities);
  TEST_RUN(test_collectives_refused_outside_snapshot_dispatch);
  TEST_RUN(test_post_snapshot_refuses_serial_only_under_multiple_tasks);

  TEST_SUMMARY();
  return TEST_RESULT();
}
