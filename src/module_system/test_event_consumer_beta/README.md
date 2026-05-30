# `test_event_consumer_beta`

Infrastructure-only event consumer used by routing tests.

## Contract

- Supported mode: `process_per_event`
- Consumes `test_event_alt` from `test_event_producer`
- Verifies selective delivery when one producer emits multiple event names

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
