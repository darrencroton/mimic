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
from io import StringIO
from pathlib import Path
import numpy as np

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import load_binary_halos

# Repository paths
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"

# Core halo properties (physics-agnostic, always present)
# These 24 properties are defined in metadata/properties/halo_properties.yaml
# and should be present in all Mimic output, regardless of enabled physics modules
CORE_HALO_PROPERTIES = {
    'SnapNum', 'Type', 'HaloIndex', 'CentralHaloIndex', 'MimicHaloIndex',
    'MimicTreeIndex', 'SimulationHaloIndex', 'MergeStatus', 'mergeIntoID',
    'mergeIntoSnapNum', 'dT', 'Pos', 'Vel', 'Spin', 'Len', 'Mvir',
    'CentralMvir', 'Rvir', 'Vvir', 'Vmax', 'VelDisp', 'infallMvir',
    'infallVvir', 'infallVmax'
}


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


def compare_halos_comprehensive(halos1, halos2, label1="dataset1", label2="dataset2", rtol=1e-6, properties_to_compare=None):
    """
    Comprehensive comparison of all properties for all halos between two datasets.

    Compares every property of every halo, using appropriate comparison methods:
    - Integers: Exact match required
    - Floats: Relative tolerance (default 1e-6)
    - Vectors: Component-wise relative tolerance
    - Sentinels: Must match exactly (e.g., -1 for unset values)

    Args:
        halos1: First halo array (numpy recarray)
        halos2: Second halo array (numpy recarray)
        label1: Descriptive label for first dataset (e.g., "test")
        label2: Descriptive label for second dataset (e.g., "baseline")
        rtol: Relative tolerance for floating-point comparisons (default 1e-6)
        properties_to_compare: Optional set of property names to compare.
                             If None, compares all common properties.
                             If provided, only compares properties in this set.

    Returns:
        tuple: (passed, report_text) where passed is bool and report_text is str
    """
    report = StringIO()
    all_passed = True

    # Check halo counts match
    if len(halos1) != len(halos2):
        report.write(f"\n❌ HALO COUNT MISMATCH:\n")
        report.write(f"  {label1}: {len(halos1)} halos\n")
        report.write(f"  {label2}: {len(halos2)} halos\n")
        return False, report.getvalue()

    n_halos = len(halos1)

    # Get all property names
    props1 = set(halos1.dtype.names)
    props2 = set(halos2.dtype.names)

    # Determine which properties to compare
    if properties_to_compare is not None:
        # Filter to requested properties that exist in both datasets
        common_props = sorted((props1 & props2) & properties_to_compare)

        # Report if some requested properties are missing
        missing_in_1 = properties_to_compare - props1
        missing_in_2 = properties_to_compare - props2

        report.write(f"\nComparing {n_halos} halos across {len(common_props)} core properties...\n")

        if missing_in_1:
            report.write(f"⚠️  Core properties missing in {label1}: {', '.join(sorted(missing_in_1))}\n")
            all_passed = False
        if missing_in_2:
            report.write(f"⚠️  Core properties missing in {label2}: {', '.join(sorted(missing_in_2))}\n")
            all_passed = False

        # Report non-core properties (informational only)
        extra_in_1 = props1 - properties_to_compare
        extra_in_2 = props2 - properties_to_compare
        if extra_in_1 or extra_in_2:
            report.write(f"ℹ️  Additional properties present but not compared:\n")
            if extra_in_1:
                report.write(f"  In {label1}: {', '.join(sorted(extra_in_1))}\n")
            if extra_in_2:
                report.write(f"  In {label2}: {', '.join(sorted(extra_in_2))}\n")
    else:
        # Compare all common properties
        common_props = sorted(props1 & props2)
        report.write(f"\nComparing {n_halos} halos across all {len(common_props)} properties...\n")

        if props1 != props2:
            report.write(f"\n⚠️  Property sets differ:\n")
            only_in_1 = props1 - props2
            only_in_2 = props2 - props1
            if only_in_1:
                report.write(f"  Only in {label1}: {', '.join(sorted(only_in_1))}\n")
            if only_in_2:
                report.write(f"  Only in {label2}: {', '.join(sorted(only_in_2))}\n")

    for prop_name in common_props:
        arr1 = halos1[prop_name]
        arr2 = halos2[prop_name]

        # Determine property type
        dtype = arr1.dtype

        # Handle vector properties (3-component arrays)
        if len(arr1.shape) > 1 and arr1.shape[1] == 3:
            # Vector property (Pos, Vel, Spin)
            mismatches = []
            for component in range(3):
                comp_name = ['x', 'y', 'z'][component]

                # Compare with relative tolerance for floats
                if not np.allclose(arr1[:, component], arr2[:, component], rtol=rtol, atol=0, equal_nan=False):
                    # Find which halos differ
                    diffs = ~np.isclose(arr1[:, component], arr2[:, component], rtol=rtol, atol=0, equal_nan=False)
                    for halo_idx in np.where(diffs)[0]:
                        val1 = arr1[halo_idx, component]
                        val2 = arr2[halo_idx, component]
                        rel_diff = abs(val1 - val2) / (abs(val2) + 1e-30)  # Avoid division by zero
                        mismatches.append((halo_idx, comp_name, val1, val2, rel_diff))

            if mismatches:
                all_passed = False
                report.write(f"\n❌ Property '{prop_name}' mismatches (showing first 10 of {len(mismatches)}):\n")
                for halo_idx, comp, v1, v2, rel_diff in mismatches[:10]:
                    report.write(f"  Halo {halo_idx} [{comp}]: {label1}={v1:.6e}, {label2}={v2:.6e} (rel_diff={rel_diff:.2e})\n")
                if len(mismatches) > 10:
                    report.write(f"  ... and {len(mismatches) - 10} more mismatches\n")
                report.write(f"  Summary: {len(mismatches)} component mismatches across {n_halos * 3} total components ({100.0 * len(mismatches) / (n_halos * 3):.2f}%)\n")

        # Handle scalar integer properties
        elif np.issubdtype(dtype, np.integer):
            # Exact comparison for integers
            if not np.array_equal(arr1, arr2):
                diffs = arr1 != arr2
                diff_indices = np.where(diffs)[0]

                all_passed = False
                report.write(f"\n❌ Property '{prop_name}' mismatches (showing first 10 of {len(diff_indices)}):\n")
                for halo_idx in diff_indices[:10]:
                    val1 = arr1[halo_idx]
                    val2 = arr2[halo_idx]
                    report.write(f"  Halo {halo_idx}: {label1}={val1}, {label2}={val2}\n")
                if len(diff_indices) > 10:
                    report.write(f"  ... and {len(diff_indices) - 10} more mismatches\n")
                report.write(f"  Summary: {len(diff_indices)} of {n_halos} halos differ ({100.0 * len(diff_indices) / n_halos:.2f}%)\n")

        # Handle scalar floating-point properties
        elif np.issubdtype(dtype, np.floating):
            # Relative tolerance comparison for floats
            if not np.allclose(arr1, arr2, rtol=rtol, atol=0, equal_nan=False):
                diffs = ~np.isclose(arr1, arr2, rtol=rtol, atol=0, equal_nan=False)
                diff_indices = np.where(diffs)[0]

                all_passed = False
                report.write(f"\n❌ Property '{prop_name}' mismatches (showing first 10 of {len(diff_indices)}):\n")
                for halo_idx in diff_indices[:10]:
                    val1 = arr1[halo_idx]
                    val2 = arr2[halo_idx]
                    # Calculate relative difference safely
                    if val2 == 0.0:
                        rel_diff = abs(val1)  # Absolute difference when baseline is zero
                    else:
                        rel_diff = abs(val1 - val2) / abs(val2)
                    report.write(f"  Halo {halo_idx}: {label1}={val1:.6e}, {label2}={val2:.6e} (rel_diff={rel_diff:.2e})\n")
                if len(diff_indices) > 10:
                    report.write(f"  ... and {len(diff_indices) - 10} more mismatches\n")
                report.write(f"  Summary: {len(diff_indices)} of {n_halos} halos differ ({100.0 * len(diff_indices) / n_halos:.2f}%)\n")

        else:
            # Unknown type - try exact comparison
            if not np.array_equal(arr1, arr2):
                report.write(f"\n⚠️  Property '{prop_name}' (type {dtype}) differs but comparison method unknown\n")
                all_passed = False

    if all_passed:
        report.write(f"\n✓ All {len(common_props)} properties match for all {n_halos} halos\n")

    return all_passed, report.getvalue()


def test_binary_format_execution():
    """
    Test that Mimic runs successfully with binary output format

    What: Executes Mimic with test_binary.par (binary output format)
    Expected: Zero exit code, no crashes
    Validates: Binary format end-to-end execution
    """
    print("Testing binary format execution...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    param_file = TEST_DATA_DIR / "test_binary.par"
    assert param_file.exists(), f"Parameter file not found: {param_file}"

    # Run Mimic
    returncode, _stdout, stderr = run_mimic(param_file)

    # Check execution success
    assert returncode == 0, f"Mimic failed with code {returncode}\nSTDERR: {stderr}"

    print(f"  ✓ Binary format execution successful")


def test_binary_format_loading():
    """
    Test that binary output file can be loaded and parsed

    What: Loads model_z0.000_0 using load_binary_halos() framework function
    Expected: File exists, halos array is populated, metadata is valid
    Validates: Binary format structure is readable by analysis tools
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
    print(f"  Loading: {output_file.relative_to(REPO_ROOT)}")
    halos, metadata = load_binary_halos(output_file)

    # Validate loaded data
    assert metadata['TotHalos'] > 0, "No halos loaded from binary file"
    assert len(halos) == metadata['TotHalos'], "Halo count mismatch"
    assert hasattr(halos, 'Mvir'), "Binary data missing expected property (Mvir)"
    assert hasattr(halos, 'Rvir'), "Binary data missing expected property (Rvir)"

    print(f"  ✓ Loaded {metadata['TotHalos']} halos from CURRENT binary output")
    print(f"    Trees: {metadata.get('Ntrees', 'N/A')}, File size: {output_file.stat().st_size:,} bytes")


def test_binary_baseline_comparison():
    """
    CURRENTLY DISABLED - Binary baseline comparison

    Status: This test is disabled because binary format cannot handle schema evolution.
            The binary format is not self-describing, so the loader uses the current
            dtype (from generated code) which may differ from the baseline's dtype.

    Alternative validation:
            - test_hdf5_baseline_comparison validates core property determinism
            - test_format_equivalence validates binary matches HDF5 output
            Together, these provide complete binary output validation.

    Original purpose: Compare current binary output against committed baseline (core only)

    Kept for: Potential future use if binary format versioning is implemented
    """
    print("Testing binary baseline comparison...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Load current test output
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"

    # Run Mimic if needed
    if not output_file.exists():
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    print(f"  Loading CURRENT: {output_file.relative_to(REPO_ROOT)}")
    halos_test, metadata_test = load_binary_halos(output_file)
    print(f"    → {metadata_test['TotHalos']} halos, {metadata_test.get('Ntrees', 'N/A')} trees")

    # Load committed baseline
    baseline_dir = TEST_DATA_DIR / "output" / "baseline" / "binary"
    baseline_file = baseline_dir / "model_z0.000_0"

    assert baseline_file.exists(), (
        f"Baseline file not found: {baseline_file}\n"
        f"Run Mimic once to establish baseline, then commit the baseline file."
    )

    print(f"  Loading BASELINE: {baseline_file.relative_to(REPO_ROOT)}")
    halos_baseline, metadata_baseline = load_binary_halos(baseline_file)
    print(f"    → {metadata_baseline['TotHalos']} halos, {metadata_baseline.get('Ntrees', 'N/A')} trees")

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

    # Comprehensive comparison of core properties for all halos
    # Only compare core (physics-agnostic) properties since baseline may have
    # been generated with different physics modules enabled
    print(f"  Comparing all core properties for all halos...")
    passed, report = compare_halos_comprehensive(
        halos_test, halos_baseline,
        label1="test", label2="baseline",
        rtol=1e-6,
        properties_to_compare=CORE_HALO_PROPERTIES
    )

    # Print report
    print(report)

    # Assert that comparison passed
    assert passed, (
        f"Binary output does not match baseline.\n"
        f"In physics-free mode, all core halo properties should be identical.\n"
        f"See detailed comparison report above."
    )

    print(f"  ✓ Binary output matches baseline - all core properties validated")


def test_hdf5_format_execution():
    """
    Test that Mimic runs successfully with HDF5 output format

    What: Executes Mimic with test_hdf5.par (HDF5 output format)
    Expected: Zero exit code, no crashes (or skips if HDF5 not compiled)
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
    Test that HDF5 output file can be loaded and parsed

    What: Loads model_000.hdf5 using load_hdf5_halos() function
    Expected: File exists, halos array is populated, metadata is valid
    Validates: HDF5 format structure is readable by analysis tools
    Requires: h5py library (skips if not available)
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
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        print(f"  Skipping detailed validation (h5py not available)")
        print(f"  Install with: pip install h5py")
        return

    # Load halos
    print(f"  Loading: {output_file.relative_to(REPO_ROOT)}")
    halos, metadata = load_hdf5_halos(output_file)

    # Validate loaded data
    assert metadata['TotHalos'] > 0, "No halos loaded from HDF5 file"
    assert len(halos) == metadata['TotHalos'], "Halo count mismatch"

    print(f"  ✓ Loaded {metadata['TotHalos']} halos from CURRENT HDF5 output")
    print(f"    File size: {output_file.stat().st_size:,} bytes")


def test_hdf5_baseline_comparison():
    """
    Test that current HDF5 output matches committed baseline (core properties only)

    What: Compares tests/data/output/hdf5/model_000.hdf5 (current test run)
          against tests/data/output/baseline/hdf5/model_000.hdf5 (committed baseline)

    Comparison: All 24 CORE halo properties for ALL halos
                (Physics-agnostic properties only: Mvir, Rvir, Pos, Vel, etc.)
                Module properties (ColdGas, StellarMass) are NOT compared

    Tolerance: 1e-6 relative for floats, exact for integers

    Expected: All core properties match exactly (within tolerance)

    Validates: Core halo tracking is deterministic and hasn't regressed

    Requires: h5py library (skips if not available)

    Note: If this test fails after a deliberate core change, regenerate baseline with:
          cp tests/data/output/hdf5/model_000.hdf5 tests/data/output/baseline/hdf5/
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
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        print(f"  Skipping (h5py not available)")
        return

    # Load current test output
    output_dir = TEST_DATA_DIR / "output" / "hdf5"
    output_file = output_dir / "model_000.hdf5"

    # Run Mimic if needed
    if not output_file.exists():
        param_file = TEST_DATA_DIR / "test_hdf5.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic execution failed: {stderr}"

    print(f"  Loading CURRENT: {output_file.relative_to(REPO_ROOT)}")
    halos_test, metadata_test = load_hdf5_halos(output_file)
    print(f"    → {metadata_test['TotHalos']} halos")

    # Load committed baseline
    baseline_dir = TEST_DATA_DIR / "output" / "baseline" / "hdf5"
    baseline_file = baseline_dir / "model_000.hdf5"

    assert baseline_file.exists(), (
        f"Baseline file not found: {baseline_file}\n"
        f"Run Mimic once with HDF5 to establish baseline, then commit the baseline file."
    )

    print(f"  Loading BASELINE: {baseline_file.relative_to(REPO_ROOT)}")
    halos_baseline, metadata_baseline = load_hdf5_halos(baseline_file)
    print(f"    → {metadata_baseline['TotHalos']} halos")

    # Compare halo counts
    assert metadata_test['TotHalos'] == metadata_baseline['TotHalos'], (
        f"Halo count mismatch: test={metadata_test['TotHalos']}, "
        f"baseline={metadata_baseline['TotHalos']}"
    )

    # Comprehensive comparison of core properties for all halos
    # Only compare core (physics-agnostic) properties since baseline may have
    # been generated with different physics modules enabled
    print(f"  Comparing all core properties for all halos...")
    passed, report = compare_halos_comprehensive(
        halos_test, halos_baseline,
        label1="test", label2="baseline",
        rtol=1e-6,
        properties_to_compare=CORE_HALO_PROPERTIES
    )

    # Print report
    print(report)

    # Assert that comparison passed
    assert passed, (
        f"HDF5 output does not match baseline.\n"
        f"In physics-free mode, all core halo properties should be identical.\n"
        f"See detailed comparison report above."
    )

    print(f"  ✓ HDF5 output matches baseline - all core properties validated")


def test_format_equivalence():
    """
    Test that binary and HDF5 formats produce identical output (all properties)

    What: Compares tests/data/output/binary/model_z0.000_0 (binary format)
          against tests/data/output/hdf5/model_000.hdf5 (HDF5 format)
          Both generated in same test run with same modules enabled

    Comparison: ALL properties for ALL halos (core + modules)
                Including: ColdGas, StellarMass, and any other module properties

    Tolerance: 1e-6 relative for floats, exact for integers

    Expected: Perfect agreement - both formats write identical values

    Validates: Format consistency - output format doesn't affect results

    Requires: h5py library (skips if not available)

    Note: This test compares ALL properties because both files are generated
          in the same run, so they should have identical property sets
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
        import h5py  # noqa: F401 - import used only for availability check
    except ImportError:
        print(f"  Skipping (h5py not available)")
        return

    # Load current binary output
    binary_dir = TEST_DATA_DIR / "output" / "binary"
    binary_file = binary_dir / "model_z0.000_0"

    # Run Mimic if needed
    if not binary_file.exists():
        print(f"  Running Mimic to generate binary output...")
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"Binary Mimic execution failed: {stderr}"

    print(f"  Loading BINARY: {binary_file.relative_to(REPO_ROOT)}")
    halos_binary, metadata_binary = load_binary_halos(binary_file)
    print(f"    → {metadata_binary['TotHalos']} halos")

    # Load current HDF5 output
    hdf5_dir = TEST_DATA_DIR / "output" / "hdf5"
    hdf5_file = hdf5_dir / "model_000.hdf5"

    # Run Mimic if needed
    if not hdf5_file.exists():
        print(f"  Running Mimic to generate HDF5 output...")
        param_file = TEST_DATA_DIR / "test_hdf5.par"
        returncode, _, stderr = run_mimic(param_file)
        assert returncode == 0, f"HDF5 Mimic execution failed: {stderr}"

    print(f"  Loading HDF5: {hdf5_file.relative_to(REPO_ROOT)}")
    halos_hdf5, metadata_hdf5 = load_hdf5_halos(hdf5_file)
    print(f"    → {metadata_hdf5['TotHalos']} halos")

    # Compare halo counts
    assert metadata_binary['TotHalos'] == metadata_hdf5['TotHalos'], (
        f"Halo count mismatch: binary={metadata_binary['TotHalos']}, "
        f"hdf5={metadata_hdf5['TotHalos']}"
    )

    print(f"  ✓ Halo count matches: {metadata_binary['TotHalos']} halos in both formats")

    # Comprehensive comparison of all properties for all halos
    print(f"  Comparing all properties for all halos between formats...")
    passed, report = compare_halos_comprehensive(
        halos_binary, halos_hdf5,
        label1="binary", label2="HDF5",
        rtol=1e-6
    )

    # Print report
    print(report)

    # Assert that comparison passed
    assert passed, (
        f"Binary and HDF5 formats do not produce identical output.\n"
        f"Both formats should write exactly the same property values.\n"
        f"See detailed comparison report above."
    )

    print(f"  ✓ Binary and HDF5 formats produce identical output - all properties validated")

    # Print file size comparison (informational)
    print(f"  Binary file size: {binary_file.stat().st_size:,} bytes")
    print(f"  HDF5 file size:   {hdf5_file.stat().st_size:,} bytes")
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

    # Testing Strategy:
    # - HDF5 baseline test validates core property determinism
    # - Format equivalence test validates binary matches HDF5 output
    # - Binary baseline test is DISABLED: binary format cannot handle schema evolution
    #   (not self-describing, loader uses current dtype which may differ from baseline)

    tests = [
        test_binary_format_execution,
        test_binary_format_loading,
        # test_binary_baseline_comparison,  # DISABLED: See note above
        test_hdf5_format_execution,
        test_hdf5_format_loading,
        test_hdf5_baseline_comparison,  # Validates core property determinism
        test_format_equivalence,  # Validates binary matches HDF5 (all properties)
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
            # Print captured output before showing failure
            output = output_buffer.getvalue()
            if output:
                print(output, end='')
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            # Print captured output before showing error
            output = output_buffer.getvalue()
            if output:
                print(output, end='')
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
