#!/usr/bin/env python

"""
SAGE Bulge Mass Fraction Plot

This module generates a plot showing the bulge and disk mass fractions vs. stellar mass for SAGE galaxy data.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    get_stellar_mass_label,
    setup_legend,
    setup_plot_fonts,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    warn,
    check_field_has_values,
    check_required_fields,
        setup_figure,
    validate_filtered_data,
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
    Create a bulge mass fraction vs. stellar mass plot.

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
        required_fields=['StellarMass', 'BulgeMass'],
        plot_name='Bulge Mass Fraction'
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    # Calculate bulge and disk fractions
    # Handle division by zero safely
    valid_galaxies = np.where(galaxies.StellarMass > 0.0)[0]

    # Validate filtered data
    is_valid, skip_msg = validate_filtered_data(valid_galaxies, "Bulge Mass Fraction", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # Calculate bulge and disk fractions
    f_bulge = galaxies.BulgeMass[valid_galaxies] / galaxies.StellarMass[valid_galaxies]
    f_disk = 1.0 - f_bulge

    # Convert stellar mass to log scale
    mass = np.log10(galaxies.StellarMass[valid_galaxies] * 1.0e10 / hubble_h)

    # Set up mass bins for the averaging
    binwidth = 0.2
    shift = binwidth / 2.0
    mass_range = np.arange(8.5 - shift, 12.0 + shift, binwidth)
    bins = len(mass_range)

    # Initialize arrays for average values and variances
    f_bulge_ave = np.zeros(bins)
    f_bulge_var = np.zeros(bins)
    f_disk_ave = np.zeros(bins)
    f_disk_var = np.zeros(bins)

    # Calculate average values and variances in each bin
    for i in range(bins - 1):
        bin_mask = (mass >= mass_range[i]) & (mass < mass_range[i + 1])
        if np.sum(bin_mask) > 0:
            f_bulge_ave[i] = np.mean(f_bulge[bin_mask])
            f_bulge_var[i] = np.std(f_bulge[bin_mask]) ** 2  # Variance
            f_disk_ave[i] = np.mean(f_disk[bin_mask])
            f_disk_var[i] = np.std(f_disk[bin_mask]) ** 2  # Variance

    # Print some debug information if verbose mode is enabled
    if verbose:
        print(f"Bulge Mass Fraction plot debug:")
        print(f"  Number of galaxies: {len(valid_galaxies)}")
        print(f"  Stellar mass range: {min(mass):.2f} to {max(mass):.2f}")
        print(f"  Bulge fraction range: {min(f_bulge):.3f} to {max(f_bulge):.3f}")
        print(f"  Number of mass bins: {bins}")

    # Plot bulge fractions
    mask_bulge = f_bulge_ave > 0.0
    ax.plot(
        mass_range[mask_bulge] + shift, f_bulge_ave[mask_bulge], "r-", label="bulge"
    )
    ax.fill_between(
        mass_range[mask_bulge] + shift,
        np.clip(f_bulge_ave[mask_bulge] + np.sqrt(f_bulge_var[mask_bulge]), 0, 1),
        np.clip(f_bulge_ave[mask_bulge] - np.sqrt(f_bulge_var[mask_bulge]), 0, 1),
        facecolor="red",
        alpha=0.25,
    )

    # Plot disk fractions
    mask_disk = f_disk_ave > 0.0
    ax.plot(
        mass_range[mask_disk] + shift, f_disk_ave[mask_disk], "k-", label="disk stars"
    )
    ax.fill_between(
        mass_range[mask_disk] + shift,
        np.clip(f_disk_ave[mask_disk] + np.sqrt(f_disk_var[mask_disk]), 0, 1),
        np.clip(f_disk_ave[mask_disk] - np.sqrt(f_disk_var[mask_disk]), 0, 1),
        facecolor="black",
        alpha=0.25,
    )

    # Customize the plot
    ax.set_xlabel(get_stellar_mass_label(), fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"Stellar Mass Fraction", fontsize=AXIS_LABEL_SIZE)

    # Set axis limits - matching the original plot
    ax.set_xlim(mass_range[0], mass_range[bins - 1])
    ax.set_ylim(0.0, 1.05)

    # Add consistently styled legend
    setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "BulgeMassFraction", output_format, verbose)
    return plot_path, None