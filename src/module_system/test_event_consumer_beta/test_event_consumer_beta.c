/**
 * @file    test_event_consumer_beta.c
 * @brief   Second synthetic event consumer for event routing integration tests
 *
 * WARNING: This module is for TESTING INFRASTRUCTURE ONLY
 *
 * Subscribes to test_event_producer's test_event_alt (distinct from alpha's
 * test_event). Together with test_event_consumer_alpha, verifies that selective
 * routing delivers events only to the consumer with the matching subscription.
 */

#include "error.h"
#include "module_interface.h"
#include "types.h"

/** Cumulative events received by this consumer */
static int events_received = 0;

int test_event_consumer_beta_init(void) {
    INFO_LOG("test_event_consumer_beta initialized");
    return 0;
}

int test_event_consumer_beta_process(struct ModuleContext *ctx,
                                     struct Halo *halos, int ngal) {
    if (ctx == NULL || halos == NULL || ngal <= 0) {
        return 0;
    }

    if (ctx->active_event == NULL) {
        ERROR_LOG("test_event_consumer_beta called without active_event");
        return -1;
    }

    events_received++;

    const struct ModuleEvent *ev = ctx->active_event;
    INFO_LOG("TEST_CONSUMER_BETA: received event #%d "
             "(producer_module_id=%d, event_id=%d, source=%d, target=%d, "
             "value0=%.3f, value1=%.3f)",
             events_received, ev->producer_module_id, ev->event_id,
             ev->source_index, ev->target_index, ev->value0, ev->value1);

    return 0;
}

int test_event_consumer_beta_cleanup(void) {
    INFO_LOG("test_event_consumer_beta: total events received = %d",
             events_received);
    events_received = 0;
    return 0;
}
