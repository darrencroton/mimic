#!/usr/bin/env python3
"""
Output Format Integration Test

Validates: Binary and HDF5 output format correctness and baseline comparison
Phase: Phase 3 (Testing Framework Enhancement)

This test validates that Mimic's output system correctly:
- Produces binary output files with correct structure
- Produces HDF5 output files (when compiled with HDF5)
- Output data is loadable and has valid structure
- Output matches baseline reference data
- Both formats contain equivalent halo counts

Test cases:
  - test_binary_format_execution: Binary format end-to-end execution
  - test_binary_format_loading: Binary output can be loaded
  - test_binary_baseline_comparison: Binary output matches baseline
  - test_hdf5_format_execution: HDF5 format end-to-end execution (if available)
  - test_hdf5_format_loading: HDF5 output can be loaded (if available)
  - test_hdf5_baseline_comparison: HDF5 output matches baseline (if available)

Author: Mimic Testing Team
Date: 2025-11-10
"""

import subprocess
import sys
from pathlib import Path
import numpy as np

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import load_binary_halos

# Repository paths
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"


def ensure_output_dirs():
    """
    Create output directories if they don't exist

    Creates the binary and HDF5 output directories required by test parameter files.
    This ensures tests work correctly after make test-clean or in fresh clones.
    """
    (TEST_DATA_DIR / "output" / "binary").mkdir(parents=True, exist_ok=True)
    (TEST_DATA_DIR / "output" / "hdf5").mkdir(parents=True, exist_ok=True)


# Ensure output directories exist before any tests run
ensure_output_dirs()


def run_mimic(param_file):
    """
    Execute Mimic with specified parameter file

    Args:
        param_file (Path): Path to parameter file

    Returns:
        tuple: (returncode, stdout, stderr)
    """
    result = subprocess.run(
        [str(MIMIC_EXE), str(param_file)],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True
    )
    return result.returncode, result.stdout, result.stderr


def check_hdf5_support():
    """
    Check if Mimic was compiled with HDF5 support

    Returns:
        bool: True if HDF5 support is available
    """
    # Run mimic with --version or similar to check for HDF5
    # For now, we'll check if HDF5 output succeeds
    param_file = TEST_DATA_DIR / "test_hdf5.par"
    if not param_file.exists():
        return False

    returncode, stdout, stderr = run_mimic(param_file)

    # If it fails with "unknown output format" or similar, HDF5 not supported
    if returncode != 0:
        output = (stdout + stderr).lower()
        if "hdf5" in output and ("unknown" in output or "not supported" in output or "not compiled" in output):
            return False

    return returncode == 0


def load_hdf5_halos(output_file):
    """
    Load halo data from HDF5 output file

    Args:
        output_file (Path): Path to HDF5 output file

    Returns:
        tuple: (halos, metadata) where halos is structured array
    """
    try:
        import h5py
    except ImportError:
        raise ImportError("h5py not available - cannot load HDF5 output")

    with h5py.File(output_file, 'r') as f:
        # Mimic HDF5 structure: Root contains snapshot groups (e.g., 'Snap063')
        # Each snapshot group contains 'Galaxies' dataset (structured array)

        # Get snapshot groups (e.g., 'Snap063')
        snap_groups = [key for key in f.keys() if key.startswith('Snap')]

        if not snap_groups:
            raise ValueError(f"No snapshot groups found in HDF5 file: {output_file}")

        # For testing, we expect one snapshot (Snap063 for z=0)
        # Use the first snapshot group found
        snap_name = snap_groups[0]
        snap_group = f[snap_name]

        # Read halo data from 'Galaxies' dataset
        if 'Galaxies' not in snap_group:
            raise ValueError(f"No 'Galaxies' dataset found in {snap_name}")

        # Load the structured array directly
        halos = snap_group['Galaxies'][:]

        # Get metadata from group attributes
        attrs = dict(snap_group.attrs) if hasattr(snap_group, 'attrs') else {}

        # Also check for TreeHalosPerSnap to get tree count
        ntrees = len(snap_group['TreeHalosPerSnap'][:]) if 'TreeHalosPerSnap' in snap_group else 1

        # Create metadata
        metadata = {
            'TotHalos': len(halos),
            'Ntrees': ntrees,
            'NoutputSnaps': 1,
            'SnapshotName': snap_name,
        }
        metadata.update(attrs)

    # Convert to recarray for attribute access (outside the 'with' block)
    halos = halos.view(np.recarray)

    return halos, metadata


def test_binary_format_execution():
    """
    Test that binary format executes successfully

    Expected: Mimic runs to completion with binary output
    Validates: Binary format end-to-end execution
    """
    print("Testing binary format execution...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    param_file = TEST_DATA_DIR / "test_binary.par"
    assert param_file.exists(), f"Parameter file not found: {param_file}"

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution success
    assert returncode == 0, f"Mimic failed with code {returncode}\nSTDERR: {stderr}"

    print(f"  ✓ Binary format execution successful")


def test_binary_format_loading():
    """
    Test that binary output can be loaded

    Expected: Binary file exists and can be loaded
    Validates: Binary format structure and loadability
    """
    print("Testing binary format data loading...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Check output file exists
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    # Run Mimic if needed
    if not output_file.exists():
        print(f"  Running Mimic to generate output...")
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    assert output_file.exists(), f"Binary output file not created: {output_file}"

    # Load halos
    halos, metadata = load_binary_halos(output_file)

    # Validate loaded data
    assert metadata['TotHalos'] > 0, "No halos loaded from binary file"
    assert len(halos) == metadata['TotHalos'], "Halo count mismatch"
    assert hasattr(halos, 'Mvir'), "Binary data missing expected property (Mvir)"
    assert hasattr(halos, 'Rvir'), "Binary data missing expected property (Rvir)"

    print(f"  ✓ Binary output loaded successfully")
    print(f"  Total halos: {metadata['TotHalos']}")
    print(f"  Number of trees: {metadata.get('Ntrees', 'N/A')}")
    print(f"  File size: {output_file.stat().st_size:,} bytes")


def test_binary_baseline_comparison():
    """
    Test that binary output matches baseline reference data

    Expected: Halo counts match baseline, properties are consistent
    Validates: Binary output regression testing
    """
    print("Testing binary baseline comparison...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Load test output
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"

    # Run Mimic if needed
    if not output_file.exists():
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    halos_test, metadata_test = load_binary_halos(output_file)

    # Load baseline
    baseline_dir = TEST_DATA_DIR / "output" / "baseline" / "binary"
    baseline_file = baseline_dir / "model_z0.000_0"

    assert baseline_file.exists(), (
        f"Baseline file not found: {baseline_file}\n"
        f"Run Mimic once to establish baseline, then commit the baseline file."
    )

    halos_baseline, metadata_baseline = load_binary_halos(baseline_file)

    # Compare halo counts
    assert metadata_test['TotHalos'] == metadata_baseline['TotHalos'], (
        f"Halo count mismatch: test={metadata_test['TotHalos']}, "
        f"baseline={metadata_baseline['TotHalos']}"
    )

    # Compare tree counts
    if 'Ntrees' in metadata_test and 'Ntrees' in metadata_baseline:
        assert metadata_test['Ntrees'] == metadata_baseline['Ntrees'], (
            f"Tree count mismatch: test={metadata_test['Ntrees']}, "
            f"baseline={metadata_baseline['Ntrees']}"
        )

    print(f"  ✓ Binary output matches baseline")
    print(f"  Halos: {metadata_test['TotHalos']} (test) == {metadata_baseline['TotHalos']} (baseline)")

    # Compare mass ranges (should be similar, allowing for minor numerical differences)
    test_mass_range = (np.min(halos_test.Mvir), np.max(halos_test.Mvir))
    baseline_mass_range = (np.min(halos_baseline.Mvir), np.max(halos_baseline.Mvir))

    # Check if ranges match (handle zero values properly)
    # If both values are zero, that's a perfect match
    # Otherwise, calculate relative difference
    def safe_relative_diff(val1, val2):
        """Calculate relative difference, handling zeros properly"""
        if val1 == val2:  # Perfect match (including both zero)
            return 0.0
        if val2 == 0.0:  # Avoid division by zero
            return abs(val1)  # If baseline is 0 but test isn't, that's a big difference
        return abs(val1 - val2) / abs(val2)

    rel_diff_min = safe_relative_diff(test_mass_range[0], baseline_mass_range[0])
    rel_diff_max = safe_relative_diff(test_mass_range[1], baseline_mass_range[1])

    # Allow 0.1% relative difference in mass ranges
    tolerance = 0.001
    assert rel_diff_min < tolerance and rel_diff_max < tolerance, (
        f"Mass range mismatch:\n"
        f"  Test: {test_mass_range[0]:.2e} to {test_mass_range[1]:.2e}\n"
        f"  Baseline: {baseline_mass_range[0]:.2e} to {baseline_mass_range[1]:.2e}\n"
        f"  Relative diff (min): {rel_diff_min:.2e}, (max): {rel_diff_max:.2e}"
    )

    print(f"  Mass range: {test_mass_range[0]:.2e} to {test_mass_range[1]:.2e} Msun/h")


def test_hdf5_format_execution():
    """
    Test that HDF5 format executes successfully (if compiled with HDF5)

    Expected: Mimic runs to completion with HDF5 output (or skips gracefully)
    Validates: HDF5 format end-to-end execution
    """
    print("Testing HDF5 format execution...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Check if HDF5 parameter file exists
    param_file = TEST_DATA_DIR / "test_hdf5.par"
    if not param_file.exists():
        print(f"  Skipping (test_hdf5.par not found)")
        return

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check if HDF5 support is available
    if returncode != 0:
        output = (stdout + stderr).lower()
        if "hdf5" in output and ("unknown" in output or "not supported" in output or "not compiled" in output):
            print(f"  Skipping (Mimic not compiled with HDF5 support)")
            print(f"  Rebuild with: make clean && make USE-HDF5=yes")
            return
        else:
            assert False, f"Mimic failed with code {returncode}\nSTDERR: {stderr}"

    print(f"  ✓ HDF5 format execution successful")


def test_hdf5_format_loading():
    """
    Test that HDF5 output can be loaded (if HDF5 support available)

    Expected: HDF5 file exists and can be loaded
    Validates: HDF5 format structure and loadability
    """
    print("Testing HDF5 format data loading...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Check if HDF5 is supported
    if not check_hdf5_support():
        print(f"  Skipping (Mimic not compiled with HDF5 support)")
        return

    # Check output file exists
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    output_file = output_dir / "model_000.hdf5"  # filenr 0

    # Run Mimic if needed
    if not output_file.exists():
        print(f"  Running Mimic to generate HDF5 output...")
        param_file = TEST_DATA_DIR / "test_hdf5.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    assert output_file.exists(), f"HDF5 output file not created: {output_file}"

    # Check if h5py is available
    try:
        import h5py
    except ImportError:
        print(f"  Skipping detailed validation (h5py not available)")
        print(f"  Install with: pip install h5py")
        return

    # Load halos
    halos, metadata = load_hdf5_halos(output_file)

    # Validate loaded data
    assert metadata['TotHalos'] > 0, "No halos loaded from HDF5 file"
    assert len(halos) == metadata['TotHalos'], "Halo count mismatch"

    print(f"  ✓ HDF5 output loaded successfully")
    print(f"  Total halos: {metadata['TotHalos']}")
    print(f"  File size: {output_file.stat().st_size:,} bytes")


def test_hdf5_baseline_comparison():
    """
    Test that HDF5 output matches baseline reference data (if HDF5 available)

    Expected: Halo counts match baseline
    Validates: HDF5 output regression testing
    """
    print("Testing HDF5 baseline comparison...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Check if HDF5 is supported
    if not check_hdf5_support():
        print(f"  Skipping (Mimic not compiled with HDF5 support)")
        return

    # Check if h5py is available
    try:
        import h5py
    except ImportError:
        print(f"  Skipping (h5py not available)")
        return

    # Load test output
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    output_file = output_dir / "model_000.hdf5"

    # Run Mimic if needed
    if not output_file.exists():
        param_file = TEST_DATA_DIR / "test_hdf5.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    halos_test, metadata_test = load_hdf5_halos(output_file)

    # Load baseline
    baseline_dir = TEST_DATA_DIR / "output" / "baseline" / "hdf5"
    baseline_file = baseline_dir / "model_000.hdf5"

    assert baseline_file.exists(), (
        f"Baseline file not found: {baseline_file}\n"
        f"Run Mimic once with HDF5 to establish baseline, then commit the baseline file."
    )

    halos_baseline, metadata_baseline = load_hdf5_halos(baseline_file)

    # Compare halo counts
    assert metadata_test['TotHalos'] == metadata_baseline['TotHalos'], (
        f"Halo count mismatch: test={metadata_test['TotHalos']}, "
        f"baseline={metadata_baseline['TotHalos']}"
    )

    print(f"  ✓ HDF5 output matches baseline")
    print(f"  Halos: {metadata_test['TotHalos']} (test) == {metadata_baseline['TotHalos']} (baseline)")


def test_format_equivalence():
    """
    Test that binary and HDF5 formats produce identical output

    Expected: Both formats contain same halo count and equivalent data
    Validates: Format consistency - both formats write same information
    """
    print("Testing binary vs HDF5 format equivalence...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Check if HDF5 is supported
    if not check_hdf5_support():
        print(f"  Skipping (Mimic not compiled with HDF5 support)")
        return

    # Check if h5py is available
    try:
        import h5py
    except ImportError:
        print(f"  Skipping (h5py not available)")
        return

    # Load binary output
    binary_dir = TEST_DATA_DIR / "output" / "binary"
    binary_file = binary_dir / "model_z0.000_0"

    # Run Mimic if needed
    if not binary_file.exists():
        print(f"  Running Mimic to generate binary output...")
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Binary Mimic execution failed: {stderr}"

    halos_binary, metadata_binary = load_binary_halos(binary_file)

    # Load HDF5 output
    hdf5_dir = TEST_DATA_DIR / "output" / "hdf5"
    hdf5_file = hdf5_dir / "model_000.hdf5"

    # Run Mimic if needed
    if not hdf5_file.exists():
        print(f"  Running Mimic to generate HDF5 output...")
        param_file = TEST_DATA_DIR / "test_hdf5.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"HDF5 Mimic execution failed: {stderr}"

    halos_hdf5, metadata_hdf5 = load_hdf5_halos(hdf5_file)

    # Compare halo counts
    assert metadata_binary['TotHalos'] == metadata_hdf5['TotHalos'], (
        f"Halo count mismatch: binary={metadata_binary['TotHalos']}, "
        f"hdf5={metadata_hdf5['TotHalos']}"
    )

    print(f"  ✓ Both formats contain same halo count: {metadata_binary['TotHalos']}")

    # Compare data structure - check that both have same fields
    binary_fields = set(halos_binary.dtype.names)
    hdf5_fields = set(halos_hdf5.dtype.names)

    # Both should have at least core halo properties
    core_properties = {'Mvir', 'Rvir', 'Vmax', 'Spin'}
    assert core_properties.issubset(binary_fields), f"Binary missing core properties: {core_properties - binary_fields}"
    assert core_properties.issubset(hdf5_fields), f"HDF5 missing core properties: {core_properties - hdf5_fields}"

    print(f"  ✓ Both formats contain core halo properties")

    # Report any field differences (informational, not a failure)
    if binary_fields != hdf5_fields:
        only_binary = binary_fields - hdf5_fields
        only_hdf5 = hdf5_fields - binary_fields
        if only_binary:
            print(f"  Note: Fields only in binary: {', '.join(sorted(only_binary))}")
        if only_hdf5:
            print(f"  Note: Fields only in HDF5: {', '.join(sorted(only_hdf5))}")
    else:
        print(f"  ✓ Both formats have identical field structure ({len(binary_fields)} fields)")

    # Compare mass ranges - should be identical for same input data
    binary_mass_range = (np.min(halos_binary.Mvir), np.max(halos_binary.Mvir))
    hdf5_mass_range = (np.min(halos_hdf5.Mvir), np.max(halos_hdf5.Mvir))

    # Mass ranges should match exactly (same calculation, same data)
    assert np.allclose(binary_mass_range[0], hdf5_mass_range[0], rtol=1e-10), (
        f"Minimum mass mismatch: binary={binary_mass_range[0]:.6e}, hdf5={hdf5_mass_range[0]:.6e}"
    )
    assert np.allclose(binary_mass_range[1], hdf5_mass_range[1], rtol=1e-10), (
        f"Maximum mass mismatch: binary={binary_mass_range[1]:.6e}, hdf5={hdf5_mass_range[1]:.6e}"
    )

    print(f"  ✓ Mass ranges match: {binary_mass_range[0]:.2e} to {binary_mass_range[1]:.2e} Msun/h")

    # Compare a sample of halo properties for identical values
    # Take first 10 halos (or fewer if less available)
    n_sample = min(10, len(halos_binary), len(halos_hdf5))

    for i in range(n_sample):
        # Check Mvir matches
        assert np.allclose(halos_binary.Mvir[i], halos_hdf5.Mvir[i], rtol=1e-10), (
            f"Halo {i} Mvir mismatch: binary={halos_binary.Mvir[i]:.6e}, hdf5={halos_hdf5.Mvir[i]:.6e}"
        )
        # Check Rvir matches
        assert np.allclose(halos_binary.Rvir[i], halos_hdf5.Rvir[i], rtol=1e-10), (
            f"Halo {i} Rvir mismatch: binary={halos_binary.Rvir[i]:.6e}, hdf5={halos_hdf5.Rvir[i]:.6e}"
        )

    print(f"  ✓ Sample halo properties match (checked {n_sample} halos)")
    print(f"  Binary file size: {binary_file.stat().st_size:,} bytes")
    print(f"  HDF5 file size:   {hdf5_file.stat().st_size:,} bytes")

    # Calculate size ratio
    size_ratio = hdf5_file.stat().st_size / binary_file.stat().st_size
    print(f"  Size ratio (HDF5/binary): {size_ratio:.2f}x")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    print("Integration Test: Output Formats")
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"ERROR: Mimic executable not found: {MIMIC_EXE}")
        print("Build it first with: make")
        return 1

    tests = [
        test_binary_format_execution,
        test_binary_format_loading,
        # test_binary_baseline_comparison removed - binary format can't handle schema evolution
        # Core determinism validated via HDF5 baseline + format equivalence tests
        test_hdf5_format_execution,
        test_hdf5_format_loading,
        test_hdf5_baseline_comparison,
        test_format_equivalence,
    ]

    passed = 0
    failed = 0
    skipped = 0

    # ANSI color codes
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[0;33m'
    NC = '\033[0m'  # No Color

    for test in tests:
        print()
        try:
            # Capture print output to detect skips
            import io
            from contextlib import redirect_stdout

            output_buffer = io.StringIO()
            with redirect_stdout(output_buffer):
                test()

            output = output_buffer.getvalue()
            print(output, end='')  # Print the output

            if "Skipping" in output:
                print(f"{YELLOW}⊘ SKIP: {test.__name__}{NC}")
                skipped += 1
            else:
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
    print("Test Summary: Output Formats")
    print("=" * 60)
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Skipped: {skipped}")
    print(f"Total:   {passed + failed + skipped}")
    print("=" * 60)

    if failed == 0:
        print(f"{GREEN}✓ All executed tests passed!{NC}")
        if skipped > 0:
            print(f"{YELLOW}⊘ {skipped} test(s) skipped (HDF5 support not available){NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
