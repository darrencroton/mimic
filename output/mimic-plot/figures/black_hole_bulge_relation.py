#!/usr/bin/env python

"""
SAGE Black Hole - Bulge Mass Relation Plot

This module generates a plot of the relationship between black hole mass and bulge mass from SAGE galaxy data.
"""

import os
import random

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    setup_legend,
    setup_plot_fonts,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    warn,
    check_required_fields,
    create_empty_plot_with_message,
    setup_figure,
    save_and_close_figure,
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

    Returns:
        Path to the saved plot file
    """
    # Check required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=['BlackHoleMass', 'BulgeMass'],
        plot_name='Black Hole-Bulge Relation'
    )

    # Set up the figure
    fig, ax = setup_figure()

    if not success:
        warn(msg)
        create_empty_plot_with_message(ax, msg, IN_FIGURE_TEXT_SIZE)
        return save_and_close_figure(fig, output_dir, "BlackHoleBulgeRelation", output_format, verbose)

    # Set random seed for reproducibility when sampling points
    random.seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    # Maximum number of points to plot (for better performance and readability)
    dilute = 7500

    # Filter for valid galaxies with both bulge and black hole mass
    w = np.where((galaxies.BulgeMass > 0.01) & (galaxies.BlackHoleMass > 0.00001))[0]

    # Check if we have any galaxies to plot
    if len(w) == 0:
        msg = "No galaxies found with both bulge and black hole mass"
        warn(msg)
        create_empty_plot_with_message(ax, msg, IN_FIGURE_TEXT_SIZE)
        os.makedirs(output_dir, exist_ok=True)
        output_path = os.path.join(output_dir, f"BlackHoleBulgeRelation{output_format}")
        plt.savefig(output_path)
        plt.close()
        return output_path

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
    ax.scatter(
        bulge_mass, bh_mass, marker="o", s=1, c="k", alpha=0.5, label="Model galaxies"
    )

    # Add Häring & Rix 2004 observational relation
    # M_BH = 10^(8.2) * (M_bulge/10^11)^1.12
    x_hr = np.logspace(8, 12, 100)
    y_hr = 10 ** (8.2) * (x_hr / 1.0e11) ** 1.12

    ax.plot(np.log10(x_hr), np.log10(y_hr), "b-", label="Häring & Rix 2004", lw=2)

    # Customize the plot
    ax.set_xlabel(r"log$_{10}$ M$_{\rm bulge}$ (M$_{\odot}$)", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"log$_{10}$ M$_{\rm BH}$ (M$_{\odot}$)", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.5))

    # Set axis limits - matching the original plot
    ax.set_xlim(8.0, 12.0)
    ax.set_ylim(6.0, 10.0)

    # Add consistently styled legend
    setup_legend(ax, loc="upper left")

    # Save and close the figure
    return save_and_close_figure(fig, output_dir, "BlackHoleBulgeRelation", output_format, verbose)
