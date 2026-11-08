#!/usr/bin/env python

"""
SAGE26 CGM Fraction Against Halo Mass Plot

Port of the "CGM fraction vs Mvir" section of SAGE26's plotting script
(plotting/allresults-local.py:2008-2039): SAGE26's circumgalactic-medium fraction quantity of
every galaxy against halo mass. The section is the specification; where this port differs it is
a permitted deviation below.

Requires: CGMgas, Mvir

Fields read (units): CGMgas and Mvir (1e10 Msun/h, converted to Msun as SAGE26 converts them:
x 1e10 / hubble_h, allresults-local.py:364, :375).

Permitted deviations: none beyond the display freedoms (figure size, font sizes, colours, marker
sizes, dpi, bbox) taken through the house helpers.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)


def cgm_fraction(cgm_gas, mvir):
    """
    Return SAGE26's CGM-fraction quantity: log10(CGMgas / Mvir) times 0.17.

    SAGE26 allresults-local.py:2023: `np.log10(CGMgas / Mvir) * 0.17`, commented "Normalize by
    cosmic baryon fraction".
    """
    # SAGE26 figure parity: allresults-local.py:2023 multiplies log10(CGMgas / Mvir) by 0.17,
    # the cosmic baryon fraction, under the comment "Normalize by cosmic baryon fraction".
    # Suspected: a normalisation would divide the fraction by 0.17 (log10(CGMgas / Mvir / 0.17)),
    # not scale its logarithm, so the plotted quantity is not a normalised log fraction.
    return np.log10(cgm_gas / mvir) * 0.17


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
    Create the CGM fraction against halo mass plot.

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
        required_fields=["CGMgas", "Mvir"],
        plot_name="CGM Fraction Vs Mvir",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "cgm_fraction_vs_mvir", (10.0, 12.0), (-2.5, 0.0)
    )

    CGMgas = galaxies.CGMgas * 1.0e10 / hubble_h
    Mvir = galaxies.Mvir * 1.0e10 / hubble_h

    # allresults-local.py:2023: every galaxy; a galaxy with no CGM gas or no halo mass has an
    # infinite or undefined value and is not drawn, as in SAGE26
    with np.errstate(divide="ignore", invalid="ignore"):
        CGM_fraction = cgm_fraction(CGMgas, Mvir)
        log_Mvir = np.log10(Mvir)

    if verbose:
        print("CGM Fraction Vs Mvir plot debug:")
        print(f"  Number of galaxies: {len(CGM_fraction)}")
        print(f"  Finite CGM fractions: {int(np.count_nonzero(np.isfinite(CGM_fraction)))}")

    fig, ax = setup_figure()

    # allresults-local.py:2028
    ax.scatter(log_Mvir, CGM_fraction, s=2, alpha=0.5, rasterized=True, gid="model")

    ax.set_xlabel(r"$\log_{10} M_{\rm vir}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\mathrm{CGM\ Fraction}$", fontsize=AXIS_LABEL_SIZE)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    plot_path = save_and_close_figure(
        fig, output_dir, "cgm_fraction_vs_mvir", output_format, verbose
    )
    return plot_path, None
