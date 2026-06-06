/**
 * @file    test_event_consumer_gamma.c
 * @brief   Third synthetic event consumer for multi-producer routing tests
 *
 * WARNING: This module is for TESTING INFRASTRUCTURE ONLY
 *
 * Subscribes to test_event_producer_b's test_event_b. When run alongside
 * test_event_consumer_alpha (which subscribes to test_event_producer's test_event),
 * verifies that two producers in the same phase dispatch independently.
 */

#include "error.h"
#include "module_interface.h"
#include "types.h"

/** Cumulative events received by this consumer */
static int events_received = 0;

int test_event_consumer_gamma_init(void) {
  INFO_LOG("test_event_consumer_gamma initialized");
  return 0;
}

int test_event_consumer_gamma_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ctx == NULL || halos == NULL || ngal <= 0) {
    return 0;
  }

  if (ctx->active_event == NULL) {
    ERROR_LOG("test_event_consumer_gamma called without active_event");
    return -1;
  }

  events_received++;

  const struct ModuleEvent *ev = ctx->active_event;
  INFO_LOG("TEST_CONSUMER_GAMMA: received event #%d "
           "(producer_module_id=%d, event_id=%d, source=%d, target=%d, "
           "value0=%.3f, value1=%.3f)",
           events_received, ev->producer_module_id, ev->event_id, ev->source_index,
           ev->target_index, ev->value0, ev->value1);

  return 0;
}

int test_event_consumer_gamma_cleanup(void) {
  INFO_LOG("test_event_consumer_gamma: total events received = %d", events_received);
  events_received = 0;
  return 0;
}
