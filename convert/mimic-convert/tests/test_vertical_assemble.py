"""The forest assembler (vertical/assemble.py) on synthetic version 3 datasets.

Oracles, each typed literally from the fixture's topology as (``MostBoundID``, the five links as
forest-local ranks, ``SnapNum``) by rank: :mod:`fixtures`' correspondence forests 1100, 1200,
1300 and 1400 converted to a version 3 dataset (1300's one tree ends at snapshot 2, so the forest
is absent from four of the six slabs; 1100 spans three trees whose FoF groups mix them);
forest 2700, whose root has a progenitor chain of three; the three-piece forest cut by its
decided table, where a satellite promoted out of its host's group is a group of one and its
former group is in another forest; the committed ``shin-uchuu`` ``worked_ascii`` fixture; the
committed ``mini-millennium-horizontal`` fixtures, whose ``HaloRankInForest`` is the L-Halo
source's within-tree row, not ordered by snapshot, checked against an independent index of the
dataset's rows; and a thousand small forests, on which the per-forest Python work is counted and
the rows each slab reads are bounded. Every refusal comes from a copy of a dataset mutated in one
way.
"""

import collections
import math
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import fixtures  # noqa: E402
import test_rewriter  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from test_forest_census import convert_ascii  # noqa: E402
from test_vertical_plan import edit_column  # noqa: E402
from vertical.assemble import (  # noqa: E402
    DEFAULT_WINDOW_ROWS,
    LINK_FIELDS,
    LINK_SNAPSHOT_COLUMNS,
    AssembledForest,
    ForestAssembler,
    memory_bound,
)
from vertical.record import record_dtype  # noqa: E402

SHIN_UCHUU_RECORD = REPO_ROOT / "simulations" / "shin-uchuu-ascii" / "halo_properties.yaml"
MINI_MILLENNIUM_RECORD = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"
WORKED_ASCII = REPO_ROOT / "simulations" / "shin-uchuu" / "_tests" / "data" / "worked_ascii"
MM_HORIZONTAL = REPO_ROOT / "simulations" / "mini-millennium-horizontal"
MM_HORIZONTAL_RECORD = MM_HORIZONTAL / "halo_properties.yaml"
MM_HORIZONTAL_DATA = MM_HORIZONTAL / "_tests" / "data"

#: Every payload field of the shin-uchuu record, copied verbatim from the column of that name.
PAYLOAD_FIELDS = (
    "Len",
    "M_Crit200",
    "Pos",
    "Vel",
    "Spin",
    "VelDisp",
    "Vmax",
    "MostBoundID",
    "SnapNum",
)

# The correspondence dataset's forests by rank: (MostBoundID, Descendant, FirstProgenitor,
# NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup, SnapNum). Snapshots descend; within
# snapshot 4 of forest 1100 the FoF groups are [1101, 1121], [1102], [1111].
FOREST_1100 = [
    (1100, -1, 3, -1, 0, 1, 5),
    (1110, -1, 6, -1, 0, -1, 5),
    (1120, -1, 4, -1, 2, -1, 5),
    (1101, 0, 7, 5, 3, 4, 4),
    (1121, 2, -1, -1, 3, -1, 4),
    (1102, 0, -1, -1, 5, -1, 4),
    (1111, 1, -1, -1, 6, -1, 4),
    (1103, 3, -1, -1, 7, -1, 3),
]
FOREST_1200 = [
    (1200, -1, 2, -1, 0, 1, 5),
    (1210, -1, 3, -1, 0, -1, 5),
    (1201, 0, 4, -1, 2, 3, 4),
    (1211, 1, 5, -1, 2, -1, 4),
    (1202, 2, -1, -1, 4, -1, 3),
    (1212, 3, -1, -1, 5, -1, 3),
]
FOREST_1300 = [(1300, -1, 1, -1, 0, -1, 2), (1301, 0, -1, -1, 1, -1, 1)]
FOREST_1400 = [(1400, -1, 1, -1, 0, -1, 5), (1401, 0, 2, -1, 1, -1, 4), (1402, 1, -1, -1, 2, -1, 3)]

#: Forest 2700: 2750's progenitors in chain order 2753, 2751, 2752 (ranks 3, 4, 5).
FOREST_2700 = [
    (2710, -1, 2, -1, 0, -1, 5),
    (2750, -1, 3, -1, 1, -1, 5),
    (2711, 0, -1, -1, 2, 3, 4),
    (2753, 1, -1, 4, 2, -1, 4),
    (2751, 1, -1, 5, 4, -1, 4),
    (2752, 1, -1, -1, 5, -1, 4),
]

# The three-piece cut: 2501 (ForestIndex 4) and 2521 and 2522 (ForestIndex 3) were satellites
# of 2511 and 2512, which keep ForestIndex 1; promoted, each is a FoF group of one, and the
# former hosts' groups hold the host alone.
PIECE_2500 = [
    (2510, -1, 1, -1, 0, -1, 5),
    (2511, 0, 2, -1, 1, -1, 4),
    (2512, 1, 3, -1, 2, -1, 3),
    (2513, 2, 4, -1, 3, -1, 2),
    (2514, 3, -1, -1, 4, -1, 1),
]
PIECE_8001 = [
    (2520, -1, 2, -1, 0, 1, 5),
    (2530, -1, -1, -1, 0, -1, 5),
    (2521, 0, 3, -1, 2, -1, 4),
    (2522, 2, -1, -1, 3, -1, 3),
]
PIECE_8002 = [(2500, -1, 1, -1, 0, -1, 5), (2501, 0, -1, -1, 1, -1, 4)]

#: The worked_ascii fixture's forest 10 (ForestIndex 0): 1010's progenitors 1012, 1011, 1013.
WORKED_10 = [
    (1010, -1, 3, -1, 0, -1, 5),
    (1020, -1, 5, -1, 1, -1, 5),
    (1011, 0, 6, 4, 2, -1, 4),
    (1012, 0, -1, 2, 3, -1, 4),
    (1013, 0, -1, -1, 4, -1, 4),
    (1021, 1, -1, -1, 5, -1, 4),
    (1014, 2, -1, -1, 6, -1, 3),
]


def rows(forest: AssembledForest):
    """A forest's records as the oracle tuples."""
    return [
        (
            int(record["MostBoundID"]),
            *(int(record[name]) for name in LINK_FIELDS),
            int(record["SnapNum"]),
        )
        for record in forest.records
    ]


def convert(root, forests):
    return convert_ascii(root, [fixtures.all_trees(forests)], forests)["dataset"]


def retype_column(dataset_dir, snap, name, dtype):
    """Rewrite one ``/halos`` column of one slab with another dtype, values unchanged."""
    path = Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap)
    with h5py.File(path, "r+") as handle:
        values = handle["halos"][name][...]
        del handle["halos"][name]
        handle["halos"].create_dataset(name, data=values.astype(dtype))


class CorrespondenceDataset(unittest.TestCase):
    """The correspondence forests, 1300's early-ending tree included: ForestIndex 0 to 3."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="vertical_assemble_"))
        cls.dataset_dir = convert(
            cls.tmp / "source", fixtures.early_ending_correspondence_forests()
        )
        cls.dataset = HorizontalDataset(cls.dataset_dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="vertical_assemble_case_"))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)

    def mutated(self, *edits):
        """A copy of the dataset with each ``(snap, column, edit)`` applied."""
        target = Path(tempfile.mkdtemp(dir=self.work)) / "dataset"
        shutil.copytree(self.dataset_dir, target)
        for snap, name, edit in edits:
            edit_column(target, snap, name, edit)
        return HorizontalDataset(target)

    def assembled(self, first, stop, dataset=None, window_rows=DEFAULT_WINDOW_ROWS, **kwargs):
        assembler = ForestAssembler(dataset or self.dataset, SHIN_UCHUU_RECORD, window_rows)
        return list(assembler.forests(first, stop, **kwargs))


class AssemblyTests(CorrespondenceDataset):
    def test_the_worked_forest_at_every_window_size(self):
        """Forest 1100 in rank order with local links, as a view of the record dtype, whether
        a window holds every slab whole or splits its four-row block at snapshot 4."""
        for window_rows in (1, 2, 3, DEFAULT_WINDOW_ROWS):
            with self.subTest(window_rows=window_rows):
                forests = self.assembled(0, 4, window_rows=window_rows)
                self.assertEqual([forest.forest for forest in forests], [0, 1, 2, 3])
                self.assertEqual(rows(forests[0]), FOREST_1100)
                self.assertEqual(rows(forests[1]), FOREST_1200)
                self.assertEqual(forests[0].records.dtype, record_dtype(SHIN_UCHUU_RECORD))
                self.assertIsNotNone(forests[0].records.base)

    def test_a_forest_absent_from_some_slabs(self):
        """Forest 1300 has blocks in snapshots 2 and 1 only; the others contribute nothing."""
        self.assertEqual(rows(self.assembled(2, 3)[0]), FOREST_1300)

    def test_the_last_forest_and_a_range_starting_mid_dataset(self):
        self.assertEqual([rows(forest) for forest in self.assembled(3, 4)], [FOREST_1400])
        self.assertEqual(
            [(forest.forest, rows(forest)) for forest in self.assembled(1, 3)],
            [(1, FOREST_1200), (2, FOREST_1300)],
        )

    def test_the_payload_is_copied_verbatim(self):
        """Every payload field of every record equals its source row's column, byte for byte,
        the row found by MostBoundID in the slab the record's SnapNum names."""
        forests = self.assembled(0, 4)
        columns = {}
        for snap in self.dataset.snapshots:
            columns[snap] = self.dataset.read_rows(
                snap, PAYLOAD_FIELDS, 0, self.dataset.n_halos[snap]
            )
        checked = 0
        for forest in forests:
            for record in forest.records:
                slab = columns[int(record["SnapNum"])]
                row = int(np.flatnonzero(slab["MostBoundID"] == record["MostBoundID"])[0])
                for name in PAYLOAD_FIELDS:
                    self.assertEqual(record[name].tobytes(), slab[name][row].tobytes(), name)
                    self.assertEqual(record[name].dtype, slab[name].dtype, name)
                checked += 1
        self.assertEqual(checked, 19)

    def test_expected_totals_are_those_of_the_range_alone(self):
        """Index 0 of ``expected_totals`` is the range's first forest."""
        forests = self.assembled(0, 4, expected_totals=np.array([8, 6, 2, 3]))
        self.assertEqual([len(forest.records) for forest in forests], [8, 6, 2, 3])
        forests = self.assembled(1, 3, expected_totals=np.array([6, 2]))
        self.assertEqual([len(forest.records) for forest in forests], [6, 2])
        for window_rows in (1, DEFAULT_WINDOW_ROWS):
            with self.assertRaisesRegex(
                ConverterError, r"forest 2 has 2 halos, but the plan expects 3"
            ):
                self.assembled(1, 3, window_rows=window_rows, expected_totals=[6, 3])
        with self.assertRaisesRegex(
            ConverterError, r"1 expected totals for the 2 forests of \[1, 3\)"
        ):
            self.assembled(1, 3, expected_totals=[6])

    def test_the_memory_bound(self):
        """The stated function: two windows per slab plus the forest in flight, and the batch
        at the measured allowance of 160 B of temporaries per row."""
        self.assertEqual(
            memory_bound(10, 3, 7, 100, 50), (2 * 3 * 10 + 7) * 100 + 10 * (100 + 50 + 160)
        )
        assembler = ForestAssembler(self.dataset, SHIN_UCHUU_RECORD)
        # ForestIndex, HaloRankInForest, MostBoundID and the five links (int64), the three
        # target snapshots, Len and SnapNum (int32), three float32 scalars and three vectors
        self.assertEqual(assembler.row_bytes, 8 * 8 + 4 * 5 + 4 * 3 + 12 * 3)
        self.assertEqual(
            assembler.memory_bound(5_000_000),
            memory_bound(
                DEFAULT_WINDOW_ROWS, 6, 5_000_000, 132, record_dtype(SHIN_UCHUU_RECORD).itemsize
            ),
        )


class RefusalTests(CorrespondenceDataset):
    def test_refusals_from_one_way_mutations(self):
        def link_into_another_forest(values):
            values[0] = 4  # 1101's NextProgenitor, from 1102 (row 2) to forest 1's 1201

        def rank_out_of_range(values):
            values[0] = 9  # 1103, forest 0's rank 7, past the forest's eight halos

        def rank_repeated(values):
            values[0] = 6  # 1103's rank 7 repeats 1111's rank 6, so no halo has rank 7

        def wrong_companion(values):
            values[0] = 3  # 1300's FirstProgenitorSnapshot: forest 2 has no block at 3

        def null_companion(values):
            values[6] = -1  # 1401's DescendantSnapshot, its Descendant staying 5

        def null_central(values):
            values[2] = -1  # 1120's FirstHaloInFOFgroup

        def empty_slab(values):
            values[0] = 0  # 1100's FirstProgenitorSnapshot: snapshot 0 holds no row at all

        def out_of_order(values):
            values[1] = 1  # snapshot 4's ForestIndex [0, 0, 0, 0, 1, 1, 3] -> [0, 1, 0, ...]

        def relabel(values):
            values[0] = 3  # forest 2's row, given to forest 3 ...

        def rerank_to(rank):
            def rerank(values):
                values[0] = rank  # ... as its next rank, so forest 2 keeps no row

            return rerank

        cases = [
            (
                "a link into another forest's block",
                [(4, "NextProgenitor", link_into_another_forest)],
                r"forest 0, rank 3: NextProgenitor -> row 4 lies outside the forest's block of "
                r"snapshot 4 \(rows \[0, 4\)\)",
            ),
            (
                "a rank out of range",
                [(3, "HaloRankInForest", rank_out_of_range)],
                r"forest 0: rank 9 is outside \[0, 8\), the forest's halo count",
            ),
            (
                "a repeated rank",
                [(3, "HaloRankInForest", rank_repeated)],
                r"forest 0: rank 6 is held by more than one halo \(the 8 halos' ranks must be "
                r"0\.\.7 once each\)",
            ),
            (
                "a wrong companion snapshot",
                [(2, "FirstProgenitorSnapshot", wrong_companion)],
                r"forest 2, rank 0: FirstProgenitor -> row 0 names snapshot 3, in which the "
                r"forest has no block",
            ),
            (
                "a null companion beside a link",
                [(4, "DescendantSnapshot", null_companion)],
                r"forest 3, rank 1: Descendant is 5 but DescendantSnapshot is -1",
            ),
            (
                "a null FirstHaloInFOFgroup",
                [(5, "FirstHaloInFOFgroup", null_central)],
                r"forest 0, rank 2: FirstHaloInFOFgroup is -1",
            ),
            (
                "ForestIndex out of order",
                [(4, "ForestIndex", out_of_order)],
                r"snapshot_004\.h5: ForestIndex is not non-decreasing: row 2 holds 0 after 1",
                # one-row windows emit forest 0 after the window holding row 1, before the
                # window with row 2 is read: its five halos' ranks (0, 1, 2, 3, then 7) are
                # refused first
                r"forest 0: rank 7 is outside \[0, 5\)",
            ),
            (
                "a companion naming an empty slab",
                [(5, "FirstProgenitorSnapshot", empty_slab)],
                r"forest 0, rank 0: FirstProgenitor -> row 0 names snapshot 0, in which the "
                r"forest has no block",
            ),
            (
                "a forest with no rows",
                [
                    (2, "ForestIndex", relabel),
                    (2, "HaloRankInForest", rerank_to(3)),
                    (1, "ForestIndex", relabel),
                    (1, "HaloRankInForest", rerank_to(4)),
                ],
                r"forest 2 has no rows in any slab",
            ),
        ]
        for label, edits, message, *at_one_row in cases:
            with self.subTest(label):
                dataset = self.mutated(*edits)
                for window_rows, expected in (
                    (DEFAULT_WINDOW_ROWS, message),
                    (1, at_one_row[0] if at_one_row else message),
                ):
                    with self.assertRaisesRegex(ConverterError, expected):
                        self.assembled(0, 4, dataset, window_rows)

    def test_a_column_of_the_wrong_dtype_is_refused_before_any_row_is_read(self):
        target = self.work / "dataset"
        shutil.copytree(self.dataset_dir, target)
        retype_column(target, 4, "Len", np.int64)
        dataset = HorizontalDataset(target)
        with mock.patch.object(dataset, "read_rows", side_effect=AssertionError("a row read")):
            with self.assertRaisesRegex(
                ConverterError, r"snapshot_004\.h5: /halos/Len is int64, not the record's Len int32"
            ):
                ForestAssembler(dataset, SHIN_UCHUU_RECORD)
        retype_column(target, 1, "Pos", np.float64)
        with self.assertRaisesRegex(
            ConverterError,
            r"snapshot_001\.h5: /halos/Pos is float64\[3\], not the record's Pos float32\[3\]",
        ):
            ForestAssembler(HorizontalDataset(target), SHIN_UCHUU_RECORD)

    def test_a_record_field_without_a_column_is_refused(self):
        with self.assertRaisesRegex(
            ConverterError, r"snapshot_000\.h5: no /halos/M_Mean200 column, which the record's"
        ):
            ForestAssembler(self.dataset, MINI_MILLENNIUM_RECORD)

    def test_a_rank_the_link_type_cannot_hold_is_refused(self):
        """No fixture reaches 2^31 halos: the limit is lowered on the instance instead."""
        assembler = ForestAssembler(self.dataset, SHIN_UCHUU_RECORD)
        assembler.link_limit = 6
        with self.assertRaisesRegex(
            ConverterError, r"forest 0: rank 7 does not fit the record's link type int32"
        ):
            list(assembler.forests(0, 4))

    def test_ranges_and_windows_are_checked(self):
        for first, stop in ((2, 2), (-1, 2), (0, 5), (True, 2)):
            with self.subTest(first=first, stop=stop):
                with self.assertRaisesRegex(ConverterError, r"forest range \[.*\) is not within"):
                    self.assembled(first, stop)
        with self.assertRaisesRegex(ConverterError, r"window_rows must be a positive integer"):
            ForestAssembler(self.dataset, SHIN_UCHUU_RECORD, 0)


class TopologyTests(unittest.TestCase):
    """A progenitor chain of three, promoted halos, and the committed worked fixture."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="vertical_assemble_topology_"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @staticmethod
    def assembled(dataset_dir, first, stop):
        assembler = ForestAssembler(HorizontalDataset(dataset_dir), SHIN_UCHUU_RECORD)
        return [rows(forest) for forest in assembler.forests(first, stop)]

    def test_a_progenitor_chain_of_three(self):
        dataset_dir = convert(self.tmp / "head", [fixtures.progenitor_head_forest()])
        self.assertEqual(self.assembled(dataset_dir, 0, 1), [FOREST_2700])

    def test_a_promoted_halo_and_its_former_group(self):
        forest = fixtures.three_piece_forest()
        root = self.tmp / "three"
        source = test_rewriter.cut_source(root, forest, test_rewriter.cut_layout(forest, 3))
        table = test_rewriter.write_table(root, "cut", fixtures.three_piece_cut_table(), source)
        test_rewriter.cut(source, root / "out", root / "scratch", table)
        pieces = self.assembled(root / "out" / "dataset", 1, 5)
        self.assertEqual(pieces[0], PIECE_2500)
        self.assertEqual(pieces[2], PIECE_8001)
        self.assertEqual(pieces[3], PIECE_8002)

    def test_the_committed_worked_ascii_fixture(self):
        self.assertEqual(self.assembled(WORKED_ASCII, 0, 1), [WORKED_10])


class SourceRankConventionTests(unittest.TestCase):
    """The ``lhalo_binary`` route's committed fixtures: ``HaloRankInForest`` is the source's
    within-tree row, which the concatenation of the slab blocks from the highest snapshot down
    does not give in order (``worked_graph``'s forest holds ranks 0, 1, 3, 4, 2 in that
    order)."""

    FIXTURES = ("worked_graph", "forest_blocks", "wide_slab", "three_snapshot_chain", "adjacent")

    def test_the_worked_graph_ranks_are_not_in_snapshot_order(self):
        dataset = HorizontalDataset(MM_HORIZONTAL_DATA / "worked_graph")
        ranks = []
        for snap in reversed(dataset.snapshots):
            block = dataset.read_rows(snap, ("HaloRankInForest",), 0, dataset.n_halos[snap])
            ranks += block["HaloRankInForest"].tolist()
        self.assertEqual(ranks, [0, 1, 3, 4, 2])

    def test_every_record_is_its_source_row_and_every_link_its_targets_rank(self):
        """Every record's payload equals the dataset row with the same (forest, rank), and every
        link names the rank of the row the source link points at, found by the index built here
        from the dataset's columns alone.

        ``worked_graph`` and ``three_snapshot_chain`` carry links whose companion snapshot is
        not the owner's neighbour (``Descendant`` from snapshot 0 to snapshot 2, say), so they
        are mapped through their companion columns, not through owner +- 1; ``adjacent`` has
        none."""
        gapped = {}
        for name in self.FIXTURES:
            dataset = HorizontalDataset(MM_HORIZONTAL_DATA / name)
            windows = (
                (2, DEFAULT_WINDOW_ROWS) if max(dataset.n_halos) < 100 else (DEFAULT_WINDOW_ROWS,)
            )
            for window_rows in windows:
                with self.subTest(fixture=name, window_rows=window_rows):
                    assembler = ForestAssembler(dataset, MM_HORIZONTAL_RECORD, window_rows)
                    gapped[name] = self.check_against_the_source_rows(dataset, assembler)
        self.assertGreater(gapped["worked_graph"], 0)
        self.assertGreater(gapped["three_snapshot_chain"], 0)
        self.assertEqual(gapped["adjacent"], 0)

    def check_against_the_source_rows(self, dataset, assembler):
        columns = {
            snap: dataset.read_rows(snap, assembler.columns, 0, dataset.n_halos[snap])
            for snap in dataset.snapshots
        }
        row_of = {}
        for snap, block in columns.items():
            for row, key in enumerate(zip(block["ForestIndex"], block["HaloRankInForest"])):
                key = (int(key[0]), int(key[1]))
                self.assertNotIn(key, row_of)
                row_of[key] = (snap, row)
        forests = list(assembler.forests(0, dataset.n_forests_total))
        self.assertEqual(sum(len(forest.records) for forest in forests), len(row_of))
        gapped = 0
        for forest in forests:
            for rank, record in enumerate(forest.records):
                snap, row = row_of[(forest.forest, rank)]
                source = columns[snap]
                for field in assembler.payload_fields:
                    self.assertEqual(
                        record[field].tobytes(), source[field][row].tobytes(), (forest.forest, rank)
                    )
                for link in LINK_FIELDS:
                    target = int(source[link][row])
                    expected = -1
                    if target != -1:
                        companion = LINK_SNAPSHOT_COLUMNS.get(link)
                        target_snap = int(source[companion][row]) if companion else snap
                        gapped += abs(target_snap - snap) > 1
                        target_block = columns[target_snap]
                        self.assertEqual(int(target_block["ForestIndex"][target]), forest.forest)
                        expected = int(target_block["HaloRankInForest"][target])
                    self.assertEqual(int(record[link]), expected, (forest.forest, rank, link))
        return gapped


class ManyForestsTests(unittest.TestCase):
    """A thousand small forests: the per-forest Python work is a bounded number of calls, the
    reads are windows and search probes only, and the window size changes nothing."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="vertical_assemble_many_"))
        cls.dataset = HorizontalDataset(convert(cls.tmp / "many", fixtures.small_forests(1000)))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def consume(self, stop, window_rows=2048):
        assembler = ForestAssembler(self.dataset, SHIN_UCHUU_RECORD, window_rows)
        return [rows(forest) for forest in assembler.forests(0, stop)]

    @staticmethod
    def calls_during(function):
        """Python and C calls made while ``function`` runs."""
        calls = 0

        def profile(_frame, event, _arg):
            nonlocal calls
            if event in ("call", "c_call"):
                calls += 1

        sys.setprofile(profile)
        try:
            function()
        finally:
            sys.setprofile(None)
        return calls

    def test_the_per_forest_work_is_bounded(self):
        """Both ranges read each slab's rows in one window and the search probes of two binary
        searches per slab, so the difference in calls is the per-forest cost of the iteration
        alone (consumed by a zero-length deque): the generator resumptions and the record's
        construction, three calls per forest; a bound of five holds that."""
        counts = {}
        probes_allowed = sum(2 * math.ceil(math.log2(rows + 1)) for rows in self.dataset.n_halos)
        for stop in (250, 1000):
            assembler = ForestAssembler(self.dataset, SHIN_UCHUU_RECORD, 2048)
            with mock.patch.object(
                self.dataset, "read_rows", wraps=self.dataset.read_rows
            ) as reads:

                def exhaust(assembler=assembler, stop=stop):
                    collections.deque(assembler.forests(0, stop), maxlen=0)

                counts[stop] = self.calls_during(exhaust)
                names = [call.args[1] for call in reads.call_args_list]
            self.assertEqual(sum(1 for name in names if name == assembler.columns), 2, stop)
            self.assertLessEqual(
                sum(1 for name in names if name == ("ForestIndex",)), probes_allowed
            )
            self.assertEqual(
                len(names),
                sum(1 for name in names if name in (assembler.columns, ("ForestIndex",))),
            )
        self.assertLessEqual(counts[1000] - counts[250], 5 * 750)

    def test_no_slab_reads_a_row_past_the_range(self):
        """Over the range [100, 400) each slab's windows tile exactly its rows of the range:
        none starts before the range's first row or reads at or past its first row of the
        stop. Every other read is a one-row probe of ``ForestIndex``."""
        first, stop = 100, 400
        bounds = {}
        for snap in self.dataset.snapshots:
            forest = self.dataset.read_column(snap, "ForestIndex")
            bounds[snap] = (
                int(np.searchsorted(forest, first, "left")),
                int(np.searchsorted(forest, stop, "left")),
            )
        expected = self.consume(1000)[first:stop]
        for window_rows in (1, 7, 2048):
            with self.subTest(window_rows=window_rows):
                assembler = ForestAssembler(self.dataset, SHIN_UCHUU_RECORD, window_rows)
                with mock.patch.object(
                    self.dataset, "read_rows", wraps=self.dataset.read_rows
                ) as reads:
                    assembled = [rows(forest) for forest in assembler.forests(first, stop)]
                self.assertEqual(assembled, expected)
                windows = collections.defaultdict(list)
                for call in reads.call_args_list:
                    snap, names, start, end = call.args
                    if names == ("ForestIndex",):
                        self.assertEqual(end - start, 1)
                    else:
                        self.assertEqual(names, assembler.columns)
                        windows[snap].append((start, end))
                for snap, (low, high) in bounds.items():
                    read = windows.get(snap, [])
                    self.assertEqual(read[0][0] if read else low, low, snap)
                    self.assertEqual(read[-1][1] if read else high, high, snap)
                    self.assertTrue(all(a[1] == b[0] for a, b in zip(read, read[1:])), snap)
                    self.assertTrue(all(end - start <= window_rows for start, end in read), snap)
                self.assertTrue(sum(len(read) for read in windows.values()) >= 2)

    def test_every_window_size_gives_the_same_forests(self):
        expected = self.consume(1000)
        self.assertEqual(len(expected), 1000)
        self.assertEqual(expected[0], [(100000, -1, -1, -1, 0, -1, 5)])
        self.assertEqual(
            expected[1], [(100010, -1, 1, -1, 0, -1, 5), (100011, 0, -1, -1, 1, -1, 4)]
        )
        for window_rows in (1, 64, 1000):
            with self.subTest(window_rows=window_rows):
                self.assertEqual(self.consume(1000, window_rows), expected)


if __name__ == "__main__":
    unittest.main()
