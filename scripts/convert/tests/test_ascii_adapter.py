"""The Consistent-Trees ASCII canonical bridge
(scripts/convert/adapters/ctrees_ascii.py).

Two independent oracles:

- **literal expectations** written out from the fixture topology and source
  text -- source coordinates, ``SourceHaloID`` prefix sums, extra values and a
  hand-checked set of links;
- **the legacy ASCII-to-v2 route** on the same fixture, whose emitted HDF5 is
  joined to the canonical rows by ``(SnapNum, ForestIndex, HaloRankInForest)``
  -- never by row number, since canonical rows are in ``SourceHaloID`` order
  -- with links compared by the target halo's ctrees id.
"""

import os
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
from adapters import ctrees_ascii  # noqa: E402
from adapters.base import LINK_FIELDS  # noqa: E402
from adapters.ctrees_ascii import (  # noqa: E402
    CTreesAsciiAdapter,
    batch_term_bytes,
    prepare_workdir,
)
from column_schema import build_schema, load_column_map, parse_column_map  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixups import fixed_scratch_name, run_fixups  # noqa: E402
from hdf5_writer import run_write  # noqa: E402
from links import links_scratch_name, run_links  # noqa: E402
from scatter import Manifest, run_scatter, source_units_name  # noqa: E402
from sort_index import index_name, run_sort, sorted_scratch_name  # noqa: E402
from test_fixups import capture_stderr  # noqa: E402

PROFILE_DIR = Path(__file__).resolve().parents[1] / "profiles"
DEFAULT_PROFILE = PROFILE_DIR / "consistent_trees_ascii.yaml"
ALL_TYPES_PROFILE = Path(__file__).parent / "data" / "column_maps" / "ascii_all_extra_types.yaml"


def schema_of(path=ALL_TYPES_PROFILE):
    return build_schema(load_column_map(path))


def f32(text):
    return np.float64(text).astype(np.float32)


class Env:
    """Two source files over the standard forests: forest 100's two trees are
    split across the files, and each file interleaves several forests."""

    def __init__(self, root: Path, extra_columns=()):
        self.root = root
        self.forests = fixtures.standard_forests()
        multi, satellite, early, zero_mass, sub = self.forests
        self.file_trees = [
            [multi.trees[0], satellite.trees[0], early.trees[0]],
            [zero_mass.trees[0], multi.trees[1], sub.trees[0]],
        ]
        self.tree_files = [
            fixtures.write_ctrees_file(
                root / "tree_{}.dat".format(i), trees, extra_columns=extra_columns
            )
            for i, trees in enumerate(self.file_trees)
        ]
        self.forests_list = fixtures.write_forests_list(root / "forests.list", self.forests)
        self.a_list = fixtures.write_a_list(root / "test.a_list")
        self.sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
        self.halos = {h.halo_id: h for trees in self.file_trees for t in trees for h in t.halos}

    def prepare(self, schema, workdir="extended", **kwargs):
        with capture_stderr():
            prepare_workdir(
                schema,
                self.tree_files,
                self.forests_list,
                self.a_list,
                self.sim_info,
                self.root / workdir,
                **kwargs,
            )
        return self.root / workdir

    def legacy_v2(self, workdir="legacy"):
        path = self.root / workdir
        with capture_stderr():
            run_scatter(
                self.tree_files,
                self.forests_list,
                self.a_list,
                path,
                simulation_info_path=self.sim_info,
            )
            run_sort(path)
            run_fixups(path, self.a_list, self.sim_info)
            run_links(path)
            run_write(path, self.a_list, self.sim_info)
        return path / "hdf5"


def collect(adapter, max_rows=1000):
    """Concatenate every batch into one table of named columns."""
    batches = list(adapter.iter_batches(max_rows))
    table = {}
    for group in ("identity", "coordinates", "links", "payload", "extras"):
        for name in getattr(batches[0], group):
            table[name] = np.concatenate([getattr(b, group)[name] for b in batches])
    return batches, table


def expected_coordinates(env):
    """Literal source coordinates and SourceHaloIDs from the file layout:
    units are forests in order of first ``#tree`` marker per file, rows are
    numbered within their unit in file order, ids prefix-sum in (file, unit)."""
    coordinates = {}
    units_by_file = []
    for file_ordinal, trees in enumerate(env.file_trees):
        forest_of_root = {t.root_id: f.forest_id for f in env.forests for t in f.trees}
        unit_of_forest = {}
        counts = []
        for tree in trees:
            forest = forest_of_root[tree.root_id]
            if forest not in unit_of_forest:
                unit_of_forest[forest] = len(counts)
                counts.append(0)
            unit = unit_of_forest[forest]
            for halo in tree.halos:
                coordinates[halo.halo_id] = (file_ordinal, unit, counts[unit])
                counts[unit] += 1
        units_by_file.append(counts)
    base = {}
    next_id = 1
    for file_ordinal, counts in enumerate(units_by_file):
        for unit, count in enumerate(counts):
            base[(file_ordinal, unit)] = next_id
            next_id += count
    ids = {hid: base[(f, u)] + r for hid, (f, u, r) in coordinates.items()}
    return coordinates, ids


class TestCanonicalBridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.env = Env(Path(cls.tmp.name))
        cls.schema = schema_of()
        cls.workdir = cls.env.prepare(cls.schema, chunksize=2)
        cls.adapter = CTreesAsciiAdapter(cls.schema, cls.workdir)
        cls.batches, cls.table = collect(cls.adapter, max_rows=3)
        cls.row_of = {int(i): k for k, i in enumerate(cls.table["MostBoundID"])}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_source_coordinates_and_ids_are_literal(self):
        coordinates, ids = expected_coordinates(self.env)
        # spelled out for the split forest 100: tree 101 is file 0's unit 0,
        # tree 102 is file 1's unit 1 (after forest 500's tree 501)
        self.assertEqual(coordinates[1013], (0, 0, 3))
        self.assertEqual(coordinates[1020], (1, 1, 0))
        self.assertEqual(ids[1010], 1)
        self.assertEqual(ids[5010], 11)
        self.assertEqual(ids[1020], 13)
        for hid, k in self.row_of.items():
            got = tuple(
                int(self.table[n][k])
                for n in ("source_file_ordinal", "unit_ordinal", "row_ordinal")
            )
            self.assertEqual(got, coordinates[hid], hid)
            self.assertEqual(int(self.table["SourceHaloID"][k]), ids[hid], hid)
        self.assertEqual(sorted(self.table["SourceHaloID"].tolist()), list(range(1, 18)))

    def test_inventory_is_the_unit_prefix_order(self):
        inventory = self.adapter.inventory()
        self.assertEqual(
            [(u.source_file_ordinal, u.unit_ordinal, u.n_halos) for u in inventory.units],
            [(0, 0, 4), (0, 1, 4), (0, 2, 2), (1, 0, 2), (1, 1, 2), (1, 2, 3)],
        )
        self.assertEqual(inventory.total_halos, 17)

    def test_batches_are_bounded_single_snapshot_and_increasing(self):
        for batch in self.batches:
            self.assertLessEqual(batch.n_rows, 3)
            self.assertEqual(len(set(batch.payload["SnapNum"].tolist())), 1)
            self.assertTrue(np.all(np.diff(batch.identity["SourceHaloID"]) > 0))

    def test_emission_is_independent_of_batch_size(self):
        for max_rows in (1, 7, 10**6):
            _batches, table = collect(self.adapter, max_rows)
            for name, values in self.table.items():
                self.assertEqual(table[name].tobytes(), values.tobytes(), (max_rows, name))

    def test_each_snapshot_is_verified_once_per_pass(self):
        """The fixed and links scratch verified while a snapshot is the upcoming
        neighbour are the ones its batches come from: one checksum each per
        pass, and the emitted rows are unchanged."""
        real_verify = Manifest.verify_intermediate
        verified = []

        def recording_verify(manifest, path, what):
            verified.append((str(Path(path).resolve()), what))
            return real_verify(manifest, path, what)

        adapter = CTreesAsciiAdapter(self.schema, self.workdir)
        adapter.inventory()
        snapshots = Manifest.load_or_create(self.workdir).data["snapshots"]
        expected = sorted(
            (str(Path(entry[key]).resolve()), what)
            for entry in snapshots.values()
            for key, what in (
                ("fixed_file", "fixed snapshot scratch"),
                ("links_file", "snapshot links scratch"),
            )
        )
        with mock.patch.object(Manifest, "verify_intermediate", recording_verify):
            for n_pass in (1, 2):
                _batches, table = collect(adapter, max_rows=3)
                self.assertEqual(sorted(verified), sorted(expected * n_pass), n_pass)
                for name, values in self.table.items():
                    self.assertEqual(table[name].tobytes(), values.tobytes(), (n_pass, name))

    def test_default_batch_size_fits_a_budget_sized_to_the_snapshots(self):
        """A batch is charged at the snapshot's own row count, so the CLI's
        default of 1 << 20 rows no longer demands a full batch's bytes for a
        handful of halos: 512 MiB, below that full-batch charge, suffices."""
        budget = 512 * 1024**2
        self.assertGreater(batch_term_bytes(self.schema, 1 << 20), budget)
        adapter = CTreesAsciiAdapter(self.schema, self.workdir, memory_budget_bytes=budget)
        _batches, table = collect(adapter, max_rows=1 << 20)
        for name, values in self.table.items():
            self.assertEqual(table[name].tobytes(), values.tobytes(), name)

    def test_extras_carry_literal_source_values(self):
        for hid, k in self.row_of.items():
            halo = self.env.halos[hid]
            tree_root = next(t.root_id for ts in self.env.file_trees for t in ts if halo in t.halos)
            self.assertEqual(int(self.table["NumProg"][k]), halo.num_prog)
            self.assertEqual(int(self.table["HaloID"][k]), hid)
            self.assertEqual(int(self.table["TreeRoot"][k]), tree_root)
            self.assertEqual(self.table["CatalogRvir"][k].tobytes(), f32("150.0").tobytes())
            self.assertEqual(float(self.table["RvirDouble"][k]), 150.0)
            self.assertEqual(self.table["Counters"][k].tolist(), [halo.num_prog, 0, halo.snap])
            raw = np.array([f32("{:.5e}".format(v)) for v in (halo.jx, halo.jy, halo.jz)])
            self.assertEqual(self.table["RawJ"][k].tobytes(), raw.tobytes())
            if f32("{:.5e}".format(halo.mvir)) != 0:
                # core Spin is normalised, the RawJ extra is not
                self.assertNotEqual(self.table["Spin"][k].tobytes(), raw.tobytes())
            else:
                self.assertEqual(self.table["Spin"][k].tobytes(), raw.tobytes())

    def test_hand_checked_links(self):
        def target(hid, name):
            value = int(self.table[name][self.row_of[hid]])
            if value == -1:
                return -1
            k = int(np.nonzero(self.table["SourceHaloID"] == value)[0][0])
            return int(self.table["MostBoundID"][k])

        # mass tie 1011/1012 (6e11 each): the first encountered stays in front
        self.assertEqual(target(1010, "FirstProgenitor"), 1011)
        self.assertEqual(target(1011, "NextProgenitor"), 1012)
        self.assertEqual(target(1012, "NextProgenitor"), -1)
        self.assertEqual(target(1013, "Descendant"), 1011)
        # both independent FoF centrals of forest 100 survive as self-central
        self.assertEqual(target(1010, "FirstHaloInFOFgroup"), 1010)
        self.assertEqual(target(1020, "FirstHaloInFOFgroup"), 1020)
        # the sub-subhalo is re-hosted onto its ultimate central (fix_upid)
        self.assertEqual(target(6012, "FirstHaloInFOFgroup"), 6010)
        self.assertEqual(target(6010, "NextHaloInFOFgroup"), 6011)
        self.assertEqual(target(6011, "NextHaloInFOFgroup"), 6012)
        # a forest split across files keeps its cross-file descendant link
        self.assertEqual(target(1021, "Descendant"), 1020)
        self.assertEqual(target(1020, "FirstProgenitor"), 1021)
        self.assertEqual(target(4011, "Descendant"), 4010)

    def test_matches_the_legacy_v2_route_by_source_identity(self):
        hdf5_dir = self.env.legacy_v2()
        key = {
            (int(s), int(f), int(r)): k
            for k, (s, f, r) in enumerate(
                zip(
                    self.table["SnapNum"], self.table["ForestIndex"], self.table["HaloRankInForest"]
                )
            )
        }
        target_snap = {
            "Descendant": 1,
            "FirstProgenitor": -1,
            "NextProgenitor": 0,
            "FirstHaloInFOFgroup": 0,
            "NextHaloInFOFgroup": 0,
        }
        slabs = {}
        for path in sorted(hdf5_dir.glob("snapshot_*.h5")):
            with h5py.File(path, "r") as handle:
                snap = int(handle["header"].attrs["snapshot_number"])
                slabs[snap] = {name: handle["halos"][name][...] for name in handle["halos"]}
        sid_to_id = dict(
            zip(self.table["SourceHaloID"].tolist(), self.table["MostBoundID"].tolist())
        )
        matched = 0
        for snap, slab in slabs.items():
            for i in range(slab["MostBoundID"].size):
                k = key[(snap, int(slab["ForestIndex"][i]), int(slab["HaloRankInForest"][i]))]
                for name in (
                    "Len",
                    "SnapNum",
                    "M_Crit200",
                    "Pos",
                    "Vel",
                    "Spin",
                    "VelDisp",
                    "Vmax",
                    "MostBoundID",
                ):
                    self.assertEqual(
                        np.asarray(slab[name][i]).tobytes(),
                        np.asarray(self.table[name][k]).tobytes(),
                        (snap, name),
                    )
                for name in LINK_FIELDS:
                    row = int(slab[name][i])
                    link = int(self.table[name][k])
                    if row == -1:
                        self.assertEqual(link, -1, (snap, name))
                        continue
                    expected = int(slabs[snap + target_snap[name]]["MostBoundID"][row])
                    self.assertEqual(sid_to_id[link], expected, (snap, name))
                matched += 1
        self.assertEqual(matched, 17)

    def test_forest_records(self):
        records = list(self.adapter.iter_forests())
        self.assertEqual(
            [
                (r.forest_index, r.forest_id, r.source_file_ordinal, r.unit_ordinal, r.n_halos)
                for r in records
            ],
            [
                (0, 100, -1, -1, 6),  # spans both files
                (1, 200, 0, 1, 4),
                (2, 400, 0, 2, 2),
                (3, 500, 1, 0, 2),
                (4, 600, 1, 2, 3),
            ],
        )


class TestBridgeRuns(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_serial_and_pooled_preparations_emit_identical_batches(self):
        tables = []
        for pool_size in (1, 2):
            root = self.root / str(pool_size)
            root.mkdir()
            env = Env(root)
            workdir = env.prepare(schema_of(), pool_size=pool_size, chunksize=1)
            _b, table = collect(CTreesAsciiAdapter(schema_of(), workdir), 4)
            tables.append(table)
        for name in tables[0]:
            self.assertEqual(tables[0][name].tobytes(), tables[1][name].tobytes(), name)

    def test_resumed_default_run_is_a_skip_with_identical_output(self):
        env = Env(self.root)
        schema = schema_of(DEFAULT_PROFILE)
        workdir = env.prepare(schema)
        _b, first = collect(CTreesAsciiAdapter(schema, workdir))
        manifest_before = (workdir / "manifest.json").read_text()
        env.prepare(schema)
        self.assertEqual((workdir / "manifest.json").read_text(), manifest_before)
        _b, second = collect(CTreesAsciiAdapter(schema, workdir))
        self.assertEqual(set(first), set(second))
        self.assertEqual(
            sorted(second),
            sorted(
                [
                    "SourceHaloID",
                    "ForestIndex",
                    "HaloRankInForest",
                    "source_file_ordinal",
                    "unit_ordinal",
                    "row_ordinal",
                ]
                + list(LINK_FIELDS)
                + [f.name for f in schema.payload_fields]
            ),
        )
        for name in first:
            self.assertEqual(first[name].tobytes(), second[name].tobytes(), name)

    def test_resume_after_an_interrupted_preparation_matches_a_clean_one(self):
        env = Env(self.root)
        schema = schema_of()
        _b, clean = collect(CTreesAsciiAdapter(schema, env.prepare(schema, workdir="clean")))
        interrupted = self.root / "interrupted"
        with capture_stderr():
            run_scatter(
                env.tree_files,
                env.forests_list,
                env.a_list,
                interrupted,
                simulation_info_path=env.sim_info,
                schema=schema,
            )
            run_sort(interrupted, snapshots=[4, 5])
        env.prepare(schema, workdir="interrupted")
        _b, resumed = collect(CTreesAsciiAdapter(schema, interrupted))
        for name in clean:
            self.assertEqual(clean[name].tobytes(), resumed[name].tobytes(), name)

    def test_an_oversized_snapshot_is_refused_right_after_scatter(self):
        """The largest snapshot's batch term is refused as soon as scatter has
        counted it -- no snapshot is sorted, fixed or linked -- at its exact
        requirement, and a re-run under a budget that holds it resumes from
        the kept scatter output to the same batches as a clean preparation."""
        env = Env(self.root)
        schema = schema_of()
        max_rows = 2
        _b, clean = collect(CTreesAsciiAdapter(schema, env.prepare(schema, workdir="clean")))
        workdir = self.root / "early"

        for only in ({"max_rows": max_rows}, {"memory_budget_bytes": 1 << 30}):
            with self.subTest(only=only), self.assertRaisesRegex(ConverterError, "together"):
                env.prepare(schema, workdir="early", **only)
            self.assertFalse(workdir.exists())

        with mock.patch.object(ctrees_ascii, "run_sort", side_effect=AssertionError("sorted")):
            with self.assertRaisesRegex(ConverterError, "above the configured memory budget"):
                env.prepare(schema, workdir="early", max_rows=max_rows, memory_budget_bytes=1)
        rows_of = {
            int(snap): int(entry["rows"])
            for snap, entry in Manifest.load_or_create(workdir).data["snapshots"].items()
        }
        largest = max(rows_of.values())
        refused = min(snap for snap, n_rows in rows_of.items() if n_rows == largest)
        # all-types profile: 200-byte fixed record + 36 links + 152 canonical + 56 extras
        requirement = 2 * min(max_rows, largest) * (200 + 36 + 152 + 56)

        def statuses():
            data = Manifest.load_or_create(workdir).data
            return {entry["status"] for entry in data["snapshots"].values()}

        with self.assertRaisesRegex(
            ConverterError,
            r"^snapshot {} \({} halos, batches of {}\) needs {} bytes, above the configured "
            r"memory budget of {} bytes; raise memory_budget_bytes or lower max_rows$".format(
                refused, largest, max_rows, requirement, requirement - 1
            ),
        ):
            env.prepare(
                schema, workdir="early", max_rows=max_rows, memory_budget_bytes=requirement - 1
            )
        self.assertEqual(statuses(), {"concatenated"})
        later_stage_files = {
            name(snap)
            for snap in rows_of
            for name in (sorted_scratch_name, index_name, fixed_scratch_name, links_scratch_name)
        }
        self.assertEqual({path.name for path in workdir.rglob("*")} & later_stage_files, set())

        env.prepare(schema, workdir="early", max_rows=max_rows, memory_budget_bytes=requirement)
        self.assertEqual(statuses(), {"linked"})
        _b, resumed = collect(CTreesAsciiAdapter(schema, workdir))
        for name in clean:
            self.assertEqual(clean[name].tobytes(), resumed[name].tobytes(), name)

        # a linked workdir resumes through the link stage alone, as before
        manifest_before = (workdir / "manifest.json").read_text()
        env.prepare(schema, workdir="early", max_rows=max_rows, memory_budget_bytes=1)
        self.assertEqual((workdir / "manifest.json").read_text(), manifest_before)

    def test_resume_after_linking_rechecks_the_inputs(self):
        env = Env(self.root)
        schema = schema_of()
        workdir = env.prepare(schema)
        other_a_list = fixtures.write_a_list(
            self.root / "other.a_list", fixtures.A_LIST[:-1] + [0.99]
        )
        for label, kwargs in (
            ("source order", {"tree_files": list(reversed(env.tree_files))}),
            ("a_list", {"a_list_path": other_a_list}),
        ):
            with self.subTest(label):
                args = {
                    "tree_files": env.tree_files,
                    "forests_list_path": env.forests_list,
                    "a_list_path": env.a_list,
                    "simulation_info_path": env.sim_info,
                }
                args.update(kwargs)
                with self.assertRaisesRegex(ConverterError, "refusing to resume"):
                    prepare_workdir(schema, workdir=workdir, **args)
        with self.assertRaisesRegex(ConverterError, "refusing to resume"):
            env.prepare(schema_of(DEFAULT_PROFILE))

    def test_resume_after_linking_refuses_a_source_modified_in_place(self):
        """Same path, same order, different bytes: refused exactly as the
        legacy route's scatter refuses it, never silently accepted."""
        schema = schema_of()
        for label in ("size changes", "same size, new mtime"):
            with self.subTest(label), tempfile.TemporaryDirectory() as tmp:
                env = Env(Path(tmp))
                workdir = env.prepare(schema)
                manifest_before = (workdir / "manifest.json").read_bytes()
                path = env.tree_files[1]
                text = path.read_text()
                stat = path.stat()
                if label == "size changes":
                    path.write_text(text.replace("150.0", "150.25", 1))
                else:
                    # one digit of a Rvir token, same length; mtime moved on
                    path.write_text(text.replace("150.0", "151.0", 1))
                    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10**9))
                    self.assertEqual(path.stat().st_size, stat.st_size)
                with self.assertRaisesRegex(
                    ConverterError, "source file\\(s\\) changed after snapshots were finalized"
                ):
                    env.prepare(schema)
                self.assertEqual((workdir / "manifest.json").read_bytes(), manifest_before)
                # the legacy route refuses the same modification the same way
                legacy = Path(tmp) / "legacy"
                with capture_stderr():
                    path.write_text(text)
                    run_scatter(
                        env.tree_files,
                        env.forests_list,
                        env.a_list,
                        legacy,
                        simulation_info_path=env.sim_info,
                    )
                    path.write_text(text.replace("150.0", "150.25", 1))
                    with self.assertRaisesRegex(
                        ConverterError, "source file\\(s\\) changed after snapshots were finalized"
                    ):
                        run_scatter(
                            env.tree_files,
                            env.forests_list,
                            env.a_list,
                            legacy,
                            simulation_info_path=env.sim_info,
                        )

    def test_wide_schema(self):
        env = Env(self.root)
        data = {
            "schema_version": 1,
            "source_format": "consistent_trees_ascii",
            "required_columns": dict(load_column_map(DEFAULT_PROFILE).required_columns),
            "extra_fields": [
                {
                    "name": "Wide{:02d}".format(k),
                    "sources": [{"field": "Rvir"}],
                    "type": "double",
                    "units": "kpc/h",
                    "h_convention": "carried",
                    "description": "wide schema column {}".format(k),
                }
                for k in range(40)
            ],
        }
        data["required_columns"] = {role: list(a) for role, a in data["required_columns"].items()}
        schema = build_schema(parse_column_map(data, "<wide>"))
        workdir = env.prepare(schema)
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual(manifest.layout.dtype.itemsize, 132 + 40 * 8)
        _b, table = collect(CTreesAsciiAdapter(schema, workdir), 5)
        for k in range(40):
            self.assertTrue(np.all(table["Wide{:02d}".format(k)] == 150.0))

    def test_integers_above_2_53_survive_end_to_end(self):
        big = {1010: "9007199254740993", 2010: "-9223372036854775808", 6012: "9223372036854775807"}
        env = Env(self.root, extra_columns=[("Big", lambda h: big.get(h.halo_id, str(h.halo_id)))])
        data = {
            "schema_version": 1,
            "source_format": "consistent_trees_ascii",
            "required_columns": {
                role: list(a) for role, a in load_column_map(DEFAULT_PROFILE).required_columns
            },
            "extra_fields": [
                {
                    "name": "BigID",
                    "sources": [{"field": "Big"}],
                    "type": "long long",
                    "units": "dimensionless",
                    "h_convention": "none",
                    "description": "an exact 64-bit integer column",
                }
            ],
        }
        schema = build_schema(parse_column_map(data, "<big>"))
        _b, table = collect(CTreesAsciiAdapter(schema, env.prepare(schema, chunksize=3)))
        got = dict(zip(table["MostBoundID"].tolist(), table["BigID"].tolist()))
        for hid in env.halos:
            self.assertEqual(got[hid], int(big.get(hid, str(hid))))


class TestBridgeRefusals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.env = Env(Path(cls.tmp.name))
        cls.workdir = cls.env.prepare(schema_of())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_a_different_schema_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "refusing to read it as this schema"):
            CTreesAsciiAdapter(schema_of(DEFAULT_PROFILE), self.workdir).inventory()

    def test_a_non_ascii_schema_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "needs a 'consistent_trees_ascii' schema"):
            CTreesAsciiAdapter(schema_of(PROFILE_DIR / "consistent_trees_hdf5.yaml"), self.workdir)

    def test_missing_manifest_and_legacy_workdir_are_refused(self):
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            CTreesAsciiAdapter(schema_of(), self.env.root / "absent").inventory()
        with tempfile.TemporaryDirectory() as tmp:
            env = Env(Path(tmp))
            with capture_stderr():
                run_scatter(
                    env.tree_files,
                    env.forests_list,
                    env.a_list,
                    Path(tmp) / "w",
                    simulation_info_path=env.sim_info,
                )
            with self.assertRaisesRegex(ConverterError, "refusing to read"):
                CTreesAsciiAdapter(schema_of(), Path(tmp) / "w").inventory()

    def test_unfinished_preparation_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = Env(Path(tmp))
            with capture_stderr():
                run_scatter(
                    env.tree_files,
                    env.forests_list,
                    env.a_list,
                    Path(tmp) / "w",
                    simulation_info_path=env.sim_info,
                    schema=schema_of(),
                )
                run_sort(Path(tmp) / "w")
            with self.assertRaisesRegex(ConverterError, "not linked yet"):
                CTreesAsciiAdapter(schema_of(), Path(tmp) / "w").inventory()

    def test_bad_arguments_are_refused(self):
        adapter = CTreesAsciiAdapter(schema_of(), self.workdir)
        for bad in (0, True, 1.5):
            with self.subTest(max_rows=bad), self.assertRaises(ConverterError):
                next(adapter.iter_batches(bad))
        for bad in (0, -1, 2.0, True):
            with self.subTest(budget=bad), self.assertRaises(ConverterError):
                CTreesAsciiAdapter(schema_of(), self.workdir, memory_budget_bytes=bad)

    def test_forest_view_budget_is_checked_before_the_table_is_loaded(self):
        adapter = CTreesAsciiAdapter(schema_of(), self.workdir)
        adapter.inventory()
        adapter.memory_budget_bytes = 16
        loads = []
        real_load = np.load

        def recording_load(path, *args, **kwargs):
            loads.append((Path(path).name, kwargs.get("mmap_mode")))
            return real_load(path, *args, **kwargs)

        with mock.patch.object(ctrees_ascii.np, "load", recording_load):
            with self.assertRaisesRegex(
                ConverterError, "forest sidecar view .* above the configured"
            ):
                list(adapter.iter_forests())
        table_loads = [mode for name, mode in loads if name == "forest_index_table.npy"]
        self.assertEqual(table_loads, ["r"])

    def test_budget_is_checked_before_allocation(self):
        tiny = CTreesAsciiAdapter(schema_of(), self.workdir, memory_budget_bytes=1024)
        with self.assertRaisesRegex(ConverterError, "source inventory .* above the configured"):
            tiny.inventory()
        # the inventory is built under an ample budget, then the ceiling drops
        # to enough for the bitset but not for one snapshot's bounded read
        small = CTreesAsciiAdapter(schema_of(), self.workdir)
        small.inventory()
        small.memory_budget_bytes = (
            ctrees_ascii.SOURCE_ID_READ_ROWS * ctrees_ascii.SOURCE_ID_READ_BYTES_PER_ROW
        )
        with self.assertRaisesRegex(ConverterError, "snapshot .* above the configured"):
            next(small.iter_batches(10**5))

    def test_an_oversized_snapshot_is_refused_at_its_exact_requirement(self):
        """The largest snapshot's figure, rebuilt from literal widths (the
        all-types profile's 200-byte fixed record, 36-byte links record,
        152 canonical bytes and 56 extra bytes per row), is the exact
        boundary: one byte less is refused naming the snapshot, and exactly
        that much converts."""
        schema = schema_of()
        rows_of = {
            int(snap): int(entry["rows"])
            for snap, entry in Manifest.load_or_create(self.workdir).data["snapshots"].items()
        }
        max_rows = 2

        def requirement(snap):
            n_rows = rows_of[snap]
            neighbours = rows_of.get(snap - 1, 0) + rows_of.get(snap + 1, 0)
            return (
                8 * (neighbours + n_rows)
                + 16 * n_rows
                + 65536 * 40
                + 2 * min(max_rows, n_rows) * (200 + 36 + 152 + 56)
            )

        worst = max(requirement(snap) for snap in rows_of)
        refused = min(snap for snap in rows_of if requirement(snap) == worst)
        adapter = CTreesAsciiAdapter(schema, self.workdir)
        adapter.inventory()
        adapter.memory_budget_bytes = worst - 1
        with self.assertRaisesRegex(
            ConverterError,
            r"^snapshot {} \({} halos, neighbours \d+ and \d+, batches of {}\) needs {} bytes, "
            r"above the configured memory budget of {} bytes; raise memory_budget_bytes or "
            r"lower max_rows$".format(refused, rows_of[refused], max_rows, worst, worst - 1),
        ):
            list(adapter.iter_batches(max_rows))
        adapter.memory_budget_bytes = worst
        self.assertEqual(
            sum(batch.n_rows for batch in adapter.iter_batches(max_rows)), sum(rows_of.values())
        )

    def test_batch_term_is_pinned_to_the_record_widths(self):
        default, all_types = schema_of(DEFAULT_PROFILE), schema_of()
        # default profile: 144-byte fixed record + 36 links + 152 canonical
        self.assertEqual(batch_term_bytes(default, 1 << 20), 2 * (1 << 20) * 332)
        self.assertEqual(batch_term_bytes(default, 1 << 20, 4), 2 * 4 * 332)
        self.assertEqual(batch_term_bytes(default, 3, 0), 0)
        # all-types profile: 200-byte fixed record + 36 + 152 + 56 extra bytes
        self.assertEqual(batch_term_bytes(all_types, 10, 3), 2 * 3 * 444)
        self.assertEqual(batch_term_bytes(all_types, 2, 5), 2 * 2 * 444)
        self.assertEqual(batch_term_bytes(all_types, 7), batch_term_bytes(all_types, 7, 7))
        for bad in (0, True, 1.5):
            with self.subTest(max_rows=bad), self.assertRaises(ConverterError):
                batch_term_bytes(default, bad)
        for bad in (-1, 1.5, True):
            with self.subTest(n_rows=bad), self.assertRaises(ConverterError):
                batch_term_bytes(default, 4, bad)
        with self.assertRaisesRegex(ConverterError, "consistent_trees_ascii schema"):
            batch_term_bytes(schema_of(PROFILE_DIR / "consistent_trees_hdf5.yaml"), 4)

    def test_tampered_unit_sidecar_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = Env(Path(tmp))
            workdir = env.prepare(schema_of())
            sidecar = workdir / "scratch" / source_units_name(1)
            table = np.load(sidecar)
            table[0, 1] += 1
            np.save(sidecar, table)
            with self.assertRaisesRegex(ConverterError, "checksum"):
                CTreesAsciiAdapter(schema_of(), workdir).inventory()

    def test_forged_coordinates_are_caught(self):
        """Re-registered (so checksums pass) fixed records whose coordinates
        collide or leave the inventory: the adapter's own checks catch both."""
        from fixups import fixed_layout

        for field, value, message in (
            ("src_row_ordinal", 0, "occurs more than once|emitted twice"),
            ("src_unit_ordinal", 99, "outside the source inventory"),
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                env = Env(Path(tmp))
                workdir = env.prepare(schema_of())
                manifest = Manifest.load_or_create(workdir)
                dtype, _tag = fixed_layout(manifest.layout)
                entry = manifest.data["snapshots"]["5"]
                records = np.fromfile(entry["fixed_file"], dtype=dtype)
                records[field][:] = value
                records.tofile(entry["fixed_file"])
                manifest.register_intermediate(
                    entry["fixed_file"],
                    "snapshot-fixed",
                    rows=int(records.size),
                    dtype_tag=manifest.data["intermediates"][entry["fixed_file"]]["dtype_tag"],
                )
                manifest.save()
                with self.assertRaisesRegex(ConverterError, message):
                    list(CTreesAsciiAdapter(schema_of(), workdir).iter_batches(100))


class BudgetAccountingTests(unittest.TestCase):
    """Re-measure the real inventory path so INVENTORY_BYTES_PER_UNIT and
    INVENTORY_BASE_BYTES cannot silently drift below what it allocates."""

    def test_inventory_peak_is_within_the_declared_terms(self):
        schema = schema_of(DEFAULT_PROFILE)
        for n_units in (2, 3000):
            with self.subTest(n_units=n_units), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                forests = [
                    fixtures.ForestSpec(
                        forest_id=10 + k,
                        trees=[
                            fixtures.TreeSpec(
                                root_id=10 + k,
                                halos=[fixtures.HaloSpec(halo_id=10 + k, snap=5, mvir=1.0e11)],
                            )
                        ],
                    )
                    for k in range(n_units)
                ]
                trees = fixtures.all_trees(forests)
                half = len(trees) // 2
                files = [
                    fixtures.write_ctrees_file(root / "t0.dat", trees[:half]),
                    fixtures.write_ctrees_file(root / "t1.dat", trees[half:]),
                ]
                with capture_stderr():
                    prepare_workdir(
                        schema,
                        files,
                        fixtures.write_forests_list(root / "forests.list", forests),
                        fixtures.write_a_list(root / "a"),
                        fixtures.write_simulation_info(root / "s.yaml"),
                        root / "w",
                    )
                adapter = CTreesAsciiAdapter(schema, root / "w")
                tracemalloc.start()
                try:
                    base = tracemalloc.get_traced_memory()[0]
                    adapter.inventory()
                    peak = tracemalloc.get_traced_memory()[1] - base
                finally:
                    tracemalloc.stop()
                self.assertEqual(len(adapter.inventory().units), n_units)
                self.assertLessEqual(
                    peak,
                    ctrees_ascii.INVENTORY_BASE_BYTES
                    + n_units * ctrees_ascii.INVENTORY_BYTES_PER_UNIT,
                )


if __name__ == "__main__":
    unittest.main()
