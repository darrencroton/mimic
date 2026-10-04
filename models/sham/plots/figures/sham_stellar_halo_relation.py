#!/usr/bin/env python

"""
SHAM stellar-to-halo relation diagnostics.

Stellar mass against the peak halo mass and the peak maximum circular velocity for the SHAM
sample (ShamGhost == 0), centrals (Type 0) and satellites (Type 1) shown separately, with the
running median and 16th-84th percentile band. The bins come from the profile keys
axes.sham_stellar_halo_mpeak and axes.sham_stellar_halo_vpeak (x range) at fixed bin widths.
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import SAMPLE_LABEL, setup_plot_fonts
from output_utils import (
    check_required_fields,
    get_profile_axes,
    make_bin_edges,
    save_and_close_figure,
    validate_filtered_data,
)

MPEAK_BIN_WIDTH = 0.2  # dex
VPEAK_BIN_WIDTH = 0.08  # dex


def _median_relation(x, y, bins):
    centers = 0.5 * (bins[:-1] + bins[1:])
    med = np.full(len(centers), np.nan)
    p16 = np.full(len(centers), np.nan)
    p84 = np.full(len(centers), np.nan)

    for i in range(len(centers)):
        w = (x >= bins[i]) & (x < bins[i + 1])
        if np.count_nonzero(w) < 5:
            continue
        med[i] = np.median(y[w])
        p16[i], p84[i] = np.percentile(y[w], [16.0, 84.0])

    return centers, med, p16, p84


def plot(
    galaxies,
    volume,
    metadata,
    params,
    output_dir="plots",
    output_format=".png",
    verbose=False,
):
    """Plot SHAM-assigned stellar mass against Mpeak and Vpeak for the sample (ShamGhost == 0)."""
    del volume

    success, _, msg = check_required_fields(
        galaxies,
        required_fields=["StellarMass", "ShamMpeak", "ShamVpeak", "ShamGhost", "Type"],
        plot_name="SHAM Stellar-Halo Relation",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    h = metadata["hubble_h"]
    w = np.where(
        (galaxies.ShamGhost == 0)
        & (galaxies.StellarMass > 0.0)
        & (galaxies.ShamMpeak > 0.0)
        & (galaxies.ShamVpeak > 0.0)
        & np.isfinite(galaxies.StellarMass)
    )[0]
    is_valid, skip_msg = validate_filtered_data(w, "SHAM Stellar-Halo Relation", verbose)
    if not is_valid:
        return None, skip_msg

    log_mstar = np.log10(galaxies.StellarMass[w] * 1.0e10 / h)
    log_mpeak = np.log10(galaxies.ShamMpeak[w] * 1.0e10 / h)
    log_vpeak = np.log10(galaxies.ShamVpeak[w])
    is_sat = galaxies.Type[w] == 1

    fig, axes = plt.subplots(1, 2, figsize=(12, 5), constrained_layout=True)
    for ax in axes:
        setup_plot_fonts(ax)

    axes[0].scatter(
        log_mpeak[~is_sat],
        log_mstar[~is_sat],
        s=3,
        c="0.15",
        alpha=0.25,
        label="Centrals (Type 0)",
    )
    axes[0].scatter(
        log_mpeak[is_sat],
        log_mstar[is_sat],
        s=3,
        c="#c44e52",
        alpha=0.25,
        label="Satellites (Type 1)",
    )
    x_min_0, x_max_0, y_min_0, y_max_0 = get_profile_axes(
        params, "sham_stellar_halo_mpeak", (10.0, 15.0), (7.5, 12.2)
    )
    bins = make_bin_edges(x_min_0, x_max_0, MPEAK_BIN_WIDTH)
    xmid, med, p16, p84 = _median_relation(log_mpeak, log_mstar, bins)
    good = np.isfinite(med)
    axes[0].plot(xmid[good], med[good], color="#0173b2", lw=2, label="Median")
    axes[0].fill_between(xmid[good], p16[good], p84[good], color="#0173b2", alpha=0.18, lw=0)
    axes[0].set_xlabel(r"log$_{10}$ M$_{\rm peak}$ [M$_{\odot}$]")
    axes[0].set_ylabel(r"log$_{10}$ M$_*$ [M$_{\odot}$]")
    axes[0].set_xlim(x_min_0, x_max_0)
    axes[0].set_ylim(y_min_0, y_max_0)
    axes[0].legend(frameon=False, loc="lower right")

    axes[1].scatter(
        log_vpeak[~is_sat],
        log_mstar[~is_sat],
        s=3,
        c="0.15",
        alpha=0.25,
        label="Centrals (Type 0)",
    )
    axes[1].scatter(
        log_vpeak[is_sat],
        log_mstar[is_sat],
        s=3,
        c="#c44e52",
        alpha=0.25,
        label="Satellites (Type 1)",
    )
    x_min_1, x_max_1, y_min_1, y_max_1 = get_profile_axes(
        params, "sham_stellar_halo_vpeak", (1.7, 3.5), (7.5, 12.2)
    )
    vbins = make_bin_edges(x_min_1, x_max_1, VPEAK_BIN_WIDTH)
    xmid, med, p16, p84 = _median_relation(log_vpeak, log_mstar, vbins)
    good = np.isfinite(med)
    axes[1].plot(xmid[good], med[good], color="#0173b2", lw=2, label="Median")
    axes[1].fill_between(xmid[good], p16[good], p84[good], color="#0173b2", alpha=0.18, lw=0)
    axes[1].set_xlabel(r"log$_{10}$ V$_{\rm peak}$ [km/s]")
    axes[1].set_ylabel(r"log$_{10}$ M$_*$ [M$_{\odot}$]")
    axes[1].set_xlim(x_min_1, x_max_1)
    axes[1].set_ylim(y_min_1, y_max_1)
    axes[1].legend(frameon=False, loc="lower right")
    fig.suptitle(SAMPLE_LABEL)

    return (
        save_and_close_figure(fig, output_dir, "ShamStellarHaloRelation", output_format, verbose),
        None,
    )
