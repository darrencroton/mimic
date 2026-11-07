#!/usr/bin/env python

"""
SAGE26 Star Formation Rate Evolution Plot

Port of the "Plotting SFR evolution with redshift bins" section of SAGE26's history plotting
script (plotting/allresults-history.py:755-841, figure SFR_evolution): the mean star formation
rate of galaxies against stellar mass, in the fifteen redshift bins, with the standard error of
the mean over the snapshots of a bin as a shaded band. The section is the specification; where
this port differs it is a permitted deviation below.

Requires: StellarMass, SfrDisk, SfrBulge

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-history.py:337) and SfrDisk and SfrBulge (Msun/yr; the rate is
their sum, :799).

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:761-765), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26.

Permitted deviations: none beyond drawing the figure from the run's eight snapshots, where SAGE26
reads 64, so a bin holds at most one snapshot and its standard error is zero.
"""

import warnings

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._snapshot_selection import Z_BINS, snapshots_in_z_range
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Number of the last redshift bins that start their mass bins at 8.0 (allresults-history.py:788)
N_HIGH_Z_BINS = 4


def mass_bins_for(index):
    """
    Return the stellar-mass bin edges of redshift bin ``index``.

    SAGE26 allresults-history.py:788-791: `np.arange(8.0, 12.5, 0.1)` for the last four z bins
    (`i >= len(z_bins) - 4`), else `np.arange(7.2, 12.5, 0.1)`.
    """
    if index >= len(Z_BINS) - N_HIGH_Z_BINS:
        return np.arange(8.0, 12.5, 0.1)
    return np.arange(7.2, 12.5, 0.1)


def select_star_forming(stellar_mass, sfr_total):
    """
    Return the indices of galaxies with positive stellar mass and positive star formation rate.

    SAGE26 allresults-history.py:800: `np.where((StellarMassFull > 0.0) & (sfr_total > 0.0))[0]`.
    """
    return np.where((stellar_mass > 0.0) & (sfr_total > 0.0))[0]


def binned_mean_and_std(masses, values, mass_bins):
    """
    Return the mean and standard deviation of ``values`` in each stellar-mass bin.

    SAGE26 allresults-history.py:805-813: bins hold NaN until a bin has a galaxy, and a galaxy is
    in bin j when `masses >= mass_bins[j]` and `masses < mass_bins[j+1]`.
    """
    n_bins = len(mass_bins) - 1
    means = np.full(n_bins, np.nan)
    stds = np.full(n_bins, np.nan)
    for j in range(n_bins):
        mask = (masses >= mass_bins[j]) & (masses < mass_bins[j + 1])
        if np.sum(mask) > 0:
            means[j] = np.mean(values[mask])
            stds[j] = np.std(values[mask])
    return means, stds


def mean_and_error(means_stack):
    """
    Return the mean over snapshots, its standard error, and the bins where both are defined.

    SAGE26 allresults-history.py:820-822: `np.nanmean(stack, axis=0)`,
    `np.nanstd(stack, axis=0) / np.sqrt(len(stack))` and `~np.isnan(mean) & ~np.isnan(err)`.
    """
    with warnings.catch_warnings():
        # SAGE26 silences these (allresults-history.py:13-14): an empty bin has no mean
        warnings.simplefilter("ignore", RuntimeWarning)
        mean = np.nanmean(means_stack, axis=0)
        err = np.nanstd(means_stack, axis=0) / np.sqrt(len(means_stack))
    valid = ~np.isnan(mean) & ~np.isnan(err)
    return mean, err, valid


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the star formation rate evolution plot.

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
        return None, "No snapshot data available for the SFR evolution plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "SfrDisk", "SfrBulge"],
        plot_name="SFR Evolution",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    x_min, x_max, y_min, y_max = get_profile_axes(params, "sfr_evolution", (8.0, 12.2), (-4.0, 3.0))

    # allresults-history.py:768 (plasma, dark to light)
    colors = plt.cm.plasma(np.linspace(0.1, 0.9, len(Z_BINS)))

    fig, ax = setup_figure(figsize=(10, 8))

    # allresults-history.py:782-827
    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        mass_bins = mass_bins_for(i)
        mass_centers = mass_bins[:-1] + 0.05

        sfr_snapshots = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
            sfr_total = galaxies.SfrDisk + galaxies.SfrBulge
            w = select_star_forming(stellar_mass, sfr_total)
            if len(w) > 0:
                masses = np.log10(stellar_mass[w])
                sfrs = np.log10(sfr_total[w])
                sfr_snapshots.append(binned_mean_and_std(masses, sfrs, mass_bins))
        if len(sfr_snapshots) == 0:
            continue

        sfr_mean, sfr_err, valid = mean_and_error(np.array([x[0] for x in sfr_snapshots]))
        if not np.any(valid):
            continue

        label = f"{z_min:.1f} < z < {z_max:.1f}"
        ax.plot(
            mass_centers[valid],
            sfr_mean[valid],
            color=colors[i],
            linewidth=2,
            label=label,
            gid="model",
        )
        ax.fill_between(
            mass_centers[valid],
            sfr_mean[valid] - sfr_err[valid],
            sfr_mean[valid] + sfr_err[valid],
            color=colors[i],
            alpha=0.3,
            gid="model",
        )

    # allresults-history.py:829-836
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"$\log_{10} M_* [M_\odot]$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\log_{{10}}\mathrm{{SFR}}\ [M_\odot/yr]$", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))
    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc="lower right", frameon=False, ncol=3)

    fig.tight_layout()
    plot_path = save_and_close_figure(fig, output_dir, "SFR_evolution", output_format, verbose)
    return plot_path, None
