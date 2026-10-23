"""The forest assembler: each forest of a ``ForestIndex`` range of a forest-blocked version 3
dataset as one L-Halo record array in rank order, with forest-local links.

**The pass.** Every slab of a version 3 dataset is grouped by forest in ascending ``ForestIndex``
(``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``, Source Identity), so a forest's rows in a
slab are one contiguous block and consecutive forests' blocks are consecutive.
:meth:`ForestAssembler.forests` finds, in every slab, the rows of the range by two binary
searches on ``ForestIndex`` (the first row of the range's first forest and the first row at or
past its stop), then streams the slab's rows of the range forward in windows of ``window_rows``
rows, always reading from the slab that lags furthest behind. A forest is complete once every
slab has read past it or has read all of its rows of the range; a slab in which it is absent
contributes an empty block. The forests the windows complete are assembled together, in batches
of about ``window_rows`` rows (a forest larger than that is a batch of its own), so the
per-forest Python work is a view and a yield, whatever the number of forests.

**The record.** A forest's records are placed by ``HaloRankInForest``: the record at position
``rank`` of the forest holds the halo of that rank, so the rank is the record's position whatever
convention the source route gives it (the reference vertical-driver order for the Consistent-Trees
ASCII route, the source's within-tree row for the L-Halo and forests-HDF5 routes, which is not
ordered by snapshot). The assembler requires the ranks of a forest's rows to be a permutation of
0..n-1 and refuses otherwise, naming the forest and the first rank out of range or repeated; a
row of another forest inside a block is refused as a ``ForestIndex`` out of order. The payload
fields are copied verbatim from the source columns into the record
:func:`vertical.record.record_dtype` derives for the named package (no unit, type or convention
change), so every payload column must already have the record field's dtype and shape, which is
checked before any row is read.

**Links.** Each of the five links is mapped from (target snapshot, global row), the snapshot
from its companion column for ``Descendant``, ``FirstProgenitor`` and ``NextProgenitor`` and the
owner's own for the FoF links, to the target's ``HaloRankInForest``, read from the forest's own
block of the target slab; a non-null link whose target lies outside that block, or whose
companion names a slab in which the forest has no block, is refused naming the forest, the
halo's rank and the link. ``-1`` maps to ``-1``; ``FirstHaloInFOFgroup`` is never ``-1``. A mapped
rank the record's link type cannot hold is refused naming the forest.

**Memory.** Each slab keeps the windows it has read and not yet emitted as they were read, and
rows are copied once, out of those windows into the batch they are assembled with.
:func:`memory_bound` states the bound per assembler instance: the buffered windows (per slab,
its block of the forest in flight, one window read beyond it within the range and the
view-retained remainder of one more) and the batch being assembled (its columns, its records and
a measured allowance of temporaries). No slab reads a row at or past its first row of the range's
stop. The assembler holds no other per-forest state.
"""

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple, Union

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from vertical.common import require_positive_int  # noqa: E402
from vertical.record import record_dtype  # noqa: E402

#: Rows per window read from each slab: 2^20 rows is 8 MiB of an int64 column.
DEFAULT_WINDOW_ROWS = 1 << 20

#: The five links of an L-Halo record, in the format's order.
LINK_FIELDS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)

#: The companion target-snapshot column of each link that may leave its owner's slab; the FoF
#: links stay in the owner's slab.
LINK_SNAPSHOT_COLUMNS = {
    "Descendant": "DescendantSnapshot",
    "FirstProgenitor": "FirstProgenitorSnapshot",
    "NextProgenitor": "NextProgenitorSnapshot",
}

#: The null link, in the source and in the record.
NULL_LINK = -1

#: The allowance per batch row for the assembler's temporaries in :func:`memory_bound`: the
#: per-row snapshot, record positions, the permutation check's counts and, per link kind, the
#: targets, masks and mapped ranks, plus the per-forest counts and offsets. Measured with
#: ``tracemalloc`` over one batch at 120 B per row for a single forest in which every link of
#: every row is non-null (the most the per-link temporaries can hold) and 136 B per row when
#: every forest is one halo (the most the per-forest arrays can add), plus 24 B of headroom
#: that also covers the 8 B per forest of cumulative counts :meth:`ForestAssembler.forests`
#: keeps while a batch is emitted.
WORKING_BYTES_PER_ROW = 160

_FOREST = "ForestIndex"
_RANK = "HaloRankInForest"
_INT64 = np.dtype("<i8")
_INT32 = np.dtype("<i4")

#: The frontier of a slab that has no row left to read for the range.
_COMPLETE = np.iinfo(np.int64).max


@dataclass(frozen=True)
class AssembledForest:
    """One forest: its ``ForestIndex`` and its records in rank order, a view into the batch
    the forest was assembled with (holding it keeps that batch alive)."""

    forest: int
    records: np.ndarray


def memory_bound(
    window_rows: int,
    n_slabs: int,
    largest_forest_halos: int,
    row_bytes: int,
    record_bytes: int,
) -> int:
    """The bytes one assembler instance holds at most, for a range whose largest forest has
    ``largest_forest_halos`` halos.

    The buffered windows hold at most ``2 * n_slabs * window_rows + largest_forest_halos`` rows
    of ``row_bytes`` each: per slab, its block of the forest in flight (those blocks sum to at
    most the forest's total), one window of rows read beyond it (within the range: no slab reads
    past its end row), and up to one more window kept alive by the view of its oldest, partly
    emitted window. The batch being assembled holds at
    most ``max(window_rows, largest_forest_halos)`` rows (the forests a set of windows
    completes are assembled in batches of about ``window_rows`` rows; a larger forest is a
    batch alone), each as its columns (``row_bytes``, copied out of the windows), its record
    (``record_bytes``) and :data:`WORKING_BYTES_PER_ROW` of temporaries; nothing else is
    concatenated, so no other transient reaches the size of a batch.
    """
    held = 2 * n_slabs * window_rows + largest_forest_halos
    batch = max(window_rows, largest_forest_halos)
    return held * row_bytes + batch * (row_bytes + record_bytes + WORKING_BYTES_PER_ROW)


class _Slab:
    """One slab's cursor over the rows of a ``ForestIndex`` range: the next unread row, the end
    of the range's rows, and the windows read but not yet emitted.

    The windows are kept as they were read (``chunks``, oldest first, the oldest possibly a
    view of its window's unemitted tail); nothing is concatenated until rows are taken into a
    batch, so every row is copied once however many windows a forest's block spans.
    """

    def __init__(self, snap: int, n_rows: int, first: int):
        self.snap = snap
        self.n_rows = n_rows
        self.next_row = 0
        #: the first row at or past the range's stop (set once the search has run)
        self.end_row = n_rows
        #: the global row of the first buffered row
        self.base_row = 0
        self.chunks: List[Dict[str, np.ndarray]] = []
        #: rows over every chunk
        self.length = 0
        #: the ``ForestIndex`` of the last buffered row, the range's first forest before any
        self.last_forest = first

    @property
    def exhausted(self) -> bool:
        return self.next_row >= self.end_row

    def frontier(self) -> int:
        """The first forest this slab is not known to have read past: every forest below it
        is complete here. A slab with nothing buffered and rows unread must be read first."""
        if self.exhausted:
            return _COMPLETE
        return self.last_forest if self.length else -1


class ForestAssembler:
    """Assembles the forests of a version 3 dataset into a package's L-Halo records.

    Args:
        dataset: the open dataset.
        halo_properties: the package's ``halo_properties.yaml`` path; the record is
            :func:`vertical.record.record_dtype` of it.
        window_rows: rows per window read from each slab.

    Raises:
        ConverterError: for a ``window_rows`` that is not a positive integer; a record without
            the five links or with links of differing types; a record field, identity or
            topology column that a slab lacks; or a column whose dtype or shape is not the
            record field's (payload) or the format's (``int64`` identity and links, ``int32``
            target snapshots), naming the slab and the column, before any row is read.
    """

    def __init__(
        self,
        dataset: HorizontalDataset,
        halo_properties: Union[str, Path],
        window_rows: int = DEFAULT_WINDOW_ROWS,
    ):
        self.dataset = dataset
        self.window_rows = require_positive_int("window_rows", window_rows)
        self.dtype = record_dtype(halo_properties)
        missing = [name for name in LINK_FIELDS if name not in self.dtype.names]
        if missing:
            raise ConverterError(
                "the record lacks the link field(s) {}; an L-Halo record carries all five".format(
                    ", ".join(missing)
                )
            )
        link_type = self.dtype.fields[LINK_FIELDS[0]][0]
        if not np.issubdtype(link_type, np.integer) or any(
            self.dtype.fields[name][0] != link_type for name in LINK_FIELDS
        ):
            raise ConverterError(
                "the record's five links must share one integer type, got {}".format(
                    [str(self.dtype.fields[name][0]) for name in LINK_FIELDS]
                )
            )
        self.link_dtype = link_type
        #: the largest rank the record's link type holds
        self.link_limit = int(np.iinfo(self.link_dtype).max)
        self.payload_fields = tuple(name for name in self.dtype.names if name not in LINK_FIELDS)

        expected: Dict[str, Tuple[np.dtype, Tuple[int, ...], str]] = {}
        for name in (_FOREST, _RANK):
            expected[name] = (_INT64, (), "the format's int64")
        for name in LINK_FIELDS:
            expected[name] = (_INT64, (), "the format's int64")
        for name in LINK_SNAPSHOT_COLUMNS.values():
            expected[name] = (_INT32, (), "the format's int32")
        for name in self.payload_fields:
            field = self.dtype.fields[name][0]
            expected[name] = (
                field.base,
                tuple(field.shape),
                "the record's {} {}{}".format(name, field.base, list(field.shape) or ""),
            )
        #: the columns each window reads, in a fixed order
        self.columns = tuple(expected)
        self.row_bytes = sum(
            int(base.itemsize) * int(np.prod(shape, dtype=np.int64))
            for base, shape, _ in expected.values()
        )
        for snap in dataset.snapshots:
            present = dataset.halo_dtypes(snap)
            for name, (base, shape, what) in expected.items():
                if name not in present:
                    raise ConverterError(
                        "{}: no /halos/{} column, which {} needs".format(
                            dataset.snapshot_path(snap), name, what
                        )
                    )
                found, found_shape = present[name]
                if np.dtype(found) != base or tuple(found_shape) != shape:
                    raise ConverterError(
                        "{}: /halos/{} is {}{}, not {}".format(
                            dataset.snapshot_path(snap),
                            name,
                            np.dtype(found),
                            list(found_shape) or "",
                            what,
                        )
                    )

    def memory_bound(self, largest_forest_halos: int) -> int:
        """:func:`memory_bound` for this instance's window, slab count, columns and record."""
        return memory_bound(
            self.window_rows,
            len(self.dataset.snapshots),
            int(largest_forest_halos),
            self.row_bytes,
            int(self.dtype.itemsize),
        )

    # ---- the pass ---------------------------------------------------------

    def forests(
        self, first: int, stop: int, expected_totals: Optional[Sequence[int]] = None
    ) -> Iterator[AssembledForest]:
        """Yield every forest of ``[first, stop)`` in ascending ``ForestIndex``, each as its
        records in rank order with forest-local links.

        Args:
            first, stop: the ``ForestIndex`` range, within ``[0, n_forests_total)``.
            expected_totals: optionally, the halo total of each forest of the range alone, in
                order (index 0 is forest ``first``; the slice of
                :func:`vertical.plan.forest_totals` over the range); a forest whose assembled
                halo count differs is refused.

        Raises:
            ConverterError: for a range outside the dataset; ``expected_totals`` of a length
                other than the range's; a slab whose ``ForestIndex`` is not non-decreasing over
                its rows of the range (the rows outside the range are never read, so the whole
                dataset's order is :func:`vertical.plan.forest_totals`' check); a forest with no
                rows in any slab or with a halo count other than its expected total; ranks that
                are not a permutation of 0..n-1; a link that is malformed, leaves the forest's
                block of its target slab, or names a slab the forest has no block in; or a rank
                the record's link type cannot hold. Each names the forest.
        """
        first, stop = self._check_range(first, stop)
        if expected_totals is not None and len(expected_totals) != stop - first:
            raise ConverterError(
                "{}: {} expected totals for the {} forests of [{}, {})".format(
                    self.dataset.directory, len(expected_totals), stop - first, first, stop
                )
            )
        slabs = [
            _Slab(snap, n, first) for snap, n in zip(self.dataset.snapshots, self.dataset.n_halos)
        ]
        for slab in slabs:
            slab.next_row = slab.base_row = self._first_row(slab, first)
            slab.end_row = self._first_row(slab, stop, low=slab.next_row)
        next_emit = first
        while next_emit < stop:
            frontiers = [slab.frontier() for slab in slabs]
            frontier = min(frontiers)
            if frontier <= next_emit:
                self._read_window(slabs[frontiers.index(frontier)])
                continue
            batch_stop = min(frontier, stop)
            cumulative = self._check_totals(slabs, first, next_emit, batch_stop, expected_totals)
            chunk_first = next_emit
            while chunk_first < batch_stop:
                index = chunk_first - next_emit
                before = int(cumulative[index - 1]) if index else 0
                fits = int(np.searchsorted(cumulative, before + self.window_rows, "right"))
                chunk_stop = next_emit + max(fits, index + 1)
                yield from self._emit(slabs, chunk_first, chunk_stop)
                chunk_first = chunk_stop
            next_emit = batch_stop

    def _check_range(self, first, stop) -> Tuple[int, int]:
        total = self.dataset.n_forests_total
        if (
            isinstance(first, bool)
            or isinstance(stop, bool)
            or not isinstance(first, (int, np.integer))
            or not isinstance(stop, (int, np.integer))
            or not 0 <= int(first) < int(stop) <= total
        ):
            raise ConverterError(
                "{}: forest range [{}, {}) is not within [0, {})".format(
                    self.dataset.directory, first, stop, total
                )
            )
        return int(first), int(stop)

    def _first_row(self, slab: _Slab, forest: int, low: int = 0) -> int:
        """The first row at or after ``low`` whose ``ForestIndex`` is at least ``forest``, by
        binary search over single-row reads of ``ForestIndex`` (``n_rows`` when there is none)."""
        high = slab.n_rows
        while low < high:
            mid = (low + high) // 2
            value = int(self.dataset.read_rows(slab.snap, (_FOREST,), mid, mid + 1)[_FOREST][0])
            if value < forest:
                low = mid + 1
            else:
                high = mid
        return low

    def _read_window(self, slab: _Slab) -> None:
        """Append the slab's next window, which ends at most at its end row, to its buffer,
        checking ``ForestIndex`` continues non-decreasing from the last buffered row (or from
        the range's first forest)."""
        stop = min(slab.next_row + self.window_rows, slab.end_row)
        window = self.dataset.read_rows(slab.snap, self.columns, slab.next_row, stop)
        forest = window[_FOREST]
        before = np.concatenate(([slab.last_forest], forest[:-1]))
        drops = np.flatnonzero(forest < before)
        if drops.size:
            row = int(drops[0])
            raise ConverterError(
                "{}: ForestIndex is not non-decreasing: row {} holds {} after {}".format(
                    self.dataset.snapshot_path(slab.snap),
                    slab.next_row + row,
                    forest[row],
                    before[row],
                )
            )
        slab.chunks.append(window)
        slab.length += forest.size
        slab.last_forest = int(forest[-1])
        slab.next_row = stop

    def _check_totals(
        self, slabs: List[_Slab], range_first: int, first: int, stop: int, expected_totals
    ) -> np.ndarray:
        """Every forest of ``[first, stop)`` has rows, and its expected total if one is given
        (``expected_totals`` indexed from the range's first forest, ``range_first``); returns
        the cumulative row count over the forests."""
        totals = np.zeros(stop - first, dtype=np.int64)
        edges = np.arange(first, stop + 1, dtype=np.int64)
        for slab in slabs:
            for chunk in slab.chunks:
                totals += np.diff(np.searchsorted(chunk[_FOREST], edges, "left"))
        empty = np.flatnonzero(totals == 0)
        if empty.size:
            raise ConverterError(
                "{}: forest {} has no rows in any slab".format(
                    self.dataset.directory, first + int(empty[0])
                )
            )
        if expected_totals is not None:
            planned = np.asarray(
                expected_totals[first - range_first : stop - range_first], dtype=np.int64
            )
            differs = np.flatnonzero(planned != totals)
            if differs.size:
                index = int(differs[0])
                raise ConverterError(
                    "{}: forest {} has {} halos, but the plan expects {}".format(
                        self.dataset.directory, first + index, totals[index], planned[index]
                    )
                )
        return np.cumsum(totals)

    def _emit(self, slabs: List[_Slab], first: int, stop: int) -> Iterator[AssembledForest]:
        """Assemble the forests ``[first, stop)`` from the buffered windows, dropping the rows
        taken from each slab (the oldest remaining window becomes a view of its tail), and
        yield them one by one."""
        slices = []
        parts = []
        for slab in reversed(slabs):
            taken = 0
            while slab.chunks:
                chunk = slab.chunks[0]
                rows = int(np.searchsorted(chunk[_FOREST], stop, "left"))
                if rows == 0:
                    break
                if rows == chunk[_FOREST].size:
                    parts.append(slab.chunks.pop(0))
                else:
                    parts.append({name: chunk[name][:rows] for name in self.columns})
                    slab.chunks[0] = {name: chunk[name][rows:] for name in self.columns}
                taken += rows
                if rows < chunk[_FOREST].size:
                    break
            slices.append((slab, taken))
        columns = {name: np.concatenate([part[name] for part in parts]) for name in self.columns}
        n_slabs = len(slabs)
        snapshot = np.concatenate(
            [np.full(n, slab.snap, dtype=np.int32) for slab, n in slices if n]
        )
        batch_start = np.zeros(n_slabs, dtype=np.int64)
        row_base = np.zeros(n_slabs, dtype=np.int64)
        slice_rows = np.zeros(n_slabs, dtype=np.int64)
        start = 0
        for slab, n in slices:
            batch_start[slab.snap] = start
            row_base[slab.snap] = slab.base_row
            slice_rows[slab.snap] = n
            start += n
            slab.base_row += n
            slab.length -= n
        records, offsets = self._assemble(
            first, stop, columns, snapshot, batch_start, row_base, slice_rows
        )
        bounds = offsets.tolist()
        for index in range(stop - first):
            yield AssembledForest(first + index, records[bounds[index] : bounds[index + 1]])

    def _assemble(self, first, stop, columns, snapshot, batch_start, row_base, slice_rows):
        """The records of the forests ``[first, stop)`` from their rows in concatenation
        order (highest snapshot first), and each forest's offset into them."""
        where = str(self.dataset.directory)
        forest = columns[_FOREST] - first
        rank = columns[_RANK]
        n_rows = forest.size
        counts = np.bincount(forest, minlength=stop - first)
        offsets = np.zeros(stop - first + 1, dtype=np.int64)
        np.cumsum(counts, out=offsets[1:])
        outside = np.flatnonzero((rank < 0) | (rank >= counts[forest]))
        if outside.size:
            at = int(outside[0])
            raise ConverterError(
                "{}: forest {}: rank {} is outside [0, {}), the forest's halo count".format(
                    where, first + int(forest[at]), rank[at], counts[forest[at]]
                )
            )
        position = offsets[forest] + rank
        taken = np.bincount(position, minlength=n_rows)
        repeated = np.flatnonzero(taken > 1)
        if repeated.size:
            owner = int(np.searchsorted(offsets, repeated[0], "right")) - 1
            raise ConverterError(
                "{}: forest {}: rank {} is held by more than one halo (the {} halos' ranks "
                "must be 0..{} once each)".format(
                    where,
                    first + owner,
                    repeated[0] - offsets[owner],
                    counts[owner],
                    counts[owner] - 1,
                )
            )
        records = np.zeros(n_rows, dtype=self.dtype)
        for name in self.payload_fields:
            records[name][position] = columns[name]
        for name in LINK_FIELDS:
            companion = LINK_SNAPSHOT_COLUMNS.get(name)
            target_snap = columns[companion] if companion else snapshot
            source, mapped = _map_links(
                where,
                first,
                name,
                forest,
                rank,
                columns[name],
                target_snap,
                batch_start,
                row_base,
                slice_rows,
            )
            if source.size:
                over = np.flatnonzero(mapped[source] > self.link_limit)
                if over.size:
                    at = int(source[over[0]])
                    raise ConverterError(
                        "{}: forest {}: rank {} does not fit the record's link type {}".format(
                            where, first + int(forest[at]), mapped[at], self.link_dtype
                        )
                    )
            records[name][position] = mapped.astype(self.link_dtype)
        return records, offsets


def _map_links(
    where, first, name, forest, rank, link, target_snap, batch_start, row_base, slice_rows
):
    """Map one link kind from (target snapshot, global row) to the target's rank.

    Returns ``(source, mapped)``: the batch indices of the rows whose link is not null, and
    every row's mapped rank (``-1`` where null).

    Raises:
        ConverterError: for a null ``FirstHaloInFOFgroup``; a companion snapshot that is ``-1``
            without its link being ``-1`` or the reverse; a target snapshot the dataset has no
            slab for; or a target row outside the forest's block of the target slab (in
            particular a slab in which the forest has no block), naming the forest, the halo's
            rank, the link and its target.
    """
    null = link == NULL_LINK
    if name == "FirstHaloInFOFgroup" and null.any():
        at = int(np.argmax(null))
        raise ConverterError(
            "{}: forest {}, rank {}: FirstHaloInFOFgroup is -1 (a central names itself)".format(
                where, first + int(forest[at]), rank[at]
            )
        )
    companion = LINK_SNAPSHOT_COLUMNS.get(name)
    if companion:
        mismatch = np.flatnonzero((target_snap == NULL_LINK) != null)
        if mismatch.size:
            at = int(mismatch[0])
            raise ConverterError(
                "{}: forest {}, rank {}: {} is {} but {} is {}".format(
                    where,
                    first + int(forest[at]),
                    rank[at],
                    name,
                    link[at],
                    companion,
                    target_snap[at],
                )
            )
    source = np.flatnonzero(~null)
    mapped = np.full(forest.size, NULL_LINK, dtype=np.int64)
    if not source.size:
        return source, mapped
    snaps = target_snap[source].astype(np.int64)
    rows = link[source]
    owners = forest[source]
    outside = np.flatnonzero((snaps < 0) | (snaps >= batch_start.size))
    if outside.size:
        at = int(source[outside[0]])
        raise ConverterError(
            "{}: forest {}, rank {}: {} -> row {} names snapshot {}, which the dataset has no "
            "slab for".format(
                where, first + int(forest[at]), rank[at], name, link[at], target_snap[at]
            )
        )
    local = rows - row_base[snaps]
    inside = (local >= 0) & (local < slice_rows[snaps])
    target = np.zeros(source.size, dtype=np.int64)
    target[inside] = batch_start[snaps[inside]] + local[inside]
    good = inside.copy()
    good[inside] = forest[target[inside]] == owners[inside]
    if not good.all():
        bad = int(np.argmin(good))
        at = int(source[bad])
        snap = int(snaps[bad])
        block = forest[batch_start[snap] : batch_start[snap] + slice_rows[snap]]
        low = int(row_base[snap] + np.searchsorted(block, owners[bad], "left"))
        high = int(row_base[snap] + np.searchsorted(block, owners[bad], "right"))
        if low == high:
            reason = "names snapshot {}, in which the forest has no block".format(snap)
        else:
            reason = "lies outside the forest's block of snapshot {} (rows [{}, {}))".format(
                snap, low, high
            )
        raise ConverterError(
            "{}: forest {}, rank {}: {} -> row {} {}".format(
                where, first + int(forest[at]), rank[at], name, link[at], reason
            )
        )
    mapped[source] = rank[target]
    return source, mapped
