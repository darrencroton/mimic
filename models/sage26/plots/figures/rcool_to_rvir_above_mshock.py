#!/usr/bin/env python

"""
SAGE26 Cooling Radius over Virial Radius Plot, Haloes above the Shock Mass

Port of the "Quick visual: rcool/Rvir distribution for the Mvir > Mshock subset" section of
SAGE26's history plotting script (plotting/allresults-history.py:387-455, figure
RcoolToRvir_above_Mshock). The section is the specification; where this port differs it is a
permitted deviation below.

Requires: Mvir, RcoolToRvir

Fields read (units): Mvir (1e10 Msun/h, converted to Msun as SAGE26 converts it:
x 1e10 / hubble_h, allresults-history.py:342) and RcoolToRvir (rcool / Rvir, dimensionless; a
negative value is SAGE26's "undefined" sentinel and is excluded, :413). Every galaxy row of the
snapshot is read, with no Type cut, as in SAGE26.

Snapshots: the written snapshot nearest z = 0, 2 and 6 (:390, :399-400), chosen through
``nearest_snapshot``.

Permitted deviations: the nearest snapshot is searched among the written snapshots only (SAGE26
searches all 64 and the run writes eight); the measurement SAGE26 prints (:387-429) goes to the log
(printed with --verbose), and only the strict fraction in each legend entry stays on the figure, as
in SAGE26 (:443-446).
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import AXIS_LABEL_SIZE
from figures._snapshot_selection import nearest_snapshot
from output_utils import (
    check_required_fields,
    save_and_close_figure,
    setup_figure,
)

# Mshock = 6e11 Msun, physical (allresults-history.py:389, `Mshock_Msun = 6.0e11`)
MSHOCK_MSUN = 6.0e11

# Target redshifts (:390, `measure_targets = [0.0, 2.0, 6.0]`)
TARGET_REDSHIFTS = [0.0, 2.0, 6.0]

# Histogram bins (:432, `rr_bins = np.arange(0.0, 2.2, 0.05)`)
RATIO_BINS = np.arange(0.0, 2.2, 0.05)


def select_above_mshock(mvir_msun, rcool_ratio):
    """
    Return the mask of haloes above the shock mass with a defined cooling radius.

    SAGE26 allresults-history.py:413 and :440: `sel = (Mvir > Mshock_Msun) & (rcool_ratio >= 0.0)`, with
    Mvir in Msun.
    """
    return (mvir_msun > MSHOCK_MSUN) & (rcool_ratio >= 0.0)


def strict_fraction(rcool_ratio):
    """
    Return the fraction of selected haloes with rcool / Rvir above 1.

    SAGE26 allresults-history.py:421: `f_strict = np.mean(rcool_ratio[sel] > 1.0)`.
    """
    return np.mean(rcool_ratio > 1.0)


def at_least_one_fraction(rcool_ratio):
    """
    Return the fraction of selected haloes with rcool / Rvir at or above 1.

    SAGE26 allresults-history.py:422: `f_geq = np.mean(rcool_ratio[sel] >= 1.0)` (logged only).
    """
    return np.mean(rcool_ratio >= 1.0)


def clip_to_bins(rcool_ratio):
    """
    Return the ratios clipped to the ends of the histogram bins.

    SAGE26 allresults-history.py:443: `np.clip(rcool_ratio[sel], rr_bins[0], rr_bins[-1])`.
    """
    return np.clip(rcool_ratio, RATIO_BINS[0], RATIO_BINS[-1])


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the cooling radius over virial radius distribution plot.

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
        return None, "No snapshot data available for the rcool/Rvir plot"
    first_snap = next(iter(snapshots.values()))

    success, _, msg = check_required_fields(
        first_snap[0],
        required_fields=["Mvir", "RcoolToRvir"],
        plot_name="Rcool to Rvir above Mshock",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # SAGE26 sets no axis window (allresults-history.py:446-453)
    colors = plt.cm.viridis(np.linspace(0.15, 0.85, len(TARGET_REDSHIFTS)))

    fig, ax = setup_figure()

    if verbose:
        print(f"  Mshock = {MSHOCK_MSUN:.2e} Msun (De Lucia & Blaizot 2006 eq. 38)")
        print(
            f"  {'z':>6} {'snap':>5} {'N(Mvir>Mshock)':>15} "
            f"{'f(rcool>Rvir)':>14} {'f(rcool>=Rvir)':>15}"
        )

    # allresults-history.py:434-446
    for k, target_z in enumerate(TARGET_REDSHIFTS):
        snap = nearest_snapshot(snapshots, target_z)
        galaxies, _volume, metadata = snapshots[snap]
        if len(galaxies) == 0:
            continue
        z_here = metadata["redshift"]
        hubble_h = metadata["hubble_h"]

        mvir = galaxies.Mvir * 1.0e10 / hubble_h
        rcool_ratio = galaxies.RcoolToRvir
        sel = select_above_mshock(mvir, rcool_ratio)
        n_selected = int(np.sum(sel))
        if n_selected == 0:
            if verbose:
                print(f"  {z_here:>6.2f} {snap:>5} {0:>15} {'--':>14} {'--':>15}")
            continue

        f_strict = strict_fraction(rcool_ratio[sel])
        if verbose:
            f_geq = at_least_one_fraction(rcool_ratio[sel])
            print(f"  {z_here:>6.2f} {snap:>5} {n_selected:>15} {f_strict:>14.3f} {f_geq:>15.3f}")

        _, _, patches = ax.hist(
            clip_to_bins(rcool_ratio[sel]),
            bins=RATIO_BINS,
            histtype="step",
            lw=2,
            density=True,
            color=colors[k],
            label=f"z = {z_here:.1f}  (N={n_selected}, f$_{{>1}}$={f_strict:.2f})",
        )
        for patch in patches:
            patch.set_gid("model")

    # allresults-history.py:447-453
    ax.axvline(1.0, color="k", ls="--", lw=1, alpha=0.7)
    ax.set_xlabel(r"$r_{\rm cool}\,/\,R_{\rm vir}$", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("PDF", fontsize=AXIS_LABEL_SIZE)
    ax.set_title(r"$M_{\rm vir} > M_{\rm shock}\ (6\times10^{11}\,M_\odot)$ haloes")
    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc="upper left", frameon=False)

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "RcoolToRvir_above_Mshock", output_format, verbose
    )
    return plot_path, None
