/**
 * @file    test_event_producer.c
 * @brief   Synthetic event producer for event system integration tests
 *
 * WARNING: This module is for TESTING INFRASTRUCTURE ONLY
 *
 * Emits a configurable number of test_event events per process() call so
 * that test_event_consumer_alpha and test_event_consumer_beta can verify
 * correct subscription routing and delivery.
 */

#include "module_system/generated/event_contracts.h"
#include "module_system/parameter_helpers.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

/**
 * Upper bound accepted for either emit-count parameter.
 *
 * Deliberately above the 4096-event fixed cap the phase event buffer used to
 * carry, so a test can drive that buffer past the old ceiling and prove it now
 * grows (tests/unit/test_event_buffer_growth.c). It stays a bound rather than
 * being removed so a mistyped parameter still fails in init() instead of
 * emitting for a very long time.
 */
#define TEST_EVENT_PRODUCER_MAX_EMIT_COUNT 65536

/** Number of test_event events to emit per process() call (0 = none, N = at most N per group) */
static int EMIT_COUNT;

/** Number of test_event_alt events to emit per process() call (0 = none, N = at most N per group)
 */
static int EMIT_ALT_COUNT;

/** Cumulative events emitted across all process() calls */
static int total_emitted = 0;

int test_event_producer_init(void) {
  if (model_get_int("TestEventProducerEmitCount", &EMIT_COUNT) != 0) {
    ERROR_LOG("Failed to read TestEventProducerEmitCount from model_parameters");
    return -1;
  }
  if (EMIT_COUNT < 0 || EMIT_COUNT > TEST_EVENT_PRODUCER_MAX_EMIT_COUNT) {
    ERROR_LOG("TestEventProducerEmitCount must be in [0, %d], got %d",
              TEST_EVENT_PRODUCER_MAX_EMIT_COUNT, EMIT_COUNT);
    return -1;
  }
  if (model_get_int("TestEventProducerEmitAltCount", &EMIT_ALT_COUNT) != 0) {
    ERROR_LOG("Failed to read TestEventProducerEmitAltCount from model_parameters");
    return -1;
  }
  if (EMIT_ALT_COUNT < 0 || EMIT_ALT_COUNT > TEST_EVENT_PRODUCER_MAX_EMIT_COUNT) {
    ERROR_LOG("TestEventProducerEmitAltCount must be in [0, %d], got %d",
              TEST_EVENT_PRODUCER_MAX_EMIT_COUNT, EMIT_ALT_COUNT);
    return -1;
  }
  INFO_LOG("test_event_producer initialized (emit_count=%d, emit_alt_count=%d)", EMIT_COUNT,
           EMIT_ALT_COUNT);
  return 0;
}

int test_event_producer_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ctx == NULL || halos == NULL || ngal <= 0) {
    return 0;
  }

  /* Emit test_event (EMIT_COUNT=0 means emit none) */
  for (int i = 0; i < ngal && i < EMIT_COUNT; i++) {
    if (halos[i].galaxy == NULL) {
      continue;
    }
    int target = (i + 1 < ngal) ? i + 1 : 0;
    int rc = module_emit_event(ctx, TEST_EVENT_PRODUCER_EVENT_TEST_EVENT, i, target,
                               (double)total_emitted, (double)ngal);
    if (rc != 0) {
      ERROR_LOG("test_event_producer: test_event emit failed (i=%d)", i);
      return -1;
    }
    total_emitted++;
  }

  /* Emit test_event_alt (EMIT_ALT_COUNT=0 means emit none) */
  for (int i = 0; i < ngal && i < EMIT_ALT_COUNT; i++) {
    if (halos[i].galaxy == NULL) {
      continue;
    }
    int target = (i + 1 < ngal) ? i + 1 : 0;
    int rc = module_emit_event(ctx, TEST_EVENT_PRODUCER_EVENT_TEST_EVENT_ALT, i, target,
                               (double)total_emitted, (double)ngal);
    if (rc != 0) {
      ERROR_LOG("test_event_producer: test_event_alt emit failed (i=%d)", i);
      return -1;
    }
    total_emitted++;
  }

  return 0;
}

int test_event_producer_cleanup(void) {
  INFO_LOG("test_event_producer: total events emitted = %d", total_emitted);
  total_emitted = 0;
  return 0;
}
