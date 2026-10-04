#!/usr/bin/env python3
"""Unit tests for output_utils.periodic_pair_counts() and correlation_function().

Covers: agreement with a brute-force O(N^2) count, xi of a uniform random sample, the
periodic boundary, the small-N guard, the coarse-grid (ncell < 3) and capped-grid (32 cells per
axis) paths, the tile memory bound, the input domain, and the figure-support helpers
wrap_into_box(), require_full_box(), log_radial_edges(), xi_series() and read_module_parameters().
"""

import os
import sys
import tracemalloc

import numpy as np

parent_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
repo_root = os.path.dirname(os.path.dirname(parent_dir))
sys.path.insert(0, parent_dir)
sys.path.insert(0, os.path.join(repo_root, "tests"))

import output_utils
from framework import run_test_suite
from output_utils import (
    correlation_function,
    log_radial_edges,
    periodic_pair_counts,
    read_module_parameters,
    require_full_box,
    wrap_into_box,
    xi_series,
)

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


def test_blocked_evaluation_matches_brute_force():
    """A tiny tile budget forces many tiles in both dimensions; counts agree and tiles obey it."""
    rng = np.random.default_rng(99)
    positions = rng.uniform(0.0, BOX, size=(400, 3))
    # r_max = box/2 gives a 2-cell grid, so each cell holds ~50 points and ~1250 pairs.
    r_edges = np.linspace(0.0, 0.5 * BOX, 7)
    brute = brute_force_counts(positions, BOX, r_edges)
    default_counts = periodic_pair_counts(positions, BOX, r_edges)
    assert brute.sum() > 10000, "Test is vacuous: too few pairs counted"
    assert np.array_equal(default_counts, brute)

    original_budget = output_utils._PAIR_COUNT_BLOCK_PAIRS
    original_kernel = output_utils._separations_sq
    largest = []

    def spy(points_a, points_b, box_size):
        largest.append(len(points_a) * len(points_b))
        return original_kernel(points_a, points_b, box_size)

    try:
        output_utils._separations_sq = spy
        for budget in (1, 7, 100):
            output_utils._PAIR_COUNT_BLOCK_PAIRS = budget
            largest.clear()
            counts = periodic_pair_counts(positions, BOX, r_edges)
            assert np.array_equal(counts, brute), f"budget={budget}: {counts} != {brute}"
            assert len(largest) > 50, f"budget={budget}: expected many tiles, got {len(largest)}"
            assert max(largest) <= budget, f"budget={budget}: a tile held {max(largest)} pairs"
    finally:
        output_utils._PAIR_COUNT_BLOCK_PAIRS = original_budget
        output_utils._separations_sq = original_kernel


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


def test_capped_grid_matches_brute_force_on_clustered_points():
    """r_max <= 1 in a box of 100 hits the 32-cell cap; clustered counts still match brute force."""
    rng = np.random.default_rng(31415)
    centres = rng.uniform(0.0, BOX, size=(30, 3))
    centres[:4] = [[0.2, 50.0, 99.8], [99.9, 0.1, 0.1], [50.0, 0.3, 50.0], [0.1, 99.9, 50.0]]
    members = centres[rng.integers(0, len(centres), size=1800)]
    positions = np.mod(members + rng.normal(0.0, 0.25, size=(1800, 3)), BOX)
    r_edges = np.linspace(0.0, 1.0, 6)
    assert np.floor(BOX / r_edges[-1]) > output_utils._MAX_PAIR_COUNT_CELLS_PER_AXIS
    grid = periodic_pair_counts(positions, BOX, r_edges)
    brute = brute_force_counts(positions, BOX, r_edges)
    assert brute.sum() > 10000, "Test is vacuous: too few clustered pairs"
    assert np.array_equal(grid, brute), f"grid {grid} != brute force {brute}"


def test_separation_tile_matches_reference_and_stays_within_memory_bound():
    """_separations_sq equals the broadcast formula, and a 1e6-pair tile peaks near 24 MB."""
    rng = np.random.default_rng(5)
    points_a = rng.uniform(0.0, BOX, size=(40, 3))
    points_b = rng.uniform(0.0, BOX, size=(25, 3))
    delta = points_a[:, None, :] - points_b[None, :, :]
    delta -= BOX * np.round(delta / BOX)
    expected = (delta**2).sum(axis=2)
    got = output_utils._separations_sq(points_a, points_b, BOX)
    assert got.shape == (40, 25)
    assert np.allclose(got, expected, rtol=1e-12, atol=0.0)

    big_a = rng.uniform(0.0, BOX, size=(1000, 3))
    big_b = rng.uniform(0.0, BOX, size=(1000, 3))
    tracemalloc.start()
    try:
        output_utils._separations_sq(big_a, big_b, BOX)
        _current, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    assert peak < 30e6, f"1e6-pair tile peaked at {peak / 1e6:.1f} MB, documented about 24 MB"


def test_wrap_into_box_maps_the_boundary_to_zero_and_keeps_inside_points():
    """A coordinate at (or rounded up to) box_size is the point 0; interior points are unchanged."""
    positions = np.array([[BOX, 0.0, 12.5], [BOX + 1.5, -1.5, 99.0]], dtype=np.float32)
    wrapped = wrap_into_box(positions, BOX)
    assert wrapped.dtype == np.float64
    assert wrapped.min() >= 0.0 and wrapped.max() < BOX
    assert wrapped[0].tolist() == [0.0, 0.0, 12.5]
    assert np.allclose(wrapped[1], [1.5, BOX - 1.5, 99.0])
    nan_row = wrap_into_box(np.array([[np.nan, 1.0, 1.0]]), BOX)
    assert np.isnan(nan_row[0, 0]), "NaN must pass through for correlation_function() to reject"


def test_require_full_box_reports_a_skip_only_for_partial_or_missing_box():
    """None for the whole box; a skip message for a partial volume, or a missing/bad box size."""
    assert require_full_box(BOX**3, BOX) is None
    assert require_full_box(BOX**3 * (1.0 + 1.0e-9), BOX) is None
    assert "Only part of the box" in require_full_box(0.5 * BOX**3, BOX)
    for bad in (None, 0.0, -1.0):
        assert "no positive box_size" in require_full_box(BOX**3, bad)


def test_log_radial_edges_spacing_and_range_guard():
    """Edges span [r_min, r_max] geometrically at about bins_per_dex; bad ranges raise."""
    edges = log_radial_edges(0.1, 20.0, 5)
    assert len(edges) == 13, "5 per dex over log10(200) = 2.30 dex is 12 bins"
    assert np.isclose(edges[0], 0.1) and np.isclose(edges[-1], 20.0)
    assert np.allclose(np.diff(np.log10(edges)), np.log10(200.0) / 12.0)
    assert len(log_radial_edges(1.0, 1.1, 5)) == 2, "A narrow range still gets one bin"
    for r_min, r_max in ((0.0, 5.0), (-1.0, 5.0), (5.0, 5.0), (5.0, 1.0), (np.nan, 5.0)):
        assert _raises_value_error(log_radial_edges, r_min, r_max, 5), (r_min, r_max)


def test_xi_series_selects_drawable_bins_and_counts_hidden_ones():
    """Bins with pairs and xi > 0 are shown; bins with pairs but xi <= 0 are only counted."""
    # A 5 x 5 x 5 lattice of spacing 20: no pairs below 20, 375 pairs at exactly 20 (fewer than
    # the 474.8 expected at random in [10, 25), so xi < 0 there) and 3125 in [25, 45) (more than
    # the 2451 expected, so xi > 0).
    axis = 20.0 * np.arange(5)
    lattice = np.array(np.meshgrid(axis, axis, axis)).reshape(3, -1).T
    shown, n_hidden, xi, xi_err, dd = xi_series(lattice, BOX, np.array([1.0, 10.0, 25.0, 45.0]))
    assert dd.tolist() == [0, 375, 3125]
    assert shown.tolist() == [False, False, True]
    assert n_hidden == 1 and xi[1] < 0.0 < xi[2]
    assert xi_err.shape == (3,)

    clump = np.array([[10.0, 10.0, 10.0], [10.2, 10.0, 10.0], [10.0, 10.2, 10.0]])
    shown, n_hidden, *_ = xi_series(clump, BOX, np.array([0.1, 0.5, 5.0, 20.0]))
    assert shown.tolist() == [True, False, False]
    assert n_hidden == 0, "A bin with no pairs is not a hidden bin"

    straddling = np.array([[BOX, 5.0, 5.0], [0.1, 5.0, 5.0]])
    assert xi_series(straddling, BOX, np.array([0.05, 0.2, 1.0]))[4].tolist() == [1, 0]
    bad = np.array([[np.nan, 1.0, 1.0], [1.0, 1.0, 1.0]])
    assert _raises_value_error(xi_series, bad, BOX, np.array([1.0, 2.0]))


def test_read_module_parameters_returns_floats_and_names_the_missing():
    """Numeric parameters (or numeric strings) are read; absent or non-numeric ones are missing."""
    params = {"EnabledModules": {"parameters": {"A": 1, "B": "2.5", "C": "x", "D": None}}}
    values, missing = read_module_parameters(params, ("A", "B", "C", "D", "E"))
    assert values == {"A": 1.0, "B": 2.5}
    assert missing == ["C", "D", "E"]
    for empty in ({}, {"EnabledModules": None}, {"EnabledModules": {"parameters": None}}):
        assert read_module_parameters(empty, ("A",)) == ({}, ["A"])


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_grid_counts_match_brute_force_bin_for_bin,
            test_coarse_grid_matches_brute_force,
            test_blocked_evaluation_matches_brute_force,
            test_uniform_random_xi_consistent_with_zero,
            test_pair_across_periodic_boundary_uses_minimum_image,
            test_pairs_counted_once_and_self_pairs_excluded,
            test_fewer_than_two_points_returns_zero_counts_and_nan_xi,
            test_known_clustered_pair_gives_expected_xi,
            test_every_domain_violation_raises,
            test_capped_grid_matches_brute_force_on_clustered_points,
            test_separation_tile_matches_reference_and_stays_within_memory_bound,
            test_wrap_into_box_maps_the_boundary_to_zero_and_keeps_inside_points,
            test_require_full_box_reports_a_skip_only_for_partial_or_missing_box,
            test_log_radial_edges_spacing_and_range_guard,
            test_xi_series_selects_drawable_bins_and_counts_hidden_ones,
            test_read_module_parameters_returns_floats_and_names_the_missing,
        ],
        "Correlation function helper (test_correlation_function.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
