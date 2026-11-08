#!/usr/bin/env python

"""
SAGE26 Cooling Rate Against Halo Mass Plot

Port of the "Plotting tcool/tff vs Mvir" section of SAGE26's plotting script
(plotting/allresults-local.py:1940-2005): the cooling rate of galaxies against halo mass on a
logarithmic axis, the circumgalactic-medium regime in blue and the hot regime in red; a galaxy of
any other regime is drawn coloured by virial temperature, with a colour bar, only where there is
one. The section is the specification; where this port differs it is a permitted deviation below.

Requires: CoolingRate, Mvir, Rvir, Regime, Vvir

Fields read (units): CoolingRate (Msun/Gyr over h, models/sage26/model_properties.yaml, where
SAGE26's label says Msun/Gyr; held in SAGE26's variable `tcool`, allresults-local.py:1948), Mvir (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, :1949), Rvir (Mpc/h, only to select), Regime (0 circumgalactic medium,
1 hot) and Vvir (km/s; the virial temperature is 35.9 Vvir^2 K, :411).

Permitted deviations: none beyond the display freedoms (figure size, font sizes, colours, marker
sizes, dpi, bbox) taken through the house helpers.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from matplotlib.colors import LogNorm
from output_utils import (
    check_required_fields,
    save_and_close_figure,
    setup_figure,
)


def virial_temperature(vvir):
    """
    Return the virial temperature in Kelvin of each halo, 35.9 times the virial velocity squared.

    SAGE26 allresults-local.py:411: `35.9 * (Vvir)**2  # in Kelvin`.
    """
    return 35.9 * (vvir) ** 2


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
    Create the cooling rate against halo mass plot.

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
        required_fields=["CoolingRate", "Mvir", "Rvir", "Regime", "Vvir"],
        plot_name="Tcool Vs Mvir",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]

    # SAGE26 figure parity: allresults-local.py:1943 and the figure's name say "tcool/tff", but
    # the quantity drawn is CoolingRate (:1948, labelled "Cooling rate" at :2000).
    tcool = galaxies.CoolingRate
    Mvir = galaxies.Mvir * 1.0e10 / hubble_h  # Convert to solar masses
    Rvir = galaxies.Rvir
    Regime = galaxies.Regime
    Tvir = virial_temperature(galaxies.Vvir)

    # allresults-local.py:1954-1960
    w = np.where((Mvir > 0.0) & (tcool > 0.0) & (Rvir > 0.0))[0]
    with np.errstate(divide="ignore", invalid="ignore"):
        log_Mvir = np.log10(Mvir)[w]
    tcool = tcool[w]
    Regime = Regime[w]
    Tvir = Tvir[w]

    cgm = Regime == 0
    hot = Regime == 1
    unclassified = ~(cgm | hot)

    if verbose:
        print("Tcool Vs Mvir plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")
        print(
            f"  CGM regime: {int(np.count_nonzero(cgm))}, hot regime: "
            f"{int(np.count_nonzero(hot))}, unclassified: {int(np.count_nonzero(unclassified))}"
        )

    fig, ax = setup_figure()

    # allresults-local.py:1983-1996. A regime with no galaxy draws nothing; the colour bar
    # exists only with the unclassified galaxies.
    if np.any(cgm):
        ax.scatter(
            log_Mvir[cgm],
            tcool[cgm],
            s=2,
            alpha=0.5,
            color="tab:blue",
            label="CGM regime",
            rasterized=True,
            gid="model",
        )
    if np.any(hot):
        ax.scatter(
            log_Mvir[hot],
            tcool[hot],
            s=2,
            alpha=0.5,
            color="tab:red",
            label="Hot regime",
            rasterized=True,
            gid="model",
        )
    if np.any(unclassified):
        temperature_points = ax.scatter(
            log_Mvir[unclassified],
            tcool[unclassified],
            s=2,
            alpha=0.5,
            c=Tvir[unclassified],
            cmap="viridis",
            norm=LogNorm(),
            label="Unclassified",
            rasterized=True,
            gid="model",
        )
        colorbar = fig.colorbar(temperature_points, ax=ax, pad=0.02)
        colorbar.set_label(r"$T_{\rm vir}$ (K)", fontsize=AXIS_LABEL_SIZE)
    ax.set_yscale("log")

    # SAGE26 sets no axis window here (allresults-local.py:2002-2003 are commented out)
    ax.set_xlabel(r"$\log_{10} M_{\rm vir}\ (M_{\odot})$", fontsize=AXIS_LABEL_SIZE)
    # SAGE26 figure parity: allresults-local.py:2000 labels the axis Msun/Gyr, but the plotted
    # CoolingRate is Msun/Gyr over h (its declared unit, model_properties.yaml). Mirrored as written.
    ax.set_ylabel(
        r"$\mathrm{Cooling\ rate}\ (M_{\odot}\ \mathrm{Gyr}^{-1})$", fontsize=AXIS_LABEL_SIZE
    )
    setup_legend(ax, loc="best", frameon=False)

    plot_path = save_and_close_figure(fig, output_dir, "tcool_vs_mvir", output_format, verbose)
    return plot_path, None
