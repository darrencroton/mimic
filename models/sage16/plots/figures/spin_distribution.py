#!/usr/bin/env python

"""
Mimic Spin Distribution Plot

This module generates a plot showing the distribution of galaxy spin parameters.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    setup_legend,
    setup_plot_fonts,
)
from matplotlib.ticker import MaxNLocator, MultipleLocator
from output_utils import (
    check_field_has_values,
    check_required_fields,
    save_and_close_figure,
    setup_figure,
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
    Create a spin parameter distribution plot.

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
        galaxies, required_fields=["Spin", "Rvir", "Vvir"], plot_name="Spin Distribution"
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Filter for valid galaxies with non-zero Vvir and Rvir
    valid_galaxies = np.where(
        (galaxies.Vvir > 0.0)
        & (galaxies.Rvir > 0.0)
        & (galaxies.Spin[:, 0] ** 2 + galaxies.Spin[:, 1] ** 2 + galaxies.Spin[:, 2] ** 2 > 0.0)
    )[0]

    # Validate filtered data
    is_valid, skip_msg = validate_filtered_data(valid_galaxies, "Spin Distribution", verbose)
    if not is_valid:
        return None, skip_msg

    # Calculate spin parameter according to the formula:
    # λ = |J| / (√2 * Vvir * Rvir)
    # where |J| is the magnitude of the angular momentum vector (Spin in Mimic)
    spin_magnitude = np.sqrt(
        galaxies.Spin[valid_galaxies, 0] ** 2
        + galaxies.Spin[valid_galaxies, 1] ** 2
        + galaxies.Spin[valid_galaxies, 2] ** 2
    )

    spin_parameter = spin_magnitude / (
        np.sqrt(2) * galaxies.Vvir[valid_galaxies] * galaxies.Rvir[valid_galaxies]
    )

    # Filter out any invalid values
    valid_spins = np.where(
        (spin_parameter > 0.0) & (spin_parameter < 1.0) & np.isfinite(spin_parameter)
    )[0]

    # Second validation: check if we have valid spin parameter values
    is_valid, skip_msg = validate_filtered_data(
        valid_spins, "Spin Distribution (valid parameters)", verbose
    )
    if not is_valid:
        return None, skip_msg

    spin_parameter = spin_parameter[valid_spins]

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # Print some debug information
    # Print some debug information if verbose mode is enabled
    if verbose:
        print(f"  Number of galaxies with valid spin: {len(spin_parameter)}")
        print(f"  Spin parameter range: {min(spin_parameter):.4f} to {max(spin_parameter):.4f}")
        print(f"  Mean spin parameter: {np.mean(spin_parameter):.4f}")
        print(f"  Median spin parameter: {np.median(spin_parameter):.4f}")

    # Create histogram of spin parameters
    bin_min = -0.02
    bin_max = 0.5
    bin_width = 0.01
    bins = int((bin_max - bin_min) / bin_width)

    counts, bin_edges = np.histogram(spin_parameter, range=(bin_min, bin_max), bins=bins)
    bin_centers = bin_edges[:-1] + bin_width / 2

    # Plot the histogram
    ax.plot(bin_centers, counts, "k-", lw=2, label="Simulation")

    # Add a theoretical log-normal distribution for comparison (optional)
    # Parameters for log-normal are typical for halos
    lambda_0 = 0.035  # Typical value for halos
    sigma = 0.5  # Typical value for width of the distribution

    # Create log-normal distribution (normalized to match the histogram peak)
    x = np.linspace(0.001, 0.5, 1000)
    p_lognormal = (1 / (x * sigma * np.sqrt(2 * np.pi))) * np.exp(
        -np.log(x / lambda_0) ** 2 / (2 * sigma**2)
    )

    # Normalize to match the histogram peak
    norm_factor = max(counts) / max(p_lognormal)
    p_lognormal *= norm_factor

    # Plot theoretical distribution
    ax.plot(x, p_lognormal, "r--", lw=1.5, label=r"Log-normal ($\lambda_0=0.035$)")

    # Customize the plot
    ax.set_xlabel(r"Spin Parameter", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"Number", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks with MaxNLocator to avoid excessive ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.01))
    # Use MaxNLocator instead to prevent too many ticks
    ax.yaxis.set_minor_locator(MaxNLocator(10))

    # Set axis limits
    ax.set_xlim(0.0, 0.25)
    y_max = max(counts) * 1.1
    ax.set_ylim(0, y_max)

    # Add consistently styled legend
    setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "SpinDistribution", output_format, verbose)
    return plot_path, None
