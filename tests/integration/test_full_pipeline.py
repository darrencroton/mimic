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
    NC,
    RED,
    REPO_ROOT,
    TEST_DATA_DIR,
    check_no_memory_leaks,
    core_input_file,
    ensure_output_dirs,
    run_mimic,
    run_mimic_fresh,
    run_test_suite,
)

# Ensure output directories exist before any tests run
ensure_output_dirs()


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
    Test that Mimic logs its key run milestones

    Expected: startup, processing, and completion milestones in output
    Validates: the run-lifecycle messages emitted in src/core/main.c and
               src/core/tree_driver.c stay present and stable

    Only milestones that appear in both default and verbose modes are checked,
    because run_mimic() runs with --verbose (which replaces the default
    configuration/output lines with section banners).
    """
    print("Testing stdout content...")

    param_file = core_input_file("test_binary.yaml")
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"{RED}Mimic execution failed{NC}"

    output_combined = stdout + stderr
    for milestone in (
        "Mimic Galaxy Evolution Framework",  # startup banner
        "Processing 1 input file",  # tree driver begins
        "Mimic completed successfully",  # clean completion
    ):
        assert milestone in output_combined, f"{RED}Missing run milestone: '{milestone}'{NC}"

    print("  ✓ Stdout contains expected run milestones")


def test_memory_profile_survives_quiet_mode():
    """
    Test that the run-end memory profile is reported even under --quiet

    Expected: the profile block and its four terms appear in a --quiet run
    Validates: print_run_memory_profile() (src/util/run_profile.c) reports
               regardless of the log threshold

    The profile records peak RSS and the C/P/G terms the snapshot driver's memory
    projection is parametric in. Those measurements cannot be recovered without
    repeating the run, and --quiet is the documented mode for batch and
    production runs -- exactly the runs whose memory is worth measuring. Because
    the block is emitted at INFO, a --quiet threshold of WARNING would silently
    discard all of it, so the report deliberately lowers and restores the
    threshold around itself. This test pins that behaviour; without it the
    regression is invisible until a long run finishes having measured nothing.

    run_mimic() always passes --verbose, so the run under test is invoked as
    "--verbose --quiet". Those two flags do not compete: --verbose only turns on
    the verbose message format, while --quiet is the only one of the pair that
    sets the log threshold. The threshold under test is therefore genuinely
    WARNING, which is the condition that matters, since the suppression this
    pins is purely a threshold comparison in log_message().
    """
    print("Testing memory profile under --quiet...")

    param_file = core_input_file("test_binary.yaml")
    returncode, stdout, stderr = run_mimic(param_file, extra_args=["--quiet"])
    assert returncode == 0, f"{RED}Mimic execution failed under --quiet{NC}"

    output_combined = stdout + stderr
    for line in (
        "Run memory profile",
        "Peak process RSS",
        "Output buffer capacity C",
        "Output population P",
        "Galaxy pool high-water G",
    ):
        assert line in output_combined, f"{RED}--quiet suppressed the profile line: '{line}'{NC}"

    print("  ✓ Memory profile is reported under --quiet")


def main():
    """Run this file's tests via the shared framework runner."""
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    return run_test_suite(
        [
            test_basic_execution,
            test_output_files_created,
            test_output_directory_created_if_missing,
            test_no_memory_leaks,
            test_output_loadable,
            test_stdout_content,
            test_memory_profile_survives_quiet_mode,
        ],
        "Full Pipeline (test_full_pipeline.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
