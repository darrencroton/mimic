#!/usr/bin/env python

"""
Mimic HOD Satellite Phase-Space Plot

Two-panel check of where hod_populate puts its satellites and how fast they move.

Left: the cumulative distribution of s = r_com / ((1 + z) Rvir_host) for the created
satellites, where r_com is the minimum-image distance from the satellite to its host in
comoving Mpc/h and Rvir is the host's physical virial radius (so s is the physical radius in
units of Rvir). It is compared with the satellite-weighted average over hosts of each host's
own NFW enclosed-mass fraction at its concentration c:

    F(s) = (1 / N_sat) sum over satellites of m(c s) / m(c),    m(x) = ln(1 + x) - x / (1 + x)

with c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC from the run file and
M = Mvir in Msun/h. Averaging each host's own profile, rather than using one concentration,
keeps the prediction exact for a population spanning a wide host-mass range.

Right: the histogram of every component of the satellite-minus-host velocity offset divided
by the host's Vvir / sqrt(2), against the unit Gaussian.

The snapshot redshift comes from the package's a_list through SnapshotRedshiftMapper.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    SAMPLE_LABEL,
    host_lookup,
    read_hod_parameters,
    setup_legend,
    setup_plot_fonts,
)
from output_utils import (
    check_required_fields,
    get_profile_axes,
    make_bin_edges,
    save_and_close_figure,
)
from snapshot_redshift_mapper import SnapshotRedshiftMapper

PLOT_KEY = "hod_satellite_profile"
PLOT_XLIM = (-4.0, 4.0)  # Velocity panel, offset / (Vvir / sqrt(2))
PLOT_YLIM = (0.0, 0.5)
VELOCITY_BINWIDTH = 0.25
RADIUS_GRID_POINTS = 201
PREDICTION_CHUNK = 20000  # Satellites per block when averaging the host profiles
REQUIRED_PARAMETERS = ("HODConcA", "HODConcLogMpivot", "HODConcB", "HODConcC")


def nfw_enclosed_fraction(x, c):
    """Return m(c x) / m(c) for x in [0, 1] and concentration c (broadcasts)."""
    cx = c * x
    return (np.log1p(cx) - cx / (1.0 + cx)) / (np.log1p(c) - c / (1.0 + c))


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

    Returns:
        (s, concentrations, velocity_ratio): s = physical radius / Rvir_host for each
        satellite, the host concentration at the run's parameters, and an (N, 3) array of the
        velocity offset over Vvir_host / sqrt(2).
    """
    _hosts, sats, sat_host = host_lookup(galaxies)
    usable = (
        (galaxies.Rvir[sat_host] > 0.0)
        & (galaxies.Vvir[sat_host] > 0.0)
        & (galaxies.Mvir[sat_host] > 0.0)
    )
    sats, hosts = sats[usable], sat_host[usable]

    delta = galaxies.Pos[sats].astype(np.float64) - galaxies.Pos[hosts].astype(np.float64)
    delta -= box_size * np.round(delta / box_size)
    r_com = np.sqrt((delta**2).sum(axis=1))
    s = r_com / ((1.0 + redshift) * galaxies.Rvir[hosts].astype(np.float64))

    mass = galaxies.Mvir[hosts].astype(np.float64) * 1.0e10
    conc = concentration(mass, redshift, par)

    sigma_1d = galaxies.Vvir[hosts].astype(np.float64) / np.sqrt(2.0)
    dv = galaxies.Vel[sats].astype(np.float64) - galaxies.Vel[hosts].astype(np.float64)
    return s, conc, dv / sigma_1d[:, None]


def snapshot_redshift(galaxies, params):
    """Return (redshift, None) for the single snapshot in galaxies, or (None, reason)."""
    snapshots = np.unique(galaxies.SnapNum)
    if len(snapshots) != 1:
        return None, f"Expected one snapshot in the data, found SnapNum values {snapshots.tolist()}"
    mapper_params = dict(params)
    mapper_params["verbose"] = False
    try:
        mapper = SnapshotRedshiftMapper(None, mapper_params, params.get("OutputDir"))
        return mapper.get_redshift(int(snapshots[0])), None
    except SystemExit:
        return None, f"Snapshot {int(snapshots[0])} is not in the package's a_list"


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
        metadata: Dictionary with additional metadata (box_size)
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
            "SnapNum",
            "Pos",
            "Vel",
            "Rvir",
            "Vvir",
            "Mvir",
            "HODGhost",
            "UniqueGalaxyID",
            "UniqueCentralGalaxyID",
        ],
        plot_name="HOD Satellite Profile",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    par, missing = read_hod_parameters(params, REQUIRED_PARAMETERS)
    if missing:
        return None, "HOD parameters missing from the run file's modules.parameters: " + ", ".join(
            missing
        )
    box_size = metadata.get("box_size")
    if not box_size or box_size <= 0:
        return None, "metadata carries no positive box_size"

    redshift, reason = snapshot_redshift(galaxies, params)
    if redshift is None:
        return None, reason

    s, conc, v_ratio = satellite_phase_space(galaxies, box_size, redshift, par)
    if len(s) == 0:
        return None, "No created satellites (Type 2, HODGhost == 0) with a Type 0 host"

    grid = np.linspace(0.0, 1.0, RADIUS_GRID_POINTS)
    predicted = predicted_cumulative(grid, conc)
    s_sorted = np.sort(s)
    empirical = np.arange(1, len(s) + 1) / len(s)
    ks = float(np.max(np.abs(empirical - np.interp(s_sorted, grid, predicted))))
    if verbose:
        print(f"  z={redshift:.4f}, satellites={len(s)}, max |F_emp - F_pred|={ks:.4f}")
        print(f"  velocity ratio mean={v_ratio.mean():+.4f}, std={v_ratio.std():.4f}")

    fig, (ax_r, ax_v) = plt.subplots(1, 2, figsize=(13, 5.5))
    setup_plot_fonts(ax_r)
    setup_plot_fonts(ax_v)

    ax_r.step(s_sorted, empirical, where="post", c="tab:red", lw=2, label="created satellites")
    ax_r.plot(grid, predicted, "k--", lw=1.5, label="NFW, each host's own $c$")
    ax_r.set_xlim(0.0, 1.0)
    ax_r.set_ylim(0.0, 1.02)
    ax_r.set_xlabel(
        r"$r_{\rm com}\,/\,[(1+z)\,R_{\rm vir,host}]$  (physical radius / $R_{\rm vir}$)",
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

    x_min, x_max, _y0, _y1 = get_profile_axes(params, PLOT_KEY, PLOT_XLIM, PLOT_YLIM)
    edges = make_bin_edges(x_min, x_max, VELOCITY_BINWIDTH)
    for axis, name, colour in zip(range(3), "xyz", ("tab:blue", "tab:green", "tab:orange")):
        ax_v.hist(
            v_ratio[:, axis],
            bins=edges,
            density=True,
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
    ax_v.set_xlabel(
        r"$(v_{\rm sat} - v_{\rm host})\,/\,(V_{\rm vir,host}/\sqrt{2})$  [dimensionless]",
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
