#!/usr/bin/env python3
"""Unit tests for the physics helpers inside the hod and sham figure packages.

Covers: the HOD occupation law() (and its mean_lambda and mean_ncen parts), host_lookup(),
the unmatched-satellite count of realised_occupation(),
nfw_enclosed_fraction() and the SHAM stellar-mass-function target_mass_function() with its
conversion to the simulation's h. Needs no Mimic output.
"""

import importlib
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
MIMIC_PLOT_DIR = HERE.parent
REPO_ROOT = HERE.parent.parent.parent
sys.path.insert(0, str(MIMIC_PLOT_DIR))
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite


def load_figure_package(model):
    """Import models/<model>/plots/figures as `figures`, replacing any earlier package."""
    for name in [m for m in sys.modules if m == "figures" or m.startswith("figures.")]:
        del sys.modules[name]
    plots_dir = str(REPO_ROOT / "models" / model / "plots")
    sys.path.insert(0, plots_dir)
    try:
        return importlib.import_module("figures")
    finally:
        sys.path.remove(plots_dir)


HOD = load_figure_package("hod")
HOD_OCCUPATION = HOD.hod_occupation
HOD_PROFILE = HOD.hod_satellite_profile
SHAM_SMF = load_figure_package("sham").stellar_mass_function

OCCUPATION = {
    "HODLogMmin": 12.0,
    "HODSigmaLogM": 0.3,
    "HODLogM0": 11.0,
    "HODLogM1": 13.0,
    "HODAlpha": 1.0,
}


def test_satellite_mean_is_zero_at_and_below_m0():
    """lambda = 0 for M <= 10^HODLogM0, so the satellite mean and variance vanish there."""
    log_m = np.array([8.0, 10.99, 11.0])
    assert HOD_OCCUPATION.mean_lambda(log_m, OCCUPATION).tolist() == [0.0, 0.0, 0.0]
    mean, variance = HOD_OCCUPATION.law(log_m, OCCUPATION)["sat"]
    assert mean.tolist() == [0.0, 0.0, 0.0] and variance.tolist() == [0.0, 0.0, 0.0]


def test_alpha_zero_gives_unit_lambda_above_m0_only():
    """With HODAlpha = 0 the satellite mean is 1 above the cutoff and 0 at or below it."""
    flat = dict(OCCUPATION, HODAlpha=0.0)
    lam = HOD_OCCUPATION.mean_lambda(np.array([10.0, 11.0, 11.0001, 13.0, 15.0]), flat)
    assert lam.tolist() == [0.0, 0.0, 1.0, 1.0, 1.0]


def test_law_reproduces_the_zheng_form():
    """<Ncen>, lambda and the three means follow Zheng, Coil & Zehavi (2007) at known masses."""
    par = dict(OCCUPATION, HODAlpha=1.5)
    log_m = np.array([12.0, 13.0])
    p = HOD_OCCUPATION.mean_ncen(log_m, par)
    assert p[0] == 0.5, "<Ncen> = 1/2 at M = Mmin"
    assert np.isclose(p[1], 0.5 * (1.0 + math.erf(1.0 / 0.3)), rtol=1e-14)
    lam = ((10.0**13 - 10.0**11) / 10.0**13) ** 1.5
    assert np.isclose(HOD_OCCUPATION.mean_lambda(log_m, par)[1], lam, rtol=1e-12)
    law = HOD_OCCUPATION.law(log_m, par)
    assert np.allclose(law["cen"][0], p)
    assert np.isclose(law["sat"][0][1], p[1] * lam, rtol=1e-12)
    assert np.isclose(law["tot"][0][1], p[1] * (1.0 + lam), rtol=1e-12)


def test_law_variances_match_exact_enumeration():
    """Per-host variances equal the exact moments of Ncen ~ Bernoulli(p), Nsat ~ Poisson(lam)."""
    log_m = np.array([11.5, 12.0, 12.4, 13.2])
    law = HOD_OCCUPATION.law(log_m, OCCUPATION)
    p = HOD_OCCUPATION.mean_ncen(log_m, OCCUPATION)
    lam = HOD_OCCUPATION.mean_lambda(log_m, OCCUPATION)
    for i in range(len(log_m)):
        pmf = [math.exp(-lam[i]) * lam[i] ** n / math.factorial(n) for n in range(120)]
        moments = {key: [0.0, 0.0] for key in ("sat", "tot")}
        for present, weight in ((1, p[i]), (0, 1.0 - p[i])):
            for n, mass in enumerate(pmf):
                nsat = present * n
                for key, value in (("sat", nsat), ("tot", present * (1 + n))):
                    moments[key][0] += weight * mass * value
                    moments[key][1] += weight * mass * value**2
        for key, (first, second) in moments.items():
            assert np.isclose(law[key][0][i], first, rtol=1e-10, atol=1e-14), (key, i)
            assert np.isclose(law[key][1][i], second - first**2, rtol=1e-9, atol=1e-14), (key, i)
    assert np.allclose(law["cen"][1], p * (1.0 - p))


def make_halos(rows):
    """Build a halo recarray from (Type, HODGhost, UniqueGalaxyID, UniqueCentralGalaxyID) rows."""
    dtype = [
        ("Type", np.int32),
        ("HODGhost", np.int32),
        ("UniqueGalaxyID", np.int64),
        ("UniqueCentralGalaxyID", np.int64),
    ]
    return np.array(rows, dtype=dtype).view(np.recarray)


def test_host_lookup_maps_each_satellite_to_its_host_row():
    """Satellites resolve to the row of the Type 0 row carrying their central ID, in any order."""
    halos = make_halos(
        [
            (0, 0, 300, 300),  # 0 host, IDs deliberately out of order
            (0, 1, 100, 100),  # 1 host drawn absent
            (2, 0, -5, 100),  # 2 satellite of row 1
            (1, 0, 7, 7),  # 3 Type 1 row: neither host nor satellite
            (2, 0, -6, 300),  # 4 satellite of row 0
            (2, 1, -7, 300),  # 5 scaffold satellite: not in the sample
            (2, 0, -8, 999),  # 6 satellite whose host is absent
            (0, 0, 200, 200),  # 7 host
            (2, 0, -9, 200),  # 8 satellite of row 7
        ]
    )
    host_rows, sat_rows, sat_host = HOD.host_lookup(halos)
    assert sorted(host_rows.tolist()) == [0, 1, 7]
    assert sat_rows.tolist() == [2, 4, 8], "Only sample satellites with a found host remain"
    assert sat_host.tolist() == [1, 0, 7]


def test_host_lookup_with_no_hosts_or_no_satellites_is_empty():
    """Either population empty gives empty satellite arrays, never an indexing error."""
    only_hosts = make_halos([(0, 0, 1, 1), (0, 0, 2, 2)])
    hosts, sats, sat_host = HOD.host_lookup(only_hosts)
    assert hosts.tolist() == [0, 1] and len(sats) == 0 and len(sat_host) == 0
    only_sats = make_halos([(2, 0, -1, 9)])
    hosts, sats, sat_host = HOD.host_lookup(only_sats)
    assert len(hosts) == 0 and len(sats) == 0 and len(sat_host) == 0


def test_realised_occupation_counts_satellites_whose_host_is_missing():
    """Satellites with no Type 0 host are counted as unmatched and enter no host's occupation."""
    dtype = [
        ("Type", np.int32),
        ("Mvir", np.float32),
        ("HODGhost", np.int32),
        ("UniqueGalaxyID", np.int64),
        ("UniqueCentralGalaxyID", np.int64),
    ]
    halos = np.array(
        [
            (0, 100.0, 0, 1, 1),  # host of 1e12 Msun/h with its central
            (2, 0.0, 0, -1, 1),  # satellite of that host
            (2, 0.0, 0, -2, 999),  # satellite whose host is not in the data
        ],
        dtype=dtype,
    ).view(np.recarray)
    edges = np.array([11.5, 12.5])
    occ = HOD_OCCUPATION.realised_occupation(halos, edges, OCCUPATION)
    assert occ["unmatched"] == 1
    assert occ["hosts"].tolist() == [1]
    assert occ["cen"][0] == 1.0 and occ["sat"][0] == 1.0 and occ["tot"][0] == 2.0


def test_nfw_enclosed_fraction_is_continuous_across_the_series_switch():
    """The series branch (c < 0.1) and the closed form (c >= 0.1) agree where they meet."""
    switch = HOD_PROFILE.NFW_SERIES_LIMIT
    x = np.linspace(0.0, 1.0, 41)
    below = HOD_PROFILE.nfw_enclosed_fraction(x, np.nextafter(switch, 0.0))
    above = HOD_PROFILE.nfw_enclosed_fraction(x, switch)
    assert np.allclose(below, above, rtol=0.0, atol=1e-12), np.abs(below - above).max()
    # Each branch also meets the textbook closed form away from the switch.
    for c in (0.05, 0.5, 5.0, 50.0):
        reference = (np.log1p(c * x) - c * x / (1.0 + c * x)) / (np.log1p(c) - c / (1.0 + c))
        got = HOD_PROFILE.nfw_enclosed_fraction(x, c)
        assert np.allclose(got, reference, rtol=1e-8, atol=1e-10), c


def test_nfw_enclosed_fraction_limits_and_monotonicity():
    """0 at the centre, 1 at the virial radius, monotone, and finite for tiny concentrations."""
    x = np.linspace(0.0, 1.0, 201)
    for c in (1e-8, 1e-3, 0.09, 0.1, 1.0, 10.0, 300.0):
        f = HOD_PROFILE.nfw_enclosed_fraction(x, c)
        assert np.all(np.isfinite(f)), c
        assert f[0] == 0.0 and np.isclose(f[-1], 1.0, rtol=1e-12), c
        assert np.all(np.diff(f) >= 0.0), c
    assert np.allclose(HOD_PROFILE.nfw_enclosed_fraction(x, 1e-8), x**2, atol=1e-7)


TARGET = {
    "ShamTargetLogMstar": 10.66,
    "ShamTargetPhi1": 3.96e-3,
    "ShamTargetAlpha1": -0.35,
    "ShamTargetPhi2": 0.79e-3,
    "ShamTargetAlpha2": -1.47,
    "ShamTargetHubble": 0.7,
    "ShamTargetLogMassFloor": 8.0,
}


def schechter(log_mass, log_ms, phi1, phi2):
    """Double Schechter per dex with the target's slopes, from converted parameters."""
    x = 10.0 ** (np.asarray(log_mass) - log_ms)
    slopes = (TARGET["ShamTargetAlpha1"] + 1.0, TARGET["ShamTargetAlpha2"] + 1.0)
    return np.log(10.0) * np.exp(-x) * (phi1 * x ** slopes[0] + phi2 * x ** slopes[1])


def test_target_mass_function_converts_to_the_simulation_h():
    """The h conversion reproduces the two worked examples in the sham_rank_match README."""
    log_mass = np.linspace(8.0, 12.0, 9)
    worked = (
        (0.6774, 10.68851, 3.58870e-3, 7.15927e-4),
        (0.73, 10.62355, 4.49127e-3, 8.95987e-4),
    )
    for h_sim, log_ms, phi1, phi2 in worked:
        got = SHAM_SMF.target_mass_function(log_mass, TARGET, h_sim)
        expected = schechter(log_mass, log_ms, phi1, phi2)
        # The quoted values carry five significant figures, which bounds the agreement.
        assert np.allclose(got, expected, rtol=3e-4, atol=0.0), (h_sim, got / expected)


def test_target_mass_function_is_unconverted_at_the_published_h():
    """At h_sim = ShamTargetHubble the published parameters apply unchanged."""
    log_mass = np.linspace(8.0, 12.0, 9)
    got = SHAM_SMF.target_mass_function(log_mass, TARGET, TARGET["ShamTargetHubble"])
    expected = schechter(log_mass, 10.66, 3.96e-3, 0.79e-3)
    assert np.allclose(got, expected, rtol=1e-12, atol=0.0)


def main():
    """Run this file's tests via the shared framework runner."""
    return run_test_suite(
        [
            test_satellite_mean_is_zero_at_and_below_m0,
            test_alpha_zero_gives_unit_lambda_above_m0_only,
            test_law_reproduces_the_zheng_form,
            test_law_variances_match_exact_enumeration,
            test_host_lookup_maps_each_satellite_to_its_host_row,
            test_host_lookup_with_no_hosts_or_no_satellites_is_empty,
            test_realised_occupation_counts_satellites_whose_host_is_missing,
            test_nfw_enclosed_fraction_is_continuous_across_the_series_switch,
            test_nfw_enclosed_fraction_limits_and_monotonicity,
            test_target_mass_function_converts_to_the_simulation_h,
            test_target_mass_function_is_unconverted_at_the_published_h,
        ],
        "Figure package helpers (test_figure_helpers.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
