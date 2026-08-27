"""External merge-sort rank core for the ctrees -> snapshot-HDF5 converter
(CONVERTER-SCALE-PASS-PLAN.md Slice 4).

The shipped rank pass (``links.compute_identity``) concatenates five int64 key
columns over *all* snapshots, runs one global ``np.lexsort`` and ranks within
forest groups. Measured 187.84 B/halo — 4.30 TB at the 22.9e9-halo Shin-Uchuu
production scale, 8.4x installed RAM. This module produces the **identical
global ordering** under an explicit memory budget: bounded chunks are sorted
and spilled to disk as sorted runs, then k-way merged while
``HaloRankInForest`` is assigned in one streaming pass over the merged key
order.

**Key order** (the reference tree-driver order, ``ctrees_utils.c:524-547``, and
exactly the order ``links.compute_identity`` builds with
``np.lexsort((ids, pid, upid, neg_snap, forest))``)::

    forest_id ascending, snap DESCENDING, upid ascending, pid ascending,
    id ascending, and finally the record's global position ascending

on post-fix values. The trailing position makes the order *total*, which is
what reproduces ``np.lexsort``'s stability without depending on a sort
algorithm being stable: two records with an identical five-field key are
ordered by input position in both formulations.

**Rank definition** (unchanged): ``HaloRankInForest`` is the 0-based index of a
record within its forest in that order, so ranks are dense over
``0 .. count-1`` for every forest.

**Inputs.** ``rank_forests`` consumes an iterable of ``(snap, records)`` blocks.
``snap`` is one int64-valued snapshot number for the whole block; ``records`` is
a numpy structured array carrying at least the fields ``forest_id``, ``upid``,
``pid`` and ``id``, **each exactly int64** — any other dtype is rejected, never
coerced (a silently truncated int32 id would corrupt every rank downstream).
Extra fields are ignored, so a ``fixups.FIXED_RECORD_DTYPE`` array can be handed
over as-is. A snapshot may be delivered as several consecutive blocks (the
memory budget bounds this module, not its caller) and a snapshot contributing
zero records may be delivered as an empty block or omitted entirely. **A
record's global position is its index in the concatenation of the blocks in
iteration order** — so a caller that feeds snapshots in ascending order, each in
slab order, gets exactly the positions ``compute_identity`` assigns today.

**Outputs.** Ranks are written to ``ranks_path`` as a flat int64 array indexed by
global position (the caller memory-maps or slices it per snapshot); the
per-forest group boundaries — observed forest ids, ascending, and their halo
counts — are returned in memory on the :class:`RankSortResult`, together with
the peak spill bytes the run held on disk.

**Memory.** ``budget_bytes`` bounds the working memory this module allocates,
and it is bounded in *bytes* rather than in records because the working set is
not made of records alone: a merge holds each record twice over (once in a run
buffer, once in the ready block gathered out of those buffers) and the rank
pass derives int64 and bool scratch from the ready block. Both per-record costs
are counted, not estimated — see :data:`GEN_BYTES_PER_RECORD` and
:data:`MERGE_BYTES_PER_RECORD` — and every allocation is reported to a meter
whose high-water marks come back as ``peak_resident_bytes`` and
``peak_resident_records``, so a caller can *assert* the bound rather than trust
it. Run generation and merging do not overlap, so each phase gets the whole
budget.

Three allocations sit outside that bound by construction and are stated here
rather than hidden: the returned group boundaries are O(number of forests), not
O(number of records); the ranks backing store is a memory-mapped file whose
resident pages are page cache the kernel may reclaim; and numpy's own sort
scratch inside ``ndarray.sort`` is not visible to this module (it sorts in
place, so it is small, but it is not zero).

**Spill lifetime.** This module creates its spill files in a private directory
of its own making and removes every one of them itself, on the success path
(after the ranks have been written and verified) and on every failure path.
No caller deletes a spill file and spills are never registered as manifest
intermediates. A partially written ranks file is removed on failure too.

numpy + stdlib only. Deliberately not a general sorting library: it exists for
this one key.
"""

import contextlib
import os
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, List, Sequence, Tuple

import numpy as np

#: Spill record layout. The field ORDER is the key order: numpy compares
#: structured scalars field by field in dtype order, so ``ndarray.sort`` and
#: ``np.searchsorted`` on this dtype are lexicographic in exactly the reference
#: order. ``neg_snap`` is ``-snap``, which turns the reference's descending
#: snapshot into an ascending field; ``position`` is the record's global input
#: position and makes the order total.
SPILL_DTYPE = np.dtype(
    [
        ("forest_id", "<i8"),
        ("neg_snap", "<i8"),
        ("upid", "<i8"),
        ("pid", "<i8"),
        ("id", "<i8"),
        ("position", "<i8"),
    ],
    align=False,
)

#: Bytes per spilled record (six int64 = the plan's analytic 48 B/halo).
SPILL_RECORD_NBYTES = SPILL_DTYPE.itemsize

#: Caller-supplied key fields, which must each be exactly int64.
KEY_ARRAY_FIELDS = ("forest_id", "upid", "pid", "id")

#: Ranks backing-store element type and the sentinel every slot is initialised
#: to, so verification can prove that every position was written exactly once.
RANK_DTYPE = np.dtype("<i8")
RANK_UNWRITTEN = -1

#: Working bytes per record during run generation: the spill record itself in
#: the chunk being filled (48), plus one int64 of the position ramp the chunk is
#: numbered from (8). The ramp is allocated once and reused, so this is the
#: whole cost.
GEN_BYTES_PER_RECORD = SPILL_RECORD_NBYTES + 8

#: Working bytes per record of scratch the rank pass derives from one merged
#: block: a bool mask (1) and, in the worst case of one forest per record, six
#: int64 arrays (48) — group ids, ranks, group starts, group counts, the
#: gathered starts, and the contiguous index copy numpy makes to scatter into
#: the memory-mapped store. Reserved at the worst case before the first of them
#: is allocated, so the meter never reports less than is held.
SCRATCH_BYTES_PER_RECORD = 1 + 6 * 8

#: Working bytes per record of a merge's buffered working set: the record in a
#: run buffer (48), the same record again in the ready block gathered out of
#: those buffers (48), and the rank pass's scratch (49). The merge holds no
#: other record-sized allocation — the boundary key is a VIEW into the buffer
#: that supplied it, never a copy.
MERGE_BYTES_PER_RECORD = 2 * SPILL_RECORD_NBYTES + SCRATCH_BYTES_PER_RECORD

#: A k-way merge needs at least one buffered record per run, so two-way merging
#: — the narrowest useful merge — needs two.
MIN_MERGE_RECORDS = 2
MIN_BUDGET_BYTES = MIN_MERGE_RECORDS * MERGE_BYTES_PER_RECORD

#: Upper bound on merge fan-in. Two independent limits meet here: every merged
#: run holds an open file descriptor (macOS's default soft NOFILE is 256), and
#: each merge iteration does Python-level work proportional to the fan-in. The
#: effective cap is also bounded by the budget, since each run needs at least
#: one resident record. Exceeding it costs merge passes, never correctness.
MAX_MERGE_FANIN = 512
_RESERVED_FDS = 32

#: Modulus for the verification's second moment. A Mersenne prime below 2**31,
#: so a reduced rank squares inside int64 (< 2**62) and a block of reduced
#: squares sums inside int64 for any block this module reads.
_MOMENT_MODULUS = 2**31 - 1


class RankSortError(RuntimeError):
    """Raised for every contract violation and verification failure in this
    module: a rejected input dtype, an impossible budget, a spill file that
    does not read back as written, or a ranks array that does not verify.

    A ``RuntimeError`` subclass, like ``ctrees_parser.ConverterError``, but
    deliberately independent of it — this core is self-contained, so a caller
    inside the converter translates it at its own boundary.
    """


@dataclass(frozen=True)
class RankSortResult:
    """What one ``rank_forests`` call produced, and what it cost.

    ``forest_ids`` are the observed forests in ascending order and
    ``forest_counts`` their halo counts in the same order — the per-forest group
    boundaries, equivalent to the ``starts`` array the in-memory formulation
    derives (see :meth:`forest_starts`). ``ranks_path`` holds ``total_records``
    int64 ranks indexed by global input position.

    ``peak_spill_bytes`` is the high-water mark of live spill bytes on disk, and
    ``ranks_bytes`` the size of the backing store, for the pass's storage
    envelope.

    ``peak_resident_bytes`` and ``peak_resident_records`` are measured
    high-water marks of the working memory and the record buffers this module
    held; the first is bounded by ``budget_bytes`` and the second by
    ``budget_records``. ``run_records`` is how many records one generated run
    holds and ``merge_records`` how many a merge buffers at once — both derived
    from the budget, and reported so a caller can see what a budget bought.
    """

    total_records: int
    n_forests: int
    forest_ids: np.ndarray
    forest_counts: np.ndarray
    max_rank: int
    ranks_path: str
    ranks_bytes: int
    peak_spill_bytes: int
    peak_resident_bytes: int
    peak_resident_records: int
    budget_bytes: int
    budget_records: int
    run_records: int
    merge_records: int
    n_runs: int
    n_merge_passes: int
    merge_fanin: int

    def forest_starts(self) -> np.ndarray:
        """Offset of each forest's first record in the merged key order."""
        starts = np.zeros(self.forest_counts.size, dtype=np.int64)
        if starts.size:
            np.cumsum(self.forest_counts[:-1], out=starts[1:])
        return starts


def open_ranks(ranks_path, mode: str = "r") -> np.ndarray:
    """Open a ranks backing store written by :func:`rank_forests`.

    Returns an int64 array indexed by global input position — a memory map for
    a non-empty store, so a caller resident-set stays bounded by the slices it
    touches.
    """
    ranks_path = Path(ranks_path)
    size = ranks_path.stat().st_size
    if size % RANK_DTYPE.itemsize:
        raise RankSortError(
            "{}: {} bytes is not a whole number of int64 ranks".format(ranks_path, size)
        )
    if size == 0:
        return np.empty(0, dtype=RANK_DTYPE)
    return np.memmap(str(ranks_path), dtype=RANK_DTYPE, mode=mode, shape=(size // 8,))


def rank_forests(
    blocks: Iterable[Tuple[int, np.ndarray]],
    ranks_path,
    *,
    budget_bytes: int,
    spill_dir=None,
) -> RankSortResult:
    """Rank every record within its forest in reference tree-driver order,
    under an explicit memory budget.

    ``blocks`` is an iterable of ``(snap, records)`` — see the module docstring
    for the record contract and how global positions are assigned. Ranks are
    written to ``ranks_path``; spill files go in a private directory created
    under ``spill_dir`` (default: the ranks file's own directory) and are
    removed before this call returns, whether it returns or raises.
    """
    budget_bytes = int(budget_bytes)
    if budget_bytes < MIN_BUDGET_BYTES:
        raise RankSortError(
            "memory budget of {} byte(s) is below the {} a two-way merge needs ({} record(s) at "
            "{} B/record of working set)".format(
                budget_bytes, MIN_BUDGET_BYTES, MIN_MERGE_RECORDS, MERGE_BYTES_PER_RECORD
            )
        )
    run_records = max(1, budget_bytes // GEN_BYTES_PER_RECORD)
    merge_records = max(MIN_MERGE_RECORDS, budget_bytes // MERGE_BYTES_PER_RECORD)
    ranks_path = Path(ranks_path)
    spill_root = Path(spill_dir) if spill_dir is not None else ranks_path.parent
    spills = _Spills(tempfile.mkdtemp(prefix="rank_spill_", dir=str(spill_root)))
    residency = _Residency()
    ranks_created = False
    try:
        runs, total = _generate_runs(blocks, spills, run_records, residency)
        n_runs = len(runs)
        fanin_cap = _fanin_cap(merge_records)
        runs, n_passes = _reduce_runs(runs, spills, fanin_cap, merge_records, residency)
        ranks_created = True
        forest_ids, forest_counts, max_rank = _assign_ranks(
            runs, ranks_path, total, merge_records, residency
        )
        _verify_ranks(ranks_path, total, forest_counts, max_rank, budget_bytes, residency)
        return RankSortResult(
            total_records=total,
            n_forests=int(forest_ids.size),
            forest_ids=forest_ids,
            forest_counts=forest_counts,
            max_rank=max_rank,
            ranks_path=str(ranks_path),
            ranks_bytes=total * RANK_DTYPE.itemsize,
            peak_spill_bytes=spills.peak_bytes,
            peak_resident_bytes=residency.peak_bytes,
            peak_resident_records=residency.peak_records,
            budget_bytes=budget_bytes,
            budget_records=budget_bytes // SPILL_RECORD_NBYTES,
            run_records=run_records,
            merge_records=merge_records,
            n_runs=n_runs,
            n_merge_passes=n_passes,
            merge_fanin=len(runs),
        )
    except BaseException:
        # a half-written ranks file is worthless and must never be mistaken for
        # a completed one; this call created it, so this call removes it
        if ranks_created:
            _unlink(ranks_path)
        raise
    finally:
        # the success path reaches here only after the ranks were written AND
        # verified above, which is the point at which the spills stop being
        # needed; every other path reaches it too
        spills.cleanup()


# --------------------------------------------------------------------------
# memory accounting
# --------------------------------------------------------------------------


class _Residency:
    """Working memory this module holds, and its high-water marks.

    Every allocation is reported here — record buffers in both bytes and
    records, derived int64/bool scratch in bytes — so ``peak_bytes`` is a
    measurement of what the core held rather than an argument about what it
    should have held. An instrument that only ever counts the buffers that were
    *sized* from the budget cannot reveal an overrun, which is the whole reason
    the scratch is counted too.
    """

    def __init__(self) -> None:
        self.bytes_current = 0
        self.bytes_peak = 0
        self.records_current = 0
        self.records_peak = 0

    def acquire(self, nbytes: int, records: int = 0) -> None:
        self.bytes_current += int(nbytes)
        self.records_current += int(records)
        if self.bytes_current > self.bytes_peak:
            self.bytes_peak = self.bytes_current
        if self.records_current > self.records_peak:
            self.records_peak = self.records_current

    def acquire_records(self, records: int) -> None:
        self.acquire(int(records) * SPILL_RECORD_NBYTES, int(records))

    def release(self, nbytes: int, records: int = 0) -> None:
        self.bytes_current -= int(nbytes)
        self.records_current -= int(records)
        if self.bytes_current < 0 or self.records_current < 0:  # pragma: no cover
            raise RankSortError(
                "residency accounting went negative ({} byte(s), {} record(s))".format(
                    self.bytes_current, self.records_current
                )
            )

    def release_records(self, records: int) -> None:
        self.release(int(records) * SPILL_RECORD_NBYTES, int(records))

    @property
    def peak_bytes(self) -> int:
        return self.bytes_peak

    @property
    def peak_records(self) -> int:
        return self.records_peak


# --------------------------------------------------------------------------
# spill files
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class _Run:
    """One sorted run on disk, bound to what was written: a record count and a
    CRC32 over its bytes, both re-checked as the run is read back. A run is
    written and re-read within a single call, so this in-memory binding is the
    whole provenance chain — but it is a binding, not an assumption."""

    path: Path
    n_records: int
    crc: int


class _Spills:
    """The private spill directory and its live-byte high-water mark."""

    def __init__(self, directory) -> None:
        self.directory = Path(directory)
        self.live_bytes = 0
        self.peak_bytes = 0
        self._sizes = {}
        self._counter = 0

    def new_path(self, tag: str) -> Path:
        self._counter += 1
        path = self.directory / "run_{:06d}_{}.bin".format(self._counter, tag)
        self._sizes[path] = 0
        return path

    def note_written(self, path: Path, nbytes: int) -> None:
        self._sizes[path] = self._sizes.get(path, 0) + nbytes
        self.live_bytes += nbytes
        if self.live_bytes > self.peak_bytes:
            self.peak_bytes = self.live_bytes

    def remove(self, path: Path) -> None:
        self.live_bytes -= self._sizes.pop(path, 0)
        _unlink(path)

    def cleanup(self) -> None:
        for path in list(self._sizes):
            self.remove(path)
        try:
            os.rmdir(self.directory)
        except OSError:
            # a foreign file in the directory is not one this core created, and
            # the failure path must not mask the exception that got us here
            pass


def _unlink(path) -> None:
    try:
        os.unlink(str(path))
    except FileNotFoundError:
        pass


class _RunWriter:
    """Streams records into one spill run, checksumming as it writes."""

    def __init__(self, spills: _Spills, tag: str) -> None:
        self.spills = spills
        self.path = spills.new_path(tag)
        self.handle = open(str(self.path), "wb")
        self.crc = 0
        self.n_records = 0

    def write(self, records: np.ndarray) -> None:
        records.tofile(self.handle)
        self.crc = zlib.crc32(records.data, self.crc)
        self.n_records += int(records.size)
        self.spills.note_written(self.path, int(records.size) * SPILL_RECORD_NBYTES)

    def finish(self) -> _Run:
        self.handle.close()
        return _Run(self.path, self.n_records, self.crc)

    def abort(self) -> None:
        self.handle.close()


class _RunReader:
    """Reads one sorted run back in bounded blocks, verifying its size on open
    and its CRC32 once the last record has been read."""

    def __init__(self, run: _Run, block_records: int) -> None:
        expected = run.n_records * SPILL_RECORD_NBYTES
        actual = os.path.getsize(str(run.path))
        if actual != expected:
            raise RankSortError(
                "{}: spill run is {} byte(s), expected {} for {} record(s)".format(
                    run.path, actual, expected, run.n_records
                )
            )
        self.run = run
        self.block_records = int(block_records)
        self.handle = open(str(run.path), "rb")
        self.remaining = int(run.n_records)
        self.crc = 0
        self.buf = np.empty(0, dtype=SPILL_DTYPE)
        self.offset = 0

    def refill(self, residency: _Residency) -> bool:
        """Drop the current block and load the next. False once the run is
        exhausted, at which point its CRC32 must match what was written."""
        residency.release_records(self.buf.size)
        self.buf = np.empty(0, dtype=SPILL_DTYPE)
        self.offset = 0
        if self.remaining <= 0:
            if self.crc != self.run.crc:
                raise RankSortError(
                    "{}: spill run read back with CRC32 {} but was written with {} — the "
                    "merge input does not match the run that was spilled".format(
                        self.run.path, self.crc, self.run.crc
                    )
                )
            return False
        count = min(self.block_records, self.remaining)
        block = np.fromfile(self.handle, dtype=SPILL_DTYPE, count=count)
        if block.size != count:
            raise RankSortError(
                "{}: spill run yielded {} of {} expected record(s)".format(
                    self.run.path, block.size, count
                )
            )
        self.crc = zlib.crc32(block.data, self.crc)
        self.remaining -= count
        self.buf = block
        residency.acquire_records(block.size)
        return True

    def close(self, residency: _Residency) -> None:
        residency.release_records(self.buf.size)
        self.buf = np.empty(0, dtype=SPILL_DTYPE)
        self.handle.close()


def _open_run_reader(run: _Run, block_records: int) -> _RunReader:
    """Indirection so a test can force a failure part-way through a merge."""
    return _RunReader(run, block_records)


# --------------------------------------------------------------------------
# run generation
# --------------------------------------------------------------------------


def _unpack_block(item, index: int) -> Tuple[int, np.ndarray]:
    try:
        snap, records = item
    except (TypeError, ValueError):
        raise RankSortError(
            "block {}: expected a (snap, records) pair, got {!r}".format(index, type(item).__name__)
        ) from None
    return snap, records


def _validate_block(snap, records, index: int) -> int:
    """Reject anything that is not the frozen input contract. Coercion is not
    an option here: an int32 id silently truncated to fit would reorder a
    forest, and every UniqueGalaxyID downstream derives from that order."""
    if isinstance(snap, bool) or not isinstance(snap, (int, np.integer)):
        raise RankSortError(
            "block {}: snap must be an integer, got {!r}".format(index, type(snap).__name__)
        )
    snap = int(snap)
    # int64.min is excluded DELIBERATELY, and the bound is strict for a reason:
    # the key stores -snap, and -(-2**63) is not representable as int64, so
    # admitting it would wrap silently and sort that snapshot to the wrong end.
    if not np.iinfo(np.int64).min < snap <= np.iinfo(np.int64).max:
        raise RankSortError(
            "block {}: snap {} is outside the int64 range this key can negate — the key stores "
            "-snap, so {} is excluded as well as anything beyond int64".format(
                index, snap, np.iinfo(np.int64).min
            )
        )
    if not isinstance(records, np.ndarray) or records.dtype.names is None:
        raise RankSortError(
            "block {}: records must be a numpy structured array, got {!r}".format(
                index, type(records).__name__
            )
        )
    if records.ndim != 1:
        raise RankSortError(
            "block {}: records must be 1-D, got shape {}".format(index, records.shape)
        )
    for field in KEY_ARRAY_FIELDS:
        if field not in records.dtype.names:
            raise RankSortError(
                "block {}: records are missing the key field {!r} (have {})".format(
                    index, field, ", ".join(records.dtype.names)
                )
            )
        dtype = records.dtype.fields[field][0]
        # compare on INTENT (signed, 8 bytes), not on the store dtype: a
        # genuinely-int64 array that happens to be big-endian is a valid input
        # and numpy converts it on the copy into SPILL_DTYPE
        if dtype.kind != "i" or dtype.itemsize != 8:
            raise RankSortError(
                "block {}: key field {!r} has dtype {} — every key field must be int64 and is "
                "never coerced".format(index, field, dtype.str)
            )
    return snap


def _generate_runs(
    blocks: Iterable[Tuple[int, np.ndarray]],
    spills: _Spills,
    run_records: int,
    residency: _Residency,
) -> Tuple[List[_Run], int]:
    """Fill one budget-sized chunk at a time, sort it, spill it as a run.

    The chunk and the position ramp it is numbered from are allocated once and
    reused, so run generation holds exactly ``run_records`` records however many
    blocks arrive and however large each one is: a caller block wider than the
    chunk is split across runs.
    """
    chunk = np.empty(run_records, dtype=SPILL_DTYPE)
    ramp = np.arange(run_records, dtype=np.int64)
    residency.acquire_records(run_records)
    residency.acquire(run_records * 8)
    try:
        runs: List[_Run] = []
        filled = 0
        position = 0
        for index, item in enumerate(blocks):
            snap, records = _unpack_block(item, index)
            snap = _validate_block(snap, records, index)
            count = int(records.size)
            taken = 0
            while taken < count:
                take = min(count - taken, run_records - filled)
                _copy_keys(chunk, ramp, filled, records, taken, take, snap, position + taken)
                filled += take
                taken += take
                if filled == run_records:
                    runs.append(_spill_run(chunk[:filled], spills))
                    filled = 0
            position += count
        if filled:
            runs.append(_spill_run(chunk[:filled], spills))
        return runs, position
    finally:
        residency.release(run_records * 8)
        residency.release_records(run_records)


def _copy_keys(
    chunk: np.ndarray,
    ramp: np.ndarray,
    at: int,
    records: np.ndarray,
    src: int,
    count: int,
    snap: int,
    position: int,
) -> None:
    """Copy one caller block's keys into the chunk, in spill layout.

    The position column is written from the preallocated ramp and shifted in
    place, so numbering a chunk allocates nothing.
    """
    target = chunk[at : at + count]
    source = records[src : src + count]
    target["forest_id"] = source["forest_id"]
    target["neg_snap"] = -snap
    target["upid"] = source["upid"]
    target["pid"] = source["pid"]
    target["id"] = source["id"]
    target["position"] = ramp[:count]
    target["position"] += position


def _spill_run(records: np.ndarray, spills: _Spills) -> _Run:
    """Sort one chunk into key order in place and write it out as a run."""
    records.sort()
    writer = _RunWriter(spills, "gen")
    try:
        writer.write(records)
    except BaseException:
        writer.abort()
        raise
    return writer.finish()


# --------------------------------------------------------------------------
# merging
# --------------------------------------------------------------------------


def _fanin_cap(merge_records: int) -> int:
    """Largest number of runs one merge pass may consume."""
    try:
        import resource

        soft = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
        by_files = MAX_MERGE_FANIN if soft <= 0 else int(soft) - _RESERVED_FDS
    except (ImportError, OSError, ValueError):  # pragma: no cover - platform fallback
        by_files = MAX_MERGE_FANIN
    # each run needs at least one buffered record, so the fan-in can never
    # exceed the records the budget lets a merge buffer at once
    return max(2, min(MAX_MERGE_FANIN, by_files, merge_records))


def _block_records(merge_records: int, fanin: int) -> int:
    """Records buffered per run in a merge of ``fanin`` runs.

    ``MERGE_BYTES_PER_RECORD`` already charges each buffered record for its
    second copy in the ready block and for the rank pass's scratch, so the
    buffered working set is divided by the fan-in alone.
    """
    return max(1, merge_records // fanin)


def _key_at(buf: np.ndarray, index: int) -> Tuple[int, ...]:
    """One record's full six-field key as a tuple of Python ints.

    numpy has no ``minimum`` loop for a void dtype, so the smallest of the
    buffered ceilings is found by comparing tuples; the boundary itself is then
    taken as a VIEW into the buffer that supplied it, so choosing it allocates
    no records.
    """
    return tuple(int(buf[name][index]) for name in SPILL_DTYPE.names)


def _lowest_ceiling(live: Sequence["_RunReader"]) -> np.ndarray:
    """The smallest of the buffered runs' last keys, as a one-record VIEW.

    Every buffered run's last key is a ceiling on what that run can still
    deliver from disk, so the smallest of them bounds the merged prefix that is
    already complete. The result is a view into the buffer that supplied it,
    never a copy: a merge iteration must not allocate records outside the
    buffers the budget sized, and an allocation this module does not make is
    the only kind its residency meter cannot catch.
    """
    lowest = None
    lowest_key = None
    for reader in live:
        key = _key_at(reader.buf, reader.buf.size - 1)
        if lowest_key is None or key < lowest_key:
            lowest_key = key
            lowest = reader
    return lowest.buf[-1:]


def _reduce_runs(
    runs: List[_Run],
    spills: _Spills,
    fanin_cap: int,
    merge_records: int,
    residency: _Residency,
) -> Tuple[List[_Run], int]:
    """Merge runs until few enough remain for one final pass to consume.

    Without this, a merge of ``k`` runs would hold ``k`` buffers and ``k``
    grows with the total record count — exactly the scaling this module
    exists to remove.
    """
    passes = 0
    while len(runs) > fanin_cap:
        merged: List[_Run] = []
        for start in range(0, len(runs), fanin_cap):
            group = runs[start : start + fanin_cap]
            if len(group) == 1:
                merged.append(group[0])
                continue
            writer = _RunWriter(spills, "merge")
            try:
                merged_blocks = _merge_runs(group, merge_records, residency)
                with contextlib.closing(merged_blocks):
                    for block in merged_blocks:
                        writer.write(block)
            except BaseException:
                writer.abort()
                raise
            merged.append(writer.finish())
            for run in group:
                spills.remove(run.path)
        runs = merged
        passes += 1
    return runs, passes


def _merge_runs(
    runs: Sequence[_Run], merge_records: int, residency: _Residency
) -> Iterator[np.ndarray]:
    """Yield the records of ``runs`` in global key order, in bounded blocks.

    Block-wise k-way merge around :func:`_lowest_ceiling`: everything buffered
    at or below that boundary is already the complete prefix of the merged
    order, so gather it, sort the gathered set, emit. The run that set the
    boundary is drained entirely each iteration, which is what guarantees
    progress.
    """
    fanin = len(runs)
    block_records = _block_records(merge_records, fanin)
    readers: List[_RunReader] = []
    try:
        # opened inside the try so a failure part-way through still closes the
        # readers that were already opened
        for run in runs:
            readers.append(_open_run_reader(run, block_records))
        live = [reader for reader in readers if reader.refill(residency)]
        while live:
            boundary = _lowest_ceiling(live)
            ready = []
            for reader in live:
                tail = reader.buf[reader.offset :]
                count = int(np.searchsorted(tail, boundary, side="right")[0])
                if count:
                    ready.append(reader.buf[reader.offset : reader.offset + count])
                    reader.offset += count
            block = np.concatenate(ready)
            residency.acquire_records(block.size)
            try:
                block.sort()
                yield block
            finally:
                residency.release_records(block.size)
            live = _advance(live, residency)
    finally:
        for reader in readers:
            reader.close(residency)


def _advance(live: List[_RunReader], residency: _Residency) -> List[_RunReader]:
    """Refill the readers this iteration drained; drop the ones that ran out."""
    still = []
    for reader in live:
        if reader.offset < reader.buf.size or reader.refill(residency):
            still.append(reader)
    return still


# --------------------------------------------------------------------------
# rank assignment and verification
# --------------------------------------------------------------------------


class _ForestAccumulator:
    """Observed forest ids and their halo counts, grown as the merge streams.

    O(number of forests), not O(number of records) — the one output of this
    module that cannot be bounded by the memory budget, since it IS the
    per-forest group boundaries the caller asked for.
    """

    def __init__(self) -> None:
        self._ids = np.empty(1024, dtype=np.int64)
        self._counts = np.empty(1024, dtype=np.int64)
        self._n = 0

    def _reserve(self, extra: int) -> None:
        need = self._n + extra
        if need <= self._ids.size:
            return
        capacity = self._ids.size
        while capacity < need:
            capacity *= 2
        ids = np.empty(capacity, dtype=np.int64)
        counts = np.empty(capacity, dtype=np.int64)
        ids[: self._n] = self._ids[: self._n]
        counts[: self._n] = self._counts[: self._n]
        self._ids = ids
        self._counts = counts

    def extend(self, ids: np.ndarray, counts: np.ndarray) -> None:
        self._reserve(ids.size)
        self._ids[self._n : self._n + ids.size] = ids
        self._counts[self._n : self._n + counts.size] = counts
        self._n += int(ids.size)

    def add_to_last(self, extra: int) -> None:
        self._counts[self._n - 1] += extra

    def last_count(self) -> int:
        return int(self._counts[self._n - 1])

    def finish(self) -> Tuple[np.ndarray, np.ndarray]:
        return self._ids[: self._n].copy(), self._counts[: self._n].copy()


def _assign_ranks(
    runs: Sequence[_Run],
    ranks_path: Path,
    total: int,
    merge_records: int,
    residency: _Residency,
) -> Tuple[np.ndarray, np.ndarray, int]:
    """One streaming pass over the merged key order, writing each record's
    within-forest rank to its global position in the backing store."""
    if total == 0:
        with open(str(ranks_path), "wb"):
            pass
        return np.empty(0, dtype=np.int64), np.empty(0, dtype=np.int64), -1

    forests = _ForestAccumulator()
    written = 0
    ranks_mm = np.memmap(str(ranks_path), dtype=RANK_DTYPE, mode="w+", shape=(total,))
    try:
        # every slot starts unwritten so verification can prove each position
        # was assigned exactly once, rather than argue that it must have been
        ranks_mm[:] = RANK_UNWRITTEN
        current_forest = None
        merged_blocks = _merge_runs(runs, merge_records, residency)
        with contextlib.closing(merged_blocks):
            for block in merged_blocks:
                size = int(block.size)
                # reserved at the worst case BEFORE the first scratch array is
                # allocated, so the meter never reports less than is held
                scratch = size * SCRATCH_BYTES_PER_RECORD
                residency.acquire(scratch)
                try:
                    forest = block["forest_id"]
                    opens = np.empty(size, dtype=bool)
                    opens[0] = True
                    np.not_equal(forest[1:], forest[:-1], out=opens[1:])
                    starts = np.nonzero(opens)[0]
                    counts = np.diff(starts, append=size)
                    group_id = np.cumsum(opens)
                    group_id -= 1
                    ranks = np.arange(size, dtype=np.int64)
                    ranks -= starts[group_id]
                    if current_forest is not None and int(forest[0]) == current_forest:
                        # the block boundary fell inside a forest: continue its
                        # numbering
                        ranks[: int(counts[0])] += forests.last_count()
                        forests.add_to_last(int(counts[0]))
                        forests.extend(forest[starts[1:]], counts[1:])
                    else:
                        forests.extend(forest[starts], counts)
                    ranks_mm[block["position"]] = ranks
                    current_forest = int(forest[-1])
                    written += size
                finally:
                    residency.release(scratch)
    finally:
        ranks_mm.flush()
        del ranks_mm

    if written != total:
        raise RankSortError(
            "merge emitted {} record(s) for {} input record(s)".format(written, total)
        )
    forest_ids, forest_counts = forests.finish()
    if forest_ids.size > 1 and not bool(np.all(np.diff(forest_ids) > 0)):
        raise RankSortError(
            "merged order did not group forests: observed forest ids are not strictly ascending"
        )
    return forest_ids, forest_counts, int(forest_counts.max()) - 1


def _dense_moments(forest_counts: np.ndarray) -> Tuple[int, int]:
    """The rank sum and modular sum of squares a dense store must have.

    For a forest of ``c`` halos the dense ranks are ``0 .. c-1``, so they
    contribute ``c(c-1)/2`` and ``(c-1)c(2c-1)/6``. Both are accumulated as
    Python ints: at production a single squared rank (~1.6e20) already exceeds
    int64, so the second moment is only reduced modulo a prime once it is
    exact.
    """
    total = 0
    squares = 0
    for value in forest_counts:
        count = int(value)
        total += count * (count - 1) // 2
        squares += (count - 1) * count * (2 * count - 1) // 6
    return total, squares % _MOMENT_MODULUS


def _verify_ranks(
    ranks_path: Path,
    total: int,
    forest_counts: np.ndarray,
    max_rank: int,
    budget_bytes: int,
    residency: _Residency,
) -> None:
    """Re-read the backing store and check it against the group boundaries.

    The ranks were scattered to disk by global position and the counts were
    accumulated in merged order, so checking one against the other is a real
    end-to-end check on two independently derived things, not a restatement of
    one of them.

    **What it proves, exactly.** The store is the right length, no position was
    left unwritten, and the ranks' count, largest value, sum and (modular) sum
    of squares are those of a store that is dense ``0 .. count-1`` within every
    forest. That is a corruption and partial-write guard strong enough to
    reject a mis-scattered store — the sum alone is not, since e.g. counts
    ``[3, 2]`` admit ``[0, 2, 2, 0, 0]`` as readily as the dense
    ``[0, 1, 2, 0, 1]``, and the second moment is what separates them. **It is
    still four aggregates, not a proof of density**: a corruption contrived to
    preserve all four would pass. Density itself rests on how ``_assign_ranks``
    constructs the ranks, and on the oracle-equality and per-forest density
    tests in ``tests/test_rank_sort.py``.

    No spill file is removed until this passes.
    """
    expected_bytes = total * RANK_DTYPE.itemsize
    actual_bytes = os.path.getsize(str(ranks_path))
    if actual_bytes != expected_bytes:
        raise RankSortError(
            "{}: ranks store is {} byte(s), expected {} for {} record(s)".format(
                ranks_path, actual_bytes, expected_bytes, total
            )
        )
    if total == 0:
        return
    expected_total = int(np.sum(forest_counts))
    if expected_total != total:
        raise RankSortError(
            "per-forest counts sum to {} but {} record(s) were ranked".format(expected_total, total)
        )
    expected_sum, expected_squares = _dense_moments(forest_counts)
    # cap the read block twice over: by the memory budget, and so that a
    # block's rank sum cannot overflow int64 whatever the forest sizes are.
    # The running totals are Python ints and cannot overflow at all.
    block = max(1, min(budget_bytes // RANK_DTYPE.itemsize, 2**62 // (max_rank + 1)))
    observed_sum = 0
    observed_squares = 0
    observed_max = -1
    seen = 0
    with open(str(ranks_path), "rb") as handle:
        while True:
            chunk = np.fromfile(handle, dtype=RANK_DTYPE, count=block)
            if chunk.size == 0:
                break
            residency.acquire(chunk.nbytes)
            try:
                if bool(np.any(chunk == RANK_UNWRITTEN)):
                    raise RankSortError(
                        "{}: {} position(s) were never assigned a rank".format(
                            ranks_path, int(np.count_nonzero(chunk == RANK_UNWRITTEN))
                        )
                    )
                observed_sum += int(np.sum(chunk))
                observed_max = max(observed_max, int(chunk.max()))
                reduced = chunk % _MOMENT_MODULUS
                reduced *= reduced
                reduced %= _MOMENT_MODULUS
                observed_squares += int(np.sum(reduced))
                seen += int(chunk.size)
            finally:
                residency.release(chunk.nbytes)
    observed_squares %= _MOMENT_MODULUS
    if seen != total:
        raise RankSortError("{}: read back {} of {} rank(s)".format(ranks_path, seen, total))
    if observed_max != max_rank:
        raise RankSortError(
            "{}: largest rank read back is {}, but the per-forest counts require {}".format(
                ranks_path, observed_max, max_rank
            )
        )
    if observed_sum != expected_sum:
        raise RankSortError(
            "{}: ranks sum to {}, but dense ranks over the per-forest counts require {}".format(
                ranks_path, observed_sum, expected_sum
            )
        )
    if observed_squares != expected_squares:
        raise RankSortError(
            "{}: ranks are not dense within every forest — their squares sum to {} (mod {}), but "
            "dense ranks over the per-forest counts require {}".format(
                ranks_path, observed_squares, _MOMENT_MODULUS, expected_squares
            )
        )
