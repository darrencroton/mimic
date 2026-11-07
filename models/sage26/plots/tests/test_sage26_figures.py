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
Tolerance: none for labels, stems, field sets and the reservoir helper (the helper's literals
           are dyadic, so the sums are exact); the CGM curve is compared with the total curve
           by a plain inequality

Every expectation is written from the plan of the c16 plotting node (Slice 1), the figure's
source text at the starting commit (66a3e949, transcribed: no label is computed from the code
under test) or SAGE26's plotting script (plotting/allresults-local.py, lines cited at each use).
Figures render with MPLBACKEND=Agg into a temporary directory under $TMPDIR, removed at the end.
It needs no Mimic run.

Run: mimic_venv/bin/python models/sage26/plots/tests/test_sage26_figures.py
"""

import importlib
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

# Registry table of the c16 plan, Slice 1: registry name -> File stem (rows S1-S18, E1-E4)
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


def panel(x_label, y_label, title=""):
    """Return the expected labels of one data axes (no z label, no side titles)."""
    return {
        "x": x_label,
        "y": y_label,
        "z": "",
        "title": title,
        "title_left": "",
        "title_right": "",
    }


# name -> list of expected data axes in creation order. No figure has a title or a colour bar.
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
    "hmf_evolution": [panel(HALO_MASS, MASS_FUNCTION)],
    "smf_evolution": [panel(STELLAR_MASS, MASS_FUNCTION)],
    "sfr_density_evolution": [
        panel(REDSHIFT, r"log$_{10}$ SFR density [M$_{\odot}$ yr$^{-1}$ Mpc$^{-3}$]")
    ],
    "stellar_mass_density_evolution": [
        panel(REDSHIFT, r"log$_{10}$ $\rho_{*}$ [M$_{\odot}$ Mpc$^{-3}$]")
    ],
}
# name -> expected colour-bar labels in creation order; no registered figure draws a colour bar
COLORBAR_LABELS = {name: [] for name in FILE_STEMS}

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
    """The CGM mean curve (SAGE26 plotting/allresults-local.py:1325, label 'Circumgalactic Medium')."""
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


def build_cases():
    """Return every case: five per registered figure, then the helper and added-series cases."""
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
    ]
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
