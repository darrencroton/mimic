#!/usr/bin/env python

"""
SAGE26 Star Formation Rate Against Halo Accretion Rate Plot

Follows the "Plotting SFR vs halo accretion rate" section of SAGE26's plotting script: the star
formation rate against the gas accretion rate (the cooling rate plus the cold-stream rate) of
galaxies with both positive, coloured by stellar mass, with the line of star formation rate equal to
accretion rate. Where it differs from SAGE26 the difference is stated below.

Requires: SfrDisk, SfrBulge, mdot_cool, mdot_stream, StellarMass

Fields read (units): SfrDisk and SfrBulge (Msun/yr; the rate is their sum), mdot_cool and
mdot_stream (Msun/yr; SAGE26's MdotCool and MdotStream) and StellarMass (1e10 Msun/h, converted to
Msun as SAGE26 converts it: x 1e10 / hubble_h). The redshift in the title is metadata["redshift"].

Differences from SAGE26: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once), so the drawn subset differs when the candidates exceed DILUTE.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from figures._sampling import draw_sample
from figures.star_formation_rate import total_sfr
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26's `dilute` as in force at this figure
DILUTE = 100000


def halo_accretion_rate(mdot_cool, mdot_stream):
    """
    Return the gas accretion rate of each galaxy, the cooling rate plus the cold-stream rate.

    As in SAGE26: `MdotCool + MdotStream`.
    """
    return mdot_cool + mdot_stream


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
    Create the star formation rate against accretion rate plot.

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
        required_fields=["SfrDisk", "SfrBulge", "mdot_cool", "mdot_stream", "StellarMass"],
        plot_name="SFR Vs Halo Accretion Rate",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    redshift = metadata["redshift"]
    rng = random.Random(2222)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "sfr_vs_halo_accretion_rate", (0.0, 4.0), (-6.0, 4.0)
    )

    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h

    SFR = total_sfr(galaxies.SfrDisk, galaxies.SfrBulge)
    HaloAccretionRate = halo_accretion_rate(galaxies.mdot_cool, galaxies.mdot_stream)
    # SAGE parity: SAGE26 comments out `(Type == 0) &` in the selection, so satellites are drawn,
    # while the title says "Centrals".
    w = np.where((SFR > 0.0) & (HaloAccretionRate > 0.0))[0]
    w = draw_sample(w, rng, DILUTE)

    log_acc = np.log10(HaloAccretionRate[w])
    log_sfr = np.log10(SFR[w])

    # Colour points by stellar mass
    log_mstar = np.log10(np.maximum(stellar_mass[w], 1.0))

    if verbose:
        print("SFR Vs Halo Accretion Rate plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")

    fig, ax = setup_figure()

    sc = ax.scatter(
        log_acc,
        log_sfr,
        c=log_mstar,
        s=2,
        alpha=0.5,
        cmap="viridis",
        rasterized=True,
        gid="model",
    )
    cbar = fig.colorbar(sc, ax=ax, pad=0.02)
    cbar.set_label(r"$\log_{10} M_{\star}\ [M_{\odot}]$", fontsize=AXIS_LABEL_SIZE)
    cbar.solids.set_alpha(1.0)

    # 1:1 reference line
    x_ref = np.array([-4.0, 4.0])
    ax.plot(x_ref, x_ref, "k--", lw=1.2, alpha=0.5, label="SFR = accretion rate")

    ax.set_xlabel(
        r"$\log_{10}\ \dot{M}_{\rm acc}\ (M_{\odot}\ \mathrm{yr}^{-1})$", fontsize=AXIS_LABEL_SIZE
    )
    ax.set_ylabel(
        r"$\log_{10}\ \mathrm{SFR}\ (M_{\odot}\ \mathrm{yr}^{-1})$", fontsize=AXIS_LABEL_SIZE
    )
    ax.set_title(f"Centrals — z = {redshift:.2f}", fontsize=AXIS_LABEL_SIZE)

    ax.xaxis.set_minor_locator(MultipleLocator(0.25))
    ax.yaxis.set_minor_locator(MultipleLocator(0.25))
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    setup_legend(ax, loc="upper left")

    plot_path = save_and_close_figure(
        fig, output_dir, "sfr_vs_halo_accretion_rate", output_format, verbose
    )
    return plot_path, None
