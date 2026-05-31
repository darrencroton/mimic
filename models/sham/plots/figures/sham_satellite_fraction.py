#!/usr/bin/env python

"""SHAM satellite fraction diagnostic."""

import numpy as np

from output_utils import (
    check_required_fields,
    save_and_close_figure,
    setup_figure,
    validate_filtered_data,
)


def plot(
    galaxies,
    volume,
    metadata,
    params,
    output_dir="plots",
    output_format=".png",
    verbose=False,
):
    """Plot satellite fraction as a function of SHAM stellar mass."""
    del volume, params

    success, _, msg = check_required_fields(
        galaxies,
        required_fields=["StellarMass", "Type"],
        plot_name="SHAM Satellite Fraction",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    h = metadata["hubble_h"]
    w = np.where((galaxies.StellarMass > 0.0) & np.isfinite(galaxies.StellarMass))[0]
    is_valid, skip_msg = validate_filtered_data(w, "SHAM Satellite Fraction", verbose)
    if not is_valid:
        return None, skip_msg

    log_mstar = np.log10(galaxies.StellarMass[w] * 1.0e10 / h)
    is_satellite = galaxies.Type[w] != 0
    bins = np.arange(8.0, 12.4, 0.25)
    centers = 0.5 * (bins[:-1] + bins[1:])
    frac = np.full(len(centers), np.nan)
    err = np.full(len(centers), np.nan)

    for i in range(len(centers)):
        in_bin = (log_mstar >= bins[i]) & (log_mstar < bins[i + 1])
        n = np.count_nonzero(in_bin)
        if n < 5:
            continue
        f = np.count_nonzero(is_satellite[in_bin]) / n
        frac[i] = f
        err[i] = np.sqrt(f * (1.0 - f) / n)

    good = np.isfinite(frac)
    fig, ax = setup_figure()
    ax.errorbar(
        centers[good],
        frac[good],
        yerr=err[good],
        color="#0173b2",
        marker="o",
        lw=1.8,
    )
    ax.set_xlabel(r"log$_{10}$ M$_*$ [M$_{\odot}$]")
    ax.set_ylabel("Satellite fraction")
    ax.set_xlim(8.0, 12.2)
    ax.set_ylim(0.0, 1.0)
    ax.grid(True, color="0.9", lw=0.8)

    if verbose:
        total_sat = np.count_nonzero(is_satellite)
        print(f"  satellite fraction: {total_sat}/{len(w)} = {total_sat / len(w):.3f}")

    return (
        save_and_close_figure(
            fig, output_dir, "ShamSatelliteFraction", output_format, verbose
        ),
        None,
    )

