# `test_event_producer_b`

Infrastructure-only second event producer used by multi-producer routing tests.

## Contract

- Supported mode: `process_full_halo`
- Emits synthetic `test_event_b` events
- Uses a test parameter to control event count
- Drives routing tests for `test_event_consumer_gamma`

## Parameters

- `TestEventProducerBEmitCount`

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
