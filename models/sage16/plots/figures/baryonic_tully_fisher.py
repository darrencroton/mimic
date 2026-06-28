#!/usr/bin/env python

"""
SAGE Baryonic Tully-Fisher Relationship Plot

This module generates a baryonic Tully-Fisher plot from SAGE galaxy data.
"""

from random import sample, seed

import numpy as np
from figures import AXIS_LABEL_SIZE, get_baryonic_mass_label, get_vmax_label, setup_legend
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
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
    dilute=7500,
    verbose=False,
):
    """
    Create a baryonic Tully-Fisher plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with SAGE parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        dilute: Maximum number of points to plot for visual clarity (default: 7500).
                This parameter is unique to Tully-Fisher plots to prevent overplotting
                in scatter plots while maintaining statistical representation.
        verbose: Enable verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=["StellarMass", "ColdGas", "BulgeMass", "Vmax"],
        plot_name="Baryonic Tully-Fisher",
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Set random seed for reproducibility when diluting
    seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "baryonic_tully_fisher", (1.4, 2.6), (8.0, 12.0)
    )

    # Select Sb/c galaxies (Type=0 and bulge/total ratio between 0.1 and 0.5)
    # First filter for non-zero stellar mass to avoid division by zero
    valid_mass = (
        (galaxies.Type == 0)
        & (galaxies.StellarMass > 0.0)
        & (galaxies.StellarMass + galaxies.ColdGas > 0.0)
    )

    # Then calculate ratios safely
    bulge_to_stellar = np.zeros_like(galaxies.StellarMass)
    bulge_to_stellar[valid_mass] = galaxies.BulgeMass[valid_mass] / galaxies.StellarMass[valid_mass]

    # Now apply all filters
    w = np.where(valid_mass & (bulge_to_stellar > 0.1) & (bulge_to_stellar < 0.5))[0]

    # Check if we have any galaxies to plot
    if len(w) == 0:
        msg = "No suitable galaxies found for Tully-Fisher plot"
        return None, msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # Dilute the sample if needed
    if len(w) > dilute:
        w = sample(list(w), dilute)

    # Calculate baryonic mass and max velocity
    mass = np.log10((galaxies.StellarMass[w] + galaxies.ColdGas[w]) * 1.0e10 / hubble_h)
    vel = np.log10(galaxies.Vmax[w])

    # Plot the model galaxies
    ax.scatter(vel, mass, marker="o", s=1, c="k", alpha=0.5, label="Model Sb/c galaxies")

    # Plot Stark, McGaugh & Swatters 2009 relation
    w_obs = np.arange(0.5, 10.0, 0.5)
    TF = 3.94 * w_obs + 1.79
    ax.plot(w_obs, TF, "b-", lw=2.0, label="Stark, McGaugh \\& Swatters 2009")

    # Customize the plot
    ax.set_ylabel(get_baryonic_mass_label(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_vmax_label(), fontsize=AXIS_LABEL_SIZE)

    # Set the axis limits and minor ticks
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.xaxis.set_minor_locator(MultipleLocator(0.05))
    ax.yaxis.set_minor_locator(MultipleLocator(0.25))

    # Add consistently styled legend
    setup_legend(ax, loc="lower right")

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "BaryonicTullyFisher", output_format, verbose
    )
    return plot_path, None
