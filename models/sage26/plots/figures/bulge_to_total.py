#!/usr/bin/env python

"""
SAGE26 Bulge-to-Total Plot

Port of the "Plotting the bulge-to-total ratio" section of SAGE26's plotting script
(plotting/allresults-local.py:1179-1244): the bulge-to-total mass ratio of every galaxy against
stellar mass, with the median and 16-84th percentile of each stellar-mass bin, the mean
merger-driven and mean instability-driven components, and the Moffett et al. (2016)
observations. The section is the specification; where this port differs it is a permitted
deviation below.

Requires: StellarMass, BulgeMass, MergerBulgeMass, InstabilityBulgeMass

Fields read (units): StellarMass, BulgeMass, MergerBulgeMass and InstabilityBulgeMass, each in
1e10 Msun/h and converted to Msun as SAGE26 converts them (x 1e10 / hubble_h,
allresults-local.py:365, :367, :383, :384). SAGE26's `Moffet16.dat` overlay is held inline as
OBS_MOFFETT16.

Permitted deviations: none beyond the display freedoms (figure size, font sizes, colours, marker
sizes, dpi, bbox) taken through the house helpers.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Moffett et al. (2016, MNRAS, 462, 4336) GAMA bulge mass fractions, transcribed from SAGE26's
# data/morphology/Moffet16.dat (read at allresults-local.py:1224). Columns: log10(stellar
# mass / Msun), fraction of mass in the bulge, statistical -1 sigma boundary, statistical
# +1 sigma boundary, in the file's row order.
OBS_MOFFETT16 = np.array(
    [
        [8.09035, 0.0139795, 0.000875908, 0.0305517],
        [8.30661, 0.0323736, 0.0158714, 0.0389954],
        [8.49904, 0.0670245, 0.0571442, 0.0703179],
        [8.69981, 0.0655179, 0.0523793, 0.0721398],
        [8.90379, 0.120034, 0.110189, 0.123398],
        [9.10405, 0.158049, 0.148204, 0.167894],
        [9.29967, 0.252016, 0.245464, 0.271742],
        [9.49512, 0.359157, 0.346019, 0.385540],
        [9.69119, 0.416897, 0.383998, 0.446573],
        [9.89518, 0.471414, 0.445101, 0.507571],
        [10.0958, 0.479787, 0.446818, 0.529188],
        [10.2927, 0.474952, 0.442053, 0.514473],
        [10.4942, 0.417458, 0.364798, 0.460272],
        [10.6951, 0.406071, 0.366550, 0.468611],
        [10.8938, 0.565942, 0.476984, 0.651535],
        [11.0879, 0.775178, 0.623716, 0.913502],
        [11.2947, 0.915359, 0.674869, 1.000000],
        [11.4861, 1.00000, 0.465843, 1.000000],
    ]
)

# SAGE26 shifts the Moffett masses by imf_shift('chabrier') (allresults-local.py:1226): the
# dex of the selected IMF (whichimf = 1, Chabrier, :46) above the Chabrier scale minus the dex
# of the data's native IMF (Chabrier), both 0.00 (:52-53, :60-72).
MOFFETT_IMF_SHIFT_DEX = 0.0 - 0.0

# Stellar-mass bins of the percentile and mean curves: np.arange(8.5, 12.5 + binwidth, binwidth)
# with binwidth 0.25 (allresults-local.py:1189-1190), and the smallest bin SAGE26 summarises
# (:1205, "too few galaxies for a meaningful percentile")
BIN_WIDTH = 0.25
BIN_START = 8.5
BIN_STOP = 12.5
MIN_GALAXIES_PER_BIN = 5


def bulge_to_total_ratios(stellar_mass, bulge_mass, merger_bulge_mass, instability_bulge_mass):
    """
    Return (B/T, merger-driven B/T, instability-driven B/T) of the selected galaxies.

    SAGE26 allresults-local.py:1185-1187: BulgeMass / StellarMass, MergerBulgeMass /
    StellarMass and InstabilityBulgeMass / StellarMass, each over the selected galaxies.
    """
    btot = bulge_mass / stellar_mass
    btot_merger = merger_bulge_mass / stellar_mass
    btot_instab = instability_bulge_mass / stellar_mass
    return btot, btot_merger, btot_instab


def bulge_to_total_bins(mass, btot, btot_merger, btot_instab):
    """
    Return the per-bin statistics of the bulge-to-total ratio.

    SAGE26 allresults-local.py:1189-1210: bins from np.arange(8.5, 12.5 + 0.25, 0.25) assigned
    with np.digitize; a bin with fewer than 5 galaxies is left at NaN; otherwise it holds the
    median and 16th / 84th percentiles of B/T and the means of the merger- and
    instability-driven ratios.

    Returns:
        (bin_centres, median, lower, upper, merger_mean, instability_mean), each one value
        per bin, NaN where the bin is not summarised
    """
    bin_edges = np.arange(BIN_START, BIN_STOP + BIN_WIDTH, BIN_WIDTH)
    bin_centres = bin_edges[:-1] + BIN_WIDTH / 2.0
    digitized = np.digitize(mass, bin_edges)

    btot_med = np.full(len(bin_centres), np.nan)
    btot_lo = np.full(len(bin_centres), np.nan)
    btot_hi = np.full(len(bin_centres), np.nan)
    merger_ave = np.full(len(bin_centres), np.nan)
    instab_ave = np.full(len(bin_centres), np.nan)

    for i in range(len(bin_centres)):
        sel = digitized == i + 1
        if np.sum(sel) < MIN_GALAXIES_PER_BIN:  # too few galaxies for a meaningful percentile
            continue
        btot_med[i] = np.median(btot[sel])
        btot_lo[i], btot_hi[i] = np.percentile(btot[sel], [16.0, 84.0])
        merger_ave[i] = np.mean(btot_merger[sel])
        instab_ave[i] = np.mean(btot_instab[sel])
    return bin_centres, btot_med, btot_lo, btot_hi, merger_ave, instab_ave


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
    Create the bulge-to-total ratio plot.

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
        required_fields=["StellarMass", "BulgeMass", "MergerBulgeMass", "InstabilityBulgeMass"],
        plot_name="Bulge To Total",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "bulge_to_total", (8.5, 12.5), (0.0, 1.05)
    )

    stellar_mass = galaxies.StellarMass * 1.0e10 / hubble_h
    bulge_mass = galaxies.BulgeMass * 1.0e10 / hubble_h
    merger_bulge_mass = galaxies.MergerBulgeMass * 1.0e10 / hubble_h
    instability_bulge_mass = galaxies.InstabilityBulgeMass * 1.0e10 / hubble_h

    # allresults-local.py:1183-1187
    w = np.where((stellar_mass > 0.0) & (bulge_mass > 0.0))[0]
    mass = np.log10(stellar_mass[w])
    btot, btot_merger, btot_instab = bulge_to_total_ratios(
        stellar_mass[w], bulge_mass[w], merger_bulge_mass[w], instability_bulge_mass[w]
    )
    bin_centres, btot_med, btot_lo, btot_hi, merger_ave, instab_ave = bulge_to_total_bins(
        mass, btot, btot_merger, btot_instab
    )

    if verbose:
        print("Bulge To Total plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")
        print(f"  Bins summarised: {int(np.count_nonzero(np.isfinite(btot_med)))}")

    fig, ax = setup_figure()

    # allresults-local.py:1212-1219
    good = np.isfinite(btot_med)
    ax.fill_between(
        bin_centres[good],
        btot_lo[good],
        btot_hi[good],
        facecolor="k",
        alpha=0.2,
        label="Model 16-84th percentile",
        gid="model",
    )
    ax.plot(bin_centres[good], btot_med[good], "k-", lw=2, label="Model median", gid="model")
    ax.plot(
        bin_centres[good],
        merger_ave[good],
        "--",
        color="firebrick",
        lw=1.5,
        label="Mean merger-driven",
        gid="model",
    )
    ax.plot(
        bin_centres[good],
        instab_ave[good],
        "-.",
        color="royalblue",
        lw=1.5,
        label="Mean instability-driven",
        gid="model",
    )

    # allresults-local.py:1221, points on top of the lines
    ax.scatter(mass, btot, color="k", s=0.5, zorder=10, alpha=0.45, gid="model")

    # Moffett et al. (2016), allresults-local.py:1224-1230
    moffett = OBS_MOFFETT16
    obs_mass = moffett[:, 0] + MOFFETT_IMF_SHIFT_DEX
    ax.errorbar(
        obs_mass,
        moffett[:, 1],
        yerr=[moffett[:, 1] - moffett[:, 2], moffett[:, 3] - moffett[:, 1]],
        fmt="o",
        ms=4,
        color="darkorange",
        mfc="darkorange",
        ecolor="darkorange",
        elinewidth=1,
        capsize=2,
        zorder=5,
        label="Moffett et al. (2016)",
    )

    # allresults-local.py:1232-1234
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_ylabel(r"$M_{\mathrm{bulge}} / M_{\mathrm{stars}}$", fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)

    setup_legend(ax, loc="upper left")

    plot_path = save_and_close_figure(fig, output_dir, "BulgeToTotal", output_format, verbose)
    return plot_path, None
