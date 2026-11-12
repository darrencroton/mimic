#!/usr/bin/env python3
"""
sage26 registered figures, rendered from a synthetic catalogue (models/sage26/plots/figures/).

Validates: the generic cases of tests/framework/plot_checks.py for every registered figure: it
           renders a non-empty PNG and returns no skip message; at least one artist tagged
           gid="model" holds a finite data point; every field it reads unconditionally is in its
           PLOT_REQUIREMENTS entry (and, for an evolution figure, in its EVOLUTION_PLOT_FIELDS
           entry) and it runs without any one of its optional fields
           the derivation cases of sage26_plot_derivations.py: a hand-built catalogue per figure
           whose plotted points are known literals (mass-function normalisation, the red/blue
           split, the SFR and stellar-mass densities, the Sb/c selection, the metallicity
           conversion; the CGM curve of baryon_fraction, the CGM series of
           mass_reservoir_scatter, the H2 and H I lines of gas_mass_function, and the selection
           and unit conversion of mass_loading_vs_vvir, stellar_vs_halo_mass_by_regime,
           sfr_vs_halo_accretion_rate, tcool_vs_mvir and cgm_fraction_vs_mvir)
           the labels where they carry a unit or a convention that differs from SAGE26's; the
           colour bar of tcool_vs_mvir; the run parameters the rcool figure reads; the sampling cap
           of each sampled figure; a unit case for each figure-local helper and for the shared
           binning helpers; the bin, threshold and window constants a figure holds only inline;
           the snapshot-selection helpers (inclusive z-range ends, empty bins, nearest snapshot,
           its tie, order independence); and end-to-end wiring cases for the evolution figures,
           each a one-snapshot catalogue whose plotted point is a hand-derived literal
Tolerance: none for field sets, caps, selections and the reservoir helper (its literals are
           dyadic, so the sums are exact); plotted points of the derivation cases to a relative
           1e-5 (float32 input); helper unit cases to a relative 1e-12 (decimal literals) or
           exactly (dyadic literals); the smoothed-histogram peak to an absolute 2e-3; the cosmic
           age to 5e-4 (4e-3 where the helper's trapezoid error of 0.31 percent is the limit)

Every expectation is a literal written from the figure's source text or from SAGE26's plotting
scripts, never computed from the code under test. Figures render with MPLBACKEND=Agg into a
temporary directory under $TMPDIR, removed at the end. It needs no Mimic run.

Run: mimic_venv/bin/python models/sage26/plots/tests/test_sage26_figures.py
"""

import contextlib
import io
import math
import os
import sys
from pathlib import Path

# Set before Matplotlib is imported (the figure package imports it)
os.environ["MPLBACKEND"] = "Agg"

import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(HERE))

import sage26_plot_fixtures as fixtures
from framework.plot_checks import PlotHarness, model_lines, tagged_model_artists
from framework.plot_derivations import derivation_cases
from sage26_plot_derivations import DERIVATIONS, INTEGER_FIELDS
from sage26_plot_fixtures import HUBBLE_H, build_catalogue, build_evolution_input

HARNESS = PlotHarness("sage26", fixtures, model_gid=True)


def close(got, expected, rel=1e-12):
    """Assert two arrays agree to a relative tolerance."""
    got = np.asarray(got, dtype=float)
    expected = np.asarray(expected, dtype=float)
    assert got.shape == expected.shape, f"shape {got.shape}, expected {expected.shape}"
    assert np.allclose(got, expected, rtol=rel, atol=0.0), f"{got.tolist()} != {expected.tolist()}"


def f32(values):
    """Return the values as a float32 array, the type of the output's float fields."""
    return np.asarray(values, dtype=np.float32)


def is_colorbar_axes(ax):
    """True when Matplotlib's colour-bar reference is set on the axes."""
    return getattr(ax, "_colorbar", None) is not None


def data_axes(fig):
    """Return the data axes of a figure (colour-bar axes left out), in creation order."""
    return [ax for ax in fig.axes if not is_colorbar_axes(ax)]


def axes_labels(ax):
    """Return the labels of one axes: x, y, z (3-D only), title, left and right title."""
    return {
        "x": ax.get_xlabel(),
        "y": ax.get_ylabel(),
        "z": ax.get_zlabel() if hasattr(ax, "get_zlabel") else "",
        "title": ax.get_title("center"),
        "title_left": ax.get_title("left"),
        "title_right": ax.get_title("right"),
    }


def describe_figure(fig):
    """Return (data-axes label dicts, colour-bar labels), each in creation order."""
    data = [axes_labels(ax) for ax in data_axes(fig)]
    bars = [ax.get_ylabel() or ax.get_xlabel() for ax in fig.axes if is_colorbar_axes(ax)]
    return data, bars


# --------------------------------------------------------------------------------------
# Reservoir helper
# --------------------------------------------------------------------------------------
def test_reservoir_helper_returns_seven_reservoirs_and_their_exact_sum():
    """baryon_reservoirs() on literal arrays: seven reservoirs, CGMgas among them, exact total.

    Two groups: galaxies 0-1 (central 0, Mvir 8) and galaxies 2-4 (central 2, Mvir 16). Every
    value is a small dyadic number, so each group sum and fraction is exact in float64.
    Group sums (group 0, group 1): StellarMass (2, 8), ColdGas (4, 4), HotGas (8, 0),
    CGMgas (1, 8), EjectedGas (2, 0), ICS (0, 1), BlackHoleMass (0, 1); dividing by Mvir (8, 16)
    gives the literals below.
    """
    helper = HARNESS.figure_module("baryon_fraction").baryon_reservoirs
    galaxies = {
        "StellarMass": np.array([1, 1, 2, 2, 4], dtype=np.float32),
        "ColdGas": np.array([2, 2, 1, 1, 2], dtype=np.float32),
        "HotGas": np.array([4, 4, 0, 0, 0], dtype=np.float32),
        "CGMgas": np.array([1, 0, 4, 4, 0], dtype=np.float32),
        "EjectedGas": np.array([0, 2, 0, 0, 0], dtype=np.float32),
        "ICS": np.array([0, 0, 1, 0, 0], dtype=np.float32),
        "BlackHoleMass": np.array([0, 0, 0, 1, 0], dtype=np.float32),
    }
    optional = {field: True for field in galaxies}
    expected = {
        "stars": [0.25, 0.5],
        "cold": [0.5, 0.25],
        "hot": [1.0, 0.0],
        "cgm": [0.125, 0.5],
        "ejected": [0.25, 0.0],
        "ics": [0.0, 0.0625],
        "bh": [0.0, 0.0625],
        "baryons": [2.125, 1.375],
    }
    groups = {
        "group_of_galaxy": np.array([0, 0, 1, 1, 1]),
        "n_groups": 2,
        "central_group": np.array([0, 1]),
        "central_mvir": np.array([8.0, 16.0]),
    }
    got = helper(galaxies, optional, **groups)
    assert set(got) == set(expected), f"reservoirs {sorted(got)}, expected {sorted(expected)}"
    for key, values in expected.items():
        assert got[key].tolist() == values, f"{key}: {got[key].tolist()}, expected {values}"
    total = sum(got[key] for key in expected if key != "baryons")
    assert np.array_equal(got["baryons"], total), "the total is not the sum of the reservoirs"

    # A reservoir the run does not write contributes zeros and is left out of the total
    without_cgm = helper(galaxies, {**optional, "CGMgas": False}, **groups)
    assert without_cgm["cgm"].tolist() == [0.0, 0.0]
    assert without_cgm["baryons"].tolist() == [2.0, 0.875]


# --------------------------------------------------------------------------------------
# Labels that carry a unit or a convention, and the colour bar of tcool_vs_mvir
# --------------------------------------------------------------------------------------
def test_labels__units_and_conventions_that_differ_from_sage26():
    """Three labels state something a reader acts on.

    mass_reservoir_scatter plots masses in Msun (mass_to_msun), so its x label drops SAGE26's
    h^-1 and its y label names the log and the unit; the figures that carry a redshift in the
    title (concentration_vs_mvir per panel, sfr_vs_halo_accretion_rate) show the snapshot's z.
    """
    axes = describe_figure(HARNESS.render("mass_reservoir_scatter").fig)[0]
    assert [a["x"] for a in axes] == [r"log$_{10}$ M$_{\rm vir}$ [M$_{\odot}$]"], axes
    assert [a["y"] for a in axes] == [r"log$_{10}$ M$_{\rm reservoir}$ [M$_{\odot}$]"], axes
    titles = [a["title"] for a in describe_figure(HARNESS.render("concentration_vs_mvir").fig)[0]]
    assert titles == ["Centrals — z = 0.00", "Satellites — z = 0.00"], titles
    axes = describe_figure(HARNESS.render("sfr_vs_halo_accretion_rate").fig)[0]
    assert [a["title"] for a in axes] == ["Centrals — z = 0.00"], axes


def test_labels__tcool_vs_mvir_with_only_regimes_0_and_1_has_no_colour_bar():
    """As in SAGE26: with only regimes 0 and 1 there is no colour bar (the default catalogue's
    satellites, of regime -1, bring the T_vir colour bar; see the derivation case)."""
    catalogue = build_catalogue()
    catalogue.Regime = np.where(np.isin(catalogue.Regime, (0, 1)), catalogue.Regime, 0)
    assert np.all(np.isin(catalogue.Regime, (0, 1))), "a Regime outside 0 and 1 remains"
    run = HARNESS.call_figure("tcool_vs_mvir", catalogue, "tcool-no-unclassified")
    assert run.skip is None and run.fig is not None, f"tcool_vs_mvir skipped: {run.skip}"
    data, bars = describe_figure(run.fig)
    assert bars == [], f"colour-bar labels {bars}"
    assert len(data) == 1, f"{len(data)} data axes"


# --------------------------------------------------------------------------------------
# Run parameters the rcool figure reads from the run file
# --------------------------------------------------------------------------------------
def rcool_run_with_mshock(mshock, label):
    """Run rcool_to_rvir_above_mshock on one snapshot of three haloes of 1.37e12 Msun.

    Mvir 100 and 60 (1e10 Msun/h at h = 0.73) are 1.37e11 x 10 = 1.37e12 and 8.22e11 Msun; their
    ratios are 0.5, 1.5 and 1.5. ``mshock`` is the run's MShockMsun, or None to leave it out.
    """
    params = fixtures.run_parameters()
    if mshock is None:
        del params["EnabledModules"]["parameters"]["MShockMsun"]
    else:
        params["EnabledModules"]["parameters"]["MShockMsun"] = mshock
    spec = {63: (0.0, {"Mvir": f32([100, 100, 60]), "RcoolToRvir": f32([0.5, 1.5, 1.5])})}
    run = HARNESS.call_figure(
        "rcool_to_rvir_above_mshock", HARNESS.hand_built(spec), label, params=params
    )
    assert run.skip is None and run.fig is not None, f"skipped: {run.skip}"
    ax = data_axes(run.fig)[0]
    return ax.get_title(), [t.get_text() for t in ax.get_legend().get_texts()]


def test_run_parameters__rcool_title_and_selection_use_the_runs_mshock_msun():
    """MShockMsun of 1e12 is in the title and in the selection, so a misspelled key could not
    silently fall back to SAGE26's 6e11: the haloes of 1.37e12 and 8.22e11 Msun give N = 3
    above 6e11 but N = 2 above 1e12 (one of two above ratio 1)."""
    title, legend = rcool_run_with_mshock(1.0e12, "rcool-mshock-1e12")
    assert "10^{12}" in title, f"the title should carry the run's 1e12 shock mass: {title}"
    assert legend == ["z = 0.0  (N=2, f$_{>1}$=0.50)"], f"legend {legend}"
    title, legend = rcool_run_with_mshock(6.0e11, "rcool-mshock-6e11")
    assert legend == ["z = 0.0  (N=3, f$_{>1}$=0.67)"], f"legend {legend}"


def test_run_parameters__missing_key_warns_and_uses_the_fallback():
    """Without MShockMsun in the run the figure warns and uses SAGE26's 6e11."""
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        title, _ = rcool_run_with_mshock(None, "rcool-mshock-missing")
    out = buffer.getvalue()
    assert "WARNING" in out and "MShockMsun" in out, f"no fallback warning: {out!r}"
    assert "10^{11}" in title, f"the title should carry the 6e11 fallback: {title}"


# --------------------------------------------------------------------------------------
# Cap cases: the sample size of each sampled figure
# --------------------------------------------------------------------------------------
# name -> literal cap: SAGE26's script-level `dilute` 7500, `dilute_sfr` 10000 and `dilute`
# reassigned to 100000
CAPS = {
    "spatial_distribution_3d_box": 7500,
    "star_formation_rate": 10000,
    "mass_loading_vs_vvir": 7500,
    "stellar_vs_halo_mass_by_regime": 7500,
    "specific_star_formation_rate": 100000,
    "concentration_vs_mvir": 100000,  # per panel
    "sfr_vs_halo_accretion_rate": 100000,
    "ejection_efficiency_vs_vvir": 100000,
    "disk_radius_vs_stellar_mass": 100000,
}
LOWERED_CAP = 40  # below every figure's candidate count in the synthetic catalogue
# The figures/_sampling.py helper each figure calls (draw_sample unless named here)
SAMPLERS = {
    "concentration_vs_mvir": "draw_sorted_sample",
    "specific_star_formation_rate": "draw_sample_positions",
}


def test_cap__sampled_figures_hold_their_literal_cap_and_sample_to_it():
    """Each sampled figure's DILUTE is SAGE26's literal, and with the cap lowered to 40 the
    figure's sampler is asked for 40 of more candidates (twice for the two panels of
    concentration_vs_mvir) and returns 40; a plain scatter then plots exactly 40 points."""
    for name, cap in CAPS.items():
        module = HARNESS.figure_module(name)
        assert module.DILUTE == cap, f"{name}: DILUTE {module.DILUTE}, expected {cap}"

        sampler = SAMPLERS.get(name, "draw_sample")
        original_cap, original_sample = module.DILUTE, getattr(module, sampler)
        drawn = []

        def recording_sample(candidates, rng, dilute, original=original_sample, drawn=drawn):
            sample = original(candidates, rng, dilute)
            drawn.append((len(candidates), len(sample)))
            return sample

        module.DILUTE = LOWERED_CAP
        setattr(module, sampler, recording_sample)
        try:
            run = HARNESS.call_figure(name, build_catalogue(), f"{name}-lowered-cap")
        finally:
            module.DILUTE = original_cap
            setattr(module, sampler, original_sample)
        assert run.skip is None, f"{name}: skipped with the lowered cap: {run.skip}"
        expected_calls = 2 if name == "concentration_vs_mvir" else 1
        assert len(drawn) == expected_calls, f"{name}: {len(drawn)} sampling calls: {drawn}"
        for candidates, size in drawn:
            assert candidates > LOWERED_CAP, f"{name}: only {candidates} candidates"
            assert size == LOWERED_CAP, f"{name}: sample of {size}, expected {LOWERED_CAP}"
        if name in ("star_formation_rate", "mass_loading_vs_vvir", "spatial_distribution_3d_box"):
            plotted = [points for _, points in tagged_model_artists(run.fig) if points]
            assert plotted == [LOWERED_CAP], f"{name}: plotted points {plotted}"


# --------------------------------------------------------------------------------------
# Unit cases of the helpers the figures add. Each expectation is a literal written from SAGE26's
# expression, not computed by the helper.
# --------------------------------------------------------------------------------------
# (figure, helper, arguments, expected): the one-expression helpers, each pinning the quantity a
# figure plots or the order of its arguments
ONE_LINERS = [
    # BulgeMass / StellarMass, MergerBulgeMass / StellarMass, InstabilityBulgeMass / StellarMass
    (
        "bulge_to_total",
        "bulge_to_total_ratios",
        (np.array([4.0, 8.0]), np.array([1.0, 4.0]), np.array([2.0, 1.0]), np.array([3.0, 2.0])),
        [[0.25, 0.5], [0.5, 0.125], [0.75, 0.25]],
    ),
    # SfrDisk + SfrBulge
    (
        "star_formation_rate",
        "total_sfr",
        (np.array([1.5, 0.25]), np.array([0.5, 2.0])),
        [2.0, 2.25],
    ),
    # MdotCool + MdotStream
    (
        "sfr_vs_halo_accretion_rate",
        "halo_accretion_rate",
        (np.array([1.5, 0.0]), np.array([0.5, 2.0])),
        [2.0, 2.0],
    ),
    # 35.9 * Vvir**2: Vvir 10 and 100 km/s give 3590 and 359000 K
    ("tcool_vs_mvir", "virial_temperature", (np.array([10.0, 100.0]),), [3590.0, 359000.0]),
    # np.log10(CGMgas / Mvir) * 0.17 (SAGE26's scaling of the logarithm, kept as written): CGMgas
    # 1 over Mvir 10 and 100 is -1 and -2, so -0.17 and -0.34
    (
        "cgm_fraction_vs_mvir",
        "cgm_fraction",
        (np.array([1.0, 1.0]), np.array([10.0, 100.0])),
        [-0.17, -0.34],
    ),
    # DiskRadius * 1e3 * 1.68: 0.01 and 0.002 Mpc/h give 16.8 and 3.36 kpc/h
    (
        "disk_radius_vs_stellar_mass",
        "effective_disk_radius",
        (np.array([0.01, 0.002]),),
        [16.8, 3.36],
    ),
    # Rvir * 1e3: 0.25 and 0.5 Mpc/h give 250 and 500 kpc/h
    ("disk_radius_vs_stellar_mass", "virial_radius_kpc", (np.array([0.25, 0.5]),), [250.0, 500.0]),
    # np.sum(types == 0) and np.sum(types >= 1): orphans (Type 2) are satellites
    (
        "central_satellite_counts",
        "count_centrals_and_satellites",
        (np.array([0, 0, 1, 2, 3]),),
        [2, 3],
    ),
]


def test_unit__one_expression_helpers():
    """Each one-expression helper returns the literal of SAGE26's expression (see ONE_LINERS)."""
    for figure, helper, arguments, expected in ONE_LINERS:
        got = getattr(HARNESS.figure_module(figure), helper)(*arguments)
        try:
            close(got, expected)
        except AssertionError as error:
            raise AssertionError(f"{figure}.{helper}: {error}") from None


def test_unit__bulge_to_total_bins():
    """As in SAGE26: 0.25 dex bins from 8.5 to 12.5, a bin needs 5 galaxies.

    Bin 0 (8.5 to 8.75) holds 4 galaxies, so it is not summarised; bin 1 (8.75 to 9.0) holds
    exactly 5 B/T of 0.1 to 0.5, whose median is 0.3 and whose 16th and 84th percentiles (linear
    interpolation at 0.64 and 3.36 of 4 intervals) are 0.164 and 0.436. One galaxy in the first
    bin and five in the last (12.25 to 12.5, centre 12.375) summarise only the sixteenth bin.
    """
    helper = HARNESS.figure_module("bulge_to_total").bulge_to_total_bins
    mass = np.array([8.6, 8.6, 8.7, 8.7] + [8.8, 8.8, 8.9, 8.9, 8.95])
    btot = np.array([0.9, 0.9, 0.9, 0.9] + [0.1, 0.2, 0.3, 0.4, 0.5])
    merger = np.array([0.0] * 4 + [0.1, 0.1, 0.1, 0.1, 0.1])
    instab = np.array([0.0] * 4 + [0.0, 0.25, 0.25, 0.25, 0.5])
    centres, med, lo, hi, merger_mean, instab_mean = helper(mass, btot, merger, instab)
    assert len(centres) == 16, f"{len(centres)} bin centres"
    close(centres[:3], [8.625, 8.875, 9.125])
    assert (
        np.isnan(med[0]) and np.isnan(lo[0]) and np.isnan(hi[0])
    ), "the 4-galaxy bin is summarised"
    assert np.isnan(merger_mean[0]) and np.isnan(instab_mean[0])
    close(med[1], 0.3)
    close(lo[1], 0.164)
    close(hi[1], 0.436)
    close(merger_mean[1], 0.1)
    close(instab_mean[1], 0.25)
    assert np.all(np.isnan(med[2:])), "an empty bin is summarised"
    mass = np.array([8.6] + [12.3, 12.35, 12.4, 12.4, 12.45])
    ratio = np.full(6, 0.5)
    centres, med, *_ = helper(mass, ratio, ratio, ratio)
    close(centres[[0, -1]], [8.625, 12.375])
    assert np.isfinite(med).tolist() == [False] * 15 + [True], f"summarised bins {med.tolist()}"


def test_unit__log_specific_sfr():
    """As in SAGE26: log10((SfrDisk + SfrBulge) / StellarMass), -99 where the stellar mass is not
    above zero. Rates 2, 0, 4, 4 over masses 4, 0, 16, 0 give log10 0.5, -99, log10 0.25, -99."""
    helper = HARNESS.figure_module("specific_star_formation_rate").log_specific_sfr
    got = helper(
        np.array([1.0, 0.0, 3.0, 3.0]),
        np.array([1.0, 0.0, 1.0, 1.0]),
        np.array([4.0, 0.0, 16.0, 0.0]),
    )
    close(got, [math.log10(0.5), -99.0, math.log10(0.25), -99.0])


def test_unit__smoothed_histogram():
    """As in SAGE26: ten galaxies in one cell at (10.0, -10.0).

    The cell is Gaussian-smoothed with sigma 1.5 cells, so the peak is 10 / (2 pi 1.5^2) =
    0.7074 (the kernel is cut at 4 sigma) and the total stays 10. Rows above the clip line are
    zeroed: clipping above -11 removes every cell of a population at -10, clipping below -11
    keeps all of it. The grid is np.linspace(8.0, 12.2, 100) by np.linspace(-13, -8, 100), 99
    cells each way.
    """
    helper = HARNESS.figure_module("specific_star_formation_rate").smoothed_histogram
    x = np.full(10, 10.0)
    y = np.full(10, -10.0)
    h, x_centers, y_centers = helper(x, y)
    assert h.shape == (99, 99), f"shape {h.shape}"
    assert abs(h.max() - 10.0 / (2.0 * math.pi * 1.5**2)) < 2e-3, f"peak {h.max()}"
    assert abs(h.sum() - 10.0) < 1e-6, f"total {h.sum()}"
    close(x_centers[[0, -1]], [8.0 + 0.5 * 4.2 / 99, 12.2 - 0.5 * 4.2 / 99])
    close(y_centers[[0, -1]], [-13.0 + 0.5 * 5.0 / 99, -8.0 - 0.5 * 5.0 / 99])
    above, _, _ = helper(x, y, clip_above=-11.0)
    assert above.sum() == 0.0, f"clip_above kept {above.sum()}"
    below, _, _ = helper(x, y, clip_below=-11.0)
    assert abs(below.sum() - 10.0) < 1e-6, f"clip_below kept {below.sum()}"
    # A population at -12 is the other way about
    low_y = np.full(10, -12.0)
    kept, _, _ = helper(x, low_y, clip_above=-11.0)
    assert abs(kept.sum() - 10.0) < 1e-6, f"clip_above kept {kept.sum()} of a population at -12"
    dropped, _, _ = helper(x, low_y, clip_below=-11.0)
    assert dropped.sum() == 0.0, f"clip_below kept {dropped.sum()} of a population at -12"


def test_unit__sigma_levels():
    """As in SAGE26: the cells 60, 20, 10, 5, 3, 1, 1 (and zeros) sum to 100, so the cumulative
    fractions are .60 .80 .90 .95 .98 .99 1.0; the first at or above 0.997 is the last cell (1),
    at or above 0.95 the fourth (5), at or above 0.68 the second (20)."""
    helper = HARNESS.figure_module("specific_star_formation_rate").sigma_levels
    cells = np.array([[1.0, 60.0, 0.0], [5.0, 0.0, 20.0], [10.0, 3.0, 1.0]])
    assert helper(cells) == [1.0, 5.0, 20.0], f"levels {helper(cells)}"
    assert helper(np.zeros((3, 3))) is None, "an empty histogram has levels"


def test_unit__ejection_efficiency():
    """As in SAGE26 (epsilon_disk 2.9, epsilon_halo 0.3, V_sn 501).

    (1) MassLoading 2.9 (f = 1), rate 1, Vvir 100: E_fb = 0.5 * 0.3 * 1 * 1 * 251001 = 37650.15,
    E_lift = 0.5 * 2.9 * 1e4 = 14500, eject = 23150.15 / 5000 = 4.63003, efficiency 4.63003.
    (2) the same at Vvir 1000: E_lift = 1.45e6 exceeds E_fb, so the ejection is clipped to 0.
    (3) MassLoading 5.8 (f = 2), disk 1 + bulge 1 (rate 2), Vvir 100: E_fb = 0.5 * 0.3 * 2 * 2 *
    251001 = 150600.6, E_lift = 0.5 * 11.6 * 1e4 = 58000, eject = 92600.6 / 5000 = 18.52012,
    efficiency 9.26006.
    """
    helper = HARNESS.figure_module("ejection_efficiency_vs_vvir").ejection_efficiency
    got = helper(
        np.array([2.9, 2.9, 5.8]),
        np.array([0.25, 0.25, 1.0]),
        np.array([0.75, 0.75, 1.0]),
        np.array([100.0, 1000.0, 100.0]),
    )
    close(got, [4.63003, 0.0, 9.26006], rel=1e-9)


# --------------------------------------------------------------------------------------
# Bin, threshold and window constants the figures hold only inline
# --------------------------------------------------------------------------------------
def test_bins__specific_star_formation_rate_constants():
    """As in SAGE26: np.linspace(8.0, 12.2, 100) and np.linspace(-13, -8, 100); sSFRcut = -11.0
    (drawn as the dashed line); -99 for a galaxy with no stellar mass."""
    module = HARNESS.figure_module("specific_star_formation_rate")
    assert len(module.X_BINS) == 100, f"{len(module.X_BINS)} x edges"
    close([module.X_BINS[0], module.X_BINS[-1]], [8.0, 12.2])
    close(module.X_BINS[1] - module.X_BINS[0], 4.2 / 99)
    assert len(module.Y_BINS) == 100, f"{len(module.Y_BINS)} y edges"
    close([module.Y_BINS[0], module.Y_BINS[-1]], [-13.0, -8.0])
    close(module.Y_BINS[1] - module.Y_BINS[0], 5.0 / 99)
    assert module.SSFR_CUT == -11.0, f"SSFR_CUT {module.SSFR_CUT}"
    assert module.NO_SSFR == -99.0, f"NO_SSFR {module.NO_SSFR}"
    cut_lines = [
        line
        for ax in HARNESS.render("specific_star_formation_rate").fig.axes
        for line in ax.lines
        if line.get_label() == "sSFR cut = -11.0"
    ]
    assert len(cut_lines) == 1, f"{len(cut_lines)} cut lines"
    assert list(cut_lines[0].get_ydata()) == [-11.0, -11.0], "the cut line is not at -11"


def test_bins__ejection_efficiency_edges():
    """As in SAGE26: np.arange(1.0, 3.0, 0.1), 20 edges from 1.0 to 2.9 and 19 centres 1.05,
    1.15, ... 2.85, which the median line is drawn at."""
    module = HARNESS.figure_module("ejection_efficiency_vs_vvir")
    assert len(module.BIN_EDGES) == 20, f"{len(module.BIN_EDGES)} edges"
    close([module.BIN_EDGES[0], module.BIN_EDGES[-1]], [1.0, 2.9])
    close(module.BIN_EDGES[1] - module.BIN_EDGES[0], 0.1)
    lines = [
        artist
        for artist, _ in tagged_model_artists(HARNESS.render("ejection_efficiency_vs_vvir").fig)
        if artist.get_label() == "Median ± 1σ"
    ]
    assert len(lines) == 1, f"{len(lines)} median lines"
    centres = lines[0].get_xdata()
    assert len(centres) == 19, f"{len(centres)} median-line points"
    close(centres[[0, 1, -1]], [1.05, 1.15, 2.85])


def catalogue_with_stellar_masses(masses_msun):
    """Return the synthetic catalogue whose first galaxies hold the given stellar masses (Msun).

    Every galaxy after them has no stellar mass, so no mass cut passes it. The first galaxies
    also satisfy every other cut of the figures that test a mass threshold (a halo, a virial
    radius, a disk radius, a star formation rate, a concentration), and alternate between
    Type 0 and Type 1.
    """
    catalogue = build_catalogue()
    k = len(masses_msun)
    catalogue.StellarMass[:] = 0.0
    catalogue.StellarMass[:k] = np.asarray(masses_msun, dtype=float) * HUBBLE_H / 1.0e10
    catalogue.Mvir[:k] = 1.0
    catalogue.Rvir[:k] = 0.2 + 0.01 * np.arange(k)
    catalogue.DiskScaleRadius[:k] = 0.01
    catalogue.SfrDisk[:k] = 1.0
    catalogue.SfrBulge[:k] = 0.0
    catalogue.Concentration[:k] = 10.0
    catalogue.Type[:k] = np.arange(k) % 2
    return catalogue


def test_bins__disk_radius_edges():
    """As in SAGE26: Rvir is multiplied by 1e3 (kpc/h) and the bin edges are
    np.logspace(log10(Rvir[w].min()), log10(Rvir[w].max()), 50), so 49 centres.

    Eleven galaxies of log M* 10 (inside the window) with Rvir 0.20, 0.21, ... 0.30 Mpc/h, so
    Rvir[w] runs from 200 to 300 kpc/h. The 50 edges are 200 * r^k for k = 0 .. 49 with r = (300 /
    200)^(1/49) = exp(ln 1.5 / 49) = exp(0.0082748) = 1.0083091. The first centre is (200 + 200 r) /
    2 = 100 (1 + r) = 200.83091 and the last, between the edges 300 / r and 300, is 150 (1 + 1 / r)
    = 298.76390; successive centres are in the ratio r. The median line is drawn at those centres.
    """
    module = HARNESS.figure_module("disk_radius_vs_stellar_mass")
    assert module.N_BIN_EDGES == 50, f"N_BIN_EDGES {module.N_BIN_EDGES}"
    catalogue = catalogue_with_stellar_masses(np.full(11, 1.0e10))
    assert np.allclose(catalogue.Rvir[:11], 0.2 + 0.01 * np.arange(11)), "fixture Rvir changed"
    run = HARNESS.call_figure("disk_radius_vs_stellar_mass", catalogue, "disk-bins")
    assert run.skip is None and run.fig is not None, f"skipped: {run.skip}"
    lines = [
        a
        for a, _ in tagged_model_artists(run.fig)
        if a.get_label() == "Median ± 1σ (matching Somerville+18 selection)"
    ]
    assert len(lines) == 1, f"{len(lines)} median lines"
    centres = np.asarray(lines[0].get_xdata(), dtype=float)
    assert len(centres) == 49, f"{len(centres)} median-line points"
    # Rvir is stored in single precision, so the minimum and maximum carry a relative 1e-7
    close(centres[[0, -1]], [200.83091, 298.76390], rel=1e-6)
    close(centres[1] / centres[0], 1.0083091, rel=1e-6)
    close(centres[-1] / centres[-2], 1.0083091, rel=1e-6)


def contour_sets_by_side(fig, cut):
    """Return (sets above, sets below) the cut: the model contour sets holding vertices, by the
    side of the line y = cut their vertices lie on (the star-forming set is clipped below the cut,
    the quiescent set above it, so no set straddles it)."""
    from matplotlib.contour import ContourSet

    above = below = 0
    for artist, points in tagged_model_artists(fig):
        if not isinstance(artist, ContourSet) or points == 0:
            continue
        y = np.concatenate([np.asarray(path.vertices)[:, 1] for path in artist.get_paths()])
        if np.all(y > cut):
            above += 1
        elif np.all(y < cut):
            below += 1
        else:
            raise AssertionError("a contour set straddles the sSFR cut")
    return above, below


def population_catalogue(n_star_forming, n_quiescent):
    """Return the synthetic catalogue whose first galaxies form two populations around the sSFR
    cut of -11: n_star_forming at log sSFR -10 (+/- 0.2) and n_quiescent at -12 (+/- 0.2), all of
    log M* about 10.5 (+/- 0.3). Every other galaxy has no stellar mass, so it has no sSFR."""
    n = n_star_forming + n_quiescent
    catalogue = build_catalogue()
    rng = np.random.default_rng(7)
    log_mass = rng.normal(10.5, 0.3, n)
    log_ssfr = np.concatenate(
        [rng.normal(-10.0, 0.2, n_star_forming), rng.normal(-12.0, 0.2, n_quiescent)]
    )
    catalogue.StellarMass[:] = 0.0
    catalogue.StellarMass[:n] = 10.0**log_mass * HUBBLE_H / 1.0e10
    catalogue.SfrDisk[:n] = 10.0 ** (log_mass + log_ssfr)
    catalogue.SfrBulge[:n] = 0.0
    return catalogue


def test_threshold__specific_star_formation_rate_population_minimum_is_10():
    """As in SAGE26: `if len(x) < 10: return`, so a population of fewer than 10 galaxies draws no
    contour and one of 10 or more does, each population on its own side of sSFRcut.

    9 star-forming and 12 quiescent galaxies: only the quiescent contours (below -11) are drawn.
    10 star-forming and 9 quiescent: only the star-forming contours (above -11) are drawn.
    """
    for n_star_forming, n_quiescent, want_above, want_below in [
        (9, 12, False, True),
        (10, 9, True, False),
    ]:
        catalogue = population_catalogue(n_star_forming, n_quiescent)
        run = HARNESS.call_figure(
            "specific_star_formation_rate", catalogue, f"ssfr-{n_star_forming}"
        )
        assert run.skip is None and run.fig is not None, f"skipped: {run.skip}"
        above, below = contour_sets_by_side(run.fig, -11.0)
        case = f"{n_star_forming} star-forming, {n_quiescent} quiescent"
        assert (above > 0) == want_above, f"{case}: {above} contour sets above the cut"
        assert (below > 0) == want_below, f"{case}: {below} contour sets below the cut"


def scatter_points(fig):
    """Return the finite points of the model scatters of a figure, summed."""
    from matplotlib.collections import PathCollection

    return sum(
        points for artist, points in tagged_model_artists(fig) if isinstance(artist, PathCollection)
    )


def cut_masses(cut_msun):
    """Six stellar masses below a cut (0.99 to 0.001 of it) and six above (1.01 to 1000 times)."""
    return cut_msun * np.array(
        [0.99, 0.5, 0.2, 0.1, 0.01, 0.001, 1.01, 2.0, 5.0, 10.0, 100.0, 1000.0]
    )


# (figure, stellar masses in Msun, points drawn): six masses below and six above each cut
MASS_CUTS = [
    ("spatial_distribution_3d_box", cut_masses(1.0e9), 6),  # more than 1e9 Msun
    ("star_formation_rate", cut_masses(0.01), 6),  # more than 0.01 Msun
    ("concentration_vs_mvir", cut_masses(1.0e8), 6),  # more than 1e8 Msun, over both panels
    # 9.3 < log10(M*/Msun) < 11.3: three galaxies at each of 9.29, 9.31, 11.29 and 11.31, of
    # which the 9.31 and 11.29 are inside
    ("disk_radius_vs_stellar_mass", 10.0 ** np.array([9.29, 9.31, 11.29, 11.31] * 3), 6),
]


def test_threshold__stellar_mass_cuts_and_windows_of_the_scatter_figures():
    """As in SAGE26: only galaxies above each figure's stellar-mass cut (or inside the disk-radius
    window) are drawn. The masses are in Msun and the catalogue stores x h / 1e10, so a figure
    that dropped the conversion would draw a different number of points."""
    for name, masses, expected in MASS_CUTS:
        run = HARNESS.call_figure(name, catalogue_with_stellar_masses(masses), name + "-cut")
        assert run.skip is None and run.fig is not None, f"{name}: skipped: {run.skip}"
        points = scatter_points(run.fig)
        assert points == expected, f"{name}: {points} points drawn, expected {expected}"


# --------------------------------------------------------------------------------------
# The snapshot-selection helpers (figures/_snapshot_selection.py)
# --------------------------------------------------------------------------------------
# The eight snapshots the mini-Millennium run files write and their redshifts, z = 1 / a - 1 with
# `a` the scale factors of simulations/mini-millennium/mini-millennium.a_list (line n + 1 of the
# file holds snapshot n). Written out, as a second reading of the a_list.
RUN_REDSHIFTS = {
    63: 1 / 1.000000 - 1,
    37: 1 / 0.419161 - 1,
    32: 1 / 0.325730 - 1,
    27: 1 / 0.246280 - 1,
    23: 1 / 0.193070 - 1,
    20: 1 / 0.159012 - 1,
    18: 1 / 0.138950 - 1,
    16: 1 / 0.120887 - 1,
}
# The order the engine supplies the snapshots in: increasing redshift
ENGINE_ORDER = [63, 37, 32, 27, 23, 20, 18, 16]
DECREASING_ORDER = [16, 18, 20, 23, 27, 32, 37, 63]
SHUFFLED_ORDER = [27, 16, 63, 20, 37, 18, 32, 23]

# The fifteen redshift bins of SAGE26's history plotting script
SAGE26_Z_BINS = [
    (0.2, 0.5),
    (0.5, 0.8),
    (0.8, 1.1),
    (1.1, 1.5),
    (1.5, 2.0),
    (2.0, 2.5),
    (2.5, 3.0),
    (3.0, 3.5),
    (3.5, 4.5),
    (4.5, 5.5),
    (5.5, 6.5),
    (6.5, 7.5),
    (7.5, 8.5),
    (8.5, 10.0),
    (10.0, 12.0),
]


def selection_module():
    """Return figures/_snapshot_selection.py, imported by the figure modules."""
    return sys.modules["figures._snapshot_selection"]


def snapshots_with_redshifts(redshift_by_snapshot, order=None):
    """Return an engine-style mapping holding only each snapshot's redshift, in ``order``."""
    order = list(redshift_by_snapshot) if order is None else order
    return {snap: (None, 0.0, {"redshift": redshift_by_snapshot[snap]}) for snap in order}


def test_selection__z_range_includes_both_ends():
    """As in SAGE26: `z_min <= z <= z_max`. Snapshot 11 sits at z = 1.5, which is z_max of the
    bin (1.1, 1.5) and z_min of (1.5, 2.0); snapshot 10 sits at z_min = 1.0 and snapshot 12 at
    z_max = 2.0.

    The eight written snapshots fall in the fifteen bins: 37 (z = 1.386) alone in (1.1, 1.5), 32
    in (2.0, 2.5), 27 in (3.0, 3.5), 23 in (3.5, 4.5), 20 in (4.5, 5.5), 18 in (5.5, 6.5), 16 in
    (6.5, 7.5) and snapshot 63 (z = 0) in none.
    """
    helper = selection_module().snapshots_in_z_range
    snapshots = snapshots_with_redshifts({10: 1.0, 11: 1.5, 12: 2.0})
    assert helper(snapshots, 1.1, 1.5) == [11], "z_max not inclusive"
    assert helper(snapshots, 1.5, 2.0) == [11, 12], "z_min or z_max not inclusive"
    assert helper(snapshots, 1.0, 1.4) == [10], "z_min not inclusive"

    snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, ENGINE_ORDER)
    expected = {3: [37], 5: [32], 7: [27], 8: [23], 9: [20], 10: [18], 11: [16]}
    for index, (z_min, z_max) in enumerate(SAGE26_Z_BINS):
        got = helper(snapshots, z_min, z_max)
        assert got == expected.get(index, []), f"bin {index} ({z_min}, {z_max}): {got}"


def test_selection__empty_bin_returns_no_snapshot():
    """A bin no written snapshot falls in returns nothing (SAGE26 then skips the bin); no written
    snapshot at all gives no nearest snapshot and no snapshot."""
    module = selection_module()
    snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, ENGINE_ORDER)
    assert module.snapshots_in_z_range(snapshots, 0.2, 0.5) == [], "bin (0.2, 0.5)"
    assert module.snapshots_in_z_range(snapshots, 10.0, 12.0) == [], "bin (10.0, 12.0)"
    assert module.nearest_snapshot({}, 2.0) is None, "nearest of no snapshot"
    assert module.all_snapshots({}) == [], "all of no snapshot"


def test_selection__nearest_snapshot_for_the_targets_of_the_plan():
    """SAGE26's target redshifts (0, 2, 6) and (0.1, 1, 2, 4, 6, 8) on the written snapshots:
    0 -> 63, 2 -> 32, 6 -> 18; 0.1 -> 63, 1.0 -> 37, 2.0 -> 32, 4.0 -> 23, 6.0 -> 18, 8.0 -> 16."""
    helper = selection_module().nearest_snapshot
    snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, ENGINE_ORDER)
    expected = {0.0: 63, 2.0: 32, 6.0: 18, 0.1: 63, 1.0: 37, 4.0: 23, 8.0: 16}
    for target, snap in expected.items():
        assert helper(snapshots, target) == snap, f"target {target}: {helper(snapshots, target)}"


def test_selection__nearest_tie_returns_the_lower_snapshot_number():
    """As in SAGE26: `np.argmin` returns the first minimum in snapshot-number order. Snapshots 10
    (z = 1.0) and 20 (z = 3.0) are both 1.0 from the target 2.0; the lower number wins whichever
    order they are supplied in.

    The same eight snapshots in the engine's increasing-redshift order, a decreasing-redshift and
    a fixed shuffled order give identical results from all three helpers: snapshot numbers in
    increasing order.
    """
    module = selection_module()
    for order in ([10, 20], [20, 10]):
        snapshots = snapshots_with_redshifts({10: 1.0, 20: 3.0}, order)
        got = module.nearest_snapshot(snapshots, 2.0)
        assert got == 10, f"tie in order {order}: {got}"
    for order in (ENGINE_ORDER, DECREASING_ORDER, SHUFFLED_ORDER):
        snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, order)
        assert module.snapshots_in_z_range(snapshots, 2.0, 3.5) == [27, 32], f"range, {order}"
        assert module.nearest_snapshot(snapshots, 4.0) == 23, f"nearest, {order}"
        assert module.all_snapshots(snapshots) == [16, 18, 20, 23, 27, 32, 37, 63], f"all, {order}"


# --------------------------------------------------------------------------------------
# Figure-level cases on small hand-built catalogues
# --------------------------------------------------------------------------------------


def test_unit__arange_constants_of_the_new_bins():
    """The edges SAGE26 builds, written from its np.arange calls: (0.0, 2.2, 0.05),
    (8.0 / 7.2 to 12.5), (10.0 to 15.0), (6.0 to 11.5), (8.0 / 7.2 to 13.5) and (7.2 to 12.5).
    np.arange stops short of its stop when the
    width does not divide the range exactly, so the counts and last edges are the literals NumPy
    returns for those arguments (read once, written here)."""
    figures = {name: HARNESS.figure_module(name) for name in HARNESS.evolution_names}
    cases = [
        (figures["rcool_to_rvir_above_mshock"].RATIO_BINS, 44, 0.0, 2.15),
        (figures["sfr_evolution"].mass_bins_for(0), 53, 7.2, 12.4),
        (figures["sfr_evolution"].mass_bins_for(10), 53, 7.2, 12.4),
        (figures["sfr_evolution"].mass_bins_for(11), 45, 8.0, 12.4),
        (figures["sfr_evolution"].mass_bins_for(14), 45, 8.0, 12.4),
        (figures["stellar_mass_halo_mass_relation"].HALO_MASS_BINS, 50, 10.0, 14.9),
        (figures["black_hole_mass_function"].MASS_BINS, 55, 6.0, 11.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(0), 63, 7.2, 13.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(10), 63, 7.2, 13.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(11), 55, 8.0, 13.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(14), 55, 8.0, 13.4),
        (figures["metallicity_evolution"].mass_bins(), 53, 7.2, 12.4),
        (figures["h2_fraction_evolution"].mass_bins(), 53, 7.2, 12.4),
    ]
    for index, (edges, count, first, last) in enumerate(cases):
        assert len(edges) == count, f"case {index}: {len(edges)} edges, expected {count}"
        close([edges[0], edges[-1]], [first, last], rel=1e-9)
    close(np.diff(figures["rcool_to_rvir_above_mshock"].RATIO_BINS)[:3], [0.05] * 3, rel=1e-9)


# ---- rcool_to_rvir_above_mshock ----
def test_unit__rcool_helpers():
    """As in SAGE26: `(Mvir > Mshock_Msun) & (rcool_ratio >= 0.0)`, `np.mean(r > 1.0)` and
    `np.mean(r >= 1.0)`, and `np.clip` to the first and last bin edge (0.0 and 2.15)."""
    module = HARNESS.figure_module("rcool_to_rvir_above_mshock")
    assert module.MSHOCK_MSUN == 6.0e11, f"Mshock {module.MSHOCK_MSUN}"
    assert module.TARGET_REDSHIFTS == [0.0, 2.0, 6.0], f"targets {module.TARGET_REDSHIFTS}"
    mvir = np.array([5.0e11, 6.0e11, 7.0e11, 8.0e11])
    ratio = np.array([0.5, 0.5, -0.1, 0.0])
    got = module.select_above_mshock(mvir, ratio).tolist()
    assert got == [False, False, False, True], f"selection {got}"
    ratios = np.array([0.5, 1.0, 1.5, 2.0])
    assert module.strict_fraction(ratios) == 0.5, "strict fraction"
    assert module.at_least_one_fraction(ratios) == 0.75, "fraction at or above 1"
    close(module.clip_to_bins(np.array([-1.0, 1.0, 3.0])), [0.0, 1.0, 2.15])


def test_legend__rcool_to_rvir_above_mshock():
    """As in SAGE26: `z = {actual_z:.1f}  (N={N}, f$_{{>1}}$={f_strict:.2f})` for the snapshots
    nearest z = 0, 2 and 6 (snapshots 63, 32, 18). Mvir is in 1e10 Msun/h, so Msun = Mvir x 1e10
    / 0.73. Snapshot 63: three haloes above 6e11 Msun with ratios 0.5, 1.5, 1.5 (one is -1,
    undefined, one is below Mshock), N = 3, 2 of 3 above 1. Snapshot 32: four above, one of them
    above 1, 0.25. Snapshot 18: three above (43.9 is 6.014e11 Msun), the 43.7 halo (5.986e11
    Msun) is below Mshock, none above 1, 0.00."""
    spec = {
        63: (
            0.0,
            {"Mvir": f32([100, 100, 100, 100, 10]), "RcoolToRvir": f32([0.5, 1.5, 1.5, -1, 5])},
        ),
        32: (
            RUN_REDSHIFTS[32],
            {"Mvir": f32([100, 100, 100, 100]), "RcoolToRvir": f32([2.0, 0.5, 0.5, 0.5])},
        ),
        18: (
            RUN_REDSHIFTS[18],
            {"Mvir": f32([100, 100, 43.9, 43.7]), "RcoolToRvir": f32([1.0, 1.0, 0.5, 3.0])},
        ),
    }
    run = HARNESS.run_hand_built("rcool_to_rvir_above_mshock", spec, "rcool-legend")
    texts = sorted(t.get_text() for t in data_axes(run.fig)[0].get_legend().get_texts())
    expected = [
        "z = 0.0  (N=3, f$_{>1}$=0.67)",
        "z = 2.1  (N=4, f$_{>1}$=0.25)",
        "z = 6.2  (N=3, f$_{>1}$=0.00)",
    ]
    assert texts == sorted(expected), f"legend {texts}, SAGE26 gives {sorted(expected)}"


def legend_texts(name):
    """Return the sorted legend texts of a rendered figure (every axes together)."""
    fig = HARNESS.render(name).fig
    assert fig is not None, f"{name}: no figure was captured"
    return sorted(
        t.get_text() for ax in fig.axes if ax.get_legend() for t in ax.get_legend().get_texts()
    )


# Legend text that names data: the redshift bins the eight snapshots populate (bins 3, 5, 7, 8, 9,
# 10 and 11: snapshots 37, 32, 27, 23, 20, 18 and 16, written from SAGE26_Z_BINS and
# RUN_REDSHIFTS) for the binned figures, and the redshift of each snapshot drawn for the black-hole
# mass function (`z = {actual_z:.1f} (SAGE)`: 63 -> 0.0, 37 -> 1.4, 32 -> 2.1, 23 -> 4.2, 18 -> 6.2,
# 16 -> 7.3) with `z = {target_z:.1f} (Obs)` for the six targets
POPULATED_BIN_LABELS = [
    "1.1 < z < 1.5",
    "2.0 < z < 2.5",
    "3.0 < z < 3.5",
    "3.5 < z < 4.5",
    "4.5 < z < 5.5",
    "5.5 < z < 6.5",
    "6.5 < z < 7.5",
]
DATA_LEGENDS = [
    ("sfr_evolution", POPULATED_BIN_LABELS),
    (
        "black_hole_mass_function",
        [f"z = {z} (SAGE)" for z in ("0.0", "1.4", "2.1", "4.2", "6.2", "7.3")]
        + [f"z = {z} (Obs)" for z in ("0.1", "1.0", "2.0", "4.0", "6.0", "8.0")],
    ),
]


def test_legend__entries_that_name_the_redshifts_drawn():
    """A binned evolution figure labels the redshift bins the written snapshots populate, and the
    black-hole mass function the redshift of each snapshot drawn and of each observation."""
    for name, expected in DATA_LEGENDS:
        got = legend_texts(name)
        assert got == sorted(expected), f"{name} legend {got}, SAGE26 {sorted(expected)}"


# ---- sfr_evolution, quenched_fraction_evolution, metallicity_evolution, h2_fraction_evolution:
# the helpers they share the form of ----
def test_unit__sfr_evolution_helpers():
    """As in SAGE26 (`(M > 0.0) & (sfr_total > 0.0)`, and `(M > 0.0) & (metals > 0.0)` for
    metallicity_evolution), and the binning helpers the history figures share (_binning.py).

    Masses 8.05, 8.07 and 8.35 over the bins 8.0, 8.1, 8.2, 8.3, 8.4 hold the values 1, 3 and 5:
    the first bin has mean 2 and standard deviation 1 (`np.std` of 1 and 3), the last mean 5 and
    standard deviation 0, and the empty middle bins are NaN. The stack [[1, nan], [3, nan]] has
    the mean over snapshots 2 in the first bin (`np.nanmean`), its error `np.nanstd([1, 3]) /
    np.sqrt(2)` = 1 / 1.41421356, and the empty second bin is not valid. Values 1, 3 in the bin
    1.0-1.1, 2, 4, 9 in 1.1-1.2 and one beyond the last edge have the medians 2, 4 and none and
    the population standard deviations 1, sqrt(26 / 3) and none.
    """
    module = HARNESS.figure_module("sfr_evolution")
    got = module.select_star_forming(np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0]))
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    got = HARNESS.figure_module("metallicity_evolution").select_with_metals(
        np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0])
    )
    assert got.tolist() == [0, 3], f"metals selection {got.tolist()}"

    bins = np.array([8.0, 8.1, 8.2, 8.3, 8.4])
    means, stds = module.binned_mean_and_std(
        np.array([8.05, 8.07, 8.35]), np.array([1.0, 3.0, 5.0]), bins
    )
    assert np.isnan(means).tolist() == [False, True, True, False], f"means {means.tolist()}"
    close(means[[0, 3]], [2.0, 5.0])
    close(stds[[0, 3]], [1.0, 0.0])
    assert np.isnan(stds).tolist() == [False, True, True, False], f"stds {stds.tolist()}"

    mean, err, valid = module.mean_and_error(np.array([[1.0, np.nan], [3.0, np.nan]]))
    close(mean[:1], [2.0])
    close(err[:1], [0.7071067811865476])
    assert valid.tolist() == [True, False], f"valid {valid.tolist()}"

    medians, stds = HARNESS.figure_module("ejection_efficiency_vs_vvir").binned_median_and_std(
        np.array([1.05, 1.05, 1.15, 1.15, 1.15, 1.35]),
        np.array([1.0, 3.0, 2.0, 4.0, 9.0, 7.0]),
        np.array([1.0, 1.1, 1.2, 1.3]),
    )
    assert len(medians) == 3 and len(stds) == 3, f"{len(medians)} medians, {len(stds)} stds"
    assert medians[:2] == [2.0, 4.0], f"medians {medians}"
    assert np.isnan(medians[2]) and np.isnan(stds[2]), "an empty bin has a median"
    close(stds[:2], [1.0, math.sqrt(26.0 / 3.0)])


def test_unit__h2_fraction_evolution_helpers():
    """As in SAGE26: the hydrogen fraction 0.74, `(mstar > 0.0) & (cold > 0.0)` and `np.clip(h2 /
    (cold * 0.74), 0.0, 1.0)`. H2 of 0.37, 0.9 and 0.0 over cold gas 1.0 give 0.5, 1 (0.9 / 0.74
    = 1.216 is clipped) and 0."""
    module = HARNESS.figure_module("h2_fraction_evolution")
    assert module.HYDROGEN_MASS_FRAC == 0.74, f"hydrogen fraction {module.HYDROGEN_MASS_FRAC}"
    got = module.select_with_cold_gas(
        np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0])
    )
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    close(
        module.molecular_fraction(np.array([0.37, 0.9, 0.0]), np.array([1.0, 1.0, 1.0])),
        [0.5, 1.0, 0.0],
    )


def test_unit__stellar_mass_halo_mass_relation_helpers():
    """As in SAGE26: `(M > 0.0) & (Mhalo > 0.0)` and the mean of the linear stellar mass in each
    log10 halo-mass bin. Halo masses 10.05, 10.07 and 10.35 over the bins 10.0, 10.1,
    10.2, 10.3, 10.4 hold stellar masses 10, 30 and 50: the first bin has mean 20, the last 50,
    the middle bins NaN."""
    module = HARNESS.figure_module("stellar_mass_halo_mass_relation")
    got = module.select_with_halo(np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0]))
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    bins = np.array([10.0, 10.1, 10.2, 10.3, 10.4])
    means = module.mean_stellar_mass_in_halo_bins(
        np.array([10.0, 30.0, 50.0]), np.array([10.05, 10.07, 10.35]), bins
    )
    assert np.isnan(means).tolist() == [False, True, True, False], f"means {means.tolist()}"
    close(means[[0, 3]], [20.0, 50.0])


# ---- quenched_fraction_evolution ----
def test_unit__quenched_fraction_helpers():
    """As in SAGE26: sSFRcut -11.0, `sfr / M < 10.0**sSFRcut`, and the fraction of the selected
    galaxies that are quenched in each mass bin. A galaxy of 1e10 Msun
    forming 0.09 Msun/yr has 9e-12 per year and is quenched, one forming 0.11 has 1.1e-11 and is
    not. In the bins 8.0 to 8.1 and 8.1 to 8.2, masses 8.05, 8.06, 8.07 and 8.15, quenched True,
    False, True, True and selected True, True, False, True: the first bin holds two selected
    galaxies of which one is quenched (0.5), the second one selected quenched galaxy (1.0), and
    the third bin none (NaN)."""
    module = HARNESS.figure_module("quenched_fraction_evolution")
    assert module.SSFR_CUT == -11.0, f"sSFRcut {module.SSFR_CUT}"
    got = module.is_quenched(np.array([0.09, 0.11]), np.array([1.0e10, 1.0e10]))
    assert got.tolist() == [True, False], f"quenched {got.tolist()}"
    fractions = module.quenched_fraction_in_bins(
        np.array([8.05, 8.06, 8.07, 8.15]),
        np.array([True, False, True, True]),
        np.array([True, True, False, True]),
        np.array([8.0, 8.1, 8.2, 8.3]),
    )
    close(fractions[:2], [0.5, 1.0])
    assert np.isnan(fractions[2]), f"empty bin {fractions[2]}"


def test_figure__quenched_fraction_orphans_are_in_neither_curve():
    """As in SAGE26 (rate = SfrDisk + SfrBulge, over the stellar mass in Msun): centrals are
    `gtype == 0` and satellites `gtype == 1`, so an orphan (Type 2) is in neither. One snapshot
    at z = 1.39 (the bin 1.1 to 1.5) with three galaxies of 1.37e10 Msun (StellarMass 1.0 x 1e10
    / 0.73) in one mass bin: a central and an orphan forming 0.05 + 0.05 = 0.1 Msun/yr (7.3e-12
    per year, quenched) and a satellite forming 0.09 + 0.09 = 0.18 (1.3e-11, not quenched). The
    central curve is 1.0 and the satellite curve 0.0; counting the orphan as a satellite would
    give 0.5, and taking only the disk rate would quench the satellite (6.6e-12) and give 1.0."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {
                "StellarMass": f32([1.0, 1.0, 1.0]),
                "SfrDisk": f32([0.05, 0.09, 0.05]),
                "SfrBulge": f32([0.05, 0.09, 0.05]),
                "Type": np.array([0, 1, 2], dtype=np.int32),
            },
        )
    }
    run = HARNESS.run_hand_built("quenched_fraction_evolution", spec, "quenched-orphans")
    lines = model_lines(run.fig)
    assert len(lines) == 2, f"{len(lines)} model lines"
    central, satellite = lines
    assert central.get_linestyle() == "-" and satellite.get_linestyle() == ":", "line styles"
    assert central.get_ydata().tolist() == [1.0], f"central {central.get_ydata().tolist()}"
    assert satellite.get_ydata().tolist() == [0.0], f"satellite {satellite.get_ydata().tolist()}"
    close(central.get_xdata(), [10.15])


# ---- concentration_evolution ----
def test_unit__concentration_statistics():
    """As in SAGE26 (the four mass bins): `(conc > 0) & (mvir >= mlo) & (mvir < mhi)`; with at
    least 5 galaxies the median and the 16th and 84th percentiles, else NaN. Concentrations 1 to
    5 give the median 3 and, by linear interpolation at 0.64 and 3.36 of 4 intervals, 1.64 and
    4.36; four give NaN; a concentration of 0 and a halo exactly at the upper edge are not
    counted, one exactly at the lower edge is (the last case below: five counted with it, four
    without)."""
    module = HARNESS.figure_module("concentration_evolution")
    assert [bounds[:2] for bounds in module.MASS_BINS] == [
        (1e10, 1e11),
        (1e11, 1e12),
        (1e12, 1e13),
        (1e13, 1e15),
    ], f"mass bins {module.MASS_BINS}"
    assert module.MIN_GALAXIES == 5, f"minimum {module.MIN_GALAXIES}"
    median, low, high = module.concentration_statistics(
        np.array([1.0, 2.0, 3.0, 4.0, 5.0]), np.full(5, 5.0e10), 1e10, 1e11
    )
    close([median, low, high], [3.0, 1.64, 4.36])
    four = module.concentration_statistics(
        np.array([1.0, 2.0, 3.0, 4.0]), np.full(4, 5.0e10), 1e10, 1e11
    )
    assert np.isnan(four).all(), f"four galaxies {four}"
    # Seven rows of which five count: the halo exactly at the lower edge 1e10 (concentration 1) and
    # four ordinary ones (2 to 5) make five, so with the lower edge counted the median is 3 and the
    # percentiles 1.64 and 4.36 (as for 1 to 5 above), and without it (`>` instead of `>=`) there
    # are four and the result is NaN; a halo exactly at the upper edge 1e11 (concentration 100)
    # and a zero concentration (in an ordinary halo) are not counted: counting either would give
    # the median 3.5 (1, 2, 3, 4, 5, 100) or 2.5 (0, 1, 2, 3, 4, 5).
    edge = module.concentration_statistics(
        np.array([1.0, 2.0, 3.0, 4.0, 5.0, 100.0, 0.0]),
        np.array([1.0e10, 2.0e10, 3.0e10, 4.0e10, 5.0e10, 1.0e11, 6.0e10]),
        1e10,
        1e11,
    )
    close(edge, [3.0, 1.64, 4.36])


# ---- star_formation_efficiency ----
def test_unit__star_formation_efficiency_helpers():
    """As in SAGE26: f_b = 0.17, `(gtype == 0) & (mstar > 0) & (mvir > 0)`, at least 5 galaxies,
    `sfe = mstar / (mvir * f_baryon)`, and the median and 16th and 84th percentiles. Stellar
    masses 17, 34, 51, 68, 85 over halo masses of 100 give 1, 2, 3, 4, 5: median 3, percentiles
    1.64 and 4.36 (linear interpolation at 0.64 and 3.36 of 4 intervals); four galaxies give
    none."""
    module = HARNESS.figure_module("star_formation_efficiency")
    assert module.F_BARYON == 0.17, f"f_b {module.F_BARYON}"
    assert module.MIN_CENTRALS == 5, f"minimum {module.MIN_CENTRALS}"
    got = module.select_centrals(
        np.array([0, 1, 0, 0, 2]),
        np.array([1.0, 1.0, 0.0, 1.0, 1.0]),
        np.array([1.0, 1.0, 1.0, 0.0, 1.0]),
    )
    assert got.tolist() == [0], f"selection {got.tolist()}"
    stellar = np.array([17.0, 34.0, 51.0, 68.0, 85.0])
    close(module.efficiency_statistics(stellar, np.full(5, 100.0)), [3.0, 1.64, 4.36])
    assert module.efficiency_statistics(stellar[:4], np.full(4, 100.0)) is None, "four galaxies"


def test_unit__age_of_the_universe():
    """As in SAGE26: (9.778 / h) x the integral of dz / ((1 + z) E(z)) from z to 1000 (trapezoid
    rule on 10000 points). For a flat universe with Omega_lambda = 1 (E = 1) the integral is
    ln(1001 / (1 + z)), so with h = 0.5 the age at z = 0 is 19.556 x ln(1001) = 135.1076 and at
    z = 1 is 19.556 x ln(1001 / 2) = 121.5524. For Omega_m = 1 (E = (1 + z)^1.5) the integral is
    2/3 x ((1 + z)^-1.5 - 1001^-1.5), so with h = 1 the age at z = 0 is 6.5187 x (1 - 1001^-1.5) =
    6.51846 and at z = 3 is 6.5187 x (4^-1.5 - 1001^-1.5) = 0.814628.

    The trapezoid error is 1e-4 to 2e-4 for three of these, compared to 5e-4, so a 0.1 percent
    error in 9.778 fails. At z = 0 in the Einstein-de Sitter case the integrand is steepest and
    the quadrature is 0.31 percent high, so that row is compared to 4e-3.
    """
    helper = HARNESS.figure_module("star_formation_efficiency").z_to_age_gyr
    rows = [
        ((0.0, 0.0, 1.0, 0.5), 135.1076, 5e-4),
        ((1.0, 0.0, 1.0, 0.5), 121.5524, 5e-4),
        ((0.0, 1.0, 0.0, 1.0), 6.51846, 4e-3),
        ((3.0, 1.0, 0.0, 1.0), 0.814628, 5e-4),
    ]
    for args, expected, rel in rows:
        close(helper(*args), expected, rel=rel)


# ---- central_satellite_counts and quenched_central_satellite_counts ----
def test_unit__quenched_counts_helper():
    """As in SAGE26 (sSFRcut -11.0): `sfr / M < 10.0**sSFRcut`, with `(types == 0) &
    quenched` and `(types >= 1) & quenched`. Five galaxies of 1e10 Msun with Types 0, 0, 1, 2, 1
    and rates 0.09, 0.11, 0.05, 0.01, 0.2: the quenched are the first, third and fourth, so one
    central and two satellites; a galaxy with no stellar mass is never quenched (its rate is
    infinite or NaN)."""
    module = HARNESS.figure_module("quenched_central_satellite_counts")
    assert module.SSFR_CUT == -11.0, f"sSFRcut {module.SSFR_CUT}"
    stellar = np.array([1.0e10] * 5 + [0.0, 0.0])
    sfr = np.array([0.09, 0.11, 0.05, 0.01, 0.2, 1.0, 0.0])
    types = np.array([0, 0, 1, 2, 1, 0, 1])
    got = module.count_quenched(types, sfr, stellar)
    assert [int(v) for v in got] == [1, 2], f"counts {got}"


COUNT_TICKS = [
    "0.2-0.5",
    "0.5-0.8",
    "0.8-1.1",
    "1.1-1.5",
    "1.5-2.0",
    "2.0-2.5",
    "2.5-3.0",
    "3.0-3.5",
    "3.5-4.5",
    "4.5-5.5",
    "5.5-6.5",
    "6.5-7.5",
    "7.5-8.5",
    "8.5-10.0",
    "10.0-12.0",
]


def test_figure__counts_figures_label_the_redshift_bins_and_draw_no_legend():
    """The two counts figures label SAGE26's fifteen redshift bins at 45 degrees on a window of
    -0.5 to 14.5, draw no legend, and set the top limit 1.155 x the tallest bar (matplotlib's 5
    percent margin above 1.1 x the autoscaled top)."""
    for name in ("central_satellite_counts", "quenched_central_satellite_counts"):
        fig = HARNESS.render(name).fig
        ax = data_axes(fig)[0]
        ticks = [t.get_text() for t in ax.get_xticklabels()]
        assert ticks == COUNT_TICKS, f"{name} ticks {ticks}"
        rotations = {t.get_rotation() for t in ax.get_xticklabels()}
        assert rotations == {45.0}, f"{name} rotation {rotations}"
        assert ax.get_legend() is None, f"{name} draws a legend, SAGE26 has none"
        close(ax.get_xlim(), [-0.5, 14.5])
        heights = [artist.get_height() for artist, _ in tagged_model_artists(fig)]
        close([ax.get_ylim()[0], ax.get_ylim()[1] / max(heights)], [0.0, 1.155], rel=1e-9)


def test_figure__central_satellite_counts_bars_are_the_mean_counts():
    """As in SAGE26: a bar of the mean central count at i - 0.15 and one of the mean satellite
    count at i + 0.15, both of width 0.3. Snapshot 37 (z = 1.39, bin 3) holds Types 0, 0, 1, 2,
    1: 2 centrals and 3 satellites (the orphan counts as one)."""
    spec = {37: (RUN_REDSHIFTS[37], {"Type": np.array([0, 0, 1, 2, 1], dtype=np.int32)})}
    run = HARNESS.run_hand_built("central_satellite_counts", spec, "counts-bars")
    bars = sorted(
        (a.get_x() + a.get_width() / 2, a.get_height(), a.get_width())
        for a, _ in tagged_model_artists(run.fig)
    )
    close([b[0] for b in bars], [2.85, 3.15])
    close([b[1] for b in bars], [2.0, 3.0])
    close([b[2] for b in bars], [0.3, 0.3])


# ---- central_satellite_fraction ----
def test_unit__central_satellite_fractions_helper():
    """As in SAGE26: `n_cen / n_total` and `n_sat / n_total` with `types >= 1` for satellites.
    Types 0, 0, 1, 2 give 0.5 and 0.5; Types 0, 1, 1, 1 give 0.25 and 0.75."""
    helper = HARNESS.figure_module("central_satellite_fraction").central_and_satellite_fractions
    close(helper(np.array([0, 0, 1, 2])), [0.5, 0.5])
    close(helper(np.array([0, 1, 1, 1])), [0.25, 0.75])


def test_figure__central_satellite_fraction_window_ends_at_the_largest_redshift():
    """As in SAGE26: x from 0 to the largest z drawn, y from 0 to 1.05. Two snapshots (z = 1.39
    and 2.07) with Types 0, 1 and 0, 0, 0, 1: the lines are drawn at those redshifts, centrals
    0.5 and 0.75, satellites 0.5 and 0.25."""
    spec = {
        37: (RUN_REDSHIFTS[37], {"Type": np.array([0, 1], dtype=np.int32)}),
        32: (RUN_REDSHIFTS[32], {"Type": np.array([0, 0, 0, 1], dtype=np.int32)}),
    }
    run = HARNESS.run_hand_built("central_satellite_fraction", spec, "fraction-window")
    ax = data_axes(run.fig)[0]
    close(ax.get_xlim(), [0.0, 2.0700273232431767])
    close(ax.get_ylim(), [0.0, 1.05])
    centrals, satellites = model_lines(run.fig)
    close(centrals.get_ydata(), [0.5, 0.75])
    close(satellites.get_ydata(), [0.5, 0.25])
    close(centrals.get_xdata(), [1.3857181369449925, 2.0700273232431767])


# ---- central_satellite_fraction_massive_quiescent ----
def test_unit__ssfr_threshold():
    """As in SAGE26: `0.2 * E_z / ((9.778 / h) * 1e9)` with E_z = sqrt(Om (1 + z)^3 + OL). For Om
    = 1, OL = 0, h = 1: at z = 0, E = 1 and 0.2 / 9.778e9; at z = 3, E = 8 and 1.6 / 9.778e9. For
    Om = 0, OL = 1, h = 0.5: E = 1 and 0.2 / (19.556e9)."""
    helper = HARNESS.figure_module("central_satellite_fraction_massive_quiescent").ssfr_threshold
    close([helper(0.0, 1.0, 0.0, 1.0), helper(3.0, 1.0, 0.0, 1.0)], [0.2 / 9.778e9, 1.6 / 9.778e9])
    close([helper(2.0, 0.0, 1.0, 0.5)], [0.2 / 19.556e9])


def test_unit__massive_quiescent_counts():
    """Twenty galaxies of stellar mass 1 to 20: the 90th percentile is 18.1 (interpolation at
    index 17.1 between 18 and 19), so the most massive are those of mass 19 and 20. With a
    threshold of 0.5 per unit mass and rates 5 and 20 for them (5 / 19 = 0.263 is below the
    threshold, 20 / 20 = 1.0 is not) there is one quiescent galaxy, the mass-19 one, here an
    orphan (a satellite): counts (1, 0, 1). A snapshot with 9 galaxies of positive mass is
    skipped (fewer than 10), as is one with no quiescent galaxy among the most massive; a galaxy
    with no mass is left out of the percentile."""
    module = HARNESS.figure_module("central_satellite_fraction_massive_quiescent")
    assert module.MIN_GALAXIES == 10, f"minimum {module.MIN_GALAXIES}"
    assert module.MASS_PERCENTILE == 90, f"percentile {module.MASS_PERCENTILE}"
    mass = np.arange(1.0, 21.0)
    sfr = np.full(20, 100.0)
    sfr[18] = 5.0  # mass 19: 5 / 19 = 0.263 < 0.5, quiescent
    sfr[19] = 20.0  # mass 20: 1.0 > 0.5, forming
    types = np.zeros(20, dtype=int)
    types[18] = 2  # the quiescent galaxy is an orphan, a satellite
    got = module.massive_quiescent_counts(mass, sfr, types, 0.5)
    assert [int(v) for v in got] == [1, 0, 1], f"counts {got}"
    # Quiescent galaxies below the top 10% do not count: raise the rates of the top two
    sfr_all_forming = np.full(20, 0.1)
    sfr_all_forming[18:] = 100.0
    assert module.massive_quiescent_counts(mass, sfr_all_forming, types, 0.5) is None, "none"
    # Nine galaxies of positive mass, plus eleven with no mass
    sparse_mass = np.concatenate([np.arange(1.0, 10.0), np.zeros(11)])
    assert module.massive_quiescent_counts(sparse_mass, sfr, types, 0.5) is None, "nine galaxies"
    # Ten of positive mass are enough: the 90th percentile of 1..10 is 9.1, so only mass 10 is
    # most massive; it is a central with a low rate
    ten_mass = np.concatenate([np.arange(1.0, 11.0), np.zeros(10)])
    ten_sfr = np.concatenate([np.full(10, 0.1), np.zeros(10)])
    ten_types = np.zeros(20, dtype=int)
    got = module.massive_quiescent_counts(ten_mass, ten_sfr, ten_types, 0.5)
    assert [int(v) for v in got] == [1, 1, 0], f"ten galaxies {got}"


def test_figure__cosmology_missing_from_the_run_is_a_skip_with_a_message():
    """Omega, OmegaLambda and Hubble_h come from the engine's run parameters; without them the
    two figures that need the cosmology return a message, not an error."""
    for name in ("star_formation_efficiency", "central_satellite_fraction_massive_quiescent"):
        snapshots = build_evolution_input(HARNESS.evolution_fields(name))
        path, skip = HARNESS.figures.PLOT_FUNCS[name](
            snapshots=snapshots,
            params={},
            output_dir=HARNESS.output_dir(name + "-no-cosmology"),
            output_format=".png",
            verbose=False,
        )
        plt.close("all")
        assert path is None and skip, f"{name}: path {path}, skip {skip}"


# ---- ffb_fraction_cut_down ----
def test_unit__ffb_fraction_helper():
    """As in SAGE26 (MIN_PARTICLES 20): rows with `Len >= 20`, then `np.sum(ffb == 1) / len(ffb)`,
    NaN with no such row. Rows with Len 19, 20, 25, 100 and
    FFBRegime 1, 1, 0, 1: the Len 19 row is left out, so 1 of 3 (the Len 20 and Len 100 rows are
    FFB). With every Len below 20 the fraction is NaN, not 0."""
    helper = HARNESS.figure_module("ffb_fraction_cut_down").ffb_fraction
    assert HARNESS.figure_module("ffb_fraction_cut_down").MIN_PARTICLES == 20, "MIN_PARTICLES"
    ffb = np.array([1, 1, 0, 1])
    length = np.array([19, 20, 25, 100])
    close([helper(ffb, length)], [2.0 / 3.0])
    only_unresolved = helper(np.array([1, 1]), np.array([19, 1]))
    assert np.isnan(only_unresolved), f"all unresolved gave {only_unresolved}, not NaN"
    assert np.isnan(helper(np.array([], dtype=int), np.array([], dtype=int))), "empty snapshot"


def test_unit__ffb_step_edges_for_the_written_snapshots():
    """As in SAGE26: `z <= 15`, then the edges 15.0, the midpoint of each consecutive pair of
    redshifts in increasing snapshot number, 0.0. The eight written snapshots 16, 18, 20, 23, 27,
    32, 37, 63 (z = 7.272, 6.197, 5.289, 4.179, 3.060, 2.070, 1.386, 0) give 15.0, the seven
    midpoints 6.7345, 5.7428, 4.7341, 3.6199, 2.5652, 1.7279, 0.6929 and 0.0 (midpoints of the
    redshifts 1/a - 1 of the a_list scale factors), whatever order the snapshots are supplied in;
    a snapshot above z = 15 is not drawn."""
    helper = HARNESS.figure_module("ffb_fraction_cut_down").step_edges
    z = RUN_REDSHIFTS
    expected = [
        15.0,
        (z[16] + z[18]) / 2.0,
        (z[18] + z[20]) / 2.0,
        (z[20] + z[23]) / 2.0,
        (z[23] + z[27]) / 2.0,
        (z[27] + z[32]) / 2.0,
        (z[32] + z[37]) / 2.0,
        (z[37] + z[63]) / 2.0,
        0.0,
    ]
    close(expected[1:8], [6.7345, 5.7428, 4.7341, 3.6199, 2.5652, 1.7279, 0.6929], rel=1e-3)
    for order in (ENGINE_ORDER, DECREASING_ORDER, SHUFFLED_ORDER):
        drawn, edges = helper(snapshots_with_redshifts(RUN_REDSHIFTS, order))
        assert drawn == [16, 18, 20, 23, 27, 32, 37, 63], f"drawn {drawn} in order {order}"
        close(edges, expected)
    high = {5: 16.0, 6: 15.0, 7: 3.0}
    drawn, edges = helper(snapshots_with_redshifts(high))
    assert drawn == [6, 7], f"z above 15 drawn: {drawn}"
    close(edges, [15.0, 9.0, 0.0])


def test_figure__ffb_fraction_step_values_and_axes():
    """Snapshots 63 and 16 with Len 30, 30, 30, 10 and FFBRegime 1, 0, 0, 1 (the unresolved row
    is left out: 1 of 3 = 0.333) and Len 30, 30 and FFBRegime 0, 0 (zero, which the log axis does
    not show). The step is drawn in log10(1 + z) from 0 to log10(16) on a log y axis, with the
    edges at 15, half the redshift of snapshot 16, and 0."""
    spec = {
        63: (
            0.0,
            {
                "FFBRegime": np.array([1, 0, 0, 1], dtype=np.int32),
                "Len": np.array([30, 30, 30, 10], dtype=np.int32),
            },
        ),
        16: (
            RUN_REDSHIFTS[16],
            {
                "FFBRegime": np.array([0, 0], dtype=np.int32),
                "Len": np.array([30, 30], dtype=np.int32),
            },
        ),
    }
    run = HARNESS.run_hand_built("ffb_fraction_cut_down", spec, "ffb-step")
    steps = [artist for artist, _ in tagged_model_artists(run.fig)]
    assert len(steps) == 1, f"{len(steps)} model artists"
    values, edges = steps[0].get_data().values, steps[0].get_data().edges
    # In increasing snapshot number: snapshot 16 (0), then 63 (1/3)
    close(values, [0.0, 1.0 / 3.0])
    close(edges, np.log10(1 + np.array([15.0, RUN_REDSHIFTS[16] / 2.0, 0.0])))
    main, _ = data_axes(run.fig)
    close(main.get_xlim(), [0.0, math.log10(16.0)])
    assert main.get_yscale() == "log", "y scale"


def test_window__star_formation_efficiency_runs_from_the_age_today_down_to_0_3_gyr():
    """As in SAGE26: log y, x from the age of the universe at z = 0 down to 0.3 Gyr (a reversed
    axis), y from 1e-6 to 1.0. For Omega_m = 0.25, Omega_lambda = 0.75, h = 0.73 the age today is
    (9.778 / 0.73) x 2 / (3 sqrt(0.75)) x asinh(sqrt(0.75 / 0.25)) = 13.394 x 1.0139 = 13.58 Gyr
    (the closed form of flat LCDM; the integral to z = 1000 is 0.003 Gyr short)."""
    ax = data_axes(HARNESS.render("star_formation_efficiency").fig)[0]
    xlim = ax.get_xlim()
    close([xlim[0]], [13.58], rel=2e-3)
    close([xlim[1]], [0.3])
    close(ax.get_ylim(), [1e-6, 1.0])
    assert ax.get_yscale() == "log", f"y scale {ax.get_yscale()}"


# ---- metallicity_evolution: the SAGE26 plotting defect mirrored as written ----
def test_figure__metallicity_evolution_draws_the_raw_metal_mass():
    """SAGE26 reads MetalsStellarMass with no x 1e10 / h and draws its log10. A galaxy of
    stellar mass 1.0 (1.37e10 Msun, log10 = 10.137, h = 0.73) with MetalsStellarMass 0.01 is
    drawn at log10(0.01) = -2, not at log10(0.01 x 1e10 / 0.73) = 8.14; the mass-bin centre is
    10.15 (the bin 10.1 to 10.2)."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {"StellarMass": f32([1.0, 1.0]), "MetalsStellarMass": f32([0.01, 0.01])},
        )
    }
    run = HARNESS.run_hand_built("metallicity_evolution", spec, "metallicity-raw")
    (line,) = model_lines(run.fig)
    close(line.get_xdata(), [10.15])
    close(line.get_ydata(), [-2.0], rel=1e-6)


# ---- black_hole_mass_function: helper and observations ----
def test_unit__black_hole_mass_function_helper():
    """As in SAGE26: `np.histogram(np.log10(M), bins=np.arange(6.0, 11.5, 0.1))` over `volume *
    bin_width`, the bin width being `bins[1] - bins[0]` = 0.1, and a bin is drawn when `phi > 0`;
    only galaxies with `M > 0` count. Black holes of 10^6.05 (twice) and 10^6.15 Msun and one of
    no mass, in a volume of 100 Mpc^3: 2 / (100 x 0.1) = 0.2 in the bin centred 6.05 and 1 / 10 =
    0.1 in the one centred 6.15; with no black hole the helper gives nothing."""
    module = HARNESS.figure_module("black_hole_mass_function")
    assert module.TARGET_REDSHIFTS == [0.1, 1.0, 2.0, 4.0, 6.0, 8.0], "targets"
    masses = np.array([10**6.05, 10**6.05, 10**6.15, 0.0])
    centers, phi, valid = module.mass_function(masses, 100.0)
    close(centers[:3], [6.05, 6.15, 6.25])
    close(phi[:3], [0.2, 0.1, 0.0])
    assert valid.tolist()[:3] == [True, True, False], f"valid {valid.tolist()[:3]}"
    assert int(valid.sum()) == 2, f"{int(valid.sum())} bins drawn"
    assert module.mass_function(np.zeros(3), 100.0) is None, "no black hole"


def test_unit__black_hole_mass_function_observations_have_the_shape_of_the_files():
    """SAGE26's fig4_bhmf_z{0.1,1.0,2.0,4.0,6.0,8.0}.txt data files hold 66 rows each of 4
    columns (log10 Mbh, best, 16th, 84th percentile), which the inline arrays reproduce."""
    module = HARNESS.figure_module("black_hole_mass_function")
    assert sorted(module.OBS_TRINITY) == [0.1, 1.0, 2.0, 4.0, 6.0, 8.0], "redshifts"
    for z, obs in module.OBS_TRINITY.items():
        assert obs.shape == (66, 4), f"z = {z}: shape {obs.shape}"
    close(module.OBS_TRINITY[0.1][0], [4.0, 1.868892e-02, 1.607018e-02, 2.439403e-02])


# ---- the conversions and wiring each figure applies to its fields ----
# StellarMass 1.0 is 1.0 x 1e10 / 0.73 = 1.3699e10 Msun, log10 = 10.13668, in the mass bin
# 10.1 to 10.2 (centre 10.15); Mvir 10.0 is 1.3699e11 Msun, log10 = 11.13668 (bin centre 11.15).
LOG_UNIT_MASS = 10.136677  # log10(1e10 / 0.73)


def test_figure__sfr_evolution_converts_mass_and_adds_the_two_rates():
    """As in SAGE26: x 1e10 / h, SfrDisk + SfrBulge and the log10 of both. One galaxy
    of StellarMass 1.0 forming 1.0 + 1.0 Msun/yr at z = 1.39: drawn at the bin centre 10.15, y =
    log10(2) = 0.30103."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {"StellarMass": f32([1.0]), "SfrDisk": f32([1.0]), "SfrBulge": f32([1.0])},
        )
    }
    (line,) = model_lines(HARNESS.run_hand_built("sfr_evolution", spec, "wiring-sfr").fig)
    close(line.get_xdata(), [10.15])
    close(line.get_ydata(), [0.30103], rel=1e-5)


def test_figure__stellar_mass_halo_mass_relation_converts_both_masses():
    """As in SAGE26: both x 1e10 / h, the stellar mass stays linear, the halo mass is log10, and
    the mean stellar mass is log10. One galaxy of StellarMass 1.0 in a halo of
    Mvir 10.0 at z = 1.39: drawn at the halo-mass bin centre 11.15, y = 10.136677."""
    spec = {
        37: (RUN_REDSHIFTS[37], {"StellarMass": f32([1.0]), "Mvir": f32([10.0])}),
    }
    (line,) = model_lines(
        HARNESS.run_hand_built("stellar_mass_halo_mass_relation", spec, "wiring-smhm").fig
    )
    close(line.get_xdata(), [11.15])
    close(line.get_ydata(), [LOG_UNIT_MASS], rel=1e-6)


def test_figure__h2_fraction_evolution_converts_the_three_masses():
    """As in SAGE26: x 1e10 / h and H2 / (cold x 0.74). One galaxy with StellarMass 1.0,
    ColdGas 1.0 and H2gas 0.37 at z = 1.39: drawn at 10.15, with the fraction 0.37 / 0.74 = 0.5."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {"StellarMass": f32([1.0]), "ColdGas": f32([1.0]), "H2gas": f32([0.37])},
        )
    }
    (line,) = model_lines(HARNESS.run_hand_built("h2_fraction_evolution", spec, "wiring-h2").fig)
    close(line.get_xdata(), [10.15])
    close(line.get_ydata(), [0.5], rel=1e-6)


def test_figure__concentration_evolution_converts_the_halo_mass_and_is_drawn_at_the_redshift():
    """As in SAGE26: x 1e10 / h, and the redshift of the snapshot. Five haloes of Mvir 8.0
    at z = 0, with concentrations 1 to 5: 8.0 x 1e10 / 0.73 = 1.096e11 Msun is in the bin 1e11 to
    1e12 (without the division by h it would be 8e10 Msun, in the bin 1e10 to 1e11). One line,
    labelled for the 1e11 bin, at the median 3."""
    spec = {
        63: (
            0.0,
            {"Mvir": f32([8.0] * 5), "Concentration": f32([1, 2, 3, 4, 5])},
        )
    }
    run = HARNESS.run_hand_built("concentration_evolution", spec, "wiring-concentration")
    (line,) = model_lines(run.fig)
    assert line.get_label() == r"$10^{11} - 10^{12}$ $M_\odot$", f"label {line.get_label()}"
    close(line.get_xdata(), [0.0])
    close(line.get_ydata(), [3.0])


def test_figure__black_hole_mass_function_converts_mass_and_volume():
    """As in SAGE26: x 1e10 / h; volume = (BoxSize / h)^3, so the engine's volume in (Mpc/h)^3
    over h^3; counts over volume x bin width. Two black holes of 10^6.55
    Msun (BlackHoleMass 10^6.55 x 0.73 / 1e10 = 2.6e-4) in the only snapshot fall in the bin 6.5
    to 6.6 (centre 6.55); the volume is 62.5^3 / 0.73^3 = 627,566 Mpc^3, so phi = 2 / (627,566 x
    0.1) = 3.187e-5. Every one of the six targets selects that snapshot, which is drawn once, so
    the figure holds one model line."""
    code_mass = 10**6.55 * 0.73 / 1.0e10
    spec = {63: (0.0, {"BlackHoleMass": f32([code_mass, code_mass])})}
    lines = model_lines(HARNESS.run_hand_built("black_hole_mass_function", spec, "wiring-bhmf").fig)
    assert len(lines) == 1, f"{len(lines)} model lines"
    close(lines[0].get_xdata(), [6.55])
    close(lines[0].get_ydata(), [3.187e-5], rel=1e-3)


def test_figure__star_formation_efficiency_converts_masses_and_ages_the_snapshot():
    """As in SAGE26: M_* / (M_vir x f_b), at the age of the universe at the snapshot's redshift.
    Five centrals of StellarMass 1.0 in haloes of Mvir 10.0 at z = 1.39: 1.0 / (10.0 x 0.17) =
    0.58824, at the age of a flat universe with Omega_m = 0.25 and h = 0.73 at z = 1.386: (9.778
    / 0.73) x 2 / (3 sqrt(0.75)) x asinh(sqrt(3) / 2.386^1.5) = 4.68 Gyr."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {
                "StellarMass": f32([1.0] * 5),
                "Mvir": f32([10.0] * 5),
                "Type": np.zeros(5, dtype=np.int32),
            },
        )
    }
    line = model_lines(HARNESS.run_hand_built("star_formation_efficiency", spec, "wiring-sfe").fig)[
        0
    ]
    close(line.get_ydata(), [0.58824], rel=1e-4)
    close(line.get_xdata(), [4.68], rel=1e-2)


def test_figure__quenched_counts_convert_the_mass_and_add_the_two_rates():
    """As in SAGE26: the rate is SfrDisk + SfrBulge. Two centrals of 1.37e10 Msun (StellarMass
    1.0): one forming 0.05 + 0.05 = 0.1 Msun/yr (7.3e-12 per year, quenched) and one forming 0.09
    + 0.09 = 0.18 (1.3e-11, not): one quenched central, no quenched satellite. Taking only the
    disk rate would quench both."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {
                "StellarMass": f32([1.0, 1.0]),
                "SfrDisk": f32([0.05, 0.09]),
                "SfrBulge": f32([0.05, 0.09]),
                "Type": np.array([0, 0], dtype=np.int32),
            },
        )
    }
    run = HARNESS.run_hand_built("quenched_central_satellite_counts", spec, "wiring-qcounts")
    bars = sorted(
        (a.get_x() + a.get_width() / 2, a.get_height()) for a, _ in tagged_model_artists(run.fig)
    )
    close([b[0] for b in bars], [2.85, 3.15])
    close([b[1] for b in bars], [1.0, 0.0])


def test_figure__massive_quiescent_converts_the_mass_and_uses_the_run_cosmology():
    """Ten galaxies of 1.37e10 Msun (StellarMass 1.0, every one among the most massive) forming
    0.09 + 0.09 = 0.18 Msun/yr (1.31e-11 per year; 1.8e-11 if the mass were not divided by h),
    seven central and three satellite, at z = 0 and z = 1.39 with Omega_m = 0.25, Omega_lambda =
    0.75, h = 0.73: the quiescence limit 0.2 E(z) h / 9.778e9 is 1.49e-11 at z = 0 and 3.0e-11 at
    z = 1.39, so all ten are quiescent at both: fractions 0.7 and 0.3 at both redshifts (the
    unconverted mass would leave no quiescent galaxy at z = 0 and so no point there)."""
    galaxies = {
        "StellarMass": f32([1.0] * 10),
        "SfrDisk": f32([0.09] * 10),
        "SfrBulge": f32([0.09] * 10),
        "Type": np.array([0] * 7 + [1] * 3, dtype=np.int32),
    }
    spec = {63: (0.0, galaxies), 37: (RUN_REDSHIFTS[37], galaxies)}
    run = HARNESS.run_hand_built("central_satellite_fraction_massive_quiescent", spec, "wiring-mq")
    centrals, satellites = model_lines(run.fig)
    close(centrals.get_xdata(), [0.0, RUN_REDSHIFTS[37]])
    close(centrals.get_ydata(), [0.7, 0.7])
    close(satellites.get_ydata(), [0.3, 0.3])


CASES = [
    test_reservoir_helper_returns_seven_reservoirs_and_their_exact_sum,
    test_labels__units_and_conventions_that_differ_from_sage26,
    test_labels__tcool_vs_mvir_with_only_regimes_0_and_1_has_no_colour_bar,
    test_run_parameters__rcool_title_and_selection_use_the_runs_mshock_msun,
    test_run_parameters__missing_key_warns_and_uses_the_fallback,
    test_cap__sampled_figures_hold_their_literal_cap_and_sample_to_it,
    test_unit__one_expression_helpers,
    test_unit__bulge_to_total_bins,
    test_unit__log_specific_sfr,
    test_unit__smoothed_histogram,
    test_unit__sigma_levels,
    test_unit__ejection_efficiency,
    test_bins__specific_star_formation_rate_constants,
    test_bins__ejection_efficiency_edges,
    test_bins__disk_radius_edges,
    test_threshold__specific_star_formation_rate_population_minimum_is_10,
    test_threshold__stellar_mass_cuts_and_windows_of_the_scatter_figures,
    test_selection__z_range_includes_both_ends,
    test_selection__empty_bin_returns_no_snapshot,
    test_selection__nearest_snapshot_for_the_targets_of_the_plan,
    test_selection__nearest_tie_returns_the_lower_snapshot_number,
    test_unit__arange_constants_of_the_new_bins,
    test_unit__rcool_helpers,
    test_legend__rcool_to_rvir_above_mshock,
    test_legend__entries_that_name_the_redshifts_drawn,
    test_unit__sfr_evolution_helpers,
    test_unit__h2_fraction_evolution_helpers,
    test_unit__stellar_mass_halo_mass_relation_helpers,
    test_unit__quenched_fraction_helpers,
    test_figure__quenched_fraction_orphans_are_in_neither_curve,
    test_unit__concentration_statistics,
    test_unit__star_formation_efficiency_helpers,
    test_unit__age_of_the_universe,
    test_unit__quenched_counts_helper,
    test_figure__counts_figures_label_the_redshift_bins_and_draw_no_legend,
    test_figure__central_satellite_counts_bars_are_the_mean_counts,
    test_unit__central_satellite_fractions_helper,
    test_figure__central_satellite_fraction_window_ends_at_the_largest_redshift,
    test_unit__ssfr_threshold,
    test_unit__massive_quiescent_counts,
    test_figure__cosmology_missing_from_the_run_is_a_skip_with_a_message,
    test_unit__ffb_fraction_helper,
    test_unit__ffb_step_edges_for_the_written_snapshots,
    test_figure__ffb_fraction_step_values_and_axes,
    test_window__star_formation_efficiency_runs_from_the_age_today_down_to_0_3_gyr,
    test_figure__metallicity_evolution_draws_the_raw_metal_mass,
    test_unit__black_hole_mass_function_helper,
    test_unit__black_hole_mass_function_observations_have_the_shape_of_the_files,
    test_figure__sfr_evolution_converts_mass_and_adds_the_two_rates,
    test_figure__stellar_mass_halo_mass_relation_converts_both_masses,
    test_figure__h2_fraction_evolution_converts_the_three_masses,
    test_figure__concentration_evolution_converts_the_halo_mass_and_is_drawn_at_the_redshift,
    test_figure__black_hole_mass_function_converts_mass_and_volume,
    test_figure__star_formation_efficiency_converts_masses_and_ages_the_snapshot,
    test_figure__quenched_counts_convert_the_mass_and_add_the_two_rates,
    test_figure__massive_quiescent_converts_the_mass_and_uses_the_run_cosmology,
]


def main():
    """Run the shared cases, the derivation cases and this file's cases in a temporary directory."""
    cases = (
        HARNESS.registry_cases() + derivation_cases(HARNESS, DERIVATIONS, INTEGER_FIELDS) + CASES
    )
    return HARNESS.run(cases, "sage26 figures (test_sage26_figures.py)")


if __name__ == "__main__":
    sys.exit(main())
