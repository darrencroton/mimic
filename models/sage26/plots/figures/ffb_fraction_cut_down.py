#!/usr/bin/env python

"""
SAGE26 Feedback-Free-Burst Fraction Plot (cut down from paper figure 23c)

Follows SAGE26's paper figure 23c (`plot_23c_ffb_fraction_bk25`): the fraction of resolved
galaxies that are in the feedback-free-burst regime against redshift, drawn as one step per
written snapshot in the plotting coordinate log10(1 + z), with SAGE26's x axes (bottom ticks at
z = 0, 1, 2, 3, 5, 7, 10, 15; a twin top axis in log10(1 + z)) and a log y axis.

Requires: FFBRegime, Len

Selection: every galaxy row of a snapshot with `Len >= 20` (`MIN_PARTICLES`), with no Type cut.
The fraction is the number of those rows with `FFBRegime == 1` over their number, NaN for a
snapshot with no such row; snapshots above z = 15 are not drawn.

Differences from SAGE26: only the written snapshots are drawn (SAGE26 reads all 64), so each step
spans the redshifts between written snapshots by SAGE26's midpoint rule; SAGE26's MBK25
comparison run is not drawn, so the y label is 'FFB fraction' rather than 'FFB/MBK25 fraction'
and the title says the figure is cut down; the file stem is FFB_Fraction_Li24_cut_down.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_plot_fonts
from figures._snapshot_selection import all_snapshots
from output_utils import check_required_fields, save_and_close_figure, setup_figure

# Minimum dark-matter particles for a resolved halo (SAGE26's `MIN_PARTICLES = 20`)
MIN_PARTICLES = 20

# Highest redshift drawn (SAGE26's `z_mask = z <= 15`) and the top step edge
Z_MAX = 15.0

# Bottom ticks of the redshift axis
Z_TICKS = [0, 1, 2, 3, 5, 7, 10, 15]

# Top ticks of the log10(1 + z) axis
LOG1PZ_TICKS = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.2]


def ffb_fraction(ffb_regime, length):
    """
    Return the fraction of resolved galaxies in the feedback-free-burst regime.

    SAGE26 keeps the rows with `Len >= MIN_PARTICLES` and takes
    `np.sum(ffb_regime == 1) / len(ffb_regime)` of them, or NaN ("undefined rather than zero")
    when none remain.
    """
    resolved = length >= MIN_PARTICLES
    n_resolved = int(np.sum(resolved))
    if n_resolved == 0:
        return np.nan
    return np.sum(ffb_regime[resolved] == 1) / n_resolved


def step_edges(snapshots):
    """
    Return the drawn snapshots and the redshift edges of their steps.

    As in SAGE26: the snapshots with `z <= 15` in increasing snapshot
    number, then `z_edges = [15.0]`, the midpoint `(z[i] + z[i+1]) / 2.0` of each consecutive
    pair, and `0.0`.

    Args:
        snapshots: Mapping of snapshot number to (galaxies, volume, metadata); only the
            redshift of each snapshot is read, and the order of the mapping does not matter.

    Returns:
        Tuple of (snapshot numbers drawn, in increasing snapshot number; the redshift edges, one
        more than the snapshots).
    """
    drawn = [snap for snap in all_snapshots(snapshots) if snapshots[snap][2]["redshift"] <= Z_MAX]
    z_filtered = [snapshots[snap][2]["redshift"] for snap in drawn]
    z_edges = [Z_MAX]
    for i in range(len(z_filtered) - 1):
        z_edges.append((z_filtered[i] + z_filtered[i + 1]) / 2.0)
    z_edges.append(0.0)
    return drawn, np.array(z_edges)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the cut-down feedback-free-burst fraction plot.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if a field is missing
    """
    if len(snapshots) == 0:
        return None, "No snapshot data available for the FFB fraction plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["FFBRegime", "Len"],
        plot_name="FFB Fraction (cut down)",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    drawn, z_edges = step_edges(snapshots)
    frac_filtered = np.array(
        [ffb_fraction(snapshots[snap][0].FFBRegime, snapshots[snap][0].Len) for snap in drawn],
        dtype=float,
    )
    log1pz_edges = np.log10(1 + z_edges)

    fig, ax = setup_figure()

    # No step is drawn when no snapshot is written
    if len(drawn) > 0:
        step = ax.stairs(
            frac_filtered,
            log1pz_edges,
            fill=False,
            label="FFB",
            edgecolor="black",
            linewidth=2,
        )
        step.set_gid("model")

    if verbose and np.any(np.isfinite(frac_filtered) & (frac_filtered > 0)):
        peak = int(np.nanargmax(frac_filtered))
        z_peak = snapshots[drawn[peak]][2]["redshift"]
        print(f"    FFB: peak fraction {np.nanmax(frac_filtered):.3f} at z={z_peak:.2f}")

    ax.set_yscale("log")
    ax.set_ylabel("FFB fraction", fontsize=AXIS_LABEL_SIZE)
    ax.set_title(
        f"Cut-down version of SAGE26 paper plot 23c: {len(drawn)} output snapshots, no MBK25 run",
        fontsize=10,
    )
    ax.legend(loc="upper left", frameon=False)

    # As SAGE26's `_ffb_histogram_x_axes`
    ax.set_xlim(0, np.log10(1 + Z_MAX))
    ax.set_xlabel(r"$z$", fontsize=AXIS_LABEL_SIZE)
    ax.set_xticks([np.log10(1 + zt) for zt in Z_TICKS])
    ax.set_xticklabels([str(zt) for zt in Z_TICKS])

    ax_top = ax.twiny()
    setup_plot_fonts(ax_top)  # house tick-label size, as on the bottom axis
    ax_top.set_xlim(ax.get_xlim())
    ax_top.set_xticks(LOG1PZ_TICKS)
    ax_top.set_xticklabels([f"{t:g}" for t in LOG1PZ_TICKS])
    ax_top.set_xlabel(r"$\log_{10}(1+z)$", labelpad=10, fontsize=AXIS_LABEL_SIZE)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "FFB_Fraction_Li24_cut_down", output_format, verbose
    )
    return plot_path, None
