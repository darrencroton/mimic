#!/usr/bin/env python

"""
SAGE Baryonic Mass Function Plot

This module generates a baryonic mass function plot from SAGE galaxy data.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    get_baryonic_mass_label,
    get_mass_function_labels,
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
    calculate_mass_function,
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
    Create a baryonic mass function plot.

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
        required_fields=['StellarMass', 'ColdGas'],
        plot_name='Baryonic Mass Function'
    )

    if not success:
        warn(msg)
        return None, f"Required fields missing: {msg}"

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    whichimf = 1  # Default to Chabrier
    if "WhichIMF" in params:
        whichimf = int(params["WhichIMF"])

    # Set up binning
    binwidth = 0.1  # mass function histogram bin width

    # Prepare data
    w = np.where((galaxies.StellarMass + galaxies.ColdGas) > 0.0)[0]

    # Check if we have any galaxies to plot
    if len(w) == 0:
        msg = "No galaxies found with baryonic mass > 0.0"
        warn(msg)
        return None, msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    mass = np.log10((galaxies.StellarMass[w] + galaxies.ColdGas[w]) * 1.0e10 / hubble_h)

    # Calculate baryonic mass function
    xaxis, bmf = calculate_mass_function(mass, volume, hubble_h, binwidth)

    # Plot the main histogram
    ax.plot(xaxis, bmf, "k-", label="Model")

    # Bell et al. 2003 BMF (h=1.0 converted to h=0.73)
    M = np.arange(7.0, 13.0, 0.01)
    Mstar = np.log10(5.3 * 1.0e10 / hubble_h / hubble_h)
    alpha = -1.21
    phistar = 0.0108 * hubble_h**3
    xval = 10.0 ** (M - Mstar)
    yval = np.log(10.0) * phistar * xval ** (alpha + 1) * np.exp(-xval)

    if whichimf == 0:
        # converted diet Salpeter IMF to Salpeter IMF
        ax.plot(np.log10(10.0**M / 0.7), yval, "b-", lw=2.0, label="Bell et al. 2003")
    elif whichimf == 1:
        # converted diet Salpeter IMF to Salpeter IMF, then to Chabrier IMF
        ax.plot(
            np.log10(10.0**M / 0.7 / 1.8), yval, "g--", lw=1.5, label="Bell et al. 2003"
        )

    # Customize the plot
    ax.set_yscale("log")
    ax.set_xlim(8.0, 12.5)
    ax.set_ylim(1.0e-6, 1.0e-1)
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))

    ax.set_ylabel(get_mass_function_labels(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_baryonic_mass_label(), fontsize=AXIS_LABEL_SIZE)

    # Add consistently styled legend
    setup_legend(ax, loc="lower left")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "BaryonicMassFunction", output_format, verbose)
    return plot_path, None