"""Phase 3 links, ranks, and identity fields for the ctrees -> snapshot-HDF5
converter (plan Slice 6).

Implements the conversion plan's Phase 3 steps 6-9 on the fixed per-snapshot
arrays produced by the Slice 5 fix-up stage. Reference semantics replicated
exactly against ``assign_mergertree_indices`` (src/io/tree/ctrees/ctrees_utils.c):

- **Reference order** (ctrees_utils.c:524-547): the reference sorts each forest
  by descending scale, then ascending upid, pid, id, over POST-fix values.
  Within one snapshot the scale is constant, so the within-snapshot reference
  ("encounter") order reduces to ascending ``(upid, pid, id)`` — NOT the
  ascending-id slab order (the conversion plan's IDENTITY TRAP note).
- **FoF chains** (ctrees_utils.c:549-623): walking the reference order, a
  ``pid == -1`` central opens its group (``FirstHaloInFOFgroup`` self-reference,
  ``NextHaloInFOFgroup`` starts at -1) and every satellite is appended at the
  chain tail in reference order. A group whose first member is not its central
  is the reference's "sort did not place the FOF before the subs" hard error.
- **Descendants** (ctrees_utils.c:628-665): resolved by id at snapshot N+1
  (adjacency was validated in the fix-up stage); a missing target aborts with
  examples — never rewritten to -1, which would silently drop a merger link.
- **Progenitor chains** (ctrees_utils.c:667-706): replicated LITERALLY. The
  first progenitor encountered becomes the chain; each later progenitor is
  promoted to the chain FRONT if strictly more massive (native float32 Mvir,
  the units the reference compares — struct halo_data carries native Msun/h
  through assign_mergertree_indices) than the CURRENT chain head, otherwise
  appended at the tail. The final chain is NOT "max-Mvir first with the rest
  in encounter order" in general: an ascending-mass sequence promotes every
  progenitor and yields a fully reversed chain. The literal insertion loop is
  the parity-bearing semantics; see ``build_progenitor_links``.
- **Identity** (conversion plan Phase 3 step 9): ``HaloRankInForest`` is the
  within-forest index in reference tree-driver order — (snapshot descending,
  upid, pid, id ascending) over post-fix values, equivalent to the reference's
  scale-descending sort because the a_list is strictly increasing (asserted
  from the manifest's observed pairs). ``ForestIndex`` is the dense run-scoped
  forest number carried from the Phase 0 table.

``FirstProgenitor`` values for snapshot N+1 are computed while snapshot N is
resident and carried in a persistent pending buffer (one ``.bin`` intermediate
per target snapshot, so the stage resumes per snapshot). Link fields are int32
snapshot-local indices into the ascending-id slab order, which the emission
slice writes out unchanged (ascending id == ascending |MostBoundID|, the
Slice 5 invariant). All aborts carry counts and concrete examples — never
repair.

**The rank pass is bounded** — the rank/identity pass was wired to the external
merge-sort core in ``c5573d0c``, on the core landed in ``3d52446c``. It used to
concatenate five int64 key columns over every snapshot and run one global
``np.lexsort`` — 187.84 B/halo measured, 4.30 TB at the 22.9e9-halo Shin-Uchuu
production scale. ``HaloRankInForest`` now comes from the external merge-sort
core in ``rank_sort.py`` under an explicit memory budget, ``ForestIndex`` is
derived per snapshot from the Phase 0 forest table (no global pass is needed for
it), and both columns are written to on-disk arrays indexed by global position:
``compute_identity`` returns a :class:`SnapshotIdentity` accessor that keeps
only the adjacent snapshot pair the link stage is working on resident. Identity
verification is bounded the same way and is exact — see :func:`verify_identity`.
The ordering, the ranks and every emitted byte are unchanged; only the memory
profile is.

Out of scope (plan Slice 6 non-goals): HDF5 emission (Slice 7).
"""

import os
import shutil
import sys
import tempfile
import weakref
from collections import OrderedDict
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import ConverterError  # noqa: E402
from fixups import FIXED_DTYPE_TAG, FIXED_RECORD_DTYPE  # noqa: E402
from rank_sort import (  # noqa: E402
    MIN_BUDGET_BYTES,
    RANK_DTYPE,
    RankSortError,
    rank_forests,
)
from scatter import Manifest  # noqa: E402

#: Per-snapshot link/identity record, row-aligned with the fixed scratch file
#: (ascending-id slab order). Link fields are int32 snapshot-local indices
#: (format invariant 2); identity fields are int64 per the frozen contract.
LINKS_RECORD_DTYPE = np.dtype(
    [
        ("Descendant", "<i4"),
        ("FirstProgenitor", "<i4"),
        ("NextProgenitor", "<i4"),
        ("FirstHaloInFOFgroup", "<i4"),
        ("NextHaloInFOFgroup", "<i4"),
        ("ForestIndex", "<i8"),
        ("HaloRankInForest", "<i8"),
    ],
    align=False,
)

#: Human-readable dtype identity recorded in every links-file manifest entry.
LINKS_DTYPE_TAG = "ctrees-links-v1/itemsize=36/" + ",".join(
    "{}:{}".format(name, LINKS_RECORD_DTYPE.fields[name][0].str)
    for name in LINKS_RECORD_DTYPE.names
)

_INT32_MAX = np.iinfo(np.int32).max

#: Default working-memory budget for the rank/identity pass, in bytes. It bounds
#: the external merge core's buffers and this module's own streaming windows. It
#: deliberately does NOT cover the forest-count-sized tables (the Phase 0 forest
#: table, the per-forest counts and offsets) or the exact one-bit-per-halo
#: verification bitset: those are what make the identity assertion exact, and
#: their sizes are reported rather than estimated. 2 GiB is the Slice 5 default
#: because it keeps ``links`` peak RSS at the 406,668,896-halo rehearsal scale
#: well under the plan's 24 GB ceiling while generating few enough spill runs to
#: merge in a single pass.
DEFAULT_RANK_BUDGET_BYTES = 2 * 1024**3

#: How that budget is split: one share for this module's streaming windows, the
#: remaining shares for the external merge core. The core is the phase a large
#: budget actually buys something for (fewer, longer sorted runs); the windows
#: here only need to be large enough for efficient sequential I/O.
STREAM_BUDGET_SHARE = 4

#: Bytes the identity stream holds per fixed-record row while feeding the core,
#: enumerated rather than estimated: the block being read (120), the PREVIOUS
#: block, which the core's own ``for`` loop still references while the generator
#: reads the next one (120), the int64 ForestIndex derived from a block (8), and
#: numpy's own contiguous copy of the strided ``forest_id`` column that
#: ``np.searchsorted`` makes (8).
IDENTITY_STREAM_BYTES_PER_ROW = 2 * FIXED_RECORD_DTYPE.itemsize + 2 * RANK_DTYPE.itemsize

#: Bytes the verification stream holds per row, enumerated rather than
#: estimated. Eleven int64-wide arrays: the ForestIndex and rank blocks read
#: back from disk, each row's forest count, the indices of the in-range rows,
#: their slot indices, those slots' byte indices, the argsort permutation, the
#: sorted slots, and the deduplicated slots with their byte indices and byte
#: group starts. Six one-byte masks: in-range, the slot bit masks, duplicate,
#: repeat, the deduplicated bit masks, and rejected. Every one of them is
#: chunk-sized, and the total is the worst case with all of them live.
VERIFY_STREAM_BYTES_PER_ROW = 11 * RANK_DTYPE.itemsize + 6

#: The two on-disk identity arrays inside the accessor's private directory, both
#: indexed by global position over the whole run (8 B/halo each).
FOREST_INDEX_STORE_NAME = "forest_index.i64"
RANKS_STORE_NAME = "ranks.i64"

#: Prefix of that private directory, created under the workdir scratch dir. The
#: identity pass creates it and removes it — on success and on every failure
#: path — so it is never registered as a manifest intermediate: it does not
#: outlive the call that made it, and no later stage may depend on it.
IDENTITY_DIR_PREFIX = "links_identity_"

#: Slots of the verification bitset examined at once when a failure message
#: needs the first rank a forest does not hold. Bounded so the diagnostic scan
#: of a single huge forest cannot itself allocate without limit.
_MISSING_RANK_SCAN_SLOTS = 1 << 20


def links_scratch_name(snap: int) -> str:
    return "snap_{:03d}_links.bin".format(snap)


def pending_fp_name(snap: int) -> str:
    """Pending FirstProgenitor buffer FOR snapshot ``snap`` (int32, one value
    per snap slab row), written while snapshot ``snap - 1`` is resident."""
    return "snap_{:03d}_pending_fp.bin".format(snap)


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def reference_order(records: np.ndarray) -> np.ndarray:
    """Encounter permutation: ascending (upid, pid, id) over post-fix values —
    the assign_mergertree_indices sort key (ctrees_utils.c:530-547) restricted
    to one snapshot, where the scale comparison is constant."""
    return np.lexsort((records["id"], records["pid"], records["upid"]))


def validate_slab(records: np.ndarray, snap: int, context: str) -> None:
    """Guards the link stage relies on: int32-indexable slab (format invariant
    2), strictly ascending ids (slab-order contract from the sort stage), and
    the post-fix central invariant ``upid == id`` where ``pid == -1``."""
    n = records.size
    if n > _INT32_MAX:
        raise ConverterError(
            "{}: snapshot {} has {} halos, exceeding the int32 link-field bound {}".format(
                context, snap, n, _INT32_MAX
            )
        )
    ids = records["id"]
    if n > 1:
        unordered = np.nonzero(ids[1:] <= ids[:-1])[0]
        if unordered.size:
            examples = [
                "(row={}, id={}, next id={})".format(int(r), int(ids[r]), int(ids[r + 1]))
                for r in unordered[:5]
            ]
            raise ConverterError(
                "{}: snapshot {} slab is not strictly ascending in id at {} position(s); "
                "examples: {}".format(context, snap, unordered.size, ", ".join(examples))
            )
    central = records["pid"] == -1
    bad = central & (records["upid"] != ids)
    if bad.any():
        rows = np.nonzero(bad)[0][:5]
        examples = ["(id={}, upid={})".format(int(ids[r]), int(records["upid"][r])) for r in rows]
        raise ConverterError(
            "{}: snapshot {} has {} central(s) with upid != id after fix-ups; "
            "examples: {}".format(context, snap, int(bad.sum()), ", ".join(examples))
        )


def build_fof_chains(
    records: np.ndarray, order: np.ndarray, snap: int, context: str
) -> Tuple[np.ndarray, np.ndarray]:
    """FoF chain construction in reference order (ctrees_utils.c:549-623).

    Groups are contiguous runs of equal resolved ``upid`` in the reference
    order; each group's first member must be its central (``pid == -1`` and
    ``id == upid`` — otherwise the reference's "sort did not place the FOF
    before the subs" error), every member must share the central's forest,
    and satellites chain in reference order, ending at -1.
    """
    n = records.size
    first_fof = np.full(n, -1, dtype=np.int32)
    next_fof = np.full(n, -1, dtype=np.int32)
    if n == 0:
        return first_fof, next_fof
    upid_sorted = records["upid"][order]
    new_group = np.r_[True, upid_sorted[1:] != upid_sorted[:-1]]
    group_id = np.cumsum(new_group) - 1
    starts = np.nonzero(new_group)[0]
    heads = order[starts]

    bad_head = (records["pid"][heads] != -1) | (records["id"][heads] != upid_sorted[starts])
    if bad_head.any():
        idx = np.nonzero(bad_head)[0][:5]
        examples = [
            "(group upid={}, first member id={}, pid={})".format(
                int(upid_sorted[starts[g]]),
                int(records["id"][heads[g]]),
                int(records["pid"][heads[g]]),
            )
            for g in idx
        ]
        raise ConverterError(
            "{}: snapshot {} has {} FoF group(s) whose first member in reference order "
            "is not the group's central; examples: {}".format(
                context, snap, int(bad_head.sum()), ", ".join(examples)
            )
        )

    member_forest = records["forest_id"][order]
    central_forest = records["forest_id"][heads][group_id]
    bad_forest = member_forest != central_forest
    if bad_forest.any():
        idx = np.nonzero(bad_forest)[0][:5]
        examples = [
            "(id={}, forest={}, central id={}, central forest={})".format(
                int(records["id"][order[i]]),
                int(member_forest[i]),
                int(records["id"][heads[group_id[i]]]),
                int(central_forest[i]),
            )
            for i in idx
        ]
        raise ConverterError(
            "{}: snapshot {} has {} FoF member(s) in a different forest than their "
            "central; examples: {}".format(
                context, snap, int(bad_forest.sum()), ", ".join(examples)
            )
        )

    first_fof[order] = heads[group_id].astype(np.int32)
    same_group = ~new_group[1:]
    next_fof[order[:-1][same_group]] = order[1:][same_group].astype(np.int32)
    return first_fof, next_fof


def build_descendants(
    records: np.ndarray, next_ids: np.ndarray, snap: int, context: str
) -> np.ndarray:
    """Descendant merge-join on sorted ids (conversion plan Phase 3 step 7).

    ``next_ids`` is snapshot N+1's sorted id index; positions in it ARE the
    N+1 slab indices. A ``desc_id`` with no target is corrupt input under the
    adjacency invariant — abort with examples, never rewrite to -1.
    """
    n = records.size
    desc = np.full(n, -1, dtype=np.int32)
    has = np.nonzero(records["desc_id"] != -1)[0]
    if has.size == 0:
        return desc
    targets = records["desc_id"][has]
    pos = np.searchsorted(next_ids, targets)
    if next_ids.size:
        clipped = np.minimum(pos, next_ids.size - 1)
        missing = (pos >= next_ids.size) | (next_ids[clipped] != targets)
    else:
        missing = np.ones(targets.size, dtype=bool)
    if missing.any():
        rows = has[missing][:5]
        examples = [
            "(id={}, desc_id={})".format(int(records["id"][r]), int(records["desc_id"][r]))
            for r in rows
        ]
        raise ConverterError(
            "{}: snapshot {} has {} descendant link(s) with no target halo at snapshot {}; "
            "examples: {}".format(context, snap, int(missing.sum()), snap + 1, ", ".join(examples))
        )
    desc[has] = pos.astype(np.int32)
    return desc


def build_progenitor_links(
    records: np.ndarray, order: np.ndarray, desc: np.ndarray, n_next: int
) -> Tuple[np.ndarray, np.ndarray]:
    """Progenitor chains, replicating the reference insertion loop LITERALLY
    (ctrees_utils.c:667-706).

    Progenitors of each descendant are enumerated in reference encounter order
    (the ``order`` permutation — (upid, pid, id), NOT slab order). The first
    becomes the chain; each later progenitor is promoted to the chain front if
    its native float32 Mvir is strictly greater than the CURRENT chain head's
    (``forest[first_prog].Mvir < forest[i].Mvir``), otherwise appended at the
    tail. Ties therefore keep the first-encountered halo in front, and an
    ascending-mass sequence yields a fully reversed chain — the literal
    reference behaviour, deliberately not simplified to "max first, remainder
    in encounter order".

    Returns ``(next_prog, pending_fp)``: NextProgenitor chains within this
    snapshot's slab, and the FirstProgenitor values for snapshot N+1 (indexed
    by N+1 slab position, -1 where a halo has no progenitor).
    """
    next_prog = np.full(records.size, -1, dtype=np.int32)
    pending_fp = np.full(n_next, -1, dtype=np.int32)
    enc = order[desc[order] != -1]
    if enc.size == 0:
        return next_prog, pending_fp
    d = desc[enc]
    by_desc = np.argsort(d, kind="stable")  # stable: keeps encounter order per descendant
    rows = enc[by_desc]
    d_sorted = d[by_desc]
    new_d = np.r_[True, d_sorted[1:] != d_sorted[:-1]]
    starts = np.nonzero(new_d)[0]
    ends = np.r_[starts[1:], d_sorted.size]
    single = (ends - starts) == 1
    pending_fp[d_sorted[starts[single]]] = rows[starts[single]].astype(np.int32)

    mvir = records["Mvir"]
    for s, e in zip(starts[~single], ends[~single]):
        progs = rows[s:e]
        head = int(progs[0])
        tail = head
        for p in progs[1:]:
            p = int(p)
            if mvir[head] < mvir[p]:
                next_prog[p] = head
                head = p
            else:
                next_prog[tail] = p
                tail = p
        pending_fp[d_sorted[s]] = head
    return next_prog, pending_fp


def verify_descendant_forests(
    desc: np.ndarray,
    forest_index: np.ndarray,
    next_forest_index: np.ndarray,
    records: np.ndarray,
    snap: int,
    context: str,
) -> None:
    """A descendant must live in the same forest as its progenitor — the
    reference resolves descendants inside one forest's array, so a cross-forest
    match is unrepresentable there. The global merge-join could silently
    accept one if forests.list mis-grouped linked trees; abort instead."""
    has = np.nonzero(desc != -1)[0]
    if has.size == 0:
        return
    bad = forest_index[has] != next_forest_index[desc[has]]
    if bad.any():
        rows = has[bad][:5]
        examples = [
            "(id={}, desc_id={}, ForestIndex={}, descendant ForestIndex={})".format(
                int(records["id"][r]),
                int(records["desc_id"][r]),
                int(forest_index[r]),
                int(next_forest_index[desc[r]]),
            )
            for r in rows
        ]
        raise ConverterError(
            "{}: snapshot {} has {} descendant link(s) crossing forest boundaries; "
            "examples: {}".format(context, snap, int(bad.sum()), ", ".join(examples))
        )


class _Int64Column:
    """A flat ``RANK_DTYPE`` array on disk, read back in bounded chunks.

    The identity pass hands two of these to :func:`verify_identity` in place of
    the resident arrays the in-memory formulation used, which is what makes the
    assertion bounded: nothing here ever holds more than one chunk.
    """

    def __init__(self, path, size: int):
        self.path = Path(path)
        self.size = int(size)

    def chunks(self, chunk_rows: int) -> Iterator[np.ndarray]:
        remaining = self.size
        with open(self.path, "rb") as handle:
            while remaining > 0:
                block = np.fromfile(handle, dtype=RANK_DTYPE, count=min(chunk_rows, remaining))
                if block.size == 0:
                    raise ConverterError(
                        "{}: ended after {} of {} int64 value(s)".format(
                            self.path, self.size - remaining, self.size
                        )
                    )
                remaining -= block.size
                yield block


class _ResidentColumn:
    """An int64 array already in memory, chunked through the same interface, so
    the verifier has ONE code path: callers holding arrays (the unit tests, any
    small caller) and the identity pass's on-disk stores are verified by exactly
    the same arithmetic."""

    def __init__(self, values: np.ndarray):
        self.values = values
        self.size = int(values.size)

    def chunks(self, chunk_rows: int) -> Iterator[np.ndarray]:
        for start in range(0, self.size, chunk_rows):
            yield self.values[start : start + chunk_rows]


def _as_column(values):
    return _ResidentColumn(values) if isinstance(values, np.ndarray) else values


def _not_dense_message(context: str, n_forests: int) -> str:
    return "{}: observed ForestIndex values are not dense over [0, {})".format(context, n_forests)


def _count_halos_per_forest(fi_column, n_forests: int, chunk_rows: int, context: str) -> np.ndarray:
    """Halos per ForestIndex, and with it the density condition: every value in
    ``[0, n_forests)`` and every forest in that range observed at least once —
    exactly what the in-memory formulation's ``fi[starts] == arange(n_forests)``
    comparison asserted."""
    counts = np.zeros(n_forests, dtype=np.int64)
    for block in fi_column.chunks(chunk_rows):
        if block.size and (int(block.min()) < 0 or int(block.max()) >= n_forests):
            raise ConverterError(_not_dense_message(context, n_forests))
        counts += np.bincount(block, minlength=n_forests)
    if not counts.all():
        raise ConverterError(_not_dense_message(context, n_forests))
    return counts


def _first_unheld_rank(bits: np.ndarray, offset: int, count: int) -> int:
    """The first rank in one forest that no halo holds, read back out of the
    verification bitset for a failure message. Always exists when that forest
    rejected a pair: its halos then cover fewer than ``count`` distinct slots."""
    for start in range(0, count, _MISSING_RANK_SCAN_SLOTS):
        stop = min(start + _MISSING_RANK_SCAN_SLOTS, count)
        slots = np.arange(offset + start, offset + stop, dtype=np.int64)
        held = (bits[slots >> 3] >> (slots & 7).astype(np.uint8)) & np.uint8(1)
        free = np.nonzero(held == 0)[0]
        if free.size:
            return int(start + int(free[0]))
    return -1


def _verify_rank_density(
    fi_column, rank_column, counts: np.ndarray, offsets: np.ndarray, chunk_rows: int, context: str
) -> None:
    """Per-forest rank density and (ForestIndex, HaloRankInForest) uniqueness,
    proved exactly with one bit per halo.

    ``offsets[forest] + rank`` is the position that halo would occupy in the
    lexsorted (ForestIndex, rank) order, so the pairs are dense and unique if
    and only if every halo claims an in-range slot and no slot is claimed twice.
    """
    total = fi_column.size
    bits = np.zeros((total + 7) // 8, dtype=np.uint8)
    n_bad = 0
    examples = []
    for fi, rank in zip(fi_column.chunks(chunk_rows), rank_column.chunks(chunk_rows)):
        limits = counts[fi]
        in_range = (rank >= 0) & (rank < limits)
        rows = np.nonzero(in_range)[0]
        slots = offsets[fi[rows]] + rank[rows]
        byte = slots >> 3
        mask = (1 << (slots & 7)).astype(np.uint8)
        # claimed before this chunk...
        duplicate = (bits[byte] & mask) != 0
        # ...or twice inside it: sorting the chunk's slots makes equal slots
        # adjacent, and the first of each run keeps the slot
        order = np.argsort(slots, kind="stable")
        sorted_slots = slots[order]
        repeat = np.zeros(sorted_slots.size, dtype=bool)
        if sorted_slots.size > 1:
            repeat[1:] = sorted_slots[1:] == sorted_slots[:-1]
        duplicate[order] = duplicate[order] | repeat
        if sorted_slots.size:
            unique_slots = sorted_slots[~repeat]
            unique_bytes = unique_slots >> 3
            unique_masks = (1 << (unique_slots & 7)).astype(np.uint8)
            # distinct slots can share a byte, so the bits of one byte are OR-ed
            # together before the store: a buffered ``|=`` over repeated byte
            # indices would drop all but one of them
            byte_starts = np.nonzero(np.r_[True, unique_bytes[1:] != unique_bytes[:-1]])[0]
            bits[unique_bytes[byte_starts]] |= np.bitwise_or.reduceat(unique_masks, byte_starts)
        rejected = ~in_range
        rejected[rows[duplicate]] = True
        n_rejected = int(rejected.sum())
        if n_rejected:
            n_bad += n_rejected
            if len(examples) < 5:
                for row in np.nonzero(rejected)[0][: 5 - len(examples)]:
                    examples.append((int(fi[row]), int(rank[row])))
    if n_bad:
        detail = [
            "(ForestIndex={}, rank={}, expected {})".format(
                forest, rank, _first_unheld_rank(bits, int(offsets[forest]), int(counts[forest]))
            )
            for forest, rank in examples
        ]
        raise ConverterError(
            "{}: {} (ForestIndex, HaloRankInForest) pair(s) violate per-forest "
            "density/uniqueness; examples: {}".format(context, n_bad, ", ".join(detail))
        )


def verify_identity(
    forest_index,
    ranks,
    n_forests: int,
    context: str,
    *,
    budget_bytes: int = DEFAULT_RANK_BUDGET_BYTES,
) -> None:
    """The plan's global identity assertion, checked on the OUTPUT arrays
    independently of how they were constructed: (ForestIndex, HaloRankInForest)
    pairs unique, ForestIndex dense over [0, n_forests), and ranks dense per
    forest (format invariant 4).

    ``forest_index`` and ``ranks`` are each either a resident int64 array or an
    on-disk :class:`_Int64Column`; both are consumed in bounded chunks, so the
    assertion holds at any scale (plan Slice 5). Two streaming passes replace
    the global ``np.lexsort`` the in-memory formulation used:

    1. count halos per ForestIndex, which settles density over
       ``[0, n_forests)``;
    2. claim ``forest_offset[ForestIndex] + rank`` in a one-bit-per-halo bitset
       — exactly the position that halo would occupy in the lexsorted order. A
       rank outside its forest's ``[0, count)``, or a slot claimed twice, IS a
       per-forest density or pair-uniqueness violation, and nothing else is.

    Exact by construction rather than by aggregate: sums, hashes and extrema all
    admit collisions (``[0,0,2 | 1,1]`` matches dense ``[0,1,2 | 0,1]`` in sum,
    maximum and modular sum-of-squares), a bitset does not.

    Resident cost: one bit per halo, 8 bytes per forest for the counts and 8 for
    the offsets, plus streaming windows out of ``budget_bytes``.

    **The reported violation count and the five examples are this formulation's
    own.** Reproducing the lexsorted formulation's positional count exactly
    would need a multiplicity per slot instead of a bit (>= 1 B/halo, 22.9 GB at
    production) — the memory wall this slice exists to remove. What is detected
    is identical, the message shapes are identical, and ``expected`` in each
    example is the first rank in that forest that no halo holds.
    """
    fi_column = _as_column(forest_index)
    rank_column = _as_column(ranks)
    if fi_column.size != rank_column.size:
        raise ConverterError(
            "{}: identity verification got {} ForestIndex value(s) against {} rank(s)".format(
                context, fi_column.size, rank_column.size
            )
        )
    n_forests = int(n_forests)
    chunk_rows = max(1, (budget_bytes // STREAM_BUDGET_SHARE) // VERIFY_STREAM_BYTES_PER_ROW)
    counts = _count_halos_per_forest(fi_column, n_forests, chunk_rows, context)
    offsets = np.zeros(n_forests, dtype=np.int64)
    if n_forests > 1:
        np.cumsum(counts[:-1], out=offsets[1:])
    _verify_rank_density(fi_column, rank_column, counts, offsets, chunk_rows, context)


def _validate_monotonic_pairs(manifest: Manifest) -> None:
    """Snapshot-descending rank order equals the reference's scale-descending
    order only if snapshot number and scale factor are in strict one-to-one
    ascending correspondence; assert both halves from the manifest's
    a_list-validated observed pairs. A snapshot observed with more than one
    scale value would let the reference's scale sort split what the converter
    treats as one slab, so it aborts even though each value is within the
    a_list tolerance."""
    pairs = sorted((int(s), float(a)) for s, a in manifest.data["observed_pairs"])
    for (snap_a, scale_a), (snap_b, scale_b) in zip(pairs, pairs[1:]):
        if snap_b == snap_a:
            raise ConverterError(
                "snapshot {} was observed with multiple scale factors ({} and {}) — the "
                "reference scale-descending sort could order these halos differently from "
                "one snapshot-number slab".format(snap_a, scale_a, scale_b)
            )
        if scale_b <= scale_a:
            raise ConverterError(
                "observed scale factors are not strictly increasing with snapshot number: "
                "snapshot {} has scale {} but snapshot {} has scale {} — snapshot-descending "
                "rank order would not match the reference scale-descending sort".format(
                    snap_a, scale_a, snap_b, scale_b
                )
            )


def _load_fixed(manifest: Manifest, snap: int) -> np.ndarray:
    """Verify and load one snapshot's fixed scratch file.

    The link stage works on a whole snapshot at a time by construction (FoF
    chains and the progenitor insertion loop are per-snapshot), so this loads
    the slab; the rank pass, which does not need a whole snapshot, streams the
    same file in bounded blocks instead (:func:`_iter_identity_blocks`)."""
    entry = manifest.data["snapshots"][str(snap)]
    _check_fixed_dtype_tag(manifest, entry["fixed_file"])
    manifest.verify_intermediate(entry["fixed_file"], "fixed snapshot scratch")
    records = np.fromfile(entry["fixed_file"], dtype=FIXED_RECORD_DTYPE)
    if len(records) != entry["rows"]:
        raise ConverterError(
            "{}: has {} rows, manifest records {}".format(
                entry["fixed_file"], len(records), entry["rows"]
            )
        )
    return records


def _remove_identity_store(directory: Path, store_bytes: Optional[int] = None) -> bool:
    """Remove one identity store, reporting a removal that did not happen, and
    raising NOTHING whatever goes wrong.

    Returns True only when the directory is CONFIRMED gone. That return value is
    what lets ownership be released exactly when the bytes are, rather than when
    an attempt was made — see :meth:`SnapshotIdentity.close`.

    Every phase of the store's ownership routes through here — the identity
    pass's own failure handler, :meth:`SnapshotIdentity.close`, and the
    lifetime finalizer that backs both — so the two halves of the guarantee
    cannot drift apart: the bytes are removed if they can be, and if they
    cannot, that fact is on the record.

    It must not raise, for two independent reasons. It runs while an exception
    is already propagating, where a second failure would replace the traceback
    that explains the run; and it runs from a finalizer, where an exception is
    unraisable anyway. Hence the broad ``except``: reporting is best-effort by
    construction, and a closed stderr or a failing ``stat`` must not turn
    cleanup into a new failure. ``BaseException`` is deliberately NOT caught, so
    a KeyboardInterrupt still gets out.

    ``store_bytes`` is what the store holds when that is known; ``None`` means
    the pass failed before it could know, and the message then says the size is
    unknown rather than printing a number that would be wrong.
    """
    try:
        shutil.rmtree(directory, ignore_errors=True)
        if not directory.exists():
            return True
        size = "{} byte(s)".format(store_bytes) if store_bytes is not None else "size unknown"
        _log(
            "links: WARNING — could not remove the identity store {} ({}); "
            "remove it by hand before trusting the workdir's storage envelope".format(
                directory, size
            )
        )
    except Exception:
        pass
    # anything short of a confirmed absence — a removal that failed, a stat that
    # failed, a report that failed — leaves the store still needing an owner
    return False


#: Snapshots the identity accessor keeps resident. Two, because the link stage
#: works on an adjacent pair: ``link_one_snapshot`` verifies snapshot N's halo
#: forests against snapshot N+1's (``verify_descendant_forests``), and nothing
#: in the stage ever reaches wider than that.
RESIDENT_SNAPSHOTS = 2


class SnapshotIdentity:
    """Per-snapshot ``(ForestIndex, HaloRankInForest)`` accessor backed by two
    on-disk int64 arrays indexed by global position.

    ``identity[snap]`` returns that snapshot's two int64 arrays in slab order —
    element for element what the ``{snap: (forest_index, ranks)}`` dict handed
    the link stage before. What changed is residency: the dict was a pair of
    views into arrays covering every snapshot (16 B/halo, 366 GB at production),
    while this holds at most :data:`RESIDENT_SNAPSHOTS` snapshots at a time.

    The accessor owns its backing directory, and owns it from the instant it
    exists rather than from the instant a caller acquires it: :meth:`close`
    releases it deterministically, the identity pass removes it on every failure
    path before this object exists, and a ``weakref.finalize`` registered in
    ``__init__`` releases it if ``close`` is never called — or was called and
    did not succeed. **From the moment the directory exists there is no
    reachable in-process path, normal, exceptional or asynchronous, on which
    nothing owns it.** What makes that true rather than nearly true is the
    detach rule: ``weakref.finalize`` is ONE-SHOT and marks itself dead before
    it calls the function, so ``close`` removes the store directly and detaches
    the finalizer only once the directory is confirmed gone. A removal that
    fails, or is interrupted part-way by an asynchronous exception, therefore
    leaves the store still owned, and the next holder — a later ``close``,
    object destruction, or interpreter exit — retries it and reports again if it
    fails again. (The one exception is outside Python's reach: a signal whose
    default disposition kills the process, such as an un-handled SIGTERM or a
    SIGKILL, leaves the store for the operator, as it leaves every other
    temporary file.) The reported byte counts are for the storage envelope
    (plan Slice 8): ``peak_spill_bytes`` is what the merge core held on disk at
    its high-water mark, ``store_bytes`` what the two identity arrays occupy.
    ``n_runs``, ``n_merge_passes`` and ``merge_records`` report what the budget
    bought inside the core, so a caller — or a test — can tell whether the spill
    and merge paths were exercised at all.
    """

    def __init__(
        self,
        directory,
        layout: Dict[int, Tuple[int, int]],
        *,
        peak_spill_bytes: int,
        store_bytes: int,
        n_runs: int = 0,
        n_merge_passes: int = 0,
        merge_records: int = 0,
    ):
        self.directory = Path(directory)
        self.forest_index_path = self.directory / FOREST_INDEX_STORE_NAME
        self.ranks_path = self.directory / RANKS_STORE_NAME
        self.peak_spill_bytes = int(peak_spill_bytes)
        self.store_bytes = int(store_bytes)
        self.n_runs = int(n_runs)
        self.n_merge_passes = int(n_merge_passes)
        self.merge_records = int(merge_records)
        self._layout = dict(layout)
        self._resident = OrderedDict()
        # Ownership is bound to this object's LIFETIME, not to a caller reaching
        # close() or a ``with``: those are two separate statements in the caller
        # and an asynchronous exception can land between them. Registering here
        # means the store is owned from the instant the accessor exists, so
        # there is no reachable path — normal, exceptional or asynchronous — on
        # which nothing owns it. The finalizer holds no reference to ``self``
        # (a Path and an int), or it could never become unreachable.
        self._finalizer = weakref.finalize(
            self, _remove_identity_store, self.directory, self.store_bytes
        )

    def __contains__(self, snap) -> bool:
        return int(snap) in self._layout

    def __getitem__(self, snap) -> Tuple[np.ndarray, np.ndarray]:
        snap = int(snap)
        if snap in self._resident:
            self._resident.move_to_end(snap)
            return self._resident[snap]
        if snap not in self._layout:
            raise KeyError(snap)
        offset, rows = self._layout[snap]
        pair = (
            self._read(self.forest_index_path, "ForestIndex", snap, offset, rows),
            self._read(self.ranks_path, "HaloRankInForest", snap, offset, rows),
        )
        self._resident[snap] = pair
        while len(self._resident) > RESIDENT_SNAPSHOTS:
            self._resident.popitem(last=False)
        return pair

    def resident_snapshots(self) -> Tuple[int, ...]:
        """Which snapshots are held right now — the working set a memory bound
        is asserted against, rather than inferred."""
        return tuple(self._resident)

    def close(self) -> None:
        """Drop the resident arrays and remove the backing directory.

        Raises no ordinary exception, deliberately: it runs on the way out of a
        FAILING link stage as well as a successful one, and an unlink error must
        not mask the failure that got the stage here. A removal that did not
        happen is reported through the module's log rather than passed over in
        silence — these are ``store_bytes`` that the storage envelope assumes
        are gone — and the store keeps its owner so the attempt is retried.

        Idempotent: a second call re-attempts nothing that already succeeded (an
        absent directory confirms as gone) and logs nothing.
        """
        self._resident.clear()
        if _remove_identity_store(self.directory, self.store_bytes):
            # Ownership is given up only once the bytes are CONFIRMED gone.
            # ``weakref.finalize`` is one-shot and marks itself dead BEFORE it
            # calls, so releasing through it — or detaching unconditionally —
            # would hand a failed or interrupted removal to nobody: no later
            # close(), no destruction and no interpreter exit would retry it.
            # Leaving it alive is what keeps the chain unbroken, at the price of
            # a second WARNING if the retry fails as well, which is a fact worth
            # printing twice rather than losing.
            self._finalizer.detach()

    def __enter__(self) -> "SnapshotIdentity":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    @staticmethod
    def _read(path: Path, what: str, snap: int, offset: int, rows: int) -> np.ndarray:
        if rows == 0:
            return np.empty(0, dtype=RANK_DTYPE)
        block = np.fromfile(path, dtype=RANK_DTYPE, count=rows, offset=offset * RANK_DTYPE.itemsize)
        if block.size != rows:
            raise ConverterError(
                "{}: holds {} of the {} {} value(s) snapshot {} needs".format(
                    path, block.size, rows, what, snap
                )
            )
        return block


def _check_fixed_dtype_tag(manifest: Manifest, path: str) -> None:
    """The fixed-file dtype tag is part of the deal: a scratch file written by a
    different converter revision must not be ranked or linked."""
    tag = manifest.data["intermediates"][path].get("dtype_tag")
    if tag != FIXED_DTYPE_TAG:
        raise ConverterError(
            "{}: fixed-file dtype tag {!r} != expected {!r} — refusing to link".format(
                path, tag, FIXED_DTYPE_TAG
            )
        )


def _iter_identity_blocks(
    manifest: Manifest,
    snaps,
    forest_table: np.ndarray,
    fi_handle,
    layout: Dict[int, Tuple[int, int]],
    chunk_rows: int,
):
    """Feed the rank core one bounded block of fixed records at a time, deriving
    and persisting ForestIndex as it goes.

    Blocks are yielded snapshot-ascending, each in slab order, so a record's
    global position is exactly the position the in-memory formulation's
    concatenation gave it — which is what makes the two orderings identical.
    ``layout`` is filled in with each snapshot's ``(offset, rows)`` window into
    the identity stores, and each snapshot's fixed file is dtype-tag-checked and
    checksum-verified before a byte of it is read.
    """
    position = 0
    for snap in snaps:
        entry = manifest.data["snapshots"][str(snap)]
        path = entry["fixed_file"]
        _check_fixed_dtype_tag(manifest, path)
        manifest.verify_intermediate(path, "fixed snapshot scratch")
        rows = 0
        with open(path, "rb") as handle:
            while True:
                records = np.fromfile(handle, dtype=FIXED_RECORD_DTYPE, count=chunk_rows)
                if records.size == 0:
                    break
                forest_index = np.searchsorted(forest_table, records["forest_id"])
                # written through the buffer as a memoryview: no whole-chunk
                # bytes copy, and a no-op cast on a little-endian host
                fi_handle.write(np.ascontiguousarray(forest_index, dtype=RANK_DTYPE).data)
                rows += records.size
                yield snap, records
        if rows != entry["rows"]:
            raise ConverterError(
                "{}: has {} rows, manifest records {}".format(path, rows, entry["rows"])
            )
        layout[snap] = (position, rows)
        position += rows


def _max_rank(rank_column, budget_bytes: int) -> int:
    """``max_halo_rank_in_forest`` for the manifest, streamed from the stored
    ranks column so it is derived from the values the link stage will write, not
    from the sort core's own bookkeeping. -1 for an empty run, exactly as
    ``int(ranks.max()) if total else -1`` gave before."""
    chunk_rows = max(1, (budget_bytes // STREAM_BUDGET_SHARE) // RANK_DTYPE.itemsize)
    max_rank = -1
    for block in rank_column.chunks(chunk_rows):
        max_rank = max(max_rank, int(block.max()))
    return max_rank


def compute_identity(
    manifest: Manifest, *, budget_bytes: int = DEFAULT_RANK_BUDGET_BYTES
) -> Tuple[SnapshotIdentity, int, int]:
    """Rank pass (conversion plan Phase 3 step 9) under an explicit memory
    budget: HaloRankInForest per forest in reference tree-driver order over all
    snapshots — (snapshot descending, upid, pid, id ascending) on post-fix
    values — plus the dense ForestIndex from the Phase 0 table.

    The ordering comes from the external merge-sort core (``rank_sort``), which
    reproduces the in-memory ``np.lexsort`` formulation's global order exactly
    while holding only its budget in records; ForestIndex is
    ``np.searchsorted(forest_table, forest_id)``, which needs no global pass and
    is computed per snapshot. Both columns are written to on-disk arrays indexed
    by global position, and only the snapshots the link stage is working on are
    read back.

    Returns ``(identity accessor, n_forests_total, max_halo_rank_in_forest)``.
    **The caller owns the accessor and must ``close()`` it**: the two stores are
    per-invocation scratch under the workdir, deliberately not manifest
    intermediates, and every failure path here removes them before raising.
    """
    _validate_monotonic_pairs(manifest)
    stream_bytes = max(1, budget_bytes // STREAM_BUDGET_SHARE)
    core_bytes = budget_bytes - stream_bytes
    if core_bytes < MIN_BUDGET_BYTES or stream_bytes < IDENTITY_STREAM_BYTES_PER_ROW:
        raise ConverterError(
            "rank-pass memory budget of {} byte(s) is too small: the external merge core needs "
            "at least {} byte(s) and the identity stream at least {} byte(s) for one row".format(
                budget_bytes, MIN_BUDGET_BYTES, IDENTITY_STREAM_BYTES_PER_ROW
            )
        )
    table_path = Path(manifest.workdir) / "forest_index_table.npy"
    manifest.verify_intermediate(table_path, "forest index table")
    forest_table = np.load(table_path)
    snaps = sorted(int(s) for s in manifest.data["snapshots"])
    scratch_dir = Path(manifest.data["snapshots"][str(snaps[0])]["fixed_file"]).parent
    # set before the store exists, so that the handler below can report a size
    # on a late failure and say "unknown" on an early one, and so that NOTHING
    # sits between the mkdtemp and the try
    store_bytes = None
    directory = Path(tempfile.mkdtemp(prefix=IDENTITY_DIR_PREFIX, dir=str(scratch_dir)))
    try:
        forest_index_path = directory / FOREST_INDEX_STORE_NAME
        ranks_path = directory / RANKS_STORE_NAME
        chunk_rows = max(1, stream_bytes // IDENTITY_STREAM_BYTES_PER_ROW)
        layout: Dict[int, Tuple[int, int]] = {}
        with open(forest_index_path, "wb") as fi_handle:
            blocks = _iter_identity_blocks(
                manifest, snaps, forest_table, fi_handle, layout, chunk_rows
            )
            try:
                result = rank_forests(
                    blocks, ranks_path, budget_bytes=core_bytes, spill_dir=directory
                )
            except RankSortError as exc:
                raise ConverterError("identity pass: {}".format(exc)) from exc
            finally:
                blocks.close()

        if not np.array_equal(result.forest_ids, forest_table):
            missing = np.setdiff1d(forest_table, result.forest_ids)
            extra = np.setdiff1d(result.forest_ids, forest_table)
            raise ConverterError(
                "observed forests do not match the Phase 0 forest index table: {} listed "
                "forest(s) have no halos (examples: {}), {} observed forest(s) are unlisted "
                "(examples: {})".format(
                    missing.size, missing[:5].tolist(), extra.size, extra[:5].tolist()
                )
            )
        n_forests_total = int(forest_table.size)
        total = sum(rows for _, rows in layout.values())
        if total != result.total_records:
            raise ConverterError(
                "identity pass: ranked {} record(s) but the snapshot windows cover {}".format(
                    result.total_records, total
                )
            )
        expected_bytes = total * RANK_DTYPE.itemsize
        for path in (forest_index_path, ranks_path):
            actual = path.stat().st_size
            if actual != expected_bytes:
                raise ConverterError(
                    "{}: identity store is {} bytes, expected {} ({} rows x {} bytes)".format(
                        path, actual, expected_bytes, total, RANK_DTYPE.itemsize
                    )
                )
        # from here on the store's size is known, so a failed removal can name
        # it: this is the earliest point at which the number is true
        store_bytes = 2 * expected_bytes
        peak_spill_bytes = int(result.peak_spill_bytes)
        n_runs, n_merge_passes = int(result.n_runs), int(result.n_merge_passes)
        merge_records = int(result.merge_records)
        # the per-forest arrays on the result are O(number of forests) and are
        # not needed again; the verifier below builds its own from the stored
        # ForestIndex column, deliberately independently of the core's grouping
        del result, forest_table

        fi_column = _Int64Column(forest_index_path, total)
        rank_column = _Int64Column(ranks_path, total)
        verify_identity(
            fi_column, rank_column, n_forests_total, "identity pass", budget_bytes=budget_bytes
        )
        max_rank = _max_rank(rank_column, budget_bytes)
        identity = SnapshotIdentity(
            directory,
            layout,
            peak_spill_bytes=peak_spill_bytes,
            store_bytes=store_bytes,
            n_runs=n_runs,
            n_merge_passes=n_merge_passes,
            merge_records=merge_records,
        )
        _log(
            "links: rank pass — {} halo(s) over {} snapshot(s), {} forest(s); budget {} B "
            "({} sorted run(s), {} merge pass(es)); peak spill {} B, identity stores {} B "
            "on disk".format(
                total,
                len(snaps),
                n_forests_total,
                budget_bytes,
                n_runs,
                n_merge_passes,
                peak_spill_bytes,
                identity.store_bytes,
            )
        )
        return identity, n_forests_total, max_rank
    except BaseException:
        # EVERY statement between the mkdtemp above and this return is inside
        # this guard, the setup lines, the success log and the return itself
        # included: the store is this call's to remove until the caller holds
        # the accessor, and the caller cannot hold it before the return
        # completes. Logging is the case that made the rule explicit — a bare
        # print raises BrokenPipeError on a closed pipe and ENOSPC on a full
        # volume, which is exactly what this storage-constrained pass invites,
        # and stranding the store then would leave 16 B/halo of scratch that
        # nothing else in the converter owns. Removal goes through the shared
        # helper so this branch reports a failed removal exactly as close()
        # does, and so that cleaning up after a failure cannot raise and
        # replace the exception that explains the run.
        _remove_identity_store(directory, store_bytes)
        raise


def link_one_snapshot(
    manifest: Manifest,
    snap: int,
    identity: SnapshotIdentity,
) -> None:
    """Link one snapshot: FoF chains, descendant merge-join, progenitor
    chains, pending FirstProgenitor hand-off, and identity carry-through.

    ``identity`` is the accessor :func:`compute_identity` returns; this function
    asks it only for snapshot ``snap`` and its successor, which is what bounds
    the accessor's residency."""
    entry = manifest.data["snapshots"][str(snap)]
    if entry.get("status") == "linked":
        verify_links_output(manifest, snap)
        return

    records = _load_fixed(manifest, snap)
    context = entry["fixed_file"]
    scratch_dir = Path(entry["fixed_file"]).parent
    validate_slab(records, snap, context)
    order = reference_order(records)
    first_fof, next_fof = build_fof_chains(records, order, snap, context)

    next_entry = manifest.data["snapshots"].get(str(snap + 1))
    if next_entry is not None:
        manifest.verify_intermediate(next_entry["index_file"], "snapshot id index")
        next_ids = np.fromfile(next_entry["index_file"], dtype=np.int64)
        if len(next_ids) != next_entry["rows"]:
            raise ConverterError(
                "{}: has {} ids, manifest records {}".format(
                    next_entry["index_file"], len(next_ids), next_entry["rows"]
                )
            )
    else:
        next_ids = np.empty(0, dtype=np.int64)

    desc = build_descendants(records, next_ids, snap, context)
    if next_entry is not None:
        verify_descendant_forests(
            desc, identity[snap][0], identity[snap + 1][0], records, snap, context
        )
    next_prog, pending_fp = build_progenitor_links(records, order, desc, next_ids.size)

    prev_entry = manifest.data["snapshots"].get(str(snap - 1))
    if prev_entry is not None:
        pending_path = scratch_dir / pending_fp_name(snap)
        manifest.verify_intermediate(pending_path, "pending first-progenitor buffer")
        first_prog = np.fromfile(pending_path, dtype=np.int32)
        if len(first_prog) != records.size:
            raise ConverterError(
                "{}: pending buffer has {} values, snapshot {} has {} halos".format(
                    pending_path, len(first_prog), snap, records.size
                )
            )
    else:
        # no data at snapshot snap-1 (start of the run or an empty snapshot):
        # nothing can have progenitors here
        first_prog = np.full(records.size, -1, dtype=np.int32)

    forest_index, ranks = identity[snap]
    if forest_index.size != records.size:
        raise ConverterError(
            "{}: identity pass produced {} rows for snapshot {}, expected {}".format(
                context, forest_index.size, snap, records.size
            )
        )

    links = np.zeros(records.size, dtype=LINKS_RECORD_DTYPE)
    links["Descendant"] = desc
    links["FirstProgenitor"] = first_prog
    links["NextProgenitor"] = next_prog
    links["FirstHaloInFOFgroup"] = first_fof
    links["NextHaloInFOFgroup"] = next_fof
    links["ForestIndex"] = forest_index
    links["HaloRankInForest"] = ranks

    if next_entry is not None:
        pending_next = scratch_dir / pending_fp_name(snap + 1)
        pending_fp.tofile(pending_next)
        reread_pending = np.fromfile(pending_next, dtype=np.int32)
        if not np.array_equal(reread_pending, pending_fp):
            raise ConverterError(
                "{}: pending buffer re-read does not match what was written".format(pending_next)
            )
        manifest.register_intermediate(
            pending_next, "pending-first-progenitor", rows=int(pending_fp.size), dtype_tag="<i4"
        )

    links_path = scratch_dir / links_scratch_name(snap)
    links.tofile(links_path)
    reread = np.fromfile(links_path, dtype=LINKS_RECORD_DTYPE)
    if not np.array_equal(reread, links):
        raise ConverterError(
            "{}: links file re-read does not match what was written".format(links_path)
        )
    manifest.register_intermediate(
        links_path, "snapshot-links", rows=int(links.size), dtype_tag=LINKS_DTYPE_TAG
    )
    entry["links_file"] = str(links_path.resolve())
    entry["status"] = "linked"
    manifest.save()
    _log(
        "links: snapshot {} — {} rows, {} FoF group(s), {} descendant link(s), "
        "{} progenitor sibling link(s)".format(
            snap,
            records.size,
            int((first_fof == np.arange(records.size, dtype=np.int32)).sum()),
            int((desc != -1).sum()),
            int((next_prog != -1).sum()),
        )
    )


def _no_consumer_indexes(manifest: Manifest, snaps) -> List[str]:
    """The id index files no ``link_one_snapshot`` call will ever read.

    ``link_one_snapshot(snap)`` resolves descendants against snapshot
    ``snap + 1``'s ``index_file``, so ``snap_NNN.idx``'s consumer is the link of
    the snapshot BELOW it — the relation runs the opposite way from the
    intuition, and the highest snapshot's index is consumed perfectly normally.
    What has no consumer is an index whose predecessor is not in the recorded
    snapshot set, because linking iterates only recorded snapshots: ``idx_0``
    always, and every index across a gap in that set. Those are deletable as
    soon as linking starts (plan Slice 8 deletion table).
    """
    recorded = set(snaps)
    return [
        manifest.data["snapshots"][str(snap)]["index_file"]
        for snap in snaps
        if snap - 1 not in recorded
    ]


def _consumed_by_link(manifest: Manifest, snap: int, recorded) -> List[str]:
    """The intermediates ``link_one_snapshot(snap)`` was the terminal consumer
    of, named once that snapshot's links file is verified, registered and saved.

    At most two, and deliberately never the fixed file: ``fixed`` is read again
    by the writer (``_load_snapshot_scratch``), which is its terminal consumer,
    so it must not be deleted here. What ends here is snapshot ``snap``'s
    pending first-progenitor buffer, written by the link of ``snap - 1`` and
    read once, and snapshot ``snap + 1``'s id index, read once to resolve
    descendants. Either is absent when the neighbouring snapshot is not
    recorded, which is why each is guarded separately rather than assumed.
    """
    entry = manifest.data["snapshots"][str(snap)]
    scratch_dir = Path(entry["fixed_file"]).parent
    paths: List[str] = []
    if snap - 1 in recorded:
        paths.append(str(scratch_dir / pending_fp_name(snap)))
    if snap + 1 in recorded:
        paths.append(manifest.data["snapshots"][str(snap + 1)]["index_file"])
    return paths


def verify_links_output(manifest: Manifest, snap: int) -> None:
    """Verify one snapshot's links file the way a skip-trust must: manifest
    ownership and content checksum, the frozen dtype tag, the recorded row
    count, and the on-disk size those rows imply.

    This is the successor half of the delete-after-verify protocol for
    everything the link stage consumes. Both places that trust an already-linked
    snapshot call it — ``link_one_snapshot``'s skip path, and the short-circuit
    in :func:`run_links`, which reaches the drains without going through
    ``link_one_snapshot`` at all. A links file that is missing or tampered must
    stop the drain there exactly as it stops the skip here, or the stage would
    delete an index and a pending buffer whose successor it never checked.
    """
    entry = manifest.data["snapshots"][str(snap)]
    meta = manifest.verify_intermediate(entry["links_file"], "snapshot links scratch")
    if meta.get("dtype_tag") != LINKS_DTYPE_TAG:
        raise ConverterError(
            "{}: links-file dtype tag {!r} != current {!r} — refusing to skip-trust an "
            "artifact from a different converter revision".format(
                entry["links_file"], meta.get("dtype_tag"), LINKS_DTYPE_TAG
            )
        )
    if meta.get("rows") != entry["rows"]:
        raise ConverterError(
            "{}: links file records {} rows, snapshot manifest records {}".format(
                entry["links_file"], meta.get("rows"), entry["rows"]
            )
        )
    expected_bytes = entry["rows"] * LINKS_RECORD_DTYPE.itemsize
    actual_bytes = Path(entry["links_file"]).stat().st_size
    if actual_bytes != expected_bytes:
        raise ConverterError(
            "{}: links file is {} bytes, expected {} ({} rows x {} bytes)".format(
                entry["links_file"],
                actual_bytes,
                expected_bytes,
                entry["rows"],
                LINKS_RECORD_DTYPE.itemsize,
            )
        )


def _verify_links_outputs_before_draining(manifest: Manifest, snaps) -> None:
    """Verify every links output the short-circuit is about to delete behind.

    A links file the writer has already consumed is accepted on the record
    without being re-read — its own successor, the emitted HDF5, was verified
    dataset-by-dataset when the writer took it, and re-reading a file that is
    deliberately gone is not possible anyway. Every other snapshot's links file
    is still supposed to be on disk, and is checked.
    """
    for snap in snaps:
        entry = manifest.data["snapshots"][str(snap)]
        if manifest.is_consumed(entry["links_file"]):
            continue
        verify_links_output(manifest, snap)


def _consume_unreachable_indexes(manifest: Manifest, snaps, delete: bool) -> None:
    """Drain the indexes that have no consumer, reporting each removal."""
    for path in manifest.consume_intermediates(
        _no_consumer_indexes(manifest, snaps), delete=delete
    ):
        _log("links: consumed {} — no recorded snapshot below it reads it".format(path))


def _consume_after_link(manifest: Manifest, snap: int, recorded, delete: bool) -> None:
    """Drain what the link of ``snap`` was the terminal consumer of, reporting
    each removal. Idempotent, so it is safe to call for a snapshot linked by an
    earlier run — which is what lets the short-circuit below finish a
    consumption that a flag-off link run, or an interrupted writer, deferred."""
    for path in manifest.consume_intermediates(
        _consumed_by_link(manifest, snap, recorded), delete=delete
    ):
        _log("links: snapshot {} — consumed {}".format(snap, path))


def _consumed_fixed_snapshots(manifest: Manifest, snaps) -> List[int]:
    """Snapshots whose fixed scratch the writer has already consumed."""
    return [
        snap
        for snap in snaps
        if manifest.is_consumed(manifest.data["snapshots"][str(snap)]["fixed_file"])
    ]


def run_links(
    workdir,
    *,
    budget_bytes: int = DEFAULT_RANK_BUDGET_BYTES,
    consume_intermediates: bool = False,
) -> Manifest:
    """Run the link stage over every fixed snapshot, in ascending order.

    Snapshot subsets are deliberately not supported: FirstProgenitor values
    flow forward through the pending buffer, so snapshots must be linked in
    order. Snapshots already linked are verified and skipped; the run-scoped
    identity values are recomputed and must match what a previous run
    recorded (refuse-not-repair).

    ``budget_bytes`` is the rank/identity pass's working-memory budget (CLI:
    ``links --memory-budget-mb``). It changes how much of the pass is resident
    and nothing else: every value written is identical at any budget.

    ``consume_intermediates`` (CLI: ``--consume-intermediates``) turns on the
    plan Slice 8 deletion of each link's own consumed predecessors — the pending
    first-progenitor buffers and the id indexes, never the fixed or links files,
    whose terminal consumer is the writer. It is off by default and changes no
    emitted byte.
    """
    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))
    snaps = sorted(int(s) for s in manifest.data["snapshots"])
    if not snaps:
        raise ConverterError("{}: manifest records no snapshots".format(workdir))
    for snap in snaps:
        status = manifest.data["snapshots"][str(snap)].get("status")
        if status not in ("fixed", "linked"):
            raise ConverterError(
                "snapshot {}: unexpected status {!r}; run fixups first".format(snap, status)
            )

    # Once the writer has consumed the fixed inputs, the rank pass cannot run
    # again: it streams every snapshot's fixed file. Re-running a fully linked
    # stage in that state is a skip, not a verification failure — the outputs
    # are complete and the inputs were deliberately released. This is
    # conditioned on the inputs actually being recorded as consumed, NOT on
    # every snapshot merely being linked: while those inputs are still present
    # a links re-run remains reachable, and the refuse-not-repair comparison of
    # the run-scoped identity values below has to keep running.
    consumed_fixed = _consumed_fixed_snapshots(manifest, snaps)
    if consumed_fixed and all(
        manifest.data["snapshots"][str(snap)].get("status") == "linked" for snap in snaps
    ):
        _log(
            "links: every snapshot is already linked and the fix-up scratch of {} of {} "
            "snapshot(s) was consumed by the write stage (e.g. {}) — skipping the rank "
            "pass".format(
                len(consumed_fixed),
                len(snaps),
                manifest.data["snapshots"][str(consumed_fixed[0])]["fixed_file"],
            )
        )
        # Returning here must not skip this stage's OWN deletions. Reaching
        # this point means every snapshot is linked, so every consumption point
        # in the table below is provably past — but they may never have run:
        # `links` may have run with the flag off and the operator turned it on
        # only now, or a writer run may have been interrupted after consuming
        # some fixed files. Draining both sets is what keeps the deletion table
        # whole rather than partial; ``consume_intermediates`` is idempotent, so
        # a set already drained costs nothing.
        #
        # Verify first, delete second — the protocol, unchanged by the fact
        # that this path never calls ``link_one_snapshot``. Without this the
        # short-circuit would delete an index and a pending buffer whose
        # successor it had not looked at, which is precisely what the normal
        # path's skip-trust prevents.
        _verify_links_outputs_before_draining(manifest, snaps)
        _consume_unreachable_indexes(manifest, snaps, consume_intermediates)
        recorded = set(snaps)
        for snap in snaps:
            _consume_after_link(manifest, snap, recorded, consume_intermediates)
        return manifest

    identity, n_forests_total, max_rank = compute_identity(manifest, budget_bytes=budget_bytes)
    # ``with`` is the deterministic release, not the ownership: the accessor
    # owns its store from construction (see SnapshotIdentity), so an exception
    # landing between the call above and this line cannot strand it
    with identity:
        recorded = manifest.data.get("links")
        computed = {"n_forests_total": n_forests_total, "max_halo_rank_in_forest": max_rank}
        if recorded is not None and recorded != computed:
            raise ConverterError(
                "run-scoped identity values changed across runs: manifest records {}, "
                "recomputed {} — refusing to mix link outputs".format(recorded, computed)
            )
        manifest.data["links"] = computed
        manifest.save()

        recorded = set(snaps)
        _consume_unreachable_indexes(manifest, snaps, consume_intermediates)

        for snap in snaps:
            link_one_snapshot(manifest, snap, identity)
            _consume_after_link(manifest, snap, recorded, consume_intermediates)
    _log(
        "links: {} snapshot(s) linked — n_forests_total={}, max_halo_rank_in_forest={}".format(
            len(snaps), n_forests_total, max_rank
        )
    )
    return manifest
