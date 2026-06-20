#!/usr/bin/env python

"""
Mimic Baryon Fraction Plot

This module generates a plot showing the baryon fraction vs. halo mass.
Adaptively plots all available baryonic components.

Requires: Mvir, Type
Optional: StellarMass, ColdGas, HotGas, EjectedGas, ICS, BlackHoleMass
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from matplotlib.ticker import MaxNLocator, MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
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
    Create a baryon fraction vs. halo mass plot.

    Adaptively plots all available baryonic components. If no baryonic
    properties are available (physics-free mode), displays an informative
    message. Otherwise, plots all available components and their total.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Show detailed output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check required and optional fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=["Mvir", "UniqueCentralGalaxyID"],
        optional_fields=["StellarMass", "ColdGas", "HotGas", "EjectedGas", "ICS", "BlackHoleMass"],
        plot_name="Baryon Fraction",
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    # Get the baryon fraction parameter (or use default cosmic value if not available)
    baryon_frac = params.get("BaryonFrac", 0.17) if params else 0.17

    # Check which optional baryonic properties are available
    has_stellar = optional.get("StellarMass", False)
    has_cold = optional.get("ColdGas", False)
    has_hot = optional.get("HotGas", False)
    has_ejected = optional.get("EjectedGas", False)
    has_ics = optional.get("ICS", False)
    has_bh = optional.get("BlackHoleMass", False)

    # Check if we have any baryonic properties at all
    has_any_baryons = any([has_stellar, has_cold, has_hot, has_ejected, has_ics, has_bh])

    if not has_any_baryons:
        # No baryonic properties available - create plot with message
        msg = "No baryonic properties found\n(Enable physics modules to generate baryon data)"
        if verbose:
            warn(msg)
        return None, msg
    # Only use central galaxies (Type = 0) with non-zero Mvir
    central_mask = (galaxies.Type == 0) & (galaxies.Mvir > 0.0)

    # Check if we have any central galaxies to plot
    if not np.any(central_mask):
        msg = "No central galaxies found with Mvir > 0"
        if verbose:
            warn(msg)
        return None, msg
    # Set up halo mass bins
    min_halo = 11.0
    max_halo = 16.0
    interval = 0.1
    nbins = int((max_halo - min_halo) / interval)
    halo_bins = np.arange(min_halo, max_halo, interval)

    # Arrays to store results
    central_halo_mass = []  # Central halo mass
    mean_baryon_fraction = []  # Total baryon fraction
    mean_stars = []  # Stellar component (including bulge if available)
    mean_cold = []  # Cold gas component
    mean_hot = []  # Hot gas component
    mean_ejected = []  # Ejected gas component
    mean_ics = []  # Intracluster stars component
    mean_bh = []  # Black hole component

    # Pre-compute central galaxy information and halo masses for faster lookup
    valid_mvir = (galaxies.Mvir > 0) & central_mask
    if not np.any(valid_mvir):
        if verbose:
            warn("No central galaxies found with Mvir > 0")
        # Already handled above, but being defensive

    # Compute log halo masses for all valid centrals
    halo_mass = np.full(len(galaxies), -np.inf)  # Initialize with negative infinity
    halo_mass[valid_mvir] = np.log10(galaxies.Mvir[valid_mvir] * 1.0e10 / hubble_h)

    # Loop through halo mass bins
    for i in range(nbins - 1):
        # Get central galaxies in this mass bin
        bin_mask = central_mask & (halo_mass >= halo_bins[i]) & (halo_mass < halo_bins[i + 1])
        centrals_in_bin = np.where(bin_mask)[0]

        # Skip if not enough central galaxies in this bin
        if len(centrals_in_bin) < 3:  # Require at least 3 galaxies for statistics
            continue

        # Get central indices for galaxies in this bin
        central_indices_in_bin = galaxies.UniqueCentralGalaxyID[centrals_in_bin]

        # Create masks for all galaxies belonging to these centrals
        central_groups = np.isin(galaxies.UniqueCentralGalaxyID, central_indices_in_bin)

        # Extract baryonic components for all groups at once (only if available)
        group_central_indices = galaxies.UniqueCentralGalaxyID[central_groups]

        # Get available components
        group_data = {}
        if has_stellar:
            group_data["stellar"] = galaxies.StellarMass[central_groups]
        if has_cold:
            group_data["cold"] = galaxies.ColdGas[central_groups]
        if has_hot:
            group_data["hot"] = galaxies.HotGas[central_groups]
        if has_ejected:
            group_data["ejected"] = galaxies.EjectedGas[central_groups]
        if has_ics:
            group_data["ics"] = galaxies.ICS[central_groups]
        if has_bh:
            group_data["bh"] = galaxies.BlackHoleMass[central_groups]

        # Initialize arrays to hold the sums for each central
        baryon_fractions = np.zeros(len(centrals_in_bin))
        stellar_fractions = np.zeros(len(centrals_in_bin))
        cold_fractions = np.zeros(len(centrals_in_bin))
        hot_fractions = np.zeros(len(centrals_in_bin))
        ejected_fractions = np.zeros(len(centrals_in_bin))
        ics_fractions = np.zeros(len(centrals_in_bin))
        bh_fractions = np.zeros(len(centrals_in_bin))
        halo_masses = np.zeros(len(centrals_in_bin))

        # Process each central galaxy in the bin
        for j, central_idx in enumerate(centrals_in_bin):
            central_gal_index = galaxies.UniqueCentralGalaxyID[central_idx]

            # Find all galaxies in this halo
            group_mask = group_central_indices == central_gal_index

            if np.any(group_mask):
                # Sum components across all galaxies in the halo (only available ones)
                stars = np.sum(group_data["stellar"][group_mask]) if has_stellar else 0.0
                cold = np.sum(group_data["cold"][group_mask]) if has_cold else 0.0
                hot = np.sum(group_data["hot"][group_mask]) if has_hot else 0.0
                ejected = np.sum(group_data["ejected"][group_mask]) if has_ejected else 0.0
                ics = np.sum(group_data["ics"][group_mask]) if has_ics else 0.0
                bh = np.sum(group_data["bh"][group_mask]) if has_bh else 0.0

                # Total baryons (only sum what's available)
                baryons = stars + cold + hot + ejected + ics + bh

                # Calculate fractions relative to halo mass
                baryon_fractions[j] = baryons / galaxies.Mvir[central_idx]
                stellar_fractions[j] = stars / galaxies.Mvir[central_idx]
                cold_fractions[j] = cold / galaxies.Mvir[central_idx]
                hot_fractions[j] = hot / galaxies.Mvir[central_idx]
                ejected_fractions[j] = ejected / galaxies.Mvir[central_idx]
                ics_fractions[j] = ics / galaxies.Mvir[central_idx]
                bh_fractions[j] = bh / galaxies.Mvir[central_idx]

                # Store the central halo mass (log10, in Msun)
                halo_masses[j] = np.log10(galaxies.Mvir[central_idx] * 1.0e10 / hubble_h)

        # Calculate means for this bin
        central_halo_mass.append(np.mean(halo_masses))
        mean_baryon_fraction.append(np.mean(baryon_fractions))
        mean_stars.append(np.mean(stellar_fractions))
        mean_cold.append(np.mean(cold_fractions))
        mean_hot.append(np.mean(hot_fractions))
        mean_ejected.append(np.mean(ejected_fractions))
        mean_ics.append(np.mean(ics_fractions))
        mean_bh.append(np.mean(bh_fractions))

    # Convert to numpy arrays
    central_halo_mass = np.array(central_halo_mass)
    mean_baryon_fraction = np.array(mean_baryon_fraction)
    mean_stars = np.array(mean_stars)
    mean_cold = np.array(mean_cold)
    mean_hot = np.array(mean_hot)
    mean_ejected = np.array(mean_ejected)
    mean_ics = np.array(mean_ics)
    mean_bh = np.array(mean_bh)

    # Print debug information if verbose mode is enabled
    if verbose:
        print(f"Baryon Fraction plot debug:")
        print(f"  Number of mass bins with data: {len(central_halo_mass)}")
        if len(central_halo_mass) > 0:
            print(
                f"  Halo mass range: {min(central_halo_mass):.2f} to {max(central_halo_mass):.2f}"
            )
            print(
                f"  Mean baryon fraction range: {min(mean_baryon_fraction):.3f} to {max(mean_baryon_fraction):.3f}"
            )
        print(f"  Cosmic baryon fraction (parameter): {baryon_frac:.3f}")
        print(f"  Available components:")
        print(f"    Stars: {has_stellar}")
        print(f"    Cold gas: {has_cold}")
        print(f"    Hot gas: {has_hot}")
        print(f"    Ejected gas: {has_ejected}")
        print(f"    ICS: {has_ics}")
        print(f"    Black holes: {has_bh}")

    # Check if we have any data to plot
    if len(central_halo_mass) == 0:
        msg = "Insufficient data for baryon fraction analysis\n(Not enough central galaxies in mass bins)"
        if verbose:
            warn(msg)
        return None, msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    # Plot the results
    # Total baryon fraction
    ax.plot(central_halo_mass, mean_baryon_fraction, "k-", lw=2, label="TOTAL")

    # Individual components (only plot if available and non-zero)
    if has_stellar:
        if np.any(mean_stars > 0):
            ax.plot(central_halo_mass, mean_stars, "k--", label="Stars")

    if has_cold:
        if np.any(mean_cold > 0):
            ax.plot(central_halo_mass, mean_cold, "b-", label="Cold")

    if has_hot:
        if np.any(mean_hot > 0):
            ax.plot(central_halo_mass, mean_hot, "r-", label="Hot")

    if has_ejected:
        if np.any(mean_ejected > 0):
            ax.plot(central_halo_mass, mean_ejected, "g-", label="Ejected")

    if has_ics:
        if np.any(mean_ics > 0):
            ax.plot(central_halo_mass, mean_ics, "y-", label="ICS")

    # Black hole mass is typically too small to show up well, but plot if available
    if has_bh:
        if np.any(mean_bh > 1e-6):  # Only if non-negligible
            ax.plot(central_halo_mass, mean_bh, "k:", label="BH")

    # Add a horizontal line showing the cosmic baryon fraction
    ax.axhline(
        y=baryon_frac,
        color="k",
        linestyle=":",
        lw=1.5,
        label=f"Cosmic: {baryon_frac:.2f}",
    )

    # Customize the plot
    ax.set_xlabel(r"Central log$_{10}$ M$_{\rm vir}$ [M$_{\odot}$]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"Baryon Fraction", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks with MaxNLocator to avoid excessive ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MaxNLocator(10))

    # Set axis limits
    y_max_data = max(0.23, max(mean_baryon_fraction) * 1.1)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "baryon_fraction", (10.8, 15.0), (0.0, y_max_data)
    )
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    # Add consistently styled legend
    leg = setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "BaryonFraction", output_format, verbose)
    return plot_path, None
