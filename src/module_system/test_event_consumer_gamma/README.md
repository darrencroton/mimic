# `test_event_consumer_gamma`

Infrastructure-only event consumer used by routing tests.

## Contract

- Supported mode: `process_per_event`
- Consumes `test_event_b` from `test_event_producer_b`
- Verifies routing when multiple independent producers are configured in the same phase

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
