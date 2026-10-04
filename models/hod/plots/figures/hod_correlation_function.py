#!/usr/bin/env python

"""
Mimic HOD Correlation Function Plot

Real-space two-point correlation function xi(r) of the HOD sample (HODGhost == 0), from the
model-neutral pair counter in output_utils (periodic cell grid, analytic random term), on
logarithmic bins from 0.1 to 20 Mpc/h by default. The profile keys axes.hod_correlation_function
xmin/xmax set the radial range (comoving Mpc/h, within the helper's domain: at most half the
box) and ymin/ymax the log10 y range. Error bars are Poisson errors on the pair counts. The
figure is annotated with the realised number density and satellite fraction of the sample.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE, SAMPLE_LABEL, setup_legend, setup_plot_fonts
from output_utils import (
    check_required_fields,
    correlation_function,
    get_profile_axes,
    save_and_close_figure,
)

PLOT_KEY = "hod_correlation_function"
PLOT_XLIM = (0.1, 20.0)  # comoving Mpc/h
PLOT_YLIM = (1.0e-2, 1.0e4)
BINS_PER_DEX = 5


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
    Create the real-space correlation function plot.

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
        required_fields=["Type", "Pos", "HODGhost"],
        plot_name="HOD Correlation Function",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    box_size = metadata.get("box_size")
    if not box_size or box_size <= 0:
        return None, "metadata carries no positive box_size"
    box_volume = float(box_size) ** 3
    if abs(volume - box_volume) > 1.0e-6 * box_volume:
        return None, (
            f"Only part of the box was read (volume {volume:.6g} of {box_volume:.6g} "
            "(Mpc/h)^3): the analytic random-pair term needs the whole periodic box"
        )

    sample = galaxies.HODGhost == 0
    n_sample = int(sample.sum())
    if n_sample < 2:
        return None, f"Fewer than two sample members (HODGhost == 0): {n_sample}"
    n_sat = int((sample & (galaxies.Type == 2)).sum())

    r_min, r_max, y_min, y_max = get_profile_axes(
        params, PLOT_KEY, PLOT_XLIM, PLOT_YLIM, log_y=True
    )
    if not 0.0 < r_min < r_max:
        return None, f"Radial range must satisfy 0 < xmin < xmax, got [{r_min}, {r_max}]"
    nbins = max(1, round(BINS_PER_DEX * np.log10(r_max / r_min)))
    r_edges = np.geomspace(r_min, r_max, nbins + 1)

    # Wrap into [0, box_size): a coordinate stored as exactly box_size (or rounded up to it by
    # the output precision) is the same point as 0 in a periodic box.
    with np.errstate(invalid="ignore"):
        positions = np.mod(galaxies.Pos[sample].astype(np.float64), box_size)
    positions[positions >= box_size] = 0.0
    try:
        xi, xi_err, dd, _rr = correlation_function(positions, box_size, r_edges)
    except ValueError as exc:
        return None, f"Correlation function input rejected: {exc}"
    if dd.sum() == 0:
        return None, "No separation bin contains a pair"

    # A logarithmic axis can only show xi > 0; count the bins with pairs that it cannot show.
    shown = (dd > 0) & (xi > 0.0)
    n_hidden = int(np.count_nonzero((dd > 0) & (xi <= 0.0)))
    r_centre = np.sqrt(r_edges[1:] * r_edges[:-1])
    lower = np.minimum(xi_err[shown], 0.999 * xi[shown])

    if verbose:
        print(f"  sample={n_sample}, satellites={n_sat}, bins={nbins}")
        for r, x, e, d in zip(r_centre, xi, xi_err, dd):
            print(f"  r={r:8.3f}  xi={x:12.4f} +- {e:.4f}  DD={d}")

    fig, ax = plt.subplots(figsize=(8, 6))
    setup_plot_fonts(ax)
    ax.errorbar(
        r_centre[shown],
        xi[shown],
        yerr=[lower, xi_err[shown]],
        fmt="o",
        c="k",
        ms=5,
        capsize=2,
        label="HOD sample",
    )
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(r_min, r_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"$r$ [Mpc h$^{-1}$, comoving]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\xi(r)$  (" + SAMPLE_LABEL + ")", fontsize=AXIS_LABEL_SIZE)
    ax.text(
        0.04,
        0.06,
        f"$N$ = {n_sample}\n"
        f"$n$ = {n_sample / box_volume:.3e} (Mpc/h)$^{{-3}}$\n"
        f"$f_{{\\rm sat}}$ = {n_sat / n_sample:.3f}",
        transform=ax.transAxes,
        fontsize=12,
    )
    if n_hidden:
        ax.text(
            0.96,
            0.06,
            f"{n_hidden} bin(s) with pairs but $\\xi \\leq 0$ not shown",
            transform=ax.transAxes,
            fontsize=10,
            ha="right",
        )
    setup_legend(ax, loc="upper right")

    plot_path = save_and_close_figure(
        fig, output_dir, "HODCorrelationFunction", output_format, verbose
    )
    return plot_path, None
