#!/usr/bin/env python

"""
Mimic Baryon Fraction Plot

This module generates a plot showing the baryon fraction vs. halo mass.
Adaptively plots all available baryonic components.

Requires: Mvir, Type
Optional: StellarMass, ColdGas, HotGas, EjectedMass, ICS, BlackHoleMass, BulgeMass
"""

import os

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    setup_legend,
    setup_plot_fonts,
)
from matplotlib.ticker import MultipleLocator, MaxNLocator


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
        Path to the saved plot file
    """
    # Set up the figure
    fig, ax = plt.subplots(figsize=(8, 6))

    # Apply consistent font settings
    setup_plot_fonts(ax)

    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    # Get the baryon fraction parameter (or use default cosmic value if not available)
    baryon_frac = params.get("BaryonFrac", 0.17) if params else 0.17

    # Check which baryonic properties are available
    available_fields = set(galaxies.dtype.names)
    has_stellar = "StellarMass" in available_fields
    has_cold = "ColdGas" in available_fields
    has_hot = "HotGas" in available_fields
    has_ejected = "EjectedMass" in available_fields
    has_ics = "ICS" in available_fields
    has_bh = "BlackHoleMass" in available_fields
    has_bulge = "BulgeMass" in available_fields

    # Check if we have any baryonic properties at all
    has_any_baryons = any(
        [has_stellar, has_cold, has_hot, has_ejected, has_ics, has_bh, has_bulge]
    )

    if not has_any_baryons:
        # No baryonic properties available - create plot with message
        if verbose:
            print("No baryonic properties found in galaxy data")

        ax.text(
            0.5,
            0.5,
            "No baryonic properties found\n(Enable physics modules to generate baryon data)",
            horizontalalignment="center",
            verticalalignment="center",
            transform=ax.transAxes,
            fontsize=IN_FIGURE_TEXT_SIZE,
        )

        # Set up basic axes labels
        ax.set_xlabel(
            r"Central log$_{10}$ M$_{\rm vir}$ (M$_{\odot}$)",
            fontsize=AXIS_LABEL_SIZE,
        )
        ax.set_ylabel(r"Baryon Fraction", fontsize=AXIS_LABEL_SIZE)
        ax.set_xlim(10.8, 15.0)
        ax.set_ylim(0.0, 0.23)

        # Save the figure
        os.makedirs(output_dir, exist_ok=True)
        output_path = os.path.join(output_dir, f"BaryonFraction{output_format}")
        if verbose:
            print(f"Saving Baryon Fraction plot (no data) to: {output_path}")
        plt.savefig(output_path)
        plt.close()
        return output_path

    # Only use central galaxies (Type = 0) with non-zero Mvir
    central_mask = (galaxies.Type == 0) & (galaxies.Mvir > 0.0)

    # Check if we have any central galaxies to plot
    if not np.any(central_mask):
        if verbose:
            print("No central galaxies found with Mvir > 0")

        ax.text(
            0.5,
            0.5,
            "No central galaxies found with Mvir > 0",
            horizontalalignment="center",
            verticalalignment="center",
            transform=ax.transAxes,
            fontsize=IN_FIGURE_TEXT_SIZE,
        )

        # Save the figure
        os.makedirs(output_dir, exist_ok=True)
        output_path = os.path.join(output_dir, f"BaryonFraction{output_format}")
        plt.savefig(output_path)
        plt.close()
        return output_path

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
            print("No central galaxies found with Mvir > 0")
        # Already handled above, but being defensive

    # Compute log halo masses for all valid centrals
    halo_mass = np.full(len(galaxies), -np.inf)  # Initialize with negative infinity
    halo_mass[valid_mvir] = np.log10(galaxies.Mvir[valid_mvir] * 1.0e10 / hubble_h)

    # Loop through halo mass bins
    for i in range(nbins - 1):
        # Get central galaxies in this mass bin
        bin_mask = (
            central_mask & (halo_mass >= halo_bins[i]) & (halo_mass < halo_bins[i + 1])
        )
        centrals_in_bin = np.where(bin_mask)[0]

        # Skip if not enough central galaxies in this bin
        if len(centrals_in_bin) < 3:  # Require at least 3 galaxies for statistics
            continue

        # Get central indices for galaxies in this bin
        central_indices_in_bin = galaxies.CentralHaloIndex[centrals_in_bin]

        # Create masks for all galaxies belonging to these centrals
        central_groups = np.isin(galaxies.CentralHaloIndex, central_indices_in_bin)

        # Extract baryonic components for all groups at once (only if available)
        group_central_indices = galaxies.CentralHaloIndex[central_groups]

        # Get available components
        group_data = {}
        if has_stellar:
            group_data["stellar"] = galaxies.StellarMass[central_groups]
        if has_bulge:
            group_data["bulge"] = galaxies.BulgeMass[central_groups]
        if has_cold:
            group_data["cold"] = galaxies.ColdGas[central_groups]
        if has_hot:
            group_data["hot"] = galaxies.HotGas[central_groups]
        if has_ejected:
            group_data["ejected"] = galaxies.EjectedMass[central_groups]
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
            central_gal_index = galaxies.CentralHaloIndex[central_idx]

            # Find all galaxies in this halo
            group_mask = group_central_indices == central_gal_index

            if np.any(group_mask):
                # Sum components across all galaxies in the halo (only available ones)
                stars = 0.0
                if has_stellar:
                    stars += np.sum(group_data["stellar"][group_mask])
                if has_bulge:
                    stars += np.sum(
                        group_data["bulge"][group_mask]
                    )  # Add bulge to stellar

                cold = np.sum(group_data["cold"][group_mask]) if has_cold else 0.0
                hot = np.sum(group_data["hot"][group_mask]) if has_hot else 0.0
                ejected = (
                    np.sum(group_data["ejected"][group_mask]) if has_ejected else 0.0
                )
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
        print(f"    Stars: {has_stellar or has_bulge}")
        print(f"    Cold gas: {has_cold}")
        print(f"    Hot gas: {has_hot}")
        print(f"    Ejected gas: {has_ejected}")
        print(f"    ICS: {has_ics}")
        print(f"    Black holes: {has_bh}")

    # Check if we have any data to plot
    if len(central_halo_mass) == 0:
        if verbose:
            print("No data in mass bins (all bins had < 3 centrals)")

        ax.text(
            0.5,
            0.5,
            "Insufficient data for baryon fraction analysis\n(Not enough central galaxies in mass bins)",
            horizontalalignment="center",
            verticalalignment="center",
            transform=ax.transAxes,
            fontsize=IN_FIGURE_TEXT_SIZE,
        )

        # Set up basic axes labels
        ax.set_xlabel(
            r"Central log$_{10}$ M$_{\rm vir}$ (M$_{\odot}$)",
            fontsize=AXIS_LABEL_SIZE,
        )
        ax.set_ylabel(r"Baryon Fraction", fontsize=AXIS_LABEL_SIZE)
        ax.set_xlim(10.8, 15.0)
        ax.set_ylim(0.0, 0.23)

        # Save the figure
        os.makedirs(output_dir, exist_ok=True)
        output_path = os.path.join(output_dir, f"BaryonFraction{output_format}")
        plt.savefig(output_path)
        plt.close()
        return output_path

    # Plot the results
    # Total baryon fraction
    ax.plot(central_halo_mass, mean_baryon_fraction, "k-", lw=2, label="TOTAL")

    # Fill between to show variance
    variance = np.zeros_like(mean_baryon_fraction) + 0.02  # Simple approximation
    ax.fill_between(
        central_halo_mass,
        np.clip(mean_baryon_fraction - variance, 0, None),
        np.clip(mean_baryon_fraction + variance, 0, None),
        facecolor="purple",
        alpha=0.25,
    )

    # Individual components (only plot if available and non-zero)
    if has_stellar or has_bulge:
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
    ax.set_xlabel(
        r"Central log$_{10}$ M$_{\rm vir}$ (M$_{\odot}$)", fontsize=AXIS_LABEL_SIZE
    )
    ax.set_ylabel(r"Baryon Fraction", fontsize=AXIS_LABEL_SIZE)

    # Set the x and y axis minor ticks with MaxNLocator to avoid excessive ticks
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MaxNLocator(10))

    # Set axis limits - matching the original plot
    ax.set_xlim(10.8, 15.0)
    ax.set_ylim(0.0, max(0.23, max(mean_baryon_fraction) * 1.1))

    # Add consistently styled legend
    leg = setup_legend(ax, loc="upper right")

    # Save the figure, ensuring the output directory exists
    try:
        os.makedirs(output_dir, exist_ok=True)
    except Exception as e:
        if verbose:
            print(f"Warning: Could not create output directory {output_dir}: {e}")
        # Try to use a subdirectory of the current directory as fallback
        output_dir = "./plots"
        os.makedirs(output_dir, exist_ok=True)

    output_path = os.path.join(output_dir, f"BaryonFraction{output_format}")
    if verbose:
        print(f"Saving Baryon Fraction plot to: {output_path}")
    plt.savefig(output_path)
    plt.close()

    return output_path
