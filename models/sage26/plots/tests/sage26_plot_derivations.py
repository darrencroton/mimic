"""Hand-built derivation rows for the figures of sage26 (models/sage26/plots/figures/).

Each row (``framework.plot_derivations.Derivation``) calls one registered figure on a tiny
catalogue whose plotted answer is stated as literals: mass-function normalisation by volume,
``h^3`` and bin width, the red/blue split, the star-formation-rate and stellar-mass densities, the
Sb/c selection and the metallicity conversion, then the figures only sage26 has (the CGM curves,
the H2 and H I lines, the regime selections and the unit conversions of the mass-loading, regime,
accretion-rate, cooling-time and CGM-fraction figures). sage16 keeps its own copy of the rows for
the figures it shares, as it keeps its own copy of the figures. A row labelled None names the
artist tagged gid=model that carries no legend label.

Units of every row: ``h = 0.5``, a volume of 1000 (Mpc/h)^3, masses in the output's 1e10 Msun/h, so
a stored mass ``m`` is ``m * 1e10 / 0.5 = 2e10 m`` Msun and a mass function with one galaxy per
0.1 dex bin is ``1 / 1000 * 0.5^3 / 0.1 = 1.25e-3`` Mpc^-3 dex^-1. Expected logarithms are written
out to six decimals: ``log10(2.4e10) = 10.380211``.
"""

from framework.plot_derivations import Derivation

# Fields that hold integers; every other field of a hand-built record is a float32, as in the output
INTEGER_FIELDS = frozenset(
    {"Type", "Regime", "FFBRegime", "Len", "SnapNum", "UniqueGalaxyID", "UniqueCentralGalaxyID"}
)

# The mass function of one galaxy per 0.1 dex bin at h = 0.5 and volume 1000: 1 / 1000 * h^3 / 0.1
PHI_ONE = 1.25e-3

# The red/blue cut sSFR = 1e-11 per year and the stored rates below: a galaxy of 1.2 (2.4e10 Msun)
# forming 0.12 Msun/yr has sSFR 5e-12 (red), one forming 0.48 has 2e-11 (blue)
RED_SFR, BLUE_SFR = 0.12, 0.48

COMMON_DERIVATIONS = [
    Derivation(
        "halo_mass_function",
        "halo_mass_function",
        # Two centrals and a satellite of Mvir 1.2 (log10 Msun 10.380211, the bin 10.3 to 10.4,
        # centre 10.35) and a central with no mass: only the two centrals are counted
        {"Type": [0, 0, 1, 0], "Mvir": [1.2, 1.2, 1.2, 0.0]},
        {"All Halos": [(10.35, 2 * PHI_ONE)]},
        drop_zero_y=True,
    ),
    Derivation(
        "stellar_mass_function",
        "stellar_mass_function",
        # Three galaxies of 1.2 (the bin 10.3 to 10.4) of which two are red and one blue, and a
        # galaxy with no stellar mass (not counted anywhere, whatever its rate)
        {
            "Type": [0, 0, 0, 0],
            "StellarMass": [1.2, 1.2, 1.2, 0.0],
            "StarFormationRate": [RED_SFR, RED_SFR, BLUE_SFR, 5.0],
        },
        {
            "Model - All": [(10.35, 3 * PHI_ONE)],
            "Model - Red": [(10.35, 2 * PHI_ONE)],
            "Model - Blue": [(10.35, PHI_ONE)],
        },
        drop_zero_y=True,
    ),
    Derivation(
        "baryonic_mass_function",
        "baryonic_mass_function",
        # Stellar 0.7 + cold 0.5 = 1.2 for two galaxies (the stars alone, 1.4e10 Msun, would be
        # the bin 10.1 to 10.2); a galaxy with neither is not counted
        {"StellarMass": [0.7, 0.7, 0.0], "ColdGas": [0.5, 0.5, 0.0]},
        {"Model": [(10.35, 2 * PHI_ONE)]},
        drop_zero_y=True,
    ),
    Derivation(
        "quiescent_fraction",
        "quiescent_fraction",
        # In the bin 10.3 to 10.4: a red and a blue central, two red satellites
        {
            "Type": [0, 0, 1, 1],
            "StellarMass": [1.2, 1.2, 1.2, 1.2],
            "StarFormationRate": [RED_SFR, BLUE_SFR, RED_SFR, RED_SFR],
        },
        {
            "All": [(10.35, 0.75)],
            "Centrals": [(10.35, 0.5)],
            "Satellites": [(10.35, 1.0)],
        },
    ),
    Derivation(
        "sfr_density_evolution",
        "sfr_density_evolution",
        # Rates 3 + 5 at z = 0 and 15 + 25 at z = 1 over 1000 (Mpc/h)^3 times h^3; the galaxy with
        # no stellar mass is outside the stellar-mass window, so its rate of 100 is not summed
        {
            63: (0.0, {"StellarMass": [1.0, 2.0, 0.0], "StarFormationRate": [3.0, 5.0, 100.0]}),
            37: (1.0, {"StellarMass": [1.0, 2.0, 0.0], "StarFormationRate": [15.0, 25.0, 100.0]}),
        },
        # 8 / 1000 * 0.125 = 1e-3 and 40 / 1000 * 0.125 = 5e-3
        {"Model": [(0.0, -3.0), (1.0, -2.301030)]},
    ),
    Derivation(
        "stellar_mass_density_evolution",
        "stellar_mass_density_evolution",
        # Stellar mass sums 4 (z = 0) and 2 (z = 1) in 1e10 Msun/h: 4e10 / 0.5 = 8e10 Msun over
        # 1000 / 0.125 = 8000 Mpc^3 is 1e7 Msun/Mpc^3, and 2e10 / 0.5 over 8000 is 5e6
        {
            63: (0.0, {"StellarMass": [1.0, 3.0]}),
            37: (1.0, {"StellarMass": [0.5, 1.5]}),
        },
        {"Model": [(0.0, 7.0), (1.0, 6.698970)]},
    ),
    Derivation(
        "baryonic_tully_fisher",
        "baryonic_tully_fisher",
        # Rows: (a) a central of B/T 0.3 and Vmax 100; (b) B/T 0.05, (c) B/T 0.6 and (d) a satellite
        # of B/T 0.3, none of which is a Sb/c central; (e) a central of B/T 0.2 and Vmax 200
        {
            "Type": [0, 0, 0, 1, 0],
            "StellarMass": [1.0, 1.0, 1.0, 1.0, 1.0],
            "ColdGas": [1.0, 1.0, 1.0, 1.0, 3.0],
            "BulgeMass": [0.3, 0.05, 0.6, 0.3, 0.2],
            "Vmax": [100.0, 100.0, 100.0, 100.0, 200.0],
        },
        # x = log10 Vmax; y = log10((stellar + cold) * 1e10 / 0.5): 2 -> 4e10, 4 -> 8e10
        {"Model Sb/c galaxies": [(2.0, 10.602060), (2.301030, 10.903090)]},
    ),
    Derivation(
        "gas_fraction",
        "gas_fraction",
        # The same galaxies: the selection is the Sb/c one, x = log10(stellar * 1e10 / 0.5),
        # y = cold / (stellar + cold)
        {
            "Type": [0, 0, 0, 1, 0],
            "StellarMass": [1.0, 1.0, 1.0, 1.0, 1.0],
            "ColdGas": [1.0, 1.0, 1.0, 1.0, 3.0],
            "BulgeMass": [0.3, 0.05, 0.6, 0.3, 0.2],
        },
        {"Model Sb/c galaxies": [(10.301030, 0.5), (10.301030, 0.75)]},
    ),
    Derivation(
        "metallicity",
        "metallicity",
        # Two centrals of stellar 1.0 and cold 1.0 with metals 0.04 (Z / 0.02 = 2) and 0.01 (0.5);
        # a central with a gas fraction of 0.05 (below the 0.1 floor) and a satellite are out
        {
            "Type": [0, 0, 0, 1],
            "StellarMass": [1.0, 1.0, 19.0, 1.0],
            "ColdGas": [1.0, 1.0, 1.0, 1.0],
            "MetalsColdGas": [0.04, 0.01, 0.04, 0.04],
        },
        # y = 12 + log10(O/H) = log10((metals / cold) / 0.02) + 9
        {"Model galaxies": [(10.301030, 8.698970), (10.301030, 9.301030)]},
    ),
]

SAGE26_DERIVATIONS = [
    Derivation(
        "baryon_fraction_cgm_curve",
        "baryon_fraction",
        # Three centrals of Mvir 10 (log10 Msun 11.30103, the bin 11.3 to 11.4), each its own
        # group: CGM gas 1, 2, 3 and stars 2 give the fractions 0.1, 0.2, 0.3 and 0.2
        {
            "Type": [0, 0, 0],
            "UniqueCentralGalaxyID": [100, 101, 102],
            "Mvir": [10.0, 10.0, 10.0],
            "StellarMass": [2.0, 2.0, 2.0],
            "CGMgas": [1.0, 2.0, 3.0],
        },
        # The bin's mean: CGM 0.2, and stars + CGM 0.4 in the total
        {"CGM": [(11.301030, 0.2)], "TOTAL": [(11.301030, 0.4)]},
    ),
    Derivation(
        "mass_reservoir_scatter_cgm_series",
        "mass_reservoir_scatter",
        # CGM gas 1 of Mvir 10, CGM gas 4 of Mvir 100 and no CGM gas of Mvir 10; every galaxy
        # has stellar mass 1, so each is drawn. The CGM series is log10(max(CGM, 1 Msun))
        {
            "Type": [0, 0, 0],
            "Mvir": [10.0, 100.0, 10.0],
            "StellarMass": [1.0, 1.0, 1.0],
            "ColdGas": [0.0, 0.0, 0.0],
            "HotGas": [0.0, 0.0, 0.0],
            "CGMgas": [1.0, 4.0, 0.0],
            "EjectedGas": [0.0, 0.0, 0.0],
            "ICS": [0.0, 0.0, 0.0],
        },
        {
            "CGM gas": [(11.301030, 10.301030), (12.301030, 10.903090), (11.301030, 0.0)],
        },
    ),
    Derivation(
        "gas_mass_function_h2_and_hi_lines",
        "gas_mass_function",
        # Three galaxies with cold gas 2; H2 gas 1.2, 1.2 and none; H I gas 0.6 each. H2 of
        # 2.4e10 Msun is the bin 10.3 to 10.4 (two galaxies); H I of 1.2e10 the bin 10.0 to 10.1
        {
            "ColdGas": [2.0, 2.0, 2.0],
            "H2gas": [1.2, 1.2, 0.0],
            "H1gas": [0.6, 0.6, 0.6],
        },
        {
            "Model - H2 Gas": [(10.35, 2 * PHI_ONE)],
            "Model - HI Gas": [(10.05, 3 * PHI_ONE)],
        },
        drop_zero_y=True,
    ),
    Derivation(
        "mass_loading_vs_vvir",
        "mass_loading_vs_vvir",
        # Galaxies 1 (no stellar mass) and 2 (no mass loading) are not drawn; x is log10 Vvir
        {
            "StellarMass": [1.0, 0.0, 1.0, 1.0],
            "MassLoading": [2.0, 3.0, 0.0, 5.0],
            "Vvir": [100.0, 100.0, 100.0, 1000.0],
        },
        {None: [(2.0, 2.0), (3.0, 5.0)]},
    ),
    Derivation(
        "stellar_vs_halo_mass_by_regime",
        "stellar_vs_halo_mass_by_regime",
        # Regime 0 (CGM) and 1 (hot) are drawn in their own series; Regime -1 and a galaxy with no
        # stellar mass are not drawn. Both masses are x 1e10 / h: Mvir 2 -> 4e10, 4 -> 8e10
        {
            "Mvir": [2.0, 4.0, 2.0, 2.0],
            "StellarMass": [1.0, 1.0, 1.0, 0.0],
            "Regime": [0, 1, -1, 0],
        },
        {
            "CGM Regime": [(10.602060, 10.301030)],
            "Hot Regime": [(10.903090, 10.301030)],
        },
    ),
    Derivation(
        "sfr_vs_halo_accretion_rate",
        "sfr_vs_halo_accretion_rate",
        # Galaxy 0: rate 1 + 1, accretion 5 + 5, stellar mass 100 (2e12 Msun). Galaxy 1 has no
        # star formation and galaxy 2 no accretion: not drawn. Galaxy 3: rate 0.25 + 0.25,
        # accretion 0 + 50 and no stellar mass, whose colour is log10(max(M, 1)) = 0
        {
            "SfrDisk": [1.0, 0.0, 1.0, 0.25],
            "SfrBulge": [1.0, 0.0, 1.0, 0.25],
            "mdot_cool": [5.0, 5.0, 0.0, 0.0],
            "mdot_stream": [5.0, 5.0, 0.0, 50.0],
            "StellarMass": [100.0, 1.0, 1.0, 0.0],
        },
        # (log10 accretion, log10 rate, log10 stellar mass)
        {None: [(1.0, 0.301030, 12.301030), (1.698970, -0.301030, 0.0)]},
    ),
    Derivation(
        "tcool_vs_mvir",
        "tcool_vs_mvir",
        # Galaxies 0 (regime 0), 1 (regime 1) and 2 (regime -1, coloured by 35.9 Vvir^2 = 359000 K)
        # are drawn; 3 has no cooling, 4 no Rvir and 5 no Mvir
        {
            "CoolingRate": [1e9, 2e9, 3e9, 0.0, 1e9, 1e9],
            "Mvir": [2.0, 4.0, 2.0, 2.0, 2.0, 0.0],
            "Rvir": [0.2, 0.2, 0.2, 0.2, 0.0, 0.2],
            "Regime": [0, 1, -1, 0, 0, 0],
            "Vvir": [100.0] * 6,
        },
        {
            "CGM regime": [(10.602060, 1e9)],
            "Hot regime": [(10.903090, 2e9)],
            "Unclassified": [(10.602060, 3e9, 359000.0)],
        },
    ),
    Derivation(
        "cgm_fraction_vs_mvir",
        "cgm_fraction_vs_mvir",
        # CGM 0.1 of Mvir 2 (2e9 over 4e10 Msun = 0.05) and CGM 1 of Mvir 4 (0.25): 0.17 log10 of
        # those. A galaxy with no CGM gas and one with no halo mass have no finite value
        {"CGMgas": [0.1, 1.0, 0.0, 1.0], "Mvir": [2.0, 4.0, 2.0, 0.0]},
        {None: [(10.602060, -0.221175), (10.903090, -0.102350)]},
    ),
]

DERIVATIONS = COMMON_DERIVATIONS + SAGE26_DERIVATIONS
