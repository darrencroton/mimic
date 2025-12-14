#!/usr/bin/env python

"""
Mimic Star Formation Rate Density Evolution Plot

This module generates a plot of the star formation rate density evolution from Mimic galaxy data.
Requires: Sfr property (from galaxy physics modules)
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    LEGEND_FONT_SIZE,
    get_redshift_label,
    get_sfr_density_label,
    setup_legend,
    setup_plot_fonts,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    warn,
    check_field_has_values,
    check_required_fields,
        setup_figure,
    validate_evolution_snapshot,
    save_and_close_figure,
)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create a star formation rate density evolution plot.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output

    verbose: Whether to print verbose output


    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check if we have any snapshots
    if len(snapshots) == 0:
        return None, "No snapshot data available for SFR density evolution plot"
    # Check if SFR field is available in the data
    # Get the first snapshot to check for field availability
    first_snap = next(iter(snapshots.values()))
    galaxies_sample = first_snap[0]

    success, optional, msg = check_required_fields(
        galaxies_sample,
        required_fields=['Sfr'],
        plot_name='SFR Density Evolution'
    )

    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation: Check if Sfr has any non-zero values
    has_sfr, count, msg = check_field_has_values(
        galaxies_sample.Sfr, 'Sfr', threshold=0.0
    )
    if not has_sfr:
        return None, f"Field validation failed: {msg}"

    # Calculate SFR density for each snapshot BEFORE creating figure
    sfr_density = []
    redshifts = []

    for snap, (galaxies, volume, metadata) in snapshots.items():
        # Get the redshift for this snapshot
        redshift = metadata.get("redshift", 0.0)
        redshifts.append(redshift)

        # Extract hubble_h from metadata
        hubble_h = metadata.get("hubble_h", 0.73)

        # Skip if volume is zero (no valid data)
        if volume == 0:
            sfr_density.append(0.0)
            continue

        # Sum SFR and normalize by volume
        sfr_sum = np.sum(galaxies.Sfr)
        sfr_density.append(sfr_sum / volume * hubble_h**3)

    # Convert to numpy arrays
    redshifts = np.array(redshifts)
    sfr_density = np.array(sfr_density)

    # Sort by redshift
    sort_idx = np.argsort(redshifts)
    redshifts = redshifts[sort_idx]
    sfr_density = sfr_density[sort_idx]

    # Debug information
    if verbose:
        print(f"  Number of snapshots: {len(snapshots)}")
        print(f"  Redshifts available: {redshifts}")
        print(f"  SFR density values: {sfr_density}")

    # Check if we have any nonzero SFR density values
    nonzero = np.where(sfr_density > 0.0)[0]
    if len(nonzero) == 0:
        return None, "No nonzero SFR density values found across all snapshots"

    if verbose:
        print(f"  Plotting {len(nonzero)} nonzero SFR density points")

    # NOW create the figure (only after all validation passed)
    fig, ax = setup_figure()

    # Add observational data (compilation used in many papers)
    ObsSFRdensity = np.array(
        [
            [0, 0.0158489, 0, 0, 0.0251189, 0.01000000],
            [0.150000, 0.0173780, 0, 0.300000, 0.0181970, 0.0165959],
            [0.0425000, 0.0239883, 0.0425000, 0.0425000, 0.0269153, 0.0213796],
            [0.200000, 0.0295121, 0.100000, 0.300000, 0.0323594, 0.0269154],
            [0.350000, 0.0147911, 0.200000, 0.500000, 0.0173780, 0.0125893],
            [0.625000, 0.0275423, 0.500000, 0.750000, 0.0331131, 0.0229087],
            [0.825000, 0.0549541, 0.750000, 1.00000, 0.0776247, 0.0389045],
            [0.625000, 0.0794328, 0.500000, 0.750000, 0.0954993, 0.0660693],
            [0.700000, 0.0323594, 0.575000, 0.825000, 0.0371535, 0.0281838],
            [1.25000, 0.0467735, 1.50000, 1.00000, 0.0660693, 0.0331131],
            [0.750000, 0.0549541, 0.500000, 1.00000, 0.0389045, 0.0776247],
            [1.25000, 0.0741310, 1.00000, 1.50000, 0.0524807, 0.104713],
            [1.75000, 0.0562341, 1.50000, 2.00000, 0.0398107, 0.0794328],
            [2.75000, 0.0794328, 2.00000, 3.50000, 0.0562341, 0.112202],
            [4.00000, 0.0309030, 3.50000, 4.50000, 0.0489779, 0.0194984],
            [0.250000, 0.0398107, 0.00000, 0.500000, 0.0239883, 0.0812831],
            [0.750000, 0.0446684, 0.500000, 1.00000, 0.0323594, 0.0776247],
            [1.25000, 0.0630957, 1.00000, 1.50000, 0.0478630, 0.109648],
            [1.75000, 0.0645654, 1.50000, 2.00000, 0.0489779, 0.112202],
            [2.50000, 0.0831764, 2.00000, 3.00000, 0.0512861, 0.158489],
            [3.50000, 0.0776247, 3.00000, 4.00000, 0.0416869, 0.169824],
            [4.50000, 0.0977237, 4.00000, 5.00000, 0.0416869, 0.269153],
            [5.50000, 0.0426580, 5.00000, 6.00000, 0.0177828, 0.165959],
            [3.00000, 0.120226, 2.00000, 4.00000, 0.173780, 0.0831764],
            [3.04000, 0.128825, 2.69000, 3.39000, 0.151356, 0.109648],
            [4.13000, 0.114815, 3.78000, 4.48000, 0.144544, 0.0912011],
            [0.350000, 0.0346737, 0.200000, 0.500000, 0.0537032, 0.0165959],
            [0.750000, 0.0512861, 0.500000, 1.00000, 0.0575440, 0.0436516],
            [1.50000, 0.0691831, 1.00000, 2.00000, 0.0758578, 0.0630957],
            [2.50000, 0.147911, 2.00000, 3.00000, 0.169824, 0.128825],
            [3.50000, 0.0645654, 3.00000, 4.00000, 0.0776247, 0.0512861],
        ],
        dtype=np.float32,
    )

    ObsRedshift = ObsSFRdensity[:, 0]
    xErrLo = np.abs(ObsSFRdensity[:, 0] - ObsSFRdensity[:, 2])
    xErrHi = np.abs(ObsSFRdensity[:, 3] - ObsSFRdensity[:, 0])

    ObsSFR = np.log10(ObsSFRdensity[:, 1])
    yErrLo = np.abs(np.log10(ObsSFRdensity[:, 1]) - np.log10(ObsSFRdensity[:, 4]))
    yErrHi = np.abs(np.log10(ObsSFRdensity[:, 5]) - np.log10(ObsSFRdensity[:, 1]))

    # Plot observational data
    ax.errorbar(
        ObsRedshift,
        ObsSFR,
        yerr=[yErrLo, yErrHi],
        xerr=[xErrLo, xErrHi],
        color="g",
        lw=1.0,
        alpha=0.3,
        marker="o",
        ls="none",
        label="Observations",
    )

    # Plot the model results (nonzero was already validated above)
    ax.plot(
        redshifts[nonzero],
        np.log10(sfr_density[nonzero]),
        "b-",
        lw=3.0,
        label="Model",
    )

    # Customize the plot
    ax.set_ylabel(get_sfr_density_label(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_redshift_label(), fontsize=AXIS_LABEL_SIZE)

    ax.xaxis.set_minor_locator(MultipleLocator(1))
    ax.yaxis.set_minor_locator(MultipleLocator(0.5))

    ax.set_xlim(0.0, 8.0)
    ax.set_ylim(-3.0, -0.4)

    # Add consistently styled legend
    setup_legend(ax, loc="upper right")

    # Save and close the figure
    plot_path = save_and_close_figure(fig, output_dir, "SFR_Density_Evolution", output_format, verbose)
    return plot_path, None