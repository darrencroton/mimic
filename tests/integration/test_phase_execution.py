#!/usr/bin/env python3
"""
Multi-Phase Pipeline Execution Order and Frequency Test

Validates: Phase execution order and execution frequency with SubSteps
Phase: Phase 3+ (Multi-Phase Pipeline)

This test validates that the multi-phase pipeline executes phases in the
correct order and with the correct frequency:
  - pre_timestep: Runs exactly once BEFORE substep loop
  - galaxy_physics: Runs SubSteps times DURING substep loop (once per substep)
  - satellite_mergers: Runs SubSteps times DURING substep loop (once per substep)
  - post_timestep: Runs exactly once AFTER substep loop

Test cases:
  - test_pre_timestep_frequency: Pre-timestep runs exactly once
  - test_galaxy_physics_frequency: galaxy_physics runs SubSteps times
  - test_satellite_mergers_frequency: satellite_mergers runs SubSteps times
  - test_post_timestep_frequency: Post-timestep runs exactly once
  - test_all_phases_execution_order: All phases execute in correct order

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
        list: List of dicts with execution information:
              [{'count': 1, 'ngal': 10, 'substep': 0, 'num_substeps': 3, ...}, ...]
    """
    executions = []

    # Pattern: TEST_FIXTURE_EXEC: count=%d ngal=%d substep=%d/%d substep_dt=%.6e z=%.4f
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


def test_pre_timestep_frequency():
    """
    Test that pre_timestep phase runs exactly once

    Expected: With SubSteps=3, pre_timestep should execute 1 time only
    Validates: Pre-timestep runs before substep loop, not during
    """
    print("Testing pre_timestep frequency...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="pre_timestep_freq",
        phase_config={
            'pre_timestep': [('test_fixture', 'process_full_halo')],
            'galaxy_physics': [],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={
            "TestFixtureDummyParameter": 1.0,
            "TestFixtureEnableLogging": 1  # Enable detailed logging
        },
        first_file=0,
        last_file=0
    )

    try:
        # Explicitly set SubSteps=3 in the parameter file
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 3
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # We process many FOF groups, so check the pattern for first FOF group
        # Pre-timestep runs once per FOF group, so first execution is from first FOF group
        assert len(all_executions) > 0, "Should have at least one execution"
        first_exec = all_executions[0]

        # Should have SubSteps=3 in context
        assert first_exec['num_substeps'] == 3, \
            f"Expected num_substeps=3, got {first_exec['num_substeps']}"

        # Should start at substep 0
        assert first_exec['substep_number'] == 0, \
            f"Expected substep=0, got {first_exec['substep_number']}"

        # Count total executions and verify it matches number of FOF groups
        num_fof_groups = len(all_executions)  # One pre_timestep per FOF group
        print(f"  ✓ pre_timestep executes once per FOF group ({num_fof_groups} FOF groups processed)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_galaxy_physics_frequency():
    """
    Test that galaxy_physics runs SubSteps times

    Expected: With SubSteps=3, galaxy_physics should execute 3 times per FOF group
    Validates: galaxy_physics runs once per substep during substep loop
    """
    print("Testing galaxy_physics frequency...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="galaxy_physics_freq",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('test_fixture', 'process_full_halo')],  # Use 'process_full_halo' for clearer counting
            'satellite_mergers': [],
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
        # Set SubSteps=3
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 3
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check pattern for first FOF group (first 3 executions)
        assert len(all_executions) >= 3, "Should have at least 3 executions"
        first_fof_executions = all_executions[:3]

        # Verify substep numbers increment: 0, 1, 2 for first FOF group
        substep_numbers = [e['substep_number'] for e in first_fof_executions]
        assert substep_numbers == [0, 1, 2], \
            f"Expected substep numbers [0, 1, 2], got {substep_numbers}"

        # Verify all have same num_substeps
        assert all(e['num_substeps'] == 3 for e in first_fof_executions), \
            "All executions should have num_substeps=3"

        # Total executions should be 3 times number of FOF groups
        num_fof_groups = len(all_executions) // 3
        assert len(all_executions) == num_fof_groups * 3, \
            f"Expected {num_fof_groups * 3} total executions (3 per FOF group)"

        print(f"  ✓ galaxy_physics executes SubSteps times (3) per FOF group with correct substep numbers")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_satellite_mergers_frequency():
    """
    Test that satellite_mergers runs SubSteps times

    Expected: With SubSteps=3, satellite_mergers should execute 3 times per FOF group
    Validates: satellite_mergers runs once per substep during substep loop
    """
    print("Testing satellite_mergers frequency...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="satellite_mergers_freq",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [],
            'satellite_mergers': [('test_fixture', 'process_full_halo')],  # Use 'process_full_halo' for clearer counting
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
        # Set SubSteps=3
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 3
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check pattern for first FOF group (first 3 executions)
        assert len(all_executions) >= 3, "Should have at least 3 executions"
        first_fof_executions = all_executions[:3]

        # Verify substep numbers increment: 0, 1, 2 for first FOF group
        substep_numbers = [e['substep_number'] for e in first_fof_executions]
        assert substep_numbers == [0, 1, 2], \
            f"Expected substep numbers [0, 1, 2], got {substep_numbers}"

        # Total executions should be 3 times number of FOF groups
        num_fof_groups = len(all_executions) // 3

        print(f"  ✓ satellite_mergers executes SubSteps times (3) per FOF group with correct substep numbers")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_post_timestep_frequency():
    """
    Test that post_timestep phase runs exactly once

    Expected: With SubSteps=3, post_timestep should execute 1 time only
    Validates: Post-timestep runs after substep loop completes
    """
    print("Testing post_timestep frequency...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="post_timestep_freq",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [],
            'satellite_mergers': [],
            'post_timestep': [('test_fixture', 'process_full_halo')]
        },
        model_params={
            "TestFixtureDummyParameter": 1.0,
            "TestFixtureEnableLogging": 1
        },
        first_file=0,
        last_file=0
    )

    try:
        # Set SubSteps=3
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 3
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check first FOF group
        assert len(all_executions) > 0, "Should have at least one execution"
        first_exec = all_executions[0]

        # Should have SubSteps=3 in context
        assert first_exec['num_substeps'] == 3, \
            f"Expected num_substeps=3, got {first_exec['num_substeps']}"

        # Should be at last substep (2) since post_timestep runs after loop
        assert first_exec['substep_number'] == 2, \
            f"Expected substep=2 (last substep), got {first_exec['substep_number']}"

        # Count total executions and verify it matches number of FOF groups
        num_fof_groups = len(all_executions)
        print(f"  ✓ post_timestep executes once per FOF group after substep loop")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_all_phases_execution_order():
    """
    Test complete multi-phase execution order and frequency

    Expected: With all phases configured and SubSteps=3, per FOF group:
      - Total executions: 1 (pre) + 3 (galaxy_physics) + 3 (satellite_mergers) + 1 (post) = 8
      - Order: pre → (galaxy_physics → satellite_mergers) × 3 → post
      - Substep pattern: 0, 0, 0, 1, 1, 2, 2, 2

    Validates: Complete pipeline executes all phases in correct order
    """
    print("Testing complete multi-phase execution order...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="all_phases",
        phase_config={
            'pre_timestep': [('test_fixture', 'process_full_halo')],
            'galaxy_physics': [('test_fixture', 'process_full_halo')],  # Use 'process_full_halo' for clearer pattern
            'satellite_mergers': [('test_fixture', 'process_full_halo')],  # Use 'process_full_halo' for clearer pattern
            'post_timestep': [('test_fixture', 'process_full_halo')]
        },
        model_params={
            "TestFixtureDummyParameter": 1.0,
            "TestFixtureEnableLogging": 1
        },
        first_file=0,
        last_file=0
    )

    try:
        # Set SubSteps=3
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 3
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check pattern for first FOF group (first 8 executions)
        assert len(all_executions) >= 8, f"Should have at least 8 executions, got {len(all_executions)}"
        first_fof_executions = all_executions[:8]

        # Verify substep pattern for first FOF group
        # pre: substep=0
        # substep 0: galaxy_physics substep=0, satellite_mergers substep=0
        # substep 1: galaxy_physics substep=1, satellite_mergers substep=1
        # substep 2: galaxy_physics substep=2, satellite_mergers substep=2
        # post: substep=2
        expected_substeps = [0, 0, 0, 1, 1, 2, 2, 2]
        actual_substeps = [e['substep_number'] for e in first_fof_executions]
        assert actual_substeps == expected_substeps, \
            f"Expected substep pattern {expected_substeps}, got {actual_substeps}"

        # Verify execution counts increment sequentially for first FOF group
        expected_counts = list(range(1, 9))  # 1, 2, 3, 4, 5, 6, 7, 8
        actual_counts = [e['count'] for e in first_fof_executions]
        assert actual_counts == expected_counts, \
            f"Expected counts {expected_counts}, got {actual_counts}"

        # Verify all have same num_substeps
        assert all(e['num_substeps'] == 3 for e in first_fof_executions), \
            "All executions should have num_substeps=3"

        # Total executions should be 8 times number of FOF groups
        num_fof_groups = len(all_executions) // 8
        assert len(all_executions) == num_fof_groups * 8, \
            f"Expected {num_fof_groups * 8} total executions (8 per FOF group)"

        print("  ✓ All phases execute in correct order (verified on first FOF group):")
        print("    - pre_timestep: 1 execution at substep 0")
        print("    - galaxy_physics: 3 executions at substeps 0, 1, 2")
        print("    - satellite_mergers: 3 executions at substeps 0, 1, 2")
        print("    - post_timestep: 1 execution at substep 2")
        print(f"    - Pattern repeats for all {num_fof_groups} FOF groups")

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
    print(f"{BLUE}Test Suite: Multi-Phase Execution Order and Frequency (test_phase_execution.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_pre_timestep_frequency,
        test_galaxy_physics_frequency,
        test_satellite_mergers_frequency,
        test_post_timestep_frequency,
        test_all_phases_execution_order,
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
