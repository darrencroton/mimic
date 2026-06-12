#!/usr/bin/env python3
"""
[TEST NAME] - Scientific Validation Test (template)

HOW TO USE THIS TEMPLATE (works for any model package):

  1. Copy to tests/scientific/test_<name>.py (core, model-neutral physics) or
     models/<model>/modules/_tests/test_scientific_<name>.py (model-owned,
     declared under tests: scientific: in a module_info.yaml).
  2. Replace the placeholder docstrings and example checks with the physics
     requirement being validated; cite the published result if there is one.
  3. Verify: python3 tests/scientific/test_<name>.py, or make tests-scientific.

WHAT THE SCIENTIFIC TIER VALIDATES:
  - Fundamental physics of a full run, not implementation details:
    sanity (no NaN/Inf, positive masses), conservation laws, property
    ranges, ensemble statistics against known results.
  - Note that tests/scientific/test_scientific.py already validates ALL
    output properties against the ranges and sentinels declared in the
    property YAML metadata — add a new scientific test only for physics
    that metadata cannot express (relations between properties,
    statistics, literature comparisons).

TOLERANCE GUIDANCE (document why each tolerance is chosen):
  - Exact conservation: ~1e-10 (numerical precision)
  - Approximate conservation: ~1e-2 (1%)
  - Rough comparison to literature: up to ~0.5 (50%)
  - Committed-baseline comparisons: use framework baseline_rtol()
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    TEST_DATA_DIR,
    core_input_file,
    load_binary_halos,
    run_mimic_fresh,
    run_test_suite,
)


def regenerate_output():
    """Run Mimic fresh and return the output path (see test_scientific.py)."""
    output_file = TEST_DATA_DIR / "output" / "binary" / "model_z0.000_0"
    run_mimic_fresh(core_input_file("test_binary.yaml"), output_file)
    return output_file


def test_physics_sanity_check():
    """[ONE-LINE DESCRIPTION OF THE PHYSICS SANITY CHECK]

    Expected: [PHYSICALLY REASONABLE VALUES]
    """
    halos, metadata = load_binary_halos(regenerate_output())

    # Example: basic physical positivity and finiteness
    assert np.all(halos.Mvir >= 0), "Mvir must be non-negative"
    assert np.all(np.isfinite(halos.Mvir)), "Mvir must be finite"


def test_conservation_law():
    """[DESCRIPTION OF THE CONSERVATION LAW BEING TESTED]

    Expected: [QUANTITY] conserved within [TOLERANCE]
    Reference: [PAPER OR EQUATION IF APPLICABLE]
    """
    halos, metadata = load_binary_halos(regenerate_output())

    # Example shape:
    # total = np.sum(halos.ReservoirA + halos.ReservoirB)
    # expected = ...
    # rel_error = abs(total - expected) / expected
    # assert rel_error < 0.01, f"Not conserved: {rel_error*100:.2f}% error"


def test_statistical_properties():
    """[DESCRIPTION OF THE ENSEMBLE/STATISTICAL CHECK]

    Expected: [STATISTIC MATCHES EXPECTATION]
    Reference: [PUBLISHED RESULT IF APPLICABLE]
    """
    halos, metadata = load_binary_halos(regenerate_output())

    # Example shape:
    # median_mass = np.median(halos.Mvir[halos.Mvir > 0])
    # assert 0.1 < median_mass < 100.0, f"Median Mvir unreasonable: {median_mass}"


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_physics_sanity_check,
            test_conservation_law,
            test_statistical_properties,
        ],
        "[TEST SUITE NAME] Scientific Validation",
    )


if __name__ == "__main__":
    sys.exit(main())
