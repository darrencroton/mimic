#!/usr/bin/env python3
"""
Event Schema Validation Tests

Validates that the module registry generator rejects invalid event
declarations with clear error messages.

Tests the generator-level checks called out in the plan
(docs/EVENT-SYSTEM-IMPROVEMENTS.md §444-449):

  - duplicate emitted event names within a producer
  - consumer subscribing to a producer that has no events.emits
  - consumer subscribing to an event name that does not exist on the producer
  - consumer module does not support process_per_event mode
  - producer module does not support process_full_halo mode

These tests call the generator's pure validation functions directly
(no file I/O, no subprocess) for fast, isolated coverage.

Runtime/phase validation (invalid mode/event combinations at startup)
is covered in tests/integration/test_processing_modes.py.

Author: Mimic Testing Team
Date: 2026-03-18
Phase: Phase 5 (Event System Implementation)
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import TestSkipped, result_error, result_fail, result_pass, result_skip
from generate_module_registry import collect_event_info, validate_event_declarations

# ANSI color codes
BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
NC = "\033[0m"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def make_producer(name, event_names, modes=None):
    """Return a minimal producer module dict."""
    return {
        "name": name,
        "supported_processing_modes": modes or ["process_full_halo"],
        "events": {"emits": [{"name": n, "description": f"test event {n}"} for n in event_names]},
    }


def make_consumer(name, subscriptions, modes=None):
    """Return a minimal consumer module dict.

    subscriptions: list of (producer_name, event_name) tuples.
    """
    return {
        "name": name,
        "supported_processing_modes": modes or ["process_per_event"],
        "events": {"consumes": [{"producer": p, "event": e} for p, e in subscriptions]},
    }


def make_plain(name, modes=None):
    """Return a module with no event declarations."""
    return {
        "name": name,
        "supported_processing_modes": modes or ["process_full_halo"],
    }


def collect_and_validate(modules):
    """Run collect_event_info + validate_event_declarations; return errors list."""
    event_info, collect_errors = collect_event_info(modules)
    validation_errors = validate_event_declarations(modules, event_info)
    return collect_errors + validation_errors


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_duplicate_emitted_event_name_fails():
    """
    A producer declaring the same event name twice must be rejected.

    Validates: generator catches duplicate names in events.emits.
    """
    print("Testing duplicate emitted event name fails...")

    producer = {
        "name": "my_producer",
        "supported_processing_modes": ["process_full_halo"],
        "events": {
            "emits": [
                {"name": "my_event", "description": "first"},
                {"name": "my_event", "description": "duplicate"},
            ]
        },
    }

    errors = collect_and_validate([producer])

    assert any(
        "duplicate" in e and "my_event" in e for e in errors
    ), f"Expected duplicate event name error, got: {errors}"

    print(f"  ✓ Duplicate event name correctly rejected ({len(errors)} error(s))")


def test_consumer_of_nonexistent_producer_fails():
    """
    A consumer subscribing to a producer that declares no events.emits must fail.

    Validates: generator rejects references to unknown producers.
    """
    print("Testing consumer of nonexistent producer fails...")

    consumer = make_consumer("my_consumer", [("ghost_producer", "ghost_event")])
    modules = [make_plain("ghost_producer"), consumer]

    errors = collect_and_validate(modules)

    assert any(
        "ghost_producer" in e for e in errors
    ), f"Expected unknown-producer error, got: {errors}"

    print(f"  ✓ Subscription to nonexistent producer correctly rejected ({len(errors)} error(s))")


def test_consumer_of_nonexistent_event_fails():
    """
    A consumer subscribing to an event name that does not exist on the producer must fail.

    Validates: generator rejects references to undeclared event names.
    """
    print("Testing consumer of nonexistent event fails...")

    producer = make_producer("real_producer", ["real_event"])
    consumer = make_consumer("my_consumer", [("real_producer", "no_such_event")])

    errors = collect_and_validate([producer, consumer])

    assert any(
        "no_such_event" in e and "real_producer" in e for e in errors
    ), f"Expected undeclared-event error, got: {errors}"

    print(f"  ✓ Subscription to nonexistent event correctly rejected ({len(errors)} error(s))")


def test_emits_on_non_full_halo_module_fails():
    """
    A module declaring events.emits but not supporting process_full_halo must fail.

    Validates: generator enforces emitter mode constraint.
    """
    print("Testing events.emits on non-process_full_halo module fails...")

    producer = make_producer("wrong_mode_producer", ["my_event"], modes=["process_by_galaxy"])

    errors = collect_and_validate([producer])

    assert any(
        "wrong_mode_producer" in e and "process_full_halo" in e for e in errors
    ), f"Expected mode constraint error for emitter, got: {errors}"

    print(
        f"  ✓ events.emits on non-process_full_halo module correctly rejected ({len(errors)} error(s))"
    )


def test_consumes_on_non_per_event_module_fails():
    """
    A module declaring events.consumes but not supporting process_per_event must fail.

    Validates: generator enforces consumer mode constraint.
    """
    print("Testing events.consumes on non-process_per_event module fails...")

    producer = make_producer("my_producer", ["my_event"])
    consumer = make_consumer(
        "wrong_mode_consumer", [("my_producer", "my_event")], modes=["process_full_halo"]
    )

    errors = collect_and_validate([producer, consumer])

    assert any(
        "wrong_mode_consumer" in e and "process_per_event" in e for e in errors
    ), f"Expected mode constraint error for consumer, got: {errors}"

    print(
        f"  ✓ events.consumes on non-process_per_event module correctly rejected ({len(errors)} error(s))"
    )


def test_valid_declarations_produce_no_errors():
    """
    A well-formed producer/consumer pair must pass without errors.

    Validates: the validation logic does not produce false positives.
    """
    print("Testing valid declarations produce no errors...")

    producer = make_producer("good_producer", ["event_a", "event_b"])
    consumer_a = make_consumer("consumer_a", [("good_producer", "event_a")])
    consumer_b = make_consumer("consumer_b", [("good_producer", "event_b")])

    errors = collect_and_validate([producer, consumer_a, consumer_b])

    assert len(errors) == 0, f"Expected no errors for valid declarations, got: {errors}"

    print(f"  ✓ Valid declarations accepted with no errors")


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


def main():
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: Event Schema Validation (test_event_schema_validation.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_duplicate_emitted_event_name_fails,
        test_consumer_of_nonexistent_producer_fails,
        test_consumer_of_nonexistent_event_fails,
        test_emits_on_non_full_halo_module_fails,
        test_consumes_on_non_per_event_module_fails,
        test_valid_declarations_produce_no_errors,
    ]

    passed = 0
    failed = 0
    skipped = 0

    for test_func in tests:
        print()
        try:
            test_func()
            result_pass(test_func.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test_func.__name__, str(e))
            skipped += 1
        except AssertionError as e:
            result_fail(test_func.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test_func.__name__, str(e).splitlines()[0])
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
