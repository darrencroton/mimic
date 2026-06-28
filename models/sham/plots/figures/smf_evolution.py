#!/usr/bin/env python

"""
Mimic Stellar Mass Function Evolution Plot

This module generates a stellar mass function evolution plot from Mimic galaxy data.
Requires: StellarMass property (from galaxy physics modules)
"""

import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    get_mass_function_labels,
    get_stellar_mass_label,
    setup_legend,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_field_has_values,
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
    validate_evolution_snapshot,
    warn,
)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create a stellar mass function evolution plot.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with Mimic parameters
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
        return None, "No snapshot data available for SMF evolution plot"

    # Check required fields using first snapshot
    first_snap = next(iter(snapshots.values()))
    galaxies_sample = first_snap[0]

    success, optional, msg = check_required_fields(
        galaxies_sample,
        required_fields=["StellarMass"],
        plot_name="Stellar Mass Function Evolution",
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation: Check if StellarMass has any non-zero values
    has_mass, count, msg = check_field_has_values(
        galaxies_sample.StellarMass, "StellarMass", threshold=0.0
    )
    if not has_mass:
        return None, f"Field validation failed: {msg}"

    # Define target redshifts and their tolerances
    target_redshifts = [0.0, 1.3, 2.0, 3.0]
    target_tolerance = [0.2, 0.5, 0.5, 0.5]

    # Determine IMF type from params
    whichimf = 1  # Default to Chabrier
    if "WhichIMF" in params:
        whichimf = int(params["WhichIMF"])

    mass_min, mass_max, y_min, y_max = get_profile_axes(
        params, "stellar_mass_function", (8.0, 12.5), (1.0e-6, 1.0e-1), log_y=True
    )

    # Set up binning
    binwidth = 0.1

    # Debug information
    if verbose:
        print(f"  Number of snapshots: {len(snapshots)}")
        for snap, (galaxies, volume, metadata) in snapshots.items():
            print(
                f"  Snapshot {snap}: {len(galaxies)} galaxies, z={metadata.get('redshift', 'unknown')}"
            )

    # Sort snapshots by redshift and filter to target redshifts
    sorted_snapshots = [
        (snap, galaxies, volume, metadata)
        for snap, (galaxies, volume, metadata) in snapshots.items()
    ]
    # Sort by redshift
    sorted_snapshots.sort(key=lambda x: x[3]["redshift"])

    # Filter snapshots to match target redshifts
    target_snapshots = []

    # For each target redshift, find the closest snapshot within tolerance
    for i, target_z in enumerate(target_redshifts):
        tolerance = target_tolerance[i]
        # Filter snapshots with z >= target_z
        candidates = [s for s in sorted_snapshots if s[3]["redshift"] >= target_z]
        if candidates:
            # Find the closest one
            closest = min(candidates, key=lambda x: abs(x[3]["redshift"] - target_z))
            # Check if it's within tolerance
            if abs(closest[3]["redshift"] - target_z) <= tolerance:
                target_snapshots.append(closest)
                if verbose:
                    print(
                        f"Target z={target_z:.1f}: Using snapshot with z={closest[3]['redshift']:.3f}"
                    )
        elif verbose:
            warn(f"Target z={target_z:.1f}: No suitable snapshot found")

    # Colors for different redshifts
    colors = ["k", "b", "g", "r", "m", "y", "c", "orange"]

    # Check if we have any snapshots to plot
    if len(target_snapshots) == 0:
        return None, "No snapshots found matching target redshifts for SMF evolution plot"

    # NOW create the figure (only after all validation passed)
    fig, ax = setup_figure()

    # Add Marchesini et al. 2009 observational data (z=[0.1])
    M = np.arange(7.0, 11.8, 0.01)
    Mstar = np.log10(10.0**10.96)
    alpha = -1.18
    phistar = 30.87 * 1e-4
    xval = 10.0 ** (M - Mstar)
    yval = np.log(10.0) * phistar * xval ** (alpha + 1) * np.exp(-xval)

    if whichimf == 0:
        ax.plot(
            np.log10(10.0**M * 1.6),
            yval,
            ":",
            lw=10,
            alpha=0.5,
            label="Marchesini et al. 2009 z=[0.1]",
        )
    elif whichimf == 1:
        ax.plot(
            np.log10(10.0**M * 1.6 / 1.8),
            yval,
            ":",
            lw=10,
            alpha=0.5,
            label="Marchesini et al. 2009 z=[0.1]",
        )

    # Add Marchesini et al. 2009 observational data (z=[1.3,2.0])
    M = np.arange(9.3, 11.8, 0.01)
    Mstar = np.log10(10.0**10.91)
    alpha = -0.99
    phistar = 10.17 * 1e-4
    xval = 10.0 ** (M - Mstar)
    yval = np.log(10.0) * phistar * xval ** (alpha + 1) * np.exp(-xval)

    if whichimf == 0:
        ax.plot(
            np.log10(10.0**M * 1.6),
            yval,
            "b:",
            lw=10,
            alpha=0.5,
            label="... z=[1.3,2.0]",
        )
    elif whichimf == 1:
        ax.plot(
            np.log10(10.0**M * 1.6 / 1.8),
            yval,
            "b:",
            lw=10,
            alpha=0.5,
            label="... z=[1.3,2.0]",
        )

    # Add Marchesini et al. 2009 observational data (z=[2.0,3.0])
    M = np.arange(9.7, 11.8, 0.01)
    Mstar = np.log10(10.0**10.96)
    alpha = -1.01
    phistar = 3.95 * 1e-4
    xval = 10.0 ** (M - Mstar)
    yval = np.log(10.0) * phistar * xval ** (alpha + 1) * np.exp(-xval)

    if whichimf == 0:
        ax.plot(
            np.log10(10.0**M * 1.6),
            yval,
            "g:",
            lw=10,
            alpha=0.5,
            label="... z=[2.0,3.0]",
        )
    elif whichimf == 1:
        ax.plot(
            np.log10(10.0**M * 1.6 / 1.8),
            yval,
            "g:",
            lw=10,
            alpha=0.5,
            label="... z=[2.0,3.0]",
        )

    # Add Marchesini et al. 2009 observational data (z=[3.0,4.0])
    M = np.arange(10.0, 11.8, 0.01)
    Mstar = np.log10(10.0**11.38)
    alpha = -1.39
    phistar = 0.53 * 1e-4
    xval = 10.0 ** (M - Mstar)
    yval = np.log(10.0) * phistar * xval ** (alpha + 1) * np.exp(-xval)

    if whichimf == 0:
        ax.plot(
            np.log10(10.0**M * 1.6),
            yval,
            "r:",
            lw=10,
            alpha=0.5,
            label="... z=[3.0,4.0]",
        )
    elif whichimf == 1:
        ax.plot(
            np.log10(10.0**M * 1.6 / 1.8),
            yval,
            "r:",
            lw=10,
            alpha=0.5,
            label="... z=[3.0,4.0]",
        )

    # Plot model SMFs at target redshifts
    for i, (snap, galaxies, volume, metadata) in enumerate(target_snapshots):
        hubble_h = metadata["hubble_h"]
        redshift = metadata["redshift"]
        color = colors[i % len(colors)]

        # Debug output - only show if verbose is enabled
        if verbose:
            print(f"Processing SMF for snapshot {snap}, z={redshift:.1f}")

        # Select all galaxies with valid stellar mass
        w = np.where(galaxies.StellarMass > 0.0)[0]

        # Validate this snapshot - skip if no galaxies found
        is_valid, skip_msg = validate_evolution_snapshot(w, redshift, "SMF Evolution", verbose)
        if not is_valid:
            continue

        mass = np.log10(galaxies.StellarMass[w] * 1.0e10 / hubble_h)

        # Set up histogram bins
        mi = np.floor(min(mass)) - 1
        ma = np.floor(max(mass)) + 1

        # Force reasonable limits for stellar masses
        mi = max(mi, mass_min)
        ma = min(ma, mass_max)

        nbins = int((ma - mi) / binwidth)

        # Calculate histogram
        counts, binedges = np.histogram(mass, range=(mi, ma), bins=nbins)
        xaxis = binedges[:-1] + 0.5 * binwidth

        # Plot the histogram
        ax.plot(xaxis, counts / volume * hubble_h**3 / binwidth, color=color, linestyle="-", lw=2)

        # Store redshift values for labels in the top right corner
        if i == 0:
            # Initialize list of redshifts for the legend
            redshift_labels = []
        # Add this redshift to the list
        redshift_labels.append((redshift, color))

    # Customize the plot
    ax.set_yscale("log")
    ax.set_xlim(mass_min, mass_max)
    ax.set_ylim(y_min, y_max)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))

    ax.set_ylabel(get_mass_function_labels(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_stellar_mass_label(), fontsize=AXIS_LABEL_SIZE)

    # Add redshift labels in the top right corner
    if "redshift_labels" in locals():
        # Sort labels by redshift
        redshift_labels.sort(key=lambda x: x[0])
        # Position for the first label
        x_pos = 11.8
        y_pos = 6e-2  # Near the top of the plot (log scale)
        for z, color in redshift_labels:
            ax.text(
                x_pos,
                y_pos,
                f"z = {z:.1f}",
                color=color,
                fontsize=IN_FIGURE_TEXT_SIZE,
                ha="right",
                va="top",
            )
            # Move down for the next label
            y_pos *= 0.6  # Reduces y-position by 40% each time (works well with log scale)

    # Add consistently styled legend
    setup_legend(ax, loc="lower left")

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "StellarMassFunction_Evolution", output_format, verbose
    )
    return plot_path, None
