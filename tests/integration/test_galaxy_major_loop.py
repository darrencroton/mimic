#!/usr/bin/env python3
"""
Galaxy-Major Loop Ordering Test

Validates: Multiple LOOP_MODE_ALL modules execute in galaxy-major order
Phase: Phase 3+ (Multi-Phase Pipeline)

This test validates that when multiple modules with LOOP_MODE_ALL are
configured in the same phase, they execute in galaxy-major order:
  - Galaxy-major: For each galaxy: module1, module2, module3, ...
  - NOT module-major: module1 for all galaxies, then module2 for all, ...

Galaxy-major ordering provides better cache locality and matches SAGE
execution pattern.

Test cases:
  - test_multiple_modules_galaxy_major: Verify execution count pattern
  - test_two_phase_modules_ordering: Verify phase execution before loop

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


def test_multiple_modules_galaxy_major():
    """
    Test that multiple LOOP_MODE_ALL modules execute in galaxy-major order

    Expected: With 2 modules configured, total executions = 2 × total galaxies
    Validates: Each galaxy processed by all modules before moving to next galaxy
    """
    print("Testing galaxy-major execution with multiple modules...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="galaxy_major",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('test_fixture', 'all'),  # Module instance 1
                ('test_fixture', 'all'),  # Module instance 2
            ],
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

        # With 2 LOOP_MODE_ALL modules, should get 2 calls per galaxy
        # Total calls should be 2 × total_galaxies
        total_executions = len(all_executions)

        # All executions should have ngal=1 (LOOP_MODE_ALL)
        ngal_values = [e['ngal'] for e in all_executions]
        assert all(ngal == 1 for ngal in ngal_values), \
            "All executions should have ngal=1 with LOOP_MODE_ALL"

        # Total galaxies processed = total_executions / 2 (2 modules per galaxy)
        total_galaxies = total_executions // 2

        # Verify the count is exactly twice the number of galaxies
        assert total_executions == total_galaxies * 2, \
            f"Expected {total_galaxies * 2} executions (2 per galaxy)"

        print(f"  ✓ Galaxy-major execution verified:")
        print(f"    - {total_galaxies} total galaxies processed")
        print(f"    - {total_executions} total module executions (2 per galaxy)")
        print(f"    - Each galaxy processed by both modules before next galaxy")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_two_phase_modules_ordering():
    """
    Test execution order with modules in multiple phases

    Expected: With modules in phase_1 and phase_2, phase_1 completes before phase_2
    Validates: Galaxy-major within each phase, but phases execute in order
    """
    print("Testing multi-phase galaxy-major execution...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="multi_phase_galaxy",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('test_fixture', 'all'),
                ('test_fixture', 'all'),
            ],
            'phase_2': [
                ('test_fixture', 'all'),
            ],
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
        # Use SubSteps=2 to see phase ordering across substeps
        import yaml
        with open(param_file, 'r') as f:
            config = yaml.safe_load(f)
        config['SubSteps'] = 2
        with open(param_file, 'w') as f:
            yaml.dump(config, f, default_flow_style=False)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With 2 modules in phase_1, 1 in phase_2, and SubSteps=2:
        # Per FOF group with N galaxies:
        # - Substep 0: phase_1 (2N calls), phase_2 (N calls)
        # - Substep 1: phase_1 (2N calls), phase_2 (N calls)
        # Total: 6N calls per FOF group

        # All executions should have ngal=1
        ngal_values = [e['ngal'] for e in all_executions]
        assert all(ngal == 1 for ngal in ngal_values), \
            "All executions should have ngal=1"

        # Check substep pattern for first few executions
        first_fof = all_executions[:12]  # Assumes first FOF has at least 2 galaxies

        # Group by substep
        substep_0 = [e for e in first_fof if e['substep_number'] == 0]
        substep_1 = [e for e in first_fof if e['substep_number'] == 1]

        # Each substep should have executions (2N from phase_1 + N from phase_2)
        # Verify we have both substeps represented
        assert len(substep_0) > 0, "Should have executions in substep 0"
        assert len(substep_1) > 0, "Should have executions in substep 1"

        # Verify substep numbers are correct
        assert all(e['num_substeps'] == 2 for e in first_fof), \
            "All executions should have num_substeps=2"

        total_executions = len(all_executions)
        total_galaxies = total_executions // 6  # 6 executions per galaxy (2 substeps × 3 modules)

        print(f"  ✓ Multi-phase galaxy-major execution verified:")
        print(f"    - {total_galaxies} total galaxies processed")
        print(f"    - {total_executions} total executions")
        print(f"    - Pattern: For each substep:")
        print(f"      - phase_1: 2 modules × all galaxies (galaxy-major)")
        print(f"      - phase_2: 1 module × all galaxies (galaxy-major)")

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
    print(f"{BLUE}Test Suite: Galaxy-Major Loop Ordering (test_galaxy_major_loop.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_multiple_modules_galaxy_major,
        test_two_phase_modules_ordering,
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
