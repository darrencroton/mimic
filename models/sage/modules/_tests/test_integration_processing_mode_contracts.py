#!/usr/bin/env python3
"""
Shared integration tests for processing-mode contract validation.

These checks ensure startup rejects unsupported mode/module combinations for
representative modules from each runtime contract group.
"""

import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import create_test_param_file, run_mimic


def assert_invalid_mode_rejected(output_name, phase_config, expected_mode, expected_supported):
    """Run Mimic and assert startup rejects an unsupported processing mode."""
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name,
        phase_config=phase_config,
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined_output = stdout + stderr

        assert returncode != 0, "Invalid processing mode should fail startup"
        assert f"does not support processing mode '{expected_mode}'" in combined_output
        assert f"Supported modes: {expected_supported}" in combined_output
    finally:
        shutil.rmtree(temp_dir)


def test_full_halo_only_module_rejects_process_by_galaxy():
    assert_invalid_mode_rejected(
        output_name="mode_contract_full_halo_rejects_by_galaxy",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_apply_infall", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        expected_mode="process_by_galaxy",
        expected_supported="process_full_halo",
    )


def test_by_galaxy_only_module_rejects_process_full_halo():
    assert_invalid_mode_rejected(
        output_name="mode_contract_by_galaxy_rejects_full_halo",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_apply_cooling", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        expected_mode="process_full_halo",
        expected_supported="process_by_galaxy",
    )


def test_dual_mode_module_rejects_process_full_halo():
    assert_invalid_mode_rejected(
        output_name="mode_contract_dual_rejects_full_halo",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_quasar_mode", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        expected_mode="process_full_halo",
        expected_supported="process_by_galaxy, process_per_event",
    )


def test_full_halo_only_module_rejects_process_per_event():
    # process_per_event is the historically highest-risk invalid mode: it was
    # the specific mode that triggered the original merger-pathway bug, and
    # it was absent from the rejection matrix until this test was added.
    assert_invalid_mode_rejected(
        output_name="mode_contract_full_halo_rejects_per_event",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_apply_infall", "process_per_event")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        expected_mode="process_per_event",
        expected_supported="process_full_halo",
    )


def main():
    test_full_halo_only_module_rejects_process_by_galaxy()
    test_by_galaxy_only_module_rejects_process_full_halo()
    test_dual_mode_module_rejects_process_full_halo()
    test_full_halo_only_module_rejects_process_per_event()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
