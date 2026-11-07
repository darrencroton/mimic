#!/usr/bin/env python

"""
SAGE26 Mass Loading Against Virial Velocity Plot

Port of the "Plotting mass loading factor vs Vvir" section of SAGE26's plotting script
(plotting/allresults-local.py:1656-1683): the supernova mass-loading factor of galaxies with
stellar mass and a positive mass loading against the virial velocity. The section is the
specification; where this port differs it is a permitted deviation below.

Requires: StellarMass, MassLoading, Vvir

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-local.py:365), MassLoading (dimensionless) and Vvir (km/s).

Permitted deviations: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once, allresults-local.py:357), so the drawn subset differs when the
candidates exceed DILUTE. SAGE26 prints a skip notice and draws nothing when no galaxy has
MassLoading > 0 (:1662-1665); this figure draws its empty axes instead, as it skips only for
a missing field.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE
from output_utils import (
    check_required_fields,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26's script-level `dilute` (allresults-local.py:73),
# in force here (it is reassigned only at :1735)
DILUTE = 7500


def draw_sample(candidates, rng):
    """
    Return the galaxies to draw: all candidates, or a random sample of DILUTE of them.

    SAGE26 allresults-local.py:1667: `if(len(w) > dilute): w = sample(list(w), dilute)`, with
    ``rng`` standing in for SAGE26's script-wide stream.
    """
    if len(candidates) > DILUTE:
        return rng.sample(list(candidates), DILUTE)
    return candidates


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

    # allresults-local.py:1661-1670
    w = np.where((stellar_mass > 0.0) & (galaxies.MassLoading > 0.0))[0]
    w = draw_sample(w, rng)
    mass_loading = galaxies.MassLoading[w]
    # A galaxy without a halo has Vvir 0 and log10 = -inf, which is not drawn, as in SAGE26
    with np.errstate(divide="ignore"):
        log_vvir = np.log10(galaxies.Vvir[w])

    if verbose:
        print("Mass Loading Vs Vvir plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")

    fig, ax = setup_figure()

    # allresults-local.py:1672. SAGE26 sets no axis window here (:1677-1678 are commented out).
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
