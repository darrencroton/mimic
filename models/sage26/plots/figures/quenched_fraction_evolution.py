#!/usr/bin/env python

"""
SAGE26 Quenched Fraction Evolution Plot

Port of the "Quenched fraction as a function of StellarMass across the same redshift bins
(centrals/satellites separated)" section of SAGE26's history plotting script
(plotting/allresults-history.py:1252-1358, figure QuenchedFraction.StellarMassFunctionEvolution):
the fraction of quiescent central galaxies (solid line, with a one-sigma band across the
snapshots of a bin) and of satellite galaxies (dotted line) against stellar mass, in the fifteen
redshift bins. The section is the specification; where this port differs it is a permitted
deviation below.

Requires: StellarMass, SfrDisk, SfrBulge, Type

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-history.py:337), SfrDisk and SfrBulge (Msun/yr; the rate is their
sum, :1273) and Type (0 central, 1 satellite; :1297, :1307, so orphans, Type 2, are in neither
curve). A galaxy is quiescent when its specific star formation rate is below 10^sSFRcut per
year, sSFRcut = -11.0 (:23, :1286).

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:1257), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26.

SAGE26 also computes the quenched fraction of all galaxies in each bin (:1289-1294, :1322,
:1328); only its commented-out line draws it (:1333), so it is not computed here.

Permitted deviations (c16 Slice 3, row `quenched_fraction_evolution`): none beyond the rules (the
figure is drawn from the run's eight snapshots, where SAGE26 reads 64, so a bin holds at most one
snapshot and the one-sigma band has zero width).
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
# (allresults-history.py:23, `sSFRcut = -11.0`)
SSFR_CUT = -11.0

# Number of the last redshift bins that start their mass bins at 8.0 (allresults-history.py:1262)
N_HIGH_Z_BINS = 4


def mass_bins_for(index):
    """
    Return the stellar-mass bin edges of redshift bin ``index``.

    SAGE26 allresults-history.py:1262-1265: `np.arange(8.0, 13.5, 0.1)` for the last four z bins
    (`i >= len(z_bins) - 4`), else `np.arange(7.2, 13.5, 0.1)`.
    """
    if index >= len(Z_BINS) - N_HIGH_Z_BINS:
        return np.arange(8.0, 13.5, 0.1)
    return np.arange(7.2, 13.5, 0.1)


def is_quenched(sfr_total, stellar_mass):
    """
    Return whether each galaxy is quiescent.

    SAGE26 allresults-history.py:1282 and :1286: `sSFR_linear = sfr_total / stellar_mass` and
    `quenched = sSFR_linear < 10.0**sSFRcut`, with the stellar mass in Msun.
    """
    return (sfr_total / stellar_mass) < 10.0**SSFR_CUT


def quenched_fraction_in_bins(masses, quenched, selected, mass_bins):
    """
    Return the quenched fraction of the ``selected`` galaxies in each stellar-mass bin.

    SAGE26 allresults-history.py:1298-1303 (centrals) and :1308-1313 (satellites): a galaxy is in
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

    # allresults-history.py:1257-1344
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

            # Every galaxy with positive stellar mass (:1278)
            w = np.where(stellar_mass > 0.0)[0]
            if len(w) > 0:
                masses = np.log10(stellar_mass[w])
                with np.errstate(divide="ignore", invalid="ignore"):
                    quenched = is_quenched(sfr_total[w], stellar_mass[w])
                gtype = galtype[w]
                # SAGE26 figure parity: allresults-history.py:1297 and :1307 take centrals as
                # `gtype == 0` and satellites as `gtype == 1`, so orphans (Type 2) are in neither
                # curve, whereas the same script counts "orphans (Type 2) are satellites too" with
                # `types >= 1` in its counts figures (:1828, :1871, :1948, :2009). Mirrored as
                # written.
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
            # SAGE26 silences these (allresults-history.py:13-14): an empty bin has no mean
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
        )

    # allresults-history.py:1346-1354
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
