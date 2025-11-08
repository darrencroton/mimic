#!/usr/bin/env python3
"""
Physics Sanity Scientific Test

Validates: Physical sanity of halo properties
Phase: Phase 2 (Testing Framework)

This test validates that all halo properties are physically reasonable:
- No NaN or Inf values
- Positive masses (Mvir > 0)
- Positive radii (Rvir > 0)
- Positive velocities (Vvir > 0, Vmax > 0)
- Positive particle counts (Len > 0)
- Valid snapshot numbers (0 <= SnapNum <= MaxSnap)

Test cases:
  - test_no_nans: No NaN values in any field
  - test_no_infs: No Inf values in any field
  - test_positive_masses: All masses are positive
  - test_positive_radii: All radii are positive
  - test_positive_velocities: All velocities are positive
  - test_positive_particle_counts: All halos have particles
  - test_valid_snapshots: Snapshot numbers are valid

Author: Mimic Testing Team
Date: 2025-11-08
"""

import subprocess
import sys
from pathlib import Path

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import load_binary_halos, validate_no_nans, validate_no_infs

# Repository paths
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"


def run_mimic_if_needed():
    """
    Run Mimic if output doesn't exist

    Returns:
        Path: Path to output file
    """
    output_dir = TEST_DATA_DIR / "output" / "baseline"
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


def test_no_nans():
    """
    Test that no halo properties contain NaN values

    Expected: All fields are finite numbers
    Validates: Numerical stability
    """
    print("Testing for NaN values...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)
    print(f"  Loaded {metadata['TotHalos']} halos from {metadata['Ntrees']} trees")

    # Check for NaNs
    nan_counts = validate_no_nans(halos)

    if nan_counts:
        print(f"  ✗ Found NaN values in {len(nan_counts)} field(s):")
        for field, count in nan_counts.items():
            print(f"    {field}: {count} NaN values")
        assert False, f"Found NaN values in {len(nan_counts)} field(s)"
    else:
        print(f"  ✓ No NaN values found in any field")


def test_no_infs():
    """
    Test that no halo properties contain infinite values

    Expected: All fields are finite numbers
    Validates: Numerical stability
    """
    print("Testing for Inf values...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Check for infinities
    inf_counts = validate_no_infs(halos)

    if inf_counts:
        print(f"  ✗ Found Inf values in {len(inf_counts)} field(s):")
        for field, count in inf_counts.items():
            print(f"    {field}: {count} Inf values")
        assert False, f"Found Inf values in {len(inf_counts)} field(s)"
    else:
        print(f"  ✓ No Inf values found in any field")


def test_positive_masses():
    """
    Test that all halo masses are positive

    Expected: Mvir > 0 for all halos
    Validates: Mass calculation correctness
    """
    print("Testing for positive masses...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Check Mvir > 0
    import numpy as np
    negative_mask = halos.Mvir <= 0
    count_negative = np.sum(negative_mask)

    if count_negative > 0:
        # Get some examples
        indices = np.where(negative_mask)[0][:5]
        examples = [(int(i), float(halos.Mvir[i])) for i in indices]

        print(f"  ✗ Found {count_negative} halos with non-positive mass:")
        for idx, mass in examples:
            print(f"    Halo {idx}: Mvir = {mass}")
        assert False, f"Found {count_negative} halos with non-positive mass"
    else:
        min_mass = np.min(halos.Mvir)
        max_mass = np.max(halos.Mvir)
        print(f"  ✓ All {len(halos)} halos have positive mass")
        print(f"  Mass range: {min_mass:.2e} to {max_mass:.2e} Msun/h")


def test_positive_radii():
    """
    Test that all halo radii are positive

    Expected: Rvir > 0 for all halos
    Validates: Radius calculation correctness
    """
    print("Testing for positive radii...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Check Rvir > 0
    import numpy as np
    negative_mask = halos.Rvir <= 0
    count_negative = np.sum(negative_mask)

    if count_negative > 0:
        # Get some examples
        indices = np.where(negative_mask)[0][:5]
        examples = [(int(i), float(halos.Rvir[i])) for i in indices]

        print(f"  ✗ Found {count_negative} halos with non-positive radius:")
        for idx, radius in examples:
            print(f"    Halo {idx}: Rvir = {radius}")
        assert False, f"Found {count_negative} halos with non-positive radius"
    else:
        min_radius = np.min(halos.Rvir)
        max_radius = np.max(halos.Rvir)
        print(f"  ✓ All {len(halos)} halos have positive radius")
        print(f"  Radius range: {min_radius:.4f} to {max_radius:.4f} Mpc/h")


def test_positive_velocities():
    """
    Test that all halo velocities are positive

    Expected: Vvir > 0 and Vmax > 0 for all halos
    Validates: Velocity calculation correctness
    """
    print("Testing for positive velocities...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    import numpy as np

    # Check Vvir > 0
    negative_vvir = np.sum(halos.Vvir <= 0)

    # Check Vmax > 0
    negative_vmax = np.sum(halos.Vmax <= 0)

    errors = []
    if negative_vvir > 0:
        errors.append(f"Vvir: {negative_vvir} halos")
    if negative_vmax > 0:
        errors.append(f"Vmax: {negative_vmax} halos")

    if errors:
        print(f"  ✗ Found halos with non-positive velocities:")
        for err in errors:
            print(f"    {err}")
        assert False, f"Found halos with non-positive velocities: {', '.join(errors)}"
    else:
        print(f"  ✓ All {len(halos)} halos have positive velocities")
        print(f"  Vvir range: {np.min(halos.Vvir):.2f} to {np.max(halos.Vvir):.2f} km/s")
        print(f"  Vmax range: {np.min(halos.Vmax):.2f} to {np.max(halos.Vmax):.2f} km/s")


def test_positive_particle_counts():
    """
    Test that all halos have particles

    Expected: Len > 0 for all halos
    Validates: Halo construction correctness
    """
    print("Testing for positive particle counts...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Check Len > 0
    import numpy as np
    zero_particles = np.sum(halos.Len <= 0)

    if zero_particles > 0:
        indices = np.where(halos.Len <= 0)[0][:5]
        examples = [(int(i), int(halos.Len[i])) for i in indices]

        print(f"  ✗ Found {zero_particles} halos with no particles:")
        for idx, count in examples:
            print(f"    Halo {idx}: Len = {count}")
        assert False, f"Found {zero_particles} halos with no particles"
    else:
        min_len = np.min(halos.Len)
        max_len = np.max(halos.Len)
        print(f"  ✓ All {len(halos)} halos have particles")
        print(f"  Particle count range: {min_len} to {max_len}")


def test_valid_snapshots():
    """
    Test that all snapshot numbers are valid

    Expected: 0 <= SnapNum <= 63
    Validates: Snapshot handling correctness
    """
    print("Testing for valid snapshot numbers...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    output_file = run_mimic_if_needed()

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Check 0 <= SnapNum <= 63
    import numpy as np
    invalid_snap = np.sum((halos.SnapNum < 0) | (halos.SnapNum > 63))

    if invalid_snap > 0:
        indices = np.where((halos.SnapNum < 0) | (halos.SnapNum > 63))[0][:5]
        examples = [(int(i), int(halos.SnapNum[i])) for i in indices]

        print(f"  ✗ Found {invalid_snap} halos with invalid snapshot:")
        for idx, snap in examples:
            print(f"    Halo {idx}: SnapNum = {snap}")
        assert False, f"Found {invalid_snap} halos with invalid snapshot"
    else:
        unique_snaps = np.unique(halos.SnapNum)
        print(f"  ✓ All {len(halos)} halos have valid snapshots")
        print(f"  Snapshot numbers present: {sorted(unique_snaps)}")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    print("=" * 60)
    print("Scientific Test: Physics Sanity")
    print("=" * 60)
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")
    print()

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"ERROR: Mimic executable not found: {MIMIC_EXE}")
        print("Build it first with: make")
        return 1

    tests = [
        test_no_nans,
        test_no_infs,
        test_positive_masses,
        test_positive_radii,
        test_positive_velocities,
        test_positive_particle_counts,
        test_valid_snapshots,
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
