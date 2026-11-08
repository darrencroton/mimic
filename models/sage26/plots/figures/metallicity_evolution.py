#!/usr/bin/env python

"""
SAGE26 Metallicity Evolution Plot

Port of the "Plotting metallicity vs stellar mass at different redshifts" section of SAGE26's
history plotting script (plotting/allresults-history.py:1537-1605, figure MetallicityEvolution):
the mean of log10 of the stellar metal mass of galaxies against stellar mass, in the fifteen
redshift bins, with the standard error of the mean over the snapshots of a bin as a shaded band.
The section is the specification; where this port differs it is a permitted deviation below.

Requires: StellarMass, MetalsStellarMass

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-history.py:337) and MetalsStellarMass (1e10 Msun/h, read as it is,
with no conversion: :365).

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:1542-1546), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26.

SAGE26 sets no axis window (its limits are commented out, :1594-1595), so the port sets none.

Permitted deviations: none beyond drawing the figure from the run's eight snapshots, where SAGE26
reads 64, so a bin holds at most one snapshot and its standard error is zero.
"""

import warnings

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._snapshot_selection import Z_BINS, snapshots_in_z_range
from matplotlib.ticker import MultipleLocator
from output_utils import check_required_fields, save_and_close_figure, setup_figure


def mass_bins():
    """
    Return the stellar-mass bin edges, the same for every redshift bin.

    SAGE26 allresults-history.py:1554: `np.arange(7.2, 12.5, 0.1)`.
    """
    return np.arange(7.2, 12.5, 0.1)


def select_with_metals(stellar_mass, metals):
    """
    Return the indices of galaxies with positive stellar mass and positive stellar metal mass.

    SAGE26 allresults-history.py:1561:
    `np.where((StellarMassFull > 0.0) & (MetallicityFull > 0.0))[0]`.
    """
    return np.where((stellar_mass > 0.0) & (metals > 0.0))[0]


def binned_mean_and_std(masses, values, bins):
    """
    Return the mean and standard deviation of ``values`` in each stellar-mass bin.

    SAGE26 allresults-history.py:1565-1573: bins hold NaN until a bin has a galaxy, and a galaxy
    is in bin j when `masses >= mass_bins[j]` and `masses < mass_bins[j+1]`.
    """
    n_bins = len(bins) - 1
    means = np.full(n_bins, np.nan)
    stds = np.full(n_bins, np.nan)
    for j in range(n_bins):
        mask = (masses >= bins[j]) & (masses < bins[j + 1])
        if np.sum(mask) > 0:
            means[j] = np.mean(values[mask])
            stds[j] = np.std(values[mask])
    return means, stds


def mean_and_error(means_stack):
    """
    Return the mean over snapshots, its standard error, and the bins where both are defined.

    SAGE26 allresults-history.py:1581-1583: `np.nanmean(stack, axis=0)`,
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
    Create the metallicity evolution plot.

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
        return None, "No snapshot data available for the metallicity evolution plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "MetalsStellarMass"],
        plot_name="Metallicity Evolution",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # allresults-history.py:1547 (viridis)
    colors = plt.cm.viridis(np.linspace(0.1, 0.9, len(Z_BINS)))
    bins = mass_bins()
    mass_centers = bins[:-1] + 0.05

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1549-1592
    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        metallicity_snapshots = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
            # SAGE26 figure parity: allresults-history.py:365 reads MetalsStellarMass, the
            # stellar metal mass in 1e10 Msun/h, with no x 1e10 / hubble_h and no division by the
            # stellar mass, yet the y axis (:1597) is labelled log10 Z, a metallicity. Mirrored as
            # written.
            metals = galaxies.MetalsStellarMass
            w = select_with_metals(stellar_mass, metals)
            if len(w) > 0:
                masses = np.log10(stellar_mass[w])
                metallicities = np.log10(metals[w])
                metallicity_snapshots.append(binned_mean_and_std(masses, metallicities, bins))

        if len(metallicity_snapshots) == 0:
            continue

        metallicity_mean, metallicity_err, valid = mean_and_error(
            np.array([x[0] for x in metallicity_snapshots])
        )
        if not np.any(valid):
            continue

        label = f"{z_min:.1f} < z < {z_max:.1f}"
        ax.plot(
            mass_centers[valid],
            metallicity_mean[valid],
            color=colors[i],
            linewidth=2,
            label=label,
            gid="model",
        )
        ax.fill_between(
            mass_centers[valid],
            metallicity_mean[valid] - metallicity_err[valid],
            metallicity_mean[valid] + metallicity_err[valid],
            color=colors[i],
            alpha=0.3,
            gid="model",
        )

    # allresults-history.py:1596-1599
    ax.set_xlabel(r"$\log_{10} M_* [M_\odot]$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\log_{10} Z$", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))
    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc="upper left", frameon=False)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "MetallicityEvolution", output_format, verbose
    )
    return plot_path, None
