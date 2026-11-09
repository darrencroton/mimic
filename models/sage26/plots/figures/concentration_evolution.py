#!/usr/bin/env python

"""
SAGE26 Concentration Evolution Plot

Follows the ConcentrationEvolution figure of SAGE26's history plotting script: the median halo
concentration, with a 16th to 84th percentile band, against redshift in four halo-mass bins. Where
it differs from SAGE26 the difference is stated below.

Requires: Concentration, Mvir

Fields read (units): Concentration (dimensionless; a galaxy needs a positive value) and Mvir (1e10
Msun/h, converted to Msun as SAGE26 converts it: x 1e10 / hubble_h). Every galaxy row is read, with
no Type cut, as in SAGE26.

Snapshots: every written snapshot, chosen through ``all_snapshots``, taken in increasing snapshot
number; a snapshot without galaxies is skipped.

Differences from SAGE26: none beyond drawing the figure from the snapshots the run writes, where
SAGE26 reads all 64.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._snapshot_selection import all_snapshots
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Halo-mass bins in Msun with their legend text
MASS_BINS = [
    (1e10, 1e11, r"$10^{10} - 10^{11}$"),
    (1e11, 1e12, r"$10^{11} - 10^{12}$"),
    (1e12, 1e13, r"$10^{12} - 10^{13}$"),
    (1e13, 1e15, r"$10^{13} - 10^{15}$"),
]

# Line colours of the four bins
COLORS = ["dodgerblue", "forestgreen", "darkorange", "firebrick"]

# A bin needs at least this many galaxies at a snapshot
MIN_GALAXIES = 5


def concentration_statistics(concentration, mvir_msun, mass_lo, mass_hi):
    """
    Return the median and the 16th and 84th percentiles of the concentration in one mass bin.

    As in SAGE26: `w = np.where((conc > 0) & (mvir >= mlo) & (mvir < mhi))[0]`; with `len(w) >= 5`
    the median and the 16th and 84th percentiles of `conc[w]`, else NaN for all three.
    """
    w = np.where((concentration > 0) & (mvir_msun >= mass_lo) & (mvir_msun < mass_hi))[0]
    if len(w) >= MIN_GALAXIES:
        return (
            np.median(concentration[w]),
            np.percentile(concentration[w], 16),
            np.percentile(concentration[w], 84),
        )
    return np.nan, np.nan, np.nan


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the concentration evolution plot.

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
        return None, "No snapshot data available for the concentration evolution plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["Concentration", "Mvir"],
        plot_name="Concentration Evolution",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "concentration_evolution", (0.0, 15.0), (0.0, 30.0)
    )

    z_arr = []
    medians = {i: [] for i in range(len(MASS_BINS))}
    p16 = {i: [] for i in range(len(MASS_BINS))}
    p84 = {i: [] for i in range(len(MASS_BINS))}

    for snap in all_snapshots(snapshots):
        galaxies, _volume, metadata = snapshots[snap]
        if len(galaxies) == 0:
            continue
        hubble_h = metadata["hubble_h"]
        conc = galaxies.Concentration
        mvir = galaxies.Mvir * 1.0e10 / hubble_h

        z_arr.append(metadata["redshift"])
        for i, (mass_lo, mass_hi, _label) in enumerate(MASS_BINS):
            median, low, high = concentration_statistics(conc, mvir, mass_lo, mass_hi)
            medians[i].append(median)
            p16[i].append(low)
            p84[i].append(high)

    fig, ax = setup_figure(figsize=(10, 7))

    z_arr = np.array(z_arr)
    for i, (_, _, label) in enumerate(MASS_BINS):
        med = np.array(medians[i])
        lo = np.array(p16[i])
        hi = np.array(p84[i])
        valid = np.isfinite(med)
        if np.any(valid):
            ax.plot(
                z_arr[valid],
                med[valid],
                color=COLORS[i],
                lw=2.5,
                label=label + r" $M_\odot$",
                gid="model",
            )
            ax.fill_between(
                z_arr[valid], lo[valid], hi[valid], color=COLORS[i], alpha=0.15, gid="model"
            )

    ax.set_xlabel(r"Redshift $z$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"Concentration $c$", fontsize=AXIS_LABEL_SIZE)
    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc="upper right", frameon=False)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "ConcentrationEvolution", output_format, verbose
    )
    return plot_path, None
