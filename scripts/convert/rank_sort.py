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
the peak spill bytes the run held on disk. That peak is a measurement and must
be consumed as one: it exceeds the key set at 48 B/record whenever the run
needed a merge pass.

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

**What the budget does not cover.** Four rounds of review each found one more
allocation the meter did not count, so the exclusions are enumerated here in
full rather than described, and :data:`UNMETERED_ALLOWANCE_BYTES` is the single
number a test asserts the total against:

1. **The ranks backing store.** A memory-mapped file, not a heap allocation:
   ``8 * total_records`` bytes on disk, whose resident pages are page cache the
   kernel may reclaim. Reported as ``ranks_bytes``.
2. **The per-forest group boundaries.** 16 bytes per observed forest in the
   accumulator, capacity doubling. O(number of forests), never O(number of
   records) — and they ARE the output the caller asked for.
3. **Per-run bookkeeping.** One run record, one path, one spill-ledger entry and
   one file handle per spill run, a few hundred bytes each. O(number of runs),
   which is ``total_records / run_records`` — so it SHRINKS as the budget grows.
   The handles are unbuffered for this reason: a stdlib read buffer is sized
   from the filesystem block size, which is neither this module's choice nor
   visible to its meter, and there is one per run.
4. **Per-iteration interpreter churn.** The small ints, tuples and array views a
   merge iteration creates. O(merge iterations x fan-in) objects of ~100 bytes,
   all promptly reclaimable; both factors also shrink as the budget grows.
5. **numpy's own scratch.** ``ndarray.sort`` sorts in place, so it is small, but
   it is not zero and it is not visible here. Likewise each preallocated buffer
   carries an ndarray header and the allocator's own size rounding.

Categories 3 and 4 are the reason the allowance is stated for ordinary
budget-to-input ratios: at a pathological ratio — a budget thousands of times
smaller than the data, which no real run would use — they were measured at about
1 MB and would exceed it. Category 1 is never on the heap. Nothing in this list
is a record buffer: those are all counted.

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

#: Working bytes per record of the scratch the rank pass holds: one bool mask
#: (1) and seven int64-wide arrays (56) — the index ramp, the group-start
#: carrier, the ranks, the group starts, the group counts, the group forest ids,
#: and the contiguous ``intp`` index the scatter into the memory-mapped store
#: needs. All eight are allocated ONCE for the largest block a merge can yield
#: and reused through ``out=``, so the rank pass allocates nothing per block.
SCRATCH_BYTES_PER_RECORD = 1 + 7 * 8

#: Working bytes per record of a merge: the record in a run's read buffer (48),
#: the same record again in the single reusable block buffer the ready set is
#: gathered into (48), and the rank pass's scratch (57). Every one of those is
#: preallocated and reused, so a merge iteration allocates nothing at all: the
#: boundary key is a view into the buffer that supplied it, and the yielded
#: block is a view into the block buffer.
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

#: Working bytes per record of the verification pass, which reads the ranks
#: store back in blocks: the block itself (8), the int64 scratch the squared
#: residues are folded through (8), and the bool mask the sentinel scan writes
#: (1). All three are preallocated once and reused through ``out=``, so a
#: verification block allocates nothing.
VERIFY_BYTES_PER_RECORD = 8 + 8 + 1

#: Allocation this module makes that is NOT charged against ``budget_bytes``,
#: as one number a test can assert against. Every category it covers is
#: enumerated in the module docstring's "What the budget does not cover". It is
#: set from measurement: across budgets from 313 KB to 5.0 MB the excluded
#: total stayed between 83 KB and 294 KB — roughly FLAT in absolute terms while
#: falling from 38% to 6% of the budget — so 512 KiB is that worst case with a
#: 1.8x margin for platform variation. It covers per-run, per-forest and
#: per-iteration bookkeeping, never a record buffer. Because it is flat while a
#: record buffer is budget-proportional, the allocation test that asserts
#: against it uses a budget large enough that one escaped buffer (786 KB there)
#: cannot hide inside the allowance.
UNMETERED_ALLOWANCE_BYTES = 1 << 19

#: Modulus for the verification's second moment. A Mersenne prime below 2**31,
#: so a reduced rank squares inside int64. It does NOT bound the sum of those
#: residues — that is what :func:`_verify_block_records` is for.
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
    envelope. **Consume the reported figure, never a ``total * 48`` formula.**
    It equals the whole key set at 48 B/record only when ``n_merge_passes`` is
    zero; every merge pass holds its inputs and its output at once, and the
    peak was measured at 1.13x to 1.55x the key set across ordinary
    multi-pass configurations.

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
        # unbuffered, like every other spill handle: see the module docstring's
        # exclusion list for why a stdlib read/write buffer is not free
        self.handle = open(str(self.path), "wb", buffering=0)
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
    """Reads one sorted run back through a slice of the merge's buffer arena.

    The reader does not own its buffer: :func:`_merge_runs` allocates one arena
    for every run in the merge and hands each reader a fixed slice of it, along
    with that slice's per-field column views. Both are needed for the ceiling
    key below, and taking them per reader rather than once per merge made
    numpy's structured-field machinery cost a few kilobytes per run.

    The handle is unbuffered because a stdlib read buffer is sized from the
    filesystem's block size — neither this module's choice nor visible to its
    meter — and there is one handle per run, so that cost scales with fan-in.

    Verifies the run's size on open and its CRC32 once the last record is read.
    """

    __slots__ = (
        "run",
        "buffer",
        "columns",
        "capacity",
        "raw",
        "handle",
        "remaining",
        "crc",
        "size",
        "offset",
        "ceiling",
    )

    def __init__(self, run: _Run, buffer: np.ndarray, columns: Sequence[np.ndarray]) -> None:
        expected = run.n_records * SPILL_RECORD_NBYTES
        actual = os.path.getsize(str(run.path))
        if actual != expected:
            raise RankSortError(
                "{}: spill run is {} byte(s), expected {} for {} record(s)".format(
                    run.path, actual, expected, run.n_records
                )
            )
        self.run = run
        self.buffer = buffer
        self.columns = columns
        self.capacity = int(buffer.size)
        self.raw = memoryview(buffer).cast("B")
        self.handle = open(str(run.path), "rb", buffering=0)
        self.remaining = int(run.n_records)
        self.crc = 0
        self.size = 0
        self.offset = 0
        #: this buffer's last key, i.e. the most this run can still deliver from
        #: it. Refreshed on refill and NOT while a block is being consumed: the
        #: last record does not move as ``offset`` advances.
        self.ceiling = None

    def refill(self) -> bool:
        """Load the next block into this reader's slice of the arena. False once
        the run is exhausted, at which point its CRC32 must match what was
        written."""
        self.size = 0
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
        count = min(self.capacity, self.remaining)
        wanted = count * SPILL_RECORD_NBYTES
        got = _fill_block(self.handle, self.raw[:wanted])
        if got != wanted:
            raise RankSortError(
                "{}: spill run yielded {} of {} expected byte(s)".format(self.run.path, got, wanted)
            )
        block = self.buffer[:count]
        self.crc = zlib.crc32(block.data, self.crc)
        self.remaining -= count
        self.size = count
        self.ceiling = tuple(int(column[count - 1]) for column in self.columns)
        return True

    def close(self) -> None:
        self.handle.close()


def _open_run_reader(run: _Run, buffer: np.ndarray, columns: Sequence[np.ndarray]) -> _RunReader:
    """Indirection so a test can force a failure part-way through a merge."""
    return _RunReader(run, buffer, columns)


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
    # one strided view per field, taken ONCE: a structured field access builds a
    # fresh view and dtype, and there are five of them per copied span
    chunk_columns = tuple(chunk[name] for name in SPILL_DTYPE.names)
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
            # the caller's key columns, taken once per block rather than once
            # per copied span: a block wider than the chunk is split across many
            # spans, and each structured field access builds a view and a dtype
            source_columns = tuple(records[name] for name in KEY_ARRAY_FIELDS)
            count = int(records.size)
            taken = 0
            while taken < count:
                take = min(count - taken, run_records - filled)
                _copy_keys(
                    chunk_columns,
                    source_columns,
                    ramp,
                    filled,
                    taken,
                    take,
                    snap,
                    position + taken,
                )
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
    chunk_columns: Sequence[np.ndarray],
    source_columns: Sequence[np.ndarray],
    ramp: np.ndarray,
    at: int,
    src: int,
    count: int,
    snap: int,
    position: int,
) -> None:
    """Copy one span of a caller block's keys into the chunk, in spill layout.

    Both sides are addressed through column views taken once by the caller, and
    the positions are numbered from the preallocated ramp in place, so copying a
    span allocates nothing.
    """
    forest, neg_snap, upid, pid, identifier, positions = chunk_columns
    src_forest, src_upid, src_pid, src_id = source_columns
    end = at + count
    src_end = src + count
    forest[at:end] = src_forest[src:src_end]
    neg_snap[at:end] = -snap
    upid[at:end] = src_upid[src:src_end]
    pid[at:end] = src_pid[src:src_end]
    identifier[at:end] = src_id[src:src_end]
    positions[at:end] = ramp[:count]
    positions[at:end] += position


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


def _count_at_or_below(
    columns: Sequence[np.ndarray], lo: int, hi: int, key: Tuple[int, ...]
) -> int:
    """Index of the first record in ``[lo, hi)`` whose key exceeds ``key``.

    A binary search over the six key fields as a tuple of Python ints.
    ``np.searchsorted`` would do this in C, but on a structured dtype it runs
    numpy's dtype PROMOTION path and builds a fresh dtype on every call — once
    per buffered run per merge iteration, which measured as this module's
    largest allocation site and grew with the record count. The comparison here
    is the same lexicographic order: the fields are compared in dtype order,
    which is the key order.
    """
    while lo < hi:
        mid = (lo + hi) // 2
        # compare field by field and stop at the first difference: building a
        # whole six-field tuple per probe was itself measurable churn, and
        # almost every comparison resolves on forest_id
        at_or_below = True
        for column, bound in zip(columns, key):
            value = int(column[mid])
            if value != bound:
                at_or_below = value < bound
                break
        if at_or_below:
            lo = mid + 1
        else:
            hi = mid
    return lo


class _MergedBlock:
    """The merge's current block, together with the two key columns a consumer
    reads from it.

    One instance per merge, updated in place. Taking a structured field view
    (``block["forest_id"]``) builds a fresh view AND a fresh dtype every time,
    and doing that once per block made numpy's field machinery the largest
    remaining allocation site in this module, growing with the record count.
    The column views here are taken once, off the reusable block buffer, and
    only sliced per block.

    ``records``, ``forest`` and ``position`` are views into that reusable
    buffer and are valid only until the consumer asks for the next block.
    """

    __slots__ = ("records", "forest", "position", "size")

    def __init__(self) -> None:
        self.records = None
        self.forest = None
        self.position = None
        self.size = 0


def _lowest_ceiling(live: Sequence["_RunReader"]) -> Tuple[int, ...]:
    """The smallest of the buffered runs' cached ceiling keys.

    Every buffered run's last key is a ceiling on what that run can still
    deliver from disk, so the smallest of them bounds the merged prefix that is
    already complete. Each reader caches its own ceiling on refill, so this
    returns one of those existing tuples rather than building anything: a merge
    iteration must not allocate, and an allocation this module does not make is
    the only kind its residency meter cannot catch.
    """
    lowest = None
    for reader in live:
        key = reader.ceiling
        if lowest is None or key < lowest:
            lowest = key
    return lowest


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
                        writer.write(block.records)
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

    Yields one :class:`_MergedBlock`, reused every iteration; **its arrays are
    views into one reusable buffer and are valid only until the consumer asks
    for the next block.** That is deliberate: allocating a fresh block per
    iteration left the previous one alive in the consumer's loop variable while
    the next was being built, so the true peak was twice what any counter
    reported. A consumer that needs a block to outlive its iteration must copy
    it.
    """
    fanin = len(runs)
    block_records = _block_records(merge_records, fanin)
    readers: List[_RunReader] = []
    buffered = fanin * block_records
    # two allocations for the whole merge: one arena the runs read into, one
    # block buffer the ready set is gathered into. Per-field column views are
    # taken once here, off those two, and only sliced afterwards.
    arena = np.empty(buffered, dtype=SPILL_DTYPE)
    arena_columns = tuple(arena[name] for name in SPILL_DTYPE.names)
    block_buffer = np.empty(buffered, dtype=SPILL_DTYPE)
    forest_column = block_buffer["forest_id"]
    position_column = block_buffer["position"]
    residency.acquire_records(2 * buffered)
    try:
        # opened inside the try so a failure part-way through still closes the
        # readers that were already opened
        for index, run in enumerate(runs):
            low = index * block_records
            high = low + block_records
            readers.append(
                _open_run_reader(
                    run,
                    arena[low:high],
                    tuple(column[low:high] for column in arena_columns),
                )
            )
        merged = _MergedBlock()
        live = [reader for reader in readers if reader.refill()]
        while live:
            boundary = _lowest_ceiling(live)
            ready = []
            gathered = 0
            for reader in live:
                end = _count_at_or_below(reader.columns, reader.offset, reader.size, boundary)
                count = end - reader.offset
                if count:
                    ready.append(reader.buffer[reader.offset : end])
                    reader.offset = end
                    gathered += count
            block = block_buffer[:gathered]
            np.concatenate(ready, out=block)
            block.sort()
            merged.records = block
            merged.forest = forest_column[:gathered]
            merged.position = position_column[:gathered]
            merged.size = gathered
            yield merged
            live = _advance(live)
    finally:
        residency.release_records(2 * buffered)
        for reader in readers:
            reader.close()


def _advance(live: List[_RunReader]) -> List[_RunReader]:
    """Refill the readers this iteration drained; drop the ones that ran out."""
    still = []
    for reader in live:
        if reader.offset < reader.size or reader.refill():
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
    within-forest rank to its global position in the backing store.

    Every scratch array is allocated once for the largest block a merge can
    yield and reused through ``out=``, so this pass allocates nothing per
    block: a per-block allocation would still be alive, bound to the loop
    variable, while the next one was being made.
    """
    if total == 0:
        with open(str(ranks_path), "wb"):
            pass
        return np.empty(0, dtype=np.int64), np.empty(0, dtype=np.int64), -1

    forests = _ForestAccumulator()
    written = 0
    ramp = np.arange(merge_records, dtype=np.int64)
    group_start = np.empty(merge_records, dtype=np.int64)
    ranks = np.empty(merge_records, dtype=np.int64)
    starts = np.empty(merge_records, dtype=np.int64)
    counts = np.empty(merge_records, dtype=np.int64)
    group_forest = np.empty(merge_records, dtype=np.int64)
    positions = np.empty(merge_records, dtype=np.intp)
    seams = np.empty(merge_records, dtype=bool)
    residency.acquire(merge_records * SCRATCH_BYTES_PER_RECORD)
    ranks_mm = np.memmap(str(ranks_path), dtype=RANK_DTYPE, mode="w+", shape=(total,))
    try:
        # every slot starts unwritten so verification can prove each position
        # was assigned exactly once, rather than argue that it must have been
        ranks_mm[:] = RANK_UNWRITTEN
        current_forest = None
        merged_blocks = _merge_runs(runs, merge_records, residency)
        with contextlib.closing(merged_blocks):
            for merged in merged_blocks:
                size = merged.size
                forest = merged.forest
                seam = seams[:size]
                seam[0] = True
                np.not_equal(forest[1:], forest[:-1], out=seam[1:])

                # the group boundaries: where a forest opens, which forest, and
                # how many records it holds in this block
                n_groups = int(np.count_nonzero(seam))
                np.compress(seam, ramp[:size], out=starts[:n_groups])
                np.compress(seam, forest, out=group_forest[:n_groups])
                np.subtract(starts[1:n_groups], starts[: n_groups - 1], out=counts[: n_groups - 1])
                counts[n_groups - 1] = size - int(starts[n_groups - 1])

                # rank within group = index - the index the group opened at,
                # carried forward by a running maximum so no gather is needed
                np.multiply(seam, ramp[:size], out=group_start[:size])
                np.maximum.accumulate(group_start[:size], out=group_start[:size])
                np.subtract(ramp[:size], group_start[:size], out=ranks[:size])

                if current_forest is not None and int(forest[0]) == current_forest:
                    # the block boundary fell inside a forest: continue its
                    # numbering
                    first = int(counts[0])
                    ranks[:first] += forests.last_count()
                    forests.add_to_last(first)
                    forests.extend(group_forest[1:n_groups], counts[1:n_groups])
                else:
                    forests.extend(group_forest[:n_groups], counts[:n_groups])

                np.copyto(positions[:size], merged.position)
                ranks_mm[positions[:size]] = ranks[:size]
                current_forest = int(forest[-1])
                written += size
    finally:
        ranks_mm.flush()
        del ranks_mm
        residency.release(merge_records * SCRATCH_BYTES_PER_RECORD)

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


def _fill_block(handle, raw: memoryview) -> int:
    """Read ``raw`` full from ``handle``, returning the bytes read.

    A raw file handle may return a short read at any point, so top the buffer
    up rather than treating one as end of file; the only short return is then
    the last block, and the store's size was already checked to be a whole
    number of ranks.
    """
    got = 0
    while got < len(raw):
        read = handle.readinto(raw[got:])
        if not read:
            break
        got += read
    return got


def _verify_block_records(budget_bytes: int, max_rank: int, total: int) -> int:
    """How many ranks the verification pass reads at a time.

    Four ceilings meet here, and each one bounds a different quantity the loop
    in :func:`_verify_ranks` holds or sums. They are spelled out because the
    running totals there are Python ints and cannot overflow, while the
    per-block reductions run in int64 and can:

    - **memory** — the pass holds ``VERIFY_BYTES_PER_RECORD`` bytes per record
      of the block, and that must fit the caller's budget.
    - **the file** — never allocate buffers larger than the store itself.
    - **the rank sum** — ``np.sum`` over a block of ranks must stay inside
      int64, and a rank is at most ``max_rank``, so ``block * max_rank`` is the
      quantity to bound.
    - **the residue sum** — ``np.sum`` over a block of squared residues must
      too, and a residue is bounded by ``_MOMENT_MODULUS - 1``, **not** by
      ``max_rank``. This is the ceiling that binds when the budget is large and
      the forests are small: without it a 34 GB budget over low-rank data
      overflows and REJECTS a correct store, which this module would then
      delete.
    """
    by_memory = int(budget_bytes) // VERIFY_BYTES_PER_RECORD
    by_file = max(1, int(total))
    by_rank_sum = 2**62 // (int(max_rank) + 1)
    by_residue_sum = 2**62 // _MOMENT_MODULUS
    return max(1, min(by_memory, by_file, by_rank_sum, by_residue_sum))


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
    block = _verify_block_records(budget_bytes, max_rank, total)
    # preallocated once and reused through out=, so the loop below allocates
    # nothing per block and the whole pass costs exactly what is metered here
    chunk_buf = np.empty(block, dtype=RANK_DTYPE)
    work = np.empty(block, dtype=RANK_DTYPE)
    mask = np.empty(block, dtype=bool)
    residency.acquire(block * VERIFY_BYTES_PER_RECORD)
    observed_sum = 0
    observed_squares = 0
    observed_max = -1
    seen = 0
    try:
        raw = memoryview(chunk_buf).cast("B")
        # unbuffered: a BufferedReader would add its own 8 KB read buffer on
        # top of the block this pass just sized to the budget, and _fill_block
        # makes the short reads a raw handle is allowed to return harmless
        with open(str(ranks_path), "rb", buffering=0) as handle:
            while True:
                nbytes = _fill_block(handle, raw)
                if not nbytes:
                    break
                if nbytes % RANK_DTYPE.itemsize:
                    raise RankSortError(
                        "{}: read {} byte(s), not a whole number of int64 ranks".format(
                            ranks_path, nbytes
                        )
                    )
                size = nbytes // RANK_DTYPE.itemsize
                chunk = chunk_buf[:size]
                found = mask[:size]
                np.equal(chunk, RANK_UNWRITTEN, out=found)
                if bool(np.any(found)):
                    raise RankSortError(
                        "{}: {} position(s) were never assigned a rank".format(
                            ranks_path, int(np.count_nonzero(found))
                        )
                    )
                # each block sum runs in int64; _verify_block_records bounds
                # the block so that neither can wrap: ranks are at most
                # max_rank, residues at most _MOMENT_MODULUS - 1
                observed_sum += int(np.sum(chunk))
                observed_max = max(observed_max, int(np.max(chunk)))
                residues = work[:size]
                np.mod(chunk, _MOMENT_MODULUS, out=residues)
                np.multiply(residues, residues, out=residues)
                np.mod(residues, _MOMENT_MODULUS, out=residues)
                observed_squares += int(np.sum(residues))
                seen += size
    finally:
        residency.release(block * VERIFY_BYTES_PER_RECORD)
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
