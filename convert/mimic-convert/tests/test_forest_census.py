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
- the identity record against the committed version 2 fixture
  (``simulations/micro-uchuu-ascii-horizontal/_tests/data/generic``) and its
  ``fixture_manifest.json``;
- the decided table against tables written out by hand from the fixture
  topology, on version 3 and version 2 datasets the converter writes from the
  same forests, and against the partition that cuts every co-membership ended
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
import pipeline  # noqa: E402
from census import (  # noqa: E402
    aggregate,
    cut,
    cut_table,
    occupancy,
    partition,
    table,
    trees,
)
from column_schema import build_schema, load_column_map  # noqa: E402
from errors import ConverterError  # noqa: E402
from fixups import run_fixups  # noqa: E402
from hdf5_writer import run_write as run_write_v2  # noqa: E402
from hdf5_writer_v3 import HorizontalV3Writer  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from links import run_links  # noqa: E402
from scatter import run_scatter  # noqa: E402
from sort_index import run_sort  # noqa: E402
from source_index import SourceIndex  # noqa: E402
from test_fixups import capture_stderr  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
GENERIC_V2 = (
    REPO_ROOT / "simulations" / "micro-uchuu-ascii-horizontal" / "_tests" / "data" / "generic"
)
ASCII_PROFILE = Path(__file__).resolve().parents[1] / "profiles" / "consistent_trees_ascii.yaml"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def write_locations(path, tree_files):
    """``locations.dat`` for ``tree_files`` (``FileID`` = list position): each
    ``#tree`` marker's root with the byte offset of its block's first data row."""
    lines = ["#TreeRootID FileID Offset Filename"]
    for file_id, tree_file in enumerate(tree_files):
        position = 0
        for line in Path(tree_file).read_bytes().splitlines(keepends=True):
            position += len(line)
            if line.startswith(b"#tree "):
                root = int(line.split()[1])
                lines.append("{} {} {} {}".format(root, file_id, position, Path(tree_file).name))
    Path(path).write_text("\n".join(lines) + "\n")
    return Path(path)


def convert_ascii(root: Path, file_trees, forests):
    """Convert fixture trees, ``file_trees[i]`` written to ``tree_<i>.dat``, to a
    version 3 dataset; returns the paths the census needs."""
    root.mkdir(parents=True)
    tree_files = [
        fixtures.write_ctrees_file(root / "tree_{}.dat".format(i), trees_of_file)
        for i, trees_of_file in enumerate(file_trees)
    ]
    forests_list = fixtures.write_forests_list(root / "forests.list", forests)
    locations = write_locations(root / "locations.dat", tree_files)
    sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
    a_list = fixtures.write_a_list(root / "fixture.a_list")
    work = root / "work"
    pipeline.initialize(
        work,
        build_schema(load_column_map(ASCII_PROFILE)),
        {
            "tree_files": [str(path) for path in tree_files],
            "forests_list": str(forests_list),
            "simulation_info": str(sim_info),
        },
        a_list,
        ingest_max_rows=4,
        transpose_budget_bytes=4 << 20,
    )
    with capture_stderr():
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        manifest = pipeline.run_write(work, HorizontalV3Writer(sim_info))
    return {
        "dataset": manifest.artifact_path(manifest.stage("write")["directory"]),
        "forests_list": forests_list,
        "locations": locations,
        "preparation_manifest": manifest.artifact_path(pipeline.ASCII_PREPARATION_DIR)
        / "manifest.json",
    }


def convert_ascii_v2(root: Path, file_trees, forests):
    """Convert fixture trees as :func:`convert_ascii` does, through the
    version 2 route (scatter, sort, fix-ups, links, the version 2 writer);
    returns the paths the census needs."""
    root.mkdir(parents=True)
    tree_files = [
        fixtures.write_ctrees_file(root / "tree_{}.dat".format(i), trees_of_file)
        for i, trees_of_file in enumerate(file_trees)
    ]
    forests_list = fixtures.write_forests_list(root / "forests.list", forests)
    locations = write_locations(root / "locations.dat", tree_files)
    sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
    a_list = fixtures.write_a_list(root / "fixture.a_list")
    work = root / "work"
    with capture_stderr():
        run_scatter(tree_files, forests_list, a_list, work, simulation_info_path=sim_info)
        run_sort(work)
        run_fixups(work, a_list, sim_info)
        run_links(work)
        run_write_v2(work, a_list, sim_info)
    return {"dataset": work / "hdf5", "forests_list": forests_list, "locations": locations}


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


def copy_generic(destination: Path) -> Path:
    shutil.copytree(GENERIC_V2, destination)
    return destination


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


class TestPartition(unittest.TestCase):
    def test_heavy_forest_shares_range_with_light_neighbour(self):
        # tests/unit/test_horizontal_partition.c, weights {1, 5, 1} on two ranks
        self.assertEqual(partition.cut_forests([1, 5, 1], 2), [0, 2, 3])
        self.assertEqual(partition.partition_cut([1, 5, 1], 2, 1), [0, 2, 3])

    def test_single_rank_and_empty_dataset(self):
        # tests/unit/test_horizontal_partition.c, weights {3, 0, 7, 1} on one rank
        self.assertEqual(partition.cut_forests([3, 0, 7, 1], 1), [0, 4])
        for ntask in range(1, 6):
            self.assertEqual(partition.cut_forests([], ntask), [0] * (ntask + 1))
            self.assertEqual(partition.partition_cut([], ntask, 3), [0] * (3 * ntask + 1))

    def test_zero_weight_forests_ride_in_a_neighbouring_range(self):
        # tests/unit/test_horizontal_partition.c, forests 0, 2, 3 and 5 empty
        self.assertEqual(partition.cut_forests([0, 3, 0, 0, 2, 0], 2), [0, 4, 6])

    def test_forest_blocks_recorded_chunkings(self):
        weights = [2, 1, 7, 2, 2, 3]
        # serial chunk legs (test_chunked_sweep.py): [0, 3, 6] at G = 2 and
        # [0, 2, 3, 6, 6, ...] for every G >= 3, so G = 8 has five idle chunks
        self.assertEqual(partition.partition_cut(weights, 1, 2), [0, 3, 6])
        self.assertEqual(partition.partition_cut(weights, 1, 3), [0, 2, 3, 6])
        self.assertEqual(partition.partition_cut(weights, 1, 8), [0, 2, 3] + [6] * 6)
        # MPI chunk legs (2, 2) and (3, 3), derived by hand: tasks [0, 3, 6]
        # then chunks of [2, 1, 7] and [2, 2, 3]; tasks [0, 2, 3, 6] then
        # chunks of [2, 1], [7] (two idle) and [2, 2, 3]
        self.assertEqual(partition.partition_cut(weights, 2, 2), [0, 2, 3, 5, 6])
        self.assertEqual(partition.partition_cut(weights, 3, 3), [0, 1, 2, 2, 3, 3, 3, 4, 5, 6])
        # the task level packs at the smallest feasible capacity (7, the heaviest
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
        self.assertEqual([table.files_of(i).tolist() for i in range(4)], [[0], [0], [0, 1], [1]])
        # F's markers F, F, (G), F: one unit, rank 0; G is rank 1 although an F
        # marker follows it; H spans files; in b.dat H's marker comes first, so
        # K is rank 1 there
        np.testing.assert_array_equal(table.file_ordinal, [0, 0, -1, 1])
        np.testing.assert_array_equal(table.unit_ordinal, [0, 1, -1, 1])

    def test_a_root_in_one_file_only_is_refused(self):
        with self.assertRaisesRegex(ConverterError, r"1 tree root\(s\) only in .*forests.list"):
            SourceIndex.load(*self.write(self.FORESTS + [(99, 9)], self.LOCATIONS))
        with self.assertRaisesRegex(ConverterError, r"1 only in .*locations.dat \(e.g. \[99\]"):
            SourceIndex.load(*self.write(self.FORESTS, self.LOCATIONS + [(99, 1, 900, "b.dat")]))

    def test_a_repeated_root_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "listed more than once; examples: \\[12\\]"):
            SourceIndex.load(*self.write(self.FORESTS + [(12, 1)], self.LOCATIONS))


# ---------------------------------------------------------------------------
# The dataset reader and the identity record (version 2)
# ---------------------------------------------------------------------------


class TestVersion2Dataset(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="census_v2_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.manifest = json.loads((GENERIC_V2 / "fixture_manifest.json").read_text())

    def manifest_counts(self):
        files = self.manifest["files"]
        return [
            files["snapshot_{:03d}.h5".format(snap)]["header_attributes"]["n_halos"]["value"]
            for snap in range(self.manifest["n_snapshots"])
        ]

    def test_identity_record(self):
        with h5py.File(GENERIC_V2 / "forests.h5", "r") as handle:
            forest_ids = handle["ForestID"][...]
        header = self.manifest["files"]["snapshot_000.h5"]["header_attributes"]
        self.assertEqual(
            HorizontalDataset(GENERIC_V2).identity(),
            {
                "format_version": 2,
                "source_format": None,
                "n_forests_total": header["n_forests_total"]["value"],
                "n_halos": self.manifest_counts(),
                "forest_id_sha256": hashlib.sha256(forest_ids.astype("<i8").tobytes()).hexdigest(),
            },
        )

    def test_header_and_bounded_block_reads(self):
        dataset = HorizontalDataset(GENERIC_V2)
        self.assertEqual(dataset.snapshots, tuple(range(6)))
        self.assertEqual(list(dataset.scale_factors), self.manifest["a_list"])
        self.assertEqual(dataset.header(4)["n_halos"], 6)
        whole = read_slab_column(GENERIC_V2, 4, "MostBoundID")
        blocks = list(dataset.iter_column(4, "MostBoundID", block_rows=4))
        self.assertEqual([(start, block.size) for start, block in blocks], [(0, 4), (4, 2)])
        np.testing.assert_array_equal(np.concatenate([b for _s, b in blocks]), whole)
        self.assertEqual(list(dataset.iter_column(0, "MostBoundID")), [])
        with self.assertRaisesRegex(ConverterError, "no /halos/Nothing"):
            list(dataset.iter_column(4, "Nothing"))
        with self.assertRaisesRegex(ConverterError, "positive integer"):
            list(dataset.iter_column(4, "MostBoundID", block_rows=0))

    def test_a_missing_snapshot_is_refused(self):
        broken = copy_generic(self.tmp / "broken")
        (broken / "snapshot_003.h5").rename(self.tmp / "aside.h5")
        with self.assertRaisesRegex(ConverterError, "missing \\['snapshot_003.h5'\\]"):
            HorizontalDataset(broken)

    def test_occupancy_prints_the_manifest_counts(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr():
            status = forest_census.main(
                ["occupancy", "--dataset", str(GENERIC_V2), "--aggregate", str(self.tmp / "agg")]
            )
        self.assertEqual(status, 0)
        text = stdout.getvalue()
        counts = self.manifest_counts()
        for snap, count in enumerate(counts):
            self.assertIn("snapshot {:3d}: {} halos".format(snap, count), text)
        self.assertIn("total halos: {}".format(sum(counts)), text)
        self.assertIn("n_forests_total 3", text)

    def test_trees_on_version_2_follow_the_descendant_chains(self):
        dataset = HorizontalDataset(GENERIC_V2)
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
        """A copy of ``generic`` with ``edit(handle)`` applied to one file."""
        copy = copy_generic(self.tmp / name)
        with h5py.File(copy / file_name, "r+") as handle:
            edit(handle)
        return copy

    def test_inconsistent_datasets_are_refused_at_open(self):
        def set_header(name, value):
            def edit(handle):
                handle["header"].attrs[name] = value

            return edit

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
                set_header("format_version", np.int32(3)),
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
        no_links = copy_generic(self.tmp / "no_links")
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
        gapped = copy_generic(self.tmp / "gapped")
        for path in gapped.glob("snapshot_*.h5"):
            with h5py.File(path, "r+") as handle:
                handle["header"].attrs["links_adjacent"] = np.int32(0)
        with self.assertRaisesRegex(ConverterError, "links_adjacent is 0"):
            trees.run_trees(HorizontalDataset(gapped), self.tmp / "agg")

    def test_a_repeated_terminal_id_is_refused(self):
        repeated = copy_generic(self.tmp / "repeated")
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

    def test_the_dataset_is_version_3_ascii(self):
        self.assertEqual(self.dataset.format_version, 3)
        self.assertEqual(self.dataset.source_format, "consistent_trees_ascii")
        self.assertEqual(self.dataset.n_halos, (0, 1, 1, 4, 7, 6))

    def test_index_forest_table_matches_the_adapters_sidecar(self):
        table = self.index.forest_table()
        with h5py.File(self.paths["dataset"] / "forests.h5", "r") as handle:
            sidecar = {name: handle[name][...] for name in handle}
        np.testing.assert_array_equal(table.forest_ids, sidecar["ForestID"])
        # file_ordinal is the FileID; it equals SourceFileOrdinal here only because
        # convert_ascii gives the converter its tree files in FileID order
        np.testing.assert_array_equal(table.file_ordinal, sidecar["SourceFileOrdinal"])
        np.testing.assert_array_equal(table.unit_ordinal, sidecar["SourceUnitOrdinal"])
        # literal: 1100 unit 0 of file 0 (markers F, F, G, F), 1200 spans, 1300
        # unit 1 of file 0, 1400 unit 0 of file 1
        np.testing.assert_array_equal(table.file_ordinal, [0, -1, 0, 1])
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

    def test_top_shares_count_the_most_populous_forests(self):
        shares = occupancy.top_shares(np.array([5, 1, 3, 1]), 10, tops=(1, 2, 10))
        self.assertEqual(shares["1"], {"halos": 5, "share": 0.5})
        self.assertEqual(shares["2"], {"halos": 8, "share": 0.8})
        self.assertEqual(shares["10"], {"halos": 10, "share": 1.0})

    def test_widest_slab_is_the_lowest_numbered_on_a_tie(self):
        self.assertEqual(occupancy.widest_snapshot([3, 7, 2, 7]), 1)
        self.assertIsNone(occupancy.widest_snapshot([]))

    def test_aggregate_sizes_are_measured_and_stated(self):
        base = occupancy.occupancy_dir(self.agg)
        recorded = self.occupancy["aggregates"]
        self.assertEqual(
            recorded["slab_pairs"]["bytes"], aggregate.directory_bytes(base, "slab_*.npy")
        )
        self.assertEqual(recorded["slab_pairs"]["pairs"], 11)
        self.assertEqual(
            recorded["per_forest"]["bytes"], aggregate.directory_bytes(base, "forest_*.npy")
        )
        tree_dir = trees.trees_dir(self.agg)
        sizes = self.trees["aggregates"]
        self.assertEqual(
            sizes["labels"]["bytes"], aggregate.directory_bytes(tree_dir / trees.LABELS_DIR)
        )
        self.assertEqual(
            sizes["slab_tree_pairs"]["bytes"],
            sum(path.stat().st_size for path in tree_dir.glob("slab_*.npy")),
        )
        for record in list(recorded.values()) + list(sizes.values()):
            self.assertIn(" B ", record["formula"])

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

    def test_a_malformed_report_is_refused(self):
        report = self.tmp / "malformed_report.json"
        for document, message in (
            ({"totals": {}}, "no source_files"),
            ([1, 2], "no source_files"),
            ({"source_files": {"/a/t.dat": {"parsed_count": "13"}}}, "not a non-negative integer"),
            ({"source_files": {"/a/t.dat": {"parsed_count": -1}}}, "not a non-negative integer"),
            ({"source_files": {"/a/t.dat": {"parsed_count": True}}}, "not a non-negative integer"),
            ({"source_files": {"/a/t.dat": {}}}, "has no parsed_count"),
        ):
            report.write_text(json.dumps(document))
            with self.assertRaisesRegex(ConverterError, message):
                trees.load_parsed_counts(report)
        report.write_text("{not json")
        with self.assertRaisesRegex(ConverterError, "malformed_report.json: not readable as JSON"):
            trees.load_parsed_counts(report)

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
        # widest slab 4, weights [4, 2, 0, 1]: two tasks cut at [0, 1, 4]
        self.assertEqual(summary["weights"]["snapshot"], 4)
        self.assertEqual(summary["weights"]["heaviest_forest"], {"forest_index": 0, "rows": 4})
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
            trees.run_trees(HorizontalDataset(GENERIC_V2), self.agg)
        with self.assertRaisesRegex(ConverterError, "produced from a different dataset"):
            occupancy.run_occupancy(HorizontalDataset(GENERIC_V2), self.agg)

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
        malformed = self.tmp / "bad_report.json"
        malformed.write_text("{not json")
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
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr():
            status = forest_census.main(["trees"] + dataset + index + report)
        self.assertEqual(status, 0)
        self.assertIn("trees: 7 effective trees over 19 halos", stdout.getvalue())
        self.assertIn(
            "root correspondence: pass (1 roots outside the last snapshot)", stdout.getvalue()
        )
        self.assertIn("conservation: pass", stdout.getvalue())

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

        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr():
            self.assertEqual(forest_census.main(["occupancy"] + dataset), 0)
            status = forest_census.main(
                ["partition", "--aggregate", other, "--ntask", "1,2", "--nchunk", "1,2"]
            )
        self.assertEqual(status, 0)
        self.assertIn(
            "ntask   2 nchunk   1: widest range 4 rows (snapshot 4, task 0, chunk 0)",
            stdout.getvalue(),
        )
        recorded = aggregate.read_json(Path(other) / "partition" / "summary.json")
        self.assertEqual(
            recorded["aggregates"]["summary"]["bytes"],
            (Path(other) / "partition" / "summary.json").stat().st_size,
        )
        self.assertEqual(recorded["aggregates"]["summary"]["range_integers"], 7 * (1 + 2 + 2 + 4))

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
            (cut.run_cut, ()),
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
                expected = cut.chain_order(encounter, mass)
                got = members[np.argsort(positions[members])].tolist()
                self.assertEqual(got, expected, group)
                self.assertEqual(sorted(positions[members].tolist()), list(range(members.size)))
        self.assertEqual(cut.chain_positions([], [], [], [], np.zeros(0)).size, 0)

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
        selection,
        [1, 2, 4],
        [1, 2, 4],
        1 << 30,
        [6, 9, 16],
        1.0,
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
    version 3 (:class:`TestDecidedTableVersion2` repeats every case on version 2).

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

    convert = staticmethod(convert_ascii)
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
        cls.paths = cls.convert(cls.tmp / "source", decided_layout(), decided_forests())
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
        sage = self.cut["predicted_effects"]["sage16_halos_only"]
        self.assertEqual(
            (sage["seed_halos"], sage["dependent_halos"], sage["upper_bound_halos"]), (12, 19, 25)
        )
        hod_sham = self.cut["predicted_effects"]["hod_sham"]
        self.assertEqual(hod_sham["identity_recomputed_halos"], 25)
        self.assertEqual(len(hod_sham["mechanisms"]), 3)

    def test_the_retained_rows_and_the_bytes_read(self):
        # snapshot 3: 2201 and its central 2101; snapshot 4: the progenitors of
        # 1120, 1500, 2100 and 2400 (six) and the centrals 1101, 1510, 2202, 2302
        self.assertEqual(self.rows("retained_rows"), [0, 0, 0, 2, 10, 0])
        self.assertEqual(self.rows("rows_read"), [0, 0, 0, 4, 12, 9])
        self.check_bytes_read(self.cut["io"]["per_column"], 8)
        self.assertEqual(self.cut["io"]["bytes_read"], sum(self.cut["io"]["per_column"].values()))

    def check_bytes_read(self, read, link):
        # the split forests' rows span every slab here (forests 1200 and 1400
        # lie between them); the construction reads the final slab once more
        self.assertEqual(read["ForestIndex"], 8 * (34 + 12))
        self.assertEqual(read["FirstHaloInFOFgroup"], link * (34 + 12))
        self.assertEqual(read["Descendant"], link * 34)
        for name, width in (("MostBoundID", 8), ("M_Crit200", 4), ("NextProgenitor", link)):
            self.assertTrue(width * 12 <= read[name] <= width * (7 + 15), name)
        self.assertEqual(
            self.table["io"]["per_column"], {"FirstHaloInFOFgroup": 12 * link, "ForestIndex": 96}
        )

    # ---- the partition ----

    def points(self):
        return {(p["ntask"], p["nchunk"]): p for p in self.cut["partition"]["grid"]}

    def test_the_partition_with_the_pieces_installed(self):
        points = self.points()
        # widest slab 4, installed weights [3, 2, 1, 2, 2] for ForestIndex 0..4
        # (the kept pieces), then 1 for each of 2001..2005
        self.assertEqual(points[(1, 2)]["forest_cuts"], [0, 4, 10])
        self.assertEqual(points[(1, 2)]["widest_rows_per_snapshot"], [0, 0, 0, 4, 8, 6])
        self.assertEqual(points[(1, 1)]["widest"]["rows"], 15)
        self.assertEqual(points[(1, 4)]["forest_cuts"], [0, 2, 5, 10, 10])
        self.assertEqual(points[(1, 4)]["job_rows"], 5)
        self.assertEqual(points[(1, 4)]["retention_memory_ceiling_mb"], 5 << 10)
        partition_record = self.cut["partition"]
        self.assertEqual(
            (partition_record["bytes_per_halo"], partition_record["reserve_gib"]), (1 << 30, 1.0)
        )
        # uncut, forest 2000's five rows of snapshot 4 are the floor
        self.assertEqual(partition_record["uncut_floor"]["rows"], 5)

        def best(ntask, nchunk, rows):
            return {
                "ntask": ntask,
                "nchunk": nchunk,
                "widest_rows": rows,
                "job_rows": rows,
                "process_bytes": rows << 30,
                "job_bytes": rows << 30,
                "retention_memory_ceiling_mb": rows << 10,
            }

        self.assertEqual(
            [
                (c["class_gib"], c["usable_bytes"], c["smallest_fitting_point"])
                for c in partition_record["laptop_classes"]
            ],
            [
                (6, 5 << 30, best(1, 4, 5)),
                (9, 8 << 30, best(1, 2, 8)),
                (16, 15 << 30, best(1, 1, 15)),
            ],
        )

    def test_a_class_is_judged_on_the_job_not_one_process(self):
        # two ranks over [0, 4) and [4, 10): rank 0's widest slab holds 8 rows,
        # rank 1's 7; one process needs 8 GiB, which 9 GiB less the reserve
        # holds, but the two ranks run at once and need 15
        two_ranks = self.points()[(2, 1)]
        self.assertEqual(two_ranks["forest_cuts"], [0, 4, 10])
        self.assertEqual(two_ranks["task_widest_rows"], [8, 7])
        self.assertEqual((two_ranks["process_bytes"], two_ranks["job_bytes"]), (8 << 30, 15 << 30))
        self.assertEqual(two_ranks["fits_gib"], [16])

    def test_memory_and_the_startup_weights_are_stated(self):
        memory = self.cut["memory"]
        self.assertEqual(
            (memory["startup_weights"]["uncut_bytes"], memory["startup_weights"]["cut_bytes"]),
            (8 * 5, 8 * 10),
        )
        self.assertEqual(memory["widest_pass_slab"]["dense_row_bytes"], 12 * 15)
        for summary in (self.cut, self.table):
            for name, value in summary["memory"]["peak_rss_bytes_after"].items():
                self.assertGreater(value, 0, name)
        self.assertEqual(
            self.cut["aggregates"]["summary"]["bytes"],
            (cut.cut_dir(self.agg) / "summary.json").stat().st_size,
        )
        sizes = self.table["aggregates"]
        self.assertEqual(sizes["table"]["bytes"], Path(self.table["table"]["path"]).stat().st_size)
        self.assertEqual(
            sizes["record"]["bytes"], (table.table_dir(self.agg) / "record.json").stat().st_size
        )

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
        varying = ("dataset_dir", "io", "memory", "aggregates", "table")
        for got, want in ((summary_table, self.table), (summary_cut, self.cut)):
            got = json.loads(json.dumps(got).replace(str(other), str(self.agg)))
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
        self.assertNotEqual(summary_table["scope"], "whole simulation")
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
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr() as captured:
            self.assertEqual(forest_census.main(["occupancy"] + dataset), 0)
            self.assertEqual(forest_census.main(["trees"] + dataset + index), 0)
            self.assertEqual(forest_census.main(["partition", "--aggregate", other]), 0)
            self.assertEqual(forest_census.main(["table"] + dataset + index), 0)
            status = forest_census.main(["cut"] + dataset + grid)
        self.assertEqual(status, 0, captured.text)
        text = stdout.getvalue()
        self.assertIn("table: the z = 0 FoF groups of every forest (whole simulation)", text)
        self.assertIn(
            "z = 0 FoF groups: 10 over 5 forests; 3 forest(s) split into 8 groups; 10 forests "
            "after the cut",
            text,
        )
        self.assertIn("pieces: 8 (5 fresh); table ", text)
        self.assertIn("cut (whole simulation): 3 split forest(s), 8 piece(s) (5 fresh)", text)
        self.assertIn("6 promoted (0 at the final snapshot), 1 progenitor chain(s) changed", text)
        self.assertIn(
            "    6 GiB less 1.0 GiB: fits at ntask 1 nchunk 4 (job 5 rows, process 5", text
        )
        self.assertIn("   16 GiB less 1.0 GiB: fits at ntask 1 nchunk 1 (job 15 rows", text)
        self.assertEqual(
            (Path(other) / "table" / "forests.list").read_bytes(),
            (self.agg / "table" / "forests.list").read_bytes(),
        )
        # the reserve is a required argument
        with capture_stderr(), self.assertRaises(SystemExit):
            forest_census.main(["cut"] + dataset)

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
        self.assertIn("an id outside int64", captured.text)
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
        real = cut.decided_table

        def wrong(*args, **kwargs):
            built = real(*args, **kwargs)
            # tree 1110, a z = 0 satellite of 1100, moved into 1120's piece
            roots = trees.load_roots(other)[built.local].tolist()
            built.tree_piece[roots.index(1110)] = built.tree_piece[roots.index(1120)]
            return built

        with mock.patch.object(cut, "decided_table", wrong):
            with self.assertRaisesRegex(
                ConverterError, "1 halo\\(s\\) promoted at the final snapshot"
            ):
                cut.run_cut(self.dataset, other, reserve_gib=1.0, laptop_gib=[6])
        self.assertFalse((cut.cut_dir(other) / "summary.json").exists())


class TestDecidedTableVersion2(TestDecidedTable):
    """Every case of :class:`TestDecidedTable` on the same forests converted
    through the version 2 route: the table is the same file, and every count
    the same; only the bytes read differ (4 B links, rows not grouped by
    forest)."""

    convert = staticmethod(convert_ascii_v2)

    def test_the_dataset_is_version_2(self):
        self.assertEqual(self.dataset.format_version, 2)
        self.assertIsNone(self.dataset.source_format)

    def check_bytes_read(self, read, link):
        TestDecidedTable.check_bytes_read(self, read, 4)


class TestNoSplitForest(unittest.TestCase):
    """Forests 1200 and 1400, each ending in one z = 0 group: the decided table
    is the identity table and the cut costs nothing, on version 3 and 2."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="census_no_split_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_version_3_and_version_2(self):
        _multi, spanning, single = fixtures.correspondence_forests()
        forests = [spanning, single]
        for name, convert in (("v3", convert_ascii), ("v2", convert_ascii_v2)):
            with self.subTest(name):
                paths = convert(self.tmp / name, [fixtures.all_trees(forests)], forests)
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
        sage = self.cut["predicted_effects"]["sage16_halos_only"]
        self.assertEqual(
            (sage["seed_halos"], sage["dependent_halos"], sage["upper_bound_halos"]), (5, 12, 12)
        )
        # three tied trees of 4 halos: 4100 keeps 4000
        self.assertEqual(
            read_table(self.table["table"]["path"]), {4100: 4000, 4200: 4001, 4300: 4002}
        )


class TestEarlyEndingRefused(unittest.TestCase):
    """A census holding a tree that ends before the final snapshot is refused
    by the table and the cut, naming the count, before any output: the
    negative fixture (version 3) and the committed version 2 fixture."""

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

    def test_the_negative_fixture_is_refused(self):
        forests = fixtures.early_ending_correspondence_forests()
        paths = convert_ascii(self.tmp / "early", correspondence_layout(), forests)
        self.check_refused(
            paths["dataset"],
            (paths["forests_list"], paths["locations"]),
            "1 tree(s) end before the final snapshot 5 (e.g. roots [1300])",
        )

    def test_the_committed_version_2_fixture_is_refused(self):
        dataset = HorizontalDataset(GENERIC_V2)
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
        self.check_refused(
            GENERIC_V2,
            (forests_list, locations),
            "1 tree(s) end before the final snapshot 5 (e.g. roots [3010])",
        )


if __name__ == "__main__":
    unittest.main()
