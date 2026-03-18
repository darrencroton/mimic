/**
 * @file    test_event_producer_b.c
 * @brief   Second synthetic event producer for multi-producer routing tests
 *
 * WARNING: This module is for TESTING INFRASTRUCTURE ONLY
 *
 * Emits test_event_b so that, when run alongside test_event_producer in
 * the same phase, the test harness can verify that two independent producers
 * dispatch their events independently to their respective consumers.
 */

#include "_system/generated/event_contracts.h"
#include "error.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

/** Number of test_event_b events to emit per process() call (0 = none, N = at most N per group) */
static int EMIT_B_COUNT;

/** Cumulative events emitted across all process() calls */
static int total_emitted = 0;

int test_event_producer_b_init(void) {
    if (model_get_int("TestEventProducerBEmitCount", &EMIT_B_COUNT) != 0) {
        ERROR_LOG("Failed to read TestEventProducerBEmitCount from model_parameters");
        return -1;
    }
    if (EMIT_B_COUNT < 0 || EMIT_B_COUNT > 64) {
        ERROR_LOG("TestEventProducerBEmitCount must be in [0, 64], got %d", EMIT_B_COUNT);
        return -1;
    }
    INFO_LOG("test_event_producer_b initialized (emit_b_count=%d)", EMIT_B_COUNT);
    return 0;
}

int test_event_producer_b_process(struct ModuleContext *ctx, struct Halo *halos,
                                  int ngal) {
    if (ctx == NULL || halos == NULL || ngal <= 0) {
        return 0;
    }

    for (int i = 0; i < ngal && i < EMIT_B_COUNT; i++) {
        if (halos[i].galaxy == NULL) {
            continue;
        }
        int target = (i + 1 < ngal) ? i + 1 : 0;
        int rc = module_emit_event(ctx,
                                   TEST_EVENT_PRODUCER_B_EVENT_TEST_EVENT_B,
                                   i, target,
                                   (double)total_emitted,
                                   (double)ngal);
        if (rc != 0) {
            ERROR_LOG("test_event_producer_b: test_event_b emit failed (i=%d)", i);
            return -1;
        }
        total_emitted++;
    }

    return 0;
}

int test_event_producer_b_cleanup(void) {
    INFO_LOG("test_event_producer_b: total events emitted = %d", total_emitted);
    total_emitted = 0;
    return 0;
}
