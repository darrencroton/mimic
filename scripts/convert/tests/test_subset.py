"""Tests for the whole-forest subset selector/extractor (``scripts/convert/subset.py``).

Each test pins one named constraint from
``docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`` -> "Subset Selection and Extraction".
The failure modes these guard against are silent and expensive: a byte extent
off by the ``#tree`` line length corrupts trees without erroring, and a partial
forest passes every extraction check while converting differently from the same
forest in a full run.

The synthetic dataset uses eight files because ``read_locations()`` asserts the
file count is a perfect cube (8 = 2^3) as well as contiguous from zero, and it
places one forest's trees in two different files because forests spanning files
is what makes "a subset of files is not a subset of forests" true.
"""

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ctrees_parser  # noqa: E402
import fixtures  # noqa: E402
import fixups  # noqa: E402
import scatter  # noqa: E402
import subset  # noqa: E402

A_LIST = fixtures.A_LIST
FINAL_SNAP = len(A_LIST) - 1


def make_forest(forest_id: int, n_trees: int, base_mass: float) -> fixtures.ForestSpec:
    """A forest of ``n_trees`` trees, every root at the final snapshot.

    Roots at the final snapshot is not decoration: it is the premise the root
    sampler asserts on every row, measured to hold for 5000/5000 sampled
    micro-Uchuu trees.
    """
    trees = []
    for t in range(n_trees):
        root_id = forest_id * 1000 + t * 10
        mass = base_mass * (1.0 + 0.25 * t)
        trees.append(
            fixtures.TreeSpec(
                root_id=root_id,
                halos=[
                    fixtures.HaloSpec(halo_id=root_id, snap=FINAL_SNAP, mvir=mass, num_prog=1),
                    fixtures.HaloSpec(
                        halo_id=root_id + 1,
                        snap=FINAL_SNAP - 1,
                        mvir=mass * 0.8,
                        desc_id=root_id,
                    ),
                ],
            )
        )
    return fixtures.ForestSpec(forest_id=forest_id, trees=trees)


def index_one_file(path: Path, file_id: int, name: str):
    """Locations rows for one written file: the offset recorded for a tree is
    that of its first DATA row, not of its ``#tree`` marker."""
    rows = []
    pending = None
    pos = 0
    with open(path, "rb") as handle:
        for line in handle:
            if line.startswith(b"#tree "):
                pending = int(line.split()[1])
            elif pending is not None and not line.startswith(b"#"):
                rows.append((pending, file_id, pos, name))
                pending = None
            pos += len(line)
    return rows


def write_dataset(root: Path, placement, forests) -> Path:
    """Write a multi-file ctrees dataset plus its three index artifacts.

    ``placement`` maps file index -> list of TreeSpec; ``forests`` is the
    ForestSpec list the trees came from.
    """
    trees_dir = root / "trees"
    index_dir = root / "index"
    trees_dir.mkdir(parents=True, exist_ok=True)
    index_dir.mkdir(parents=True, exist_ok=True)

    locations = []
    sizes = []
    for file_id in sorted(placement):
        name = "tree_{}_0_0.dat".format(file_id)
        path = trees_dir / name
        fixtures.write_ctrees_file(path, placement[file_id], a_list=A_LIST)
        locations.extend(index_one_file(path, file_id, name))
        sizes.append((name, path.stat().st_size))

    with open(index_dir / "locations.dat", "w") as handle:
        handle.write("#TreeRootID FileID Offset Filename\n")
        for root_id, file_id, offset, name in locations:
            handle.write("{} {} {} {}\n".format(root_id, file_id, offset, name))
    fixtures.write_forests_list(index_dir / "forests.list", forests)
    with open(index_dir / "filesizes.tsv", "w") as handle:
        for name, size in sizes:
            handle.write("/remote/path/{}\t{}\n".format(name, size))
    fixtures.write_a_list(index_dir / "a_list.txt", A_LIST)
    return index_dir


def run(*argv) -> int:
    """Drive one subcommand through the real CLI parser, letting SubsetError out."""
    args = subset.build_parser().parse_args([str(a) for a in argv])
    return args.func(args)


class SubsetTestCase(unittest.TestCase):
    """Common eight-file dataset: 24 forests spread over 8 files, one of them
    (forest 7) deliberately spanning two files."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="mimic-subset-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

        self.forests = [make_forest(fid, 1 + fid % 3, 1.0e11 * (1 + fid)) for fid in range(1, 25)]
        placement = {i: [] for i in range(8)}
        for i, forest in enumerate(self.forests):
            for j, tree in enumerate(forest.trees):
                # forest 7's trees straddle two files: forests may span files
                if forest.forest_id == 7:
                    placement[(j * 3) % 8].append(tree)
                else:
                    placement[i % 8].append(tree)
        self.placement = placement
        self.index_dir = write_dataset(self.tmp, placement, self.forests)
        self.trees_dir = self.tmp / "trees"
        self.work = self.tmp / "work"

    def plan(self, m=0):
        run("plan-candidates", "--index", self.index_dir, "--out", self.work, "--m", m)
        return np.load(self.work / "tree_table.npy")

    def sample(self):
        run(
            "sample-roots",
            "--candidates",
            self.work / "candidates.npy",
            "--filemap",
            self.work / "filemap.json",
            "--trees",
            self.trees_dir,
            "--a-list",
            self.index_dir / "a_list.txt",
            "--out",
            self.work / "root_values.npy",
            "--progress-every",
            0,
        )
        return np.load(self.work / "root_values.npy")

    def finalize(self, out, target_trees=1000, k=2, seed=7):
        return run(
            "finalize",
            "--tree-table",
            self.work / "tree_table.npy",
            "--forest-table",
            self.work / "forest_table.npy",
            "--candidates",
            self.work / "candidates.npy",
            "--root-values",
            self.work / "root_values.npy",
            "--filemap",
            self.work / "filemap.json",
            "--out",
            out,
            "--target-trees",
            target_trees,
            "--k",
            k,
            "--seed",
            seed,
            "--bytes-per-halo",
            500.0,
        )


class HeaderContractTests(unittest.TestCase):
    def test_column_names_match_the_frozen_converter_parser(self):
        """The tool duplicates the converter's header logic because the parser
        module imports pandas, which the data node does not have. Pin the two
        against each other so the duplication cannot drift silently."""
        for dialect in ("indexed", "fields"):
            header = fixtures.header_line(dialect)
            self.assertEqual(
                subset.parse_header_columns(header),
                ctrees_parser.parse_header_line(header),
                "header dialect {!r} diverges from ctrees_parser".format(dialect),
            )

    def test_missing_and_duplicate_columns_abort(self):
        with self.assertRaises(subset.SubsetError):
            subset.resolve_sample_columns(["scale", "id", "Mvir"])
        with self.assertRaises(subset.SubsetError):
            subset.resolve_sample_columns(["scale", "id", "Mvir", "Jx", "Jx", "Jy", "Jz"])


class NumericContractTests(unittest.TestCase):
    def test_spin_matches_the_reference_convention_bit_for_bit(self):
        """The sampler's values must be the ones the reader would hold, or they
        are not comparable with converted output at ties. Includes the zero-mass
        carve-out, where raw J is deliberately kept."""
        mvir = np.array([1.0e12, 0.0, 3.7e10, 1.0], dtype=np.float64)
        jx = np.array([4.2e10, 9.9e9, -1.3e11, 0.0], dtype=np.float64)

        records = np.zeros(mvir.size, dtype=ctrees_parser.RECORD_DTYPE)
        records["Mvir"] = mvir.astype(np.float32)
        records["Jx"] = jx.astype(np.float32)
        records["Jy"] = jx.astype(np.float32)
        records["Jz"] = jx.astype(np.float32)
        fixups.normalise_spin(records)

        np.testing.assert_array_equal(subset.spin_from_widened(mvir, jx), records["Jx"])
        self.assertEqual(records["Jx"][1], np.float32(9.9e9), "zero-mass halo must keep raw J")

    def test_parsed_tokens_are_quantized_to_float32_before_widening(self):
        token = b"1.23456789012345e12"
        self.assertEqual(subset.widened_float32(token, "mvir"), float(np.float32(float(token))))
        self.assertNotEqual(subset.widened_float32(token, "mvir"), float(token))

    def test_non_finite_and_overflowing_values_abort(self):
        """The converter aborts on these; carrying an infinity here would poison
        the sampled Spin bound and the mass ranking instead."""
        for token in (b"nan", b"inf", b"-inf", b"1e39", b"-1e39"):
            with self.assertRaises(subset.SubsetError):
                subset.widened_float32(token, "jz")


class ByteExtentTests(SubsetTestCase):
    def test_extents_reconstruct_every_source_file_exactly(self):
        """A tree's body ends at the next tree's '#tree' line, and the last
        tree's body ends at the file size. Reconstructing each file from the
        computed extents is the only check that catches an off-by-the-marker
        error, which corrupts trees without erroring."""
        table = self.plan()
        for file_id in sorted(self.placement):
            name = "tree_{}_0_0.dat".format(file_id)
            source = (self.trees_dir / name).read_bytes()
            rows = table[table["file_id"] == file_id]
            rows = rows[np.argsort(rows["offset"])]

            first_marker = source.index(b"#tree ")
            rebuilt = bytearray(source[:first_marker])
            for row in rows:
                rebuilt += subset.tree_marker_bytes(int(row["tree_root_id"]))
                start = int(row["offset"])
                rebuilt += source[start : start + int(row["extent"])]
            self.assertEqual(
                bytes(rebuilt), source, "file {} does not round-trip from its extents".format(name)
            )

    def test_a_body_never_swallows_the_next_marker(self):
        table = self.plan()
        for file_id in sorted(self.placement):
            source = (self.trees_dir / "tree_{}_0_0.dat".format(file_id)).read_bytes()
            for row in table[table["file_id"] == file_id]:
                start = int(row["offset"])
                body = source[start : start + int(row["extent"])]
                self.assertNotIn(b"#tree ", body)
                self.assertTrue(body.endswith(b"\n"))


class RootSamplingTests(SubsetTestCase):
    def test_sampled_values_are_exact_for_sampled_rows(self):
        self.plan()
        values = self.sample()
        candidates = np.load(self.work / "candidates.npy")
        np.testing.assert_array_equal(values["tree_root_id"], candidates["tree_root_id"])

        expected = {}
        for forest in self.forests:
            for tree in forest.trees:
                expected[tree.root_id] = tree.halos[0].mvir
        for row in values:
            self.assertAlmostEqual(
                row["mvir"],
                float(np.float32(expected[int(row["tree_root_id"])])),
                delta=abs(row["mvir"]) * 1e-6,
            )

    def test_a_row_that_is_not_its_tree_root_aborts(self):
        """The reader never checks that the first data row is the tree root, so
        the sampler must. A violated premise means the design needs revisiting."""
        self.plan()
        path = self.trees_dir / "tree_0_0_0.dat"
        text = path.read_text().splitlines()
        for i, line in enumerate(text):
            if line.startswith("#tree "):
                columns = text[i + 1].split()
                columns[1] = str(int(columns[1]) + 12345)
                text[i + 1] = " ".join(columns)
                break
        path.write_text("\n".join(text) + "\n")
        with self.assertRaisesRegex(subset.SubsetError, "first-data-row-is-root"):
            self.sample()

    def test_a_marker_disagreeing_with_the_index_aborts(self):
        """Nothing else in the pipeline checks that locations.dat's TreeRootID and
        the '#tree' marker agree, so a marker-only corruption must stop here rather
        than be silently rewritten into the subset from the index's value."""
        self.plan()
        path = self.trees_dir / "tree_0_0_0.dat"
        text = path.read_text().splitlines()
        for i, line in enumerate(text):
            if line.startswith("#tree "):
                text[i] = "#tree {}".format(int(line.split()[1]) + 7)
                break
        path.write_text("\n".join(text) + "\n")
        with self.assertRaisesRegex(subset.SubsetError, "not immediately preceded by its own"):
            self.sample()

    def test_a_non_finite_scale_aborts(self):
        """NaN fails every comparison, so it would slip through the tolerance test
        that is supposed to prove the row is a z=0 root."""
        self.plan()
        path = self.trees_dir / "tree_0_0_0.dat"
        text = path.read_text().splitlines()
        for i, line in enumerate(text):
            if line.startswith("#tree "):
                columns = text[i + 1].split()
                columns[0] = "nan"
                text[i + 1] = " ".join(columns)
                break
        path.write_text("\n".join(text) + "\n")
        with self.assertRaisesRegex(subset.SubsetError, "non-finite scale"):
            self.sample()

    def test_a_gate_disabling_flag_value_is_refused(self):
        """Some flag values do not fail loudly, they quietly switch a check off: a
        NaN tolerance makes every scale comparison vacuously true, and a negative
        bytes-per-halo makes every halo estimate negative so Gate B passes
        everything. Only the flags that gate something are guarded this way."""
        self.plan()
        self.sample()
        for bad in ("nan", "-1"):
            with self.assertRaisesRegex(subset.SubsetError, "--scale-atol must be finite"):
                run(
                    "sample-roots",
                    "--candidates",
                    self.work / "candidates.npy",
                    "--filemap",
                    self.work / "filemap.json",
                    "--trees",
                    self.trees_dir,
                    "--a-list",
                    self.index_dir / "a_list.txt",
                    "--out",
                    self.work / "root_values.npy",
                    "--scale-atol",
                    bad,
                    "--progress-every",
                    0,
                )
        with self.assertRaisesRegex(subset.SubsetError, "--bytes-per-halo must be finite"):
            run(
                "finalize",
                "--tree-table",
                self.work / "tree_table.npy",
                "--forest-table",
                self.work / "forest_table.npy",
                "--candidates",
                self.work / "candidates.npy",
                "--root-values",
                self.work / "root_values.npy",
                "--filemap",
                self.work / "filemap.json",
                "--out",
                self.tmp / "badgate",
                "--target-trees",
                1,
                "--k",
                0,
                "--seed",
                1,
                "--bytes-per-halo",
                -506.3,
            )
        with self.assertRaisesRegex(subset.SubsetError, "--min-recovery must be finite"):
            run(
                "calibrate-proxy",
                "--tree-table",
                self.work / "tree_table.npy",
                "--root-values",
                self.work / "root_values.npy",
                "--min-recovery",
                -1,
            )

    def test_a_root_row_below_the_final_scale_aborts(self):
        self.plan()
        path = self.trees_dir / "tree_0_0_0.dat"
        text = path.read_text().splitlines()
        for i, line in enumerate(text):
            if line.startswith("#tree "):
                columns = text[i + 1].split()
                columns[0] = "{:.5f}".format(A_LIST[0])
                text[i + 1] = " ".join(columns)
                break
        path.write_text("\n".join(text) + "\n")
        with self.assertRaisesRegex(subset.SubsetError, "final a_list scale"):
            self.sample()


class SelectionTests(SubsetTestCase):
    def test_forests_are_selected_whole_even_across_files(self):
        """Selecting part of a forest changes fix_flybys/fix_upid semantics for
        it, so a forest spanning two files must arrive complete or not at all."""
        self.plan()
        self.sample()
        out = self.tmp / "selection"
        self.assertEqual(self.finalize(out), 0, "every assertion should hold for this dataset")
        selection = np.load(out / "selection.npy")

        table = np.load(self.work / "tree_table.npy")
        for forest_id in np.unique(selection["forest_id"]):
            expected = np.sort(table["tree_root_id"][table["forest_id"] == forest_id])
            got = np.sort(selection["tree_root_id"][selection["forest_id"] == forest_id])
            np.testing.assert_array_equal(expected, got)

        spanning = selection[selection["forest_id"] == 7]
        if spanning.size:
            self.assertGreater(np.unique(spanning["file_id"]).size, 1, "forest 7 should span files")

    def test_every_file_contributes_and_the_manifest_records_the_assertions(self):
        """File coverage is not optional and not automatic: a hole surfaces only
        after extraction and transfer are paid for."""
        self.plan()
        self.sample()
        out = self.tmp / "selection"
        # a tiny target leaves the random draw far short of covering 8 files,
        # so closure has to do the work
        self.assertEqual(self.finalize(out, target_trees=1, k=0), 0)
        manifest = json.loads((out / "selection.json").read_text())

        coverage = manifest["assertions"]["file_coverage"]
        self.assertTrue(coverage["passed"])
        self.assertEqual(coverage["files_covered"], 8)
        self.assertTrue(coverage["is_perfect_cube"])
        self.assertTrue(coverage["forests_added_for_closure"], "closure should have been needed")
        self.assertTrue(manifest["assertions"]["gate_a_tree_count"]["passed"])

        # the super-forest assertion's content is the recorded reason, not just a
        # boolean: Gate A excludes on projected reader allocation, 152,000 B/tree
        super_forest = manifest["assertions"]["super_forest_excluded"]["super_forest"]
        self.assertTrue(manifest["assertions"]["super_forest_excluded"]["passed"])
        self.assertEqual(
            super_forest["projected_reader_allocation_bytes"],
            super_forest["n_trees"] * subset.DEFAULT_TREE_ALLOC_BYTES,
        )

        selection = np.load(out / "selection.npy")
        np.testing.assert_array_equal(np.unique(selection["file_id"]), np.arange(8))

    def test_one_forest_closes_every_file_it_touches_in_the_same_round(self):
        """Forests span files (C8), so a single added forest can close several
        missed files at once. Treating the later ones as still-unclosable aborts a
        perfectly good selection with a spurious blocker."""
        tmp = Path(tempfile.mkdtemp(prefix="mimic-subset-span-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)

        singles = [make_forest(fid, 1, 1.0e11 * fid) for fid in range(1, 7)]
        spanning = make_forest(50, 2, 5.0e11)  # its two trees land in files 6 and 7
        placement = {i: list(singles[i].trees) for i in range(6)}
        placement[6] = [spanning.trees[0]]
        placement[7] = [spanning.trees[1]]
        index_dir = write_dataset(tmp, placement, singles + [spanning])

        work = tmp / "work"
        run("plan-candidates", "--index", index_dir, "--out", work, "--m", 0)
        run(
            "sample-roots",
            "--candidates",
            work / "candidates.npy",
            "--filemap",
            work / "filemap.json",
            "--trees",
            tmp / "trees",
            "--a-list",
            index_dir / "a_list.txt",
            "--out",
            work / "root_values.npy",
            "--progress-every",
            0,
        )
        out = tmp / "selection"
        rc = run(
            "finalize",
            "--tree-table",
            work / "tree_table.npy",
            "--forest-table",
            work / "forest_table.npy",
            "--candidates",
            work / "candidates.npy",
            "--root-values",
            work / "root_values.npy",
            "--filemap",
            work / "filemap.json",
            "--out",
            out,
            "--target-trees",
            1,
            "--k",
            0,
            "--seed",
            3,
        )
        self.assertEqual(rc, 0)
        selection = np.load(out / "selection.npy")
        np.testing.assert_array_equal(np.unique(selection["file_id"]), np.arange(8))
        # the spanning forest arrived whole, closing files 6 and 7 together
        self.assertEqual(int((selection["forest_id"] == 50).sum()), 2)

    def test_a_file_touched_only_by_intractable_forests_is_a_blocker(self):
        """No complete-forest closure exists for such a file, and improvising a
        partial one is exactly what the whole-forest invariant forbids. The plan
        requires this to stop the run, not warn."""
        tmp = Path(tempfile.mkdtemp(prefix="mimic-subset-blocked-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)

        small = [make_forest(fid, 1, 1.0e11 * fid) for fid in range(1, 8)]
        big = make_forest(99, 5, 9.0e12)
        placement = {i: list(small[i].trees) for i in range(7)}
        placement[7] = list(big.trees)  # file 7 is reachable only through forest 99
        index_dir = write_dataset(tmp, placement, small + [big])

        work = tmp / "work"
        run("plan-candidates", "--index", index_dir, "--out", work, "--m", 0)
        run(
            "sample-roots",
            "--candidates",
            work / "candidates.npy",
            "--filemap",
            work / "filemap.json",
            "--trees",
            tmp / "trees",
            "--a-list",
            index_dir / "a_list.txt",
            "--out",
            work / "root_values.npy",
            "--progress-every",
            0,
        )
        with self.assertRaisesRegex(subset.SubsetError, "no complete-forest closure exists for it"):
            run(
                "finalize",
                "--tree-table",
                work / "tree_table.npy",
                "--forest-table",
                work / "forest_table.npy",
                "--candidates",
                work / "candidates.npy",
                "--root-values",
                work / "root_values.npy",
                "--filemap",
                work / "filemap.json",
                "--out",
                tmp / "selection",
                "--target-trees",
                1,
                "--k",
                0,
                "--seed",
                7,
                "--max-trees-per-forest",
                4,  # excludes forest 99, and only forest 99
            )


class StatisticsTests(unittest.TestCase):
    def test_representativeness_accepts_a_uniform_draw_and_rejects_a_skewed_one(self):
        rng = np.random.default_rng(3)
        population = np.concatenate(
            [rng.integers(1, 4, 20000), rng.integers(4, 40, 5000), rng.integers(40, 400, 900)]
        )
        uniform = rng.choice(population, size=4000, replace=False)
        self.assertTrue(subset.representativeness(population, uniform)["passed"])

        skewed = population[population < 4][:4000]
        self.assertFalse(subset.representativeness(population, skewed)["passed"])

    def test_pooling_uses_the_population_only(self):
        """Bin edges chosen after seeing the sample would make the test
        unfalsifiable, so pooling must not depend on the sample."""
        population = np.concatenate([np.ones(5000, dtype=np.int64), np.full(3, 1000)])
        wide = subset.representativeness(population, population[:2500])
        narrow = subset.representativeness(population, population[:10])
        self.assertEqual(
            [row["log10_n_trees_range"] for row in wide["bins"]],
            [row["log10_n_trees_range"] for row in narrow["bins"]],
        )

    def test_recovery_fraction_is_one_for_a_perfect_proxy(self):
        forests = np.repeat(np.arange(500, dtype=np.int64), 2)
        mvir = np.repeat(np.arange(500, dtype=np.float64), 2)
        fraction, m = subset.recovery_fraction(forests, mvir.copy(), mvir, 0.5, 100)
        self.assertEqual(fraction, 1.0)
        self.assertEqual(m, 500)

    def test_recovery_fraction_falls_for_an_anticorrelated_proxy(self):
        forests = np.arange(500, dtype=np.int64)
        mvir = np.arange(500, dtype=np.float64)
        fraction, _ = subset.recovery_fraction(forests, -mvir, mvir, 0.2, 100)
        self.assertEqual(fraction, 0.0)

    def test_spearman_is_one_for_a_monotone_relation(self):
        x = np.array([1.0, 2.0, 3.0, 4.0])
        self.assertAlmostEqual(subset.spearman_rank_correlation(x, x**3), 1.0)
        self.assertAlmostEqual(subset.spearman_rank_correlation(x, -x), -1.0)


class ExtractionTests(SubsetTestCase):
    def extract(self, selection_dir, out_dir):
        return run(
            "extract",
            "--selection",
            selection_dir,
            "--trees",
            self.trees_dir,
            "--out",
            out_dir,
            "--progress-every",
            0,
        )

    def full_pipeline(self):
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)
        out_dir = self.tmp / "subset"
        self.assertEqual(self.extract(selection_dir, out_dir), 0)
        return selection_dir, out_dir

    def test_the_extracted_subset_parses_and_covers_its_roots(self):
        """The acceptance the handoff names: the emitted subset must satisfy the
        same structural contract the converter enforces on real input."""
        selection_dir, out_dir = self.full_pipeline()
        selection = np.load(selection_dir / "selection.npy")

        observed = []
        for file_id in range(8):
            path = out_dir / "tree_{}_0_0.dat".format(file_id)
            prescan = ctrees_parser.prescan_file(path)
            self.assertEqual(
                prescan.declared_tree_count,
                len(prescan.tree_root_ids),
                "{}: rewritten count line disagrees with its '#tree' markers".format(path.name),
            )
            observed.extend(prescan.tree_root_ids.tolist())

        forest_map = scatter.load_forests_list(out_dir / "forests.list")
        scatter.validate_root_coverage(np.asarray(observed, dtype=np.int64), forest_map)
        np.testing.assert_array_equal(
            np.sort(np.asarray(observed, dtype=np.int64)), np.sort(selection["tree_root_id"])
        )

        report = json.loads((out_dir / "extract_report.json").read_text())
        self.assertTrue(report["passed"])
        self.assertEqual(report["n_trees_verified"], int(selection.size))

    def test_recorded_offsets_point_at_data_rows_preceded_by_their_marker(self):
        _, out_dir = self.full_pipeline()
        rows = index_one_file(out_dir / "tree_0_0_0.dat", 0, "tree_0_0_0.dat")
        emitted = (out_dir / "tree_0_0_0.dat").read_bytes()
        with open(out_dir / "locations.dat") as handle:
            recorded = {
                int(line.split()[0]): int(line.split()[2])
                for line in handle
                if not line.startswith("#")
            }
        for root_id, _, offset, _ in rows:
            self.assertEqual(recorded[root_id], offset)
            marker = subset.tree_marker_bytes(root_id)
            self.assertEqual(emitted[offset - len(marker) : offset], marker)

    def test_the_count_line_keeps_its_original_field_width(self):
        """The count line is rewritten in place at the source's own width:
        conservative preservation of a field the converter checks."""
        _, out_dir = self.full_pipeline()
        for file_id in range(8):
            source = (self.trees_dir / "tree_{}_0_0.dat".format(file_id)).read_bytes()
            emitted = (out_dir / "tree_{}_0_0.dat".format(file_id)).read_bytes()
            with open(self.trees_dir / "tree_{}_0_0.dat".format(file_id), "rb") as handle:
                _, start, end, _ = subset.locate_header(handle, "source")
            with open(out_dir / "tree_{}_0_0.dat".format(file_id), "rb") as handle:
                _, new_start, new_end, _ = subset.locate_header(handle, "emitted")
            self.assertEqual(end - start, new_end - new_start)
            self.assertEqual(source[:start], emitted[:new_start])

    def test_verification_detects_a_corrupted_body(self):
        """The md5 check compares what was written against what was read, so a
        byte that changes after the copy must be caught."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)
        selection = np.load(selection_dir / "selection.npy")
        rows = selection[selection["file_id"] == 0]

        out_dir = self.tmp / "subset"
        out_dir.mkdir()
        emitted = out_dir / "tree_0_0_0.dat"
        extracted = subset._extract_one_file(self.trees_dir / "tree_0_0_0.dat", emitted, rows)
        self.assertEqual(subset._verify_one_file(emitted, extracted), [])

        data = bytearray(emitted.read_bytes())
        body_start = extracted[1][0]
        data[body_start + 3] = ord("9") if data[body_start + 3] != ord("9") else ord("8")
        emitted.write_bytes(bytes(data))

        failures = subset._verify_one_file(emitted, extracted)
        self.assertTrue(any("md5" in message for message in failures), failures)

    def test_verification_detects_a_body_that_swallowed_the_next_marker(self):
        """The classic off-by-the-marker extent bug. The md5 check is blind to it --
        the same wrong bytes are both copied and hashed -- so the emitted structure
        has to be checked directly, or the error surfaces only after transfer."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir)
        selection = np.load(selection_dir / "selection.npy")
        file_id = next(
            f for f in np.unique(selection["file_id"]) if (selection["file_id"] == f).sum() > 1
        )
        rows = selection[selection["file_id"] == file_id].copy()

        # regress the first tree's extent to the next tree's offset, i.e. forget to
        # subtract the next '#tree' line -- the exact mistake the check exists for
        rows["extent"][0] = rows["offset"][1] - rows["offset"][0]

        name = "tree_{}_0_0.dat".format(int(file_id))
        out_dir = self.tmp / "swallowed"
        out_dir.mkdir()
        emitted = out_dir / name
        extracted = subset._extract_one_file(self.trees_dir / name, emitted, rows)
        failures = subset._verify_one_file(emitted, extracted)
        self.assertTrue(any("swallowed" in message for message in failures), failures)

    def test_verification_detects_an_extent_off_by_a_single_byte(self):
        """Overrunning by less than a whole marker leaves '\\n##tree', which the
        marker scan cannot see -- so the body's final byte is checked too. The same
        check catches an extent that stops mid-row."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir)
        selection = np.load(selection_dir / "selection.npy")
        file_id = next(
            f for f in np.unique(selection["file_id"]) if (selection["file_id"] == f).sum() > 1
        )
        name = "tree_{}_0_0.dat".format(int(file_id))

        for delta in (1, -1):
            rows = selection[selection["file_id"] == file_id].copy()
            rows["extent"][0] += delta
            out_dir = self.tmp / "offby{}".format(delta)
            out_dir.mkdir()
            emitted = out_dir / name
            extracted = subset._extract_one_file(self.trees_dir / name, emitted, rows)
            failures = subset._verify_one_file(emitted, extracted)
            self.assertTrue(
                any("not a newline" in message for message in failures),
                "delta={}: {}".format(delta, failures),
            )

    def test_extracting_into_the_source_directory_is_refused(self):
        """The subset replaces every tree_*.dat and both index files, so pointing
        --out at --trees destroys an irreplaceable source dataset."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)
        with self.assertRaisesRegex(subset.SubsetError, "would overwrite the source"):
            self.extract(selection_dir, self.trees_dir)

    def test_a_source_file_with_a_trailer_after_its_last_row_is_rejected(self):
        """The last tree's body is [offset, file_size), so a trailer is copied into
        it and surfaces only when the converter rejects the row -- after transfer."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)

        path = self.trees_dir / "tree_0_0_0.dat"
        path.write_bytes(path.read_bytes() + b"END\n")
        with self.assertRaisesRegex(subset.SubsetError, "final line is not a data row"):
            self.extract(selection_dir, self.tmp / "trailer")

    def test_a_source_marker_disagreeing_with_the_selection_is_refused(self):
        """Only the top-M candidates are sampled, so for every other selected tree
        this is the one check that the index entry matches the data it points at.
        Relabelling one tree's halos as another's converts cleanly and is wrong
        only in the science."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)

        selection = np.load(selection_dir / "selection.npy")
        target = selection[selection["file_id"] == 0][0]
        root = int(target["tree_root_id"])

        # rewrite that tree's marker to a different id of the SAME digit width, so
        # every recorded offset still lines up and only the identity disagrees
        wrong = str(root)[:-1] + ("0" if str(root)[-1] != "0" else "1")
        path = self.trees_dir / "tree_0_0_0.dat"
        path.write_text(
            path.read_text().replace("#tree {}\n".format(root), "#tree {}\n".format(wrong), 1)
        )
        with self.assertRaisesRegex(subset.SubsetError, "not preceded by its own marker"):
            self.extract(selection_dir, self.tmp / "badmarker")

    def test_a_source_file_without_a_trailing_newline_is_rejected(self):
        """An extractor precondition, checked per file: the last tree's body is
        taken as [offset, file_size), so a missing trailing newline corrupts it."""
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)

        path = self.trees_dir / "tree_0_0_0.dat"
        path.write_bytes(path.read_bytes().rstrip(b"\n"))
        with self.assertRaisesRegex(subset.SubsetError, "does not end with a newline"):
            self.extract(selection_dir, self.tmp / "subset3")

    def test_a_non_cube_file_count_is_rejected(self):
        self.plan()
        self.sample()
        selection_dir = self.tmp / "selection"
        self.finalize(selection_dir, target_trees=1, k=0)
        filemap = json.loads((selection_dir / "filemap.json").read_text())
        del filemap["7"]
        (selection_dir / "filemap.json").write_text(json.dumps(filemap))
        with self.assertRaisesRegex(subset.SubsetError, "perfect cube"):
            self.extract(selection_dir, self.tmp / "subset4")


if __name__ == "__main__":
    unittest.main()
