/**
 * @file    test_event_buffer_growth.c
 * @brief   Unit tests for phase event buffer growth in the module dispatcher
 *
 * The dispatcher's phase-local event buffer used to be a fixed 4096-slot static
 * array: a producer emitting more than that in a single execute_phase() call
 * got a -1 from module_emit_event(), which physics producers propagate out of
 * process(), which execute_phase() turns into a FATAL_ERROR. That cap is a
 * function of FoF group size, so it held on small catalogs and aborted
 * production runs on large ones.
 *
 * These tests drive one execute_phase() call past the old cap and assert that
 * it completes and that every emitted event still reaches its subscribed
 * consumer, so a reintroduced fixed cap fails here rather than mid-run.
 *
 * Framework fixtures only (VISION.md Principle 1): test_event_producer emits,
 * test_event_consumer_alpha subscribes. No production physics module is involved.
 */

#include "../framework/test_framework.h"
#include "../../src/core/module_registry.h"
#include "../framework/test_phase_config.h"
#include "../../src/core/module_interface.h"
#include "../../src/include/types.h"
#include "../../src/include/proto.h"
#include "../../src/include/globals.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Shared core-test fixtures (config reset, registration) */
#include "../framework/core_test_fixtures.h"

/**
 * Events emitted in the single execute_phase() call under test.
 *
 * Must exceed the historical 4096-slot cap by enough that the test still
 * proves the point if the growth increment changes, and stays small enough
 * that the buffer costs well under a megabyte. test_event_producer emits at
 * most one event per halo, so this is also the workspace size.
 */
#define EVENTS_TO_EMIT 5000

/** Log line test_event_consumer_alpha writes once per delivered event */
#define CONSUMER_EVENT_MARKER "TEST_CONSUMER_ALPHA: received event #"

/** Log line the dispatcher writes each time the event buffer is reallocated */
#define BUFFER_GROWTH_MARKER "Growing phase event buffer"

/* Workspace owned by the test for the duration of one case */
static struct Halo *workspace = NULL;
static struct GalaxyData *galaxies = NULL;

/**
 * @brief   Build a flat FoF workspace of `ngal` halos, each with a galaxy
 *
 * test_event_producer only requires a non-NULL galaxy pointer per halo, so the
 * galaxy slots are zeroed rather than populated with physics. Index 0 is the
 * central; the rest are satellites, matching how the drivers lay a workspace out.
 */
static void build_workspace(int ngal) {
  workspace = mymalloc_cat((size_t)ngal * sizeof(struct Halo), MEM_HALOS);
  galaxies = mymalloc_cat((size_t)ngal * sizeof(struct GalaxyData), MEM_GALAXIES);
  memset(workspace, 0, (size_t)ngal * sizeof(struct Halo));
  memset(galaxies, 0, (size_t)ngal * sizeof(struct GalaxyData));

  for (int i = 0; i < ngal; i++) {
    workspace[i].Type = (i == 0) ? 0 : 1;
    workspace[i].HaloNr = i;
    workspace[i].SnapNum = 1;
    workspace[i].galaxy = &galaxies[i];
  }
}

static void free_workspace(void) {
  if (galaxies != NULL) {
    myfree(galaxies);
    galaxies = NULL;
  }
  if (workspace != NULL) {
    myfree(workspace);
    workspace = NULL;
  }
}

/** @brief Set the two test_event_producer parameters in MimicConfig.ModelParams */
static void set_event_producer_params(int emit_count, int emit_alt_count) {
  int idx = 0;

  strcpy(MimicConfig.ModelParams[idx].param_name, "TestEventProducerEmitCount");
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%d", emit_count);
  idx++;

  strcpy(MimicConfig.ModelParams[idx].param_name, "TestEventProducerEmitAltCount");
  snprintf(MimicConfig.ModelParams[idx].value, MAX_STRING_LEN, "%d", emit_alt_count);
  idx++;

  MimicConfig.NumModelParams = idx;
}

/** @brief Minimal module context for a single-substep full-halo phase */
static void build_context(struct ModuleContext *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->redshift = 0.0;
  ctx->snapshot_number = 1;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
  ctx->central_index = 0;
  ctx->central_galaxy = &workspace[0];
  ctx->active_event = NULL;
  ctx->params = &MimicConfig;
}

/** @brief Count lines in `log` containing `marker` */
static int count_marker_lines(FILE *log, const char *marker) {
  char line[1024];
  int count = 0;

  rewind(log);
  while (fgets(line, sizeof(line), log) != NULL) {
    if (strstr(line, marker) != NULL) {
      count++;
    }
  }
  return count;
}

/**
 * @test    test_event_buffer_grows_past_legacy_cap
 * @brief   One execute_phase() call emitting far more than 4096 events completes
 *
 * Expected: the phase completes, the buffer reports growth, and the subscribed
 * consumer receives exactly one delivery per emitted event.
 * Validates: the phase event buffer sizes itself to the emitting FoF group
 * instead of failing at a fixed cap, and reallocation does not disturb dispatch.
 */
int test_event_buffer_grows_past_legacy_cap(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  set_event_producer_params(EVENTS_TO_EMIT, 0);
  test_phase_add("galaxy_physics", "test_event_producer", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "test_event_consumer_alpha", PROCESSING_MODE_PER_EVENT);
  MimicConfig.SubSteps = 1;

  int init_result = module_system_init();
  TEST_ASSERT_EQUAL(init_result, 0, "module_system_init should succeed for producer+alpha");

  build_workspace(EVENTS_TO_EMIT);

  struct ModuleContext ctx;
  build_context(&ctx);

  /* The consumer logs one INFO line per delivered event. Capture the log to a
   * temporary file: it keeps the tier's output readable and makes the delivery
   * count directly assertable rather than inferred. */
  FILE *log = tmpfile();
  TEST_ASSERT(log != NULL, "tmpfile() should provide a capture stream for the dispatch log");

  LogLevel prior_level = get_log_level();
  set_log_level(LOG_LEVEL_INFO);
  FILE *prior_output = set_log_output(log);

  /* ===== EXECUTE ===== */
  /* Before the fix this call aborted the process inside execute_phase(). */
  execute_phase(MimicConfig.substep_phases[0].modules, MimicConfig.substep_phases[0].num_modules,
                &ctx, workspace, EVENTS_TO_EMIT);

  set_log_output(prior_output);
  set_log_level(prior_level);

  /* ===== VERIFY ===== */
  int delivered = count_marker_lines(log, CONSUMER_EVENT_MARKER);
  int growths = count_marker_lines(log, BUFFER_GROWTH_MARKER);

  TEST_ASSERT_EQUAL(delivered, EVENTS_TO_EMIT,
                    "Every emitted event should reach the subscribed consumer");
  TEST_ASSERT(growths > 0, "The event buffer should have grown to hold the emitted events");

  /* ===== CLEANUP ===== */
  fclose(log);
  free_workspace();
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_event_buffer_capacity_is_reused_across_phases
 * @brief   A second, equally large phase runs without further reallocation
 *
 * Expected: the second execute_phase() call delivers the same event count and
 * logs no additional buffer growth.
 * Validates: the buffer is run-persistent grow-to-high-water, not a per-phase
 * allocation — VISION.md Principle 5's bounded, explicitly owned memory, with
 * no allocation churn on the dispatch hot path.
 */
int test_event_buffer_capacity_is_reused_across_phases(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  set_event_producer_params(EVENTS_TO_EMIT, 0);
  test_phase_add("galaxy_physics", "test_event_producer", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "test_event_consumer_alpha", PROCESSING_MODE_PER_EVENT);
  MimicConfig.SubSteps = 1;

  int init_result = module_system_init();
  TEST_ASSERT_EQUAL(init_result, 0, "module_system_init should succeed for producer+alpha");

  build_workspace(EVENTS_TO_EMIT);

  struct ModuleContext ctx;
  build_context(&ctx);

  FILE *log = tmpfile();
  TEST_ASSERT(log != NULL, "tmpfile() should provide a capture stream for the dispatch log");

  LogLevel prior_level = get_log_level();
  set_log_level(LOG_LEVEL_INFO);
  FILE *prior_output = set_log_output(log);

  /* ===== EXECUTE ===== */
  /* First call sizes the buffer; only the second is under test. */
  execute_phase(MimicConfig.substep_phases[0].modules, MimicConfig.substep_phases[0].num_modules,
                &ctx, workspace, EVENTS_TO_EMIT);

  int growths_after_first = count_marker_lines(log, BUFFER_GROWTH_MARKER);
  int delivered_after_first = count_marker_lines(log, CONSUMER_EVENT_MARKER);

  fseek(log, 0, SEEK_END);
  execute_phase(MimicConfig.substep_phases[0].modules, MimicConfig.substep_phases[0].num_modules,
                &ctx, workspace, EVENTS_TO_EMIT);

  set_log_output(prior_output);
  set_log_level(prior_level);

  /* ===== VERIFY ===== */
  int growths_total = count_marker_lines(log, BUFFER_GROWTH_MARKER);
  int delivered_total = count_marker_lines(log, CONSUMER_EVENT_MARKER);

  TEST_ASSERT_EQUAL(delivered_after_first, EVENTS_TO_EMIT,
                    "First phase should deliver one event per emitted event");
  TEST_ASSERT_EQUAL(delivered_total, 2 * EVENTS_TO_EMIT,
                    "Second phase should deliver the same number of events again");
  TEST_ASSERT_EQUAL(growths_total, growths_after_first,
                    "Second phase should reuse the existing capacity without reallocating");

  /* ===== CLEANUP ===== */
  fclose(log);
  free_workspace();
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Phase Event Buffer Growth\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);

  TEST_RUN(test_event_buffer_grows_past_legacy_cap);
  TEST_RUN(test_event_buffer_capacity_is_reused_across_phases);

  TEST_SUMMARY();

  printf("\n");
  printf("Memory leak check:\n");
  check_memory_leaks();

  return TEST_RESULT();
}
