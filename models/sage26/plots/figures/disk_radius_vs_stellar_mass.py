#!/usr/bin/env python

"""
SAGE26 Disk Radius Against Virial Radius Plot

Port of the "Plotting disk radii vs stellar mass" section of SAGE26's plotting script
(plotting/allresults-local.py:2120-2188): the effective disk radius against the virial radius of
galaxies with a stellar mass between 10^9.3 and 10^11.3 Msun (the stellar mass only selects),
with the median and one standard deviation of each virial-radius bin and the Somerville et al.
(2018) stellar-to-halo size ratio. The section is the specification; where this port differs
it is a permitted deviation below.

Requires: DiskScaleRadius, StellarMass, Rvir

Fields read (units): DiskScaleRadius (Mpc/h; SAGE26's DiskRadius, :2129), StellarMass (1e10
Msun/h, converted to Msun as SAGE26 converts it: x 1e10 / hubble_h, :2137) and Rvir (Mpc/h,
:2138). Both radii are drawn in kpc/h. SAGE26's `RSRHSomerville.dat` overlay is held inline as
OBS_SOMERVILLE18.

Permitted deviations: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once, allresults-local.py:357), so the drawn subset differs when the
candidates exceed DILUTE. SAGE26 prints a skip notice when no galaxy is selected (:2142-2144) and
then fails; this figure draws its empty axes instead, as it skips only for a missing field.
"""

import random
import warnings

import numpy as np
from figures import AXIS_LABEL_SIZE, setup_legend
from output_utils import (
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26's `dilute`, reassigned to 100000 at
# allresults-local.py:1735 and in force here (:2146)
DILUTE = 100000

# Somerville et al. (2018), GAMA at z = 0.1: the stellar-to-halo size ratio SRHR (the stellar
# scale radius over the halo radius) in eight stellar mass bins, transcribed from SAGE26's
# data/SizesAndAM/RSRHSomerville.dat (read at allresults-local.py:2169). Columns: log10(Mstar /
# Msun), SRHR, in the file's row order. SAGE26 draws column 1 only (:2170).
OBS_SOMERVILLE18 = np.array(
    [
        [9.25168, 0.0183622],
        [9.55369, 0.0169048],
        [9.85570, 0.0163679],
        [10.1633, 0.0164830],
        [10.4541, 0.0156776],
        [10.7673, 0.0163295],
        [11.0470, 0.0176335],
        [11.3434, 0.0169815],
    ]
)

# Number of bin edges of the median line: np.logspace(..., 50), allresults-local.py:2155
N_BIN_EDGES = 50


def draw_sample(candidates, rng):
    """
    Return the galaxies to draw: all candidates, or a random sample of DILUTE of them.

    SAGE26 allresults-local.py:2146: `if(len(w) > dilute): w = sample(list(w), dilute)`, with
    ``rng`` standing in for SAGE26's script-wide stream.
    """
    if len(candidates) > DILUTE:
        return rng.sample(list(candidates), DILUTE)
    return candidates


def effective_disk_radius(disk_scale_radius):
    """
    Return the effective disk radius in kpc/h from the disk scale length in Mpc/h.

    SAGE26 allresults-local.py:2129-2136: the scale length times 1.0e3 (Mpc/h to kpc/h), then
    times 1.68 (r_e = 1.68 r_d for a pure exponential disk).
    """
    disk_radius = disk_scale_radius * 1.0e3  # Convert to kpc/h
    disk_radius = disk_radius * 1.68  # Scale length -> effective radius
    return disk_radius


def virial_radius_kpc(rvir):
    """
    Return the virial radius in kpc/h from Mpc/h.

    SAGE26 allresults-local.py:2138-2139: `Rvir * 1.0e3`.
    """
    return rvir * 1.0e3


def binned_median_and_std(x, values, bin_edges):
    """
    Return the median and standard deviation of ``values`` in each bin of ``x``.

    SAGE26 allresults-local.py:2157-2159: np.digitize of x on the edges, then the median and
    np.std of the values in bin i for i = 1 .. len(edges) - 1. An empty bin has no median
    (NaN).
    """
    digitized = np.digitize(x, bin_edges)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        medians = [np.median(values[digitized == i]) for i in range(1, len(bin_edges))]
        stds = [np.std(values[digitized == i]) for i in range(1, len(bin_edges))]
    return medians, stds


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
    Create the disk radius against virial radius plot.

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
        required_fields=["DiskScaleRadius", "StellarMass", "Rvir"],
        plot_name="Disk Radius Vs Stellar Mass",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    rng = random.Random(2222)
    # SAGE26 sets only the y window (allresults-local.py:2183); its x window is the data's
    _, _, y_min, y_max = get_profile_axes(
        params, "disk_radius_vs_stellar_mass", (None, None), (10**-2, 10**2)
    )

    # allresults-local.py:2129-2139
    DiskRadius = effective_disk_radius(galaxies.DiskScaleRadius)
    StellarMass = galaxies.StellarMass * 1.0e10 / hubble_h  # Convert to solar masses
    Rvir = virial_radius_kpc(galaxies.Rvir)

    # allresults-local.py:2141-2146. A galaxy with no stellar mass has log10 = -inf, which the
    # mass window leaves out.
    with np.errstate(divide="ignore", invalid="ignore"):
        log_stellar_mass = np.log10(StellarMass)
    w = np.where(
        (Rvir > 0.0) & (DiskRadius > 0.0) & (log_stellar_mass > 9.3) & (log_stellar_mass < 11.3)
    )[0]
    w = draw_sample(w, rng)

    if verbose:
        print("Disk Radius Vs Stellar Mass plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")

    fig, ax = setup_figure()

    if len(w) > 0:
        # allresults-local.py:2152
        ax.scatter(Rvir[w], DiskRadius[w], s=2, alpha=0.5, rasterized=True, gid="model")

        # Median line and 1-sigma shading, allresults-local.py:2155-2162
        bin_edges = np.logspace(np.log10(Rvir[w].min()), np.log10(Rvir[w].max()), N_BIN_EDGES)
        bin_centers = 0.5 * (bin_edges[:-1] + bin_edges[1:])
        medians, stds = binned_median_and_std(Rvir[w], DiskRadius[w], bin_edges)
        ax.fill_between(
            bin_centers,
            np.array(medians) - np.array(stds),
            np.array(medians) + np.array(stds),
            alpha=0.2,
            rasterized=True,
            gid="model",
        )
        ax.plot(
            bin_centers,
            medians,
            color="k",
            lw=2,
            label="Median ± 1σ (matching Somerville+18 selection)",
            gid="model",
        )

        # Somerville et al. (2018), allresults-local.py:2164-2176. Both radii are kpc/h, so the
        # ratio carries no h and needs no correction.
        re_over_rvir = OBS_SOMERVILLE18[:, 1]
        rvir_ref = np.array([Rvir[w].min(), Rvir[w].max()])
        ax.fill_between(
            rvir_ref,
            re_over_rvir.min() * rvir_ref,
            re_over_rvir.max() * rvir_ref,
            color="firebrick",
            alpha=0.25,
            zorder=5,
            label=r"Somerville+18 SRHR (9.3 < $\log M_\star$ < 11.3)",
        )
        ax.plot(
            rvir_ref,
            np.median(re_over_rvir) * rvir_ref,
            "--",
            color="firebrick",
            lw=2,
            zorder=6,
        )

    ax.set_xlabel(r"$R_{\rm vir}\ (\mathrm{kpc}/h)$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel(r"$R_{\rm e,disk}\ (\mathrm{kpc}/h)$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylim(y_min, y_max)
    ax.set_xscale("log")
    ax.set_yscale("log")
    setup_legend(ax, loc="upper left", frameon=False)

    plot_path = save_and_close_figure(
        fig, output_dir, "disk_radius_vs_stellar_mass", output_format, verbose
    )
    return plot_path, None
