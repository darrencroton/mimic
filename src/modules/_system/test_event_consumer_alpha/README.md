# `test_event_consumer_alpha`

Infrastructure-only event consumer used by routing tests.

## Contract

- Supported mode: `process_per_event`
- Consumes `test_event` from `test_event_producer`
- Verifies that a consumer subscribed to one producer/event pair does not receive unrelated events

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
