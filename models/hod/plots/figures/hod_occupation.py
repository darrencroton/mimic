#!/usr/bin/env python

"""
Mimic HOD Occupation Plot

Realised mean occupation of the HOD sample against the analytic law of the run.

Hosts are the Type 0 rows; the sample is every row with HODGhost == 0 (the hosts whose
central was drawn present, and the created satellites, which are Type 2 rows naming their host
through UniqueCentralGalaxyID). For each host-mass bin the figure shows the realised
<Ncen>, <Nsat> and <Ntot> with the sampling error expected under the law, over the analytic
curves of Zheng, Coil & Zehavi (2007) computed from the parameters in the run file:

    <Ncen>(M) = 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]
    lambda(M) = ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha    for M > 10^HODLogM0
    <Nsat>(M) = <Ncen> lambda,    <Ntot>(M) = <Ncen> (1 + lambda)

The error bars are the standard error of the bin mean under that law. A host has a central
with probability p = <Ncen> (a binomial draw) and, only then, Nsat ~ Poisson(lambda), so per
host Var(Ncen) = p (1 - p), Var(Nsat) = p lambda + p (1 - p) lambda^2 and
Var(Ntot) = p (1 + 3 lambda + lambda^2) - p^2 (1 + lambda)^2.
"""

import math

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
    validate_filtered_data,
)

PLOT_KEY = "hod_occupation"
PLOT_XLIM = (10.8, 15.0)  # log10(M / [Msun/h])
PLOT_YLIM = (1.0e-3, 3.0e2)
BINWIDTH_DEX = 0.2  # Matches the module's audit bins
MIN_HOSTS_PER_BIN = 10  # Bins with fewer hosts are not drawn
MIN_HOSTS_FOR_CHECK = 50  # Bins the 3-sigma agreement is stated for
REQUIRED_PARAMETERS = ("HODLogMmin", "HODSigmaLogM", "HODLogM0", "HODLogM1", "HODAlpha")

_erf = np.vectorize(math.erf, otypes=[float])


def mean_ncen(log_m, par):
    """Return <Ncen> at log10 M [Msun/h]."""
    return 0.5 * (1.0 + _erf((np.asarray(log_m) - par["HODLogMmin"]) / par["HODSigmaLogM"]))


def mean_lambda(log_m, par):
    """Return the Poisson mean of the satellite count of a host that has a central."""
    mass = 10.0 ** np.asarray(log_m, dtype=float)
    excess = np.maximum(mass - 10.0 ** par["HODLogM0"], 0.0)
    return (excess / 10.0 ** par["HODLogM1"]) ** par["HODAlpha"]


def law(log_m, par):
    """
    Return the analytic mean and per-host variance of the three occupations.

    Returns:
        dict keyed "cen", "sat", "tot", each (mean, variance) at log_m.
    """
    p = mean_ncen(log_m, par)
    lam = mean_lambda(log_m, par)
    return {
        "cen": (p, p * (1.0 - p)),
        "sat": (p * lam, p * lam + p * (1.0 - p) * lam**2),
        "tot": (p * (1.0 + lam), p * (1.0 + 3.0 * lam + lam**2) - (p * (1.0 + lam)) ** 2),
    }


def realised_occupation(galaxies, edges, par):
    """
    Bin the realised occupation of the hosts and compare it with the law.

    Args:
        galaxies: Halo recarray (Type, Mvir, HODGhost, UniqueGalaxyID, UniqueCentralGalaxyID).
        edges: log10(M / [Msun/h]) bin edges.
        par: dict of the five occupation parameters.

    Returns:
        dict with arrays over bins: "centre", "hosts", and for each of "cen", "sat", "tot" the
        realised mean ("<k>"), the law averaged over the bin's own hosts ("<k>_law") and the
        standard error under the law ("<k>_se").
    """
    # Mvir is in 1e10 Msun/h, so the mass in Msun/h is Mvir * 1e10 with no h factor.
    host_rows, _sat_rows, sat_host = host_lookup(galaxies)
    host_rows = host_rows[galaxies.Mvir[host_rows] > 0.0]
    log_m = np.log10(galaxies.Mvir[host_rows].astype(np.float64) * 1.0e10)

    nsat_by_row = np.bincount(sat_host, minlength=len(galaxies))
    realised = {
        "cen": (galaxies.HODGhost[host_rows] == 0).astype(np.float64),
        "sat": nsat_by_row[host_rows].astype(np.float64),
    }
    realised["tot"] = realised["cen"] + realised["sat"]

    nbins = len(edges) - 1
    bin_of = np.searchsorted(edges, log_m, side="right") - 1
    bin_of[log_m == edges[-1]] = nbins - 1
    expected = law(log_m, par)

    out = {"centre": 0.5 * (edges[1:] + edges[:-1]), "hosts": np.zeros(nbins, dtype=np.int64)}
    for key in ("cen", "sat", "tot"):
        for suffix in ("", "_law", "_se"):
            out[key + suffix] = np.full(nbins, np.nan)
    for b in range(nbins):
        members = bin_of == b
        n = int(members.sum())
        out["hosts"][b] = n
        if n == 0:
            continue
        for key in ("cen", "sat", "tot"):
            out[key][b] = realised[key][members].mean()
            out[key + "_law"][b] = expected[key][0][members].mean()
            out[key + "_se"][b] = math.sqrt(expected[key][1][members].mean() / n)
    return out


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
    Create the realised-versus-analytic mean occupation plot.

    Args:
        galaxies: Halo recarray with the HOD sample flag
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters (EnabledModules supplies the HOD parameters)
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message): exactly one is not None.
    """
    success, _optional, msg = check_required_fields(
        galaxies,
        required_fields=["Type", "Mvir", "HODGhost", "UniqueGalaxyID", "UniqueCentralGalaxyID"],
        plot_name="HOD Occupation",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    par, missing = read_hod_parameters(params, REQUIRED_PARAMETERS)
    if missing:
        return None, "HOD parameters missing from the run file's modules.parameters: " + ", ".join(
            missing
        )
    if par["HODSigmaLogM"] <= 0.0:
        return None, "HODSigmaLogM must be > 0"

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, PLOT_KEY, PLOT_XLIM, PLOT_YLIM, log_y=True
    )
    edges = make_bin_edges(x_min, x_max, BINWIDTH_DEX)

    w = np.where((galaxies.Type == 0) & (galaxies.Mvir > 0.0))[0]
    is_valid, skip_msg = validate_filtered_data(w, "HOD Occupation", verbose)
    if not is_valid:
        return None, skip_msg
    if not np.any(galaxies.HODGhost == 0):
        return None, "No sample members (HODGhost == 0): the snapshot was not populated"

    occ = realised_occupation(galaxies, edges, par)
    drawn = occ["hosts"] >= MIN_HOSTS_PER_BIN
    if not np.any(drawn):
        return None, f"No host-mass bin has at least {MIN_HOSTS_PER_BIN} hosts"

    if verbose:
        for b in np.where(occ["hosts"] >= MIN_HOSTS_FOR_CHECK)[0]:
            pulls = ", ".join(
                f"{k}={(occ[k][b] - occ[k + '_law'][b]) / occ[k + '_se'][b]:+.2f}"
                for k in ("cen", "sat", "tot")
                if occ[k + "_se"][b] > 0.0
            )
            print(f"  log M={occ['centre'][b]:.2f} hosts={occ['hosts'][b]}: pull {pulls}")

    fig, ax = plt.subplots(figsize=(8, 6))
    setup_plot_fonts(ax)

    grid = np.linspace(x_min, x_max, 400)
    curves = law(grid, par)
    series = (
        ("cen", r"$\langle N_{\rm cen}\rangle$", "tab:blue", "o"),
        ("sat", r"$\langle N_{\rm sat}\rangle$", "tab:red", "s"),
        ("tot", r"$\langle N_{\rm tot}\rangle$", "k", "^"),
    )
    for key, label, colour, marker in series:
        ax.plot(grid, np.where(curves[key][0] > 0, curves[key][0], np.nan), "-", c=colour, lw=1.5)
        mean = occ[key][drawn]
        err = occ[key + "_se"][drawn]
        shown = mean > 0.0
        lower = np.minimum(err[shown], 0.999 * mean[shown])
        ax.errorbar(
            occ["centre"][drawn][shown],
            mean[shown],
            yerr=[lower, err[shown]],
            fmt=marker,
            c=colour,
            ms=5,
            capsize=2,
            label=label + " realised",
        )
    ax.plot([], [], "-", c="0.4", lw=1.5, label="analytic law (lines)")

    ax.set_yscale("log")
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"log$_{10}$ M$_{\rm vir}$ [M$_{\odot}$ h$^{-1}$]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\langle N\rangle$ per host  (" + SAMPLE_LABEL + ")", fontsize=AXIS_LABEL_SIZE)
    ax.set_title(
        f"Mean occupation, bins of {BINWIDTH_DEX:g} dex with >= {MIN_HOSTS_PER_BIN} hosts; "
        "bars: expected sampling error",
        fontsize=10,
    )
    setup_legend(ax, loc="upper left")

    plot_path = save_and_close_figure(fig, output_dir, "HODOccupation", output_format, verbose)
    return plot_path, None
