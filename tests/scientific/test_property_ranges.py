#!/usr/bin/env python3
"""
Property Ranges Scientific Test

Validates: Halo properties are within physically reasonable ranges
Phase: Phase 2 (Testing Framework)

This test validates that all halo properties fall within expected ranges:
- Masses: 10^8 < Mvir < 10^16 Msun/h (reasonable for simulations)
- Radii: 0.001 < Rvir < 10.0 Mpc/h
- Velocities: 10 < Vvir, Vmax < 5000 km/s
- Positions: Inside simulation box (0 < x,y,z < BoxSize)
- Particle counts: Len >= 20 (minimum halo size)

Test cases:
  - test_mass_ranges: Mvir in reasonable range
  - test_radius_ranges: Rvir in reasonable range
  - test_velocity_ranges: Vvir, Vmax in reasonable range
  - test_position_ranges: Positions inside box
  - test_particle_count_ranges: Len meets minimum

Author: Mimic Testing Team
Date: 2025-11-08
"""

import subprocess
import sys
from pathlib import Path
import numpy as np

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import load_binary_halos, validate_range

# Repository paths
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"

# Expected ranges (from simulation cosmology and resolution)
# NOTE: If tests fail, these ranges can be adjusted based on actual data
EXPECTED_RANGES = {
    'Mvir_min': 1.0e8,   # Minimum halo mass (Msun/h)
    'Mvir_max': 1.0e16,  # Maximum halo mass (Msun/h)
    'Rvir_min': 0.001,   # Minimum virial radius (Mpc/h)
    'Rvir_max': 10.0,    # Maximum virial radius (Mpc/h)
    'Vvir_min': 10.0,    # Minimum virial velocity (km/s)
    'Vvir_max': 5000.0,  # Maximum virial velocity (km/s)
    'Vmax_min': 10.0,    # Minimum maximum velocity (km/s)
    'Vmax_max': 5000.0,  # Maximum maximum velocity (km/s)
    'BoxSize': 62.5,     # Simulation box size (Mpc/h)
    'Len_min': 20,       # Minimum particle count
    'MaxSnap': 63,       # Maximum snapshot number
}


def run_mimic_if_needed():
    """
    Run Mimic if output doesn't exist

    Returns:
        Path: Path to output file
    """
    output_dir = TEST_DATA_DIR / "expected" / "test"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    if not output_file.exists():
        print("  Running Mimic to generate output...")
        param_file = TEST_DATA_DIR / "test.par"
        result = subprocess.run(
            [str(MIMIC_EXE), str(param_file)],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True
        )
        if result.returncode != 0:
            print(f"STDOUT:\n{result.stdout}")
            print(f"STDERR:\n{result.stderr}")
            raise RuntimeError(f"Mimic execution failed with code {result.returncode}")

    return output_file


def test_mass_ranges():
    """
    Test that halo masses are in reasonable range

    Expected: 10^8 < Mvir < 10^16 Msun/h
    Validates: Mass calculations are physically reasonable
    """
    print("Testing halo mass ranges...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)
    print(f"  Validating {metadata['TotHalos']} halos")

    # Validate Mvir range
    result = validate_range(
        halos, 'Mvir',
        EXPECTED_RANGES['Mvir_min'],
        EXPECTED_RANGES['Mvir_max']
    )

    if not result['passed']:
        print(f"  ✗ Mvir values outside expected range:")
        print(f"    Expected: {EXPECTED_RANGES['Mvir_min']:.2e} to {EXPECTED_RANGES['Mvir_max']:.2e} Msun/h")
        print(f"    Actual range: {result['min_value']:.2e} to {result['max_value']:.2e} Msun/h")

        if result['count_below'] > 0:
            print(f"    {result['count_below']} halos below minimum:")
            for idx, val in result['examples_below']:
                print(f"      Halo {idx}: Mvir = {val:.2e}")

        if result['count_above'] > 0:
            print(f"    {result['count_above']} halos above maximum:")
            for idx, val in result['examples_above']:
                print(f"      Halo {idx}: Mvir = {val:.2e}")

        assert False, (
            f"Mvir range validation failed: {result['count_below']} below min, "
            f"{result['count_above']} above max"
        )
    else:
        print(f"  ✓ All {len(halos)} halos have mass in expected range")
        print(f"  Mvir range: {result['min_value']:.2e} to {result['max_value']:.2e} Msun/h")


def test_radius_ranges():
    """
    Test that halo radii are in reasonable range

    Expected: 0.001 < Rvir < 10.0 Mpc/h
    Validates: Radius calculations are physically reasonable
    """
    print("Testing halo radius ranges...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Validate Rvir range
    result = validate_range(
        halos, 'Rvir',
        EXPECTED_RANGES['Rvir_min'],
        EXPECTED_RANGES['Rvir_max']
    )

    if not result['passed']:
        print(f"  ✗ Rvir values outside expected range:")
        print(f"    Expected: {EXPECTED_RANGES['Rvir_min']:.4f} to {EXPECTED_RANGES['Rvir_max']:.4f} Mpc/h")
        print(f"    Actual range: {result['min_value']:.4f} to {result['max_value']:.4f} Mpc/h")

        if result['count_below'] > 0:
            print(f"    {result['count_below']} halos below minimum:")
            for idx, val in result['examples_below']:
                print(f"      Halo {idx}: Rvir = {val:.4f}")

        if result['count_above'] > 0:
            print(f"    {result['count_above']} halos above maximum:")
            for idx, val in result['examples_above']:
                print(f"      Halo {idx}: Rvir = {val:.4f}")

        assert False, (
            f"Rvir range validation failed: {result['count_below']} below min, "
            f"{result['count_above']} above max"
        )
    else:
        print(f"  ✓ All {len(halos)} halos have radius in expected range")
        print(f"  Rvir range: {result['min_value']:.4f} to {result['max_value']:.4f} Mpc/h")


def test_velocity_ranges():
    """
    Test that halo velocities are in reasonable range

    Expected: 10 < Vvir, Vmax < 5000 km/s
    Validates: Velocity calculations are physically reasonable
    """
    print("Testing halo velocity ranges...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Validate Vvir range
    vvir_result = validate_range(
        halos, 'Vvir',
        EXPECTED_RANGES['Vvir_min'],
        EXPECTED_RANGES['Vvir_max']
    )

    # Validate Vmax range
    vmax_result = validate_range(
        halos, 'Vmax',
        EXPECTED_RANGES['Vmax_min'],
        EXPECTED_RANGES['Vmax_max']
    )

    failures = []

    if not vvir_result['passed']:
        failures.append('Vvir')
        print(f"  ✗ Vvir values outside expected range:")
        print(f"    Expected: {EXPECTED_RANGES['Vvir_min']:.2f} to {EXPECTED_RANGES['Vvir_max']:.2f} km/s")
        print(f"    Actual range: {vvir_result['min_value']:.2f} to {vvir_result['max_value']:.2f} km/s")

        if vvir_result['count_below'] > 0:
            print(f"    {vvir_result['count_below']} halos below minimum:")
            for idx, val in vvir_result['examples_below'][:3]:
                print(f"      Halo {idx}: Vvir = {val:.2f}")

        if vvir_result['count_above'] > 0:
            print(f"    {vvir_result['count_above']} halos above maximum:")
            for idx, val in vvir_result['examples_above'][:3]:
                print(f"      Halo {idx}: Vvir = {val:.2f}")

    if not vmax_result['passed']:
        failures.append('Vmax')
        print(f"  ✗ Vmax values outside expected range:")
        print(f"    Expected: {EXPECTED_RANGES['Vmax_min']:.2f} to {EXPECTED_RANGES['Vmax_max']:.2f} km/s")
        print(f"    Actual range: {vmax_result['min_value']:.2f} to {vmax_result['max_value']:.2f} km/s")

        if vmax_result['count_below'] > 0:
            print(f"    {vmax_result['count_below']} halos below minimum:")
            for idx, val in vmax_result['examples_below'][:3]:
                print(f"      Halo {idx}: Vmax = {val:.2f}")

        if vmax_result['count_above'] > 0:
            print(f"    {vmax_result['count_above']} halos above maximum:")
            for idx, val in vmax_result['examples_above'][:3]:
                print(f"      Halo {idx}: Vmax = {val:.2f}")

    if failures:
        assert False, f"Velocity range validation failed for: {', '.join(failures)}"
    else:
        print(f"  ✓ All {len(halos)} halos have velocities in expected range")
        print(f"  Vvir range: {vvir_result['min_value']:.2f} to {vvir_result['max_value']:.2f} km/s")
        print(f"  Vmax range: {vmax_result['min_value']:.2f} to {vmax_result['max_value']:.2f} km/s")


def test_position_ranges():
    """
    Test that halo positions are inside simulation box

    Expected: 0 <= x, y, z < BoxSize
    Validates: Position tracking correctness
    """
    print("Testing halo position ranges...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Check each component of position
    BoxSize = EXPECTED_RANGES['BoxSize']

    violations = []
    for i, axis in enumerate(['x', 'y', 'z']):
        pos_component = halos.Pos[:, i]

        below_zero = np.sum(pos_component < 0)
        above_box = np.sum(pos_component >= BoxSize)

        if below_zero > 0:
            violations.append(f"{axis}: {below_zero} halos below 0")
        if above_box > 0:
            violations.append(f"{axis}: {above_box} halos >= BoxSize")

    if violations:
        print(f"  ✗ Position violations found:")
        for v in violations:
            print(f"    {v}")
        assert False, f"Position range validation failed: {', '.join(violations)}"
    else:
        print(f"  ✓ All {len(halos)} halos have positions inside box")
        print(f"  Box size: {BoxSize} Mpc/h")
        for i, axis in enumerate(['x', 'y', 'z']):
            pos_min = np.min(halos.Pos[:, i])
            pos_max = np.max(halos.Pos[:, i])
            print(f"  {axis} range: {pos_min:.4f} to {pos_max:.4f} Mpc/h")


def test_particle_count_ranges():
    """
    Test that halos meet minimum particle count

    Expected: Len >= 20 (typical minimum for halos)
    Validates: Halo selection criteria
    """
    print("Testing particle count ranges...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    min_particles = EXPECTED_RANGES['Len_min']

    below_min = np.sum(halos.Len < min_particles)

    if below_min > 0:
        # Get examples
        indices = np.where(halos.Len < min_particles)[0][:5]
        examples = [(int(i), int(halos.Len[i])) for i in indices]

        print(f"  ✗ {below_min} halos below minimum particle count:")
        print(f"    Expected: Len >= {min_particles}")
        for idx, count in examples:
            print(f"      Halo {idx}: Len = {count}")

        assert False, f"Found {below_min} halos below minimum particle count"
    else:
        actual_min = np.min(halos.Len)
        actual_max = np.max(halos.Len)
        print(f"  ✓ All {len(halos)} halos meet minimum particle count")
        print(f"  Particle count range: {actual_min} to {actual_max}")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    print("=" * 60)
    print("Scientific Test: Property Ranges")
    print("=" * 60)
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    print()
    print("Expected ranges:")
    for key, val in EXPECTED_RANGES.items():
        print(f"  {key}: {val}")
    print()

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"ERROR: Mimic executable not found: {MIMIC_EXE}")
        print("Build it first with: make")
        return 1

    tests = [
        test_mass_ranges,
        test_radius_ranges,
        test_velocity_ranges,
        test_position_ranges,
        test_particle_count_ranges,
    ]

    passed = 0
    failed = 0

    # ANSI color codes
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    NC = '\033[0m'  # No Color

    for test in tests:
        print()
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            print(f"{RED}✗ ERROR: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1

    print()
    print("=" * 60)
    print("Test Summary")
    print("=" * 60)
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print(f"Total:  {passed + failed}")
    print("=" * 60)

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
