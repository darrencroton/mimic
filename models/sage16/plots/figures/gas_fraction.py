#!/usr/bin/env python

"""
SAGE Gas Fraction Plot

This module generates a plot showing the gas fraction vs. stellar mass for SAGE galaxy data.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE, get_stellar_mass_label, setup_legend
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
    validate_filtered_data,
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
    Create a gas fraction vs. stellar mass plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with SAGE parameters
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
        galaxies, required_fields=["StellarMass", "ColdGas", "BulgeMass"], plot_name="Gas Fraction"
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Set random seed for reproducibility when sampling points
    random.seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    x_min, x_max, y_min, y_max = get_profile_axes(params, "gas_fraction", (8.0, 12.0), (0.0, 1.0))

    # Maximum number of points to plot (for better performance and readability)
    dilute = 7500

    # First filter for valid mass values - avoid division by zero
    valid_mass = (
        (galaxies.Type == 0)
        & (galaxies.StellarMass > 0.0)
        & (galaxies.StellarMass + galaxies.ColdGas > 0.0)
    )

    # Calculate ratio safely for valid galaxies
    bulge_ratio = np.zeros_like(galaxies.StellarMass)
    bulge_ratio[valid_mass] = galaxies.BulgeMass[valid_mass] / galaxies.StellarMass[valid_mass]

    # Now apply all filters
    w = np.where(valid_mass & (bulge_ratio > 0.1) & (bulge_ratio < 0.5))[0]

    # Validate filtered data
    is_valid, skip_msg = validate_filtered_data(w, "Gas Fraction", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # If we have too many galaxies, randomly sample a subset
    if len(w) > dilute:
        w = random.sample(list(w), dilute)

    # Calculate gas fraction and convert stellar mass to log scale
    stellar_mass = np.log10(galaxies.StellarMass[w] * 1.0e10 / hubble_h)
    gas_fraction = galaxies.ColdGas[w] / (galaxies.StellarMass[w] + galaxies.ColdGas[w])

    # Print some debug information if verbose mode is enabled
    if verbose:
        print(f"Gas Fraction plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")
        print(f"  Stellar mass range: {min(stellar_mass):.2f} to {max(stellar_mass):.2f}")
        print(f"  Gas fraction range: {min(gas_fraction):.3f} to {max(gas_fraction):.3f}")

    # Plot the galaxy data
    ax.scatter(
        stellar_mass,
        gas_fraction,
        marker="o",
        s=1,
        c="k",
        alpha=0.5,
        label="Model Sb/c galaxies",
    )

    # Customize the plot
    ax.set_xlabel(get_stellar_mass_label(), fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"Cold Mass / (Cold+Stellar Mass)", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.05))

    # Set axis limits - matching the original plot
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    # Add consistently styled legend
    setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "GasFraction", output_format, verbose)
    return plot_path, None
