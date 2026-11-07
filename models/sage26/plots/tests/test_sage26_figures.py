#!/usr/bin/env python3
"""
sage26 registered figures, rendered from a synthetic catalogue (models/sage26/plots/figures/).

Validates: each registered figure renders a non-empty PNG named by its registry file stem and
           returns no skip message; at least one artist tagged gid="model" holds a finite data
           point; the figure's data axes and colour-bar axes carry exactly the labels and titles
           of its source at the starting commit; every field the figure reads unconditionally
           is in its PLOT_REQUIREMENTS entry (and, for an evolution figure, every field it reads
           is in its EVOLUTION_PLOT_FIELDS entry) and a figure called without one of its
           optional fields does not raise; the baryon-fraction reservoir helper returns the
           seven reservoirs with the total equal to their sum; baryon_fraction draws a "CGM"
           curve, mass_reservoir_scatter a "CGM gas" series and gas_mass_function the
           "Model - H2 Gas" and "Model - HI Gas" lines, each tagged gid="model"
           (Slice 2 adds the twelve SAGE26 z = 0 figures S19-S30: the same cases, a Cap case for
           each sampled figure, a unit case for each helper, and both the colour-bar and the
           no-colour-bar labels of tcool_vs_mvir; Slice 3 adds the fourteen evolution figures
           E5-E18 (thirteen ports of SAGE26's allresults-history.py and the cut-down port of
           paper_plots.py plot 23c): the same cases, a window case and a legend case where the
           figure has one, the snapshot-selection helper's cases (inclusive z-range ends, empty
           bins, nearest snapshot and its tie, order independence), a unit case for every helper,
           and cases pinning each bin constant, threshold, legend text and conversion)
Tolerance: none for labels, stems, field sets, caps and the reservoir helper (the helper's
           literals are dyadic, so the sums are exact); the CGM curve is compared with the total
           curve by a plain inequality; helper unit cases compare with a relative tolerance of
           1e-12 (decimal literals) or exactly (dyadic literals), and the smoothed-histogram
           peak with an absolute tolerance of 2e-3

Every expectation is written from the plan of the c16 plotting node (Slices 1 to 3), the
figure's source text at the starting commit (66a3e949, transcribed: no label is computed from the
code under test) or SAGE26's plotting scripts (plotting/allresults-local.py,
plotting/allresults-history.py and plotting/paper_plots.py, lines cited at each use).
Figures render with MPLBACKEND=Agg into a temporary directory under $TMPDIR, removed at the end.
It needs no Mimic run.

Run: mimic_venv/bin/python models/sage26/plots/tests/test_sage26_figures.py
"""

import importlib
import math
import os
import shutil
import sys
import tempfile
from pathlib import Path

# Set before Matplotlib is imported (the figure package imports it)
os.environ["MPLBACKEND"] = "Agg"

import matplotlib.pyplot as plt
import numpy as np
from numpy.lib import recfunctions

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
MIMIC_PLOT_DIR = REPO_ROOT / "plot" / "mimic-plot"
sys.path.insert(0, str(MIMIC_PLOT_DIR))
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(HERE))

from framework import run_test_suite
from sage26_plot_fixtures import (
    HUBBLE_H,
    VOLUME,
    build_catalogue,
    build_evolution_input,
    record_dtype,
    run_parameters,
    snapshot_metadata,
)


def load_figure_package(model):
    """Import models/<model>/plots/figures as `figures`, replacing any earlier package."""
    for name in [m for m in sys.modules if m == "figures" or m.startswith("figures.")]:
        del sys.modules[name]
    plots_dir = str(REPO_ROOT / "models" / model / "plots")
    sys.path.insert(0, plots_dir)
    try:
        return importlib.import_module("figures")
    finally:
        sys.path.remove(plots_dir)


FIGURES = load_figure_package("sage26")

# Final registry table of the c16 plan, Slice 3: registry name -> File stem (rows S1-S30, E1-E18)
SNAPSHOT_STEMS = {
    "halo_mass_function": "HaloMassFunction",
    "halo_occupation": "HaloOccupation",
    "spin_distribution": "SpinDistribution",
    "velocity_distribution": "VelocityDistribution",
    "spatial_distribution": "SpatialDistribution",
    "stellar_mass_function": "StellarMassFunction",
    "cold_gas_function": "ColdGasFunction",
    "baryon_fraction": "BaryonFraction",
    "baryonic_mass_function": "BaryonicMassFunction",
    "gas_mass_function": "GasMassFunction",
    "baryonic_tully_fisher": "BaryonicTullyFisher",
    "specific_sfr": "SpecificSFR",
    "black_hole_bulge_relation": "BlackHoleBulgeRelation",
    "gas_fraction": "GasFraction",
    "metallicity": "Metallicity",
    "bulge_mass_fraction": "BulgeMassFraction",
    "quiescent_fraction": "QuiescentFraction",
    "mass_reservoir_scatter": "MassReservoirScatter",
    "bulge_to_total": "BulgeToTotal",
    "spatial_distribution_3d_box": "SpatialDistribution3D_Box",
    "star_formation_rate": "StarFormationRate",
    "mass_loading_vs_vvir": "mass_loading_vs_vvir",
    "stellar_vs_halo_mass_by_regime": "stellar_vs_halo_mass_by_regime",
    "specific_star_formation_rate": "specific_star_formation_rate",
    "concentration_vs_mvir": "concentration_vs_mvir",
    "sfr_vs_halo_accretion_rate": "sfr_vs_halo_accretion_rate",
    "tcool_vs_mvir": "tcool_vs_mvir",
    "cgm_fraction_vs_mvir": "cgm_fraction_vs_mvir",
    "ejection_efficiency_vs_vvir": "ejection_efficiency_vs_vvir",
    "disk_radius_vs_stellar_mass": "disk_radius_vs_stellar_mass",
}
EVOLUTION_STEMS = {
    "hmf_evolution": "HaloMassFunction_Evolution",
    "smf_evolution": "StellarMassFunction_Evolution",
    "sfr_density_evolution": "SFR_Density_Evolution",
    "stellar_mass_density_evolution": "Stellar_Mass_Density_Evolution",
    "rcool_to_rvir_above_mshock": "RcoolToRvir_above_Mshock",
    "sfr_evolution": "SFR_evolution",
    "stellar_mass_halo_mass_relation": "StellarMassHaloMassRelation",
    "black_hole_mass_function": "BlackHoleMassFunction",
    "quenched_fraction_evolution": "QuenchedFraction.StellarMassFunctionEvolution",
    "concentration_evolution": "ConcentrationEvolution",
    "metallicity_evolution": "MetallicityEvolution",
    "h2_fraction_evolution": "H2FractionEvolution",
    "star_formation_efficiency": "StarFormationEfficiency",
    "central_satellite_counts": "CentralSatelliteCounts",
    "central_satellite_fraction": "CentralSatelliteFraction",
    "central_satellite_fraction_massive_quiescent": "CentralSatelliteFraction_MassiveQuiescent",
    "quenched_central_satellite_counts": "QuenchedCentralSatelliteCounts",
    "ffb_fraction_cut_down": "FFB_Fraction_Li24_cut_down",
}
FILE_STEMS = {**SNAPSHOT_STEMS, **EVOLUTION_STEMS}

# Axis strings of the forked figures, transcribed from their source at the starting commit.
# The shared ones are the returns of the get_*_label() helpers of figures/__init__.py.
MASS_FUNCTION = r"$\phi$ [Mpc$^{-3}$ dex$^{-1}$]"
HALO_MASS = r"log$_{10}$ M$_{\rm halo}$ [M$_{\odot}$]"
STELLAR_MASS = r"log$_{10}$ M$_{*}$ [M$_{\odot}$]"
BARYONIC_MASS = r"log$_{10}$ M$_{\rm bar}$ [M$_{\odot}$]"
VMAX = r"log$_{10}$ V$_{\rm max}$ [km/s]"
REDSHIFT = r"redshift"


def panel(x_label, y_label, title="", z_label=""):
    """Return the expected labels of one data axes (no side titles; a z label for a 3-D axes)."""
    return {
        "x": x_label,
        "y": y_label,
        "z": z_label,
        "title": title,
        "title_left": "",
        "title_right": "",
    }


# name -> list of expected data axes in creation order. The forked figures (S1-S18, E1-E4) have no
# title and no colour bar; the labels of the new figures (S19-S30) are SAGE26's Python string
# literals at the lines cited.
DATA_AXES = {
    "halo_mass_function": [panel(HALO_MASS, MASS_FUNCTION)],
    "halo_occupation": [panel(HALO_MASS, r"Mean occupation number $\langle N \rangle$")],
    "spin_distribution": [panel(r"Spin Parameter", r"Number")],
    "velocity_distribution": [panel(r"Velocity / H$_0$ [Mpc]", r"Box Normalised Count")],
    # plt.subplots(2, 2): three drawn panels, then the fourth, hidden by set_visible(False)
    "spatial_distribution": [
        panel(r"X [h$^{-1}$ Mpc]", r"Y [h$^{-1}$ Mpc]"),
        panel(r"X [h$^{-1}$ Mpc]", r"Z [h$^{-1}$ Mpc]"),
        panel(r"Y [h$^{-1}$ Mpc]", r"Z [h$^{-1}$ Mpc]"),
        panel("", ""),
    ],
    "stellar_mass_function": [panel(STELLAR_MASS, MASS_FUNCTION)],
    "cold_gas_function": [panel(r"log$_{10}$ M$_{\rm cold~gas}$ [M$_{\odot}$]", MASS_FUNCTION)],
    "baryon_fraction": [
        panel(r"Central log$_{10}$ M$_{\rm vir}$ [M$_{\odot}$]", r"Baryon Fraction")
    ],
    "baryonic_mass_function": [panel(BARYONIC_MASS, MASS_FUNCTION)],
    "gas_mass_function": [panel(r"log$_{10}$ M$_{\rm X}$ [M$_{\odot}$]", MASS_FUNCTION)],
    "baryonic_tully_fisher": [panel(VMAX, BARYONIC_MASS)],
    "specific_sfr": [panel(STELLAR_MASS, r"log$_{10}$ sSFR [yr$^{-1}$]")],
    "black_hole_bulge_relation": [
        panel(
            r"log$_{10}$ M$_{\rm bulge}$ [M$_{\odot}$]",
            r"log$_{10}$ M$_{\rm BH}$ [M$_{\odot}$]",
        )
    ],
    "gas_fraction": [panel(STELLAR_MASS, r"Cold Mass / (Cold+Stellar Mass)")],
    "metallicity": [panel(STELLAR_MASS, r"12 + log$_{10}$[O/H]")],
    "bulge_mass_fraction": [panel(STELLAR_MASS, r"Stellar Mass Fraction")],
    "quiescent_fraction": [panel(STELLAR_MASS, r"Quiescent Fraction")],
    "mass_reservoir_scatter": [
        panel(r"log M$_{\rm vir}$ [h$^{-1}$ M$_{\odot}$]", r"Stellar, cold, hot, ejected, ICS mass")
    ],
    # allresults-local.py:1233-1234; no title
    "bulge_to_total": [
        panel(
            r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$",
            r"$M_{\mathrm{bulge}} / M_{\mathrm{stars}}$",
        )
    ],
    # allresults-local.py:1593-1595; no title
    "spatial_distribution_3d_box": [panel("X (Mpc/h)", "Y (Mpc/h)", z_label="Z (Mpc/h)")],
    # allresults-local.py:1639-1640; no title
    "star_formation_rate": [
        panel(
            r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$",
            r"$\log_{10} \mathrm{SFR}\ (M_{\odot}\ \mathrm{yr^{-1}})$",
        )
    ],
    # allresults-local.py:1674-1675; no title
    "mass_loading_vs_vvir": [
        panel(
            r"$\log_{10} V_{\mathrm{vir}}\ (\mathrm{km/s})$",
            r"$\eta_{\mathrm{reheat}} = \dot{M}_{\mathrm{reheat}} / \dot{M}_{\star}$",
        )
    ],
    # allresults-local.py:1713-1714; no title
    "stellar_vs_halo_mass_by_regime": [
        panel(
            r"$\log_{10} M_{\mathrm{vir}}\ (M_{\odot})$",
            r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$",
        )
    ],
    # allresults-local.py:1829-1830; no title
    "specific_star_formation_rate": [
        panel(
            r"$\log_{10} M_{\mathrm{stars}}\ (M_{\odot})$",
            r"$\log_{10} \mathrm{sSFR}\ (\mathrm{yr^{-1}})$",
        )
    ],
    # allresults-local.py:1850, :1860-1874: two panels sharing y; the right panel has no y label
    "concentration_vs_mvir": [
        panel(
            r"$\log_{10} M_{\rm vir}\ [M_{\odot}]$",
            r"Concentration $c$",
            "Centrals \u2014 z = 0.00",
        ),
        panel(r"$\log_{10} M_{\rm vir}\ [M_{\odot}]$", "", "Satellites \u2014 z = 0.00"),
    ],
    # allresults-local.py:1920-1922
    "sfr_vs_halo_accretion_rate": [
        panel(
            r"$\log_{10}\ \dot{M}_{\rm acc}\ (M_{\odot}\ \mathrm{yr}^{-1})$",
            r"$\log_{10}\ \mathrm{SFR}\ (M_{\odot}\ \mathrm{yr}^{-1})$",
            "Centrals \u2014 z = 0.00",
        )
    ],
    # allresults-local.py:1999-2000; no title
    "tcool_vs_mvir": [
        panel(
            r"$\log_{10} M_{\rm vir}\ (M_{\odot})$",
            r"$\mathrm{Cooling\ rate}\ (M_{\odot}\ \mathrm{Gyr}^{-1})$",
        )
    ],
    # allresults-local.py:2030-2031; no title
    "cgm_fraction_vs_mvir": [
        panel(r"$\log_{10} M_{\rm vir}\ (M_{\odot})$", r"$\mathrm{CGM\ Fraction}$")
    ],
    # allresults-local.py:2114-2115; no title
    "ejection_efficiency_vs_vvir": [
        panel(r"$\log_{10} V_{\rm vir}\ (\mathrm{km/s})$", r"$\mathrm{Ejection\ Efficiency}$")
    ],
    # allresults-local.py:2180-2181; no title
    "disk_radius_vs_stellar_mass": [
        panel(r"$R_{\rm vir}\ (\mathrm{kpc}/h)$", r"$R_{\rm e,disk}\ (\mathrm{kpc}/h)$")
    ],
    "hmf_evolution": [panel(HALO_MASS, MASS_FUNCTION)],
    "smf_evolution": [panel(STELLAR_MASS, MASS_FUNCTION)],
    "sfr_density_evolution": [
        panel(REDSHIFT, r"log$_{10}$ SFR density [M$_{\odot}$ yr$^{-1}$ Mpc$^{-3}$]")
    ],
    "stellar_mass_density_evolution": [
        panel(REDSHIFT, r"log$_{10}$ $\rho_{*}$ [M$_{\odot}$ Mpc$^{-3}$]")
    ],
    # The history figures of Slice 3: SAGE26's Python string literals at the lines cited (none
    # draws a label SAGE26 does not; an empty string where SAGE26 sets none)
    "rcool_to_rvir_above_mshock": [  # allresults-history.py:448-451
        panel(
            r"$r_{\rm cool}\,/\,R_{\rm vir}$",
            "PDF",
            r"$M_{\rm vir} > M_{\rm shock}\ (6\times10^{11}\,M_\odot)$ haloes",
        )
    ],
    "sfr_evolution": [  # :831-832
        panel(r"$\log_{10} M_* [M_\odot]$", r"$\log_{{10}}\mathrm{{SFR}}\ [M_\odot/yr]$")
    ],
    "stellar_mass_halo_mass_relation": [  # :940-941
        panel(r"$\log_{10} M_{\rm vir}$ [$M_\odot$]", r"$\log_{10} M_*$ [$M_\odot$]")
    ],
    "black_hole_mass_function": [  # :1230-1231
        panel(r"$\log_{10} M_{\rm BH} [M_\odot]$", r"$\phi$ [Mpc$^{-3}$ dex$^{-1}$]")
    ],
    "quenched_fraction_evolution": [  # :1348-1349
        panel(r"$\log_{10} M_* [M_\odot]$", "Quenched Fraction")
    ],
    "concentration_evolution": [panel(r"Redshift $z$", r"Concentration $c$")],  # :1523-1524
    "metallicity_evolution": [panel(r"$\log_{10} M_* [M_\odot]$", r"$\log_{10} Z$")],  # :1596-1597
    "h2_fraction_evolution": [  # :1716-1717
        panel(
            r"$\log_{10} M_* [M_\odot]$",
            r"$f_{\rm H_2} = M_{\rm H_2}\,/\,(M_{\rm cold} \times 0.74)$",
        )
    ],
    "star_formation_efficiency": [  # :1792-1793
        panel(r"Cosmic time [Gyr]", r"$\varepsilon_* = M_* \,/\, (f_b \cdot M_{\rm vir})$")
    ],
    "central_satellite_counts": [panel("", "Average Count")],  # :1843 (no x label)
    "central_satellite_fraction": [panel(r"Redshift $z$", "Fraction")],  # :1892-1893
    "central_satellite_fraction_massive_quiescent": [  # :1968-1969
        panel(r"Redshift $z$", "Fraction (top 10% $M_*$, sSFR $< 0.2/t_{\\rm H}$)")
    ],
    "quenched_central_satellite_counts": [panel("", "Average Count")],  # :2024 (no x label)
    # paper_plots.py:10357-10371 and :10440; the title is the cut-down port's own (plan row E18)
    "ffb_fraction_cut_down": [
        panel(
            r"$z$",
            "FFB fraction",
            "Cut-down port of SAGE26 paper plot 23c: eight output snapshots, no MBK25 run",
        ),
        panel(r"$\log_{10}(1+z)$", ""),
    ],
}
# name -> expected colour-bar labels in creation order. Only three figures draw one: the
# concentration figure's (allresults-local.py:1876-1877) and the accretion-rate figure's (:1912-1913)
# stellar mass, and the cooling-rate figure's virial temperature (:1995-1996), which the synthetic
# catalogue's satellites (Regime -1, outside 0 and 1) bring.
COLORBAR_LABELS = {name: [] for name in FILE_STEMS}
COLORBAR_LABELS["concentration_vs_mvir"] = [r"$\log_{10} M_{\star}\ [M_{\odot}]$"]
COLORBAR_LABELS["sfr_vs_halo_accretion_rate"] = [r"$\log_{10} M_{\star}\ [M_{\odot}]$"]
COLORBAR_LABELS["tcool_vs_mvir"] = [r"$T_{\rm vir}$ (K)"]

TMP_ROOT = None  # created by main(), removed at its end


# --------------------------------------------------------------------------------------
# Reading a captured figure
# --------------------------------------------------------------------------------------
def is_colorbar_axes(ax):
    """True when Matplotlib's colour-bar reference is set on the axes."""
    return getattr(ax, "_colorbar", None) is not None


def axes_labels(ax):
    """Return the label dictionary of one axes (same keys as panel())."""
    return {
        "x": ax.get_xlabel(),
        "y": ax.get_ylabel(),
        "z": ax.get_zlabel() if hasattr(ax, "get_zlabel") else "",
        "title": ax.get_title("center"),
        "title_left": ax.get_title("left"),
        "title_right": ax.get_title("right"),
    }


def describe_figure(fig):
    """Return (data-axes label dicts, colour-bar labels), each in creation order."""
    data = [axes_labels(ax) for ax in fig.axes if not is_colorbar_axes(ax)]
    bars = [ax.get_ylabel() or ax.get_xlabel() for ax in fig.axes if is_colorbar_axes(ax)]
    return data, bars


def finite_points(artist):
    """Count the finite data points an artist holds (0 for an artist this does not know)."""
    from matplotlib.collections import PathCollection, PolyCollection, QuadMesh
    from matplotlib.contour import ContourSet
    from matplotlib.lines import Line2D
    from matplotlib.patches import Polygon, Rectangle, StepPatch

    if isinstance(artist, StepPatch):
        # A stairs step: its finite values (a NaN step is not drawn)
        values = np.asarray(artist.get_data().values, dtype=float)
        return int(np.count_nonzero(np.isfinite(values)))
    if isinstance(artist, Polygon):
        # A step histogram's outline: its finite vertices
        xy = np.asarray(artist.get_xy(), dtype=float)
        return int(np.count_nonzero(np.isfinite(xy).all(axis=1))) if len(xy) else 0
    if isinstance(artist, Line2D):
        xy = np.asarray(artist.get_xydata(), dtype=float)
        return int(np.count_nonzero(np.isfinite(xy).all(axis=1))) if len(xy) else 0
    if isinstance(artist, QuadMesh):
        values = np.ma.filled(np.ma.asarray(artist.get_array()), 0.0)
        return int(np.count_nonzero(np.isfinite(values) & (values != 0)))
    if isinstance(artist, PolyCollection):
        count = 0
        for path in artist.get_paths():
            if len(path.vertices):
                count += int(np.count_nonzero(np.isfinite(path.vertices).all(axis=1)))
        return count
    if isinstance(artist, ContourSet):
        # A filled or line contour set holds one path per level; count its finite vertices
        count = 0
        for path in artist.get_paths():
            if len(path.vertices):
                count += int(np.count_nonzero(np.isfinite(path.vertices).all(axis=1)))
        return count
    if isinstance(artist, PathCollection) and hasattr(artist, "_offsets3d"):
        # A 3-D scatter keeps its points in x, y, z arrays
        xyz = np.array(
            [np.ma.filled(np.ma.asarray(a, dtype=float), np.nan) for a in artist._offsets3d]
        )
        return int(np.count_nonzero(np.isfinite(xyz).all(axis=0))) if xyz.size else 0
    if isinstance(artist, PathCollection):
        offsets = np.ma.filled(np.ma.asarray(artist.get_offsets(), dtype=float), np.nan)
        return int(np.count_nonzero(np.isfinite(offsets).all(axis=1))) if len(offsets) else 0
    if isinstance(artist, Rectangle):
        return int(artist.get_height() != 0 and np.isfinite(artist.get_height()))
    return 0


def model_artists(fig):
    """Return [(artist, finite_points)] for every artist tagged gid="model" in a figure."""
    found = []
    for ax in fig.axes:
        for artist in ax.get_children():
            if artist.get_gid() == "model":
                found.append((artist, finite_points(artist)))
    return found


# --------------------------------------------------------------------------------------
# Running a figure
# --------------------------------------------------------------------------------------
class Run:
    """The outcome of one figure call: its return value and the figure it drew."""

    def __init__(self, path, skip, fig):
        self.path = path
        self.skip = skip
        self.fig = fig


def figure_module(name):
    """Return the module that defines a registered figure's plot() function."""
    return sys.modules[FIGURES.PLOT_FUNCS[name].__module__]


def evolution_fields(name):
    """The fields the engine reads for an evolution figure (its EVOLUTION_PLOT_FIELDS entry)."""
    return list(FIGURES.EVOLUTION_PLOT_FIELDS[name])


def call_figure(name, galaxies, label, capture=True):
    """Call a registered figure on the given input; return a Run.

    ``galaxies`` is the snapshot record for a snapshot figure, or the ``snapshots`` mapping for
    an evolution figure. save_and_close_figure is wrapped inside the module the figure calls it
    from, so the figure is recorded before it is closed; the engine is not changed.
    """
    module = figure_module(name)
    captured = {}
    original = module.save_and_close_figure

    def capturing_save(fig, *args, **kwargs):
        captured["fig"] = fig
        return original(fig, *args, **kwargs)

    out_dir = os.path.join(TMP_ROOT, label)
    kwargs = {
        "params": run_parameters(),
        "output_dir": out_dir,
        "output_format": ".png",
        "verbose": False,
    }
    if name in EVOLUTION_STEMS:
        kwargs["snapshots"] = galaxies
    else:
        kwargs.update(galaxies=galaxies, volume=VOLUME, metadata=snapshot_metadata())
    module.save_and_close_figure = capturing_save if capture else original
    try:
        path, skip = FIGURES.PLOT_FUNCS[name](**kwargs)
    finally:
        module.save_and_close_figure = original
        plt.close("all")
    return Run(path, skip, captured.get("fig"))


_CACHE = {}


def render(name):
    """Render a registered figure once from the synthetic input and cache the Run."""
    if name not in _CACHE:
        if name in EVOLUTION_STEMS:
            galaxies = build_evolution_input(evolution_fields(name))
        else:
            galaxies = build_catalogue()
        _CACHE[name] = call_figure(name, galaxies, name)
    return _CACHE[name]


# --------------------------------------------------------------------------------------
# Recording every field a figure reads
# --------------------------------------------------------------------------------------
class RecordingRow:
    """One galaxy of a recording catalogue; logs each field read by attribute or item access."""

    def __init__(self, row, reads):
        self._row = row
        self._reads = reads

    def __getattr__(self, name):
        if name in self._row.dtype.names:
            self._reads.add(name)
        return getattr(self._row, name)

    def __getitem__(self, key):
        if isinstance(key, str) and key in self._row.dtype.names:
            self._reads.add(key)
        return self._row[key]


class RecordingCatalogue:
    """A galaxy record that logs every field read by attribute or item access."""

    def __init__(self, record, reads):
        self._record = record
        self._reads = reads

    def __len__(self):
        return len(self._record)

    def __iter__(self):
        for row in self._record:
            yield RecordingRow(row, self._reads)

    def __getattr__(self, name):
        if name in self._record.dtype.names:
            self._reads.add(name)
        return getattr(self._record, name)

    def __getitem__(self, key):
        if isinstance(key, str):
            if key in self._record.dtype.names:
                self._reads.add(key)
            return self._record[key]
        sub = self._record[key]
        if isinstance(sub, np.ndarray) and sub.dtype.names:
            return RecordingCatalogue(sub, self._reads)
        if isinstance(sub, np.void):
            return RecordingRow(sub, self._reads)
        return sub


def drop_field(record, field):
    """Return the record without one field."""
    return recfunctions.drop_fields(record, [field], usemask=False, asrecarray=True)


def full_input(name):
    """Return the input carrying every output property, for a snapshot or an evolution figure."""
    if name in EVOLUTION_STEMS:
        return build_evolution_input(record_dtype().names)
    return build_catalogue()


def wrap_input(name, data, reads):
    """Wrap a figure's input so every field read is logged into ``reads``."""
    if name in EVOLUTION_STEMS:
        return {
            snap: (RecordingCatalogue(galaxies, reads), volume, metadata)
            for snap, (galaxies, volume, metadata) in data.items()
        }
    return RecordingCatalogue(data, reads)


def record_reads(name):
    """Run a figure once on a recording catalogue.

    Returns (reads, calls): every field read, and the (required_fields, optional_fields) of each
    check_required_fields call as the figure module made it. The wrapped check unwraps the
    catalogue, so the check's own look at the field names is not a read.
    """
    module = figure_module(name)
    original = module.check_required_fields
    reads, calls = set(), []

    def recording_check(galaxies, required_fields, optional_fields=None, plot_name="Plot"):
        calls.append((list(required_fields), list(optional_fields or [])))
        record = getattr(galaxies, "_record", galaxies)
        return original(record, required_fields, optional_fields, plot_name)

    module.check_required_fields = recording_check
    try:
        call_figure(name, wrap_input(name, full_input(name), reads), name + "-reads", False)
    finally:
        module.check_required_fields = original
    return reads, calls


# --------------------------------------------------------------------------------------
# Cases, one set per registered figure
# --------------------------------------------------------------------------------------
def make_renders(name):
    def case():
        run = render(name)
        assert run.skip is None, f"{name} returned a skip message: {run.skip}"
        assert run.path, f"{name} returned no path"
        path = Path(run.path)
        assert path.is_file(), f"{name}: no file at {path}"
        assert path.stat().st_size > 0, f"{name}: empty file {path}"
        with open(path, "rb") as handle:
            assert handle.read(8) == b"\x89PNG\r\n\x1a\n", f"{name}: {path} is not a PNG"

    case.__name__ = f"test_renders__{name}"
    return case


def make_file_stem(name):
    def case():
        run = render(name)
        assert run.path, f"{name} returned no path"
        assert Path(run.path).suffix == ".png", f"{name}: suffix of {run.path}"
        assert (
            Path(run.path).stem == FILE_STEMS[name]
        ), f"{name}: stem {Path(run.path).stem!r}, expected {FILE_STEMS[name]!r}"

    case.__name__ = f"test_file_stem__{name}"
    return case


def make_model_content(name):
    def case():
        run = render(name)
        assert run.fig is not None, f"{name}: no figure was captured"
        found = model_artists(run.fig)
        assert found, f"{name}: no artist is tagged gid='model'"
        assert any(
            points > 0 for _, points in found
        ), f"{name}: {len(found)} model artist(s), none holds a finite data point"

    case.__name__ = f"test_model_content__{name}"
    return case


def make_labels(name):
    def case():
        run = render(name)
        assert run.fig is not None, f"{name}: no figure was captured"
        data, bars = describe_figure(run.fig)
        assert (
            data == DATA_AXES[name]
        ), f"{name}: data axes labels {data}, expected {DATA_AXES[name]}"
        assert bars == COLORBAR_LABELS[name], f"{name}: colour-bar labels {bars}"

    case.__name__ = f"test_labels__{name}"
    return case


def make_requirements(name):
    def case():
        reads, calls = record_reads(name)
        assert reads, f"{name}: the recording run read no field"
        required, optional = set(), set()
        for required_fields, optional_fields in calls:
            required |= set(required_fields)
            optional |= set(optional_fields)
        mandatory = reads - optional
        registered = set(FIGURES.PLOT_REQUIREMENTS[name])
        # A mandatory field belongs in the entry whether its absence makes the figure raise or
        # return a skip: the engine skips a figure whose entry names an absent field.
        assert mandatory <= registered, (
            f"{name}: reads {sorted(mandatory - registered)} unconditionally, "
            f"not in PLOT_REQUIREMENTS {sorted(registered)}"
        )
        assert required <= registered, (
            f"{name}: passes required_fields {sorted(required - registered)} "
            f"not in PLOT_REQUIREMENTS {sorted(registered)}"
        )
        if name in EVOLUTION_STEMS:
            listed = set(FIGURES.EVOLUTION_PLOT_FIELDS[name])
            assert reads <= listed, (
                f"{name}: reads {sorted(reads - listed)} not in EVOLUTION_PLOT_FIELDS "
                f"{sorted(listed)}"
            )
        for field in sorted(optional):
            data = full_input(name)
            if name in EVOLUTION_STEMS:
                data = {snap: (drop_field(g, field), v, m) for snap, (g, v, m) in data.items()}
            else:
                data = drop_field(data, field)
            try:
                call_figure(name, data, f"{name}-without-{field}", False)
            except Exception as exc:
                raise AssertionError(
                    f"{name}: raised {type(exc).__name__} without optional field {field}: {exc}"
                ) from exc

    case.__name__ = f"test_requirements__{name}"
    return case


# --------------------------------------------------------------------------------------
# Reservoir helper and added series
# --------------------------------------------------------------------------------------
def test_reservoir_helper_returns_seven_reservoirs_and_their_exact_sum():
    """baryon_reservoirs() on literal arrays: seven reservoirs, CGMgas among them, exact total.

    Two groups: galaxies 0-1 (central 0, Mvir 8) and galaxies 2-4 (central 2, Mvir 16). Every
    value is a small dyadic number, so each group sum and fraction is exact in float64.
    Group sums (group 0, group 1): StellarMass (2, 8), ColdGas (4, 4), HotGas (8, 0),
    CGMgas (1, 8), EjectedGas (2, 0), ICS (0, 1), BlackHoleMass (0, 1); dividing by Mvir (8, 16)
    gives the literals below.
    """
    helper = figure_module("baryon_fraction").baryon_reservoirs
    galaxies = {
        "StellarMass": np.array([1, 1, 2, 2, 4], dtype=np.float32),
        "ColdGas": np.array([2, 2, 1, 1, 2], dtype=np.float32),
        "HotGas": np.array([4, 4, 0, 0, 0], dtype=np.float32),
        "CGMgas": np.array([1, 0, 4, 4, 0], dtype=np.float32),
        "EjectedGas": np.array([0, 2, 0, 0, 0], dtype=np.float32),
        "ICS": np.array([0, 0, 1, 0, 0], dtype=np.float32),
        "BlackHoleMass": np.array([0, 0, 0, 1, 0], dtype=np.float32),
    }
    optional = {field: True for field in galaxies}
    expected = {
        "stars": [0.25, 0.5],
        "cold": [0.5, 0.25],
        "hot": [1.0, 0.0],
        "cgm": [0.125, 0.5],
        "ejected": [0.25, 0.0],
        "ics": [0.0, 0.0625],
        "bh": [0.0, 0.0625],
        "baryons": [2.125, 1.375],
    }
    got = helper(
        galaxies,
        optional,
        group_of_galaxy=np.array([0, 0, 1, 1, 1]),
        n_groups=2,
        central_group=np.array([0, 1]),
        central_mvir=np.array([8.0, 16.0]),
    )
    assert set(got) == set(expected), f"reservoirs {sorted(got)}, expected {sorted(expected)}"
    for key, values in expected.items():
        assert got[key].tolist() == values, f"{key}: {got[key].tolist()}, expected {values}"
    total = sum(got[key] for key in expected if key != "baryons")
    assert np.array_equal(got["baryons"], total), "the total is not the sum of the reservoirs"

    # A reservoir the run does not write contributes zeros and is left out of the total
    without_cgm = helper(
        galaxies,
        {**optional, "CGMgas": False},
        group_of_galaxy=np.array([0, 0, 1, 1, 1]),
        n_groups=2,
        central_group=np.array([0, 1]),
        central_mvir=np.array([8.0, 16.0]),
    )
    assert without_cgm["cgm"].tolist() == [0.0, 0.0]
    assert without_cgm["baryons"].tolist() == [2.0, 0.875]


def series(name, label):
    """Return the model artists of a rendered figure carrying a legend label."""
    run = render(name)
    assert run.fig is not None, f"{name}: no figure was captured"
    return [artist for artist, _ in model_artists(run.fig) if artist.get_label() == label]


def test_added_series__baryon_fraction_draws_a_cgm_curve():
    """The CGM mean curve (SAGE26 plotting/allresults-local.py:1447, label 'Circumgalactic Medium')."""
    cgm = series("baryon_fraction", "CGM")
    total = series("baryon_fraction", "TOTAL")
    assert len(cgm) == 1, f"{len(cgm)} model artists labelled 'CGM'"
    assert len(total) == 1, f"{len(total)} model artists labelled 'TOTAL'"
    y_cgm, y_total = cgm[0].get_ydata(), total[0].get_ydata()
    assert len(y_cgm) == len(y_total) > 0 and np.all(y_cgm > 0), "CGM curve is empty or not > 0"
    # The total sums every reservoir, so it lies above any one of them
    assert np.all(y_total > y_cgm), "the total curve is not above the CGM curve"


def test_added_series__mass_reservoir_scatter_draws_a_cgm_gas_series():
    """The CGM gas series (SAGE26 plotting/allresults-local.py:1489, label 'CGM gas')."""
    cgm = series("mass_reservoir_scatter", "CGM gas")
    assert len(cgm) == 1, f"{len(cgm)} model artists labelled 'CGM gas'"
    assert finite_points(cgm[0]) > 0, "the CGM gas series holds no finite point"


def test_added_series__gas_mass_function_draws_the_h2_line():
    """The model H2 mass function (SAGE26 plotting/allresults-local.py:700, 'Model - H2 Gas')."""
    h2 = series("gas_mass_function", "Model - H2 Gas")
    assert len(h2) == 1, f"{len(h2)} model artists labelled 'Model - H2 Gas'"
    assert finite_points(h2[0]) > 0, "the H2 line holds no finite point"


def test_added_series__gas_mass_function_draws_the_hi_line():
    """The model H I mass function (SAGE26 plotting/allresults-local.py:701, 'Model - HI Gas')."""
    h1 = series("gas_mass_function", "Model - HI Gas")
    assert len(h1) == 1, f"{len(h1)} model artists labelled 'Model - HI Gas'"
    assert finite_points(h1[0]) > 0, "the H I line holds no finite point"


def test_colourbar_axes_are_identified_and_not_counted_as_data_axes():
    """describe_figure() lists a colour bar apart from the data axes, with its own label."""
    fig, ax = plt.subplots()
    ax.set_xlabel("data x")
    ax.set_ylabel("data y")
    image = ax.imshow(np.arange(4.0).reshape(2, 2))
    fig.colorbar(image, label="bar label")
    try:
        data, bars = describe_figure(fig)
    finally:
        plt.close(fig)
    assert [axes["x"] for axes in data] == ["data x"], f"data axes: {data}"
    assert bars == ["bar label"], f"colour-bar labels: {bars}"


# --------------------------------------------------------------------------------------
# tcool_vs_mvir colour bar: present only with a regime outside {0, 1}
# --------------------------------------------------------------------------------------
TCOOL_PANEL = panel(
    r"$\log_{10} M_{\rm vir}\ (M_{\odot})$",
    r"$\mathrm{Cooling\ rate}\ (M_{\odot}\ \mathrm{Gyr}^{-1})$",
)


def render_tcool_with_regimes(regimes_outside):
    """Render tcool_vs_mvir and return (data-axes labels, colour-bar labels).

    ``regimes_outside`` True keeps the synthetic catalogue's satellites at Regime -1 (outside 0
    and 1); False sets every such Regime to 0, so every row is of regime 0 or 1.
    """
    catalogue = build_catalogue()
    if regimes_outside:
        selected = (catalogue.CoolingRate > 0.0) & (catalogue.Mvir > 0.0) & (catalogue.Rvir > 0.0)
        assert np.any(selected & ~np.isin(catalogue.Regime, (0, 1))), "no unclassified row"
    else:
        catalogue.Regime = np.where(np.isin(catalogue.Regime, (0, 1)), catalogue.Regime, 0)
        assert np.all(np.isin(catalogue.Regime, (0, 1))), "a Regime outside 0 and 1 remains"
    run = call_figure("tcool_vs_mvir", catalogue, f"tcool-{regimes_outside}")
    assert run.skip is None and run.fig is not None, f"tcool_vs_mvir skipped: {run.skip}"
    return describe_figure(run.fig)


def test_labels__tcool_vs_mvir_with_a_regime_outside_0_and_1_has_the_colour_bar():
    """allresults-local.py:1989-1996: the unclassified rows bring the T_vir colour bar."""
    data, bars = render_tcool_with_regimes(True)
    assert data == [TCOOL_PANEL], f"data axes {data}"
    assert bars == [r"$T_{\rm vir}$ (K)"], f"colour-bar labels {bars}"


def test_labels__tcool_vs_mvir_with_only_regimes_0_and_1_has_no_colour_bar():
    """allresults-local.py:1983-1988: with only regimes 0 and 1 there is no colour bar."""
    data, bars = render_tcool_with_regimes(False)
    assert data == [TCOOL_PANEL], f"data axes {data}"
    assert bars == [], f"colour-bar labels {bars}"


# --------------------------------------------------------------------------------------
# Cap cases: the sample size of each sampled figure
# --------------------------------------------------------------------------------------
# name -> literal cap of its row of the plan: SAGE26's script-level `dilute` 7500
# (allresults-local.py:73), `dilute_sfr` 10000 (:1631) and `dilute` reassigned to 100000 (:1735)
CAPS = {
    "spatial_distribution_3d_box": 7500,  # :1571
    "star_formation_rate": 10000,  # :1632
    "mass_loading_vs_vvir": 7500,  # :1667
    "stellar_vs_halo_mass_by_regime": 7500,  # :1694
    "specific_star_formation_rate": 100000,  # :1738
    "concentration_vs_mvir": 100000,  # :1861 (per panel)
    "sfr_vs_halo_accretion_rate": 100000,  # :1901
    "ejection_efficiency_vs_vvir": 100000,  # :2055
    "disk_radius_vs_stellar_mass": 100000,  # :2146
}
LOWERED_CAP = 40  # below every figure's candidate count in the synthetic catalogue


def make_cap(name):
    def case():
        module = figure_module(name)
        assert module.DILUTE == CAPS[name], f"{name}: DILUTE {module.DILUTE}, expected {CAPS[name]}"

        original_cap, original_sample = module.DILUTE, module.draw_sample
        drawn = []

        def recording_sample(candidates, rng):
            sample = original_sample(candidates, rng)
            drawn.append((len(candidates), len(sample)))
            return sample

        module.DILUTE = LOWERED_CAP
        module.draw_sample = recording_sample
        try:
            run = call_figure(name, build_catalogue(), f"{name}-lowered-cap")
        finally:
            module.DILUTE, module.draw_sample = original_cap, original_sample
        assert run.skip is None, f"{name}: skipped with the lowered cap: {run.skip}"
        # concentration_vs_mvir samples each of its two panels; every other figure samples once
        expected_calls = 2 if name == "concentration_vs_mvir" else 1
        assert len(drawn) == expected_calls, f"{name}: {len(drawn)} sampling calls: {drawn}"
        for candidates, size in drawn:
            assert candidates > LOWERED_CAP, f"{name}: only {candidates} candidates"
            assert size == LOWERED_CAP, f"{name}: sample of {size}, expected {LOWERED_CAP}"
        # A plain scatter whose every drawn point is plotted also holds the lowered number
        if name in ("star_formation_rate", "mass_loading_vs_vvir", "spatial_distribution_3d_box"):
            plotted = [points for _, points in model_artists(run.fig) if points]
            assert plotted == [LOWERED_CAP], f"{name}: plotted points {plotted}"

    case.__name__ = f"test_cap__{name}"
    return case


# --------------------------------------------------------------------------------------
# Unit cases of the helpers the new figures add. Each expectation is a literal written from
# SAGE26's expression at the lines cited, not computed by the helper.
# --------------------------------------------------------------------------------------
def close(got, expected, rel=1e-12):
    """Assert two arrays agree to a relative tolerance."""
    got = np.asarray(got, dtype=float)
    expected = np.asarray(expected, dtype=float)
    assert got.shape == expected.shape, f"shape {got.shape}, expected {expected.shape}"
    assert np.allclose(got, expected, rtol=rel, atol=0.0), f"{got.tolist()} != {expected.tolist()}"


def test_unit__bulge_to_total_ratios():
    """allresults-local.py:1185-1187: BulgeMass / StellarMass, MergerBulgeMass / StellarMass,
    InstabilityBulgeMass / StellarMass."""
    helper = figure_module("bulge_to_total").bulge_to_total_ratios
    stellar = np.array([4.0, 8.0])
    btot, merger, instab = helper(
        stellar, np.array([1.0, 4.0]), np.array([2.0, 1.0]), np.array([3.0, 2.0])
    )
    assert btot.tolist() == [0.25, 0.5], f"B/T {btot.tolist()}"
    assert merger.tolist() == [0.5, 0.125], f"merger {merger.tolist()}"
    assert instab.tolist() == [0.75, 0.25], f"instability {instab.tolist()}"


def test_unit__bulge_to_total_bins():
    """allresults-local.py:1189-1210: 0.25 dex bins from 8.5, a bin needs 5 galaxies.

    Bin 0 (8.5 to 8.75) holds 4 galaxies, so it is not summarised; bin 1 (8.75 to 9.0) holds
    exactly 5 B/T of 0.1 to 0.5, whose median is 0.3 and whose 16th and 84th percentiles (linear
    interpolation at 0.64 and 3.36 of 4 intervals) are 0.164 and 0.436.
    """
    helper = figure_module("bulge_to_total").bulge_to_total_bins
    mass = np.array([8.6, 8.6, 8.7, 8.7] + [8.8, 8.8, 8.9, 8.9, 8.95])
    btot = np.array([0.9, 0.9, 0.9, 0.9] + [0.1, 0.2, 0.3, 0.4, 0.5])
    merger = np.array([0.0] * 4 + [0.1, 0.1, 0.1, 0.1, 0.1])
    instab = np.array([0.0] * 4 + [0.0, 0.25, 0.25, 0.25, 0.5])
    centres, med, lo, hi, merger_mean, instab_mean = helper(mass, btot, merger, instab)
    assert len(centres) == 16, f"{len(centres)} bin centres"
    close(centres[:3], [8.625, 8.875, 9.125])
    assert (
        np.isnan(med[0]) and np.isnan(lo[0]) and np.isnan(hi[0])
    ), "the 4-galaxy bin is summarised"
    assert np.isnan(merger_mean[0]) and np.isnan(instab_mean[0])
    close(med[1], 0.3)
    close(lo[1], 0.164)
    close(hi[1], 0.436)
    close(merger_mean[1], 0.1)
    close(instab_mean[1], 0.25)
    assert np.all(np.isnan(med[2:])), "an empty bin is summarised"


def test_unit__box_edges():
    """allresults-local.py:1583-1588 for BoxSize 2: the bottom square, the top square, then the
    four verticals, each edge [start corner, end corner]."""
    edges = figure_module("spatial_distribution_3d_box").box_edges(2.0)
    expected = [
        [[0, 0, 0], [2, 0, 0]],
        [[2, 0, 0], [2, 2, 0]],
        [[2, 2, 0], [0, 2, 0]],
        [[0, 2, 0], [0, 0, 0]],
        [[0, 0, 2], [2, 0, 2]],
        [[2, 0, 2], [2, 2, 2]],
        [[2, 2, 2], [0, 2, 2]],
        [[0, 2, 2], [0, 0, 2]],
        [[0, 0, 0], [0, 0, 2]],
        [[2, 0, 0], [2, 0, 2]],
        [[2, 2, 0], [2, 2, 2]],
        [[0, 2, 0], [0, 2, 2]],
    ]
    assert np.asarray(edges).tolist() == expected, f"edges {np.asarray(edges).tolist()}"


def test_unit__total_sfr():
    """allresults-local.py:1634: SfrDisk[w2] + SfrBulge[w2]."""
    helper = figure_module("star_formation_rate").total_sfr
    got = helper(np.array([1.5, 0.25]), np.array([0.5, 2.0]))
    assert got.tolist() == [2.0, 2.25], f"total {got.tolist()}"


def test_unit__log_specific_sfr():
    """allresults-local.py:1740-1743: log10((SfrDisk + SfrBulge) / StellarMass), -99 where the
    stellar mass is not above zero. Rates 2, 0, 4, 4 over masses 4, 0, 16, 0 give log10 0.5,
    -99, log10 0.25, -99."""
    helper = figure_module("specific_star_formation_rate").log_specific_sfr
    got = helper(
        np.array([1.0, 0.0, 3.0, 3.0]),
        np.array([1.0, 0.0, 1.0, 1.0]),
        np.array([4.0, 0.0, 16.0, 0.0]),
    )
    close(got, [math.log10(0.5), -99.0, math.log10(0.25), -99.0])


def test_unit__smoothed_histogram():
    """allresults-local.py:1767-1783: ten galaxies in one cell at (10.0, -10.0).

    The cell is Gaussian-smoothed with sigma 1.5 cells, so the peak is 10 / (2 pi 1.5^2) =
    0.7074 (the kernel is cut at 4 sigma) and the total stays 10. Rows above the clip line are
    zeroed: clipping above -11 removes every cell of a population at -10, clipping below -11
    keeps all of it. The grid is np.linspace(8.0, 12.2, 100) by np.linspace(-13, -8, 100), 99 cells each way.
    """
    helper = figure_module("specific_star_formation_rate").smoothed_histogram
    x = np.full(10, 10.0)
    y = np.full(10, -10.0)
    h, x_centers, y_centers = helper(x, y)
    assert h.shape == (99, 99), f"shape {h.shape}"
    assert abs(h.max() - 10.0 / (2.0 * math.pi * 1.5**2)) < 2e-3, f"peak {h.max()}"
    assert abs(h.sum() - 10.0) < 1e-6, f"total {h.sum()}"
    close(x_centers[[0, -1]], [8.0 + 0.5 * 4.2 / 99, 12.2 - 0.5 * 4.2 / 99])
    close(y_centers[[0, -1]], [-13.0 + 0.5 * 5.0 / 99, -8.0 - 0.5 * 5.0 / 99])
    above, _, _ = helper(x, y, clip_above=-11.0)
    assert above.sum() == 0.0, f"clip_above kept {above.sum()}"
    below, _, _ = helper(x, y, clip_below=-11.0)
    assert abs(below.sum() - 10.0) < 1e-6, f"clip_below kept {below.sum()}"
    # A population at -12 is the other way about
    low_y = np.full(10, -12.0)
    kept, _, _ = helper(x, low_y, clip_above=-11.0)
    assert abs(kept.sum() - 10.0) < 1e-6, f"clip_above kept {kept.sum()} of a population at -12"
    dropped, _, _ = helper(x, low_y, clip_below=-11.0)
    assert dropped.sum() == 0.0, f"clip_below kept {dropped.sum()} of a population at -12"


def test_unit__sigma_levels():
    """allresults-local.py:1786-1798: the cells 60, 20, 10, 5, 3, 1, 1 (and zeros) sum to 100,
    so the cumulative fractions are .60 .80 .90 .95 .98 .99 1.0; the first at or above 0.997 is
    the last cell (1), at or above 0.95 the fourth (5), at or above 0.68 the second (20)."""
    helper = figure_module("specific_star_formation_rate").sigma_levels
    cells = np.array([[1.0, 60.0, 0.0], [5.0, 0.0, 20.0], [10.0, 3.0, 1.0]])
    assert helper(cells) == [1.0, 5.0, 20.0], f"levels {helper(cells)}"
    assert helper(np.zeros((3, 3))) is None, "an empty histogram has levels"


def test_unit__halo_accretion_rate():
    """allresults-local.py:1894: MdotCool + MdotStream."""
    helper = figure_module("sfr_vs_halo_accretion_rate").halo_accretion_rate
    got = helper(np.array([1.5, 0.0]), np.array([0.5, 2.0]))
    assert got.tolist() == [2.0, 2.0], f"rate {got.tolist()}"


def test_unit__virial_temperature():
    """allresults-local.py:411: 35.9 * Vvir**2; Vvir 10 and 100 km/s give 3590 and 359000 K."""
    helper = figure_module("tcool_vs_mvir").virial_temperature
    close(helper(np.array([10.0, 100.0])), [3590.0, 359000.0])


def test_unit__cgm_fraction():
    """allresults-local.py:2023: np.log10(CGMgas / Mvir) * 0.17; CGMgas 1 over Mvir 10 and 100
    is log10 0.1 = -1 and log10 0.01 = -2, so -0.17 and -0.34."""
    helper = figure_module("cgm_fraction_vs_mvir").cgm_fraction
    close(helper(np.array([1.0, 1.0]), np.array([10.0, 100.0])), [-0.17, -0.34])


def test_unit__ejection_efficiency():
    """allresults-local.py:2075-2097 (epsilon_disk 2.9, epsilon_halo 0.3, V_sn 501).

    (1) MassLoading 2.9 (f = 1), rate 1, Vvir 100: E_fb = 0.5 * 0.3 * 1 * 1 * 251001 = 37650.15,
    E_lift = 0.5 * 2.9 * 1e4 = 14500, eject = 23150.15 / 5000 = 4.63003, efficiency 4.63003.
    (2) the same at Vvir 1000: E_lift = 1.45e6 exceeds E_fb, so the ejection is clipped to 0.
    (3) MassLoading 5.8 (f = 2), disk 1 + bulge 1 (rate 2), Vvir 100: E_fb = 0.5 * 0.3 * 2 * 2 *
    251001 = 150600.6, E_lift = 0.5 * 11.6 * 1e4 = 58000, eject = 92600.6 / 5000 = 18.52012,
    efficiency 9.26006.
    """
    helper = figure_module("ejection_efficiency_vs_vvir").ejection_efficiency
    got = helper(
        np.array([2.9, 2.9, 5.8]),
        np.array([0.25, 0.25, 1.0]),
        np.array([0.75, 0.75, 1.0]),
        np.array([100.0, 1000.0, 100.0]),
    )
    close(got, [4.63003, 0.0, 9.26006], rel=1e-9)


def binned_median_and_std_case(name):
    """Values 1, 3 in bin 1 (1.0-1.1), 2, 4, 9 in bin 2 (1.1-1.2) and one beyond the last edge."""
    helper = figure_module(name).binned_median_and_std
    x = np.array([1.05, 1.05, 1.15, 1.15, 1.15, 1.35])
    values = np.array([1.0, 3.0, 2.0, 4.0, 9.0, 7.0])
    medians, stds = helper(x, values, np.array([1.0, 1.1, 1.2, 1.3]))
    # bins 1 to 3: np.median 2, 4 and none; np.std (population) 1, sqrt(26 / 3) and none
    assert len(medians) == 3 and len(stds) == 3, f"{len(medians)} medians, {len(stds)} stds"
    assert medians[:2] == [2.0, 4.0], f"medians {medians}"
    assert np.isnan(medians[2]) and np.isnan(stds[2]), "an empty bin has a median"
    close(stds[:2], [1.0, math.sqrt(26.0 / 3.0)])


def test_unit__ejection_binned_median_and_std():
    """allresults-local.py:2105-2109 (np.digitize, np.median, np.std per bin)."""
    binned_median_and_std_case("ejection_efficiency_vs_vvir")


def test_unit__disk_radius_binned_median_and_std():
    """allresults-local.py:2157-2159 (np.digitize, np.median, np.std per bin)."""
    binned_median_and_std_case("disk_radius_vs_stellar_mass")


def test_unit__effective_disk_radius():
    """allresults-local.py:2130, :2136: DiskRadius * 1.0e3 * 1.68; 0.01 and 0.002 Mpc/h give
    16.8 and 3.36 kpc/h."""
    helper = figure_module("disk_radius_vs_stellar_mass").effective_disk_radius
    close(helper(np.array([0.01, 0.002])), [16.8, 3.36])


def test_unit__virial_radius_kpc():
    """allresults-local.py:2139: Rvir * 1.0e3; 0.25 and 0.5 Mpc/h give 250 and 500 kpc/h."""
    helper = figure_module("disk_radius_vs_stellar_mass").virial_radius_kpc
    assert helper(np.array([0.25, 0.5])).tolist() == [250.0, 500.0]


def test_unit__moffett_imf_shift_is_zero():
    """allresults-local.py:46, :52-53, :60-72: imf_shift('chabrier') with whichimf 1 is
    0.00 - 0.00."""
    assert figure_module("bulge_to_total").MOFFETT_IMF_SHIFT_DEX == 0.0


# --------------------------------------------------------------------------------------
# Bin and threshold cases: the constants a new figure builds its bins and its selections from.
# Each expectation is a literal written from SAGE26 at the lines cited, not read back from the
# figure; a figure-level case shows a constant that the figure holds only inline.
# --------------------------------------------------------------------------------------
def test_bins__bulge_to_total_constants():
    """allresults-local.py:1189-1190: np.arange(8.5, 12.5 + 0.25, 0.25), so 17 edges from 8.5 to
    12.5 and 16 centres from 8.625 to 12.375; :1205: a bin of fewer than 5 galaxies is skipped."""
    module = figure_module("bulge_to_total")
    assert module.BIN_START == 8.5, f"BIN_START {module.BIN_START}"
    assert module.BIN_STOP == 12.5, f"BIN_STOP {module.BIN_STOP}"
    assert module.BIN_WIDTH == 0.25, f"BIN_WIDTH {module.BIN_WIDTH}"
    assert module.MIN_GALAXIES_PER_BIN == 5, f"MIN_GALAXIES_PER_BIN {module.MIN_GALAXIES_PER_BIN}"
    # One galaxy in the first bin and five in the last: the last (12.25 to 12.5, centre 12.375)
    # is the only one summarised, and it is the sixteenth centre
    mass = np.array([8.6] + [12.3, 12.35, 12.4, 12.4, 12.45])
    ratio = np.full(6, 0.5)
    centres, med, _, _, _, _ = module.bulge_to_total_bins(mass, ratio, ratio, ratio)
    assert len(centres) == 16, f"{len(centres)} bin centres"
    close(centres[[0, -1]], [8.625, 12.375])
    assert np.isfinite(med).tolist() == [False] * 15 + [True], f"summarised bins {med.tolist()}"


def test_bins__specific_star_formation_rate_constants():
    """allresults-local.py:1758-1759: np.linspace(8.0, 12.2, 100) and np.linspace(-13, -8, 100);
    :1745: sSFRcut = -11.0 (drawn as the dashed line at :1826-1827); :1741-1743: -99 for a
    galaxy with no stellar mass."""
    module = figure_module("specific_star_formation_rate")
    assert len(module.X_BINS) == 100, f"{len(module.X_BINS)} x edges"
    close([module.X_BINS[0], module.X_BINS[-1]], [8.0, 12.2])
    close(module.X_BINS[1] - module.X_BINS[0], 4.2 / 99)
    assert len(module.Y_BINS) == 100, f"{len(module.Y_BINS)} y edges"
    close([module.Y_BINS[0], module.Y_BINS[-1]], [-13.0, -8.0])
    close(module.Y_BINS[1] - module.Y_BINS[0], 5.0 / 99)
    assert module.SSFR_CUT == -11.0, f"SSFR_CUT {module.SSFR_CUT}"
    assert module.NO_SSFR == -99.0, f"NO_SSFR {module.NO_SSFR}"
    cut_lines = [
        line
        for ax in render("specific_star_formation_rate").fig.axes
        for line in ax.lines
        if line.get_label() == "sSFR cut = -11.0"
    ]
    assert len(cut_lines) == 1, f"{len(cut_lines)} cut lines"
    assert list(cut_lines[0].get_ydata()) == [-11.0, -11.0], "the cut line is not at -11"


def test_bins__ejection_efficiency_edges():
    """allresults-local.py:2105: np.arange(1.0, 3.0, 0.1), 20 edges from 1.0 to 2.9 and 19
    centres 1.05, 1.15, ... 2.85, which the median line is drawn at (:2112)."""
    module = figure_module("ejection_efficiency_vs_vvir")
    assert len(module.BIN_EDGES) == 20, f"{len(module.BIN_EDGES)} edges"
    close([module.BIN_EDGES[0], module.BIN_EDGES[-1]], [1.0, 2.9])
    close(module.BIN_EDGES[1] - module.BIN_EDGES[0], 0.1)
    lines = [
        artist
        for artist, _ in model_artists(render("ejection_efficiency_vs_vvir").fig)
        if artist.get_label() == "Median ± 1σ"
    ]
    assert len(lines) == 1, f"{len(lines)} median lines"
    centres = lines[0].get_xdata()
    assert len(centres) == 19, f"{len(centres)} median-line points"
    close(centres[[0, 1, -1]], [1.05, 1.15, 2.85])


def test_bins__disk_radius_edges():
    """allresults-local.py:2138-2139 and :2155-2158: Rvir is multiplied by 1e3 (kpc/h) and the bin
    edges are np.logspace(log10(Rvir[w].min()), log10(Rvir[w].max()), 50), so 49 centres.

    Eleven galaxies of log M* 10 (inside the window) with Rvir 0.20, 0.21, ... 0.30 Mpc/h, so
    Rvir[w] runs from 200 to 300 kpc/h. The 50 edges are 200 * r^k for k = 0 .. 49 with
    r = (300 / 200)^(1/49) = exp(ln 1.5 / 49) = exp(0.0082748) = 1.0083091. The first centre is
    (200 + 200 r) / 2 = 100 (1 + r) = 200.83091 and the last, between the edges 300 / r and 300, is
    150 (1 + 1 / r) = 298.76390; successive centres are in the ratio r. The median line is drawn
    at those centres (:2162).
    """
    module = figure_module("disk_radius_vs_stellar_mass")
    assert module.N_BIN_EDGES == 50, f"N_BIN_EDGES {module.N_BIN_EDGES}"
    catalogue = catalogue_with_stellar_masses(np.full(11, 1.0e10))
    assert np.allclose(catalogue.Rvir[:11], 0.2 + 0.01 * np.arange(11)), "fixture Rvir changed"
    run = call_figure("disk_radius_vs_stellar_mass", catalogue, "disk-bins")
    assert run.skip is None and run.fig is not None, f"skipped: {run.skip}"
    lines = [
        a
        for a, _ in model_artists(run.fig)
        if a.get_label() == "Median ± 1σ (matching Somerville+18 selection)"
    ]
    assert len(lines) == 1, f"{len(lines)} median lines"
    centres = np.asarray(lines[0].get_xdata(), dtype=float)
    assert len(centres) == 49, f"{len(centres)} median-line points"
    # Rvir is stored in single precision, so the minimum and maximum carry a relative 1e-7
    close(centres[[0, -1]], [200.83091, 298.76390], rel=1e-6)
    close(centres[1] / centres[0], 1.0083091, rel=1e-6)
    close(centres[-1] / centres[-2], 1.0083091, rel=1e-6)


def contour_sets_by_side(fig, cut):
    """Return (sets above, sets below) the cut: the model contour sets holding vertices, by the
    side of the line y = cut their vertices lie on (the star-forming set is clipped below the cut,
    the quiescent set above it, so no set straddles it)."""
    from matplotlib.contour import ContourSet

    above = below = 0
    for artist, points in model_artists(fig):
        if not isinstance(artist, ContourSet) or points == 0:
            continue
        y = np.concatenate([np.asarray(path.vertices)[:, 1] for path in artist.get_paths()])
        if np.all(y > cut):
            above += 1
        elif np.all(y < cut):
            below += 1
        else:
            raise AssertionError("a contour set straddles the sSFR cut")
    return above, below


def population_catalogue(n_star_forming, n_quiescent):
    """Return the synthetic catalogue whose first galaxies form two populations around the sSFR
    cut of -11: n_star_forming at log sSFR -10 (+/- 0.2) and n_quiescent at -12 (+/- 0.2), all of
    log M* about 10.5 (+/- 0.3). Every other galaxy has no stellar mass, so it has no sSFR."""
    n = n_star_forming + n_quiescent
    catalogue = build_catalogue()
    rng = np.random.default_rng(7)
    log_mass = rng.normal(10.5, 0.3, n)
    log_ssfr = np.concatenate(
        [rng.normal(-10.0, 0.2, n_star_forming), rng.normal(-12.0, 0.2, n_quiescent)]
    )
    catalogue.StellarMass[:] = 0.0
    catalogue.StellarMass[:n] = 10.0**log_mass * HUBBLE_H / 1.0e10
    catalogue.SfrDisk[:n] = 10.0 ** (log_mass + log_ssfr)
    catalogue.SfrBulge[:n] = 0.0
    return catalogue


def test_threshold__specific_star_formation_rate_population_minimum_is_10():
    """allresults-local.py:1763: `if len(x) < 10: return`, so a population of fewer than 10 galaxies
    draws no contour and one of 10 or more does, each population on its own side of sSFRcut.

    9 star-forming and 12 quiescent galaxies: only the quiescent contours (below -11) are drawn.
    10 star-forming and 9 quiescent: only the star-forming contours (above -11) are drawn.
    """
    for n_star_forming, n_quiescent, want_above, want_below in [
        (9, 12, False, True),
        (10, 9, True, False),
    ]:
        catalogue = population_catalogue(n_star_forming, n_quiescent)
        run = call_figure("specific_star_formation_rate", catalogue, f"ssfr-{n_star_forming}")
        assert run.skip is None and run.fig is not None, f"skipped: {run.skip}"
        above, below = contour_sets_by_side(run.fig, -11.0)
        case = f"{n_star_forming} star-forming, {n_quiescent} quiescent"
        assert (above > 0) == want_above, f"{case}: {above} contour sets above the cut"
        assert (below > 0) == want_below, f"{case}: {below} contour sets below the cut"


def scatter_points(fig):
    """Return the finite points of the model scatters of a figure, summed."""
    from matplotlib.collections import PathCollection

    return sum(
        points for artist, points in model_artists(fig) if isinstance(artist, PathCollection)
    )


def catalogue_with_stellar_masses(masses_msun):
    """Return the synthetic catalogue whose first galaxies hold the given stellar masses (Msun).

    Every galaxy after them has no stellar mass, so no mass cut passes it. The first galaxies
    also satisfy every other cut of the figures that test a mass threshold (a halo, a virial
    radius, a disk radius, a star formation rate, a concentration), and alternate between
    Type 0 and Type 1.
    """
    catalogue = build_catalogue()
    k = len(masses_msun)
    catalogue.StellarMass[:] = 0.0
    catalogue.StellarMass[:k] = np.asarray(masses_msun, dtype=float) * HUBBLE_H / 1.0e10
    catalogue.Mvir[:k] = 1.0
    catalogue.Rvir[:k] = 0.2 + 0.01 * np.arange(k)
    catalogue.DiskScaleRadius[:k] = 0.01
    catalogue.SfrDisk[:k] = 1.0
    catalogue.SfrBulge[:k] = 0.0
    catalogue.Concentration[:k] = 10.0
    catalogue.Type[:k] = np.arange(k) % 2
    return catalogue


def stellar_mass_cut_case(name, cut_msun):
    """Render a figure with six galaxies below and six above a cut and return its scatter points.

    The six below hold 0.99, 0.5, 0.2, 0.1, 0.01 and 0.001 of the cut; the six above hold 1.01,
    2, 5, 10, 100 and 1000 times it.
    """
    below = cut_msun * np.array([0.99, 0.5, 0.2, 0.1, 0.01, 0.001])
    above = cut_msun * np.array([1.01, 2.0, 5.0, 10.0, 100.0, 1000.0])
    run = call_figure(name, catalogue_with_stellar_masses(np.concatenate([below, above])), name)
    assert run.skip is None and run.fig is not None, f"{name}: skipped: {run.skip}"
    return scatter_points(run.fig)


def test_threshold__spatial_distribution_3d_box_cut_is_1e9_msun():
    """allresults-local.py:1570: only galaxies of a stellar mass above 1e9 Msun are drawn."""
    points = stellar_mass_cut_case("spatial_distribution_3d_box", 1.0e9)
    assert points == 6, f"{points} points drawn, expected the 6 above 1e9 Msun"


def test_threshold__star_formation_rate_cut_is_0_01_msun():
    """allresults-local.py:1630: only galaxies of a stellar mass above 0.01 Msun are drawn."""
    points = stellar_mass_cut_case("star_formation_rate", 0.01)
    assert points == 6, f"{points} points drawn, expected the 6 above 0.01 Msun"


def test_threshold__concentration_vs_mvir_cut_is_1e8_msun():
    """allresults-local.py:1852-1854: only galaxies of a stellar mass above 1e8 Msun are drawn,
    over the two panels together."""
    points = stellar_mass_cut_case("concentration_vs_mvir", 1.0e8)
    assert points == 6, f"{points} points drawn, expected the 6 above 1e8 Msun"


def test_threshold__disk_radius_window_is_9_3_to_11_3():
    """allresults-local.py:2141-2146: only galaxies with 9.3 < log10(M*/Msun) < 11.3 are drawn.

    Three galaxies at each of log M* 9.29, 9.31, 11.29 and 11.31: the 9.31 and 11.29 are inside
    the window, the 9.29 and 11.31 outside.
    """
    log_masses = [9.29, 9.31, 11.29, 11.31] * 3
    catalogue = catalogue_with_stellar_masses(10.0 ** np.array(log_masses))
    run = call_figure("disk_radius_vs_stellar_mass", catalogue, "disk-window")
    assert run.skip is None and run.fig is not None, f"skipped: {run.skip}"
    points = scatter_points(run.fig)
    assert points == 6, f"{points} points drawn, expected the 6 inside the window"


# --------------------------------------------------------------------------------------
# Legend cases: the text of every legend entry of a new figure
# --------------------------------------------------------------------------------------
def legend_texts(name):
    """Return the sorted legend texts of a rendered figure (every axes together)."""
    fig = render(name).fig
    assert fig is not None, f"{name}: no figure was captured"
    return sorted(
        t.get_text() for ax in fig.axes if ax.get_legend() for t in ax.get_legend().get_texts()
    )


def legend_case(name, source_lines, expected):
    """Assert a figure's legend holds exactly the SAGE26 texts ``expected`` (order aside)."""
    got = legend_texts(name)
    assert got == sorted(
        expected
    ), f"{name} legend {got}, SAGE26 {source_lines}: {sorted(expected)}"


def test_legend__bulge_to_total():
    """allresults-local.py:1213-1230, legend at :1236."""
    legend_case(
        "bulge_to_total",
        ":1213-1230",
        [
            "Model 16-84th percentile",
            "Model median",
            "Mean merger-driven",
            "Mean instability-driven",
            "Moffett et al. (2016)",
        ],
    )


def test_legend__stellar_vs_halo_mass_by_regime():
    """allresults-local.py:1707, :1711, legend at :1719."""
    legend_case("stellar_vs_halo_mass_by_regime", ":1707, :1711", ["CGM Regime", "Hot Regime"])


def test_legend__specific_star_formation_rate():
    """allresults-local.py:1827 (sSFRcut -11.0), legend at :1838; the contour labels at :1808
    reach no legend entry."""
    legend_case("specific_star_formation_rate", ":1827", ["sSFR cut = -11.0"])


def test_legend__sfr_vs_halo_accretion_rate():
    """allresults-local.py:1918, legend at :1930."""
    legend_case("sfr_vs_halo_accretion_rate", ":1918", ["SFR = accretion rate"])


def test_legend__tcool_vs_mvir():
    """allresults-local.py:1985, :1988, :1993, legend at :2001."""
    legend_case("tcool_vs_mvir", ":1985-1993", ["CGM regime", "Hot regime", "Unclassified"])


def test_legend__ejection_efficiency_vs_vvir_has_none():
    """allresults-local.py:2112 labels the median line but SAGE26 calls no legend in the section
    (:2050-2121), so the figure draws none."""
    assert legend_texts("ejection_efficiency_vs_vvir") == [], "the figure draws a legend"


def test_legend__disk_radius_vs_stellar_mass():
    """allresults-local.py:2162, :2174, legend at :2186."""
    legend_case(
        "disk_radius_vs_stellar_mass",
        ":2162, :2174",
        [
            "Median ± 1σ (matching Somerville+18 selection)",
            r"Somerville+18 SRHR (9.3 < $\log M_\star$ < 11.3)",
        ],
    )


# --------------------------------------------------------------------------------------
# Slice 3: the snapshot-selection helpers (figures/_snapshot_selection.py)
# --------------------------------------------------------------------------------------
# The eight snapshots the run files write and their redshifts, z = 1 / a - 1 with `a` the
# scale factors of simulations/mini-millennium/mini-millennium.a_list (line n + 1 of the file
# holds snapshot n).
RUN_REDSHIFTS = {
    63: 1 / 1.000000 - 1,
    37: 1 / 0.419161 - 1,
    32: 1 / 0.325730 - 1,
    27: 1 / 0.246280 - 1,
    23: 1 / 0.193070 - 1,
    20: 1 / 0.159012 - 1,
    18: 1 / 0.138950 - 1,
    16: 1 / 0.120887 - 1,
}
# The order the engine supplies the snapshots in: increasing redshift
# (snapshot_redshift_mapper.py:267-272)
ENGINE_ORDER = [63, 37, 32, 27, 23, 20, 18, 16]
DECREASING_ORDER = [16, 18, 20, 23, 27, 32, 37, 63]
SHUFFLED_ORDER = [27, 16, 63, 20, 37, 18, 32, 23]

# The fifteen redshift bins of allresults-history.py:761-765
SAGE26_Z_BINS = [
    (0.2, 0.5),
    (0.5, 0.8),
    (0.8, 1.1),
    (1.1, 1.5),
    (1.5, 2.0),
    (2.0, 2.5),
    (2.5, 3.0),
    (3.0, 3.5),
    (3.5, 4.5),
    (4.5, 5.5),
    (5.5, 6.5),
    (6.5, 7.5),
    (7.5, 8.5),
    (8.5, 10.0),
    (10.0, 12.0),
]

# Legend text of the redshift bins the eight snapshots populate (bins 3, 5, 7, 8, 9, 10 and 11:
# snapshots 37, 32, 27, 23, 20, 18 and 16), written from the bins above and RUN_REDSHIFTS
POPULATED_BIN_LABELS = [
    "1.1 < z < 1.5",
    "2.0 < z < 2.5",
    "3.0 < z < 3.5",
    "3.5 < z < 4.5",
    "4.5 < z < 5.5",
    "5.5 < z < 6.5",
    "6.5 < z < 7.5",
]


def selection_module():
    """Return figures/_snapshot_selection.py, imported by the figure modules."""
    return sys.modules["figures._snapshot_selection"]


def snapshots_with_redshifts(redshift_by_snapshot, order=None):
    """Return an engine-style mapping holding only each snapshot's redshift, in ``order``."""
    order = list(redshift_by_snapshot) if order is None else order
    return {snap: (None, 0.0, {"redshift": redshift_by_snapshot[snap]}) for snap in order}


def test_selection__z_bins_are_the_fifteen_of_sage26():
    """allresults-history.py:761-765: the fifteen redshift bins."""
    assert selection_module().Z_BINS == SAGE26_Z_BINS, f"Z_BINS {selection_module().Z_BINS}"


def test_selection__z_range_includes_both_ends():
    """allresults-history.py:771-777: `z_min <= z <= z_max`. Snapshot 11 sits at z = 1.5, which
    is z_max of the bin (1.1, 1.5) and z_min of (1.5, 2.0); snapshot 10 sits at z_min = 1.0 and
    snapshot 12 at z_max = 2.0."""
    helper = selection_module().snapshots_in_z_range
    snapshots = snapshots_with_redshifts({10: 1.0, 11: 1.5, 12: 2.0})
    assert helper(snapshots, 1.1, 1.5) == [11], "z_max not inclusive"
    assert helper(snapshots, 1.5, 2.0) == [11, 12], "z_min or z_max not inclusive"
    assert helper(snapshots, 1.0, 1.4) == [10], "z_min not inclusive"


def test_selection__empty_bin_returns_no_snapshot():
    """A bin no written snapshot falls in returns nothing (allresults-history.py:784-785 then
    skips the bin); no written snapshot at all gives no nearest snapshot and no snapshot."""
    module = selection_module()
    snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, ENGINE_ORDER)
    assert module.snapshots_in_z_range(snapshots, 0.2, 0.5) == [], "bin (0.2, 0.5)"
    assert module.snapshots_in_z_range(snapshots, 10.0, 12.0) == [], "bin (10.0, 12.0)"
    assert module.nearest_snapshot({}, 2.0) is None, "nearest of no snapshot"
    assert module.all_snapshots({}) == [], "all of no snapshot"


def test_selection__eight_snapshots_fall_in_the_literal_bins():
    """The eight written snapshots in the fifteen bins: 37 (z = 1.386) alone in (1.1, 1.5), 32 in
    (2.0, 2.5), 27 in (3.0, 3.5), 23 in (3.5, 4.5), 20 in (4.5, 5.5), 18 in (5.5, 6.5), 16 in
    (6.5, 7.5) and snapshot 63 (z = 0) in none."""
    helper = selection_module().snapshots_in_z_range
    snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, ENGINE_ORDER)
    expected = {3: [37], 5: [32], 7: [27], 8: [23], 9: [20], 10: [18], 11: [16]}
    for index, (z_min, z_max) in enumerate(SAGE26_Z_BINS):
        got = helper(snapshots, z_min, z_max)
        assert got == expected.get(index, []), f"bin {index} ({z_min}, {z_max}): {got}"
    in_any = [s for z_min, z_max in SAGE26_Z_BINS for s in helper(snapshots, z_min, z_max)]
    assert 63 not in in_any, "snapshot 63 (z = 0) is in a bin"


def test_selection__nearest_snapshot_for_the_targets_of_the_plan():
    """allresults-history.py:390 (0, 2, 6) and :1156 (0.1, 1, 2, 4, 6, 8) on the written
    snapshots: 0 -> 63, 2 -> 32, 6 -> 18; 0.1 -> 63, 1.0 -> 37, 2.0 -> 32, 4.0 -> 23, 6.0 -> 18,
    8.0 -> 16."""
    helper = selection_module().nearest_snapshot
    snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, ENGINE_ORDER)
    expected = {
        0.0: 63,
        2.0: 32,
        6.0: 18,
        0.1: 63,
        1.0: 37,
        4.0: 23,
        8.0: 16,
    }
    for target, snap in expected.items():
        assert helper(snapshots, target) == snap, f"target {target}: {helper(snapshots, target)}"


def test_selection__nearest_tie_returns_the_lower_snapshot_number():
    """allresults-history.py:400: `np.argmin` returns the first minimum in snapshot-number order.
    Snapshots 10 (z = 1.0) and 20 (z = 3.0) are both 1.0 from the target 2.0; the lower number
    wins whichever order they are supplied in."""
    helper = selection_module().nearest_snapshot
    for order in ([10, 20], [20, 10]):
        snapshots = snapshots_with_redshifts({10: 1.0, 20: 3.0}, order)
        assert helper(snapshots, 2.0) == 10, f"tie in order {order}: {helper(snapshots, 2.0)}"


def test_selection__results_do_not_depend_on_the_order_snapshots_are_supplied_in():
    """The same snapshots in increasing-redshift (the engine's), decreasing-redshift and a fixed
    shuffled order give identical results from all three helpers: snapshot numbers in increasing
    order (allresults-history.py:334, :773-776)."""
    module = selection_module()
    for order in (ENGINE_ORDER, DECREASING_ORDER, SHUFFLED_ORDER):
        snapshots = snapshots_with_redshifts(RUN_REDSHIFTS, order)
        assert module.snapshots_in_z_range(snapshots, 2.0, 3.5) == [27, 32], f"range, {order}"
        assert module.nearest_snapshot(snapshots, 4.0) == 23, f"nearest, {order}"
        assert module.all_snapshots(snapshots) == [16, 18, 20, 23, 27, 32, 37, 63], f"all, {order}"


# --------------------------------------------------------------------------------------
# Slice 3: figure-level cases on small hand-built catalogues
# --------------------------------------------------------------------------------------
def hand_built(spec):
    """Return the ``snapshots`` argument of an evolution figure from a literal specification.

    ``spec`` maps a snapshot number to (redshift, {field: values}); the snapshots are ordered by
    increasing redshift, as the engine orders them.
    """
    snapshots = {}
    for snap, (redshift, arrays) in sorted(spec.items(), key=lambda item: item[1][0]):
        n = len(next(iter(arrays.values())))
        record = np.zeros(n, dtype=[(name, np.asarray(v).dtype) for name, v in arrays.items()])
        record = record.view(np.recarray)
        for name, values in arrays.items():
            record[name] = values
        snapshots[snap] = (record, VOLUME, snapshot_metadata(redshift, n))
    return snapshots


def run_hand_built(name, spec, label):
    """Call an evolution figure on a hand-built input and return its Run."""
    run = call_figure(name, hand_built(spec), label)
    assert run.skip is None, f"{name} returned a skip message: {run.skip}"
    assert run.fig is not None, f"{name}: no figure was captured"
    return run


def f32(values):
    """Return the values as a float32 array, the type of the output's float fields."""
    return np.asarray(values, dtype=np.float32)


def model_lines(fig):
    """Return the Line2D artists tagged gid="model", in drawing order."""
    from matplotlib.lines import Line2D

    return [a for a, _ in model_artists(fig) if isinstance(a, Line2D)]


def data_axes(fig):
    """Return the data axes of a figure (colour-bar axes left out), in creation order."""
    return [ax for ax in fig.axes if not is_colorbar_axes(ax)]


def test_unit__arange_constants_of_the_new_bins():
    """The edges SAGE26 builds, written from its np.arange calls: allresults-history.py:432
    (0.0, 2.2, 0.05), :789 and :791 (8.0 / 7.2 to 12.5), :863 (10.0 to 15.0), :1170 (6.0 to
    11.5), :1263 and :1265 (8.0 / 7.2 to 13.5), :1554 and :1617 (7.2 to 12.5). np.arange stops
    short of its stop when the width does not divide the range exactly, so the counts and last
    edges are the literals NumPy returns for those arguments (read once, written here)."""
    figures = {name: figure_module(name) for name in FILE_STEMS}
    cases = [
        (figures["rcool_to_rvir_above_mshock"].RATIO_BINS, 44, 0.0, 2.15),
        (figures["sfr_evolution"].mass_bins_for(0), 53, 7.2, 12.4),
        (figures["sfr_evolution"].mass_bins_for(10), 53, 7.2, 12.4),
        (figures["sfr_evolution"].mass_bins_for(11), 45, 8.0, 12.4),
        (figures["sfr_evolution"].mass_bins_for(14), 45, 8.0, 12.4),
        (figures["stellar_mass_halo_mass_relation"].HALO_MASS_BINS, 50, 10.0, 14.9),
        (figures["black_hole_mass_function"].MASS_BINS, 55, 6.0, 11.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(0), 63, 7.2, 13.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(10), 63, 7.2, 13.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(11), 55, 8.0, 13.4),
        (figures["quenched_fraction_evolution"].mass_bins_for(14), 55, 8.0, 13.4),
        (figures["metallicity_evolution"].mass_bins(), 53, 7.2, 12.4),
        (figures["h2_fraction_evolution"].mass_bins(), 53, 7.2, 12.4),
    ]
    for index, (edges, count, first, last) in enumerate(cases):
        assert len(edges) == count, f"case {index}: {len(edges)} edges, expected {count}"
        close([edges[0], edges[-1]], [first, last], rel=1e-9)
    close(np.diff(figures["rcool_to_rvir_above_mshock"].RATIO_BINS)[:3], [0.05] * 3, rel=1e-9)


# ---- rcool_to_rvir_above_mshock ----
def test_unit__rcool_helpers():
    """allresults-history.py:413 (`(Mvir > Mshock_Msun) & (rcool_ratio >= 0.0)`), :421-422
    (`np.mean(r > 1.0)`, `np.mean(r >= 1.0)`), :443 (`np.clip` to the first and last bin edge,
    0.0 and 2.15)."""
    module = figure_module("rcool_to_rvir_above_mshock")
    assert module.MSHOCK_MSUN == 6.0e11, f"Mshock {module.MSHOCK_MSUN}"  # :389
    assert module.TARGET_REDSHIFTS == [0.0, 2.0, 6.0], f"targets {module.TARGET_REDSHIFTS}"  # :390
    mvir = np.array([5.0e11, 6.0e11, 7.0e11, 8.0e11])
    ratio = np.array([0.5, 0.5, -0.1, 0.0])
    got = module.select_above_mshock(mvir, ratio).tolist()
    assert got == [False, False, False, True], f"selection {got}"
    ratios = np.array([0.5, 1.0, 1.5, 2.0])
    assert module.strict_fraction(ratios) == 0.5, "strict fraction"
    assert module.at_least_one_fraction(ratios) == 0.75, "fraction at or above 1"
    close(module.clip_to_bins(np.array([-1.0, 1.0, 3.0])), [0.0, 1.0, 2.15])


def test_legend__rcool_to_rvir_above_mshock():
    """allresults-history.py:443-446: `z = {actual_z:.1f}  (N={N}, f$_{{>1}}$={f_strict:.2f})`
    for the snapshots nearest z = 0, 2 and 6 (snapshots 63, 32, 18). Mvir is in 1e10 Msun/h, so
    Msun = Mvir x 1e10 / 0.73. Snapshot 63: three haloes above 6e11 Msun with ratios 0.5, 1.5,
    1.5 (one is -1, undefined, one is below Mshock), N = 3, 2 of 3 above 1. Snapshot 32: four
    above, one of them above 1, 0.25. Snapshot 18: three above (43.9 is 6.014e11 Msun), the
    43.7 halo (5.986e11 Msun) is below Mshock, none above 1, 0.00."""
    spec = {
        63: (
            0.0,
            {"Mvir": f32([100, 100, 100, 100, 10]), "RcoolToRvir": f32([0.5, 1.5, 1.5, -1, 5])},
        ),
        32: (
            RUN_REDSHIFTS[32],
            {"Mvir": f32([100, 100, 100, 100]), "RcoolToRvir": f32([2.0, 0.5, 0.5, 0.5])},
        ),
        18: (
            RUN_REDSHIFTS[18],
            {"Mvir": f32([100, 100, 43.9, 43.7]), "RcoolToRvir": f32([1.0, 1.0, 0.5, 3.0])},
        ),
    }
    run = run_hand_built("rcool_to_rvir_above_mshock", spec, "rcool-legend")
    texts = sorted(t.get_text() for t in data_axes(run.fig)[0].get_legend().get_texts())
    expected = [
        "z = 0.0  (N=3, f$_{>1}$=0.67)",
        "z = 2.1  (N=4, f$_{>1}$=0.25)",
        "z = 6.2  (N=3, f$_{>1}$=0.00)",
    ]
    assert texts == sorted(expected), f"legend {texts}, SAGE26 :443-446 gives {sorted(expected)}"


# ---- sfr_evolution, quenched_fraction_evolution, metallicity_evolution, h2_fraction_evolution:
# the helpers they share the form of ----
def binned_case(module):
    """Assert ``binned_mean_and_std`` on literal values: masses 8.05, 8.07 and 8.35 over the bins
    8.0, 8.1, 8.2, 8.3, 8.4 hold the values 1, 3 and 5; the first bin has mean 2 and standard
    deviation 1 (`np.std` of 1 and 3), the last mean 5 and standard deviation 0, and the empty
    middle bins are NaN."""
    bins = np.array([8.0, 8.1, 8.2, 8.3, 8.4])
    means, stds = module.binned_mean_and_std(
        np.array([8.05, 8.07, 8.35]), np.array([1.0, 3.0, 5.0]), bins
    )
    assert np.isnan(means).tolist() == [False, True, True, False], f"means {means.tolist()}"
    close(means[[0, 3]], [2.0, 5.0])
    close(stds[[0, 3]], [1.0, 0.0])
    assert np.isnan(stds).tolist() == [False, True, True, False], f"stds {stds.tolist()}"


def mean_and_error_case(module):
    """Assert ``mean_and_error`` on the stack [[1, nan], [3, nan]]: the mean over snapshots is 2
    in the first bin (`np.nanmean`), its error `np.nanstd([1, 3]) / np.sqrt(2)` = 1 / 1.41421356,
    and the empty second bin is not valid."""
    mean, err, valid = module.mean_and_error(np.array([[1.0, np.nan], [3.0, np.nan]]))
    close(mean[:1], [2.0])
    close(err[:1], [0.7071067811865476])
    assert valid.tolist() == [True, False], f"valid {valid.tolist()}"


def test_unit__sfr_evolution_helpers():
    """allresults-history.py:800 (`(M > 0.0) & (sfr_total > 0.0)`), :805-813, :820-822."""
    module = figure_module("sfr_evolution")
    got = module.select_star_forming(np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0]))
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    binned_case(module)
    mean_and_error_case(module)


def test_unit__metallicity_evolution_helpers():
    """allresults-history.py:1561 (`(M > 0.0) & (metals > 0.0)`), :1565-1573, :1581-1583."""
    module = figure_module("metallicity_evolution")
    got = module.select_with_metals(np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0]))
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    binned_case(module)
    mean_and_error_case(module)


def test_unit__h2_fraction_evolution_helpers():
    """allresults-history.py:1616 (0.74), :1654 (`(mstar > 0.0) & (cold > 0.0)`), :1660
    (`np.clip(h2 / (cold * 0.74), 0.0, 1.0)`), :1662-1668, :1700-1702. H2 of 0.37, 0.9 and 0.0
    over cold gas 1.0 give 0.5, 1 (0.9 / 0.74 = 1.216 is clipped) and 0."""
    module = figure_module("h2_fraction_evolution")
    assert module.HYDROGEN_MASS_FRAC == 0.74, f"hydrogen fraction {module.HYDROGEN_MASS_FRAC}"
    got = module.select_with_cold_gas(
        np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0])
    )
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    close(
        module.molecular_fraction(np.array([0.37, 0.9, 0.0]), np.array([1.0, 1.0, 1.0])),
        [0.5, 1.0, 0.0],
    )
    binned_case(module)
    mean_and_error_case(module)


def test_unit__stellar_mass_halo_mass_relation_helpers():
    """allresults-history.py:889-890 (`(M > 0.0) & (Mhalo > 0.0)`), :897-903 (the mean of the
    linear stellar mass in each log10 halo-mass bin), :914-918. Halo masses 10.05, 10.07 and 10.35
    over the bins 10.0, 10.1, 10.2, 10.3, 10.4 hold stellar masses 10, 30 and 50: the first bin
    has mean 20, the last 50, the middle bins NaN."""
    module = figure_module("stellar_mass_halo_mass_relation")
    got = module.select_with_halo(np.array([1.0, 0.0, 2.0, 3.0]), np.array([1.0, 1.0, 0.0, 2.0]))
    assert got.tolist() == [0, 3], f"selection {got.tolist()}"
    bins = np.array([10.0, 10.1, 10.2, 10.3, 10.4])
    means = module.mean_stellar_mass_in_halo_bins(
        np.array([10.0, 30.0, 50.0]), np.array([10.05, 10.07, 10.35]), bins
    )
    assert np.isnan(means).tolist() == [False, True, True, False], f"means {means.tolist()}"
    close(means[[0, 3]], [20.0, 50.0])
    mean_and_error_case(module)


def test_unit__stellar_mass_halo_mass_relation_line_styles():
    """allresults-history.py:860: `['-', ':', '--', '-.'] * 4`."""
    module = figure_module("stellar_mass_halo_mass_relation")
    expected = ["-", ":", "--", "-."] * 4
    assert module.LINE_STYLES == expected, f"line styles {module.LINE_STYLES}"


def test_figure__stellar_mass_halo_mass_relation_line_styles_follow_the_bin_index():
    """:924-925 draws bin i with `line_styles[i]`; the populated bins are 3, 5, 7, 8, 9, 10 and
    11, whose styles are '-.', ':', '-.', '-', ':', '--', '-.' (the cycle of :860)."""
    lines = model_lines(render("stellar_mass_halo_mass_relation").fig)
    got = [line.get_linestyle() for line in lines]
    assert got == ["-.", ":", "-.", "-", ":", "--", "-."], f"line styles {got}"


# ---- quenched_fraction_evolution ----
def test_unit__quenched_fraction_helpers():
    """allresults-history.py:23 (sSFRcut -11.0), :1282 and :1286 (`sfr / M < 10.0**sSFRcut`),
    :1298-1303 (the fraction of the selected galaxies that are quenched in each mass bin). A
    galaxy of 1e10 Msun forming 0.09 Msun/yr has 9e-12 per year and is quenched, one forming
    0.11 has 1.1e-11 and is not. In the bins 8.0 to 8.1 and 8.1 to 8.2, masses 8.05, 8.06, 8.07
    and 8.15, quenched True, False, True, True and selected True, True, False, True: the first
    bin holds two selected galaxies of which one is quenched (0.5), the second one selected
    quenched galaxy (1.0), and the third bin none (NaN)."""
    module = figure_module("quenched_fraction_evolution")
    assert module.SSFR_CUT == -11.0, f"sSFRcut {module.SSFR_CUT}"
    got = module.is_quenched(np.array([0.09, 0.11]), np.array([1.0e10, 1.0e10]))
    assert got.tolist() == [True, False], f"quenched {got.tolist()}"
    fractions = module.quenched_fraction_in_bins(
        np.array([8.05, 8.06, 8.07, 8.15]),
        np.array([True, False, True, True]),
        np.array([True, True, False, True]),
        np.array([8.0, 8.1, 8.2, 8.3]),
    )
    close(fractions[:2], [0.5, 1.0])
    assert np.isnan(fractions[2]), f"empty bin {fractions[2]}"


def test_figure__quenched_fraction_orphans_are_in_neither_curve():
    """allresults-history.py:1273 (rate = SfrDisk + SfrBulge), :1282 (rate over the stellar mass
    in Msun), :1297 and :1307: centrals are `gtype == 0` and satellites `gtype == 1`, so an orphan
    (Type 2) is in neither. One snapshot at z = 1.39 (the bin 1.1 to 1.5) with three galaxies of
    1.37e10 Msun (StellarMass 1.0 x 1e10 / 0.73) in one mass bin: a central and an orphan forming
    0.05 + 0.05 = 0.1 Msun/yr (7.3e-12 per year, quenched) and a satellite forming 0.09 + 0.09 =
    0.18 (1.3e-11, not quenched). The central curve is 1.0 and the satellite curve 0.0; counting
    the orphan as a satellite would give 0.5, and taking only the disk rate would quench the
    satellite (6.6e-12) and give 1.0."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {
                "StellarMass": f32([1.0, 1.0, 1.0]),
                "SfrDisk": f32([0.05, 0.09, 0.05]),
                "SfrBulge": f32([0.05, 0.09, 0.05]),
                "Type": np.array([0, 1, 2], dtype=np.int32),
            },
        )
    }
    run = run_hand_built("quenched_fraction_evolution", spec, "quenched-orphans")
    lines = model_lines(run.fig)
    assert len(lines) == 2, f"{len(lines)} model lines"
    central, satellite = lines
    assert central.get_linestyle() == "-" and satellite.get_linestyle() == ":", "line styles"
    assert central.get_ydata().tolist() == [1.0], f"central {central.get_ydata().tolist()}"
    assert satellite.get_ydata().tolist() == [0.0], f"satellite {satellite.get_ydata().tolist()}"
    close(central.get_xdata(), [10.15])


# ---- concentration_evolution ----
def test_unit__concentration_statistics():
    """allresults-history.py:1475-1480 (the four mass bins and their legend text), :1481 (the
    colours), :1501-1509 (`(conc > 0) & (mvir >= mlo) & (mvir < mhi)`; with at least 5 galaxies
    the median and the 16th and 84th percentiles, else NaN). Concentrations 1 to 5 give the median
    3 and, by linear interpolation at 0.64 and 3.36 of 4 intervals, 1.64 and 4.36; four give NaN;
    a concentration of 0 and a halo exactly at the upper edge are not counted, one exactly at the
    lower edge is (the last case below: five counted with it, four without)."""
    module = figure_module("concentration_evolution")
    assert module.MASS_BINS == [
        (1e10, 1e11, r"$10^{10} - 10^{11}$"),
        (1e11, 1e12, r"$10^{11} - 10^{12}$"),
        (1e12, 1e13, r"$10^{12} - 10^{13}$"),
        (1e13, 1e15, r"$10^{13} - 10^{15}$"),
    ], f"mass bins {module.MASS_BINS}"
    assert module.COLORS == ["dodgerblue", "forestgreen", "darkorange", "firebrick"], "colours"
    assert module.MIN_GALAXIES == 5, f"minimum {module.MIN_GALAXIES}"
    median, low, high = module.concentration_statistics(
        np.array([1.0, 2.0, 3.0, 4.0, 5.0]), np.full(5, 5.0e10), 1e10, 1e11
    )
    close([median, low, high], [3.0, 1.64, 4.36])
    four = module.concentration_statistics(
        np.array([1.0, 2.0, 3.0, 4.0]), np.full(4, 5.0e10), 1e10, 1e11
    )
    assert np.isnan(four).all(), f"four galaxies {four}"
    # Seven rows of which five count: the halo exactly at the lower edge 1e10 (concentration 1) and
    # four ordinary ones (2 to 5) make five, so with the lower edge counted the median is 3 and the
    # percentiles 1.64 and 4.36 (as for 1 to 5 above), and without it (`>` instead of `>=`) there
    # are four and the result is NaN; a halo exactly at the upper edge 1e11 (concentration 100)
    # and a zero concentration (in an ordinary halo) are not counted: counting either would give
    # the median 3.5 (1, 2, 3, 4, 5, 100) or 2.5 (0, 1, 2, 3, 4, 5).
    edge = module.concentration_statistics(
        np.array([1.0, 2.0, 3.0, 4.0, 5.0, 100.0, 0.0]),
        np.array([1.0e10, 2.0e10, 3.0e10, 4.0e10, 5.0e10, 1.0e11, 6.0e10]),
        1e10,
        1e11,
    )
    close(edge, [3.0, 1.64, 4.36])


# ---- star_formation_efficiency ----
def test_unit__star_formation_efficiency_helpers():
    """allresults-history.py:1736 (f_b = 0.17), :1762 (`(gtype == 0) & (mstar > 0) & (mvir >
    0)`), :1763-1769 (at least 5 galaxies; `sfe = mstar / (mvir * f_baryon)`; median and 16th and
    84th percentiles). Stellar masses 17, 34, 51, 68, 85 over halo masses of 100 give 1, 2, 3, 4, 5:
    median 3, percentiles 1.64 and 4.36 (linear interpolation at 0.64 and 3.36 of 4 intervals);
    four galaxies give none."""
    module = figure_module("star_formation_efficiency")
    assert module.F_BARYON == 0.17, f"f_b {module.F_BARYON}"
    assert module.MIN_CENTRALS == 5, f"minimum {module.MIN_CENTRALS}"
    got = module.select_centrals(
        np.array([0, 1, 0, 0, 2]),
        np.array([1.0, 1.0, 0.0, 1.0, 1.0]),
        np.array([1.0, 1.0, 1.0, 0.0, 1.0]),
    )
    assert got.tolist() == [0], f"selection {got.tolist()}"
    stellar = np.array([17.0, 34.0, 51.0, 68.0, 85.0])
    close(module.efficiency_statistics(stellar, np.full(5, 100.0)), [3.0, 1.64, 4.36])
    assert module.efficiency_statistics(stellar[:4], np.full(4, 100.0)) is None, "four galaxies"


def test_unit__age_of_the_universe():
    """allresults-history.py:1741-1745: (9.778 / h) x the integral of dz / ((1 + z) E(z)) from z to
    1000 (trapezoid rule on 10000 points). For a flat universe with Omega_lambda = 1 (E = 1) the
    integral is ln(1001 / (1 + z)), so with h = 0.5 the age at z = 0 is 19.556 x ln(1001) = 135.07
    and at z = 1 is 19.556 x ln(1001 / 2) = 121.52. For Omega_m = 1 (E = (1 + z)^1.5) the integral
    is 2/3 x ((1 + z)^-1.5 - 1001^-1.5), so with h = 1 the age at z = 0 is 6.5187 x (1 - 1001^-1.5)
    = 6.5185 and at z = 3 is 6.5187 x (4^-1.5 - 1001^-1.5) = 0.81481. The trapezoid error on the
    steepest integrand (0.1 steps) is below 0.3%, so the comparison is to 1%."""
    helper = figure_module("star_formation_efficiency").z_to_age_gyr
    de_sitter = [helper(0.0, 0.0, 1.0, 0.5), helper(1.0, 0.0, 1.0, 0.5)]
    close(de_sitter, [135.07, 121.52], rel=1e-2)
    einstein_de_sitter = [helper(0.0, 1.0, 0.0, 1.0), helper(3.0, 1.0, 0.0, 1.0)]
    close(einstein_de_sitter, [6.5185, 0.81481], rel=1e-2)


# ---- central_satellite_counts and quenched_central_satellite_counts ----
def test_unit__central_satellite_counts_helper():
    """allresults-history.py:1827-1828: `np.sum(types == 0)` and `np.sum(types >= 1)` (orphans,
    Type 2, are satellites). Types 0, 0, 1, 2, 3 hold 2 centrals and 3 satellites."""
    module = figure_module("central_satellite_counts")
    got = module.count_centrals_and_satellites(np.array([0, 0, 1, 2, 3]))
    assert [int(v) for v in got] == [2, 3], f"counts {got}"


def test_unit__quenched_counts_helper():
    """allresults-history.py:23 (sSFRcut -11.0), :2006-2009: `sfr / M < 10.0**sSFRcut`, with
    `(types == 0) & quenched` and `(types >= 1) & quenched`. Five galaxies of 1e10 Msun with Types
    0, 0, 1, 2, 1 and rates 0.09, 0.11, 0.05, 0.01, 0.2: the quenched are the first, third and
    fourth, so one central and two satellites; a galaxy with no stellar mass is never quenched
    (its rate is infinite or NaN)."""
    module = figure_module("quenched_central_satellite_counts")
    assert module.SSFR_CUT == -11.0, f"sSFRcut {module.SSFR_CUT}"
    stellar = np.array([1.0e10] * 5 + [0.0, 0.0])
    sfr = np.array([0.09, 0.11, 0.05, 0.01, 0.2, 1.0, 0.0])
    types = np.array([0, 0, 1, 2, 1, 0, 1])
    got = module.count_quenched(types, sfr, stellar)
    assert [int(v) for v in got] == [1, 2], f"counts {got}"


def tick_labels(ax):
    """Return the text of the x tick labels."""
    return [t.get_text() for t in ax.get_xticklabels()]


COUNT_TICKS = [
    "0.2-0.5",
    "0.5-0.8",
    "0.8-1.1",
    "1.1-1.5",
    "1.5-2.0",
    "2.0-2.5",
    "2.5-3.0",
    "3.0-3.5",
    "3.5-4.5",
    "4.5-5.5",
    "5.5-6.5",
    "6.5-7.5",
    "7.5-8.5",
    "8.5-10.0",
    "10.0-12.0",
]


def counts_figure_case(name, source_lines):
    """The tick labels, rotation, windows and absent legend of a counts figure."""
    fig = render(name).fig
    ax = data_axes(fig)[0]
    assert tick_labels(ax) == COUNT_TICKS, f"{name} ticks {tick_labels(ax)}, SAGE26 {source_lines}"
    rotations = {t.get_rotation() for t in ax.get_xticklabels()}
    assert rotations == {45.0}, f"{name} rotation {rotations}"
    assert ax.get_legend() is None, f"{name} draws a legend, SAGE26 has none"
    close(ax.get_xlim(), [-0.5, 14.5])
    heights = [artist.get_height() for artist, _ in model_artists(fig)]
    # Upper limit: 1.1 x the autoscaled top, matplotlib's default 5% margin above the tallest
    # bar, so 1.155 x the tallest bar
    close([ax.get_ylim()[0], ax.get_ylim()[1] / max(heights)], [0.0, 1.155], rel=1e-9)


def test_figure__central_satellite_counts_ticks_windows_and_no_legend():
    """allresults-history.py:1839-1846."""
    counts_figure_case("central_satellite_counts", ":1839-1846")


def test_figure__quenched_central_satellite_counts_ticks_windows_and_no_legend():
    """allresults-history.py:2020-2027."""
    counts_figure_case("quenched_central_satellite_counts", ":2020-2027")


def test_figure__central_satellite_counts_bars_are_the_mean_counts():
    """allresults-history.py:1836-1837: a bar of the mean central count at i - 0.15 and one of the
    mean satellite count at i + 0.15, both of width 0.3. Snapshot 37 (z = 1.39, bin 3) holds
    Types 0, 0, 1, 2, 1: 2 centrals and 3 satellites (the orphan counts as one)."""
    spec = {37: (RUN_REDSHIFTS[37], {"Type": np.array([0, 0, 1, 2, 1], dtype=np.int32)})}
    run = run_hand_built("central_satellite_counts", spec, "counts-bars")
    bars = sorted(
        (a.get_x() + a.get_width() / 2, a.get_height(), a.get_width())
        for a, _ in model_artists(run.fig)
    )
    close([b[0] for b in bars], [2.85, 3.15])
    close([b[1] for b in bars], [2.0, 3.0])
    close([b[2] for b in bars], [0.3, 0.3])


# ---- central_satellite_fraction ----
def test_unit__central_satellite_fractions_helper():
    """allresults-history.py:1869-1876: `n_cen / n_total` and `n_sat / n_total` with
    `types >= 1` for satellites. Types 0, 0, 1, 2 give 0.5 and 0.5; Types 0, 1, 1, 1 give 0.25
    and 0.75."""
    helper = figure_module("central_satellite_fraction").central_and_satellite_fractions
    close(helper(np.array([0, 0, 1, 2])), [0.5, 0.5])
    close(helper(np.array([0, 1, 1, 1])), [0.25, 0.75])


def test_figure__central_satellite_fraction_window_ends_at_the_largest_redshift():
    """allresults-history.py:1890-1891: x from 0 to the largest z drawn, y from 0 to 1.05. Two
    snapshots (z = 1.39 and 2.07) with Types 0, 1 and 0, 0, 0, 1: the lines are drawn at those
    redshifts, centrals 0.5 and 0.75, satellites 0.5 and 0.25."""
    spec = {
        37: (RUN_REDSHIFTS[37], {"Type": np.array([0, 1], dtype=np.int32)}),
        32: (RUN_REDSHIFTS[32], {"Type": np.array([0, 0, 0, 1], dtype=np.int32)}),
    }
    run = run_hand_built("central_satellite_fraction", spec, "fraction-window")
    ax = data_axes(run.fig)[0]
    close(ax.get_xlim(), [0.0, 2.0700273232431767])
    close(ax.get_ylim(), [0.0, 1.05])
    centrals, satellites = model_lines(run.fig)
    close(centrals.get_ydata(), [0.5, 0.75])
    close(satellites.get_ydata(), [0.5, 0.25])
    close(centrals.get_xdata(), [1.3857181369449925, 2.0700273232431767])


# ---- central_satellite_fraction_massive_quiescent ----
def test_unit__ssfr_threshold():
    """allresults-history.py:1913, :1938-1939: `0.2 * E_z / ((9.778 / h) * 1e9)` with E_z =
    sqrt(Om (1 + z)^3 + OL). For Om = 1, OL = 0, h = 1: at z = 0, E = 1 and 0.2 / 9.778e9; at z = 3,
    E = 8 and 1.6 / 9.778e9. For Om = 0, OL = 1, h = 0.5: E = 1 and 0.2 / (19.556e9)."""
    helper = figure_module("central_satellite_fraction_massive_quiescent").ssfr_threshold
    close([helper(0.0, 1.0, 0.0, 1.0), helper(3.0, 1.0, 0.0, 1.0)], [0.2 / 9.778e9, 1.6 / 9.778e9])
    close([helper(2.0, 0.0, 1.0, 0.5)], [0.2 / 19.556e9])


def test_unit__massive_quiescent_counts():
    """allresults-history.py:1926-1948. Twenty galaxies of stellar mass 1 to 20: the 90th
    percentile is 18.1 (interpolation at index 17.1 between 18 and 19), so the most massive are
    those of mass 19 and 20. With a threshold of 0.5 per unit mass and rates 5 and 20 for them
    (5 / 19 = 0.263 is below the threshold, 20 / 20 = 1.0 is not) there is one quiescent galaxy,
    the mass-19 one, here an orphan (a satellite): counts (1, 0, 1). A snapshot with 9 galaxies of
    positive mass is skipped (fewer than 10), as is one with no quiescent galaxy among the most
    massive; a galaxy with no mass is left out of the percentile."""
    module = figure_module("central_satellite_fraction_massive_quiescent")
    assert module.MIN_GALAXIES == 10, f"minimum {module.MIN_GALAXIES}"
    assert module.MASS_PERCENTILE == 90, f"percentile {module.MASS_PERCENTILE}"
    mass = np.arange(1.0, 21.0)
    sfr = np.full(20, 100.0)
    sfr[18] = 5.0  # mass 19: 5 / 19 = 0.263 < 0.5, quiescent
    sfr[19] = 20.0  # mass 20: 1.0 > 0.5, forming
    types = np.zeros(20, dtype=int)
    types[18] = 2  # the quiescent galaxy is an orphan, a satellite
    got = module.massive_quiescent_counts(mass, sfr, types, 0.5)
    assert [int(v) for v in got] == [1, 0, 1], f"counts {got}"
    # Quiescent galaxies below the top 10% do not count: raise the rates of the top two
    sfr_all_forming = np.full(20, 0.1)
    sfr_all_forming[18:] = 100.0
    assert module.massive_quiescent_counts(mass, sfr_all_forming, types, 0.5) is None, "none"
    # Nine galaxies of positive mass, plus eleven with no mass
    sparse_mass = np.concatenate([np.arange(1.0, 10.0), np.zeros(11)])
    assert module.massive_quiescent_counts(sparse_mass, sfr, types, 0.5) is None, "nine galaxies"
    # Ten of positive mass are enough: the 90th percentile of 1..10 is 9.1, so only mass 10 is
    # most massive; it is a central with a low rate
    ten_mass = np.concatenate([np.arange(1.0, 11.0), np.zeros(10)])
    ten_sfr = np.concatenate([np.full(10, 0.1), np.zeros(10)])
    ten_types = np.zeros(20, dtype=int)
    got = module.massive_quiescent_counts(ten_mass, ten_sfr, ten_types, 0.5)
    assert [int(v) for v in got] == [1, 1, 0], f"ten galaxies {got}"


def test_figure__cosmology_missing_from_the_run_is_a_skip_with_a_message():
    """Omega, OmegaLambda and Hubble_h come from the engine's run parameters
    (mimic-plot.py:588-593); without them the two figures that need the cosmology return a
    message, not an error."""
    for name in ("star_formation_efficiency", "central_satellite_fraction_massive_quiescent"):
        snapshots = build_evolution_input(evolution_fields(name))
        path, skip = FIGURES.PLOT_FUNCS[name](
            snapshots=snapshots,
            params={},
            output_dir=os.path.join(TMP_ROOT, name + "-no-cosmology"),
            output_format=".png",
            verbose=False,
        )
        plt.close("all")
        assert path is None and skip, f"{name}: path {path}, skip {skip}"


# ---- ffb_fraction_cut_down ----
def test_unit__ffb_fraction_helper():
    """paper_plots.py:83 (MIN_PARTICLES 20), :513-515 (rows with `Len >= 20`), :10404-10410
    (`np.sum(ffb == 1) / len(ffb)`, NaN with no such row). Rows with Len 19, 20, 25, 100 and
    FFBRegime 1, 1, 0, 1: the Len 19 row is left out, so 1 of 3 (the Len 20 and Len 100 rows are
    FFB). With every Len below 20 the fraction is NaN, not 0."""
    helper = figure_module("ffb_fraction_cut_down").ffb_fraction
    assert figure_module("ffb_fraction_cut_down").MIN_PARTICLES == 20, "MIN_PARTICLES"
    ffb = np.array([1, 1, 0, 1])
    length = np.array([19, 20, 25, 100])
    close([helper(ffb, length)], [2.0 / 3.0])
    only_unresolved = helper(np.array([1, 1]), np.array([19, 1]))
    assert np.isnan(only_unresolved), f"all unresolved gave {only_unresolved}, not NaN"
    assert np.isnan(helper(np.array([], dtype=int), np.array([], dtype=int))), "empty snapshot"


def test_unit__ffb_step_edges_for_the_written_snapshots():
    """paper_plots.py:10418-10427: `z <= 15`, then the edges 15.0, the midpoint of each
    consecutive pair of redshifts in increasing snapshot number, 0.0. The eight written
    snapshots 16, 18, 20, 23, 27, 32, 37, 63 (z = 7.272, 6.197, 5.289, 4.179, 3.060, 2.070, 1.386,
    0) give 15.0, the seven midpoints 6.7345, 5.7428, 4.7341, 3.6199, 2.5652, 1.7279, 0.6929 and
    0.0 (midpoints of the redshifts 1/a - 1 of the a_list scale factors), whatever order the
    snapshots are supplied in; a snapshot above z = 15 is not drawn."""
    helper = figure_module("ffb_fraction_cut_down").step_edges
    z = RUN_REDSHIFTS
    expected = [
        15.0,
        (z[16] + z[18]) / 2.0,
        (z[18] + z[20]) / 2.0,
        (z[20] + z[23]) / 2.0,
        (z[23] + z[27]) / 2.0,
        (z[27] + z[32]) / 2.0,
        (z[32] + z[37]) / 2.0,
        (z[37] + z[63]) / 2.0,
        0.0,
    ]
    close(expected[1:8], [6.7345, 5.7428, 4.7341, 3.6199, 2.5652, 1.7279, 0.6929], rel=1e-3)
    for order in (ENGINE_ORDER, DECREASING_ORDER, SHUFFLED_ORDER):
        drawn, edges = helper(snapshots_with_redshifts(RUN_REDSHIFTS, order))
        assert drawn == [16, 18, 20, 23, 27, 32, 37, 63], f"drawn {drawn} in order {order}"
        close(edges, expected)
    high = {5: 16.0, 6: 15.0, 7: 3.0}
    drawn, edges = helper(snapshots_with_redshifts(high))
    assert drawn == [6, 7], f"z above 15 drawn: {drawn}"
    close(edges, [15.0, 9.0, 0.0])


def test_figure__ffb_fraction_step_values_and_axes():
    """paper_plots.py:10431-10433, :10357-10371. Snapshots 63 and 16 with Len 30, 30, 30, 10 and
    FFBRegime 1, 0, 0, 1 (the unresolved row is left out: 1 of 3 = 0.333) and Len 30, 30 and
    FFBRegime 0, 0 (zero, which the log axis does not show). The step is drawn in log10(1 + z)
    from 0 to log10(16) with the bottom ticks 0, 1, 2, 3, 5, 7, 10, 15 and the top ticks 0.0 to
    1.2."""
    spec = {
        63: (
            0.0,
            {
                "FFBRegime": np.array([1, 0, 0, 1], dtype=np.int32),
                "Len": np.array([30, 30, 30, 10], dtype=np.int32),
            },
        ),
        16: (
            RUN_REDSHIFTS[16],
            {
                "FFBRegime": np.array([0, 0], dtype=np.int32),
                "Len": np.array([30, 30], dtype=np.int32),
            },
        ),
    }
    run = run_hand_built("ffb_fraction_cut_down", spec, "ffb-step")
    steps = [artist for artist, _ in model_artists(run.fig)]
    assert len(steps) == 1, f"{len(steps)} model artists"
    values, edges = steps[0].get_data().values, steps[0].get_data().edges
    # In increasing snapshot number: snapshot 16 (0), then 63 (1/3)
    close(values, [0.0, 1.0 / 3.0])
    z16 = RUN_REDSHIFTS[16]
    close(edges, np.log10(1 + np.array([15.0, z16 / 2.0, 0.0])))
    main, top = data_axes(run.fig)
    close(main.get_xlim(), [0.0, math.log10(16.0)])
    close(main.get_xticks(), [math.log10(1 + zt) for zt in (0, 1, 2, 3, 5, 7, 10, 15)])
    assert [t.get_text() for t in main.get_xticklabels()] == [
        "0",
        "1",
        "2",
        "3",
        "5",
        "7",
        "10",
        "15",
    ], "bottom tick labels"
    close(top.get_xticks(), [0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.2])
    assert [t.get_text() for t in top.get_xticklabels()] == [
        "0",
        "0.2",
        "0.4",
        "0.6",
        "0.8",
        "1",
        "1.2",
    ], "top tick labels"
    assert main.get_yscale() == "log", "y scale"


# ---- axis windows, scales and legends of the new evolution figures ----
# name -> (x window, y window, y scale); None where SAGE26 sets no window. Written from the row
# of the plan and the SAGE26 lines cited.
WINDOWS = {
    "sfr_evolution": ((8.0, 12.2), (-4.0, 3.0), "linear"),  # :829-830
    "stellar_mass_halo_mass_relation": ((10.0, 15.0), (7.0, 12.0), "linear"),  # :936-937
    "black_hole_mass_function": ((6.0, 11.0), (1e-5, 1e-1), "log"),  # :1225-1227
    "quenched_fraction_evolution": ((8.0, 12.5), (0.0, 0.8), "linear"),  # :1346-1347
    "concentration_evolution": ((0.0, 15.0), (0.0, 30.0), "linear"),  # :1526-1527
    "h2_fraction_evolution": ((7.2, 12.0), (0.0, 1.05), "linear"),  # :1714-1715
    "central_satellite_fraction": (None, (0.0, 1.05), "linear"),  # :1890-1891
    "central_satellite_fraction_massive_quiescent": (None, (0.0, 1.05), "linear"),  # :1966-1967
}


def make_window(name):
    def case():
        ax = data_axes(render(name).fig)[0]
        x_window, y_window, scale = WINDOWS[name]
        if x_window is not None:
            close(ax.get_xlim(), x_window)
        close(ax.get_ylim(), y_window)
        assert ax.get_yscale() == scale, f"{name}: y scale {ax.get_yscale()}, expected {scale}"

    case.__name__ = f"test_window__{name}"
    return case


def test_window__star_formation_efficiency_runs_from_the_age_today_down_to_0_3_gyr():
    """allresults-history.py:1789-1791: log y, x from the age of the universe at z = 0 down to
    0.3 Gyr (a reversed axis), y from 1e-6 to 1.0. For Omega_m = 0.25, Omega_lambda = 0.75, h = 0.73
    the age today is (9.778 / 0.73) x 2 / (3 sqrt(0.75)) x asinh(sqrt(0.75 / 0.25)) = 13.394 x
    1.0139 = 13.58 Gyr (the closed form of flat LCDM; the integral to z = 1000 is 0.003 Gyr
    short)."""
    ax = data_axes(render("star_formation_efficiency").fig)[0]
    xlim = ax.get_xlim()
    close([xlim[0]], [13.58], rel=2e-3)
    close([xlim[1]], [0.3])
    close(ax.get_ylim(), [1e-6, 1.0])
    assert ax.get_yscale() == "log", f"y scale {ax.get_yscale()}"


# name -> sorted legend texts the figure draws on the synthetic run (bins 3, 5, 7, 8, 9, 10, 11)
# or None when SAGE26 draws no legend
BIN_LEGEND_FIGURES = [
    ("sfr_evolution", ":825, legend :834"),
    ("stellar_mass_halo_mass_relation", ":923, legend :947"),
    ("quenched_fraction_evolution", ":1332, legend :1351"),
    ("metallicity_evolution", ":1585, legend :1599"),
    ("h2_fraction_evolution", ":1704, legend :1719"),
]


def make_bin_legend(name, source_lines):
    def case():
        legend_case(name, source_lines, POPULATED_BIN_LABELS)

    case.__name__ = f"test_legend__{name}"
    return case


def test_legend__black_hole_mass_function():
    """allresults-history.py:1187 (`z = {actual_z:.1f} (SAGE)`, the redshift of the snapshot
    drawn: 63 -> 0.0, 37 -> 1.4, 32 -> 2.1, 23 -> 4.2, 18 -> 6.2, 16 -> 7.3) and :1213
    (`z = {target_z:.1f} (Obs)` for the six targets), legend at :1237."""
    legend_case(
        "black_hole_mass_function",
        ":1187, :1213",
        [
            "z = 0.0 (SAGE)",
            "z = 1.4 (SAGE)",
            "z = 2.1 (SAGE)",
            "z = 4.2 (SAGE)",
            "z = 6.2 (SAGE)",
            "z = 7.3 (SAGE)",
            "z = 0.1 (Obs)",
            "z = 1.0 (Obs)",
            "z = 2.0 (Obs)",
            "z = 4.0 (Obs)",
            "z = 6.0 (Obs)",
            "z = 8.0 (Obs)",
        ],
    )


def test_legend__concentration_evolution():
    """allresults-history.py:1475-1480 and :1518-1519 (`label + r' $M_\\odot$'`), legend :1525."""
    legend_case(
        "concentration_evolution",
        ":1475-1480, :1519",
        [
            r"$10^{10} - 10^{11}$ $M_\odot$",
            r"$10^{11} - 10^{12}$ $M_\odot$",
            r"$10^{12} - 10^{13}$ $M_\odot$",
            r"$10^{13} - 10^{15}$ $M_\odot$",
        ],
    )


def test_legend__central_satellite_fractions():
    """allresults-history.py:1887-1888 and :1963-1964: 'Centrals' and 'Satellites'."""
    for name in ("central_satellite_fraction", "central_satellite_fraction_massive_quiescent"):
        legend_case(name, ":1887-1888 / :1963-1964", ["Centrals", "Satellites"])


def test_legend__figures_without_a_legend():
    """SAGE26 draws no legend in the sections of star_formation_efficiency (:1731-1798),
    central_satellite_counts (commented out, :1844-1846) and
    quenched_central_satellite_counts (commented out, :2025-2027)."""
    for name in (
        "star_formation_efficiency",
        "central_satellite_counts",
        "quenched_central_satellite_counts",
    ):
        assert legend_texts(name) == [], f"{name} draws a legend"


def test_legend__ffb_fraction_cut_down():
    """paper_plots.py:10385 (label 'FFB'), legend :10441."""
    legend_case("ffb_fraction_cut_down", ":10385", ["FFB"])


# ---- metallicity_evolution: the SAGE26 plotting defect mirrored as written ----
def test_figure__metallicity_evolution_draws_the_raw_metal_mass():
    """allresults-history.py:365 reads MetalsStellarMass with no x 1e10 / h and :1564 draws its
    log10. A galaxy of stellar mass 1.0 (1.37e10 Msun, log10 = 10.137, h = 0.73) with
    MetalsStellarMass 0.01 is drawn at log10(0.01) = -2, not at log10(0.01 x 1e10 / 0.73) = 8.14;
    the mass-bin centre is 10.15 (the bin 10.1 to 10.2)."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {"StellarMass": f32([1.0, 1.0]), "MetalsStellarMass": f32([0.01, 0.01])},
        )
    }
    run = run_hand_built("metallicity_evolution", spec, "metallicity-raw")
    (line,) = model_lines(run.fig)
    close(line.get_xdata(), [10.15])
    close(line.get_ydata(), [-2.0], rel=1e-6)


# ---- black_hole_mass_function: helper and observations ----
def test_unit__black_hole_mass_function_helper():
    """allresults-history.py:1170-1185: `np.histogram(np.log10(M), bins=np.arange(6.0, 11.5, 0.1))`
    over `volume * bin_width`, the bin width being `bins[1] - bins[0]` = 0.1, and a bin is drawn
    when `phi > 0`; only galaxies with `M > 0` count. Black holes of 10^6.05 (twice) and 10^6.15
    Msun and one of no mass, in a volume of 100 Mpc^3: 2 / (100 x 0.1) = 0.2 in the bin centred
    6.05 and 1 / 10 = 0.1 in the one centred 6.15; with no black hole the helper gives nothing."""
    module = figure_module("black_hole_mass_function")
    assert module.TARGET_REDSHIFTS == [0.1, 1.0, 2.0, 4.0, 6.0, 8.0], "targets"  # :1156
    masses = np.array([10**6.05, 10**6.05, 10**6.15, 0.0])
    centers, phi, valid = module.mass_function(masses, 100.0)
    close(centers[:3], [6.05, 6.15, 6.25])
    close(phi[:3], [0.2, 0.1, 0.0])
    assert valid.tolist()[:3] == [True, True, False], f"valid {valid.tolist()[:3]}"
    assert int(valid.sum()) == 2, f"{int(valid.sum())} bins drawn"
    assert module.mass_function(np.zeros(3), 100.0) is None, "no black hole"


def test_unit__black_hole_mass_function_observations_have_the_shape_of_the_files():
    """R/data/bh/fig4_bhmf_z{0.1,1.0,2.0,4.0,6.0,8.0}.txt: 66 rows each of 4 columns (log10 Mbh,
    best, 16th, 84th percentile). Their values are checked against the files by the transcription
    script, which reads SAGE26's data."""
    module = figure_module("black_hole_mass_function")
    assert sorted(module.OBS_TRINITY) == [0.1, 1.0, 2.0, 4.0, 6.0, 8.0], "redshifts"
    for z, obs in module.OBS_TRINITY.items():
        assert obs.shape == (66, 4), f"z = {z}: shape {obs.shape}"
    close(module.OBS_TRINITY[0.1][0], [4.0, 1.868892e-02, 1.607018e-02, 2.439403e-02])


# ---- the conversions and wiring each figure applies to its fields ----
# StellarMass 1.0 is 1.0 x 1e10 / 0.73 = 1.3699e10 Msun, log10 = 10.13668, in the mass bin
# 10.1 to 10.2 (centre 10.15); Mvir 10.0 is 1.3699e11 Msun, log10 = 11.13668 (bin centre 11.15).
LOG_UNIT_MASS = 10.136677  # log10(1e10 / 0.73)


def test_figure__sfr_evolution_converts_mass_and_adds_the_two_rates():
    """allresults-history.py:337 (x 1e10 / h), :799 (SfrDisk + SfrBulge), :802-803 (log10 of both).
    One galaxy of StellarMass 1.0 forming 1.0 + 1.0 Msun/yr at z = 1.39: drawn at the bin centre
    10.15, y = log10(2) = 0.30103."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {"StellarMass": f32([1.0]), "SfrDisk": f32([1.0]), "SfrBulge": f32([1.0])},
        )
    }
    (line,) = model_lines(run_hand_built("sfr_evolution", spec, "wiring-sfr").fig)
    close(line.get_xdata(), [10.15])
    close(line.get_ydata(), [0.30103], rel=1e-5)


def test_figure__stellar_mass_halo_mass_relation_converts_both_masses():
    """allresults-history.py:337 and :342 (both x 1e10 / h), :893-894 (the stellar mass stays
    linear, the halo mass is log10), :924 (log10 of the mean stellar mass). One galaxy of
    StellarMass 1.0 in a halo of Mvir 10.0 at z = 1.39: drawn at the halo-mass bin centre 11.15,
    y = 10.136677."""
    spec = {
        37: (RUN_REDSHIFTS[37], {"StellarMass": f32([1.0]), "Mvir": f32([10.0])}),
    }
    (line,) = model_lines(
        run_hand_built("stellar_mass_halo_mass_relation", spec, "wiring-smhm").fig
    )
    close(line.get_xdata(), [11.15])
    close(line.get_ydata(), [LOG_UNIT_MASS], rel=1e-6)


def test_figure__h2_fraction_evolution_converts_the_three_masses():
    """allresults-history.py:337, :345 and :357 (x 1e10 / h), :1660 (H2 / (cold x 0.74)). One
    galaxy with StellarMass 1.0, ColdGas 1.0 and H2gas 0.37 at z = 1.39: drawn at 10.15, with the
    fraction 0.37 / 0.74 = 0.5."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {"StellarMass": f32([1.0]), "ColdGas": f32([1.0]), "H2gas": f32([0.37])},
        )
    }
    (line,) = model_lines(run_hand_built("h2_fraction_evolution", spec, "wiring-h2").fig)
    close(line.get_xdata(), [10.15])
    close(line.get_ydata(), [0.5], rel=1e-6)


def test_figure__concentration_evolution_converts_the_halo_mass_and_is_drawn_at_the_redshift():
    """allresults-history.py:342 (x 1e10 / h), :1498 (the redshift of the snapshot), :1501-1503.
    Five haloes of Mvir 8.0 at z = 0, with concentrations 1 to 5: 8.0 x 1e10 / 0.73 = 1.096e11 Msun
    is in the bin 1e11 to 1e12 (without the division by h it would be 8e10 Msun, in the bin 1e10 to
    1e11). One line, labelled for the 1e11 bin, at the median 3."""
    spec = {
        63: (
            0.0,
            {"Mvir": f32([8.0] * 5), "Concentration": f32([1, 2, 3, 4, 5])},
        )
    }
    run = run_hand_built("concentration_evolution", spec, "wiring-concentration")
    (line,) = model_lines(run.fig)
    assert line.get_label() == r"$10^{11} - 10^{12}$ $M_\odot$", f"label {line.get_label()}"
    close(line.get_xdata(), [0.0])
    close(line.get_ydata(), [3.0])


def test_figure__black_hole_mass_function_converts_mass_and_volume():
    """allresults-history.py:340 (x 1e10 / h), :305 (volume = (BoxSize / h)^3, so the engine's
    volume in (Mpc/h)^3 over h^3), :1181-1182 (counts over volume x bin width). Two black holes
    of 10^6.55 Msun (BlackHoleMass 10^6.55 x 0.73 / 1e10 = 2.6e-4) in the only snapshot, which
    every one of the six targets selects, fall in the bin 6.5 to 6.6 (centre 6.55); the volume is
    62.5^3 / 0.73^3 = 627,566 Mpc^3, so phi = 2 / (627,566 x 0.1) = 3.187e-5 on each of the six
    lines."""
    code_mass = 10**6.55 * 0.73 / 1.0e10
    spec = {63: (0.0, {"BlackHoleMass": f32([code_mass, code_mass])})}
    lines = model_lines(run_hand_built("black_hole_mass_function", spec, "wiring-bhmf").fig)
    assert len(lines) == 6, f"{len(lines)} model lines"
    for line in lines:
        close(line.get_xdata(), [6.55])
        close(line.get_ydata(), [3.187e-5], rel=1e-3)


def test_figure__star_formation_efficiency_converts_masses_and_ages_the_snapshot():
    """allresults-history.py:337 and :342, :1766 (M_* / (M_vir x f_b)), :1767 (the age at the
    snapshot's redshift). Five centrals of StellarMass 1.0 in haloes of Mvir 10.0 at z = 1.39:
    1.0 / (10.0 x 0.17) = 0.58824, at the age of a flat universe with Omega_m = 0.25 and h = 0.73
    at z = 1.386: (9.778 / 0.73) x 2 / (3 sqrt(0.75)) x asinh(sqrt(3) / 2.386^1.5) = 4.68 Gyr."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {
                "StellarMass": f32([1.0] * 5),
                "Mvir": f32([10.0] * 5),
                "Type": np.zeros(5, dtype=np.int32),
            },
        )
    }
    line = model_lines(run_hand_built("star_formation_efficiency", spec, "wiring-sfe").fig)[0]
    close(line.get_ydata(), [0.58824], rel=1e-4)
    close(line.get_xdata(), [4.68], rel=1e-2)


def test_figure__quenched_counts_convert_the_mass_and_add_the_two_rates():
    """allresults-history.py:337, :2004 (SfrDisk + SfrBulge), :2006-2009. Two centrals of 1.37e10
    Msun (StellarMass 1.0): one forming 0.05 + 0.05 = 0.1 Msun/yr (7.3e-12 per year, quenched)
    and one forming 0.09 + 0.09 = 0.18 (1.3e-11, not): one quenched central, no quenched
    satellite. Taking only the disk rate would quench both."""
    spec = {
        37: (
            RUN_REDSHIFTS[37],
            {
                "StellarMass": f32([1.0, 1.0]),
                "SfrDisk": f32([0.05, 0.09]),
                "SfrBulge": f32([0.05, 0.09]),
                "Type": np.array([0, 0], dtype=np.int32),
            },
        )
    }
    run = run_hand_built("quenched_central_satellite_counts", spec, "wiring-qcounts")
    bars = sorted(
        (a.get_x() + a.get_width() / 2, a.get_height()) for a, _ in model_artists(run.fig)
    )
    close([b[0] for b in bars], [2.85, 3.15])
    close([b[1] for b in bars], [1.0, 0.0])


def test_figure__massive_quiescent_converts_the_mass_and_uses_the_run_cosmology():
    """allresults-history.py:337, :1922, :1938-1942, :1947-1952. Ten galaxies of 1.37e10 Msun
    (StellarMass 1.0, every one among the most massive) forming 0.09 + 0.09 = 0.18 Msun/yr
    (1.31e-11 per year; 1.8e-11 if the mass were not divided by h), seven central and three
    satellite, at z = 0 and z = 1.39 with Omega_m = 0.25, Omega_lambda = 0.75, h = 0.73: the
    quiescence limit 0.2 E(z) h / 9.778e9 is 1.49e-11 at z = 0 and 3.0e-11 at z = 1.39, so all ten
    are quiescent at both: fractions 0.7 and 0.3 at both redshifts (the unconverted mass would
    leave no quiescent galaxy at z = 0 and so no point there)."""
    galaxies = {
        "StellarMass": f32([1.0] * 10),
        "SfrDisk": f32([0.09] * 10),
        "SfrBulge": f32([0.09] * 10),
        "Type": np.array([0] * 7 + [1] * 3, dtype=np.int32),
    }
    spec = {63: (0.0, galaxies), 37: (RUN_REDSHIFTS[37], galaxies)}
    run = run_hand_built("central_satellite_fraction_massive_quiescent", spec, "wiring-mq")
    centrals, satellites = model_lines(run.fig)
    close(centrals.get_xdata(), [0.0, RUN_REDSHIFTS[37]])
    close(centrals.get_ydata(), [0.7, 0.7])
    close(satellites.get_ydata(), [0.3, 0.3])


BIN_CASES = [
    test_bins__bulge_to_total_constants,
    test_bins__specific_star_formation_rate_constants,
    test_bins__ejection_efficiency_edges,
    test_bins__disk_radius_edges,
    test_threshold__specific_star_formation_rate_population_minimum_is_10,
    test_threshold__spatial_distribution_3d_box_cut_is_1e9_msun,
    test_threshold__star_formation_rate_cut_is_0_01_msun,
    test_threshold__concentration_vs_mvir_cut_is_1e8_msun,
    test_threshold__disk_radius_window_is_9_3_to_11_3,
    test_legend__bulge_to_total,
    test_legend__stellar_vs_halo_mass_by_regime,
    test_legend__specific_star_formation_rate,
    test_legend__sfr_vs_halo_accretion_rate,
    test_legend__tcool_vs_mvir,
    test_legend__ejection_efficiency_vs_vvir_has_none,
    test_legend__disk_radius_vs_stellar_mass,
    # Slice 3
    test_selection__z_bins_are_the_fifteen_of_sage26,
    test_selection__z_range_includes_both_ends,
    test_selection__empty_bin_returns_no_snapshot,
    test_selection__eight_snapshots_fall_in_the_literal_bins,
    test_selection__nearest_snapshot_for_the_targets_of_the_plan,
    test_selection__nearest_tie_returns_the_lower_snapshot_number,
    test_selection__results_do_not_depend_on_the_order_snapshots_are_supplied_in,
    test_unit__arange_constants_of_the_new_bins,
    test_legend__rcool_to_rvir_above_mshock,
    test_unit__sfr_evolution_helpers,
    test_unit__stellar_mass_halo_mass_relation_line_styles,
    test_figure__stellar_mass_halo_mass_relation_line_styles_follow_the_bin_index,
    test_figure__quenched_fraction_orphans_are_in_neither_curve,
    test_figure__central_satellite_counts_ticks_windows_and_no_legend,
    test_figure__quenched_central_satellite_counts_ticks_windows_and_no_legend,
    test_figure__central_satellite_counts_bars_are_the_mean_counts,
    test_figure__central_satellite_fraction_window_ends_at_the_largest_redshift,
    test_figure__cosmology_missing_from_the_run_is_a_skip_with_a_message,
    test_figure__ffb_fraction_step_values_and_axes,
    test_window__star_formation_efficiency_runs_from_the_age_today_down_to_0_3_gyr,
    test_legend__black_hole_mass_function,
    test_legend__concentration_evolution,
    test_legend__central_satellite_fractions,
    test_legend__figures_without_a_legend,
    test_legend__ffb_fraction_cut_down,
    test_figure__metallicity_evolution_draws_the_raw_metal_mass,
    test_figure__sfr_evolution_converts_mass_and_adds_the_two_rates,
    test_figure__stellar_mass_halo_mass_relation_converts_both_masses,
    test_figure__h2_fraction_evolution_converts_the_three_masses,
    test_figure__concentration_evolution_converts_the_halo_mass_and_is_drawn_at_the_redshift,
    test_figure__black_hole_mass_function_converts_mass_and_volume,
    test_figure__star_formation_efficiency_converts_masses_and_ages_the_snapshot,
    test_figure__quenched_counts_convert_the_mass_and_add_the_two_rates,
    test_figure__massive_quiescent_converts_the_mass_and_uses_the_run_cosmology,
]


UNIT_CASES = [
    test_unit__bulge_to_total_ratios,
    test_unit__bulge_to_total_bins,
    test_unit__box_edges,
    test_unit__total_sfr,
    test_unit__log_specific_sfr,
    test_unit__smoothed_histogram,
    test_unit__sigma_levels,
    test_unit__halo_accretion_rate,
    test_unit__virial_temperature,
    test_unit__cgm_fraction,
    test_unit__ejection_efficiency,
    test_unit__ejection_binned_median_and_std,
    test_unit__disk_radius_binned_median_and_std,
    test_unit__effective_disk_radius,
    test_unit__virial_radius_kpc,
    test_unit__moffett_imf_shift_is_zero,
    # Slice 3
    test_unit__rcool_helpers,
    test_unit__metallicity_evolution_helpers,
    test_unit__h2_fraction_evolution_helpers,
    test_unit__stellar_mass_halo_mass_relation_helpers,
    test_unit__quenched_fraction_helpers,
    test_unit__concentration_statistics,
    test_unit__star_formation_efficiency_helpers,
    test_unit__age_of_the_universe,
    test_unit__central_satellite_counts_helper,
    test_unit__quenched_counts_helper,
    test_unit__central_satellite_fractions_helper,
    test_unit__ssfr_threshold,
    test_unit__massive_quiescent_counts,
    test_unit__ffb_fraction_helper,
    test_unit__ffb_step_edges_for_the_written_snapshots,
    test_unit__black_hole_mass_function_helper,
    test_unit__black_hole_mass_function_observations_have_the_shape_of_the_files,
]


def build_cases():
    """Return every case: five per registered figure, the helper and added-series cases, the
    tcool colour-bar cases, a Cap case per sampled figure, a window case per windowed history
    figure, a legend case per redshift-binned figure, a unit case per new helper, and the
    selection, bin, threshold and legend cases of the new figures."""
    cases = []
    for name in FILE_STEMS:
        cases += [
            make_renders(name),
            make_file_stem(name),
            make_model_content(name),
            make_labels(name),
            make_requirements(name),
        ]
    cases += [
        test_reservoir_helper_returns_seven_reservoirs_and_their_exact_sum,
        test_added_series__baryon_fraction_draws_a_cgm_curve,
        test_added_series__mass_reservoir_scatter_draws_a_cgm_gas_series,
        test_added_series__gas_mass_function_draws_the_h2_line,
        test_added_series__gas_mass_function_draws_the_hi_line,
        test_colourbar_axes_are_identified_and_not_counted_as_data_axes,
        test_labels__tcool_vs_mvir_with_a_regime_outside_0_and_1_has_the_colour_bar,
        test_labels__tcool_vs_mvir_with_only_regimes_0_and_1_has_no_colour_bar,
    ]
    cases += [make_cap(name) for name in CAPS]
    cases += [make_window(name) for name in WINDOWS]
    cases += [make_bin_legend(name, lines) for name, lines in BIN_LEGEND_FIGURES]
    cases += UNIT_CASES
    cases += BIN_CASES
    return cases


def main():
    """Run this file's tests via the shared framework runner, in a temporary directory."""
    global TMP_ROOT
    TMP_ROOT = tempfile.mkdtemp(prefix="sage26-figures-test-")
    try:
        return run_test_suite(build_cases(), "sage26 figures (test_sage26_figures.py)")
    finally:
        shutil.rmtree(TMP_ROOT, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
