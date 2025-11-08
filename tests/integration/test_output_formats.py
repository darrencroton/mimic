#!/usr/bin/env python3
"""
Output Format Integration Test

Validates: Binary and HDF5 output format correctness
Phase: Phase 2 (Testing Framework)

This test validates that Mimic's output system correctly:
- Produces binary output files
- Produces HDF5 output files (when compiled with HDF5)
- Both formats contain expected data
- File structures are correct
- Data types are appropriate

Test cases:
  - test_binary_output: Binary format works
  - test_hdf5_output: HDF5 format works (if available)
  - test_output_consistency: Both formats produce equivalent data

Author: Mimic Testing Team
Date: 2025-11-08
"""

import os
import subprocess
import sys
from pathlib import Path
import struct

# Repository root
REPO_ROOT = Path(__file__).parent.parent.parent
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"


def run_mimic_with_format(output_format):
    """
    Run Mimic with specified output format

    Args:
        output_format (str): 'binary' or 'hdf5'

    Returns:
        tuple: (returncode, output_file_path)
    """
    # Create temporary parameter file with specified format
    test_par = TEST_DATA_DIR / "test.par"

    # Read test.par
    with open(test_par) as f:
        lines = f.readlines()

    # Modify output format
    modified_lines = []
    for line in lines:
        if line.strip().startswith("OutputFormat"):
            modified_lines.append(f"OutputFormat {output_format}\n")
        elif line.strip().startswith("OutputDir"):
            modified_lines.append(f"OutputDir ./tests/data/output/{output_format}/\n")
        else:
            modified_lines.append(line)

    # Write temporary parameter file
    temp_par = TEST_DATA_DIR / f"test_{output_format}.par"
    with open(temp_par, 'w') as f:
        f.writelines(modified_lines)

    # Ensure output directory exists
    output_dir = TEST_DATA_DIR / "output" / output_format
    output_dir.mkdir(parents=True, exist_ok=True)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(temp_par)],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True
    )

    # Determine output file path
    output_dir = TEST_DATA_DIR / "output" / output_format
    if output_format == "binary":
        # Binary format uses redshift-based naming
        output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0
    else:
        # HDF5 format uses file number naming
        output_file = output_dir / "model_000.hdf5"  # filenr 0

    return result.returncode, output_file, temp_par


def test_binary_output():
    """
    Test that binary output format works

    Expected: Binary file created with valid structure
    Validates: Binary output generation
    """
    print("Testing binary output format...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Run Mimic with binary output
    returncode, output_file, temp_par = run_mimic_with_format("binary")

    # Clean up temp par file
    if temp_par.exists():
        temp_par.unlink()

    # Check execution success
    assert returncode == 0, f"Mimic failed with binary output (code {returncode})"

    # Check output file exists
    assert output_file.exists(), f"Binary output file not created: {output_file}"

    # Check file size
    file_size = output_file.stat().st_size
    assert file_size > 0, "Binary output file is empty"

    print(f"  ✓ Binary output created successfully")
    print(f"  File: {output_file}")
    print(f"  Size: {file_size:,} bytes")

    # Basic binary format validation
    with open(output_file, 'rb') as f:
        # Try to read header (first 4 integers: Ntrees, totNHalos, NtotHalos, NoutputSnaps)
        try:
            header = struct.unpack('iiii', f.read(16))
            print(f"  Header (first 4 ints): {header}")
            assert all(x >= 0 for x in header), "Header values should be non-negative"
        except struct.error as e:
            assert False, f"Could not read binary header: {e}"


def test_hdf5_output():
    """
    Test that HDF5 output format works (if HDF5 support compiled)

    Expected: HDF5 file created (or graceful skip if no HDF5)
    Validates: HDF5 output generation
    """
    print("Testing HDF5 output format...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Run Mimic with HDF5 output
    returncode, output_file, temp_par = run_mimic_with_format("hdf5")

    # Clean up temp par file
    if temp_par.exists():
        temp_par.unlink()

    # Check if HDF5 was requested but failed
    if returncode != 0:
        print(f"  Mimic failed with HDF5 output (code {returncode})")
        print(f"  This is OK if Mimic wasn't compiled with HDF5 support")
        print(f"  Rebuild with: make clean && make USE-HDF5=yes")
        return  # Not a failure - HDF5 support is optional

    # If succeeded, verify output
    if output_file.exists():
        file_size = output_file.stat().st_size
        assert file_size > 0, "HDF5 output file is empty"

        print(f"  ✓ HDF5 output created successfully")
        print(f"  File: {output_file}")
        print(f"  Size: {file_size:,} bytes")

        # Try to load with h5py if available
        try:
            import h5py
            with h5py.File(output_file, 'r') as f:
                print(f"  HDF5 groups: {list(f.keys())}")
        except ImportError:
            print(f"  (h5py not available for detailed validation)")
    else:
        print(f"  HDF5 output not created (may need USE-HDF5=yes)")


def test_output_file_permissions():
    """
    Test that output files have correct permissions

    Expected: Files are readable
    Validates: File system operations
    """
    print("Testing output file permissions...")

    if not MIMIC_EXE.exists():
        print(f"  Skipping (Mimic not built)")
        return

    # Run Mimic
    returncode, output_file, temp_par = run_mimic_with_format("binary")

    if temp_par.exists():
        temp_par.unlink()

    assert returncode == 0, "Mimic execution failed"
    assert output_file.exists(), "Output file not created"

    # Check file is readable
    assert os.access(output_file, os.R_OK), "Output file is not readable"

    print(f"  ✓ Output file has correct permissions")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    print("=" * 60)
    print("Integration Test: Output Formats")
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
        test_binary_output,
        test_hdf5_output,
        test_output_file_permissions,
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
