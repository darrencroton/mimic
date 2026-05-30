# `test_event_producer`

Infrastructure-only event producer used by routing tests.

## Contract

- Supported mode: `process_full_halo`
- Emits synthetic `test_event` and `test_event_alt` events
- Uses test parameters to control event counts
- Drives selective-routing tests for `test_event_consumer_alpha` and `test_event_consumer_beta`

## Parameters

- `TestEventProducerEmitCount`
- `TestEventProducerEmitAltCount`

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
