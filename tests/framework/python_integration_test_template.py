#!/usr/bin/env python3
"""
[Module Name] Module - Integration Test (template)

HOW TO USE THIS TEMPLATE (works for any model package):

  1. Copy to models/<model>/modules/<mymodule>/_tests/test_integration_<mymodule>.py
  2. Replace [module_name] placeholders with your module name.
  3. Declare the test in your module_info.yaml:
       tests:
         integration: _tests/test_integration_<mymodule>.py
     The registry generator discovers it from there (make tests-integration).
  4. Flesh out the skeleton tests with your module's real end-to-end checks.

WHAT THE INTEGRATION TIER VALIDATES (vs the C unit tier):
  - End-to-end behavior through the full pipeline (real Mimic runs)
  - Physics correctness at the system level (realistic values, relationships)
  - Conservation laws across the pipeline
  - Parameter sensitivity (changing params changes results)
  - Edge cases visible in output (no NaN/Inf, physical ranges)
  - Memory safety (the run's own leak report)

Internal calculations and detailed math belong in the C unit tests, not here.
Keep each test under ~30 seconds: the create_test_param_file() defaults run a
single tree file.
"""

import shutil
import sys
from pathlib import Path


def find_repo_root(start):
    """Find the Mimic repository root from a copied test file."""
    for candidate in [start, *start.parents]:
        if (candidate / "Makefile").is_file() and (candidate / "tests" / "framework").is_dir():
            return candidate
    raise RuntimeError(f"Could not find Mimic repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    check_no_memory_leaks,
    create_test_param_file,
    load_binary_halos,
    run_mimic,
    run_test_suite,
)


def test_full_pipeline_execution():
    """Module executes in the full pipeline and produces loadable output.

    This one is fully worked: it is the minimum every module integration
    test should do. The skeletons below follow the same run-then-validate
    shape with different assertions.
    """
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="[module_name]_pipeline",
        phase_config={
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
                # Add dependency modules before/after as your pipeline requires
            ],
        },
        model_params={
            "Param1Name": 1.0,
            # Add every parameter your module's init() requires
        },
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

        # The allocator's leak report is in the run's own output
        assert check_no_memory_leaks(stdout, stderr), "Should not have memory leaks"

        output_file = output_dir / "model_z0.000_0"
        halos, metadata = load_binary_halos(output_file)
        assert len(halos) > 0, "Should have halos in output"

        # Validate your module's output properties exist and are finite:
        # assert np.all(np.isfinite(halos["MyProperty"])), "MyProperty should be finite"
    finally:
        shutil.rmtree(temp_dir)


def test_physics_validation():
    """Physics correctness at the system level.

    Skeleton: run the pipeline (as above), then assert your module's output
    properties have physically reasonable values and expected relationships,
    e.g. positive masses, rates below physical bounds, monotonic relations.
    """


def test_conservation_laws():
    """Conservation across the pipeline (if applicable).

    Skeleton: run the pipeline, then assert the total of the reservoirs your
    module exchanges (mass, metals, ...) is conserved within tolerance.
    """


def test_parameter_sensitivity():
    """Changing module parameters changes the results.

    Skeleton: run the pipeline twice with different model_params values and
    assert the relevant output property differs (and, if known, in the
    expected direction).
    """


def test_edge_cases():
    """Boundary conditions visible in output.

    Skeleton: run the pipeline, then assert no NaN/Inf in your module's
    properties and that non-negative quantities stay non-negative across all
    galaxy types (centrals, satellites, orphans).
    """


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_full_pipeline_execution,
            test_physics_validation,
            test_conservation_laws,
            test_parameter_sensitivity,
            test_edge_cases,
        ],
        "[module_name] Integration Tests",
    )


if __name__ == "__main__":
    sys.exit(main())
