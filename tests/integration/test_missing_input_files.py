#!/usr/bin/env python3
"""
Missing input file handling of the vertical driver

Validates: a vertical run with no input file at all refuses to start, and a run with some
requested input files missing warns once with a summary and processes the rest.

A mistyped first_file/last_file or SimulationDir used to log one INFO line per missing file and
finish "successfully" with an empty output. The driver now treats an empty range as fatal and
reports a partial range as a single warning (src/core/vertical_driver.c).
"""

import shutil
import sys
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    MIMIC_EXE,
    TestNotApplicable,
    TestSkipped,
    compiled_simulation,
    create_test_param_file,
    default_simulation,
    not_applicable_if_selected_package_is_horizontal,
    run_mimic,
    run_test_suite,
)


def _require_default_vertical_build():
    """Require a built mimic on the default simulation package.

    The horizontal driver has no file range, and the two cases assume the default package's
    per-file L-Halo layout with tree file 0 present and file 1 absent: Consistent-Trees ASCII
    ignores the range and Consistent-Trees HDF5 refuses it in the reader, so neither reaches
    the driver's check.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    not_applicable_if_selected_package_is_horizontal("a first_file..last_file input range")
    if compiled_simulation() != default_simulation():
        raise TestNotApplicable(
            f"selected package {compiled_simulation()} is not {default_simulation()}, whose "
            "per-file layout these cases assume"
        )


def _param_file_with_file_range(output_name, first_file, last_file):
    """Create a run file whose input range is first_file..last_file.

    The generated run files carry an explicit ``input:`` range, which takes precedence over the
    range create_test_param_file() writes into the simulation config, so the range is set in the
    run file as well (the same step test_unique_galaxy_id_encoding.py takes).
    """
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name, first_file=first_file, last_file=last_file
    )
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)
    config.setdefault("input", {})
    config["input"]["first_file"] = first_file
    config["input"]["last_file"] = last_file
    with open(param_file, "w") as handle:
        yaml.dump(config, handle, default_flow_style=False, sort_keys=False)
    return param_file, output_dir, temp_dir


def test_no_input_file_is_fatal():
    """
    Test that a range naming no existing input file stops the run before any processing.

    Expected: non-zero exit, and a fatal message naming first_file, last_file and
    SimulationDir.
    Validates: the vertical driver's no-input check (check_partition_availability).
    """
    print("Testing a first_file/last_file range with no input file...")

    # ===== SETUP =====
    _require_default_vertical_build()
    param_file, output_dir, temp_dir = _param_file_with_file_range(
        "missing_inputs_none", first_file=900, last_file=901
    )

    try:
        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        output = stdout + stderr
        assert returncode != 0, "A run with no input file must fail, not finish empty"
        assert "No input file exists" in output, f"Missing the fatal message:\n{output}"
        for needle in ("first_file=900", "last_file=901", "SimulationDir"):
            assert needle in output, f"Fatal message should name '{needle}':\n{output}"

        print("  ✓ No input file in range 900..901 is fatal and names the range and directory")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def test_partly_missing_input_files_warn_and_run():
    """
    Test that a range with some input files missing runs the present ones and warns once.

    Expected: exit 0 and one warning summarising how many requested files were missing. The
    shared test dataset ships tree file 0 only, so file 1 of a 0..1 range is absent.
    Validates: skip semantics are kept, and the summary reports 1 of 2 files missing.
    """
    print("Testing a first_file/last_file range with one input file missing...")

    # ===== SETUP =====
    _require_default_vertical_build()
    param_file, output_dir, temp_dir = _param_file_with_file_range(
        "missing_inputs_partial", first_file=0, last_file=1
    )

    try:
        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)

        # ===== VALIDATE =====
        output = stdout + stderr
        assert (
            returncode == 0
        ), f"A partly present range should still run (rc={returncode}):\n{output}"
        summary = "1 of 2 requested input files are missing and were skipped"
        assert output.count(summary) == 1, f"Expected exactly one warning '{summary}':\n{output}"

        print("  ✓ Partly missing range runs and warns once: 1 of 2 input files missing")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_no_input_file_is_fatal,
            test_partly_missing_input_files_warn_and_run,
        ],
        "Missing Input Files (test_missing_input_files.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
