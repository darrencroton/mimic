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
  :mod:`fixtures`' correspondence-valid forests (each ``#tree`` root id is its
  terminal halo's id), the labels also against each halo's root found by
  walking ``desc_id`` in the fixture specification;
- the index reader against hand-written index files, and against the sidecar
  ordinals the ASCII adapter itself writes;
- the identity record against the committed version 2 fixture
  (``simulations/micro-uchuu-ascii-horizontal/_tests/data/generic``) and its
  ``fixture_manifest.json``;
- the co-membership graph against pairs written out by hand from the
  severance forests' topology (:func:`severance_forests`) and against a
  whole-slab oracle over the same columns; rules and components against
  hand-built graphs and a sequential union-find; naming, invariants,
  severance, the progenitor-order change and the partition with the pieces
  installed against values derived by hand from the same topology.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
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
    graph,
    occupancy,
    partition,
    rules,
    trees,
)
from column_schema import build_schema, load_column_map  # noqa: E402
from errors import ConverterError  # noqa: E402
from hdf5_writer_v3 import HorizontalV3Writer  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
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


#: The correspondence-valid forests' file layout: file 0 holds the ``#tree``
#: markers 1100, 1110, 1300, 1120, 1200 -- forest 1100's markers repeat around
#: forest 1300's (F, F, G, F) -- and file 1 holds 1400, 1210, so forest 1200
#: spans both files.
def correspondence_layout():
    multi, spanning, early, single = fixtures.correspondence_forests()
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
    """The correspondence-valid forests (layout: :func:`correspondence_layout`).

    ``ForestIndex`` 0..3 are forests 1100, 1200, 1300 and 1400. Halos per
    snapshot: 0, 1 (1301), 1 (1300), 4 (1103, 1202, 1212, 1402), 7 (1101, 1102,
    1111, 1121, 1201, 1211, 1401) and 6 (the six roots at snapshot 5).
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_v3_"))
        forests = fixtures.correspondence_forests()
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

    def test_every_halo_is_still_labelled(self):
        roots = trees.load_roots(self.tmp / "agg")
        self.assertEqual(roots.size, 10)
        labelled = sum(trees.load_labels(self.tmp / "agg", s).size for s in self.dataset.snapshots)
        self.assertEqual(labelled, self.dataset.total_halos)
        self.assertEqual(self.summary["forest_mismatch_halos"], 0)


# ---------------------------------------------------------------------------
# The co-membership graph, rules, cut tables and the cut
# ---------------------------------------------------------------------------


def five_halo_forest():
    """Forest 1500, five halos in two trees: 1530 (tree 1500) is a satellite of
    1510 (tree 1600) at snapshot 4 and shares descendant 1500 with 1520, of
    equal mass. Before a cut the encounter keys are (1510, 1510, 1530) and
    (1520, -1, 1520), so the chain is [1530, 1520]; severed, 1530 becomes
    (1530, -1, 1530) and the chain [1520, 1530]."""
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
    """Forest 2000, four trees. Trees 2100 and 2200 share FoF groups in both
    directions: 2201 is 2101's satellite at snapshot 3, and 2102 and 2103 are
    2202's at snapshot 4 (two members, one snapshot). 2401 (tree 2400) is
    2302's (tree 2300) satellite at snapshot 4, with the larger mass."""
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


def severance_forests():
    """Forests 1500 (``ForestIndex`` 0) and 2000 (1). Root ordinals: 1500 -> 0,
    1600 -> 1, 2100 -> 2, 2200 -> 3, 2300 -> 4, 2400 -> 5. Tree totals 3, 2,
    4, 3, 3, 2; halos per snapshot 0, 0, 0, 3, 8, 6."""
    return [five_halo_forest(), switching_forest()]


def f32(value):
    """A fixture mass as the dataset stores it, widened."""
    return float(np.float32(value))


def pair_array(rows):
    """Merged pair records from ``(lo, hi, snapshots, halos, mass)`` tuples."""
    pairs = np.zeros(len(rows), dtype=graph.PAIR_DTYPE)
    for at, (lo, hi, snapshots, halos, mass) in enumerate(rows):
        pairs[at] = (lo, hi, snapshots, 0, 0, halos, mass)
    return pairs


def sequential_components(n_nodes, edges):
    """A plain sequential union-find, each component named by its smallest node."""
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


def oracle_slab_pairs(dataset_dir, snap, labels, tree_forest, forests):
    """A slab's pairs from whole columns: {(lo, hi): (members, mass)}."""
    with h5py.File(Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap), "r") as handle:
        forest = handle["halos"]["ForestIndex"][...]
        central = handle["halos"]["FirstHaloInFOFgroup"][...]
        mass = handle["halos"]["M_Crit200"][...]
    pairs = {}
    for row in range(forest.size):
        if forest[row] not in forests or central[row] == row:
            continue
        own, host = int(labels[row]), int(labels[central[row]])
        if own == host:
            continue
        key = (min(own, host), max(own, host))
        members, total = pairs.get(key, (0, 0.0))
        pairs[key] = (members + 1, total + float(mass[row]))
    return pairs


class TestRules(unittest.TestCase):
    #: Local trees 0..5: (0, 1) long-lived, (1, 2) many halos, (3, 4) massive, (4, 5) all.
    PAIRS = [(0, 1, 3, 1, 1.0), (1, 2, 1, 5, 1.0), (3, 4, 1, 1, 100.0), (4, 5, 2, 2, 2.0)]

    def components(self, rule, batch_rows=1 << 20):
        trees_local = np.arange(6, dtype=np.int32)
        return graph.rule_components(pair_array(self.PAIRS), trees_local, rule, batch_rows).tolist()

    def test_each_threshold_kind(self):
        cases = {
            "d=any,h=any,m=any": [0, 0, 0, 3, 3, 3],
            "d=2": [0, 0, 2, 3, 4, 4],
            "h=2": [0, 1, 1, 3, 4, 4],
            "m=50": [0, 1, 2, 3, 3, 5],
            "d=2,h=2": [0, 1, 2, 3, 4, 4],
            "d=4": [0, 1, 2, 3, 4, 5],
        }
        for spec, expected in cases.items():
            for batch_rows in (1, 3, 1 << 20):
                self.assertEqual(
                    self.components(rules.parse_rule(spec), batch_rows),
                    expected,
                    (spec, batch_rows),
                )

    def test_a_rule_at_its_minimum_keeps_every_edge(self):
        complete = self.components(rules.COMPLETE)
        for spec in ("d=0,h=0", "d=1,h=1,m=any", "m=1"):
            self.assertEqual(self.components(rules.parse_rule(spec)), complete, spec)
        self.assertTrue(rules.COMPLETE.keeps_every_edge)
        self.assertEqual(rules.component_count(np.array(complete)), 2)

    def test_names_and_parsing(self):
        self.assertEqual(rules.COMPLETE.name, "d=any,h=any,m=any")
        self.assertEqual(rules.parse_rule("m=1e12, d=5").name, "d=5,h=any,m=1000000000000.0")
        self.assertEqual(rules.parse_rule("d=5,h=2,m=any"), rules.Rule(5, 2, None))
        self.assertEqual(
            rules.parse_rule("h=3").record(),
            {"name": "d=any,h=3,m=any", "min_snapshots": None, "min_halos": 3, "min_mass": None},
        )
        for text, message in (
            ("", "expected d="),
            ("x=1", "is not one of"),
            ("d=1,d=2", "d given twice"),
            ("d=-1", "not a non-negative integer"),
            ("h=1.5", "not a non-negative integer"),
            ("m=nan", "not a finite number"),
            ("d", "is not one of"),
        ):
            with self.assertRaisesRegex(ConverterError, message):
                rules.parse_rule(text)

    def test_names_are_lossless(self):
        self.assertNotEqual(rules.parse_rule("m=1234567").name, rules.parse_rule("m=1234568").name)
        generator = np.random.default_rng(8)
        masses = np.concatenate(
            [generator.random(200) * 10.0 ** generator.integers(-5, 16, 200), [0.1, 1e300, -2.5]]
        )
        for mass in masses.tolist():
            rule = rules.Rule(3, None, mass)
            self.assertEqual(rules.parse_rule(rule.name), rule, rule.name)
        for rule in (rules.COMPLETE, rules.Rule(0, 7, None), rules.Rule(None, None, 0.0)):
            self.assertEqual(rules.parse_rule(rule.name), rule)

    def test_union_find_agrees_with_a_sequential_one(self):
        generator = np.random.default_rng(20261008)
        for n_nodes, n_edges in ((1, 0), (7, 3), (50, 40), (300, 280), (300, 900)):
            edges = generator.integers(0, n_nodes, size=(n_edges, 2))
            # a long path through a shuffled order, so hooks chain deeply
            path = generator.permutation(n_nodes)
            edges = np.concatenate([edges, np.stack([path[:-1], path[1:]], axis=1)[: n_nodes // 3]])
            expected = sequential_components(n_nodes, edges.tolist())
            for batch in (1, 7, 10_000):

                def batches(edges=edges, batch=batch):
                    for start in range(0, len(edges), batch):
                        yield edges[start : start + batch, 0], edges[start : start + batch, 1]

                self.assertEqual(rules.union_find(n_nodes, batches).tolist(), expected)
        with self.assertRaisesRegex(ConverterError, "outside \\[0, 2\\)"):
            rules.union_find(2, lambda: iter([(np.array([0]), np.array([2]))]))


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
        header = {"rule": {"name": "d=2,h=any,m=any"}, "table": {"md5": "x"}}
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
        with self.assertRaisesRegex(ConverterError, "joins trees of two forests"):
            cut_table.name_pieces(
                np.array([0, 0]),
                np.array([0, 1]),
                np.array([1, 1]),
                np.array([1, 2]),
                np.array([7, 9]),
                9,
            )


class TestGraphAndCut(unittest.TestCase):
    """The severance forests (:func:`severance_forests`) converted to version 3
    in one file; the graph and the cut over both forests."""

    BOTH = ["--forest-index", "0", "--forest-index", "1"]

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_cut_"))
        forests = severance_forests()
        cls.paths = convert_ascii(cls.tmp / "source", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.index = SourceIndex.load(cls.paths["forests_list"], cls.paths["locations"])
        cls.agg = cls.tmp / "agg"
        cls.prepare(cls.agg)
        cls.graph = run_quietly(graph.run_graph, cls.dataset, cls.agg, [0, 1])
        cls.rules = [rules.parse_rule(text) for text in ("d=2", "m=1e12", "d=any")]
        cls.cut = run_quietly(
            cut.run_cut,
            cls.dataset,
            cls.agg,
            cls.index.tree_roots,
            cls.index.forest_ids,
            cls.index_files(),
            cls.rules,
            [cls.rules[0]],
            [1, 2, 4],
            [1, 2, 4],
            1 << 30,
            [4, 16],
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def prepare(cls, agg, block_rows=1 << 22):
        run_quietly(occupancy.run_occupancy, cls.dataset, agg, block_rows)
        run_quietly(
            trees.run_trees,
            cls.dataset,
            agg,
            cls.index,
            trees.load_parsed_counts(cls.paths["preparation_manifest"]),
            block_rows,
        )

    @classmethod
    def index_arrays(cls):
        return cls.index.tree_roots, cls.index.forest_ids

    @classmethod
    def index_files(cls):
        return {
            name: {"path": str(path), "md5": cut_table.md5_file(path)}
            for name, path in (
                ("forests_list", cls.paths["forests_list"]),
                ("locations", cls.paths["locations"]),
            )
        }

    def rule_summary(self, name, summary=None):
        summary = self.cut if summary is None else summary
        return next(r for r in summary["rules"] if r["rule"]["name"] == name)

    # ---- the graph ----

    def test_per_slab_pairs(self):
        expected = {
            3: [(2, 3, 1, f32(1e11))],
            4: [(0, 1, 1, f32(5e11)), (2, 3, 2, f32(6e11) + f32(2e11)), (4, 5, 1, f32(1.5e12))],
        }
        for snap in self.dataset.snapshots:
            edges = graph.load_slab_edges(self.agg, snap)
            self.assertEqual(edges.dtype, graph.EDGE_DTYPE)
            self.assertEqual([tuple(e) for e in edges.tolist()], expected.get(snap, []), snap)

    def test_a_switching_host_counts_once_per_snapshot(self):
        pairs = graph.load_pairs(self.agg)
        self.assertEqual(pairs.dtype, graph.PAIR_DTYPE)
        self.assertEqual(
            [tuple(p) for p in pairs.tolist()],
            [
                (0, 1, 1, 4, 4, 1, f32(5e11)),
                (2, 3, 2, 3, 4, 3, f32(1e11) + (f32(6e11) + f32(2e11))),
                (4, 5, 1, 4, 4, 1, f32(1.5e12)),
            ],
        )

    def test_the_slab_pairs_agree_with_a_whole_slab_oracle(self):
        tree_forest = np.load(trees.trees_dir(self.agg) / "tree_forest.npy")
        for snap in self.dataset.snapshots:
            labels = trees.load_labels(self.agg, snap)
            expected = oracle_slab_pairs(self.paths["dataset"], snap, labels, tree_forest, (0, 1))
            got = {
                (int(e["lo"]), int(e["hi"])): (int(e["halos"]), float(e["mass"]))
                for e in graph.load_slab_edges(self.agg, snap)
            }
            self.assertEqual(got, expected, snap)

    def test_the_complete_graph_components_are_a_result(self):
        np.testing.assert_array_equal(graph.load_forest_trees(self.agg), [0, 1, 2, 3, 4, 5])
        np.testing.assert_array_equal(graph.load_components(self.agg), [0, 0, 2, 2, 4, 4])
        record = self.graph["complete_graph"]
        self.assertEqual(record["components"], 3)
        one, two = record["per_forest"]
        self.assertEqual((one["forest_id"], one["components"]), (1500, 1))
        self.assertFalse(one["separable_without_severing"])
        self.assertEqual((two["forest_id"], two["components"]), (2000, 2))
        self.assertTrue(two["separable_without_severing"])
        self.assertEqual(
            two["largest"],
            [
                {"smallest_root_id": 2100, "trees": 2, "halos": 7},
                {"smallest_root_id": 2300, "trees": 2, "halos": 5},
            ],
        )
        self.assertEqual(self.graph["pairs"], {"count": 3, "max_snapshots": 2, "halos": 5})

    def test_the_graph_defaults_to_the_largest_forest(self):
        other = self.tmp / "agg_default"
        self.prepare(other)
        summary = run_quietly(graph.run_graph, self.dataset, other)
        self.assertEqual(summary["forests"], [1])
        self.assertEqual(graph.load_pairs(other)[["lo", "hi"]].tolist(), [(2, 3), (4, 5)])
        np.testing.assert_array_equal(graph.load_forest_trees(other), [2, 3, 4, 5])
        with self.assertRaisesRegex(ConverterError, "ForestIndex \\[2\\] outside \\[0, 2\\)"):
            graph.run_graph(self.dataset, other, [2])

    def test_aggregate_sizes_are_stated(self):
        sizes = self.graph["aggregates"]
        base = graph.graph_dir(self.agg)
        self.assertEqual(sizes["slab_edges"]["pairs"], 4)
        self.assertEqual(
            sizes["slab_edges"]["bytes"], aggregate.directory_bytes(base, "slab_*_edges.npy")
        )
        self.assertEqual(sizes["pairs"]["bytes"], (base / "pairs.npy").stat().st_size)
        self.assertEqual(sizes["pairs"]["bytes"], 256 + 36 * 3)
        self.assertEqual(sizes["slab_edges"]["bytes"], 6 * 192 + 20 * 4)
        cut_sizes = self.cut["aggregates"]
        self.assertEqual(
            cut_sizes["assignments"]["bytes"],
            aggregate.directory_bytes(cut.cut_dir(self.agg), "assignment.npy"),
        )
        for record in list(sizes.values()) + list(cut_sizes.values()):
            self.assertIn(" B ", record["formula"])

    # ---- the cut ----

    def test_pieces_are_named_under_f6(self):
        # d=2 keeps only (2100, 2200): pieces A (3) keeps 1500, {T1, T2} (7)
        # keeps 2000; fresh by size then root: T3 (3) 2001, B (2, root 1600)
        # 2002, T4 (2, root 2400) 2003
        np.testing.assert_array_equal(
            cut.load_assignment(self.agg, self.rules[0]), [1500, 2002, 2000, 2000, 2001, 2003]
        )
        # m=1e12 keeps only (2300, 2400), whose piece (5 halos) is forest 2000's
        # largest and keeps 2000; fresh: T1 (4) 2001, T2 (3) 2002, B (2) 2003
        np.testing.assert_array_equal(
            cut.load_assignment(self.agg, self.rules[1]), [1500, 2003, 2001, 2002, 2000, 2000]
        )
        # the complete rule: forest 1500 whole, forest 2000 split along its components
        np.testing.assert_array_equal(
            cut.load_assignment(self.agg, self.rules[2]), [1500, 1500, 2000, 2000, 2001, 2001]
        )
        pieces = self.rule_summary("d=2,h=any,m=any")["pieces"]
        self.assertEqual((pieces["count"], pieces["fresh"]), (5, 3))
        five, switching = pieces["per_forest"]
        self.assertEqual(
            five["kept_piece"],
            {
                "id": 1500,
                "forest_id": 1500,
                "trees": 1,
                "halos": 3,
                "smallest_root_id": 1500,
                "peak_occupancy": 2,
                "peak_snapshot": 4,
            },
        )
        self.assertEqual(
            [(p["id"], p["halos"], p["peak_occupancy"]) for p in switching["largest_fresh_pieces"]],
            [(2001, 3, 1), (2003, 2, 1)],
        )

    def test_severance_and_the_progenitor_order_change(self):
        entry = self.rule_summary("d=2,h=any,m=any")
        severance = entry["severance"]
        self.assertEqual(severance["promoted_halos"], 2)  # 1530 and 2401
        self.assertEqual(severance["groups_losing_members"], 2)  # 1510's and 2302's
        self.assertEqual(severance["groups_central_leaves_members_stay"], 2)
        rows = {row["snapshot"]: row for row in severance["per_snapshot"]}
        self.assertEqual(rows[4]["promoted_halos"], 2)
        self.assertEqual(rows[3]["promoted_halos"], 0)  # (2100, 2200) is kept
        # descendant 1500's chain [1530, 1520] becomes [1520, 1530]
        self.assertEqual(rows[4]["progenitor_order_changed"], 1)
        self.assertEqual(rows[4]["first_progenitor_changed"], 1)
        self.assertEqual(entry["progenitor_order"]["descendants_changed"], 1)
        edges = entry["edges"]
        self.assertEqual(edges["kept"]["pairs"], 1)
        self.assertEqual((edges["dropped"]["pairs"], edges["dropped"]["halos"]), (2, 2))
        self.assertEqual(edges["severed"]["pairs"], 2)
        # checked: every affected descendant with two or more progenitors, over
        # all rules (1500 for d=2; 1500 and 2100 for m=1e12)
        self.assertEqual(self.cut["stored_chain_check"]["descendants"], 2)
        self.assertEqual(self.cut["stored_chain_check"]["mismatches"], 0)

    def test_relabelled_halos_and_predicted_effects(self):
        entry = self.rule_summary("d=2,h=any,m=any")
        self.assertEqual(
            entry["relabelled"],
            {
                "forest_id_changed_halos": 7,
                "rank_recomputed_halos": 17,
                "source_halo_id_shifted_halos": 17,
            },
        )
        sage = entry["predicted_effects"]["sage16_halos_only"]
        # seeds: 1530, 2401, 1510, 2302 at snapshot 4, and 1500 (its chain) at 5;
        # dependents add 1600, 2300, 2400 at 5
        self.assertEqual(sage["seed_halos"], 5)
        self.assertEqual(sage["dependent_halos"], 8)
        self.assertEqual(sage["upper_bound_halos"], 10)  # pieces A, B, T3, T4
        self.assertEqual(entry["predicted_effects"]["hod_sham"]["halos"], 17)
        rows = {row["snapshot"]: row for row in entry["severance"]["per_snapshot"]}
        self.assertEqual((rows[4]["seed_halos"], rows[4]["affected_halos"]), (4, 4))
        self.assertEqual((rows[5]["seed_halos"], rows[5]["affected_halos"]), (1, 4))

    def test_the_complete_rule_severs_nothing(self):
        entry = self.rule_summary("d=any,h=any,m=any")
        self.assertEqual(entry["edges"]["dropped"]["pairs"], 0)
        self.assertEqual(entry["severance"]["promoted_halos"], 0)
        self.assertEqual(entry["predicted_effects"]["sage16_halos_only"]["dependent_halos"], 0)
        # its components are the complete graph's
        self.assertEqual(entry["components"], self.graph["complete_graph"]["components"])

    def test_the_partition_with_the_pieces_installed(self):
        entry = self.rule_summary("d=2,h=any,m=any")
        points = {(p["ntask"], p["nchunk"]): p for p in entry["partition"]["grid"]}
        # widest slab 4, installed weights [2, 3, 1, 1, 1] (A, {T1, T2}, then
        # 2001, 2002, 2003)
        self.assertEqual(points[(1, 2)]["forest_cuts"], [0, 2, 5])
        self.assertEqual(points[(1, 2)]["widest_rows_per_snapshot"], [0, 0, 0, 2, 5, 3])
        self.assertEqual(points[(1, 1)]["widest"]["rows"], 8)
        self.assertEqual(points[(1, 4)]["widest"]["rows"], 3)
        self.assertEqual(points[(1, 4)]["implied_bytes"], 3 << 30)
        self.assertEqual(points[(1, 4)]["fits_gib"], [4, 16])
        self.assertEqual(points[(1, 2)]["fits_gib"], [16])
        classes = entry["partition"]["laptop_classes"]
        self.assertEqual(
            [(c["class_gib"], c["smallest_fitting_point"]) for c in classes],
            [
                (4, {"ntask": 1, "nchunk": 4, "widest_rows": 3, "implied_bytes": 3 << 30}),
                (16, {"ntask": 1, "nchunk": 1, "widest_rows": 8, "implied_bytes": 8 << 30}),
            ],
        )

    def test_the_materialised_table_and_its_record(self):
        entry = self.rule_summary("d=2,h=any,m=any")
        table = entry["table"]
        path = Path(table["path"])
        self.assertEqual(table["md5"], cut_table.md5_file(path))
        self.assertEqual(table["rows"], 6)
        for other in self.rules[1:]:
            self.assertIsNone(self.rule_summary(other.name)["table"])
            self.assertFalse((cut.rule_dir(self.agg, other) / "forests.list").exists())
        # round trip through source_index.py
        cut_index = SourceIndex.load(path, self.paths["locations"])
        np.testing.assert_array_equal(cut_index.tree_roots, self.index.tree_roots)
        np.testing.assert_array_equal(cut_index.forest_ids, [1500, 2002, 2000, 2000, 2001, 2003])
        self.assertEqual(
            cut_index.forest_table().forest_ids.tolist(), [1500, 2000, 2001, 2002, 2003]
        )
        totals = np.load(trees.trees_dir(self.agg) / "tree_totals.npy")
        cut_table.read_cut_table(path, self.index.tree_roots, self.index.forest_ids, totals)
        record = aggregate.read_json(cut.rule_dir(self.agg, self.rules[0]) / "record.json")
        self.assertEqual(record["dataset"], self.dataset.identity())
        self.assertEqual(record["rule"], self.rules[0].record())
        self.assertEqual(record["table"], table)
        self.assertEqual(record["forests"]["forest_id"], [1500, 2000])
        self.assertEqual(
            record["index_files"]["forests_list"]["md5"],
            cut_table.md5_file(self.paths["forests_list"]),
        )
        self.assertEqual(
            record["pieces"],
            {
                "id": [1500, 2000, 2001, 2002, 2003],
                "forest_id": [1500, 2000, 2000, 1500, 2000],
                "trees": [1, 2, 1, 1, 1],
                "halos": [3, 7, 3, 2, 2],
                "peak_occupancy": [2, 3, 1, 1, 1],
                "peak_snapshot": [4, 4, 3, 4, 4],
            },
        )

    def test_a_forest_with_one_component_yields_the_identity_table(self):
        other = self.tmp / "agg_identity"
        self.prepare(other)
        run_quietly(graph.run_graph, self.dataset, other, [0])
        complete = rules.parse_rule("d=any")
        summary = run_quietly(
            cut.run_cut, self.dataset, other, *self.index_arrays(), {}, [complete], [complete]
        )
        table = Path(self.rule_summary(complete.name, summary)["table"]["path"])
        roots, ids = cut_table.read_cut_table(
            table, *self.index_arrays(), np.load(trees.trees_dir(other) / "tree_totals.npy")
        )
        np.testing.assert_array_equal(ids, self.index.forest_ids)
        self.assertEqual(self.rule_summary(complete.name, summary)["pieces"]["fresh"], 0)

    def test_results_do_not_depend_on_the_block_size(self):
        other = self.tmp / "agg_block_1"
        self.prepare(other, block_rows=1)
        run_quietly(graph.run_graph, self.dataset, other, [0, 1], 1)
        summary = run_quietly(
            cut.run_cut,
            self.dataset,
            other,
            *self.index_arrays(),
            self.index_files(),
            self.rules,
            [self.rules[0]],
            [1, 2, 4],
            [1, 2, 4],
            1 << 30,
            [4, 16],
            1,
        )
        for name in ("graph", "cut"):
            for path in sorted((self.agg / name).rglob("*.npy")) + sorted(
                (self.agg / name).rglob("forests.list")
            ):
                twin = other / path.relative_to(self.agg)
                self.assertEqual(path.read_bytes(), twin.read_bytes(), path)
        strip = json.loads(json.dumps(summary["rules"]).replace(str(other), str(self.agg)))
        self.assertEqual(strip, self.cut["rules"])

    def test_the_graph_and_cut_command_lines(self):
        other = str(self.tmp / "agg_cli")
        dataset = ["--dataset", str(self.paths["dataset"]), "--aggregate", other]
        index = ["--forests-list", str(self.paths["forests_list"])]
        index += ["--locations", str(self.paths["locations"])]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr() as captured:
            self.assertEqual(forest_census.main(["occupancy"] + dataset), 0)
            self.assertEqual(forest_census.main(["trees"] + dataset + index), 0)
            self.assertEqual(forest_census.main(["graph"] + dataset + self.BOTH), 0)
            status = forest_census.main(
                ["cut"]
                + dataset
                + index
                + ["--rule", "d=2", "--rule", "h=2"]
                + ["--materialise", "d=2,h=any", "--ntask", "1", "--nchunk", "1,4"]
            )
        self.assertEqual(status, 0, captured.text)
        text = stdout.getvalue()
        self.assertIn("graph: forests [0, 1], 6 trees, 3 distinct pairs", text)
        self.assertIn(
            "complete graph: ForestIndex 1 (ForestID 2000) has 2 component(s), separable "
            "without severing",
            text,
        )
        self.assertIn(
            "rule d=2,h=any,m=any: 5 piece(s) (3 fresh), 2 promoted, 1 progenitor chain(s) changed",
            text,
        )
        self.assertIn("   16 GiB: fits at ntask 1 nchunk 1 (widest 8 rows)", text)
        self.assertIn(
            "md5 computed once",
            aggregate.read_json(Path(other) / "cut" / "summary.json")["index_files"]["note"],
        )
        self.assertTrue(
            (Path(other) / "cut" / "rules" / "d=2,h=any,m=any" / "forests.list").is_file()
        )
        self.assertFalse(
            (Path(other) / "cut" / "rules" / "d=any,h=2,m=any" / "forests.list").exists()
        )

    def test_cut_refusals_come_before_any_work(self):
        other = self.tmp / "agg_refusals"
        self.prepare(other)
        dataset = ["--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        index = ["--forests-list", str(self.paths["forests_list"])]
        index += ["--locations", str(self.paths["locations"])]
        for extra, message in (
            (["--rule", "q=2"], "is not one of"),
            (["--rule", "d=2", "--rule", "d=2,m=any"], "given more than once"),
            (["--rule", "d=2", "--materialise", "d=3"], "not among the rules"),
            (["--rule", "d=2"], "run graph first"),
        ):
            with capture_stderr() as captured:
                status = forest_census.main(["cut"] + dataset + index + extra)
            self.assertEqual(status, 2, extra)
            self.assertIn(message, captured.text)
            self.assertNotIn("Traceback", captured.text)
            self.assertFalse((other / "cut").exists(), extra)
        with self.assertRaisesRegex(ConverterError, "run trees first"):
            graph.run_graph(self.dataset, self.tmp / "agg_empty")

    def test_bytes_read_are_measured(self):
        # every row belongs to the two named forests: version 3 links are int64
        halos = self.dataset.total_halos
        self.assertEqual(
            self.graph["io"]["per_column"],
            {"FirstHaloInFOFgroup": 8 * halos, "ForestIndex": 8 * halos, "M_Crit200": 4 * halos},
        )
        read = self.cut["io"]["per_column"]
        for name in ("ForestIndex", "FirstHaloInFOFgroup", "Descendant"):
            self.assertEqual(read[name], 8 * halos, name)
        # retained rows lie only in the slabs with a severed edge, 3 and 4 (3 + 8 rows)
        for name, width in (("MostBoundID", 8), ("M_Crit200", 4), ("NextProgenitor", 8)):
            self.assertTrue(0 < read[name] <= width * (3 + 8), name)
        self.assertEqual(self.cut["io"]["bytes_read"], sum(read.values()))

    def test_index_files_reassigning_a_root_are_refused(self):
        other = self.tmp / "agg_reassigned"
        self.prepare(other)
        run_quietly(graph.run_graph, self.dataset, other, [0])
        # root 2400 lies outside the selected forest 1500; move it to forest 1500
        replaced = self.tmp / "reassigned.list"
        replaced.write_text(
            self.paths["forests_list"].read_text().replace("2400 2000", "2400 1500")
        )
        arguments = ["cut", "--dataset", str(self.paths["dataset"]), "--aggregate", str(other)]
        arguments += ["--forests-list", str(replaced), "--locations", str(self.paths["locations"])]
        with capture_stderr() as captured:
            status = forest_census.main(arguments + ["--rule", "d=2", "--materialise", "d=2"])
        self.assertEqual(status, 2)
        self.assertIn("give 1 tree root(s) a forest other than the dataset's", captured.text)
        self.assertIn("[2400]", captured.text)
        self.assertFalse(cut.cut_dir(other).exists())

    def test_outputs_of_an_earlier_run_are_refused(self):
        other = self.tmp / "agg_stale_rules"
        self.prepare(other)
        run_quietly(graph.run_graph, self.dataset, other, [0, 1])
        d2, m = self.rules[0], self.rules[1]
        run_quietly(cut.run_cut, self.dataset, other, *self.index_arrays(), {}, [d2], [d2])
        record = aggregate.read_json(cut.rule_dir(other, d2) / "record.json")
        self.assertEqual(
            record["forests"],
            {"forest_index": [0, 1], "forest_id": [1500, 2000], "graph_forests": [0, 1]},
        )
        before = sorted(path.name for path in cut.cut_dir(other).rglob("*"))
        for rules_now, materialise, stale in (
            ([m], [], "d=2,h=any,m=any"),
            ([d2], [], "d=2,h=any,m=any/forests.list"),
        ):
            with self.assertRaisesRegex(ConverterError, "would not rewrite.*" + stale):
                cut.run_cut(self.dataset, other, *self.index_arrays(), {}, rules_now, materialise)
        self.assertEqual(sorted(path.name for path in cut.cut_dir(other).rglob("*")), before)
        # the same run again rewrites exactly its own outputs
        run_quietly(cut.run_cut, self.dataset, other, *self.index_arrays(), {}, [d2], [d2])

    def test_a_stale_edge_list_is_refused(self):
        other = self.tmp / "agg_stale"
        self.prepare(other)
        run_quietly(graph.run_graph, self.dataset, other, [0, 1])
        edges = graph.load_slab_edges(other, 4, mmap=False)
        aggregate.save_array(graph.edge_path(other, 4), edges[1:])
        with self.assertRaisesRegex(ConverterError, "edge list predicts 1; the graph aggregates"):
            cut.run_cut(self.dataset, other, *self.index_arrays(), {}, [self.rules[0]])


def splitting_forest():
    """Forest 3000: at snapshot 4, central 3101 (tree 3100) hosts 3201 and 3202
    (tree 3200) and 3301 (tree 3300); each tree's root is a central at 5."""
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


class TestGroupSplitting(unittest.TestCase):
    """One group losing members to two pieces: the group, remnant and
    several-member remnant counts differ."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_split_"))
        forests = [splitting_forest()]
        cls.paths = convert_ascii(cls.tmp / "source", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        index = SourceIndex.load(cls.paths["forests_list"], cls.paths["locations"])
        cls.agg = cls.tmp / "agg"
        run_quietly(occupancy.run_occupancy, cls.dataset, cls.agg)
        run_quietly(trees.run_trees, cls.dataset, cls.agg, index)
        run_quietly(graph.run_graph, cls.dataset, cls.agg)
        cls.summary = run_quietly(
            cut.run_cut,
            cls.dataset,
            cls.agg,
            index.tree_roots,
            index.forest_ids,
            {},
            [rules.parse_rule("d=2"), rules.parse_rule("h=2")],
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def severance(self, name):
        entry = next(r for r in self.summary["rules"] if r["rule"]["name"] == name)
        return entry["severance"]

    def test_one_group_two_remnants_one_with_several_members(self):
        # d=2 drops (3100, 3200) and (3100, 3300): three pieces
        severance = self.severance("d=2,h=any,m=any")
        self.assertEqual(severance["promoted_halos"], 3)
        self.assertEqual(severance["groups_losing_members"], 1)
        self.assertEqual(severance["groups_central_leaves_members_stay"], 2)
        self.assertEqual(severance["remnants_with_several_members"], 1)
        for name in (
            "groups_losing_members",
            "groups_central_leaves_members_stay",
            "remnants_with_several_members",
        ):
            self.assertIn(name, severance["definitions"])

    def test_a_split_keeping_one_tree(self):
        # h=2 keeps (3100, 3200) (two members) and drops (3100, 3300)
        severance = self.severance("d=any,h=2,m=any")
        self.assertEqual(severance["promoted_halos"], 1)
        self.assertEqual(severance["groups_losing_members"], 1)
        self.assertEqual(severance["groups_central_leaves_members_stay"], 1)
        self.assertEqual(severance["remnants_with_several_members"], 0)


class TestVersion2EndToEnd(unittest.TestCase):
    """occupancy, trees, graph and cut on the committed version 2 fixture, with
    index files written from its census roots, and a table read back."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="census_v2_cut_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def write_index(self, dataset):
        probe = self.tmp / "probe"
        run_quietly(trees.run_trees, dataset, probe)
        roots = trees.load_roots(probe)
        tree_forest = np.load(trees.trees_dir(probe) / "tree_forest.npy")
        forest_ids = dataset.forest_ids()[tree_forest]
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

    def test_the_census_runs_end_to_end(self):
        dataset = HorizontalDataset(GENERIC_V2)
        forests_list, locations = self.write_index(dataset)
        agg = str(self.tmp / "agg")
        base = ["--dataset", str(GENERIC_V2), "--aggregate", agg]
        index = ["--forests-list", str(forests_list), "--locations", str(locations)]
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), capture_stderr() as captured:
            for arguments in (
                ["occupancy"] + base,
                ["trees"] + base + index,
                ["graph"] + base,
                ["cut"]
                + base
                + index
                + ["--rule", "d=2", "--rule", "d=any"]
                + ["--materialise", "d=2", "--materialise", "d=any"],
            ):
                self.assertEqual(forest_census.main(arguments), 0, captured.text)
        self.assertIn("root correspondence: pass", stdout.getvalue())
        summary = aggregate.read_json(Path(agg) / "cut" / "summary.json")
        self.assertEqual(summary["stored_chain_check"]["mismatches"], 0)
        roots = trees.load_roots(agg)
        totals = np.load(trees.trees_dir(agg) / "tree_totals.npy")
        sidecar = dataset.forest_ids()[np.load(trees.trees_dir(agg) / "tree_forest.npy")]
        for entry in summary["rules"]:
            table = Path(entry["table"]["path"])
            self.assertEqual(entry["table"]["md5"], cut_table.md5_file(table))
            got_roots, ids = cut_table.read_cut_table(table, roots, sidecar, totals)
            np.testing.assert_array_equal(got_roots, roots)
            np.testing.assert_array_equal(
                ids[np.load(graph.graph_dir(agg) / "forest_trees.npy")],
                cut.load_assignment(agg, rules.parse_rule(entry["rule"]["name"])),
            )


class TestCutWithoutCorrespondence(unittest.TestCase):
    """The adapter's standard forests fail the root correspondence: the cut
    explores but refuses to write a table."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="census_cut_mismatch_"))
        forests = fixtures.standard_forests()
        cls.paths = convert_ascii(cls.tmp / "standard", [fixtures.all_trees(forests)], forests)
        cls.dataset = HorizontalDataset(cls.paths["dataset"])
        cls.index = SourceIndex.load(cls.paths["forests_list"], cls.paths["locations"])
        cls.agg = cls.tmp / "agg"
        run_quietly(occupancy.run_occupancy, cls.dataset, cls.agg)
        run_quietly(trees.run_trees, cls.dataset, cls.agg, cls.index)
        run_quietly(graph.run_graph, cls.dataset, cls.agg)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_index_files_that_do_not_describe_the_census_are_refused(self):
        rule = rules.parse_rule("d=2")
        index = (self.index.tree_roots, self.index.forest_ids)
        with self.assertRaisesRegex(ConverterError, "verdict is 'fail'; a cut table is written"):
            cut.run_cut(self.dataset, self.agg, *index, {}, [rule], [rule])
        # exploring needs the index files to describe the census as well
        with self.assertRaisesRegex(ConverterError, "do not list this census's tree roots"):
            cut.run_cut(self.dataset, self.agg, *index, {}, [rule])
        self.assertFalse(cut.cut_dir(self.agg).exists())

    def test_the_graph_runs_on_version_2(self):
        tmp = self.tmp / "v2"
        dataset = HorizontalDataset(GENERIC_V2)
        run_quietly(trees.run_trees, dataset, tmp)
        summary = run_quietly(graph.run_graph, dataset, tmp)
        tree_forest = np.load(trees.trees_dir(tmp) / "tree_forest.npy")
        forests = tuple(summary["forests"])
        for snap in dataset.snapshots:
            expected = oracle_slab_pairs(
                GENERIC_V2, snap, trees.load_labels(tmp, snap), tree_forest, forests
            )
            got = {
                (int(e["lo"]), int(e["hi"])): (int(e["halos"]), float(e["mass"]))
                for e in graph.load_slab_edges(tmp, snap)
            }
            self.assertEqual(got, expected, snap)


if __name__ == "__main__":
    unittest.main()
