/**
 * @file    test_record_creation.c
 * @brief   Unit tests for the record-creation contract (module_create_record())
 *
 * Validates, by driving execute_module_pipeline() over hand-built FoF
 * workspaces with framework fixtures only (hand-built probe modules defined
 * here and the test_fixture module; never a production module):
 * - every refusal: outside a running process_full_halo callback (no callback,
 *   init(), by-galaxy, per-event and snapshot callbacks), a host outside the
 *   committed rows, a created row as host, a non-Type 0/1 host, a host without a
 *   galaxy, the per-host identity radix and a synthetic identity space that does
 *   not fit int64; a refused call stages no row and allocates no galaxy
 * - staged-row initialisation field by field for a Type 0 and a Type 1 host,
 *   staged-row pointers that stay valid across later creations, staging blocks
 *   that grow geometrically (logarithmic block count), and created galaxies
 *   independent of their host's and of each other (pointer and mutation)
 * - inheritance of created rows: marshalled and passed through
 *   inherit_descendant_halos() into a second workspace and pool, they keep
 *   their ID and Type 2 and their galaxies are deep copies (value carried,
 *   pointer and mutation independent of source and siblings)
 * - visibility: the next full-halo module of the phase, the by-galaxy pass,
 *   every substep phase and post_timestep see created rows, with
 *   ctx->central_galaxy re-pointed; the test_fixture by-galaxy execution log
 *   grows by exactly the rows created in pre_timestep
 * - the event rule: an event naming a row created in the same callback is
 *   rejected, a later callback may target it and the consumer receives it
 * - the marshal merge: created rows emitted after their host's slice, for hosts
 *   in the first, middle and last segment and out of workspace order, Type 3
 *   created rows dropped, and an unplaceable host fatal
 * - memory: no allocation without creation, grow-to-high-water scratch, release
 *   at teardown and no leak; created IDs repeat across FoF steps
 * - the encoder round trip of every created ID
 */

#include "framework/test_framework.h"
#include "framework/child_capture.h"
#include "core/fof_workspace.h"
#include "core/galaxy_pool.h"
#include "core/inheritance.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "core/output_buffer.h"
#include "framework/test_phase_config.h"
#include "include/galaxy_id.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "io/vertical/reader.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Shared core-test fixtures (config reset, registration) */
#include "framework/core_test_fixtures.h"

/** Producer ID and event of the hand-built creator module (unused by generated modules) */
#define CREATOR_MODULE_ID 901
#define CREATOR_EVENT_ID 1

/** The synthetic identity space every case runs in unless it says otherwise */
#define TEST_UNIT 3
#define TEST_ROWS_PER_UNIT 100
#define TEST_UNITS 8

/** Value the probes and test_fixture write to TestDummyProperty */
#define FIXTURE_DUMMY_VALUE 0.25

/* ==========================================================================
 * Probe modules
 * ========================================================================== */

/**
 * Per-call behaviour of a probe, set by each case. `call` counts the probe's
 * process() calls in the current case so a case can act differently per phase.
 */
typedef int (*probe_action_fn)(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call);

static probe_action_fn creator_action = NULL;
static probe_action_fn observer_action = NULL;
static int creator_calls = 0;
static int observer_calls = 0;

/** Whether rc_creator's init() tries to create a record, and what it got back */
static bool creator_init_creates = false;
static int creator_init_rc = 0;

/** rc_consumer: rows it received and what a creation attempt from it returned */
#define MAX_RECEIVED 16
static long long consumer_received_ids[MAX_RECEIVED];
static int consumer_received_targets[MAX_RECEIVED];
static int consumer_received = 0;
static bool consumer_creates = false;
static int consumer_create_rc = 0;

/** rc_snapshot: what a creation attempt from a snapshot callback returned */
static int snapshot_create_rc = 0;

static int creator_init(void) {
  if (creator_init_creates) {
    struct Halo *row = NULL;
    creator_init_rc = module_create_record(NULL, 0, &row);
  }
  return 0;
}

static int probe_cleanup(void) { return 0; }

static int creator_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  const int call = creator_calls++;
  return creator_action != NULL ? creator_action(ctx, halos, ngal, call) : 0;
}

static int observer_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  const int call = observer_calls++;
  return observer_action != NULL ? observer_action(ctx, halos, ngal, call) : 0;
}

static int consumer_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ngal;
  if (consumer_received < MAX_RECEIVED) {
    consumer_received_ids[consumer_received] = halos[0].UniqueGalaxyID;
    consumer_received_targets[consumer_received] = ctx->active_event->target_index;
    consumer_received++;
  }
  if (consumer_creates) {
    struct Halo *row = NULL;
    consumer_create_rc = module_create_record(ctx, 0, &row);
  }
  return 0;
}

static int snapshot_process(const struct SnapshotContext *ctx, const struct Halo *halos,
                            int64_t count) {
  (void)ctx;
  (void)halos;
  (void)count;
  struct Halo *row = NULL;
  snapshot_create_rc = module_create_record(NULL, 0, &row);
  return 0;
}

static const enum ProcessingMode fof_modes[] = {PROCESSING_MODE_FULL_HALO,
                                                PROCESSING_MODE_BY_GALAXY};
static const enum ProcessingMode per_event_modes[] = {PROCESSING_MODE_PER_EVENT};
static const enum ProcessingMode snapshot_modes[] = {PROCESSING_MODE_SNAPSHOT};
static const int creator_events[] = {CREATOR_EVENT_ID};
static const struct EventSubscription consumer_subscriptions[] = {
    {CREATOR_MODULE_ID, CREATOR_EVENT_ID, "rc_event", "rc_creator"}};

static struct Module creator_module = {.name = "rc_creator",
                                       .init = creator_init,
                                       .process = creator_process,
                                       .cleanup = probe_cleanup,
                                       .supported_processing_modes = fof_modes,
                                       .num_supported_modes = 2,
                                       .module_id = CREATOR_MODULE_ID,
                                       .emitted_event_ids = creator_events,
                                       .num_emitted_events = 1};

static struct Module observer_module = {.name = "rc_observer",
                                        .init = probe_cleanup,
                                        .process = observer_process,
                                        .cleanup = probe_cleanup,
                                        .supported_processing_modes = fof_modes,
                                        .num_supported_modes = 2};

static struct Module consumer_module = {.name = "rc_consumer",
                                        .init = probe_cleanup,
                                        .process = consumer_process,
                                        .cleanup = probe_cleanup,
                                        .supported_processing_modes = per_event_modes,
                                        .num_supported_modes = 1,
                                        .subscriptions = consumer_subscriptions,
                                        .num_subscriptions = 1};

static struct Module snapshot_module = {.name = "rc_snapshot",
                                        .init = probe_cleanup,
                                        .process_snapshot = snapshot_process,
                                        .cleanup = probe_cleanup,
                                        .supported_processing_modes = snapshot_modes,
                                        .num_supported_modes = 1};

static void ensure_probes_registered(void) {
  static bool registered = false;
  ensure_modules_registered();
  if (!registered) {
    module_registry_add(&creator_module);
    module_registry_add(&observer_module);
    module_registry_add(&consumer_module);
    module_registry_add(&snapshot_module);
    registered = true;
  }
}

/* ==========================================================================
 * Log capture
 * ========================================================================== */

static FILE *log_file = NULL;
static char log_text[1 << 20];

/** @brief Route every log line at INFO and above into a temporary file */
static void capture_log(void) {
  log_file = tmpfile();
  initialize_error_handling(LOG_LEVEL_INFO, log_file);
}

/** @brief Stop capturing and load what was logged into log_text */
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

static int count_occurrences(const char *text, const char *needle) {
  int count = 0;
  for (const char *p = strstr(text, needle); p != NULL; p = strstr(p + 1, needle)) {
    count++;
  }
  return count;
}

/* ==========================================================================
 * Workspace and pipeline fixtures
 * ========================================================================== */

static struct FoFWorkspace workspace;
static struct GalaxyPool *pool = NULL;
static struct ModuleContext context;

/**
 * @brief   Build a FoF workspace of the given Types, every row with a galaxy
 *
 * Row i has HaloNr 10 + i and UniqueGalaxyID 1000 + i, and distinct halo-side
 * values so field copies are checkable. Capacity equals the row count, so the
 * first commit must grow (and may move) the rows.
 */
static void build_workspace(const int *types, int n) {
  memset(&workspace, 0, sizeof(workspace));
  pool = galaxy_pool_create(64);
  workspace.halos = mymalloc_cat((size_t)n * sizeof(struct Halo), MEM_HALOS);
  memset(workspace.halos, 0, (size_t)n * sizeof(struct Halo));
  workspace.count = n;
  workspace.capacity = n;
  workspace.pool = pool;
  workspace.identity = (struct RecordIdentitySpace){
      .unit = TEST_UNIT, .rows_per_unit = TEST_ROWS_PER_UNIT, .fits = true, .units = TEST_UNITS};
  workspace.base_count = n;

  for (int i = 0; i < n; i++) {
    struct Halo *h = &workspace.halos[i];
    h->Type = types[i];
    h->HaloNr = 10 + i;
    h->UniqueGalaxyID = 1000 + i;
    h->UniqueCentralGalaxyID = 1000;
    h->CentralHalo = 0;
    h->SnapNum = 4;
    h->dT = 0.5 + i;
    h->Len = 100 + i;
    h->Mvir = 10.0 + i;
    h->deltaMvir = 1.0 + i;
    h->CentralMvir = 10.0;
    h->Rvir = 0.2 + 0.01 * i;
    h->Vvir = 150.0 + i;
    h->Vmax = 180.0 + i;
    h->infallMvir = 7.0 + i;
    h->infallVvir = 70.0 + i;
    h->infallVmax = 77.0 + i;
    for (int j = 0; j < 3; j++) {
      h->Pos[j] = 1.0f + (float)(i + j);
      h->Vel[j] = 2.0f + (float)(i * j);
    }
    h->galaxy = galaxy_pool_alloc(pool);
    init_galaxy_defaults(h->galaxy);
  }

  memset(&context, 0, sizeof(context));
  context.snapshot_number = 5;
  context.num_substeps = 1;
  context.central_index = 0;
  context.central_galaxy = &workspace.halos[0];
  context.params = &MimicConfig;
}

static void free_workspace(void) {
  fof_workspace_destroy(&workspace);
  if (pool != NULL) {
    galaxy_pool_destroy(pool);
    pool = NULL;
  }
}

/**
 * @brief   Release everything a pipeline case may hold
 *
 * Safe to repeat and after a failed assertion: restores logging if a capture
 * is open, cleans up the module system, frees the workspace and its pool, and
 * releases the record-creation scratch.
 *
 * @return  module_system_cleanup()'s return code
 */
static int release_case(void) {
  if (log_file != NULL) {
    (void)captured_log();
  }
  const int cleanup_rc = module_system_cleanup();
  free_workspace();
  module_release_record_creation_scratch();
  return cleanup_rc;
}

static int64_t pool_high_water(void) {
  struct GalaxyPoolStats stats;
  galaxy_pool_stats(pool, &stats);
  return stats.galaxies_high_water;
}

/** @brief Reset configuration and probe state; set the fixture's two required parameters */
static void prepare_config(int fixture_logging) {
  reset_config();
  ensure_probes_registered();
  MimicConfig.SubSteps = 1;
  MimicConfig.ProcessingOrder = INPUT_PROCESSING_ORDER_VERTICAL;

  strcpy(MimicConfig.ModelParams[0].param_name, "TestFixtureDummyParameter");
  snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "%.10g", FIXTURE_DUMMY_VALUE);
  strcpy(MimicConfig.ModelParams[1].param_name, "TestFixtureEnableLogging");
  snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "%d", fixture_logging);
  MimicConfig.NumModelParams = 2;

  creator_action = NULL;
  observer_action = NULL;
  creator_calls = 0;
  observer_calls = 0;
  creator_init_creates = false;
  creator_init_rc = 0;
  consumer_received = 0;
  consumer_creates = false;
  consumer_create_rc = 0;
  snapshot_create_rc = 0;
}

static void add_fixture_create_records(int n) {
  const int i = MimicConfig.NumModelParams++;
  strcpy(MimicConfig.ModelParams[i].param_name, "TestFixtureCreateRecords");
  snprintf(MimicConfig.ModelParams[i].value, MAX_STRING_LEN, "%d", n);
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

/** @brief Decode a created ID into (unit, row, ordinal); the inverse of the encoder */
static void decode_created_id(int64_t id, int64_t rows_per_unit, int64_t *unit, int64_t *row,
                              int *ordinal) {
  const int64_t k = -id - 1;
  const int64_t host_key = k / MAX_CREATED_RECORDS_PER_HOST;
  *ordinal = (int)(k % MAX_CREATED_RECORDS_PER_HOST);
  *row = host_key % rows_per_unit;
  *unit = host_key / rows_per_unit;
}

/* ==========================================================================
 * Refusals
 * ========================================================================== */

/** Creation attempts made from a by-galaxy call, with their return codes */
static int by_galaxy_rc = 0;

static int create_from_any_mode(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  (void)call;
  struct Halo *row = &workspace.halos[0];
  const int rc = module_create_record(ctx, 0, &row);
  if (ngal == 1) {
    by_galaxy_rc = rc;
  }
  if (rc >= 0 || row != NULL) {
    by_galaxy_rc = 99; /* a refused call must stage nothing and clear *row */
  }
  return 0;
}

static int emit_then_return(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  (void)ngal;
  (void)call;
  return module_emit_event(ctx, CREATOR_EVENT_ID, 0, 1, 0.0, 0.0);
}

/** @brief Body of test_refused_outside_full_halo; the wrapper releases its resources */
static int refused_outside_full_halo_body(void) {
  const int types[] = {0, 1, 2};

  /* No callback running at all. */
  capture_log();
  struct Halo sentinel;
  struct Halo *row = &sentinel;
  TEST_ASSERT_EQUAL(module_create_record(NULL, 0, &row), -1, "a direct call is refused");
  TEST_ASSERT(row == NULL, "a refused call clears *row");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "outside any module callback") != NULL, "names the missing callback");

  /* init(), by-galaxy, per-event, then snapshot callbacks. */
  prepare_config(0);
  creator_init_creates = true;
  creator_action = emit_then_return;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("rc_consumer", PROCESSING_MODE_PER_EVENT);
  add_post_timestep("rc_observer", PROCESSING_MODE_BY_GALAXY);
  test_post_snapshot_add("rc_snapshot", PROCESSING_MODE_SNAPSHOT);
  consumer_creates = true;
  observer_action = create_from_any_mode;
  by_galaxy_rc = 0;

  capture_log();
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 3);
  const int64_t high_water = pool_high_water();
  execute_module_pipeline(&context, &workspace);
  struct SnapshotContext snapshot_ctx = {.snapshot_number = 5, .params = &MimicConfig};
  execute_post_snapshot(&snapshot_ctx, workspace.halos, workspace.count);
  log = captured_log();

  TEST_ASSERT_EQUAL(creator_init_rc, -1, "init() cannot create");
  TEST_ASSERT(strstr(log, "module 'rc_creator': called from init()") != NULL,
              "the init refusal names the module and init()");
  TEST_ASSERT_EQUAL(consumer_create_rc, -1, "a per-event consumer cannot create");
  TEST_ASSERT(strstr(log, "module 'rc_consumer': called from a process_per_event callback") != NULL,
              "the per-event refusal names the module and its mode");
  TEST_ASSERT_EQUAL(by_galaxy_rc, -1, "a by-galaxy callback cannot create");
  TEST_ASSERT(strstr(log, "module 'rc_observer': called from a process_by_galaxy callback") != NULL,
              "the by-galaxy refusal names the module and its mode");
  TEST_ASSERT_EQUAL(snapshot_create_rc, -1, "a snapshot callback cannot create");
  TEST_ASSERT(strstr(log, "module 'rc_snapshot': called from a process_snapshot callback") != NULL,
              "the snapshot refusal names the module and its mode");
  TEST_ASSERT_EQUAL(workspace.count, 3, "no row was added");
  TEST_ASSERT_EQUAL(pool_high_water(), high_water, "no galaxy was allocated");
  return TEST_PASS;
}

/**
 * @test    test_refused_outside_full_halo
 * @brief   Creation is refused outside a running process_full_halo callback
 */
int test_refused_outside_full_halo(void) {
  const int result = refused_outside_full_halo_body();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return result;
}

/** Return codes of the refusal sequence, in order */
static int refusal_rc[8];
static struct Halo *refusal_rows[8];
static int64_t refusal_high_water[2];
static int radix_rc = 0;
static int created_host_rc = 0;

static int refuse_bad_hosts(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  if (call == 0) {
    const int hosts[] = {-1, ngal, 2, 3, 4};
    refusal_high_water[0] = pool_high_water();
    for (int i = 0; i < 5; i++) {
      refusal_rows[i] = &workspace.halos[0];
      refusal_rc[i] = module_create_record(ctx, hosts[i], &refusal_rows[i]);
    }
    refusal_high_water[1] = pool_high_water();

    /* Fill host 0 to the identity radix (MAX_CREATED_RECORDS_PER_HOST), then one more. */
    struct Halo *row = NULL;
    for (int n = 0; n < MAX_CREATED_RECORDS_PER_HOST; n++) {
      if (module_create_record(ctx, 0, &row) < 0) {
        return -1;
      }
    }
    radix_rc = module_create_record(ctx, 0, &row);
    return 0;
  }

  /* A later callback: a committed created row cannot host, even promoted. */
  struct Halo *row = NULL;
  halos[ngal - 1].Type = 1;
  created_host_rc = module_create_record(ctx, ngal - 1, &row);
  halos[ngal - 1].Type = 2;
  return 0;
}

/** @brief Body of test_refused_bad_hosts; the wrapper releases its resources */
static int refused_bad_hosts_body(void) {
  /* Row 4 is a Type 1 with no galaxy. */
  const int types[] = {0, 1, 2, 3, 1};

  prepare_config(0);
  creator_action = refuse_bad_hosts;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  add_post_timestep("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 5);
  workspace.halos[4].galaxy = NULL;

  capture_log();
  execute_module_pipeline(&context, &workspace);
  const char *log = captured_log();

  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL(refusal_rc[i], -1, "each bad host is refused");
    TEST_ASSERT(refusal_rows[i] == NULL, "each refusal clears *row");
  }
  TEST_ASSERT_EQUAL(refusal_high_water[1], refusal_high_water[0], "refusals allocate no galaxy");
  TEST_ASSERT(strstr(log, "host_index=-1 is outside the 5 committed rows") != NULL,
              "a negative host is outside the committed rows");
  TEST_ASSERT(strstr(log, "host_index=5 is outside the 5 committed rows") != NULL,
              "the committed count itself is outside");
  TEST_ASSERT(strstr(log, "host row 2 (UniqueGalaxyID 1002) is Type 2") != NULL,
              "a Type 2 host is refused");
  TEST_ASSERT(strstr(log, "host row 3 (UniqueGalaxyID 1003) is Type 3") != NULL,
              "a Type 3 host is refused");
  TEST_ASSERT(strstr(log, "host row 4 (UniqueGalaxyID 1004) has no galaxy") != NULL,
              "a host without a galaxy is refused");
  TEST_ASSERT_EQUAL(radix_rc, -1,
                    "a record past MAX_CREATED_RECORDS_PER_HOST on one host is refused");
  char radix_text[64];
  snprintf(radix_text, sizeof(radix_text), "already has %d created records",
           MAX_CREATED_RECORDS_PER_HOST);
  TEST_ASSERT(strstr(log, radix_text) != NULL, "names the radix");
  TEST_ASSERT_EQUAL(created_host_rc, -1, "a created row cannot host");
  TEST_ASSERT(strstr(log, "created rows cannot host") != NULL, "names the created-host rule");
  TEST_ASSERT(strstr(log, "module 'rc_creator'") != NULL, "refusals name the module");
  TEST_ASSERT_EQUAL(workspace.count, 5 + MAX_CREATED_RECORDS_PER_HOST,
                    "only the successful creations were committed");
  TEST_ASSERT_EQUAL(pool_high_water(), refusal_high_water[0] + MAX_CREATED_RECORDS_PER_HOST,
                    "one galaxy per successful creation");
  return TEST_PASS;
}

/**
 * @test    test_refused_bad_hosts
 * @brief   Bad hosts and the identity radix are refused without staging anything
 */
int test_refused_bad_hosts(void) {
  const int result = refused_bad_hosts_body();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return result;
}

static int fits_rc = 0;

static int create_once(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  (void)ngal;
  (void)call;
  struct Halo *row = NULL;
  fits_rc = module_create_record(ctx, 0, &row);
  return 0;
}

/**
 * @test    test_refused_when_identity_space_does_not_fit
 * @brief   A space that does not fit int64 refuses creation with both numbers, radix and driver
 */
int test_refused_when_identity_space_does_not_fit(void) {
  const int types[] = {0};
  prepare_config(0);
  MimicConfig.ProcessingOrder = INPUT_PROCESSING_ORDER_HORIZONTAL;
  creator_action = create_once;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 1);
  /* MAX_CREATED_RECORDS_PER_HOST (2^10) * 2^40 * 2^14 = 2^64 > INT64_MAX: does not fit. */
  workspace.identity = (struct RecordIdentitySpace){
      .unit = 0, .rows_per_unit = INT64_C(1) << 14, .fits = false, .units = INT64_C(1) << 40};
  TEST_ASSERT(
      !mimic_created_record_space_fits(workspace.identity.units, workspace.identity.rows_per_unit),
      "the synthetic space really does not fit");
  const int64_t high_water = pool_high_water();

  capture_log();
  execute_module_pipeline(&context, &workspace);
  const char *log = captured_log();

  TEST_ASSERT_EQUAL(fits_rc, -1, "creation is refused");
  char expected[256];
  snprintf(expected, sizeof(expected),
           "the horizontal driver's created-record identity space does not fit int64 "
           "(units=1099511627776, rows_per_unit=16384, radix=%d)",
           MAX_CREATED_RECORDS_PER_HOST);
  TEST_ASSERT(strstr(log, expected) != NULL,
              "the refusal carries units, rows_per_unit, the radix and the driver name");
  TEST_ASSERT_EQUAL(workspace.count, 1, "nothing was committed");
  TEST_ASSERT_EQUAL(pool_high_water(), high_water, "no galaxy was allocated");

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  free_workspace();
  module_release_record_creation_scratch();
  return TEST_PASS;
}

/* ==========================================================================
 * Initialisation
 * ========================================================================== */

/**
 * Records created after the first one in test_staged_row_initialisation. With
 * the two hosts' first records that stages 1902 rows, which span the first
 * three staging blocks (256 + 512 + 1024 = 1792 rows; module_registry.c's
 * RECORD_STAGING_BLOCK_ROWS doubling per block), so the first row's pointer
 * must survive two later block allocations. Each host gets 951 records, below
 * MAX_CREATED_RECORDS_PER_HOST.
 */
#define LATER_CREATIONS 1900

static struct Halo host_copies[2];
static int created_index[2];
static struct Halo *first_row = NULL;
static bool first_row_survived = false;

static int create_from_both_hosts(struct ModuleContext *ctx, struct Halo *halos, int ngal,
                                  int call) {
  (void)ngal;
  (void)call;
  host_copies[0] = halos[0];
  host_copies[1] = halos[1];

  struct Halo *row = NULL;
  created_index[0] = module_create_record(ctx, 0, &row);
  first_row = row;
  first_row->galaxy->TestDummyProperty = 0.5f;
  created_index[1] = module_create_record(ctx, 1, &row);

  /* Cross several staging blocks; the first row must not move. */
  for (int n = 0; n < LATER_CREATIONS; n++) {
    if (module_create_record(ctx, (n % 2 == 0) ? 0 : 1, &row) < 0) {
      return -1;
    }
  }
  first_row_survived =
      first_row->galaxy->TestDummyProperty == 0.5f && first_row->UniqueGalaxyID < 0;
  return 0;
}

/** @brief Assert the initialisation contract of one created row against its host */
static int check_created_row(const struct Halo *created, const struct Halo *host, int ordinal) {
  TEST_ASSERT_EQUAL(created->Type, 2, "a created row is Type 2");
  TEST_ASSERT(created->Mvir == 0.0, "Mvir is zero");
  TEST_ASSERT_EQUAL(created->Len, 0, "Len is zero");
  TEST_ASSERT(created->deltaMvir == -host->Mvir, "deltaMvir is minus the host Mvir");
  TEST_ASSERT_EQUAL(created->HaloNr, host->HaloNr, "HaloNr is the host's");
  TEST_ASSERT_EQUAL(created->CentralHalo, host->CentralHalo, "CentralHalo is the host's");
  TEST_ASSERT(created->CentralMvir == host->CentralMvir, "CentralMvir is the host's");
  TEST_ASSERT_EQUAL(created->UniqueCentralGalaxyID, host->UniqueCentralGalaxyID,
                    "UniqueCentralGalaxyID is the host's");
  TEST_ASSERT_EQUAL(created->SnapNum, host->SnapNum, "SnapNum is the host's");
  TEST_ASSERT(created->dT == host->dT, "dT is the host's");
  TEST_ASSERT(created->Rvir == host->Rvir, "Rvir is the host's");
  TEST_ASSERT(created->Vvir == host->Vvir, "Vvir is the host's");
  for (int j = 0; j < 3; j++) {
    TEST_ASSERT(created->Pos[j] == host->Pos[j], "Pos is the host's");
    TEST_ASSERT(created->Vel[j] == host->Vel[j], "Vel is the host's");
  }
  if (host->Type == 0) {
    TEST_ASSERT(created->infallMvir == host->Mvir, "Type 0 host: infallMvir is its Mvir");
    TEST_ASSERT(created->infallVvir == host->Vvir, "Type 0 host: infallVvir is its Vvir");
    TEST_ASSERT(created->infallVmax == host->Vmax, "Type 0 host: infallVmax is its Vmax");
  } else {
    TEST_ASSERT(created->infallMvir == host->infallMvir, "Type 1 host: infallMvir kept");
    TEST_ASSERT(created->infallVvir == host->infallVvir, "Type 1 host: infallVvir kept");
    TEST_ASSERT(created->infallVmax == host->infallVmax, "Type 1 host: infallVmax kept");
  }
  TEST_ASSERT_EQUAL(
      created->UniqueGalaxyID,
      mimic_encode_created_galaxy_id(TEST_UNIT, host->HaloNr, TEST_ROWS_PER_UNIT, ordinal),
      "UniqueGalaxyID is the encoder applied to unit, host HaloNr and ordinal");
  TEST_ASSERT(created->galaxy != NULL && created->galaxy != host->galaxy,
              "the galaxy is a fresh pool slot");
  return TEST_PASS;
}

/**
 * @test    test_staged_row_initialisation
 * @brief   Created rows from a Type 0 and a Type 1 host follow the initialisation contract
 */
int test_staged_row_initialisation(void) {
  const int types[] = {0, 1};
  prepare_config(0);
  creator_action = create_from_both_hosts;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 2);

  execute_module_pipeline(&context, &workspace);

  TEST_ASSERT_EQUAL(created_index[0], 2, "the first record's future index is the committed count");
  TEST_ASSERT_EQUAL(created_index[1], 3, "indices continue in creation order");
  TEST_ASSERT(first_row_survived, "a staged row pointer survives later creations");
  TEST_ASSERT_EQUAL(workspace.count, 2 + 2 + LATER_CREATIONS, "every record was committed");
  TEST_ASSERT_EQUAL(workspace.base_count, 2, "base_count is untouched by the commit");
  TEST_ASSERT(check_created_row(&workspace.halos[2], &host_copies[0], 0) == TEST_PASS,
              "Type 0 host's record initialised by contract");
  TEST_ASSERT(check_created_row(&workspace.halos[3], &host_copies[1], 0) == TEST_PASS,
              "Type 1 host's record initialised by contract");
  TEST_ASSERT(workspace.halos[2].galaxy->TestDummyProperty == 0.5f,
              "what the module wrote through the staged row was committed");
  TEST_ASSERT(workspace.halos[3].galaxy->TestDummyProperty == 0.0f,
              "the galaxy starts from init_galaxy_defaults()");
  TEST_ASSERT_EQUAL(workspace.created_host[0], 0, "created_host records host 0");
  TEST_ASSERT_EQUAL(workspace.created_host[1], 1, "created_host records host 1");

  /* Encoder round trip and per-host ordinals over every created row. */
  int next_ordinal[2] = {0, 0};
  for (int64_t r = workspace.base_count; r < workspace.count; r++) {
    const int64_t host = workspace.created_host[r - workspace.base_count];
    int64_t unit = 0;
    int64_t row = 0;
    int ordinal = 0;
    decode_created_id(workspace.halos[r].UniqueGalaxyID, TEST_ROWS_PER_UNIT, &unit, &row, &ordinal);
    TEST_ASSERT_EQUAL(unit, TEST_UNIT, "decoded unit is the published unit");
    TEST_ASSERT_EQUAL(row, workspace.halos[host].HaloNr, "decoded row is the host's HaloNr");
    TEST_ASSERT_EQUAL(ordinal, next_ordinal[host], "ordinals count per host in creation order");
    next_ordinal[host]++;
  }

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  free_workspace();
  module_release_record_creation_scratch();
  return TEST_PASS;
}

/** Rows in the first staging block (RECORD_STAGING_BLOCK_ROWS in module_registry.c) */
#define FIRST_STAGING_BLOCK_ROWS 256

/** Records one callback stages in test_staging_blocks_are_logarithmic, over five hosts */
#define MANY_CREATIONS 5000
#define MANY_HOSTS 5

static struct Halo *many_first_row = NULL;
static bool many_first_row_survived = false;

static int create_many(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  (void)ngal;
  (void)call;
  struct Halo *row = NULL;
  for (int n = 0; n < MANY_CREATIONS; n++) {
    if (module_create_record(ctx, n % MANY_HOSTS, &row) < 0) {
      return -1;
    }
    if (n == 0) {
      many_first_row = row;
      many_first_row->galaxy->TestDummyProperty = 0.75f;
    }
  }
  many_first_row_survived =
      many_first_row->galaxy->TestDummyProperty == 0.75f && many_first_row->UniqueGalaxyID < 0;
  return 0;
}

/** @brief Body of test_staging_blocks_are_logarithmic; the wrapper releases its resources */
static int staging_blocks_body(void) {
  const int types[MANY_HOSTS] = {0, 1, 1, 1, 1};
  prepare_config(0);
  creator_action = create_many;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, MANY_HOSTS);

  execute_module_pipeline(&context, &workspace);

  /* Blocks double, so b blocks hold FIRST_STAGING_BLOCK_ROWS * (2^b - 1) rows. */
  int64_t expected_blocks = 0;
  while (FIRST_STAGING_BLOCK_ROWS * ((INT64_C(1) << expected_blocks) - 1) < MANY_CREATIONS) {
    expected_blocks++;
  }
  TEST_ASSERT_EQUAL(workspace.count, MANY_HOSTS + MANY_CREATIONS, "every record was committed");
  TEST_ASSERT_EQUAL(module_record_creation_staging_blocks(), expected_blocks,
                    "staging thousands of rows allocates a logarithmic handful of blocks");
  TEST_ASSERT(many_first_row_survived, "the first staged row survives every later creation");
  TEST_ASSERT(workspace.halos[MANY_HOSTS].galaxy->TestDummyProperty == 0.75f,
              "what was written through the first staged row was committed");
  return TEST_PASS;
}

/**
 * @test    test_staging_blocks_are_logarithmic
 * @brief   One callback staging thousands of rows holds only a few never-moving blocks
 */
int test_staging_blocks_are_logarithmic(void) {
  const int result = staging_blocks_body();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  TEST_ASSERT_EQUAL(module_record_creation_staging_blocks(), 0, "release frees every block");
  return result;
}

/** Records created on each of the two hosts in test_created_galaxies_are_independent */
#define PER_HOST_INDEPENDENT 3

static int create_on_both_hosts(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  (void)ngal;
  (void)call;
  struct Halo *row = NULL;
  for (int n = 0; n < 2 * PER_HOST_INDEPENDENT; n++) {
    if (module_create_record(ctx, n % 2, &row) < 0) {
      return -1;
    }
  }
  return 0;
}

/**
 * @test    test_created_galaxies_are_independent
 * @brief   Every created galaxy is its own pool slot: no pointer or write is shared
 *
 * The in-memory half of the contract's "galaxy deep-copied" requirement (the
 * HDF5 half is in tests/integration/test_record_creation.py): each created row's
 * galaxy differs from its host's and from every other created row's, and a
 * write to one changes no other.
 */
int test_created_galaxies_are_independent(void) {
  const int types[] = {0, 1};
  prepare_config(0);
  creator_action = create_on_both_hosts;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 2);

  execute_module_pipeline(&context, &workspace);

  const int64_t rows = workspace.count;
  TEST_ASSERT_EQUAL(rows, 2 + 2 * PER_HOST_INDEPENDENT, "every record was committed");
  for (int64_t a = 0; a < rows; a++) {
    for (int64_t b = a + 1; b < rows; b++) {
      TEST_ASSERT(workspace.halos[a].galaxy != workspace.halos[b].galaxy,
                  "no two rows (hosts or created) share a galaxy");
    }
  }
  for (int64_t a = 2; a < rows; a++) {
    workspace.halos[a].galaxy->TestDummyProperty = 0.125f * (float)(a - 1);
  }
  for (int64_t a = 0; a < rows; a++) {
    const float expected = (a < 2) ? 0.0f : 0.125f * (float)(a - 1);
    TEST_ASSERT(workspace.halos[a].galaxy->TestDummyProperty == expected,
                "a write to one created galaxy changes no other galaxy");
  }

  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return TEST_PASS;
}

/** Second-generation state of test_created_rows_are_inherited_by_deep_copy, released by its
 * wrapper on every path */
static struct OutputBuffer inherited_buffer = {NULL, 0, 0};
static struct Halo *descendant_rows = NULL;
static struct GalaxyPool *descendant_pool = NULL;

/** @brief Release the second generation built by inherited_by_deep_copy_body() */
static void release_descendant(void) {
  myfree(inherited_buffer.halos);
  inherited_buffer = (struct OutputBuffer){NULL, 0, 0};
  myfree(descendant_rows);
  descendant_rows = NULL;
  if (descendant_pool != NULL) {
    galaxy_pool_destroy(descendant_pool);
    descendant_pool = NULL;
  }
}

/** @brief Distinct, non-default TestDummyProperty for created row @p r */
static float written_value(int64_t r) { return 0.2f + 0.1f * (float)r; }

/** @brief Body of test_created_rows_are_inherited_by_deep_copy; the wrapper releases */
static int inherited_by_deep_copy_body(void) {
  /* First generation: a Type 0 host (segment 0) and a Type 1 host (segment 1),
   * PER_HOST_INDEPENDENT records each, created through the pipeline. */
  const int types[] = {0, 1};
  prepare_config(0);
  creator_action = create_on_both_hosts;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 2);
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT_EQUAL(workspace.count, 2 + 2 * PER_HOST_INDEPENDENT, "every record was committed");

  /* Non-default values, so inheriting defaults instead of copying would fail. */
  for (int64_t r = workspace.base_count; r < workspace.count; r++) {
    workspace.halos[r].galaxy->TestDummyProperty = written_value(r);
  }

  /* Marshal both slices; each host's records follow its own row. */
  struct OutputBufferSegment segments[2];
  for (int s = 0; s < 2; s++) {
    segments[s] = (struct OutputBufferSegment){.source_id = s,
                                               .snapshot_number = 5,
                                               .workspace_start = s,
                                               .workspace_count = 1,
                                               .output_first = -1,
                                               .output_count = 0};
  }
  inherited_buffer.halos = mymalloc_cat(4 * sizeof(struct Halo), MEM_HALOS);
  inherited_buffer.capacity = 4;
  marshal_workspace_to_output_buffer(&workspace, &inherited_buffer, segments, 2);
  const int64_t nrows = inherited_buffer.count;
  TEST_ASSERT_EQUAL(nrows, workspace.count, "every row was marshalled");
  TEST_ASSERT_EQUAL(segments[0].output_count, 1 + PER_HOST_INDEPENDENT,
                    "the Type 0 host's segment carries its records");

  /* Next snapshot: one FoF-central descendant whose progenitors are both
   * marshalled segments, the Type 0 host's on the main branch (as the drivers
   * gather them), inherited into a second workspace from a second pool. */
  struct InheritanceProgenitorGalaxy progenitors[2 + 2 * PER_HOST_INDEPENDENT];
  for (int64_t i = 0; i < nrows; i++) {
    progenitors[i].source = &inherited_buffer.halos[i];
    progenitors[i].source_time = 14.0;
    progenitors[i].is_main_branch = (i < segments[0].output_count);
  }
  struct InheritanceDescendant descendant;
  memset(&descendant, 0, sizeof(descendant));
  descendant.halo_nr = 7;
  descendant.current_snap = 6;
  descendant.current_time = 10.0;
  descendant.new_halo_dt = 2.5;
  descendant.virial_mass = 150.0;
  descendant.virial_radius = 1.5;
  descendant.virial_velocity = 250.0;
  descendant.is_fof_central = 1;
  descendant.unique_galaxy_id = 111000222LL;

  descendant_pool = galaxy_pool_create(16);
  descendant_rows = mymalloc_cat((size_t)nrows * sizeof(struct Halo), MEM_HALOS);
  memset(descendant_rows, 0, (size_t)nrows * sizeof(struct Halo));
  const int64_t end = inherit_descendant_halos(descendant_pool, descendant_rows, 0, nrows,
                                               &descendant, progenitors, nrows);
  TEST_ASSERT_EQUAL(end, nrows, "every progenitor row was inherited");

  int inherited_records = 0;
  for (int64_t i = 0; i < end; i++) {
    const struct Halo *source = progenitors[i].source;
    const struct Halo *inherited = &descendant_rows[i];
    TEST_ASSERT_EQUAL(inherited->UniqueGalaxyID, source->UniqueGalaxyID,
                      "an inherited row keeps its UniqueGalaxyID");
    TEST_ASSERT(inherited->galaxy != NULL && inherited->galaxy != source->galaxy,
                "an inherited galaxy is a new slot, not the source's");
    for (int64_t j = 0; j < end; j++) {
      TEST_ASSERT(j == i || inherited->galaxy != descendant_rows[j].galaxy,
                  "no two inherited rows share a galaxy");
      TEST_ASSERT(inherited->galaxy != inherited_buffer.halos[j].galaxy,
                  "no inherited galaxy is any source row's galaxy");
    }
    if (source->UniqueGalaxyID < 0) {
      TEST_ASSERT(inherited->UniqueGalaxyID < 0, "a created ID stays negative");
      TEST_ASSERT_EQUAL(inherited->Type, 2, "an inherited created row is Type 2");
      TEST_ASSERT(inherited->galaxy->TestDummyProperty == source->galaxy->TestDummyProperty &&
                      source->galaxy->TestDummyProperty != 0.0f,
                  "the inherited galaxy carries the value written, not defaults");
      inherited_records++;
    }
  }
  TEST_ASSERT_EQUAL(inherited_records, 2 * PER_HOST_INDEPENDENT, "every created row was inherited");

  /* Mutating one inherited created galaxy changes no source and no sibling. The
   * last inherited row is the Type 1 host's last record (marshal order). */
  const int64_t mutated = end - 1;
  TEST_ASSERT(descendant_rows[mutated].UniqueGalaxyID < 0, "the last inherited row is created");
  const float before = descendant_rows[mutated].galaxy->TestDummyProperty;
  descendant_rows[mutated].galaxy->TestDummyProperty = 0.95f;
  for (int64_t i = 0; i < end; i++) {
    TEST_ASSERT(inherited_buffer.halos[i].galaxy->TestDummyProperty != 0.95f,
                "a write to an inherited galaxy does not reach any source galaxy");
    if (i != mutated) {
      TEST_ASSERT(descendant_rows[i].galaxy->TestDummyProperty != 0.95f,
                  "a write to an inherited galaxy does not reach a sibling");
    }
  }
  TEST_ASSERT(progenitors[mutated].source->galaxy->TestDummyProperty == before,
              "the mutated row's source keeps the value written");
  return TEST_PASS;
}

/**
 * @test    test_created_rows_are_inherited_by_deep_copy
 * @brief   Created rows survive marshal and inheritance with deep-copied galaxies
 *
 * The in-memory proof of the contract's "galaxy deep-copied" at inheritance,
 * which the HDF5 output cannot show (tests/integration/test_record_creation.py
 * keeps the output-side inheritance assertions).
 */
int test_created_rows_are_inherited_by_deep_copy(void) {
  const int result = inherited_by_deep_copy_body();
  release_descendant();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return result;
}

/* ==========================================================================
 * Visibility
 * ========================================================================== */

#define CREATED_PER_TYPE0 2
static int creator_creates_per_type0 = 0;
static int observed_ngal[8];
static int observed_full_calls = 0;
static int observed_by_galaxy_rows = 0;
static int observed_by_galaxy_created = 0;
static bool central_pointer_current = true;

static int create_per_type0(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)call;
  struct Halo *row = NULL;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type != 0) {
      continue;
    }
    for (int n = 0; n < creator_creates_per_type0; n++) {
      if (module_create_record(ctx, i, &row) < 0) {
        return -1;
      }
    }
  }
  return 0;
}

static int observe(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)call;
  if (ngal == 1) {
    /* by-galaxy visit (every full-halo call here sees at least three rows) */
    observed_by_galaxy_rows++;
    if (halos[0].UniqueGalaxyID < 0) {
      observed_by_galaxy_created++;
    }
    return 0;
  }
  if (observed_full_calls < 8) {
    observed_ngal[observed_full_calls] = ngal;
  }
  observed_full_calls++;
  if (ctx->central_galaxy != &halos[ctx->central_index] || halos != workspace.halos) {
    central_pointer_current = false;
  }
  return 0;
}

/**
 * @brief   Run the visibility pipeline once, creating `per_type0` records per Type 0 host
 *
 * pre_timestep: rc_creator (full halo), rc_observer (full halo), rc_observer (by galaxy);
 * substep phase "evolve" x 2 substeps: rc_observer (full halo);
 * post_timestep: test_fixture (by galaxy, logging on, no creation parameter).
 * The caller releases the case (release_case()) after reading the probes.
 *
 * @return  The number of TEST_FIXTURE_EXEC lines the fixture logged, or -1
 */
static int run_visibility_pipeline(int per_type0) {
  const int types[] = {0, 1, 2};
  prepare_config(1);
  MimicConfig.SubSteps = 2;
  creator_creates_per_type0 = per_type0;
  creator_action = create_per_type0;
  observer_action = observe;
  observed_full_calls = 0;
  observed_by_galaxy_rows = 0;
  observed_by_galaxy_created = 0;
  central_pointer_current = true;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("rc_observer", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("rc_observer", PROCESSING_MODE_BY_GALAXY);
  test_phase_add("evolve", "rc_observer", PROCESSING_MODE_FULL_HALO);
  add_post_timestep("test_fixture", PROCESSING_MODE_BY_GALAXY);

  if (module_system_init() != 0) {
    return -1;
  }
  build_workspace(types, 3);
  context.num_substeps = 2;

  capture_log();
  execute_module_pipeline(&context, &workspace);
  const char *log = captured_log();
  return count_occurrences(log, "TEST_FIXTURE_EXEC:");
}

/** @brief Body of test_visibility_across_modules_phases_and_substeps; the wrapper cleans up */
static int visibility_body(void) {
  const int baseline_executions = run_visibility_pipeline(0);
  TEST_ASSERT_EQUAL(baseline_executions, 3, "without creation the fixture visits three rows");
  (void)release_case();

  const int executions = run_visibility_pipeline(CREATED_PER_TYPE0);
  TEST_ASSERT_EQUAL(executions - baseline_executions, CREATED_PER_TYPE0,
                    "the by-galaxy fixture's execution log rises by exactly the rows created in "
                    "pre_timestep");
  TEST_ASSERT_EQUAL(observed_full_calls, 3, "one pre_timestep call and one per substep");
  for (int i = 0; i < observed_full_calls; i++) {
    TEST_ASSERT_EQUAL(observed_ngal[i], 3 + CREATED_PER_TYPE0,
                      "every later full-halo call receives the created rows");
  }
  TEST_ASSERT_EQUAL(observed_by_galaxy_rows, 3 + CREATED_PER_TYPE0,
                    "the same phase's by-galaxy pass visits every row");
  TEST_ASSERT_EQUAL(observed_by_galaxy_created, CREATED_PER_TYPE0,
                    "the by-galaxy pass visits the created rows");
  TEST_ASSERT(central_pointer_current,
              "ctx->central_galaxy addresses the current central row after the commit");
  TEST_ASSERT_EQUAL(workspace.count, 3 + CREATED_PER_TYPE0, "the workspace holds the created rows");
  return TEST_PASS;
}

/**
 * @test    test_visibility_across_modules_phases_and_substeps
 * @brief   Committed records reach the next module, the by-galaxy pass, substeps and later phases
 */
int test_visibility_across_modules_phases_and_substeps(void) {
  const int result = visibility_body();
  TEST_ASSERT_EQUAL(release_case(), 0, "cleanup succeeds");
  return result;
}

/**
 * @test    test_fixture_creates_records
 * @brief   test_fixture with TestFixtureCreateRecords creates per Type 0 host and logs it
 */
int test_fixture_creates_records(void) {
  const int types[] = {0, 1, 2};
  prepare_config(0);
  add_fixture_create_records(3);
  test_pre_timestep_add("test_fixture", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 3);

  capture_log();
  execute_module_pipeline(&context, &workspace);
  const char *log = captured_log();

  TEST_ASSERT_EQUAL(workspace.count, 6, "three records on the one Type 0 host");
  for (int64_t r = 3; r < 6; r++) {
    TEST_ASSERT(workspace.halos[r].UniqueGalaxyID < 0, "created rows carry negative IDs");
    TEST_ASSERT(workspace.halos[r].galaxy->TestDummyProperty == (float)FIXTURE_DUMMY_VALUE,
                "the fixture writes TestDummyProperty on each created row");
  }
  TEST_ASSERT_EQUAL(count_occurrences(log, "TEST_FIXTURE_CREATE: host=1000 created=3"), 1,
                    "one TEST_FIXTURE_CREATE line per host");
  TEST_ASSERT_EQUAL(count_occurrences(log, "TEST_FIXTURE_CREATE:"), 1, "only Type 0 rows host");

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  free_workspace();
  module_release_record_creation_scratch();
  return TEST_PASS;
}

/* A by-galaxy test_fixture with the creation parameter fails at its first creation. */
static void run_fixture_by_galaxy_with_creation(const char *arg) {
  (void)arg;
  const int types[] = {0, 1};
  prepare_config(0);
  add_fixture_create_records(1);
  test_pre_timestep_add("test_fixture", PROCESSING_MODE_BY_GALAXY);
  if (module_system_init() != 0) {
    return;
  }
  build_workspace(types, 2);
  execute_module_pipeline(&context, &workspace);
}

/**
 * @test    test_fixture_by_galaxy_creation_fails
 * @brief   A by-galaxy configuration of the creating fixture fails by the API's gate
 */
int test_fixture_by_galaxy_creation_fails(void) {
  const int rc = expect_fatal(NULL, run_fixture_by_galaxy_with_creation,
                              "module 'test_fixture': called from a process_by_galaxy callback",
                              "Module 'test_fixture' failed on galaxy");
  TEST_ASSERT_EQUAL(rc, 1, "the run aborts at the first creation call");
  return TEST_PASS;
}

/* ==========================================================================
 * Events
 * ========================================================================== */

static int same_callback_target_rc = 0;
static int same_callback_source_rc = 0;
static int committed_target_rc = 0;
static int same_phase_target_rc = 0;
static int later_target_rc = 0;
static int event_created_index = -1;

static int create_and_emit(struct ModuleContext *ctx, struct Halo *halos, int ngal, int call) {
  (void)halos;
  (void)ngal;
  if (call == 0) {
    struct Halo *row = NULL;
    event_created_index = module_create_record(ctx, 0, &row);
    same_callback_target_rc =
        module_emit_event(ctx, CREATOR_EVENT_ID, 0, event_created_index, 0.0, 0.0);
    same_callback_source_rc =
        module_emit_event(ctx, CREATOR_EVENT_ID, event_created_index, 0, 0.0, 0.0);
    committed_target_rc = module_emit_event(ctx, CREATOR_EVENT_ID, 0, 1, 0.0, 0.0);
    return 0;
  }
  const int rc = module_emit_event(ctx, CREATOR_EVENT_ID, 0, event_created_index, 0.0, 0.0);
  if (call == 1) {
    same_phase_target_rc = rc; /* the next full-halo entry of the same phase */
  } else {
    later_target_rc = rc; /* post_timestep */
  }
  return 0;
}

/**
 * @test    test_event_rule
 * @brief   An event cannot name a row created in the same callback; a later callback can
 */
int test_event_rule(void) {
  const int types[] = {0, 1};
  prepare_config(0);
  creator_action = create_and_emit;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("rc_consumer", PROCESSING_MODE_PER_EVENT);
  add_post_timestep("rc_creator", PROCESSING_MODE_FULL_HALO);
  add_post_timestep("rc_consumer", PROCESSING_MODE_PER_EVENT);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 2);

  capture_log();
  execute_module_pipeline(&context, &workspace);
  const char *log = captured_log();

  TEST_ASSERT_EQUAL(creator_calls, 3, "two pre_timestep entries and one post_timestep entry");
  TEST_ASSERT_EQUAL(event_created_index, 2, "the record was created");
  TEST_ASSERT_EQUAL(same_callback_target_rc, -1, "a same-callback created target is rejected");
  TEST_ASSERT_EQUAL(same_callback_source_rc, -1, "a same-callback created source is rejected");
  TEST_ASSERT(strstr(log, "invalid target_index=2: outside the 2 committed rows") != NULL,
              "the target rejection names the committed count");
  TEST_ASSERT(strstr(log, "invalid source_index=2: outside the 2 committed rows") != NULL,
              "the source rejection names the committed count");
  TEST_ASSERT_EQUAL(committed_target_rc, 0, "a committed target is accepted");
  TEST_ASSERT_EQUAL(same_phase_target_rc, 0,
                    "the next callback of the same phase may target the created row");
  TEST_ASSERT_EQUAL(later_target_rc, 0, "a later phase's callback may target the created row");
  TEST_ASSERT_EQUAL(consumer_received, 3, "the consumer received the three accepted events");
  for (int e = 1; e < 3; e++) {
    TEST_ASSERT_EQUAL(consumer_received_targets[e], 2, "later events target the created row");
    TEST_ASSERT_EQUAL(consumer_received_ids[e], workspace.halos[2].UniqueGalaxyID,
                      "the consumer was handed the created row itself");
  }
  TEST_ASSERT(consumer_received_ids[1] < 0, "which carries a created ID");

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  free_workspace();
  module_release_record_creation_scratch();
  return TEST_PASS;
}

/* ==========================================================================
 * Marshal merge
 * ========================================================================== */

static struct Halo marshal_rows[11];
static int64_t marshal_hosts[5] = {4, 0, 2, 0, 4};
static struct GalaxyData marshal_galaxies[11];

/**
 * @brief   Three segments of two rows with created rows in the tail
 *
 * Segments [0,2) [2,4) [4,6) at snapshots 7, 8 and 9; created rows 6..10 have
 * hosts {4, 0, 2, 0, 4}: the first created row's host is in the last segment,
 * and row 10 is retired to Type 3.
 */
static void build_marshal_workspace(struct FoFWorkspace *ws, struct OutputBufferSegment *segments) {
  memset(marshal_rows, 0, sizeof(marshal_rows));
  memset(marshal_galaxies, 0, sizeof(marshal_galaxies));
  const int types[11] = {0, 2, 1, 2, 1, 2, 2, 2, 2, 2, 3};
  for (int i = 0; i < 11; i++) {
    marshal_rows[i].Type = types[i];
    marshal_rows[i].UniqueGalaxyID = (i < 6) ? 100 + i : -(i - 5);
    marshal_rows[i].galaxy = &marshal_galaxies[i];
  }
  memset(ws, 0, sizeof(*ws));
  ws->halos = marshal_rows;
  ws->count = 11;
  ws->capacity = 11;
  ws->base_count = 6;
  ws->created_host = marshal_hosts;
  ws->created_capacity = 5;
  for (int s = 0; s < 3; s++) {
    segments[s] = (struct OutputBufferSegment){.source_id = 50 + s,
                                               .snapshot_number = 7 + s,
                                               .workspace_start = 2 * s,
                                               .workspace_count = 2,
                                               .output_first = -1,
                                               .output_count = 0};
  }
}

/**
 * @test    test_marshal_merge
 * @brief   Created rows follow their host's slice in creation order; Type 3 ones are dropped
 */
int test_marshal_merge(void) {
  struct FoFWorkspace ws;
  struct OutputBufferSegment segments[3];
  build_marshal_workspace(&ws, segments);
  struct OutputBuffer buffer = {mymalloc_cat(4 * sizeof(struct Halo), MEM_HALOS), 0, 4};

  marshal_workspace_to_output_buffer(&ws, &buffer, segments, 3);

  /* seg0: 100 101 then created rows 7 (-2) and 9 (-4); seg1: 102 103 then 8 (-3);
   * seg2: 104 105 then 6 (-1); row 10 (-5) is Type 3. */
  const long long expected[] = {100, 101, -2, -4, 102, 103, -3, 104, 105, -1};
  const int expected_snap[] = {7, 7, 7, 7, 8, 8, 8, 9, 9, 9};
  TEST_ASSERT_EQUAL(buffer.count, 10, "every surviving row was emitted once");
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL(buffer.halos[i].UniqueGalaxyID, expected[i],
                      "rows are emitted slice first, then that slice's created rows");
    TEST_ASSERT_EQUAL(buffer.halos[i].SnapNum, expected_snap[i],
                      "a created row takes its host segment's snapshot number");
  }
  const int64_t expected_first[3] = {0, 4, 7};
  const int64_t expected_count[3] = {4, 3, 3};
  for (int s = 0; s < 3; s++) {
    TEST_ASSERT_EQUAL(segments[s].output_first, expected_first[s],
                      "each segment's range starts after the previous one's created rows");
    TEST_ASSERT_EQUAL(segments[s].output_count, expected_count[s],
                      "each segment's range counts its surviving created rows");
  }
  TEST_ASSERT(marshal_rows[10].galaxy == NULL, "a Type 3 created row's galaxy is released");

  /* A descriptor without a created-host map has no created rows. */
  struct FoFWorkspace plain = {.halos = marshal_rows, .count = 6, .capacity = 6};
  struct OutputBufferSegment one = {
      .source_id = 1, .snapshot_number = 3, .workspace_start = 0, .workspace_count = 6};
  buffer.count = 0;
  marshal_workspace_to_output_buffer(&plain, &buffer, &one, 1);
  TEST_ASSERT_EQUAL(buffer.count, 6, "base_count is ignored without a created-host map");

  myfree(buffer.halos);
  return TEST_PASS;
}

static void marshal_with_unplaced_host(const char *arg) {
  (void)arg;
  struct FoFWorkspace ws;
  struct OutputBufferSegment segments[3];
  build_marshal_workspace(&ws, segments);
  struct OutputBuffer buffer = {mymalloc_cat(16 * sizeof(struct Halo), MEM_HALOS), 0, 16};
  /* Drop the last segment: rows 6 and 10 now have no segment holding their host. */
  marshal_workspace_to_output_buffer(&ws, &buffer, segments, 2);
}

/**
 * @test    test_marshal_unplaced_host_is_fatal
 * @brief   A created row whose host lies in no segment aborts the marshal
 */
int test_marshal_unplaced_host_is_fatal(void) {
  const int rc = expect_fatal(NULL, marshal_with_unplaced_host, "Marshalled 3 of 5 created records",
                              "lies in no output segment");
  TEST_ASSERT_EQUAL(rc, 1, "an unplaceable created row is fatal");
  return TEST_PASS;
}

/* ==========================================================================
 * Memory and determinism
 * ========================================================================== */

/**
 * @test    test_memory_high_water_and_release
 * @brief   Scratch exists only once a record is created, grows to a high water and is released
 */
int test_memory_high_water_and_release(void) {
  const int types[] = {0, 1, 2};
  size_t start[MEM_MAX_CATEGORY];
  for (int c = 0; c < MEM_MAX_CATEGORY; c++) {
    start[c] = memory_category_bytes((MemoryCategory)c);
  }

  prepare_config(0);
  creator_action = create_per_type0;
  test_pre_timestep_add("rc_creator", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pipeline initialises");
  build_workspace(types, 3);

  /* A FoF step that creates nothing allocates nothing. */
  creator_creates_per_type0 = 0;
  const size_t before_none = memory_category_bytes(MEM_HALOS);
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_HALOS), before_none,
                    "no creation, no record-creation allocation");
  TEST_ASSERT(workspace.created_host == NULL, "no created-host map without creation");

  /* Step 1: 300 records: more than the first staging block's 256 rows, so the
   * scratch reaches two blocks (256 + 512 rows) and later steps stay inside them. */
  creator_creates_per_type0 = 300;
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT_EQUAL(workspace.count, 303, "300 records committed");
  int64_t first_ids[300];
  for (int i = 0; i < 300; i++) {
    first_ids[i] = workspace.halos[3 + i].UniqueGalaxyID;
  }
  const size_t after_first = memory_category_bytes(MEM_HALOS);
  TEST_ASSERT(after_first > before_none, "creation allocated workspace and scratch memory");

  /* Step 2: the same FoF group again, as the next step would rebuild it. */
  workspace.count = 3;
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_HALOS), after_first,
                    "a repeat step reuses the high-water scratch without allocating");
  for (int i = 0; i < 300; i++) {
    TEST_ASSERT_EQUAL(workspace.halos[3 + i].UniqueGalaxyID, first_ids[i],
                      "ordinals restart per FoF step, so created IDs repeat exactly");
  }

  /* Step 3: fewer records: nothing shrinks. */
  workspace.count = 3;
  creator_creates_per_type0 = 10;
  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT_EQUAL(memory_category_bytes(MEM_HALOS), after_first,
                    "scratch stays at its high water");

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");
  free_workspace();
  module_release_record_creation_scratch();
  module_release_record_creation_scratch(); /* idempotent */
  for (int c = 0; c < MEM_MAX_CATEGORY; c++) {
    TEST_ASSERT_EQUAL(memory_category_bytes((MemoryCategory)c), start[c],
                      "every category returns to its starting bytes: no leak");
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: record creation (module_create_record)\n");
  printf("============================================================\n");
  printf("%s", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_refused_outside_full_halo);
  TEST_RUN(test_refused_bad_hosts);
  TEST_RUN(test_refused_when_identity_space_does_not_fit);
  TEST_RUN(test_staged_row_initialisation);
  TEST_RUN(test_staging_blocks_are_logarithmic);
  TEST_RUN(test_created_galaxies_are_independent);
  TEST_RUN(test_created_rows_are_inherited_by_deep_copy);
  TEST_RUN(test_visibility_across_modules_phases_and_substeps);
  TEST_RUN(test_fixture_creates_records);
  TEST_RUN(test_fixture_by_galaxy_creation_fails);
  TEST_RUN(test_event_rule);
  TEST_RUN(test_marshal_merge);
  TEST_RUN(test_marshal_unplaced_host_is_fatal);
  TEST_RUN(test_memory_high_water_and_release);

  TEST_SUMMARY();
  return TEST_RESULT();
}
