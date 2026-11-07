#!/usr/bin/env python

"""
SAGE26 Specific Star Formation Rate Density Plot

Port of the "Plotting specific SFR vs stellar mass" section of SAGE26's plotting script
(plotting/allresults-local.py:1728-1842): the density of galaxies in specific star formation
rate against stellar mass, as smoothed filled contours of the quiescent (red) and star-forming
(blue) populations either side of the specific star formation rate cut at -11 yr^-1. The
section is the specification; where this port differs it is a permitted deviation below.

Requires: StellarMass, SfrDisk, SfrBulge

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-local.py:365), SfrDisk and SfrBulge (Msun/yr; the rate is their
sum, :1740).

Permitted deviations: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once, allresults-local.py:357), so the drawn subset differs when the
candidates exceed DILUTE.
"""

import random
import warnings

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26 reassigns its script-level `dilute` to 100000
# here (allresults-local.py:1735), and the value holds for every later section
DILUTE = 100000

# Specific star formation rate dividing quiescent from star-forming galaxies (:1745), in yr^-1
SSFR_CUT = -11.0

# Grid of the density calculation, allresults-local.py:1758-1759
X_BINS = np.linspace(8.0, 12.2, 100)
Y_BINS = np.linspace(-13, -8, 100)

# Value of the specific star formation rate of a galaxy with no stellar mass (:1741)
NO_SSFR = -99.0


def draw_sample(candidates, rng):
    """
    Return the positions SAGE26 draws: all candidates, or DILUTE random positions.

    SAGE26 allresults-local.py:1738: `if(len(w2) > dilute): w2 = sample(list(range(len(w2))),
    dilute)`, with ``rng`` standing in for SAGE26's script-wide stream. The positions are drawn
    from range(len(w2)), not from w2 itself.
    """
    if len(candidates) > DILUTE:
        return rng.sample(list(range(len(candidates))), DILUTE)
    return candidates


def log_specific_sfr(sfr_disk, sfr_bulge, stellar_mass):
    """
    Return log10 of the specific star formation rate, -99 where there is no stellar mass.

    SAGE26 allresults-local.py:1740-1743: the rate is SfrDisk + SfrBulge; sSFR starts at -99 and
    holds log10(rate / StellarMass) where StellarMass > 0.
    """
    starformationrate = sfr_disk + sfr_bulge
    sSFR = np.full_like(starformationrate, NO_SSFR)
    mask = stellar_mass > 0
    with np.errstate(divide="ignore", invalid="ignore"):
        sSFR[mask] = np.log10(starformationrate[mask] / stellar_mass[mask])
    return sSFR


def smoothed_histogram(x, y, clip_above=None, clip_below=None):
    """
    Return the smoothed 2-D histogram of a population and the bin centres of its grid.

    SAGE26 allresults-local.py:1767-1783: np.histogram2d on X_BINS and Y_BINS, transposed,
    smoothed with a Gaussian filter of sigma 1.5, then zeroed in the rows whose centre lies
    above ``clip_above`` or below ``clip_below``.

    Returns:
        (H_smooth, x_centers, y_centers)
    """
    from scipy.ndimage import gaussian_filter

    H, xedges, yedges = np.histogram2d(x, y, bins=[X_BINS, Y_BINS])
    H = H.T  # Transpose for correct orientation

    H_smooth = gaussian_filter(H, sigma=1.5)

    y_centers = (yedges[:-1] + yedges[1:]) / 2
    if clip_above is not None:
        mask_2d = y_centers[:, np.newaxis] <= clip_above
        H_smooth = H_smooth * mask_2d
    if clip_below is not None:
        mask_2d = y_centers[:, np.newaxis] >= clip_below
        H_smooth = H_smooth * mask_2d
    x_centers = (xedges[:-1] + xedges[1:]) / 2
    return H_smooth, x_centers, y_centers


def sigma_levels(H_smooth):
    """
    Return the (3 sigma, 2 sigma, 1 sigma) contour levels of a smoothed histogram, or None.

    SAGE26 allresults-local.py:1786-1798: the non-zero cells sorted from the highest, their
    cumulative sum normalised to 1; each level is the first cell value at which the cumulative
    fraction reaches 0.997, 0.95 and 0.68 (the lowest non-zero value if it never does). None
    where every cell is zero.
    """
    sorted_H = np.sort(H_smooth.flatten())[::-1]
    sorted_H = sorted_H[sorted_H > 0]  # Remove zeros
    if len(sorted_H) == 0:
        return None

    cumsum = np.cumsum(sorted_H)
    cumsum = cumsum / cumsum[-1]

    level_3sigma = (
        sorted_H[np.where(cumsum >= 0.997)[0][0]] if np.any(cumsum >= 0.997) else sorted_H[-1]
    )
    level_2sigma = (
        sorted_H[np.where(cumsum >= 0.95)[0][0]] if np.any(cumsum >= 0.95) else sorted_H[-1]
    )
    level_1sigma = (
        sorted_H[np.where(cumsum >= 0.68)[0][0]] if np.any(cumsum >= 0.68) else sorted_H[-1]
    )
    return [level_3sigma, level_2sigma, level_1sigma]


def plot_density_contours(ax, x, y, color, label, clip_above=None, clip_below=None):
    """
    Plot filled contours with 1, 2 and 3 sigma levels (SAGE26 allresults-local.py:1761-1816).

    A population of fewer than 10 galaxies, or of an all-zero histogram, draws nothing.
    """
    if len(x) < 10:
        return

    H_smooth, x_centers, y_centers = smoothed_histogram(x, y, clip_above, clip_below)
    levels = sigma_levels(H_smooth)
    if levels is None:
        return
    alphas = [0.3, 0.5, 0.7]

    # Plot filled contours. SAGE26 passes `label=label` to the last contourf (:1806-1808);
    # Matplotlib 3.10 does not use it (it warns that the keyword is unused), so no legend
    # entry results, in SAGE26's figure as in this one.
    # SAGE26 figure parity: allresults-local.py:1808 passes label= to contourf, which gives
    # no legend entry under Matplotlib 3.10, so "Quiescent" and "Star-forming" never appear.
    for i, (level, alpha) in enumerate(zip(levels, alphas)):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            if i == len(levels) - 1:
                filled = ax.contourf(
                    x_centers,
                    y_centers,
                    H_smooth,
                    levels=[level, H_smooth.max()],
                    colors=[color],
                    alpha=alpha,
                    label=label,
                )
            else:
                filled = ax.contourf(
                    x_centers,
                    y_centers,
                    H_smooth,
                    levels=[level, levels[i + 1] if i + 1 < len(levels) else H_smooth.max()],
                    colors=[color],
                    alpha=alpha,
                )
        filled.set_gid("model")

    # Add contour lines
    lines = ax.contour(
        x_centers, y_centers, H_smooth, levels=levels, colors=color, linewidths=1.0, alpha=0.8
    )
    lines.set_gid("model")


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
    Create the specific star formation rate density plot.

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
        plot_name="Specific Star Formation Rate",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    rng = random.Random(2222)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "specific_star_formation_rate", (8.0, 12.2), (-13.0, -8.0)
    )

    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
    sfr_disk = galaxies.SfrDisk
    sfr_bulge = galaxies.SfrBulge

    # allresults-local.py:1737-1743
    w2 = np.where(stellar_mass > 0.0)[0]
    # SAGE26 figure parity: allresults-local.py:1738 samples positions from range(len(w2)) and
    # then indexes the full arrays with them (StellarMass[w2]), so above DILUTE candidates it
    # draws arbitrary galaxies, zero-mass ones included (mapped to -99 and left out). The
    # mini-Millennium run is below DILUTE, where w2 is not sampled.
    w2 = draw_sample(w2, rng)
    # A zero-mass galaxy the sampled branch can reach has log10 = -inf, which is not drawn
    with np.errstate(divide="ignore"):
        mass = np.log10(stellar_mass[w2])
    sSFR = log_specific_sfr(sfr_disk[w2], sfr_bulge[w2], stellar_mass[w2])

    # Separate populations, allresults-local.py:1749-1755
    sf_mask = (sSFR > SSFR_CUT) & (sSFR > NO_SSFR)  # Star-forming
    q_mask = (sSFR <= SSFR_CUT) & (sSFR > NO_SSFR)  # Quiescent
    mass_sf = mass[sf_mask]
    sSFR_sf = sSFR[sf_mask]
    mass_q = mass[q_mask]
    sSFR_q = sSFR[q_mask]

    if verbose:
        print("Specific Star Formation Rate plot debug:")
        print(f"  Number of galaxies drawn: {len(w2)}")
        print(f"  Quiescent: {len(mass_q)}, star-forming: {len(mass_sf)}")

    fig, ax = setup_figure(figsize=(10, 8))

    # Quiescent population (red), clipped above the cut; star-forming (blue), clipped below it
    # (allresults-local.py:1818-1824)
    if len(mass_q) > 0:
        plot_density_contours(ax, mass_q, sSFR_q, "firebrick", "Quiescent", clip_above=SSFR_CUT)
    if len(mass_sf) > 0:
        plot_density_contours(
            ax, mass_sf, sSFR_sf, "dodgerblue", "Star-forming", clip_below=SSFR_CUT
        )

    # The specific star formation rate cut line, allresults-local.py:1826-1827
    ax.axhline(
        y=SSFR_CUT,
        color="k",
        linestyle="--",
        linewidth=2,
        label=f"sSFR cut = {SSFR_CUT}",
        zorder=10,
    )

    ax.set_ylabel(r"$\log_{10} \mathrm{sSFR}\ (\mathrm{yr^{-1}})$", fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)

    # allresults-local.py:1832-1838
    ax.xaxis.set_minor_locator(MultipleLocator(0.1))
    ax.yaxis.set_minor_locator(MultipleLocator(0.25))
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    setup_legend(ax, loc="upper right")

    plot_path = save_and_close_figure(
        fig, output_dir, "specific_star_formation_rate", output_format, verbose
    )
    return plot_path, None
