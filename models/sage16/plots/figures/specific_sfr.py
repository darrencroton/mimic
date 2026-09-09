#!/usr/bin/env python

"""
SAGE Specific Star Formation Rate Plot

This module generates a specific star formation rate plot from SAGE galaxy data.
"""

from random import seed

import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    get_ssfr_label,
    get_stellar_mass_label,
    setup_legend,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_field_has_values,
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    select_scatter_sample,
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
    dilute=7500,
    verbose=False,
):
    """
    Create a specific star formation rate plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with SAGE parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        dilute: Maximum number of points to plot (for clarity)
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check for required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=["StellarMass", "StarFormationRate"],
        plot_name="Specific Star Formation Rate",
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation: Check if StellarMass and StarFormationRate have non-zero values.
    # threshold=0.0 (not a resolution-tied literal) since the real mass floor is the
    # profile's x_min, applied below.
    has_mass, count, msg = check_field_has_values(
        galaxies.StellarMass, "StellarMass", threshold=0.0
    )
    if not has_mass:
        return None, f"Field validation failed: {msg}"

    has_sfr, count, msg = check_field_has_values(
        galaxies.StarFormationRate, "StarFormationRate", threshold=0.0
    )
    if not has_sfr:
        return None, f"Field validation failed: {msg}"

    # Set random seed for reproducibility when diluting
    seed(2222)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "specific_sfr", (8.0, 12.0), (-14.0, -8.0)
    )

    # Floor at the display axis's x_min, not an absolute-mass literal, so a
    # profile override reaches the plotted points, not just the canvas.
    has_stellar_mass = galaxies.StellarMass > 0.0
    log_stellar_mass = np.full(len(galaxies), -np.inf)
    log_stellar_mass[has_stellar_mass] = np.log10(
        galaxies.StellarMass[has_stellar_mass] * 1.0e10 / hubble_h
    )
    w = np.where(has_stellar_mass & (log_stellar_mass >= x_min))[0]

    # Filter-level validation: Check if filtering produced results
    is_valid, skip_msg = validate_filtered_data(w, "Specific SFR", verbose)
    if not is_valid:
        return None, skip_msg

    # Calculate stellar mass and specific SFR for every candidate, before
    # restricting to the display box and diluting -- sSFR must be known to
    # tell which points are actually inside the box.
    mass = log_stellar_mass[w]
    sfr = galaxies.StarFormationRate[w]

    # Avoid log10(0) and division by zero
    valid_sfr = sfr > 0

    # Initialize ssfr with a very low value (below the plot range)
    ssfr = np.full_like(mass, -15.0)  # Well below typical plot range

    if np.any(valid_sfr):
        # Calculate SSFR only for galaxies with non-zero SFR
        stellar_mass_phys = galaxies.StellarMass[w][valid_sfr] * 1.0e10 / hubble_h
        ssfr[valid_sfr] = np.log10(sfr[valid_sfr] / stellar_mass_phys)

    # Restrict to the display box before diluting, so the dilution budget is
    # spent on points that will actually be visible.
    keep = select_scatter_sample(mass, ssfr, x_min, x_max, y_min, y_max, dilute)
    mass = mass[keep]
    ssfr = ssfr[keep]

    is_valid, skip_msg = validate_filtered_data(mass, "Specific SFR", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # Plot the model galaxies
    ax.scatter(mass, ssfr, marker="o", s=1, c="k", alpha=0.5, label="Model galaxies")

    # Add a horizontal line at the division between star-forming and quiescent galaxies
    ssfr_cut = -11.0
    ax.axhline(y=ssfr_cut, c="r", ls="--", lw=2)
    # Use IN_FIGURE_TEXT_SIZE for consistent text sizing
    ax.text(11, ssfr_cut + 0.1, "Star-forming", color="b", fontsize=IN_FIGURE_TEXT_SIZE)
    ax.text(11, ssfr_cut - 0.5, "Quiescent", color="r", fontsize=IN_FIGURE_TEXT_SIZE)

    # Customize the plot
    ax.set_ylabel(get_ssfr_label(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_stellar_mass_label(), fontsize=AXIS_LABEL_SIZE)

    # Set the axis limits and minor ticks
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.5))

    # Add consistently styled legend
    setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "SpecificSFR", output_format, verbose)
    return plot_path, None
