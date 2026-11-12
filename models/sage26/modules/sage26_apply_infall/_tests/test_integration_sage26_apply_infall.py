#!/usr/bin/env python3
"""
sage26_apply_infall Module - Integration Test

Validates: the infall budget sage26_prepare_infall_budget stores for each FoF central is applied
by sage26_apply_infall so that the group's baryons reach HaloBaryonFraction * Mvir, at the default
substep count and at a non-default one.

InfallingGas is a transport field that is not written (model_properties.yaml, output: false), so
the budget is witnessed by its signature in the written reservoirs. In this infall-only pipeline
(reionization, the budget, the application; no cooling, star formation or feedback) the budget sets
each FoF group's baryons to HaloBaryonFraction * Mvir (sage26_prepare_infall_budget, with Mvir read
as a float), so no group ends below that allowance. A group can end above it only where a negative
budget exceeded the central's ejected and hot gas: sage26_apply_infall then clears both with their
metals, and the satellites' hot gas, which the budget does not consolidate, keeps the group above
its allowance. Such groups must exist on the test trees (the positive witness of negative infall).

Test cases:
  - test_substep_distribution: the allowance invariant holds with the budget split over 4 substeps
  - test_negative_infall_physics: the allowance invariant at the default substep count, centrals
    that a negative budget exhausted hold no hot or ejected gas, and the run reports no leaks
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

from framework import (  # noqa: E402
    SharedRuns,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

RUNS = SharedRuns()

# Infall-only pipeline: reionization sets HaloBaryonFraction, the budget is computed in
# pre_timestep and applied per substep in galaxy_physics
INFALL_PHASES = {
    "pre_timestep": [
        ("sage26_reionization", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [("sage26_apply_infall", "process_full_halo")],
    "satellite_mergers": [],
    "post_timestep": [],
}
INFALL_PARAMS = {"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0}

# Baryon reservoirs the infall-only pipeline can populate
RESERVOIRS = ["StellarMass", "ColdGas", "HotGas", "EjectedGas", "ICS", "BlackHoleMass", "CGMgas"]


def assert_group_baryons_reach_allowance(halos):
    """
    Assert every FoF group's baryons are at or above HaloBaryonFraction * Mvir, and return the
    centrals of the groups that end above it.

    Every row carries its FoF central's UniqueGalaxyID in UniqueCentralGalaxyID, so a group's
    baryons are the sum over the rows sharing that ID. The groups at the allowance must exist,
    the groups above it must be ones a negative budget exhausted, and those must exist too; their
    centrals must hold no hot or ejected gas.

    Args:
        halos: Structured array of the output rows.

    Returns:
        Structured array of the exhausted centrals.
    """
    centrals = halos[halos["Type"] == 0]
    assert len(centrals) > 0, "Should have Type 0 centrals for testing"

    row_baryons = sum(halos[name].astype(np.float64) for name in RESERVOIRS)
    group_ids, group_index = np.unique(halos["UniqueCentralGalaxyID"], return_inverse=True)
    group_sums = np.bincount(group_index, weights=row_baryons)
    position = np.searchsorted(group_ids, centrals["UniqueGalaxyID"])
    assert np.all(
        group_ids[position] == centrals["UniqueGalaxyID"]
    ), "every central should head its own FoF group"
    group_baryons = group_sums[position]
    allowance = centrals["HaloBaryonFraction"].astype(np.float64) * centrals["Mvir"].astype(
        np.float32
    ).astype(np.float64)

    # 1e-5 relative for the float reservoirs, plus an absolute 1e-8 (1e2 Msun/h) for the float
    # residue the per-substep application leaves in groups whose allowance is zero (Mvir 0)
    tolerance = 1e-5 * allowance + 1e-8
    below = group_baryons < allowance - tolerance
    assert not np.any(below), f"{int(np.sum(below))} FoF groups end below their baryon allowance"
    at_allowance = np.abs(group_baryons - allowance) <= tolerance
    assert np.any(at_allowance), "the budget should bring FoF groups to their allowance"

    exhausted = group_baryons > allowance + tolerance
    assert np.any(exhausted), "some negative budgets should exceed the central's reservoirs"
    for field in ["HotGas", "EjectedGas"]:
        assert np.all(
            centrals[exhausted][field] == 0.0
        ), f"a central whose negative infall exceeded its reservoirs should hold no {field}"
    return centrals[exhausted]


def test_substep_distribution():
    """
    The infall budget is partitioned over substeps: at SubSteps 4 every group's baryons satisfy
    the same allowance invariant as at the default count (none below HaloBaryonFraction * Mvir,
    some at it, the exhausted centrals clear of hot and ejected gas).

    This is the only run at a non-default substep count, so it asserts the full allowance
    invariant, not just that the run completes.
    """
    run = RUNS.run(
        "substeps_4",
        phase_config=INFALL_PHASES,
        model_params=INFALL_PARAMS,
        substeps=4,
    )
    assert run.returncode == 0, f"Execution with substeps should succeed\nSTDERR: {run.stderr}"

    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have halos in output"
    assert_group_baryons_reach_allowance(halos)


def test_negative_infall_physics():
    """
    Negative infall removes gas from the ejected and then the hot reservoir of the central, and
    every group's baryons end at their allowance.

    Also asserts the reservoirs of the centrals are finite and non-negative on this partial
    pipeline, and that the run reports no memory leaks.
    """
    run = RUNS.run("infall_default", phase_config=INFALL_PHASES, model_params=INFALL_PARAMS)
    assert run.returncode == 0, f"Mimic execution should succeed\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"

    output_file = run.output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    halos, _metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have halos in output"

    centrals = halos[halos["Type"] == 0]
    for field in ["HotGas", "EjectedGas"]:
        assert np.all(np.isfinite(centrals[field])), f"{field} should have finite values"
        assert np.all(centrals[field] >= 0), f"{field} should be non-negative"

    exhausted = assert_group_baryons_reach_allowance(halos)
    print(
        f"  Negative infall exhausted the central's hot and ejected gas in {len(exhausted)} groups"
    )


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_substep_distribution, test_negative_infall_physics],
            "sage26_apply_infall Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
