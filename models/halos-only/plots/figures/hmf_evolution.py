#!/usr/bin/env python

"""
Mimic Halo Mass Function Evolution Plot

This module generates a halo mass function evolution plot from Mimic halo data.
"""

# Third-party packages
import numpy as np

# Local application imports
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    get_halo_mass_label,
    get_mass_function_labels,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    calculate_mass_function,
    check_field_has_values,
    check_required_fields,
    save_and_close_figure,
    setup_figure,
    validate_evolution_snapshot,
    warn,
)

# Target redshifts for evolution plots
TARGET_REDSHIFTS = [0.0, 1.0, 2.0, 3.0]
TARGET_TOLERANCE = [0.2, 0.5, 0.5, 0.5]  # Tolerance for each target redshift

# Physical limits for halo mass functions
HALO_MASS_MIN = 10.0  # log10(Msun) - below resolution limit
HALO_MASS_MAX = 16.0  # log10(Msun) - above cluster scale
BINWIDTH_DEX = 0.1  # Standard bin width in dex
PLOT_XLIM = (10.0, 15.0)  # Plot x-axis limits
PLOT_YLIM = (1.0e-6, 1.0e-1)  # Plot y-axis limits

# Standard evolution plot colors (consistent across all evolution plots)
EVOLUTION_COLORS = ["k", "b", "g", "r", "m", "y", "c", "orange"]


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create a halo mass function evolution plot.

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
        return None, "No snapshot data available for HMF evolution plot"

    # Check required fields using first snapshot
    first_snap = next(iter(snapshots.values()))
    galaxies_sample = first_snap[0]

    success, optional, msg = check_required_fields(
        galaxies_sample, required_fields=["Mvir"], plot_name="Halo Mass Function Evolution"
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation: Check if Mvir has any non-zero values
    has_mvir, count, msg = check_field_has_values(galaxies_sample.Mvir, "Mvir", threshold=0.0)
    if not has_mvir:
        return None, f"Field validation failed: {msg}"

    # Debug information
    if verbose:
        print(f"  Number of snapshots: {len(snapshots)}")
        for snap, (galaxies, volume, metadata) in snapshots.items():
            print(
                f"  Snapshot {snap}: {len(galaxies)} halos, z={metadata.get('redshift', 'unknown')}"
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
    for i, target_z in enumerate(TARGET_REDSHIFTS):
        tolerance = TARGET_TOLERANCE[i]
        # Filter snapshots with z >= target_z
        candidates = [s for s in sorted_snapshots if s[3]["redshift"] >= target_z]
        if candidates:
            # Find the closest one
            closest = min(candidates, key=lambda x: abs(x[3]["redshift"] - target_z))
            # Check if it's within tolerance
            if abs(closest[3]["redshift"] - target_z) <= tolerance:
                target_snapshots.append(closest)
                if verbose:
                    warn(
                        f"Target z={target_z:.1f}: Using snapshot with z={closest[3]['redshift']:.3f}"
                    )
        elif verbose:
            warn(f"Target z={target_z:.1f}: No suitable snapshot found")

    # Check if we have any snapshots to plot
    if len(target_snapshots) == 0:
        return None, "No snapshots found matching target redshifts for HMF evolution plot"

    # NOW create the figure (only after all validation passed)
    fig, ax = setup_figure()

    # Plot model HMFs at target redshifts
    for i, (snap, galaxies, volume, metadata) in enumerate(target_snapshots):
        hubble_h = metadata["hubble_h"]
        redshift = metadata["redshift"]
        color = EVOLUTION_COLORS[i % len(EVOLUTION_COLORS)]

        # Debug output - only show if verbose is enabled
        if verbose:
            print(f"Processing HMF for snapshot {snap}, z={redshift:.1f}")

        # Select halos (Type 0 = central galaxies = halos) with valid mass
        w = np.where((galaxies.Type == 0) & (galaxies.Mvir > 0.0))[0]

        # Validate this snapshot - skip if no halos found
        is_valid, skip_msg = validate_evolution_snapshot(w, redshift, "HMF Evolution", verbose)
        if not is_valid:
            continue

        mass = np.log10(galaxies.Mvir[w] * 1.0e10 / hubble_h)

        # Calculate mass function
        xaxis, hmf = calculate_mass_function(
            mass, volume, hubble_h, BINWIDTH_DEX, HALO_MASS_MIN, HALO_MASS_MAX
        )

        # Plot the histogram
        ax.plot(xaxis, hmf, color=color, linestyle="-", lw=2)

        # Store redshift values for labels in the top right corner
        if i == 0:
            # Initialize list of redshifts for the legend
            redshift_labels = []
        # Add this redshift to the list
        redshift_labels.append((redshift, color))

    # Customize the plot
    ax.set_yscale("log")
    ax.set_xlim(*PLOT_XLIM)
    ax.set_ylim(*PLOT_YLIM)
    ax.xaxis.set_minor_locator(MultipleLocator(BINWIDTH_DEX))

    ax.set_ylabel(get_mass_function_labels(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_halo_mass_label(), fontsize=AXIS_LABEL_SIZE)

    # Add redshift labels in the top right corner
    if "redshift_labels" in locals():
        # Sort labels by redshift
        redshift_labels.sort(key=lambda x: x[0])
        # Position for the first label
        x_pos = 14.8
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

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "HaloMassFunction_Evolution", output_format, verbose
    )
    return plot_path, None
