#!/usr/bin/env python

"""
SAGE26 Central and Satellite Fraction Plot

Port of the "Plotting central/satellite fraction as a function of redshift" section of SAGE26's
history plotting script (plotting/allresults-history.py:1856-1900, figure CentralSatelliteFraction):
the fractions of galaxies that are central and that are satellites (orphans included) against
redshift. The section is the specification; where this port differs it is a permitted deviation
below.

Requires: Type

Fields read (units): Type (0 central, 1 satellite, 2 orphan; :1870-1871, a Type of 1 or more is
a satellite); a snapshot's redshift is its metadata["redshift"].

Snapshots: every written snapshot (:1865), chosen through ``all_snapshots``; a snapshot without
galaxies is skipped (:1866-1873).

Permitted deviations: none beyond drawing the figure from the run's eight snapshots, where
SAGE26 reads 64.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._snapshot_selection import all_snapshots
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)


def central_and_satellite_fractions(types):
    """
    Return the fractions of galaxies that are central and that are satellite.

    SAGE26 allresults-history.py:1869-1876: `n_total = len(types)`, `n_cen = np.sum(types == 0)`,
    `n_sat = np.sum(types >= 1)` (">= 1: orphans (Type 2) are satellites too") and
    `n_cen / n_total`, `n_sat / n_total`.
    """
    n_total = len(types)
    n_cen = np.sum(types == 0)
    n_sat = np.sum(types >= 1)
    return n_cen / n_total, n_sat / n_total


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the central and satellite fraction plot.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if a field is missing
    """
    if len(snapshots) == 0:
        return None, "No snapshot data available for the central and satellite fraction plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0], required_fields=["Type"], plot_name="Central Satellite Fraction"
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # allresults-history.py:1861-1880
    z_arr_cs = []
    central_frac = []
    satellite_frac = []

    for snap in all_snapshots(snapshots):
        galaxies, _volume, metadata = snapshots[snap]
        if len(galaxies) == 0:
            continue
        cen, sat = central_and_satellite_fractions(galaxies.Type)
        z_arr_cs.append(metadata["redshift"])
        central_frac.append(cen)
        satellite_frac.append(sat)

    z_arr_cs = np.array(z_arr_cs)
    central_frac = np.array(central_frac)
    satellite_frac = np.array(satellite_frac)

    # Sort by redshift (:1882-1885)
    sort_idx = np.argsort(z_arr_cs)
    z_arr_cs = z_arr_cs[sort_idx]
    central_frac = central_frac[sort_idx]
    satellite_frac = satellite_frac[sort_idx]

    x_min, x_max, y_min, y_max = get_profile_axes(
        params,
        "central_satellite_fraction",
        (0.0, max(z_arr_cs) if len(z_arr_cs) > 0 else 12),
        (0.0, 1.05),
    )

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1887-1896
    ax.plot(z_arr_cs, central_frac, color="steelblue", lw=2.5, label="Centrals", gid="model")
    ax.plot(z_arr_cs, satellite_frac, color="firebrick", lw=2.5, label="Satellites", gid="model")

    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"Redshift $z$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("Fraction", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.05))
    ax.legend(loc="center right", frameon=False)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "CentralSatelliteFraction", output_format, verbose
    )
    return plot_path, None
