#!/usr/bin/env python3
"""
Integration tests for input.processing_order startup validation.
"""

import shutil
import sys
import tempfile
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import MIMIC_EXE, TestSkipped, create_test_param_file, run_mimic, run_test_suite

TEMP_DIR = None


def processing_order_param_file(processing_order):
    """
    Return a run file path with input.processing_order set to the given string.

    Generates a base test run file via create_test_param_file, rewrites
    input.processing_order in the config, and writes a named YAML to TEMP_DIR.
    """
    param_file, _output_dir, _ = create_test_param_file(
        output_name=f"processing_order_{processing_order}",
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )
    with open(param_file, "r") as handle:
        config = yaml.safe_load(handle)

    config.setdefault("input", {})["processing_order"] = processing_order

    rewritten = Path(TEMP_DIR) / f"{processing_order}.yaml"
    with open(rewritten, "w") as handle:
        yaml.safe_dump(config, handle, default_flow_style=False, sort_keys=False)
    return rewritten


def test_unknown_processing_order_fails_fast():
    """
    Test that an unrecognised input.processing_order value fails at startup.

    Expected: Non-zero exit; output includes the bad value name and "Valid values are tree_ordered, snapshot_ordered".
    Validates: startup validation rejects unknown ordering strings with an actionable message.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file = processing_order_param_file("not_a_real_ordering")
    returncode, stdout, stderr = run_mimic(param_file)
    output = stdout + stderr

    assert returncode != 0, "Unknown processing_order should fail startup validation"
    assert "Unknown input.processing_order 'not_a_real_ordering'" in output
    assert "Valid values are tree_ordered, snapshot_ordered" in output


def test_snapshot_ordered_reports_unimplemented_driver():
    """
    Test that snapshot_ordered is recognised but fails because the driver is not yet implemented.

    Expected: Non-zero exit; output includes "snapshot-ordered driver is not implemented yet".
    Validates: The value is accepted by the parser but fast-fails before processing begins.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file = processing_order_param_file("snapshot_ordered")
    returncode, stdout, stderr = run_mimic(param_file)
    output = stdout + stderr

    assert returncode != 0, "snapshot_ordered should fail until the snapshot driver exists"
    assert "snapshot-ordered driver is not implemented yet" in output


def main():
    global TEMP_DIR
    TEMP_DIR = Path(tempfile.mkdtemp(prefix="mimic_processing_order_"))
    try:
        tests = [
            test_unknown_processing_order_fails_fast,
            test_snapshot_ordered_reports_unimplemented_driver,
        ]
        return run_test_suite(tests, "Processing Order Validation")
    finally:
        shutil.rmtree(TEMP_DIR)


if __name__ == "__main__":
    sys.exit(main())
