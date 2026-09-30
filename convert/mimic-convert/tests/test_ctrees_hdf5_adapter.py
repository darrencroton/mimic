"""Unit tests: the Consistent-Trees forests-HDF5 adapter
(convert/mimic-convert/adapters/ctrees_hdf5.py).

**The oracle in this module is deliberately independent of the adapter.**
Synthetic sources are written with plain ``h5py`` from hand-declared rows, and
every expected value is either a literal or computed with scalar Python
arithmetic: float32 rounding through ``struct``, C's round-half-away-from-zero
through ``decimal``. Nothing here calls ``ctrees_payload``,
``convert_snapshots`` or any other adapter helper to build an expectation --
a test that did would only confirm the code agrees with itself.

The two committed source-layout fixtures are named explicitly and read back
independently, including their link types, so the tests prove the dependency
graph each one exercises rather than merely that the file exists:

- ``simulations/micro-uchuu-hdf5/_tests/data/MicroUchuu_test_mergertree_info.h5``
  (``File0`` is an ordinary group; one physical file);
- ``simulations/uchuu/_tests/data/mergertree_info.h5`` (``File0`` is an
  ``ExternalLink`` to ``mergertree_0.h5``; two physical files).

Every structural rule the adapter enforces was run over all 440,651 real
micro-Uchuu forests-HDF5 forests (22,580,924 halos) with zero violations
before it was made a gate, so none rejects valid source data.
"""

import os
import shutil
import struct
import sys
import tempfile
import tracemalloc
import unittest
from decimal import ROUND_HALF_UP, Decimal
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import column_schema as cs  # noqa: E402
from adapters import ctrees_hdf5 as ch  # noqa: E402
from adapters import source_inventory as si  # noqa: E402
from adapters import topology  # noqa: E402
from adapters.ctrees_hdf5 import (  # noqa: E402
    INVENTORY_BASE_BYTES,
    INVENTORY_BYTES_PER_UNIT,
    TOPOLOGY_BASE_BYTES,
    TOPOLOGY_READ_BUFFER_BYTES_PER_ROW,
    TOPOLOGY_READ_CHUNK_ROWS,
    ConverterError,
    CTreesHDF5Adapter,
)
from adapters.topology import VALIDATION_BYTES_PER_HALO  # noqa: E402

REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
PROFILE_DIR = os.path.join(REPO_ROOT, "convert", "mimic-convert", "profiles")
MICRO_FIXTURE = os.path.join(
    REPO_ROOT,
    "simulations",
    "micro-uchuu-hdf5",
    "_tests",
    "data",
    "MicroUchuu_test_mergertree_info.h5",
)
UCHUU_FIXTURE_DIR = os.path.join(REPO_ROOT, "simulations", "uchuu", "_tests", "data")
UCHUU_FIXTURE = os.path.join(UCHUU_FIXTURE_DIR, "mergertree_info.h5")
REAL_MICRO_UCHUU = os.path.join(
    REPO_ROOT, "simulations", "micro-uchuu-hdf5", "snapshots", "MicroUchuu_mergertree_info.h5"
)

#: The package value (simulations/*uchuu*/simulation_info.yaml), 1e10 Msun/h.
PARTICLE_MASS = 0.0327


# ==========================================================================
# Independent oracle
# ==========================================================================


def f32(value):
    """IEEE float32 rounding of a Python float, via struct -- not numpy."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def expected_len(mvir, particle_mass=PARTICLE_MASS):
    """C: ``(int)round((double)(float)Mvir * 1e-10 / PartMass)``."""
    particles = f32(mvir) * 1e-10 / particle_mass
    return int(Decimal(particles).quantize(Decimal(1), rounding=ROUND_HALF_UP))


def expected_spin(j, mvir):
    """C: ``(float)((double)(float)J * (1.0 / (double)(float)Mvir))``, unless Mvir is 0."""
    mass = f32(mvir)
    angular = f32(j)
    if mass == 0.0:
        return angular
    return f32(angular * (1.0 / mass))


LINKS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)
FLOATS = ("Mvir", "x", "y", "z", "vx", "vy", "vz", "Jx", "Jy", "Jz", "vrms", "vmax")

_ROW_DEFAULTS = {
    "Descendant": -1,
    "FirstProgenitor": -1,
    "NextProgenitor": -1,
    "NextHaloInFOFgroup": -1,
    "Mvir": 3.27e10,
    "x": 1.5,
    "y": 2.5,
    "z": 3.5,
    "vx": 10.0,
    "vy": 20.0,
    "vz": 30.0,
    "Jx": 0.0,
    "Jy": 0.0,
    "Jz": 0.0,
    "vrms": 50.0,
    "vmax": 100.0,
    "snap": 0,
}


def halo(**values):
    """One forest-local row; FirstHaloInFOFgroup defaults to the row itself."""
    return values


def forest(forest_id, *rows):
    return {"id": forest_id, "rows": list(rows)}


def lone(forest_id, **values):
    """A one-halo forest: its own FoF central with no links."""
    return forest(forest_id, halo(**values))


def pack_file(forests, order=None, first_id=1):
    """Lay out one file's forests contiguously in ``order`` (storage order).

    Returns the per-halo columns and the ForestInfo rows in *ForestInfo row
    order* (the order of ``forests``), with each forest's offset wherever
    ``order`` put it. Written by hand from the format description, not from
    the adapter.
    """
    order = list(range(len(forests))) if order is None else list(order)
    assert sorted(order) == list(range(len(forests)))
    columns = {name: [] for name in LINKS + FLOATS + ("id", "snap")}
    offsets = [0] * len(forests)
    cursor = 0
    next_id = first_id
    for index in order:
        offsets[index] = cursor
        for local, row in enumerate(forests[index]["rows"]):
            values = dict(_ROW_DEFAULTS)
            values["FirstHaloInFOFgroup"] = local
            values["id"] = next_id
            next_id += 1
            values.update(row)
            for name in columns:
                columns[name].append(values[name])
        cursor += len(forests[index]["rows"])
    info = [
        (entry["id"], offsets[index], len(entry["rows"]), 1) for index, entry in enumerate(forests)
    ]
    return columns, info


FOREST_INFO_DTYPE = np.dtype(
    [
        ("ForestID", "<i8"),
        ("ForestHalosOffset", "<i8"),
        ("ForestNhalos", "<i8"),
        ("ForestNtrees", "<i8"),
    ]
)


def _write_file_group(group, forests, order, snap_name, snap_float, first_id, extra_columns):
    columns, info = pack_file(forests, order, first_id)
    group.attrs["Nforests"] = np.int64(len(forests))
    group.attrs["Nhalos"] = np.int64(len(columns["id"]))
    group.attrs["contiguous-halo-props"] = np.int8(1)
    group.create_dataset("ForestInfo", data=np.array(info, dtype=FOREST_INFO_DTYPE))
    params = group.create_group("simulation_params")
    params.attrs["Boxsize"] = 100.0
    data = group.create_group("Forests")
    for name in LINKS + ("id",):
        data.create_dataset(name, data=np.array(columns[name], dtype="<i8"))
    for name in FLOATS:
        data.create_dataset(name, data=np.array(columns[name], dtype="<f8"))
    snap_dtype = "<f8" if snap_float else "<i8"
    data.create_dataset(snap_name, data=np.array(columns["snap"], dtype=snap_dtype))
    for name, values in (extra_columns or {}).items():
        data.create_dataset(name, data=np.asarray(values))
    return len(columns["id"])


def write_source(
    directory,
    files,
    layout="external",
    snap_name="Snap_num",
    snap_float=False,
    extra_columns=None,
    root_totals=True,
):
    """Write a forests-HDF5 source by hand. ``files`` is a list of dicts with
    ``forests`` and optional ``order``/``extra_columns``.

    ``layout="external"`` is the full-Uchuu organisation (``info.h5`` with one
    ``ExternalLink`` per ``File<N>`` to ``forests_<N>.h5``); ``"internal"`` is
    the micro-Uchuu fixture's (``File<N>`` groups inside ``info.h5``).
    """
    info_path = os.path.join(directory, "info.h5")
    total_forests = 0
    total_halos = 0
    next_id = 1
    with h5py.File(info_path, "w") as info:
        info.attrs["Nfiles"] = np.int64(len(files))
        for ordinal, spec in enumerate(files):
            extras = spec.get("extra_columns", extra_columns)
            if layout == "external":
                name = "forests_{}.h5".format(ordinal)
                with h5py.File(os.path.join(directory, name), "w") as data:
                    n = _write_file_group(
                        data,
                        spec["forests"],
                        spec.get("order"),
                        snap_name,
                        snap_float,
                        next_id,
                        extras,
                    )
                info["File{}".format(ordinal)] = h5py.ExternalLink(name, "/")
            else:
                group = info.create_group("File{}".format(ordinal))
                n = _write_file_group(
                    group,
                    spec["forests"],
                    spec.get("order"),
                    snap_name,
                    snap_float,
                    next_id,
                    extras,
                )
            next_id += n
            total_halos += n
            total_forests += len(spec["forests"])
        if root_totals:
            info.attrs["TotNforests"] = np.int64(total_forests)
            info.attrs["TotNhalos"] = np.int64(total_halos)
    return info_path


def group_path(directory, ordinal, layout="external"):
    """(file, group) holding ``File<ordinal>``'s contents, for corruption."""
    if layout == "external":
        return os.path.join(directory, "forests_{}.h5".format(ordinal)), "/"
    return os.path.join(directory, "info.h5"), "/File{}".format(ordinal)


def load_schema(name="consistent_trees_hdf5.yaml"):
    return cs.build_schema(cs.load_column_map(os.path.join(PROFILE_DIR, name)))


def schema_from(extra_fields, snap_aliases=("Snap_num", "Snap_idx")):
    """A consistent_trees_hdf5 schema with the given extras, built through
    the real profile parser."""
    required = {role: [role] for role in cs.REQUIRED_ROLES["consistent_trees_hdf5"]}
    required["snap"] = list(snap_aliases)
    document = {
        "schema_version": 1,
        "source_format": "consistent_trees_hdf5",
        "required_columns": required,
        "extra_fields": extra_fields,
    }
    return cs.build_schema(cs.parse_column_map(document, "<test>"))


def adapter_for(info_path, schema=None, first_file=0, last_file=None, **kwargs):
    if last_file is None:
        with h5py.File(info_path, "r") as handle:
            last_file = int(handle.attrs["Nfiles"]) - 1
    kwargs.setdefault("particle_mass", PARTICLE_MASS)
    return CTreesHDF5Adapter(
        schema or load_schema(), info_path, first_file=first_file, last_file=last_file, **kwargs
    )


def collect(adapter, max_rows=1000):
    """Concatenate every batch into one table per column group."""
    batches = list(adapter.iter_batches(max_rows))
    table = {}
    for group in ("identity", "coordinates", "links", "payload", "extras"):
        columns = {}
        for batch in batches:
            for name, values in getattr(batch, group).items():
                columns.setdefault(name, []).append(values)
        table[group] = {name: np.concatenate(parts) for name, parts in columns.items()}
    return table, batches


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ctrees_hdf5_adapter_")
        self.addCleanup(shutil.rmtree, self.tmp)


#: A four-halo forest with a real merger: two z=0 halos (rows 0, 1) in one
#: FoF group, each with one progenitor at the previous snapshot (rows 2, 3).
MERGER_FOREST = forest(
    7,
    halo(snap=5, FirstProgenitor=2, NextHaloInFOFgroup=1),
    halo(snap=5, FirstProgenitor=3, FirstHaloInFOFgroup=0),
    halo(snap=4, Descendant=0),
    halo(snap=4, Descendant=1),
)


# ==========================================================================
# The two committed source-layout fixtures
# ==========================================================================


class CommittedFixtureTests(unittest.TestCase):
    """Both committed fixtures, with independently declared expectations."""

    #: Hand-declared from the fixtures' own stored arrays (read back below):
    #: forest 0 has four halos, forests 1 and 2 one each.
    STORED_LINKS = {
        "Descendant": [-1, -1, 0, 1, -1, -1],
        "FirstProgenitor": [2, 3, -1, -1, -1, -1],
        "NextProgenitor": [-1, -1, -1, -1, -1, -1],
        "FirstHaloInFOFgroup": [0, 0, 2, 3, 0, 0],
        "NextHaloInFOFgroup": [1, -1, -1, -1, -1, -1],
    }
    #: The same links re-expressed as target SourceHaloID: forest 0 starts at
    #: id 1, forest 1 at 5, forest 2 at 6.
    EXPECTED_LINKS = {
        "Descendant": [-1, -1, 1, 2, -1, -1],
        "FirstProgenitor": [3, 4, -1, -1, -1, -1],
        "NextProgenitor": [-1, -1, -1, -1, -1, -1],
        "FirstHaloInFOFgroup": [1, 1, 3, 4, 5, 6],
        "NextHaloInFOFgroup": [2, -1, -1, -1, -1, -1],
    }
    MVIR = [
        32500000000.0,
        40000000000.0,
        55000000000.0,
        27500000000.0,
        60000000000.0,
        15000000000.0,
    ]

    def read_independently(self, path):
        with h5py.File(path, "r") as handle:
            link = handle.get("File0", getlink=True)
            forests = handle["File0/Forests"]
            snap_name = "Snap_num" if "Snap_num" in forests else "Snap_idx"
            stored = {name: forests[name][()] for name in forests}
            info = handle["File0/ForestInfo"][()]
            data_file = handle["File0"].file.filename
        return link, stored, info, snap_name, data_file

    def check_fixture(self, path, expected_link_type, expected_snap, expected_ids):
        link, stored, info, snap_name, data_file = self.read_independently(path)
        self.assertIsInstance(link, expected_link_type)
        self.assertEqual(snap_name, expected_snap)
        for name, values in self.STORED_LINKS.items():
            self.assertEqual(stored[name].tolist(), values, name)
        self.assertEqual(stored["Mvir"].tolist(), self.MVIR)
        self.assertEqual(info["ForestHalosOffset"].tolist(), [0, 4, 5])
        self.assertEqual(info["ForestNhalos"].tolist(), [4, 1, 1])

        adapter = adapter_for(path, max_snapshot=49)
        table, _ = collect(adapter, max_rows=4)
        self.assertEqual(table["identity"]["SourceHaloID"].tolist(), [1, 2, 3, 4, 5, 6])
        self.assertEqual(table["identity"]["ForestIndex"].tolist(), [0, 0, 0, 0, 1, 2])
        self.assertEqual(table["identity"]["HaloRankInForest"].tolist(), [0, 1, 2, 3, 0, 0])
        self.assertEqual(table["coordinates"]["source_file_ordinal"].tolist(), [0] * 6)
        self.assertEqual(table["coordinates"]["unit_ordinal"].tolist(), [0, 0, 0, 0, 1, 2])
        for name, values in self.EXPECTED_LINKS.items():
            self.assertEqual(table["links"][name].tolist(), values, name)
        payload = table["payload"]
        self.assertEqual(payload["SnapNum"].tolist(), [49, 49, 48, 48, 49, 49])
        self.assertEqual(payload["SnapNum"].dtype, np.int32)
        self.assertEqual(payload["MostBoundID"].tolist(), expected_ids)
        self.assertEqual(payload["M_Crit200"].tolist(), [f32(m) for m in self.MVIR])
        self.assertEqual(payload["Len"].tolist(), [expected_len(m) for m in self.MVIR])
        # Literal, so the oracle itself is pinned too.
        self.assertEqual(payload["Len"].tolist(), [99, 122, 168, 84, 183, 46])
        self.assertEqual(payload["Pos"][:, 0].tolist(), [5.0, 15.0, 25.0, 26.0, 35.0, 36.0])
        self.assertEqual(payload["Vel"][:, 2].tolist(), [30.0, 31.0, 32.0, 33.0, 34.0, 35.0])
        self.assertEqual(payload["VelDisp"].tolist(), [80.0, 85.0, 90.0, 75.0, 95.0, 70.0])
        self.assertEqual(payload["Vmax"].tolist(), [120.0, 125.0, 130.0, 115.0, 135.0, 110.0])
        self.assertEqual(payload["Spin"].tolist(), [[0.0, 0.0, 0.0]] * 6)
        self.assertEqual(
            [
                (r.forest_index, r.forest_id, r.unit_ordinal, r.n_halos)
                for r in adapter.iter_forests()
            ],
            [
                (0, info["ForestID"][0], 0, 4),
                (1, info["ForestID"][1], 1, 1),
                (2, info["ForestID"][2], 2, 1),
            ],
        )
        return adapter, data_file

    def test_micro_uchuu_in_file_layout(self):
        adapter, data_file = self.check_fixture(
            MICRO_FIXTURE, h5py.HardLink, "Snap_num", list(range(900001, 900007))
        )
        # One physical file backs everything, and it is the fixture itself.
        self.assertEqual(os.path.realpath(data_file), os.path.realpath(MICRO_FIXTURE))
        dependencies = adapter.dependencies()
        self.assertEqual([d.identity.path for d in dependencies], [os.path.realpath(MICRO_FIXTURE)])
        self.assertIn("/File0/Forests/Mvir", dependencies[0].objects)
        self.assertIn("/", dependencies[0].objects)

    def test_full_uchuu_external_link_layout(self):
        adapter, data_file = self.check_fixture(
            UCHUU_FIXTURE, h5py.ExternalLink, "Snap_idx", list(range(990001, 990007))
        )
        data_path = os.path.realpath(os.path.join(UCHUU_FIXTURE_DIR, "mergertree_0.h5"))
        self.assertEqual(os.path.realpath(data_file), data_path)
        by_path = {d.identity.path: d.objects for d in adapter.dependencies()}
        self.assertEqual(set(by_path), {os.path.realpath(UCHUU_FIXTURE), data_path})
        # The info file backs only the root; every object read comes through
        # the external link from the data file.
        self.assertEqual(by_path[os.path.realpath(UCHUU_FIXTURE)], ("/",))
        self.assertIn("/File0", by_path[data_path])
        self.assertIn("/File0/Forests/Snap_idx", by_path[data_path])
        size = os.path.getsize(data_path)
        pinned = [d.identity for d in adapter.dependencies() if d.identity.path == data_path][0]
        self.assertEqual(pinned.size_bytes, size)

    def test_full_uchuu_fixture_without_its_data_file_fails_before_any_row(self):
        tmp = tempfile.mkdtemp(prefix="uchuu_fixture_")
        self.addCleanup(shutil.rmtree, tmp)
        shutil.copy(UCHUU_FIXTURE, tmp)
        adapter = adapter_for(os.path.join(tmp, "mergertree_info.h5"), last_file=0)
        batches = adapter.iter_batches(10)
        with self.assertRaisesRegex(ConverterError, "external link .*unresolved"):
            next(batches)
        # With the data file restored beside it, the same copy converts.
        shutil.copy(os.path.join(UCHUU_FIXTURE_DIR, "mergertree_0.h5"), tmp)
        table, _ = collect(adapter_for(os.path.join(tmp, "mergertree_info.h5")))
        self.assertEqual(table["identity"]["SourceHaloID"].shape[0], 6)


# ==========================================================================
# Value conventions
# ==========================================================================


class ConventionTests(TempDirCase):
    """ctrees conventions, in the C reader's order, against scalar oracles."""

    def convert(self, rows, particle_mass=PARTICLE_MASS, **source):
        info = write_source(
            self.tmp, [{"forests": [lone(i + 1, **row) for i, row in enumerate(rows)]}], **source
        )
        table, _ = collect(adapter_for(info, particle_mass=particle_mass))
        return table["payload"]

    def test_mass_position_velocity_are_narrowed_to_float32(self):
        rows = [
            dict(
                Mvir=1.2345678912345678e12,
                x=12.345678901234567,
                vy=-321.0987654321,
                vrms=45.678901234,
                vmax=1e-40,
            ),
            dict(Mvir=6.54321e9, z=99.99999999, vx=1e-50, vmax=3.4e38),
        ]
        payload = self.convert(rows)
        self.assertEqual(payload["M_Crit200"].dtype, np.float32)
        self.assertEqual(payload["M_Crit200"].tolist(), [f32(r["Mvir"]) for r in rows])
        self.assertEqual(payload["Pos"][:, 0].tolist(), [f32(12.345678901234567), f32(1.5)])
        self.assertEqual(payload["Pos"][:, 2].tolist(), [f32(3.5), f32(99.99999999)])
        self.assertEqual(payload["Vel"][:, 1].tolist(), [f32(-321.0987654321), f32(20.0)])
        # Finite underflow follows the IEEE cast (1e-50 -> 0.0, 1e-40 -> subnormal).
        self.assertEqual(payload["Vel"][:, 0].tolist(), [f32(10.0), 0.0])
        self.assertEqual(payload["Vmax"].tolist(), [f32(1e-40), f32(3.4e38)])
        self.assertEqual(payload["VelDisp"].tolist(), [f32(45.678901234), f32(50.0)])

    def test_spin_is_normalised_on_the_narrowed_mass(self):
        # C multiplies by the reciprocal rather than dividing; with 24-bit
        # float32 operands the two cannot differ after the float32 cast, so
        # no test can tell them apart. Cast order can, and is pinned here.
        # Chosen so that normalising the unnarrowed float64 values gives a
        # different float32 (14.528942108154297) from the C reader's order.
        j, mvir = 5405231842636.223, 372032027743.45966
        payload = self.convert([dict(Mvir=mvir, Jx=j, Jy=-j, Jz=0.0)])
        self.assertEqual(expected_spin(j, mvir), 14.528943061828613)
        self.assertNotEqual(f32(j / mvir), expected_spin(j, mvir))
        self.assertEqual(
            payload["Spin"][0].tolist(), [14.528943061828613, -14.528943061828613, 0.0]
        )

    def test_spin_matches_the_oracle_across_magnitudes(self):
        rows = [
            dict(Mvir=m, Jx=jx, Jy=jy, Jz=jz)
            for m, jx, jy, jz in (
                (1.0e10, 3.3e12, -7.7e11, 1.0),
                (6.54e9, 1.23456789e13, 9.87654321e12, -5.5e12),
                (2.0e15, 1.0e17, 3.0e16, 7.0e14),
            )
        ]
        payload = self.convert(rows)
        for row, spin in zip(rows, payload["Spin"].tolist()):
            self.assertEqual(spin, [expected_spin(row[k], row["Mvir"]) for k in ("Jx", "Jy", "Jz")])

    def test_zero_mass_leaves_j_unnormalised_and_len_zero(self):
        payload = self.convert(
            [dict(Mvir=0.0, Jx=1.5e12, Jy=-2.0, Jz=0.0), dict(Mvir=-0.0, Jx=4.0)]
        )
        self.assertEqual(payload["Spin"][0].tolist(), [f32(1.5e12), -2.0, 0.0])
        self.assertEqual(payload["Spin"][1].tolist(), [4.0, 0.0, 0.0])
        self.assertEqual(payload["Len"].tolist(), [0, 0])
        # Signed zero survives as data.
        self.assertTrue(np.signbit(payload["M_Crit200"][1]))

    def test_len_rounds_half_away_from_zero_like_c(self):
        # 1.25e9 * 1e-10 / 0.25 is exactly 0.5 in double arithmetic: C's
        # round() gives 1, a round-half-to-even implementation would give 0.
        self.assertEqual(f32(1.25e9) * 1e-10 / 0.25, 0.5)
        payload = self.convert([dict(Mvir=1.25e9)], particle_mass=0.25)
        self.assertEqual(payload["Len"].tolist(), [1])

    def test_len_is_derived_from_the_narrowed_mass(self):
        # 1249999937 narrows to 1250000000.0, so C derives exactly 0.5 -> 1;
        # deriving from the float64 value would give 0.4999999748 -> 0.
        self.assertEqual(f32(1249999937.0), 1250000000.0)
        payload = self.convert([dict(Mvir=1249999937.0)], particle_mass=0.25)
        self.assertEqual(payload["Len"].tolist(), [1])

    def test_len_matches_the_oracle(self):
        masses = [3.27e8, 3.27e10, 1.23456789e12, 9.99e14, 6.54e9 + 1234.5]
        payload = self.convert([dict(Mvir=m) for m in masses])
        self.assertEqual(payload["Len"].dtype, np.int32)
        self.assertEqual(payload["Len"].tolist(), [expected_len(m) for m in masses])
        self.assertEqual(payload["Len"].tolist()[:2], [1, 100])

    def test_most_bound_id_is_the_stored_id_bit_for_bit(self):
        big = 2**62 + 12345
        payload = self.convert([dict(id=big), dict(id=-5), dict(id=big)])
        self.assertEqual(payload["MostBoundID"].tolist(), [big, -5, big])
        self.assertEqual(payload["MostBoundID"].dtype, np.int64)

    def test_negative_mass_is_fatal_as_in_the_vertical_reader(self):
        with self.assertRaisesRegex(ConverterError, "row 0: derived particle count"):
            self.convert([dict(Mvir=-1.0e10)])

    def test_len_above_int_max_is_fatal(self):
        with self.assertRaisesRegex(ConverterError, "derived particle count"):
            self.convert([dict(Mvir=1.0e30)], particle_mass=1e-3)

    def test_float32_overflow_is_rejected_naming_the_row(self):
        with self.assertRaisesRegex(
            ConverterError, r"forest row 1 .*row 0: x value .* overflows float32"
        ):
            self.convert([dict(), dict(x=1e39)])

    def test_non_finite_values_are_rejected(self):
        with self.assertRaisesRegex(ConverterError, "vrms holds the non-finite value"):
            self.convert([dict(vrms=float("nan"))])
        with self.assertRaisesRegex(ConverterError, "Jz holds the non-finite value"):
            self.convert([dict(Jz=float("inf"))])

    def test_spin_overflow_is_rejected(self):
        with self.assertRaisesRegex(ConverterError, "Spin = J / Mvir overflows float32"):
            self.convert([dict(Mvir=1e-30, Jx=1e30)], particle_mass=1.0)

    def test_particle_mass_must_be_positive_and_finite(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}])
        for bad in (0.0, -0.0327, float("nan"), float("inf"), True, "0.0327", None):
            with self.assertRaises(ConverterError, msg=repr(bad)):
                adapter_for(info, particle_mass=bad)
        adapter_for(info, particle_mass=np.float64(0.0327))
        adapter_for(info, particle_mass=1)


# ==========================================================================
# Snapshots
# ==========================================================================


class SnapshotTests(TempDirCase):
    def write(self, snaps, snap_name="Snap_num", snap_float=False):
        rows = [lone(i + 1, snap=s) for i, s in enumerate(snaps)]
        return write_source(
            self.tmp, [{"forests": rows}], snap_name=snap_name, snap_float=snap_float
        )

    def test_integer_snapshots(self):
        info = self.write([0, 7, 49])
        table, _ = collect(adapter_for(info, max_snapshot=49))
        self.assertEqual(table["payload"]["SnapNum"].tolist(), [0, 7, 49])

    def test_integral_float_snapshots(self):
        info = self.write([0.0, 7.0, 49.0], snap_name="Snap_idx", snap_float=True)
        table, _ = collect(adapter_for(info, max_snapshot=49))
        self.assertEqual(table["payload"]["SnapNum"].tolist(), [0, 7, 49])
        self.assertEqual(table["payload"]["SnapNum"].dtype, np.int32)

    def test_nonintegral_float_snapshot_fails(self):
        info = self.write([3.0, 48.5], snap_name="Snap_idx", snap_float=True)
        with self.assertRaisesRegex(
            ConverterError, r"row 0: snapshot value 48\.5 is not an integer"
        ):
            collect(adapter_for(info))

    def test_nonfinite_float_snapshot_fails(self):
        info = self.write([float("nan")], snap_name="Snap_idx", snap_float=True)
        with self.assertRaisesRegex(ConverterError, "is not an integer"):
            collect(adapter_for(info))

    def test_negative_and_out_of_range_snapshots_fail(self):
        with self.assertRaisesRegex(ConverterError, r"snapshot value -1 is outside \[0, "):
            collect(adapter_for(self.write([-1])))
        tmp = tempfile.mkdtemp(dir=self.tmp)
        info = write_source(tmp, [{"forests": [lone(1, snap=50)]}])
        with self.assertRaisesRegex(ConverterError, r"outside \[0, 49\]"):
            collect(adapter_for(info, max_snapshot=49))
        tmp = tempfile.mkdtemp(dir=self.tmp)
        info = write_source(tmp, [{"forests": [lone(1, snap=2**31)]}])
        with self.assertRaisesRegex(ConverterError, r"outside \[0, 2147483647\]"):
            collect(adapter_for(info))

    def test_both_spellings_in_one_file_is_ambiguous(self):
        info = self.write([1])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            handle["Forests"].create_dataset("Snap_idx", data=np.array([1.0]))
        with self.assertRaisesRegex(ConverterError, "resolve ambiguously"):
            adapter_for(info).inventory()

    def test_missing_snapshot_column_fails(self):
        info = self.write([1])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/Snap_num"]
        with self.assertRaisesRegex(ConverterError, "none of the aliases"):
            adapter_for(info).inventory()

    def test_spelling_is_resolved_per_file(self):
        write_source(self.tmp, [{"forests": [lone(1, snap=3)]}, {"forests": [lone(2, snap=4)]}])
        with h5py.File(os.path.join(self.tmp, "forests_1.h5"), "r+") as handle:
            values = handle["Forests/Snap_num"][()]
            del handle["Forests/Snap_num"]
            handle["Forests"].create_dataset("Snap_idx", data=values.astype("<f8"))
        table, _ = collect(adapter_for(os.path.join(self.tmp, "info.h5")))
        self.assertEqual(table["payload"]["SnapNum"].tolist(), [3, 4])

    def test_a_4_byte_snapshot_column_fails(self):
        info = self.write([1])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/Snap_num"]
            handle["Forests"].create_dataset("Snap_num", data=np.array([1], dtype="<i4"))
        with self.assertRaisesRegex(ConverterError, "stored as <i4"):
            adapter_for(info).inventory()


# ==========================================================================
# Topology and identity
# ==========================================================================


class TopologyTests(TempDirCase):
    def test_forest_local_links_use_their_own_offset(self):
        # ForestInfo row order is [A, B, C] but storage order is [C, A, B],
        # so every forest sits at a nonzero offset that differs from its row.
        forests = [
            MERGER_FOREST,
            lone(8, snap=2),
            forest(9, halo(snap=1, Descendant=1), halo(snap=2, FirstProgenitor=0)),
        ]
        info = write_source(self.tmp, [{"forests": forests, "order": [2, 0, 1]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r") as handle:
            table_info = handle["ForestInfo"][()]
        self.assertEqual(table_info["ForestHalosOffset"].tolist(), [2, 6, 0])
        adapter = adapter_for(info)
        table, _ = collect(adapter)
        # Emission is ForestInfo row order: A (ids 1-4), B (5), C (6-7).
        self.assertEqual(table["identity"]["ForestIndex"].tolist(), [0, 0, 0, 0, 1, 2, 2])
        self.assertEqual(table["identity"]["HaloRankInForest"].tolist(), [0, 1, 2, 3, 0, 0, 1])
        self.assertEqual(table["links"]["FirstProgenitor"].tolist(), [3, 4, -1, -1, -1, -1, 6])
        self.assertEqual(table["links"]["Descendant"].tolist(), [-1, -1, 1, 2, -1, 7, -1])
        self.assertEqual(table["links"]["FirstHaloInFOFgroup"].tolist(), [1, 1, 3, 4, 5, 6, 7])
        self.assertEqual(table["links"]["NextHaloInFOFgroup"].tolist(), [2, -1, -1, -1, -1, -1, -1])
        # Stored ids follow storage order (C first), so they identify rows.
        self.assertEqual(table["payload"]["MostBoundID"].tolist(), [3, 4, 5, 6, 7, 1, 2])

    def test_chain_order_and_forward_gaps_survive_exactly(self):
        # Three progenitors of row 0 in a stored, non-mass order, one of them
        # two snapshots back (a forward gap), plus a three-member FoF chain.
        gapped = forest(
            11,
            halo(snap=9, FirstProgenitor=3, NextHaloInFOFgroup=2),
            halo(snap=9, FirstHaloInFOFgroup=0),
            halo(snap=9, FirstHaloInFOFgroup=0, NextHaloInFOFgroup=1),
            halo(snap=8, Descendant=0, NextProgenitor=5, Mvir=1e10),
            halo(snap=8, NextHaloInFOFgroup=6),
            halo(snap=7, Descendant=0, NextProgenitor=6, Mvir=9e12),
            halo(snap=8, Descendant=0, Mvir=5e11, FirstHaloInFOFgroup=4),
        )
        info = write_source(self.tmp, [{"forests": [lone(1), gapped]}])
        table, _ = collect(adapter_for(info))
        links = table["links"]
        base = 2  # forest 1 starts after the lone forest's id 1
        self.assertEqual(links["FirstProgenitor"][1 + 0], base + 3)
        self.assertEqual(
            links["NextProgenitor"].tolist()[1:], [-1, -1, -1, base + 5, -1, base + 6, -1]
        )
        self.assertEqual(
            links["NextHaloInFOFgroup"].tolist()[1:], [base + 2, -1, base + 1, -1, base + 6, -1, -1]
        )
        # The gap: row 5 at snapshot 7 descends to row 0 at snapshot 9.
        self.assertEqual(table["payload"]["SnapNum"][1 + 5], 7)
        self.assertEqual(links["Descendant"][1 + 5], base + 0)

    def test_link_equal_to_the_forest_size_is_out_of_forest(self):
        # Global row 4 exists (the next forest), but the value is forest-local.
        bad = forest(1, halo(snap=1), halo(snap=0, Descendant=2))
        info = write_source(self.tmp, [{"forests": [bad, lone(2), lone(3)]}])
        with self.assertRaisesRegex(ConverterError, "points to row 2, outside this 2-halo tree"):
            collect(adapter_for(info))

    def test_link_below_the_null_sentinel_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(1, NextProgenitor=-2)]}])
        with self.assertRaisesRegex(ConverterError, "only -1 is the null sentinel"):
            collect(adapter_for(info))

    def test_null_fof_central_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(1, FirstHaloInFOFgroup=-1)]}])
        with self.assertRaisesRegex(ConverterError, "never null"):
            collect(adapter_for(info))

    def test_non_forward_descendant_fails(self):
        info = write_source(
            self.tmp, [{"forests": [forest(1, halo(snap=3), halo(snap=3, Descendant=0))]}]
        )
        with self.assertRaisesRegex(ConverterError, "not forward"):
            collect(adapter_for(info))

    def test_inconsistent_progenitor_round_trip_fails(self):
        info = write_source(
            self.tmp, [{"forests": [forest(1, halo(snap=3, FirstProgenitor=1), halo(snap=2))]}]
        )
        with self.assertRaisesRegex(ConverterError, "round trip is inconsistent"):
            collect(adapter_for(info))

    def test_invalid_fof_membership_fails(self):
        info = write_source(
            self.tmp, [{"forests": [forest(1, halo(snap=3), halo(snap=2, FirstHaloInFOFgroup=0))]}]
        )
        with self.assertRaisesRegex(ConverterError, "FoF links stay in the current snapshot"):
            collect(adapter_for(info))

    def test_an_invalid_forest_emits_nothing_after_it(self):
        bad = forest(2, halo(snap=3), halo(snap=3, Descendant=0))
        info = write_source(self.tmp, [{"forests": [lone(1), bad]}])
        seen = []
        with self.assertRaises(ConverterError):
            for batch in adapter_for(info).iter_batches(1):
                seen.extend(batch.identity["SourceHaloID"].tolist())
        self.assertEqual(seen, [1])


class InventoryTests(TempDirCase):
    def three_files(self, layout="external", directory=None):
        return write_source(
            directory or self.tmp,
            [
                {"forests": [lone(10), lone(11)]},
                {"forests": [MERGER_FOREST]},
                {
                    "forests": [
                        lone(30),
                        forest(
                            31,
                            halo(snap=1, NextHaloInFOFgroup=1),
                            halo(snap=1, FirstHaloInFOFgroup=0),
                        ),
                    ]
                },
            ],
            layout=layout,
        )

    def test_file_prefix_enumeration_across_files(self):
        for layout in ("external", "internal"):
            with self.subTest(layout=layout):
                info = self.three_files(layout, tempfile.mkdtemp(dir=self.tmp))
                adapter = adapter_for(info)
                table, _ = collect(adapter, max_rows=3)
                self.assertEqual(
                    table["identity"]["ForestIndex"].tolist(), [0, 1, 2, 2, 2, 2, 3, 4, 4]
                )
                self.assertEqual(
                    table["coordinates"]["source_file_ordinal"].tolist(),
                    [0, 0, 1, 1, 1, 1, 2, 2, 2],
                )
                self.assertEqual(
                    table["coordinates"]["unit_ordinal"].tolist(), [0, 1, 0, 0, 0, 0, 0, 1, 1]
                )
                self.assertEqual(table["identity"]["SourceHaloID"].tolist(), list(range(1, 10)))
                self.assertEqual(
                    [(r.forest_id, r.source_file_ordinal) for r in adapter.iter_forests()],
                    [(10, 0), (11, 0), (7, 1), (30, 2), (31, 2)],
                )

    def test_a_file_subrange_keeps_the_sources_own_file_numbers(self):
        info = self.three_files()
        table, _ = collect(adapter_for(info, first_file=1, last_file=2))
        self.assertEqual(
            table["coordinates"]["source_file_ordinal"].tolist(), [1, 1, 1, 1, 2, 2, 2]
        )
        # The C reader's forest numbering starts at the first requested file.
        self.assertEqual(table["identity"]["ForestIndex"].tolist(), [0, 0, 0, 0, 1, 2, 2])

    def test_requesting_a_file_beyond_nfiles_fails(self):
        info = self.three_files()
        with self.assertRaisesRegex(ConverterError, "Nfiles is 3"):
            adapter_for(info, last_file=3).inventory()

    def test_a_missing_file_group_fails(self):
        info = self.three_files()
        with h5py.File(info, "r+") as handle:
            del handle["File1"]
        with self.assertRaisesRegex(ConverterError, "/File1: does not exist"):
            adapter_for(info).inventory()

    def test_bad_file_ranges_fail(self):
        info = self.three_files()
        for first, last in ((2, 1), (-1, 0), (0.0, 1), (True, 1)):
            with self.assertRaises(ConverterError, msg=(first, last)):
                adapter_for(info, first_file=first, last_file=last)

    def test_root_and_file_totals_are_cross_checked(self):
        info = self.three_files()
        with h5py.File(info, "r+") as handle:
            handle.attrs["TotNhalos"] = np.int64(10)
        with self.assertRaisesRegex(ConverterError, "TotNhalos is 10"):
            adapter_for(info).inventory()
        # A subrange cannot be checked against whole-dataset totals.
        adapter_for(info, first_file=0, last_file=1).inventory()
        with h5py.File(os.path.join(self.tmp, "forests_1.h5"), "r+") as handle:
            handle.attrs["Nhalos"] = np.int64(5)
        with self.assertRaisesRegex(ConverterError, "Nhalos attribute is 5"):
            adapter_for(info, first_file=1, last_file=1).inventory()

    def test_file_attributes_are_checked(self):
        cases = (
            ("Nforests", np.int64(0), "at least one forest"),
            ("Nforests", np.float64(1.0), "scalar 8-byte integer"),
            ("Nforests", np.int32(2), "scalar 8-byte integer"),
            ("contiguous-halo-props", np.int8(0), "array-of-structs"),
        )
        for name, value, message in cases:
            with self.subTest(name=name, value=value):
                tmp = tempfile.mkdtemp(dir=self.tmp)
                info = write_source(tmp, [{"forests": [lone(1), lone(2)]}])
                with h5py.File(os.path.join(tmp, "forests_0.h5"), "r+") as handle:
                    handle.attrs[name] = value
                with self.assertRaisesRegex(ConverterError, message):
                    adapter_for(info).inventory()
        tmp = tempfile.mkdtemp(dir=self.tmp)
        info = write_source(tmp, [{"forests": [lone(1)]}])
        with h5py.File(info, "r+") as handle:
            del handle.attrs["Nfiles"]
        with self.assertRaisesRegex(ConverterError, "missing required attribute 'Nfiles'"):
            adapter_for(info, last_file=0).inventory()

    def test_real_bool_contiguous_flag_is_accepted(self):
        # The real micro-Uchuu data file stores it as an HDF5 boolean enum.
        info = write_source(self.tmp, [{"forests": [lone(1)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            handle.attrs["contiguous-halo-props"] = np.True_
        collect(adapter_for(info))

    def test_the_same_file_group_twice_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}, {"forests": [lone(2)]}])
        with h5py.File(info, "r+") as handle:
            del handle["File1"]
            handle["File1"] = h5py.ExternalLink("forests_0.h5", "/")
        with self.assertRaisesRegex(ConverterError, "/File1 is the same HDF5 object as /File0"):
            adapter_for(info).inventory()

    def test_input_validation(self):
        info = self.three_files()
        with self.assertRaisesRegex(ConverterError, "needs a 'consistent_trees_hdf5' schema"):
            CTreesHDF5Adapter(
                cs.build_schema(
                    cs.load_column_map(os.path.join(PROFILE_DIR, "consistent_trees_ascii.yaml"))
                ),
                info,
                first_file=0,
                last_file=0,
                particle_mass=PARTICLE_MASS,
            )
        with self.assertRaisesRegex(ConverterError, "missing or not a regular file"):
            adapter_for(os.path.join(self.tmp, "absent.h5"), last_file=0)
        with self.assertRaisesRegex(ConverterError, "not a filesystem path"):
            adapter_for(3.5, last_file=0)
        adapter = adapter_for(info)
        for bad in (0, -1, 1.0, True):
            with self.assertRaises(ConverterError):
                next(adapter.iter_batches(bad))
        for bad in (-1, 1.5):
            with self.assertRaises(ConverterError):
                adapter_for(info, max_snapshot=bad)
        with self.assertRaisesRegex(ConverterError, "not an HDF5|cannot open"):
            not_hdf5 = os.path.join(self.tmp, "text.h5")
            with open(not_hdf5, "w") as handle:
                handle.write("not hdf5")
            adapter_for(not_hdf5, last_file=0).inventory()


# ==========================================================================
# ForestInfo extent/offset/count agreement
# ==========================================================================


class ForestTableTests(TempDirCase):
    def test_pure_rules(self):
        ok = ch.validate_forest_table
        ok([0, 3, 5], [3, 2, 1], 6, "t")
        ok([3, 0, 5], [2, 3, 1], 6, "t")  # permuted rows still tile
        ok([0, 6, 3], [3, 0, 3], 6, "t")  # an empty forest may sit at the end
        cases = (
            (([0, 3], [3, -1], 3), "negative ForestNhalos"),
            (([0, -3], [3, 0], 3), "negative ForestHalosOffset"),
            (([0, 3], [3, 4], 6), r"slab \[3, 3\+4\)"),
            (([0, 7], [3, 0], 6), r"slab \[7, 7\+0\)"),
            (([0, 3], [3, 2], 6), "sum to 5"),
            (([0, 2], [3, 3], 6), "gap or overlap"),
            (([1, 3], [2, 3], 6), "sum to 5|starts at"),
            (([0, 4], [3, 3], 7), "sum to 6"),
            (([1, 4], [3, 3], 6), "no ForestInfo forest starts at row 0|gap or overlap|slab"),
        )
        for (offsets, counts, extent), message in cases:
            with self.subTest(offsets=offsets, counts=counts):
                with self.assertRaisesRegex(ConverterError, message):
                    ok(offsets, counts, extent, "t")

    def test_offsets_and_counts_beyond_int32_are_exact(self):
        big = 2**32 + 7
        ch.validate_forest_table([big, 0, 2 * big], [big, big, 5], 2 * big + 5, "t")
        with self.assertRaisesRegex(ConverterError, "gap or overlap at Forests row 4294967303"):
            ch.validate_forest_table([0, big - 1], [big, 6], big + 6, "t")

    def corrupt_info(self, mutate):
        info = write_source(self.tmp, [{"forests": [lone(1), lone(2), lone(3)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            table = handle["ForestInfo"][()]
            del handle["ForestInfo"]
            table = mutate(table)
            handle.create_dataset("ForestInfo", data=table)
        return info

    def test_overlapping_forests_in_a_file_fail(self):
        def mutate(table):
            table["ForestHalosOffset"] = [0, 0, 2]
            table["ForestNhalos"] = [2, 1, 1]
            return table

        with self.assertRaisesRegex(ConverterError, "sum to 4|gap or overlap"):
            adapter_for(self.corrupt_info(mutate)).inventory()

    def test_forest_info_rows_disagreeing_with_nforests_fail(self):
        info = self.corrupt_info(lambda table: table[:2])
        with self.assertRaisesRegex(ConverterError, "holds 2 rows but Nforests is 3"):
            adapter_for(info).inventory()

    def test_forest_info_layout_is_checked(self):
        narrow = np.dtype(
            [("ForestID", "<i8"), ("ForestHalosOffset", "<i8"), ("ForestNhalos", "<i8")]
        )
        info = self.corrupt_info(
            lambda table: np.array([tuple(r)[:3] for r in table], dtype=narrow)
        )
        with self.assertRaisesRegex(ConverterError, "record is 24 bytes"):
            adapter_for(info).inventory()
        tmp = tempfile.mkdtemp(dir=self.tmp)
        self.tmp = tmp
        renamed = np.dtype(
            [
                ("ForestID", "<i8"),
                ("Offset", "<i8"),
                ("ForestNhalos", "<i8"),
                ("ForestNtrees", "<i8"),
            ]
        )
        info = self.corrupt_info(lambda table: np.array([tuple(r) for r in table], dtype=renamed))
        with self.assertRaisesRegex(ConverterError, r"lacks member\(s\) \['ForestHalosOffset'\]"):
            adapter_for(info).inventory()

    def test_dataset_extents_must_agree(self):
        info = write_source(self.tmp, [{"forests": [lone(1), lone(2)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/vmax"]
            handle["Forests"].create_dataset("vmax", data=np.zeros(3))
        with self.assertRaisesRegex(ConverterError, "must share one per-halo extent"):
            adapter_for(info).inventory()


# ==========================================================================
# Dependencies: external links, VDS, unallocated storage
# ==========================================================================


class DependencyTests(TempDirCase):
    def test_a_missing_backing_file_fails_before_any_row(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}, {"forests": [lone(2)]}])
        os.rename(os.path.join(self.tmp, "forests_1.h5"), os.path.join(self.tmp, "moved.h5"))
        batches = adapter_for(info).iter_batches(1)
        with self.assertRaisesRegex(
            ConverterError, r"/File1: external link to 'forests_1.h5' is unresolved"
        ):
            next(batches)

    def test_a_same_named_file_elsewhere_is_never_substituted(self):
        # The data file is absent beside the info file but present in the
        # current directory, where HDF5's own search would find it.
        source = os.path.join(self.tmp, "source")
        elsewhere = os.path.join(self.tmp, "elsewhere")
        os.makedirs(source)
        os.makedirs(elsewhere)
        info = write_source(source, [{"forests": [lone(1)]}])
        os.rename(os.path.join(source, "forests_0.h5"), os.path.join(elsewhere, "forests_0.h5"))
        cwd = os.getcwd()
        os.chdir(elsewhere)
        try:
            with self.assertRaisesRegex(ConverterError, "is unresolved"):
                adapter_for(info).inventory()
        finally:
            os.chdir(cwd)

    def test_an_external_link_resolved_somewhere_else_fails(self):
        source = os.path.join(self.tmp, "source")
        other = os.path.join(self.tmp, "other")
        os.makedirs(source)
        os.makedirs(other)
        info = write_source(source, [{"forests": [lone(1)]}])
        shutil.copy(os.path.join(source, "forests_0.h5"), other)
        with mock.patch.dict(os.environ, {"HDF5_EXT_PREFIX": other}):
            with self.assertRaisesRegex(ConverterError, "resolved to .*other.*not the declared"):
                adapter_for(info).inventory()

    def test_a_nested_external_link_pins_every_file(self):
        info = write_source(self.tmp, [{"forests": [lone(1), lone(2)]}])
        data = os.path.join(self.tmp, "forests_0.h5")
        with h5py.File(data, "r+") as handle:
            mvir = handle["Forests/Mvir"][()]
            del handle["Forests/Mvir"]
            handle["Forests/Mvir"] = h5py.ExternalLink("mass.h5", "/Mvir")
        with h5py.File(os.path.join(self.tmp, "mass.h5"), "w") as handle:
            handle.create_dataset("Mvir", data=mvir)
        adapter = adapter_for(info)
        by_path = {d.identity.path: d.objects for d in adapter.dependencies()}
        mass = os.path.realpath(os.path.join(self.tmp, "mass.h5"))
        self.assertEqual(set(by_path), {os.path.realpath(info), os.path.realpath(data), mass})
        self.assertEqual(by_path[mass], ("/File0/Forests/Mvir",))
        table, _ = collect(adapter)
        self.assertEqual(table["payload"]["M_Crit200"].tolist(), [f32(3.27e10)] * 2)
        os.rename(os.path.join(self.tmp, "mass.h5"), os.path.join(self.tmp, "gone.h5"))
        with self.assertRaisesRegex(
            ConverterError, "Forests/Mvir: external link to 'mass.h5' is unresolved"
        ):
            adapter_for(info).inventory()

    def test_a_virtual_dataset_fails_even_when_its_source_is_present(self):
        for present in (False, True):
            with self.subTest(source_present=present):
                tmp = tempfile.mkdtemp(dir=self.tmp)
                info = write_source(tmp, [{"forests": [lone(1), lone(2)]}])
                data = os.path.join(tmp, "forests_0.h5")
                if present:
                    with h5py.File(os.path.join(tmp, "vsrc.h5"), "w") as handle:
                        handle.create_dataset("vmax", data=np.array([7.0, 8.0]))
                with h5py.File(data, "r+") as handle:
                    del handle["Forests/vmax"]
                    layout = h5py.VirtualLayout(shape=(2,), dtype="<f8")
                    layout[:] = h5py.VirtualSource("vsrc.h5", "vmax", shape=(2,))
                    handle["Forests"].create_virtual_dataset("vmax", layout, fillvalue=100.0)
                with self.assertRaisesRegex(ConverterError, "Forests/vmax: is a virtual dataset"):
                    adapter_for(info).inventory()

    def test_unallocated_storage_fails_rather_than_reading_fill_values(self):
        info = write_source(self.tmp, [{"forests": [lone(1), lone(2)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/vrms"]
            handle["Forests"].create_dataset("vrms", shape=(2,), dtype="<f8")
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r") as handle:
            self.assertEqual(handle["Forests/vrms"][()].tolist(), [0.0, 0.0])  # the hazard
        with self.assertRaisesRegex(ConverterError, "only 0 of 16 bytes of storage are allocated"):
            adapter_for(info).inventory()

    def test_a_partially_allocated_chunked_dataset_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(i) for i in range(1, 5)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/x"]
            dataset = handle["Forests"].create_dataset("x", shape=(4,), dtype="<f8", chunks=(2,))
            dataset[0:2] = [1.0, 2.0]
        with self.assertRaisesRegex(ConverterError, "only 1 of 2 chunks are allocated"):
            adapter_for(info).inventory()

    def test_fully_written_chunked_and_compressed_datasets_are_accepted(self):
        info = write_source(self.tmp, [{"forests": [lone(i) for i in range(1, 5)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/x"]
            handle["Forests"].create_dataset(
                "x", data=np.array([1.0, 2.0, 3.0, 4.0]), chunks=(3,), compression="gzip"
            )
        table, _ = collect(adapter_for(info))
        self.assertEqual(table["payload"]["Pos"][:, 0].tolist(), [1.0, 2.0, 3.0, 4.0])

    def test_hdf5_external_raw_storage_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(1), lone(2)]}])
        raw = os.path.join(self.tmp, "raw.bin")
        with open(raw, "wb") as handle:
            handle.write(np.array([5.0, 6.0], dtype="<f8").tobytes())
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/vx"]
            handle["Forests"].create_dataset("vx", shape=(2,), dtype="<f8", external=[(raw, 0, 16)])
        with self.assertRaisesRegex(ConverterError, "external file"):
            adapter_for(info).inventory()

    def test_soft_links_on_the_read_path_fail(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}], layout="internal")
        with h5py.File(info, "r+") as handle:
            handle["File0/Forests/Alias"] = h5py.SoftLink("/File0/Forests/vmax")
            handle.move("File0/Forests/vmax", "File0/Forests/vmax_real")
            handle["File0/Forests/vmax"] = h5py.SoftLink("/File0/Forests/vmax_real")
        with self.assertRaisesRegex(ConverterError, "vmax: is a soft link"):
            adapter_for(info).inventory()

    def test_a_group_where_a_dataset_belongs_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/vz"]
            handle["Forests"].create_group("vz")
        with self.assertRaisesRegex(ConverterError, "Forests/vz: is not a dataset"):
            adapter_for(info).inventory()

    def test_a_dependency_changed_after_inventory_fails(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}])
        adapter = adapter_for(info)
        adapter.inventory()
        data = os.path.join(self.tmp, "forests_0.h5")
        stat = os.stat(data)
        os.utime(data, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10**9))
        with self.assertRaisesRegex(ConverterError, "changed after inventory"):
            next(adapter.iter_batches(10))

    def test_pin_source_file_records_the_physical_identity(self):
        path = os.path.join(self.tmp, "pinned.bin")
        with open(path, "wb") as handle:
            handle.write(b"12345")
        link = os.path.join(self.tmp, "link.bin")
        os.symlink(path, link)
        pinned = si.pin_source_file(link)
        status = os.stat(path)
        self.assertEqual(pinned.path, os.path.realpath(path))
        self.assertEqual(
            (pinned.size_bytes, pinned.mtime_ns, pinned.device, pinned.inode),
            (5, status.st_mtime_ns, status.st_dev, status.st_ino),
        )
        with self.assertRaisesRegex(ConverterError, "cannot be pinned"):
            si.pin_source_file(os.path.join(self.tmp, "absent.bin"))
        with self.assertRaisesRegex(ConverterError, "is not a regular file"):
            si.pin_source_file(self.tmp)

    def test_dependency_objects_name_everything_read(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}])
        adapter = adapter_for(info)
        objects = [o for d in adapter.dependencies() for o in d.objects]
        expected = {"/", "/File0", "/File0/ForestInfo", "/File0/Forests"}
        expected.update("/File0/Forests/" + name for name in LINKS + FLOATS + ("id", "Snap_num"))
        self.assertEqual(set(objects), expected)
        self.assertEqual(len(objects), len(expected))


# ==========================================================================
# Stored dtypes
# ==========================================================================


class DtypeTests(TempDirCase):
    def replace(self, name, values, dtype):
        info = write_source(self.tmp, [{"forests": [lone(1), lone(2)]}])
        with h5py.File(os.path.join(self.tmp, "forests_0.h5"), "r+") as handle:
            del handle["Forests/" + name]
            handle["Forests"].create_dataset(name, data=np.asarray(values, dtype=dtype))
        return info

    def test_role_dtypes_must_be_what_the_c_reader_reads(self):
        cases = (
            ("Mvir", [3.27e10, 3.27e10], ">f8", "Mvir' is stored as >f8"),
            ("Mvir", [3.27e10, 3.27e10], "<f4", "stored as <f4"),
            ("Mvir", [3, 3], "<i8", "stored as <i8"),
            ("Descendant", [-1, -1], "<i4", "stored as <i4"),
            ("Descendant", [-1.0, -1.0], "<f8", "stored as <f8"),
            ("id", [1, 2], "<u8", "stored as <u8"),
            ("id", [1, 2], ">i8", "stored as >i8"),
        )
        for name, values, dtype, message in cases:
            with self.subTest(name=name, dtype=dtype):
                self.tmp = tempfile.mkdtemp(dir=self.tmp)
                with self.assertRaisesRegex(ConverterError, message):
                    adapter_for(self.replace(name, values, dtype)).inventory()

    def test_a_two_dimensional_role_fails(self):
        info = self.replace("x", [[1.0, 2.0], [3.0, 4.0]], "<f8")
        with self.assertRaisesRegex(ConverterError, "must be one-dimensional"):
            adapter_for(info).inventory()


# ==========================================================================
# Declarative extras
# ==========================================================================


class ExtraFieldTests(TempDirCase):
    def extra(self, name, sources, type_):
        return {
            "name": name,
            "sources": sources,
            "type": type_,
            "units": "dimensionless",
            "h_convention": "none",
            "description": "test extra",
        }

    def test_the_shipped_extras_example_carries_raw_source_values(self):
        extra_columns = {
            "Rvir": np.array([123.456789012345, 7.0e-3], dtype="<f8"),
            "Spin": np.array([0.0312345678901, 0.05], dtype="<f8"),
            "pid": np.array([-1, 2**61 + 3], dtype="<i8"),
        }
        info = write_source(
            self.tmp, [{"forests": [lone(1), lone(2)]}], extra_columns=extra_columns
        )
        table, _ = collect(
            adapter_for(info, load_schema("consistent_trees_hdf5_extras_example.yaml"))
        )
        extras = table["extras"]
        self.assertEqual(extras["CatalogRvir"].dtype, np.float64)
        self.assertEqual(extras["CatalogRvir"].tolist(), [123.456789012345, 7.0e-3])
        self.assertEqual(extras["SpinParameter"].tolist(), [0.0312345678901, 0.05])
        self.assertEqual(extras["ParentID"].tolist(), [-1, 2**61 + 3])

    def test_an_extra_reusing_a_role_keeps_its_pre_convention_value(self):
        j, mvir = 5405231842636.223, 372032027743.45966
        info = write_source(self.tmp, [{"forests": [lone(1, Jx=j, Mvir=mvir)]}])
        schema = schema_from(
            [
                self.extra("RawJx", [{"field": "Jx"}], "double"),
                self.extra("RawMass", [{"field": "Mvir"}], "double"),
                self.extra("CatalogId", [{"field": "id"}], "long long"),
            ]
        )
        table, _ = collect(adapter_for(info, schema))
        self.assertEqual(table["extras"]["RawJx"].tolist(), [j])
        self.assertEqual(table["extras"]["RawMass"].tolist(), [mvir])
        self.assertEqual(table["extras"]["CatalogId"].tolist(), [1])
        self.assertEqual(table["payload"]["Spin"][0, 0], expected_spin(j, mvir))

    def test_vector_extras_from_scalars_and_components(self):
        extra_columns = {
            "A": np.array([[1.5, 2.5, 3.5], [4.5, 5.5, 6.5]], dtype="<f4"),
            "n": np.array([7, -8], dtype="<i4"),
        }
        info = write_source(
            self.tmp, [{"forests": [lone(1), lone(2)]}], extra_columns=extra_columns
        )
        schema = schema_from(
            [
                self.extra(
                    "Shape", [{"field": "A", "component": c} for c in (2, 0, 1)], "vec3_float"
                ),
                self.extra("AY", [{"field": "A", "component": 1}], "float"),
                self.extra("Count", [{"field": "n"}], "int"),
                self.extra("Where", [{"field": "x"}, {"field": "y"}, {"field": "z"}], "vec3_float"),
            ]
        )
        with self.assertRaisesRegex(
            ConverterError, r"stores <f8, but the extra is declared vec3_float"
        ):
            adapter_for(info, schema).inventory()
        schema = schema_from(
            [
                self.extra(
                    "Shape", [{"field": "A", "component": c} for c in (2, 0, 1)], "vec3_float"
                ),
                self.extra("AY", [{"field": "A", "component": 1}], "float"),
                self.extra("Count", [{"field": "n"}], "int"),
            ]
        )
        table, _ = collect(adapter_for(info, schema))
        self.assertEqual(table["extras"]["Shape"].tolist(), [[3.5, 1.5, 2.5], [6.5, 4.5, 5.5]])
        self.assertEqual(table["extras"]["AY"].tolist(), [2.5, 5.5])
        self.assertEqual(table["extras"]["Count"].dtype, np.int32)
        self.assertEqual(table["extras"]["Count"].tolist(), [7, -8])

    def test_extra_type_and_arity_mismatches_fail(self):
        extra_columns = {"A": np.zeros((2, 3), dtype="<f8"), "n": np.array([1, 2], dtype="<i8")}
        info = write_source(
            self.tmp, [{"forests": [lone(1), lone(2)]}], extra_columns=extra_columns
        )
        cases = (
            (
                [self.extra("E", [{"field": "Mvir"}], "float")],
                "stores <f8, but the extra is declared float",
            ),
            (
                [self.extra("E", [{"field": "n"}], "double")],
                "integers never pass through floating point",
            ),
            (
                [self.extra("E", [{"field": "n"}], "int")],
                "stores <i8, but the extra is declared int",
            ),
            ([self.extra("E", [{"field": "A"}], "double")], "names a stored scalar"),
            (
                [self.extra("E", [{"field": "n", "component": 0}], "long long")],
                "component 0 of a stored vector",
            ),
            ([self.extra("E", [{"field": "absent"}], "double")], "does not carry field 'absent'"),
        )
        for extras, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ConverterError, message):
                    adapter_for(info, schema_from(extras)).inventory()

    def test_non_finite_extra_values_fail(self):
        info = write_source(
            self.tmp,
            [{"forests": [lone(1), lone(2)]}],
            extra_columns={"R": np.array([1.0, np.nan])},
        )
        schema = schema_from([self.extra("R2", [{"field": "R"}], "double")])
        with self.assertRaisesRegex(
            ConverterError, "row 0: extra 'R2' source 'R' holds the non-finite"
        ):
            collect(adapter_for(info, schema))


# ==========================================================================
# Chunking and 64-bit arithmetic
# ==========================================================================


def chain_forest(n):
    """A linear main-branch chain of n halos across n snapshots."""
    rows = [
        halo(
            snap=n - 1 - i,
            FirstProgenitor=i + 1 if i + 1 < n else -1,
            Descendant=i - 1,
            Mvir=3.27e10 + i,
        )
        for i in range(n)
    ]
    return forest(99, *rows)


class ChunkingTests(TempDirCase):
    def test_a_forest_larger_than_the_chunk_is_split_and_unchanged(self):
        info = write_source(
            self.tmp, [{"forests": [lone(1), chain_forest(9), lone(2)], "order": [2, 1, 0]}]
        )
        reference, _ = collect(adapter_for(info), max_rows=1000)
        for max_rows in (1, 2, 3, 4, 7):
            with self.subTest(max_rows=max_rows):
                table, batches = collect(adapter_for(info), max_rows=max_rows)
                self.assertTrue(all(b.n_rows <= max_rows for b in batches))
                self.assertEqual(sum(b.n_rows for b in batches), 11)
                for group in ("identity", "links", "payload"):
                    for name, values in reference[group].items():
                        np.testing.assert_array_equal(table[group][name], values)
        self.assertEqual(
            reference["links"]["FirstProgenitor"].tolist()[1:10], [3, 4, 5, 6, 7, 8, 9, 10, -1]
        )
        self.assertEqual(
            reference["payload"]["Len"].tolist()[1:10],
            [expected_len(3.27e10 + i) for i in range(9)],
        )

    def test_topology_reads_are_chunked_within_a_forest(self):
        info = write_source(self.tmp, [{"forests": [chain_forest(10)]}])
        with mock.patch.object(ch, "TOPOLOGY_READ_CHUNK_ROWS", 3):
            reads = []
            original = CTreesHDF5Adapter._read

            def spy(dataset, low, high, component, context):
                reads.append(high - low)
                return original(dataset, low, high, component, context)

            with mock.patch.object(CTreesHDF5Adapter, "_read", staticmethod(spy)):
                collect(adapter_for(info), max_rows=4)
        self.assertLessEqual(max(reads), 4)
        self.assertIn(3, reads)

    def test_links_and_identities_beyond_int32_stay_exact(self):
        """A sparse synthetic forest far past 2**31: only the requested chunk
        is ever materialised, so no billion-row fixture is needed."""
        offset = 2**33 + 11
        n_halos = 2**32 + 5
        start = 2**32 - 2
        base_id = 2**40 + 1

        class Synthetic:
            name = "/synthetic"

            def __init__(self, fill):
                self.fill = fill

            def __getitem__(self, key):
                window = np.arange(key.start, key.stop, dtype=np.int64)
                return self.fill(window)

        def local(rows):
            """Forest-local row index of a global dataset row."""
            return rows - offset

        datasets = {
            "Descendant": Synthetic(lambda rows: np.where(local(rows) > 0, local(rows) - 1, -1)),
            "FirstProgenitor": Synthetic(
                lambda rows: np.where(local(rows) + 1 < n_halos, local(rows) + 1, -1)
            ),
            "NextProgenitor": Synthetic(lambda rows: np.full(rows.shape, -1)),
            "FirstHaloInFOFgroup": Synthetic(local),
            "NextHaloInFOFgroup": Synthetic(lambda rows: np.full(rows.shape, -1)),
            "Snap_num": Synthetic(lambda rows: np.full(rows.shape, 3)),
            "id": Synthetic(lambda rows: rows * 3),
        }
        for role in FLOATS:
            datasets[role] = Synthetic(lambda rows: np.full(rows.shape, 3.27e10))
        plan = ch._FilePlan(
            ordinal=5,
            forest_base=2**34,
            halo_extent=offset + n_halos,
            offsets=np.array([offset]),
            counts=np.array([n_halos]),
            forest_ids=np.array([77]),
            roles={
                role: ("Snap_num" if role == "snap" else role)
                for role in cs.REQUIRED_ROLES["consistent_trees_hdf5"]
            },
            extras={},
            dtypes={},
        )
        adapter = CTreesHDF5Adapter.__new__(CTreesHDF5Adapter)
        adapter.schema = load_schema()
        adapter.particle_mass = PARTICLE_MASS
        adapter.max_snapshot = None
        columns = adapter._columns(
            datasets, plan, 0, offset, n_halos, base_id, start, 4, "synthetic"
        )
        rows = [start, start + 1, start + 2, start + 3]
        self.assertEqual(columns["identity"]["SourceHaloID"].tolist(), [base_id + r for r in rows])
        self.assertEqual(columns["identity"]["HaloRankInForest"].tolist(), rows)
        self.assertEqual(columns["identity"]["ForestIndex"].tolist(), [2**34] * 4)
        self.assertEqual(columns["links"]["Descendant"].tolist(), [base_id + r - 1 for r in rows])
        self.assertEqual(
            columns["links"]["FirstProgenitor"].tolist(), [base_id + r + 1 for r in rows]
        )
        self.assertEqual(
            columns["links"]["FirstHaloInFOFgroup"].tolist(), [base_id + r for r in rows]
        )
        self.assertEqual(
            columns["payload"]["MostBoundID"].tolist(), [(offset + r) * 3 for r in rows]
        )
        for group in columns.values():
            for values in group.values():
                if values.dtype.kind == "i" and values.dtype != np.int32:
                    self.assertEqual(values.dtype, np.int64)

    def test_a_super_forest_is_refused_before_any_whole_forest_allocation(self):
        adapter = CTreesHDF5Adapter.__new__(CTreesHDF5Adapter)
        adapter.memory_budget_bytes = ch.DEFAULT_MEMORY_BUDGET_BYTES
        adapter.max_snapshot = None
        n_halos = 2**31 + 3
        with mock.patch.object(np, "empty", side_effect=AssertionError("allocated")):
            with self.assertRaisesRegex(
                ConverterError, "structural validation of 2147483651 halos"
            ):
                adapter._read_forest_topology({}, None, 0, n_halos, "super")


# ==========================================================================
# Read windows
# ==========================================================================


def batch_bytes(batches):
    """Every batch as comparable bytes: size, then each column's dtype, shape
    and raw bytes, group by group."""
    out = []
    for batch in batches:
        record = [batch.n_rows]
        for group in ("identity", "coordinates", "links", "payload", "extras"):
            for name, values in sorted(getattr(batch, group).items()):
                record.append((group, name, values.dtype.str, values.shape, values.tobytes()))
        out.append(record)
    return out


def per_forest_batches(adapter, max_rows):
    """The batches the per-forest route emits for ``adapter``'s source."""
    with mock.patch.object(ch, "_in_storage_order", return_value=False):
        return list(adapter.iter_batches(max_rows))


def numbered(forest_id, first_id, forest_spec):
    """``forest_spec`` with every row's catalog ``id`` set explicitly, so the
    stored ids do not depend on where the forest sits in the datasets."""
    rows = [dict(row, id=first_id + index) for index, row in enumerate(forest_spec["rows"])]
    return forest(forest_id, *rows)


class ReadWindowTests(TempDirCase):
    """Storage-order files are read through read windows; every other file,
    and every forest larger than a window, is read per forest. Both routes
    must emit byte-identical batches."""

    def mixed_forests(self):
        return [
            numbered(1, 100, lone(1)),
            numbered(2, 200, chain_forest(9)),
            numbered(3, 300, MERGER_FOREST),
            forest(4),
            numbered(5, 500, lone(5)),
            numbered(6, 600, chain_forest(4)),
        ]

    def test_committed_fixtures_emit_identical_batches_through_both_routes(self):
        for fixture in (MICRO_FIXTURE, UCHUU_FIXTURE):
            for max_rows in (1, 2, 3, 4, 1000):
                with self.subTest(fixture=os.path.basename(fixture), max_rows=max_rows):
                    adapter = adapter_for(fixture)
                    adapter.inventory()
                    self.assertTrue(ch._in_storage_order(adapter._plans[0]))
                    self.assertEqual(
                        batch_bytes(adapter.iter_batches(max_rows)),
                        batch_bytes(per_forest_batches(adapter, max_rows)),
                    )

    def test_windows_smaller_than_a_forest_fall_back_for_that_forest_only(self):
        info = write_source(
            self.tmp,
            [
                {
                    "forests": self.mixed_forests(),
                    "extra_columns": {"pid": np.arange(19, dtype="<i8") * 7 - 3},
                },
                {
                    "forests": [numbered(7, 700, lone(7))],
                    "extra_columns": {"pid": np.array([11], dtype="<i8")},
                },
            ],
            layout="internal",
        )
        schema = schema_from(
            [
                {
                    "name": "ParentID",
                    "sources": [{"field": "pid"}],
                    "type": "long long",
                    "units": "dimensionless",
                    "h_convention": "none",
                    "description": "test extra",
                }
            ]
        )
        for window_rows in (1, 3, 5, 16, TOPOLOGY_READ_CHUNK_ROWS):
            for max_rows in (1, 3, 7, 1000):
                with self.subTest(window_rows=window_rows, max_rows=max_rows):
                    adapter = adapter_for(info, schema)
                    with mock.patch.object(ch, "TOPOLOGY_READ_CHUNK_ROWS", window_rows):
                        windowed = batch_bytes(adapter.iter_batches(max_rows))
                        reference = batch_bytes(per_forest_batches(adapter, max_rows))
                    self.assertEqual(windowed, reference)

    def test_a_source_not_in_storage_order_is_read_per_forest_with_the_same_batches(self):
        """The same forests laid out in reverse storage order take the
        per-forest route, and emit exactly what the storage-order layout emits
        through its read window."""
        forests = self.mixed_forests()
        for name in ("ordered", "shuffled"):
            os.makedirs(os.path.join(self.tmp, name))
        ordered = write_source(os.path.join(self.tmp, "ordered"), [{"forests": forests}])
        shuffled = write_source(
            os.path.join(self.tmp, "shuffled"),
            [{"forests": forests, "order": list(reversed(range(len(forests))))}],
        )
        windows = []
        real_read_window = CTreesHDF5Adapter._read_window

        def spy(adapter_self, *args, **kwargs):
            window = real_read_window(adapter_self, *args, **kwargs)
            windows.append(window)
            return window

        for max_rows in (1, 4, 1000):
            with self.subTest(max_rows=max_rows):
                windows.clear()
                with mock.patch.object(CTreesHDF5Adapter, "_read_window", spy):
                    reference = batch_bytes(adapter_for(ordered).iter_batches(max_rows))
                    self.assertEqual(len(windows), 1)
                    windows.clear()
                    adapter = adapter_for(shuffled)
                    adapter.inventory()
                    self.assertFalse(ch._in_storage_order(adapter._plans[0]))
                    self.assertEqual(batch_bytes(adapter.iter_batches(max_rows)), reference)
                self.assertEqual(windows, [])

    def test_each_dataset_is_read_once_per_window(self):
        n_forests, window_rows = 50, 16
        info = write_source(self.tmp, [{"forests": [lone(i) for i in range(1, n_forests + 1)]}])
        reads = []
        original = CTreesHDF5Adapter._read

        def spy(dataset, low, high, component, context):
            reads.append((dataset.name, low, high))
            return original(dataset, low, high, component, context)

        with mock.patch.object(ch, "TOPOLOGY_READ_CHUNK_ROWS", window_rows):
            with mock.patch.object(CTreesHDF5Adapter, "_read", staticmethod(spy)):
                table, _ = collect(adapter_for(info), max_rows=7)
        self.assertEqual(table["identity"]["SourceHaloID"].tolist(), list(range(1, 51)))
        n_datasets = len(cs.REQUIRED_ROLES["consistent_trees_hdf5"])
        windows = sorted({(low, high) for _name, low, high in reads})
        self.assertEqual(windows, [(0, 16), (16, 32), (32, 48), (48, 50)])
        self.assertEqual(len(reads), len(windows) * n_datasets)

    def test_batches_own_their_memory_rather_than_viewing_a_window(self):
        """A batch that viewed its read window would keep the whole window
        alive until the batch is written."""
        info = write_source(
            self.tmp,
            [{"forests": self.mixed_forests()}],
            extra_columns={
                "Rvir": np.linspace(1.0, 2.0, 19),
                "Spin": np.zeros(19),
                "pid": np.arange(19),
            },
        )
        schema = load_schema("consistent_trees_hdf5_extras_example.yaml")
        for max_rows in (1, 5, 1000):
            with self.subTest(max_rows=max_rows):
                for batch in adapter_for(info, schema).iter_batches(max_rows):
                    for group in ("identity", "coordinates", "links", "payload", "extras"):
                        for name, values in getattr(batch, group).items():
                            self.assertTrue(values.flags.owndata, (group, name))

    def test_the_window_is_sized_from_the_budget(self):
        info = write_source(self.tmp, [{"forests": self.mixed_forests()}])
        adapter = adapter_for(info)
        adapter.inventory()
        plan = adapter._plans[0]
        per_row = ch._window_bytes_per_row(plan)
        # 19 eight-byte role datasets plus the 32 B/row transient allowance.
        self.assertEqual(per_row, 19 * 8 + TOPOLOGY_READ_BUFFER_BYTES_PER_ROW)
        self.assertEqual(adapter._window_rows(plan), TOPOLOGY_READ_CHUNK_ROWS)
        for rows in (0, 1, 27, 1000):
            adapter.memory_budget_bytes = TOPOLOGY_BASE_BYTES + rows * (
                VALIDATION_BYTES_PER_HALO + per_row
            )
            self.assertEqual(adapter._window_rows(plan), rows)
            adapter.memory_budget_bytes += VALIDATION_BYTES_PER_HALO + per_row - 1
            self.assertEqual(adapter._window_rows(plan), rows)
        adapter.memory_budget_bytes = TOPOLOGY_BASE_BYTES - 1
        self.assertEqual(adapter._window_rows(plan), 0)

    def test_a_window_held_forest_is_charged_the_window_as_its_read_buffer(self):
        info = write_source(self.tmp, [{"forests": self.mixed_forests()}])
        adapter = adapter_for(info)
        charged = []
        real_check = ch.check_validation_budget

        def spy(n_halos, read_buffer_bytes, base_bytes, budget, context):
            charged.append((n_halos, read_buffer_bytes, base_bytes))
            return real_check(n_halos, read_buffer_bytes, base_bytes, budget, context)

        with mock.patch.object(ch, "check_validation_budget", spy):
            collect(adapter)
        window_bytes = 19 * ch._window_bytes_per_row(adapter._plans[0])
        self.assertEqual(
            charged,
            [(n, window_bytes, TOPOLOGY_BASE_BYTES) for n in (1, 9, 4, 1, 4)],
        )


# ==========================================================================
# Budget
# ==========================================================================


class BudgetTests(TempDirCase):
    def test_an_over_budget_inventory_fails_before_forest_info_is_read(self):
        info = write_source(self.tmp, [{"forests": [lone(i) for i in range(1, 11)]}])
        with mock.patch.object(h5py.Dataset, "fields", side_effect=AssertionError("read")):
            with self.assertRaisesRegex(ConverterError, "inventory of 10 forests"):
                adapter_for(
                    info, memory_budget_bytes=INVENTORY_BASE_BYTES + 9 * INVENTORY_BYTES_PER_UNIT
                ).inventory()
        adapter_for(
            info, memory_budget_bytes=INVENTORY_BASE_BYTES + 10 * INVENTORY_BYTES_PER_UNIT
        ).inventory()

    def test_forest_validation_budget_is_exact_at_its_boundary(self):
        n = 50
        info = write_source(self.tmp, [{"forests": [chain_forest(n)]}])
        need = (
            n * VALIDATION_BYTES_PER_HALO
            + n * TOPOLOGY_READ_BUFFER_BYTES_PER_ROW
            + TOPOLOGY_BASE_BYTES
        )
        # Inventory is built first under the default budget; the ceiling is
        # then lowered to exercise only the per-forest term at its boundary.
        adapter = adapter_for(info)
        adapter.inventory()
        adapter.memory_budget_bytes = need
        collect(adapter)
        adapter.memory_budget_bytes = need - 1
        with self.assertRaisesRegex(ConverterError, "structural validation of 50 halos"):
            collect(adapter)

    #: Raw bytes one emitted row reads without extras: nineteen 8-byte role
    #: datasets. Canonical bytes: 88 of identity, coordinates and links plus
    #: the 64-byte ctrees payload.
    RAW_ROW_BYTES = 19 * 8
    CANONICAL_ROW_BYTES = 88 + 64

    def emission_need(self, rows, extra_raw=0, extra_canonical=0):
        return (
            2 * rows * (self.RAW_ROW_BYTES + extra_raw + self.CANONICAL_ROW_BYTES + extra_canonical)
        )

    def test_an_emission_buffer_over_the_budget_is_refused_before_any_forest_is_read(self):
        n = 1000
        info = write_source(self.tmp, [{"forests": [chain_forest(n)]}])
        need = self.emission_need(n)
        self.assertGreater(need - 1, INVENTORY_BASE_BYTES + INVENTORY_BYTES_PER_UNIT)
        adapter = adapter_for(info, memory_budget_bytes=need - 1)
        with mock.patch.object(
            CTreesHDF5Adapter, "_read", staticmethod(mock.Mock(side_effect=AssertionError("read")))
        ):
            with self.assertRaises(ConverterError) as caught:
                list(adapter.iter_batches(n))
        message = str(caught.exception)
        self.assertIn(
            "emission (batches of 1000 rows, 152 B/row raw read plus 152 B/row canonical", message
        )
        self.assertIn("--ingest-max-rows", message)
        self.assertIn("memory_budget_bytes", message)

    def test_an_emission_buffer_at_the_budget_is_accepted(self):
        n = 1000
        info = write_source(self.tmp, [{"forests": [chain_forest(n)]}])
        table, batches = collect(adapter_for(info, memory_budget_bytes=self.emission_need(n)), n)
        self.assertEqual([batch.n_rows for batch in batches], [n])
        self.assertEqual(table["identity"]["SourceHaloID"].tolist(), list(range(1, n + 1)))
        # Far above the source's halo count, the term is charged for its rows.
        collect(adapter_for(info, memory_budget_bytes=self.emission_need(n)), 100 * n)

    def test_the_extras_widen_both_emission_widths(self):
        """The extras example reads three more 8-byte datasets and emits three
        8-byte columns."""
        n = 1000
        extra_columns = {"Rvir": np.ones(n), "Spin": np.zeros(n), "pid": np.arange(n)}
        info = write_source(self.tmp, [{"forests": [chain_forest(n)]}], extra_columns=extra_columns)
        schema = load_schema("consistent_trees_hdf5_extras_example.yaml")
        need = self.emission_need(n, extra_raw=24, extra_canonical=24)
        with self.assertRaisesRegex(
            ConverterError, "176 B/row raw read plus 176 B/row canonical columns"
        ):
            list(adapter_for(info, schema, memory_budget_bytes=need - 1).iter_batches(n))
        collect(adapter_for(info, schema, memory_budget_bytes=need), n)

    def test_the_default_batch_size_fits_the_default_budget_for_the_shipped_profiles(self):
        """Pins the figures at the CLI defaults: ``--ingest-max-rows`` 1 << 20
        and the 2 GiB budget, for a source of at least that many halos."""
        import pipeline

        self.assertEqual(pipeline.DEFAULT_INGEST_MAX_ROWS, 1 << 20)
        self.assertEqual(ch.DEFAULT_MEMORY_BUDGET_BYTES, 2 * 1024**3)
        rows = pipeline.DEFAULT_INGEST_MAX_ROWS
        for profile, width, figure in (
            ("consistent_trees_hdf5.yaml", 152, 637_534_208),
            ("consistent_trees_hdf5_extras_example.yaml", 176, 738_197_504),
        ):
            with self.subTest(profile=profile):
                schema = load_schema(profile)
                self.assertEqual(topology.canonical_row_bytes(schema), width)
                self.assertEqual(2 * rows * (width + width), figure)
                self.assertLessEqual(figure, ch.DEFAULT_MEMORY_BUDGET_BYTES)
                topology.check_emission_budget(
                    schema, rows, rows, width, ch.DEFAULT_MEMORY_BUDGET_BYTES
                )

    def test_non_positive_or_fractional_budgets_are_rejected(self):
        info = write_source(self.tmp, [{"forests": [lone(1)]}])
        for bad in (0, -1, 1.5, True):
            with self.assertRaises(ConverterError):
                adapter_for(info, memory_budget_bytes=bad)


class BudgetAccountingTests(TempDirCase):
    """Re-measure the budget constants against the real code paths, so a
    constant that silently understates its path fails here."""

    @staticmethod
    def chain(n):
        """Worst validation shape: every halo has both a descendant and a
        FirstProgenitor."""
        return {
            "Descendant": np.arange(-1, n - 1, dtype="<i8"),
            "FirstProgenitor": np.where(np.arange(1, n + 1) < n, np.arange(1, n + 1), -1).astype(
                "<i8"
            ),
            "NextProgenitor": np.full(n, -1, dtype="<i8"),
            "FirstHaloInFOFgroup": np.arange(n, dtype="<i8"),
            "NextHaloInFOFgroup": np.full(n, -1, dtype="<i8"),
        }

    def write_chain(self, n, snap_float=True):
        path = os.path.join(self.tmp, "chain_{}.h5".format(n))
        links = self.chain(n)
        with h5py.File(path, "w") as handle:
            handle.attrs["Nfiles"] = np.int64(1)
            group = handle.create_group("File0")
            group.attrs["Nforests"] = np.int64(1)
            group.attrs["contiguous-halo-props"] = np.int8(1)
            group.create_dataset(
                "ForestInfo", data=np.array([(1, 0, n, 1)], dtype=FOREST_INFO_DTYPE)
            )
            data = group.create_group("Forests")
            for name, values in links.items():
                data.create_dataset(name, data=values)
            data.create_dataset("id", data=np.arange(n, dtype="<i8"))
            for name in FLOATS:
                data.create_dataset(name, data=np.full(n, 3.27e10))
            snaps = np.arange(n - 1, -1, -1)
            data.create_dataset("Snap_idx", data=snaps.astype("<f8" if snap_float else "<i8"))
        return path

    def measure_topology(self, n):
        adapter = adapter_for(self.write_chain(n), last_file=0)
        adapter.inventory()
        plan = adapter._plans[0]
        with adapter._open_info() as handle:
            datasets = adapter._reopen(handle, plan)
            tracemalloc.start()
            try:
                columns = adapter._read_forest_topology(datasets, plan, 0, n, "measure")
                topology.validate_tree(columns, n, "measure", None)
                del columns
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
        return peak

    def test_the_topology_path_stays_within_its_declared_budget(self):
        for n in (1, 10, 100, 1000, 70000, 200000):
            with self.subTest(n=n):
                peak = self.measure_topology(n)
                declared = (
                    n * VALIDATION_BYTES_PER_HALO
                    + min(TOPOLOGY_READ_CHUNK_ROWS, n) * TOPOLOGY_READ_BUFFER_BYTES_PER_ROW
                    + TOPOLOGY_BASE_BYTES
                )
                self.assertLessEqual(peak, declared)

    def measure_window(self, n):
        """Peak of the read-window route for a file holding one chain forest:
        the window read, the forest's topology from it and its validation."""
        adapter = adapter_for(self.write_chain(n), last_file=0)
        adapter.inventory()
        plan = adapter._plans[0]
        nonempty = np.flatnonzero(plan.counts > 0)
        ends = plan.offsets[nonempty] + plan.counts[nonempty]
        with adapter._open_info() as handle:
            datasets = adapter._reopen(handle, plan)
            tracemalloc.start()
            try:
                window = adapter._read_window(
                    datasets, plan, nonempty, ends, 0, TOPOLOGY_READ_CHUNK_ROWS
                )
                columns = adapter._window_topology(window, plan, 0, n, "measure")
                topology.validate_tree(columns, n, "measure", None)
                del columns
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
        return peak, window.read_buffer_bytes

    def test_the_window_path_stays_within_its_declared_budget(self):
        for n in (1, 10, 100, 1000, 20000, 60000):
            with self.subTest(n=n):
                peak, read_buffer_bytes = self.measure_window(n)
                self.assertLessEqual(
                    peak,
                    topology.validation_budget_bytes(n, read_buffer_bytes, TOPOLOGY_BASE_BYTES),
                )

    def test_the_read_buffer_constant_covers_the_measured_read(self):
        """The read phase alone, net of its 48 B/halo of retained columns."""
        n = 200000
        adapter = adapter_for(self.write_chain(n), last_file=0)
        adapter.inventory()
        plan = adapter._plans[0]
        with adapter._open_info() as handle:
            datasets = adapter._reopen(handle, plan)
            tracemalloc.start()
            try:
                adapter._read_forest_topology(datasets, plan, 0, n, "measure")
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
        transient = peak - 8 * len(ch.TOPOLOGY_COLUMNS) * n
        self.assertLessEqual(
            transient,
            TOPOLOGY_READ_CHUNK_ROWS * TOPOLOGY_READ_BUFFER_BYTES_PER_ROW + TOPOLOGY_BASE_BYTES,
        )

    def test_the_inventory_path_stays_within_its_declared_budget(self):
        for n_forests in (1, 5000, 24000, 60000):
            with self.subTest(n_forests=n_forests):
                path = os.path.join(self.tmp, "inv_{}.h5".format(n_forests))
                write_many_lone_forests(path, n_forests)
                adapter = adapter_for(path, last_file=0)
                tracemalloc.start()
                try:
                    adapter.inventory()
                    _, peak = tracemalloc.get_traced_memory()
                finally:
                    tracemalloc.stop()
                self.assertLessEqual(
                    peak, n_forests * INVENTORY_BYTES_PER_UNIT + INVENTORY_BASE_BYTES
                )


def write_many_lone_forests(path, n_forests):
    """One file of ``n_forests`` one-halo forests, written vectorised."""
    with h5py.File(path, "w") as handle:
        handle.attrs["Nfiles"] = np.int64(1)
        group = handle.create_group("File0")
        group.attrs["Nforests"] = np.int64(n_forests)
        group.attrs["contiguous-halo-props"] = np.int8(1)
        rows = np.arange(n_forests, dtype="<i8")
        table = np.zeros(n_forests, dtype=FOREST_INFO_DTYPE)
        table["ForestID"] = rows + 100
        table["ForestHalosOffset"] = rows
        table["ForestNhalos"] = 1
        table["ForestNtrees"] = 1
        group.create_dataset("ForestInfo", data=table)
        data = group.create_group("Forests")
        for name in LINKS:
            fill = 0 if name == "FirstHaloInFOFgroup" else -1
            data.create_dataset(name, data=np.full(n_forests, fill, dtype="<i8"))
        data.create_dataset("id", data=rows)
        for name in FLOATS:
            data.create_dataset(name, data=np.full(n_forests, 3.27e10))
        data.create_dataset("Snap_num", data=np.zeros(n_forests, dtype="<i8"))


# ==========================================================================
# Real micro-Uchuu forests-HDF5 (optional; read-only)
# ==========================================================================


@unittest.skipUnless(
    os.path.isfile(REAL_MICRO_UCHUU), "real micro-Uchuu forests-HDF5 is not present"
)
class RealMicroUchuuTests(unittest.TestCase):
    """Inventory and the first forests of the real 13 GB dataset, compared
    with direct h5py reads. The whole-dataset conversion is checked by the
    micro-uchuu-hdf5-horizontal parity gate; this is the adapter's own sampled check."""

    @classmethod
    def setUpClass(cls):
        cls.adapter = adapter_for(REAL_MICRO_UCHUU, last_file=0, max_snapshot=49)
        cls.inventory = cls.adapter.inventory()

    def test_inventory_and_dependencies(self):
        self.assertEqual(self.inventory.total_halos, 22580924)
        self.assertEqual(len(self.inventory.units), 440651)
        paths = [os.path.basename(d.identity.path) for d in self.adapter.dependencies()]
        self.assertEqual(paths, ["MicroUchuu_mergertree.h5", "MicroUchuu_mergertree_info.h5"])

    def test_first_rows_match_direct_reads(self):
        n_rows = 200000
        batch = next(self.adapter.iter_batches(n_rows))
        with h5py.File(REAL_MICRO_UCHUU, "r") as handle:
            info = handle["File0/ForestInfo"][()]
            forests = handle["File0/Forests"]
            raw = {name: forests[name][:n_rows] for name in LINKS + FLOATS + ("id", "Snap_num")}
        offsets = info["ForestHalosOffset"]
        counts = info["ForestNhalos"]
        self.assertEqual(int(offsets[0]), 0)
        forest_of_row = np.repeat(np.arange(len(counts)), counts)[:n_rows]
        base = offsets[forest_of_row] + 1  # ForestInfo is in storage order here
        for name in LINKS:
            local = raw[name]
            expected = np.where(local >= 0, base + local, -1)
            np.testing.assert_array_equal(batch.links[name], expected, err_msg=name)
        np.testing.assert_array_equal(batch.identity["ForestIndex"], forest_of_row)
        np.testing.assert_array_equal(batch.payload["MostBoundID"], raw["id"])
        np.testing.assert_array_equal(batch.payload["SnapNum"], raw["Snap_num"])
        np.testing.assert_array_equal(batch.payload["M_Crit200"], raw["Mvir"].astype(np.float32))
        sample = range(0, n_rows, 9973)
        self.assertEqual(
            [int(batch.payload["Len"][i]) for i in sample],
            [expected_len(float(raw["Mvir"][i])) for i in sample],
        )
        self.assertEqual(
            [float(batch.payload["Spin"][i, 1]) for i in sample],
            [expected_spin(float(raw["Jy"][i]), float(raw["Mvir"][i])) for i in sample],
        )


if __name__ == "__main__":
    unittest.main()
