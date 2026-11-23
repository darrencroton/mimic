#!/usr/bin/env python3
"""
--skip refusal of a partly present binary partition

Validates: a vertical run under --skip refuses a partition whose binary output files an earlier
run left partly present, and the refusal deletes none of them.

A binary partition has one output file per requested snapshot. Under --skip a partition with all
its files is skipped and one with some of them is fatal ("Partial output exists ... Remove the
partial files or rerun without --skip."). The failure exit removes every path the run registered
as in progress (vertical_driver_remove_incomplete_outputs()), so the refusal must register none
of the earlier run's files, or the run that tells the user to remove the files would delete them
itself (src/core/vertical_driver.c, claim_and_process_partition()).
"""

import hashlib
import shutil
import sys
from pathlib import Path

import yaml

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    MIMIC_EXE,
    TestSkipped,
    core_input_file,
    create_test_param_file,
    not_applicable_if_selected_package_is_horizontal,
    run_mimic,
    run_mimic_fresh,
    run_test_suite,
)


def _partition_files(output_dir):
    """Return the partition-0 binary output files of output_dir, sorted by name."""
    return sorted(output_dir.glob("model_z*_0"))


def _digests(paths):
    """Return {path: sha256 of its bytes} for every path."""
    return {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}


def test_skip_refuses_partly_present_partition_and_keeps_its_files():
    """A --skip rerun skips a complete partition and refuses a partly present one, keeping it."""
    print("Testing --skip on a partly present binary partition...")

    # ===== SETUP =====
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    not_applicable_if_selected_package_is_horizontal("binary output and --skip")
    with open(core_input_file("test_binary.yaml"), "r") as handle:
        last_snapshot = int(yaml.safe_load(handle)["output"]["snapshot_list"][-1])
    param_file, output_dir, temp_dir = create_test_param_file(
        "skip_partial_output",
        output_format="binary",
        snapshot_list=[last_snapshot - 1, last_snapshot],
    )

    try:
        run_mimic_fresh(param_file)
        files = _partition_files(output_dir)
        assert len(files) == 2, f"Expected one file per requested snapshot, found {files}"

        # ===== CONTROL: a complete partition is skipped =====
        before = _digests(files)
        returncode, stdout, stderr = run_mimic(param_file, extra_args=["--skip"])
        output = stdout + stderr
        assert returncode == 0, f"--skip on a complete partition must succeed:\n{output}"
        assert "already exists ... skipping" in output, f"Expected a skip message:\n{output}"
        assert _digests(files) == before, "--skip on a complete partition must not touch it"

        # ===== EXECUTE: one file of the two is missing =====
        removed, kept = files
        removed.unlink()
        kept_digest = before[kept]
        returncode, stdout, stderr = run_mimic(param_file, extra_args=["--skip"])

        # ===== VALIDATE =====
        output = stdout + stderr
        assert returncode != 0, f"--skip on a partly present partition must be refused:\n{output}"
        message = "Partial output exists for partition 0 (1 of 2 files)"
        assert message in output, f"Missing the refusal message '{message}':\n{output}"
        assert "Remove the partial files or rerun without --skip" in output, output
        assert kept.exists(), f"The refusal deleted the earlier run's file {kept}"
        assert _digests([kept])[kept] == kept_digest, f"{kept} changed during the refusal"
        assert not removed.exists(), f"The refusal created {removed}"
        assert not list(output_dir.glob("*.inflight")), "No in-flight marker may be left behind"

        print("  ✓ --skip refused the partly present partition and left its file in place")

    finally:
        # ===== CLEANUP =====
        shutil.rmtree(temp_dir)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [test_skip_refuses_partly_present_partition_and_keeps_its_files],
        "--skip Partial Output Refusal (test_skip_partial_output.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
