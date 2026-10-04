#!/usr/bin/env python

"""
Mimic SHAM Correlation Function Plot

Real-space two-point correlation function xi(r) of the SHAM sample (ShamGhost == 0) in
stellar-mass thresholds, from the model-neutral pair counter in output_utils (periodic cell
grid, analytic random term), on logarithmic bins from 0.1 to 20 Mpc/h by default. Each
threshold selects the members with log10(M*/Msun) at or above it and is labelled with the
number density of that sample; a threshold with no positive xi in range is listed as not drawn,
and bins with pairs but xi <= 0 (not representable on the log axis) are counted on the figure.
The profile key axes.sham_correlation_function sets xmin/xmax
(comoving Mpc/h, within the helper's domain: at most half the box), ymin/ymax (the log10 y
range) and mass_thresholds (a list of log10 stellar masses in Msun at the simulation's h,
default [10.0, 10.5]). Error bars are Poisson errors on the pair counts.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE, SAMPLE_LABEL, setup_legend, setup_plot_fonts
from output_utils import (
    check_required_fields,
    get_profile_axes,
    log_radial_edges,
    require_full_box,
    save_and_close_figure,
    xi_series,
)

PLOT_KEY = "sham_correlation_function"
PLOT_XLIM = (0.1, 20.0)  # comoving Mpc/h
PLOT_YLIM = (1.0e-2, 1.0e4)
BINS_PER_DEX = 5
DEFAULT_THRESHOLDS = (10.0, 10.5)  # log10(M*/Msun)
COLOURS = ("#0173b2", "#c44e52", "#029e73", "#de8f05", "#8172b3", "#937860")


def _read_thresholds(params):
    """Return the profile's stellar-mass thresholds, or the defaults when unset."""
    axes = ((params.get("PlotProfile") or {}).get("axes") or {}).get(PLOT_KEY) or {}
    raw = axes.get("mass_thresholds", DEFAULT_THRESHOLDS)
    if not isinstance(raw, (list, tuple)):
        raw = [raw]
    return sorted({float(value) for value in raw})


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
    Create the real-space correlation function plot by stellar-mass threshold.

    Args:
        galaxies: Galaxy recarray with the SHAM sample flag
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata (box_size, hubble_h)
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message): exactly one is not None.
    """
    success, _optional, msg = check_required_fields(
        galaxies,
        required_fields=["StellarMass", "Pos", "ShamGhost"],
        plot_name="SHAM Correlation Function",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    box_size = metadata.get("box_size")
    skip = require_full_box(volume, box_size)
    if skip:
        return None, skip
    box_volume = float(box_size) ** 3

    try:
        thresholds = _read_thresholds(params)
    except (TypeError, ValueError) as exc:
        return None, f"Invalid mass_thresholds in the plot profile: {exc}"
    if not thresholds:
        return None, "The plot profile lists no mass_thresholds"

    r_min, r_max, y_min, y_max = get_profile_axes(
        params, PLOT_KEY, PLOT_XLIM, PLOT_YLIM, log_y=True
    )
    try:
        r_edges = log_radial_edges(r_min, r_max, BINS_PER_DEX)
    except ValueError as exc:
        return None, str(exc)
    nbins = len(r_edges) - 1
    r_centre = np.sqrt(r_edges[1:] * r_edges[:-1])

    member = galaxies.ShamGhost == 0
    h = metadata["hubble_h"]
    with np.errstate(divide="ignore"):
        log_mstar = np.log10(galaxies.StellarMass.astype(np.float64) * 1.0e10 / h)

    fig, ax = plt.subplots(figsize=(8, 6))
    setup_plot_fonts(ax)
    drawn = 0
    skipped = []
    hidden = []
    for threshold, colour in zip(thresholds, COLOURS * (len(thresholds) // len(COLOURS) + 1)):
        sample = member & (log_mstar >= threshold)
        n_sample = int(sample.sum())
        if n_sample < 2:
            skipped.append(f"log M* >= {threshold:g}: {n_sample} members")
            continue

        try:
            shown, n_hidden, xi, xi_err, dd = xi_series(galaxies.Pos[sample], box_size, r_edges)
        except ValueError as exc:
            plt.close(fig)
            return None, f"Correlation function input rejected: {exc}"
        if dd.sum() == 0:
            skipped.append(f"log M* >= {threshold:g}: no separation bin contains a pair")
            continue

        if not np.any(shown):
            skipped.append(f"log M* >= {threshold:g}: {n_hidden} bin(s) with pairs but xi <= 0")
            continue
        if n_hidden:
            hidden.append(f"log M* >= {threshold:g}: {n_hidden}")
        lower = np.minimum(xi_err[shown], 0.999 * xi[shown])
        if verbose:
            print(f"  log M* >= {threshold:g}: sample={n_sample}, bins={nbins}")
            for r, x, e, d in zip(r_centre, xi, xi_err, dd):
                print(f"  r={r:8.3f}  xi={x:12.4f} +- {e:.4f}  DD={d}")
        ax.errorbar(
            r_centre[shown],
            xi[shown],
            yerr=[lower, xi_err[shown]],
            fmt="o",
            c=colour,
            ms=5,
            capsize=2,
            label=(
                f"$\\log M_* \\geq {threshold:g}$,  "
                f"$n$ = {n_sample / box_volume:.2e} (Mpc/h)$^{{-3}}$"
            ),
        )
        drawn += 1

    if drawn == 0:
        plt.close(fig)
        return None, "No stellar-mass threshold has a positive xi (" + "; ".join(skipped) + ")"

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(r_min, r_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"$r$ [Mpc h$^{-1}$, comoving]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\xi(r)$  (" + SAMPLE_LABEL + ")", fontsize=AXIS_LABEL_SIZE)
    if skipped:
        ax.text(
            0.04,
            0.06,
            "not drawn: " + "; ".join(skipped),
            transform=ax.transAxes,
            fontsize=10,
        )
    if hidden:
        ax.text(
            0.96,
            0.06,
            "bin(s) with pairs but $\\xi \\leq 0$ not shown:\n" + "; ".join(hidden),
            transform=ax.transAxes,
            fontsize=10,
            ha="right",
        )
    setup_legend(ax, loc="upper right")

    plot_path = save_and_close_figure(
        fig, output_dir, "ShamCorrelationFunction", output_format, verbose
    )
    return plot_path, None
