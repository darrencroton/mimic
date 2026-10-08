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
  ``fixture_manifest.json``.
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

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
import forest_census  # noqa: E402
import pipeline  # noqa: E402
from census import aggregate, occupancy, partition, trees  # noqa: E402
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
            trees.run_trees, cls.dataset, cls.agg, cls.index, cls.paths["preparation_manifest"]
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
        record = trees.conservation(roots, totals, self.index, report)
        self.assertEqual(record["verdict"], "fail")
        self.assertEqual(record["files_disagreeing"], 1)
        report.write_text(json.dumps({"totals": {}}))
        with self.assertRaisesRegex(ConverterError, "no source_files"):
            trees.conservation(roots, totals, self.index, report)

    def test_results_do_not_depend_on_the_block_size(self):
        other = self.tmp / "agg_block_1"
        run_quietly(occupancy.run_occupancy, self.dataset, other, block_rows=1)
        run_quietly(
            trees.run_trees,
            self.dataset,
            other,
            self.index,
            self.paths["preparation_manifest"],
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


if __name__ == "__main__":
    unittest.main()
