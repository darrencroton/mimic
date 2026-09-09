#!/usr/bin/env python3
"""Unit tests for output_utils scatter-sampling and binning helpers.

Covers select_scatter_sample() and make_bin_edges().
"""

import os
import random
import sys

import numpy as np

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo_root = os.path.dirname(os.path.dirname(parent_dir))
sys.path.insert(0, parent_dir)
sys.path.insert(0, os.path.join(repo_root, "tests"))

from framework import run_test_suite
from output_utils import make_bin_edges, select_scatter_sample


def test_select_scatter_sample_keeps_all_in_box_points():
    """All candidates inside the box survive when under the dilution budget."""
    x = np.array([1.0, 2.0, 3.0])
    y = np.array([1.0, 2.0, 3.0])
    keep = select_scatter_sample(x, y, 0.0, 5.0, 0.0, 5.0, dilute=10)

    assert sorted(keep.tolist()) == [0, 1, 2], f"Expected all 3 points, got {keep}"


def test_select_scatter_sample_drops_out_of_box_points():
    """A point outside either axis range is excluded, even under the dilution budget."""
    x = np.array([1.0, 20.0, 3.0])
    y = np.array([1.0, 2.0, 30.0])
    keep = select_scatter_sample(x, y, 0.0, 5.0, 0.0, 5.0, dilute=10)

    assert sorted(keep.tolist()) == [0], f"Expected only index 0, got {keep}"


def test_select_scatter_sample_narrow_y_window_does_not_starve_the_sample():
    """
    Regression: dilution must not be spent on points invisible on the display axis.

    Reproduces the reported bug directly: with a wide x range but a narrow y
    window, most candidates fall outside y even though x is fine. Diluting
    before restricting to the box would waste nearly the whole budget on
    invisible points; filtering first must not.
    """
    rng = np.random.default_rng(0)
    n = 100000
    x = rng.uniform(0.0, 10.0, n)
    # Only 1% of points fall inside the y window [4.9, 5.1].
    y = rng.uniform(0.0, 100.0, n)
    y[:1000] = rng.uniform(4.9, 5.1, 1000)

    keep = select_scatter_sample(x, y, 0.0, 10.0, 4.9, 5.1, dilute=500)

    assert len(keep) == 500, f"Expected the full dilution budget of 500, got {len(keep)}"
    assert np.all((y[keep] >= 4.9) & (y[keep] <= 5.1)), "Selected points must fall inside the box"


def test_select_scatter_sample_dilutes_to_budget():
    """More in-box candidates than the dilution budget are randomly downsampled."""
    x = np.arange(1000, dtype=float)
    y = np.zeros(1000)
    keep = select_scatter_sample(x, y, 0.0, 1000.0, -1.0, 1.0, dilute=100)

    assert len(keep) == 100, f"Expected exactly 100 points, got {len(keep)}"
    assert len(set(keep.tolist())) == 100, "Selected indices must be unique"


def test_select_scatter_sample_reproducible_with_seeded_rng():
    """The same seeded RNG produces the same selection (matches figures' random.seed(2222))."""
    x = np.arange(1000, dtype=float)
    y = np.zeros(1000)

    random.seed(2222)
    first = select_scatter_sample(x, y, 0.0, 1000.0, -1.0, 1.0, dilute=50, rng=random)

    random.seed(2222)
    second = select_scatter_sample(x, y, 0.0, 1000.0, -1.0, 1.0, dilute=50, rng=random)

    assert first.tolist() == second.tolist(), "Same seed must give the same sample"


def test_select_scatter_sample_multi_series_keeps_point_if_any_series_in_box():
    """A point with several y series (e.g. mass_reservoir_scatter) is kept if any series is in range."""
    x = np.array([1.0, 1.0, 1.0])
    y_series = [
        np.array([100.0, 100.0, 100.0]),  # out of range for every point
        np.array([100.0, 2.0, 100.0]),  # in range only for index 1
    ]
    keep = select_scatter_sample(x, y_series, 0.0, 5.0, 0.0, 5.0, dilute=10)

    assert keep.tolist() == [1], f"Expected only index 1, got {keep}"


def test_select_scatter_sample_empty_when_nothing_in_box():
    """No candidates in the box returns an empty selection, not an error."""
    x = np.array([100.0, 200.0])
    y = np.array([100.0, 200.0])
    keep = select_scatter_sample(x, y, 0.0, 5.0, 0.0, 5.0, dilute=10)

    assert len(keep) == 0, f"Expected no points, got {keep}"


def test_make_bin_edges_includes_both_endpoints():
    """Regression: np.arange(min, max, width) silently excludes max; make_bin_edges must not."""
    edges = make_bin_edges(10.8, 15.0, 0.1)

    assert np.isclose(edges[0], 10.8), f"First edge should be 10.8, got {edges[0]}"
    assert np.isclose(edges[-1], 15.0), f"Last edge should be 15.0, got {edges[-1]}"


def test_make_bin_edges_uses_approximately_the_requested_width():
    """The realised bin width stays close to the requested width."""
    edges = make_bin_edges(0.0, 10.0, 0.5)
    widths = np.diff(edges)

    assert np.allclose(widths, 0.5), f"Expected uniform 0.5-wide bins, got {widths}"


def test_make_bin_edges_handles_a_range_narrower_than_one_bin():
    """A range narrower than one bin width still returns at least one bin."""
    edges = make_bin_edges(1.0, 1.05, 0.5)

    assert len(edges) >= 2, f"Expected at least 2 edges (1 bin), got {len(edges)}"
    assert np.isclose(edges[0], 1.0)
    assert np.isclose(edges[-1], 1.05)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_select_scatter_sample_keeps_all_in_box_points,
            test_select_scatter_sample_drops_out_of_box_points,
            test_select_scatter_sample_narrow_y_window_does_not_starve_the_sample,
            test_select_scatter_sample_dilutes_to_budget,
            test_select_scatter_sample_reproducible_with_seeded_rng,
            test_select_scatter_sample_multi_series_keeps_point_if_any_series_in_box,
            test_select_scatter_sample_empty_when_nothing_in_box,
            test_make_bin_edges_includes_both_endpoints,
            test_make_bin_edges_uses_approximately_the_requested_width,
            test_make_bin_edges_handles_a_range_narrower_than_one_bin,
        ],
        "Scatter Sampling and Binning Helpers (test_scatter_and_binning_helpers.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
