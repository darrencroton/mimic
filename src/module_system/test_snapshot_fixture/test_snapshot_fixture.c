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
 * snapshot callback family with a real module instead of a production one.
 *
 * Vision Principle #1: Physics-Agnostic Core Infrastructure
 */

#include <stdint.h>

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

/**
 * @brief   Process one borrowed snapshot population
 *
 * Checks the borrowed-view contract (see struct Module.process_snapshot), then
 * sets TestDummyProperty = TEST_SNAPSHOT_FIXTURE_VALUE on every entry through
 * halos[i].galaxy, the only permitted write. Logs one TEST_SNAPSHOT_FIXTURE_EXEC
 * marker per call so tests can count and order calls.
 *
 * @param   ctx     Snapshot context
 * @param   halos   Borrowed snapshot population; may be NULL when count is 0
 * @param   count   Number of entries in halos
 * @return  0 on success, -1 if the caller violated the borrowed-view contract
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

  snapshot_call_count++;

  for (int64_t i = 0; i < count; i++) {
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("test_snapshot_fixture: entry %lld has NULL galaxy at snapshot %d", (long long)i,
                ctx->snapshot_number);
      return -1;
    }
    halos[i].galaxy->TestDummyProperty = TEST_SNAPSHOT_FIXTURE_VALUE;
  }

  INFO_LOG("TEST_SNAPSHOT_FIXTURE_EXEC: call=%d snapshot=%d count=%lld z=%.4f", snapshot_call_count,
           ctx->snapshot_number, (long long)count, ctx->redshift);
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
