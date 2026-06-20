#!/usr/bin/env python

"""
SAGE Black Hole - Bulge Mass Relation Plot

This module generates a plot of the relationship between black hole mass and bulge mass from SAGE galaxy data.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
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
    Create a black hole - bulge mass relation plot.

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
    # Check required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=["BlackHoleMass", "BulgeMass"],
        plot_name="Black Hole-Bulge Relation",
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Set random seed for reproducibility when sampling points
    random.seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "black_hole_bulge_relation", (8.0, 12.0), (6.0, 10.0)
    )

    # Maximum number of points to plot (for better performance and readability)
    dilute = 7500

    # Filter for valid galaxies with both bulge and black hole mass
    w = np.where((galaxies.BulgeMass > 0.01) & (galaxies.BlackHoleMass > 0.00001))[0]

    # Validate filtered data
    is_valid, skip_msg = validate_filtered_data(w, "Black Hole-Bulge Relation", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # If we have too many galaxies, randomly sample a subset
    if len(w) > dilute:
        w = random.sample(list(w), dilute)

    # Convert to physical units (Msun)
    bh_mass = np.log10(galaxies.BlackHoleMass[w] * 1.0e10 / hubble_h)
    bulge_mass = np.log10(galaxies.BulgeMass[w] * 1.0e10 / hubble_h)

    # Print some debug information if verbose mode is enabled
    if verbose:
        print(f"Black Hole-Bulge Relation plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")
        print(f"  Bulge mass range: {min(bulge_mass):.2f} to {max(bulge_mass):.2f}")
        print(f"  Black hole mass range: {min(bh_mass):.2f} to {max(bh_mass):.2f}")

    # Plot the galaxy data
    ax.scatter(bulge_mass, bh_mass, marker="o", s=1, c="k", alpha=0.5, label="Model galaxies")

    # Add Häring & Rix 2004 observational relation
    # M_BH = 10^(8.2) * (M_bulge/10^11)^1.12
    x_hr = np.logspace(8, 12, 100)
    y_hr = 10 ** (8.2) * (x_hr / 1.0e11) ** 1.12

    ax.plot(np.log10(x_hr), np.log10(y_hr), "b-", label="Häring & Rix 2004", lw=2)

    # Customize the plot
    ax.set_xlabel(r"log$_{10}$ M$_{\rm bulge}$ [M$_{\odot}$]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"log$_{10}$ M$_{\rm BH}$ [M$_{\odot}$]", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.5))

    # Set axis limits - matching the original plot
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    # Add consistently styled legend
    setup_legend(ax, loc="upper left")

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "BlackHoleBulgeRelation", output_format, verbose
    )
    return plot_path, None
