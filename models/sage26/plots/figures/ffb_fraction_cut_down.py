#!/usr/bin/env python

"""
SAGE26 Feedback-Free-Burst Fraction Plot (cut-down port of paper figure 23c)

A cut-down port of SAGE26's `plot_23c_ffb_fraction_bk25` (plotting/paper_plots.py:10374-10446,
as the staged source numbers it), with its x-axis helper `_ffb_histogram_x_axes`
(:10349-10371) and the resolution cut of its reader `read_snap_from_files` (:461-520): the
fraction of resolved galaxies that are in the feedback-free-burst regime (FFBRegime 1) against
redshift, drawn as a step for each written snapshot. This is not SAGE26's exact figure; the cut-down
items are listed below.

Requires: FFBRegime, Len

Fields read (units): FFBRegime (1 for a galaxy in the feedback-free-burst regime) and Len (the
number of dark-matter particles of the halo). Every galaxy row of the snapshot with
`Len >= 20` (`MIN_PARTICLES`, :83, applied at :513-515) is counted, with no Type or other cut.

Kept from SAGE26: the selection above; the fraction, the number of those rows with
`FFBRegime == 1` over their number, NaN (undefined, not zero) for a snapshot with no such row
(:10404-10410); the `z <= 15` filter (:10418-10420); the step edges, 15.0, then the midpoint of
each consecutive pair of the drawn snapshots' redshifts taken in increasing snapshot number, then
0.0 (:10422-10427), in the plotting coordinate log10(1 + z) (:10429); `ax.stairs(...,
fill=False)` for the primary model, labelled 'FFB' (:10385, :10431-10433); log y (:10439); the
legend at upper left (:10441); the x axes of `_ffb_histogram_x_axes`: a window from 0 to
log10(1 + 15), bottom ticks at z = 0, 1, 2, 3, 5, 7, 10, 15 labelled with those values, and a
twin top axes with ticks at log10(1 + z) = 0.0 to 1.2 in steps of 0.2 labelled `f'{t:g}'`
(:10357-10371).

Cut down from SAGE26, each a permitted deviation:
  - Snapshots: the written snapshots (eight for the mini-Millennium run files), through
    ``all_snapshots``, instead of SAGE26's 64 (`range(64)`, :10401). The edges are SAGE26's
    midpoint rule applied to those snapshots, so each step spans the redshifts between written
    snapshots; the highest written snapshot's step extends by that rule from the midpoint to the
    next snapshot up to the z <= 15 limit, drawing that snapshot's fraction across redshifts where
    no snapshot is written (SAGE26's own edge construction, :10422), and SAGE26's separate steps
    above the highest written snapshot are absent.
  - Comparison run: SAGE26's MBK25 curve (`FFB_BK25_SMOOTH_DIR`, :10386), from a run the package
    does not make, is not drawn.
  - Y label: 'FFB fraction' instead of SAGE26's 'FFB/MBK25 fraction' (:10440), because no MBK25
    curve is drawn.
  - Title: 'Cut-down port of SAGE26 paper plot 23c: eight output snapshots, no MBK25 run', which
    SAGE26's figure does not have, saying it is not SAGE26's exact figure.
  - File stem: FFB_Fraction_Li24_cut_down, not SAGE26's FFB_Fraction_Li24_vs_MBK25 (:10447), for
    the same reason.
  - Style: SAGE26's paper style file (`setup_style`, :429-431) is not applied.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_plot_fonts
from figures._snapshot_selection import all_snapshots
from output_utils import check_required_fields, save_and_close_figure, setup_figure

# Minimum dark-matter particles for a resolved halo (paper_plots.py:83, `MIN_PARTICLES = 20`)
MIN_PARTICLES = 20

# Highest redshift drawn (paper_plots.py:10418, `z_mask = z <= 15`) and the top step edge (:10422)
Z_MAX = 15.0

# Bottom ticks of the redshift axis (paper_plots.py:10361)
Z_TICKS = [0, 1, 2, 3, 5, 7, 10, 15]

# Top ticks of the log10(1 + z) axis (paper_plots.py:10367)
LOG1PZ_TICKS = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.2]


def ffb_fraction(ffb_regime, length):
    """
    Return the fraction of resolved galaxies in the feedback-free-burst regime.

    SAGE26 paper_plots.py:513-515 keeps the rows with `Len >= MIN_PARTICLES`, and :10404-10410
    takes `np.sum(ffb_regime == 1) / len(ffb_regime)` of them, or NaN ("undefined rather than
    zero") when none remain.
    """
    resolved = length >= MIN_PARTICLES
    n_resolved = int(np.sum(resolved))
    if n_resolved == 0:
        return np.nan
    return np.sum(ffb_regime[resolved] == 1) / n_resolved


def step_edges(snapshots):
    """
    Return the drawn snapshots and the redshift edges of their steps.

    SAGE26 paper_plots.py:10418-10427: the snapshots with `z <= 15` in increasing snapshot
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

    # paper_plots.py:10398-10420
    drawn, z_edges = step_edges(snapshots)
    frac_filtered = np.array(
        [ffb_fraction(snapshots[snap][0].FFBRegime, snapshots[snap][0].Len) for snap in drawn],
        dtype=float,
    )
    log1pz_edges = np.log10(1 + z_edges)

    fig, ax = setup_figure()

    # paper_plots.py:10431-10433: no step is drawn when no snapshot is written
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

    # paper_plots.py:10439-10441
    ax.set_yscale("log")
    ax.set_ylabel("FFB fraction", fontsize=AXIS_LABEL_SIZE)
    ax.set_title(
        "Cut-down port of SAGE26 paper plot 23c: eight output snapshots, no MBK25 run", fontsize=10
    )
    ax.legend(loc="upper left", frameon=False)

    # _ffb_histogram_x_axes, paper_plots.py:10357-10371
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
