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


def run_budget(records_per_run):
    """A budget that sizes one generated run to ``records_per_run`` records.

    Stated through the module's own constant rather than as a bare multiple of
    48: the budget bounds working BYTES, and a record costs more than its spill
    layout while it is being built.
    """
    return rank_sort.GEN_BYTES_PER_RECORD * records_per_run


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
        self.assert_matches_oracle(blocks, budget_bytes=run_budget(4096))

    def test_equality_across_budgets_forcing_one_two_and_many_runs(self):
        blocks = random_blocks(n_snaps=6, per_snap=120, n_forests=9, seed=41)
        total = sum(int(records.size) for _, records in blocks)
        cases = {
            "one": (run_budget(total), 1, 1),
            "two": (run_budget((total // 2) + 1), 2, 2),
            "many": (run_budget(total // 16), 8, None),
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
            self.run_core(blocks, run_budget(total // 16), name="probe.i64").n_runs, 8
        )

    def test_single_forest(self):
        blocks = random_blocks(n_snaps=5, per_snap=60, n_forests=1, seed=2)
        result = self.assert_matches_oracle(blocks, budget_bytes=run_budget(32))
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
        result = self.assert_matches_oracle(blocks, budget_bytes=run_budget(16))
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
        result = self.assert_matches_oracle([(3, records)], budget_bytes=run_budget(8))
        ranks = self.ranks_of(result)
        # rank must be the descending-id position: the largest id ranks last
        np.testing.assert_array_equal(ranks, np.arange(count - 1, -1, -1))

    def test_snapshot_contributing_zero_records(self):
        blocks = random_blocks(n_snaps=4, per_snap=30, n_forests=3, seed=9)
        empty = np.zeros(0, dtype=KEY_DTYPE)
        blocks.insert(2, (99, empty))
        blocks.append((100, empty))
        self.assert_matches_oracle(blocks, budget_bytes=run_budget(16))

    def test_single_record_overall(self):
        records = make_block([5], [-1], [-1], [17])
        result = self.assert_matches_oracle([(2, records)], budget_bytes=run_budget(8))
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
        result = self.assert_matches_oracle([(1, records)], budget_bytes=run_budget(6))
        np.testing.assert_array_equal(self.ranks_of(result), np.arange(count, dtype=np.int64))

    def test_ranks_are_dense_within_every_forest(self):
        blocks = random_blocks(n_snaps=6, per_snap=90, n_forests=7, seed=77)
        result = self.run_core(blocks, budget_bytes=run_budget(64))
        ranks = self.ranks_of(result)
        forest = np.concatenate([records["forest_id"] for _, records in blocks])
        self.assertEqual(int(result.forest_counts.sum()), ranks.size)
        for forest_id, count in zip(result.forest_ids, result.forest_counts):
            mine = np.sort(ranks[forest == forest_id])
            np.testing.assert_array_equal(mine, np.arange(count, dtype=np.int64))

    def test_forest_starts_are_the_group_boundaries(self):
        blocks = random_blocks(n_snaps=4, per_snap=70, n_forests=5, seed=5)
        _, expected_ids, expected_counts = lexsort_oracle(blocks)
        result = self.run_core(blocks, budget_bytes=run_budget(48))
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
            # the yielded block is a VIEW into one reusable buffer, so a
            # consumer that keeps it past its iteration must copy — which is
            # what makes the merge allocate nothing per block
            merged = np.concatenate(
                [block.position.copy() for block in rank_sort._merge_runs(runs, 48, residency)]
            )
        finally:
            spills.cleanup()
        np.testing.assert_array_equal(merged, expected)

    def test_results_satisfy_the_shipped_identity_assertion(self):
        # not a transcription: this is links.verify_identity itself, the
        # assertion the converter makes about the arrays this core replaces
        blocks = random_blocks(n_snaps=5, per_snap=100, n_forests=8, seed=1234)
        result = self.run_core(blocks, budget_bytes=run_budget(64))
        forest = np.concatenate([records["forest_id"] for _, records in blocks])
        forest_index = np.searchsorted(result.forest_ids, forest)
        verify_identity(forest_index, self.ranks_of(result), result.n_forests, "slice 4 core")


class TestMemoryBound(RankSortCase):
    def test_resident_working_set_bounded_and_independent_of_total(self):
        budget = run_budget(64)
        peaks = {}
        for label, per_snap in (("small", 60), ("four_times", 240)):
            blocks = random_blocks(n_snaps=5, per_snap=per_snap, n_forests=6, seed=3)
            result = self.assert_matches_oracle(blocks, budget, name="ranks_{}.i64".format(label))
            peaks[label] = (result.peak_resident_bytes, result.peak_resident_records)
            self.assertLessEqual(result.peak_resident_bytes, result.budget_bytes)
            self.assertLessEqual(result.peak_resident_records, result.budget_records)
            self.assertGreaterEqual(result.n_runs, 4)
        # a 4x larger input must not hold more working memory
        self.assertEqual(peaks["small"], peaks["four_times"])

    def test_actual_allocation_for_a_whole_call_stays_within_the_budget(self):
        """Measure what a complete ``rank_forests`` call really allocates.

        Every other memory test here reads ``_Residency``, and four rounds of
        review each found one more allocation that counter did not know about —
        a test that reads the instrument cannot detect the instrument being
        incomplete. This one measures ``tracemalloc``'s peak for the whole call,
        so a fifth omission fails it whatever the meter says, and asserts it
        against the budget plus the module's single enumerated allowance.
        """
        # a budget large enough that one escaped record buffer (48 B x
        # merge_records = 786 KB here) cannot hide inside the allowance, which
        # is flat in absolute terms
        merge_records = 16384
        budget = merge_records * rank_sort.MERGE_BYTES_PER_RECORD
        allowance = rank_sort.UNMETERED_ALLOWANCE_BYTES
        self.assertGreater(merge_records * rank_sort.SPILL_RECORD_NBYTES, allowance)
        peaks = {}
        for label, per_snap in (("base", 8000), ("four_times", 32000)):
            blocks = random_blocks(n_snaps=6, per_snap=per_snap, n_forests=60, seed=64)
            gc.collect()
            tracemalloc.start()
            try:
                tracemalloc.reset_peak()
                before = tracemalloc.get_traced_memory()[0]
                result = self.run_core(blocks, budget, name="ranks_{}.i64".format(label))
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
            observed = peak - before
            peaks[label] = observed
            with self.subTest(input=label):
                # a real multi-run merge, so the merge, assign and verify
                # phases all run
                self.assertGreater(result.n_runs, 1)
                # the correctness of this configuration is asserted too, so the
                # measurement cannot be of a call that did the wrong thing
                expected_ranks, _, _ = lexsort_oracle(blocks)
                np.testing.assert_array_equal(self.ranks_of(result), expected_ranks)
                # the module's own meter still holds ...
                self.assertLessEqual(result.peak_resident_bytes, budget)
                # ... and so does what was actually allocated
                self.assertLessEqual(observed, budget + allowance)
                # the bound is not vacuous: the call really did allocate its
                # budget-sized buffers
                self.assertGreater(observed, budget // 2)
        # and the excluded categories do not scale with the record count: a 4x
        # larger input at the same budget must not move the peak by more than
        # the allowance
        self.assertLessEqual(peaks["four_times"] - peaks["base"], allowance)

    def test_bound_holds_when_the_fanin_fills_the_budget(self):
        # the configuration where the merge is most at risk of overrunning: the
        # fan-in is capped by the budget itself, so every run buffers a single
        # record and the per-run bookkeeping is at its largest relative to the
        # budget. An instrument that counted only the budget-sized buffers
        # could not see an overrun here.
        merge_records = 8
        budget = merge_records * rank_sort.MERGE_BYTES_PER_RECORD
        blocks = random_blocks(n_snaps=6, per_snap=60, n_forests=5, seed=21)
        result = self.assert_matches_oracle(blocks, budget)
        self.assertEqual(result.merge_records, merge_records)
        self.assertEqual(rank_sort._fanin_cap(result.merge_records), merge_records)
        self.assertGreater(result.n_runs, merge_records)
        self.assertGreaterEqual(result.n_merge_passes, 1)
        self.assertLessEqual(result.peak_resident_bytes, budget)
        self.assertLessEqual(result.peak_resident_records, result.budget_records)

    def test_the_merge_boundary_is_a_view_not_an_allocation(self):
        # the residency meter can only catch allocations this module reports;
        # an allocation it never makes is the one kind no meter can see, so pin
        # structurally that choosing the boundary copies nothing
        blocks = random_blocks(n_snaps=3, per_snap=40, n_forests=4, seed=44)
        spills = rank_sort._Spills(tempfile.mkdtemp(dir=str(self.spills)))
        try:
            residency = rank_sort._Residency()
            runs, _ = rank_sort._generate_runs(blocks, spills, 16, residency)
            self.assertGreater(len(runs), 2)
            arena = np.empty(8 * len(runs), dtype=rank_sort.SPILL_DTYPE)
            columns = tuple(arena[name] for name in rank_sort.SPILL_DTYPE.names)
            readers = [
                rank_sort._open_run_reader(
                    run,
                    arena[i * 8 : i * 8 + 8],
                    tuple(column[i * 8 : i * 8 + 8] for column in columns),
                )
                for i, run in enumerate(runs)
            ]
            try:
                live = [reader for reader in readers if reader.refill()]
                boundary = rank_sort._lowest_ceiling(live)
                # the boundary must be one of the readers' ALREADY CACHED
                # ceiling tuples, by identity — choosing it must not build
                # anything, since an allocation the module never makes is the
                # only kind its meter cannot catch
                self.assertTrue(
                    any(boundary is reader.ceiling for reader in live),
                    "the merge boundary must be a cached ceiling, not a fresh object",
                )
                # and it must be the smallest of them
                self.assertEqual(boundary, min(reader.ceiling for reader in live))
            finally:
                for reader in readers:
                    reader.close()
        finally:
            spills.cleanup()

    def test_merge_phase_alone_stays_within_the_budget(self):
        # the end-to-end peak is set by run generation's one budget-sized
        # chunk, which would mask a merge that scaled with the run count; meter
        # the merge on its own so the bound is asserted where it is at risk
        run_records = 64
        merge_records = 24
        merge_budget = merge_records * rank_sort.MERGE_BYTES_PER_RECORD
        for per_snap in (60, 240):
            with self.subTest(per_snap=per_snap):
                blocks = random_blocks(n_snaps=6, per_snap=per_snap, n_forests=5, seed=8)
                spills = rank_sort._Spills(tempfile.mkdtemp(dir=str(self.spills)))
                try:
                    runs, total = rank_sort._generate_runs(
                        blocks, spills, run_records, rank_sort._Residency()
                    )
                    self.assertGreaterEqual(len(runs), 5)
                    meter = rank_sort._Residency()
                    emitted = 0
                    for block in rank_sort._merge_runs(runs, merge_records, meter):
                        emitted += block.size
                    self.assertEqual(emitted, total)
                    self.assertGreater(meter.peak_bytes, 0)
                    self.assertLessEqual(meter.peak_bytes, merge_budget)
                    self.assertLessEqual(
                        meter.peak_records, merge_budget // rank_sort.SPILL_RECORD_NBYTES
                    )
                    self.assertEqual(meter.bytes_current, 0)
                    self.assertEqual(meter.records_current, 0)
                finally:
                    spills.cleanup()

    def test_the_meter_counts_the_scratch_the_rank_pass_derives(self):
        # a meter that counted only record buffers would report the same number
        # whether or not the rank pass allocated anything, so pin that the
        # end-to-end peak exceeds what the record buffers alone can explain
        blocks = random_blocks(n_snaps=4, per_snap=90, n_forests=5, seed=31)
        result = self.run_core(blocks, budget_bytes=run_budget(48))
        self.assertGreater(
            result.peak_resident_bytes,
            result.peak_resident_records * rank_sort.SPILL_RECORD_NBYTES,
        )


class TestRanksVerification(RankSortCase):
    """The guard that must hold before any spill is deleted."""

    def write_ranks(self, values):
        path = self.work / "ranks.i64"
        np.asarray(values, dtype=np.int64).tofile(str(path))
        return path

    def verify(self, path, total, counts, max_rank):
        rank_sort._verify_ranks(
            path,
            total,
            np.asarray(counts, dtype=np.int64),
            max_rank,
            run_budget(64),
            rank_sort._Residency(),
        )

    def test_the_verification_block_cannot_overflow_either_sum(self):
        """The block ceiling, checked by arithmetic instead of by allocating
        billions of records.

        The squared residues summed per block are bounded by the modulus, NOT
        by ``max_rank``, so a ceiling derived only from the rank sum leaves a
        large budget over small forests free to wrap int64 — which rejects a
        correct store, and this module then deletes it.
        """
        modulus = rank_sort._MOMENT_MODULUS
        scenarios = (
            ("large budget, small forests", 80 * 1024**3, 10**6, 10**12),
            ("the threshold near 34 GB", 34 * 1024**3, 10**3, 10**12),
            ("the Shin-Uchuu shape", 24 * 1024**3, 1280000000, 22_900_000_000),
            ("the smallest budget", rank_sort.MIN_BUDGET_BYTES, 0, 1),
        )
        for label, budget, max_rank, total in scenarios:
            with self.subTest(scenario=label):
                block = rank_sort._verify_block_records(budget, max_rank, total)
                self.assertGreaterEqual(block, 1)
                self.assertLessEqual(block, total)
                # a block of ranks must sum inside int64 ...
                self.assertLess(block * max_rank, 2**63)
                # ... and so must a block of squared residues
                self.assertLess(block * (modulus - 1), 2**63)
                # ... within the budget
                self.assertLessEqual(block * rank_sort.VERIFY_BYTES_PER_RECORD, budget)

    def test_verification_allocates_no_more_than_its_budget(self):
        """Observe the ALLOCATIONS, not the module's own meter.

        numpy reports its allocations to ``tracemalloc``, so this measures what
        the pass really held. A test that reads the instrument cannot catch the
        instrument being wrong, which is the lesson of this slice.
        """
        total = 100_000
        counts = np.array([total], dtype=np.int64)
        path = self.write_ranks(np.arange(total, dtype=np.int64))
        budget = total * 8  # one whole store's worth of int64
        meter = rank_sort._Residency()
        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            before = tracemalloc.get_traced_memory()[0]
            rank_sort._verify_ranks(path, total, counts, total - 1, budget, meter)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        observed = peak - before
        # tracemalloc traces the interpreter's own objects in the call as well
        # as numpy's buffers, so allow a small absolute margin for those — the
        # defect this catches (per-block temporaries, or a block sized by the
        # store's 8 B/record instead of the pass's 17) overshoots by hundreds
        # of kilobytes, not by this
        interpreter_noise = 64 * 1024
        self.assertLessEqual(observed, budget + interpreter_noise)
        # the bound is not vacuous: the pass really did allocate its buffers
        self.assertGreater(observed, budget // 2)
        # and the module's own meter agrees with what was observed
        self.assertLessEqual(meter.peak_bytes, budget)
        self.assertEqual(meter.bytes_current, 0)

    def test_a_dense_store_verifies(self):
        path = self.write_ranks([0, 1, 2, 0, 1])
        self.verify(path, 5, [3, 2], 2)

    def test_a_non_dense_store_with_the_right_sum_and_max_is_refused(self):
        # [0, 2, 2, 0, 0] has the same record count, the same largest rank and
        # the same sum as the dense [0, 1, 2, 0, 1]; only a second moment
        # separates them, and spills are deleted once this passes
        path = self.write_ranks([0, 2, 2, 0, 0])
        with self.assertRaises(RankSortError) as caught:
            self.verify(path, 5, [3, 2], 2)
        self.assertIn("not dense", str(caught.exception))

    def test_an_unwritten_position_is_refused(self):
        path = self.write_ranks([0, 1, 2, 0, rank_sort.RANK_UNWRITTEN])
        with self.assertRaises(RankSortError) as caught:
            self.verify(path, 5, [3, 2], 2)
        self.assertIn("never assigned", str(caught.exception))

    def test_a_wrong_sum_is_refused(self):
        path = self.write_ranks([0, 1, 2, 0, 2])
        with self.assertRaises(RankSortError) as caught:
            self.verify(path, 5, [3, 2], 2)
        self.assertIn("sum", str(caught.exception))

    def test_a_short_store_is_refused(self):
        path = self.write_ranks([0, 1, 2, 0])
        with self.assertRaises(RankSortError) as caught:
            self.verify(path, 5, [3, 2], 2)
        self.assertIn("byte(s)", str(caught.exception))


class TestSpillLifetime(RankSortCase):
    def test_peak_spill_bytes_are_reported(self):
        # Every record is spilled at least once, so the peak is AT LEAST the
        # whole key set at 48 B/record — but it equals that only when no merge
        # pass ran. A pass holds its inputs and its output on disk at once, and
        # the peak was measured at 1.13x to 1.55x the key set across ordinary
        # multi-pass configurations. Slice 8 must fold in the REPORTED number,
        # never a total * 48 formula.
        blocks = random_blocks(n_snaps=6, per_snap=200, n_forests=8, seed=11)
        total = sum(int(records.size) for _, records in blocks)
        key_set = total * rank_sort.SPILL_RECORD_NBYTES

        single = self.run_core(blocks, budget_bytes=run_budget(256), name="single.i64")
        self.assertEqual(single.n_merge_passes, 0)
        self.assertEqual(single.peak_spill_bytes, key_set)
        self.assertEqual(single.ranks_bytes, total * 8)

        multi = self.run_core(
            blocks,
            budget_bytes=8 * rank_sort.MERGE_BYTES_PER_RECORD,
            name="multi.i64",
        )
        self.assertGreaterEqual(multi.n_merge_passes, 1)
        self.assertGreater(multi.peak_spill_bytes, key_set)

    def test_no_spill_survives_a_successful_run(self):
        blocks = random_blocks(n_snaps=4, per_snap=80, n_forests=5, seed=12)
        result = self.run_core(blocks, budget_bytes=run_budget(32))
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
            self.run_core(blocks(), budget_bytes=run_budget(16))
        self.assertIn("source exploded", str(caught.exception))
        self.assertEqual(sorted(os.listdir(self.spills)), [])
        self.assertFalse((self.work / "ranks.i64").exists())

    def test_no_spill_survives_a_failing_merge(self):
        blocks = random_blocks(n_snaps=3, per_snap=60, n_forests=4, seed=14)
        real_opener = rank_sort._open_run_reader
        calls = {"n": 0}

        def flaky(run, buffer, columns):
            calls["n"] += 1
            if calls["n"] == 2:
                raise OSError("merge input vanished")
            return real_opener(run, buffer, columns)

        with mock.patch.object(rank_sort, "_open_run_reader", flaky):
            with self.assertRaises(OSError):
                self.run_core(blocks, budget_bytes=run_budget(24))
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
                    self.run_core([(1, records)], budget_bytes=run_budget(8))
                self.assertIn(field, str(caught.exception))
                self.assertIn("int64", str(caught.exception))
                self.assertEqual(sorted(os.listdir(self.spills)), [])

    def test_a_missing_key_field_is_rejected(self):
        records = np.zeros(3, dtype=np.dtype([("forest_id", "<i8"), ("id", "<i8")]))
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(0, records)], budget_bytes=run_budget(8))
        self.assertIn("upid", str(caught.exception))

    def test_an_unstructured_array_is_rejected(self):
        with self.assertRaises(RankSortError):
            self.run_core([(0, np.zeros(3, dtype=np.int64))], budget_bytes=run_budget(8))

    def test_a_two_dimensional_block_is_rejected(self):
        records = np.zeros((2, 2), dtype=KEY_DTYPE)
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(0, records)], budget_bytes=run_budget(8))
        self.assertIn("1-D", str(caught.exception))

    def test_a_non_integer_snap_is_rejected(self):
        records = make_block([1], [-1], [-1], [1])
        for snap in (1.0, "3", True, None):
            with self.subTest(snap=snap):
                with self.assertRaises(RankSortError) as caught:
                    self.run_core([(snap, records)], budget_bytes=run_budget(8))
                self.assertIn("integer", str(caught.exception))

    def test_int64_min_snap_is_rejected_because_the_key_negates_it(self):
        # the key stores -snap and -(-2**63) is unrepresentable, so the guard is
        # deliberately strict at the bottom end; a future editor "fixing" the
        # off-by-one would reintroduce a silent wrap
        records = make_block([1], [-1], [-1], [1])
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(int(np.iinfo(np.int64).min), records)], budget_bytes=run_budget(8))
        message = str(caught.exception)
        self.assertIn("negate", message)
        self.assertIn(str(np.iinfo(np.int64).min), message)

    def test_a_big_endian_int64_key_field_is_accepted(self):
        # the gate is on intent (signed, 8 bytes), not on the store's byte
        # order: a genuinely-int64 array is valid input whatever its endianness
        dtype = np.dtype([(name, ">i8") for name in ("forest_id", "upid", "pid", "id")])
        records = np.zeros(6, dtype=dtype)
        records["forest_id"] = [2, 1, 2, 1, 2, 1]
        records["upid"] = [-1, -1, 5, 5, -1, -1]
        records["pid"] = [-1, -1, 5, 5, -1, -1]
        records["id"] = [10, 11, 12, 13, 14, 15]
        self.assert_matches_oracle([(1, records)], budget_bytes=run_budget(8))

    def test_a_malformed_block_is_rejected(self):
        with self.assertRaises(RankSortError) as caught:
            self.run_core([object()], budget_bytes=run_budget(8))
        self.assertIn("(snap, records)", str(caught.exception))

    def test_a_budget_below_the_merge_floor_is_rejected(self):
        records = make_block([1], [-1], [-1], [1])
        with self.assertRaises(RankSortError) as caught:
            self.run_core([(0, records)], budget_bytes=rank_sort.MIN_BUDGET_BYTES - 1)
        self.assertIn("below the", str(caught.exception))

    def test_an_empty_input_produces_an_empty_store(self):
        result = self.run_core([], budget_bytes=run_budget(8))
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
