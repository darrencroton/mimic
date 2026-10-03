/**
 * @file    test_snapshot_module_contract.c
 * @brief   Unit tests for the typed snapshot callback and its registration contract
 *
 * Validates:
 * - The C processing-mode table: string names, parsing and callback-family lookup,
 *   including fail-closed handling of unknown strings and enum values
 * - Family-aware registration: snapshot-only, FoF-only and dual-mode modules are
 *   accepted with a NULL unused callback, and every missing advertised callback,
 *   empty mode list or unknown mode value is fatal
 * - Real callback invocations through registered Module structs and the test-build
 *   fixtures (test_snapshot_fixture snapshot-only, test_fixture dual mode)
 * - FoF phases reject a snapshot-family mode, so a snapshot callback is never
 *   skipped silently or a NULL FoF callback dereferenced
 * - The post_snapshot phase: lifecycle collection, phase visiting order and
 *   cleanup; startup validation of its entries (and of process_snapshot in the
 *   fixed FoF phases) before any init(), with the diagnostic naming phase and
 *   module; execute_post_snapshot() calling each entry once in YAML order (also
 *   when a FoF placement makes pipeline order differ) with the caller's
 *   population (count zero with a non-NULL buffer included), writes visible to
 *   the next entry, the event guard, fatal callback failure, its own fatal
 *   argument and state guards, and a real marshalled buffer that never contains
 *   a Type 3 entry
 *
 * Registration aborts through FATAL_ERROR and the registry has no unregister, so
 * each hand-built registration case runs in a forked child (child_capture.h).
 * The unit runner always builds with MIMIC_TEST_BUILD, so the framework fixture
 * modules are linked and registered.
 */

#include "framework/test_framework.h"
#include "framework/child_capture.h"
#include "../../core/fof_workspace.h"
#include "../../core/module_registry.h"
#include "framework/test_phase_config.h"
#include "../../core/module_interface.h"
#include "../../core/output_buffer.h"
#include "../../include/types.h"
#include "../../include/proto.h"
#include "../../include/globals.h"
#include "../../util/error.h"
#include "../../util/memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Fixture callbacks: linked from src/module_system/test_*fixture/ in test builds */
extern int test_fixture_init(void);
extern int test_fixture_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int test_fixture_process_snapshot(const struct SnapshotContext *ctx,
                                         const struct Halo *halos, int64_t count);
extern int test_fixture_cleanup(void);
extern int test_snapshot_fixture_init(void);
extern int test_snapshot_fixture_process_snapshot(const struct SnapshotContext *ctx,
                                                  const struct Halo *halos, int64_t count);
extern int test_snapshot_fixture_cleanup(void);

/** Value test_snapshot_fixture writes (TEST_SNAPSHOT_FIXTURE_VALUE in its source) */
#define SNAPSHOT_FIXTURE_VALUE 0.5f

/** Value the snapshot probe module writes (inside TestDummyProperty's [0, 1] range) */
#define PROBE_VALUE 0.125f

/** Size of the synthetic snapshot population */
#define POPULATION_SIZE 3

static int modules_registered = 0;

static void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }

static void ensure_modules_registered(void) {
  if (!modules_registered) {
    register_all_modules();
    modules_registered = 1;
  }
}

static void set_test_fixture_params(double dummy_val) {
  strcpy(MimicConfig.ModelParams[0].param_name, "TestFixtureDummyParameter");
  snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "%.10g", dummy_val);
  strcpy(MimicConfig.ModelParams[1].param_name, "TestFixtureEnableLogging");
  snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "%d", 0);
  MimicConfig.NumModelParams = 2;
}

/**
 * @brief   Build a synthetic snapshot population of Types 0, 1 and 2
 *
 * Every entry has a galaxy and a sentinel TestDummyProperty of 0, and halo
 * fields that a callback must leave untouched.
 */
static void build_population(struct Halo *halos, struct GalaxyData *galaxies, int64_t count) {
  memset(halos, 0, (size_t)count * sizeof(*halos));
  memset(galaxies, 0, (size_t)count * sizeof(*galaxies));
  for (int64_t i = 0; i < count; i++) {
    halos[i].Type = (int)(i % 3);
    halos[i].UniqueGalaxyID = 100 + i;
    halos[i].CentralHalo = 7; /* workspace-local; never a snapshot offset */
    halos[i].galaxy = &galaxies[i];
  }
}

static struct SnapshotContext make_snapshot_context(void) {
  struct SnapshotContext ctx = {
      .snapshot_number = 5, .redshift = 1.25, .time = 3.5, .params = &MimicConfig};
  return ctx;
}

/* ==========================================================================
 * Hand-built registration cases (each runs in a forked child)
 * ========================================================================== */

static const enum ProcessingMode snapshot_only_modes[] = {PROCESSING_MODE_SNAPSHOT};
static const enum ProcessingMode fof_only_modes[] = {PROCESSING_MODE_BY_GALAXY,
                                                     PROCESSING_MODE_FULL_HALO};
static const enum ProcessingMode dual_modes[] = {PROCESSING_MODE_FULL_HALO,
                                                 PROCESSING_MODE_SNAPSHOT};
static const enum ProcessingMode count_mode[] = {PROCESSING_MODE_COUNT};
static const enum ProcessingMode unknown_mode[] = {(enum ProcessingMode)99};

/** Lifecycle of the snapshot-only fixture, shared by most registration cases */
#define SNAPSHOT_LIFECYCLE                                                                         \
  .init = test_snapshot_fixture_init, .cleanup = test_snapshot_fixture_cleanup

/** Lifecycle of the dual-mode test_fixture */
#define FIXTURE_LIFECYCLE .init = test_fixture_init, .cleanup = test_fixture_cleanup

/**
 * @brief   Every hand-built registration case, by name
 *
 * Accepted cases bind the fixtures' real callbacks; each rejected case differs
 * from an accepted one by exactly the defect its name states.
 */
static const struct Module registration_cases[] = {
    {.name = "snapshot_only",
     SNAPSHOT_LIFECYCLE,
     .process_snapshot = test_snapshot_fixture_process_snapshot,
     .supported_processing_modes = snapshot_only_modes,
     .num_supported_modes = 1},
    {.name = "snapshot_missing_callback",
     SNAPSHOT_LIFECYCLE,
     .supported_processing_modes = snapshot_only_modes,
     .num_supported_modes = 1},
    {.name = "fof_only",
     FIXTURE_LIFECYCLE,
     .process = test_fixture_process,
     .supported_processing_modes = fof_only_modes,
     .num_supported_modes = 2},
    {.name = "fof_missing_callback",
     FIXTURE_LIFECYCLE,
     .supported_processing_modes = fof_only_modes,
     .num_supported_modes = 2},
    {.name = "dual",
     FIXTURE_LIFECYCLE,
     .process = test_fixture_process,
     .process_snapshot = test_fixture_process_snapshot,
     .supported_processing_modes = dual_modes,
     .num_supported_modes = 2},
    {.name = "dual_missing_fof",
     FIXTURE_LIFECYCLE,
     .process_snapshot = test_fixture_process_snapshot,
     .supported_processing_modes = dual_modes,
     .num_supported_modes = 2},
    {.name = "dual_missing_snapshot",
     FIXTURE_LIFECYCLE,
     .process = test_fixture_process,
     .supported_processing_modes = dual_modes,
     .num_supported_modes = 2},
    {.name = "empty_modes",
     SNAPSHOT_LIFECYCLE,
     .process_snapshot = test_snapshot_fixture_process_snapshot,
     .supported_processing_modes = snapshot_only_modes,
     .num_supported_modes = 0},
    {.name = "null_modes",
     SNAPSHOT_LIFECYCLE,
     .process_snapshot = test_snapshot_fixture_process_snapshot,
     .supported_processing_modes = NULL,
     .num_supported_modes = 1},
    {.name = "unknown_mode_value",
     SNAPSHOT_LIFECYCLE,
     .process = test_fixture_process,
     .process_snapshot = test_snapshot_fixture_process_snapshot,
     .supported_processing_modes = unknown_mode,
     .num_supported_modes = 1},
    {.name = "count_mode_value",
     SNAPSHOT_LIFECYCLE,
     .process = test_fixture_process,
     .process_snapshot = test_snapshot_fixture_process_snapshot,
     .supported_processing_modes = count_mode,
     .num_supported_modes = 1},
    {.name = "missing_init",
     .init = NULL,
     .cleanup = test_snapshot_fixture_cleanup,
     .process_snapshot = test_snapshot_fixture_process_snapshot,
     .supported_processing_modes = snapshot_only_modes,
     .num_supported_modes = 1},
};

#undef SNAPSHOT_LIFECYCLE
#undef FIXTURE_LIFECYCLE

/**
 * @brief   Look up the Module for a named registration case
 *
 * Runs only in a forked child; an unknown name exits the child non-zero so a
 * mistyped case can never pass as an accepted registration.
 */
static struct Module make_case_module(const char *which) {
  for (size_t i = 0; i < sizeof(registration_cases) / sizeof(registration_cases[0]); i++) {
    if (strcmp(registration_cases[i].name, which) == 0) {
      return registration_cases[i];
    }
  }
  fprintf(stderr, "no registration case named '%s'\n", which);
  exit(3);
}

/**
 * @brief   Child body: register the named case, then invoke its callbacks
 *
 * A rejected case aborts inside module_registry_add(). An accepted case calls
 * every non-NULL callback through the registered struct on a real population
 * and exits non-zero (message on stderr) if any result breaks the contract.
 * The FoF callback is called directly; the snapshot callback is dispatched by
 * execute_post_snapshot(), because the snapshot fixture accepts a call only
 * under snapshot dispatch (its event-emission probe must be rejected).
 */
static void register_case_body(const char *which) {
  static struct Module mod;
  mod = make_case_module(which);
  reset_config();
  module_registry_add(&mod);

  set_test_fixture_params(0.75);
  if (mod.process_snapshot != NULL) {
    test_post_snapshot_add(which, PROCESSING_MODE_SNAPSHOT);
  } else {
    test_pre_timestep_add(which, mod.supported_processing_modes[0]);
  }
  if (module_system_init() != 0) {
    fprintf(stderr, "case %s: module_system_init failed\n", which);
    exit(2);
  }

  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  struct SnapshotContext ctx = make_snapshot_context();

  if (mod.process != NULL) {
    struct ModuleContext fof_ctx;
    memset(&fof_ctx, 0, sizeof(fof_ctx));
    fof_ctx.params = &MimicConfig;
    build_population(halos, galaxies, POPULATION_SIZE);
    if (mod.process(&fof_ctx, halos, POPULATION_SIZE) != 0 ||
        !(galaxies[0].TestDummyProperty > 0.0f)) {
      fprintf(stderr, "case %s: FoF process did not write the Type 0 galaxy\n", which);
      exit(5);
    }
  }

  if (mod.process_snapshot != NULL) {
    /* A failing callback is fatal inside execute_post_snapshot(), so reaching
     * the checks below means both calls returned 0. */
    build_population(halos, galaxies, POPULATION_SIZE);
    execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
    execute_post_snapshot(&ctx, NULL, 0);
    for (int64_t i = 0; i < POPULATION_SIZE; i++) {
      if (!(galaxies[i].TestDummyProperty > 0.0f)) {
        fprintf(stderr, "case %s: entry %lld not written by process_snapshot\n", which,
                (long long)i);
        exit(4);
      }
    }
  }
  if (module_system_cleanup() != 0) {
    fprintf(stderr, "case %s: module_system_cleanup failed\n", which);
    exit(6);
  }
}

/** @brief Child body: run the generated registration of every compiled module */
static void register_generated_body(const char *unused) {
  (void)unused;
  register_all_modules();
}

/* ==========================================================================
 * Tests
 * ========================================================================== */

/**
 * @test    test_mode_table_round_trip
 * @brief   Every enumerated mode has a name, parses back, and maps to its family
 */
int test_mode_table_round_trip(void) {
  /* ===== SETUP ===== */
  const enum ModuleCallbackFamily expected[PROCESSING_MODE_COUNT] = {
      [PROCESSING_MODE_FULL_HALO] = MODULE_CALLBACK_FAMILY_FOF,
      [PROCESSING_MODE_PER_EVENT] = MODULE_CALLBACK_FAMILY_FOF,
      [PROCESSING_MODE_BY_GALAXY] = MODULE_CALLBACK_FAMILY_FOF,
      [PROCESSING_MODE_SNAPSHOT] = MODULE_CALLBACK_FAMILY_SNAPSHOT,
  };

  /* ===== EXECUTE / VALIDATE ===== */
  TEST_ASSERT_EQUAL(PROCESSING_MODE_FULL_HALO, 0, "existing enum value FULL_HALO unchanged");
  TEST_ASSERT_EQUAL(PROCESSING_MODE_PER_EVENT, 1, "existing enum value PER_EVENT unchanged");
  TEST_ASSERT_EQUAL(PROCESSING_MODE_BY_GALAXY, 2, "existing enum value BY_GALAXY unchanged");

  for (int m = 0; m < PROCESSING_MODE_COUNT; m++) {
    const char *name = processing_mode_to_string((enum ProcessingMode)m);
    TEST_ASSERT(strcmp(name, "unknown") != 0, "every enumerated mode has a configuration name");

    enum ProcessingMode parsed = PROCESSING_MODE_COUNT;
    TEST_ASSERT_EQUAL(processing_mode_from_string(name, &parsed), 0, "mode name parses");
    TEST_ASSERT_EQUAL((int)parsed, m, "mode name parses back to the same enumerator");

    enum ModuleCallbackFamily family;
    TEST_ASSERT_EQUAL(processing_mode_family((enum ProcessingMode)m, &family), 0,
                      "every enumerated mode has a callback family");
    TEST_ASSERT_EQUAL((int)family, (int)expected[m], "mode maps to its explicit callback family");
  }

  TEST_ASSERT_STRING_EQUAL(processing_mode_to_string(PROCESSING_MODE_SNAPSHOT), "process_snapshot",
                           "snapshot mode configuration name");
  return TEST_PASS;
}

/**
 * @test    test_mode_lookup_fails_closed
 * @brief   Unknown strings and enum values are rejected, never defaulted to a family
 */
int test_mode_lookup_fails_closed(void) {
  /* ===== SETUP ===== */
  const char *bad_names[] = {"process_global", "", "PROCESS_SNAPSHOT", "process_snapshot "};
  const enum ProcessingMode bad_modes[] = {PROCESSING_MODE_COUNT, (enum ProcessingMode) - 1,
                                           (enum ProcessingMode)99};

  /* ===== EXECUTE / VALIDATE ===== */
  for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
    enum ProcessingMode out = PROCESSING_MODE_BY_GALAXY;
    TEST_ASSERT_EQUAL(processing_mode_from_string(bad_names[i], &out), -1,
                      "unknown mode string is rejected");
    TEST_ASSERT_EQUAL((int)out, (int)PROCESSING_MODE_BY_GALAXY, "output untouched on rejection");
  }
  enum ProcessingMode out = PROCESSING_MODE_BY_GALAXY;
  TEST_ASSERT_EQUAL(processing_mode_from_string(NULL, &out), -1, "NULL mode string is rejected");

  for (size_t i = 0; i < sizeof(bad_modes) / sizeof(bad_modes[0]); i++) {
    enum ModuleCallbackFamily family = MODULE_CALLBACK_FAMILY_SNAPSHOT;
    TEST_ASSERT_EQUAL(processing_mode_family(bad_modes[i], &family), -1,
                      "unknown enum value has no callback family");
    TEST_ASSERT_EQUAL((int)family, (int)MODULE_CALLBACK_FAMILY_SNAPSHOT,
                      "family untouched on rejection");
    TEST_ASSERT_STRING_EQUAL(processing_mode_to_string(bad_modes[i]), "unknown",
                             "unknown enum value is named 'unknown'");
  }
  return TEST_PASS;
}

/**
 * @test    test_registration_accepts_each_family
 * @brief   Snapshot-only, FoF-only and dual-mode modules register with a NULL unused
 *          callback, and their registered callbacks run on a real population
 */
int test_registration_accepts_each_family(void) {
  const char *cases[] = {"snapshot_only", "fof_only", "dual"};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int ok = expect_success(cases[i], register_case_body);
    if (ok != 1) {
      fprintf(stderr, "  accepted case: %s\n", cases[i]);
    }
    TEST_ASSERT_EQUAL(ok, 1, "valid callback family combination registers and runs");
  }
  return TEST_PASS;
}

/**
 * @test    test_registration_rejects_missing_callbacks
 * @brief   Every advertised family must have its typed callback
 */
int test_registration_rejects_missing_callbacks(void) {
  struct {
    const char *which;
    const char *needle;
  } cases[] = {
      {"snapshot_missing_callback", "NULL process_snapshot callback"},
      {"fof_missing_callback", "NULL process callback"},
      {"dual_missing_snapshot", "NULL process_snapshot callback"},
      {"dual_missing_fof", "NULL process callback"},
      {"missing_init", "NULL init or cleanup"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int ok = expect_fatal(cases[i].which, register_case_body, cases[i].which, cases[i].needle);
    TEST_ASSERT_EQUAL(ok, 1, "missing advertised callback is fatal at registration");
  }
  return TEST_PASS;
}

/**
 * @test    test_registration_rejects_invalid_modes
 * @brief   Empty, NULL and unknown mode lists fail closed at registration
 */
int test_registration_rejects_invalid_modes(void) {
  struct {
    const char *which;
    const char *needle;
  } cases[] = {
      {"empty_modes", "advertises no processing modes"},
      {"null_modes", "advertises no processing modes"},
      {"unknown_mode_value", "unknown processing mode value 99"},
      {"count_mode_value", "unknown processing mode value"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int ok = expect_fatal(cases[i].which, register_case_body, cases[i].which, cases[i].needle);
    TEST_ASSERT_EQUAL(ok, 1, "invalid mode list is fatal at registration");
  }
  return TEST_PASS;
}

/**
 * @test    test_generated_fixtures_register
 * @brief   Generated registration of every fixture family passes the family checks
 *
 * register_all_modules() includes the snapshot-only, dual-mode and FoF-only
 * event fixtures in a test build; any missing advertised callback would abort.
 */
int test_generated_fixtures_register(void) {
  int ok = expect_success(NULL, register_generated_body);
  TEST_ASSERT_EQUAL(ok, 1, "generated registration of all fixture families succeeds");
  return TEST_PASS;
}

/* Phase setup helpers, defined with the snapshot probe below. */
static void prepare_phase_config(double dummy_param);
static int init_probe_phase(const char *const *modules, int nmodules);

/** @brief Child body: dispatch the snapshot fixture with a NULL galaxy pointer */
static void snapshot_fixture_null_galaxy_body(const char *unused) {
  (void)unused;
  static const char *const modules[] = {"test_snapshot_fixture"};
  if (init_probe_phase(modules, 1) != 0) {
    exit(2);
  }
  static struct Halo halos[POPULATION_SIZE];
  static struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  halos[1].galaxy = NULL;
  struct SnapshotContext ctx = make_snapshot_context();
  execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
}

/**
 * @test    test_snapshot_fixture_callback_contract
 * @brief   The snapshot-only fixture writes only through galaxy pointers, honours
 *          count zero, rejects contract violations, and accepts a call only under
 *          snapshot dispatch
 */
int test_snapshot_fixture_callback_contract(void) {
  /* ===== SETUP ===== */
  static const char *const modules[] = {"test_snapshot_fixture"};
  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  struct Halo before[POPULATION_SIZE];
  memcpy(before, halos, sizeof(halos));
  struct SnapshotContext ctx = make_snapshot_context();

  /* ===== EXECUTE / VALIDATE: outside dispatch ===== */
  /* With no dispatch active, module_emit_event() takes its direct-unit-test
   * shortcut and accepts the fixture's probe event, so the fixture refuses. */
  TEST_ASSERT_EQUAL(test_snapshot_fixture_init(), 0, "snapshot fixture init succeeds");
  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, halos, POPULATION_SIZE), -1,
                    "a direct call outside snapshot dispatch is refused (probe event accepted)");
  TEST_ASSERT(galaxies[0].TestDummyProperty == 0.0f, "a refused call writes nothing");
  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, NULL, POPULATION_SIZE), -1,
                    "NULL population with a positive count is rejected");
  TEST_ASSERT_EQUAL(test_snapshot_fixture_cleanup(), 0, "snapshot fixture cleanup succeeds");

  /* ===== EXECUTE / VALIDATE: through execute_post_snapshot() ===== */
  TEST_ASSERT_EQUAL(init_probe_phase(modules, 1), 0, "snapshot-only phase initialises");
  execute_post_snapshot(&ctx, NULL, 0);
  execute_post_snapshot(&ctx, halos, 0);
  TEST_ASSERT(galaxies[0].TestDummyProperty == 0.0f, "count zero writes nothing");

  execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  for (int64_t i = 0; i < POPULATION_SIZE; i++) {
    TEST_ASSERT(galaxies[i].TestDummyProperty == SNAPSHOT_FIXTURE_VALUE,
                "every Type 0/1/2 entry is written through its galaxy pointer");
  }
  TEST_ASSERT(memcmp(before, halos, sizeof(halos)) == 0, "no halo field or pointer changes");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "module system cleanup succeeds");

  TEST_ASSERT_EQUAL(expect_fatal("null_galaxy", snapshot_fixture_null_galaxy_body,
                                 "Module 'test_snapshot_fixture' failed in phase 'post_snapshot'",
                                 "return code -1"),
                    1, "a NULL galaxy pointer fails the dispatched call");

  /* ===== CLEANUP ===== */
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_dual_fixture_snapshot_callback
 * @brief   The dual-mode test_fixture initialises once through the module system
 *          and its snapshot callback writes the configured parameter
 */
int test_dual_fixture_snapshot_callback(void) {
  /* ===== SETUP ===== */
  prepare_phase_config(0.75);
  test_pre_timestep_add("test_fixture", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);

  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  struct Halo before[POPULATION_SIZE];
  memcpy(before, halos, sizeof(halos));
  struct SnapshotContext ctx = make_snapshot_context();

  /* ===== EXECUTE ===== */
  int init_result = module_system_init();
  int snapshot_result = test_fixture_process_snapshot(&ctx, halos, POPULATION_SIZE);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(init_result, 0, "dual-mode fixture initialises in FoF phases");
  TEST_ASSERT_EQUAL(module_system_pipeline_count(), 1,
                    "a module configured in two phases is initialised once");
  TEST_ASSERT_EQUAL(snapshot_result, 0, "dual-mode snapshot callback succeeds");
  for (int64_t i = 0; i < POPULATION_SIZE; i++) {
    TEST_ASSERT(galaxies[i].TestDummyProperty == 0.75f,
                "snapshot callback writes the parameter to every entry");
  }
  TEST_ASSERT(memcmp(before, halos, sizeof(halos)) == 0, "no halo field or pointer changes");
  TEST_ASSERT_EQUAL(test_fixture_process_snapshot(&ctx, NULL, 0), 0,
                    "count zero with a NULL population succeeds");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_fof_phase_rejects_snapshot_family
 * @brief   FoF phases accept only FoF modes the module supports
 *
 * A snapshot-only module cannot be configured with a FoF mode, and the
 * snapshot mode is not accepted in a FoF phase even by a dual-mode module.
 */
int test_fof_phase_rejects_snapshot_family(void) {
  struct {
    const char *module;
    enum ProcessingMode mode;
  } cases[] = {
      {"test_snapshot_fixture", PROCESSING_MODE_FULL_HALO},
      {"test_snapshot_fixture", PROCESSING_MODE_SNAPSHOT},
      {"test_fixture", PROCESSING_MODE_SNAPSHOT},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    /* ===== SETUP ===== */
    prepare_phase_config(0.5);
    test_phase_add("galaxy_physics", cases[i].module, cases[i].mode);

    /* ===== EXECUTE / VALIDATE ===== */
    TEST_ASSERT_EQUAL(module_system_init(), -1, "FoF phase rejects an unsupported family/mode");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/* ==========================================================================
 * post_snapshot phase: a recording probe module
 * ========================================================================== */

/** What the probe saw on one call */
struct ProbeCall {
  int sequence;             /**< Global call order across every probe instance */
  int snapshot_number;      /**< ctx->snapshot_number */
  double redshift;          /**< ctx->redshift */
  double time;              /**< ctx->time */
  const void *params;       /**< ctx->params */
  const struct Halo *halos; /**< The population pointer it was handed */
  int64_t count;            /**< The count it was handed */
  float seen[8];            /**< TestDummyProperty of the first entries, before writing */
  int types[8];             /**< Type of the first entries */
  int emit_result;          /**< module_emit_event() result from inside the callback */
};

#define PROBE_MAX_CALLS 8

static struct ProbeCall probe_calls[PROBE_MAX_CALLS];
static int probe_ncalls = 0;
static int probe_sequence = 0;
static int probe_return_code = 0;
static int probe_init_calls = 0;
static int probe_reenter = 0; /**< Non-zero: the callback re-enters execute_post_snapshot() */

static void reset_probe(void) {
  memset(probe_calls, 0, sizeof(probe_calls));
  probe_ncalls = 0;
  probe_sequence = 0;
  probe_return_code = 0;
  probe_init_calls = 0;
  probe_reenter = 0;
}

static int probe_init(void) {
  probe_init_calls++;
  return 0;
}
static int probe_cleanup(void) { return 0; }

/**
 * @brief   Snapshot callback that records its arguments and what it saw, then
 *          writes PROBE_VALUE through every galaxy pointer
 */
static int probe_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                  int64_t count) {
  if (probe_reenter) {
    execute_post_snapshot(ctx, halos, count); /* must be fatal: dispatch is already active */
  }
  if (probe_ncalls < PROBE_MAX_CALLS) {
    struct ProbeCall *call = &probe_calls[probe_ncalls];
    call->sequence = ++probe_sequence;
    call->snapshot_number = ctx->snapshot_number;
    call->redshift = ctx->redshift;
    call->time = ctx->time;
    call->params = ctx->params;
    call->halos = halos;
    call->count = count;
    for (int64_t i = 0; i < count && i < 8; i++) {
      call->seen[i] = halos[i].galaxy->TestDummyProperty;
      call->types[i] = halos[i].Type;
    }
    struct ModuleContext fof_ctx;
    memset(&fof_ctx, 0, sizeof(fof_ctx));
    call->emit_result = module_emit_event(&fof_ctx, 1, 0, 0, 0.0, 0.0);
  }
  probe_ncalls++;
  for (int64_t i = 0; i < count; i++) {
    halos[i].galaxy->TestDummyProperty = PROBE_VALUE;
  }
  return probe_return_code;
}

static const enum ProcessingMode probe_modes[] = {PROCESSING_MODE_SNAPSHOT};

static struct Module probe_module = {.name = "snapshot_probe",
                                     .init = probe_init,
                                     .process = NULL,
                                     .process_snapshot = probe_process_snapshot,
                                     .cleanup = probe_cleanup,
                                     .supported_processing_modes = probe_modes,
                                     .num_supported_modes = 1};

static int probe_registered = 0;

/** @brief Register the generated modules and the probe once in this process */
static void ensure_probe_registered(void) {
  ensure_modules_registered();
  if (!probe_registered) {
    module_registry_add(&probe_module);
    probe_registered = 1;
  }
}

/**
 * @brief   Start a fresh configuration: register the generated modules and the
 *          probe, reset the probe record, and set the fixture parameters
 *
 * @param   dummy_param  Value test_fixture writes (TestFixtureDummyParameter)
 */
static void prepare_phase_config(double dummy_param) {
  reset_config();
  init_memory_system(0);
  ensure_probe_registered();
  reset_probe();
  set_test_fixture_params(dummy_param);
  MimicConfig.SubSteps = 1;
}

/** @brief Configure post_snapshot from a module list (all process_snapshot) and init */
static int init_probe_phase(const char *const *modules, int nmodules) {
  prepare_phase_config(0.75);
  for (int i = 0; i < nmodules; i++) {
    test_post_snapshot_add(modules[i], PROCESSING_MODE_SNAPSHOT);
  }
  return module_system_init();
}

/** Capacity of the buffer record_phase_visitor() appends to */
#define PHASE_LIST_CAPACITY 512

/** @brief Visitor recording the phase names for_each_phase visits, comma-separated */
static void record_phase_visitor(const char *phase_name, struct PhaseModuleConfig *modules,
                                 int num_modules, void *userdata) {
  (void)modules;
  char *buffer = userdata;
  char entry[96];
  snprintf(entry, sizeof(entry), "%s%s:%d", buffer[0] != '\0' ? "," : "", phase_name, num_modules);
  strncat(buffer, entry, PHASE_LIST_CAPACITY - 1 - strlen(buffer));
}

/**
 * @test    test_post_snapshot_dispatch_order_and_visibility
 * @brief   Each entry runs once, in YAML order, on the caller's own population,
 *          and sees what the previous entry wrote
 */
int test_post_snapshot_dispatch_order_and_visibility(void) {
  /* ===== SETUP ===== */
  static const char *const fixture_then_probe[] = {"test_snapshot_fixture", "snapshot_probe"};
  static const char *const probe_then_fixture[] = {"snapshot_probe", "test_snapshot_fixture"};
  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  struct SnapshotContext ctx = make_snapshot_context();

  /* ===== EXECUTE / VALIDATE: fixture first ===== */
  TEST_ASSERT_EQUAL(init_probe_phase(fixture_then_probe, 2), 0, "two-entry phase initialises");
  build_population(halos, galaxies, POPULATION_SIZE);
  execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  TEST_ASSERT_EQUAL(probe_ncalls, 1, "the probe runs exactly once per dispatch");
  TEST_ASSERT(probe_calls[0].halos == halos, "the probe borrows the caller's buffer, not a copy");
  TEST_ASSERT_EQUAL((int)probe_calls[0].count, POPULATION_SIZE, "the probe sees the full count");
  TEST_ASSERT_EQUAL(probe_calls[0].snapshot_number, ctx.snapshot_number, "snapshot number passed");
  TEST_ASSERT(probe_calls[0].redshift == ctx.redshift && probe_calls[0].time == ctx.time,
              "redshift and time passed unchanged");
  TEST_ASSERT(probe_calls[0].params == (const void *)&MimicConfig, "config pointer passed");
  for (int i = 0; i < POPULATION_SIZE; i++) {
    TEST_ASSERT(probe_calls[0].seen[i] == SNAPSHOT_FIXTURE_VALUE,
                "the second entry sees the first entry's write");
    TEST_ASSERT(galaxies[i].TestDummyProperty == PROBE_VALUE, "the last entry's write survives");
  }
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");

  /* ===== EXECUTE / VALIDATE: probe first ===== */
  TEST_ASSERT_EQUAL(init_probe_phase(probe_then_fixture, 2), 0, "reordered phase initialises");
  build_population(halos, galaxies, POPULATION_SIZE);
  execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  for (int i = 0; i < POPULATION_SIZE; i++) {
    TEST_ASSERT(probe_calls[0].seen[i] == 0.0f, "the first entry sees the initial value");
    TEST_ASSERT(galaxies[i].TestDummyProperty == SNAPSHOT_FIXTURE_VALUE,
                "the later fixture's write survives");
  }
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");

  /* ===== CLEANUP ===== */
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_post_snapshot_zero_count_is_a_real_call
 * @brief   Count zero calls every entry, with a non-NULL buffer passed through
 *          and nothing written
 */
int test_post_snapshot_zero_count_is_a_real_call(void) {
  /* ===== SETUP ===== */
  static const char *const modules[] = {"test_snapshot_fixture", "snapshot_probe"};
  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  struct SnapshotContext ctx = make_snapshot_context();
  TEST_ASSERT_EQUAL(init_probe_phase(modules, 2), 0, "phase initialises");

  /* ===== EXECUTE ===== */
  execute_post_snapshot(&ctx, halos, 0); /* allocated, non-NULL, empty */
  execute_post_snapshot(&ctx, NULL, 0);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(probe_ncalls, 2, "both empty dispatches call the probe");
  TEST_ASSERT(probe_calls[0].halos == halos && probe_calls[0].count == 0,
              "a non-NULL empty buffer is passed through with count zero");
  TEST_ASSERT(probe_calls[1].halos == NULL && probe_calls[1].count == 0,
              "a NULL empty buffer is passed through with count zero");
  for (int i = 0; i < POPULATION_SIZE; i++) {
    TEST_ASSERT(galaxies[i].TestDummyProperty == 0.0f, "an empty call writes nothing");
  }

  /* ===== CLEANUP ===== */
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_post_snapshot_rejects_event_emission
 * @brief   module_emit_event() fails under snapshot dispatch and keeps its
 *          direct-unit-test shortcut outside it
 */
int test_post_snapshot_rejects_event_emission(void) {
  /* ===== SETUP ===== */
  static const char *const modules[] = {"snapshot_probe"};
  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  struct SnapshotContext ctx = make_snapshot_context();
  TEST_ASSERT_EQUAL(init_probe_phase(modules, 1), 0, "phase initialises");

  /* ===== EXECUTE ===== */
  execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  struct ModuleContext outside;
  memset(&outside, 0, sizeof(outside));
  int outside_result = module_emit_event(&outside, 1, 0, 0, 0.0, 0.0);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(probe_calls[0].emit_result, -1,
                    "emission with a non-NULL context is rejected during snapshot dispatch");
  TEST_ASSERT_EQUAL(outside_result, 0, "the no-active-phase shortcut is unchanged afterwards");
  TEST_ASSERT_EQUAL(module_emit_event(NULL, 1, 0, 0, 0.0, 0.0), -1,
                    "a NULL context is still rejected");

  /* ===== CLEANUP ===== */
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Child body: dispatch a probe that returns 7 at snapshot 5 */
static void failing_probe_body(const char *unused) {
  (void)unused;
  static const char *const modules[] = {"test_snapshot_fixture", "snapshot_probe"};
  if (init_probe_phase(modules, 2) != 0) {
    exit(2);
  }
  probe_return_code = 7;
  static struct Halo halos[POPULATION_SIZE];
  static struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  struct SnapshotContext ctx = make_snapshot_context();
  execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  fprintf(stderr, "execute_post_snapshot returned after a failing callback\n");
}

/**
 * @test    test_post_snapshot_callback_failure_is_fatal
 * @brief   A non-zero callback return aborts, naming module, snapshot and code
 */
int test_post_snapshot_callback_failure_is_fatal(void) {
  int ok = expect_fatal("failing_probe", failing_probe_body,
                        "Module 'snapshot_probe' failed in phase 'post_snapshot' at snapshot 5",
                        "with return code 7");
  TEST_ASSERT_EQUAL(ok, 1, "a failing snapshot callback is fatal with module, snapshot and code");
  return TEST_PASS;
}

/**
 * @brief   Child body: call execute_post_snapshot() in the named invalid way
 *
 * Every case must abort inside execute_post_snapshot(); reaching the final
 * message means a guard let the call through.
 */
static void dispatch_guard_body(const char *which) {
  static struct Halo halos[POPULATION_SIZE];
  static struct GalaxyData galaxies[POPULATION_SIZE];
  static const char *const probe_only[] = {"snapshot_probe"};
  struct SnapshotContext ctx = make_snapshot_context();
  prepare_phase_config(0.75);
  build_population(halos, galaxies, POPULATION_SIZE);

  if (strcmp(which, "null_ctx") == 0) {
    execute_post_snapshot(NULL, halos, POPULATION_SIZE);
  } else if (strcmp(which, "null_params") == 0) {
    ctx.params = NULL;
    execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  } else if (strcmp(which, "negative_count") == 0) {
    execute_post_snapshot(&ctx, halos, -1);
  } else if (strcmp(which, "null_halos") == 0) {
    execute_post_snapshot(&ctx, NULL, POPULATION_SIZE);
  } else if (strcmp(which, "nested_dispatch") == 0) {
    if (init_probe_phase(probe_only, 1) != 0) {
      exit(2);
    }
    probe_reenter = 1;
    execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  } else if (strcmp(which, "unresolved_entry") == 0) {
    /* Configured but never passed through module_system_init(): resolved stays NULL. */
    test_post_snapshot_add("test_snapshot_fixture", PROCESSING_MODE_SNAPSHOT);
    execute_post_snapshot(&ctx, halos, POPULATION_SIZE);
  }
  fprintf(stderr, "execute_post_snapshot returned for case %s\n", which);
}

/**
 * @test    test_post_snapshot_dispatch_guards_are_fatal
 * @brief   execute_post_snapshot() aborts on a missing context or parameters, an
 *          invalid population, a nested dispatch and an unresolved entry
 */
int test_post_snapshot_dispatch_guards_are_fatal(void) {
  struct {
    const char *which;
    const char *needle_a;
    const char *needle_b;
  } cases[] = {
      {"null_ctx", "execute_post_snapshot called without a snapshot context", NULL},
      {"null_params", "execute_post_snapshot called without a snapshot context", NULL},
      {"negative_count", "invalid population at snapshot 5", "count=-1"},
      {"null_halos", "invalid population at snapshot 5", "count=3"},
      {"nested_dispatch", "called at snapshot 5 while another phase dispatch is active", NULL},
      {"unresolved_entry",
       "Module 'test_snapshot_fixture' in phase 'post_snapshot' was not resolved",
       "module_system_init() must run before execute_post_snapshot()"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int ok =
        expect_fatal(cases[i].which, dispatch_guard_body, cases[i].needle_a, cases[i].needle_b);
    if (ok != 1) {
      fprintf(stderr, "  guard case: %s\n", cases[i].which);
    }
    TEST_ASSERT_EQUAL(ok, 1, "an invalid execute_post_snapshot() call is fatal with its reason");
  }
  return TEST_PASS;
}

/**
 * @test    test_post_snapshot_runs_in_yaml_order_not_pipeline_order
 * @brief   With test_fixture configured in a FoF phase and again second in
 *          post_snapshot, the snapshot phase runs its own YAML order
 *
 * The pipeline lists test_fixture first (its FoF phase is visited before
 * post_snapshot), so in the first case pipeline order would run test_fixture
 * before test_snapshot_fixture and leave SNAPSHOT_FIXTURE_VALUE behind; YAML
 * order runs test_fixture last and leaves its parameter value. The reversed
 * list is the control: same pipeline, opposite surviving value.
 */
int test_post_snapshot_runs_in_yaml_order_not_pipeline_order(void) {
  struct {
    const char *first;
    const char *second;
    float survivor; /* value the second (last) entry writes */
  } cases[] = {
      {"test_snapshot_fixture", "test_fixture", 0.75f},
      {"test_fixture", "test_snapshot_fixture", SNAPSHOT_FIXTURE_VALUE},
  };
  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  struct SnapshotContext ctx = make_snapshot_context();

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    /* ===== SETUP ===== */
    prepare_phase_config(0.75);
    test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);
    test_post_snapshot_add(cases[c].first, PROCESSING_MODE_SNAPSHOT);
    test_post_snapshot_add(cases[c].second, PROCESSING_MODE_SNAPSHOT);
    TEST_ASSERT_EQUAL(module_system_init(), 0, "FoF and snapshot phases initialise together");
    TEST_ASSERT_EQUAL(module_system_pipeline_count(), 2, "each module joins the pipeline once");

    /* ===== EXECUTE ===== */
    build_population(halos, galaxies, POPULATION_SIZE);
    execute_post_snapshot(&ctx, halos, POPULATION_SIZE);

    /* ===== VALIDATE ===== */
    for (int i = 0; i < POPULATION_SIZE; i++) {
      TEST_ASSERT(galaxies[i].TestDummyProperty == cases[c].survivor,
                  "the entry listed last in post_snapshot runs last");
    }

    /* ===== CLEANUP ===== */
    TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_post_snapshot_lifecycle
 * @brief   Snapshot-only and dual-mode modules join the pipeline once, appear in
 *          dependency queries and provenance order, and are cleaned up with the
 *          phase configuration; an empty phase leaves the visited phases unchanged
 */
int test_post_snapshot_lifecycle(void) {
  /* ===== SETUP ===== */
  prepare_phase_config(0.75);
  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);
  test_post_snapshot_add("test_snapshot_fixture", PROCESSING_MODE_SNAPSHOT);
  test_post_snapshot_add("test_fixture", PROCESSING_MODE_SNAPSHOT);

  /* ===== EXECUTE ===== */
  int init_result = module_system_init();
  char visited[PHASE_LIST_CAPACITY] = "";
  for_each_phase(record_phase_visitor, visited);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(init_result, 0, "FoF and snapshot phases initialise together");
  TEST_ASSERT_EQUAL(module_system_pipeline_count(), 2,
                    "the dual-mode module configured twice is initialised once");
  TEST_ASSERT(module_configured_anywhere("test_snapshot_fixture"),
              "a snapshot-only module is configured anywhere");
  TEST_ASSERT(module_configured_in_phase("test_fixture", MimicConfig.post_snapshot,
                                         MimicConfig.num_post_snapshot, PROCESSING_MODE_SNAPSHOT),
              "the dual-mode module is found in post_snapshot with process_snapshot");
  TEST_ASSERT(!module_configured_anywhere("snapshot_probe"), "an unconfigured module is not");
  TEST_ASSERT_STRING_EQUAL(visited,
                           "pre_timestep:0,galaxy_physics:1,post_timestep:0,post_snapshot:2",
                           "post_snapshot is visited last, after the FoF phases");

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  TEST_ASSERT(MimicConfig.post_snapshot == NULL && MimicConfig.num_post_snapshot == 0,
              "cleanup releases the post_snapshot configuration");

  /* An unconfigured phase is not visited at all. */
  reset_config();
  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);
  visited[0] = '\0';
  for_each_phase(record_phase_visitor, visited);
  TEST_ASSERT_STRING_EQUAL(visited, "pre_timestep:0,galaxy_physics:1,post_timestep:0",
                           "an empty post_snapshot leaves the visited phases unchanged");
  test_free_substep_phases();

  /* ===== CLEANUP ===== */
  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Child body: configure an unregistered module in post_snapshot */
static void unknown_module_body(const char *unused) {
  (void)unused;
  static const char *const modules[] = {"no_such_snapshot_module"};
  init_probe_phase(modules, 1);
}

/** One illegal configuration: the bad entry, where it goes, and what startup must say */
struct ValidationCase {
  const char *what;         /**< Description printed if the case is accepted */
  const char *phase;        /**< pre_timestep, post_timestep or post_snapshot */
  const char *module;       /**< Module of the bad entry */
  enum ProcessingMode mode; /**< Mode of the bad entry */
  const char *reason;       /**< Diagnostic text following "Module '<module>' " */
};

/** The case validation_case_body() runs; set by the parent before each fork */
static const struct ValidationCase *current_validation_case = NULL;

/**
 * @brief   Child body: configure the snapshot probe as a valid first
 *          post_snapshot entry plus one bad entry, and run startup validation
 *
 * Exits 1 only when module_system_init() rejected the configuration before
 * calling any init() (the probe's own init() counts calls) and cleanup then
 * released the configuration; any other outcome returns, so the child exits 0
 * and expect_fatal() prints the reason and the captured diagnostics.
 */
static void validation_case_body(const char *unused) {
  (void)unused;
  const struct ValidationCase *c = current_validation_case;
  prepare_phase_config(0.75);
  test_post_snapshot_add("snapshot_probe", PROCESSING_MODE_SNAPSHOT);
  if (strcmp(c->phase, POST_SNAPSHOT_PHASE_NAME) == 0) {
    test_post_snapshot_add(c->module, c->mode);
  } else if (strcmp(c->phase, "pre_timestep") == 0) {
    test_pre_timestep_add(c->module, c->mode);
  } else {
    MimicConfig.post_timestep = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.post_timestep[0].module_name = strdup(c->module);
    MimicConfig.post_timestep[0].processing_mode = c->mode;
    MimicConfig.post_timestep[0].resolved = NULL;
    MimicConfig.num_post_timestep = 1;
  }

  if (module_system_init() != -1) {
    fprintf(stderr, "case accepted: %s\n", c->what);
    return;
  }
  if (probe_init_calls != 0) {
    fprintf(stderr, "case %s: a module init() ran before the rejection\n", c->what);
    return;
  }
  module_system_cleanup();
  if (MimicConfig.post_snapshot != NULL) {
    fprintf(stderr, "case %s: configuration not released after rejection\n", c->what);
    return;
  }
  exit(1);
}

/**
 * @test    test_post_snapshot_validation
 * @brief   Startup rejects every illegal post_snapshot entry, and process_snapshot
 *          in each fixed FoF phase, before any init(), naming phase and module
 */
int test_post_snapshot_validation(void) {
  static const struct ValidationCase cases[] = {
      {"FoF mode in post_snapshot", POST_SNAPSHOT_PHASE_NAME, "test_fixture",
       PROCESSING_MODE_FULL_HALO, "is configured with processing mode 'process_full_halo'"},
      {"module without the snapshot mode", POST_SNAPSHOT_PHASE_NAME, "test_event_producer",
       PROCESSING_MODE_SNAPSHOT, "does not support processing mode 'process_snapshot'"},
      {"duplicate entry", POST_SNAPSHOT_PHASE_NAME, "snapshot_probe", PROCESSING_MODE_SNAPSHOT,
       "is listed more than once"},
      {"process_snapshot in pre_timestep", "pre_timestep", "test_fixture", PROCESSING_MODE_SNAPSHOT,
       "is configured with processing mode 'process_snapshot'"},
      {"process_snapshot in post_timestep", "post_timestep", "test_fixture",
       PROCESSING_MODE_SNAPSHOT, "is configured with processing mode 'process_snapshot'"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    /* ===== SETUP ===== */
    char phase_needle[128];
    char module_needle[192];
    snprintf(phase_needle, sizeof(phase_needle), "Configuration error in phase '%s'",
             cases[i].phase);
    snprintf(module_needle, sizeof(module_needle), "Module '%s' %s", cases[i].module,
             cases[i].reason);
    current_validation_case = &cases[i];

    /* ===== EXECUTE / VALIDATE ===== */
    int ok = expect_fatal(cases[i].what, validation_case_body, phase_needle, module_needle);
    if (ok != 1) {
      fprintf(stderr, "  rejected case: %s\n", cases[i].what);
    }
    TEST_ASSERT_EQUAL(ok, 1,
                      "an illegal entry is rejected before any init(), naming phase and module");
  }

  int ok = expect_fatal("unknown_module", unknown_module_body,
                        "Unknown module 'no_such_snapshot_module'", "phase 'post_snapshot'");
  TEST_ASSERT_EQUAL(ok, 1, "an unregistered post_snapshot module is fatal, naming the phase");

  /* ===== CLEANUP ===== */
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_post_snapshot_population_excludes_type3
 * @brief   A buffer built by the real marshaller carries every Type 0/1/2 and no
 *          Type 3, and callback writes reach the galaxies the workspace shares
 *
 * This is the buffer the horizontal driver hands to execute_post_snapshot()
 * (cur->processed); the marshaller drops Type 3 entries while copying, and the
 * copied halos keep their galaxy pointers, so what a callback writes is what
 * inheritance and output later read.
 */
int test_post_snapshot_population_excludes_type3(void) {
  /* ===== SETUP ===== */
  static const char *const modules[] = {"snapshot_probe"};
  TEST_ASSERT_EQUAL(init_probe_phase(modules, 1), 0, "phase initialises");
  struct Halo workspace[4];
  struct GalaxyData galaxies[4];
  memset(workspace, 0, sizeof(workspace));
  memset(galaxies, 0, sizeof(galaxies));
  const int types[4] = {0, 3, 1, 2};
  for (int i = 0; i < 4; i++) {
    workspace[i].Type = types[i];
    workspace[i].UniqueGalaxyID = 200 + i;
    workspace[i].galaxy = &galaxies[i];
  }
  struct OutputBuffer buffer = {mymalloc_cat(4 * sizeof(struct Halo), MEM_HALOS), 0, 4};
  struct OutputBufferSegment segment = {.source_id = 0,
                                        .snapshot_number = 5,
                                        .workspace_start = 0,
                                        .workspace_count = 4,
                                        .output_first = -1,
                                        .output_count = 0};
  const struct FoFWorkspace ws = {.halos = workspace, .count = 4, .capacity = 4};
  marshal_workspace_to_output_buffer(&ws, &buffer, &segment, 1);
  struct SnapshotContext ctx = make_snapshot_context();

  /* ===== EXECUTE ===== */
  execute_post_snapshot(&ctx, buffer.halos, buffer.count);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL((int)probe_calls[0].count, 3, "the population holds the three survivors");
  TEST_ASSERT(probe_calls[0].types[0] == 0 && probe_calls[0].types[1] == 1 &&
                  probe_calls[0].types[2] == 2,
              "Types 0, 1 and 2 arrive in marshalling order; Type 3 does not");
  TEST_ASSERT(galaxies[0].TestDummyProperty == PROBE_VALUE &&
                  galaxies[2].TestDummyProperty == PROBE_VALUE &&
                  galaxies[3].TestDummyProperty == PROBE_VALUE,
              "writes reach the shared galaxies of every survivor");
  TEST_ASSERT(galaxies[1].TestDummyProperty == 0.0f, "the Type 3 galaxy is never handed over");

  /* ===== CLEANUP ===== */
  myfree(buffer.halos);
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: snapshot module callback contract\n");
  printf("============================================================\n");
  printf("%s", NC);

  init_memory_system(0);

  TEST_RUN(test_mode_table_round_trip);
  TEST_RUN(test_mode_lookup_fails_closed);
  TEST_RUN(test_registration_accepts_each_family);
  TEST_RUN(test_registration_rejects_missing_callbacks);
  TEST_RUN(test_registration_rejects_invalid_modes);
  TEST_RUN(test_generated_fixtures_register);
  TEST_RUN(test_snapshot_fixture_callback_contract);
  TEST_RUN(test_dual_fixture_snapshot_callback);
  TEST_RUN(test_fof_phase_rejects_snapshot_family);
  TEST_RUN(test_post_snapshot_dispatch_order_and_visibility);
  TEST_RUN(test_post_snapshot_zero_count_is_a_real_call);
  TEST_RUN(test_post_snapshot_rejects_event_emission);
  TEST_RUN(test_post_snapshot_callback_failure_is_fatal);
  TEST_RUN(test_post_snapshot_dispatch_guards_are_fatal);
  TEST_RUN(test_post_snapshot_runs_in_yaml_order_not_pipeline_order);
  TEST_RUN(test_post_snapshot_lifecycle);
  TEST_RUN(test_post_snapshot_validation);
  TEST_RUN(test_post_snapshot_population_excludes_type3);

  TEST_SUMMARY();
  return TEST_RESULT();
}
