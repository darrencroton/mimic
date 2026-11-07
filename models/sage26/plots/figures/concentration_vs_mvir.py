#!/usr/bin/env python

"""
SAGE26 Concentration Against Halo Mass Plot

Port of the "Plotting concentration vs Mvir colored by stellar mass (centrals & satellites)"
section of SAGE26's plotting script (plotting/allresults-local.py:1846-1882): the halo
concentration against halo mass in two panels side by side, centrals on the left and satellites
on the right, with the points coloured by stellar mass on a shared scale. The section is the
specification; where this port differs it is a permitted deviation below.

Requires: Concentration, Mvir, StellarMass, Type

Fields read (units): Concentration (dimensionless), Mvir and StellarMass (1e10 Msun/h, converted
to Msun as SAGE26 converts them: x 1e10 / hubble_h, allresults-local.py:364-365) and Type (0
central, above 0 satellite). The redshift in the titles is metadata["redshift"].

Permitted deviations (c16 Slice 2, row `concentration_vs_mvir`): the sample comes from one
random.Random(2222) per figure, drawn centrals first and then satellites from that one stream,
not from SAGE26's script-wide stream (seeded once, allresults-local.py:357), so the drawn subset
differs when a panel's candidates exceed DILUTE.
"""

import random

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE, setup_plot_fonts
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
)

# Number of galaxies drawn at most per panel: SAGE26's `dilute`, reassigned to 100000 at
# allresults-local.py:1735 and in force here (:1861)
DILUTE = 100000


def draw_sample(candidates, rng):
    """
    Return the galaxies one panel draws: all candidates, or DILUTE of them in sorted order.

    SAGE26 allresults-local.py:1861-1864: `if len(w) > dilute: sel = sorted(sample(list(w),
    dilute)) else: sel = w`, with ``rng`` standing in for SAGE26's script-wide stream.
    """
    if len(candidates) > DILUTE:
        return sorted(rng.sample(list(candidates), DILUTE))
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
    Create the concentration against halo mass plot.

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
        required_fields=["Concentration", "Mvir", "StellarMass", "Type"],
        plot_name="Concentration Vs Mvir",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    redshift = metadata["redshift"]
    rng = random.Random(2222)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "concentration_vs_mvir", (9.5, 15.0), (0.0, 40.0)
    )

    mvir = galaxies.Mvir * 1.0e10 / hubble_h
    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
    concentration = galaxies.Concentration
    galaxy_type = galaxies.Type

    # allresults-local.py:1852-1854
    base = np.where((concentration > 0) & (mvir > 0) & (stellar_mass > 1.0e8))[0]
    w_cen = np.intersect1d(base, np.where(galaxy_type == 0)[0])
    w_sat = np.intersect1d(base, np.where(galaxy_type > 0)[0])

    # Shared colour limits, allresults-local.py:1857-1858. With no galaxy there is no scale.
    if len(base) > 0:
        vmin = np.log10(stellar_mass[base]).min()
        vmax = np.log10(stellar_mass[base]).max()
    else:
        vmin = vmax = None

    if verbose:
        print("Concentration Vs Mvir plot debug:")
        print(f"  Centrals: {len(w_cen)}, satellites: {len(w_sat)}")

    fig, (ax_cen, ax_sat) = plt.subplots(1, 2, figsize=(16, 7), sharey=True)
    setup_plot_fonts(ax_cen)
    setup_plot_fonts(ax_sat)

    # allresults-local.py:1860-1872: centrals then satellites, from one stream
    sc = None
    for ax, w, title in [(ax_cen, w_cen, "Centrals"), (ax_sat, w_sat, "Satellites")]:
        sel = draw_sample(w, rng)
        sc = ax.scatter(
            np.log10(mvir[sel]),
            concentration[sel],
            c=np.log10(stellar_mass[sel]),
            s=3,
            alpha=0.4,
            cmap="viridis",
            vmin=vmin,
            vmax=vmax,
            rasterized=True,
            gid="model",
        )
        ax.set_xlabel(r"$\log_{10} M_{\rm vir}\ [M_{\odot}]$", fontsize=AXIS_LABEL_SIZE)
        ax.set_title(f"{title} — z = {redshift:.2f}", fontsize=AXIS_LABEL_SIZE)
        ax.set_xlim(x_min, x_max)
        ax.set_ylim(y_min, y_max)

    ax_cen.set_ylabel(r"Concentration $c$", fontsize=AXIS_LABEL_SIZE)

    # The colour bar of the satellite panel, allresults-local.py:1876-1878
    cbar = fig.colorbar(sc, ax=ax_sat, pad=0.02)
    cbar.set_label(r"$\log_{10} M_{\star}\ [M_{\odot}]$", fontsize=AXIS_LABEL_SIZE)
    cbar.solids.set_alpha(1.0)

    plot_path = save_and_close_figure(
        fig, output_dir, "concentration_vs_mvir", output_format, verbose
    )
    return plot_path, None
