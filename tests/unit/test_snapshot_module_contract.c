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
 *
 * Registration aborts through FATAL_ERROR and the registry has no unregister, so
 * each hand-built registration case runs in a forked child (child_capture.h).
 * The unit runner always builds with MIMIC_TEST_BUILD, so the framework fixture
 * modules are linked and registered.
 */

#include "framework/test_framework.h"
#include "framework/child_capture.h"
#include "../../core/module_registry.h"
#include "framework/test_phase_config.h"
#include "../../core/module_interface.h"
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

/**
 * @brief   Construct the Module for a named registration case
 *
 * Accepted cases bind the fixtures' real callbacks; rejected cases differ from
 * an accepted one by exactly the defect named in the case.
 */
static struct Module make_case_module(const char *which) {
  static enum ProcessingMode unknown_mode[1];
  struct Module mod = {.name = which,
                       .init = test_snapshot_fixture_init,
                       .process = NULL,
                       .process_snapshot = NULL,
                       .cleanup = test_snapshot_fixture_cleanup,
                       .supported_processing_modes = NULL,
                       .num_supported_modes = 0};

  if (strcmp(which, "snapshot_only") == 0 || strcmp(which, "snapshot_missing_callback") == 0) {
    mod.supported_processing_modes = snapshot_only_modes;
    mod.num_supported_modes = 1;
    if (strcmp(which, "snapshot_only") == 0) {
      mod.process_snapshot = test_snapshot_fixture_process_snapshot;
    }
  } else if (strcmp(which, "fof_only") == 0 || strcmp(which, "fof_missing_callback") == 0) {
    mod.init = test_fixture_init;
    mod.cleanup = test_fixture_cleanup;
    mod.supported_processing_modes = fof_only_modes;
    mod.num_supported_modes = 2;
    if (strcmp(which, "fof_only") == 0) {
      mod.process = test_fixture_process;
    }
  } else if (strncmp(which, "dual", 4) == 0) {
    mod.init = test_fixture_init;
    mod.cleanup = test_fixture_cleanup;
    mod.supported_processing_modes = dual_modes;
    mod.num_supported_modes = 2;
    mod.process = (strcmp(which, "dual_missing_fof") == 0) ? NULL : test_fixture_process;
    mod.process_snapshot =
        (strcmp(which, "dual_missing_snapshot") == 0) ? NULL : test_fixture_process_snapshot;
  } else if (strcmp(which, "empty_modes") == 0) {
    mod.process_snapshot = test_snapshot_fixture_process_snapshot;
    mod.supported_processing_modes = snapshot_only_modes;
    mod.num_supported_modes = 0;
  } else if (strcmp(which, "null_modes") == 0) {
    mod.process_snapshot = test_snapshot_fixture_process_snapshot;
    mod.num_supported_modes = 1;
  } else if (strcmp(which, "unknown_mode_value") == 0) {
    unknown_mode[0] = (enum ProcessingMode)99;
    mod.process = test_fixture_process;
    mod.process_snapshot = test_snapshot_fixture_process_snapshot;
    mod.supported_processing_modes = unknown_mode;
    mod.num_supported_modes = 1;
  } else if (strcmp(which, "count_mode_value") == 0) {
    mod.process = test_fixture_process;
    mod.process_snapshot = test_snapshot_fixture_process_snapshot;
    mod.supported_processing_modes = count_mode;
    mod.num_supported_modes = 1;
  } else if (strcmp(which, "missing_init") == 0) {
    mod.init = NULL;
    mod.process_snapshot = test_snapshot_fixture_process_snapshot;
    mod.supported_processing_modes = snapshot_only_modes;
    mod.num_supported_modes = 1;
  }
  return mod;
}

/**
 * @brief   Child body: register the named case, then invoke its callbacks
 *
 * A rejected case aborts inside module_registry_add(). An accepted case calls
 * every non-NULL callback through the registered struct on a real population
 * and exits non-zero (message on stderr) if any result breaks the contract.
 */
static void register_case_body(const char *which) {
  static struct Module mod;
  mod = make_case_module(which);
  module_registry_add(&mod);

  set_test_fixture_params(0.75);
  if (mod.init() != 0) {
    fprintf(stderr, "case %s: init failed\n", which);
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
    build_population(halos, galaxies, POPULATION_SIZE);
    if (mod.process_snapshot(&ctx, halos, POPULATION_SIZE) != 0 ||
        mod.process_snapshot(&ctx, NULL, 0) != 0) {
      fprintf(stderr, "case %s: process_snapshot returned failure\n", which);
      exit(3);
    }
    for (int64_t i = 0; i < POPULATION_SIZE; i++) {
      if (!(galaxies[i].TestDummyProperty > 0.0f)) {
        fprintf(stderr, "case %s: entry %lld not written by process_snapshot\n", which,
                (long long)i);
        exit(4);
      }
    }
  }
  mod.cleanup();
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

/**
 * @test    test_snapshot_fixture_callback_contract
 * @brief   The snapshot-only fixture writes only through galaxy pointers and
 *          honours count zero and contract violations
 */
int test_snapshot_fixture_callback_contract(void) {
  /* ===== SETUP ===== */
  struct Halo halos[POPULATION_SIZE];
  struct GalaxyData galaxies[POPULATION_SIZE];
  build_population(halos, galaxies, POPULATION_SIZE);
  struct Halo before[POPULATION_SIZE];
  memcpy(before, halos, sizeof(halos));
  struct SnapshotContext ctx = make_snapshot_context();
  TEST_ASSERT_EQUAL(test_snapshot_fixture_init(), 0, "snapshot fixture init succeeds");

  /* ===== EXECUTE / VALIDATE ===== */
  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, NULL, 0), 0,
                    "count zero with a NULL population succeeds");
  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, halos, 0), 0,
                    "count zero with a non-NULL population succeeds");
  TEST_ASSERT(galaxies[0].TestDummyProperty == 0.0f, "count zero writes nothing");

  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, halos, POPULATION_SIZE), 0,
                    "positive count succeeds");
  for (int64_t i = 0; i < POPULATION_SIZE; i++) {
    TEST_ASSERT(galaxies[i].TestDummyProperty == SNAPSHOT_FIXTURE_VALUE,
                "every Type 0/1/2 entry is written through its galaxy pointer");
  }
  TEST_ASSERT(memcmp(before, halos, sizeof(halos)) == 0, "no halo field or pointer changes");

  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, NULL, POPULATION_SIZE), -1,
                    "NULL population with a positive count is rejected");
  halos[1].galaxy = NULL;
  TEST_ASSERT_EQUAL(test_snapshot_fixture_process_snapshot(&ctx, halos, POPULATION_SIZE), -1,
                    "NULL galaxy pointer with a positive count is rejected");

  /* ===== CLEANUP ===== */
  TEST_ASSERT_EQUAL(test_snapshot_fixture_cleanup(), 0, "snapshot fixture cleanup succeeds");
  return TEST_PASS;
}

/**
 * @test    test_dual_fixture_snapshot_callback
 * @brief   The dual-mode test_fixture initialises once through the module system
 *          and its snapshot callback writes the configured parameter
 */
int test_dual_fixture_snapshot_callback(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  set_test_fixture_params(0.75);
  test_pre_timestep_add("test_fixture", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;

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
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();
    set_test_fixture_params(0.5);
    test_phase_add("galaxy_physics", cases[i].module, cases[i].mode);
    MimicConfig.SubSteps = 1;

    /* ===== EXECUTE / VALIDATE ===== */
    TEST_ASSERT_EQUAL(module_system_init(), -1, "FoF phase rejects an unsupported family/mode");

    /* ===== CLEANUP ===== */
    module_system_cleanup();
  }
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

  TEST_SUMMARY();
  return TEST_RESULT();
}
