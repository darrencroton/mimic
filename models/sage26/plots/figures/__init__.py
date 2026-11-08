"""Mimic SAGE26 figure modules."""

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


def get_cold_gas_label():
    """Return consistent x-axis label for cold gas plots."""
    return r"log$_{10}$ M$_{\rm cold~gas}$ [M$_{\odot}$]"


def get_baryonic_mass_label():
    """Return consistent x-axis label for baryonic mass plots."""
    return r"log$_{10}$ M$_{\rm bar}$ [M$_{\odot}$]"


def get_gas_mass_label():
    """Return consistent x-axis label for gas mass plots."""
    return r"log$_{10}$ M$_{\rm X}$ [M$_{\odot}$]"


def get_sfr_density_label():
    """Return consistent y-axis label for SFR density plots."""
    return r"log$_{10}$ SFR density [M$_{\odot}$ yr$^{-1}$ Mpc$^{-3}$]"


def get_ssfr_label():
    """Return consistent y-axis label for specific SFR plots."""
    return r"log$_{10}$ sSFR [yr$^{-1}$]"


def get_black_hole_mass_label():
    """Return consistent x-axis label for black hole mass plots."""
    return r"log$_{10}$ M$_{\rm BH}$ [M$_{\odot}$]"


def get_bulge_mass_label():
    """Return consistent x-axis label for bulge mass plots."""
    return r"log$_{10}$ M$_{\rm bulge}$ [M$_{\odot}$]"


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
    baryon_fraction,
    baryonic_mass_function,
    baryonic_tully_fisher,
    black_hole_bulge_relation,
    black_hole_mass_function,
    bulge_mass_fraction,
    bulge_to_total,
    central_satellite_counts,
    central_satellite_fraction,
    central_satellite_fraction_massive_quiescent,
    cgm_fraction_vs_mvir,
    cold_gas_function,
    concentration_evolution,
    concentration_vs_mvir,
    disk_radius_vs_stellar_mass,
    ejection_efficiency_vs_vvir,
    ffb_fraction_cut_down,
    gas_fraction,
    gas_mass_function,
    h2_fraction_evolution,
    halo_mass_function,
    halo_occupation,
    hmf_evolution,
    mass_loading_vs_vvir,
    mass_reservoir_scatter,
    metallicity,
    metallicity_evolution,
    quenched_central_satellite_counts,
    quenched_fraction_evolution,
    quiescent_fraction,
    rcool_to_rvir_above_mshock,
    sfr_density_evolution,
    sfr_evolution,
    sfr_vs_halo_accretion_rate,
    smf_evolution,
    spatial_distribution,
    spatial_distribution_3d_box,
    specific_sfr,
    specific_star_formation_rate,
    spin_distribution,
    star_formation_efficiency,
    star_formation_rate,
    stellar_mass_density_evolution,
    stellar_mass_function,
    stellar_mass_halo_mass_relation,
    stellar_vs_halo_mass_by_regime,
    tcool_vs_mvir,
    velocity_distribution,
)

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
    "baryonic_mass_function",
    "gas_mass_function",
    "baryonic_tully_fisher",
    "specific_sfr",
    "black_hole_bulge_relation",
    "gas_fraction",
    "metallicity",
    "bulge_mass_fraction",
    "quiescent_fraction",
    "mass_reservoir_scatter",
    # SAGE26 allresults-local.py figures no forked figure covers
    "bulge_to_total",
    "spatial_distribution_3d_box",
    "star_formation_rate",
    "mass_loading_vs_vvir",
    "stellar_vs_halo_mass_by_regime",
    "specific_star_formation_rate",
    "concentration_vs_mvir",
    "sfr_vs_halo_accretion_rate",
    "tcool_vs_mvir",
    "cgm_fraction_vs_mvir",
    "ejection_efficiency_vs_vvir",
    "disk_radius_vs_stellar_mass",
]

EVOLUTION_PLOTS = [
    # Halo property evolution (always available)
    "hmf_evolution",
    # Galaxy physics evolution (require physics modules)
    "smf_evolution",
    "sfr_density_evolution",
    "stellar_mass_density_evolution",
    # SAGE26 allresults-history.py figures no forked figure covers
    "rcool_to_rvir_above_mshock",
    "sfr_evolution",
    "stellar_mass_halo_mass_relation",
    "black_hole_mass_function",
    "quenched_fraction_evolution",
    "concentration_evolution",
    "metallicity_evolution",
    "h2_fraction_evolution",
    "star_formation_efficiency",
    "central_satellite_counts",
    "central_satellite_fraction",
    "central_satellite_fraction_massive_quiescent",
    "quenched_central_satellite_counts",
    # Cut-down port of SAGE26's paper_plots.py figure 23c (eight snapshots, no MBK25 run)
    "ffb_fraction_cut_down",
]

PLOT_REQUIREMENTS = {
    # Every field a figure reads unconditionally: all it reads except the fields it names in
    # the optional_fields of its check_required_fields call, whether a missing one would make
    # it raise or return a skip. The engine skips a figure whose entry names an absent field.
    # Halo plots
    "halo_mass_function": ["Mvir", "Type"],
    "halo_occupation": ["Mvir", "Type", "UniqueCentralGalaxyID"],
    "hmf_evolution": ["Mvir", "Type"],
    "spin_distribution": ["Spin", "Rvir", "Vvir"],
    "velocity_distribution": ["Pos", "Vel"],
    "spatial_distribution": ["Mvir", "Pos"],
    # Galaxy physics plots
    "stellar_mass_function": ["StellarMass"],  # StarFormationRate optional (red/blue split)
    "cold_gas_function": ["ColdGas"],
    "smf_evolution": ["StellarMass"],
    # Reservoir fields (StellarMass, ColdGas, HotGas, CGMgas, EjectedGas, ICS, BlackHoleMass)
    # are optional: the figure draws those the run writes
    "baryon_fraction": ["Mvir", "Type", "UniqueCentralGalaxyID"],
    "baryonic_mass_function": ["StellarMass", "ColdGas"],
    "gas_mass_function": ["ColdGas"],  # H2gas, H1gas, SfrDisk, SfrBulge, StellarMass optional
    "baryonic_tully_fisher": ["StellarMass", "ColdGas", "Vmax", "BulgeMass", "Type"],
    "specific_sfr": ["StellarMass", "StarFormationRate"],
    "black_hole_bulge_relation": ["BlackHoleMass", "BulgeMass"],
    "gas_fraction": ["StellarMass", "ColdGas", "BulgeMass", "Type"],
    "metallicity": ["StellarMass", "ColdGas", "MetalsColdGas", "Type"],
    "bulge_mass_fraction": ["StellarMass", "BulgeMass"],
    "quiescent_fraction": ["StellarMass", "StarFormationRate", "Type"],  # CentralMvir optional
    "mass_reservoir_scatter": [
        "Mvir",
        "Type",
        "StellarMass",
        "ColdGas",
        "HotGas",
        "CGMgas",
        "EjectedGas",
        "ICS",
    ],
    # SAGE26 allresults-local.py figures no forked figure covers
    "bulge_to_total": ["StellarMass", "BulgeMass", "MergerBulgeMass", "InstabilityBulgeMass"],
    "spatial_distribution_3d_box": ["Mvir", "StellarMass", "Pos"],
    "star_formation_rate": ["StellarMass", "SfrDisk", "SfrBulge"],
    "mass_loading_vs_vvir": ["StellarMass", "MassLoading", "Vvir"],
    "stellar_vs_halo_mass_by_regime": ["Mvir", "StellarMass", "Regime"],
    "specific_star_formation_rate": ["StellarMass", "SfrDisk", "SfrBulge"],
    "concentration_vs_mvir": ["Concentration", "Mvir", "StellarMass", "Type"],
    "sfr_vs_halo_accretion_rate": [
        "SfrDisk",
        "SfrBulge",
        "mdot_cool",
        "mdot_stream",
        "StellarMass",
    ],
    "tcool_vs_mvir": ["CoolingRate", "Mvir", "Rvir", "Regime", "Vvir"],
    "cgm_fraction_vs_mvir": ["CGMgas", "Mvir"],
    "ejection_efficiency_vs_vvir": ["SfrDisk", "SfrBulge", "MassLoading", "Vvir", "Mvir"],
    "disk_radius_vs_stellar_mass": ["DiskScaleRadius", "StellarMass", "Rvir"],
    "sfr_density_evolution": ["StarFormationRate", "StellarMass"],
    "stellar_mass_density_evolution": ["StellarMass"],
    # SAGE26 allresults-history.py figures no forked figure covers
    "rcool_to_rvir_above_mshock": ["Mvir", "RcoolToRvir"],
    "sfr_evolution": ["StellarMass", "SfrDisk", "SfrBulge"],
    "stellar_mass_halo_mass_relation": ["StellarMass", "Mvir"],
    "black_hole_mass_function": ["BlackHoleMass"],
    "quenched_fraction_evolution": ["StellarMass", "SfrDisk", "SfrBulge", "Type"],
    "concentration_evolution": ["Concentration", "Mvir"],
    "metallicity_evolution": ["StellarMass", "MetalsStellarMass"],
    "h2_fraction_evolution": ["StellarMass", "ColdGas", "H2gas"],
    "star_formation_efficiency": ["StellarMass", "Mvir", "Type"],
    "central_satellite_counts": ["Type"],
    "central_satellite_fraction": ["Type"],
    "central_satellite_fraction_massive_quiescent": [
        "StellarMass",
        "SfrDisk",
        "SfrBulge",
        "Type",
    ],
    "quenched_central_satellite_counts": ["StellarMass", "SfrDisk", "SfrBulge", "Type"],
    "ffb_fraction_cut_down": ["FFBRegime", "Len"],
}

# Galaxy fields each evolution figure actually reads. Evolution plots hold every requested
# snapshot in memory simultaneously, so the plotting engine reads only the union of these
# fields from HDF5 output instead of the whole record. Keep an entry for every plot in
# EVOLUTION_PLOTS and keep it a superset of that plot's PLOT_REQUIREMENTS: a name missing
# here makes the engine fall back to reading every field, and a field missing from a listed
# entry would leave the figure without data it needs.
EVOLUTION_PLOT_FIELDS = {
    "hmf_evolution": ["Type", "Mvir"],
    "smf_evolution": ["StellarMass"],
    "sfr_density_evolution": ["StarFormationRate", "StellarMass"],
    "stellar_mass_density_evolution": ["StellarMass"],
    "rcool_to_rvir_above_mshock": ["Mvir", "RcoolToRvir"],
    "sfr_evolution": ["StellarMass", "SfrDisk", "SfrBulge"],
    "stellar_mass_halo_mass_relation": ["StellarMass", "Mvir"],
    "black_hole_mass_function": ["BlackHoleMass"],
    "quenched_fraction_evolution": ["StellarMass", "SfrDisk", "SfrBulge", "Type"],
    "concentration_evolution": ["Concentration", "Mvir"],
    "metallicity_evolution": ["StellarMass", "MetalsStellarMass"],
    "h2_fraction_evolution": ["StellarMass", "ColdGas", "H2gas"],
    "star_formation_efficiency": ["StellarMass", "Mvir", "Type"],
    "central_satellite_counts": ["Type"],
    "central_satellite_fraction": ["Type"],
    "central_satellite_fraction_massive_quiescent": [
        "StellarMass",
        "SfrDisk",
        "SfrBulge",
        "Type",
    ],
    "quenched_central_satellite_counts": ["StellarMass", "SfrDisk", "SfrBulge", "Type"],
    "ffb_fraction_cut_down": ["FFBRegime", "Len"],
}

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
    "baryonic_mass_function": baryonic_mass_function.plot,
    "gas_mass_function": gas_mass_function.plot,
    "baryonic_tully_fisher": baryonic_tully_fisher.plot,
    "specific_sfr": specific_sfr.plot,
    "black_hole_bulge_relation": black_hole_bulge_relation.plot,
    "gas_fraction": gas_fraction.plot,
    "metallicity": metallicity.plot,
    "bulge_mass_fraction": bulge_mass_fraction.plot,
    "quiescent_fraction": quiescent_fraction.plot,
    "mass_reservoir_scatter": mass_reservoir_scatter.plot,
    "bulge_to_total": bulge_to_total.plot,
    "spatial_distribution_3d_box": spatial_distribution_3d_box.plot,
    "star_formation_rate": star_formation_rate.plot,
    "mass_loading_vs_vvir": mass_loading_vs_vvir.plot,
    "stellar_vs_halo_mass_by_regime": stellar_vs_halo_mass_by_regime.plot,
    "specific_star_formation_rate": specific_star_formation_rate.plot,
    "concentration_vs_mvir": concentration_vs_mvir.plot,
    "sfr_vs_halo_accretion_rate": sfr_vs_halo_accretion_rate.plot,
    "tcool_vs_mvir": tcool_vs_mvir.plot,
    "cgm_fraction_vs_mvir": cgm_fraction_vs_mvir.plot,
    "ejection_efficiency_vs_vvir": ejection_efficiency_vs_vvir.plot,
    "disk_radius_vs_stellar_mass": disk_radius_vs_stellar_mass.plot,
    "sfr_density_evolution": sfr_density_evolution.plot,
    "stellar_mass_density_evolution": stellar_mass_density_evolution.plot,
    "rcool_to_rvir_above_mshock": rcool_to_rvir_above_mshock.plot,
    "sfr_evolution": sfr_evolution.plot,
    "stellar_mass_halo_mass_relation": stellar_mass_halo_mass_relation.plot,
    "black_hole_mass_function": black_hole_mass_function.plot,
    "quenched_fraction_evolution": quenched_fraction_evolution.plot,
    "concentration_evolution": concentration_evolution.plot,
    "metallicity_evolution": metallicity_evolution.plot,
    "h2_fraction_evolution": h2_fraction_evolution.plot,
    "star_formation_efficiency": star_formation_efficiency.plot,
    "central_satellite_counts": central_satellite_counts.plot,
    "central_satellite_fraction": central_satellite_fraction.plot,
    "central_satellite_fraction_massive_quiescent": central_satellite_fraction_massive_quiescent.plot,
    "quenched_central_satellite_counts": quenched_central_satellite_counts.plot,
    "ffb_fraction_cut_down": ffb_fraction_cut_down.plot,
}
