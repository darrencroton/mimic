"""The Consistent-Trees ASCII canonical bridge
(convert/mimic-convert/adapters/ctrees_ascii.py).

Two oracles:

- **literal expectations** written out from the fixture topology and source
  text -- the (``ForestIndex``, ``HaloRankInForest``) position each halo's
  ``SourceHaloID`` is, extra values and a hand-checked set of links;
- **the link stage's own scratch** (:func:`link_stage_slabs`): the fixed and
  links records the reference reconstruction left in the prepared workdir,
  whose row-index links are in the stage's ascending-id slab order. They are
  joined to the canonical rows by ``(SnapNum, ForestIndex, HaloRankInForest)``
  -- never by row number, since canonical rows are in ``SourceHaloID`` order
  -- with links compared by the target halo's ctrees id, so the adapter's and
  the transpose's re-expression of the stage's output is checked against the
  output itself. The link stage is checked against hand-computed golden links
  in ``test_links``.

Under a declared cut table the oracle is the dataset rewriter: on
:mod:`fixtures`' adversarial cut forests, the route's conversion under the
table must equal the rewriter's cut of the route's uncut output in every
``/halos`` column, header attribute and sidecar value, with the same
promotions per snapshot.
"""

import json
import os
import sys
import tempfile
import tracemalloc
import unittest
from dataclasses import astuple
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import conversion_manifest as cm  # noqa: E402
import fixtures  # noqa: E402
import pipeline  # noqa: E402
from adapters import ctrees_ascii  # noqa: E402
from adapters.base import LINK_FIELDS, SourceInventory, SourceUnit  # noqa: E402
from adapters.ctrees_ascii import (  # noqa: E402
    CTreesAsciiAdapter,
    batch_term_bytes,
    prepare_workdir,
)
from column_schema import build_schema, load_column_map, parse_column_map  # noqa: E402
from conversion_manifest import inventory_record  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    CUT_FIXTURES,
    DEFAULT_PROFILE,
    PROFILE_DIR,
    assert_datasets_equal,
    capture_stderr,
    convert_under_cut,
    cut_companions,
    cut_layout,
    cut_source,
    quiet_output,
    read_sidecar,
    rewrite_cut,
    route_parameters,
    schema_of,
    tree_state,
    write_table,
)
from fixups import fixed_layout, fixed_scratch_name, run_fixups  # noqa: E402
from hdf5_writer_v3 import HorizontalV3Writer  # noqa: E402
from links import LINKS_RECORD_DTYPE, links_scratch_name  # noqa: E402
from scatter import Manifest, run_scatter, source_units_name, tree_rows_name  # noqa: E402
from sort_index import index_name, run_sort, sorted_scratch_name  # noqa: E402


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


def link_stage_slabs(workdir):
    """``{snap: {column: array}}`` of a prepared workdir's link-stage output:
    each snapshot's fixed records and the row-aligned links records the link
    stage wrote for them, in the stage's ascending-id slab order, under the
    ``/halos`` names the canonical batches use. Row-index links target the
    same slab, except ``Descendant`` (N+1) and ``FirstProgenitor`` (N-1)."""
    manifest = Manifest.load(workdir)
    fixed_dtype = fixed_layout(manifest.layout)[0]
    slabs = {}
    for snap, entry in manifest.data["snapshots"].items():
        fixed = np.fromfile(entry["fixed_file"], dtype=fixed_dtype)
        linked = np.fromfile(entry["links_file"], dtype=LINKS_RECORD_DTYPE)
        assert fixed.size == linked.size, snap
        slab = {name: linked[name] for name in LINK_FIELDS}
        slab.update(
            {
                "ForestIndex": linked["ForestIndex"],
                "HaloRankInForest": linked["HaloRankInForest"],
                "Len": fixed["Len"],
                "SnapNum": np.full(fixed.size, int(snap), dtype=np.int32),
                "M_Crit200": fixed["Mvir"],
                "Pos": np.column_stack((fixed["X"], fixed["Y"], fixed["Z"])),
                "Vel": np.column_stack((fixed["VX"], fixed["VY"], fixed["VZ"])),
                "Spin": np.column_stack((fixed["Jx"], fixed["Jy"], fixed["Jz"])),
                "VelDisp": fixed["vrms"],
                "Vmax": fixed["vmax"],
                "MostBoundID": fixed["MostBoundID"],
            }
        )
        slabs[int(snap)] = slab
    return slabs


def collect(adapter, max_rows=1000):
    """Concatenate every batch into one table of named columns."""
    batches = list(adapter.iter_batches(max_rows))
    table = {}
    for group in ("identity", "coordinates", "links", "payload", "extras"):
        for name in getattr(batches[0], group):
            table[name] = np.concatenate([getattr(b, group)[name] for b in batches])
    return batches, table


#: ``SourceHaloID`` of every halo of :class:`Env`, listed by hand: forests in
#: ascending id (``ForestIndex`` 0..4 = forests 100, 200, 400, 500, 600, with
#: 6, 4, 2, 2 and 3 halos over all snapshots, so bases 1, 7, 11, 13, 15), each
#: forest's halos in reference vertical order (snapshot descending, then upid,
#: pid and id ascending over post-fix-up hosts). Forest 100's two trees lie in
#: different files, which no longer matters to its ids.
LITERAL_IDS = {
    1010: 1,  # forest 100, snap 5: two independent centrals, id order
    1020: 2,
    1011: 3,  # snap 4: three centrals
    1012: 4,
    1021: 5,
    1013: 6,  # snap 3
    2010: 7,  # forest 200, snap 5: central before its satellite
    2011: 8,
    2012: 9,  # snap 4: central before its satellite
    2013: 10,
    4010: 11,  # forest 400
    4011: 12,
    5010: 13,  # forest 500: the zero-mass satellite after its central
    5011: 14,
    6010: 15,  # forest 600: the sub-subhalo 6012 re-hosted to 6010, after 6011
    6011: 16,
    6012: 17,
}

#: The halo counts of the five forests, in ``ForestIndex`` order.
FOREST_COUNTS = [6, 4, 2, 2, 3]


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
        inventory = self.adapter.inventory()
        self.assertEqual(
            {hid: int(self.table["SourceHaloID"][k]) for hid, k in self.row_of.items()},
            LITERAL_IDS,
        )
        for hid, k in self.row_of.items():
            forest = int(self.table["ForestIndex"][k])
            rank = int(self.table["HaloRankInForest"][k])
            # the id is the (ForestIndex, HaloRankInForest) position ...
            self.assertEqual(LITERAL_IDS[hid], 1 + sum(FOREST_COUNTS[:forest]) + rank, hid)
            self.assertEqual(LITERAL_IDS[hid], inventory.base_id(0, forest) + rank, hid)
            # ... the batch carries the (0, forest, rank) shim coordinate, and the
            # inventory inverts the id to exactly that
            got = tuple(
                int(self.table[n][k])
                for n in ("source_file_ordinal", "unit_ordinal", "row_ordinal")
            )
            self.assertEqual(got, (0, forest, rank), hid)
            self.assertEqual(
                astuple(inventory.coordinate(LITERAL_IDS[hid])), (0, forest, rank), hid
            )
        # every forest's rows are contiguous ids in rank order
        for forest, count in enumerate(FOREST_COUNTS):
            rows = np.nonzero(self.table["ForestIndex"] == forest)[0]
            by_rank = rows[np.argsort(self.table["HaloRankInForest"][rows])]
            self.assertEqual(self.table["HaloRankInForest"][by_rank].tolist(), list(range(count)))
            first = 1 + sum(FOREST_COUNTS[:forest])
            self.assertEqual(
                self.table["SourceHaloID"][by_rank].tolist(), list(range(first, first + count))
            )
        self.assertEqual(sorted(self.table["SourceHaloID"].tolist()), list(range(1, 18)))

    def test_inventory_is_one_unit_per_forest_in_forest_index_order(self):
        inventory = self.adapter.inventory()
        self.assertEqual(
            [(u.source_file_ordinal, u.unit_ordinal, u.n_halos) for u in inventory.units],
            [(0, forest, count) for forest, count in enumerate(FOREST_COUNTS)],
        )
        self.assertEqual(inventory.total_halos, 17)
        self.assertIs(inventory.physical_units, False)
        # the record declares no physical units, and its digest is the
        # per-forest one, not the physical (file, unit) one
        record = inventory_record(inventory)
        self.assertEqual(record["files"], [])
        self.assertEqual(record["n_units"], 5)
        physical = SourceInventory(
            [
                SourceUnit(f, u, n)
                for (f, u), n in zip(
                    [(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2)], [4, 4, 2, 2, 2, 3]
                )
            ]
        )
        self.assertNotEqual(record["units_sha256"], inventory_record(physical)["units_sha256"])

    def test_the_inventory_reads_no_halo_row(self):
        """Per-forest totals come from the unit sidecars and the forest table
        alone: no fixed or links scratch is opened."""
        adapter = CTreesAsciiAdapter(self.schema, self.workdir)
        verified = []
        real_verify = Manifest.verify_intermediate

        def recording_verify(manifest, path, what):
            verified.append(Path(path).name)
            return real_verify(manifest, path, what)

        unread = AssertionError("a snapshot was read")
        with mock.patch.object(Manifest, "verify_intermediate", recording_verify):
            with mock.patch.object(CTreesAsciiAdapter, "_open_snapshot", side_effect=unread):
                adapter.inventory()
        self.assertEqual(
            sorted(verified),
            sorted(["forest_index_table.npy", source_units_name(0), source_units_name(1)]),
        )

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

    def test_matches_the_link_stage_output_by_source_identity(self):
        """Every canonical row is one row of the link stage's output, with
        every payload value byte-identical and every link resolving to the
        same halo."""
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
        slabs = link_stage_slabs(self.workdir)
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


class TransposedTopologyTests(unittest.TestCase):
    """The forest-rank ids reorder every slab, and the row-index links of the
    transposed slabs follow the new order: every link of every halo must still
    resolve to the same halo -- by target ``(SnapNum, MostBoundID)``, not by
    integer -- as the link stage's own row-index links do in its ascending-id
    slab order (:func:`link_stage_slabs`), so payload, logical topology and
    chain order are unchanged while the numeric row indices move."""

    LINK_SNAPSHOT = {
        "Descendant": "DescendantSnapshot",
        "FirstProgenitor": "FirstProgenitorSnapshot",
        "NextProgenitor": "NextProgenitorSnapshot",
        "FirstHaloInFOFgroup": None,
        "NextHaloInFOFgroup": None,
    }

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.env = Env(Path(cls.tmp.name))
        schema = schema_of(DEFAULT_PROFILE)
        work = Path(cls.tmp.name) / "generic"
        parameters = {
            "tree_files": [str(path) for path in cls.env.tree_files],
            "forests_list": str(cls.env.forests_list),
            "simulation_info": str(cls.env.sim_info),
        }
        pipeline.initialize(
            work,
            schema,
            parameters,
            cls.env.a_list,
            ingest_max_rows=3,
            transpose_budget_bytes=4 << 20,
        )
        with capture_stderr():
            pipeline.run_ingest(work)
            pipeline.run_transpose(work)
        manifest = cm.ConversionManifest.load(work)
        dtype = pipeline.transpose_module.output_dtype(manifest.schema)
        cls.slabs = {}
        for relpath in manifest.stage("transpose")["artifacts"]:
            entry = manifest.artifact(relpath)
            rows = np.fromfile(manifest.artifact_path(relpath), dtype=dtype)
            if rows.size:
                cls.slabs[int(entry["snapshot"])] = rows
        cls.stage = {
            snap: slab
            for snap, slab in link_stage_slabs(work / pipeline.ASCII_PREPARATION_DIR).items()
            if slab["MostBoundID"].size
        }

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def logical_links(self, slabs, target_snapshot):
        """``{(snap, MostBoundID): {link: (target snap, target MostBoundID)}}``."""
        table = {}
        for snap, slab in slabs.items():
            for row in range(slab["MostBoundID"].size):
                links = {}
                for name in LINK_FIELDS:
                    target = int(slab[name][row])
                    if target == -1:
                        links[name] = None
                        continue
                    at = target_snapshot(slab, name, row, snap)
                    links[name] = (at, int(slabs[at]["MostBoundID"][target]))
                table[(snap, int(slab["MostBoundID"][row]))] = links
        return table

    def test_slabs_are_forest_blocked_in_id_order_and_reordered(self):
        for snap, slab in self.slabs.items():
            ids = slab["SourceHaloID"]
            self.assertTrue(bool(np.all(np.diff(ids) > 0)), snap)
            key = list(zip(slab["ForestIndex"].tolist(), slab["HaloRankInForest"].tolist()))
            self.assertEqual(key, sorted(key), snap)
            for k, hid in enumerate(slab["MostBoundID"].tolist()):
                self.assertEqual(int(ids[k]), LITERAL_IDS[hid], (snap, hid))
        # under physical (file, unit, row) ids, 1020 (forest 100's second
        # tree, file 1's unit 1) would hold id 13, after forest 200 (file 0)
        # and forest 500 (file 1's unit 0); under the forest-rank convention it
        # follows forest 100's first central directly, so snapshot 5's rows
        # reorder
        self.assertEqual(
            self.slabs[5]["MostBoundID"].tolist(),
            [1010, 1020, 2010, 2011, 5010, 5011, 6010, 6011, 6012],
        )

    def test_every_link_resolves_to_the_same_halo_as_the_link_stage(self):
        target_offset = {
            "Descendant": 1,
            "FirstProgenitor": -1,
            "NextProgenitor": 0,
            "FirstHaloInFOFgroup": 0,
            "NextHaloInFOFgroup": 0,
        }

        def v3_target(slab, name, row, snap):
            column = self.LINK_SNAPSHOT[name]
            return snap if column is None else int(slab[column][row])

        def stage_target(slab, name, row, snap):
            return snap + target_offset[name]

        converted = self.logical_links(self.slabs, v3_target)
        reference = self.logical_links(self.stage, stage_target)
        self.assertEqual(len(converted), 17)
        self.assertEqual(converted, reference)
        # every payload value is unchanged for the same halo
        for snap, slab in self.slabs.items():
            stage = self.stage[snap]
            at = {int(h): k for k, h in enumerate(stage["MostBoundID"])}
            for row, hid in enumerate(slab["MostBoundID"].tolist()):
                for name in ("Len", "M_Crit200", "Pos", "Vel", "Spin", "VelDisp", "Vmax"):
                    self.assertEqual(
                        np.asarray(slab[name][row]).tobytes(),
                        np.asarray(stage[name][at[hid]]).tobytes(),
                        (snap, hid, name),
                    )


class TestBridgeRuns(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """One clean preparation of the standard sources, read-only for the tests that use
        it: a prepared workdir names its files by absolute path, so a test that mutates
        one prepares its own."""
        cls.shared = tempfile.TemporaryDirectory()
        cls.env = Env(Path(cls.shared.name))
        cls.schema = schema_of()
        cls.clean_workdir = cls.env.prepare(cls.schema, workdir="clean")
        _b, cls.clean = collect(CTreesAsciiAdapter(cls.schema, cls.clean_workdir))

    @classmethod
    def tearDownClass(cls):
        cls.shared.cleanup()

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
        env, schema, clean = self.env, self.schema, self.clean
        interrupted = env.root / "interrupted"
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
        env, schema, clean = self.env, self.schema, self.clean
        max_rows = 2
        workdir = env.root / "early"

        for only in ({"max_rows": max_rows}, {"memory_budget_bytes": 1 << 30}):
            with self.subTest(only=only), self.assertRaisesRegex(ConverterError, "together"):
                env.prepare(schema, workdir="early", **only)
            self.assertFalse(workdir.exists())

        with mock.patch.object(ctrees_ascii, "run_sort", side_effect=AssertionError("sorted")):
            with self.assertRaisesRegex(ConverterError, "above the configured memory budget"):
                env.prepare(schema, workdir="early", max_rows=max_rows, memory_budget_bytes=1)
        rows_of = {
            int(snap): int(entry["rows"])
            for snap, entry in Manifest.load(workdir).data["snapshots"].items()
        }
        largest = max(rows_of.values())
        refused = min(snap for snap, n_rows in rows_of.items() if n_rows == largest)
        requirement = batch_term_bytes(schema, max_rows, largest)

        def statuses():
            data = Manifest.load(workdir).data
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
        env, schema, workdir = self.env, self.schema, self.clean_workdir
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
            env.prepare(schema_of(DEFAULT_PROFILE), workdir=workdir.name)

    def test_resume_after_linking_refuses_a_source_modified_in_place(self):
        """Same path, same order, different bytes: refused exactly as scatter
        itself refuses it on resume, never silently accepted."""
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
                # scatter refuses the same modification the same way (shown on
                # a separate workdir)
                separate = Path(tmp) / "separate"
                with capture_stderr():
                    path.write_text(text)
                    run_scatter(
                        env.tree_files,
                        env.forests_list,
                        env.a_list,
                        separate,
                        schema,
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
                            separate,
                            schema,
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
        manifest = Manifest.load(workdir)
        self.assertEqual(manifest.layout.dtype.itemsize, 108 + 40 * 8)
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
        hdf5_schema = schema_of(PROFILE_DIR / "consistent_trees_hdf5.yaml")
        with self.assertRaisesRegex(ConverterError, "needs a 'consistent_trees_ascii' schema"):
            CTreesAsciiAdapter(hdf5_schema, self.workdir)
        with self.assertRaisesRegex(ConverterError, "consistent_trees_ascii schema"):
            batch_term_bytes(hdf5_schema, 4)

    def test_a_missing_manifest_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            CTreesAsciiAdapter(schema_of(), self.env.root / "absent").inventory()

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
            with self.subTest(batch_term_max_rows=bad), self.assertRaises(ConverterError):
                batch_term_bytes(schema_of(DEFAULT_PROFILE), bad)
        for bad in (-1, 1.5, True):
            with self.subTest(batch_term_n_rows=bad), self.assertRaises(ConverterError):
                batch_term_bytes(schema_of(DEFAULT_PROFILE), 4, bad)
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
        # the first term charged is the manifest load, refused before it is read
        with self.assertRaisesRegex(
            ConverterError, "^loading the conversion manifest .* above the configured"
        ):
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
        all-types profile's 176-byte fixed record, 36-byte links record,
        152 canonical bytes and 56 extra bytes per row), is the exact
        boundary: one byte less is refused naming the snapshot, and exactly
        that much converts."""
        schema = schema_of()
        rows_of = {
            int(snap): int(entry["rows"])
            for snap, entry in Manifest.load(self.workdir).data["snapshots"].items()
        }
        max_rows = 2

        def requirement(snap):
            n_rows = rows_of[snap]
            neighbours = rows_of.get(snap - 1, 0) + rows_of.get(snap + 1, 0)
            return (
                8 * (neighbours + n_rows)
                + 16 * n_rows
                + 65536 * 40
                + 2 * min(max_rows, n_rows) * (176 + 36 + 152 + 56)
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

    @staticmethod
    def _forge(workdir, snap, key, kind, dtype, edit):
        """Rewrite one snapshot's scratch record file through ``edit`` and
        re-register it, so its checksum passes and only the adapter's own
        checks stand between the forgery and the batches."""
        manifest = Manifest.load(workdir)
        entry = manifest.data["snapshots"][str(snap)]
        records = np.fromfile(entry[key], dtype=dtype)
        edit(records)
        records.tofile(entry[key])
        manifest.register_intermediate(
            entry[key],
            kind,
            rows=int(records.size),
            dtype_tag=manifest.data["intermediates"][entry[key]]["dtype_tag"],
        )
        manifest.save()

    def test_forged_identities_are_caught(self):
        """Re-registered links records whose ``ForestIndex`` or
        ``HaloRankInForest`` collide or leave the inventory: the adapter's own
        range and uniqueness checks catch each, naming the snapshot, halo id,
        forest and rank."""
        from links import LINKS_RECORD_DTYPE

        def set_field(field, value):
            def edit(records):
                records[field][:] = value

            return edit

        for label, edit, message in (
            (
                "every rank 0",
                set_field("HaloRankInForest", 0),
                r"^snapshot 5: SourceHaloID 1 occurs more than once -- two rows hold the same "
                r"\(ForestIndex 0, HaloRankInForest 0\) position$",
            ),
            (
                "forest outside the table",
                set_field("ForestIndex", 99),
                r"^snapshot 5: halo id \d+ has ForestIndex 99 and HaloRankInForest \d+ outside "
                r"the source inventory \(the inventory holds 5 forest\(s\)\)$",
            ),
            (
                "rank at the forest's count",
                set_field("HaloRankInForest", 6),
                r"^snapshot 5: halo id 1010 has ForestIndex 0 and HaloRankInForest 6 outside "
                r"the source inventory \(forest 0 holds 6 halo\(s\)\)$",
            ),
            (
                "negative rank",
                set_field("HaloRankInForest", -1),
                r"^snapshot 5: halo id 1010 has ForestIndex 0 and HaloRankInForest -1 outside",
            ),
        ):
            with self.subTest(label), tempfile.TemporaryDirectory() as tmp:
                env = Env(Path(tmp))
                workdir = env.prepare(schema_of())
                self._forge(workdir, 5, "links_file", "snapshot-links", LINKS_RECORD_DTYPE, edit)
                with self.assertRaisesRegex(ConverterError, message):
                    list(CTreesAsciiAdapter(schema_of(), workdir).iter_batches(100))

    def test_scratch_disagreeing_with_its_recorded_rows_is_refused(self):
        """The fixed and links records of a snapshot are joined row by row, so
        either file one record short of the snapshot's recorded rows is
        refused before a batch is built. Truncated in place, the file fails its
        registered checksum; truncated and re-registered, so the checksum
        passes, it fails the adapter's own size check against the recorded
        rows."""
        for key, kind in (("fixed_file", "snapshot-fixed"), ("links_file", "snapshot-links")):
            for reregister in (False, True):
                with self.subTest(key=key, reregister=reregister):
                    with tempfile.TemporaryDirectory() as tmp:
                        env = Env(Path(tmp))
                        workdir = env.prepare(schema_of())
                        manifest = Manifest.load(workdir)
                        dtype = (
                            fixed_layout(manifest.layout)[0]
                            if key == "fixed_file"
                            else LINKS_RECORD_DTYPE
                        )
                        rows = int(manifest.data["snapshots"]["5"]["rows"])
                        if reregister:
                            message = r"is {} bytes, expected {} rows x {} bytes".format(
                                (rows - 1) * dtype.itemsize, rows, dtype.itemsize
                            )
                            self._truncate_and_register(workdir, 5, key, kind, dtype)
                        else:
                            path = Path(manifest.data["snapshots"]["5"][key])
                            with open(path, "r+b") as handle:
                                handle.truncate((rows - 1) * dtype.itemsize)
                            message = r"content checksum .* refusing to use it"
                        with self.assertRaisesRegex(ConverterError, message):
                            list(CTreesAsciiAdapter(schema_of(), workdir).iter_batches(100))

    @staticmethod
    def _truncate_and_register(workdir, snap, key, kind, dtype):
        """Drop one snapshot scratch file's last record and re-register the
        shorter file, leaving the snapshot's recorded ``rows`` unchanged."""
        manifest = Manifest.load(workdir)
        entry = manifest.data["snapshots"][str(snap)]
        records = np.fromfile(entry[key], dtype=dtype)[:-1]
        records.tofile(entry[key])
        manifest.register_intermediate(
            entry[key],
            kind,
            rows=int(records.size),
            dtype_tag=manifest.data["intermediates"][entry[key]]["dtype_tag"],
        )
        manifest.save()


class AsciiPipelineTests(unittest.TestCase):
    """The generic pipeline's ASCII route: it resumes an interrupted conversion
    to the uninterrupted one's products and records and validates the
    ``forest-rank`` identity scheme."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        src = self.root / "ascii"
        src.mkdir()
        forests = fixtures.standard_forests()
        tree = fixtures.write_ctrees_file(src / "tree_0_0_0.dat", fixtures.all_trees(forests))
        self.parameters = {
            "tree_files": [str(tree)],
            "forests_list": str(fixtures.write_forests_list(src / "forests.list", forests)),
            "simulation_info": str(fixtures.write_simulation_info(src / "simulation_info.yaml")),
        }
        self.a_list = fixtures.write_a_list(src / "fixture.a_list")
        self.schema = schema_of(DEFAULT_PROFILE)
        self.sizes = dict(ingest_max_rows=4, transpose_budget_bytes=4 << 20)

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_current_scheme_workdir_resumes_normally(self):
        """An interrupted current-scheme ASCII conversion resumes through
        every stage to the uninterrupted one's transposed products."""
        clean = self.root / "clean"
        pipeline.initialize(clean, self.schema, self.parameters, self.a_list, **self.sizes)
        work = self.root / "resumed"
        pipeline.initialize(work, self.schema, self.parameters, self.a_list, **self.sizes)
        with capture_stderr():
            pipeline.run_ingest(clean)
            pipeline.run_transpose(clean)
            with mock.patch.object(
                pipeline, "_stream_chunks", side_effect=RuntimeError("interrupted")
            ):
                with self.assertRaises(RuntimeError):
                    pipeline.run_ingest(work)
            pipeline.run_ingest(work)
            pipeline.run_transpose(work)
        got, want = cm.ConversionManifest.load(work), cm.ConversionManifest.load(clean)
        self.assertEqual(
            [got.artifact(r)["sha256"] for r in got.stage("transpose")["artifacts"]],
            [want.artifact(r)["sha256"] for r in want.stage("transpose")["artifacts"]],
        )

    def test_a_workdir_recorded_without_the_scheme_is_refused_untouched(self):
        """A workdir whose recorded parameters lack ``identity_scheme`` (one an
        earlier converter wrote, its digest self-consistent) is refused untouched
        by ``initialize`` as a different conversion and by every stage, whether
        its ingest was interrupted or completed."""
        refusals = [
            ("initialize", "different conversion"),
            ("run_ingest", r"differ from the ones this converter records .*identity_scheme"),
            ("run_transpose", r"differ from the ones this converter records .*identity_scheme"),
            ("run_write", r"differ from the ones this converter records .*identity_scheme"),
        ]
        for complete in (False, True):
            work = self.root / "unrecorded_{}".format(int(complete))
            pipeline.initialize(work, self.schema, self.parameters, self.a_list, **self.sizes)
            with capture_stderr():
                if complete:
                    pipeline.run_ingest(work)
                else:
                    with mock.patch.object(
                        pipeline, "_stream_chunks", side_effect=RuntimeError("interrupted")
                    ), self.assertRaises(RuntimeError):
                        pipeline.run_ingest(work)
            path = work / cm.MANIFEST_NAME
            data = json.loads(path.read_text())
            del data["configuration"]["adapter"]["parameters"]["identity_scheme"]
            data["configuration_sha256"] = cm.canonical_sha256(data["configuration"])
            path.write_text(json.dumps(data))
            before = tree_state(work)
            for entry, refusal in refusals:
                with self.subTest(complete_ingest=complete, entry=entry):
                    with capture_stderr(), self.assertRaisesRegex(ConverterError, refusal):
                        if entry == "initialize":
                            pipeline.initialize(
                                work, self.schema, self.parameters, self.a_list, **self.sizes
                            )
                        elif entry == "run_write":
                            simulation_info = self.parameters["simulation_info"]
                            pipeline.run_write(work, HorizontalV3Writer(simulation_info))
                        else:
                            getattr(pipeline, entry)(work)
                    self.assertEqual(tree_state(work), before)

    def test_another_identity_scheme_is_refused(self):
        for value in ("physical", None, 1):
            refusal = "identity_scheme must be 'forest-rank'"
            with self.subTest(value=value), self.assertRaisesRegex(ConverterError, refusal):
                pipeline.canonical_parameters(
                    "consistent_trees_ascii", dict(self.parameters, identity_scheme=value)
                )
        recorded = pipeline.canonical_parameters("consistent_trees_ascii", self.parameters)
        self.assertEqual(recorded["identity_scheme"], "forest-rank")
        # the recorded form re-validates to itself (the CLI hands it back)
        self.assertEqual(
            pipeline.canonical_parameters("consistent_trees_ascii", recorded), recorded
        )


def stated_inventory_bytes(workdir, n_forests, n_units, n_files):
    """The inventory budget the adapter states, rebuilt from its named terms."""
    return (
        ctrees_ascii.INVENTORY_BASE_BYTES
        + os.path.getsize(Path(workdir) / "manifest.json")
        * ctrees_ascii.MANIFEST_LOAD_BYTES_PER_BYTE
        + n_forests * ctrees_ascii.INVENTORY_BYTES_PER_UNIT
        + n_units * ctrees_ascii.SOURCE_UNIT_TABLE_BYTES
        + n_files * ctrees_ascii.INVENTORY_BYTES_PER_FILE
    )


class BudgetAccountingTests(unittest.TestCase):
    """Re-measure the real inventory path so INVENTORY_BYTES_PER_UNIT (per
    forest), SOURCE_UNIT_TABLE_BYTES (per physical unit),
    INVENTORY_BYTES_PER_FILE (per source file), MANIFEST_LOAD_BYTES_PER_BYTE
    and INVENTORY_BASE_BYTES cannot silently drift below what it allocates."""

    def test_inventory_peak_is_within_the_declared_terms(self):
        schema = schema_of(DEFAULT_PROFILE)
        # (forests, trees per forest, files): one physical unit per forest in
        # two files; every forest split across both files; and a fragmented
        # catalogue -- few forests, every one-halo tree in a file of its own
        for n_forests, n_trees, n_files in ((2, 1, 2), (3000, 1, 2), (3000, 2, 2), (2, 200, 400)):
            case = self.subTest(forests=n_forests, trees=n_trees, files=n_files)
            with case, tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                forests = [
                    fixtures.ForestSpec(
                        forest_id=10 * (k + 1),
                        trees=[
                            fixtures.TreeSpec(
                                root_id=100000 * (k + 1) + t,
                                halos=[
                                    fixtures.HaloSpec(
                                        halo_id=100000 * (k + 1) + t, snap=5, mvir=1.0e11
                                    )
                                ],
                            )
                            for t in range(n_trees)
                        ],
                    )
                    for k in range(n_forests)
                ]
                if n_files == n_forests * n_trees:  # fragmented: a file per tree
                    parts = [[tree] for tree in fixtures.all_trees(forests)]
                elif n_files == n_trees:  # split: tree t of every forest in file t
                    parts = [[f.trees[t] for f in forests] for t in range(n_trees)]
                else:  # the forests halved between the two files
                    trees = fixtures.all_trees(forests)
                    parts = [trees[: len(trees) // 2], trees[len(trees) // 2 :]]
                self.assertEqual(len(parts), n_files)
                files = [
                    fixtures.write_ctrees_file(root / "t{}.dat".format(i), part)
                    for i, part in enumerate(parts)
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
                n_units = int(adapter._unit_counts.size)
                expected_units = sum(
                    sum(1 for f in forests if any(t in part for t in f.trees)) for part in parts
                )
                self.assertEqual(n_units, expected_units)
                self.assertEqual(len(adapter.inventory().units), n_forests)
                self.assertEqual(sum(1 for _ in adapter.iter_forests()), n_forests)
                self.assertLessEqual(
                    peak, stated_inventory_bytes(root / "w", n_forests, n_units, n_files)
                )

    def test_the_inventory_budget_is_stated_against_the_forest_count(self):
        """The refusal's figure is exactly the base, the manifest, per-forest,
        physical unit and per-file terms, checked before the sidecar it names
        is materialised: one byte less is refused, that much builds."""
        with tempfile.TemporaryDirectory() as tmp:
            env = Env(Path(tmp))
            workdir = env.prepare(schema_of())
            # 5 forests; 3 physical units in each of the two files
            need = stated_inventory_bytes(workdir, 5, 6, 2)
            manifest_bytes = os.path.getsize(workdir / "manifest.json")
            with self.assertRaisesRegex(
                ConverterError,
                r"^the source inventory \(5 forest\(s\); 6 physical unit\(s\) through file 1 "
                r"of 2; a {}-byte manifest\) needs {} bytes, above the configured memory budget "
                r"of {} bytes; raise memory_budget_bytes$".format(manifest_bytes, need, need - 1),
            ):
                CTreesAsciiAdapter(schema_of(), workdir, memory_budget_bytes=need - 1).inventory()
            inventory = CTreesAsciiAdapter(
                schema_of(), workdir, memory_budget_bytes=need
            ).inventory()
            self.assertEqual(len(inventory.units), 5)

    def test_the_manifest_term_is_refused_before_the_manifest_is_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = Env(Path(tmp))
            workdir = env.prepare(schema_of())
            manifest_bytes = os.path.getsize(workdir / "manifest.json")
            need = (
                ctrees_ascii.INVENTORY_BASE_BYTES
                + manifest_bytes * ctrees_ascii.MANIFEST_LOAD_BYTES_PER_BYTE
            )
            adapter = CTreesAsciiAdapter(schema_of(), workdir, memory_budget_bytes=need - 1)
            unread = AssertionError("the manifest was read")
            with mock.patch.object(Manifest, "load", side_effect=unread):
                with self.assertRaisesRegex(
                    ConverterError,
                    r"^loading the conversion manifest \({} bytes\) needs {} bytes".format(
                        manifest_bytes, need
                    ),
                ):
                    adapter.inventory()


# ---------------------------------------------------------------------------
# A declared cut table
# ---------------------------------------------------------------------------


class CutTableTests(unittest.TestCase):
    """The route under a declared cut table, against the rewriter's cut of
    the route's uncut output (the fixture-scale equivalence of the two
    producers of a cut dataset), and its bindings and refusals."""

    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="ascii_cut_"))
        forest = fixtures.severed_satellite_forest()
        cls.severed = cut_source(cls.root / "severed", forest, cut_layout(forest, 1))
        cls.severed_table = write_table(
            cls.root / "severed", "cut", fixtures.severed_satellite_cut_table(), cls.severed
        )

    @classmethod
    def tearDownClass(cls):
        import shutil

        shutil.rmtree(cls.root, ignore_errors=True)

    def test_the_route_under_a_table_equals_the_cut_of_its_uncut_output(self):
        import convert_trees
        from scatter import file_md5

        for name, forest_of, table_of, split_after in CUT_FIXTURES:
            with self.subTest(fixture=name):
                root = self.root / "equal" / name
                forest = forest_of()
                source = cut_source(root, forest, cut_layout(forest, split_after))
                table = write_table(root, "cut", table_of(), source)
                summary = rewrite_cut(source, root / "c", root / "c_scratch", table)
                manifest = convert_under_cut(source, root / "route", table)
                dataset = manifest.artifact_path(manifest.stage("write")["directory"])
                assert_datasets_equal(self, root / "c" / "dataset", dataset)
                # and the cut is not the identity: one more forest per fresh piece
                self.assertEqual(
                    read_sidecar(dataset / "forests.h5")["ForestID"].size,
                    read_sidecar(source["dataset"] / "forests.h5")["ForestID"].size
                    + summary["cut"]["fresh_pieces"],
                )
                self.assertGreater(summary["cut"]["fresh_pieces"], 0)
                # the same severance: the route's promotions per snapshot are the cut operation's
                prepared = json.loads(
                    (root / "route" / pipeline.ASCII_PREPARATION_DIR / "manifest.json").read_text()
                )
                promoted = {
                    int(snap): entry["cut_promotions"]
                    for snap, entry in prepared["snapshots"].items()
                    if entry["cut_promotions"]
                }
                self.assertTrue(promoted)
                self.assertEqual(
                    promoted,
                    {
                        entry["snapshot"]: entry["promoted_halos"]
                        for entry in summary["cut"]["per_snapshot"]
                        if entry["promoted_halos"]
                    },
                )
                recorded = manifest.configuration["adapter"]["parameters"]
                self.assertEqual(recorded["cut_table_md5"], file_md5(table))
                with quiet_output():
                    self.assertEqual(
                        convert_trees.main(["validate", "--workdir", str(root / "route")]), 0
                    )

    def test_a_host_in_another_forest_is_refused_by_name_without_a_table(self):
        """Only a declared cut table makes a satellite whose host lies in
        another forest convertible: without one, ingest refuses the satellite
        as an unresolved host under the catalogue's own partition."""
        forest = fixtures.severed_satellite_forest()
        root = self.root / "foreign"
        root.mkdir()
        for index, trees in enumerate(cut_layout(forest, 1)):
            fixtures.write_ctrees_file(root / "tree_{}.dat".format(index), trees)
        # tree 2110 is a forest of its own, so its satellite 2111's host (in
        # tree 2100) lies in another forest
        first, last = cut_companions()
        listed = [
            first,
            fixtures.ForestSpec(forest_id=2100, trees=forest.trees[:1]),
            fixtures.ForestSpec(forest_id=2150, trees=forest.trees[1:]),
            last,
        ]
        fixtures.write_forests_list(root / "forests.list", listed)
        fixtures.write_simulation_info(root / "simulation_info.yaml")
        fixtures.write_a_list(root / "fixture.a_list")
        work = root / "route"
        pipeline.initialize(
            work,
            schema_of(DEFAULT_PROFILE),
            route_parameters({"root": root}),
            root / "fixture.a_list",
            ingest_max_rows=4,
            transpose_budget_bytes=4 << 20,
        )
        refusal = (
            r"origin id=2111, at id=2111, upid=2101, pid=2101, forest=2150: neither target "
            r"present within the forest"
        )
        with capture_stderr():
            with self.assertRaisesRegex(ConverterError, refusal):
                pipeline.run_ingest(work)

    def test_the_cut_table_term_is_refused_over_budget_before_the_table_is_loaded(self):
        parameters = route_parameters(self.severed, self.severed_table)
        n_trees = 4  # the two companions' trees and the forest's two
        need = n_trees * ctrees_ascii.CUT_TABLE_BYTES_PER_TREE
        with mock.patch.object(
            ctrees_ascii, "load_cut_table", side_effect=AssertionError("loaded")
        ):
            refusal = (
                r"^loading the cut table \({} trees\) needs {} bytes, above the configured "
                r"memory budget of {} bytes; raise memory_budget_bytes$".format(
                    n_trees, need, need - 1
                )
            )
            with capture_stderr(), self.assertRaisesRegex(ConverterError, refusal):
                prepare_workdir(
                    schema_of(DEFAULT_PROFILE),
                    parameters["tree_files"],
                    parameters["forests_list"],
                    self.severed["root"] / "fixture.a_list",
                    parameters["simulation_info"],
                    self.root / "budget",
                    max_rows=4,
                    memory_budget_bytes=need - 1,
                    cut_table_path=self.severed_table,
                )

    def test_without_a_table_nothing_is_recorded(self):
        recorded = pipeline.canonical_parameters(
            "consistent_trees_ascii", route_parameters(self.severed)
        )
        self.assertNotIn("cut_table", recorded)
        self.assertNotIn("cut_table_md5", recorded)
        prepared = self.severed["preparation_manifest"]
        provenance = json.loads(prepared.read_text())["provenance"]
        self.assertNotIn("cut_table", provenance)
        self.assertEqual(list(prepared.parent.glob("scratch/tree_rows_src_*.npy")), [])

    def test_the_table_is_inside_the_configuration_digest(self):
        uncut = pipeline.canonical_parameters(
            "consistent_trees_ascii", route_parameters(self.severed)
        )
        recorded = pipeline.canonical_parameters(
            "consistent_trees_ascii", route_parameters(self.severed, self.severed_table)
        )
        self.assertEqual(
            dict(recorded, cut_table=None, cut_table_md5=None),
            dict(uncut, cut_table=None, cut_table_md5=None),
        )
        self.assertEqual(recorded["cut_table"], os.path.realpath(self.severed_table))
        # the recorded form re-validates to itself, and only against the same bytes
        self.assertEqual(
            pipeline.canonical_parameters("consistent_trees_ascii", recorded), recorded
        )
        with self.assertRaisesRegex(ConverterError, "not the recorded"):
            pipeline.canonical_parameters(
                "consistent_trees_ascii", dict(recorded, cut_table_md5="0" * 32)
            )
        with self.assertRaisesRegex(ConverterError, "needs a cut_table"):
            pipeline.canonical_parameters(
                "consistent_trees_ascii", dict(uncut, cut_table_md5=recorded["cut_table_md5"])
            )
        # a different table is a different conversion of the same files
        work = self.root / "digest"
        convert_under_cut(self.severed, work, self.severed_table)
        other = write_table(self.root / "severed", "other", {}, self.severed)
        with self.assertRaisesRegex(ConverterError, "holds a different conversion"):
            pipeline.initialize(
                work,
                schema_of(DEFAULT_PROFILE),
                route_parameters(self.severed, other),
                self.severed["root"] / "fixture.a_list",
                ingest_max_rows=4,
                transpose_budget_bytes=4 << 20,
            )

    def test_no_other_route_accepts_a_table(self):
        for source_format, parameters in (
            ("lhalo_binary", {"sources": [[0, str(self.severed_table)]]}),
            (
                "consistent_trees_hdf5",
                {"info_path": "x", "first_file": 0, "last_file": 0, "particle_mass": 1.0},
            ),
        ):
            with self.subTest(source_format=source_format):
                with self.assertRaisesRegex(ConverterError, r"unknown \['cut_table'\]"):
                    pipeline.canonical_parameters(
                        source_format, dict(parameters, cut_table=str(self.severed_table))
                    )

    def prepare(self, workdir, cut_table, **kwargs):
        source = self.severed
        root = source["root"]
        with capture_stderr():
            return prepare_workdir(
                schema_of(DEFAULT_PROFILE),
                sorted(root.glob("tree_*.dat")),
                root / "forests.list",
                root / "fixture.a_list",
                root / "simulation_info.yaml",
                workdir,
                cut_table_path=cut_table,
                **kwargs,
            )

    def test_a_table_whose_roots_are_not_the_index_files_is_refused_before_scatter(self):
        source = self.severed
        cases = (
            ("missing", {2110: None}, r"misses 1 forests.list tree root\(s\) \(e.g. \[2110\]\)"),
            ("foreign", {9999: 9999}, r"lists 1 root\(s\) forests.list does not \(e.g. \[9999\]\)"),
        )
        for name, change, pattern in cases:
            with self.subTest(case=name):
                rows = {
                    tree.root_id: forest.forest_id
                    for forest in source["forests"]
                    for tree in forest.trees
                }
                rows.update(fixtures.severed_satellite_cut_table())
                for root_id, forest_id in change.items():
                    if forest_id is None:
                        del rows[root_id]
                    else:
                        rows[root_id] = forest_id
                table = fixtures.write_cut_table(self.root / (name + ".list"), rows)
                workdir = self.root / ("roots_" + name)
                with self.assertRaisesRegex(ConverterError, pattern):
                    self.prepare(workdir, table)
                self.assertFalse((workdir / "manifest.json").exists(), "scatter ran")
                self.assertEqual(list((workdir / "scratch").iterdir()), [])

    def test_a_violated_invariant_is_refused_after_scatter_and_before_sort(self):
        # the smaller piece {2110} keeps the forest's id, the larger takes the fresh one
        table = write_table(self.root, "swapped", {2100: 8001, 2110: 2100}, self.severed)
        workdir = self.root / "swapped_work"
        with self.assertRaisesRegex(ConverterError, "not kept by their largest piece"):
            self.prepare(workdir, table)
        statuses = {
            entry["status"]
            for entry in json.loads((workdir / "manifest.json").read_text())["snapshots"].values()
        }
        self.assertEqual(statuses, {"concatenated"})

    def test_a_resume_must_name_the_same_table_or_none(self):
        workdir = self.root / "resume"
        with mock.patch.object(ctrees_ascii, "run_sort", side_effect=RuntimeError("stop")):
            with self.assertRaisesRegex(RuntimeError, "stop"):
                self.prepare(workdir, self.severed_table)
        other = write_table(self.root, "resume_other", {}, self.severed)
        for table, pattern in (
            (other, "cut table md5 .* differs"),
            (None, "cut table md5 None differs"),
        ):
            with self.subTest(table=table):
                with self.assertRaisesRegex(ConverterError, pattern):
                    self.prepare(workdir, table)
        # the fix-up stage refuses to run without the bound table
        with self.assertRaisesRegex(ConverterError, "given cut table md5 None but scatter bound"):
            root = self.severed["root"]
            run_fixups(workdir, root / "fixture.a_list", root / "simulation_info.yaml")
        # a resume with the same table completes, and a linked workdir still checks it
        self.prepare(workdir, self.severed_table)
        with self.assertRaisesRegex(ConverterError, "cut table differs"):
            self.prepare(workdir, other)
        self.prepare(workdir, self.severed_table)

    def test_an_interrupted_install_resumes_to_the_uninterrupted_output(self):
        import scatter

        whole = convert_under_cut(self.severed, self.root / "install_whole", self.severed_table)
        expected = whole.artifact_path(whole.stage("write")["directory"])
        real_replace, real_install = os.replace, scatter._install_table

        def after_replacing(name):
            def replace(src, dst):
                real_replace(src, dst)
                if Path(dst).name == name:
                    raise RuntimeError("injected after replacing {}".format(name))

            return mock.patch.object(scatter.os, "replace", side_effect=replace)

        def before_the_save():
            def install(manifest, path, kind, table, **record):
                real_install(manifest, path, kind, table, **record)
                if kind == "forest-index-table":
                    raise RuntimeError("injected before the manifest save")

            return mock.patch.object(scatter, "_install_table", side_effect=install)

        for boundary, injection in (
            ("units", after_replacing(source_units_name(1))),
            ("table", after_replacing("forest_index_table.npy")),
            ("save", before_the_save()),
        ):
            with self.subTest(boundary=boundary):
                work = self.root / ("install_" + boundary)
                with injection, self.assertRaisesRegex(RuntimeError, "injected"):
                    convert_under_cut(self.severed, work, self.severed_table)
                with capture_stderr():
                    pipeline.run_ingest(work)
                    pipeline.run_transpose(work)
                    manifest = pipeline.run_write(
                        work, HorizontalV3Writer(self.severed["root"] / "simulation_info.yaml")
                    )
                assert_datasets_equal(
                    self, expected, manifest.artifact_path(manifest.stage("write")["directory"])
                )

    def test_the_installed_partition_is_the_tables(self):
        workdir = self.root / "installed"
        self.prepare(workdir, self.severed_table)
        np.testing.assert_array_equal(
            np.load(workdir / "forest_index_table.npy"), [1400, 2100, 8000, 8001]
        )
        # file 0 holds 1400 and tree 2100; file 1 holds tree 2110 (the fresh
        # piece) and forest 8000's tree, in that marker order
        self.assertEqual(
            np.load(workdir / "scratch" / source_units_name(0)).tolist(), [[1400, 3], [2100, 3]]
        )
        self.assertEqual(
            np.load(workdir / "scratch" / source_units_name(1)).tolist(), [[8001, 2], [8000, 3]]
        )
        manifest = Manifest.load(workdir)
        # every source's tree-rows sidecar is a registered intermediate
        for path in manifest.data["provenance"]["source_files"]:
            entry = manifest.source_entry(path)
            sidecar = (workdir / "scratch" / tree_rows_name(entry["src_index"])).resolve()
            self.assertEqual(manifest.data["intermediates"][str(sidecar)]["kind"], "tree-rows")
        fixed_dtype = fixed_layout(manifest.layout)[0]
        records = [
            record
            for entry in manifest.data["snapshots"].values()
            for record in np.fromfile(entry["fixed_file"], dtype=fixed_dtype)
        ]
        by_id = {int(r["id"]): r for r in records}
        self.assertEqual(int(by_id[2111]["forest_id"]), 8001)
        self.assertEqual((int(by_id[2111]["pid"]), int(by_id[2111]["upid"])), (-1, 2111))
        self.assertEqual(int(by_id[2101]["forest_id"]), 2100)


if __name__ == "__main__":
    unittest.main()
