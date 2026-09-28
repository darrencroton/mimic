"""Unit tests for the bounded transpose (scripts/convert/transpose.py).

**The oracle is hand-written.** :data:`GRAPH` is a small gapped,
mixed-progenitor catalog over six snapshots -- snapshot 3 is empty, siblings
sit in earlier *and* later snapshots than each other, one branch ends early,
``MostBoundID`` values repeat and go negative -- and :data:`EXPECTED` spells
out every output row and every resolved ``(row, snapshot)`` target by hand.
Nothing in it is computed by the code under test.

Every other positive case compares against that literal table or against the
roomy-budget output after it has matched the table: different batch orders
(inventory-major like L-Halo, snapshot-major like ASCII, shuffled), tiny
budgets and forced two-way merges must all produce the same bytes.

The real-data case converts mini-Millennium file 0 and checks every link and
payload bit against a ``numpy``-level read of the binary with a hand-declared
record layout -- not the adapter's schema helpers and not the transpose's
remapping.
"""

import contextlib
import gc
import os
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
import rank_sort  # noqa: E402
import transpose as tp  # noqa: E402
from adapters import base  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from source_keys import RESOLVED_DTYPE  # noqa: E402

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(TESTS_DIR)))
ALL_EXTRAS_PROFILE = os.path.join(TESTS_DIR, "data", "column_maps", "ascii_all_extra_types.yaml")
MINI_MILLENNIUM = os.path.join(REPO_ROOT, "simulations", "mini-millennium")
REAL_FILE = os.path.join(MINI_MILLENNIUM, "snapshots", "trees_063.0")

ROOMY = 8 << 20
SNAPSHOTS = tuple(range(6))


def ascii_schema(profile=None):
    if profile is not None:
        return cs.build_schema(cs.load_column_map(profile))
    document = {
        "schema_version": 1,
        "source_format": "consistent_trees_ascii",
        "required_columns": {role: [role] for role in cs.REQUIRED_ROLES["consistent_trees_ascii"]},
        "extra_fields": [],
    }
    return cs.build_schema(cs.parse_column_map(document, origin="<test>"))


# ==========================================================================
# The hand-specified graph
# ==========================================================================

#: (name, SourceHaloID, ForestIndex, HaloRankInForest, SnapNum, Descendant,
#:  FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup,
#:  MostBoundID). Links name halos; ``None`` is null; FirstHaloInFOFgroup
#: ``None`` means "self". Forest A is listed depth-first, as an L-Halo tree
#: would store it, so its rows are far from snapshot order.
GRAPH = (
    ("a0", 1, 0, 0, 5, None, "a1", None, None, None, 42),
    ("a1", 2, 0, 1, 4, "a0", "a2", "a4", None, "a7", 200),
    ("a2", 3, 0, 2, 2, "a1", "a3", None, None, "a8", 300),
    ("a3", 4, 0, 3, 1, "a2", None, None, None, "a6", -7),
    ("a6", 5, 0, 4, 1, "a4", None, None, "a3", None, 500),
    ("a8", 6, 0, 5, 2, None, None, None, "a2", None, 600),
    ("a4", 7, 0, 6, 2, "a0", "a5", "a7", None, None, 700),
    ("a5", 8, 0, 7, 0, "a4", None, "a6", None, None, 42),
    ("a7", 9, 0, 8, 4, "a0", None, None, "a1", None, 900),
    ("b0", 10, 1, 0, 5, None, "b1", None, None, None, 1000),
    ("b1", 11, 1, 1, 4, "b0", "b2", None, None, None, 1100),
    ("b2", 12, 1, 2, 0, "b1", None, None, None, None, 1200),
    ("b3", 13, 1, 3, 0, None, None, None, None, None, 42),
    ("c0", 14, 2, 0, 5, None, None, None, None, None, -1400),
    ("c1", 15, 2, 1, 0, None, None, None, None, None, 1500),
)

#: Snapshot -> output rows, each (SourceHaloID, Descendant, DescendantSnapshot,
#: FirstProgenitor, FirstProgenitorSnapshot, NextProgenitor,
#: NextProgenitorSnapshot, FirstHaloInFOFgroup, NextHaloInFOFgroup). Written
#: out by hand from the graph above.
EXPECTED = {
    0: [
        (8, 2, 2, -1, -1, 1, 1, 0, -1),  # a5: sibling a6 sits LATER, at snap 1
        (12, 2, 4, -1, -1, -1, -1, 1, -1),  # b2: descendant four snapshots ahead
        (13, -1, -1, -1, -1, -1, -1, 2, -1),  # b3: a tree that ends at once
        (15, -1, -1, -1, -1, -1, -1, 3, -1),
    ],
    1: [
        (4, 0, 2, -1, -1, -1, -1, 0, 1),  # a3
        (5, 2, 2, -1, -1, -1, -1, 0, -1),  # a6: FoF satellite of a3
    ],
    2: [
        (3, 0, 4, 0, 1, -1, -1, 0, 1),  # a2: descendant across empty snap 3
        (6, -1, -1, -1, -1, -1, -1, 0, -1),  # a8: branch ends early
        (7, 0, 5, 0, 0, 1, 4, 2, -1),  # a4: gap to 5; sibling a7 LATER
    ],
    3: [],
    4: [
        (2, 0, 5, 0, 2, 2, 2, 0, 1),  # a1: progenitor across snap 3; sibling EARLIER
        (9, 0, 5, -1, -1, -1, -1, 0, -1),  # a7
        (11, 1, 5, 1, 0, -1, -1, 2, -1),  # b1: progenitor four snapshots back
    ],
    5: [
        (1, -1, -1, 0, 4, -1, -1, 0, -1),  # a0
        (10, -1, -1, 2, 4, -1, -1, 1, -1),  # b0
        (14, -1, -1, -1, -1, -1, -1, 2, -1),  # c0
    ],
}

EXPECTED_COLUMNS = (
    "SourceHaloID",
    "Descendant",
    "DescendantSnapshot",
    "FirstProgenitor",
    "FirstProgenitorSnapshot",
    "NextProgenitor",
    "NextProgenitorSnapshot",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)


def graph_table():
    """The graph as rows of source keys, ids ascending."""
    ids = {row[0]: row[1] for row in GRAPH}
    rows = []

    def key(other):
        return -1 if other is None else ids[other]

    for _name, sid, forest, rank, snap, desc, fp, np_, ffof, nfof, mbid in GRAPH:
        rows.append(
            dict(
                sid=sid,
                forest=forest,
                rank=rank,
                snap=snap,
                links=(key(desc), key(fp), key(np_), sid if ffof is None else ids[ffof], key(nfof)),
                mbid=mbid,
            )
        )
    return rows


def replicated_rows(copies):
    """``copies`` disjoint copies of the graph: copy ``k`` adds ``15 k`` to
    every SourceHaloID and link key and ``3 k`` to every ForestIndex."""
    rows = []
    for copy in range(copies):
        shift = len(GRAPH) * copy
        for row in graph_table():
            row = dict(row)
            row["sid"] += shift
            row["forest"] += 3 * copy
            row["links"] = tuple(key + shift if key > 0 else key for key in row["links"])
            rows.append(row)
    return rows


def replicated_expected(copies):
    """:data:`EXPECTED` for :func:`replicated_rows`, by arithmetic alone: ids
    ascend by copy, so copy ``k``'s row ``r`` of a snapshot holding ``n`` rows
    per copy lands at ``k n + r``, and a target in snapshot ``t`` moves by
    ``k`` times *that* snapshot's per-copy count."""
    per_copy = {snapshot: len(rows) for snapshot, rows in EXPECTED.items()}

    def shift(row, snap, copy):
        return row if row < 0 else row + copy * per_copy[snap]

    table = {}
    for snapshot, rows in EXPECTED.items():
        out = []
        for copy in range(copies):
            for sid, desc, desc_snap, fp, fp_snap, np_, np_snap, ffof, nfof in rows:
                out.append(
                    (
                        sid + len(GRAPH) * copy,
                        shift(desc, desc_snap, copy),
                        desc_snap,
                        shift(fp, fp_snap, copy),
                        fp_snap,
                        shift(np_, np_snap, copy),
                        np_snap,
                        shift(ffof, snapshot, copy),
                        shift(nfof, snapshot, copy),
                    )
                )
        table[snapshot] = out
    return table


def payload_values(sid):
    """Distinct, bit-sensitive payload per halo: -0.0 and a float32 extreme
    included, so a cast or reorder would show."""
    mass = -0.0 if sid == 3 else np.float32(sid) * np.float32(1.5)
    if sid == 5:
        mass = np.float32(3.4028235e38)
    return {
        "Len": sid * 11,
        "M_Crit200": mass,
        "Pos": (sid, -sid, 0.5),
        "Vel": (sid * 2.0, -0.0, 1e-38),
        "Spin": (0.25, sid, -sid),
        "VelDisp": sid / 7.0,
        "Vmax": sid * 3.25,
    }


def extra_values(extra, sid):
    spec = extra.spec
    if extra.type == "long long":
        return 2**53 + sid if extra.name == "HaloID" else -(2**62) + sid
    if extra.type == "int":
        return np.iinfo(np.int32).max - sid
    if extra.type == "double":
        return 1.0 / 3.0 + sid
    if extra.type == "float":
        return -0.0 if sid == 8 else sid / 9.0
    if extra.type == "vec3_int":
        return (np.iinfo(np.int32).min + sid, 0, sid)
    assert spec.n_components == 3
    return (sid, -0.0, 1.0 / sid)


def make_batch(schema, rows):
    n_rows = len(rows)
    column = lambda values, dtype: np.asarray(values, dtype=dtype)  # noqa: E731
    identity = {
        "SourceHaloID": column([row["sid"] for row in rows], np.int64),
        "ForestIndex": column([row["forest"] for row in rows], np.int64),
        "HaloRankInForest": column([row["rank"] for row in rows], np.int64),
    }
    coordinates = {
        "source_file_ordinal": column([1 if row["forest"] == 2 else 0 for row in rows], np.int64),
        "unit_ordinal": column([row["forest"] % 2 for row in rows], np.int64),
        "row_ordinal": identity["HaloRankInForest"].copy(),
    }
    links = {
        name: column([row["links"][index] for row in rows], np.int64)
        for index, name in enumerate(base.LINK_FIELDS)
    }
    payload = {}
    for field in schema.payload_fields:
        spec = cs.EXTRA_TYPES[field.type]
        if field.name == "SnapNum":
            payload[field.name] = column([row["snap"] for row in rows], np.int32)
        elif field.name == "MostBoundID":
            payload[field.name] = column([row["mbid"] for row in rows], np.int64)
        else:
            values = [payload_values(row["sid"])[field.name] for row in rows]
            payload[field.name] = column(values, spec.numpy_dtype).reshape(
                (n_rows,) if spec.n_components == 1 else (n_rows, spec.n_components)
            )
    extras = {}
    for extra in schema.extra_fields:
        spec = extra.spec
        values = [extra_values(extra, row["sid"]) for row in rows]
        extras[extra.name] = column(values, spec.numpy_dtype).reshape(
            (n_rows,) if spec.n_components == 1 else (n_rows, spec.n_components)
        )
    return base.CanonicalBatch(
        schema=schema,
        identity=identity,
        coordinates=coordinates,
        links=links,
        payload=payload,
        extras=extras,
    )


def batch_source(schema, order="inventory", size=4, rows=None, seed=0):
    """A ``batches(max_rows)`` callable over the graph in one of three orders:
    ``inventory`` (ids ascending, like L-Halo), ``snapshot-major`` (like the
    ASCII adapter, latest snapshot first) or ``shuffled`` (random ascending
    groups in random order)."""
    rows = list(rows if rows is not None else graph_table())

    def groups():
        if order == "inventory":
            return [rows[i : i + size] for i in range(0, len(rows), size)]
        if order == "snapshot-major":
            out = []
            for snap in sorted({row["snap"] for row in rows}, reverse=True):
                members = [row for row in rows if row["snap"] == snap]
                out.extend(members[i : i + size] for i in range(0, len(members), size))
            return out
        rng = np.random.default_rng(seed)
        labels = rng.integers(0, max(1, len(rows) // size), len(rows))
        out = [[row for row, label in zip(rows, labels) if label == group] for group in set(labels)]
        rng.shuffle(out)
        return [group for group in out if group]

    def batches(max_rows):
        for group in groups():
            for start in range(0, len(group), max_rows):
                yield make_batch(schema, group[start : start + max_rows])

    return batches


class TransposeCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.counter = 0

    def out_dir(self):
        self.counter += 1
        path = self.root / "out{}".format(self.counter)
        path.mkdir()
        return path

    def run_transpose(self, schema, batches, budget=ROOMY, snapshots=SNAPSHOTS, **kwargs):
        out = self.out_dir()
        result = tp.transpose(schema, batches, snapshots, out, budget_bytes=budget, **kwargs)
        self.assertLessEqual(result.peak_resident_bytes, budget)
        return result

    def outputs(self, result):
        return {
            entry.snapshot: np.array(tp.read_snapshot(result, entry.snapshot))
            for entry in result.snapshots
        }

    def assert_matches_expected(self, result, expected=None):
        tables = self.outputs(result)
        self.assertEqual(sorted(tables), list(SNAPSHOTS))
        for snapshot, rows in (expected or EXPECTED).items():
            with self.subTest(snapshot=snapshot):
                got = [
                    tuple(int(record[name]) for name in EXPECTED_COLUMNS)
                    for record in tables[snapshot]
                ]
                self.assertEqual(got, rows)


def minimal_budget(schema):
    """The smallest budget plan_budget accepts, found by bisection."""
    low, high = 1, 1 << 22
    while low < high:
        middle = (low + high) // 2
        try:
            tp.plan_budget(schema, middle)
            high = middle
        except ConverterError:
            low = middle + 1
    return low


# ==========================================================================
# The hand-specified graph
# ==========================================================================


class TestHandSpecifiedGraph(TransposeCase):
    def test_every_row_and_target_matches_the_hand_written_table(self):
        schema = ascii_schema()
        result = self.run_transpose(schema, batch_source(schema))
        self.assert_matches_expected(result)

    def test_reported_topology_measurements(self):
        schema = ascii_schema()
        result = self.run_transpose(schema, batch_source(schema))
        self.assertEqual(result.total_halos, len(GRAPH))
        self.assertEqual([entry.n_halos for entry in result.snapshots], [4, 2, 3, 0, 3, 3])
        self.assertEqual(
            [entry.first_global_position for entry in result.snapshots], [0, 4, 6, 9, 9, 12]
        )
        # a5 0->2, a2 2->4, a4 2->5, b2 0->4
        self.assertEqual(result.n_gapped_descendants, 4)
        self.assertEqual(result.max_descendant_span, 4)
        self.assertFalse(result.links_adjacent)
        self.assertEqual(
            result.n_links,
            {
                "Descendant": 9,
                "FirstProgenitor": 6,
                "NextProgenitor": 3,
                "FirstHaloInFOFgroup": 15,
                "NextHaloInFOFgroup": 3,
            },
        )
        self.assertEqual(result.schema_digest, schema.digest)
        self.assertEqual(result.record_dtype, tp.output_dtype(schema))

    def test_the_empty_snapshot_is_an_empty_file(self):
        schema = ascii_schema()
        result = self.run_transpose(schema, batch_source(schema))
        entry = result.output(3)
        self.assertTrue(os.path.isfile(entry.path))
        self.assertEqual(os.path.getsize(entry.path), 0)
        self.assertEqual(tp.read_snapshot(result, 3).size, 0)

    def test_payload_identity_and_extras_are_moved_bit_for_bit(self):
        schema = ascii_schema(ALL_EXTRAS_PROFILE)
        result = self.run_transpose(schema, batch_source(schema))
        by_id = {row["sid"]: row for row in graph_table()}
        tables = self.outputs(result)
        for snapshot, records in tables.items():
            for record in records:
                sid = int(record["SourceHaloID"])
                expected = make_batch(schema, [by_id[sid]])
                with self.subTest(sid=sid):
                    self.assertEqual(int(record["SnapNum"]), snapshot)
                    for name in ("ForestIndex", "HaloRankInForest"):
                        self.assertEqual(int(record[name]), int(expected.identity[name][0]))
                    for field in schema.payload_fields:
                        self.assertEqual(
                            np.asarray(record[field.name]).tobytes(),
                            np.asarray(expected.payload[field.name][0])
                            .astype(record[field.name].dtype)
                            .tobytes(),
                            field.name,
                        )
                    for extra in schema.extra_fields:
                        self.assertEqual(
                            np.asarray(record[extra.name]).tobytes(),
                            np.asarray(expected.extras[extra.name][0]).tobytes(),
                            extra.name,
                        )
        # the three duplicate MostBoundID values and the negative ones survive
        mbids = sorted(int(r["MostBoundID"]) for records in tables.values() for r in records)
        self.assertEqual(mbids.count(42), 3)
        self.assertIn(-7, mbids)
        self.assertIn(-1400, mbids)
        self.assertEqual(int(tables[2][0]["HaloID"]), 2**53 + 3)

    def test_output_layout_is_the_v3_halo_table_little_endian(self):
        schema = ascii_schema(ALL_EXTRAS_PROFILE)
        dtype = tp.output_dtype(schema)
        names = list(dtype.names)
        self.assertEqual(names[:5], list(base.LINK_FIELDS))
        self.assertEqual(names[5:8], list(base.SNAPSHOT_LINK_FIELDS))
        self.assertEqual(names[8:11], ["SourceHaloID", "ForestIndex", "HaloRankInForest"])
        for name in base.LINK_FIELDS + ("SourceHaloID", "ForestIndex", "HaloRankInForest"):
            self.assertEqual(dtype[name], np.dtype("<i8"), name)
        for name in base.SNAPSHOT_LINK_FIELDS:
            self.assertEqual(dtype[name], np.dtype("<i4"), name)
        self.assertEqual(dtype["Pos"].shape, (3,))
        self.assertEqual(dtype["Counters"].base, np.dtype("<i4"))
        self.assertEqual(dtype["RvirDouble"], np.dtype("<f8"))


class TestOrderAndBudgetIndependence(TransposeCase):
    COPIES = 10

    def test_batch_order_and_size_never_change_the_output(self):
        schema = ascii_schema(ALL_EXTRAS_PROFILE)
        reference = self.run_transpose(schema, batch_source(schema))
        self.assert_matches_expected(reference)
        expected = self.outputs(reference)
        for order, size, seed in (
            ("inventory", 1, 0),
            ("inventory", 15, 0),
            ("snapshot-major", 2, 0),
            ("shuffled", 3, 1),
            ("shuffled", 2, 7),
        ):
            with self.subTest(order=order, size=size, seed=seed):
                result = self.run_transpose(schema, batch_source(schema, order, size, seed=seed))
                got = self.outputs(result)
                for snapshot in SNAPSHOTS:
                    self.assertEqual(got[snapshot].tobytes(), expected[snapshot].tobytes())

    def test_tiny_budgets_and_forced_merge_passes_never_change_the_output(self):
        """150 halos over ten copies of the graph: the roomy run must match
        the arithmetic expectation, and the minimal budget -- with and without
        a forced two-way fan-in -- must reproduce it byte for byte from many
        runs and several merge passes."""
        schema = ascii_schema(ALL_EXTRAS_PROFILE)
        rows = replicated_rows(self.COPIES)
        reference = self.run_transpose(schema, batch_source(schema, "inventory", 50, rows))
        self.assert_matches_expected(reference, replicated_expected(self.COPIES))
        expected = self.outputs(reference)
        tiny = minimal_budget(schema)
        for label, fanin in (("minimal", None), ("two-way", 2)):
            with self.subTest(label=label):
                context = (
                    mock.patch.object(rank_sort, "_keyed_fanin_cap", return_value=fanin)
                    if fanin
                    else contextlib.nullcontext()
                )
                with context:
                    result = self.run_transpose(
                        schema, batch_source(schema, "shuffled", 7, rows, seed=3), budget=tiny
                    )
                got = self.outputs(result)
                for snapshot in SNAPSHOTS:
                    self.assertEqual(got[snapshot].tobytes(), expected[snapshot].tobytes())
                self.assertGreater(min(result.sort_runs.values()), 2)
                if fanin:
                    self.assertGreater(min(result.sort_merge_passes.values()), 0)
                self.assertGreaterEqual(result.chain_rounds, 1)

    def test_an_adjacent_only_catalog_reports_links_adjacent(self):
        schema = ascii_schema()
        rows = [
            dict(sid=1, forest=0, rank=0, snap=1, links=(-1, 2, -1, 1, -1), mbid=1),
            dict(sid=2, forest=0, rank=1, snap=0, links=(1, -1, -1, 2, -1), mbid=1),
        ]
        result = self.run_transpose(schema, batch_source(schema, rows=rows), snapshots=(0, 1, 2))
        self.assertTrue(result.links_adjacent)
        self.assertEqual(result.max_descendant_span, 1)
        self.assertEqual([entry.n_halos for entry in result.snapshots], [1, 1, 0])


# ==========================================================================
# Rejections and failure hygiene
# ==========================================================================


class TestRejections(TransposeCase):
    def assert_rejected_cleanly(self, schema, batches, pattern, snapshots=SNAPSHOTS, budget=ROOMY):
        out = self.out_dir()
        with self.assertRaisesRegex(ConverterError, pattern):
            tp.transpose(schema, batches, snapshots, out, budget_bytes=budget)
        # no output, no half-written file and no private scratch survives
        self.assertEqual(sorted(os.listdir(out)), [])

    def edited(self, sid, **changes):
        rows = graph_table()
        for row in rows:
            if row["sid"] == sid:
                row.update(changes)
        return rows

    def test_a_snapshot_outside_the_a_list_is_refused(self):
        schema = ascii_schema()
        self.assert_rejected_cleanly(
            schema, batch_source(schema), "SnapNum 5, which is not one of", snapshots=range(5)
        )

    def test_a_missing_link_target_is_refused_with_nothing_written(self):
        schema = ascii_schema()
        # a5's FoF chain continues into a halo the source does not contain
        rows = self.edited(8, links=(7, -1, 5, 8, 99))
        self.assert_rejected_cleanly(schema, batch_source(schema, rows=rows), "not a halo")

    def test_a_duplicate_source_key_across_batches_is_refused(self):
        schema = ascii_schema()
        rows = graph_table()
        clone = dict(rows[-1])  # c1 again, now at snapshot 5 too
        clone["snap"] = 5
        for order in ("inventory", "snapshot-major"):
            with self.subTest(order=order):
                source = batch_source(schema, order, rows=rows)

                def batches(max_rows, source=source):
                    # the duplicate reaches the transpose in a batch of its own
                    yield from source(max_rows)
                    yield make_batch(schema, [clone])

                self.assert_rejected_cleanly(schema, batches, "carried by two halos")

    def test_a_duplicate_source_key_within_a_snapshot_is_refused(self):
        schema = ascii_schema()
        rows = graph_table()
        rows.append(dict(rows[-1]))  # c1 twice at snapshot 0
        batches = lambda max_rows: (make_batch(schema, [row]) for row in rows)  # noqa: E731
        self.assert_rejected_cleanly(schema, batches, "duplicated|carried by two halos")

    def test_a_cross_forest_link_is_refused(self):
        schema = ascii_schema()
        # b3 (forest B, snapshot 0) names a5 (forest A, snapshot 0) as its central
        rows = self.edited(13, links=(-1, -1, -1, 8, -1))
        self.assert_rejected_cleanly(schema, batch_source(schema, rows=rows), "cross forests")

    def test_a_cyclic_sibling_chain_is_refused(self):
        schema = ascii_schema()
        rows = graph_table()
        by_id = {row["sid"]: row for row in rows}
        # a0's chain becomes a1 -> (end); a4 and a7 point at each other
        by_id[2]["links"] = (1, 3, -1, 2, 9)
        by_id[7]["links"] = (1, 8, 9, 7, -1)
        by_id[9]["links"] = (1, -1, 7, 2, -1)
        self.assert_rejected_cleanly(
            schema, batch_source(schema, rows=rows), "NextProgenitor chain .* never terminates"
        )

    def test_an_oversized_batch_is_refused(self):
        schema = ascii_schema()
        rows = replicated_rows(20)
        budget = minimal_budget(schema)
        self.assertLess(tp.plan_budget(schema, budget).max_batch_rows, len(rows))
        batches = lambda max_rows: iter([make_batch(schema, rows)])  # noqa: E731
        self.assert_rejected_cleanly(schema, batches, "asked for at most", budget=budget)

    def test_a_batch_of_another_schema_or_type_is_refused(self):
        schema = ascii_schema()
        other = ascii_schema(ALL_EXTRAS_PROFILE)
        self.assert_rejected_cleanly(schema, batch_source(other), "was planned for")
        self.assert_rejected_cleanly(
            schema, lambda max_rows: iter([object()]), "not a CanonicalBatch"
        )

    def test_an_invalid_batch_is_refused_by_the_contract_check(self):
        schema = ascii_schema()

        def batches(max_rows):
            batch = make_batch(schema, graph_table()[:2])
            batch.payload["M_Crit200"][0] = np.nan
            yield batch

        self.assert_rejected_cleanly(schema, batches, "non-finite")

    def test_a_failure_during_assembly_leaves_no_output(self):
        schema = ascii_schema()
        real = tp._SnapshotWriter.write
        calls = []

        def failing(self, first, out):
            calls.append(first)
            real(self, first, out)
            raise OSError("disk full")

        out = self.out_dir()
        with mock.patch.object(tp._SnapshotWriter, "write", failing):
            with self.assertRaisesRegex(OSError, "disk full"):
                tp.transpose(schema, batch_source(schema), SNAPSHOTS, out, budget_bytes=ROOMY)
        self.assertTrue(calls)
        self.assertEqual(os.listdir(out), [])

    def test_existing_outputs_are_never_overwritten(self):
        schema = ascii_schema()
        out = self.out_dir()
        victim = out / tp.output_name(2)
        victim.write_bytes(b"precious")
        with self.assertRaisesRegex(ConverterError, "refusing to overwrite"):
            tp.transpose(schema, batch_source(schema), SNAPSHOTS, out, budget_bytes=ROOMY)
        self.assertEqual(victim.read_bytes(), b"precious")
        self.assertEqual(os.listdir(out), [victim.name])

    def test_missing_directories_are_refused(self):
        schema = ascii_schema()
        with self.assertRaisesRegex(ConverterError, "does not exist"):
            tp.transpose(
                schema, batch_source(schema), SNAPSHOTS, self.root / "no", budget_bytes=ROOMY
            )
        with self.assertRaisesRegex(ConverterError, "spill directory"):
            tp.transpose(
                schema,
                batch_source(schema),
                SNAPSHOTS,
                self.out_dir(),
                budget_bytes=ROOMY,
                spill_dir=self.root / "no",
            )

    def test_bad_parameters_fail_before_any_batch_is_read(self):
        schema = ascii_schema()

        def never(max_rows):
            raise AssertionError("the batch source must not be touched")

        cases = (
            (dict(snapshots=(0, 2**31)), "int32"),
            (dict(snapshots=(0, 0)), "strictly ascending"),
            (dict(budget_bytes=True), "integer"),
            (dict(budget_bytes=1e7), "integer"),
            (dict(budget_bytes=4096), "too small"),
        )
        for overrides, pattern in cases:
            with self.subTest(overrides=overrides):
                arguments = dict(snapshots=SNAPSHOTS, budget_bytes=ROOMY)
                arguments.update(overrides)
                with self.assertRaisesRegex(ConverterError, pattern):
                    tp.transpose(
                        schema,
                        never,
                        arguments["snapshots"],
                        self.out_dir(),
                        budget_bytes=arguments["budget_bytes"],
                    )
        with self.assertRaisesRegex(ConverterError, "CanonicalSchema"):
            tp.transpose(object(), never, SNAPSHOTS, self.out_dir(), budget_bytes=ROOMY)


# ==========================================================================
# Memory, widths and scratch
# ==========================================================================


class TestBudget(TransposeCase):
    def test_widths_come_from_the_schema(self):
        plain = ascii_schema()
        wide = ascii_schema(ALL_EXTRAS_PROFILE)
        extra_bytes = sum(extra.spec.itemsize for extra in wide.extra_fields)
        self.assertEqual(extra_bytes, 4 + 12 + 8 + 4 + 12 + 8 + 8)
        self.assertEqual(tp.row_dtype(wide).itemsize - tp.row_dtype(plain).itemsize, extra_bytes)
        self.assertEqual(
            tp.output_dtype(wide).itemsize - tp.output_dtype(plain).itemsize, extra_bytes
        )
        # v3 core: 5 x int64 links + 3 x int32 snapshots + 3 x int64 identity
        # + the ctrees payload (Len, SnapNum, M_Crit200, 3 vec3, VelDisp, Vmax,
        # MostBoundID) = 40 + 12 + 24 + 4 + 4 + 4 + 36 + 4 + 4 + 8
        self.assertEqual(tp.output_dtype(plain).itemsize, 140)
        # a wider schema buys fewer rows from the same budget, never more memory
        budget = 1 << 20
        self.assertLess(
            tp.plan_budget(wide, budget).max_batch_rows,
            tp.plan_budget(plain, budget).max_batch_rows,
        )
        self.assertLess(
            tp.plan_budget(wide, budget).assembly_rows, tp.plan_budget(plain, budget).assembly_rows
        )

    def test_each_phase_split_fits_the_usable_budget(self):
        plan = tp.plan_budget(ascii_schema(ALL_EXTRAS_PROFILE), 3 << 20)
        usable = plan.usable_bytes
        self.assertEqual(usable, plan.budget_bytes - rank_sort.SCRATCH_WRITE_BUFFER_BYTES)
        self.assertLessEqual(plan.row_generation_bytes + plan.row_merge_bytes, usable)
        self.assertLessEqual(plan.row_merge_bytes + plan.join_generation_bytes, usable)
        self.assertLessEqual(
            plan.join_merge_bytes + plan.resolved_generation_bytes + plan.chain_generation_bytes,
            usable,
        )
        self.assertLessEqual(plan.chain_merge_bytes + plan.chain_round_generation_bytes, usable)
        self.assertLessEqual(plan.resolved_merge_bytes + plan.assembly_bytes, usable)

    def test_actual_allocation_stays_within_budget_plus_the_allowance(self):
        """``tracemalloc`` over a whole transpose of ~196,000 synthetic halos
        at a 2 MiB budget. One escaped catalog-sized int64 array (1.57 MB)
        would exceed the 1 MiB allowance, so a record-proportional leak cannot
        hide inside it."""
        schema = ascii_schema()
        table = synthetic_table(20_000, n_snapshots=20, seed=5)
        budget = 2 << 20
        gc.collect()
        out = self.out_dir()
        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            before = tracemalloc.get_traced_memory()[0]
            result = tp.transpose(
                schema, synthetic_source(schema, table), range(20), out, budget_bytes=budget
            )
            peak = tracemalloc.get_traced_memory()[1] - before
        finally:
            tracemalloc.stop()
        self.assertEqual(result.total_halos, table.shape[0])
        self.assertGreater(table.shape[0] * 8, tp.UNMETERED_ALLOWANCE_BYTES)
        self.assertLessEqual(result.peak_resident_bytes, budget)
        self.assertLessEqual(peak, budget + tp.UNMETERED_ALLOWANCE_BYTES)
        self.assertGreater(sum(result.sort_runs.values()), 20)

    def test_scratch_is_private_and_removed_and_writers_use_8192_bytes(self):
        schema = ascii_schema()
        spills = self.root / "spills"
        spills.mkdir()
        modes = []
        real_open = open

        def spy(path, mode="r", buffering=-1, *args, **kwargs):
            if any(flag in mode for flag in "wxa"):
                modes.append((mode, buffering))
            return real_open(path, mode, buffering, *args, **kwargs)

        with mock.patch("builtins.open", side_effect=spy):
            result = self.run_transpose(schema, batch_source(schema), spill_dir=spills)
        self.assertEqual(os.listdir(spills), [])
        self.assertGreater(result.peak_spill_bytes, 0)
        self.assertTrue(modes)
        self.assertEqual({buffering for _mode, buffering in modes}, {8192})


class TestPerRowScratch(TransposeCase):
    """The declared per-row and per-link scratch constants against the
    ``tracemalloc`` peak of the real ingest and assembly code, each measured
    inside a real transpose. Chunks run to tens of thousands of rows, so a
    constant a few percent short is tens of kilobytes over, far above the
    calls' fixed overhead of a few kilobytes."""

    def test_ingest_peak_stays_within_the_row_array_and_declared_scratch(self):
        """One ~97,000-row batch at a 64 MiB budget, sized so the row sorter
        never spills while it is added: the window holds validation, the
        snapshot lookup, the row array and the copy into the sorter's
        preallocated chunk, and nothing else."""
        schema = ascii_schema()
        table = synthetic_table(10_000, n_snapshots=20, seed=7)
        n_rows = table.shape[0]
        budget = 64 << 20
        plan = tp.plan_budget(schema, budget)
        row = tp.row_dtype(schema)
        run_records = (
            plan.row_generation_bytes - rank_sort.SCRATCH_WRITE_BUFFER_BYTES
        ) // rank_sort.keyed_generation_bytes_per_record(row, len(tp._ROW_KEY))
        self.assertLessEqual(n_rows, plan.max_batch_rows)
        self.assertGreater(run_records, n_rows)
        batch = next(synthetic_source(schema, table)(n_rows))
        real = tp._Transpose._ingest
        peaks = []

        def traced(transposer, batches, rows):
            gc.collect()
            tracemalloc.start()
            try:
                base = tracemalloc.get_traced_memory()[0]
                real(transposer, batches, rows)
                peaks.append(tracemalloc.get_traced_memory()[1] - base)
            finally:
                tracemalloc.stop()

        with mock.patch.object(tp._Transpose, "_ingest", traced):
            result = self.run_transpose(
                schema, lambda max_rows: iter([batch]), budget=budget, snapshots=range(20)
            )
        self.assertEqual(result.total_halos, n_rows)
        declared = n_rows * (row.itemsize + tp._INGEST_SCRATCH_BYTES_PER_ROW)
        self.assertLessEqual(
            peaks[0],
            declared,
            "ingest peaked at the row array plus {:.2f} B/row; {} are declared".format(
                peaks[0] / n_rows - row.itemsize, tp._INGEST_SCRATCH_BYTES_PER_ROW
            ),
        )

    def test_assembly_peak_stays_within_the_declared_row_and_link_scratch(self):
        """Every full chunk's ``_resolve_chunk`` runs over its own links,
        taken from the real stream first -- so the merge's allocations, which
        its sorter meters, stay outside the window -- and re-cut into blocks
        of a chosen size: 256 links, where the per-row term dominates, and
        about 1.2 chunks' worth, where each chunk's links span several
        segments and the per-link term dominates.

        The 16 MiB budget keeps each chunk below 256 KiB of int64 per column,
        numpy's threshold for eliding temporaries, which is where the per-row
        cost is highest."""
        schema = ascii_schema()
        budget = 16 << 20
        plan = tp.plan_budget(schema, budget)
        self.assertLess(plan.assembly_rows * 8, 256 << 10)
        table = synthetic_table(7_000, n_snapshots=20, seed=7)
        self.assertGreater(table.shape[0], 2 * plan.assembly_rows)
        real = tp._Transpose._resolve_chunk
        for block_links in (256, plan.assembly_rows * 6 // 5):
            with self.subTest(block_links=block_links):
                measured = []

                def traced(
                    transposer,
                    first,
                    rows,
                    out,
                    layout,
                    cursor,
                    written,
                    block_links=block_links,
                    measured=measured,
                ):
                    end = first + int(rows.size)
                    taken = [segment.copy() for segment in cursor.take_below(end)]
                    links = np.concatenate(taken) if taken else np.empty(0, RESOLVED_DTYPE)
                    del taken
                    blocks = [
                        links[start : start + block_links]
                        for start in range(0, links.size, block_links)
                    ]
                    local = tp._LinkCursor(iter(blocks))
                    gc.collect()
                    tracemalloc.start()
                    try:
                        base = tracemalloc.get_traced_memory()[0]
                        real(transposer, first, rows, out, layout, local, written)
                        peak = tracemalloc.get_traced_memory()[1] - base
                    finally:
                        tracemalloc.stop()
                    self.assertIsNone(local.block)
                    measured.append((int(rows.size), int(links.size), peak))

                with mock.patch.object(tp._Transpose, "_resolve_chunk", traced):
                    result = self.run_transpose(
                        schema, synthetic_source(schema, table), budget=budget, snapshots=range(20)
                    )
                self.assertEqual(result.total_halos, table.shape[0])
                full = [entry for entry in measured if entry[0] == plan.assembly_rows]
                self.assertGreaterEqual(len(full), 2)
                for n_rows, n_links, peak in full:
                    segment = min(block_links, n_links)
                    declared = (
                        n_rows * tp._ASSEMBLY_SCRATCH_BYTES_PER_ROW
                        + segment * tp._SCATTER_SCRATCH_BYTES_PER_RECORD
                    )
                    self.assertLessEqual(
                        peak,
                        declared,
                        "a {}-row chunk with {} links in blocks of {} peaked at {} bytes".format(
                            n_rows, n_links, block_links, peak
                        ),
                    )
                if block_links > 256:
                    self.assertTrue(all(n_links > block_links for _, n_links, _ in full))


def synthetic_table(n_forests, n_snapshots, seed):
    """Valid synthetic forests as int64 rows (sid, forest, rank, snap, five
    link keys): a gapped main branch per forest, sibling progenitors at random
    earlier snapshots, and FoF satellites that end their branch at once."""
    rng = np.random.default_rng(seed)
    out = []
    next_id = 1
    for forest in range(n_forests):
        length = int(rng.integers(2, 8))
        snaps = np.sort(rng.choice(n_snapshots, size=length, replace=False))[::-1]
        local = []  # [snap, desc, fp, np, ffof, nfof]

        def add(local, snap, central=None):
            local.append([int(snap), -1, -1, -1, central, -1])
            index = len(local) - 1
            if central is None:
                local[index][4] = index
            return index

        main = [add(local, snap) for snap in snaps]
        for i in range(length - 1):
            local[main[i]][2] = main[i + 1]
            local[main[i + 1]][1] = main[i]
        for i in range(length - 1):
            if rng.random() < 0.5:
                sibling = add(local, int(rng.integers(0, snaps[i])))
                local[sibling][1] = main[i]
                local[main[i + 1]][3] = sibling
            if rng.random() < 0.5:
                previous = main[i]
                for _ in range(int(rng.integers(1, 4))):
                    satellite = add(local, int(snaps[i]), central=main[i])
                    local[previous][5] = satellite
                    previous = satellite
        for rank, (snap, desc, fp, np_, ffof, nfof) in enumerate(local):
            key = lambda value, base=next_id: -1 if value < 0 else base + value  # noqa: E731
            out.append(
                (
                    next_id + rank,
                    forest,
                    rank,
                    snap,
                    key(desc),
                    key(fp),
                    key(np_),
                    key(ffof),
                    key(nfof),
                )
            )
        next_id += len(local)
    return np.asarray(out, dtype=np.int64)


def synthetic_source(schema, table):
    def batches(max_rows):
        for start in range(0, table.shape[0], max_rows):
            part = table[start : start + max_rows]
            n_rows = part.shape[0]
            payload = {}
            for field in schema.payload_fields:
                spec = cs.EXTRA_TYPES[field.type]
                shape = (n_rows,) if spec.n_components == 1 else (n_rows, spec.n_components)
                payload[field.name] = np.zeros(shape, dtype=spec.numpy_dtype)
            payload["SnapNum"] = part[:, 3].astype(np.int32)
            yield base.CanonicalBatch(
                schema=schema,
                identity={
                    "SourceHaloID": part[:, 0].copy(),
                    "ForestIndex": part[:, 1].copy(),
                    "HaloRankInForest": part[:, 2].copy(),
                },
                coordinates={
                    "source_file_ordinal": np.zeros(n_rows, dtype=np.int64),
                    "unit_ordinal": part[:, 1].copy(),
                    "row_ordinal": part[:, 2].copy(),
                },
                links={name: part[:, 4 + i].copy() for i, name in enumerate(base.LINK_FIELDS)},
                payload=payload,
                extras={},
            )

    return batches


# ==========================================================================
# Real mini-Millennium, against an independent binary read
# ==========================================================================

#: Hand-declared 104-byte L-Halo record (struct RawHalo), independent of the
#: shipped profile and of the adapter.
ORACLE_DTYPE = np.dtype(
    [
        ("Descendant", "<i4"),
        ("FirstProgenitor", "<i4"),
        ("NextProgenitor", "<i4"),
        ("FirstHaloInFOFgroup", "<i4"),
        ("NextHaloInFOFgroup", "<i4"),
        ("Len", "<i4"),
        ("M_Mean200", "<f4"),
        ("M_Crit200", "<f4"),
        ("M_TopHat", "<f4"),
        ("Pos", "<f4", (3,)),
        ("Vel", "<f4", (3,)),
        ("VelDisp", "<f4"),
        ("Vmax", "<f4"),
        ("Spin", "<f4", (3,)),
        ("MostBoundID", "<i8"),
        ("SnapNum", "<i4"),
        ("FileNr", "<i4"),
        ("SubhaloIndex", "<i4"),
        ("SubHalfMass", "<f4"),
    ]
)


@unittest.skipUnless(os.path.isfile(REAL_FILE), "mini-Millennium trees_063.0 is not available")
class TestRealMiniMillennium(TransposeCase):
    """mini-Millennium is the plan's gap-containing acceptance gate. File 0 in
    full: 174,845 halos, 3,323 gapped descendants, maximum span 2."""

    def test_every_link_and_payload_bit_matches_the_binary(self):
        from adapters.lhalo_binary import LHaloBinaryAdapter

        self.assertEqual(ORACLE_DTYPE.itemsize, 104)
        raw = Path(REAL_FILE).read_bytes()
        n_trees, n_total = np.frombuffer(raw[:8], dtype="<i4")
        counts = np.frombuffer(raw[8 : 8 + 4 * int(n_trees)], dtype="<i4").astype(np.int64)
        source = np.frombuffer(raw[8 + 4 * int(n_trees) :], dtype=ORACLE_DTYPE)
        self.assertEqual(source.size, int(n_total))
        tree_start = np.repeat(np.cumsum(counts) - counts, counts)
        source_id = np.arange(1, source.size + 1, dtype=np.int64)
        expected_target = {}
        for name in base.LINK_FIELDS:
            local = source[name].astype(np.int64)
            expected_target[name] = np.where(local >= 0, tree_start + local + 1, -1)

        column_map = cs.load_column_map(
            os.path.join(
                REPO_ROOT, "scripts", "convert", "profiles", "lhalo_binary_extras_example.yaml"
            )
        )
        properties = cs.load_source_properties(
            os.path.join(MINI_MILLENNIUM, "halo_properties.yaml")
        )
        schema = cs.build_schema(column_map, properties)
        adapter = LHaloBinaryAdapter(schema, [(0, REAL_FILE)], max_snapshot=63)
        result = self.run_transpose(
            schema, adapter.iter_batches, budget=16 << 20, snapshots=range(64)
        )

        out = np.concatenate([np.asarray(tp.read_snapshot(result, s)) for s in range(64)])
        order = np.lexsort((source_id, source["SnapNum"].astype(np.int64)))
        np.testing.assert_array_equal(out["SourceHaloID"], source_id[order])
        own_snapshot = source["SnapNum"][order].astype(np.int64)
        offsets = np.asarray([entry.first_global_position for entry in result.snapshots])
        snapshot_columns = dict(
            zip(("Descendant", "FirstProgenitor", "NextProgenitor"), base.SNAPSHOT_LINK_FIELDS)
        )
        for name in base.LINK_FIELDS:
            with self.subTest(link=name):
                rows = out[name]
                column = snapshot_columns.get(name)
                target_snapshot = out[column].astype(np.int64) if column else own_snapshot
                if column:
                    np.testing.assert_array_equal(out[column] == -1, rows == -1)
                position = offsets[np.maximum(target_snapshot, 0)] + rows
                got = np.where(rows >= 0, out["SourceHaloID"][np.maximum(position, 0)], -1)
                np.testing.assert_array_equal(got, expected_target[name][order])
        for name in (
            "Len",
            "M_Crit200",
            "Pos",
            "Vel",
            "Spin",
            "VelDisp",
            "Vmax",
            "MostBoundID",
            "SnapNum",
        ):
            with self.subTest(payload=name):
                self.assertEqual(
                    np.ascontiguousarray(out[name]).tobytes(),
                    np.ascontiguousarray(source[name][order]).tobytes(),
                )
        for extra in schema.extra_fields:
            with self.subTest(extra=extra.name):
                component = extra.sources[0]
                reference = source[component.field][order]
                if component.component is not None:
                    reference = reference[:, component.component]
                self.assertEqual(
                    np.ascontiguousarray(out[extra.name]).tobytes(),
                    np.ascontiguousarray(reference).tobytes(),
                )

        descendant = source["Descendant"].astype(np.int64)
        has = descendant >= 0
        span = (
            source["SnapNum"][(tree_start + descendant)[has]].astype(np.int64)
            - source["SnapNum"][has]
        )
        self.assertEqual(result.total_halos, 174_845)
        self.assertEqual(result.n_gapped_descendants, int(np.count_nonzero(span > 1)))
        self.assertEqual(result.n_gapped_descendants, 3_323)
        self.assertEqual(result.max_descendant_span, 2)


if __name__ == "__main__":
    unittest.main()
