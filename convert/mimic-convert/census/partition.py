"""An exact simulation of the horizontal driver's two-level forest partition.

:func:`cut_forests` reproduces ``horizontal_partition_cut_forests`` and
:func:`partition_cut` reproduces ``horizontal_partition_cut``
(``src/core/horizontal_partition.c``): ``ntask`` contiguous forest ranges by
minimax packing of the per-forest weights -- a binary search for the smallest
capacity at which the left-to-right greedy packing needs at most ``ntask``
ranges, then that packing -- and, within each task's range, ``nchunk`` chunk
ranges by the same rule over the task's own weights. Range ``t * nchunk + c`` is
task ``t``'s chunk ``c``; the returned ``ntask * nchunk + 1`` cuts give range
``q`` the forests ``[cuts[q], cuts[q + 1])``. A zero-weight forest always joins
the open range; ranges the packing does not need are idle and cut at the end of
the last used range (the forest count), exactly as the C code leaves them.

The greedy packing is evaluated over the weights' prefix sums: a range opened
at forest ``s`` at capacity ``C`` ends at the first forest whose inclusion
would push its sum past ``C``, which is ``searchsorted(P, P[s] + C, "right") -
1``. This is the C loop's fit test (``weights[f] > capacity - filled`` closes
the range) step for step, with zero-weight forests riding in the open range,
but costs O(ranges x log n) per capacity instead of O(n), so Shin-Uchuu's
166,547,771-forest weight vector (1.3 GB) is cut in seconds.

The weights are the widest slab's per-forest row counts, as the driver uses
(``src/core/horizontal_driver.c``, the widest slab the lowest-numbered on a tie);
:func:`range_rows` applies a partition's cuts to any slab's sparse per-forest
counts, giving each range's row count in that slab.

The ``partition`` subcommand writes one aggregate, ``partition/summary.json``.
Its size is dominated by about ``(S + 1) x sum(ntask x nchunk)`` integers over
the grid for ``S`` slabs (each point's cuts and every slab's range rows), plus
per-point and per-slab metadata of a few integers each: for Shin-Uchuu's 70
slabs and the default grid ({1, 2, 4, 8} x {1, 2, 4, 8, 16, 32}, 945 ranges in
all) about 67,000 integers, a few hundred kilobytes of JSON. The summary records
that count and its own measured size under ``aggregates``.
"""

import os
import sys
from pathlib import Path
from typing import Callable, List, Optional, Sequence

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.aggregate import (  # noqa: E402
    SUMMARY_NAME,
    bound_identity,
    require_summary,
    write_json,
)
from census.occupancy import load_slab_pairs, occupancy_dir  # noqa: E402
from errors import ConverterError  # noqa: E402

_INT64_MAX = int(np.iinfo(np.int64).max)


def _prefix_sums(weights: np.ndarray) -> np.ndarray:
    """``P`` with ``P[0] = 0`` and ``P[f + 1] = weights[0] + ... + weights[f]``,
    refusing a negative weight or a sum beyond int64 as the C code does."""
    if weights.size and int(weights.min()) < 0:
        forest = int(np.argmax(weights < 0))
        raise ConverterError(
            "partition weight of forest {} is negative ({})".format(forest, int(weights[forest]))
        )
    prefix = np.zeros(weights.size + 1, dtype=np.int64)
    np.cumsum(weights, out=prefix[1:])
    # non-negative int64 terms: an overflow wraps the running sum below its predecessor
    if prefix.size > 1:
        wrapped = np.nonzero(prefix[1:] < prefix[:-1])[0]
        if wrapped.size:
            raise ConverterError(
                "partition weights sum beyond int64 at forest {}".format(int(wrapped[0]))
            )
    return prefix


def _greedy_starts(prefix: np.ndarray, capacity: int, limit: int) -> List[int]:
    """The first forest of every range the greedy packing opens at
    ``capacity``, stopping once more than ``limit`` ranges are open."""
    n_forests = prefix.size - 1
    starts = [0]
    start = 0
    while True:
        reach = (
            int(prefix[start]) + capacity if capacity <= _INT64_MAX - int(prefix[start]) else None
        )
        end = n_forests if reach is None else int(np.searchsorted(prefix, reach, "right")) - 1
        if end >= n_forests or len(starts) > limit:
            return starts
        start = end
        starts.append(start)


def cut_forests(weights: Sequence[int], ntask: int) -> List[int]:
    """``horizontal_partition_cut_forests``: ``ntask + 1`` forest cuts.

    Raises:
        ConverterError: for ``ntask < 1``, a negative weight, or a weight sum
            beyond int64.
    """
    if int(ntask) < 1:
        raise ConverterError("partition needs at least one rank (got ntask = {})".format(ntask))
    ntask = int(ntask)
    weights = np.asarray(weights, dtype=np.int64).reshape(-1)
    n_forests = int(weights.size)
    prefix = _prefix_sums(weights)
    low = int(weights.max()) if n_forests else 0
    high = int(prefix[-1])
    # smallest capacity at which the greedy packing needs at most ntask ranges
    while low < high:
        middle = low + (high - low) // 2
        if len(_greedy_starts(prefix, middle, ntask)) <= ntask:
            high = middle
        else:
            low = middle + 1
    starts = _greedy_starts(prefix, low, ntask)
    return starts + [n_forests] * (ntask + 1 - len(starts))


def partition_cut(weights: Sequence[int], ntask: int, nchunk: int) -> List[int]:
    """``horizontal_partition_cut``: the ``ntask * nchunk + 1`` forest cuts of
    the task level and, within each task's range, the chunk level.

    Raises:
        ConverterError: for ``nchunk < 1`` or any :func:`cut_forests` refusal.
    """
    if int(nchunk) < 1:
        raise ConverterError(
            "partition needs at least one chunk per rank (got nchunk = {})".format(nchunk)
        )
    ntask, nchunk = int(ntask), int(nchunk)
    weights = np.asarray(weights, dtype=np.int64).reshape(-1)
    task_cuts = cut_forests(weights, ntask)
    if nchunk == 1:
        return task_cuts
    cuts = [0] * (ntask * nchunk + 1)
    cuts[ntask * nchunk] = task_cuts[ntask]
    for task in range(ntask):
        low, high = task_cuts[task], task_cuts[task + 1]
        chunk_cuts = cut_forests(weights[low:high], nchunk)
        for chunk in range(nchunk):
            cuts[task * nchunk + chunk] = low + chunk_cuts[chunk]
    return cuts


def slab_prefix(counts: np.ndarray) -> np.ndarray:
    """A slab's row-count prefix sums (int64, a leading 0), for :func:`range_rows`."""
    prefix = np.zeros(np.asarray(counts).size + 1, dtype=np.int64)
    np.cumsum(counts, out=prefix[1:])
    return prefix


def range_rows(
    forests: np.ndarray,
    counts: np.ndarray,
    cuts: Sequence[int],
    prefix: Optional[np.ndarray] = None,
) -> np.ndarray:
    """Rows of one slab in each range of ``cuts``.

    Args:
        forests: the slab's present ``ForestIndex`` values, ascending.
        counts: their row counts, aligned.
        cuts: a partition's forest cuts.
        prefix: :func:`slab_prefix` of ``counts``, when the caller applies
            several partitions to one slab and computes it once.

    Returns:
        int64 array of length ``len(cuts) - 1``.
    """
    if prefix is None:
        prefix = slab_prefix(counts)
    below = prefix[np.searchsorted(forests, np.asarray(cuts, dtype=np.int64), "left")]
    return np.diff(below)


# ---- the subcommand ----------------------------------------------------------

PARTITION_DIR = "partition"

#: The (ntask, nchunk) grid applied when none is given.
DEFAULT_NTASKS = (1, 2, 4, 8)
DEFAULT_NCHUNKS = (1, 2, 4, 8, 16, 32)


def widest_slab_weights(aggregate_dir, n_forests: int, widest: int) -> np.ndarray:
    """The widest slab's per-forest row counts as a dense int64 weight vector
    (8 B x ``n_forests_total``), from the occupancy aggregates."""
    weights = np.zeros(n_forests, dtype=np.int64)
    forests, counts = load_slab_pairs(aggregate_dir, widest)
    weights[forests] = counts
    return weights


def run_partition(
    aggregate_dir,
    ntasks: Sequence[int] = DEFAULT_NTASKS,
    nchunks: Sequence[int] = DEFAULT_NCHUNKS,
    log: Optional[Callable[[str], None]] = None,
) -> dict:
    """Apply :func:`partition_cut` with the widest slab's weights for every
    (ntask, nchunk) of the grid, and report every range's rows in every slab.

    Reads only the occupancy aggregates of ``aggregate_dir`` (one slab's pairs
    at a time) and writes ``partition/summary.json``: per grid point the forest
    cuts, per slab every range's rows and the widest range, and the widest range
    over all slabs; per slab the floor, the rows of its heaviest forest, which no
    range of any partition can go below.

    Raises:
        ConverterError: when the occupancy pass has not completed, or for an
            invalid grid point.
    """
    identity = bound_identity(aggregate_dir)
    occupancy = require_summary(
        occupancy_dir(aggregate_dir), "occupancy", ("dataset", "widest_slab")
    )
    if occupancy["dataset"] != identity:
        raise ConverterError(
            "{}: the occupancy summary was not produced from this directory's dataset".format(
                aggregate_dir
            )
        )
    n_forests = int(identity["n_forests_total"])
    n_halos = [int(n) for n in identity["n_halos"]]
    widest = occupancy["widest_slab"]
    grid = [(int(t), int(c)) for t in ntasks for c in nchunks]
    if widest is None or not grid:
        raise ConverterError("{}: no slab or no grid point to partition".format(aggregate_dir))
    weights = widest_slab_weights(aggregate_dir, n_forests, widest["snapshot"])
    heaviest = int(np.argmax(weights)) if n_forests else -1
    points = []
    for ntask, nchunk in grid:
        cuts = partition_cut(weights, ntask, nchunk)
        points.append(
            {
                "ntask": ntask,
                "nchunk": nchunk,
                "forest_cuts": cuts,
                "per_snapshot": [],
                "widest": {"snapshot": None, "range": None, "rows": -1},
            }
        )
    del weights

    floors = []
    for snap in range(len(n_halos)):
        forests, counts = load_slab_pairs(aggregate_dir, snap)
        floors.append(int(counts.max()) if counts.size else 0)
        prefix = slab_prefix(counts)
        for point in points:
            rows = range_rows(forests, counts, point["forest_cuts"], prefix)
            if int(rows.sum()) != n_halos[snap]:  # pragma: no cover - cuts span [0, n)
                raise ConverterError("partition ranges do not cover snapshot {}".format(snap))
            at = int(np.argmax(rows)) if rows.size else 0
            point["per_snapshot"].append(
                {
                    "snapshot": snap,
                    "range_rows": rows.tolist(),
                    "widest_range": at,
                    "widest_rows": int(rows[at]),
                }
            )
            # ascending slabs and a strict test: a tie keeps the lowest-numbered slab
            if int(rows[at]) > point["widest"]["rows"]:
                point["widest"] = {
                    "snapshot": snap,
                    "range": at,
                    "task": at // point["nchunk"],
                    "chunk": at % point["nchunk"],
                    "rows": int(rows[at]),
                }
        if log is not None:
            log("partition: snapshot {} applied to {} grid point(s)".format(snap, len(points)))

    summary = {
        "dataset": identity,
        "weights": {
            "snapshot": widest["snapshot"],
            "halos": widest["halos"],
            "heaviest_forest": {
                "forest_index": heaviest,
                "rows": floors[widest["snapshot"]],
            },
        },
        "floor": {
            "per_snapshot": floors,
            "max_rows": max(floors),
            "snapshot": int(np.argmax(floors)),
            "note": "per slab, the rows of its heaviest forest, which no partition can "
            "split: every partition's widest range in that slab holds at least this many",
        },
        "grid": points,
        "aggregates": {
            "summary": {
                "formula": "about (S + 1) x sum over the grid of ntask x nchunk integers for S "
                "slabs (cuts and per-slab range rows), plus a few integers of metadata per "
                "grid point and slab, as indented JSON",
                "range_integers": (len(n_halos) + 1) * sum(t * c for t, c in grid),
                "bytes": 0,
            }
        },
    }
    # the summary records its own size: rewrite until the recorded figure is the file's
    path = Path(aggregate_dir) / PARTITION_DIR / SUMMARY_NAME
    record = summary["aggregates"]["summary"]
    for _attempt in range(8):
        write_json(path, summary)
        size = path.stat().st_size
        if size == record["bytes"]:
            break
        record["bytes"] = size
    return summary
