"""Mimic SHAM figure modules."""

# Standard figure settings for consistent appearance across all plots.
AXIS_LABEL_SIZE = 16
TICK_LABEL_SIZE = 12
LEGEND_FONT_SIZE = 12
IN_FIGURE_TEXT_SIZE = 12


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


def get_spin_parameter_label():
    """Return consistent x-axis label for spin parameter plots."""
    return r"Spin Parameter"


def get_redshift_label():
    """Return consistent x-axis label for redshift plots."""
    return r"redshift"


def get_vmax_label():
    """Return consistent x-axis label for Vmax plots."""
    return r"log$_{10}$ V$_{\rm max}$ [km/s]"


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


from . import (
    halo_mass_function,
    halo_occupation,
    hmf_evolution,
    sham_satellite_fraction,
    sham_stellar_halo_relation,
    smf_evolution,
    spatial_distribution,
    spin_distribution,
    stellar_mass_density_evolution,
    stellar_mass_function,
    velocity_distribution,
)

SNAPSHOT_PLOTS = [
    "halo_mass_function",
    "halo_occupation",
    "spin_distribution",
    "velocity_distribution",
    "spatial_distribution",
    "stellar_mass_function",
    "sham_stellar_halo_relation",
    "sham_satellite_fraction",
]

EVOLUTION_PLOTS = [
    "hmf_evolution",
    "smf_evolution",
    "stellar_mass_density_evolution",
]

PLOT_REQUIREMENTS = {
    "halo_mass_function": [],
    "halo_occupation": [],
    "hmf_evolution": [],
    "spin_distribution": [],
    "velocity_distribution": [],
    "spatial_distribution": [],
    "stellar_mass_function": ["StellarMass"],
    "smf_evolution": ["StellarMass"],
    "stellar_mass_density_evolution": ["StellarMass"],
    "sham_stellar_halo_relation": [
        "StellarMass",
        "ShamMpeak",
        "ShamVpeak",
        "Type",
    ],
    "sham_satellite_fraction": ["StellarMass", "Type"],
}

PLOT_FUNCS = {
    "halo_mass_function": halo_mass_function.plot,
    "halo_occupation": halo_occupation.plot,
    "hmf_evolution": hmf_evolution.plot,
    "spin_distribution": spin_distribution.plot,
    "velocity_distribution": velocity_distribution.plot,
    "spatial_distribution": spatial_distribution.plot,
    "stellar_mass_function": stellar_mass_function.plot,
    "smf_evolution": smf_evolution.plot,
    "stellar_mass_density_evolution": stellar_mass_density_evolution.plot,
    "sham_stellar_halo_relation": sham_stellar_halo_relation.plot,
    "sham_satellite_fraction": sham_satellite_fraction.plot,
}
