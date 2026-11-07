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
           no-colour-bar labels of tcool_vs_mvir)
Tolerance: none for labels, stems, field sets, caps and the reservoir helper (the helper's
           literals are dyadic, so the sums are exact); the CGM curve is compared with the total
           curve by a plain inequality; helper unit cases compare with a relative tolerance of
           1e-12 (decimal literals) or exactly (dyadic literals), and the smoothed-histogram
           peak with an absolute tolerance of 2e-3

Every expectation is written from the plan of the c16 plotting node (Slices 1 and 2), the
figure's source text at the starting commit (66a3e949, transcribed: no label is computed from the
code under test) or SAGE26's plotting script (plotting/allresults-local.py, lines cited at each
use).
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

# Registry table of the c16 plan, Slice 2: registry name -> File stem (rows S1-S30, E1-E4)
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
    from matplotlib.patches import Rectangle

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
    kwargs = {"params": {}, "output_dir": out_dir, "output_format": ".png", "verbose": False}
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
]


def build_cases():
    """Return every case: five per registered figure, the helper and added-series cases, the
    tcool colour-bar cases, a Cap case per sampled figure, a unit case per new helper, and the bin,
    threshold and legend cases of the new figures."""
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
