#!/usr/bin/env python3
"""Unit tests for output_utils.periodic_pair_counts() and correlation_function().

Covers: agreement with a brute-force O(N^2) count, xi of a uniform random sample, the
periodic boundary, the small-N guard, the coarse-grid (ncell < 3) path and the input domain.
"""

import os
import sys

import numpy as np

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo_root = os.path.dirname(os.path.dirname(parent_dir))
sys.path.insert(0, parent_dir)
sys.path.insert(0, os.path.join(repo_root, "tests"))

from framework import run_test_suite
from output_utils import correlation_function, periodic_pair_counts

BOX = 100.0


def brute_force_counts(positions, box_size, r_edges):
    """Reference O(N^2) pair count with the minimum-image convention."""
    n = len(positions)
    counts = np.zeros(len(r_edges) - 1, dtype=np.int64)
    for i in range(n - 1):
        delta = positions[i + 1 :] - positions[i]
        delta -= box_size * np.round(delta / box_size)
        r = np.sqrt((delta**2).sum(axis=1))
        counts += np.histogram(r, bins=r_edges)[0]
    return counts


def test_grid_counts_match_brute_force_bin_for_bin():
    """500 random points: the cell-grid count equals the brute-force count in every bin."""
    rng = np.random.default_rng(12345)
    positions = rng.uniform(0.0, BOX, size=(500, 3))
    for r_edges in (np.linspace(0.0, 20.0, 9), np.geomspace(1.0, 50.0, 8)):
        grid = periodic_pair_counts(positions, BOX, r_edges)
        brute = brute_force_counts(positions, BOX, r_edges)
        assert np.array_equal(grid, brute), f"grid {grid} != brute force {brute}"
        assert grid.sum() > 0, "Test is vacuous: no pairs counted"


def test_coarse_grid_matches_brute_force():
    """r_max = box/2 gives a 2-cell grid whose wrapped neighbours must not double count."""
    rng = np.random.default_rng(7)
    positions = rng.uniform(0.0, BOX, size=(300, 3))
    r_edges = np.linspace(0.0, 0.5 * BOX, 6)
    grid = periodic_pair_counts(positions, BOX, r_edges)
    brute = brute_force_counts(positions, BOX, r_edges)
    assert np.array_equal(grid, brute), f"grid {grid} != brute force {brute}"


def test_uniform_random_xi_consistent_with_zero():
    """xi of a uniform random sample is within 3 Poisson errors of zero in every bin."""
    rng = np.random.default_rng(2024)
    positions = rng.uniform(0.0, BOX, size=(3000, 3))
    r_edges = np.geomspace(2.0, 20.0, 9)
    xi, xi_err, dd, rr = correlation_function(positions, BOX, r_edges)
    assert np.all(dd > 0), f"Every bin needs pairs for a meaningful test, got dd={dd}"
    assert np.all(np.abs(xi) <= 3.0 * xi_err), f"xi={xi} not within 3 sigma={3 * xi_err} of 0"
    assert np.allclose(xi_err, np.sqrt(dd) / rr)


def test_pair_across_periodic_boundary_uses_minimum_image():
    """Two points 0.2 apart across the box face are counted at 0.2, not at 99.8."""
    positions = np.array([[0.1, 50.0, 50.0], [99.9, 50.0, 50.0]])
    r_edges = np.array([0.0, 0.1, 0.3, 1.0, 50.0])
    counts = periodic_pair_counts(positions, BOX, r_edges)
    assert counts.tolist() == [0, 1, 0, 0], f"Expected one pair in [0.1, 0.3), got {counts}"

    corner = np.array([[0.1, 0.1, 0.1], [99.9, 99.9, 99.9]])
    counts = periodic_pair_counts(corner, BOX, np.array([0.0, 0.3, 0.4, 50.0]))
    assert counts.tolist() == [0, 1, 0], f"Corner pair is at 0.2*sqrt(3)=0.346, got {counts}"


def test_pairs_counted_once_and_self_pairs_excluded():
    """Three mutually close points make exactly three pairs; coincident points pair at r = 0."""
    positions = np.array([[10.0, 10.0, 10.0], [10.5, 10.0, 10.0], [10.0, 10.5, 10.0]])
    counts = periodic_pair_counts(positions, BOX, np.array([0.0, 0.6, 1.0]))
    assert counts.tolist() == [2, 1], f"Expected [2, 1], got {counts}"
    assert counts.sum() == 3

    coincident = np.array([[5.0, 5.0, 5.0], [5.0, 5.0, 5.0]])
    counts = periodic_pair_counts(coincident, BOX, np.array([0.0, 1.0]))
    assert counts.tolist() == [1], f"Two coincident points are one pair, got {counts}"


def test_fewer_than_two_points_returns_zero_counts_and_nan_xi():
    """N < 2 gives zero counts, xi = NaN and zero error bars, never a division by zero."""
    r_edges = np.array([1.0, 2.0, 5.0])
    for positions in (np.empty((0, 3)), np.array([[1.0, 2.0, 3.0]])):
        assert periodic_pair_counts(positions, BOX, r_edges).tolist() == [0, 0]
        xi, xi_err, dd, rr = correlation_function(positions, BOX, r_edges)
        assert np.all(np.isnan(xi)), f"xi should be NaN, got {xi}"
        assert xi_err.tolist() == [0.0, 0.0]
        assert dd.tolist() == [0, 0]
        assert rr.tolist() == [0.0, 0.0]


def test_known_clustered_pair_gives_expected_xi():
    """One pair in one bin: xi = 1 / RR - 1 with the analytic random-pair expectation."""
    positions = np.array([[10.0, 10.0, 10.0], [10.5, 10.0, 10.0]])
    r_edges = np.array([0.0, 1.0])
    xi, xi_err, dd, rr = correlation_function(positions, BOX, r_edges)
    expected_rr = 0.5 * 2 * 1 * (4.0 * np.pi / 3.0) / BOX**3
    assert dd.tolist() == [1]
    assert np.isclose(rr[0], expected_rr, rtol=1e-12)
    assert np.isclose(xi[0], 1.0 / expected_rr - 1.0, rtol=1e-12)
    assert np.isclose(xi_err[0], 1.0 / expected_rr, rtol=1e-12)


def _raises_value_error(helper, *args):
    try:
        helper(*args)
    except ValueError:
        return True
    return False


def test_every_domain_violation_raises():
    """Each violated input condition raises ValueError, from both public helpers."""
    good = np.array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])
    edges = np.array([1.0, 2.0, 5.0])
    bad_cases = {
        "box_size zero": (good, 0.0, edges),
        "box_size negative": (good, -1.0, edges),
        "box_size NaN": (good, np.nan, edges),
        "box_size inf": (good, np.inf, edges),
        "positions wrong width": (np.zeros((2, 2)), BOX, edges),
        "positions 1-D": (np.zeros(3), BOX, edges),
        "positions NaN": (np.array([[np.nan, 1.0, 1.0], [1.0, 1.0, 1.0]]), BOX, edges),
        "positions inf": (np.array([[np.inf, 1.0, 1.0], [1.0, 1.0, 1.0]]), BOX, edges),
        "positions negative": (np.array([[-0.1, 1.0, 1.0], [1.0, 1.0, 1.0]]), BOX, edges),
        "positions at box_size": (np.array([[BOX, 1.0, 1.0], [1.0, 1.0, 1.0]]), BOX, edges),
        "edges not increasing": (good, BOX, np.array([1.0, 1.0, 2.0])),
        "edges decreasing": (good, BOX, np.array([5.0, 2.0, 1.0])),
        "edges negative": (good, BOX, np.array([-1.0, 1.0, 2.0])),
        "edges NaN": (good, BOX, np.array([1.0, np.nan, 2.0])),
        "edges beyond half box": (good, BOX, np.array([1.0, 2.0, 50.5])),
        "edges single": (good, BOX, np.array([1.0])),
        "edges 2-D": (good, BOX, np.array([[1.0, 2.0]])),
    }
    for label, (pos, box, r_edges) in bad_cases.items():
        for helper in (periodic_pair_counts, correlation_function):
            raised = _raises_value_error(helper, pos, box, r_edges)
            assert raised, f"{helper.__name__} did not raise for: {label}"

    # The boundary itself is legal: r_edges[-1] == box_size / 2.
    periodic_pair_counts(good, BOX, np.array([1.0, 0.5 * BOX]))


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_grid_counts_match_brute_force_bin_for_bin,
            test_coarse_grid_matches_brute_force,
            test_uniform_random_xi_consistent_with_zero,
            test_pair_across_periodic_boundary_uses_minimum_image,
            test_pairs_counted_once_and_self_pairs_excluded,
            test_fewer_than_two_points_returns_zero_counts_and_nan_xi,
            test_known_clustered_pair_gives_expected_xi,
            test_every_domain_violation_raises,
        ],
        "Correlation function helper (test_correlation_function.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
