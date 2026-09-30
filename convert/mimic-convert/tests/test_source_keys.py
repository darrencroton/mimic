"""Unit tests for source-key joins, closure checks and chain acyclicity
(convert/mimic-convert/source_keys.py).

The oracle for every positive case is a hand-written expected target, never a
value derived through the join itself. The negative cases each plant exactly
one defect in an otherwise valid graph and require the named rejection.

Global positions above 2**31 are exercised *virtually*: a
:class:`SnapshotLayout` can describe a snapshot of three billion rows without
allocating one, and the join only ever sees the positions of the halos a test
actually builds. Keys above 2**53 are exercised directly.
"""

import os
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rank_sort  # noqa: E402
import source_keys as sk  # noqa: E402
from adapters.base import LINK_FIELDS, NULL_LINK  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from rank_sort import KeyedSorter, ResidencyMeter  # noqa: E402

#: A merge budget small enough that a block holds a handful of join records,
#: so groups straddle block boundaries.
TINY_MERGE = 2 * (
    rank_sort.keyed_merge_bytes_per_record(sk.JOIN_DTYPE, 3) + sk.JOIN_SCRATCH_BYTES_PER_RECORD
)
ROOMY = 1 << 20


class Halo:
    """One halo of a hand-specified graph: its key, global position, forest
    and the *source keys* of its five links (``None`` = null)."""

    def __init__(self, key, gp, forest=0, **links):
        self.key = key
        self.gp = gp
        self.forest = forest
        self.links = {name: links.get(name) for name in LINK_FIELDS}
        if self.links["FirstHaloInFOFgroup"] is None:
            self.links["FirstHaloInFOFgroup"] = key


def join_records(halos, first_gp=None):
    """Join records for halos, one call per halo so arbitrary (virtual) global
    positions are allowed."""
    parts = []
    for halo in halos:
        keys = {
            name: np.asarray([NULL_LINK if value is None else value], dtype=np.int64)
            for name, value in halo.links.items()
        }
        parts.append(
            sk.build_join_records(
                np.asarray([halo.key], dtype=np.int64),
                np.asarray([halo.forest], dtype=np.int64),
                keys,
                halo.gp,
            )
        )
    return np.concatenate(parts)


def run_join(halos, layout, merge_budget=ROOMY, spill=None):
    """Sort, join and chain-check ``halos``; returns (resolved, stats, rounds)."""
    with tempfile.TemporaryDirectory() as tmp:
        meter = ResidencyMeter()
        make = lambda dtype, key, tag: KeyedSorter(  # noqa: E731
            dtype, key, budget_bytes=ROOMY, spill_dir=tmp, residency=meter, tag=tag
        )
        joins = make(sk.JOIN_DTYPE, sk.JOIN_KEY, "join")
        resolved = make(sk.RESOLVED_DTYPE, sk.RESOLVED_KEY, "resolved")
        chains = make(sk.CHAIN_DTYPE, sk.CHAIN_KEY, "chain")
        try:
            joins.add(join_records(halos))
            join = sk.SourceKeyJoin(layout, resolved, chains)
            blocks = joins.sorted_blocks(
                budget_bytes=merge_budget,
                consumer_bytes_per_record=sk.JOIN_SCRATCH_BYTES_PER_RECORD,
            )
            for block in blocks:
                join.consume(block)
            stats = join.finish()
            rounds = sk.verify_chains_acyclic(
                chains,
                stats.n_chain_edges,
                layout,
                merge_budget_bytes=ROOMY,
                new_round=lambda: make(sk.CHAIN_DTYPE, sk.CHAIN_KEY, "chain"),
            )
            out = [
                block.copy()
                for block in resolved.sorted_blocks(budget_bytes=ROOMY, consumer_bytes_per_record=0)
            ]
            out = np.concatenate(out) if out else np.empty(0, dtype=sk.RESOLVED_DTYPE)
            return out, stats, rounds
        finally:
            for sorter in (joins, resolved, chains):
                sorter.close()


def resolved_table(resolved):
    """{(owner_gp, link name): target_gp}."""
    return {
        (int(record["owner_gp"]), LINK_FIELDS[int(record["kind"])]): int(record["target_gp"])
        for record in resolved
    }


# ==========================================================================
# A valid mixed-gap graph, positions written out by hand
# ==========================================================================

#: Snapshots 0..4, snapshot 2 empty. Counts per snapshot: 2, 2, 0, 3, 1.
#: offsets: 0, 2, 4, 4, 7, 8.
LAYOUT_SNAPSHOTS = (0, 1, 2, 3, 4)
LAYOUT_COUNTS = (2, 2, 0, 3, 1)


def valid_graph():
    """D (snap 4) has three progenitors: A (snap 3, FirstProgenitor), then B
    (snap 1: an earlier sibling, gap 1 -> 4), then C (snap 3: later than B).
    B's main progenitor E sits at snap 0 (gap 0 -> 1 is adjacent) and F at
    snap 0 is E's FoF satellite with no descendant (an early-ending branch).
    A is a FoF central with satellite C at snap 3. G at snap 1 is a lone
    halo of a second forest with no links at all."""
    return [
        # key, gp (snapshot-major, ascending key within a snapshot)
        Halo(50, 0, Descendant=20, NextHaloInFOFgroup=51),  # E  snap 0 row 0
        Halo(51, 1, FirstHaloInFOFgroup=50),  # F  snap 0 row 1
        Halo(20, 2, Descendant=10, FirstProgenitor=50, NextProgenitor=31),  # B snap 1 row 0
        Halo(90, 3, forest=1),  # G  snap 1 row 1
        Halo(30, 4, Descendant=10, NextProgenitor=20, NextHaloInFOFgroup=31),  # A snap 3 row 0
        Halo(31, 5, Descendant=10, FirstHaloInFOFgroup=30),  # C snap 3 row 1
        Halo(40, 6),  # H  snap 3 row 2, isolated
        Halo(10, 7, FirstProgenitor=30),  # D  snap 4 row 0
    ]


class TestValidGraph(unittest.TestCase):
    def setUp(self):
        self.layout = sk.SnapshotLayout(LAYOUT_SNAPSHOTS, LAYOUT_COUNTS)

    def check(self, resolved, stats):
        expected = {
            (0, "Descendant"): 2,
            (0, "FirstHaloInFOFgroup"): 0,
            (0, "NextHaloInFOFgroup"): 1,
            (1, "FirstHaloInFOFgroup"): 0,
            (2, "Descendant"): 7,
            (2, "FirstProgenitor"): 0,
            (2, "NextProgenitor"): 5,
            (2, "FirstHaloInFOFgroup"): 2,
            (3, "FirstHaloInFOFgroup"): 3,
            (4, "Descendant"): 7,
            (4, "NextProgenitor"): 2,
            (4, "FirstHaloInFOFgroup"): 4,
            (4, "NextHaloInFOFgroup"): 5,
            (5, "Descendant"): 7,
            (5, "FirstHaloInFOFgroup"): 4,
            (6, "FirstHaloInFOFgroup"): 6,
            (7, "FirstProgenitor"): 4,
            (7, "FirstHaloInFOFgroup"): 7,
        }
        self.assertEqual(resolved_table(resolved), expected)
        # B -> D spans snapshot positions 1 -> 4 and E -> B is adjacent
        self.assertEqual(stats.n_gapped_descendants, 1)
        self.assertEqual(stats.max_descendant_span, 3)
        self.assertEqual(stats.n_halos, 8)
        self.assertEqual(stats.n_chain_edges, 4)

    def test_every_target_resolves_to_its_hand_written_position(self):
        resolved, stats, rounds = run_join(valid_graph(), self.layout)
        self.check(resolved, stats)
        self.assertGreaterEqual(rounds, 1)

    def test_groups_straddling_tiny_blocks_resolve_identically(self):
        resolved, stats, _rounds = run_join(valid_graph(), self.layout, merge_budget=TINY_MERGE)
        self.check(resolved, stats)

    def test_locate_turns_positions_into_snapshot_rows(self):
        snapshot, row = self.layout.locate(np.asarray([0, 1, 2, 3, 4, 7], dtype=np.int64))
        self.assertEqual(snapshot.tolist(), [0, 0, 1, 1, 3, 4])
        self.assertEqual(row.tolist(), [0, 1, 0, 1, 0, 0])

    def test_a_large_fof_group_straddling_many_blocks_is_counted_across_them(self):
        members = 40
        halos = [Halo(1000, 0, NextHaloInFOFgroup=1001)]
        for index in range(1, members):
            nxt = 1000 + index + 1 if index < members - 1 else None
            halos.append(
                Halo(1000 + index, index, FirstHaloInFOFgroup=1000, NextHaloInFOFgroup=nxt)
            )
        layout = sk.SnapshotLayout((0,), (members,))
        resolved, stats, rounds = run_join(halos, layout, merge_budget=TINY_MERGE)
        table = resolved_table(resolved)
        for index in range(members - 1):
            self.assertEqual(table[(index, "NextHaloInFOFgroup")], index + 1)
        self.assertEqual(stats.n_chain_edges, members - 1)
        # 39 links: ceil(log2 39) = 6 rounds
        self.assertEqual(rounds, 6)


# ==========================================================================
# Planted defects
# ==========================================================================


class TestClosureRejections(unittest.TestCase):
    def setUp(self):
        self.layout = sk.SnapshotLayout(LAYOUT_SNAPSHOTS, LAYOUT_COUNTS)

    def reject(self, halos, pattern, layout=None, budgets=(ROOMY, TINY_MERGE)):
        """Require ``pattern``. Where a planted defect also breaks a group
        with a *lower* key, a stream of tiny blocks closes that group -- and
        reports its coverage failure -- first; those cases pass
        ``budgets=(ROOMY,)``, where one block runs every request check before
        any coverage check."""
        for budget in budgets:
            with self.subTest(merge_budget=budget):
                with self.assertRaisesRegex(ConverterError, pattern):
                    run_join(halos, layout or self.layout, merge_budget=budget)

    def graph_with(self, index, **changes):
        halos = valid_graph()
        halo = halos[index]
        for name, value in changes.items():
            if name in ("key", "gp", "forest"):
                setattr(halo, name, value)
            else:
                halo.links[name] = value
        return halos

    def test_a_missing_target_is_rejected(self):
        self.reject(self.graph_with(0, Descendant=21), "not a halo of this conversion")

    def test_a_target_below_every_key_is_rejected(self):
        self.reject(self.graph_with(7, FirstProgenitor=5), "not a halo of this conversion")

    def test_a_cross_forest_target_is_rejected(self):
        self.reject(self.graph_with(1, forest=3), "links never cross forests")

    def test_a_duplicate_source_key_is_rejected(self):
        self.reject(self.graph_with(6, key=90, FirstHaloInFOFgroup=90), "carried by two halos")

    def test_a_backward_descendant_is_rejected(self):
        # G (snap 1) points at E (snap 0) as its descendant
        self.reject(self.graph_with(3, Descendant=50, forest=0), "strictly later snapshot")

    def test_a_same_snapshot_descendant_is_rejected(self):
        self.reject(self.graph_with(6, Descendant=30), "strictly later snapshot")

    def test_a_forward_first_progenitor_is_rejected(self):
        self.reject(self.graph_with(0, FirstProgenitor=20), "strictly earlier snapshot")

    def test_a_first_progenitor_that_does_not_round_trip_is_rejected(self):
        # D's main progenitor becomes H, whose own Descendant is null
        self.reject(self.graph_with(7, FirstProgenitor=40), "does not round-trip", budgets=(ROOMY,))

    def test_a_next_progenitor_under_another_descendant_is_rejected(self):
        # B's sibling becomes H, which has no descendant
        self.reject(
            self.graph_with(2, NextProgenitor=40), "name the owner's descendant", budgets=(ROOMY,)
        )

    def test_a_next_progenitor_without_a_descendant_is_rejected(self):
        self.reject(self.graph_with(6, NextProgenitor=31), "no Descendant")

    def test_a_progenitor_missing_from_its_chain_is_rejected(self):
        # B's NextProgenitor is dropped: C still names D but nothing reaches it
        self.reject(self.graph_with(2, NextProgenitor=None), "reached by 0 progenitor-chain")

    def test_a_forked_sibling_chain_is_rejected(self):
        # C points back at B, so B is the NextProgenitor of both A and C
        self.reject(self.graph_with(5, NextProgenitor=20), "NextProgenitor of 2 halos")

    def test_a_fof_target_that_is_not_a_central_is_rejected(self):
        # E now names F as its central, so F's FirstHaloInFOFgroup -> E
        # targets a halo that is not a central
        self.reject(self.graph_with(0, FirstHaloInFOFgroup=51), "must be a central")

    def test_a_next_fof_member_of_another_central_is_rejected(self):
        # A's chain reaches H, whose central is itself
        self.reject(
            self.graph_with(4, NextHaloInFOFgroup=40), "share the owner's central", budgets=(ROOMY,)
        )

    def test_a_fof_link_into_another_snapshot_is_rejected(self):
        self.reject(self.graph_with(0, NextHaloInFOFgroup=20), "stay in the owner's snapshot")

    def test_a_central_reached_by_its_own_chain_is_rejected(self):
        self.reject(self.graph_with(5, NextHaloInFOFgroup=30), "FoF central but is reached")

    def test_a_fof_member_outside_its_chain_is_rejected(self):
        self.reject(self.graph_with(4, NextHaloInFOFgroup=None), "FoF member but is reached by 0")

    def test_a_sibling_cycle_beside_the_head_chain_is_rejected(self):
        """Every in-degree is right, so only the chain check can see it: D's
        chain is A -> (end), while B and C point at each other."""
        halos = valid_graph()
        halos[4].links["NextProgenitor"] = None  # A ends the chain D sees
        halos[2].links["NextProgenitor"] = 31  # B -> C
        halos[5].links["NextProgenitor"] = 20  # C -> B
        self.reject(halos, "NextProgenitor chain .* never terminates")

    def test_a_fof_cycle_is_rejected(self):
        members = [
            Halo(1, 0, NextHaloInFOFgroup=2),
            Halo(2, 1, FirstHaloInFOFgroup=1, NextHaloInFOFgroup=None),
            Halo(3, 2, FirstHaloInFOFgroup=1, NextHaloInFOFgroup=4),
            Halo(4, 3, FirstHaloInFOFgroup=1, NextHaloInFOFgroup=3),
        ]
        self.reject(
            members, "NextHaloInFOFgroup chain .* never terminates", sk.SnapshotLayout((0,), (4,))
        )


# ==========================================================================
# 64-bit positions and keys
# ==========================================================================


class TestWideIndices(unittest.TestCase):
    def test_rows_beyond_int32_resolve_exactly_without_allocating_them(self):
        """Snapshot 0 is three billion rows wide and snapshot 7 five billion;
        only the handful of halos under test exist."""
        big0 = 3_000_000_000
        big7 = 5_000_000_000
        layout = sk.SnapshotLayout((0, 3, 7), (big0, 0, big7))
        root = big0 + 4_999_999_999  # last row of snapshot 7
        progenitor = big0 - 1  # last row of snapshot 0
        satellite = big0 + 2_147_483_648  # row 2**31 of snapshot 7
        key_base = 2**53 + 1
        halos = [
            Halo(key_base + 2, progenitor, Descendant=key_base),
            Halo(key_base + 3, satellite, FirstHaloInFOFgroup=key_base),
            Halo(key_base, root, FirstProgenitor=key_base + 2, NextHaloInFOFgroup=key_base + 3),
        ]
        resolved, stats, _rounds = run_join(halos, layout)
        table = resolved_table(resolved)
        self.assertEqual(table[(progenitor, "Descendant")], root)
        self.assertEqual(table[(root, "FirstProgenitor")], progenitor)
        self.assertEqual(table[(root, "NextHaloInFOFgroup")], satellite)
        snapshot, row = layout.locate(np.asarray([root, progenitor, satellite], dtype=np.int64))
        self.assertEqual(snapshot.tolist(), [7, 0, 7])
        self.assertEqual(row.tolist(), [4_999_999_999, big0 - 1, 2_147_483_648])
        self.assertEqual(row.dtype, np.int64)
        self.assertEqual(stats.max_descendant_span, 2)  # position 0 -> 2 over empty snapshot 3

    def test_a_layout_whose_total_overflows_int64_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "overflows int64"):
            sk.SnapshotLayout((0, 1), (2**62, 2**62))

    def test_join_positions_that_would_overflow_int64_are_refused(self):
        keys = {name: np.full(2, NULL_LINK, dtype=np.int64) for name in LINK_FIELDS}
        keys["FirstHaloInFOFgroup"] = np.asarray([1, 2], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, "overflow int64"):
            sk.build_join_records(
                np.asarray([1, 2], dtype=np.int64),
                np.zeros(2, dtype=np.int64),
                keys,
                np.iinfo(np.int64).max - 1,
            )


# ==========================================================================
# Snapshot validation and layout
# ==========================================================================


class TestSnapshots(unittest.TestCase):
    def test_valid_snapshots_pass_through(self):
        self.assertEqual(sk.validate_snapshots(range(4)), (0, 1, 2, 3))
        self.assertEqual(sk.validate_snapshots([np.int32(2), 5]), (2, 5))

    def test_invalid_snapshot_lists_are_refused(self):
        cases = (
            ([], "empty"),
            ([0, True], "not an integer"),
            ([0, 1.0], "not an integer"),
            ([-1, 0], "outside"),
            ([0, 2**31], "outside"),
            ([0, 2, 2], "strictly ascending"),
            ([3, 1], "strictly ascending"),
            (7, "sequence"),
        )
        for snapshots, pattern in cases:
            with self.subTest(snapshots=snapshots):
                with self.assertRaisesRegex(ConverterError, pattern):
                    sk.validate_snapshots(snapshots)

    def test_empty_snapshots_never_own_a_position(self):
        layout = sk.SnapshotLayout((0, 1, 2, 3, 4), (0, 2, 0, 0, 1))
        self.assertEqual(layout.offsets.tolist(), [0, 0, 2, 2, 2, 3])
        positions = layout.positions(np.arange(3, dtype=np.int64))
        self.assertEqual(positions.tolist(), [1, 1, 4])

    def test_counts_must_match_snapshots_and_be_non_negative(self):
        with self.assertRaisesRegex(ConverterError, "2 snapshot counts for 3"):
            sk.SnapshotLayout((0, 1, 2), (1, 1))
        with self.assertRaisesRegex(ConverterError, "negative"):
            sk.SnapshotLayout((0, 1), (1, -1))

    def test_counts_that_int_would_coerce_are_refused(self):
        """A float count would be truncated and ``True`` read as 1, each a
        layout silently different from the rows it describes."""
        cases = (
            ((1, 2.0), "snapshot 1 has count 2.0, which is not an integer"),
            ((1.5, 2), "snapshot 0 has count 1.5, which is not an integer"),
            ((True, 2), "snapshot 0 has count True, which is not an integer"),
            (np.asarray([1.0, 2.0]), "not an integer"),
            (3, "must be a sequence"),
        )
        for counts, pattern in cases:
            with self.subTest(counts=counts):
                with self.assertRaisesRegex(ConverterError, pattern):
                    sk.SnapshotLayout((0, 1), counts)

    def test_bincount_output_is_accepted_as_counts(self):
        snap_positions = np.asarray([0, 0, 2, 3, 3, 3], dtype=np.int64)
        counts = np.bincount(snap_positions, minlength=5)
        layout = sk.SnapshotLayout((0, 1, 2, 3, 4), counts)
        self.assertEqual(layout.offsets.tolist(), [0, 2, 2, 3, 6, 6])
        self.assertEqual(layout.counts.tolist(), [2, 0, 1, 3, 0])
        self.assertEqual(layout.total, 6)

    def test_describe_names_the_snapshot_row_of_a_position(self):
        layout = sk.SnapshotLayout((0, 1, 2, 3, 4), (0, 2, 0, 0, 1))
        self.assertEqual(layout.describe(0), "snapshot 1 row 0")
        self.assertEqual(layout.describe(np.int64(1)), "snapshot 1 row 1")
        self.assertEqual(layout.describe(2), "snapshot 4 row 0")
        self.assertEqual(layout.describe(3), "global position 3 (outside the 3 converted rows)")
        self.assertEqual(layout.describe(-1), "global position -1 (outside the 3 converted rows)")


class TestJoinRecords(unittest.TestCase):
    def test_records_carry_what_each_link_must_close_on(self):
        keys = {
            "Descendant": np.asarray([7], dtype=np.int64),
            "FirstProgenitor": np.asarray([8], dtype=np.int64),
            "NextProgenitor": np.asarray([9], dtype=np.int64),
            "FirstHaloInFOFgroup": np.asarray([5], dtype=np.int64),
            "NextHaloInFOFgroup": np.asarray([6], dtype=np.int64),
        }
        records = sk.build_join_records(
            np.asarray([4], dtype=np.int64), np.asarray([2], dtype=np.int64), keys, 11
        )
        by_tag = {int(r["tag"]): r for r in records}
        self.assertEqual(sorted(by_tag), [0, 1, 2, 3, 4, 5])
        entry = by_tag[0]
        self.assertEqual((int(entry["key"]), int(entry["gp"]), int(entry["forest"])), (4, 11, 2))
        self.assertEqual((int(entry["a"]), int(entry["b"])), (7, 5))
        expect = {1: -1, 2: 4, 3: 7, 4: -1, 5: 5}
        for tag, value in expect.items():
            with self.subTest(tag=tag):
                self.assertEqual(int(by_tag[tag]["a"]), value)
                self.assertEqual(int(by_tag[tag]["b"]), 4)
                self.assertEqual(int(by_tag[tag]["gp"]), 11)

    def test_null_links_make_no_request(self):
        keys = {name: np.asarray([NULL_LINK], dtype=np.int64) for name in LINK_FIELDS}
        keys["FirstHaloInFOFgroup"] = np.asarray([3], dtype=np.int64)
        records = sk.build_join_records(
            np.asarray([3], dtype=np.int64), np.zeros(1, dtype=np.int64), keys, 0
        )
        self.assertEqual(records["tag"].tolist(), [0, sk.LINK_TAG["FirstHaloInFOFgroup"]])


class TestChainRoundLimit(unittest.TestCase):
    def test_a_long_acyclic_chain_needs_ceil_log2_rounds(self):
        length = 100
        halos = [Halo(1, 0, NextHaloInFOFgroup=2)]
        for index in range(1, length):
            nxt = index + 2 if index < length - 1 else None
            halos.append(Halo(index + 1, index, FirstHaloInFOFgroup=1, NextHaloInFOFgroup=nxt))
        layout = sk.SnapshotLayout((0,), (length,))
        _resolved, stats, rounds = run_join(halos, layout)
        self.assertEqual(stats.n_chain_edges, length - 1)
        self.assertEqual(rounds, 7)  # ceil(log2 99)

    def test_rounds_are_bounded_even_if_the_limit_were_generous(self):
        """The cycle is caught at the limit, not by running forever: patching
        the round body to report survivors forever must still stop."""
        halos = valid_graph()
        layout = sk.SnapshotLayout(LAYOUT_SNAPSHOTS, LAYOUT_COUNTS)
        calls = []

        def forever(sorter, upcoming, budget):
            calls.append(1)
            return 1, (0, 2)

        with mock.patch.object(sk, "_jump_once", side_effect=forever):
            with self.assertRaisesRegex(ConverterError, "never terminates"):
                run_join(halos, layout)
        self.assertEqual(len(calls), (4).bit_length())


if __name__ == "__main__":
    unittest.main()
