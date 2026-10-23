"""The forest census (convert/mimic-convert/forest_census.py and census/), its
bounded dataset reader (horizontal_dataset.py) and the index-file reader
(source_index.py).

Oracles:

- the partition against the literal cases of the C unit test
  (``tests/unit/test_horizontal_partition.c``) and the ``forest_blocks``
  fixture's recorded chunkings (``tests/manual/test_distributed_identity.py``,
  ``simulations/mini-millennium-horizontal/_tests/integration/test_chunked_sweep.py``);
  the C test owns the brute-force oracle;
- occupancy and labelling against counts written out by hand from the fixture
  topology, on version 3 datasets the converter's own pipeline writes from
  :mod:`fixtures`' correspondence forests (each ``#tree`` root id is its
  terminal halo's id), the labels also against each halo's root found by
  walking ``desc_id`` in the fixture specification;
- the index reader against hand-written index files, and against the sidecar
  ordinals the ASCII adapter itself writes;
- the identity record and the reader's refusals against the committed version 3
  fixture (``simulations/micro-uchuu-ascii-horizontal/_tests/data/worked_ascii``),
  whose counts, scale factors and forest ids are restated from its source
  (``.../_tests/data/source``);
- the decided table against tables written out by hand from the fixture
  topology, on version 3 datasets the converter writes from the same forests
  under two catalogue file layouts, and against the partition that cuts every
  co-membership (a pair of trees, one holding a FoF member of a group centred
  in the other) ended
  before the final snapshot, computed independently from the raw slab columns
  inside the test (:func:`all_ended_pieces`); naming, invariants, promotions,
  the progenitor-order change, the affected-history bracket and the partition
  with the pieces installed against values derived by hand from the same
  topology.
"""

import contextlib
import hashlib
import io
import json
import os
import random
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
import forest_census  # noqa: E402
from census import (  # noqa: E402
    aggregate,
    cut,
    cut_table,
    occupancy,
    partition,
    table,
    trees,
)
from census.common import in_sorted  # noqa: E402
from errors import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    capture_stderr,
    convert_ascii,
)
from horizontal_dataset import HorizontalDataset  # noqa: E402
from source_index import SourceIndex  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
#: The committed version 3 fixture: three forests (ids 10, 20 and 30) over six
#: snapshots, converted from ``../source`` by the package's ``regenerate.sh``.
WORKED_ASCII = (
    REPO_ROOT / "simulations" / "micro-uchuu-ascii-horizontal" / "_tests" / "data" / "worked_ascii"
)
#: The fixture's halos per snapshot and its forest ids, restated from its
#: source tree file and ``forests.list``.
WORKED_ASCII_N_HALOS = (0, 1, 1, 1, 6, 4)
WORKED_ASCII_FOREST_IDS = (10, 20, 30)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def terminal_root(halos, halo_id):
    """The id of the halo ``halo_id``'s descendant chain ends at."""
    while halos[halo_id].desc_id != -1:
        halo_id = halos[halo_id].desc_id
    return halo_id


def run_quietly(function, *args, **kwargs):
    with capture_stderr():
        return function(*args, **kwargs)


def read_slab_column(dataset_dir, snap, name):
    with h5py.File(Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap), "r") as handle:
        return handle["halos"][name][...]


def copy_worked_ascii(destination: Path) -> Path:
    shutil.copytree(WORKED_ASCII, destination)
    return destination


def set_header(name, value):
    """An edit setting one ``/header`` attribute of an open snapshot file."""

    def edit(handle):
        handle["header"].attrs[name] = value

    return edit


#: The correspondence forests' file layout, with the early-ending forest 1300
#: of the negative fixture: file 0 holds the ``#tree`` markers 1100, 1110,
#: 1300, 1120, 1200 -- forest 1100's markers repeat around forest 1300's (F, F,
#: G, F) -- and file 1 holds 1400, 1210, so forest 1200 spans both files.
def correspondence_layout():
    multi, spanning, early, single = fixtures.early_ending_correspondence_forests()
    return [
        [multi.trees[0], multi.trees[1], early.trees[0], multi.trees[2], spanning.trees[0]],
        [single.trees[0], spanning.trees[1]],
    ]


# ---------------------------------------------------------------------------
# The partition
# ---------------------------------------------------------------------------


def literal_cut_forests(weights, ntask):
    """``horizontal_partition_cut_forests`` of ``src/core/horizontal_partition.c``
    transcribed line by line: a binary search over the capacity of a
    forest-by-forest greedy packing."""

    def greedy_pack(capacity, cuts=None):
        ranges, filled = 1, 0
        for forest, weight in enumerate(weights):
            if weight > capacity - filled:
                if cuts is not None:
                    cuts[ranges] = forest
                ranges += 1
                filled = 0
            filled += weight
        return ranges

    low, high = max(weights, default=0), sum(weights)
    while low < high:
        middle = low + (high - low) // 2
        if greedy_pack(middle) <= ntask:
            high = middle
        else:
            low = middle + 1
    cuts = [0] * (ntask + 1)
    ranges = greedy_pack(low, cuts)
    for rank in range(ranges, ntask + 1):
        cuts[rank] = len(weights)
    return cuts


def literal_partition_cut(weights, ntask, nchunk):
    """``horizontal_partition_cut``, transcribed on :func:`literal_cut_forests`."""
    task_cuts = literal_cut_forests(weights, ntask)
    cuts = [0] * (ntask * nchunk + 1)
    for task in range(ntask + 1):
        cuts[task * nchunk] = task_cuts[task]
    if nchunk > 1:
        for task in range(ntask):
            low = task_cuts[task]
            chunk_cuts = literal_cut_forests(weights[low : task_cuts[task + 1]], nchunk)
            for chunk in range(nchunk + 1):
                cuts[task * nchunk + chunk] = chunk_cuts[chunk] + low
    return cuts


class TestPartition(unittest.TestCase):
    def test_the_prefix_sum_packing_equals_the_literal_greedy_packing(self):
        generator = random.Random(20261017)
        for _case in range(300):
            n_forests = generator.randint(0, 60)
            style = generator.choice(("small", "zero_heavy", "heavy_tailed"))
            if style == "small":
                weights = [generator.randint(1, 9) for _ in range(n_forests)]
            elif style == "zero_heavy":
                weights = [generator.choice((0, 0, 0, 1, 2, 7)) for _ in range(n_forests)]
            else:
                weights = [int(generator.paretovariate(1.1)) for _ in range(n_forests)]
            ntask, nchunk = generator.randint(1, 8), generator.randint(1, 32)
            self.assertEqual(
                partition.partition_cut(weights, ntask, nchunk),
                literal_partition_cut(weights, ntask, nchunk),
                (weights, ntask, nchunk),
            )

    def test_the_literal_cases_of_the_c_unit_test(self):
        """The cases of ``tests/unit/test_horizontal_partition.c``, which anchor the
        transcription the random oracle uses."""
        with self.subTest("a heavy forest shares a range with a light neighbour"):
            # weights {1, 5, 1} on two ranks
            self.assertEqual(partition.cut_forests([1, 5, 1], 2), [0, 2, 3])
            self.assertEqual(partition.partition_cut([1, 5, 1], 2, 1), [0, 2, 3])
        with self.subTest("a single rank and an empty dataset"):
            # weights {3, 0, 7, 1} on one rank
            self.assertEqual(partition.cut_forests([3, 0, 7, 1], 1), [0, 4])
            for ntask in range(1, 6):
                self.assertEqual(partition.cut_forests([], ntask), [0] * (ntask + 1))
                self.assertEqual(partition.partition_cut([], ntask, 3), [0] * (3 * ntask + 1))
        with self.subTest("zero-weight forests ride in a neighbouring range"):
            # forests 0, 2, 3 and 5 empty
            self.assertEqual(partition.cut_forests([0, 3, 0, 0, 2, 0], 2), [0, 4, 6])
        with self.subTest("peak weights move the cut"):
            # slabs whose peaks are [3, 3, 4] where the widest slab alone counts
            # [3, 3, 0]: on two ranks the peaks pack at capacity 6 as [3, 3] | [4],
            # the widest slab's counts at 3 as [3] | [3, 0]
            self.assertEqual(partition.cut_forests([3, 3, 4], 2), [0, 2, 3])
            self.assertEqual(partition.cut_forests([3, 3, 0], 2), [0, 1, 3])

    def test_forest_blocks_recorded_chunkings(self):
        # the forest_blocks fixture's peak weights, each forest's largest row
        # count in any one slab; the widest slab, snapshot 5, counts
        # [2, 1, 7, 2, 2, 3] (forest 2 holds 8 rows at snapshot 6). Re-derived by
        # hand for the peaks, the cuts are the same: G = 2 packs at capacity 11
        # as [2, 1, 8] | [2, 2, 3] and G >= 3 at 8, the heaviest forest, as
        # [2, 1] | [8] | [2, 2, 3]
        weights = [2, 1, 8, 2, 2, 3]
        # serial chunk legs (test_chunked_sweep.py): [0, 3, 6] at G = 2 and
        # [0, 2, 3, 6, 6, ...] for every G >= 3, so G = 8 has five idle chunks
        self.assertEqual(partition.partition_cut(weights, 1, 2), [0, 3, 6])
        self.assertEqual(partition.partition_cut(weights, 1, 3), [0, 2, 3, 6])
        self.assertEqual(partition.partition_cut(weights, 1, 8), [0, 2, 3] + [6] * 6)
        # MPI chunk legs (2, 2) and (3, 3), derived by hand: tasks [0, 3, 6]
        # (capacity 11) then chunks of [2, 1, 8] (capacity 8) and [2, 2, 3]
        # (capacity 4); tasks [0, 2, 3, 6] (capacity 8) then chunks of [2, 1],
        # [8] (two idle) and [2, 2, 3]
        self.assertEqual(partition.partition_cut(weights, 2, 2), [0, 2, 3, 5, 6])
        self.assertEqual(partition.partition_cut(weights, 3, 3), [0, 1, 2, 2, 3, 3, 3, 4, 5, 6])
        # the task level packs at the smallest feasible capacity (8, the heaviest
        # forest), so eight tasks use three ranges and five trailing ones idle
        self.assertEqual(partition.cut_forests(weights, 8), [0, 2, 3] + [6] * 6)

    def test_invalid_inputs_are_refused(self):
        with self.assertRaisesRegex(ConverterError, "at least one rank"):
            partition.cut_forests([1], 0)
        with self.assertRaisesRegex(ConverterError, "at least one chunk"):
            partition.partition_cut([1], 1, 0)
        with self.assertRaisesRegex(ConverterError, "forest 1 is negative"):
            partition.cut_forests([1, -2, 3], 2)
        with self.assertRaisesRegex(ConverterError, "beyond int64 at forest 1"):
            partition.cut_forests([1 << 62, 1 << 62, 1], 2)

    def test_range_rows_apply_the_cuts_to_sparse_slab_counts(self):
        forests = np.array([0, 2, 3, 5], dtype=np.int64)
        counts = np.array([4, 1, 6, 2], dtype=np.int32)
        np.testing.assert_array_equal(
            partition.range_rows(forests, counts, [0, 1, 3, 3, 6]), [4, 1, 0, 8]
        )

    def test_a_shared_prefix_gives_the_same_ranges(self):
        generator = np.random.default_rng(31)
        forests = np.sort(generator.choice(500, 120, replace=False)).astype(np.int64)
        counts = generator.integers(1, 50, forests.size).astype(np.int32)
        prefix = partition.slab_prefix(counts)
        for ntask, nchunk in ((1, 1), (2, 3), (4, 8), (8, 32)):
            weights = np.zeros(500, dtype=np.int64)
            weights[forests] = counts
            cuts = partition.partition_cut(weights, ntask, nchunk)
            np.testing.assert_array_equal(
                partition.range_rows(forests, counts, cuts, prefix),
                partition.range_rows(forests, counts, cuts),
            )
        np.testing.assert_array_equal(partition.slab_prefix(np.zeros(0, dtype=np.int32)), [0])


# ---------------------------------------------------------------------------
# The index files
# ---------------------------------------------------------------------------


class TestSourceIndex(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="source_index_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def write(self, forests_rows, location_rows):
        forests_list = self.tmp / "forests.list"
        forests_list.write_text(
            "#TreeRootID ForestID\n" + "".join("{} {}\n".format(*row) for row in forests_rows)
        )
        locations = self.tmp / "locations.dat"
        locations.write_text(
            "#TreeRootID FileID Offset Filename\n"
            + "".join("{} {} {} {}\n".format(*row) for row in location_rows)
        )
        return forests_list, locations

    #: File a.dat holds the markers 10, 11, 20, 12, 30 (forests F, F, G, F, H);
    #: b.dat holds 31, 40 (forests H, K). Rows are deliberately unsorted.
    FORESTS = [(40, 4), (12, 1), (30, 3), (10, 1), (20, 2), (31, 3), (11, 1)]
    LOCATIONS = [
        (31, 1, 50, "b.dat"),
        (20, 0, 300, "a.dat"),
        (10, 0, 100, "a.dat"),
        (40, 1, 150, "b.dat"),
        (12, 0, 400, "a.dat"),
        (11, 0, 200, "a.dat"),
        (30, 0, 500, "a.dat"),
    ]

    def test_arrays_are_joined_and_sorted_by_tree_root(self):
        index = SourceIndex.load(*self.write(self.FORESTS, self.LOCATIONS))
        np.testing.assert_array_equal(index.tree_roots, [10, 11, 12, 20, 30, 31, 40])
        np.testing.assert_array_equal(index.forest_ids, [1, 1, 1, 2, 3, 3, 4])
        np.testing.assert_array_equal(index.file_ids, [0, 0, 0, 0, 0, 1, 1])
        np.testing.assert_array_equal(index.offsets, [100, 200, 400, 300, 500, 50, 150])
        for array in (index.tree_roots, index.forest_ids, index.file_ids, index.offsets):
            self.assertEqual(array.dtype, np.int64)
        self.assertEqual(index.filenames, {0: "a.dat", 1: "b.dat"})
        np.testing.assert_array_equal(index.find(np.array([12, 13, 40])), [2, -1, 6])

    def test_forest_table_ranks_units_by_first_marker(self):
        table = SourceIndex.load(*self.write(self.FORESTS, self.LOCATIONS)).forest_table()
        np.testing.assert_array_equal(table.forest_ids, [1, 2, 3, 4])
        np.testing.assert_array_equal(table.n_trees, [3, 1, 2, 1])
        files = [table.files[table.file_starts[i] : table.file_starts[i + 1]] for i in range(4)]
        self.assertEqual([f.tolist() for f in files], [[0], [0], [0, 1], [1]])
        # F's markers F, F, (G), F: one unit, rank 0; G is rank 1 although an F
        # marker follows it; H spans files; in b.dat H's marker comes first, so
        # K is rank 1 there
        np.testing.assert_array_equal(table.file_id, [0, 0, -1, 1])
        np.testing.assert_array_equal(table.unit_ordinal, [0, 1, -1, 1])

    def test_marker_ordinals_rank_each_marker_within_its_file(self):
        index = SourceIndex.load(*self.write(self.FORESTS, self.LOCATIONS))
        # roots 10, 11, 12, 20, 30 are a.dat's markers at offsets 100, 200, 400, 300, 500;
        # 31 and 40 are b.dat's at 50 and 150
        np.testing.assert_array_equal(index.marker_ordinals(), [0, 1, 3, 2, 4, 0, 1])

    def test_a_root_in_one_file_only_is_refused(self):
        with self.assertRaisesRegex(ConverterError, r"1 tree root\(s\) only in .*forests.list"):
            SourceIndex.load(*self.write(self.FORESTS + [(99, 9)], self.LOCATIONS))
        with self.assertRaisesRegex(ConverterError, r"1 only in .*locations.dat \(e.g. \[99\]"):
            SourceIndex.load(*self.write(self.FORESTS, self.LOCATIONS + [(99, 1, 900, "b.dat")]))

    def test_a_repeated_root_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "listed more than once; examples: \\[12\\]"):
            SourceIndex.load(*self.write(self.FORESTS + [(12, 1)], self.LOCATIONS))


class TestIndexCheck(unittest.TestCase):
    ROOTS = np.array([10, 20], dtype=np.int64)
    INDEX_FORESTS = np.array([1, 2], dtype=np.int64)
    TREE_FOREST = np.array([0, 1], dtype=np.int64)

    def check(self, sidecar):
        return table.check_index(
            self.ROOTS, self.INDEX_FORESTS, self.ROOTS, self.TREE_FOREST, np.array(sidecar)
        )

    def test_the_largest_forest_id_must_agree(self):
        self.assertEqual(self.check([1, 2])["verdict"], "pass")
        # a sidecar forest without a tree: the fresh ids would start above id 9, not above 2
        record = self.check([1, 2, 9])
        self.assertEqual(record["verdict"], "fail")
        self.assertEqual(record["max_forest_id"], {"index_files": 2, "dataset": 9})


class TestConservation(unittest.TestCase):
    def test_sparse_file_ids_are_summed_by_listed_file(self):
        far = 2_000_000_000  # a dense vector over FileID would need 16 GB
        index = SourceIndex(
            tree_roots=np.array([10, 20, 30]),
            forest_ids=np.array([1, 2, 3]),
            file_ids=np.array([far, 5, far]),
            offsets=np.zeros(3, dtype=np.int64),
            filenames={5: "a.dat", far: "b.dat"},
        )
        record = trees.conservation(np.array([10, 20, 30, 99]), np.array([3, 4, 5, 6]), index)
        self.assertEqual(
            [(f["file_id"], f["census_halos"]) for f in record["files"]], [(5, 4), (far, 8)]
        )
        self.assertEqual(record["unattributed_halos"], 6)


# ---------------------------------------------------------------------------
# The dataset reader and the identity record (the committed fixture)
# ---------------------------------------------------------------------------


class TestCommittedDataset(unittest.TestCase):
    """The reader, the identity record and the census's refusals on the
    committed version 3 fixture (:data:`WORKED_ASCII`), or on an edited copy."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="census_committed_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_identity_record(self):
        forest_ids = np.array(WORKED_ASCII_FOREST_IDS, dtype="<i8")
        self.assertEqual(
            HorizontalDataset(WORKED_ASCII).identity(),
            {
                "format_version": 3,
                "source_format": "consistent_trees_ascii",
                "n_forests_total": 3,
                "n_halos": list(WORKED_ASCII_N_HALOS),
                "forest_id_sha256": hashlib.sha256(forest_ids.tobytes()).hexdigest(),
            },
        )

    def test_header_and_bounded_block_reads(self):
        dataset = HorizontalDataset(WORKED_ASCII)
        self.assertEqual(dataset.snapshots, tuple(range(6)))
        a_list = (WORKED_ASCII / "worked_ascii.a_list").read_text().split()
        self.assertEqual(list(dataset.scale_factors), [float(a) for a in a_list])
        self.assertEqual(dataset.header(4)["n_halos"], 6)
        whole = read_slab_column(WORKED_ASCII, 4, "MostBoundID")
        blocks = list(dataset.iter_column(4, "MostBoundID", block_rows=4))
        self.assertEqual([(start, block.size) for start, block in blocks], [(0, 4), (4, 2)])
        np.testing.assert_array_equal(np.concatenate([b for _s, b in blocks]), whole)
        self.assertEqual(list(dataset.iter_column(0, "MostBoundID")), [])
        with self.assertRaisesRegex(ConverterError, "no /halos/Nothing"):
            list(dataset.iter_column(4, "Nothing"))
        with self.assertRaisesRegex(ConverterError, "positive integer"):
            list(dataset.iter_column(4, "MostBoundID", block_rows=0))

    def test_a_missing_snapshot_is_refused(self):
        broken = copy_worked_ascii(self.tmp / "broken")
        (broken / "snapshot_003.h5").rename(self.tmp / "aside.h5")
        with self.assertRaisesRegex(ConverterError, "missing \\['snapshot_003.h5'\\]"):
            HorizontalDataset(broken)

    def test_trees_follow_the_descendant_chains(self):
        dataset = HorizontalDataset(WORKED_ASCII)
        summary = run_quietly(trees.run_trees, dataset, self.tmp / "agg")
        roots = trees.load_roots(self.tmp / "agg")
        np.testing.assert_array_equal(roots, [1010, 1020, 2010, 2011, 3010])
        expected = {5: [0, 1, 2, 3], 4: [0, 0, 0, 1, 2, 3], 3: [0], 2: [4], 1: [4], 0: []}
        for snap, labels in expected.items():
            np.testing.assert_array_equal(trees.load_labels(self.tmp / "agg", snap), labels)
        self.assertEqual(summary["root_correspondence"]["verdict"], "unchecked")
        self.assertEqual(summary["root_correspondence"]["roots_not_in_last_snapshot"]["count"], 1)
        self.assertEqual(summary["largest_tree"]["root_id"], 1010)
        self.assertEqual(summary["largest_tree"]["total_halos"], 5)

    def edited(self, name, file_name, edit):
        """A copy of the fixture with ``edit(handle)`` applied to one file."""
        copy = copy_worked_ascii(self.tmp / name)
        with h5py.File(copy / file_name, "r+") as handle:
            edit(handle)
        return copy

    def test_a_format_version_other_than_3_is_refused_by_name(self):
        every_file = copy_worked_ascii(self.tmp / "version_2")
        for path in every_file.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                handle["header"].attrs["format_version"] = np.int32(2)
        first_file = self.edited(
            "version_2_first", "snapshot_000.h5", set_header("format_version", np.int32(2))
        )
        for copy in (every_file, first_file):
            with self.assertRaisesRegex(
                ConverterError,
                "snapshot_000.h5: format_version 2 is not supported; this reader opens "
                "format_version 3 only",
            ):
                HorizontalDataset(copy)

    def test_inconsistent_datasets_are_refused_at_open(self):
        def drop_header(name):
            def edit(handle):
                del handle["header"].attrs[name]

            return edit

        def shrink_sidecar(handle):
            del handle["ForestID"]
            handle.create_dataset("ForestID", data=np.array([10, 20], dtype=np.int64))

        cases = (
            (
                "mixed",
                "snapshot_002.h5",
                set_header("format_version", np.int32(2)),
                "format_version differs between snapshot files",
            ),
            (
                "run_scoped",
                "snapshot_003.h5",
                set_header("n_forests_total", np.int64(4)),
                "n_forests_total differs between snapshot files",
            ),
            (
                "sidecar",
                "forests.h5",
                shrink_sidecar,
                "/ForestID holds 2 entries but n_forests_total is 3",
            ),
            (
                "renamed",
                "snapshot_001.h5",
                set_header("snapshot_number", np.int32(7)),
                "snapshot_001.h5: header snapshot_number 7 disagrees with the file name",
            ),
            (
                "no_scale",
                "snapshot_004.h5",
                drop_header("scale_factor"),
                "snapshot_004.h5: header attribute scale_factor missing",
            ),
        )
        for name, file_name, edit, message in cases:
            with self.assertRaisesRegex(ConverterError, message):
                HorizontalDataset(self.edited(name, file_name, edit))
        # links_adjacent absent from every file passes the agreement test, so
        # the presence check is what catches it
        no_links = copy_worked_ascii(self.tmp / "no_links")
        for path in no_links.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                del handle["header"].attrs["links_adjacent"]
        with self.assertRaisesRegex(ConverterError, "header attribute links_adjacent missing"):
            HorizontalDataset(no_links)

    def test_a_forest_index_out_of_range_is_refused_by_occupancy(self):
        def edit(handle):
            handle["halos"]["ForestIndex"][0] = 3

        dataset = HorizontalDataset(self.edited("forest_range", "snapshot_004.h5", edit))
        with self.assertRaisesRegex(ConverterError, r"ForestIndex outside \[0, 3\)"):
            occupancy.run_occupancy(dataset, self.tmp / "agg")

    def test_broken_descendant_links_are_refused_by_trees(self):
        def link_last(handle):
            handle["halos"]["Descendant"][0] = 0

        def link_beyond(handle):
            handle["halos"]["Descendant"][0] = 99

        last = HorizontalDataset(self.edited("link_last", "snapshot_005.h5", link_last))
        with self.assertRaisesRegex(ConverterError, "the last slab holds 1 non-null Descendant"):
            trees.run_trees(last, self.tmp / "agg_last")
        beyond = HorizontalDataset(self.edited("link_beyond", "snapshot_004.h5", link_beyond))
        with self.assertRaisesRegex(
            ConverterError, r"Descendant outside \[0, 4\) of the next slab"
        ):
            trees.run_trees(beyond, self.tmp / "agg_beyond")

    def test_a_gapped_dataset_is_refused_by_trees(self):
        gapped = copy_worked_ascii(self.tmp / "gapped")
        for path in gapped.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                handle["header"].attrs["links_adjacent"] = np.int32(0)
        with self.assertRaisesRegex(ConverterError, "links_adjacent is 0"):
            trees.run_trees(HorizontalDataset(gapped), self.tmp / "agg")

    def test_trees_refuses_before_loading_the_index_files(self):
        gapped = copy_worked_ascii(self.tmp / "gapped")
        for path in gapped.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                handle["header"].attrs["links_adjacent"] = np.int32(0)
        foreign = self.tmp / "foreign_agg"
        foreign.mkdir()
        identity = HorizontalDataset(WORKED_ASCII).identity()
        aggregate.write_json(foreign / aggregate.IDENTITY_NAME, dict(identity, n_forests_total=9))
        cases = (
            (gapped, self.tmp / "agg_gapped", "links_adjacent is 0"),
            (WORKED_ASCII, foreign, "produced from a different dataset"),
        )
        for dataset, agg, message in cases:
            # the index files do not exist: loading them first would be another refusal
            command = ["trees", "--dataset", str(dataset), "--aggregate", str(agg)]
            command += ["--forests-list", str(self.tmp / "forests.list")]
            command += ["--locations", str(self.tmp / "locations.dat")]
            with capture_stderr() as err:
                status = forest_census.main(command)
            self.assertEqual(status, 2, message)
            self.assertIn(message, err.text)
        # nothing was bound or written by either refusal
        self.assertFalse((self.tmp / "agg_gapped").exists())
        self.assertEqual(sorted(p.name for p in foreign.iterdir()), [aggregate.IDENTITY_NAME])

    def test_a_forest_index_out_of_range_is_refused_by_trees(self):
        # slab 5's halos are all terminal, slab 4's are all linked
        for name, file_name, row, value, message in (
            ("terminal", "snapshot_005.h5", 0, 3, "snapshot_005.h5: ForestIndex 3 at row 0 is out"),
            ("negative", "snapshot_005.h5", 2, -1, "snapshot_005.h5: ForestIndex -1 at row 2 is"),
            ("linked", "snapshot_004.h5", 1, 3, "snapshot_004.h5: ForestIndex 3 at row 1 is out"),
        ):

            def edit(handle, row=row, value=value):
                handle["halos"]["ForestIndex"][row] = value

            dataset = HorizontalDataset(self.edited(name, file_name, edit))
            agg = self.tmp / "agg_{}".format(name)
            with capture_stderr() as err:
                status = forest_census.main(
                    ["trees", "--dataset", str(dataset.directory), "--aggregate", str(agg)]
                )
            self.assertEqual(status, 2, name)
            self.assertIn(message, err.text)
            self.assertFalse((agg / "trees" / "summary.json").exists(), name)

    def test_occupancy_refuses_a_slab_that_is_not_forest_blocked(self):
        def edit(handle):
            handle["halos"]["ForestIndex"][2] = 1  # 0, 0, 1, 0, 1, 1

        dataset = HorizontalDataset(self.edited("unblocked", "snapshot_004.h5", edit))
        for block_rows in (1 << 22, 3):  # the fall within a block, and across two
            with self.assertRaisesRegex(ConverterError, "ForestIndex falls from 1 at row 2 to 0"):
                occupancy.run_occupancy(dataset, self.tmp / "agg", block_rows)

    def test_an_occupancy_count_beyond_int32_names_the_forest(self):
        dataset = HorizontalDataset(WORKED_ASCII)
        with mock.patch.object(occupancy, "INT32_MAX", 3):
            with self.assertRaisesRegex(ConverterError, "snapshot_004.h5: forest 0 holds 4 rows"):
                occupancy.run_occupancy(dataset, self.tmp / "agg")

    def test_more_roots_than_int32_ordinals_is_refused_during_the_scan(self):
        with mock.patch.object(trees, "INT32_MAX", 4):  # the fixture has 5 terminal halos
            with self.assertRaisesRegex(ConverterError, "more than 2.31 - 1 terminal roots"):
                trees.enumerate_roots(HorizontalDataset(WORKED_ASCII))

    def test_a_repeated_terminal_id_is_refused(self):
        repeated = copy_worked_ascii(self.tmp / "repeated")
        with h5py.File(repeated / "snapshot_005.h5", "r+") as handle:
            handle["halos"]["MostBoundID"][3] = 1010
        with self.assertRaisesRegex(ConverterError, "more than once.*\\[1010\\]"):
            trees.run_trees(HorizontalDataset(repeated), self.tmp / "agg")


# ---------------------------------------------------------------------------
# The census on version 3 datasets the converter writes
# ---------------------------------------------------------------------------


class TestCensusOnVersion3(unittest.TestCase):
    """The correspondence forests with the early-ending forest 1300 of the
    negative fixture (layout: :func:`correspondence_layout`): the census
    labels and reports it; the decided table refuses it
    (:class:`TestEarlyEndingRefused`).

    ``ForestIndex`` 0..3 are forests 1100, 1200, 1300 and 1400. Halos per
    snapshot: 0, 1 (1301), 1 (1300), 4 (1103, 1202, 1212, 1402), 7 (1101, 1102,
    1111, 1121, 1201, 1211, 1401) and 6 (the six roots at snapshot 5).
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_v3_"))
        forests = fixtures.early_ending_correspondence_forests()
        cls.halos = {h.halo_id: h for f in forests for t in f.trees for h in t.halos}
        cls.paths = convert_ascii(cls.tmp / "valid", correspondence_layout(), forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.index = SourceIndex.load(cls.paths["forests_list"], cls.paths["locations"])
        cls.agg = cls.tmp / "agg"
        cls.occupancy = run_quietly(occupancy.run_occupancy, cls.dataset, cls.agg)
        cls.trees = run_quietly(
            trees.run_trees,
            cls.dataset,
            cls.agg,
            cls.index,
            trees.load_parsed_counts(cls.paths["preparation_manifest"]),
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_index_forest_table_matches_the_adapters_sidecar(self):
        table = self.index.forest_table()
        with h5py.File(self.paths["dataset"] / "forests.h5", "r") as handle:
            sidecar = {name: handle[name][...] for name in handle}
        np.testing.assert_array_equal(table.forest_ids, sidecar["ForestID"])
        # file_ordinal is the FileID; it equals SourceFileOrdinal here only because
        # convert_ascii gives the converter its tree files in FileID order
        np.testing.assert_array_equal(table.file_id, sidecar["SourceFileOrdinal"])
        np.testing.assert_array_equal(table.unit_ordinal, sidecar["SourceUnitOrdinal"])
        # literal: 1100 unit 0 of file 0 (markers F, F, G, F), 1200 spans, 1300
        # unit 1 of file 0, 1400 unit 0 of file 1
        np.testing.assert_array_equal(table.file_id, [0, -1, 0, 1])
        np.testing.assert_array_equal(table.unit_ordinal, [0, -1, 1, 0])
        np.testing.assert_array_equal(table.n_trees, [3, 2, 1, 1])

    def test_per_slab_forest_pairs(self):
        expected = {
            0: ([], []),
            1: ([2], [1]),
            2: ([2], [1]),
            3: ([0, 1, 3], [1, 2, 1]),
            4: ([0, 1, 3], [4, 2, 1]),
            5: ([0, 1, 3], [3, 2, 1]),
        }
        for snap, (forests, counts) in expected.items():
            got_forests, got_counts = occupancy.load_slab_pairs(self.agg, snap)
            self.assertEqual(got_forests.dtype, np.int64)
            self.assertEqual(got_counts.dtype, np.int32)
            np.testing.assert_array_equal(got_forests, forests)
            np.testing.assert_array_equal(got_counts, counts)

    def test_per_forest_totals_and_peaks(self):
        base = occupancy.occupancy_dir(self.agg)
        np.testing.assert_array_equal(np.load(base / "forest_totals.npy"), [8, 6, 2, 3])
        np.testing.assert_array_equal(np.load(base / "forest_max_occupancy.npy"), [4, 2, 1, 1])
        # forest 1200 peaks at 2 in snapshots 3, 4 and 5: the lowest-numbered is kept
        np.testing.assert_array_equal(np.load(base / "forest_max_snapshot.npy"), [4, 3, 1, 3])

    def test_occupancy_summary_rows(self):
        summary = self.occupancy
        self.assertEqual(summary["total_halos"], 19)
        self.assertEqual(summary["widest_slab"]["snapshot"], 4)
        self.assertEqual(summary["widest_slab"]["halos"], 7)
        self.assertEqual(summary["widest_slab"]["forests_present"], 3)
        self.assertEqual(summary["widest_slab"]["top_shares"]["10"], {"halos": 7, "share": 1.0})
        self.assertEqual(summary["second_widest_slab"], {"snapshot": 5, "halos": 6})
        largest = summary["largest_forest"]
        self.assertEqual(
            (largest["forest_index"], largest["forest_id"], largest["total_halos"]), (0, 1100, 8)
        )
        self.assertEqual((largest["peak_occupancy"], largest["peak_snapshot"]), (4, 4))
        self.assertAlmostEqual(largest["share_of_all_halos"], 8 / 19)
        second = summary["second_largest_forest"]
        self.assertEqual((second["forest_id"], second["peak_occupancy"]), (1200, 2))
        self.assertEqual(summary["second_highest_peak_forest"]["forest_id"], 1200)
        self.assertEqual(summary["forests_exceeding"], {"100000": 0, "250000": 0, "1000000": 0})
        self.assertEqual([row["halos"] for row in summary["per_snapshot"]], [0, 1, 1, 4, 7, 6])
        # a tie for the widest slab goes to the lowest-numbered
        self.assertEqual(occupancy.widest_snapshot([3, 7, 2, 7]), 1)
        self.assertIsNone(occupancy.widest_snapshot([]))

    def test_top_shares_count_the_most_populous_forests(self):
        with mock.patch.object(occupancy, "TOP_SHARES", (1, 2, 10)):
            shares = occupancy.top_shares(np.array([5, 1, 3, 1]), 10)
        self.assertEqual(shares["1"], {"halos": 5, "share": 0.5})
        self.assertEqual(shares["2"], {"halos": 8, "share": 0.8})
        self.assertEqual(shares["10"], {"halos": 10, "share": 1.0})

    def test_every_halo_is_labelled_with_its_terminal_root(self):
        roots = trees.load_roots(self.agg)
        np.testing.assert_array_equal(roots, [1100, 1110, 1120, 1200, 1210, 1300, 1400])
        for snap in self.dataset.snapshots:
            labels = trees.load_labels(self.agg, snap)
            self.assertEqual(labels.dtype, np.int32)
            ids = read_slab_column(self.paths["dataset"], snap, "MostBoundID")
            expected = [terminal_root(self.halos, int(i)) for i in ids]
            self.assertEqual(roots[labels].tolist(), expected, snap)

    def test_per_tree_forest_totals_and_peaks(self):
        base = trees.trees_dir(self.agg)
        np.testing.assert_array_equal(np.load(base / "tree_forest.npy"), [0, 0, 0, 1, 1, 2, 3])
        np.testing.assert_array_equal(
            np.load(base / "tree_root_snapshot.npy"), [5, 5, 5, 5, 5, 2, 5]
        )
        np.testing.assert_array_equal(np.load(base / "tree_totals.npy"), [4, 2, 2, 3, 3, 2, 3])
        np.testing.assert_array_equal(
            np.load(base / "tree_max_occupancy.npy"), [2, 1, 1, 1, 1, 1, 1]
        )
        # ties keep the lowest-numbered slab although the pass runs backward
        np.testing.assert_array_equal(
            np.load(base / "tree_max_snapshot.npy"), [4, 4, 4, 3, 3, 1, 3]
        )
        expected = {
            5: ([0, 1, 2, 3, 4, 6], [1, 1, 1, 1, 1, 1]),
            4: ([0, 1, 2, 3, 4, 6], [2, 1, 1, 1, 1, 1]),
            3: ([0, 3, 4, 6], [1, 1, 1, 1]),
            2: ([5], [1]),
            1: ([5], [1]),
            0: ([], []),
        }
        for snap, (ordinals, counts) in expected.items():
            got_trees, got_counts = trees.load_slab_tree_pairs(self.agg, snap)
            np.testing.assert_array_equal(got_trees, ordinals)
            np.testing.assert_array_equal(got_counts, counts)

    def test_trees_summary(self):
        summary = self.trees
        self.assertEqual(summary["n_trees"], 7)
        self.assertEqual(summary["forest_mismatch_halos"], 0)
        largest = summary["largest_tree"]
        self.assertEqual(
            (largest["root_id"], largest["forest_id"], largest["total_halos"]), (1100, 1100, 4)
        )
        self.assertEqual((largest["peak_occupancy"], largest["peak_snapshot"]), (2, 4))
        self.assertEqual(
            summary["largest_forest"],
            {"forest_index": 0, "forest_id": 1100, "trees": 3, "total_halos": 8},
        )

    def test_root_correspondence_passes(self):
        record = self.trees["root_correspondence"]
        self.assertEqual(record["verdict"], "pass")
        self.assertEqual(record["terminal_roots"], 7)
        self.assertEqual(record["index_roots"], 7)
        self.assertEqual(record["roots_not_in_forests_list"]["count"], 0)
        self.assertEqual(record["forests_list_roots_without_terminal_halo"]["count"], 0)
        self.assertEqual(record["forest_mismatches"]["count"], 0)
        self.assertEqual(
            record["roots_not_in_last_snapshot"],
            {"count": 1, "examples": [1300], "in_forests_list": 1},
        )

    def test_conservation_against_the_parsed_counts(self):
        record = self.trees["conservation"]
        self.assertEqual(record["verdict"], "pass")
        self.assertEqual(
            [(f["name"], f["census_halos"], f["parsed_count"]) for f in record["files"]],
            [("tree_0.dat", 13, 13), ("tree_1.dat", 6, 6)],
        )
        self.assertEqual(record["unattributed_halos"], 0)

    def test_a_disagreeing_report_fails_the_conservation_check(self):
        report = self.tmp / "report.json"
        report.write_text(
            json.dumps(
                {
                    "source_files": {
                        "/a/tree_0.dat": {"parsed_count": 13},
                        "/a/tree_1.dat": {"parsed_count": 7},
                    }
                }
            )
        )
        roots = trees.load_roots(self.agg)
        totals = np.load(trees.trees_dir(self.agg) / "tree_totals.npy")
        record = trees.conservation(roots, totals, self.index, trees.load_parsed_counts(report))
        self.assertEqual(record["verdict"], "fail")
        self.assertEqual(record["files_disagreeing"], 1)

    def test_results_do_not_depend_on_the_block_size(self):
        other = self.tmp / "agg_block_1"
        run_quietly(occupancy.run_occupancy, self.dataset, other, block_rows=1)
        run_quietly(
            trees.run_trees,
            self.dataset,
            other,
            self.index,
            trees.load_parsed_counts(self.paths["preparation_manifest"]),
            block_rows=1,
        )
        for path in sorted(self.agg.rglob("*")):
            if path.is_file() and path.parent.name != "partition":
                twin = other / path.relative_to(self.agg)
                self.assertEqual(path.read_bytes(), twin.read_bytes(), path)

    def test_partition_of_the_fixture(self):
        other = self.tmp / "agg_partition"
        run_quietly(occupancy.run_occupancy, self.dataset, other)
        summary = run_quietly(partition.run_partition, other, [1, 2], [1, 2])
        # re-derived by hand for the peak weights: forest 2 holds one row at
        # snapshot 1 and none in the widest slab 4, so the peaks are
        # [4, 2, 1, 1] where slab 4 alone counts [4, 2, 0, 1]; two tasks still
        # pack at capacity 4 as [4] | [2, 1, 1], cuts [0, 1, 4]
        np.testing.assert_array_equal(partition.peak_weights(other, 4), [4, 2, 1, 1])
        self.assertEqual(summary["weights"]["weighting"], partition.WEIGHTING)
        self.assertEqual(summary["weights"]["heaviest_forest"], {"forest_index": 0, "rows": 4})
        self.assertEqual(summary["weights"]["widest_slab"], {"snapshot": 4, "halos": 7})
        self.assertEqual(summary["floor"]["per_snapshot"], [0, 1, 1, 2, 4, 3])
        point = next(p for p in summary["grid"] if (p["ntask"], p["nchunk"]) == (2, 1))
        self.assertEqual(point["forest_cuts"], [0, 1, 4])
        self.assertEqual(
            [row["range_rows"] for row in point["per_snapshot"]],
            [[0, 0], [0, 1], [0, 1], [1, 3], [4, 3], [3, 3]],
        )
        self.assertEqual(
            point["widest"], {"snapshot": 4, "range": 0, "task": 0, "chunk": 0, "rows": 4}
        )

    def test_partition_needs_a_completed_occupancy(self):
        other = self.tmp / "agg_no_occupancy"
        aggregate.bind(other, self.dataset.identity())
        with self.assertRaisesRegex(ConverterError, "run occupancy first"):
            partition.run_partition(other)

    def test_an_aggregate_of_another_dataset_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "produced from a different dataset"):
            trees.run_trees(HorizontalDataset(WORKED_ASCII), self.agg)
        with self.assertRaisesRegex(ConverterError, "produced from a different dataset"):
            occupancy.run_occupancy(HorizontalDataset(WORKED_ASCII), self.agg)

    def test_read_rows_reads_one_bounded_range(self):
        rows = self.dataset.read_rows(4, ("MostBoundID", "ForestIndex"), 2, 5)
        whole = read_slab_column(self.paths["dataset"], 4, "MostBoundID")
        np.testing.assert_array_equal(rows["MostBoundID"], whole[2:5])
        self.assertEqual(rows["ForestIndex"].size, 3)
        with self.assertRaisesRegex(ConverterError, r"rows \[5, 9\) are outside \[0, 7\)"):
            self.dataset.read_rows(4, ("MostBoundID",), 5, 9)

    def test_tree_counts_beyond_int32_are_refused_before_narrowing(self):
        counts = np.array([5, 1 << 31, 7], dtype=np.int64)
        ordinals = np.array([3, 9, 12], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, "snapshot 4: tree with root ordinal 9 holds"):
            trees.narrow_tree_counts(counts, ordinals, 4)
        narrowed = trees.narrow_tree_counts(counts[[0, 2]], ordinals[[0, 2]], 4)
        self.assertEqual(narrowed.dtype, np.int32)
        np.testing.assert_array_equal(narrowed, [5, 7])

    def test_a_failed_rerun_leaves_no_completion_marker(self):
        other = self.tmp / "agg_rerun"
        report = trees.load_parsed_counts(self.paths["preparation_manifest"])
        run_quietly(trees.run_trees, self.dataset, other, self.index, report)
        self.assertEqual(aggregate.require_summary(trees.trees_dir(other), "trees")["n_trees"], 7)
        # a failure inside pass 2, after the rerun has started rewriting arrays
        injected = ConverterError("injected pass 2 failure")
        with mock.patch.object(trees, "narrow_tree_counts", side_effect=injected):
            with self.assertRaisesRegex(ConverterError, "injected pass 2 failure"):
                run_quietly(trees.run_trees, self.dataset, other, self.index, report)
        with self.assertRaisesRegex(ConverterError, "run trees first"):
            aggregate.require_summary(trees.trees_dir(other), "trees")

    def test_a_bad_report_is_refused_before_anything_is_touched(self):
        malformed = self.tmp / "malformed_report.json"
        for document, message in (
            ({"totals": {}}, "no source_files"),
            ([1, 2], "no source_files"),
            ({"source_files": {"/a/t.dat": {"parsed_count": "13"}}}, "not a non-negative integer"),
            ({"source_files": {"/a/t.dat": {"parsed_count": -1}}}, "not a non-negative integer"),
            ({"source_files": {"/a/t.dat": {"parsed_count": True}}}, "not a non-negative integer"),
            ({"source_files": {"/a/t.dat": {}}}, "has no parsed_count"),
        ):
            malformed.write_text(json.dumps(document))
            with self.assertRaisesRegex(ConverterError, message):
                trees.load_parsed_counts(malformed)
        malformed.write_text("{not json")
        with self.assertRaisesRegex(ConverterError, "malformed_report.json: not readable as JSON"):
            trees.load_parsed_counts(malformed)

        other = self.tmp / "agg_bad_report"
        run_quietly(
            trees.run_trees,
            self.dataset,
            other,
            self.index,
            trees.load_parsed_counts(self.paths["preparation_manifest"]),
        )
        before = {
            path: (path.read_bytes(), path.stat().st_mtime_ns)
            for path in other.rglob("*")
            if path.is_file()
        }
        no_sources = self.tmp / "no_sources.json"
        no_sources.write_text(json.dumps({"totals": {}}))
        base = ["trees", "--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        base += ["--forests-list", str(self.paths["forests_list"])]
        base += ["--locations", str(self.paths["locations"])]
        for report, message in (
            (self.tmp / "missing.json", "No such file"),
            (malformed, "not readable as JSON"),
            (no_sources, "no source_files"),
        ):
            with capture_stderr() as captured:
                status = forest_census.main(base + ["--conversion-report", str(report)])
            self.assertEqual(status, 2, report)
            self.assertIn(message, captured.text)
        after = {
            path: (path.read_bytes(), path.stat().st_mtime_ns)
            for path in other.rglob("*")
            if path.is_file()
        }
        self.assertEqual(after, before)

    def test_the_trees_and_partition_command_lines(self):
        other = str(self.tmp / "agg_cli")
        dataset = ["--dataset", str(self.paths["dataset"]), "--aggregate", other]
        index = ["--forests-list", str(self.paths["forests_list"])]
        index += ["--locations", str(self.paths["locations"])]
        report = ["--conversion-report", str(self.paths["preparation_manifest"])]
        with contextlib.redirect_stdout(io.StringIO()), capture_stderr():
            status = forest_census.main(["trees"] + dataset + index + report)
        self.assertEqual(status, 0)
        written = aggregate.read_json(trees.trees_dir(Path(other)) / "summary.json")
        self.assertEqual(written["n_trees"], 7)
        self.assertEqual(written["root_correspondence"]["verdict"], "pass")
        self.assertEqual(written["conservation"]["verdict"], "pass")

        with capture_stderr() as captured:
            status = forest_census.main(["trees"] + dataset + index[:2])
        self.assertEqual(status, 2)
        self.assertIn("--forests-list and --locations are given together", captured.text)
        with capture_stderr() as captured:
            status = forest_census.main(["trees"] + dataset + report)
        self.assertEqual(status, 2)
        self.assertIn("--conversion-report needs --forests-list and --locations", captured.text)
        with self.assertRaisesRegex(ConverterError, "needs forests.list and locations.dat"):
            trees.run_trees(
                self.dataset,
                self.tmp / "agg_unused",
                None,
                trees.load_parsed_counts(self.paths["preparation_manifest"]),
            )

        with contextlib.redirect_stdout(io.StringIO()), capture_stderr():
            self.assertEqual(forest_census.main(["occupancy"] + dataset), 0)
            status = forest_census.main(
                ["partition", "--aggregate", other, "--ntask", "1,2", "--nchunk", "1,2"]
            )
        self.assertEqual(status, 0)
        recorded = aggregate.read_json(Path(other) / "partition" / "summary.json")
        point = next(p for p in recorded["grid"] if (p["ntask"], p["nchunk"]) == (2, 1))
        self.assertEqual(point["widest"]["rows"], 4)

        (Path(other) / "identity.json").write_text("{malformed")
        with capture_stderr() as captured:
            status = forest_census.main(["partition", "--aggregate", other])
        self.assertEqual(status, 2)
        self.assertIn("identity.json: not readable as JSON", captured.text)


class TestCorrespondenceMismatch(unittest.TestCase):
    """The adapter's standard forests, whose ``#tree`` root ids (101, 102, ...)
    are not their terminal halos' ids (1010, 1020, ...): the census reports the
    mismatch and still labels every halo; it repairs nothing."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_mismatch_"))
        forests = fixtures.standard_forests()
        cls.paths = convert_ascii(cls.tmp / "standard", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        index = SourceIndex.load(cls.paths["forests_list"], cls.paths["locations"])
        cls.summary = run_quietly(trees.run_trees, cls.dataset, cls.tmp / "agg", index)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_the_mismatch_is_reported(self):
        record = self.summary["root_correspondence"]
        self.assertEqual(record["verdict"], "fail")
        self.assertEqual(record["terminal_roots"], 10)
        self.assertEqual(record["roots_not_in_forests_list"]["count"], 10)
        self.assertEqual(
            record["roots_not_in_forests_list"]["examples"], [1010, 1020, 2010, 2011, 4010]
        )
        self.assertEqual(record["forests_list_roots_without_terminal_halo"]["count"], 6)
        self.assertEqual(record["roots_not_in_last_snapshot"]["count"], 1)
        self.assertEqual(record["roots_not_in_last_snapshot"]["in_forests_list"], 0)
        self.assertEqual(self.summary["conservation"]["unattributed_halos"], 17)

    def test_the_table_and_the_cut_refuse_a_failed_correspondence(self):
        index = SourceIndex.load(self.paths["forests_list"], self.paths["locations"])
        for function, arguments in (
            (table.run_table, (index.tree_roots, index.forest_ids, {})),
            (cut.run_cut, (1.0,)),
        ):
            with self.assertRaisesRegex(ConverterError, "verdict is 'fail'; .*needs it to pass"):
                function(self.dataset, self.tmp / "agg", *arguments)
        for name in ("table", "cut"):
            self.assertFalse((self.tmp / "agg" / name).exists(), name)

    def test_every_halo_is_still_labelled(self):
        roots = trees.load_roots(self.tmp / "agg")
        self.assertEqual(roots.size, 10)
        labelled = sum(trees.load_labels(self.tmp / "agg", s).size for s in self.dataset.snapshots)
        self.assertEqual(labelled, self.dataset.total_halos)
        self.assertEqual(self.summary["forest_mismatch_halos"], 0)


# ---------------------------------------------------------------------------
# Progenitor chains, the installed partition and the cut table's invariants
# ---------------------------------------------------------------------------


def chain_order(encounter, mass):
    """The reference incremental-insertion chain over progenitors visited in
    ``encounter`` order: the first starts the chain, each later one becomes the
    head if strictly more massive than the current head, else the tail
    (``links.build_progenitor_links``)."""
    encounter = [int(i) for i in encounter]
    chain = [encounter[0]]
    head = encounter[0]
    for item in encounter[1:]:
        if mass[head] < mass[item]:
            chain.insert(0, item)
            head = item
        else:
            chain.append(item)
    return chain


class TestInSorted(unittest.TestCase):
    def test_membership_in_an_ascending_array(self):
        needles = np.array([[1, 2, 3], [4, 6, 7]])
        got = in_sorted(np.array([2, 4, 6]), needles)
        np.testing.assert_array_equal(got, [[False, True, False], [True, True, False]])
        np.testing.assert_array_equal(in_sorted(np.zeros(0, dtype=np.int64), needles), got & False)


class TestChains(unittest.TestCase):
    """The vectorised chain positions against the reference insertion loop."""

    def random_groups(self, generator, n_groups):
        sizes = generator.integers(1, 7, n_groups)
        groups = np.repeat(np.arange(n_groups) * 3 + 11, sizes)
        n = groups.size
        # few distinct masses, so equal masses and ties at the head are common
        mass = generator.integers(0, 4, n).astype(np.float32) * np.float32(1.5e11)
        upid = generator.integers(0, 50, n)
        pid = np.where(generator.random(n) < 0.5, -1, upid)
        ids = generator.permutation(n) + 1000
        shuffle = generator.permutation(n)
        return groups[shuffle], upid[shuffle], pid[shuffle], ids[shuffle], mass[shuffle]

    def test_positions_equal_the_insertion_loop(self):
        generator = np.random.default_rng(2026)
        for n_groups in (1, 5, 400):
            groups, upid, pid, ids, mass = self.random_groups(generator, n_groups)
            positions = cut.chain_positions(groups, upid, pid, ids, mass)
            for group in np.unique(groups):
                members = np.flatnonzero(groups == group)
                encounter = members[np.lexsort((ids[members], pid[members], upid[members]))]
                expected = chain_order(encounter, mass)
                got = members[np.argsort(positions[members])].tolist()
                self.assertEqual(got, expected, group)
                self.assertEqual(sorted(positions[members].tolist()), list(range(members.size)))
        self.assertEqual(cut.chain_positions([], [], [], [], np.zeros(0)).size, 0)

    def test_a_key_that_would_overflow_int64_is_refused(self):
        groups, upid, pid, ids, mass = self.random_groups(np.random.default_rng(3), 3)
        with mock.patch.object(cut, "INT64_MAX", 5):
            with self.assertRaisesRegex(ConverterError, "overflow the int64 chain key"):
                cut.chain_positions(groups, upid, pid, ids, mass)

    def test_the_stored_chain_check(self):
        generator = np.random.default_rng(7)
        groups, upid, pid, ids, mass = self.random_groups(generator, 300)
        rows = generator.permutation(groups.size) * 2 + 5
        positions = cut.chain_positions(groups, upid, pid, ids, mass)
        next_rows = np.full(groups.size, -1, dtype=np.int64)
        for group in np.unique(groups):
            members = np.flatnonzero(groups == group)
            chain = members[np.argsort(positions[members])]
            next_rows[chain[:-1]] = rows[chain[1:]]
        self.assertEqual(cut.stored_chain_mismatches(groups, rows, next_rows, positions).size, 0)
        # break one link of a group with two or more progenitors
        broken = next_rows.copy()
        linked = np.flatnonzero(broken != -1)[0]
        broken[linked] = -1
        np.testing.assert_array_equal(
            cut.stored_chain_mismatches(groups, rows, broken, positions), [groups[linked]]
        )


class TestInstalledPieces(unittest.TestCase):
    """The fresh pieces' order, sorted once when the pieces are built
    (:class:`cut.CutState`), installs each slab as a per-slab sort of the fresh
    pieces would."""

    @staticmethod
    def reference(forests, counts, pieces, per_piece, n_forests, catalogue_max):
        counts = np.asarray(counts, dtype=np.int64).copy()
        keeping = np.flatnonzero(pieces["keeps_id"])
        at = np.searchsorted(forests, pieces["forest_index"][keeping])
        present = at < forests.size
        present[present] = forests[at[present]] == pieces["forest_index"][keeping][present]
        counts[at[present]] = per_piece[keeping[present]]
        fresh = np.flatnonzero(~pieces["keeps_id"] & (per_piece > 0))
        fresh = fresh[np.argsort(pieces["id"][fresh], kind="stable")]
        nonzero = counts > 0
        return (
            np.concatenate(
                [forests[nonzero], n_forests + (pieces["id"][fresh] - catalogue_max - 1)]
            ),
            np.concatenate([counts[nonzero], per_piece[fresh]]),
        )

    def test_matches_a_per_slab_sort(self):
        generator = np.random.default_rng(99)
        n_forests, catalogue_max = 40, 1000
        for _trial in range(50):
            named = np.sort(generator.choice(n_forests, 3, replace=False))
            n_fresh = int(generator.integers(0, 30))
            pieces = {
                # fresh ids in a scrambled piece order, as the pieces' keys leave them
                "id": np.r_[named + 500, catalogue_max + 1 + generator.permutation(n_fresh)],
                "keeps_id": np.r_[np.ones(3, dtype=bool), np.zeros(n_fresh, dtype=bool)],
                "forest_index": np.r_[named, generator.choice(named, n_fresh)],
            }
            state = cut.CutState(table=SimpleNamespace(pieces=pieces))
            forests = np.sort(generator.choice(n_forests, 25, replace=False)).astype(np.int64)
            counts = generator.integers(1, 9, forests.size)
            per_piece = generator.integers(0, 4, pieces["id"].size)
            got = cut.install_pieces(forests, counts, state, per_piece, n_forests, catalogue_max)
            want = self.reference(forests, counts, pieces, per_piece, n_forests, catalogue_max)
            np.testing.assert_array_equal(got[0], want[0])
            np.testing.assert_array_equal(got[1], want[1])

    def test_installed_weights_are_the_peaks(self):
        # forests 0..3 peak at [5, 4, 3, 2] (occupancy); forest 1 is split into
        # its kept piece (peak 1) and fresh 1001 (peak 4, in a slab where the
        # kept piece holds 1), forest 3 into its kept piece (peak 2) and fresh
        # 1002 (peak 1): by hand [5, 1, 3, 2] then [4, 1] at ForestIndex 4, 5
        agg = Path(tempfile.mkdtemp(prefix="census_installed_"))
        self.addCleanup(shutil.rmtree, agg, ignore_errors=True)
        (agg / "occupancy").mkdir()
        np.save(agg / "occupancy" / "forest_max_occupancy.npy", np.array([5, 4, 3, 2], np.int32))
        pieces = {
            "id": np.array([1002, 11, 13, 1001]),
            "keeps_id": np.array([False, True, True, False]),
            "forest_index": np.array([3, 1, 3, 1]),
        }
        table = SimpleNamespace(pieces=pieces, n_fresh=2)
        state = cut.CutState(table=table)
        state.peak = np.array([1, 1, 2, 4], dtype=np.int64)
        np.testing.assert_array_equal(
            cut.installed_weights(agg, state, 4, 1000), [5, 1, 3, 2, 4, 1]
        )


class TestCutTableInvariants(unittest.TestCase):
    """Index: roots 10, 11, 12 in forest 1 and 20, 21 in forest 2 (maximum 2);
    totals 5, 1, 2, 3, 1. The valid table cuts 12 (2 halos) and 21 (1 halo)
    off as fresh 3 and 4."""

    ROOTS = np.array([10, 11, 12, 20, 21], dtype=np.int64)
    FORESTS = np.array([1, 1, 1, 2, 2], dtype=np.int64)
    TOTALS = np.array([5, 1, 2, 3, 1], dtype=np.int64)
    VALID = [1, 1, 3, 2, 4]

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cut_table_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def check(self, ids, roots=None, totals=TOTALS):
        roots = self.ROOTS if roots is None else np.asarray(roots, dtype=np.int64)
        totals = self.TOTALS if totals is None else totals
        return cut_table.check_table(
            roots, np.asarray(ids, dtype=np.int64), self.ROOTS, self.FORESTS, totals
        )

    def test_the_valid_table_passes(self):
        self.assertEqual(
            self.check(self.VALID),
            {"trees": 5, "pieces": 4, "cut_forests": 2, "fresh_pieces": 2},
        )
        # rows in any order
        order = [4, 2, 0, 3, 1]
        self.check(np.asarray(self.VALID)[order], self.ROOTS[order])
        # the identity table: nothing cut
        self.assertEqual(self.check(self.FORESTS)["cut_forests"], 0)

    def test_every_invariant_is_refused(self):
        cases = (
            ([1, 1, 3, 2], self.ROOTS[:4], "1 tree root\\(s\\) of the index files missing"),
            ([1, 1, 3, 2, 4, 4], [10, 11, 12, 20, 21, 21], "listed more than once.*\\[21\\]"),
            ([1, 1, 3, 2, 4, 5], [10, 11, 12, 20, 21, 99], "1 not in the index files"),
            ([1, 1, 3, 2, 3], None, "1 piece\\(s\\) mix trees of several forests"),
            ([1, 1, 2, 2, 4], None, "at or below the catalogue's maximum 2"),
            ([1, 1, 0, 2, 4], None, "at or below the catalogue's maximum 2"),
            ([1, 1, 3, 4, 5], None, "keep no piece with their own id \\(e.g. \\[2\\]"),
            ([3, 3, 1, 2, 4], None, "not kept by their largest piece \\(e.g. forest 1\\)"),
            ([1, 1, 4, 2, 3], None, "fresh ids do not follow descending piece size"),
            # fresh ids 3 and 5: a gap at 4
            (
                [1, 1, 3, 2, 5],
                None,
                "not contiguous from the catalogue's maximum 2 plus one \\(id 5 "
                "where 4 was expected, 2 fresh",
            ),
            # fresh ids 4 and 5: the first is not the maximum plus one
            ([1, 1, 4, 2, 5], None, "not contiguous .*\\(id 4 where 3 was expected"),
        )
        for ids, roots, message in cases:
            with self.assertRaisesRegex(ConverterError, message):
                self.check(ids, roots)

    def test_totals_not_aligned_with_the_index_are_refused(self):
        with self.assertRaisesRegex(ConverterError, "cut table: 4 halo totals given for 5"):
            self.check(self.VALID, totals=self.TOTALS[:4])

    def test_written_tables_are_checked_on_read(self):
        path = self.tmp / "forests.list"
        md5, size = cut_table.write_table(path, self.ROOTS, np.asarray(self.VALID))
        self.assertEqual(md5, cut_table.md5_file(path))
        self.assertEqual(size, path.stat().st_size)
        self.assertEqual(path.read_text().splitlines()[:2], ["#TreeRootID ForestID", "10 1"])
        roots, ids = cut_table.read_cut_table(path, self.ROOTS, self.FORESTS, self.TOTALS)
        np.testing.assert_array_equal(roots, self.ROOTS)
        np.testing.assert_array_equal(ids, self.VALID)
        malformed = self.tmp / "malformed.list"
        malformed.write_text("#TreeRootID ForestID\n10 1\n11 1 7\n")
        with self.assertRaisesRegex(ConverterError, "malformed cut table"):
            cut_table.read_cut_table(malformed, self.ROOTS, self.FORESTS, self.TOTALS)
        for name, ids, message in (
            ("mixing", [1, 1, 3, 2, 3], "1 piece\\(s\\) mix"),
            # forest 1's id given to its smaller piece {12} (2 halos), not {10, 11} (6)
            ("smaller", [3, 3, 1, 2, 4], "not kept by their largest piece \\(e.g. forest 1\\)"),
            # fresh ids out of size order: {21} (1 halo) before {12} (2 halos)
            ("order", [1, 1, 4, 2, 3], "fresh ids do not follow descending piece size"),
        ):
            path = self.tmp / (name + ".list")
            cut_table.write_table(path, self.ROOTS, np.array(ids))
            with self.assertRaisesRegex(ConverterError, name + ".list: .*" + message):
                cut_table.read_cut_table(path, self.ROOTS, self.FORESTS, self.TOTALS)
        # the halo totals are a required argument of the reader
        with self.assertRaises(TypeError):
            cut_table.read_cut_table(path, self.ROOTS, self.FORESTS)

    def test_the_record_streams_its_piece_columns(self):
        pieces = {
            name: np.arange(5, dtype=np.int64)[::-1] + offset
            for offset, name in enumerate(cut_table.RECORD_COLUMNS)
        }
        path = self.tmp / "record.json"
        header = {"rule": {"text": table.RULE_TEXT}, "table": {"md5": "x"}}
        with mock.patch.object(cut_table, "RECORD_CHUNK", 2):
            size = cut_table.write_table_record(path, header, pieces)
        self.assertEqual(size, path.stat().st_size)
        record = aggregate.read_json(path)
        self.assertEqual(record["rule"], header["rule"])
        self.assertEqual(record["pieces"]["id"], [0, 1, 2, 3, 4])
        self.assertEqual(record["pieces"]["peak_snapshot"], [5, 6, 7, 8, 9])
        with self.assertRaisesRegex(ConverterError, "may not carry its own pieces"):
            cut_table.write_table_record(path, {"pieces": []}, pieces)

    def test_naming_and_the_tie_rule(self):
        # forest 0 (id 7): trees 0..2 as components {0, 1} (4 halos) and {2} (4
        # halos); forest 1 (id 9): {3} (2), {4} (6), {5} (2). Catalogue maximum 9.
        named = cut_table.name_pieces(
            components=np.array([0, 0, 2, 3, 4, 5]),
            local_forest=np.array([0, 0, 0, 1, 1, 1]),
            local_totals=np.array([1, 3, 4, 2, 6, 2]),
            local_roots=np.array([100, 101, 102, 200, 201, 202]),
            forest_ids=np.array([7, 9]),
            catalogue_max=9,
        )
        np.testing.assert_array_equal(named["piece"], [0, 0, 1, 2, 3, 4])
        # forest 0's two pieces tie at 4 halos: the smallest root (100) keeps 7;
        # forest 1's largest (201) keeps 9; fresh ids by size, ties by root:
        # 102 (4) -> 10, 200 (2) -> 11, 202 (2) -> 12
        np.testing.assert_array_equal(named["id"], [7, 10, 11, 9, 12])
        np.testing.assert_array_equal(named["keeps_id"], [True, False, False, True, False])
        np.testing.assert_array_equal(named["halos"], [4, 4, 2, 6, 2])
        np.testing.assert_array_equal(named["smallest_root_id"], [100, 102, 200, 201, 202])
        # a piece keyed by a tree that is not its smallest (the decided table keys
        # a piece by its z = 0 central's tree): the smallest root is still found
        keyed = cut_table.name_pieces(
            components=np.array([1, 1, 2, 3, 4, 5]),
            local_forest=np.array([0, 0, 0, 1, 1, 1]),
            local_totals=np.array([1, 3, 4, 2, 6, 2]),
            local_roots=np.array([100, 101, 102, 200, 201, 202]),
            forest_ids=np.array([7, 9]),
            catalogue_max=9,
        )
        for name in ("id", "keeps_id", "halos", "smallest_root_id"):
            np.testing.assert_array_equal(keyed[name], named[name], name)
        np.testing.assert_array_equal(keyed["root"], [0, 2, 3, 4, 5])
        with self.assertRaisesRegex(ConverterError, "joins trees of two forests"):
            cut_table.name_pieces(
                np.array([0, 0]),
                np.array([0, 1]),
                np.array([1, 1]),
                np.array([1, 2]),
                np.array([7, 9]),
                9,
            )


# ---------------------------------------------------------------------------
# The decided table and its cost
# ---------------------------------------------------------------------------


def five_halo_forest():
    """Forest 1500, five halos in two trees, each its own z = 0 group: 1530
    (tree 1500) is a satellite of 1510 (tree 1600) at snapshot 4 and shares
    descendant 1500 with 1520, of equal mass. Before the cut the encounter keys
    are (1510, 1510, 1530) and (1520, -1, 1520), so the chain is [1530, 1520];
    promoted, 1530 becomes (1530, -1, 1530) and the chain [1520, 1530]."""
    tree_a = fixtures.TreeSpec(
        root_id=1500,
        halos=[
            fixtures.HaloSpec(halo_id=1500, snap=5, mvir=1.0e12, num_prog=2),
            fixtures.HaloSpec(halo_id=1520, snap=4, mvir=5.0e11, desc_id=1500),
            fixtures.HaloSpec(halo_id=1530, snap=4, mvir=5.0e11, desc_id=1500, pid=1510, upid=1510),
        ],
    )
    tree_b = fixtures.TreeSpec(
        root_id=1600,
        halos=[
            fixtures.HaloSpec(halo_id=1600, snap=5, mvir=9.0e11, num_prog=1),
            fixtures.HaloSpec(halo_id=1510, snap=4, mvir=8.0e11, desc_id=1600),
        ],
    )
    return fixtures.ForestSpec(forest_id=1500, trees=[tree_a, tree_b])


def switching_forest():
    """Forest 2000, four trees, each its own z = 0 group. Trees 2100 and 2200
    share FoF groups in both directions: 2201 is 2101's satellite at snapshot
    3, and 2102 and 2103 are 2202's at snapshot 4 (two members, one snapshot).
    2401 (tree 2400) is 2302's (tree 2300) satellite at snapshot 4, with the
    larger mass."""
    t1 = fixtures.TreeSpec(
        root_id=2100,
        halos=[
            fixtures.HaloSpec(halo_id=2100, snap=5, mvir=1.0e12, num_prog=2),
            fixtures.HaloSpec(halo_id=2102, snap=4, mvir=6.0e11, desc_id=2100, pid=2202, upid=2202),
            fixtures.HaloSpec(halo_id=2103, snap=4, mvir=2.0e11, desc_id=2100, pid=2202, upid=2202),
            fixtures.HaloSpec(halo_id=2101, snap=3, mvir=5.0e11, desc_id=2102),
        ],
    )
    t2 = fixtures.TreeSpec(
        root_id=2200,
        halos=[
            fixtures.HaloSpec(halo_id=2200, snap=5, mvir=8.0e11, num_prog=1),
            fixtures.HaloSpec(halo_id=2202, snap=4, mvir=7.0e11, desc_id=2200, num_prog=1),
            fixtures.HaloSpec(halo_id=2201, snap=3, mvir=1.0e11, desc_id=2202, pid=2101, upid=2101),
        ],
    )
    t3 = fixtures.TreeSpec(
        root_id=2300,
        halos=[
            fixtures.HaloSpec(halo_id=2300, snap=5, mvir=3.0e12, num_prog=1),
            fixtures.HaloSpec(halo_id=2302, snap=4, mvir=2.0e12, desc_id=2300, num_prog=1),
            fixtures.HaloSpec(halo_id=2301, snap=3, mvir=1.0e12, desc_id=2302),
        ],
    )
    t4 = fixtures.TreeSpec(
        root_id=2400,
        halos=[
            fixtures.HaloSpec(halo_id=2400, snap=5, mvir=5.0e11, num_prog=1),
            fixtures.HaloSpec(halo_id=2401, snap=4, mvir=1.5e12, desc_id=2400, pid=2302, upid=2302),
        ],
    )
    return fixtures.ForestSpec(forest_id=2000, trees=[t1, t2, t3, t4])


def sequential_components(n_nodes, edges):
    """Connected components of ``edges`` by plain sequential disjoint-set merging, the
    test's own oracle, each component named by its smallest node."""
    parent = list(range(n_nodes))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for u, v in edges:
        ru, rv = find(u), find(v)
        if ru != rv:
            parent[max(ru, rv)] = min(ru, rv)
    return [find(x) for x in range(n_nodes)]


def splitting_forest():
    """Forest 3000: at snapshot 4, central 3101 (tree 3100) hosts 3201 and 3202
    (tree 3200) and 3301 (tree 3300); each tree's root is a central at 5, so
    the decided table cuts the forest into its three trees."""
    r = fixtures.TreeSpec(
        root_id=3100,
        halos=[
            fixtures.HaloSpec(halo_id=3100, snap=5, mvir=4.0e12, num_prog=1),
            fixtures.HaloSpec(halo_id=3101, snap=4, mvir=3.0e12, desc_id=3100),
        ],
    )
    s = fixtures.TreeSpec(
        root_id=3200,
        halos=[
            fixtures.HaloSpec(halo_id=3200, snap=5, mvir=9.0e11, num_prog=2),
            fixtures.HaloSpec(halo_id=3201, snap=4, mvir=2.0e11, desc_id=3200, pid=3101, upid=3101),
            fixtures.HaloSpec(halo_id=3202, snap=4, mvir=4.0e11, desc_id=3200, pid=3101, upid=3101),
        ],
    )
    u = fixtures.TreeSpec(
        root_id=3300,
        halos=[
            fixtures.HaloSpec(halo_id=3300, snap=5, mvir=5.0e11, num_prog=1),
            fixtures.HaloSpec(halo_id=3301, snap=4, mvir=3.0e11, desc_id=3300, pid=3101, upid=3101),
        ],
    )
    return fixtures.ForestSpec(forest_id=3000, trees=[r, s, u])


def propagating_forest():
    """Forest 4000, three trees, each its own z = 0 group: 4102 (tree 4100) is
    4202's satellite (tree 4200) at snapshot 2, and 4302 and 4303 (tree 4300)
    are satellites of tree 4200's centrals at snapshots 2 and 3. Every one is
    promoted, and the promoted halos' descendants run on to snapshot 5."""
    p = fixtures.TreeSpec(
        root_id=4100,
        halos=[
            fixtures.HaloSpec(halo_id=4100, snap=5, mvir=5.0e11, num_prog=1),
            fixtures.HaloSpec(halo_id=4104, snap=4, mvir=4.0e11, desc_id=4100, num_prog=1),
            fixtures.HaloSpec(halo_id=4103, snap=3, mvir=3.0e11, desc_id=4104, num_prog=1),
            fixtures.HaloSpec(halo_id=4102, snap=2, mvir=2.0e11, desc_id=4103, pid=4202, upid=4202),
        ],
    )
    q = fixtures.TreeSpec(
        root_id=4200,
        halos=[
            fixtures.HaloSpec(halo_id=4200, snap=5, mvir=4.0e12, num_prog=1),
            fixtures.HaloSpec(halo_id=4204, snap=4, mvir=3.0e12, desc_id=4200, num_prog=1),
            fixtures.HaloSpec(halo_id=4203, snap=3, mvir=2.0e12, desc_id=4204, num_prog=1),
            fixtures.HaloSpec(halo_id=4202, snap=2, mvir=1.0e12, desc_id=4203),
        ],
    )
    r = fixtures.TreeSpec(
        root_id=4300,
        halos=[
            fixtures.HaloSpec(halo_id=4300, snap=5, mvir=6.0e11, num_prog=1),
            fixtures.HaloSpec(halo_id=4304, snap=4, mvir=5.0e11, desc_id=4300, num_prog=1),
            fixtures.HaloSpec(
                halo_id=4303, snap=3, mvir=4.0e11, desc_id=4304, num_prog=1, pid=4203, upid=4203
            ),
            fixtures.HaloSpec(halo_id=4302, snap=2, mvir=3.0e11, desc_id=4303, pid=4202, upid=4202),
        ],
    )
    return fixtures.ForestSpec(forest_id=4000, trees=[p, q, r])


def naming_forest():
    """Forest 5000: three z = 0 groups of 4 halos each, a three-way tie. Group
    A's central is 5100 and its member tree 5050 has the smaller root; group C's
    central is 5300 and its member tree 5055 has the smaller root; group B is
    tree 5060 alone. By the smallest root of each piece (5050, 5055, 5060) A
    keeps 5000, then C takes 5001 and B 5002; naming by the central's tree
    (5100, 5300, 5060) would give B the forest's id."""
    spec = fixtures.HaloSpec
    return fixtures.ForestSpec(
        forest_id=5000,
        trees=[
            fixtures.TreeSpec(
                root_id=5050,
                halos=[
                    spec(halo_id=5050, snap=5, mvir=3.0e11, pid=5100, upid=5100, num_prog=1),
                    spec(halo_id=5051, snap=4, mvir=2.0e11, desc_id=5050),
                ],
            ),
            fixtures.TreeSpec(
                root_id=5055,
                halos=[
                    spec(halo_id=5055, snap=5, mvir=3.0e11, pid=5300, upid=5300, num_prog=1),
                    spec(halo_id=5056, snap=4, mvir=2.0e11, desc_id=5055),
                ],
            ),
            fixtures.TreeSpec(
                root_id=5060,
                halos=[
                    spec(halo_id=5060, snap=5, mvir=1.0e12, num_prog=2),
                    spec(halo_id=5061, snap=4, mvir=6.0e11, desc_id=5060, num_prog=1),
                    spec(halo_id=5064, snap=4, mvir=2.0e11, desc_id=5060),
                    spec(halo_id=5062, snap=3, mvir=4.0e11, desc_id=5061),
                ],
            ),
            fixtures.TreeSpec(
                root_id=5100,
                halos=[
                    spec(halo_id=5100, snap=5, mvir=2.0e12, num_prog=1),
                    spec(halo_id=5101, snap=4, mvir=1.0e12, desc_id=5100),
                ],
            ),
            fixtures.TreeSpec(
                root_id=5300,
                halos=[
                    spec(halo_id=5300, snap=5, mvir=2.0e12, num_prog=1),
                    spec(halo_id=5301, snap=4, mvir=1.0e12, desc_id=5300),
                ],
            ),
        ],
    )


def decided_forests():
    """Forests 1100, 1200 and 1400 (the correspondence forests), 1500 and 2000."""
    return fixtures.correspondence_forests() + [five_halo_forest(), switching_forest()]


def decided_layout():
    """File 0 holds forest 1100, tree 1200 of the spanning forest 1200, and the
    forests 1500 and 2000; file 1 holds forest 1400 and tree 1210, so forest
    1200 spans both files."""
    multi, spanning, single = fixtures.correspondence_forests()
    return [
        multi.trees + [spanning.trees[0]] + five_halo_forest().trees + switching_forest().trees,
        [single.trees[0], spanning.trees[1]],
    ]


def all_ended_pieces(dataset_dir):
    """Independently of the census: each tree root id's piece under the
    partition that cuts every co-membership ended before the final snapshot
    and keeps every one present at it, named by the piece's smallest root id,
    and the pairs of trees that co-membered only before the final snapshot.

    Every slab's raw columns are read whole; each halo's tree is found by
    walking ``Descendant`` from the last slab backward; a co-membership is a
    FoF member whose tree differs from its central's; the pieces are the
    components of the pairs present at the final snapshot."""
    slabs = []
    for path in sorted(Path(dataset_dir).glob("snapshot_*.h5")):
        with h5py.File(path, "r") as handle:
            halos = handle["halos"]
            slabs.append(
                {
                    name: halos[name][...]
                    for name in ("MostBoundID", "Descendant", "FirstHaloInFOFgroup")
                }
            )
    labels = [None] * len(slabs)
    for snap in reversed(range(len(slabs))):
        ids, desc = slabs[snap]["MostBoundID"], slabs[snap]["Descendant"]
        labels[snap] = [
            int(ids[row]) if desc[row] == -1 else labels[snap + 1][int(desc[row])]
            for row in range(ids.size)
        ]
    final = len(slabs) - 1
    kept, ended = set(), set()
    for snap, slab in enumerate(slabs):
        for row, central in enumerate(slab["FirstHaloInFOFgroup"]):
            own, host = labels[snap][row], labels[snap][int(central)]
            if own != host:
                (kept if snap == final else ended).add((min(own, host), max(own, host)))
    roots = sorted(set(labels[final]))
    position = {root: at for at, root in enumerate(roots)}
    components = sequential_components(len(roots), [(position[a], position[b]) for a, b in kept])
    return {root: roots[components[position[root]]] for root in roots}, ended - kept


def same_partition(assignment_a, assignment_b):
    """Whether two root -> label maps group the roots identically."""

    def groups(assignment):
        members = {}
        for root, label in assignment.items():
            members.setdefault(label, set()).add(root)
        return {frozenset(group) for group in members.values()}

    return groups(assignment_a) == groups(assignment_b)


def read_table(path):
    """A forests.list-shaped table as {root id: forest id}."""
    rows = Path(path).read_text().splitlines()[1:]
    return {int(root): int(forest) for root, forest in (row.split() for row in rows)}


def run_census(dataset, agg, paths, block_rows=1 << 22):
    """occupancy, trees (with the index files) and partition into ``agg``."""
    index = SourceIndex.load(paths["forests_list"], paths["locations"])
    run_quietly(occupancy.run_occupancy, dataset, agg, block_rows)
    run_quietly(trees.run_trees, dataset, agg, index, None, block_rows)
    run_quietly(partition.run_partition, agg, [1, 2], [1, 2])
    return index


def index_files(paths):
    return {
        name: {"path": str(paths[name]), "md5": cut_table.md5_file(paths[name])}
        for name in ("forests_list", "locations")
    }


def run_decided(dataset, agg, paths, index, selection=None, block_rows=1 << 22):
    """The table and the cut of the decided table, with the test grid: tasks and
    chunks {1, 2, 4}, 1 GiB per resident halo, classes 6, 9 and 16 GiB less a
    1 GiB reserve."""
    summary_table = run_quietly(
        table.run_table,
        dataset,
        agg,
        index.tree_roots,
        index.forest_ids,
        index_files(paths),
        selection,
        block_rows,
    )
    summary_cut = run_quietly(
        cut.run_cut,
        dataset,
        agg,
        1.0,
        selection,
        [1, 2, 4],
        [1, 2, 4],
        1 << 30,
        [6, 9, 16],
        block_rows,
    )
    return summary_table, summary_cut


def check_identity_table(case, path, index):
    """``path`` is the identity table of ``index``: every root keeps its forest id."""
    case.assertEqual(
        read_table(path),
        {int(root): int(forest) for root, forest in zip(index.tree_roots, index.forest_ids)},
    )


def check_zero_cost(case, summary_table, summary_cut, n_forests):
    """A table with no split forest and its zero-cost accounting."""
    case.assertEqual((summary_table["pieces"]["count"], summary_table["pieces"]["fresh"]), (0, 0))
    case.assertEqual(summary_table["groups"]["split_forests"]["count"], 0)
    case.assertEqual(summary_table["groups"]["forests_after"], n_forests)
    pieces = summary_cut["pieces"]
    case.assertEqual((pieces["split_forests"], pieces["count"], pieces["fresh"]), (0, 0, 0))
    case.assertEqual(pieces["forests_after"], n_forests)
    case.assertEqual(summary_cut["severance"]["promoted_halos"], 0)
    case.assertEqual(summary_cut["progenitor_order"]["descendants_changed"], 0)
    case.assertEqual(set(summary_cut["relabelled"].values()), {0})
    sage = summary_cut["predicted_effects"]["sage16_halos_only"]
    case.assertEqual((sage["dependent_halos"], sage["upper_bound_halos"]), (0, 0))


class TestDecidedTable(unittest.TestCase):
    """The decided table and its cost on :func:`decided_forests`, converted to
    version 3.

    ``ForestIndex`` 0..4 are forests 1100, 1200, 1400, 1500 and 2000. Trees by
    root: 1100 (4 halos), 1110 (2), 1120 (2), 1200 (3), 1210 (3), 1400 (3),
    1500 (3), 1600 (2), 2100 (4), 2200 (3), 2300 (3), 2400 (2); halos per
    snapshot 0, 0, 0, 7, 15, 12.

    The z = 0 groups: forest 1100 ends in {1100, 1110} (6 halos) and {1120}
    (2); 1200 in one group, {1200, 1210}, and 1400 in one, so both are
    unchanged; 1500 in {1500} (3) and {1600} (2); 2000 in its four trees. Ten
    groups, three split forests, eight pieces. The catalogue maximum is 2000;
    fresh ids by size, ties by the smallest root: 2200 -> 2001, 2300 -> 2002,
    1120 -> 2003, 1600 -> 2004, 2400 -> 2005.

    Promotions: at snapshot 3, 2201 (tree 2200) from 2101's group; at snapshot
    4, 1121 from 1101's, 1530 from 1510's, 2102 and 2103 from 2202's (one
    remnant of two members) and 2401 from 2302's; none at snapshot 5.
    """

    EXPECTED = {
        1100: 1100,
        1110: 1100,
        1120: 2003,
        1200: 1200,
        1210: 1200,
        1400: 1400,
        1500: 1500,
        1600: 2004,
        2100: 2000,
        2200: 2001,
        2300: 2002,
        2400: 2005,
    }

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_decided_"))
        cls.paths = convert_ascii(cls.tmp / "source", decided_layout(), decided_forests())
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.agg = cls.tmp / "agg"
        cls.index = run_census(cls.dataset, cls.agg, cls.paths)
        cls.table, cls.cut = run_decided(cls.dataset, cls.agg, cls.paths, cls.index)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def fresh_census(self, name):
        other = self.tmp / name
        run_census(self.dataset, other, self.paths)
        return other

    def rows(self, key):
        return [row[key] for row in self.cut["severance"]["per_snapshot"]]

    # ---- the table ----

    def test_the_groups_the_table_is_built_on(self):
        groups = self.table["groups"]
        self.assertEqual((groups["z0_halos"], groups["z0_groups"]), (12, 10))
        self.assertEqual((groups["forests_before"], groups["forests_after"]), (5, 10))
        self.assertEqual(
            groups["split_forests"], {"count": 3, "groups": 8, "extra_groups": 5, "halos": 25}
        )
        self.assertEqual(
            groups["largest_forest"],
            {"forest_index": 4, "forest_id": 2000, "halos": 12, "groups": 4, "split": True},
        )
        other = groups["other_split_forests"]
        self.assertEqual((other["count"], other["extra_groups"], other["halos"]), (2, 2, 13))
        self.assertEqual(groups["forests_with_one_group"], 2)
        self.assertEqual(
            [(f["forest_id"], f["groups"]) for f in groups["most_groups"]],
            [(2000, 4), (1100, 2), (1500, 2)],
        )
        self.assertEqual((self.table["pieces"]["count"], self.table["pieces"]["fresh"]), (8, 5))

    def test_each_forest_splits_into_exactly_its_z0_groups(self):
        got = read_table(self.table["table"]["path"])
        self.assertEqual(got, self.EXPECTED)
        # a forest ending in one z = 0 group keeps its id for every tree
        for root in (1200, 1210, 1400):
            self.assertEqual(got[root], self.index.forest_ids[self.index.find([root])[0]])
        # the table's invariants hold on read
        totals = np.load(trees.trees_dir(self.agg) / "tree_totals.npy")
        roots, ids = cut_table.read_cut_table(
            self.table["table"]["path"], self.index.tree_roots, self.index.forest_ids, totals
        )
        np.testing.assert_array_equal(roots, self.index.tree_roots)
        self.assertEqual(self.table["checks"]["table_invariants"]["cut_forests"], 3)

    def test_the_table_round_trips_through_the_index_reader(self):
        cut_index = SourceIndex.load(self.table["table"]["path"], self.paths["locations"])
        np.testing.assert_array_equal(cut_index.tree_roots, self.index.tree_roots)
        forests = cut_index.forest_table()
        self.assertEqual(
            forests.forest_ids.tolist(),
            [1100, 1200, 1400, 1500, 2000, 2001, 2002, 2003, 2004, 2005],
        )
        self.assertEqual(forests.n_trees.tolist(), [2, 2, 1, 1, 1, 1, 1, 1, 1, 1])

    def test_the_partition_that_cuts_every_ended_co_membership(self):
        independent, ended_only = all_ended_pieces(self.paths["dataset"])
        self.assertTrue(same_partition(independent, read_table(self.table["table"]["path"])))
        # the fixture does cut co-memberships: 2201, 1121, 1530, 2102/2103 and 2401's
        self.assertEqual(
            ended_only,
            {(2100, 2200), (1100, 1120), (1500, 1600), (2300, 2400)},
        )

    def test_the_record(self):
        record = aggregate.read_json(table.table_dir(self.agg) / "record.json")
        self.assertEqual(record["dataset"], self.dataset.identity())
        self.assertEqual(record["rule"]["text"], "the z = 0 FoF groups of every forest")
        self.assertEqual(record["rule"]["final_snapshot"], 5)
        self.assertEqual(record["rule"]["final_scale_factor"], 1.0)
        self.assertEqual(
            (record["rule"]["scope"], record["rule"]["selection"]), ("whole simulation", None)
        )
        self.assertEqual(record["table"], self.table["table"])
        self.assertEqual(record["table"]["md5"], cut_table.md5_file(record["table"]["path"]))
        self.assertEqual(
            record["index_files"]["forests_list"]["md5"],
            cut_table.md5_file(self.paths["forests_list"]),
        )
        self.assertEqual(record["forests"]["split"], 3)
        self.assertEqual(record["fresh_id_floor"], 2000)
        self.assertEqual(
            record["pieces"],
            {
                "id": [1100, 1500, 2000, 2001, 2002, 2003, 2004, 2005],
                "forest_id": [1100, 1500, 2000, 2000, 2000, 1100, 1500, 2000],
                "trees": [2, 1, 1, 1, 1, 1, 1, 1],
                "halos": [6, 3, 4, 3, 3, 2, 2, 2],
                "peak_occupancy": [3, 2, 2, 1, 1, 1, 1, 1],
                "peak_snapshot": [4, 4, 4, 3, 3, 4, 4, 4],
            },
        )
        # the table and the cut evaluate one assignment
        self.assertEqual(record["assignment_sha256"], self.cut["assignment_sha256"])
        self.assertEqual(self.table["assignment_sha256"], self.cut["assignment_sha256"])

    # ---- the cost ----

    def test_promotions_before_the_final_snapshot_and_none_at_it(self):
        self.assertEqual(self.rows("promoted_halos"), [0, 0, 0, 1, 5, 0])
        self.assertEqual(self.rows("groups_losing_members"), [0, 0, 0, 1, 4, 0])
        self.assertEqual(self.rows("groups_central_leaves_members_stay"), [0, 0, 0, 1, 4, 0])
        self.assertEqual(self.rows("remnants_with_several_members"), [0, 0, 0, 0, 1, 0])
        severance = self.cut["severance"]
        self.assertEqual(severance["promoted_at_final_snapshot"], 0)
        self.assertEqual(
            [
                severance[name]
                for name in (
                    "promoted_halos",
                    "groups_losing_members",
                    "groups_central_leaves_members_stay",
                    "remnants_with_several_members",
                )
            ],
            [6, 5, 5, 1],
        )
        for name in severance["per_snapshot"][0]:
            if name != "snapshot":
                self.assertIn(name, severance["definitions"])

    def test_the_progenitor_order_change(self):
        # descendant 1500's chain [1530, 1520] becomes [1520, 1530]; 2100's two
        # progenitors are both promoted and keep their order
        self.assertEqual(self.rows("progenitor_order_changed"), [0, 0, 0, 0, 1, 0])
        self.assertEqual(self.rows("first_progenitor_changed"), [0, 0, 0, 0, 1, 0])
        self.assertEqual(
            self.cut["progenitor_order"], {"descendants_changed": 1, "first_progenitor_changed": 1}
        )
        # checked: descendants 1500 and 2100, each with two progenitors
        self.assertEqual(
            self.cut["stored_chain_check"], {"descendants": 2, "mismatches": 0, "examples": []}
        )

    def test_relabelled_halos_and_the_affected_history_bracket(self):
        self.assertEqual(
            self.cut["relabelled"],
            {
                "forest_id_changed_halos": 12,
                "rank_recomputed_halos": 25,
                # SourceHaloID is recomputed from forest 1100 (ForestIndex 0) on, all 34
                # halos; its kept piece (6 halos) keeps ForestIndex 0, so its prefix stays
                "source_halo_id_shifted_halos": 28,
                "source_halo_id_recomputed_halos": 34,
            },
        )
        # seeds: 2201 and 2101 at snapshot 3; the five promoted and the four
        # losing centrals at 4; 1500 (its chain) at 5. Dependents add 2202 and
        # 2102 (already seeds) at 4 and the seven other roots below a seed at 5
        self.assertEqual(self.rows("seed_halos"), [0, 0, 0, 2, 9, 1])
        self.assertEqual(self.rows("affected_halos"), [0, 0, 0, 2, 9, 8])

    # ---- the partition ----

    def points(self):
        return {(p["ntask"], p["nchunk"]): p for p in self.cut["partition"]["grid"]}

    def test_the_partition_with_the_pieces_installed(self):
        points = self.points()
        # installed peak weights, re-derived by hand: [3, 2, 1, 2, 2] for
        # ForestIndex 0..4 (the kept pieces of 1100, 1500 and 2000 peak at 3, 2
        # and 2; forest 1200 holds 2 rows and 1400 one in each of slabs 3 to 5),
        # then 1 for each of 2001..2005; every peak equals the piece's rows in
        # the widest slab 4, so the cuts are those of the widest slab's counts
        self.assertEqual(points[(1, 2)]["forest_cuts"], [0, 4, 10])
        self.assertEqual(points[(1, 2)]["widest_rows_per_snapshot"], [0, 0, 0, 4, 8, 6])
        self.assertEqual(points[(1, 1)]["widest"]["rows"], 15)
        self.assertEqual(points[(1, 4)]["forest_cuts"], [0, 2, 5, 10, 10])
        self.assertEqual(points[(1, 4)]["job_rows"], 5)
        partition_record = self.cut["partition"]
        self.assertEqual(
            (partition_record["bytes_per_halo"], partition_record["reserve_gib"]), (1 << 30, 1.0)
        )
        # uncut, forest 2000's five rows of snapshot 4 are the floor
        self.assertEqual(partition_record["uncut_floor"]["rows"], 5)

        fitting = [
            (
                c["class_gib"],
                c["smallest_fitting_point"]["ntask"],
                c["smallest_fitting_point"]["nchunk"],
            )
            for c in partition_record["laptop_classes"]
        ]
        self.assertEqual(fitting, [(6, 1, 4), (9, 1, 2), (16, 1, 1)])

    def test_a_class_is_judged_on_the_job_not_one_process(self):
        # the peak weights above, re-derived by hand for two ranks: capacity 8
        # packs [3, 2, 1, 2] | [2, 1, 1, 1, 1, 1], ranks over [0, 4) and
        # [4, 10); rank 0's widest slab holds 8 rows,
        # rank 1's 7; one process needs 8 GiB, which 9 GiB less the reserve
        # holds, but the two ranks run at once and need 15
        two_ranks = self.points()[(2, 1)]
        self.assertEqual(two_ranks["forest_cuts"], [0, 4, 10])
        self.assertEqual(two_ranks["task_widest_rows"], [8, 7])
        self.assertEqual((two_ranks["process_bytes"], two_ranks["job_bytes"]), (8 << 30, 15 << 30))
        self.assertEqual(two_ranks["fits_gib"], [16])

    # ---- invariance, selection and the command line ----

    def test_results_do_not_depend_on_the_block_size(self):
        other = self.tmp / "agg_block_1"
        index = run_census(self.dataset, other, self.paths, block_rows=1)
        summary_table, summary_cut = run_decided(
            self.dataset, other, self.paths, index, block_rows=1
        )
        self.assertEqual(
            (other / "table" / "forests.list").read_bytes(),
            (self.agg / "table" / "forests.list").read_bytes(),
        )
        varying = ("dataset_dir", "io", "memory", "measurements", "aggregates", "table")
        for got, want in ((summary_table, self.table), (summary_cut, self.cut)):
            got = json.loads(json.dumps(got).replace(str(other), str(self.agg)))
            want = json.loads(json.dumps(want))
            for key in set(want) - set(varying):
                self.assertEqual(got[key], want[key], key)

    def test_a_forest_selection_splits_only_the_named_forests(self):
        other = self.fresh_census("agg_selection")
        selection = table.selected_forests([4], self.dataset.n_forests_total)
        summary_table, summary_cut = run_decided(
            self.dataset, other, self.paths, self.index, selection
        )
        self.assertFalse((other / "table").exists())
        got = read_table(other / "table-restricted" / "forests.list")
        # forest 2000 split, fresh by size: 2200 -> 2001, 2300 -> 2002, 2400 -> 2003
        expected = {
            root: self.index.forest_ids[at] for at, root in enumerate(self.index.tree_roots)
        }
        expected.update({2100: 2000, 2200: 2001, 2300: 2002, 2400: 2003})
        self.assertEqual(got, expected)
        record = aggregate.read_json(other / "table-restricted" / "record.json")
        self.assertEqual(record["rule"]["scope"], "restricted to the selected forests")
        self.assertIn("every other forest keeps its identity assignment", record["rule"]["text"])
        self.assertEqual(record["rule"]["selection"], {"forest_index": [4], "forest_id": [2000]})
        self.assertNotEqual(summary_table["rule"]["scope"], "whole simulation")
        self.assertEqual(summary_cut["scope"], "restricted to the selected forests")
        self.assertTrue((other / "cut-restricted" / "summary.json").is_file())
        self.assertEqual(
            [r["promoted_halos"] for r in summary_cut["severance"]["per_snapshot"]],
            [0, 0, 0, 1, 3, 0],
        )
        self.assertEqual(summary_cut["pieces"]["split_forests"], 1)

    def test_an_unchanged_only_selection_gives_the_identity_table(self):
        other = self.fresh_census("agg_unchanged_selection")
        # forest 1200 (ForestIndex 1) ends in one z = 0 group
        selection = table.selected_forests([1], self.dataset.n_forests_total)
        summary_table, summary_cut = run_decided(
            self.dataset, other, self.paths, self.index, selection
        )
        check_identity_table(self, other / "table-restricted" / "forests.list", self.index)
        check_zero_cost(self, summary_table, summary_cut, self.dataset.n_forests_total)

    def test_an_index_missing_a_census_root_is_refused(self):
        other = self.fresh_census("agg_missing_root")
        with self.assertRaisesRegex(
            ConverterError,
            "do not list this census's tree roots: 1 census root\\(s\\) missing \\(e.g. \\[1100\\]\\)"
            ".*no cut table is written",
        ):
            table.run_table(
                self.dataset, other, self.index.tree_roots[1:], self.index.forest_ids[1:], {}
            )
        self.assertFalse(table.table_dir(other).exists())

    def test_a_class_below_every_job_has_no_fitting_point(self):
        other = self.fresh_census("agg_no_fit")
        arguments = ["cut", "--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        arguments += ["--ntask", "1,2", "--nchunk", "1,2", "--bytes-per-halo", str(1 << 30)]
        arguments += ["--laptop-gib", "1", "--reserve-gib", "0.5"]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr() as captured:
            self.assertEqual(forest_census.main(arguments), 0, captured.text)
        self.assertIn("    1 GiB less 0.5 GiB: no grid point fits", stdout.getvalue())
        row = aggregate.read_json(cut.cut_dir(other) / "summary.json")["partition"]
        self.assertEqual(row["laptop_classes"][0]["feasible"], False)
        self.assertIsNone(row["laptop_classes"][0]["smallest_fitting_point"])

    def test_the_table_and_cut_command_lines(self):
        other = str(self.tmp / "agg_cli")
        dataset = ["--dataset", str(self.paths["dataset"]), "--aggregate", other]
        index = ["--forests-list", str(self.paths["forests_list"])]
        index += ["--locations", str(self.paths["locations"])]
        grid = ["--ntask", "1", "--nchunk", "1,4", "--bytes-per-halo", str(1 << 30)]
        grid += ["--laptop-gib", "6,9,16", "--reserve-gib", "1"]
        with contextlib.redirect_stdout(io.StringIO()), capture_stderr() as captured:
            self.assertEqual(forest_census.main(["occupancy"] + dataset), 0)
            self.assertEqual(forest_census.main(["trees"] + dataset + index), 0)
            self.assertEqual(forest_census.main(["partition", "--aggregate", other]), 0)
            self.assertEqual(forest_census.main(["table"] + dataset + index), 0)
            status = forest_census.main(["cut"] + dataset + grid)
        self.assertEqual(status, 0, captured.text)
        written_table = aggregate.read_json(table.table_dir(Path(other)) / "summary.json")
        self.assertEqual(written_table["groups"], self.table["groups"])
        self.assertEqual(written_table["pieces"], self.table["pieces"])
        written_cut = aggregate.read_json(cut.cut_dir(Path(other)) / "summary.json")
        self.assertEqual(written_cut["pieces"], self.cut["pieces"])
        self.assertEqual(written_cut["severance"], self.cut["severance"])
        fitting = {
            c["class_gib"]: c["smallest_fitting_point"]["nchunk"]
            for c in written_cut["partition"]["laptop_classes"]
        }
        self.assertEqual(fitting, {6: 4, 9: 4, 16: 1})
        self.assertEqual(
            (Path(other) / "table" / "forests.list").read_bytes(),
            (self.agg / "table" / "forests.list").read_bytes(),
        )
        # the reserve is a required argument
        with capture_stderr(), self.assertRaises(SystemExit):
            forest_census.main(["cut"] + dataset)

    def test_the_table_does_not_depend_on_the_catalogue_layout(self):
        """The same forests with every tree in one file, in reverse order, differ from
        the multi-file catalogue only in the sidecar's source ordinals and the index
        files: the table file is byte-identical and the record agrees except for the
        fields that name the layout's own files, ``index_files`` and the table's path."""
        root = self.tmp / "one_file"
        paths = convert_ascii_one_file(root / "source", decided_layout(), decided_forests())
        dataset = HorizontalDataset(paths["dataset"])
        with h5py.File(paths["dataset"] / "forests.h5", "r") as handle:
            np.testing.assert_array_equal(handle["SourceFileOrdinal"][...], [0] * 5)
        index = run_census(dataset, root / "agg", paths)
        self.assertEqual(sorted(index.filenames.values()), ["tree_0.dat"])
        run_quietly(
            table.run_table,
            dataset,
            root / "agg",
            index.tree_roots,
            index.forest_ids,
            index_files(paths),
        )

        def record_of(agg):
            record = aggregate.read_json(table.table_dir(agg) / "record.json")
            del record["index_files"]
            record["table"] = {k: v for k, v in record["table"].items() if k != "path"}
            return record

        self.assertEqual(
            (table.table_dir(root / "agg") / "forests.list").read_bytes(),
            (table.table_dir(self.agg) / "forests.list").read_bytes(),
        )
        self.assertEqual(record_of(root / "agg"), record_of(self.agg))

    # ---- refusals ----

    def test_refusals_come_before_any_output(self):
        other = self.tmp / "agg_refusals"
        dataset = ["--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        index = ["--forests-list", str(self.paths["forests_list"])]
        index += ["--locations", str(self.paths["locations"])]
        run_quietly(occupancy.run_occupancy, self.dataset, other)
        for arguments, message in (
            (["table"] + dataset + index, "run trees first"),
            (["cut"] + dataset + ["--reserve-gib", "1"], "run trees first"),
        ):
            with capture_stderr() as captured:
                self.assertEqual(forest_census.main(arguments), 2, arguments)
            self.assertIn(message, captured.text)
        run_quietly(trees.run_trees, self.dataset, other, self.index)
        for arguments, message in (
            (["cut"] + dataset + ["--reserve-gib", "1"], "run partition first"),
            (["cut"] + dataset + ["--reserve-gib", "16"], "no usable memory"),
            (["table"] + dataset + index + ["--forest-index", "9"], "ForestIndex \\[9\\] outside"),
            (
                ["cut"] + dataset + ["--reserve-gib", "1", "--forest-index", "-1"],
                "outside \\[0, 5\\)",
            ),
        ):
            with capture_stderr() as captured:
                self.assertEqual(forest_census.main(arguments), 2, arguments)
            self.assertRegex(captured.text, message)
            self.assertNotIn("Traceback", captured.text)
        for name in ("table", "cut", "table-restricted", "cut-restricted"):
            self.assertFalse((other / name).exists(), name)

    def test_index_files_that_do_not_describe_the_census_are_refused(self):
        other = self.fresh_census("agg_reassigned")
        replaced = self.tmp / "reassigned.list"
        replaced.write_text(
            self.paths["forests_list"].read_text().replace("2400 2000", "2400 1500")
        )
        arguments = ["table", "--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        arguments += ["--forests-list", str(replaced), "--locations", str(self.paths["locations"])]
        with capture_stderr() as captured:
            status = forest_census.main(arguments)
        self.assertEqual(status, 2)
        self.assertIn("give 1 tree root(s) a forest other than the dataset's", captured.text)
        self.assertIn("[2400]", captured.text)
        self.assertFalse(table.table_dir(other).exists())

    def test_integers_beyond_int64_are_refused(self):
        other = self.fresh_census("agg_overflow")
        huge = self.tmp / "huge.list"
        huge.write_text(
            self.paths["forests_list"].read_text().replace("2400 2000", "2400 18446744073709551616")
        )
        arguments = ["table", "--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        arguments += ["--forests-list", str(huge), "--locations", str(self.paths["locations"])]
        with capture_stderr() as captured:
            status = forest_census.main(arguments)
        self.assertEqual(status, 2)
        self.assertIn("a tree root or forest id beyond int64", captured.text)
        self.assertFalse(table.table_dir(other).exists())
        with self.assertRaisesRegex(ConverterError, "huge.list: malformed cut table"):
            cut_table.read_cut_table(
                huge,
                self.index.tree_roots,
                self.index.forest_ids,
                np.load(trees.trees_dir(other) / "tree_totals.npy"),
            )

    def test_outputs_of_an_earlier_run_are_refused(self):
        other = self.fresh_census("agg_stale")
        run_decided(self.dataset, other, self.paths, self.index)
        before = sorted(path.name for path in other.rglob("*"))
        for directory in (table.table_dir(other), cut.cut_dir(other)):
            stray = directory / "stray.npy"
            stray.write_bytes(b"left by hand")
            with self.assertRaisesRegex(ConverterError, "would not rewrite.*stray.npy"):
                run_decided(self.dataset, other, self.paths, self.index)
            stray.unlink()
        self.assertEqual(sorted(path.name for path in other.rglob("*")), before)
        # a temporary an interrupted write left for an output is replaced, not refused
        leftover = table.table_dir(other) / "record.json.tmp"
        leftover.write_bytes(b"interrupted")
        run_decided(self.dataset, other, self.paths, self.index)
        self.assertFalse(leftover.exists())

    def test_final_slab_central_references_are_validated(self):
        other = self.fresh_census("agg_references")
        last = self.dataset.snapshots[-1]
        ids = read_slab_column(self.paths["dataset"], last, "MostBoundID")
        row = {int(halo): at for at, halo in enumerate(ids)}
        for name, edit, message in (
            ("range", {1110: 99}, "1 FirstHaloInFOFgroup value\\(s\\) outside \\[0, 12\\)"),
            # 1100 names the satellite 1110: neither 1110 (named by 1100) nor 1100
            # (named by 1110) is then its own central
            (
                "self",
                {1100: row[1110]},
                re.escape(
                    "2 row(s) named as a FoF central by FirstHaloInFOFgroup are not their own "
                    "central (e.g. rows {})".format(sorted([row[1100], row[1110]]))
                ),
            ),
            # 1120, a central of forest 1100, names 1200's row in forest 1200
            (
                "foreign",
                {1120: row[1200]},
                "1 z = 0 halo\\(s\\) whose FirstHaloInFOFgroup names a row of another forest",
            ),
        ):
            copy = self.tmp / ("dataset_" + name)
            shutil.copytree(self.paths["dataset"], copy)
            with h5py.File(copy / "snapshot_{:03d}.h5".format(last), "r+") as handle:
                column = handle["halos"]["FirstHaloInFOFgroup"]
                values = column[...]
                for halo, target in edit.items():
                    values[row[halo]] = target
                column[...] = values
            with self.assertRaisesRegex(ConverterError, message):
                table.run_table(
                    HorizontalDataset(copy),
                    other,
                    self.index.tree_roots,
                    self.index.forest_ids,
                    {},
                )
        self.assertFalse(table.table_dir(other).exists())

    def test_a_split_forest_must_hold_one_piece_per_z0_central(self):
        other = self.fresh_census("agg_piece_count")
        real = cut_table.name_pieces

        def merging(components, local_forest, *args):
            # every tree of forest 2000 (ForestIndex 4) given one piece key
            components = np.array(components, copy=True)
            mine = np.flatnonzero(np.asarray(local_forest) == 4)
            components[mine] = components[mine[0]]
            return real(components, local_forest, *args)

        with mock.patch.object(table, "name_pieces", merging):
            with self.assertRaisesRegex(
                ConverterError,
                "1 split forest\\(s\\) whose piece count is not their z = 0 central count "
                "\\(e.g. ForestIndex 4: 1 pieces, 4 centrals\\)",
            ):
                run_decided(self.dataset, other, self.paths, self.index)
        self.assertFalse(table.table_dir(other).exists())

    def test_a_promotion_at_the_final_snapshot_is_refused(self):
        other = self.fresh_census("agg_final_promotion")
        real = table.decided_table

        def wrong(*args, **kwargs):
            built = real(*args, **kwargs)
            # tree 1110, a z = 0 satellite of 1100, moved into 1120's piece
            roots = trees.load_roots(other)[built.local].tolist()
            built.tree_piece[roots.index(1110)] = built.tree_piece[roots.index(1120)]
            return built

        with mock.patch.object(table, "decided_table", wrong):
            with self.assertRaisesRegex(
                ConverterError, "1 halo\\(s\\) promoted at the final snapshot"
            ):
                cut.run_cut(self.dataset, other, 1.0, laptop_gib=[6])
        self.assertFalse((cut.cut_dir(other) / "summary.json").exists())

    def test_an_installed_partition_that_loses_rows_is_refused(self):
        other = self.fresh_census("agg_installed")
        real = cut.install_pieces

        def short(*args):
            forests, counts = real(*args)
            return forests, counts + 1

        with mock.patch.object(cut, "install_pieces", short):
            with self.assertRaisesRegex(
                ConverterError, r"snapshot_00\d.h5: the installed partition holds \d+ rows but "
            ):
                cut.run_cut(self.dataset, other, 1.0, laptop_gib=[6])


def convert_ascii_one_file(root: Path, file_trees, forests):
    """:func:`convert_ascii` with every tree of ``file_trees`` written to one
    file, in reverse order: the same forests under another catalogue layout."""
    return convert_ascii(root, [[tree for trees in file_trees for tree in trees][::-1]], forests)


class TestNoSplitForest(unittest.TestCase):
    """Forests 1200 and 1400, each ending in one z = 0 group: the decided table
    is the identity table and the cut costs nothing, whether the catalogue
    holds the trees in one file or forest 1200 spans two."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="census_no_split_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_one_file_and_a_spanning_forest(self):
        _multi, spanning, single = fixtures.correspondence_forests()
        forests = [spanning, single]
        layouts = (
            ("one_file", [fixtures.all_trees(forests)]),
            ("spanning", [[spanning.trees[0]], [single.trees[0], spanning.trees[1]]]),
        )
        for name, layout in layouts:
            with self.subTest(name):
                paths = convert_ascii(self.tmp / name, layout, forests)
                dataset = HorizontalDataset(paths["dataset"])
                agg = self.tmp / ("agg_" + name)
                index = run_census(dataset, agg, paths)
                summary_table, summary_cut = run_decided(dataset, agg, paths, index)
                check_identity_table(self, table.table_dir(agg) / "forests.list", index)
                check_zero_cost(self, summary_table, summary_cut, dataset.n_forests_total)


class TestNamingTieRule(unittest.TestCase):
    """:func:`naming_forest`: three tied pieces, two of them keyed by a central
    tree that is not their smallest-root tree."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_naming_"))
        forests = [naming_forest()]
        cls.paths = convert_ascii(cls.tmp / "source", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.agg = cls.tmp / "agg"
        cls.index = run_census(cls.dataset, cls.agg, cls.paths)
        cls.table, cls.cut = run_decided(cls.dataset, cls.agg, cls.paths, cls.index)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_the_smallest_root_breaks_the_tie(self):
        self.assertEqual(
            read_table(self.table["table"]["path"]),
            {5050: 5000, 5100: 5000, 5055: 5001, 5300: 5001, 5060: 5002},
        )
        record = aggregate.read_json(table.table_dir(self.agg) / "record.json")
        self.assertEqual(record["pieces"]["halos"], [4, 4, 4])
        independent, _ended = all_ended_pieces(self.paths["dataset"])
        self.assertTrue(same_partition(independent, read_table(self.table["table"]["path"])))
        largest = self.table["pieces"]["largest"]
        self.assertEqual(
            [(p["id"], p["smallest_root_id"]) for p in largest],
            [(5000, 5050), (5001, 5055), (5002, 5060)],
        )


class TestGroupSplitting(unittest.TestCase):
    """One group losing members to two pieces: the group, remnant and
    several-member remnant counts differ."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_split_"))
        forests = [splitting_forest()]
        cls.paths = convert_ascii(cls.tmp / "source", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.agg = cls.tmp / "agg"
        cls.index = run_census(cls.dataset, cls.agg, cls.paths)
        cls.table, cls.cut = run_decided(cls.dataset, cls.agg, cls.paths, cls.index)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_one_group_two_remnants_one_with_several_members(self):
        # tree 3200 (3 halos) keeps 3000; 3100 and 3300 (2 each) take 3001, 3002
        self.assertEqual(
            read_table(self.table["table"]["path"]), {3100: 3001, 3200: 3000, 3300: 3002}
        )
        severance = self.cut["severance"]
        self.assertEqual(severance["promoted_halos"], 3)
        self.assertEqual(severance["groups_losing_members"], 1)
        self.assertEqual(severance["groups_central_leaves_members_stay"], 2)
        self.assertEqual(severance["remnants_with_several_members"], 1)
        # 3200's two promoted progenitors keep their order: checked, unchanged
        self.assertEqual(self.cut["progenitor_order"]["descendants_changed"], 0)
        self.assertEqual(self.cut["stored_chain_check"]["descendants"], 1)


class TestDependentPropagation(unittest.TestCase):
    """Seeds two and more slabs before the last: dependents propagate along
    descendant links through every later slab."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_propagation_"))
        forests = [propagating_forest()]
        cls.paths = convert_ascii(cls.tmp / "source", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.agg = cls.tmp / "agg"
        cls.index = run_census(cls.dataset, cls.agg, cls.paths)
        cls.table, cls.cut = run_decided(cls.dataset, cls.agg, cls.paths, cls.index)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_dependents_propagate_to_the_last_slab(self):
        rows = {row["snapshot"]: row for row in self.cut["severance"]["per_snapshot"]}
        # snapshot 2: 4102 and 4302 promoted from 4202's group; snapshot 3: 4303
        # from 4203's; their descendants and the losing centrals' run on to 5
        self.assertEqual([rows[snap]["promoted_halos"] for snap in range(6)], [0, 0, 2, 1, 0, 0])
        self.assertEqual([rows[snap]["seed_halos"] for snap in range(6)], [0, 0, 3, 2, 0, 0])
        self.assertEqual([rows[snap]["affected_halos"] for snap in range(6)], [0, 0, 3, 3, 3, 3])
        # snapshot 2: 4102, 4302 and their central 4202; snapshot 3: 4303 and 4203
        self.assertEqual([rows[snap]["retained_rows"] for snap in range(6)], [0, 0, 3, 2, 0, 0])
        # three tied trees of 4 halos: 4100 keeps 4000
        self.assertEqual(
            read_table(self.table["table"]["path"]), {4100: 4000, 4200: 4001, 4300: 4002}
        )


class TestEarlyEndingRefused(unittest.TestCase):
    """A census holding a tree that ends before the final snapshot is refused
    by the table and the cut, naming the count, before any output: the
    negative fixture and the committed fixture (:data:`WORKED_ASCII`)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="census_early_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def check_refused(self, dataset_dir, paths, message):
        agg = str(self.tmp / "agg_{}".format(Path(dataset_dir).name))
        dataset = ["--dataset", str(dataset_dir), "--aggregate", agg]
        index = ["--forests-list", str(paths[0]), "--locations", str(paths[1])]
        with contextlib.redirect_stdout(io.StringIO()), capture_stderr():
            for arguments in (["occupancy"] + dataset, ["trees"] + dataset + index):
                self.assertEqual(forest_census.main(arguments), 0, arguments)
            self.assertEqual(forest_census.main(["partition", "--aggregate", agg]), 0)
        for arguments in (["table"] + dataset + index, ["cut"] + dataset + ["--reserve-gib", "1"]):
            with capture_stderr() as captured:
                self.assertEqual(forest_census.main(arguments), 2, arguments)
            self.assertIn(message, captured.text)
        for name in ("table", "cut"):
            self.assertFalse((Path(agg) / name).exists(), name)

    def test_a_tree_ending_before_the_final_snapshot_is_refused(self):
        forests = fixtures.early_ending_correspondence_forests()
        paths = convert_ascii(self.tmp / "early", correspondence_layout(), forests)
        with self.subTest("the negative fixture"):
            self.check_refused(
                paths["dataset"],
                (paths["forests_list"], paths["locations"]),
                "1 tree(s) end before the final snapshot 5 (e.g. roots [1300])",
            )
        with self.subTest("the committed fixture"):
            self.check_refused(
                WORKED_ASCII,
                self.committed_index_files(),
                "1 tree(s) end before the final snapshot 5 (e.g. roots [3010])",
            )

    def committed_index_files(self):
        """A ``forests.list`` and ``locations.dat`` for the committed fixture, written
        from the roots and forests its own census finds."""
        dataset = HorizontalDataset(WORKED_ASCII)
        probe = self.tmp / "probe"
        run_quietly(trees.run_trees, dataset, probe)
        roots = trees.load_roots(probe)
        forest_ids = dataset.forest_ids()[np.load(trees.trees_dir(probe) / "tree_forest.npy")]
        forests_list = self.tmp / "forests.list"
        forests_list.write_text(
            "#TreeRootID ForestID\n"
            + "".join("{} {}\n".format(r, f) for r, f in zip(roots.tolist(), forest_ids.tolist()))
        )
        locations = self.tmp / "locations.dat"
        locations.write_text(
            "#TreeRootID FileID Offset Filename\n"
            + "".join("{} 0 {} tree_0.dat\n".format(r, 100 * at) for at, r in enumerate(roots))
        )
        return forests_list, locations


if __name__ == "__main__":
    unittest.main()
