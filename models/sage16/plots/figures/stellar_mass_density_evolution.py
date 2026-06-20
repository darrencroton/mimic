#!/usr/bin/env python

"""
SAGE Stellar Mass Density Evolution Plot

This module generates a plot of the stellar mass density evolution from SAGE galaxy data.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, get_redshift_label, setup_legend
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_field_has_values,
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create a stellar mass density evolution plot.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with SAGE parameters
        output_dir: Output directory for the plot
        output_format: File format for the output

    verbose: Whether to print verbose output


    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check if we have any snapshots
    if len(snapshots) == 0:
        return None, "No snapshot data available for stellar mass density evolution plot"
    # Check required fields using first snapshot
    first_snap = next(iter(snapshots.values()))
    galaxies_sample = first_snap[0]

    success, optional, msg = check_required_fields(
        galaxies_sample, required_fields=["StellarMass"], plot_name="Stellar Mass Density Evolution"
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation: Check if StellarMass has any non-zero values
    has_stellar_mass, count, msg = check_field_has_values(
        galaxies_sample.StellarMass, "StellarMass", threshold=0.0
    )
    if not has_stellar_mass:
        return None, f"Field validation failed: {msg}"

    # Determine IMF type from params
    whichimf = 1  # Default to Chabrier
    if "WhichIMF" in params:
        whichimf = int(params["WhichIMF"])

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "stellar_mass_density_evolution", (0.0, 8.0), (6.5, 9.0)
    )

    # Observational data from various sources
    # SMD observations from Marchesini+ 2009, h=0.7
    # Values are (minz, maxz, rho, -err, +err)

    # Dickenson 2003
    dickenson2003 = np.array(
        [
            (0.6, 1.4, 8.26, 0.08, 0.08),
            (1.4, 2.0, 7.86, 0.22, 0.33),
            (2.0, 2.5, 7.58, 0.29, 0.54),
            (2.5, 3.0, 7.52, 0.51, 0.48),
        ],
        dtype=np.float32,
    )

    # Drory 2005
    drory2005 = np.array(
        [
            (0.25, 0.75, 8.3, 0.15, 0.15),
            (0.75, 1.25, 8.16, 0.15, 0.15),
            (1.25, 1.75, 8.0, 0.16, 0.16),
            (1.75, 2.25, 7.85, 0.2, 0.2),
            (2.25, 3.0, 7.75, 0.2, 0.2),
            (3.0, 4.0, 7.58, 0.2, 0.2),
        ],
        dtype=np.float32,
    )

    # Perez-Gonzalez (2008)
    pg2008 = np.array(
        [
            (0.2, 0.4, 8.41, 0.06, 0.06),
            (0.4, 0.6, 8.37, 0.04, 0.04),
            (0.6, 0.8, 8.32, 0.05, 0.05),
            (0.8, 1.0, 8.24, 0.05, 0.05),
            (1.0, 1.3, 8.15, 0.05, 0.05),
            (1.3, 1.6, 7.95, 0.07, 0.07),
            (1.6, 2.0, 7.82, 0.07, 0.07),
            (2.0, 2.5, 7.67, 0.08, 0.08),
            (2.5, 3.0, 7.56, 0.18, 0.18),
            (3.0, 3.5, 7.43, 0.14, 0.14),
            (3.5, 4.0, 7.29, 0.13, 0.13),
        ],
        dtype=np.float32,
    )

    # Glazebrook 2004
    glazebrook2004 = np.array(
        [
            (0.8, 1.1, 7.98, 0.14, 0.1),
            (1.1, 1.3, 7.62, 0.14, 0.11),
            (1.3, 1.6, 7.9, 0.14, 0.14),
            (1.6, 2.0, 7.49, 0.14, 0.12),
        ],
        dtype=np.float32,
    )

    # Fontana 2006
    fontana2006 = np.array(
        [
            (0.4, 0.6, 8.26, 0.03, 0.03),
            (0.6, 0.8, 8.17, 0.02, 0.02),
            (0.8, 1.0, 8.09, 0.03, 0.03),
            (1.0, 1.3, 7.98, 0.02, 0.02),
            (1.3, 1.6, 7.87, 0.05, 0.05),
            (1.6, 2.0, 7.74, 0.04, 0.04),
            (2.0, 3.0, 7.48, 0.04, 0.04),
            (3.0, 4.0, 7.07, 0.15, 0.11),
        ],
        dtype=np.float32,
    )

    # Rudnick 2006
    rudnick2006 = np.array(
        [
            (0.0, 1.0, 8.17, 0.27, 0.05),
            (1.0, 1.6, 7.99, 0.32, 0.05),
            (1.6, 2.4, 7.88, 0.34, 0.09),
            (2.4, 3.2, 7.71, 0.43, 0.08),
        ],
        dtype=np.float32,
    )

    # Elsner 2008
    elsner2008 = np.array(
        [
            (0.25, 0.75, 8.37, 0.03, 0.03),
            (0.75, 1.25, 8.17, 0.02, 0.02),
            (1.25, 1.75, 8.02, 0.03, 0.03),
            (1.75, 2.25, 7.9, 0.04, 0.04),
            (2.25, 3.0, 7.73, 0.04, 0.04),
            (3.0, 4.0, 7.39, 0.05, 0.05),
        ],
        dtype=np.float32,
    )

    # Combine all observations
    obs = [
        dickenson2003,
        drory2005,
        pg2008,
        glazebrook2004,
        fontana2006,
        rudnick2006,
        elsner2008,
    ]

    # Calculate stellar mass density for each snapshot BEFORE creating figure
    smd = []
    redshifts = []

    for snap, (galaxies, volume, metadata) in snapshots.items():
        # Get the redshift for this snapshot
        redshift = metadata.get("redshift", 0.0)
        redshifts.append(redshift)

        # Extract hubble_h from metadata
        hubble_h = metadata["hubble_h"]

        # Skip if volume is zero (no valid data)
        if volume == 0:
            smd.append(0.0)
            continue

        # Select galaxies with reasonable stellar masses
        w = np.where(
            (galaxies.StellarMass / hubble_h > 0.01) & (galaxies.StellarMass / hubble_h < 1000.0)
        )[0]

        if len(w) > 0:
            # Sum stellar masses and normalize by volume
            # Need to convert to solar masses and account for volume units
            stellar_mass_sum = np.sum(galaxies.StellarMass[w]) * 1.0e10 / hubble_h
            density = stellar_mass_sum / (volume / hubble_h**3)
            smd.append(np.log10(density))
        else:
            smd.append(0.0)

    # Convert to numpy arrays
    redshifts = np.array(redshifts)
    smd = np.array(smd)

    # Sort by redshift
    sort_idx = np.argsort(redshifts)
    redshifts = redshifts[sort_idx]
    smd = smd[sort_idx]

    # Debug information
    if verbose:
        print(f"  Number of snapshots: {len(snapshots)}")
        print(f"  Redshifts available: {redshifts}")
        print(f"  Stellar Mass Density values: {smd}")

    # Check if we have any nonzero stellar mass density values
    nonzero = np.where(smd > 0.0)[0]
    if len(nonzero) == 0:
        return None, "No nonzero stellar mass density values found across all snapshots"

    if verbose:
        print(f"  Plotting {len(nonzero)} nonzero Stellar Mass Density points")

    # NOW create the figure (only after all validation passed)
    fig, ax = setup_figure()

    # Plot all observations
    for o in obs:
        xval = ((o[:, 1] - o[:, 0]) / 2.0) + o[:, 0]
        if whichimf == 0:
            ax.errorbar(
                xval,
                np.log10(10 ** o[:, 2] * 1.6),
                xerr=(xval - o[:, 0], o[:, 1] - xval),
                yerr=(o[:, 3], o[:, 4]),
                alpha=0.3,
                lw=1.0,
                marker="o",
                ls="none",
            )
        elif whichimf == 1:
            ax.errorbar(
                xval,
                np.log10(10 ** o[:, 2] * 1.6 / 1.8),
                xerr=(xval - o[:, 0], o[:, 1] - xval),
                yerr=(o[:, 3], o[:, 4]),
                alpha=0.3,
                lw=1.0,
                marker="o",
                ls="none",
            )

    # Add a line for the legend
    ax.plot([], [], color="k", alpha=0.3, marker="o", ls="none", label="Observational data")

    # Plot the model results (nonzero was already validated above)
    ax.plot(redshifts[nonzero], smd[nonzero], "k-", lw=3.0, label="Model")

    # Customize the plot
    ax.set_ylabel(r"log$_{10}$ $\rho_{*}$ [M$_{\odot}$ Mpc$^{-3}$]", fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_redshift_label(), fontsize=AXIS_LABEL_SIZE)

    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.1))

    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    # Add consistently styled legend
    setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "Stellar_Mass_Density_Evolution", output_format, verbose
    )
    return plot_path, None
