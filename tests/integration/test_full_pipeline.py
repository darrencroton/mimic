#!/usr/bin/env python3
"""
Full Pipeline Integration Test

Validates: Complete Mimic execution from input to output

This test validates that the full Mimic pipeline executes successfully:
- Reads parameter file correctly
- Loads merger tree data
- Processes trees without errors
- Writes output files
- Completes with zero memory leaks
- Produces expected output structure

"""

import sys
import tempfile
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    MIMIC_EXE,
    REPO_ROOT,
    TEST_DATA_DIR,
    TestSkipped,
    check_no_memory_leaks,
    core_input_file,
    ensure_output_dirs,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
    run_mimic_fresh,
)

# Ensure output directories exist before any tests run
ensure_output_dirs()

# ANSI color codes (module-level constants)
BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
YELLOW = "\033[1;33m"
NC = "\033[0m"


def test_basic_execution():
    """
    Test that Mimic executes successfully

    Expected: Exit code 0, no crashes
    Validates: Basic pipeline execution
    """
    print("Testing basic Mimic execution...")

    # Run Mimic on test parameter file
    param_file = core_input_file("test_binary.yaml")
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

    # Expected output location (from test_binary.yaml: writes to binary/)
    # Binary format uses redshift-based naming: model_z{redshift}_{filenr}
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    # Always regenerate output for the selected model so a stale file from a
    # previous run (possibly a different MODEL writing the same path) cannot
    # satisfy this assertion.
    param_file = core_input_file("test_binary.yaml")
    run_mimic_fresh(param_file, output_file)

    # Check output file exists
    assert output_file.exists(), f"{RED}Output file not created: {output_file}{NC}"

    # Check file size is reasonable (not empty)
    file_size = output_file.stat().st_size
    assert file_size > 0, f"{RED}Output file is empty{NC}"
    print(f"  ✓ Output file created: {output_file}")
    print(f"  File size: {file_size:,} bytes")


def test_output_directory_created_if_missing():
    """
    Test that Mimic creates missing output directories from the parameter file

    Expected: Nested output directory and output file are created automatically
    Validates: Runtime output directory creation
    """
    print("Testing automatic output directory creation...")

    source_param_file = core_input_file("test_binary.yaml")
    assert (
        source_param_file.exists()
    ), f"{RED}Test parameter file not found: {source_param_file}{NC}"

    with tempfile.TemporaryDirectory(prefix="mimic_output_dir_") as temp_root:
        temp_root_path = Path(temp_root)
        output_dir = temp_root_path / "nested" / "results"
        param_file = temp_root_path / "test_output_dir.yaml"

        with open(source_param_file, "r", encoding="utf-8") as f:
            config = yaml.safe_load(f)

        config["output"]["output_directory"] = str(output_dir)

        with open(param_file, "w", encoding="utf-8") as f:
            yaml.safe_dump(config, f, sort_keys=False)

        returncode, stdout, stderr = run_mimic(param_file)
        if returncode != 0:
            print(f"STDOUT:\n{stdout}")
            print(f"STDERR:\n{stderr}")
            assert False, f"{RED}Mimic failed to create missing output directory{NC}"

        output_file = output_dir / "model_z0.000_0"
        metadata_dir = output_dir / "metadata"

        assert output_dir.exists(), f"{RED}Output directory not created: {output_dir}{NC}"
        assert (
            output_file.exists()
        ), f"{RED}Output file not created in new directory: {output_file}{NC}"
        assert metadata_dir.exists(), f"{RED}Metadata directory not created: {metadata_dir}{NC}"

    print("  ✓ Mimic created missing output and metadata directories")


def test_no_memory_leaks():
    """
    Test that Mimic runs without memory leaks

    Expected: No leak report in the run's captured output
    Validates: Memory management correctness
    """
    print("Testing for memory leaks...")

    # Run Mimic; the allocator's leak report goes to the run's own output
    param_file = core_input_file("test_binary.yaml")
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    assert check_no_memory_leaks(stdout, stderr), f"{RED}Memory leaks detected in Mimic run{NC}"

    print("  ✓ No memory leaks detected")


def test_output_loadable():
    """
    Test that output file can be loaded and has valid structure

    Expected: Binary file has header and data
    Validates: Output format integrity
    """
    print("Testing output file structure...")

    # Expected output file (test_binary.yaml writes to binary/)
    # Binary format uses redshift-based naming: model_z{redshift}_{filenr}
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"  # snapshot 63 is z=0

    # Always regenerate output for the selected model so a stale file from a
    # previous run cannot satisfy this assertion.
    param_file = core_input_file("test_binary.yaml")
    run_mimic_fresh(param_file, output_file)

    # Try to load output file
    # Note: This is a basic check - just verify we can read binary data
    with open(output_file, "rb") as f:
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
    print("Testing stdout content...")

    param_file = core_input_file("test_binary.yaml")
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    # Check for key messages
    assert "Mimic" in stdout or "Mimic" in stderr, f"{RED}Should mention 'Mimic' in output{NC}"

    # Check for processing messages
    # (specific messages may vary, this is a basic check)
    output_combined = stdout + stderr
    assert len(output_combined) > 0, f"{RED}Should produce some output{NC}"

    print("  ✓ Stdout contains expected content")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    """
    # Print test suite header
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: Full Pipeline (test_full_pipeline.py){NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()
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
        test_output_directory_created_if_missing,
        test_no_memory_leaks,
        test_output_loadable,
        test_stdout_content,
    ]

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
