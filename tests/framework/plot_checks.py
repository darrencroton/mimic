"""Shared, model-neutral harness for the per-package figure tests (``models/<model>/plots/tests/``).

A package's figures are plain functions called with a galaxy record, so they are tested without a
Mimic run: the harness imports the package's ``figures`` registry, builds the input from the
package's own synthetic catalogue (a "fixtures" module, below) and calls every registered figure
directly with ``MPLBACKEND=Agg``. Each ``models/<model>/plots/tests/test_<model>_figures.py`` is a
thin file: a ``PlotHarness`` naming the package, its fixtures and whether its figures tag their
model artists, then ``harness.run(cases, title)`` with the package's own cases added to the shared
ones. Nothing here names a model, a figure, a galaxy property or a simulation: those, and the
expected answers of the derivation cases, belong to the package.

Shared cases, built by ``PlotHarness.registry_cases()``, per registered figure:

1. ``test_renders__<figure>`` -- the figure renders on the synthetic catalogue, returns no skip
   message and writes a non-empty PNG.
2. ``test_model_content__<figure>`` -- the rendered figure holds data: at least one artist tagged
   ``gid="model"`` has a finite point. Only for a package whose figures tag their model artists
   (``model_gid=True``); an untagged figure cannot be told apart from its axes background or an
   observational overlay, so such a package gets no such case.
3. ``test_requirements__<figure>`` -- every field the figure reads unconditionally is in its
   ``PLOT_REQUIREMENTS`` entry (the engine skips a figure whose entry names an absent field), an
   evolution figure's reads are in its ``EVOLUTION_PLOT_FIELDS`` entry, and calling the figure
   without any one of its optional fields does not raise.

and, from ``PlotHarness.structure_cases()``, the registry contract: no duplicate name and none in
both lists; ``PLOT_FUNCS`` and ``PLOT_REQUIREMENTS`` hold exactly the registry's names; every
evolution figure's field list includes its requirements; every field either table names is a
declared output property; the model's default profile lists the registry in order; and the
profile stack the engine builds for each run file lists the registry and agrees on its axes.

A package's fixtures module (``models/<model>/plots/tests/<model>_plot_fixtures.py``) provides
``build_catalogue()`` (the full synthetic record of one snapshot),
``build_evolution_input(fields)``, ``run_parameters()``, ``record_dtype()``,
``snapshot_metadata(redshift, ngals)``, ``property_files()`` (the (file, key) pairs declaring its
output properties), ``HALO_PROPERTIES`` (the simulation's halo property file) and ``VOLUME``.
The pieces every such module needs (reading the output properties from the property YAMLs, the
metadata dictionary the engine passes, the evolution input) are here, parameterised by the
simulation's cosmology and scale-factor list, which the fixtures supply.
"""

import importlib
import importlib.util
import os
import shutil
import sys
import tempfile
from collections import namedtuple
from pathlib import Path

# Set before Matplotlib is imported (the figure package imports it)
os.environ["MPLBACKEND"] = "Agg"

import matplotlib.pyplot as plt
import numpy as np
import yaml
from numpy.lib import recfunctions

from .runner import run_test_suite

REPO_ROOT = Path(__file__).resolve().parents[2]
MIMIC_PLOT_DIR = REPO_ROOT / "plot" / "mimic-plot"

CORE_PROPERTIES = REPO_ROOT / "src" / "core" / "core_properties.yaml"

_DTYPES = {
    "int": np.int32,
    "long long": np.int64,
    "float": np.float32,
    "double": np.float64,
    "vec3_float": (np.float32, (3,)),
    "vec3_double": (np.float64, (3,)),
}


# --------------------------------------------------------------------------------------
# Fixture pieces shared by the packages' synthetic catalogues
# --------------------------------------------------------------------------------------
def output_property_declarations(paths_and_keys):
    """Return {name: type string} for every ``output: true`` property of the given YAML files.

    ``paths_and_keys`` is a sequence of (file, top-level key) pairs.
    """
    declared = {}
    for path, key in paths_and_keys:
        with open(path) as handle:
            entries = yaml.safe_load(handle)[key]
        for entry in entries:
            if entry.get("output") is True:
                declared[entry["name"]] = entry["type"]
    return declared


def package_property_files(model, halo_properties):
    """Return the (file, key) pairs declaring a model's output properties on one simulation.

    ``halo_properties`` is the simulation's halo_properties.yaml.
    """
    return (
        (CORE_PROPERTIES, "halo_properties"),
        (halo_properties, "halo_properties"),
        (REPO_ROOT / "models" / model / "model_properties.yaml", "galaxy_properties"),
    )


def record_dtype_of(property_files):
    """Return the numpy dtype with one field per output property of the given files.

    A ``vec3_*`` property is a 3-element field. A renamed or removed property changes the dtype,
    so a figure that reads the old name fails loudly.
    """
    declared = output_property_declarations(property_files)
    return np.dtype([(name, _DTYPES[ptype]) for name, ptype in declared.items()])


def scale_factor_redshifts(a_list, snapshots):
    """Return {snapshot: redshift} for ``snapshots``, from a simulation's scale-factor list file."""
    scale_factors = [float(line) for line in Path(a_list).read_text().split()]
    return {snap: 1.0 / scale_factors[snap] - 1.0 for snap in snapshots}


def snapshot_metadata(hubble_h, box_size, redshift=0.0, ngals=0):
    """Return the metadata dict the plotting engine passes to a figure for one snapshot."""
    return {
        "hubble_h": hubble_h,
        "box_size": box_size,
        "volume": box_size**3,
        "ntrees": 0,
        "ngals": ngals,
        "good_files": 1,
        "schema_units": {},
        "redshift": redshift,
    }


# The full catalogue of each (builder, seed, redshift), built once: an evolution input is asked for
# by every case of every evolution figure, with the same snapshots each time
_FULL_CATALOGUES = {}


def evolution_input(build_catalogue, fields, redshifts, volume, make_metadata, seed):
    """Return the ``snapshots`` argument of an evolution figure.

    The engine reads only the fields an evolution figure lists in ``EVOLUTION_PLOT_FIELDS`` for
    each written snapshot and passes ``{snapshot: (galaxies, volume, metadata)}`` with the
    snapshots in increasing redshift, as the redshift mapper orders them. This builds that input
    for the snapshots of ``redshifts``, each holding only ``fields``.

    Args:
        build_catalogue: ``build_catalogue(seed, epoch=...)`` of the package's fixtures.
        fields: Field names the record holds (the figure's ``EVOLUTION_PLOT_FIELDS`` entry).
        redshifts: {snapshot: redshift} of the snapshots to build.
        volume: Volume of the simulation box, (Mpc/h)^3.
        make_metadata: ``make_metadata(redshift, ngals)``, the package's ``snapshot_metadata``.
        seed: Seed of the generator; each snapshot derives its own from it.
    """
    snapshots = {}
    for index, snap in enumerate(sorted(redshifts, key=lambda s: redshifts[s])):
        key = (build_catalogue, seed + index, redshifts[snap])
        if key not in _FULL_CATALOGUES:
            _FULL_CATALOGUES[key] = build_catalogue(seed + index, epoch=redshifts[snap])
        full = _FULL_CATALOGUES[key]  # never modified: the fields taken below are a copy
        galaxies = recfunctions.repack_fields(full[list(fields)]).view(np.recarray)
        snapshots[snap] = (galaxies, volume, make_metadata(redshifts[snap], len(galaxies)))
    return snapshots


def load_figure_package(model):
    """Import models/<model>/plots/figures as `figures`, replacing any earlier package."""
    if str(MIMIC_PLOT_DIR) not in sys.path:
        sys.path.insert(0, str(MIMIC_PLOT_DIR))
    for name in [m for m in sys.modules if m == "figures" or m.startswith("figures.")]:
        del sys.modules[name]
    plots_dir = str(REPO_ROOT / "models" / model / "plots")
    sys.path.insert(0, plots_dir)
    try:
        return importlib.import_module("figures")
    finally:
        sys.path.remove(plots_dir)


def load_engine():
    """Import plot/mimic-plot/mimic-plot.py, whose filename is not importable by name."""
    spec = importlib.util.spec_from_file_location(
        "mimic_plot_engine_under_test", MIMIC_PLOT_DIR / "mimic-plot.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# --------------------------------------------------------------------------------------
# Reading a captured figure
# --------------------------------------------------------------------------------------
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
    if isinstance(artist, (PolyCollection, ContourSet)):
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


def tagged_model_artists(fig):
    """Return [(artist, finite_points)] for every artist tagged gid="model" in a figure."""
    return [
        (artist, finite_points(artist))
        for ax in fig.axes
        for artist in ax.get_children()
        if artist.get_gid() == "model"
    ]


def model_lines(fig):
    """Return the Line2D artists tagged gid="model", in drawing order."""
    from matplotlib.lines import Line2D

    return [a for a, _ in tagged_model_artists(fig) if isinstance(a, Line2D)]


# The outcome of one figure call: its return value (path, skip message) and the figure it drew
Run = namedtuple("Run", ["path", "skip", "fig"])


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


# --------------------------------------------------------------------------------------
# The harness
# --------------------------------------------------------------------------------------
class PlotHarness:
    """Runs one package's registered figures on its synthetic catalogue.

    Args:
        model: Package name (``models/<model>/``).
        fixtures: The package's fixtures module (see the module docstring).
        model_gid: True when the package's figures tag their model artists ``gid="model"``;
            only then is there a model-content case (see the module docstring).
    """

    def __init__(self, model, fixtures, model_gid):
        self.model = model
        self.fixtures = fixtures
        self.model_gid = model_gid
        self.figures = load_figure_package(model)
        self.snapshot_names = list(self.figures.SNAPSHOT_PLOTS)
        self.evolution_names = list(self.figures.EVOLUTION_PLOTS)
        self.names = self.snapshot_names + self.evolution_names
        self.tmp_root = None  # created by run(), removed at its end
        self._cache = {}
        self._catalogue = None

    # ---- locating a figure -------------------------------------------------------------
    def is_evolution(self, name):
        return name in self.evolution_names

    def figure_module(self, name):
        """Return the module that defines a registered figure's plot() function."""
        return sys.modules[self.figures.PLOT_FUNCS[name].__module__]

    def evolution_fields(self, name):
        """The fields the engine reads for an evolution figure (its EVOLUTION_PLOT_FIELDS)."""
        return list(self.figures.EVOLUTION_PLOT_FIELDS[name])

    def output_dir(self, label):
        """A fresh output directory for one figure call, under the run's temporary root."""
        return os.path.join(self.tmp_root, label)

    # ---- calling a figure --------------------------------------------------------------
    def call_figure(
        self, name, galaxies, label, capture=True, params=None, volume=None, metadata=None
    ):
        """Call a registered figure on the given input; return a Run.

        ``galaxies`` is the snapshot record for a snapshot figure, or the ``snapshots`` mapping
        for an evolution figure. ``params``, ``volume`` and ``metadata`` replace the fixtures'
        run parameters, volume and snapshot metadata (snapshot figures; an evolution figure
        carries its own volume and metadata per snapshot). save_and_close_figure is wrapped
        inside the module the figure calls it from, so the figure is recorded before it is
        closed; the engine is not changed.
        """
        module = self.figure_module(name)
        captured = {}
        original = module.save_and_close_figure

        def capturing_save(fig, *args, **kwargs):
            captured["fig"] = fig
            return original(fig, *args, **kwargs)

        kwargs = {
            "params": params if params is not None else self.fixtures.run_parameters(),
            "output_dir": self.output_dir(label),
            "output_format": ".png",
            "verbose": False,
        }
        if self.is_evolution(name):
            kwargs["snapshots"] = galaxies
        else:
            kwargs.update(
                galaxies=galaxies,
                volume=self.fixtures.VOLUME if volume is None else volume,
                metadata=self.fixtures.snapshot_metadata() if metadata is None else metadata,
            )
        module.save_and_close_figure = capturing_save if capture else original
        try:
            path, skip = self.figures.PLOT_FUNCS[name](**kwargs)
        finally:
            module.save_and_close_figure = original
            plt.close("all")
        return Run(path, skip, captured.get("fig"))

    def snapshot_catalogue(self):
        """A copy of the package's synthetic snapshot catalogue (built once)."""
        if self._catalogue is None:
            self._catalogue = self.fixtures.build_catalogue()
        return self._catalogue.copy().view(np.recarray)

    def default_input(self, name):
        """The synthetic input the generic cases give a figure."""
        if self.is_evolution(name):
            return self.fixtures.build_evolution_input(self.evolution_fields(name))
        return self.snapshot_catalogue()

    def render(self, name):
        """Render a registered figure once from the synthetic input and cache the Run."""
        if name not in self._cache:
            self._cache[name] = self.call_figure(name, self.default_input(name), name)
        return self._cache[name]

    # ---- hand-built input --------------------------------------------------------------
    def hand_built(self, spec, volume=None, hubble_h=None):
        """Return the ``snapshots`` argument of an evolution figure from a literal specification.

        ``spec`` maps a snapshot number to (redshift, {field: values}); the snapshots are
        ordered by increasing redshift, as the engine orders them. ``volume`` and ``hubble_h``
        replace the fixtures' values.
        """
        snapshots = {}
        for snap, (redshift, arrays) in sorted(spec.items(), key=lambda item: item[1][0]):
            n = len(next(iter(arrays.values())))
            record = np.zeros(n, dtype=[(name, np.asarray(v).dtype) for name, v in arrays.items()])
            record = record.view(np.recarray)
            for name, values in arrays.items():
                record[name] = values
            metadata = self.fixtures.snapshot_metadata(redshift, n)
            if hubble_h is not None:
                metadata["hubble_h"] = hubble_h
            snapshots[snap] = (record, self.fixtures.VOLUME if volume is None else volume, metadata)
        return snapshots

    def run_hand_built(self, name, spec, label):
        """Call an evolution figure on a hand-built input and return its Run (never a skip)."""
        run = self.call_figure(name, self.hand_built(spec), label)
        assert run.skip is None, f"{name} returned a skip message: {run.skip}"
        assert run.fig is not None, f"{name}: no figure was captured"
        return run

    # ---- recording the fields a figure reads ---------------------------------------------
    def full_input(self, name):
        """Return the input carrying every output property, for a snapshot or evolution figure."""
        if self.is_evolution(name):
            return self.fixtures.build_evolution_input(self.fixtures.record_dtype().names)
        return self.snapshot_catalogue()

    def _wrap_input(self, name, data, reads):
        if self.is_evolution(name):
            return {
                snap: (RecordingCatalogue(galaxies, reads), volume, metadata)
                for snap, (galaxies, volume, metadata) in data.items()
            }
        return RecordingCatalogue(data, reads)

    def record_reads(self, name):
        """Run a figure once on a recording catalogue.

        Returns (reads, calls): every field read, and the (required_fields, optional_fields) of
        each check_required_fields call as the figure module made it. The wrapped check unwraps
        the catalogue, so the check's own look at the field names is not a read.
        """
        module = self.figure_module(name)
        original = module.check_required_fields
        reads, calls = set(), []

        def recording_check(galaxies, required_fields, optional_fields=None, plot_name="Plot"):
            calls.append((list(required_fields), list(optional_fields or [])))
            record = getattr(galaxies, "_record", galaxies)
            return original(record, required_fields, optional_fields, plot_name)

        module.check_required_fields = recording_check
        try:
            wrapped = self._wrap_input(name, self.full_input(name), reads)
            self.call_figure(name, wrapped, name + "-reads", capture=False)
        finally:
            module.check_required_fields = original
        return reads, calls

    # ---- the generic cases, one set per registered figure -------------------------------------
    def _renders(self, name):
        def case():
            run = self.render(name)
            assert run.skip is None, f"{name} returned a skip message: {run.skip}"
            assert run.path, f"{name} returned no path"
            path = Path(run.path)
            assert path.suffix == ".png", f"{name}: suffix of {run.path}"
            assert path.is_file(), f"{name}: no file at {path}"
            assert path.stat().st_size > 0, f"{name}: empty file {path}"
            with open(path, "rb") as handle:
                assert handle.read(8) == b"\x89PNG\r\n\x1a\n", f"{name}: {path} is not a PNG"

        case.__name__ = f"test_renders__{name}"
        return case

    def _model_content(self, name):
        def case():
            run = self.render(name)
            assert run.fig is not None, f"{name}: no figure was captured"
            found = tagged_model_artists(run.fig)
            assert found, f"{name}: no artist tagged gid='model'"
            assert any(
                points > 0 for _, points in found
            ), f"{name}: {len(found)} artist(s) tagged gid='model', none holds a finite data point"

        case.__name__ = f"test_model_content__{name}"
        return case

    def _requirements(self, name):
        def case():
            reads, calls = self.record_reads(name)
            assert reads, f"{name}: the recording run read no field"
            required, optional = set(), set()
            for required_fields, optional_fields in calls:
                required |= set(required_fields)
                optional |= set(optional_fields)
            mandatory = reads - optional
            registered = set(self.figures.PLOT_REQUIREMENTS[name])
            # A mandatory field belongs in the entry whether its absence makes the figure raise
            # or return a skip: the engine skips a figure whose entry names an absent field.
            assert mandatory <= registered, (
                f"{name}: reads {sorted(mandatory - registered)} unconditionally, "
                f"not in PLOT_REQUIREMENTS {sorted(registered)}"
            )
            assert required <= registered, (
                f"{name}: passes required_fields {sorted(required - registered)} "
                f"not in PLOT_REQUIREMENTS {sorted(registered)}"
            )
            if self.is_evolution(name):
                listed = set(self.figures.EVOLUTION_PLOT_FIELDS[name])
                assert reads <= listed, (
                    f"{name}: reads {sorted(reads - listed)} not in EVOLUTION_PLOT_FIELDS "
                    f"{sorted(listed)}"
                )
            for field in sorted(optional):
                data = self.full_input(name)
                if self.is_evolution(name):
                    data = {snap: (drop_field(g, field), v, m) for snap, (g, v, m) in data.items()}
                else:
                    data = drop_field(data, field)
                try:
                    self.call_figure(name, data, f"{name}-without-{field}", capture=False)
                except Exception as exc:
                    raise AssertionError(
                        f"{name}: raised {type(exc).__name__} without optional field {field}: {exc}"
                    ) from exc

        case.__name__ = f"test_requirements__{name}"
        return case

    def registry_cases(self):
        """Return the generic cases of every registered figure, in registry order.

        The model-content case is made only for a package whose figures tag their model artists.
        """
        cases = []
        for name in self.names:
            cases.append(self._renders(name))
            if self.model_gid:
                cases.append(self._model_content(name))
            cases.append(self._requirements(name))
        return cases

    # ---- the registry contract ---------------------------------------------------------------
    def structure_cases(self, run_files, horizontal_halo_properties=None, require_axes=True):
        """Return the registry-contract cases (see the module docstring).

        Args:
            run_files: {driver: run file} whose merged profile stack must list the registry and
                agree across drivers (for example the vertical and the horizontal run file).
            horizontal_halo_properties: halo_properties.yaml of the horizontal package; when
                given, a halo property a figure names must also be one of its output properties.
            require_axes: Whether the stack must carry an ``axes`` section.
        """
        figures = self.figures
        model_dir = REPO_ROOT / "models" / self.model
        profile = model_dir / "plots" / "profiles" / "default.yaml"

        def test_registry_lists_have_no_duplicates_and_no_name_in_both():
            """A name appears once, and a figure is either a snapshot or an evolution figure."""
            snapshot, evolution = figures.SNAPSHOT_PLOTS, figures.EVOLUTION_PLOTS
            assert len(snapshot) == len(set(snapshot)), "duplicate name in SNAPSHOT_PLOTS"
            assert len(evolution) == len(set(evolution)), "duplicate name in EVOLUTION_PLOTS"
            both = set(snapshot) & set(evolution)
            assert not both, f"in both lists: {sorted(both)}"

        def test_plot_funcs_and_requirements_have_exactly_the_registry_names():
            """PLOT_FUNCS and PLOT_REQUIREMENTS hold one entry per registered figure, no other."""
            registry = set(self.names)
            for table in ("PLOT_FUNCS", "PLOT_REQUIREMENTS"):
                names = set(getattr(figures, table))
                assert names == registry, (
                    f"{table}: extra {sorted(names - registry)}, "
                    f"missing {sorted(registry - names)}"
                )
            assert all(callable(func) for func in figures.PLOT_FUNCS.values()), "a PLOT_FUNCS entry"

        def test_every_evolution_figure_has_fields_that_include_its_requirements():
            """EVOLUTION_PLOT_FIELDS has an entry per evolution figure, a superset of its needs."""
            listed = set(figures.EVOLUTION_PLOT_FIELDS)
            assert listed == set(self.evolution_names), f"EVOLUTION_PLOT_FIELDS names: {listed}"
            for name in self.evolution_names:
                missing = set(figures.PLOT_REQUIREMENTS[name]) - set(
                    figures.EVOLUTION_PLOT_FIELDS[name]
                )
                assert (
                    not missing
                ), f"{name}: EVOLUTION_PLOT_FIELDS lacks required {sorted(missing)}"

        def test_every_registry_field_is_a_declared_output_property():
            """Each field a requirement names is declared with `output: true` in a property file.

            A renamed or removed output property would make the engine silently skip the figure.
            With a horizontal package given, a halo property must also be one of that package's
            output properties ("register only figures whose required properties the model
            declares", the developer guide).
            """
            files = self.fixtures.property_files()
            declared = set(output_property_declarations(files))
            named = set()
            for fields in figures.PLOT_REQUIREMENTS.values():
                named |= set(fields)
            for fields in figures.EVOLUTION_PLOT_FIELDS.values():
                named |= set(fields)
            undeclared = named - declared
            assert not undeclared, f"not declared output properties: {sorted(undeclared)}"
            if horizontal_halo_properties is not None:
                halo = set(
                    output_property_declarations(
                        [(self.fixtures.HALO_PROPERTIES, "halo_properties")]
                    )
                )
                horizontal = set(
                    output_property_declarations([(horizontal_halo_properties, "halo_properties")])
                )
                missing = (named & halo) - horizontal
                assert not missing, f"halo fields missing in horizontal: {sorted(missing)}"

        def test_default_profile_lists_equal_the_registry_in_order():
            """profiles/default.yaml runs exactly the registered figures, in registry order."""
            plots = yaml.safe_load(profile.read_text())["plots"]
            assert plots["snapshot"] == self.snapshot_names, f"plots.snapshot: {plots['snapshot']}"
            assert (
                plots["evolution"] == self.evolution_names
            ), f"plots.evolution: {plots['evolution']}"

        def test_profile_stacks_agree_on_plots_and_axes():
            """The stack for each run file lists the registry and carries the same axes."""
            engine = load_engine()
            stacks = {d: self._profile_stack(engine, path) for d, path in run_files.items()}
            for driver, stack in stacks.items():
                assert stack["plots"]["snapshot"] == self.snapshot_names, f"{driver}: snapshot"
                assert stack["plots"]["evolution"] == self.evolution_names, f"{driver}: evolution"
            first, *others = stacks.values()
            for other in others:
                assert other["plots"] == first["plots"], "plots differ between the run files"
                assert other.get("axes") == first.get("axes"), "axes differ between the run files"
            if require_axes:
                assert first.get("axes"), "the stack carries no axes at all"

        return [
            test_registry_lists_have_no_duplicates_and_no_name_in_both,
            test_plot_funcs_and_requirements_have_exactly_the_registry_names,
            test_every_evolution_figure_has_fields_that_include_its_requirements,
            test_every_registry_field_is_a_declared_output_property,
            test_default_profile_lists_equal_the_registry_in_order,
            test_profile_stacks_agree_on_plots_and_axes,
        ]

    def _profile_stack(self, engine, run_file):
        """Return the merged plot profile the engine builds for a run file.

        The layers and their order follow configure_plot_profile(): the global profile, the
        model's default profile, the simulation's plot_profile.yaml, then the model's
        <simulation>_plot_profile.yaml, each included only if it exists. The shipped run files
        set neither `plotting.profile` nor overrides.
        """
        run = yaml.safe_load(run_file.read_text())
        assert (
            "plotting" not in run
        ), f"{run_file.name} sets plotting keys this stack does not model"
        simulation = run["simulation"]["name"]
        profiles = REPO_ROOT / "models" / self.model / "plots" / "profiles"
        layers = [
            MIMIC_PLOT_DIR / "profiles" / "default.yaml",
            profiles / "default.yaml",
            REPO_ROOT / "simulations" / simulation / "plot_profile.yaml",
            profiles / f"{simulation}_plot_profile.yaml",
        ]
        merged = {}
        for layer in layers:
            if layer.exists():
                merged = engine.merge_profile(merged, engine.read_profile_file(layer))
        return merged

    # ---- running a suite -------------------------------------------------------------------
    def run(self, cases, title):
        """Run ``cases`` through the shared runner with figures written to a temporary directory.

        The directory is created under $TMPDIR and removed at the end; returns the exit code.
        """
        self.tmp_root = tempfile.mkdtemp(prefix=f"{self.model}-figures-test-")
        try:
            return run_test_suite(cases, title)
        finally:
            shutil.rmtree(self.tmp_root, ignore_errors=True)
