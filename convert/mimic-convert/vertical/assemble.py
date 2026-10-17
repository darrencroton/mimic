"""The forest assembler: each forest of a ``ForestIndex`` range of a forest-blocked version 3
dataset as one L-Halo record array in rank order, with forest-local links, every link verified.

**The pass.** Every slab of a version 3 dataset is grouped by forest in ascending ``ForestIndex``
(``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``, Source Identity), so a forest's rows in a
slab are one contiguous block and consecutive forests' blocks are consecutive.
:meth:`ForestAssembler.forests` finds the first row of the range's first forest in every slab by
binary search on ``ForestIndex``, then streams every slab forward in windows of ``window_rows``
rows, always reading from the slab that lags furthest behind. A forest is complete once every
slab has read past it or is exhausted; a slab in which it is absent contributes an empty block.
The forests the windows complete are assembled together, in batches of about ``window_rows``
rows (a forest larger than that is a batch of its own), so the per-forest Python work is a view
and a yield, whatever the number of forests.

**The record.** A forest's records are its slab blocks concatenated from the highest snapshot
down to the lowest, each block in row order: the reference vertical-driver order, in which
``HaloRankInForest`` is the halo's index. The assembler requires the concatenated ranks to be
exactly 0..n-1 and refuses otherwise, naming the forest and the first offending rank; a row of
another forest inside a block is refused as a ``ForestIndex`` out of order. The payload fields
are copied verbatim from the source columns into the record :func:`vertical.record.record_dtype`
derives for the named package (no unit, type or convention change), so every payload column must
already have the record field's dtype and shape, which is checked before any row is read.

**Links.** Each of the five links is mapped from (target snapshot, global row), the snapshot
from its companion column for ``Descendant``, ``FirstProgenitor`` and ``NextProgenitor`` and the
owner's own for the FoF links, to the target's ``HaloRankInForest``, read from the forest's own
block of the target slab; a non-null link whose target lies outside that block, or whose
companion names a slab in which the forest has no block, is refused naming the forest, the
halo's rank and the link. ``-1`` maps to ``-1``; ``FirstHaloInFOFgroup`` is never ``-1``. Every
mapped link is then verified: the ``MostBoundID`` of the target row must equal the
``MostBoundID`` of the record the mapped rank names. A mapped rank the record's link type cannot
hold is refused naming the forest.

**Memory.** :func:`memory_bound` states the bound per assembler instance: the windows (every
slab's unread tail of the forest in flight plus one window) and the batch being assembled (its
columns, its records and the index and mask temporaries). The assembler holds no other
per-forest state.
"""

import os
import sys
from dataclasses import dataclass
from typing import Dict, Iterator, List, Optional, Sequence, Tuple, Union

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
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

#: The allowance per batch row for the assembler's index and mask temporaries (the stable sort
#: order, record positions, per-link targets and mapped ranks), in :func:`memory_bound`.
WORKING_BYTES_PER_ROW = 128

_FOREST = "ForestIndex"
_RANK = "HaloRankInForest"
_MOST_BOUND = "MostBoundID"
_INT64 = np.dtype("<i8")
_INT32 = np.dtype("<i4")

#: The frontier of a slab that has nothing left to read for the range.
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

    The windows hold at most ``n_slabs * window_rows + largest_forest_halos`` rows of
    ``row_bytes`` each (per slab, one window plus the slab's block of the forest in flight, and
    those blocks sum to the forest's total). The batch being assembled holds at most
    ``max(window_rows, largest_forest_halos)`` rows, each as its columns (``row_bytes``), its
    record (``record_bytes``) and :data:`WORKING_BYTES_PER_ROW` of temporaries.
    """
    held = n_slabs * window_rows + largest_forest_halos
    batch = max(window_rows, largest_forest_halos)
    return held * row_bytes + batch * (row_bytes + record_bytes + WORKING_BYTES_PER_ROW)


def _require_window_rows(window_rows) -> int:
    if isinstance(window_rows, bool) or not isinstance(window_rows, (int, np.integer)):
        raise ConverterError("window_rows must be a positive integer, got {!r}".format(window_rows))
    if int(window_rows) < 1:
        raise ConverterError("window_rows must be a positive integer, got {}".format(window_rows))
    return int(window_rows)


class _Slab:
    """One slab's cursor: the next unread row, and the rows read but not yet emitted."""

    def __init__(self, snap: int, n_rows: int):
        self.snap = snap
        self.n_rows = n_rows
        self.next_row = 0
        #: the global row of ``buffer[...][0]``
        self.base_row = 0
        self.buffer: Dict[str, np.ndarray] = {}
        self.length = 0

    @property
    def exhausted(self) -> bool:
        return self.next_row >= self.n_rows

    def frontier(self, stop: int) -> int:
        """The first forest this slab is not known to have read past: every forest below it
        is complete here. A slab with nothing buffered and rows unread must be read first."""
        if self.length == 0:
            return _COMPLETE if self.exhausted else -1
        last = int(self.buffer[_FOREST][-1])
        return _COMPLETE if (last >= stop or self.exhausted) else last


class ForestAssembler:
    """Assembles the forests of a version 3 dataset into a package's L-Halo records.

    Args:
        dataset: the open dataset.
        halo_properties: the package's ``halo_properties.yaml`` path, or its parsed list; the
            record is :func:`vertical.record.record_dtype` of it.
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
        halo_properties: Union[str, "os.PathLike[str]", Sequence[dict]],
        window_rows: int = DEFAULT_WINDOW_ROWS,
    ):
        self.dataset = dataset
        self.window_rows = _require_window_rows(window_rows)
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
        for name in (_FOREST, _RANK, _MOST_BOUND):
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
            int(base.itemsize) * int(np.prod(shape, dtype=np.int64)) if shape else base.itemsize
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
            expected_totals: optionally, every forest's halo total indexed by ``ForestIndex``
                (as :func:`vertical.plan.forest_totals` returns it); a forest whose assembled
                halo count differs is refused.

        Raises:
            ConverterError: for a range outside the dataset; a slab whose ``ForestIndex`` is
                not non-decreasing; a forest with no rows in any slab or with a halo count
                other than its expected total; ranks that are not exactly 0..n-1; a link that
                is malformed, leaves the forest's block of its target slab, or names a slab the
                forest has no block in; a mapped rank whose ``MostBoundID`` is not the target
                row's; or a rank the record's link type cannot hold. Each names the forest.
        """
        first, stop = self._check_range(first, stop)
        slabs = [_Slab(snap, n) for snap, n in zip(self.dataset.snapshots, self.dataset.n_halos)]
        for slab in slabs:
            slab.next_row = slab.base_row = self._first_row(slab, first)
        next_emit = first
        while next_emit < stop:
            frontiers = [slab.frontier(stop) for slab in slabs]
            frontier = min(frontiers)
            if frontier <= next_emit:
                self._read_window(slabs[frontiers.index(frontier)], first)
                continue
            batch_stop = min(frontier, stop)
            cumulative = self._check_totals(slabs, next_emit, batch_stop, expected_totals)
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

    def _first_row(self, slab: _Slab, first: int) -> int:
        """The first row of the slab whose ``ForestIndex`` is at least ``first``, by binary
        search over single-row reads (``n_rows`` when there is none)."""
        low, high = 0, slab.n_rows
        while low < high:
            mid = (low + high) // 2
            value = int(self.dataset.read_rows(slab.snap, (_FOREST,), mid, mid + 1)[_FOREST][0])
            if value < first:
                low = mid + 1
            else:
                high = mid
        return low

    def _read_window(self, slab: _Slab, first: int) -> None:
        """Append the slab's next window to its buffer, checking ``ForestIndex`` continues
        non-decreasing from the last buffered row (or from the range's first forest)."""
        stop = min(slab.next_row + self.window_rows, slab.n_rows)
        window = self.dataset.read_rows(slab.snap, self.columns, slab.next_row, stop)
        forest = window[_FOREST]
        path = self.dataset.snapshot_path(slab.snap)
        if slab.length:
            previous = int(slab.buffer[_FOREST][-1])
        elif int(forest[0]) < first:
            raise ConverterError(
                "{}: row {} holds ForestIndex {}, below the range's first forest {} although "
                "the search for that forest's first row passed it (ForestIndex is not "
                "non-decreasing)".format(path, slab.next_row, forest[0], first)
            )
        else:
            previous = int(forest[0])
        before = np.concatenate(([previous], forest[:-1]))
        drops = np.flatnonzero(forest < before)
        if drops.size:
            row = int(drops[0])
            raise ConverterError(
                "{}: ForestIndex is not non-decreasing: row {} holds {} after {}".format(
                    path, slab.next_row + row, forest[row], before[row]
                )
            )
        if slab.length:
            for name in self.columns:
                slab.buffer[name] = np.concatenate((slab.buffer[name], window[name]))
        else:
            slab.buffer = window
        slab.length += forest.size
        slab.next_row = stop

    def _check_totals(
        self, slabs: List[_Slab], first: int, stop: int, expected_totals
    ) -> np.ndarray:
        """Every forest of ``[first, stop)`` has rows, and its expected total if one is given;
        returns the cumulative row count over the range's forests."""
        totals = np.zeros(stop - first, dtype=np.int64)
        edges = np.arange(first, stop + 1, dtype=np.int64)
        for slab in slabs:
            if slab.length:
                totals += np.diff(np.searchsorted(slab.buffer[_FOREST], edges, "left"))
        empty = np.flatnonzero(totals == 0)
        if empty.size:
            raise ConverterError(
                "{}: forest {} has no rows in any slab".format(
                    self.dataset.directory, first + int(empty[0])
                )
            )
        if expected_totals is not None:
            planned = np.asarray(expected_totals[first:stop], dtype=np.int64)
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
        """Assemble the forests ``[first, stop)`` from the buffers, trimming each slab's
        buffer to the rows that follow, and yield them one by one."""
        slices = []
        for slab in reversed(slabs):
            taken = int(np.searchsorted(slab.buffer[_FOREST], stop, "left")) if slab.length else 0
            slices.append((slab, taken))
        columns = {}
        for name in self.columns:
            columns[name] = np.concatenate([slab.buffer[name][:n] for slab, n in slices if n])
            for slab, n in slices:
                if n:
                    slab.buffer[name] = slab.buffer[name][n:].copy()
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
        order = np.argsort(forest, kind="stable")
        expected = np.arange(n_rows, dtype=np.int64) - offsets[forest[order]]
        wrong = np.flatnonzero(rank[order] != expected)
        if wrong.size:
            at = int(wrong[0])
            owner = int(forest[order][at])
            raise ConverterError(
                "{}: forest {}: rank {} found at position {}, where rank {} is expected (the "
                "blocks from the highest snapshot down must give ranks 0..{})".format(
                    where,
                    first + owner,
                    rank[order][at],
                    expected[at],
                    expected[at],
                    counts[owner] - 1,
                )
            )
        position = offsets[forest] + rank
        records = np.zeros(n_rows, dtype=self.dtype)
        for name in self.payload_fields:
            records[name][position] = columns[name]
        placed_most_bound = np.empty(n_rows, dtype=np.int64)
        placed_most_bound[position] = columns[_MOST_BOUND]
        for name in LINK_FIELDS:
            companion = LINK_SNAPSHOT_COLUMNS.get(name)
            target_snap = columns[companion] if companion else snapshot
            source, target, mapped = _map_links(
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
            _verify_links(
                where,
                first,
                name,
                forest,
                rank,
                counts,
                offsets,
                placed_most_bound,
                columns[_MOST_BOUND],
                source,
                target,
                mapped,
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

    Returns ``(source, target, mapped)``: the batch indices of the rows whose link is not
    null, the batch index each one targets, and every row's mapped rank (``-1`` where null).

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
        return source, source, mapped
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
    target = batch_start[snaps] + np.where(inside, local, 0)
    good = inside & (forest[target] == owners)
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
    return source, target, mapped


def _verify_links(
    where,
    first,
    name,
    forest,
    rank,
    counts,
    offsets,
    placed_most_bound,
    most_bound,
    source,
    target,
    mapped,
):
    """Every mapped link names a record of its forest whose ``MostBoundID`` is the target
    row's; refused otherwise, naming the forest, the halo's rank, the link and both ids."""
    if not source.size:
        return
    ranks = mapped[source]
    owners = forest[source]
    astray = np.flatnonzero((ranks < 0) | (ranks >= counts[owners]))
    if astray.size:
        at = int(source[astray[0]])
        raise ConverterError(
            "{}: forest {}, rank {}: {} maps to rank {}, outside the forest's {} halos".format(
                where, first + int(forest[at]), rank[at], name, mapped[at], counts[forest[at]]
            )
        )
    named = placed_most_bound[offsets[owners] + ranks]
    actual = most_bound[target]
    differs = np.flatnonzero(named != actual)
    if differs.size:
        at = int(source[differs[0]])
        raise ConverterError(
            "{}: forest {}, rank {}: {} maps to rank {}, whose MostBoundID {} is not the target "
            "row's {}".format(
                where,
                first + int(forest[at]),
                rank[at],
                name,
                mapped[at],
                named[differs[0]],
                actual[differs[0]],
            )
        )
