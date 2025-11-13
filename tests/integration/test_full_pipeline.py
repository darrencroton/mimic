#!/usr/bin/env python3
"""
Full Pipeline Integration Test

Validates: Complete Mimic execution from input to output
Phase: Phase 2 (Testing Framework)

This test validates that the full Mimic pipeline executes successfully:
- Reads parameter file correctly
- Loads merger tree data
- Processes trees without errors
- Writes output files
- Completes with zero memory leaks
- Produces expected output structure

Test cases:
  - test_basic_execution: Mimic runs to completion
  - test_output_files_created: Output files exist
  - test_no_memory_leaks: Zero memory leaks reported
  - test_output_loadable: Output files can be read

Author: Mimic Testing Team
Date: 2025-11-08
"""

import sys
from pathlib import Path

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    REPO_ROOT,
    TEST_DATA_DIR,
    MIMIC_EXE,
    ensure_output_dirs,
    run_mimic,
    check_no_memory_leaks,
)

# Ensure output directories exist before any tests run
ensure_output_dirs()


def test_basic_execution():
    """
    Test that Mimic executes successfully

    Expected: Exit code 0, no crashes
    Validates: Basic pipeline execution
    """
    # ANSI color codes
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    print("Testing basic Mimic execution...")

    # Run Mimic on test parameter file
    param_file = TEST_DATA_DIR / "test_binary.par"
    assert param_file.exists(), f"{RED}Test parameter file not found: {param_file}{NC}"

    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution success
    if returncode != 0:
        print(f"STDOUT:\n{stdout}")
        print(f"STDERR:\n{stderr}")
        assert False, f"{RED}Mimic execution failed with code {returncode}{NC}"

    print("  ✓ Mimic executed successfully")
    print(f"  Exit code: {returncode}")


def test_output_files_created():
    """
    Test that output files are created

    Expected: model_063.dat exists in output directory
    Validates: Output file generation
    """
    print("Testing output file creation...")

    # Expected output location (from test_binary.par: writes to binary/)
    # Binary format uses redshift-based naming: model_z{redshift}_{filenr}
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    # ANSI color codes
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    # Run Mimic if output doesn't exist
    if not output_file.exists():
        print("  Running Mimic to generate output...")
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    # Check output file exists
    assert output_file.exists(), f"{RED}Output file not created: {output_file}{NC}"

    # Check file size is reasonable (not empty)
    file_size = output_file.stat().st_size
    assert file_size > 0, f"{RED}Output file is empty{NC}"
    print(f"  ✓ Output file created: {output_file}")
    print(f"  File size: {file_size:,} bytes")


def test_no_memory_leaks():
    """
    Test that Mimic runs without memory leaks

    Expected: Zero memory leaks reported in logs
    Validates: Memory management correctness
    """
    # ANSI color codes
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    print("Testing for memory leaks...")

    # Run Mimic
    param_file = TEST_DATA_DIR / "test_binary.par"
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    # Check for memory leaks in output logs (test_binary.par writes to binary/)
    output_dir = TEST_DATA_DIR / "output" / "binary"
    has_leaks = not check_no_memory_leaks(output_dir)

    assert not has_leaks, f"{RED}Memory leaks detected in Mimic run{NC}"

    print("  ✓ No memory leaks detected")


def test_output_loadable():
    """
    Test that output file can be loaded and has valid structure

    Expected: Binary file has header and data
    Validates: Output format integrity
    """
    print("Testing output file structure...")

    # Expected output file (test_binary.par writes to binary/)
    # Binary format uses redshift-based naming: model_z{redshift}_{filenr}
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    # ANSI color codes
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    # Ensure output exists
    if not output_file.exists():
        param_file = TEST_DATA_DIR / "test_binary.par"
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    # Try to load output file
    # Note: This is a basic check - just verify we can read binary data
    with open(output_file, 'rb') as f:
        # Read first few bytes
        header = f.read(16)
        assert len(header) == 16, f"{RED}Could not read file header{NC}"

    print(f"  ✓ Output file is readable")
    print(f"  File: {output_file}")


def test_stdout_content():
    """
    Test that Mimic produces expected output messages

    Expected: Key progress messages in stdout
    Validates: Execution flow and logging
    """
    # ANSI color codes
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    print("Testing stdout content...")

    param_file = TEST_DATA_DIR / "test_binary.par"
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    # Check for key messages
    assert "Mimic" in stdout or "Mimic" in stderr, \
        f"{RED}Should mention 'Mimic' in output{NC}"

    # Check for processing messages
    # (specific messages may vary, this is a basic check)
    output_combined = stdout + stderr
    assert len(output_combined) > 0, f"{RED}Should produce some output{NC}"

    print("  ✓ Stdout contains expected content")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    # ANSI color codes
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    print("Integration Test: Full Pipeline")
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_basic_execution,
        test_output_files_created,
        test_no_memory_leaks,
        test_output_loadable,
        test_stdout_content,
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
    print("Test Summary: Full Pipeline")
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
