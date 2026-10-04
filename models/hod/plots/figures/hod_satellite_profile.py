#!/usr/bin/env python

"""
Mimic HOD Satellite Phase-Space Plot

Two-panel check of where hod_populate puts its satellites and how fast they move.

Left: the cumulative distribution of s = r_com / ((1 + z) Rvir_sat) for the created
satellites, where r_com is the minimum-image distance from the satellite to its host in
comoving Mpc/h and Rvir_sat is the virial radius the satellite row carries: hod_populate
writes the host's current virial radius at the draw redshift onto every created row, the
value it placed the satellite with (so s is the physical radius in units of Rvir and lies in
[0, 1]). It is compared with the satellite-weighted average over hosts of each host's
own NFW enclosed-mass fraction at its concentration c:

    F(s) = (1 / N_sat) sum over satellites of m(c s) / m(c),    m(x) = ln(1 + x) - x / (1 + x)

with c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC from the run file and
M the host's Mvir at the draw in Msun/h (the satellite row's infallMvir). Averaging each
satellite's own host profile, rather than using one concentration, keeps the prediction
exact for a population spanning a wide host-mass range.

Right: the histogram of every component of the satellite-minus-host velocity offset divided
by Vvir_sat / sqrt(2) (the host's current Vvir carried by the satellite row), against the
unit Gaussian. The histograms are normalised by the whole per-component sample, so a profile
window (axes.hod_satellite_profile xmin/xmax) that cuts the tails leaves them comparable with
the unconditional Gaussian; ymin/ymax set the velocity panel's y range.

The snapshot redshift is metadata["redshift"], which the engine maps from the package's a_list.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    SAMPLE_LABEL,
    host_lookup,
    setup_legend,
    setup_plot_fonts,
)
from output_utils import (
    check_required_fields,
    get_profile_axes,
    make_bin_edges,
    read_module_parameters,
    save_and_close_figure,
)

PLOT_KEY = "hod_satellite_profile"
PLOT_XLIM = (-4.0, 4.0)  # Velocity panel, offset / (Vvir / sqrt(2))
PLOT_YLIM = (0.0, 0.5)  # Velocity panel, probability density per unit offset
VELOCITY_BINWIDTH = 0.25
RADIUS_GRID_POINTS = 201
PREDICTION_CHUNK = 20000  # Satellites per block when averaging the host profiles
REQUIRED_PARAMETERS = ("HODConcA", "HODConcLogMpivot", "HODConcB", "HODConcC")


NFW_SERIES_LIMIT = 0.1  # Below this argument m(t) is summed as a series (as hod_populate.c)
NFW_SERIES_TERMS = 24  # Terms of the series; each is at most a tenth of the one before


def _nfw_scaled_mass_series(t):
    """Return m(t) / t^2 = sum over k >= 0 of (-1)^k (k + 1) / (k + 2) t^k, for 0 <= t < 0.1."""
    t = np.asarray(t, dtype=float)
    total = np.zeros_like(t)
    power = np.ones_like(t)
    for k in range(NFW_SERIES_TERMS):
        term = (k + 1.0) / (k + 2.0) * power
        total += -term if k % 2 else term
        power = power * t
    return total


def _nfw_mass(t):
    """Return m(t) = ln(1 + t) - t / (1 + t), from the series where the closed form cancels."""
    t = np.asarray(t, dtype=float)
    small = t < NFW_SERIES_LIMIT
    closed = np.log1p(np.where(small, 1.0, t)) - np.where(small, 1.0, t) / (
        1.0 + np.where(small, 1.0, t)
    )
    return np.where(small, t * t * _nfw_scaled_mass_series(np.where(small, t, 0.0)), closed)


def nfw_enclosed_fraction(x, c):
    """
    Return m(c x) / m(c) for x in [0, 1] and concentration c > 0 (broadcasts).

    Finite, monotone in x, 0 at x = 0 and 1 at x = 1 for any finite c > 0: for
    c < 0.1 it is x^2 S(c x) / S(c) with the series S = m / t^2 (the module's guard), where the
    closed form ln(1 + t) - t / (1 + t) would cancel to noise or NaN.
    """
    x, c = np.broadcast_arrays(np.asarray(x, dtype=float), np.asarray(c, dtype=float))
    series = x * x * _nfw_scaled_mass_series(np.where(c < NFW_SERIES_LIMIT, c * x, 0.0))
    series = series / _nfw_scaled_mass_series(np.where(c < NFW_SERIES_LIMIT, c, 0.0))
    safe_c = np.where(c < NFW_SERIES_LIMIT, 1.0, c)
    closed = _nfw_mass(safe_c * x) / _nfw_mass(safe_c)
    return np.where(c < NFW_SERIES_LIMIT, series, closed)


def concentration(mass_msun_h, redshift, par):
    """Return the Duffy et al. (2008) concentration the run file defines."""
    return (
        par["HODConcA"]
        * (mass_msun_h / 10.0 ** par["HODConcLogMpivot"]) ** par["HODConcB"]
        * (1.0 + redshift) ** par["HODConcC"]
    )


def predicted_cumulative(grid, conc):
    """Return the satellite-weighted mean of each host's NFW enclosed fraction on grid."""
    total = np.zeros(len(grid))
    for lo in range(0, len(conc), PREDICTION_CHUNK):
        block = conc[lo : lo + PREDICTION_CHUNK]
        total += nfw_enclosed_fraction(grid[None, :], block[:, None]).sum(axis=0)
    return total / len(conc)


def satellite_phase_space(galaxies, box_size, redshift, par):
    """
    Measure the created satellites relative to their hosts.

    The position and velocity offsets are taken from the host row (minimum-image distance,
    host bulk velocity). The normalisation uses the satellite's own row: a created row carries
    the host's current virial radius and velocity (and, in infallMvir, its Mvir; the row's own
    Mvir is zero), which are the values hod_populate placed it with.

    Returns:
        (s, concentrations, velocity_ratio): s = physical radius / the satellite's Rvir, the
        concentration at the run's parameters from the draw-time host mass, and an (N, 3) array
        of the velocity offset over the satellite's Vvir / sqrt(2).
    """
    _hosts, sats, sat_host = host_lookup(galaxies)
    usable = (
        (galaxies.Rvir[sats] > 0.0)
        & (galaxies.Vvir[sats] > 0.0)
        & (galaxies.infallMvir[sats] > 0.0)
    )
    sats, hosts = sats[usable], sat_host[usable]

    delta = galaxies.Pos[sats].astype(np.float64) - galaxies.Pos[hosts].astype(np.float64)
    delta -= box_size * np.round(delta / box_size)
    r_com = np.sqrt((delta**2).sum(axis=1))
    s = r_com / ((1.0 + redshift) * galaxies.Rvir[sats].astype(np.float64))

    mass = galaxies.infallMvir[sats].astype(np.float64) * 1.0e10
    conc = concentration(mass, redshift, par)

    sigma_1d = galaxies.Vvir[sats].astype(np.float64) / np.sqrt(2.0)
    dv = galaxies.Vel[sats].astype(np.float64) - galaxies.Vel[hosts].astype(np.float64)
    return s, conc, dv / sigma_1d[:, None]


def plot(
    galaxies,
    volume,
    metadata,
    params,
    output_dir="plots",
    output_format=".png",
    verbose=False,
):
    """
    Create the two-panel satellite phase-space plot.

    Args:
        galaxies: Halo recarray with the HOD sample flag
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata (box_size, redshift)
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message): exactly one is not None.
    """
    success, _optional, msg = check_required_fields(
        galaxies,
        required_fields=[
            "Type",
            "Pos",
            "Vel",
            "Rvir",
            "Vvir",
            "infallMvir",
            "HODGhost",
            "UniqueGalaxyID",
            "UniqueCentralGalaxyID",
        ],
        plot_name="HOD Satellite Profile",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    par, missing = read_module_parameters(params, REQUIRED_PARAMETERS)
    if missing:
        return None, "HOD parameters missing from the run file's modules.parameters: " + ", ".join(
            missing
        )
    box_size = metadata.get("box_size")
    if not box_size or box_size <= 0:
        return None, "metadata carries no positive box_size"

    redshift = metadata.get("redshift")
    if redshift is None:
        return None, "Could not map the snapshot to a redshift: metadata carries no redshift"

    s, conc, v_ratio = satellite_phase_space(galaxies, box_size, redshift, par)
    if len(s) == 0:
        return None, "No created satellites (Type 2, HODGhost == 0) with a Type 0 host"

    grid = np.linspace(0.0, 1.0, RADIUS_GRID_POINTS)
    predicted = predicted_cumulative(grid, conc)
    s_sorted = np.sort(s)
    empirical = np.arange(1, len(s) + 1) / len(s)
    ks = float(np.max(np.abs(empirical - np.interp(s_sorted, grid, predicted))))
    if verbose:
        n_created = int(np.count_nonzero((galaxies.Type == 2) & (galaxies.HODGhost == 0)))
        print(
            f"  z={redshift:.4f}, satellites={len(s)} of {n_created} created "
            f"({n_created - len(s)} dropped: no Type 0 host found or a non-positive "
            f"Rvir/Vvir/infallMvir), max |F_emp - F_pred|={ks:.4f}"
        )
        print(f"  velocity ratio mean={v_ratio.mean():+.4f}, std={v_ratio.std():.4f}")

    fig, (ax_r, ax_v) = plt.subplots(1, 2, figsize=(13, 5.5))
    setup_plot_fonts(ax_r)
    setup_plot_fonts(ax_v)

    ax_r.step(s_sorted, empirical, where="post", c="tab:red", lw=2, label="created satellites")
    ax_r.plot(grid, predicted, "k--", lw=1.5, label="NFW, each host's own $c$")
    ax_r.set_xlim(0.0, 1.0)
    ax_r.set_ylim(0.0, 1.02)
    ax_r.set_xlabel(
        r"$r_{\rm com}\,/\,[(1+z)\,R_{\rm vir,sat}]$  (physical radius / $R_{\rm vir}$)",
        fontsize=AXIS_LABEL_SIZE,
    )
    ax_r.set_ylabel(r"cumulative fraction of satellites", fontsize=AXIS_LABEL_SIZE)
    ax_r.text(
        0.04,
        0.66,
        f"{SAMPLE_LABEL}\nz = {redshift:.3f}\n$N_{{\\rm sat}}$ = {len(s)}\nmax |$\\Delta F$| = {ks:.3f}",
        transform=ax_r.transAxes,
        fontsize=12,
    )
    setup_legend(ax_r, loc="lower right")

    x_min, x_max, y_min, y_max = get_profile_axes(params, PLOT_KEY, PLOT_XLIM, PLOT_YLIM)
    edges = make_bin_edges(x_min, x_max, VELOCITY_BINWIDTH)
    # Weight by the whole per-component sample and the bin width, not density=True: a profile
    # window narrower than the data must not renormalise the retained values to unit area.
    weights = np.full(len(v_ratio), 1.0 / (len(v_ratio) * (edges[1] - edges[0])))
    for axis, name, colour in zip(range(3), "xyz", ("tab:blue", "tab:green", "tab:orange")):
        ax_v.hist(
            v_ratio[:, axis],
            bins=edges,
            weights=weights,
            histtype="step",
            lw=1.5,
            color=colour,
            label=rf"$\Delta v_{name}$",
        )
    gauss_x = np.linspace(x_min, x_max, 400)
    ax_v.plot(
        gauss_x,
        np.exp(-0.5 * gauss_x**2) / np.sqrt(2.0 * np.pi),
        "k--",
        lw=1.5,
        label="unit Gaussian",
    )
    ax_v.set_xlim(x_min, x_max)
    ax_v.set_ylim(y_min, y_max)
    ax_v.set_xlabel(
        r"$(v_{\rm sat} - v_{\rm host})\,/\,(V_{\rm vir,sat}/\sqrt{2})$  [dimensionless]",
        fontsize=AXIS_LABEL_SIZE,
    )
    ax_v.set_ylabel(r"probability density", fontsize=AXIS_LABEL_SIZE)
    ax_v.text(
        0.04,
        0.66,
        f"{SAMPLE_LABEL}\nmean = {v_ratio.mean():+.3f}\nstd = {v_ratio.std():.3f}",
        transform=ax_v.transAxes,
        fontsize=12,
    )
    setup_legend(ax_v, loc="upper right")

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "HODSatelliteProfile", output_format, verbose
    )
    return plot_path, None
