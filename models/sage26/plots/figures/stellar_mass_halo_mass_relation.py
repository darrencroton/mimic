#!/usr/bin/env python

"""
SAGE26 Stellar Mass - Halo Mass Relation Evolution Plot

Port of the "Plotting stellar mass-halo mass relation evolution" section of SAGE26's history
plotting script (plotting/allresults-history.py:846-958, figure StellarMassHaloMassRelation):
the mean stellar mass of galaxies against the mass of their halo, in the fifteen redshift bins,
with the standard error of the mean over the snapshots of a bin as a shaded band. The section is
the specification; where this port differs it is a permitted deviation below.

Requires: StellarMass, Mvir

Fields read (units): StellarMass and Mvir (1e10 Msun/h each, converted to Msun as SAGE26
converts them: x 1e10 / hubble_h, allresults-history.py:337 and :342). Every galaxy row is read,
with no Type cut, as in SAGE26.

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:852-856), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26.

Permitted deviations (c16 Slice 3, row `stellar_mass_halo_mass_relation`): none beyond the rules
(the figure is drawn from the run's eight snapshots, where SAGE26 reads 64, so a bin holds at
most one snapshot and its standard error is zero).
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

# Halo-mass bins (allresults-history.py:863, `np.arange(10.0, 15.0, 0.1)`)
HALO_MASS_BINS = np.arange(10.0, 15.0, 0.1)

# Line styles, cycled over the redshift bins (:860)
LINE_STYLES = ["-", ":", "--", "-."] * 4


def select_with_halo(stellar_mass, halo_mass):
    """
    Return the indices of galaxies with positive stellar mass and positive halo mass.

    SAGE26 allresults-history.py:889-890:
    `np.where((StellarMassFull > 0.0) & (HaloMassFull > 0.0))[0]`.
    """
    return np.where((stellar_mass > 0.0) & (halo_mass > 0.0))[0]


def mean_stellar_mass_in_halo_bins(stellar_masses, halo_masses, halo_mass_bins):
    """
    Return the mean stellar mass (linear) in each halo-mass bin; NaN for an empty bin.

    SAGE26 allresults-history.py:897-903: a galaxy is in bin j when `halo_masses >=
    halo_mass_bins[j]` and `halo_masses < halo_mass_bins[j+1]`, with halo_masses in log10.
    """
    means = np.full(len(halo_mass_bins) - 1, np.nan)
    for j in range(len(means)):
        bin_mask = (halo_masses >= halo_mass_bins[j]) & (halo_masses < halo_mass_bins[j + 1])
        if np.sum(bin_mask) > 0:
            means[j] = np.mean(stellar_masses[bin_mask])
    return means


def mean_and_error(binned):
    """
    Return the mean over snapshots, its standard error, and the bins where both are defined.

    SAGE26 allresults-history.py:914-918: `np.nanmean(stack, axis=0)`,
    `np.nanstd(stack, axis=0) / np.sqrt(len(stack))` and `~np.isnan(mean) & ~np.isnan(err)`.
    """
    with warnings.catch_warnings():
        # SAGE26 silences these (allresults-history.py:13-14): an empty bin has no mean
        warnings.simplefilter("ignore", RuntimeWarning)
        mean = np.nanmean(binned, axis=0)
        err = np.nanstd(binned, axis=0) / np.sqrt(len(binned))
    valid = ~np.isnan(mean) & ~np.isnan(err)
    return mean, err, valid


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the stellar mass - halo mass relation evolution plot.

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
        return None, "No snapshot data available for the stellar mass-halo mass plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "Mvir"],
        plot_name="Stellar Mass Halo Mass Relation",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "stellar_mass_halo_mass_relation", (10.0, 15.0), (7.0, 12.0)
    )

    # allresults-history.py:859 (plasma, dark to light)
    colors = plt.cm.plasma(np.linspace(0.1, 0.9, len(Z_BINS)))
    halo_mass_centers = HALO_MASS_BINS[:-1] + 0.05

    fig, ax = setup_figure()

    # allresults-history.py:876-931
    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        stellar_mass_binned = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            stellar_mass_all = galaxies.StellarMass * 1.0e10 / hubble_h
            halo_mass_all = galaxies.Mvir * 1.0e10 / hubble_h
            w = select_with_halo(stellar_mass_all, halo_mass_all)
            if len(w) > 0:
                stellar_masses = stellar_mass_all[w]
                halo_masses = np.log10(halo_mass_all[w])
                stellar_mass_binned.append(
                    mean_stellar_mass_in_halo_bins(stellar_masses, halo_masses, HALO_MASS_BINS)
                )
        if len(stellar_mass_binned) == 0:
            continue

        stellar_mass_mean, stellar_mass_err, valid = mean_and_error(np.array(stellar_mass_binned))
        if not np.any(valid):
            continue

        label = f"{z_min:.1f} < z < {z_max:.1f}"
        # A mean below its error has a non-positive lower edge; its logarithm is NaN, as in SAGE26
        with np.errstate(divide="ignore", invalid="ignore"):
            ax.plot(
                halo_mass_centers[valid],
                np.log10(stellar_mass_mean[valid]),
                color=colors[i],
                linewidth=2,
                linestyle=LINE_STYLES[i],
                label=label,
                gid="model",
            )
            ax.fill_between(
                halo_mass_centers[valid],
                np.log10(stellar_mass_mean[valid] - stellar_mass_err[valid]),
                np.log10(stellar_mass_mean[valid] + stellar_mass_err[valid]),
                color=colors[i],
                alpha=0.3,
                gid="model",
            )

    # allresults-history.py:936-949
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"$\log_{10} M_{\rm vir}$ [$M_\odot$]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\log_{10} M_*$ [$M_\odot$]", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))
    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc="lower right", frameon=False, ncol=2)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "StellarMassHaloMassRelation", output_format, verbose
    )
    return plot_path, None
