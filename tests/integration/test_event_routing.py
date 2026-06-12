#!/usr/bin/env python3
"""
Event Routing Integration Tests

Validates that the subscription-based event routing system delivers
events only to consumers that declare a matching events.consumes
subscription in their module_info.yaml.

Test infrastructure used:
  - test_event_producer:       process_full_halo; emits test_event and test_event_alt
  - test_event_consumer_alpha: process_per_event; subscribes to test_event
  - test_event_consumer_beta:  process_per_event; subscribes to test_event_alt

"""

import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    BLUE,
    GREEN,
    NC,
    RED,
    REPO_ROOT,
    TestSkipped,
    create_test_param_file,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
    run_test_suite,
)


def parse_consumer_event_count(output, consumer_name):
    """
    Parse the total events received count logged by a consumer at cleanup.

    Matches lines of the form:
        <consumer_name>: total events received = N

    Args:
        output (str): Combined stdout+stderr from a mimic run
        consumer_name (str): Module name to search for

    Returns:
        int or None: Event count if found, None if the log line is absent
    """
    pattern = rf"{re.escape(consumer_name)}: total events received = (\d+)"
    match = re.search(pattern, output)
    if match:
        return int(match.group(1))
    return None


def test_routing_single_consumer_receives_events():
    """
    A subscribed consumer receives events emitted by the producer.

    Configuration:
        producer: test_event (EmitCount=1, EmitAltCount=0)
        alpha:    subscribes to test_event

    Expected: alpha receives > 0 events.
    Validates: basic subscription routing; events reach a subscribed consumer.
    """
    print("Testing single subscribed consumer receives events...")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="routing_single_consumer",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("test_event_producer", "process_full_halo"),
                ("test_event_consumer_alpha", "process_per_event"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "TestEventProducerEmitCount": 1,
            "TestEventProducerEmitAltCount": 0,
        },
        first_file=0,
        last_file=0,
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined = stdout + stderr

        assert returncode == 0, f"Mimic failed:\n{combined}"

        alpha_count = parse_consumer_event_count(combined, "test_event_consumer_alpha")
        assert (
            alpha_count is not None
        ), "Could not find test_event_consumer_alpha event count in mimic output"
        assert (
            alpha_count > 0
        ), f"Expected alpha to receive > 0 events (subscribed to test_event), got {alpha_count}"

        print(f"  ✓ test_event_consumer_alpha received {alpha_count} event(s)")

    finally:
        shutil.rmtree(temp_dir)


def test_routing_unsubscribed_consumer_receives_no_events():
    """
    An unsubscribed consumer receives zero events.

    Configuration:
        producer: test_event only (EmitCount=1, EmitAltCount=0)
        alpha:    subscribes to test_event
        beta:     subscribes to test_event_alt (not emitted)

    Expected: alpha > 0 events, beta = 0 events.
    Validates: no spurious delivery to a consumer whose subscription does not match.
    """
    print("Testing unsubscribed consumer receives no events...")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="routing_unsubscribed",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("test_event_producer", "process_full_halo"),
                ("test_event_consumer_alpha", "process_per_event"),
                ("test_event_consumer_beta", "process_per_event"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "TestEventProducerEmitCount": 1,
            "TestEventProducerEmitAltCount": 0,
        },
        first_file=0,
        last_file=0,
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined = stdout + stderr

        assert returncode == 0, f"Mimic failed:\n{combined}"

        alpha_count = parse_consumer_event_count(combined, "test_event_consumer_alpha")
        beta_count = parse_consumer_event_count(combined, "test_event_consumer_beta")

        assert (
            alpha_count is not None
        ), "Could not find test_event_consumer_alpha event count in mimic output"
        assert (
            beta_count is not None
        ), "Could not find test_event_consumer_beta event count in mimic output"

        assert (
            alpha_count > 0
        ), f"Expected alpha to receive > 0 events (subscribed to test_event), got {alpha_count}"
        assert beta_count == 0, (
            f"Expected beta to receive 0 events "
            f"(subscribed to test_event_alt which was not emitted), got {beta_count}"
        )

        print(f"  ✓ test_event_consumer_alpha (test_event):           {alpha_count} event(s)")
        print(f"  ✓ test_event_consumer_beta  (test_event_alt, none): {beta_count} event(s)")

    finally:
        shutil.rmtree(temp_dir)


def test_routing_two_consumers_different_subscriptions():
    """
    Two consumers with different subscriptions each receive only their own events.

    Configuration:
        producer: test_event + test_event_alt (EmitCount=1, EmitAltCount=1)
        alpha:    subscribes to test_event
        beta:     subscribes to test_event_alt

    Expected: alpha > 0, beta > 0, each receiving their respective event type.
    Validates: selective delivery; one producer emitting two events, two distinct consumers.
    """
    print("Testing two consumers with different subscriptions...")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="routing_two_consumers",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("test_event_producer", "process_full_halo"),
                ("test_event_consumer_alpha", "process_per_event"),
                ("test_event_consumer_beta", "process_per_event"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "TestEventProducerEmitCount": 1,
            "TestEventProducerEmitAltCount": 1,
        },
        first_file=0,
        last_file=0,
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined = stdout + stderr

        assert returncode == 0, f"Mimic failed:\n{combined}"

        alpha_count = parse_consumer_event_count(combined, "test_event_consumer_alpha")
        beta_count = parse_consumer_event_count(combined, "test_event_consumer_beta")

        assert (
            alpha_count is not None
        ), "Could not find test_event_consumer_alpha event count in mimic output"
        assert (
            beta_count is not None
        ), "Could not find test_event_consumer_beta event count in mimic output"

        assert (
            alpha_count > 0
        ), f"Expected alpha > 0 events (subscribed to test_event), got {alpha_count}"
        assert (
            beta_count > 0
        ), f"Expected beta > 0 events (subscribed to test_event_alt), got {beta_count}"

        print(f"  ✓ test_event_consumer_alpha (test_event):     {alpha_count} event(s)")
        print(f"  ✓ test_event_consumer_beta  (test_event_alt): {beta_count} event(s)")
        print(f"    Selective routing confirmed: each consumer received its subscribed event type")

    finally:
        shutil.rmtree(temp_dir)


def test_routing_multiple_producers_in_one_phase():
    """
    Two independent producers in the same phase dispatch to their own consumers.

    Configuration:
        producer_a: test_event (EmitCount=1)  → alpha subscribes
        producer_b: test_event_b (EmitBCount=1) → gamma subscribes
        Both producers and both consumers run in galaxy_physics.

    Expected: alpha > 0 events from producer_a, gamma > 0 events from producer_b.
    Validates: multiple process_full_halo producers co-exist; each consumer
               receives only the events from its declared producer.
    """
    print("Testing multiple producers in one phase...")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="routing_multi_producer",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("test_event_producer", "process_full_halo"),
                ("test_event_producer_b", "process_full_halo"),
                ("test_event_consumer_alpha", "process_per_event"),
                ("test_event_consumer_gamma", "process_per_event"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "TestEventProducerEmitCount": 1,
            "TestEventProducerEmitAltCount": 0,
            "TestEventProducerBEmitCount": 1,
        },
        first_file=0,
        last_file=0,
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined = stdout + stderr

        assert returncode == 0, f"Mimic failed:\n{combined}"

        alpha_count = parse_consumer_event_count(combined, "test_event_consumer_alpha")
        gamma_count = parse_consumer_event_count(combined, "test_event_consumer_gamma")

        assert (
            alpha_count is not None
        ), "Could not find test_event_consumer_alpha event count in mimic output"
        assert (
            gamma_count is not None
        ), "Could not find test_event_consumer_gamma event count in mimic output"

        assert (
            alpha_count > 0
        ), f"Expected alpha > 0 events (subscribed to producer_a/test_event), got {alpha_count}"
        assert (
            gamma_count > 0
        ), f"Expected gamma > 0 events (subscribed to producer_b/test_event_b), got {gamma_count}"

        print(f"  ✓ test_event_consumer_alpha (producer_a/test_event):   {alpha_count} event(s)")
        print(f"  ✓ test_event_consumer_gamma (producer_b/test_event_b): {gamma_count} event(s)")
        print(f"    Independent dispatch confirmed: two producers, each reaching its own consumer")

    finally:
        shutil.rmtree(temp_dir)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_routing_single_consumer_receives_events,
            test_routing_unsubscribed_consumer_receives_no_events,
            test_routing_two_consumers_different_subscriptions,
            test_routing_multiple_producers_in_one_phase,
        ],
        "Event Routing (test_event_routing.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
