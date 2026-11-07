#!/usr/bin/env python

"""
SAGE26 Quenched Central and Satellite Counts Plot

Port of the "Plotting histogram of quiecent centrals with satellites overlaid as a function of
redshift" section of SAGE26's history plotting script (plotting/allresults-history.py:1982-2031,
figure QuenchedCentralSatelliteCounts): the average number of quenched central galaxies and of
quenched satellite galaxies (orphans included) over the snapshots in each of the fifteen redshift
bins, as pairs of bars. The section is the specification; where this port differs it is a
permitted deviation below.

Requires: StellarMass, SfrDisk, SfrBulge, Type

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-history.py:337), SfrDisk and SfrBulge (Msun/yr; the rate is their
sum, :2004) and Type (0 central, 1 or more satellite; :2008-2009). A galaxy is quenched when its
specific star formation rate is below 10^sSFRcut per year, sSFRcut = -11.0 (:23, :2007). A galaxy
with no stellar mass has a specific rate of infinity or NaN and is never quenched.

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:1987-1991), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26.

SAGE26 draws no legend (it is commented out, :2025-2027), so the port draws none.

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

# log10 of the specific star formation rate [1/yr] dividing quiescent from star-forming galaxies
# (allresults-history.py:23, `sSFRcut = -11.0`)
SSFR_CUT = -11.0


def count_quenched(types, sfr_total, stellar_mass):
    """
    Return the numbers of quenched central and quenched satellite galaxies.

    SAGE26 allresults-history.py:2006-2009: `sSFR_linear = sfr_total / stellar_mass`,
    `quenched = sSFR_linear < 10.0**sSFRcut`, `np.sum((types == 0) & quenched)` and
    `np.sum((types >= 1) & quenched)` (">= 1: orphans too"), the stellar mass in Msun.
    """
    with np.errstate(divide="ignore", invalid="ignore"):
        sfr_per_mass = sfr_total / stellar_mass
    quenched = sfr_per_mass < 10.0**SSFR_CUT
    return np.sum((types == 0) & quenched), np.sum((types >= 1) & quenched)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the quenched central and satellite counts plot.

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
        return None, "No snapshot data available for the quenched counts plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "SfrDisk", "SfrBulge", "Type"],
        plot_name="Quenched Central Satellite Counts",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # allresults-history.py:1992 (cividis)
    colors = plt.cm.cividis(np.linspace(0.1, 0.9, len(Z_BINS)))

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1994-2018
    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        quenched_central_counts = []
        quenched_satellite_counts = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            sfr_total = galaxies.SfrDisk + galaxies.SfrBulge
            stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
            n_central, n_satellite = count_quenched(galaxies.Type, sfr_total, stellar_mass)
            quenched_central_counts.append(n_central)
            quenched_satellite_counts.append(n_satellite)

        central_mean = np.mean(quenched_central_counts)
        satellite_mean = np.mean(quenched_satellite_counts)
        label = f"{z_min:.1f} < z < {z_max:.1f}"
        ax.bar(
            i - 0.15,
            central_mean,
            width=0.3,
            color=colors[i],
            label=label + " (Quenched Centrals)",
            gid="model",
        )
        ax.bar(
            i + 0.15,
            satellite_mean,
            width=0.3,
            color=colors[i],
            alpha=0.5,
            label=label + " (Quenched Satellites)",
            gid="model",
        )

    # allresults-history.py:2020-2024: the upper limit is 1.1 times the autoscaled top
    x_min, x_max, y_min, y_max = get_profile_axes(
        params,
        "quenched_central_satellite_counts",
        (-0.5, len(Z_BINS) - 0.5),
        (0, 1.1 * max(ax.get_ylim())),
    )
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xticks(range(len(Z_BINS)))
    ax.set_xticklabels(
        [f"{z_min:.1f}-{z_max:.1f}" for z_min, z_max in Z_BINS], rotation=45, ha="right"
    )
    ax.set_ylabel("Average Count", fontsize=AXIS_LABEL_SIZE)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "QuenchedCentralSatelliteCounts", output_format, verbose
    )
    return plot_path, None
