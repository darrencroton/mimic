#!/usr/bin/env python3
"""
Shared integration test for the processing-mode contract of the sage26 modules.

Startup must reject a processing mode a module does not support, naming the modes it does. The
core owns that mechanism and tests it with fixture modules (tests/integration/test_processing_modes.py);
this file runs it once on a sage26 module that declares two modes, so the ``Supported modes:`` text
the package's own ``module_info.yaml`` produces for a multi-mode module is checked.

Test cases:
    - test_dual_mode_module_rejects_process_full_halo: sage26_quasar_mode, which supports
      process_by_galaxy and process_per_event, is refused as process_full_halo at startup
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import MIMIC_EXE, SharedRuns, TestSkipped, run_test_suite

RUNS = SharedRuns()


def test_dual_mode_module_rejects_process_full_halo():
    """sage26_quasar_mode as process_full_halo fails startup, listing both supported modes."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    run = RUNS.run(
        "mode_contract_dual_rejects_full_halo",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage26_quasar_mode", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
    )
    combined_output = run.stdout + run.stderr

    assert run.returncode != 0, "Invalid processing mode should fail startup"
    assert "does not support processing mode 'process_full_halo'" in combined_output
    assert "Supported modes: process_by_galaxy, process_per_event" in combined_output


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_dual_mode_module_rejects_process_full_halo],
            "Processing Mode Contracts (test_integration_processing_mode_contracts.py)",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
