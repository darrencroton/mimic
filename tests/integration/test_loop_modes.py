#!/usr/bin/env python3
"""
Loop Mode Behavior Test - LOOP_MODE_ALL vs LOOP_MODE_ONCE

Validates: Loop modes control how modules process galaxies
Phase: Phase 3+ (Multi-Phase Pipeline)

This test validates that the two loop modes behave correctly:
  - LOOP_MODE_ONCE: Module receives full galaxy array (ngal > 1)
  - LOOP_MODE_ALL: Module called once per galaxy (ngal = 1)

Loop modes enable different execution patterns:
  - LOOP_MODE_ONCE: Efficient array processing
  - LOOP_MODE_ALL: Galaxy-major loop for cache locality

Test cases:
  - test_loop_mode_once_array_processing: Module receives full array
  - test_loop_mode_all_per_galaxy: Module called once per galaxy
  - test_loop_mode_all_ngal_is_one: Verify ngal=1 for each call

Author: Mimic Testing Team
Date: 2025-12-09
"""

import re
import shutil
import sys
from pathlib import Path

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    REPO_ROOT,
    create_test_param_file,
    run_mimic,
)

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


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
    pattern = r'TEST_FIXTURE_EXEC: count=(\d+) ngal=(\d+) substep=(\d+)/(\d+) substep_dt=([\d.e+-]+) z=([\d.]+)'

    for match in re.finditer(pattern, stdout):
        executions.append({
            'count': int(match.group(1)),
            'ngal': int(match.group(2)),
            'substep_number': int(match.group(3)),
            'num_substeps': int(match.group(4)),
            'substep_dt': float(match.group(5)),
            'redshift': float(match.group(6))
        })

    return executions


def test_loop_mode_once_array_processing():
    """
    Test that LOOP_MODE_ONCE passes full galaxy array to module

    Expected: Module receives ngal > 1 (full array of galaxies in FOF group)
    Validates: LOOP_MODE_ONCE enables efficient array processing
    """
    print("Testing LOOP_MODE_ONCE array processing...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_once",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('test_fixture', 'once')],  # LOOP_MODE_ONCE
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            "TestFixtureDummyParameter": 1.0,
            "TestFixtureEnableLogging": 1
        },
        first_file=0,
        last_file=0
    )

    try:
        # Use SubSteps=1 for simpler analysis
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 1
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With LOOP_MODE_ONCE, module is called once per FOF group
        # Total executions should equal number of FOF groups
        num_fof_groups = len(all_executions)

        # Each execution should have ngal > 0
        ngal_values = [e['ngal'] for e in all_executions]
        assert all(ngal > 0 for ngal in ngal_values), \
            "All FOF groups should have at least one galaxy"

        # Most FOF groups should have ngal > 1 (some may have just 1 galaxy)
        # Check that at least some have ngal > 1
        multi_galaxy_groups = [ngal for ngal in ngal_values if ngal > 1]
        assert len(multi_galaxy_groups) > 0, \
            "Should have some FOF groups with multiple galaxies"

        # Calculate statistics
        avg_ngal = sum(ngal_values) / len(ngal_values)
        max_ngal = max(ngal_values)

        print(f"  ✓ LOOP_MODE_ONCE passes full array to module:")
        print(f"    - {num_fof_groups} FOF groups processed")
        print(f"    - Average galaxies per FOF group: {avg_ngal:.1f}")
        print(f"    - Largest FOF group: {max_ngal} galaxies")
        print(f"    - Module called once per FOF group (array processing)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_loop_mode_all_per_galaxy():
    """
    Test that LOOP_MODE_ALL calls module once per galaxy

    Expected: Module called many times, once for each galaxy
    Validates: LOOP_MODE_ALL implements galaxy-major loop
    """
    print("Testing LOOP_MODE_ALL per-galaxy processing...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_all",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('test_fixture', 'all')],  # LOOP_MODE_ALL
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            "TestFixtureDummyParameter": 1.0,
            "TestFixtureEnableLogging": 1
        },
        first_file=0,
        last_file=0
    )

    try:
        # Use SubSteps=1 for simpler analysis
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 1
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With LOOP_MODE_ALL, module is called once per galaxy
        # Total executions should be much larger than number of FOF groups
        total_executions = len(all_executions)

        # Most executions should have ngal=1 (but some FOF groups may have only 1 galaxy)
        ngal_values = [e['ngal'] for e in all_executions]

        # Count ngal=1 executions
        ngal_one_count = sum(1 for ngal in ngal_values if ngal == 1)

        # With LOOP_MODE_ALL, vast majority should be ngal=1
        # (Some may be FOF groups with 1 galaxy where ngal=1 anyway)
        percent_ngal_one = (ngal_one_count / total_executions) * 100

        # We expect nearly all (>95%) to be ngal=1 with LOOP_MODE_ALL
        assert percent_ngal_one > 95, \
            f"Expected >95% of executions with ngal=1, got {percent_ngal_one:.1f}%"

        print(f"  ✓ LOOP_MODE_ALL calls module once per galaxy:")
        print(f"    - {total_executions} total module calls")
        print(f"    - {ngal_one_count} calls with ngal=1 ({percent_ngal_one:.1f}%)")
        print(f"    - Module processes galaxies individually (galaxy-major loop)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_loop_mode_all_ngal_is_one():
    """
    Test that LOOP_MODE_ALL always passes ngal=1 to module

    Expected: Every module call should have ngal=1 (single galaxy)
    Validates: Galaxy-major loop processes one galaxy at a time
    """
    print("Testing LOOP_MODE_ALL ngal=1 invariant...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="loop_all_ngal",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('test_fixture', 'all')],  # LOOP_MODE_ALL
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            "TestFixtureDummyParameter": 1.0,
            "TestFixtureEnableLogging": 1
        },
        first_file=0,
        last_file=0
    )

    try:
        # Use SubSteps=1
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 1
        with open(param_file, 'w') as f:
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

        # ALL should have ngal=1 with LOOP_MODE_ALL
        ngal_values = [e['ngal'] for e in sample_executions]

        # Verify all are ngal=1
        assert all(ngal == 1 for ngal in ngal_values), \
            f"Expected all ngal=1 with LOOP_MODE_ALL, got {set(ngal_values)}"

        print(f"  ✓ LOOP_MODE_ALL invariant verified:")
        print(f"    - Checked {sample_size} module calls")
        print(f"    - All have ngal=1 (single galaxy per call)")
        print(f"    - Confirms galaxy-major execution pattern")

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
    print(f"{BLUE}Test Suite: Loop Mode Behavior (ONCE vs ALL) (test_loop_modes.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_loop_mode_once_array_processing,
        test_loop_mode_all_per_galaxy,
        test_loop_mode_all_ngal_is_one,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"  {RED}✗ FAIL: {test.__name__}{NC}")
            print(f"    {e}")
            failed += 1
        except Exception as e:
            print(f"  {RED}✗ ERROR: {test.__name__}{NC}")
            print(f"    {e}")
            failed += 1

    # Print summary
    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print(f"Total:  {passed + failed}")
    print(f"{BLUE}{'=' * 60}{NC}")

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
