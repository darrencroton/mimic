#!/usr/bin/env python3
"""
sage_prepare_infall_budget Module - Integration Test

Validates: the module reads the GlobalBaryonFraction model parameter and uses it as the baryon
fraction of every FoF central when no sage_reionization precedes it. The pipeline's other
behaviour (the budget itself, applied to the group's baryons) is witnessed through its consumer in
the sage_apply_infall integration test; the budget arithmetic, consolidation and clamps are
asserted by the unit test.

Test cases:
  - test_parameters_configurable: with GlobalBaryonFraction 0.20 and no reionization, every
    central's written HaloBaryonFraction is 0.20 (the unset -1 sentinel replaced by the parameter)
"""

import sys
from pathlib import Path

import numpy as np


def find_repo_root(start):
    """Find the Mimic repository root from this test file."""
    for candidate in [start, *start.parents]:
        if (candidate / "Makefile").is_file() and (candidate / "tests" / "framework").is_dir():
            return candidate
    raise RuntimeError(f"Could not find Mimic repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import SharedRuns, load_binary_halos, run_test_suite  # noqa: E402

RUNS = SharedRuns()

# The budget alone in pre_timestep: no sage_reionization sets HaloBaryonFraction first, so the
# module falls back to GlobalBaryonFraction. 0.20 differs from the 0.17 the other tests use.
BUDGET_ONLY_PHASES = {
    "pre_timestep": [("sage_prepare_infall_budget", "process_full_halo")],
    "galaxy_physics": [],
    "satellite_mergers": [],
    "post_timestep": [],
}
BUDGET_ONLY_PARAMS = {"GlobalBaryonFraction": 0.20}


def test_parameters_configurable():
    """
    GlobalBaryonFraction reaches the output as the baryon fraction of every central.

    Without sage_reionization, HaloBaryonFraction starts at its -1 sentinel and the budget module
    replaces it with GlobalBaryonFraction, so a parameter the module failed to read (or a wrong
    sentinel) would leave -1 or the parameter's default in the output.
    """
    run = RUNS.run(
        "budget_only_gbf_0.20", phase_config=BUDGET_ONLY_PHASES, model_params=BUDGET_ONLY_PARAMS
    )
    assert run.returncode == 0, f"Execution with custom parameters should succeed\n{run.stderr}"

    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    centrals = halos[halos["Type"] == 0]
    assert len(centrals) > 0, "Should have Type 0 centrals for testing"
    fractions = centrals["HaloBaryonFraction"]
    assert np.all(fractions == 0.20), (
        f"every central's HaloBaryonFraction should be 0.20, "
        f"got values {np.unique(fractions)[:5]}"
    )


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_parameters_configurable], "sage_prepare_infall_budget Integration Tests"
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
