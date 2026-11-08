#!/usr/bin/env python

"""
SAGE26 Central and Satellite Counts Plot

Port of the "Plotting histogram of Centrals with satellites overlaid as a function of redshift"
section of SAGE26's history plotting script (plotting/allresults-history.py:1805-1850, figure
CentralSatelliteCounts): the average number of central galaxies and of satellite galaxies (orphans
included) over the snapshots in each of the fifteen redshift bins, as pairs of bars. The section
is the specification; where this port differs it is a permitted deviation below.

Requires: Type

Fields read (units): Type (0 central, 1 satellite, 2 orphan; :1827-1828, a Type of 1 or more is
a satellite).

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:1810-1814), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26. The bar of a bin with
no snapshot is not drawn.

SAGE26 draws no legend (it is commented out, :1844-1846), so the port draws none.

Permitted deviations: none beyond drawing the figure from the run's eight snapshots, where SAGE26
reads 64, so a bin holds at most one snapshot.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._snapshot_selection import Z_BINS, snapshots_in_z_range
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)


def count_centrals_and_satellites(types):
    """
    Return the numbers of central and satellite galaxies.

    SAGE26 allresults-history.py:1827-1828: `np.sum(types == 0)` and `np.sum(types >= 1)`
    (">= 1: orphans (Type 2) are satellites too").
    """
    return np.sum(types == 0), np.sum(types >= 1)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the central and satellite counts plot.

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
        return None, "No snapshot data available for the central and satellite counts plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0], required_fields=["Type"], plot_name="Central Satellite Counts"
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # allresults-history.py:1815 (cividis)
    colors = plt.cm.cividis(np.linspace(0.1, 0.9, len(Z_BINS)))

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1817-1837
    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        central_counts = []
        satellite_counts = []
        for snap in snap_indices:
            n_central, n_satellite = count_centrals_and_satellites(snapshots[snap][0].Type)
            central_counts.append(n_central)
            satellite_counts.append(n_satellite)

        central_mean = np.mean(central_counts)
        satellite_mean = np.mean(satellite_counts)
        label = f"{z_min:.1f} < z < {z_max:.1f}"
        ax.bar(
            i - 0.15,
            central_mean,
            width=0.3,
            color=colors[i],
            label=label + " (Centrals)",
            gid="model",
        )
        ax.bar(
            i + 0.15,
            satellite_mean,
            width=0.3,
            color=colors[i],
            alpha=0.5,
            label=label + " (Satellites)",
            gid="model",
        )

    # allresults-history.py:1839-1843: the upper limit is 1.1 times the autoscaled top
    x_min, x_max, y_min, y_max = get_profile_axes(
        params,
        "central_satellite_counts",
        (-0.5, len(Z_BINS) - 0.5),
        (0, 1.1 * max(ax.get_ylim())),
    )
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xticks(range(len(Z_BINS)))
    ax.set_xticklabels(
        [f"{z_min:.1f}-{z_max:.1f}" for z_min, z_max in Z_BINS], rotation=45, ha="right"
    )
    # SAGE26 figure parity: allresults-history.py:1843-1846 sets no x label and comments out the
    # legend, and no bin holds z = 0. Mirrored as written.
    ax.set_ylabel("Average Count", fontsize=AXIS_LABEL_SIZE)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "CentralSatelliteCounts", output_format, verbose
    )
    return plot_path, None
