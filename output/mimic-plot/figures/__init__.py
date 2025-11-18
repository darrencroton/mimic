"""
Mimic Figure Modules

This package contains self-contained modules for creating various plots from Mimic halo data.
"""

# Standard figure settings for consistent appearance
"""Standard figure settings for consistent appearance across all plots."""
AXIS_LABEL_SIZE = 16  # Font size for axis labels
TICK_LABEL_SIZE = 12  # Font size for tick labels
LEGEND_FONT_SIZE = 12  # Size for legend text (use numeric size instead of 'large')
IN_FIGURE_TEXT_SIZE = 12  # Size for text inside figures (annotations, etc.)


def setup_plot_fonts(ax):
    """Apply consistent font sizes to a plot."""
    # Increase tick label sizes
    ax.tick_params(axis="both", which="major", labelsize=TICK_LABEL_SIZE)
    ax.tick_params(axis="both", which="minor", labelsize=TICK_LABEL_SIZE)

    # Configure global font sizes
    import matplotlib.pyplot as plt

    plt.rcParams.update(
        {
            "font.size": TICK_LABEL_SIZE,
            "legend.fontsize": LEGEND_FONT_SIZE,
            "figure.titlesize": AXIS_LABEL_SIZE,
        }
    )

    # Make sure all labels in legends will use the same font size
    import matplotlib as mpl

    mpl.rcParams["legend.fontsize"] = LEGEND_FONT_SIZE

    return ax


def setup_legend(ax, loc="best", frameon=False):
    """Create a consistently styled legend."""
    leg = ax.legend(loc=loc, numpoints=1, labelspacing=0.1, frameon=frameon)
    for t in leg.get_texts():
        t.set_fontsize(LEGEND_FONT_SIZE)
    return leg


# Utility functions for consistent LaTeX-free labels
def get_mass_function_labels():
    """Return consistent axis labels for mass function plots."""
    y_label = r"$\phi$ (Mpc$^{-3}$ dex$^{-1}$)"
    return y_label


# Halo property utility functions
def get_halo_mass_label():
    """Return consistent x-axis label for halo mass plots."""
    x_label = r"log$_{10}$ M$_{\rm halo}$ (M$_{\odot}$)"
    return x_label


def get_spin_parameter_label():
    """Return consistent x-axis label for spin parameter plots."""
    x_label = r"Spin Parameter"
    return x_label


def get_redshift_label():
    """Return consistent x-axis label for redshift plots."""
    x_label = r"redshift"
    return x_label


def get_vmax_label():
    """Return consistent x-axis label for Vmax plots."""
    x_label = r"log$_{10}$ V$_{\rm max}$ (km/s)"
    return x_label


# Galaxy physics utility functions (require physics modules)
def get_stellar_mass_label():
    """Return consistent x-axis label for stellar mass plots."""
    x_label = r"log$_{10}$ M$_{*}$ [M$_{\odot}$]"
    return x_label


def get_cold_gas_label():
    """Return consistent x-axis label for cold gas plots."""
    x_label = r"log$_{10}$ M$_{\rm cold~gas}$ [M$_{\odot}$]"
    return x_label


# Property availability checking
def check_required_properties(galaxies, required_properties):
    """
    Check if galaxy data contains required properties.

    Args:
        galaxies: NumPy recarray with galaxy data
        required_properties: List of property names required for plot

    Returns:
        tuple: (available, missing) where available is bool and missing is list of str
    """
    if galaxies is None or len(galaxies) == 0:
        return False, required_properties

    # Get available fields from dtype
    available_fields = set(galaxies.dtype.names)

    # Check which required properties are missing
    missing = [prop for prop in required_properties if prop not in available_fields]

    return len(missing) == 0, missing


# Import all the figure modules so they can be discovered
from . import (
    baryon_fraction,
    cold_gas_function,
    halo_mass_function,
    halo_occupation,
    hmf_evolution,
    smf_evolution,
    spatial_distribution,
    spin_distribution,
    stellar_mass_function,
    velocity_distribution,
)

# Define available plot types
"""List of all available snapshot plot modules."""
SNAPSHOT_PLOTS = [
    # Halo property plots (always available)
    "halo_mass_function",
    "halo_occupation",
    "spin_distribution",
    "velocity_distribution",
    "spatial_distribution",
    # Galaxy physics plots (require physics modules)
    "stellar_mass_function",
    "cold_gas_function",
    "baryon_fraction",
]

"""List of all available evolution plot modules."""
EVOLUTION_PLOTS = [
    # Halo property evolution (always available)
    "hmf_evolution",
    # Galaxy physics evolution (require physics modules)
    "smf_evolution",
]

# Define property requirements for each plot
"""Mapping of plot names to required properties."""
PLOT_REQUIREMENTS = {
    # Halo plots (no extra properties needed beyond base halos)
    "halo_mass_function": [],
    "halo_occupation": [],
    "hmf_evolution": [],
    "spin_distribution": [],
    "velocity_distribution": [],
    "spatial_distribution": [],
    # Galaxy physics plots (require specific properties)
    "stellar_mass_function": ["StellarMass"],
    "cold_gas_function": ["ColdGas"],
    "smf_evolution": ["StellarMass"],
    "baryon_fraction": ["Mvir", "Type"],  # Baryonic properties checked internally
}

"""Mapping of plot names to their corresponding functions."""
PLOT_FUNCS = {
    "halo_mass_function": halo_mass_function.plot,
    "halo_occupation": halo_occupation.plot,
    "hmf_evolution": hmf_evolution.plot,
    "spin_distribution": spin_distribution.plot,
    "velocity_distribution": velocity_distribution.plot,
    "spatial_distribution": spatial_distribution.plot,
    "stellar_mass_function": stellar_mass_function.plot,
    "cold_gas_function": cold_gas_function.plot,
    "smf_evolution": smf_evolution.plot,
    "baryon_fraction": baryon_fraction.plot,
}
