"""Slice 4 unit tests for the external merge-sort rank core.

The binding oracle is the in-memory formulation ``links.compute_identity``
still uses: one global ``np.lexsort`` over the five key columns, ranked within
forest groups. :func:`lexsort_oracle` transcribes that expression so equality
is asserted against the same arithmetic, and
:meth:`TestOracleEquality.test_results_satisfy_the_shipped_identity_assertion`
additionally puts the core's output through the shipped ``verify_identity``.

Covers the slice's acceptance criteria: element-wise oracle equality over
randomised input and over every named degenerate shape, equality across
budgets that force 1, 2 and >= 8 runs, per-forest rank density, a resident
record count bounded by the budget and independent of the total, reported peak
spill bytes, and spill removal on the success path and on forced failures.
"""

import ast
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rank_sort  # noqa: E402
from links import verify_identity  # noqa: E402
from rank_sort import RankSortError, rank_forests  # noqa: E402

#: A caller record layout with a field the core must ignore, so the tests also
#: prove that a wider array (``fixups.FIXED_RECORD_DTYPE`` is 120 bytes) is
#: accepted as-is.
KEY_DTYPE = np.dtype(
    [
        ("forest_id", "<i8"),
        ("upid", "<i8"),
        ("pid", "<i8"),
        ("id", "<i8"),
        ("Mvir", "<f4"),
    ]
)


def make_block(forest_ids, upids, pids, ids):
    """One caller block from explicit column values."""
    records = np.zeros(len(forest_ids), dtype=KEY_DTYPE)
    records["forest_id"] = forest_ids
    records["upid"] = upids
    records["pid"] = pids
    records["id"] = ids
    return records


def random_blocks(n_snaps, per_snap, n_forests, seed, upid_span=6):
    """Randomised per-snapshot blocks with globally unique ids."""
    rng = np.random.default_rng(seed)
    blocks = []
    next_id = 0
    for snap in range(n_snaps):
        count = int(per_snap)
        records = np.zeros(count, dtype=KEY_DTYPE)
        records["forest_id"] = rng.integers(0, n_forests, count) * 7 + 3
        records["upid"] = rng.integers(-1, upid_span, count)
        records["pid"] = rng.integers(-1, upid_span, count)
        records["id"] = np.arange(next_id, next_id + count)
        next_id += count
        blocks.append((snap, records))
    return blocks


def lexsort_oracle(blocks):
    """The shipped in-memory rank formulation, transcribed from
    ``links.compute_identity`` (concatenate the five key columns over all
    snapshots, one global ``np.lexsort``, rank within forest groups).

    Returns ``(ranks in input order, observed forest ids, halos per forest)``.
    """
    forests, neg_snaps, upids, pids, ids = [], [], [], [], []
    for snap, records in blocks:
        forests.append(records["forest_id"])
        neg_snaps.append(np.full(records.size, -snap, dtype=np.int64))
        upids.append(records["upid"])
        pids.append(records["pid"])
        ids.append(records["id"])
    forest = np.concatenate(forests)
    neg_snap = np.concatenate(neg_snaps)
    upid = np.concatenate(upids)
    pid = np.concatenate(pids)
    identifier = np.concatenate(ids)
    total = forest.size

    order = np.lexsort((identifier, pid, upid, neg_snap, forest))
    sorted_forest = forest[order]
    new_forest = np.r_[True, sorted_forest[1:] != sorted_forest[:-1]]
    starts = np.nonzero(new_forest)[0]
    group_id = np.cumsum(new_forest) - 1
    ranks = np.empty(total, dtype=np.int64)
    ranks[order] = np.arange(total, dtype=np.int64) - starts[group_id]
    return ranks, sorted_forest[starts], np.diff(starts, append=total)


class RankSortCase(unittest.TestCase):
    """Per-test scratch directories, with the spill root kept separate from the
    ranks store so a leftover spill cannot hide behind the output file."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.work = self.root / "work"
        self.spills = self.root / "spills"
        self.work.mkdir()
        self.spills.mkdir()
        self.addCleanup(self.tmp.cleanup)

    def run_core(self, blocks, budget_bytes, name="ranks.i64"):
        return rank_forests(
            blocks,
            self.work / name,
            budget_bytes=budget_bytes,
            spill_dir=self.spills,
        )

    def ranks_of(self, result):
        return np.asarray(rank_sort.open_ranks(result.ranks_path))

    def assert_matches_oracle(self, blocks, budget_bytes, name="ranks.i64"):
        expected_ranks, expected_ids, expected_counts = lexsort_oracle(blocks)
        result = self.run_core(blocks, budget_bytes, name=name)
        np.testing.assert_array_equal(self.ranks_of(result), expected_ranks)
        np.testing.assert_array_equal(result.forest_ids, expected_ids)
        np.testing.assert_array_equal(result.forest_counts, expected_counts)
        self.assertEqual(result.total_records, int(expected_ranks.size))
        self.assertEqual(result.max_rank, int(expected_ranks.max()))
        return result


class TestKeyLayout(unittest.TestCase):
    def test_spill_layout_is_the_reference_key_order(self):
        # numpy compares structured scalars field by field in dtype order, so
        # the field order IS the sort key; a reordering here silently reorders
        # every forest.
        self.assertEqual(
            rank_sort.SPILL_DTYPE.names,
            ("forest_id", "neg_snap", "upid", "pid", "id", "position"),
        )
        self.assertEqual(rank_sort.SPILL_RECORD_NBYTES, 48)
        for field in rank_sort.SPILL_DTYPE.names:
            self.assertEqual(rank_sort.SPILL_DTYPE.fields[field][0], np.dtype(np.int64))


class TestOracleEquality(RankSortCase):
    def test_randomised_input_matches_the_lexsort_formulation(self):
        blocks = random_blocks(n_snaps=7, per_snap=180, n_forests=11, seed=20260826)
        self.assert_matches_oracle(blocks, budget_bytes=48 * 4096)

    def test_equality_across_budgets_forcing_one_two_and_many_runs(self):
        blocks = random_blocks(n_snaps=6, per_snap=120, n_forests=9, seed=41)
        total = sum(int(records.size) for _, records in blocks)
        cases = {
            "one": (48 * total, 1, 1),
            "two": (48 * ((total // 2) + 1), 2, 2),
            "many": (48 * (total // 16), 8, None),
        }
        for label, (budget, min_runs, max_runs) in cases.items():
            with self.subTest(budget=label):
                result = self.assert_matches_oracle(
                    blocks, budget, name="ranks_{}.i64".format(label)
                )
                self.assertGreaterEqual(result.n_runs, min_runs)
                if max_runs is not None:
                    self.assertLessEqual(result.n_runs, max_runs)
        # the "many" case must genuinely exercise the k-way merge
        self.assertGreaterEqual(
            self.run_core(blocks, 48 * (total // 16), name="probe.i64").n_runs, 8
        )

    def test_single_forest(self):
        blocks = random_blocks(n_snaps=5, per_snap=60, n_forests=1, seed=2)
        result = self.assert_matches_oracle(blocks, budget_bytes=48 * 32)
        self.assertEqual(result.n_forests, 1)

    def test_every_record_in_its_own_forest(self):
        blocks = []
        next_id = 0
        for snap in range(4):
            count = 25
            records = make_block(
                np.arange(next_id, next_id + count),
                np.full(count, -1),
                np.full(count, -1),
                np.arange(next_id, next_id + count),
            )
            next_id += count
            blocks.append((snap, records))
        result = self.assert_matches_oracle(blocks, budget_bytes=48 * 16)
        self.assertEqual(result.n_forests, 100)
        np.testing.assert_array_equal(result.forest_counts, np.ones(100, dtype=np.int64))
        self.assertEqual(result.max_rank, 0)

    def test_ties_in_upid_and_pid_broken_only_by_id(self):
        # every record shares one forest, one snapshot, one upid and one pid,
        # so id is the only field that can order them — and the ids are fed in
        # descending order so the input order cannot pass by accident
        count = 40
        records = make_block(
            np.zeros(count, dtype=np.int64),
            np.full(count, 12),
            np.full(count, 12),
            np.arange(count, 0, -1),
        )
        result = self.assert_matches_oracle([(3, records)], budget_bytes=48 * 8)
        ranks = self.ranks_of(result)
        # rank must be the descending-id position: the largest id ranks last
        np.testing.assert_array_equal(ranks, np.arange(count - 1, -1, -1))

    def test_snapshot_contributing_zero_records(self):
        blocks = random_blocks(n_snaps=4, per_snap=30, n_forests=3, seed=9)
        empty = np.zeros(0, dtype=KEY_DTYPE)
        blocks.insert(2, (99, empty))
        blocks.append((100, empty))
        self.assert_matches_oracle(blocks, budget_bytes=48 * 16)

    def test_single_record_overall(self):
        records = make_block([5], [-1], [-1], [17])
        result = self.assert_matches_oracle([(2, records)], budget_bytes=48 * 8)
        self.assertEqual(result.total_records, 1)
        self.assertEqual(result.n_forests, 1)
        self.assertEqual(result.max_rank, 0)
        np.testing.assert_array_equal(self.ranks_of(result), np.zeros(1, dtype=np.int64))

    def test_duplicate_five_field_keys_are_ordered_by_input_position(self):
        # np.lexsort is stable, so the oracle breaks a full-key tie by input
        # position; the core's trailing position field must do the same
        count = 12
        records = make_block(
            np.zeros(count, dtype=np.int64),
            np.zeros(count, dtype=np.int64),
            np.zeros(count, dtype=np.int64),
            np.zeros(count, dtype=np.int64),
        )
        result = self.assert_matches_oracle([(1, records)], budget_bytes=48 * 4)
        np.testing.assert_array_equal(self.ranks_of(result), np.arange(count, dtype=np.int64))

    def test_ranks_are_dense_within_every_forest(self):
        blocks = random_blocks(n_snaps=6, per_snap=90, n_forests=7, seed=77)
        result = self.run_core(blocks, budget_bytes=48 * 64)
        ranks = self.ranks_of(result)
        forest = np.concatenate([records["forest_id"] for _, records in blocks])
        self.assertEqual(int(result.forest_counts.sum()), ranks.size)
        for forest_id, count in zip(result.forest_ids, result.forest_counts):
            mine = np.sort(ranks[forest == forest_id])
            np.testing.assert_array_equal(mine, np.arange(count, dtype=np.int64))

    def test_forest_starts_are_the_group_boundaries(self):
        blocks = random_blocks(n_snaps=4, per_snap=70, n_forests=5, seed=5)
        _, expected_ids, expected_counts = lexsort_oracle(blocks)
        result = self.run_core(blocks, budget_bytes=48 * 48)
        expected_starts = np.concatenate(
            [np.zeros(1, dtype=np.int64), np.cumsum(expected_counts[:-1])]
        )
        np.testing.assert_array_equal(result.forest_starts(), expected_starts)
        np.testing.assert_array_equal(result.forest_ids, expected_ids)

    def test_merged_order_is_the_lexsort_permutation(self):
        """The merged key order must be the permutation ``np.lexsort`` returns,
        record for record — a strictly stronger claim than equal ranks, since
        it also pins the order of records that share a forest boundary."""
        blocks = random_blocks(n_snaps=5, per_snap=140, n_forests=6, seed=99, upid_span=3)
        forest = np.concatenate([records["forest_id"] for _, records in blocks])
        neg_snap = np.concatenate(
            [np.full(records.size, -snap, dtype=np.int64) for snap, records in blocks]
        )
        upid = np.concatenate([records["upid"] for _, records in blocks])
        pid = np.concatenate([records["pid"] for _, records in blocks])
        ids = np.concatenate([records["id"] for _, records in blocks])
        expected = np.lexsort((ids, pid, upid, neg_snap, forest))

        spills = rank_sort._Spills(tempfile.mkdtemp(dir=str(self.spills)))
        try:
            residency = rank_sort._Residency()
            runs, _ = rank_sort._generate_runs(blocks, spills, 48, residency)
            self.assertGreater(len(runs), 8)
            merged = np.concatenate(
                [block["position"] for block in rank_sort._merge_runs(runs, 48, residency)]
            )
        finally:
            spills.cleanup()
        np.testing.assert_array_equal(merged, expected)

    def test_results_satisfy_the_shipped_identity_assertion(self):
        # not a transcription: this is links.verify_identity itself, the
        # assertion the converter makes about the arrays this core replaces
        blocks = random_blocks(n_snaps=5, per_snap=100, n_forests=8, seed=1234)
        result = self.run_core(blocks, budget_bytes=48 * 64)
        forest = np.concatenate([records["forest_id"] for _, records in blocks])
        forest_index = np.searchsorted(result.forest_ids, forest)
        verify_identity(forest_index, self.ranks_of(result), result.n_forests, "slice 4 core")


class TestMemoryBound(RankSortCase):
    def test_resident_records_bounded_and_independent_of_total(self):
        budget = 48 * 64
        peaks = {}
        for label, per_snap in (("small", 60), ("four_times", 240)):
            blocks = random_blocks(n_snaps=5, per_snap=per_snap, n_forests=6, seed=3)
            result = self.assert_matches_oracle(blocks, budget, name="ranks_{}.i64".format(label))
            peaks[label] = result.peak_resident_records
            self.assertLessEqual(result.peak_resident_records, result.budget_records)
            self.assertGreaterEqual(result.n_runs, 4)
        # a 4x larger input must not hold more records
        self.assertEqual(peaks["small"], peaks["four_times"])

    def test_merge_phase_alone_stays_within_the_budget(self):
        # the end-to-end peak is set by run generation's one budget-sized
        # chunk, which would mask a merge that scaled with the run count; meter
        # the merge on its own so the bound is asserted where it is at risk
        budget_records = 64
        for per_snap in (60, 240):
            with self.subTest(per_snap=per_snap):
                blocks = random_blocks(n_snaps=6, per_snap=per_snap, n_forests=5, seed=8)
                spills = rank_sort._Spills(tempfile.mkdtemp(dir=str(self.spills)))
                try:
                    residency = rank_sort._Residency()
                    runs, total = rank_sort._generate_runs(
                        blocks, spills, budget_records, residency
                    )
                    self.assertGreaterEqual(len(runs), 5)
                    merge_meter = rank_sort._Residency()
                    emitted = 0
                    for block in rank_sort._merge_runs(runs, budget_records, merge_meter):
                        emitted += int(block.size)
                    self.assertEqual(emitted, total)
                    self.assertGreater(merge_meter.peak, 0)
                    self.assertLessEqual(merge_meter.peak, budget_records)
                    self.assertEqual(merge_meter.current, 0)
                finally:
                    spills.cleanup()


class TestSpillLifetime(RankSortCase):
    def test_peak_spill_bytes_are_reported(self):
        blocks = random_blocks(n_snaps=5, per_snap=100, n_forests=6, seed=11)
        total = sum(int(records.size) for _, records in blocks)
        result = self.run_core(blocks, budget_bytes=48 * 64)
        # a single merge pass holds every generated run at once, so the peak is
        # the whole key set at 48 B/record
        self.assertEqual(result.peak_spill_bytes, total * rank_sort.SPILL_RECORD_NBYTES)
        self.assertEqual(result.ranks_bytes, total * 8)

    def test_no_spill_survives_a_successful_run(self):
        blocks = random_blocks(n_snaps=4, per_snap=80, n_forests=5, seed=12)
        result = self.run_core(blocks, budget_bytes=48 * 32)
        self.assertGreater(result.n_runs, 1)
        self.assertEqual(sorted(os.listdir(self.spills)), [])
        self.assertTrue(Path(result.ranks_path).exists())

    def test_no_spill_survives_a_failing_input(self):
        good = random_blocks(n_snaps=3, per_snap=80, n_forests=4, seed=13)

        def blocks():
            for block in good:
                yield block
            raise RuntimeError("source exploded")

        with self.assertRaises(RuntimeError) as caught:
            self.run_core(blocks(), budget_bytes=48 * 16)
        self.assertIn("source exploded", str(caught.exception))
        self.assertEqual(sorted(os.listdir(self.spills)), [])
        self.assertFalse((self.work / "ranks.i64").exists())

    def test_no_spill_survives_a_failing_merge(self):
        blocks = random_blocks(n_snaps=3, per_snap=60, n_forests=4, seed=14)
        real_opener = rank_sort._open_run_reader
        calls = {"n": 0}

        def flaky(run, block_records):
            calls["n"] += 1
            if calls["n"] == 2:
                raise OSError("merge input vanished")
            return real_opener(run, block_records)

        with mock.patch.object(rank_sort, "_open_run_reader", flaky):
            with self.assertRaises(OSError):
                self.run_core(blocks, budget_bytes=48 * 24)
        self.assertGreaterEqual(calls["n"], 2)
        self.assertEqual(sorted(os.listdir(self.spills)), [])
        self.assertFalse((self.work / "ranks.i64").exists())

    def test_a_corrupted_spill_run_is_refused(self):
        blocks = random_blocks(n_snaps=2, per_snap=40, n_forests=3, seed=15)
        spills = rank_sort._Spills(tempfile.mkdtemp(dir=str(self.spills)))
        try:
            runs, _ = rank_sort._generate_runs(blocks, spills, 32, rank_sort._Residency())
            with open(str(runs[0].path), "r+b") as handle:
                handle.seek(0)
                first = handle.read(1)
                handle.seek(0)
                handle.write(bytes([first[0] ^ 0xFF]))
            with self.assertRaises(RankSortError) as caught:
                for _ in rank_sort._merge_runs(runs, 32, rank_sort._Residency()):
                    pass
            self.assertIn("CRC32", str(caught.exception))
        finally:
            spills.cleanup()

    def test_a_truncated_spill_run_is_refused(self):
        blocks = random_blocks(n_snaps=2, per_snap=40, n_forests=3, seed=16)
        spills = rank_sort._Spills(tempfile.mkdtemp(dir=str(self.spills)))
        try:
            runs, _ = rank_sort._generate_runs(blocks, spills, 32, rank_sort._Residency())
            with open(str(runs[0].path), "r+b") as handle:
                handle.truncate(rank_sort.SPILL_RECORD_NBYTES)
            with self.assertRaises(RankSortError) as caught:
                for _ in rank_sort._merge_runs(runs, 32, rank_sort._Residency()):
                    pass
            self.assertIn("expected", str(caught.exception))
        finally:
            spills.cleanup()


class TestInputContract(RankSortCase):
    def test_a_non_int64_key_field_is_rejected_not_coerced(self):
        for field, dtype in (("id", "<i4"), ("pid", "<f8"), ("forest_id", "<u8")):
            with self.subTest(field=field):
                fields = [(name, KEY_DTYPE.fields[name][0].str) for name in KEY_DTYPE.names]
                fields = [(name, dtype if name == field else spec) for name, spec in fields]
                records = np.zeros(3, dtype=np.dtype(fields))
                records["id"] = [1, 2, 3]
                with self.assertRaises(RankSortError) as caught:
                    self.run_core([(1, records)], budget_bytes=48 * 8)
                self.assertIn(field, str(caught.exception))
                self.assertIn("int64", str(caught.exception))
                self.assertEqual(sorted(os.listdir(self.spills)), [])

    def test_a_missing_key_field_is_rejected(self):
        records = np.zeros(3, dtype=np.dtype([("forest_id", "<i8"), ("id", "<i8")]))
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(0, records)], budget_bytes=48 * 8)
        self.assertIn("upid", str(caught.exception))

    def test_an_unstructured_array_is_rejected(self):
        with self.assertRaises(RankSortError):
            self.run_core([(0, np.zeros(3, dtype=np.int64))], budget_bytes=48 * 8)

    def test_a_two_dimensional_block_is_rejected(self):
        records = np.zeros((2, 2), dtype=KEY_DTYPE)
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(0, records)], budget_bytes=48 * 8)
        self.assertIn("1-D", str(caught.exception))

    def test_a_non_integer_snap_is_rejected(self):
        records = make_block([1], [-1], [-1], [1])
        for snap in (1.0, "3", True, None):
            with self.subTest(snap=snap):
                with self.assertRaises(RankSortError) as caught:
                    self.run_core([(snap, records)], budget_bytes=48 * 8)
                self.assertIn("integer", str(caught.exception))

    def test_a_malformed_block_is_rejected(self):
        with self.assertRaises(RankSortError) as caught:
            self.run_core([object()], budget_bytes=48 * 8)
        self.assertIn("(snap, records)", str(caught.exception))

    def test_a_budget_below_the_merge_floor_is_rejected(self):
        records = make_block([1], [-1], [-1], [1])
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(0, records)], budget_bytes=48 * 3)
        self.assertIn("at least", str(caught.exception))

    def test_an_empty_input_produces_an_empty_store(self):
        result = self.run_core([], budget_bytes=48 * 8)
        self.assertEqual(result.total_records, 0)
        self.assertEqual(result.n_forests, 0)
        self.assertEqual(result.max_rank, -1)
        self.assertEqual(result.ranks_bytes, 0)
        self.assertEqual(self.ranks_of(result).size, 0)
        self.assertEqual(sorted(os.listdir(self.spills)), [])


class TestSelfContained(unittest.TestCase):
    def test_the_core_imports_only_numpy_and_the_standard_library(self):
        source = Path(rank_sort.__file__).read_text()
        imported = set()
        for node in ast.walk(ast.parse(source)):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.level == 0:
                imported.add((node.module or "").split(".")[0])
        # numpy plus stdlib only (plan: "scripts/convert/ is stdlib + numpy +
        # h5py only"); this core needs no h5py and no converter sibling either
        self.assertEqual(
            imported,
            {
                "contextlib",
                "os",
                "tempfile",
                "zlib",
                "dataclasses",
                "pathlib",
                "typing",
                "resource",
                "numpy",
            },
        )


if __name__ == "__main__":
    unittest.main()
