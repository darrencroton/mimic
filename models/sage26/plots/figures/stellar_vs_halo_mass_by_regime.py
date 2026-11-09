#!/usr/bin/env python

"""
SAGE26 Stellar Mass Against Halo Mass By Regime Plot

Follows the "Plotting stellar mass vs halo mass colored by regime" section of SAGE26's plotting
script: the stellar mass of galaxies against the mass of their halo, the circumgalactic-medium
regime in blue and the hot regime in red. Where it differs from SAGE26 the difference is stated
below.

Requires: Mvir, StellarMass, Regime

Fields read (units): Mvir and StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts them: x
1e10 / hubble_h) and Regime (0 circumgalactic medium, 1 hot; other values are drawn in neither
series).

Differences from SAGE26: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once), so the drawn subset differs when the candidates exceed DILUTE.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from figures._sampling import draw_sample
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26's script-level `dilute`, in force here
DILUTE = 7500


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
    Create the stellar mass against halo mass plot, split by regime.

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
            - skip_message (str or None): Reason for skipping if a field is missing
    """
    success, _, msg = check_required_fields(
        galaxies,
        required_fields=["Mvir", "StellarMass", "Regime"],
        plot_name="Stellar Vs Halo Mass By Regime",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    rng = random.Random(2222)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "stellar_vs_halo_mass_by_regime", (10.0, 15.0), (8.0, 12.0)
    )

    mvir = galaxies.Mvir * 1.0e10 / hubble_h
    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h

    w = np.where((mvir > 0.0) & (stellar_mass > 0.0))[0]
    w = draw_sample(w, rng, DILUTE)
    log10_halo_mass = np.log10(mvir[w])
    log10_stellar_mass = np.log10(stellar_mass[w])
    regime_values = galaxies.Regime[w]

    # Separate the data by regime
    cgm_regime = regime_values == 0
    hot_regime = regime_values == 1

    if verbose:
        print("Stellar Vs Halo Mass By Regime plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")
        print(f"  CGM regime: {int(np.count_nonzero(cgm_regime))}")
        print(f"  Hot regime: {int(np.count_nonzero(hot_regime))}")

    fig, ax = setup_figure()

    # A regime with no galaxy draws nothing
    if np.any(cgm_regime):
        ax.scatter(
            log10_halo_mass[cgm_regime],
            log10_stellar_mass[cgm_regime],
            c="blue",
            s=1,
            alpha=0.6,
            label="CGM Regime",
            gid="model",
        )
    if np.any(hot_regime):
        ax.scatter(
            log10_halo_mass[hot_regime],
            log10_stellar_mass[hot_regime],
            c="red",
            s=1,
            alpha=0.6,
            label="Hot Regime",
            gid="model",
        )

    ax.set_xlabel(r"$\log_{10} M_{\mathrm{vir}}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    setup_legend(ax, loc="upper left")

    plot_path = save_and_close_figure(
        fig, output_dir, "stellar_vs_halo_mass_by_regime", output_format, verbose
    )
    return plot_path, None
