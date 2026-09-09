#!/usr/bin/env python

"""
SAGE Mass Reservoir Scatter Plot

This module generates a scatter plot showing the mass in different galaxy components vs. halo mass.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from matplotlib.ticker import MultipleLocator
from output_schema import mass_to_msun
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
    Create a mass reservoir scatter plot.

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
        required_fields=["Mvir", "Type", "StellarMass", "ColdGas", "HotGas", "EjectedGas", "ICS"],
        plot_name="Mass Reservoir Scatter",
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Set random seed for reproducibility when sampling points
    random.seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]
    schema_units = metadata.get("schema_units", {})

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "mass_reservoir_scatter", (10.0, 14.0), (7.5, 12.5)
    )

    # Maximum number of points to plot (for better performance and readability)
    dilute = 7500

    def mass_msun(field_name, idx):
        units = schema_units.get(field_name, "1e10 Msun/h")
        return mass_to_msun(getattr(galaxies, field_name)[idx], units, hubble_h)

    # Floor at the display axis's x_min, not an absolute-mass literal, so a
    # profile override reaches the plotted points, not just the canvas.
    candidates = np.where(
        (galaxies.Type == 0) & (galaxies.Mvir > 0.0) & (galaxies.StellarMass > 0.0)
    )[0]
    log_mvir_candidates = np.log10(mass_msun("Mvir", candidates))
    keep = log_mvir_candidates >= x_min
    w = candidates[keep]
    mvir = log_mvir_candidates[keep]

    # Validate filtered data
    is_valid, skip_msg = validate_filtered_data(w, "Mass Reservoir Scatter", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # If we have too many galaxies, randomly sample a subset
    if len(w) > dilute:
        sample_idx = random.sample(range(len(w)), dilute)
        w = w[sample_idx]
        mvir = mvir[sample_idx]

    # Get remaining masses in log10 physical Msun units
    stellar_mass = np.log10(mass_msun("StellarMass", w))
    cold_gas = np.log10(np.maximum(mass_msun("ColdGas", w), 1.0))  # Avoid log(0)
    hot_gas = np.log10(np.maximum(mass_msun("HotGas", w), 1.0))
    ejected_gas = np.log10(np.maximum(mass_msun("EjectedGas", w), 1.0))
    ics = np.log10(np.maximum(mass_msun("ICS", w), 1.0))

    # Print some debug information
    # Print some debug information if verbose mode is enabled
    if verbose:
        print(f"  Number of galaxies plotted: {len(w)}")
        print(f"  Halo mass range: {min(mvir):.2f} to {max(mvir):.2f}")
        print(f"  Stellar mass range: {min(stellar_mass):.2f} to {max(stellar_mass):.2f}")

    # Plot each mass component
    ax.scatter(mvir, stellar_mass, marker="o", s=0.8, c="k", alpha=0.5, label="Stars")
    ax.scatter(mvir, cold_gas, marker="o", s=0.8, c="blue", alpha=0.5, label="Cold gas")
    ax.scatter(mvir, hot_gas, marker="o", s=0.8, c="red", alpha=0.5, label="Hot gas")
    ax.scatter(mvir, ejected_gas, marker="o", s=0.8, c="green", alpha=0.5, label="Ejected gas")
    ax.scatter(mvir, ics, marker="x", s=5, c="yellow", alpha=0.7, label="Intracluster stars")

    # Customize the plot
    ax.set_xlabel(r"log M$_{\rm vir}$ [h$^{-1}$ M$_{\odot}$]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"Stellar, cold, hot, ejected, ICS mass", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.5))

    # Set axis limits
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    # Add text annotation 'All' in the bottom-right corner
    ax.text(0.95, 0.05, r"All", transform=ax.transAxes, fontsize=12, ha="right", va="bottom")

    # Add consistently styled legend
    setup_legend(ax, loc="upper left")

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "MassReservoirScatter", output_format, verbose
    )
    return plot_path, None
