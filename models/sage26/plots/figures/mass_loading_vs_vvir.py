#!/usr/bin/env python

"""
SAGE26 Mass Loading Against Virial Velocity Plot

Follows the "Plotting mass loading factor vs Vvir" section of SAGE26's plotting script: the
supernova mass-loading factor of galaxies with stellar mass and a positive mass loading against the
virial velocity. Where it differs from SAGE26 the difference is stated below.

Requires: StellarMass, MassLoading, Vvir

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it: x 1e10 /
hubble_h), MassLoading (dimensionless) and Vvir (km/s).

Differences from SAGE26: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once), so the drawn subset differs when the candidates exceed DILUTE.
SAGE26 prints a skip notice and draws nothing when no galaxy has MassLoading > 0; this figure draws
its empty axes instead, as it skips only for a missing field.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._sampling import draw_sample
from output_utils import (
    check_required_fields,
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
    Create the mass loading against virial velocity plot.

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
        required_fields=["StellarMass", "MassLoading", "Vvir"],
        plot_name="Mass Loading Vs Vvir",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    rng = random.Random(2222)

    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h

    w = np.where((stellar_mass > 0.0) & (galaxies.MassLoading > 0.0))[0]
    w = draw_sample(w, rng, DILUTE)
    mass_loading = galaxies.MassLoading[w]
    # A galaxy without a halo has Vvir 0 and log10 = -inf, which is not drawn, as in SAGE26
    with np.errstate(divide="ignore"):
        log_vvir = np.log10(galaxies.Vvir[w])

    if verbose:
        print("Mass Loading Vs Vvir plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")

    fig, ax = setup_figure()

    # SAGE26 sets no axis window here (its limits are commented out).
    ax.scatter(log_vvir, mass_loading, c="k", marker="x", s=1, alpha=0.9, gid="model")

    ax.set_xlabel(r"$\log_{10} V_{\mathrm{vir}}\ (\mathrm{km/s})$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(
        r"$\eta_{\mathrm{reheat}} = \dot{M}_{\mathrm{reheat}} / \dot{M}_{\star}$",
        fontsize=AXIS_LABEL_SIZE,
    )

    plot_path = save_and_close_figure(
        fig, output_dir, "mass_loading_vs_vvir", output_format, verbose
    )
    return plot_path, None
