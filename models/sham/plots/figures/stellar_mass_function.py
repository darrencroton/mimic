#!/usr/bin/env python

"""
Mimic SHAM Stellar Mass Function Plot

The stellar mass function of the SHAM sample (ShamGhost == 0) against the configured double
Schechter target, with the Baldry et al. (2008) z ~ 0.1 band for context. The target line is
built from the sham_rank_match parameters in params["EnabledModules"]["parameters"] and
converted to the simulation's h exactly as the module converts them (ShamTargetLogMstar by
2 log10(h_obs / h), ShamTargetPhi1 and ShamTargetPhi2 by (h / h_obs)^3), so the ranked
population should track the line wherever it is assigned. The dotted vertical line marks
ShamTargetLogMassFloor, below which the module masks ranks.
Requires: StellarMass and ShamGhost.
"""

import numpy as np
from figures import (
    AXIS_LABEL_SIZE,
    SAMPLE_LABEL,
    get_mass_function_labels,
    get_stellar_mass_label,
    setup_legend,
)
from matplotlib.ticker import MultipleLocator
from output_utils import (
    calculate_mass_function,
    check_field_has_values,
    check_required_fields,
    get_profile_axes,
    save_and_close_figure,
    setup_figure,
    validate_filtered_data,
)

# Physical limits for stellar mass functions
STELLAR_MASS_MAX = 13.0  # log10(Msun) - maximum stellar mass
BINWIDTH_DEX = 0.1  # Standard bin width in dex
PLOT_XLIM = (8.0, 12.5)  # Plot x-axis limits
PLOT_YLIM = (1.0e-6, 1.0e-1)  # Plot y-axis limits

# The sham_rank_match parameters that define the target.
TARGET_PARAMETER_NAMES = (
    "ShamTargetLogMstar",
    "ShamTargetPhi1",
    "ShamTargetAlpha1",
    "ShamTargetPhi2",
    "ShamTargetAlpha2",
    "ShamTargetHubble",
    "ShamTargetLogMassFloor",
)


def read_target_parameters(params):
    """
    Read the run's target parameters from params["EnabledModules"]["parameters"].

    Returns:
        (values, missing): dict name -> float for every parameter that is present and
        numeric, and the list of names that are not.
    """
    module_params = ((params.get("EnabledModules") or {}).get("parameters")) or {}
    values = {}
    missing = []
    for name in TARGET_PARAMETER_NAMES:
        try:
            values[name] = float(module_params[name])
        except (KeyError, TypeError, ValueError):
            missing.append(name)
    return values, missing


def target_mass_function(log_mass, target, hubble_sim):
    """
    Evaluate the double Schechter target per dex at the simulation's h.

    The conversion is the one sham_rank_match_build_target() applies: the Schechter mass moves
    to log10 Ms + 2 log10(h_obs / h_sim) (physical Msun at h_sim) and both normalisations
    scale by (h_sim / h_obs)^3 (Mpc^-3 at h_sim), the units of calculate_mass_function().

    Args:
        log_mass: Array of log10(M*/Msun) at h_sim
        target: Dict from read_target_parameters()
        hubble_sim: The simulation's h

    Returns:
        Number density per dex in Mpc^-3 dex^-1
    """
    h_obs = target["ShamTargetHubble"]
    h_ratio = hubble_sim / h_obs
    log_ms = target["ShamTargetLogMstar"] + 2.0 * np.log10(h_obs / hubble_sim)
    x = 10.0 ** (np.asarray(log_mass, dtype=np.float64) - log_ms)
    phi1 = target["ShamTargetPhi1"] * h_ratio**3
    phi2 = target["ShamTargetPhi2"] * h_ratio**3
    return (
        np.log(10.0)
        * np.exp(-x)
        * (
            phi1 * x ** (target["ShamTargetAlpha1"] + 1.0)
            + phi2 * x ** (target["ShamTargetAlpha2"] + 1.0)
        )
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
    """
    Create a stellar mass function plot.

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
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Extract necessary metadata
    hubble_h = metadata["hubble_h"]

    mass_min, mass_max, y_min, y_max = get_profile_axes(
        params, "stellar_mass_function", PLOT_XLIM, PLOT_YLIM, log_y=True
    )

    target, missing = read_target_parameters(params)
    if missing:
        return None, f"Target parameters missing from modules.parameters: {', '.join(missing)}"

    success, _, msg = check_required_fields(
        galaxies,
        required_fields=["StellarMass", "ShamGhost"],
        plot_name="SHAM Stellar Mass Function",
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation: Check if StellarMass has any non-zero values
    has_values, count, msg = check_field_has_values(
        galaxies.StellarMass, "StellarMass", threshold=0.0
    )
    if not has_values:
        return None, f"Field validation failed: {msg}"

    # The sample: ranked members only (ShamGhost == 0), which carry a positive stellar mass.
    w = np.where((galaxies.ShamGhost == 0) & (galaxies.StellarMass > 0.0))[0]

    # Filter-level validation: Check if filtering produced results
    is_valid, skip_msg = validate_filtered_data(w, "SHAM Stellar Mass Function", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only if validation passed)
    fig, ax = setup_figure()

    mass = np.log10(galaxies.StellarMass[w] * 1.0e10 / hubble_h)

    # Calculate mass function
    xaxis, smf = calculate_mass_function(
        mass, volume, hubble_h, BINWIDTH_DEX, mass_min, STELLAR_MASS_MAX
    )

    # Print debugging info
    if verbose:
        print(f"  mi={mass_min}, ma={STELLAR_MASS_MAX}")
        print(f"  members={len(w)}, min mass={min(mass)}, max mass={max(mass)}")
        print(f"  volume={volume}, hubble_h={hubble_h}")

    # Target double Schechter on the same h conversion as the module.
    log_grid = np.linspace(mass_min, STELLAR_MASS_MAX, 400)
    ax.plot(
        log_grid,
        target_mass_function(log_grid, target, hubble_h),
        "-",
        color="#c44e52",
        lw=2,
        label="Target (double Schechter)",
    )
    ax.axvline(target["ShamTargetLogMassFloor"], color="0.5", ls=":", lw=1.5)

    # Plot stellar mass function (empty bins have nothing to show on the log axis)
    populated = smf > 0.0
    ax.plot(xaxis[populated], smf[populated], "k-", label="Model (" + SAMPLE_LABEL + ")")

    # Baldry, Glazebrook & Driver (2008), MNRAS 388, 945: z~0.1 field stellar
    # mass function. Columns: log10(M*) (Salpeter IMF; shifted -0.26 dex to
    # Chabrier below), phi, phi uncertainty — h-dependence removed by the
    # hubble_h conversions that follow.
    baldry = np.array(
        [
            [7.05, 1.3531e-01, 6.0741e-02],
            [7.15, 1.3474e-01, 6.0109e-02],
            [7.25, 2.0971e-01, 7.7965e-02],
            [7.35, 1.7161e-01, 3.1841e-02],
            [7.45, 2.1648e-01, 5.7832e-02],
            [7.55, 2.1645e-01, 3.9988e-02],
            [7.65, 2.0837e-01, 4.8713e-02],
            [7.75, 2.0402e-01, 7.0061e-02],
            [7.85, 1.5536e-01, 3.9182e-02],
            [7.95, 1.5232e-01, 2.6824e-02],
            [8.05, 1.5067e-01, 4.8824e-02],
            [8.15, 1.3032e-01, 2.1892e-02],
            [8.25, 1.2545e-01, 3.5526e-02],
            [8.35, 9.8472e-02, 2.7181e-02],
            [8.45, 8.7194e-02, 2.8345e-02],
            [8.55, 7.0758e-02, 2.0808e-02],
            [8.65, 5.8190e-02, 1.3359e-02],
            [8.75, 5.6057e-02, 1.3512e-02],
            [8.85, 5.1380e-02, 1.2815e-02],
            [8.95, 4.4206e-02, 9.6866e-03],
            [9.05, 4.1149e-02, 1.0169e-02],
            [9.15, 3.4959e-02, 6.7898e-03],
            [9.25, 3.3111e-02, 8.3704e-03],
            [9.35, 3.0138e-02, 4.7741e-03],
            [9.45, 2.6692e-02, 5.5029e-03],
            [9.55, 2.4656e-02, 4.4359e-03],
            [9.65, 2.2885e-02, 3.7915e-03],
            [9.75, 2.1849e-02, 3.9812e-03],
            [9.85, 2.0383e-02, 3.2930e-03],
            [9.95, 1.9929e-02, 2.9370e-03],
            [10.05, 1.8865e-02, 2.4624e-03],
            [10.15, 1.8136e-02, 2.5208e-03],
            [10.25, 1.7657e-02, 2.4217e-03],
            [10.35, 1.6616e-02, 2.2784e-03],
            [10.45, 1.6114e-02, 2.1783e-03],
            [10.55, 1.4366e-02, 1.8819e-03],
            [10.65, 1.2588e-02, 1.8249e-03],
            [10.75, 1.1372e-02, 1.4436e-03],
            [10.85, 9.1213e-03, 1.5816e-03],
            [10.95, 6.1125e-03, 9.6735e-04],
            [11.05, 4.3923e-03, 9.6254e-04],
            [11.15, 2.5463e-03, 5.0038e-04],
            [11.25, 1.4298e-03, 4.2816e-04],
            [11.35, 6.4867e-04, 1.6439e-04],
            [11.45, 2.8294e-04, 9.9799e-05],
            [11.55, 1.0617e-04, 4.9085e-05],
            [11.65, 3.2702e-05, 2.4546e-05],
            [11.75, 1.2571e-05, 1.2571e-05],
            [11.85, 8.4589e-06, 8.4589e-06],
            [11.95, 7.4764e-06, 7.4764e-06],
        ],
        dtype=np.float32,
    )

    # Convert Baldry data to appropriate units and IMF (Chabrier, the target's IMF)
    baldry_xval = np.log10(10 ** baldry[:, 0] / hubble_h / hubble_h) - 0.26

    baldry_yvalU = (baldry[:, 1] + baldry[:, 2]) * hubble_h * hubble_h * hubble_h
    baldry_yvalL = (baldry[:, 1] - baldry[:, 2]) * hubble_h * hubble_h * hubble_h

    # Plot observational data with uncertainty band
    ax.fill_between(baldry_xval, baldry_yvalU, baldry_yvalL, facecolor="purple", alpha=0.25)

    # Add a legend entry for Baldry data
    ax.plot([], [], color="purple", alpha=0.3, lw=8, label="Baldry et al. 2008 (z=0.1)")

    # Customize the plot
    ax.set_yscale("log")
    ax.set_xlim(mass_min, mass_max)
    ax.set_ylim(y_min, y_max)
    ax.xaxis.set_minor_locator(MultipleLocator(BINWIDTH_DEX))

    # Set labels with larger font sizes
    ax.set_ylabel(get_mass_function_labels(), fontsize=AXIS_LABEL_SIZE)
    ax.set_xlabel(get_stellar_mass_label(), fontsize=AXIS_LABEL_SIZE)

    # Add consistently styled legend
    setup_legend(ax, loc="lower left")

    # Save and close the figure
    plot_path = save_and_close_figure(
        fig, output_dir, "StellarMassFunction", output_format, verbose
    )
    return plot_path, None
