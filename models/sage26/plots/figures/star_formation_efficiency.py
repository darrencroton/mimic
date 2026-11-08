#!/usr/bin/env python

"""
SAGE26 Star Formation Efficiency Plot

Port of the "Plotting star formation efficiency across cosmic time" section of SAGE26's history
plotting script (plotting/allresults-history.py:1731-1798, figure StarFormationEfficiency): the
median of M_* / (f_b M_vir) of central galaxies, with a 16th to 84th percentile band, against the
age of the universe at each snapshot. The section is the specification; where this port differs
it is a permitted deviation below.

Requires: StellarMass, Mvir, Type

Fields read (units): StellarMass and Mvir (1e10 Msun/h each, converted to Msun as SAGE26
converts them: x 1e10 / hubble_h, allresults-history.py:337 and :342) and Type (0 central,
:1762). The cosmology (Omega, OmegaLambda, Hubble_h) is the run's, from the engine's parameters
(plot/mimic-plot/mimic-plot.py:588-593), where SAGE26 reads it from its file header (:1738-1739);
a snapshot's redshift is its metadata["redshift"].

Snapshots: every written snapshot (:1752), chosen through ``all_snapshots``; a snapshot needs at
least 5 central galaxies with positive stellar and halo mass (:1762-1764).

Permitted deviations: SAGE26's `np.trapz` (:1745) is computed with `np.trapezoid`, the same
trapezoidal rule under NumPy 2's name; the baryon fraction f_b is the run's GlobalBaryonFraction
(modules.parameters), where SAGE26 writes the literal 0.17 (:1736), which is the fallback, with a
warning, for a run that does not give it. The figure is drawn from the run's eight snapshots, where
SAGE26 reads 64.
"""

import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._run_parameters import cosmology, module_parameter
from figures._snapshot_selection import all_snapshots
from matplotlib.ticker import MultipleLocator
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Cosmological baryon fraction Omega_b / Omega_m (allresults-history.py:1736): the fallback when
# the run does not give GlobalBaryonFraction
F_BARYON = 0.17

# A snapshot needs at least this many central galaxies (allresults-history.py:1763)
MIN_CENTRALS = 5


def z_to_age_gyr(z, omega_m, omega_l, hubble_h):
    """
    Return the age of the universe at redshift ``z`` in Gyr.

    SAGE26 allresults-history.py:1741-1745: `z_int = np.linspace(z, 1000.0, 10000)`,
    `E_z = np.sqrt(OmegaM * (1 + z_int)**3 + OmegaL)` and
    `(9.778 / Hubble_h) * np.trapz(1.0 / ((1 + z_int) * E_z), z_int)`; `np.trapz` is
    `np.trapezoid` under NumPy 2.
    """
    z_int = np.linspace(z, 1000.0, 10000)
    e_z = np.sqrt(omega_m * (1 + z_int) ** 3 + omega_l)
    return (9.778 / hubble_h) * np.trapezoid(1.0 / ((1 + z_int) * e_z), z_int)


def select_centrals(galaxy_type, stellar_mass, halo_mass):
    """
    Return the indices of central galaxies with positive stellar mass and positive halo mass.

    SAGE26 allresults-history.py:1762: `np.where((gtype == 0) & (mstar > 0) & (mvir > 0))[0]`.
    """
    return np.where((galaxy_type == 0) & (stellar_mass > 0) & (halo_mass > 0))[0]


def efficiency_statistics(stellar_mass, halo_mass, f_baryon=F_BARYON):
    """
    Return the median and the 16th and 84th percentiles of M_* / (f_b M_vir), or None when fewer
    than 5 galaxies are given.

    SAGE26 allresults-history.py:1763-1769: `if len(w) < 5: continue`,
    `sfe = mstar[w] / (mvir[w] * f_baryon)`, then `np.median(sfe)` and
    `np.percentile(sfe, 16)` and `np.percentile(sfe, 84)`.
    """
    if len(stellar_mass) < MIN_CENTRALS:
        return None
    sfe = stellar_mass / (halo_mass * f_baryon)
    return np.median(sfe), np.percentile(sfe, 16), np.percentile(sfe, 84)


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the star formation efficiency plot.

    Args:
        snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
        params: Dictionary with Mimic parameters (Omega, OmegaLambda, Hubble_h and the module
            parameter GlobalBaryonFraction are read)
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
        return None, "No snapshot data available for the star formation efficiency plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["StellarMass", "Mvir", "Type"],
        plot_name="Star Formation Efficiency",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    cosmo = cosmology(params)
    if cosmo is None:
        return None, "Run parameters Omega, OmegaLambda and Hubble_h are missing or zero"
    omega_m, omega_l, run_hubble_h = cosmo
    f_baryon = module_parameter(params, "GlobalBaryonFraction", F_BARYON)

    # allresults-history.py:1747-1769
    t_arr_sfe = []
    med_sfe = []
    p16_sfe = []
    p84_sfe = []

    for snap in all_snapshots(snapshots):
        galaxies, _volume, metadata = snapshots[snap]
        if len(galaxies) == 0:
            continue
        hubble_h = metadata["hubble_h"]
        mstar = galaxies.StellarMass * 1.0e10 / hubble_h
        mvir = galaxies.Mvir * 1.0e10 / hubble_h

        w = select_centrals(galaxies.Type, mstar, mvir)
        statistics = efficiency_statistics(mstar[w], mvir[w], f_baryon)
        if statistics is None:
            continue
        t_arr_sfe.append(z_to_age_gyr(metadata["redshift"], omega_m, omega_l, run_hubble_h))
        med_sfe.append(statistics[0])
        p16_sfe.append(statistics[1])
        p84_sfe.append(statistics[2])

    t_arr_sfe = np.array(t_arr_sfe)
    med_sfe = np.array(med_sfe)
    p16_sfe = np.array(p16_sfe)
    p84_sfe = np.array(p84_sfe)

    # Sort by ascending cosmic time (:1777-1782)
    sort_idx = np.argsort(t_arr_sfe)
    t_arr_sfe = t_arr_sfe[sort_idx]
    med_sfe = med_sfe[sort_idx]
    p16_sfe = p16_sfe[sort_idx]
    p84_sfe = p84_sfe[sort_idx]

    t_today = z_to_age_gyr(0.0, omega_m, omega_l, run_hubble_h)
    x_min, x_max, y_min, y_max = get_profile_axes(
        params, "star_formation_efficiency", (t_today, 0.3), (1.0e-6, 1.0), log_y=True
    )

    fig, ax = setup_figure(figsize=(10, 7))

    # allresults-history.py:1786-1794
    ax.plot(t_arr_sfe, med_sfe, color="steelblue", lw=2.5, gid="model")
    ax.fill_between(t_arr_sfe, p16_sfe, p84_sfe, color="steelblue", alpha=0.3, gid="model")

    ax.set_yscale("log")
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_xlabel(r"Cosmic time [Gyr]", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$\varepsilon_* = M_* \,/\, (f_b \cdot M_{\rm vir})$", fontsize=AXIS_LABEL_SIZE)
    ax.xaxis.set_minor_locator(MultipleLocator(1))

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "StarFormationEfficiency", output_format, verbose
    )
    return plot_path, None
