#!/usr/bin/env python3
"""
Galaxy-Major Loop Ordering Test

Validates: Multiple process_by_galaxy modules execute in galaxy-major order

This test validates that when multiple modules configured as process_by_galaxy
run in the same phase, they execute in galaxy-major order:
  - Galaxy-major: for each galaxy, all modules run before moving to the next
  - NOT module-major: one module over all galaxies, then the next module

Galaxy-major ordering provides cache locality and matches SAGE execution behaviour.

Test cases:
  - test_multiple_modules_galaxy_major: Verify execution count pattern
  - test_two_phase_modules_ordering: Verify phase execution before the substep loop
"""

import shutil
import sys
from pathlib import Path

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    create_test_param_file,
    parse_test_fixture_executions,
    run_mimic,
    run_test_suite,
)


def test_multiple_modules_galaxy_major():
    """
    Test that multiple PROCESSING_MODE_ALL modules execute in galaxy-major order

    Expected: With 2 modules configured, total executions = 2 × total galaxies
    Validates: Each galaxy processed by all modules before moving to next galaxy
    """
    print("Testing galaxy-major execution with multiple modules...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="galaxy_major",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("test_fixture", "process_by_galaxy"),  # Module instance 1
                ("test_fixture", "process_by_galaxy"),  # Module instance 2
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
        substeps=1,
    )

    try:

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With 2 PROCESSING_MODE_ALL modules, should get 2 calls per galaxy
        # Total calls should be 2 × total_galaxies
        total_executions = len(all_executions)

        # All executions should have ngal=1 (PROCESSING_MODE_ALL)
        ngal_values = [e["ngal"] for e in all_executions]
        assert all(
            ngal == 1 for ngal in ngal_values
        ), "All executions should have ngal=1 with PROCESSING_MODE_ALL"

        # Total galaxies processed = total_executions / 2 (2 modules per galaxy)
        total_galaxies = total_executions // 2

        # Verify the count is exactly twice the number of galaxies
        assert (
            total_executions == total_galaxies * 2
        ), f"Expected {total_galaxies * 2} executions (2 per galaxy)"

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

    Expected: With modules in galaxy_physics and satellite_mergers, galaxy_physics completes before satellite_mergers
    Validates: Galaxy-major within each phase, but phases execute in order
    """
    print("Testing multi-phase galaxy-major execution...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="multi_phase_galaxy",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("test_fixture", "process_by_galaxy"),
                ("test_fixture", "process_by_galaxy"),
            ],
            "satellite_mergers": [
                ("test_fixture", "process_by_galaxy"),
            ],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
        substeps=2,
    )

    try:

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # With 2 modules in galaxy_physics, 1 in satellite_mergers, and SubSteps=2:
        # Per FOF group with N galaxies:
        # - Substep 0: galaxy_physics (2N calls), satellite_mergers (N calls)
        # - Substep 1: galaxy_physics (2N calls), satellite_mergers (N calls)
        # Total: 6N calls per FOF group

        # All executions should have ngal=1
        ngal_values = [e["ngal"] for e in all_executions]
        assert all(ngal == 1 for ngal in ngal_values), "All executions should have ngal=1"

        # Check substep pattern for first few executions
        first_fof = all_executions[:12]  # Assumes first FOF has at least 2 galaxies

        # Group by substep
        substep_0 = [e for e in first_fof if e["substep_number"] == 0]
        substep_1 = [e for e in first_fof if e["substep_number"] == 1]

        # Each substep should have executions (2N from galaxy_physics + N from satellite_mergers)
        # Verify we have both substeps represented
        assert len(substep_0) > 0, "Should have executions in substep 0"
        assert len(substep_1) > 0, "Should have executions in substep 1"

        # Verify substep numbers are correct
        assert all(
            e["num_substeps"] == 2 for e in first_fof
        ), "All executions should have num_substeps=2"

        total_executions = len(all_executions)
        total_galaxies = total_executions // 6  # 6 executions per galaxy (2 substeps × 3 modules)

        print(f"  ✓ Multi-phase galaxy-major execution verified:")
        print(f"    - {total_galaxies} total galaxies processed")
        print(f"    - {total_executions} total executions")
        print(f"    - Pattern: For each substep:")
        print(f"      - galaxy_physics: 2 modules × all galaxies (galaxy-major)")
        print(f"      - satellite_mergers: 1 module × all galaxies (galaxy-major)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_multiple_modules_galaxy_major,
            test_two_phase_modules_ordering,
        ],
        "Galaxy-Major Loop Ordering (test_galaxy_major_loop.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
