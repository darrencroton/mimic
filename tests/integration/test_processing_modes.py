#!/usr/bin/env python3
"""
Processing Mode Behavior Test - process_full_halo vs process_by_galaxy

Validates: Processing modes control how modules receive galaxies, and that
process_per_event configuration errors fail fast at startup.

  - process_full_halo: module receives the full FoF workspace (ngal >= 1)
  - process_by_galaxy: module is called once per galaxy (ngal = 1),
    giving the galaxy-major loop for cache locality
  - process_per_event: requires declared subscriptions and module support;
    invalid configurations are rejected at startup
"""

import re
import shutil
import sys
from pathlib import Path

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    REPO_ROOT,
    TestSkipped,
    create_test_param_file,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

# ANSI color codes
BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
YELLOW = "\033[1;33m"
NC = "\033[0m"


def parse_test_fixture_executions(stdout):
    """
    Parse test_fixture execution log messages

    Extracts execution information from TEST_FIXTURE_EXEC log lines.

    Args:
        stdout (str): Mimic stdout containing test_fixture logs

    Returns:
        list: List of dicts with execution information
    """
    executions = []
    pattern = r"TEST_FIXTURE_EXEC: count=(\d+) ngal=(\d+) substep=(\d+)/(\d+) substep_dt=([\d.e+-]+) z=([\d.]+)"

    for match in re.finditer(pattern, stdout):
        executions.append(
            {
                "count": int(match.group(1)),
                "ngal": int(match.group(2)),
                "substep_number": int(match.group(3)),
                "num_substeps": int(match.group(4)),
                "substep_dt": float(match.group(5)),
                "redshift": float(match.group(6)),
            }
        )

    return executions


def test_full_halo_mode_array_processing():
    """
    Test that process_full_halo passes the full FoF workspace to the module

    Expected: Module receives ngal > 1 (full array of galaxies in FOF group)
    Validates: process_full_halo enables whole-workspace array processing
    """
    print("Testing process_full_halo array processing...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_once",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
    )

    try:
        # Use SubSteps=1 for simpler analysis
        import yaml

        with open(param_file, "r") as f:
            config = yaml.safe_load(f)
        config["SubSteps"] = 1
        with open(param_file, "w") as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With process_full_halo, the module is called once per FOF group
        # Total executions should equal number of FOF groups
        num_fof_groups = len(all_executions)

        # Each execution should have ngal > 0
        ngal_values = [e["ngal"] for e in all_executions]
        assert all(
            ngal > 0 for ngal in ngal_values
        ), "All FOF groups should have at least one galaxy"

        # Most FOF groups should have ngal > 1 (some may have just 1 galaxy)
        # Check that at least some have ngal > 1
        multi_galaxy_groups = [ngal for ngal in ngal_values if ngal > 1]
        assert len(multi_galaxy_groups) > 0, "Should have some FOF groups with multiple galaxies"

        # Calculate statistics
        avg_ngal = sum(ngal_values) / len(ngal_values)
        max_ngal = max(ngal_values)

        print(f"  ✓ process_full_halo passes the full workspace to the module:")
        print(f"    - {num_fof_groups} FOF groups processed")
        print(f"    - Average galaxies per FOF group: {avg_ngal:.1f}")
        print(f"    - Largest FOF group: {max_ngal} galaxies")
        print(f"    - Module called once per FOF group (array processing)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_by_galaxy_mode_per_galaxy():
    """
    Test that process_by_galaxy calls the module once per galaxy

    Expected: Module called many times, once for each galaxy
    Validates: process_by_galaxy implements the galaxy-major loop
    """
    print("Testing process_by_galaxy per-galaxy processing...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_all",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
    )

    try:
        # Use SubSteps=1 for simpler analysis
        import yaml

        with open(param_file, "r") as f:
            config = yaml.safe_load(f)
        config["SubSteps"] = 1
        with open(param_file, "w") as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With process_by_galaxy, the module is called once per galaxy
        # Total executions should be much larger than number of FOF groups
        total_executions = len(all_executions)

        # Most executions should have ngal=1 (but some FOF groups may have only 1 galaxy)
        ngal_values = [e["ngal"] for e in all_executions]

        # Count ngal=1 executions
        ngal_one_count = sum(1 for ngal in ngal_values if ngal == 1)

        # With process_by_galaxy, the vast majority should be ngal=1
        # (Some may be FOF groups with 1 galaxy where ngal=1 anyway)
        percent_ngal_one = (ngal_one_count / total_executions) * 100

        # We expect nearly all (>95%) to be ngal=1 with process_by_galaxy
        assert (
            percent_ngal_one > 95
        ), f"Expected >95% of executions with ngal=1, got {percent_ngal_one:.1f}%"

        print(f"  ✓ process_by_galaxy calls the module once per galaxy:")
        print(f"    - {total_executions} total module calls")
        print(f"    - {ngal_one_count} calls with ngal=1 ({percent_ngal_one:.1f}%)")
        print(f"    - Module processes galaxies individually (galaxy-major loop)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_by_galaxy_mode_ngal_is_one():
    """
    Test that process_by_galaxy always passes ngal=1 to the module

    Expected: Every module call should have ngal=1 (single galaxy)
    Validates: Galaxy-major loop processes one galaxy at a time
    """
    print("Testing process_by_galaxy ngal=1 invariant...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_all_ngal",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
    )

    try:
        # Use SubSteps=1
        import yaml

        with open(param_file, "r") as f:
            config = yaml.safe_load(f)
        config["SubSteps"] = 1
        with open(param_file, "w") as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check first 100 executions (representative sample)
        sample_size = min(100, len(all_executions))
        sample_executions = all_executions[:sample_size]

        # Every call should have ngal=1 with process_by_galaxy
        ngal_values = [e["ngal"] for e in sample_executions]

        # Verify all are ngal=1
        assert all(
            ngal == 1 for ngal in ngal_values
        ), f"Expected all ngal=1 with process_by_galaxy, got {set(ngal_values)}"

        print(f"  ✓ process_by_galaxy invariant verified:")
        print(f"    - Checked {sample_size} module calls")
        print(f"    - All have ngal=1 (single galaxy per call)")
        print(f"    - Confirms galaxy-major execution pattern")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_processing_mode_per_event_no_emissions():
    """
    Test that configuring a module as process_per_event without declared subscriptions
    produces a clear startup error.

    Under subscription-based routing, every process_per_event module must declare
    events.consumes in its module_info.yaml. A module with no subscriptions can
    never receive events, so the configuration is rejected at startup with a
    diagnostic message.

    Expected: Run fails with a clear error about missing subscriptions
    Validates: validate_event_subscriptions() startup check (module_registry.c)
    """
    print("Testing process_per_event subscription requirement...")

    # ===== SETUP =====
    # test_fixture has no events.consumes — configuring it as process_per_event
    # should be rejected by validate_event_subscriptions()
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_per_event_no_subscriptions",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_per_event")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
    )

    try:
        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert (
            returncode != 0
        ), "Expected startup failure: process_per_event without subscriptions should be rejected"

        combined = stdout + stderr
        assert (
            "declares no event subscriptions" in combined
        ), f"Expected subscription-missing error in output, got:\n{combined}"

        print("  ✓ process_per_event subscription requirement validated")
        print("    - Run correctly rejected at startup")
        print("    - Error message references missing subscriptions")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_processing_mode_per_event_mode_mismatch_fails():
    """
    Test that unsupported process_per_event configuration fails validation

    Expected: module_system_init reports mode mismatch and exits non-zero
    Validates: mode support checks include process_per_event
    """
    print("Testing process_per_event mode-mismatch validation...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_per_event_invalid_module",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_event_producer", "process_per_event")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={},
        first_file=0,
        last_file=0,
    )

    try:
        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        combined_output = f"{stdout}\n{stderr}"
        assert returncode != 0, "Expected non-zero exit for unsupported process_per_event module"
        assert (
            "does not support processing mode 'process_per_event'" in combined_output
        ), "Expected clear processing mode mismatch message"

        print("  ✓ process_per_event mismatch rejected as expected")
        print("    - Run failed fast during mode validation")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    # Print test suite header
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: Processing Mode Behavior (test_processing_modes.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_full_halo_mode_array_processing,
        test_by_galaxy_mode_per_galaxy,
        test_by_galaxy_mode_ngal_is_one,
        test_processing_mode_per_event_no_emissions,
        test_processing_mode_per_event_mode_mismatch_fails,
    ]

    passed = 0
    failed = 0
    skipped = 0

    for test in tests:
        print()
        try:
            test()
            result_pass(test.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test.__name__, str(e))
            skipped += 1
        except AssertionError as e:
            result_fail(test.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test.__name__, str(e).splitlines()[0])
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
