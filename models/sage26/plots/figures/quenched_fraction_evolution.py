#!/usr/bin/env python

"""
SAGE26 Quenched Fraction Evolution Plot

Follows the QuenchedFraction.StellarMassFunctionEvolution figure of SAGE26's history plotting
script: the fraction of quiescent central galaxies (solid line, with a one-sigma band across the
snapshots of a bin) and of satellite galaxies (dotted line) against stellar mass, in the fifteen
redshift bins. Where it differs from SAGE26 the difference is stated below.

Requires: StellarMass, SfrDisk, SfrBulge, Type

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it: x 1e10 /
hubble_h), SfrDisk and SfrBulge (Msun/yr; the rate is their sum) and Type (0 central, 1 satellite,
so orphans, Type 2, are in neither curve). A galaxy is quiescent when its specific star formation
rate is below 10^sSFRcut per year, sSFRcut = -11.0.

Snapshots: those with a redshift in each bin of ``Z_BINS``, chosen through ``snapshots_in_z_range``;
snapshot 63 (z = 0) falls in none, as in SAGE26.

SAGE26 also computes the quenched fraction of all galaxies in each bin; only its commented-out line
draws it, so it is not computed here.

Differences from SAGE26: none beyond drawing the figure from the snapshots the run writes, where
SAGE26 reads all 64, so a bin may hold a single snapshot, and the one-sigma band then has zero
width.
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

# log10 of the specific star formation rate [1/yr] dividing quiescent from star-forming galaxies
# (SAGE26's `sSFRcut = -11.0`)
SSFR_CUT = -11.0

# Number of the last redshift bins that start their mass bins at 8.0
N_HIGH_Z_BINS = 4


def mass_bins_for(index):
    """
    Return the stellar-mass bin edges of redshift bin ``index``.

    As in SAGE26: `np.arange(8.0, 13.5, 0.1)` for the last four z bins (`i >= len(z_bins) - 4`),
    else `np.arange(7.2, 13.5, 0.1)`.
    """
    if index >= len(Z_BINS) - N_HIGH_Z_BINS:
        return np.arange(8.0, 13.5, 0.1)
    return np.arange(7.2, 13.5, 0.1)


def is_quenched(sfr_total, stellar_mass):
    """
    Return whether each galaxy is quiescent.

    As in SAGE26: `sSFR_linear = sfr_total / stellar_mass` and `quenched = sSFR_linear <
    10.0**sSFRcut`, with the stellar mass in Msun.
    """
    return (sfr_total / stellar_mass) < 10.0**SSFR_CUT


def quenched_fraction_in_bins(masses, quenched, selected, mass_bins):
    """
    Return the quenched fraction of the ``selected`` galaxies in each stellar-mass bin.

    As in SAGE26, for centrals and for satellites: a galaxy is in
    bin j when `masses >= mass_bins[j]` and `masses < mass_bins[j+1]`; the fraction is
    `np.sum(quenched[mask]) / np.sum(mask)` and NaN where the bin holds no selected galaxy.
    """
    fractions = np.full(len(mass_bins) - 1, np.nan)
    for j in range(len(fractions)):
        mask = (masses >= mass_bins[j]) & (masses < mass_bins[j + 1]) & selected
        if np.sum(mask) > 0:
            fractions[j] = np.sum(quenched[mask]) / np.sum(mask)
    return fractions


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the quenched fraction evolution plot.

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
        return None, "No snapshot data available for the quenched fraction evolution plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "SfrDisk", "SfrBulge", "Type"],
        plot_name="Quenched Fraction Evolution",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "quenched_fraction_evolution", (8.0, 12.5), (0.0, 0.8)
    )

    colors = plt.cm.plasma(np.linspace(0.1, 0.9, len(Z_BINS)))

    fig, ax = setup_figure()

    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        mass_bins = mass_bins_for(i)
        mass_centers = mass_bins[:-1] + 0.05

        central_snapshots = []
        satellite_snapshots = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            sfr_total = galaxies.SfrDisk + galaxies.SfrBulge
            stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
            galtype = galaxies.Type

            # Every galaxy with positive stellar mass
            w = np.where(stellar_mass > 0.0)[0]
            if len(w) > 0:
                masses = np.log10(stellar_mass[w])
                with np.errstate(divide="ignore", invalid="ignore"):
                    quenched = is_quenched(sfr_total[w], stellar_mass[w])
                gtype = galtype[w]
                # SAGE parity: SAGE26 takes centrals as `gtype == 0` and satellites as
                # `gtype == 1`, so orphans (Type 2) are in neither curve,
                # whereas the same script counts "orphans (Type 2) are satellites too" with `types
                # >= 1` in its counts figures. Mirrored as written.
                central_snapshots.append(
                    quenched_fraction_in_bins(masses, quenched, gtype == 0, mass_bins)
                )
                satellite_snapshots.append(
                    quenched_fraction_in_bins(masses, quenched, gtype == 1, mass_bins)
                )

        if len(central_snapshots) == 0:
            continue

        central_snapshots_arr = np.array(central_snapshots)
        satellite_snapshots_arr = np.array(satellite_snapshots)
        with warnings.catch_warnings():
            # SAGE26 silences these: an empty bin has no mean
            warnings.simplefilter("ignore", RuntimeWarning)
            cen_frac_mean = np.nanmean(central_snapshots_arr, axis=0)
            sat_frac_mean = np.nanmean(satellite_snapshots_arr, axis=0)
            cen_frac_std = np.nanstd(central_snapshots_arr, axis=0)
        valid_cen = ~np.isnan(cen_frac_mean)
        valid_sat = ~np.isnan(sat_frac_mean)

        label = f"{z_min:.1f} < z < {z_max:.1f}"
        ax.plot(
            mass_centers[valid_cen],
            cen_frac_mean[valid_cen],
            color=colors[i],
            linestyle="-",
            linewidth=2,
            label=label,
            gid="model",
        )
        ax.fill_between(
            mass_centers[valid_cen],
            (cen_frac_mean - cen_frac_std)[valid_cen],
            (cen_frac_mean + cen_frac_std)[valid_cen],
            color=colors[i],
            alpha=0.18,
            linewidth=0,
            gid="model",
        )
        ax.plot(
            mass_centers[valid_sat],
            sat_frac_mean[valid_sat],
            color=colors[i],
            linestyle=":",
            linewidth=2,
            alpha=0.25,
            gid="model",
        )  # SAGE parity: SAGE26 gives the satellite curve no label

    # SAGE parity: the y window stops at 0.8, clipping fractions above it. Mirrored as written.
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"$\log_{10} M_* [M_\odot]$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("Quenched Fraction", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))
    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc="upper left", frameon=False, ncol=3)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "QuenchedFraction.StellarMassFunctionEvolution", output_format, verbose
    )
    return plot_path, None
