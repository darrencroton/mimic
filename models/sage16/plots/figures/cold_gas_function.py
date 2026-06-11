#!/usr/bin/env python

"""
Mimic Cold Gas Mass Function Plot

This module generates a cold gas mass function plot from Mimic galaxy data.
Requires: ColdGas property (from galaxy physics modules)
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, get_mass_function_labels, setup_legend
from matplotlib.ticker import MultipleLocator
from output_utils import (
    calculate_mass_function,
    check_required_fields,
    save_and_close_figure,
    setup_figure,
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
    Create a cold gas mass function plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Print debugging information

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check required fields
    success, optional, msg = check_required_fields(
        galaxies, required_fields=["ColdGas"], plot_name="Cold Gas Function"
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    # Set up binning
    binwidth = 0.1  # mass function histogram bin width

    # Select all galaxies with valid cold gas mass
    w = np.where(galaxies.ColdGas > 0.0)[0]

    # Check if we have any galaxies to plot
    if len(w) == 0:
        msg = "No galaxies found with ColdGas > 0.0"
        return None, msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # Convert cold gas mass to log scale (ColdGas is in units of 10^10 Msun/h)
    mass = np.log10(galaxies.ColdGas[w] * 1.0e10 / hubble_h)

    # Force some reasonable limits for gas masses
    mi = 8.0  # Don't go below 10^8 Msun
    ma = 12.5  # Don't go above 10^12.5 Msun

    # Calculate cold gas mass function
    xaxis, cgmf = calculate_mass_function(mass, volume, hubble_h, binwidth, mi, ma)

    # Print debugging info
    if verbose:
        print(f"  mi={mi}, ma={ma}")
        print(f"  min mass={min(mass)}, max mass={max(mass)}")
        print(f"  volume={volume}, hubble_h={hubble_h}")
        print(f"  Number of galaxies: {len(w)}")

    # Plot the cold gas mass function
    ax.plot(xaxis, cgmf, "b-", lw=2, label="Central Galaxies")

    # Customize the plot
    ax.set_yscale("log")
    ax.set_xlim(8.0, 12.5)
    ax.set_ylim(1.0e-6, 1.0e-1)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))

    # Set labels with larger font sizes
    ax.set_ylabel(get_mass_function_labels(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(r"log$_{10}$ M$_{\rm cold~gas}$ [M$_{\odot}$]", fontsize=AXIS_LABEL_SIZE)

    # Add consistently styled legend
    setup_legend(ax, loc="lower left")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "ColdGasFunction", output_format, verbose)
    return plot_path, None
