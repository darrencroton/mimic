#!/usr/bin/env python

"""
SAGE26 H2 Fraction Evolution Plot

Port of the "Plotting H2 fraction vs stellar mass at different redshifts" section of SAGE26's
history plotting script (plotting/allresults-history.py:1611-1725, figure H2FractionEvolution):
the mean molecular fraction of the cold gas, M_H2 / (M_cold x 0.74), of galaxies against stellar
mass, in the fifteen redshift bins, with the standard error of the mean over the snapshots of a
bin as a shaded band. The section is the specification; where this port differs it is a
permitted deviation below.

Requires: StellarMass, ColdGas, H2gas

Fields read (units): StellarMass, ColdGas and H2gas (1e10 Msun/h each, converted to Msun as
SAGE26 converts them: x 1e10 / hubble_h, allresults-history.py:337, :357 and :345).

Snapshots: those with a redshift in each bin of ``Z_BINS`` (:1542-1546), chosen through
``snapshots_in_z_range``; snapshot 63 (z = 0) falls in none, as in SAGE26.

SAGE26 prints diagnostics of the first snapshots and of each bin (:1620-1632, :1672-1693); they
change nothing drawn and are not reproduced.

Permitted deviations: none beyond drawing the figure from the run's eight snapshots, where SAGE26
reads 64, so a bin holds at most one snapshot and its standard error is zero.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._binning import binned_mean_and_std, mean_and_error
from figures._snapshot_selection import Z_BINS, snapshots_in_z_range
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Hydrogen mass fraction of the cold gas (allresults-history.py:1616)
HYDROGEN_MASS_FRAC = 0.74


def mass_bins():
    """
    Return the stellar-mass bin edges, the same for every redshift bin.

    SAGE26 allresults-history.py:1617: `np.arange(7.2, 12.5, 0.1)`.
    """
    return np.arange(7.2, 12.5, 0.1)


def select_with_cold_gas(stellar_mass, cold_gas):
    """
    Return the indices of galaxies with positive stellar mass and positive cold gas.

    SAGE26 allresults-history.py:1654: `np.where((mstar > 0.0) & (cold > 0.0))[0]`.
    """
    return np.where((stellar_mass > 0.0) & (cold_gas > 0.0))[0]


def molecular_fraction(h2_gas, cold_gas):
    """
    Return the molecular fraction of the cold gas, clipped to 0 to 1.

    SAGE26 allresults-history.py:1660: `np.clip(h2[w] / (cold[w] * HYDROGEN_MASS_FRAC), 0.0,
    1.0)`.
    """
    return np.clip(h2_gas / (cold_gas * HYDROGEN_MASS_FRAC), 0.0, 1.0)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the H2 fraction evolution plot.

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
        return None, "No snapshot data available for the H2 fraction evolution plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "ColdGas", "H2gas"],
        plot_name="H2 Fraction Evolution",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "h2_fraction_evolution", (7.2, 12.0), (0.0, 1.05)
    )

    # allresults-history.py:1547 (viridis)
    colors = plt.cm.viridis(np.linspace(0.1, 0.9, len(Z_BINS)))
    bins = mass_bins()
    mass_centers = bins[:-1] + 0.05

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1635-1711
    for i, (z_min, z_max) in enumerate(Z_BINS):
        snap_indices = snapshots_in_z_range(snapshots, z_min, z_max)
        if len(snap_indices) == 0:
            continue

        h2_fraction_snapshots = []
        for snap in snap_indices:
            galaxies, _volume, metadata = snapshots[snap]
            hubble_h = metadata["hubble_h"]
            mstar = galaxies.StellarMass * 1.0e10 / hubble_h
            h2 = galaxies.H2gas * 1.0e10 / hubble_h
            cold = galaxies.ColdGas * 1.0e10 / hubble_h
            if not len(mstar) > 0:
                continue
            w = select_with_cold_gas(mstar, cold)
            if len(w) == 0:
                continue
            h2_frac_vals = molecular_fraction(h2[w], cold[w])
            masses = np.log10(mstar[w])
            h2_fraction_snapshots.append(binned_mean_and_std(masses, h2_frac_vals, bins))

        if len(h2_fraction_snapshots) == 0:
            continue

        h2_frac_mean, h2_frac_err, valid = mean_and_error(
            np.array([x[0] for x in h2_fraction_snapshots])
        )
        if not np.any(valid):
            continue

        label = f"{z_min:.1f} < z < {z_max:.1f}"
        ax.plot(
            mass_centers[valid],
            h2_frac_mean[valid],
            color=colors[i],
            linewidth=2,
            label=label,
            gid="model",
        )
        ax.fill_between(
            mass_centers[valid],
            h2_frac_mean[valid] - h2_frac_err[valid],
            h2_frac_mean[valid] + h2_frac_err[valid],
            color=colors[i],
            alpha=0.3,
            gid="model",
        )

    # allresults-history.py:1713-1719
    ax.axhline(1.0, color="k", linestyle="--", linewidth=1, alpha=0.5)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"$\log_{10} M_* [M_\odot]$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(
        r"$f_{\rm H_2} = M_{\rm H_2}\,/\,(M_{\rm cold} \times 0.74)$", fontsize=AXIS_LABEL_SIZE
    )
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))
    if ax.get_legend_handles_labels()[0]:
        # SAGE26 figure parity: allresults-history.py:1719 places the legend at upper left over
        # the curves. Mirrored as written.
        ax.legend(loc="upper left", frameon=False)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "H2FractionEvolution", output_format, verbose
    )
    return plot_path, None
