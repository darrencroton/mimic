#!/usr/bin/env python

"""
SAGE26 Ejection Efficiency Against Virial Velocity Plot

Port of the "Ejection vs Vvir" section of SAGE26's plotting script
(plotting/allresults-local.py:2041-2118): the ejection efficiency of galaxies with star
formation, mass loading and a halo against the virial velocity, with the median and one
standard deviation of each virial-velocity bin. The section is the specification; where this
port differs it is a permitted deviation below.

Requires: SfrDisk, SfrBulge, MassLoading, Vvir, Mvir

Fields read (units): SfrDisk and SfrBulge (Msun/yr; the rate is their sum, :2075), MassLoading
(dimensionless), Vvir (km/s) and Mvir (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, :364, only to select).

Run parameters: the disk reheating and halo ejection efficiencies are the run's
FeedbackReheatingEpsilon and FeedbackEjectionEfficiency (modules.parameters), where SAGE26 writes
the literals 2.9 and 0.3 (:2071-2072); the literals are the fallback, with a warning, for a run
that does not give them.

Permitted deviations: the sample comes from one random.Random(2222) per figure, not from SAGE26's
script-wide stream (seeded once, allresults-local.py:357), so the drawn subset differs when the
candidates exceed DILUTE. SAGE26 prints a skip notice when no galaxy is selected (:2051-2053) and
then fails; this figure draws its empty axes instead, as it skips only for a missing field.
"""

import random

import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._binning import binned_median_and_std
from figures._run_parameters import module_parameter
from figures._sampling import draw_sample
from output_utils import (
    check_required_fields,
    save_and_close_figure,
    setup_figure,
)

# Number of galaxies drawn at most: SAGE26's `dilute`, reassigned to 100000 at
# allresults-local.py:1735 and in force here (:2055)
DILUTE = 100000

# Constants of SAGE26's feedback model as the plotting script writes them; the two efficiencies
# are the fallbacks when the run does not give FeedbackReheatingEpsilon and
# FeedbackEjectionEfficiency
EPSILON_DISK = 2.9  # allresults-local.py:2071
EPSILON_HALO = 0.3  # :2072
V_SN = 501.0  # km/s, characteristic supernova velocity (:2073)

# Bin edges of the median line: np.arange(1.0, 3.0, 0.1), allresults-local.py:2105
BIN_EDGES = np.arange(1.0, 3.0, 0.1)


def ejection_efficiency(
    mass_loading, sfr_disk, sfr_bulge, vvir, epsilon_disk=EPSILON_DISK, epsilon_halo=EPSILON_HALO
):
    """
    Return the ejection efficiency of each galaxy (the ejected mass over the star formation rate).

    SAGE26 allresults-local.py:2075-2097: with sfr = SfrDisk + SfrBulge and reheat =
    MassLoading * sfr, the feedback energy is E_fb = 0.5 * epsilon_halo * (MassLoading /
    epsilon_disk) * sfr * 501^2 (SAGE26's epsilon_halo 0.3 and epsilon_disk 2.9) and the lifting
    energy E_lift = 0.5 * reheat * Vvir^2; the ejected mass is
    max(0, (E_fb - E_lift) / (0.5 * Vvir^2)) and the efficiency is that over sfr.
    """
    sfr = sfr_disk + sfr_bulge
    reheat = mass_loading * sfr

    # Extract the pure scaling factor 'f' from eta_reheat (MassLoading)
    f_factor = mass_loading / epsilon_disk

    # Eq. 4: Feedback Energy (using f_factor, not MassLoading)
    E_fb = 0.5 * epsilon_halo * f_factor * sfr * (V_SN**2)

    # Eq. 5: Lifting Energy (using the full MassLoading / eta_reheat)
    E_lift = 0.5 * reheat * (vvir**2)

    # Eq. 6: Ejected mass
    eject = (E_fb - E_lift) / (0.5 * vvir**2)

    # Enforce the condition that ejection cannot be negative
    eject = np.maximum(0.0, eject)

    return eject / sfr


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
    Create the ejection efficiency against virial velocity plot.

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
        required_fields=["SfrDisk", "SfrBulge", "MassLoading", "Vvir", "Mvir"],
        plot_name="Ejection Efficiency Vs Vvir",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    hubble_h = metadata["hubble_h"]
    rng = random.Random(2222)
    epsilon_disk = module_parameter(params, "FeedbackReheatingEpsilon", EPSILON_DISK)
    epsilon_halo = module_parameter(params, "FeedbackEjectionEfficiency", EPSILON_HALO)

    Mvir = galaxies.Mvir * 1.0e10 / hubble_h

    # allresults-local.py:2050-2060
    w = np.where(
        (galaxies.SfrDisk + galaxies.SfrBulge > 0.0)
        & (galaxies.MassLoading > 0.0)
        & (galaxies.Vvir > 0.0)
        & (Mvir > 0.0)
    )[0]
    w = draw_sample(w, rng, DILUTE)  # allresults-local.py:2055
    MassLoading = galaxies.MassLoading[w]
    Vvir = galaxies.Vvir[w]
    SfrDisk = galaxies.SfrDisk[w]
    SfrBulge = galaxies.SfrBulge[w]

    if verbose:
        print("Ejection Efficiency Vs Vvir plot debug:")
        print(f"  Number of galaxies plotted: {len(w)}")

    fig, ax = setup_figure()

    # allresults-local.py:2102-2112. SAGE26 sets no axis window here (:2116 is commented out)
    # and calls no legend, so the median line's label is not shown.
    if len(w) > 0:
        ejection = ejection_efficiency(
            MassLoading, SfrDisk, SfrBulge, Vvir, epsilon_disk, epsilon_halo
        )
        with np.errstate(divide="ignore", invalid="ignore"):
            log_efficiency = np.log10(ejection)
        log_vvir = np.log10(Vvir)
        ax.scatter(log_vvir, log_efficiency, s=2, alpha=0.5, rasterized=True, gid="model")

        # Median line and 1-sigma shading
        bin_centers = 0.5 * (BIN_EDGES[:-1] + BIN_EDGES[1:])
        medians, stds = binned_median_and_std(log_vvir, log_efficiency, BIN_EDGES)
        ax.fill_between(
            bin_centers,
            np.array(medians) - np.array(stds),
            np.array(medians) + np.array(stds),
            alpha=0.2,
            rasterized=True,
            gid="model",
        )
        ax.plot(bin_centers, medians, color="k", lw=2, label="Median ± 1σ", gid="model")

    ax.set_xlabel(r"$\log_{10} V_{\rm vir}\ (\mathrm{km/s})$", fontsize=AXIS_LABEL_SIZE)
    # SAGE26 figure parity: allresults-local.py:2115 labels the axis "Ejection Efficiency", with no
    # log marker, for log10 of the efficiency. Mirrored as written.
    ax.set_ylabel(r"$\mathrm{Ejection\ Efficiency}$", fontsize=AXIS_LABEL_SIZE)

    plot_path = save_and_close_figure(
        fig, output_dir, "ejection_efficiency_vs_vvir", output_format, verbose
    )
    return plot_path, None
