/**
 * @file    test_snapshot_fixture.c
 * @brief   Snapshot-only test fixture module implementation
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * The module advertises only process_snapshot, so it implements init(),
 * process_snapshot() and cleanup() and no FoF process() callback; generated
 * registration sets .process = NULL. It lets infrastructure tests exercise the
 * snapshot callback family with a real module instead of a production one,
 * configured as `- test_snapshot_fixture: process_snapshot` under
 * modules.post_snapshot.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <stdint.h>
#include <string.h>

#include "error.h"
#include "module_interface.h"
#include "types.h"

/** Value written to TestDummyProperty for every entry (inside its declared [0, 1] range) */
#define TEST_SNAPSHOT_FIXTURE_VALUE 0.5f

/** Number of process_snapshot() calls since init(); reported at cleanup */
static int snapshot_call_count = 0;

/**
 * @brief   Initialize the snapshot fixture
 *
 * The fixture reads no parameters.
 *
 * @return  0 on success
 */
int test_snapshot_fixture_init(void) {
  snapshot_call_count = 0;
  INFO_LOG("Test snapshot fixture module initialized");
  INFO_LOG("  ⚠️  WARNING: Testing infrastructure only - NOT FOR PRODUCTION");
  return 0;
}

/** Event ID the fixture tries to emit; any positive value, since emission must be rejected */
#define TEST_SNAPSHOT_FIXTURE_PROBE_EVENT_ID 1

/**
 * @brief   Process one borrowed snapshot population
 *
 * Checks the borrowed-view contract (see struct Module.process_snapshot) and
 * that snapshot dispatch rejects FoF event emission: it calls
 * module_emit_event() with a non-NULL local ModuleContext and fails the call
 * unless that returns -1. Outside post_snapshot dispatch the emitter's
 * direct-unit-test shortcut accepts the event, so the fixture succeeds only
 * when the core dispatches it.
 *
 * It then summarises the population as it arrived (Type counts, entries whose
 * SnapNum is not this snapshot, entries still at TestDummyProperty's init
 * value, the largest TestDummyProperty seen, and the wrapping sum of
 * UniqueGalaxyID), sets TestDummyProperty = TEST_SNAPSHOT_FIXTURE_VALUE on
 * every entry through halos[i].galaxy, the only permitted write, and logs one
 * TEST_SNAPSHOT_FIXTURE_EXEC marker so tests can count, order and audit calls.
 *
 * @param   ctx     Snapshot context
 * @param   halos   Borrowed snapshot population; may be NULL when count is 0
 * @param   count   Number of entries in halos
 * @return  0 on success, -1 if the caller violated the borrowed-view contract
 *          or accepted the event emission
 */
int test_snapshot_fixture_process_snapshot(const struct SnapshotContext *ctx,
                                           const struct Halo *halos, int64_t count) {
  if (ctx == NULL || count < 0) {
    ERROR_LOG("test_snapshot_fixture: invalid call (ctx=%p, count=%lld)", (const void *)ctx,
              (long long)count);
    return -1;
  }
  if (count > 0 && halos == NULL) {
    ERROR_LOG("test_snapshot_fixture: NULL population with count=%lld at snapshot %d",
              (long long)count, ctx->snapshot_number);
    return -1;
  }

  struct ModuleContext probe;
  memset(&probe, 0, sizeof(probe));
  probe.params = ctx->params;
  if (module_emit_event(&probe, TEST_SNAPSHOT_FIXTURE_PROBE_EVENT_ID, 0, 0, 0.0, 0.0) != -1) {
    ERROR_LOG("test_snapshot_fixture: module_emit_event was accepted at snapshot %d; snapshot "
              "dispatch must reject FoF event emission",
              ctx->snapshot_number);
    return -1;
  }

  snapshot_call_count++;

  long long type_counts[3] = {0, 0, 0};
  long long other_types = 0;
  long long foreign_snapnum = 0;
  long long seen_zero = 0;
  float seen_max = 0.0f;
  uint64_t id_sum = 0;
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("test_snapshot_fixture: entry %lld has NULL galaxy at snapshot %d", (long long)i,
                ctx->snapshot_number);
      return -1;
    }
    if (halos[i].Type >= 0 && halos[i].Type <= 2) {
      type_counts[halos[i].Type]++;
    } else {
      other_types++;
    }
    if (halos[i].SnapNum != ctx->snapshot_number) {
      foreign_snapnum++;
    }
    const float seen = halos[i].galaxy->TestDummyProperty;
    if (seen == 0.0f) {
      seen_zero++;
    }
    if (seen > seen_max) {
      seen_max = seen;
    }
    id_sum += (uint64_t)halos[i].UniqueGalaxyID;

    halos[i].galaxy->TestDummyProperty = TEST_SNAPSHOT_FIXTURE_VALUE;
  }

  INFO_LOG("TEST_SNAPSHOT_FIXTURE_EXEC: call=%d snapshot=%d count=%lld z=%.4f time=%.9e "
           "types=%lld/%lld/%lld other_types=%lld foreign_snapnum=%lld seen_zero=%lld "
           "seen_max=%.4f id_sum=%llu emit=rejected",
           snapshot_call_count, ctx->snapshot_number, (long long)count, ctx->redshift, ctx->time,
           type_counts[0], type_counts[1], type_counts[2], other_types, foreign_snapnum, seen_zero,
           (double)seen_max, (unsigned long long)id_sum);
  return 0;
}

/**
 * @brief   Cleanup the snapshot fixture
 *
 * @return  0 on success
 */
int test_snapshot_fixture_cleanup(void) {
  INFO_LOG("TEST_SNAPSHOT_FIXTURE_CLEANUP: total_calls=%d", snapshot_call_count);
  snapshot_call_count = 0; /* reset for re-init safety */
  return 0;
}
