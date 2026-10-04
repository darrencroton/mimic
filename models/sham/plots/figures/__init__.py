"""Mimic SHAM figure modules.

Registry of the figures for the sham model package: the stellar mass function against the
configured target, the sample-filtered stellar-to-halo relations, the satellite fraction, the
correlation function by stellar-mass threshold, and four halo diagnostics copied unchanged
from the halo catalogue figures. Every SHAM-specific figure selects the sample with
ShamGhost == 0. The shipped run writes one epoch, so the package registers no evolution
figures.
"""

# Standard figure settings for consistent appearance across all plots.
AXIS_LABEL_SIZE = 16
TICK_LABEL_SIZE = 12
LEGEND_FONT_SIZE = 12
IN_FIGURE_TEXT_SIZE = 12

# Sample definition, repeated in every SHAM figure's labels.
SAMPLE_LABEL = r"sample: ShamGhost = 0"


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


def get_stellar_mass_label():
    """Return consistent x-axis label for stellar mass plots."""
    return r"log$_{10}$ M$_{*}$ [M$_{\odot}$]"


def check_required_properties(galaxies, required_properties):
    """
    Check if galaxy data contains required properties.

    Returns:
        tuple: (available, missing) where available is bool and missing is list
        of strings.
    """
    if galaxies is None or len(galaxies) == 0:
        return False, required_properties

    available_fields = set(galaxies.dtype.names)
    missing = [prop for prop in required_properties if prop not in available_fields]
    return len(missing) == 0, missing


from . import (  # noqa: E402  (the helpers above must exist before the figures import them)
    halo_mass_function,
    sham_correlation_function,
    sham_satellite_fraction,
    sham_stellar_halo_relation,
    spatial_distribution,
    spin_distribution,
    stellar_mass_function,
    velocity_distribution,
)

SNAPSHOT_PLOTS = [
    "halo_mass_function",
    "spin_distribution",
    "velocity_distribution",
    "spatial_distribution",
    "stellar_mass_function",
    "sham_stellar_halo_relation",
    "sham_satellite_fraction",
    "sham_correlation_function",
]

EVOLUTION_PLOTS = []

PLOT_REQUIREMENTS = {
    "halo_mass_function": [],
    "spin_distribution": [],
    "velocity_distribution": [],
    "spatial_distribution": [],
    "stellar_mass_function": ["StellarMass", "ShamGhost"],
    "sham_stellar_halo_relation": [
        "StellarMass",
        "ShamMpeak",
        "ShamVpeak",
        "ShamGhost",
        "Type",
    ],
    "sham_satellite_fraction": ["StellarMass", "ShamGhost", "Type"],
    "sham_correlation_function": ["StellarMass", "ShamGhost", "Pos"],
}

PLOT_FUNCS = {
    "halo_mass_function": halo_mass_function.plot,
    "spin_distribution": spin_distribution.plot,
    "velocity_distribution": velocity_distribution.plot,
    "spatial_distribution": spatial_distribution.plot,
    "stellar_mass_function": stellar_mass_function.plot,
    "sham_stellar_halo_relation": sham_stellar_halo_relation.plot,
    "sham_satellite_fraction": sham_satellite_fraction.plot,
    "sham_correlation_function": sham_correlation_function.plot,
}
