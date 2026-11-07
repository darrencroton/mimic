#!/usr/bin/env python

"""
SAGE26 Central and Satellite Fraction of Massive Quiescent Galaxies Plot

Port of the "Plotting central/satellite fraction of most massive quiescent galaxies vs redshift"
section of SAGE26's history plotting script (plotting/allresults-history.py:1906-1976, figure
CentralSatelliteFraction_MassiveQuiescent): among the 10% most massive galaxies of a snapshot
(by stellar mass) those with a specific star formation rate below 0.2 / t_H, the fractions that
are central and that are satellites (orphans included), against redshift. The section is the
specification; where this port differs it is a permitted deviation below.

Requires: StellarMass, SfrDisk, SfrBulge, Type

Fields read (units): StellarMass (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-history.py:337), SfrDisk and SfrBulge (Msun/yr; the rate is their
sum, :1922) and Type (0 central, 1 or more satellite; :1947-1948). The cosmology (Omega,
OmegaLambda, Hubble_h) is the run's, from the engine's parameters
(plot/mimic-plot/mimic-plot.py:588-593), where SAGE26 reads it from its file header
(:1911-1913); a snapshot's redshift is its metadata["redshift"].

Snapshots: every written snapshot (:1919), chosen through ``all_snapshots``; a snapshot needs at
least 10 galaxies with positive stellar mass (:1926-1928) and at least one quiescent galaxy among
the most massive (:1943-1945).

Permitted deviations (c16 Slice 3, row `central_satellite_fraction_massive_quiescent`): none
beyond the rules (the figure is drawn from the run's eight snapshots, where SAGE26 reads 64).
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

# A snapshot needs at least this many galaxies with positive stellar mass
# (allresults-history.py:1927)
MIN_GALAXIES = 10

# Percentile of the stellar mass above which a galaxy is among the most massive
# (allresults-history.py:1931)
MASS_PERCENTILE = 90


def cosmology(params):
    """
    Return (Omega, OmegaLambda, Hubble_h) from the run's parameters, or None if one is missing
    or zero.
    """
    values = tuple(params.get(key) for key in ("Omega", "OmegaLambda", "Hubble_h"))
    if any(not value for value in values):
        return None
    return values


def ssfr_threshold(z, omega_m, omega_l, hubble_h):
    """
    Return the specific star formation rate [1/yr] below which a galaxy is quiescent: 0.2 / t_H.

    SAGE26 allresults-history.py:1913 and :1938-1939: `inv_H0_yr = (9.778 / Hubble_h) * 1.0e9`,
    `E_z = np.sqrt(OmegaM_cs * (1.0 + z)**3 + OmegaL_cs)` and `0.2 * E_z / inv_H0_yr`.
    """
    inv_h0_yr = (9.778 / hubble_h) * 1.0e9
    e_z = np.sqrt(omega_m * (1.0 + z) ** 3 + omega_l)
    return 0.2 * e_z / inv_h0_yr


def massive_quiescent_counts(stellar_mass, sfr_total, types, threshold):
    """
    Return (number quiescent, number central, number satellite) among the most massive galaxies,
    or None when the snapshot is skipped.

    SAGE26 allresults-history.py:1926-1948: `w = np.where(mstar > 0.0)[0]`, skipped with fewer
    than 10; `mass_thresh = np.percentile(mstar[w], 90)`, `massive = w[mstar[w] >= mass_thresh]`;
    `sSFR = sfr_total[massive] / mstar[massive]`, `quiescent = massive[sSFR < ssfr_thresh]`,
    skipped when none; `np.sum(types[quiescent] == 0)` and `np.sum(types[quiescent] >= 1)`
    (">= 1: orphans (Type 2) are satellites too").
    """
    w = np.where(stellar_mass > 0.0)[0]
    if len(w) < MIN_GALAXIES:
        return None

    mass_thresh = np.percentile(stellar_mass[w], MASS_PERCENTILE)
    massive = w[stellar_mass[w] >= mass_thresh]
    if len(massive) == 0:
        return None

    sfr_per_mass = sfr_total[massive] / stellar_mass[massive]
    quiescent = massive[sfr_per_mass < threshold]
    n_q = len(quiescent)
    if n_q == 0:
        return None

    return n_q, np.sum(types[quiescent] == 0), np.sum(types[quiescent] >= 1)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the central and satellite fraction plot of the most massive quiescent galaxies.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with Mimic parameters (Omega, OmegaLambda and Hubble_h are read)
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if a field or a run parameter is
              missing
    """
    if len(snapshots) == 0:
        return None, "No snapshot data available for the massive quiescent fraction plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "SfrDisk", "SfrBulge", "Type"],
        plot_name="Central Satellite Fraction (Massive Quiescent)",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    cosmo = cosmology(params)
    if cosmo is None:
        return None, "Run parameters Omega, OmegaLambda and Hubble_h are missing or zero"
    omega_m, omega_l, run_hubble_h = cosmo

    # allresults-history.py:1915-1953
    z_arr_mq = []
    central_frac_mq = []
    satellite_frac_mq = []

    for snap in all_snapshots(snapshots):
        galaxies, _volume, metadata = snapshots[snap]
        if len(galaxies) == 0:
            continue
        hubble_h = metadata["hubble_h"]
        mstar = galaxies.StellarMass * 1.0e10 / hubble_h
        sfr_total = galaxies.SfrDisk + galaxies.SfrBulge

        z = metadata["redshift"]
        counts = massive_quiescent_counts(
            mstar, sfr_total, galaxies.Type, ssfr_threshold(z, omega_m, omega_l, run_hubble_h)
        )
        if counts is None:
            continue
        n_q, n_cen, n_sat = counts

        z_arr_mq.append(z)
        central_frac_mq.append(n_cen / n_q)
        satellite_frac_mq.append(n_sat / n_q)

    z_arr_mq = np.array(z_arr_mq)
    central_frac_mq = np.array(central_frac_mq)
    satellite_frac_mq = np.array(satellite_frac_mq)

    # Sort by redshift (:1958-1961)
    sort_idx = np.argsort(z_arr_mq)
    z_arr_mq = z_arr_mq[sort_idx]
    central_frac_mq = central_frac_mq[sort_idx]
    satellite_frac_mq = satellite_frac_mq[sort_idx]

    x_min, x_max, y_min, y_max = get_profile_axes(
        params,
        "central_satellite_fraction_massive_quiescent",
        (0.0, max(z_arr_mq) if len(z_arr_mq) > 0 else 12),
        (0.0, 1.05),
    )

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1963-1972
    ax.plot(z_arr_mq, central_frac_mq, color="steelblue", lw=2.5, label="Centrals", gid="model")
    ax.plot(z_arr_mq, satellite_frac_mq, color="firebrick", lw=2.5, label="Satellites", gid="model")

    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"Redshift $z$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("Fraction (top 10% $M_*$, sSFR $< 0.2/t_{\\rm H}$)", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(0.5))
    ax.yaxis.set_minor_locator(MultipleLocator(0.05))
    ax.legend(loc="center right", frameon=False)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "CentralSatelliteFraction_MassiveQuiescent", output_format, verbose
    )
    return plot_path, None
