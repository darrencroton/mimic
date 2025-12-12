#!/usr/bin/env python

"""
Mimic Halo Mass Function Plot

This module generates a halo mass function plot from Mimic halo data.
"""

# Standard library
import os

# Third-party packages
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import MultipleLocator

# Local application imports
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    get_halo_mass_label,
    get_mass_function_labels,
    setup_legend,
    setup_plot_fonts,
)
from output_utils import (
    calculate_mass_function,
    check_required_fields,
    create_empty_plot_with_message,
    save_and_close_figure,
    setup_figure,
    warn,
)

# Physical limits for halo mass functions
HALO_MASS_MIN = 10.0  # log10(Msun) - below resolution limit
HALO_MASS_MAX = 16.0  # log10(Msun) - above cluster scale
BINWIDTH_DEX = 0.1    # Standard bin width in dex
PLOT_XLIM = (10.0, 15.0)  # Plot x-axis limits
PLOT_YLIM = (1.0e-6, 1.0e-1)  # Plot y-axis limits


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
    Create a halo mass function plot.

    Args:
        galaxies: Galaxy data as a numpy recarray (containing halo information)
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output

    Returns:
        Path to the saved plot file
    """
    # Check for required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=['Mvir'],
        plot_name='Halo Mass Function'
    )

    # Set up the figure
    fig, ax = setup_figure()

    if not success:
        warn(msg)
        create_empty_plot_with_message(ax, msg, IN_FIGURE_TEXT_SIZE)
        return save_and_close_figure(fig, output_dir, "HaloMassFunction", output_format, verbose)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    # Prepare data - select halos (Type 0 = central galaxies = halos) with valid masses
    w = np.where((galaxies.Type == 0) & (galaxies.Mvir > 0.0))[0]

    if len(w) == 0:
        warn("No halos found with Mvir > 0.0")
        create_empty_plot_with_message(ax, "No halos found with Mvir > 0.0", IN_FIGURE_TEXT_SIZE)
        return save_and_close_figure(fig, output_dir, "HaloMassFunction", output_format, verbose)

    # Convert halo mass to log scale (Mvir is in units of 10^10 Msun/h)
    mass = np.log10(galaxies.Mvir[w] * 1.0e10 / hubble_h)

    # Calculate halo mass function
    xaxis, hmf = calculate_mass_function(mass, volume, hubble_h, BINWIDTH_DEX, HALO_MASS_MIN, HALO_MASS_MAX)

    # Print debugging info
    if verbose:
        print(f"  mi={HALO_MASS_MIN}, ma={HALO_MASS_MAX}")
        print(f"  min mass={min(mass)}, max mass={max(mass)}")
        print(f"  volume={volume}, hubble_h={hubble_h}")
        print(f"  Number of halos: {len(w)}")

    # Plot the halo mass function
    ax.plot(xaxis, hmf, "k-", lw=2, label="All Halos")

    # Customize the plot
    ax.set_yscale("log")
    ax.set_xlim(*PLOT_XLIM)
    ax.set_ylim(*PLOT_YLIM)
    ax.xaxis.set_minor_locator(MultipleLocator(BINWIDTH_DEX))

    # Set labels with larger font sizes
    ax.set_ylabel(get_mass_function_labels(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_halo_mass_label(), fontsize=AXIS_LABEL_SIZE)

    # Add consistently styled legend
    setup_legend(ax, loc="lower left")

    # Save and close the figure
    return save_and_close_figure(fig, output_dir, "HaloMassFunction", output_format, verbose)
