#!/usr/bin/env python

"""
SAGE26 Star Formation Rate Plot

Port of the "Plotting the SFR" section of SAGE26's plotting script
(plotting/allresults-local.py:1625-1652): the star formation rate of galaxies with a stellar mass
above 0.01 Msun against stellar mass. The section is the specification; where this port differs
it is a permitted deviation below.

Requires: StellarMass, SfrDisk, SfrBulge

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-local.py:365), SfrDisk and SfrBulge (Msun/yr; the rate is their
sum, :1634).

Permitted deviations: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once, allresults-local.py:357), so the drawn subset differs when the
candidates exceed DILUTE.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._sampling import draw_sample
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26's own `dilute_sfr` (allresults-local.py:1631)
DILUTE = 10000


def total_sfr(sfr_disk, sfr_bulge):
    """
    Return the star formation rate of each galaxy, the disk plus the bulge rate.

    SAGE26 allresults-local.py:1634: `SfrDisk[w2] + SfrBulge[w2]`.
    """
    return sfr_disk + sfr_bulge


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
    Create the star formation rate plot.

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
        required_fields=["StellarMass", "SfrDisk", "SfrBulge"],
        plot_name="Star Formation Rate",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    rng = random.Random(2222)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "star_formation_rate", (6.0, 12.2), (-5.0, 3.0)
    )

    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h

    # allresults-local.py:1630-1634
    w2 = np.where(stellar_mass > 0.01)[0]
    w2 = draw_sample(w2, rng, DILUTE)
    mass = np.log10(stellar_mass[w2])
    starformationrate = total_sfr(galaxies.SfrDisk[w2], galaxies.SfrBulge[w2])

    if verbose:
        print("Star Formation Rate plot debug:")
        print(f"  Number of galaxies plotted: {len(w2)}")

    fig, ax = setup_figure()

    # allresults-local.py:1637. A galaxy with no star formation has log10(0) = -inf and is not
    # drawn, as in SAGE26.
    with np.errstate(divide="ignore", invalid="ignore"):
        log_sfr = np.log10(starformationrate)
    ax.scatter(mass, log_sfr, c="k", marker="x", s=1, alpha=0.9, gid="model")

    ax.set_ylabel(
        r"$\log_{10} \mathrm{SFR}\ (M_{\odot}\ \mathrm{yr^{-1}})$", fontsize=AXIS_LABEL_SIZE
    )
    ax.set_xlabel(r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)

    # allresults-local.py:1643-1647
    ax.xaxis.set_minor_locator(MultipleLocator(0.05))
    ax.yaxis.set_minor_locator(MultipleLocator(0.25))
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    plot_path = save_and_close_figure(fig, output_dir, "StarFormationRate", output_format, verbose)
    return plot_path, None
