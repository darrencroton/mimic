"""Bounded transpose and 64-bit topology remapping for canonical adapter data
(contracts C1, C3 and C4 of
docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

**What it does.** :func:`transpose` consumes the canonical batches of any
source adapter (``adapters/base.py``: links carried as target ``SourceHaloID``)
and produces, for every a_list snapshot including empty ones, one flat record
file whose rows are that snapshot's halos in ascending ``SourceHaloID`` order,
with the five links resolved to int64 snapshot-local rows and the three
progenitor/descendant links qualified by their int32 target snapshot -- the
v3 ``/halos`` columns of ``docs/dev/HORIZONTAL-HDF5-FORMAT.md`` (section "Version 3"). It
writes no HDF5: ``hdf5_writer_v3.py`` writes the v3 file from these records.

**What it preserves.** Rows are *moved*, never changed: every payload,
extra and identity value is copied bit for bit, ``ForestIndex`` and
``HaloRankInForest`` are the adapter's own, and ``MostBoundID`` is carried as
data (duplicated and negative values included), never used as a key. Topology
is *remapped*, never reordered: each link names the same halo it named in the
source, so source chain order survives exactly. Gaps stay gaps -- a
descendant two snapshots ahead is written as such -- and nothing inserts,
drops, repairs or rescales a halo. The adapter's batch order does not matter:
L-Halo and forests-HDF5 stream in inventory order, ASCII snapshot-major, and
all three produce identical output for identical input.

**Pipeline.** Four phases, each an external sort over a record layout sized
from the schema's actual widths (``rank_sort.KeyedSorter``):

1. *Rows.* Every halo becomes a row record (:func:`row_dtype`) and is sorted
   by ``(SnapNum, SourceHaloID)``. A row's index in that order is its global
   position; its snapshot-local row is that minus the snapshot's offset
   (``source_keys.SnapshotLayout``). The sorted rows go to a private spool.
2. *Join.* Each row contributes a map entry and one request per non-null link
   to a stream sorted by source key (``source_keys``), resolved in one pass
   with every closure check.
3. *Chains.* ``NextProgenitor`` and ``NextHaloInFOFgroup`` chains are proven
   acyclic by pointer jumping.
4. *Assembly.* The resolved links, sorted by owner, are streamed alongside the
   row spool and written into each snapshot's output file in row order.

**Memory.** ``budget_bytes`` bounds the working buffers this module and its
sorters hold -- chunk, merge arena, gather, key column, permutation and
per-block consumer scratch -- and every one is reported to one shared meter
whose high-water mark comes back as ``peak_resident_bytes``. Each phase splits
the budget between the terms it holds *concurrently* (:class:`TransposeBudget`),
so the bound is on their sum, not on each term alone. A wider schema -- more
or wider extras -- costs more bytes per row and therefore buys fewer rows per
chunk; it never raises the bound. Refused before any data is read: a budget
too small for one row in any phase.

It is *not* a bound on interpreter RSS. Outside it: the adapter's own working
set (governed by the adapter's own ``memory_budget_bytes``; the batches it
hands over *are* metered, by their actual ``nbytes``, while this module holds
them); per-run and per-round bookkeeping and interpreter churn; O(number of
snapshots) layout arrays; ndarray headers; and numpy's transient sort scratch
beyond the permutation. Nothing held is O(halos): no id dictionary, no
catalog-sized id or link array, and no forest, FoF group or sibling chain is
ever materialised whole.

**Disk.** Spill runs and the row spool live in a private directory under
``spill_dir`` (default: ``out_dir``) and are removed on every path; the
combined high-water mark comes back as ``peak_spill_bytes``. Peak disk is
dominated by the row spool plus phase 2's join records (up to six 48-byte
records per halo) plus the output files themselves.

**Failure.** Every contract violation raises ``ConverterError`` before the
offending value could be narrowed or written. Output files are created
exclusively, only in the assembly phase, and removed again if anything fails,
so a failed call leaves no output that could be mistaken for a finished one.
The canonical input is only ever read: re-running from the same source is the
recovery path, and no scratch of this module is ever the only copy of
anything. Persistence, manifests and restart belong to ``pipeline.py``, which
calls this module.

numpy + stdlib only.
"""

import contextlib
import os
import shutil
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple

import numpy as np
from adapters.base import LINK_FIELDS, NULL_LINK, SNAPSHOT_LINK_FIELDS, CanonicalBatch
from column_schema import EXTRA_TYPES, IDENTITY_FIELDS, CanonicalSchema, ConverterError
from rank_sort import (
    SCRATCH_WRITE_BUFFER_BYTES,
    KeyedSorter,
    RankSortError,
    ResidencyMeter,
    SpillLedger,
    keyed_generation_bytes_per_record,
    keyed_merge_bytes_per_record,
    read_into,
)
from source_keys import (
    CHAIN_DTYPE,
    CHAIN_KEY,
    CHAIN_ROUND_SCRATCH_BYTES_PER_RECORD,
    JOIN_BUILD_BYTES_PER_ROW,
    JOIN_DTYPE,
    JOIN_KEY,
    JOIN_SCRATCH_BYTES_PER_RECORD,
    RESOLVED_DTYPE,
    RESOLVED_KEY,
    SnapshotLayout,
    SourceKeyJoin,
    build_join_records,
    validate_snapshots,
    verify_chains_acyclic,
)

__all__ = [
    "SNAPSHOT_FIELD",
    "SOURCE_KEY_PREFIX",
    "UNMETERED_ALLOWANCE_BYTES",
    "SnapshotOutput",
    "TransposeBudget",
    "TransposeResult",
    "output_dtype",
    "output_name",
    "plan_budget",
    "read_snapshot",
    "row_dtype",
    "transpose",
]

#: Row-record field carrying the sort's snapshot key (int64 copy of SnapNum).
#: The leading underscore cannot collide with a declared name: C2 output names
#: must start with a letter.
SNAPSHOT_FIELD = "_snap"

#: Row-record prefix of each link's *source key* (target SourceHaloID).
SOURCE_KEY_PREFIX = "_to_"

#: The target-snapshot column of each snapshot-qualified link (C3).
_SNAPSHOT_COLUMN: Dict[str, str] = {
    "Descendant": "DescendantSnapshot",
    "FirstProgenitor": "FirstProgenitorSnapshot",
    "NextProgenitor": "NextProgenitorSnapshot",
}
if tuple(_SNAPSHOT_COLUMN.values()) != SNAPSHOT_LINK_FIELDS:  # pragma: no cover - contract drift
    raise ImportError("transpose.py's snapshot-column map disagrees with adapters.base")

_IDENTITY_NAMES = tuple(field.name for field in IDENTITY_FIELDS)
_ROW_KEY = (SNAPSHOT_FIELD, "SourceHaloID")

#: Buffered writers this module itself holds open at once: the row spool in
#: phase 1, or one output file in phase 4. Sorter spill writers are carved out
#: of each sorter's own budget share.
_CONCURRENT_WRITERS = 1

#: Allocation a transpose makes that is NOT charged against ``budget_bytes``,
#: as one number a test asserts the whole call against: per-run and per-reader
#: bookkeeping, per-block interpreter churn and numpy's transient sort scratch
#: (the module docstring's "outside it" list, less the adapter). Measured with
#: ``tracemalloc`` over synthetic valid forests at a 2 MiB budget: 187 KB with
#: 22 spill runs, 324 KB with 52 and 429-437 KB with 206 -- about 160 KB flat
#: plus about 1.4 KB per run, so O(runs), which shrinks as the budget grows,
#: and never O(halos). 1 MiB covers ordinary budget-to-input ratios with
#: margin; a budget thousands of times smaller than its input would need more.
UNMETERED_ALLOWANCE_BYTES = 1 << 20

#: Per-row scratch while a batch becomes row records, beyond the row array
#: itself. The snapshot lookup's positions and their bounds-clipped copy
#: (2 x 8 B) stay alive beside the row array until ``_rows_of`` returns.
#: Before the row array exists, the lookup's membership test holds 25 B/row
#: (those 16 B, the gathered snapshot numbers and their mask) and
#: ``CanonicalBatch.validate`` 9 B/row (``np.diff`` over the ids and its
#: mask); every row array is far wider than 25 B, so neither raises the peak.
#: ``validate`` runs before ``_ingest`` takes this reservation, under the same
#: held batch, so the larger reservation that follows covers it. The last
#: 2 B/row cover the call's fixed overhead, about 4 KB. Measured with
#: ``tracemalloc`` around ``_ingest`` on one real batch: the row array plus
#: 16.0-16.8 B/row from 4,931 to 389,588 rows, with and without every extra
#: type.
_INGEST_SCRATCH_BYTES_PER_ROW = 2 * 8 + 2

#: Per-row scratch in assembly: the snapshot-position check's ``arange``, its
#: ``searchsorted`` result and the ``- 1`` applied to that result (3 x 8 B),
#: the chunk's peak. What follows is smaller: the gathered expected snapshot
#: numbers (8 B, alive to the end of the chunk) with their comparison mask,
#: then the three masks of the null check. numpy elides the ``- 1`` temporary
#: for arrays of 256 KiB or more, so a chunk of 32,768 rows or more measures
#: 16 B/row; below that the full 24 is paid (24.04 B/row measured at 28,882
#: rows).
_ASSEMBLY_SCRATCH_BYTES_PER_ROW = 3 * 8

#: Per-record consumer scratch reserved while resolved links are scattered,
#: for the one segment in flight: its owner-local rows plus ``locate``'s
#: positions, gathered snapshot numbers, gathered offsets and target rows
#: (5 x 8 B), and a kind mask. Measured with ``tracemalloc`` around
#: ``_resolve_chunk`` at 40.0-40.2 B/record for segments of 16,384 to 474,531
#: records. Each segment is scattered by a call of its own
#: (:meth:`_Transpose._scatter_links`) so that its arrays are freed before the
#: next segment is located; were they still bound, a segment would cost about
#: 58 B/record.
_SCATTER_SCRATCH_BYTES_PER_RECORD = 5 * 8 + 1


# ==========================================================================
# Record layouts
# ==========================================================================


def _field_spec(name: str, type_name: str) -> Tuple:
    spec = EXTRA_TYPES[type_name]
    base = np.dtype(spec.numpy_dtype).newbyteorder("<")
    if spec.n_components == 1:
        return (name, base)
    return (name, base, (spec.n_components,))


def _payload_specs(schema: CanonicalSchema) -> List[Tuple]:
    specs = [_field_spec(field.name, field.type) for field in schema.payload_fields]
    specs.extend(_field_spec(extra.name, extra.type) for extra in schema.extra_fields)
    return specs


def row_dtype(schema: CanonicalSchema) -> np.dtype:
    """The sorted row record: sort key, identity, source keys, payload, extras.

    Its itemsize is the schema's actual width -- every selected extra adds its
    declared bytes -- and every size and budget below is derived from it.
    """
    specs: List[Tuple] = [(SNAPSHOT_FIELD, "<i8")]
    specs.extend((name, "<i8") for name in _IDENTITY_NAMES)
    specs.extend((SOURCE_KEY_PREFIX + name, "<i8") for name in LINK_FIELDS)
    specs.extend(_payload_specs(schema))
    return np.dtype(specs)


def output_dtype(schema: CanonicalSchema) -> np.dtype:
    """One output row: the v3 ``/halos`` columns in the format table's order.

    Five int64 links, three int32 target snapshots, three int64 identities,
    then the payload and extras in their declared precision, explicitly
    little-endian.
    """
    specs: List[Tuple] = [(name, "<i8") for name in LINK_FIELDS]
    specs.extend((name, "<i4") for name in SNAPSHOT_LINK_FIELDS)
    specs.extend((name, "<i8") for name in _IDENTITY_NAMES)
    specs.extend(_payload_specs(schema))
    return np.dtype(specs)


def _batch_row_bytes(schema: CanonicalSchema) -> int:
    """Bytes one row costs in a canonical batch: identity, coordinates and
    links (eleven int64) plus the native payload and extras."""
    payload = sum(EXTRA_TYPES[field.type].itemsize for field in schema.payload_fields)
    extras = sum(extra.spec.itemsize for extra in schema.extra_fields)
    return 11 * 8 + payload + extras


def output_name(snapshot: int) -> str:
    """The output file of one snapshot."""
    return "snapshot_{:03d}.transposed".format(int(snapshot))


# ==========================================================================
# Budget
# ==========================================================================


@dataclass(frozen=True)
class TransposeBudget:
    """How one working-buffer budget is split between concurrent terms.

    Each phase's terms sum to at most ``usable_bytes``, which is
    ``budget_bytes`` less the metered write buffers that can be open at once.
    ``max_batch_rows`` is what the adapter is asked for; ``assembly_rows`` how
    many rows assembly processes per chunk.
    """

    budget_bytes: int
    usable_bytes: int
    row_itemsize: int
    output_itemsize: int
    batch_row_bytes: int
    max_batch_rows: int
    row_generation_bytes: int
    row_merge_bytes: int
    join_generation_bytes: int
    join_merge_bytes: int
    resolved_generation_bytes: int
    chain_generation_bytes: int
    chain_merge_bytes: int
    chain_round_generation_bytes: int
    resolved_merge_bytes: int
    assembly_bytes: int
    assembly_rows: int


def _require_positive_int(value, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, np.integer)):
        raise ConverterError(
            "{} must be an integer, got {!r}; a fractional or boolean value would be truncated "
            "into a different one".format(name, type(value).__name__)
        )
    value = int(value)
    if value <= 0:
        raise ConverterError("{} must be positive, got {}".format(name, value))
    return value


def plan_budget(schema: CanonicalSchema, budget_bytes: int) -> TransposeBudget:
    """Split ``budget_bytes`` between every phase's concurrent terms, or fail.

    Checked here, before any source row is read, so a budget too small for
    one record anywhere is refused up front rather than hours into a run.
    """
    budget_bytes = _require_positive_int(budget_bytes, "budget_bytes")
    row = row_dtype(schema)
    out = output_dtype(schema)
    usable = budget_bytes - _CONCURRENT_WRITERS * SCRATCH_WRITE_BUFFER_BYTES
    half = usable // 2
    quarter = usable // 4
    batch_row_bytes = _batch_row_bytes(schema)
    per_ingest_row = batch_row_bytes + row.itemsize + _INGEST_SCRATCH_BYTES_PER_ROW
    per_assembly_row = row.itemsize + out.itemsize + _ASSEMBLY_SCRATCH_BYTES_PER_ROW

    plan = TransposeBudget(
        budget_bytes=budget_bytes,
        usable_bytes=usable,
        row_itemsize=int(row.itemsize),
        output_itemsize=int(out.itemsize),
        batch_row_bytes=batch_row_bytes,
        max_batch_rows=max(0, half) // per_ingest_row,
        row_generation_bytes=usable - half,
        row_merge_bytes=half,
        join_generation_bytes=usable - half,
        join_merge_bytes=half,
        resolved_generation_bytes=quarter,
        chain_generation_bytes=usable - half - quarter,
        chain_merge_bytes=half,
        chain_round_generation_bytes=usable - half,
        resolved_merge_bytes=half,
        assembly_bytes=usable - half,
        assembly_rows=max(0, usable - half) // per_assembly_row,
    )

    writer = SCRATCH_WRITE_BUFFER_BYTES

    def generation(dtype, key) -> int:
        return keyed_generation_bytes_per_record(dtype, len(key)) + writer

    def merge(dtype, key, consumer: int) -> int:
        buffers = keyed_merge_bytes_per_record(dtype, len(key))
        # a final two-way merge with its consumer, or an intermediate two-way
        # pass with its output writer, whichever is larger
        return max(2 * (buffers + consumer), 2 * buffers + writer)

    requirements = (
        ("ingest", plan.max_batch_rows, 1, "row(s)"),
        ("assembly", plan.assembly_rows, 1, "row(s)"),
        ("row sort generation", plan.row_generation_bytes, generation(row, _ROW_KEY), "bytes"),
        (
            "row sort merge",
            plan.row_merge_bytes,
            merge(row, _ROW_KEY, JOIN_BUILD_BYTES_PER_ROW),
            "bytes",
        ),
        (
            "join generation",
            plan.join_generation_bytes,
            generation(JOIN_DTYPE, JOIN_KEY),
            "bytes",
        ),
        (
            "join merge",
            plan.join_merge_bytes,
            merge(JOIN_DTYPE, JOIN_KEY, JOIN_SCRATCH_BYTES_PER_RECORD),
            "bytes",
        ),
        (
            "resolved-link generation",
            plan.resolved_generation_bytes,
            generation(RESOLVED_DTYPE, RESOLVED_KEY),
            "bytes",
        ),
        (
            "chain generation",
            min(plan.chain_generation_bytes, plan.chain_round_generation_bytes),
            generation(CHAIN_DTYPE, CHAIN_KEY),
            "bytes",
        ),
        (
            "chain merge",
            plan.chain_merge_bytes,
            merge(CHAIN_DTYPE, CHAIN_KEY, CHAIN_ROUND_SCRATCH_BYTES_PER_RECORD),
            "bytes",
        ),
        (
            "resolved-link merge",
            plan.resolved_merge_bytes,
            merge(RESOLVED_DTYPE, RESOLVED_KEY, _SCATTER_SCRATCH_BYTES_PER_RECORD),
            "bytes",
        ),
    )
    for phase, available, needed, unit in requirements:
        if available < needed:
            raise ConverterError(
                "a working-buffer budget of {} bytes is too small for a {}-byte row schema: "
                "the {} phase has {} {} but needs at least {}".format(
                    budget_bytes, row.itemsize, phase, available, unit, needed
                )
            )
    return plan


# ==========================================================================
# Results
# ==========================================================================


@dataclass(frozen=True)
class SnapshotOutput:
    """One snapshot's output file and its population."""

    snapshot: int
    path: str
    n_halos: int
    first_global_position: int


@dataclass(frozen=True)
class TransposeResult:
    """What one :func:`transpose` produced, and what it cost.

    ``record_dtype`` is :func:`output_dtype` for the schema; every output file
    is exactly ``n_halos * record_dtype.itemsize`` bytes of it.
    ``links_adjacent`` is measured over ``Descendant`` links only (v3 header
    semantics), in a_list positions.
    """

    schema_digest: str
    record_dtype: np.dtype
    snapshots: Tuple[SnapshotOutput, ...]
    total_halos: int
    n_links: Dict[str, int]
    n_gapped_descendants: int
    max_descendant_span: int
    chain_rounds: int
    budget: TransposeBudget
    peak_resident_bytes: int
    peak_spill_bytes: int
    sort_runs: Dict[str, int]
    sort_merge_passes: Dict[str, int]

    @property
    def links_adjacent(self) -> bool:
        return self.n_gapped_descendants == 0

    def output(self, snapshot: int) -> SnapshotOutput:
        for entry in self.snapshots:
            if entry.snapshot == snapshot:
                return entry
        raise ConverterError("snapshot {} is not an output of this transpose".format(snapshot))


def read_snapshot(result: TransposeResult, snapshot: int) -> np.ndarray:
    """One snapshot's output rows (a read-only memory map when non-empty)."""
    entry = result.output(snapshot)
    size = os.path.getsize(entry.path)
    expected = entry.n_halos * result.record_dtype.itemsize
    if size != expected:
        raise ConverterError(
            "{} is {} bytes; {} rows of {} bytes require {}".format(
                entry.path, size, entry.n_halos, result.record_dtype.itemsize, expected
            )
        )
    if not entry.n_halos:
        return np.empty(0, dtype=result.record_dtype)
    return np.memmap(entry.path, dtype=result.record_dtype, mode="r", shape=(entry.n_halos,))


# ==========================================================================
# Row spool
# ==========================================================================


class _RowSpool:
    """The sorted rows, written once in global-position order and read back
    once in bounded chunks, bound to their count and CRC32."""

    def __init__(self, path: Path, dtype: np.dtype, ledger: SpillLedger, meter: ResidencyMeter):
        self.path = path
        self.dtype = dtype
        self.ledger = ledger
        self.meter = meter
        self.n_rows = 0
        self.crc = 0
        self._handle = open(str(path), "xb", buffering=SCRATCH_WRITE_BUFFER_BYTES)
        meter.acquire(SCRATCH_WRITE_BUFFER_BYTES)

    def write(self, rows: np.ndarray) -> None:
        raw = rows.view(np.uint8).reshape(-1)
        self._handle.write(raw)
        self.crc = zlib.crc32(raw, self.crc)
        self.n_rows += int(rows.size)
        self.ledger.add(raw.size)

    def close_writer(self) -> None:
        if not self._handle.closed:
            self._handle.close()
            self.meter.release(SCRATCH_WRITE_BUFFER_BYTES)

    def chunks(self, buffer: np.ndarray) -> Iterator[Tuple[int, np.ndarray]]:
        """Yield ``(first global position, rows)``; ``rows`` is a view into
        ``buffer``, valid until the next chunk."""
        itemsize = self.dtype.itemsize
        actual = os.path.getsize(str(self.path))
        if actual != self.n_rows * itemsize:
            raise ConverterError(
                "{}: row spool is {} bytes, expected {}".format(
                    self.path, actual, self.n_rows * itemsize
                )
            )
        raw = memoryview(buffer.view(np.uint8).reshape(-1))
        crc = 0
        start = 0
        with open(str(self.path), "rb", buffering=0) as handle:
            while start < self.n_rows:
                count = min(int(buffer.size), self.n_rows - start)
                wanted = count * itemsize
                got = read_into(handle, raw[:wanted])
                if got != wanted:
                    raise ConverterError(
                        "{}: row spool yielded {} of {} bytes".format(self.path, got, wanted)
                    )
                crc = zlib.crc32(raw[:wanted], crc)
                yield start, buffer[:count]
                start += count
        if crc != self.crc:
            raise ConverterError(
                "{}: row spool read back with CRC32 {} but was written with {}".format(
                    self.path, crc, self.crc
                )
            )

    def remove(self) -> None:
        self.close_writer()
        if self.path.exists():
            self.ledger.remove(self.path.stat().st_size)
            self.path.unlink()


# ==========================================================================
# The transpose
# ==========================================================================


def transpose(
    schema: CanonicalSchema,
    batches: Callable[[int], Iterable[CanonicalBatch]],
    snapshots: Sequence[int],
    out_dir,
    *,
    budget_bytes: int,
    spill_dir=None,
) -> TransposeResult:
    """Transpose canonical batches into per-snapshot, row-resolved records.

    ``batches(max_rows)`` must yield :class:`CanonicalBatch` objects of the
    given ``schema`` with at most ``max_rows`` rows each -- an adapter's bound
    ``iter_batches`` is exactly that. ``snapshots`` is every a_list snapshot
    number, ascending; each gets an output file, including those with no
    halos, and a halo whose ``SnapNum`` is not among them is refused.
    ``out_dir`` must exist and must not already contain any of the output
    names. See the module docstring for the budget and what it covers.
    """
    if not isinstance(schema, CanonicalSchema):
        raise ConverterError(
            "transpose needs a CanonicalSchema, got {!r}".format(type(schema).__name__)
        )
    snapshots = validate_snapshots(snapshots)
    plan = plan_budget(schema, budget_bytes)
    out_dir = Path(out_dir)
    if not out_dir.is_dir():
        raise ConverterError("output directory {} does not exist".format(out_dir))
    targets = [out_dir / output_name(snapshot) for snapshot in snapshots]
    existing = [str(path) for path in targets if os.path.lexists(str(path))]
    if existing:
        raise ConverterError(
            "refusing to overwrite existing transpose output: {}".format(", ".join(existing))
        )
    spill_root = Path(spill_dir) if spill_dir is not None else out_dir
    if not spill_root.is_dir():
        raise ConverterError("spill directory {} does not exist".format(spill_root))

    work = Path(tempfile.mkdtemp(prefix="transpose_", dir=str(spill_root)))
    created: List[Path] = []
    try:
        try:
            return _Transpose(schema, plan, snapshots, targets, work, created).run(batches)
        except RankSortError as exc:
            raise ConverterError("transpose: {}".format(exc)) from exc
    except BaseException:
        for path in created:
            with contextlib.suppress(FileNotFoundError):
                path.unlink()
        raise
    finally:
        shutil.rmtree(str(work), ignore_errors=True)


class _Transpose:
    """One transpose call's state; :meth:`run` drives the four phases."""

    def __init__(
        self,
        schema: CanonicalSchema,
        plan: TransposeBudget,
        snapshots: Tuple[int, ...],
        targets: Sequence[Path],
        work: Path,
        created: List[Path],
    ):
        self.schema = schema
        self.plan = plan
        self.snapshots = snapshots
        self.snapshot_array = np.asarray(snapshots, dtype=np.int64)
        self.targets = targets
        self.work = work
        self.created = created
        self.row_dtype = row_dtype(schema)
        self.out_dtype = output_dtype(schema)
        self.meter = ResidencyMeter()
        self.ledger = SpillLedger()
        self.sort_runs: Dict[str, int] = {}
        self.sort_merge_passes: Dict[str, int] = {}

    def _sorter(self, dtype, key, budget: int, tag: str) -> KeyedSorter:
        return KeyedSorter(
            dtype,
            key,
            budget_bytes=budget,
            spill_dir=self.work,
            residency=self.meter,
            ledger=self.ledger,
            tag=tag,
        )

    def _record_sort(self, tag: str, sorter: KeyedSorter) -> None:
        self.sort_runs[tag] = self.sort_runs.get(tag, 0) + sorter.n_runs
        self.sort_merge_passes[tag] = max(self.sort_merge_passes.get(tag, 0), sorter.n_merge_passes)

    def run(self, batches: Callable[[int], Iterable[CanonicalBatch]]) -> TransposeResult:
        """The four phases. **Order matters for the budget:** each sorter is
        sealed (its generation buffers released) or merged before the next
        phase's sorters are built, so no two phases' terms are ever resident
        together; :class:`TransposeBudget` splits each phase's budget between
        exactly the terms that phase holds."""
        with contextlib.ExitStack() as stack:
            rows = stack.enter_context(
                self._sorter(self.row_dtype, _ROW_KEY, self.plan.row_generation_bytes, "rows")
            )
            self._ingest(batches, rows)

            spool = _RowSpool(self.work / "rows.bin", self.row_dtype, self.ledger, self.meter)
            stack.callback(spool.remove)
            row_blocks = rows.sorted_blocks(
                budget_bytes=self.plan.row_merge_bytes,
                consumer_bytes_per_record=JOIN_BUILD_BYTES_PER_ROW,
            )
            joins = stack.enter_context(
                self._sorter(JOIN_DTYPE, JOIN_KEY, self.plan.join_generation_bytes, "join")
            )
            layout = self._sort_rows(rows, row_blocks, spool, joins)
            self._record_sort("rows", rows)
            rows.close()

            join_blocks = joins.sorted_blocks(
                budget_bytes=self.plan.join_merge_bytes,
                consumer_bytes_per_record=JOIN_SCRATCH_BYTES_PER_RECORD,
            )
            resolved = stack.enter_context(
                self._sorter(
                    RESOLVED_DTYPE, RESOLVED_KEY, self.plan.resolved_generation_bytes, "resolved"
                )
            )
            chains = self._sorter(CHAIN_DTYPE, CHAIN_KEY, self.plan.chain_generation_bytes, "chain")
            stack.callback(chains.close)
            stats = self._join(join_blocks, layout, resolved, chains)
            self._record_sort("join", joins)
            joins.close()
            # idle until assembly; sealed so it holds nothing through phase 3
            resolved.seal()

            chain_rounds = verify_chains_acyclic(
                chains,
                stats.n_chain_edges,
                layout,
                merge_budget_bytes=self.plan.chain_merge_bytes,
                new_round=lambda: self._sorter(
                    CHAIN_DTYPE, CHAIN_KEY, self.plan.chain_round_generation_bytes, "chain"
                ),
            )

            outputs = self._assemble(spool, resolved, layout, stats.n_links)
            self._record_sort("resolved", resolved)

        return TransposeResult(
            schema_digest=self.schema.digest,
            record_dtype=self.out_dtype,
            snapshots=outputs,
            total_halos=layout.total,
            n_links=dict(stats.n_links),
            n_gapped_descendants=stats.n_gapped_descendants,
            max_descendant_span=stats.max_descendant_span,
            chain_rounds=chain_rounds,
            budget=self.plan,
            peak_resident_bytes=self.meter.peak_bytes,
            peak_spill_bytes=self.ledger.peak_bytes,
            sort_runs=dict(self.sort_runs),
            sort_merge_passes=dict(self.sort_merge_passes),
        )

    # ---- phase 1a: ingest ----------------------------------------------------

    def _ingest(self, batches: Callable[[int], Iterable[CanonicalBatch]], rows: KeyedSorter):
        max_rows = self.plan.max_batch_rows
        for batch in batches(max_rows):
            if not isinstance(batch, CanonicalBatch):
                raise ConverterError(
                    "the batch source yielded {!r}, not a CanonicalBatch".format(
                        type(batch).__name__
                    )
                )
            if batch.schema != self.schema:
                raise ConverterError(
                    "a batch carries schema {} but this transpose was planned for {}".format(
                        batch.schema.digest, self.schema.digest
                    )
                )
            batch.validate()
            n_rows = batch.n_rows
            if n_rows > max_rows:
                raise ConverterError(
                    "the batch source yielded {} rows after being asked for at most {}".format(
                        n_rows, max_rows
                    )
                )
            held = _batch_nbytes(batch)
            if held > self.plan.batch_row_bytes * max(n_rows, 1):
                raise ConverterError(  # pragma: no cover - validate() fixes every width
                    "a {}-row batch holds {} bytes, more than its schema allows".format(
                        n_rows, held
                    )
                )
            scratch = n_rows * (self.row_dtype.itemsize + _INGEST_SCRATCH_BYTES_PER_ROW)
            self.meter.acquire(held + scratch)
            try:
                rows.add(self._rows_of(batch))
            finally:
                self.meter.release(held + scratch)
            # dropped before the source builds the next one, so two batches
            # are never alive at once on this side of the iterator
            del batch

    def _rows_of(self, batch: CanonicalBatch) -> np.ndarray:
        n_rows = batch.n_rows
        snap = batch.payload["SnapNum"]
        position = np.searchsorted(self.snapshot_array, snap)
        clipped = np.minimum(position, self.snapshot_array.size - 1)
        unknown = np.flatnonzero(self.snapshot_array[clipped] != snap)
        if unknown.size:
            row = int(unknown[0])
            raise ConverterError(
                "SourceHaloID {} has SnapNum {}, which is not one of the {} a_list snapshots "
                "this transpose emits".format(
                    int(batch.identity["SourceHaloID"][row]), int(snap[row]), len(self.snapshots)
                )
            )
        rows = np.empty(n_rows, dtype=self.row_dtype)
        rows[SNAPSHOT_FIELD] = snap
        for name in _IDENTITY_NAMES:
            rows[name] = batch.identity[name]
        for name in LINK_FIELDS:
            rows[SOURCE_KEY_PREFIX + name] = batch.links[name]
        for field in self.schema.payload_fields:
            rows[field.name] = batch.payload[field.name]
        for extra in self.schema.extra_fields:
            rows[extra.name] = batch.extras[extra.name]
        return rows

    # ---- phase 1b: rows sorted into global positions -------------------------

    def _sort_rows(
        self,
        rows: KeyedSorter,
        blocks: Iterator[np.ndarray],
        spool: _RowSpool,
        joins: KeyedSorter,
    ) -> SnapshotLayout:
        counts = np.zeros(len(self.snapshots), dtype=np.int64)
        position = 0
        previous: Optional[Tuple[int, int]] = None
        with contextlib.closing(blocks):
            for block in blocks:
                snap = block[SNAPSHOT_FIELD]
                ids = block["SourceHaloID"]
                self._check_row_order(snap, ids, previous)
                previous = (int(snap[-1]), int(ids[-1]))
                counts += np.bincount(
                    np.searchsorted(self.snapshot_array, snap), minlength=counts.size
                )
                joins.add(
                    build_join_records(
                        ids,
                        block["ForestIndex"],
                        {name: block[SOURCE_KEY_PREFIX + name] for name in LINK_FIELDS},
                        position,
                    )
                )
                spool.write(block)
                position += int(block.size)
        spool.close_writer()
        layout = SnapshotLayout(self.snapshots, counts)
        if layout.total != position or position != rows.n_records:
            raise ConverterError(  # pragma: no cover - the sorter's own conservation
                "row sort emitted {} of {} rows".format(position, rows.n_records)
            )
        return layout

    @staticmethod
    def _check_row_order(snap: np.ndarray, ids: np.ndarray, previous) -> None:
        """Strictly ascending ``(SnapNum, SourceHaloID)``, across blocks too:
        the sorter's own guarantee, re-checked because every row index below
        is derived from it. An equal pair is a duplicated source key."""
        if previous is not None:
            first = (int(snap[0]), int(ids[0]))
            if first <= previous:
                raise ConverterError(
                    "row order broken at SourceHaloID {} (snapshot {}): a duplicated or "
                    "out-of-order source key".format(first[1], first[0])
                )
        if snap.size > 1:
            same = snap[1:] == snap[:-1]
            bad = np.flatnonzero((snap[1:] < snap[:-1]) | (same & (ids[1:] <= ids[:-1])))
            if bad.size:
                index = int(bad[0]) + 1
                raise ConverterError(
                    "row order broken at SourceHaloID {} (snapshot {}): a duplicated or "
                    "out-of-order source key".format(int(ids[index]), int(snap[index]))
                )

    # ---- phase 2: join -------------------------------------------------------

    def _join(
        self,
        blocks: Iterator[np.ndarray],
        layout: SnapshotLayout,
        resolved: KeyedSorter,
        chains: KeyedSorter,
    ):
        join = SourceKeyJoin(layout, resolved, chains)
        with contextlib.closing(blocks):
            for block in blocks:
                join.consume(block)
        stats = join.finish()
        if stats.n_halos != layout.total:
            raise ConverterError(  # pragma: no cover - one map entry per sorted row
                "join saw {} halos of {}".format(stats.n_halos, layout.total)
            )
        return stats

    # ---- phase 4: assembly ---------------------------------------------------

    def _assemble(
        self,
        spool: _RowSpool,
        resolved: KeyedSorter,
        layout: SnapshotLayout,
        n_links: Dict[str, int],
    ) -> Tuple[SnapshotOutput, ...]:
        chunk_rows = self.plan.assembly_rows
        per_row = (
            self.row_dtype.itemsize + self.out_dtype.itemsize + _ASSEMBLY_SCRATCH_BYTES_PER_ROW
        )
        self.meter.acquire(chunk_rows * per_row)
        written = {name: 0 for name in LINK_FIELDS}
        try:
            row_buffer = np.empty(chunk_rows, dtype=self.row_dtype)
            out_buffer = np.empty(chunk_rows, dtype=self.out_dtype)
            links = resolved.sorted_blocks(
                budget_bytes=self.plan.resolved_merge_bytes,
                consumer_bytes_per_record=_SCATTER_SCRATCH_BYTES_PER_RECORD,
            )
            with contextlib.closing(links):
                # primed before any output file is opened: the merge's floor
                # check runs on the first block, and the resolved sort's own
                # intermediate passes must never overlap an output writer
                cursor = _LinkCursor(links)
                for path in self.targets:
                    with open(str(path), "xb", buffering=SCRATCH_WRITE_BUFFER_BYTES):
                        pass
                    self.created.append(path)
                writer = _SnapshotWriter(self.targets, layout, self.meter)
                try:
                    for first, rows in spool.chunks(row_buffer):
                        out = out_buffer[: rows.size]
                        self._resolve_chunk(first, rows, out, layout, cursor, written)
                        writer.write(first, out)
                finally:
                    writer.close()
                if cursor.block is not None:
                    raise ConverterError(
                        "resolved links remain for global position {} beyond the {} rows".format(
                            int(cursor.block["owner_gp"][cursor.offset]), layout.total
                        )
                    )
        finally:
            self.meter.release(chunk_rows * per_row)

        for name in LINK_FIELDS:
            if written[name] != n_links[name]:
                raise ConverterError(  # pragma: no cover - guarded per chunk below
                    "{}: {} links resolved, {} written".format(name, n_links[name], written[name])
                )
        outputs = []
        for index, (snapshot, path) in enumerate(zip(self.snapshots, self.targets)):
            n_halos = int(layout.counts[index])
            size = os.path.getsize(str(path))
            if size != n_halos * self.out_dtype.itemsize:
                raise ConverterError(
                    "{} is {} bytes after assembly; {} rows need {}".format(
                        path, size, n_halos, n_halos * self.out_dtype.itemsize
                    )
                )
            outputs.append(
                SnapshotOutput(
                    snapshot=snapshot,
                    path=str(path),
                    n_halos=n_halos,
                    first_global_position=int(layout.offsets[index]),
                )
            )
        return tuple(outputs)

    def _resolve_chunk(
        self,
        first: int,
        rows: np.ndarray,
        out: np.ndarray,
        layout: SnapshotLayout,
        cursor: "_LinkCursor",
        written: Dict[str, int],
    ) -> None:
        """Fill the output rows of global positions ``first .. first +
        len(rows)``, taking every resolved link those rows own from
        ``cursor`` and counting each written link in ``written``."""
        n_rows = int(rows.size)
        end = first + n_rows

        # the rows' own SnapNum must agree with the layout the positions were
        # cut from; a disagreement would put rows into the wrong file
        expected = layout.snapshots[layout.positions(np.arange(first, end, dtype=np.int64))]
        if not np.array_equal(rows[SNAPSHOT_FIELD], expected):
            raise ConverterError(  # pragma: no cover - both derive from the same sort
                "rows {}..{} disagree with the snapshot layout".format(first, end)
            )

        for name in LINK_FIELDS:
            out[name] = NULL_LINK
        for name in SNAPSHOT_LINK_FIELDS:
            out[name] = NULL_LINK
        for name in _IDENTITY_NAMES:
            out[name] = rows[name]
        for field in self.schema.payload_fields:
            out[field.name] = rows[field.name]
        for extra in self.schema.extra_fields:
            out[extra.name] = rows[extra.name]

        for segment in cursor.take_below(end):
            self._scatter_links(segment, first, out, layout, written)

        # every non-null source link resolved exactly once, and no null one
        for name in LINK_FIELDS:
            source_null = rows[SOURCE_KEY_PREFIX + name] == NULL_LINK
            output_null = out[name] == NULL_LINK
            if not np.array_equal(source_null, output_null):
                bad = int(np.flatnonzero(source_null != output_null)[0])
                raise ConverterError(
                    "{} of SourceHaloID {} ({}) was not resolved exactly once".format(
                        name, int(rows["SourceHaloID"][bad]), layout.describe(first + bad)
                    )
                )

    @staticmethod
    def _scatter_links(
        segment: np.ndarray,
        first: int,
        out: np.ndarray,
        layout: SnapshotLayout,
        written: Dict[str, int],
    ) -> None:
        """Write one segment of resolved links into the chunk whose row 0 is
        global position ``first``. A call of its own so that the segment's
        arrays are freed before the next segment is located
        (:data:`_SCATTER_SCRATCH_BYTES_PER_RECORD`)."""
        local = segment["owner_gp"] - first
        if int(local.min()) < 0:
            raise ConverterError(  # pragma: no cover - resolved links are owner-sorted
                "resolved link for global position {} arrived after its row".format(
                    int(segment["owner_gp"][int(np.argmin(local))])
                )
            )
        target_snapshot, target_row = layout.locate(segment["target_gp"])
        kinds = segment["kind"]
        for kind, name in enumerate(LINK_FIELDS):
            mask = kinds == kind
            if not bool(np.any(mask)):
                continue
            slots = local[mask]
            out[name][slots] = target_row[mask]
            column = _SNAPSHOT_COLUMN.get(name)
            if column is not None:
                out[column][slots] = target_snapshot[mask]
            written[name] += int(slots.size)


class _LinkCursor:
    """Read position in the owner-sorted stream of resolved links.

    ``block`` is the stream's current block -- a view the stream reuses,
    valid until the next block is requested -- or ``None`` once the stream is
    exhausted; ``offset`` is its first link not yet taken. Constructing the
    cursor requests the first block, which primes the merge behind it.
    """

    def __init__(self, blocks: Iterator[np.ndarray]):
        self._blocks = blocks
        self.block: Optional[np.ndarray] = next(blocks, None)
        self.offset = 0

    def take_below(self, end: int) -> Iterator[np.ndarray]:
        """Yield, in stream order, every remaining link whose owner's global
        position is below ``end``, as contiguous segments of the stream's
        blocks. A segment is valid only until the next one is requested."""
        while self.block is not None:
            owners = self.block["owner_gp"]
            stop = self.offset + int(np.searchsorted(owners[self.offset :], end, side="left"))
            if stop > self.offset:
                yield self.block[self.offset : stop]
            self.offset = stop
            if self.offset < self.block.size:
                return
            self.block = next(self._blocks, None)
            self.offset = 0


class _SnapshotWriter:
    """Appends output rows to the file of the snapshot they belong to. Rows
    arrive in global-position order, so one file is open at a time."""

    def __init__(self, targets: Sequence[Path], layout: SnapshotLayout, meter: ResidencyMeter):
        self.targets = targets
        self.layout = layout
        self.meter = meter
        self._position = -1
        self._handle = None

    def _switch(self, position: int) -> None:
        self.close()
        self._handle = open(str(self.targets[position]), "ab", buffering=SCRATCH_WRITE_BUFFER_BYTES)
        self.meter.acquire(SCRATCH_WRITE_BUFFER_BYTES)
        self._position = position

    def write(self, first: int, out: np.ndarray) -> None:
        """Write rows ``first .. first + len(out)`` (global positions)."""
        end = first + int(out.size)
        offsets = self.layout.offsets
        position = int(self.layout.positions(np.asarray([first], dtype=np.int64))[0])
        cursor = first
        while cursor < end:
            stop = min(end, int(offsets[position + 1]))
            if stop > cursor:
                if position != self._position:
                    self._switch(position)
                piece = out[cursor - first : stop - first]
                self._handle.write(piece.view(np.uint8).reshape(-1))
                cursor = stop
            position += 1

    def close(self) -> None:
        if self._handle is not None:
            self._handle.close()
            self._handle = None
            self.meter.release(SCRATCH_WRITE_BUFFER_BYTES)


def _batch_nbytes(batch: CanonicalBatch) -> int:
    total = 0
    for mapping in (batch.identity, batch.coordinates, batch.links, batch.payload, batch.extras):
        for values in mapping.values():
            total += int(np.asarray(values).nbytes)
    return total
