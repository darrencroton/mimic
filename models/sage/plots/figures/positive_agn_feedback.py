#!/usr/bin/env python

"""
Mimic Positive AGN Feedback Plot

Visualises the high-redshift "positive" AGN feedback of

    Silk, Begelman, Norman, Nusser & Wyse (2024),
    "Which came first: supermassive black holes or galaxies? Insights from JWST",
    arXiv:2401.02482,

as implemented by the sage_positive_agn_feedback module.

Two panels:
  (top)    The paper's column-density transition. The host ISM column
           N_H = 10^21 (1+z)^3.3 cm^-2 (paper §3.3) crosses the momentum->energy
           transition column N_cool ~ 10^24 cm^-2 (paper §5.1) near z~6 — the
           "positive -> negative" feedback switch. We plot the smooth weight
           f_pos(z) = N_H / (N_H + N_cool) used by the module.
  (bottom) The measured effect: the fraction of the cosmic stellar-mass budget
           that was formed by positive AGN feedback (AGNTriggeredStellarMass /
           StellarMass), summed over the galaxy population at each snapshot.

Requires: AGNTriggeredStellarMass, StellarMass (from sage_positive_agn_feedback).
"""

import matplotlib.pyplot as plt
import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    IN_FIGURE_TEXT_SIZE,
    get_redshift_label,
    setup_legend,
    setup_plot_fonts,
)
from output_utils import (
    check_required_fields,
    save_and_close_figure,
    warn,
)

# Paper constants (Silk et al. 2024), and the module's default transition column.
# N_H = 10^21 (1+z)^3.3 cm^-2 (§3.3); N_cool ~ 10^24 cm^-2 (§5.1, paper scale).
NH_NORM_CM2 = 1.0e21
NH_Z_EXPONENT = 3.3
# N_cool chosen so the N_H = N_cool crossover lands at the paper's z~6
# (6e23 sits within §5.1's ~10^23-10^24 cm^-2 range). Matches the module default.
N_COOL_DEFAULT_CM2 = 6.0e23


def _f_pos(redshift, n_cool=N_COOL_DEFAULT_CM2):
    """Positive-feedback weight f_pos(z) = N_H / (N_H + N_cool); matches the module."""
    n_h = NH_NORM_CM2 * np.power(1.0 + redshift, NH_Z_EXPONENT)
    return n_h / (n_h + n_cool)


def _transition_redshift(n_cool=N_COOL_DEFAULT_CM2):
    """Redshift where N_H = N_cool (f_pos = 1/2): the positive<->negative switch."""
    return (n_cool / NH_NORM_CM2) ** (1.0 / NH_Z_EXPONENT) - 1.0


def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create the positive AGN feedback diagnostic plot.

    Args:
        snapshots: Dict mapping snapshot numbers to (galaxies, volume, metadata).
        params: Dict of Mimic parameters.
        output_dir: Output directory for the plot.
        output_format: File format for the output.
        verbose: Whether to print verbose output.

    Returns:
        Tuple of (plot_path, skip_message).
    """
    if len(snapshots) == 0:
        return None, "No snapshot data available for positive AGN feedback plot"

    first_snap = next(iter(snapshots.values()))
    galaxies_sample = first_snap[0]

    success, _optional, msg = check_required_fields(
        galaxies_sample,
        required_fields=["AGNTriggeredStellarMass", "StellarMass"],
        plot_name="Positive AGN Feedback",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # Measured effect: triggered stellar-mass fraction per snapshot.
    redshifts = []
    triggered_fraction = []
    for snap, (galaxies, volume, metadata) in snapshots.items():
        z = metadata.get("redshift", 0.0)
        stellar_sum = float(np.sum(galaxies.StellarMass))
        triggered_sum = float(np.sum(galaxies.AGNTriggeredStellarMass))
        if stellar_sum <= 0.0:
            continue
        redshifts.append(z)
        triggered_fraction.append(triggered_sum / stellar_sum)

    if len(redshifts) == 0:
        return None, "No snapshots with positive stellar mass for positive AGN feedback plot"

    redshifts = np.array(redshifts)
    triggered_fraction = np.array(triggered_fraction)
    order = np.argsort(redshifts)
    redshifts = redshifts[order]
    triggered_fraction = triggered_fraction[order]

    if verbose:
        print(f"  Redshifts: {redshifts}")
        print(f"  Triggered fractions: {triggered_fraction}")

    if np.all(triggered_fraction <= 0.0):
        warn("AGNTriggeredStellarMass is zero in every snapshot — is the module enabled?")

    z_trans = _transition_redshift()

    # ---- Build the two-panel figure -------------------------------------
    fig, (ax_top, ax_bot) = plt.subplots(
        2, 1, figsize=(8, 9), sharex=True, gridspec_kw={"height_ratios": [1.0, 1.0]}
    )
    setup_plot_fonts(ax_top)
    setup_plot_fonts(ax_bot)

    z_grid = np.linspace(0.0, max(12.0, float(redshifts.max()) + 1.0), 400)
    f_pos_grid = _f_pos(z_grid)

    # Top panel: the paper's positive-feedback weight f_pos(z).
    ax_top.plot(z_grid, f_pos_grid, "b-", lw=3.0, label=r"$f_{\rm pos}(z)=N_H/(N_H+N_{\rm cool})$")
    ax_top.axvline(z_trans, color="k", ls="--", lw=1.5)
    ax_top.axhline(0.5, color="grey", ls=":", lw=1.0)
    # Shade the positive (momentum-conserving) regime.
    ax_top.axvspan(z_trans, z_grid.max(), color="b", alpha=0.07)
    ax_top.text(
        z_trans + 0.2, 0.08,
        f"positive feedback\n(momentum-conserving)\n" + r"$z \gtrsim$" + f" {z_trans:.1f}",
        fontsize=IN_FIGURE_TEXT_SIZE, color="b", va="bottom",
    )
    ax_top.text(
        z_trans - 0.2, 0.92,
        "negative feedback\n(energy-conserving)",
        fontsize=IN_FIGURE_TEXT_SIZE, color="darkred", va="top", ha="right",
    )
    ax_top.set_ylabel("positive-feedback weight", fontsize=AXIS_LABEL_SIZE)
    ax_top.set_ylim(0.0, 1.0)
    ax_top.set_title(
        "Positive AGN feedback (Silk et al. 2024): "
        + r"$N_H=10^{21}(1+z)^{3.3}\,{\rm cm^{-2}}$ vs $N_{\rm cool}\sim10^{24}\,{\rm cm^{-2}}$",
        fontsize=IN_FIGURE_TEXT_SIZE,
    )
    setup_legend(ax_top, loc="center right")

    # Bottom panel: the measured triggered stellar-mass fraction.
    ax_bot.plot(
        redshifts, triggered_fraction, "o-", color="purple", lw=2.5, ms=6,
        label="Model: AGN-triggered stellar mass fraction",
    )
    ax_bot.axvline(z_trans, color="k", ls="--", lw=1.5, label=f"transition z~{z_trans:.1f}")
    ax_bot.axvspan(z_trans, z_grid.max(), color="b", alpha=0.07)
    ax_bot.set_ylabel(
        r"$M_{\star,\rm AGN-triggered}\,/\,M_{\star,\rm total}$", fontsize=AXIS_LABEL_SIZE
    )
    ax_bot.set_xlabel(get_redshift_label(), fontsize=AXIS_LABEL_SIZE)
    ax_bot.set_ylim(0.0, max(0.05, float(triggered_fraction.max()) * 1.2))
    setup_legend(ax_bot, loc="upper left")

    ax_bot.set_xlim(0.0, z_grid.max())

    fig.tight_layout()
    plot_path = save_and_close_figure(
        fig, output_dir, "Positive_AGN_Feedback", output_format, verbose
    )
    return plot_path, None
