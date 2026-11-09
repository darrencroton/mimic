#!/usr/bin/env python

"""
SAGE26 Metallicity Evolution Plot

Follows the MetallicityEvolution figure of SAGE26's history plotting script: the mean of log10 of
the stellar metal mass of galaxies against stellar mass, in the fifteen redshift bins, with the
standard error of the mean over the snapshots of a bin as a shaded band. Where it differs from
SAGE26 the difference is stated below.

Requires: StellarMass, MetalsStellarMass

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it: x 1e10 /
hubble_h) and MetalsStellarMass (1e10 Msun/h, read as it is, with no conversion).

Snapshots: those with a redshift in each bin of ``Z_BINS``, chosen through ``snapshots_in_z_range``;
snapshot 63 (z = 0) falls in none, as in SAGE26.

SAGE26 sets no axis window (its limits are commented out), so the figure sets none.

Differences from SAGE26: none beyond drawing the figure from the snapshots the run writes, where
SAGE26 reads all 64, so a bin may hold a single snapshot, and its standard error is then zero.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._binning import binned_mean_and_std, mean_and_error
from figures._snapshot_selection import Z_BINS, snapshots_in_z_range
from matplotlib.ticker import MultipleLocator
from output_utils import check_required_fields, save_and_close_figure, setup_figure


def mass_bins():
    """
    Return the stellar-mass bin edges, the same for every redshift bin.

    As in SAGE26: `np.arange(7.2, 12.5, 0.1)`.
    """
    return np.arange(7.2, 12.5, 0.1)


def select_with_metals(stellar_mass, metals):
    """
    Return the indices of galaxies with positive stellar mass and positive stellar metal mass.

    As in SAGE26: `np.where((StellarMassFull > 0.0) & (MetallicityFull > 0.0))[0]`.
    """
    return np.where((stellar_mass > 0.0) & (metals > 0.0))[0]


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

    # Viridis
    colors = plt.cm.viridis(np.linspace(0.1, 0.9, len(Z_BINS)))
    bins = mass_bins()
    mass_centers = bins[:-1] + 0.05

    fig, ax = setup_figure(figsize=(10, 7))

    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        metallicity_snapshots = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
            # SAGE parity: SAGE26 reads MetalsStellarMass, the stellar metal mass in 1e10 Msun/h,
            # with no x 1e10 / hubble_h and no division by the stellar mass, yet the y axis is
            # labelled log10 Z, a metallicity. Mirrored as written.
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
