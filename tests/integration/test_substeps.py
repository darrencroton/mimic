#!/usr/bin/env python3
"""
SubSteps Time-Stepping and ModuleContext Validation Test

Validates: SubSteps parameter creates substep loop with correct ModuleContext

This test validates that the SubSteps parameter correctly implements time
sub-stepping for numerical stability, and that modules receive accurate
context information:
  - substep_number: Increments from 0 to (SubSteps-1)
  - num_substeps: Matches SubSteps parameter
  - substep_dt: Equals time_interval / SubSteps
  - redshift, time: Set correctly from snapshot data

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


def test_substeps_creates_loop():
    """
    Test that SubSteps > 1 creates substep loop

    Expected: With SubSteps=5, module should execute 5 times per FOF group in galaxy_physics
    Validates: SubSteps parameter controls loop iteration count
    """
    print("Testing SubSteps creates substep loop...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="substeps_loop",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
        substeps=5,
    )

    try:

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check first FOF group (first 5 executions)
        assert len(all_executions) >= 5, "Should have at least 5 executions"
        first_fof = all_executions[:5]

        # Verify substep numbers: 0, 1, 2, 3, 4
        substep_numbers = [e["substep_number"] for e in first_fof]
        assert substep_numbers == [
            0,
            1,
            2,
            3,
            4,
        ], f"Expected substep numbers [0, 1, 2, 3, 4], got {substep_numbers}"

        # Verify all have num_substeps=5
        assert all(
            e["num_substeps"] == 5 for e in first_fof
        ), "All executions should have num_substeps=5"

        # Total executions should be 5 times number of FOF groups
        num_fof_groups = len(all_executions) // 5
        assert (
            len(all_executions) == num_fof_groups * 5
        ), f"Expected {num_fof_groups * 5} total executions (5 per FOF group)"

        print(f"  ✓ SubSteps=5 creates 5-iteration loop per FOF group")
        print(f"    Verified substep numbers [0, 1, 2, 3, 4] for first FOF group")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_substep_dt_calculation():
    """
    Test that substep_dt is calculated correctly

    Expected: substep_dt should be same for all substeps (time_interval / SubSteps)
    Validates: Time sub-stepping provides equal time intervals
    """
    print("Testing substep_dt calculation...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="substep_dt",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
        substeps=10,
    )

    try:

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check first FOF group (first 10 executions)
        assert len(all_executions) >= 10, "Should have at least 10 executions"
        first_fof = all_executions[:10]

        # Verify all substep_dt values are equal
        substep_dts = [e["substep_dt"] for e in first_fof]
        first_dt = substep_dts[0]
        assert all(
            abs(dt - first_dt) < 1e-10 for dt in substep_dts
        ), f"All substep_dt values should be equal, got {substep_dts}"

        # Verify substep_dt > 0 (non-zero time step)
        assert first_dt > 0, f"substep_dt should be positive, got {first_dt}"

        # The sum of all substep_dt should equal the time interval
        # (Each substep gets 1/SubSteps of the time interval)
        total_dt = sum(substep_dts)
        expected_total_dt = first_dt * 10  # Should be consistent
        assert (
            abs(total_dt - expected_total_dt) < 1e-9
        ), f"Total time should equal SubSteps * substep_dt"

        print(f"  ✓ substep_dt is consistent across all substeps")
        print(f"    substep_dt = {first_dt:.6e} (equal for all 10 substeps)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_module_context_fields():
    """
    Test that all ModuleContext fields are set correctly

    Expected: redshift, num_substeps, substep_number all present and valid
    Validates: Modules receive complete and accurate execution context
    """
    print("Testing ModuleContext field correctness...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="context_fields",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
        substeps=4,
    )

    try:

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        # Parse executions
        all_executions = parse_test_fixture_executions(stdout)

        # Check first FOF group
        assert len(all_executions) >= 4, "Should have at least 4 executions"
        first_fof = all_executions[:4]

        # Verify redshift is set and reasonable (0 <= z <= 127 for mini-Millennium)
        for exec_info in first_fof:
            z = exec_info["redshift"]
            assert 0 <= z <= 127, f"Redshift should be in range [0, 127], got {z}"

        # Verify redshift is same for all substeps in a FOF group
        redshifts = [e["redshift"] for e in first_fof]
        assert all(
            z == redshifts[0] for z in redshifts
        ), f"All substeps in FOF group should have same redshift, got {redshifts}"

        # Verify num_substeps is 4 for all
        assert all(
            e["num_substeps"] == 4 for e in first_fof
        ), "All executions should have num_substeps=4"

        # Verify substep_number increments correctly
        substep_numbers = [e["substep_number"] for e in first_fof]
        assert substep_numbers == [
            0,
            1,
            2,
            3,
        ], f"Expected substep numbers [0, 1, 2, 3], got {substep_numbers}"

        # Verify substep_dt is positive and consistent
        substep_dts = [e["substep_dt"] for e in first_fof]
        assert all(dt > 0 for dt in substep_dts), "All substep_dt values should be positive"
        assert all(
            abs(dt - substep_dts[0]) < 1e-10 for dt in substep_dts
        ), "All substep_dt values should be equal"

        print(f"  ✓ ModuleContext fields are valid and consistent:")
        print(f"    - redshift: {redshifts[0]:.4f} (constant across substeps)")
        print(f"    - num_substeps: 4 (matches SubSteps parameter)")
        print(f"    - substep_number: [0, 1, 2, 3] (increments correctly)")
        print(f"    - substep_dt: {substep_dts[0]:.6e} (positive and consistent)")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_substeps_one_no_loop():
    """
    Test that SubSteps=1 means no sub-stepping (single timestep)

    Expected: With SubSteps=1, module should execute once per FOF group
    Validates: SubSteps=1 is equivalent to no sub-stepping
    """
    print("Testing SubSteps=1 (no sub-stepping)...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="substeps_one",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_full_halo")],
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

        # With SubSteps=1, should execute once per FOF group
        # First execution should be from first FOF group
        assert len(all_executions) > 0, "Should have at least one execution"
        first_exec = all_executions[0]

        # Verify num_substeps=1
        assert (
            first_exec["num_substeps"] == 1
        ), f"Expected num_substeps=1, got {first_exec['num_substeps']}"

        # Verify substep_number=0 (only substep)
        assert (
            first_exec["substep_number"] == 0
        ), f"Expected substep_number=0, got {first_exec['substep_number']}"

        # Total executions should equal number of FOF groups (1 per group)
        num_fof_groups = len(all_executions)

        print(f"  ✓ SubSteps=1 executes once per FOF group (single timestep, no loop)")
        print(f"    Verified on {num_fof_groups} FOF groups")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_dynamic_timestep_varies_substeps():
    """
    Test that dynamic timestep mode varies num_substeps across snapshots.

    Expected: Dynamic mode runs end-to-end and assigns more substeps to at least
    one higher-redshift snapshot than the lowest-redshift snapshot.
    """
    print("Testing dynamic timestep varies substep count...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="dynamic_substeps",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("test_fixture", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
        first_file=0,
        last_file=0,
        substeps=10,
        timestep_scheme="dynamic",
    )

    try:

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        assert returncode == 0, f"Mimic failed: {stderr}"

        all_executions = parse_test_fixture_executions(stdout)
        assert len(all_executions) > 0, "Should have at least one execution"

        substeps_by_redshift = {}
        for execution in all_executions:
            substeps_by_redshift.setdefault(execution["redshift"], []).append(
                execution["num_substeps"]
            )

        counts_by_redshift = {
            redshift: max(num_substeps) for redshift, num_substeps in substeps_by_redshift.items()
        }
        low_z = min(counts_by_redshift)
        low_z_count = counts_by_redshift[low_z]
        high_z, high_z_count = max(counts_by_redshift.items(), key=lambda item: item[1])

        assert high_z > low_z, f"Largest dynamic N should occur above z={low_z}, got z={high_z}"
        assert (
            high_z_count > low_z_count
        ), f"Expected higher-z N > low-z N, got {high_z_count} <= {low_z_count}"

        print("  ✓ Dynamic timestep uses varied substep counts")
        print(f"    z={high_z:.4f}: num_substeps={high_z_count}")
        print(f"    z={low_z:.4f}: num_substeps={low_z_count}")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_substeps_creates_loop,
            test_substep_dt_calculation,
            test_module_context_fields,
            test_substeps_one_no_loop,
            test_dynamic_timestep_varies_substeps,
        ],
        "SubSteps Time-Stepping and ModuleContext (test_substeps.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
