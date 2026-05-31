#!/usr/bin/env python

"""
Mimic Spatial Distribution Plot

This module generates a multi-panel plot showing the spatial distribution of galaxies.
"""

import random

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    setup_plot_fonts,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_field_has_values,
    check_required_fields,
    save_and_close_figure,
    validate_filtered_data,
    warn,
)


def plot(
    galaxies,
    volume,
    metadata,
    params,
    output_dir="plots",
    output_format=".png",
    verbose=False,
):
    """
    Create a spatial distribution plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check for required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=['Mvir', 'Pos'],
        plot_name='Spatial Distribution'
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation
    has_mvir, count, msg = check_field_has_values(
        galaxies.Mvir, 'Mvir', threshold=0.0
    )
    if not has_mvir:
        return None, f"Field validation failed: {msg}"

    # Set random seed for reproducibility when sampling points
    random.seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]
    box_size = metadata.get("box_size", 62.5)  # Default to Mini-Millennium

    # Filter for galaxies with non-zero halo mass
    w = np.where(galaxies.Mvir > 0.0)[0]

    # Filter-level validation
    is_valid, skip_msg = validate_filtered_data(w, "Spatial Distribution", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    # Create a figure with 3 subplots (arranged in a 2x2 grid, with one empty)
    fig, axes = plt.subplots(2, 2, figsize=(10, 10))
    axes = axes.flatten()

    # Hide the empty subplot (bottom right)
    axes[3].set_visible(False)

    # Apply consistent font settings to each subplot
    for ax in axes[:3]:
        setup_plot_fonts(ax)

    # If we have too many galaxies, randomly sample a subset
    dilute = 10000  # Higher limit for this plot to show structure
    if len(w) > dilute:
        w = random.sample(list(w), dilute)

    # Get positions
    xx = galaxies.Pos[w, 0]
    yy = galaxies.Pos[w, 1]
    zz = galaxies.Pos[w, 2]

    # Add a small buffer around the box for the plot
    buffer = box_size * 0.1

    # Print some debug information if verbose mode is enabled
    if verbose:
        print(f"  Number of galaxies plotted: {len(w)}")
        print(f"  Box size: {box_size}")
        print(f"  X position range: {min(xx):.2f} to {max(xx):.2f}")

    # Plot X-Y projection
    axes[0].scatter(xx, yy, marker="o", s=0.3, c="k", alpha=0.5)
    axes[0].set_xlabel(r"X [h$^{-1}$ Mpc]", fontsize=AXIS_LABEL_SIZE)
    axes[0].set_ylabel(r"Y [h$^{-1}$ Mpc]", fontsize=AXIS_LABEL_SIZE)
    axes[0].set_xlim(0.0 - buffer, box_size + buffer)
    axes[0].set_ylim(0.0 - buffer, box_size + buffer)

    # Plot X-Z projection
    axes[1].scatter(xx, zz, marker="o", s=0.3, c="k", alpha=0.5)
    axes[1].set_xlabel(r"X [h$^{-1}$ Mpc]", fontsize=AXIS_LABEL_SIZE)
    axes[1].set_ylabel(r"Z [h$^{-1}$ Mpc]", fontsize=AXIS_LABEL_SIZE)
    axes[1].set_xlim(0.0 - buffer, box_size + buffer)
    axes[1].set_ylim(0.0 - buffer, box_size + buffer)

    # Plot Y-Z projection
    axes[2].scatter(yy, zz, marker="o", s=0.3, c="k", alpha=0.5)
    axes[2].set_xlabel(r"Y [h$^{-1}$ Mpc]", fontsize=AXIS_LABEL_SIZE)
    axes[2].set_ylabel(r"Z [h$^{-1}$ Mpc]", fontsize=AXIS_LABEL_SIZE)
    axes[2].set_xlim(0.0 - buffer, box_size + buffer)
    axes[2].set_ylim(0.0 - buffer, box_size + buffer)

    # Adjust layout
    plt.tight_layout()

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "SpatialDistribution", output_format, verbose)
    return plot_path, None
