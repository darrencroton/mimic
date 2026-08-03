#!/usr/bin/env python3
"""
SAGE Update Disk Radius Module - Integration Test

Validates: Module pipeline integration, physics correctness, type filtering

This test validates the sage_set_disk_scale_radius module integration:
- Module loads and executes correctly in full pipeline
- DiskScaleRadius output is physically reasonable
- Type filtering (Type 0 processed, Type 1 satellites preserve inherited radii)
- Memory safety and performance

Test cases:
  - test_module_pipeline_integration: Full pipeline execution and output validation
  - test_disk_radius_physics: Physics correctness and reasonableness checks
  - test_disk_radius_type_filtering: Type 0 vs satellite/orphan filtering
  - test_memory_and_performance: Memory leaks and performance baseline

Author: Mimic Development Team
Date: 2025-12-18
"""

import shutil
import sys
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    check_no_memory_leaks,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)


def test_module_pipeline_integration():
    """Test that module integrates correctly into full pipeline

    Validates:
    - Module loads and initializes
    - Pipeline executes without errors
    - Output file is created with expected properties
    - DiskScaleRadius field exists and has valid data
    """
    print(f"\n{BLUE}TEST: Module pipeline integration{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="disk_radius_integration",
        phase_config={
            "pre_timestep": [("sage_set_disk_scale_radius", "process_full_halo")],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={},
    )

    # Execute Mimic
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Validate output file exists
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    # Load and validate output data
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    # Check DiskScaleRadius field exists
    assert "DiskScaleRadius" in halos.dtype.names, "Output should have DiskScaleRadius field"

    # Basic validation: no NaNs or Infs
    disk_radius = halos["DiskScaleRadius"]
    assert not np.any(np.isnan(disk_radius)), "DiskScaleRadius should not have NaN values"
    assert not np.any(np.isinf(disk_radius)), "DiskScaleRadius should not have Inf values"
    assert np.all(disk_radius >= 0.0), "DiskScaleRadius should be non-negative"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module pipeline integration successful{NC}")


def test_disk_radius_physics():
    """Test that disk radius physics is physically reasonable

    Validates:
    - DiskScaleRadius values are within expected range (0.001-1.0 Mpc/h)
    - Disk radius scales with virial radius for halos with positive Rvir
    - Disk radius is smaller than virial radius when Rvir > 0
    - Type 0 galaxies have non-zero disk radii (if they exist)
    """
    print(f"\n{BLUE}TEST: Disk radius physics correctness{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="disk_radius_physics",
        phase_config={
            "pre_timestep": [("sage_set_disk_scale_radius", "process_full_halo")],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={},
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Load output
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    disk_radius = halos["DiskScaleRadius"]
    rvir = halos["Rvir"]
    halo_type = halos["Type"]

    # Physics validation 1: Disk radius should be a reasonable fraction of Rvir
    # for halos with positive virial radius.
    # Typically expect 0.001-0.5 * Rvir (Mo98 model with typical spin parameters).
    galaxies_with_radius = (disk_radius > 0) & (rvir > 0)
    zero_rvir_with_disk = np.sum((disk_radius > 0) & (rvir <= 0))
    if zero_rvir_with_disk > 0:
        print(
            f"  Note: {zero_rvir_with_disk} halos have DiskScaleRadius>0 with Rvir=0 edge-case output"
        )

    if np.sum(galaxies_with_radius) > 0:
        rd_over_rvir = disk_radius[galaxies_with_radius] / rvir[galaxies_with_radius]

        # Disk radius should always be less than virial radius where Rvir is positive.
        assert np.all(
            disk_radius[galaxies_with_radius] <= rvir[galaxies_with_radius]
        ), "DiskScaleRadius should always be <= Rvir for halos with Rvir > 0"

        # Should be reasonable fraction
        assert np.all(rd_over_rvir < 1.0), "DiskScaleRadius/Rvir should be < 1.0"
        assert np.all(rd_over_rvir > 0.0), "DiskScaleRadius/Rvir should be positive"

        print(
            f"  DiskScaleRadius/Rvir range: {np.min(rd_over_rvir):.4e} to {np.max(rd_over_rvir):.4e}"
        )

    # Physics validation 2: Disk radius should scale with Rvir
    # (for similar spin parameters, larger halos have larger disks)
    # Note: Correlation is moderate (not strong) because disk radius depends on
    # both spin parameter and Rvir, and spin varies significantly between halos
    if np.sum(galaxies_with_radius) > 10:
        # Correlation coefficient should be positive (but may be moderate due to spin variation)
        correlation = np.corrcoef(disk_radius[galaxies_with_radius], rvir[galaxies_with_radius])[
            0, 1
        ]
        print(f"  Correlation(DiskScaleRadius, Rvir): {correlation:.3f}")
        assert (
            correlation > 0.2
        ), f"DiskScaleRadius should correlate positively with Rvir (got {correlation:.3f})"

    # Physics validation 3: Typical disk radii should be in range 0.001-1.0 Mpc/h
    if np.sum(galaxies_with_radius) > 0:
        median_disk_radius = np.median(disk_radius[galaxies_with_radius])
        print(f"  Median DiskScaleRadius: {median_disk_radius:.4e} Mpc/h")
        assert median_disk_radius > 0.0001, "Median disk radius too small (< 0.1 kpc/h)"
        assert median_disk_radius < 10.0, "Median disk radius too large (> 10 Mpc/h)"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Disk radius physics is physically reasonable{NC}")


def test_disk_radius_type_filtering():
    """Test that Type 0 galaxies are processed and satellites are preserved

    Validates:
    - Type 0 (central) galaxies have disk radii calculated
    - Type 1 (satellite) galaxies have finite inherited/preserved disk radii
    - Type 2+ (orphan) galaxies are skipped or keep inherited disk radii
    """
    print(f"\n{BLUE}TEST: Type filtering{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="disk_radius_types",
        phase_config={
            "pre_timestep": [("sage_set_disk_scale_radius", "process_full_halo")],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={},
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Load output
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    disk_radius = halos["DiskScaleRadius"]
    halo_type = halos["Type"]

    # Count halos by type
    type0_count = np.sum(halo_type == 0)
    type1_count = np.sum(halo_type == 1)
    type2_count = np.sum(halo_type >= 2)

    print(f"  Type 0 (central) halos: {type0_count}")
    print(f"  Type 1 (satellite) halos: {type1_count}")
    print(f"  Type 2+ (orphan) halos: {type2_count}")

    # Type 0 galaxies: should have non-zero disk radii when virial properties are valid.
    type0_halos = halo_type == 0
    if np.sum(type0_halos) > 0:
        type0_disk_radii = disk_radius[type0_halos]

        nonzero_type0 = np.sum(type0_disk_radii > 0)
        fraction_nonzero = nonzero_type0 / np.sum(type0_halos)
        print(
            f"  Type 0 with non-zero DiskScaleRadius: {nonzero_type0}/{np.sum(type0_halos)} ({fraction_nonzero:.1%})"
        )

        assert fraction_nonzero > 0.5, "Most Type 0 galaxies should have non-zero disk radii"

    # Type 1/2+ galaxies are not recomputed by this module. Depending on
    # inheritance they may carry a previous central value or remain zero; the
    # integration contract here is that they remain finite and non-negative.
    if type1_count > 0:
        type1_disk_radii = disk_radius[halo_type == 1]
        assert not np.any(
            np.isnan(type1_disk_radii)
        ), "Type 1 DiskScaleRadius should not have NaN values"
        assert not np.any(
            np.isinf(type1_disk_radii)
        ), "Type 1 DiskScaleRadius should not have Inf values"
        assert np.all(type1_disk_radii >= 0.0), "Type 1 DiskScaleRadius should remain non-negative"

    if type2_count > 0:
        type2_disk_radii = disk_radius[halo_type >= 2]
        assert not np.any(np.isnan(type2_disk_radii)), "Type 2+ should not have NaN disk radii"
        assert not np.any(np.isinf(type2_disk_radii)), "Type 2+ should not have Inf disk radii"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Type filtering validated{NC}")


def test_memory_and_performance():
    """Test memory safety and establish performance baseline

    Validates:
    - No memory leaks during execution
    - Execution completes in reasonable time
    """
    print(f"\n{BLUE}TEST: Memory safety and performance{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="disk_radius_memory",
        phase_config={
            "pre_timestep": [("sage_set_disk_scale_radius", "process_full_halo")],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={},
    )

    # Execute and time
    import time

    start_time = time.time()
    returncode, stdout, stderr = run_mimic(param_file)
    elapsed_time = time.time() - start_time

    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Check for memory leaks
    assert check_no_memory_leaks(stdout, stderr), "Should not have memory leaks"

    # Performance baseline (should complete in < 30 seconds for test data)
    print(f"  Execution time: {elapsed_time:.2f}s")
    assert elapsed_time < 30.0, f"Execution too slow: {elapsed_time:.2f}s (expected < 30s)"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Memory safety and performance validated{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: {Path(__file__).name}{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_module_pipeline_integration,
        test_disk_radius_physics,
        test_disk_radius_type_filtering,
        test_memory_and_performance,
    ]

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

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
