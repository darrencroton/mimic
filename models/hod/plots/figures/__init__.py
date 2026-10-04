"""Mimic HOD figure modules.

Registry of the figures for the hod model package: the mean occupation function with the
analytic law overlaid, the satellite phase-space check, the real-space correlation function,
and two halo diagnostics copied unchanged from halos-only (halo_mass_function and
spatial_distribution). Every HOD-specific figure selects the sample with HODGhost == 0.
"""

import numpy as np

AXIS_LABEL_SIZE = 16
TICK_LABEL_SIZE = 12
LEGEND_FONT_SIZE = 12
IN_FIGURE_TEXT_SIZE = 12

# Sample definition, repeated in every HOD figure's labels.
SAMPLE_LABEL = r"sample: HODGhost = 0"


def setup_plot_fonts(ax):
    """Apply consistent font sizes to a plot."""
    ax.tick_params(axis="both", which="major", labelsize=TICK_LABEL_SIZE)
    ax.tick_params(axis="both", which="minor", labelsize=TICK_LABEL_SIZE)

    import matplotlib as mpl
    import matplotlib.pyplot as plt

    plt.rcParams.update(
        {
            "font.size": TICK_LABEL_SIZE,
            "legend.fontsize": LEGEND_FONT_SIZE,
            "figure.titlesize": AXIS_LABEL_SIZE,
        }
    )
    mpl.rcParams["legend.fontsize"] = LEGEND_FONT_SIZE
    return ax


def setup_legend(ax, loc="best", frameon=False):
    """Create a consistently styled legend."""
    leg = ax.legend(loc=loc, numpoints=1, labelspacing=0.1, frameon=frameon)
    for text in leg.get_texts():
        text.set_fontsize(LEGEND_FONT_SIZE)
    return leg


def get_mass_function_labels():
    """Return consistent axis labels for mass function plots."""
    return r"$\phi$ [Mpc$^{-3}$ dex$^{-1}$]"


def get_halo_mass_label():
    """Return consistent x-axis label for halo mass plots."""
    return r"log$_{10}$ M$_{\rm halo}$ [M$_{\odot}$]"


def check_required_properties(galaxies, required_properties):
    """
    Check if halo data contains required properties.

    Returns:
        tuple: (available, missing) where available is bool and missing is list
        of strings.
    """
    if galaxies is None or len(galaxies) == 0:
        return False, required_properties

    available_fields = set(galaxies.dtype.names)
    missing = [prop for prop in required_properties if prop not in available_fields]
    return len(missing) == 0, missing


HOD_PARAMETER_NAMES = (
    "HODLogMmin",
    "HODSigmaLogM",
    "HODLogM0",
    "HODLogM1",
    "HODAlpha",
    "HODConcA",
    "HODConcLogMpivot",
    "HODConcB",
    "HODConcC",
)


def read_hod_parameters(params, names=HOD_PARAMETER_NAMES):
    """
    Read the run's hod_populate parameters from params["EnabledModules"]["parameters"].

    Args:
        params: Mimic params dict (the run file's modules section is EnabledModules).
        names: Parameter names to read.

    Returns:
        (values, missing): dict name -> float for every parameter that is present and
        numeric, and the list of names that are not.
    """
    module_params = ((params.get("EnabledModules") or {}).get("parameters")) or {}
    values = {}
    missing = []
    for name in names:
        try:
            values[name] = float(module_params[name])
        except (KeyError, TypeError, ValueError):
            missing.append(name)
    return values, missing


def host_lookup(galaxies):
    """
    Map each created satellite to the Type 0 host named by its UniqueCentralGalaxyID.

    Returns:
        (host_rows, sat_rows, sat_host): indices of the Type 0 rows, indices of the rows with
        Type 2 and HODGhost == 0 whose host was found, and for each such satellite the
        index (into galaxies) of its host row.
    """
    host_rows = np.where(galaxies.Type == 0)[0]
    sat_rows = np.where((galaxies.Type == 2) & (galaxies.HODGhost == 0))[0]
    if len(host_rows) == 0 or len(sat_rows) == 0:
        return host_rows, sat_rows[:0], sat_rows[:0]

    host_ids = galaxies.UniqueGalaxyID[host_rows]
    order = np.argsort(host_ids)
    sorted_ids = host_ids[order]
    wanted = galaxies.UniqueCentralGalaxyID[sat_rows]
    pos = np.clip(np.searchsorted(sorted_ids, wanted), 0, len(sorted_ids) - 1)
    found = sorted_ids[pos] == wanted
    return host_rows, sat_rows[found], host_rows[order[pos[found]]]


from . import (  # noqa: E402  (the helpers above must exist before the figures import them)
    halo_mass_function,
    hod_correlation_function,
    hod_occupation,
    hod_satellite_profile,
    spatial_distribution,
)

SNAPSHOT_PLOTS = [
    "halo_mass_function",
    "hod_occupation",
    "hod_satellite_profile",
    "hod_correlation_function",
    "spatial_distribution",
]

EVOLUTION_PLOTS = []

PLOT_REQUIREMENTS = {
    "halo_mass_function": [],
    "hod_occupation": ["Type", "Mvir", "HODGhost", "UniqueGalaxyID", "UniqueCentralGalaxyID"],
    "hod_satellite_profile": [
        "Type",
        "SnapNum",
        "Pos",
        "Vel",
        "Rvir",
        "Vvir",
        "infallMvir",
        "HODGhost",
        "UniqueGalaxyID",
        "UniqueCentralGalaxyID",
    ],
    "hod_correlation_function": ["Type", "Pos", "HODGhost"],
    "spatial_distribution": [],
}

PLOT_FUNCS = {
    "halo_mass_function": halo_mass_function.plot,
    "hod_occupation": hod_occupation.plot,
    "hod_satellite_profile": hod_satellite_profile.plot,
    "hod_correlation_function": hod_correlation_function.plot,
    "spatial_distribution": spatial_distribution.plot,
}
