"""Per-forest occupancy of every slab, and over all slabs.

One forward pass over every slab's ``ForestIndex`` column, in bounded blocks
(:meth:`horizontal_dataset.HorizontalDataset.iter_column`). Version 2 slabs are
not grouped by forest, so each block's ``(ForestIndex, count)`` pairs are added
into a per-forest scratch counter that is read back, and zeroed again, at the
end of the slab.

Aggregates written under ``<aggregate>/occupancy/`` (sizes also recorded in
``summary.json``, so Slice 9 can measure them and the procedure budget them):

- ``slab_NNN_forests.npy`` (int64, ascending) and ``slab_NNN_counts.npy``
  (int32): the forests present in slab NNN and their row counts. **12 B per
  present forest per slab** plus a 128 B header per file: ``12 x sum over
  slabs of the forests present``, about 25 GB at Shin-Uchuu scale (about
  2.1 x 10^9 (slab, forest) pairs over 70 slabs).
- ``forest_totals.npy`` (int64), ``forest_max_occupancy.npy`` (int32) and
  ``forest_max_snapshot.npy`` (int32, -1 for a forest with no halo): per forest,
  its halos over all slabs, its largest slab occupancy and the lowest-numbered
  slab where it occurs. **16 B x n_forests_total**, about 2.7 GB for Shin-Uchuu's
  166,547,771 forests (4.3 GB at F2's worst case of 271,392,048).
- ``summary.json``: the brief's measured table rows (total halos; the widest
  slab, the lowest-numbered on a tie as ``src/core/horizontal_driver.c``
  chooses it; the largest forest's total, its peak occupancy and slab; the
  second-largest forest's maximum; the forests whose occupancy exceeds 100,000,
  250,000 and 1,000,000 in any slab; the forests present in the widest slab and
  its top-10 and top-100 shares), a per-snapshot table, and the aggregate sizes.

Resident memory: 24 B x n_forests_total (the three per-forest arrays and the
int64 scratch counter; about 4.0 GB for Shin-Uchuu's 166,547,771 forests, and
8 B more per forest for the sidecar ``ForestID`` when the summary is built), one
``ForestIndex`` block (8 B x block rows) with its ``np.unique`` temporaries, and
one slab's pairs (12 B per present forest). No array of a slab's length and no
forest x snapshot matrix is held.
"""

import os
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.aggregate import (  # noqa: E402
    SUMMARY_NAME,
    begin,
    bind,
    load_array,
    save_array,
    slab_stem,
    write_json,
)
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402

OCCUPANCY_DIR = "occupancy"

#: Occupancy thresholds whose exceedance in any slab is counted (strictly above).
OCCUPANCY_THRESHOLDS = (100_000, 250_000, 1_000_000)

#: The widest slab's top-k forest shares reported.
TOP_SHARES = (10, 100)

#: Per-forest arrays: name -> dtype.
PER_FOREST_ARRAYS = {
    "forest_totals": np.int64,
    "forest_max_occupancy": np.int32,
    "forest_max_snapshot": np.int32,
}

_INT32_MAX = int(np.iinfo(np.int32).max)


def _quiet(_message: str) -> None:
    pass


def occupancy_dir(aggregate_dir) -> Path:
    return Path(aggregate_dir) / OCCUPANCY_DIR


def slab_pair_paths(aggregate_dir, snap: int) -> Tuple[Path, Path]:
    """The ``(forests, counts)`` files of one slab."""
    base = occupancy_dir(aggregate_dir)
    stem = slab_stem(snap)
    return base / (stem + "_forests.npy"), base / (stem + "_counts.npy")


def load_slab_pairs(aggregate_dir, snap: int, mmap: bool = False) -> Tuple[np.ndarray, np.ndarray]:
    """One slab's present forests (int64, ascending) and their counts (int32)."""
    forests_path, counts_path = slab_pair_paths(aggregate_dir, snap)
    return load_array(forests_path, mmap), load_array(counts_path, mmap)


def widest_snapshot(n_halos: Sequence[int]) -> Optional[int]:
    """The snapshot with the most halos, the lowest-numbered on a tie, as
    ``horizontal_widest_snapshot`` chooses it; ``None`` without snapshots."""
    widest, widest_halos = None, -1
    for snap, halos in enumerate(n_halos):
        if int(halos) > widest_halos:
            widest, widest_halos = snap, int(halos)
    return widest


def slab_occupancy(
    dataset: HorizontalDataset,
    snap: int,
    scratch: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
) -> Tuple[np.ndarray, np.ndarray]:
    """The forests present in one slab and their row counts.

    Args:
        scratch: int64 zeros of length ``n_forests_total``; zero again on return.

    Returns:
        ``(forests, counts)``: int64 ascending and int32, aligned.

    Raises:
        ConverterError: on a ``ForestIndex`` outside ``[0, n_forests_total)``.
    """
    n_forests = scratch.size
    for start, block in dataset.iter_column(snap, "ForestIndex", block_rows):
        if block.size == 0:
            continue
        low, high = int(block.min()), int(block.max())
        if low < 0 or high >= n_forests:
            raise ConverterError(
                "{}: ForestIndex outside [0, {}) in rows [{}, {}) (min {}, max {})".format(
                    dataset.snapshot_path(snap), n_forests, start, start + block.size, low, high
                )
            )
        forests, counts = np.unique(block, return_counts=True)
        scratch[forests] += counts
    present = np.flatnonzero(scratch)
    counts = scratch[present]
    scratch[present] = 0
    if counts.size and int(counts.max()) > _INT32_MAX:
        raise ConverterError("{}: a forest holds more than 2^31 - 1 rows".format(snap))
    return present.astype(np.int64), counts.astype(np.int32)


def top_shares(counts: np.ndarray, n_halos: int, tops: Sequence[int] = TOP_SHARES) -> Dict:
    """The halos and share of a slab held by its ``k`` most populous forests."""
    ordered = np.sort(np.asarray(counts, dtype=np.int64))[::-1]
    shares = {}
    for k in tops:
        halos = int(ordered[:k].sum())
        shares[str(k)] = {"halos": halos, "share": (halos / n_halos) if n_halos else None}
    return shares


def _forest_record(index: int, totals, max_occ, max_snap, forest_ids, n_halos) -> Dict:
    snap = int(max_snap[index])
    peak = int(max_occ[index])
    return {
        "forest_index": int(index),
        "forest_id": int(forest_ids[index]),
        "total_halos": int(totals[index]),
        "peak_occupancy": peak,
        "peak_snapshot": snap,
        "peak_share_of_slab": (peak / n_halos[snap]) if snap >= 0 and n_halos[snap] else None,
    }


def _second_largest(values: np.ndarray, first: int) -> Optional[int]:
    """The index of the largest value other than ``first`` (lowest index on a tie)."""
    if values.size < 2:
        return None
    kept = values[first]
    values[first] = -1
    try:
        return int(np.argmax(values))
    finally:
        values[first] = kept


def run_occupancy(
    dataset: HorizontalDataset,
    aggregate_dir,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
) -> Dict:
    """Run the occupancy pass and write its aggregates; returns the summary."""
    identity = dataset.identity()
    aggregate_dir = bind(aggregate_dir, identity)
    out = occupancy_dir(aggregate_dir)
    begin(out)
    n_forests = dataset.n_forests_total
    n_halos = dataset.n_halos
    totals = np.zeros(n_forests, dtype=np.int64)
    max_occ = np.zeros(n_forests, dtype=np.int32)
    max_snap = np.full(n_forests, -1, dtype=np.int32)
    scratch = np.zeros(n_forests, dtype=np.int64)
    widest = widest_snapshot(n_halos)

    per_snapshot: List[Dict] = []
    widest_record: Optional[Dict] = None
    pairs_total = pair_bytes = 0
    for snap in dataset.snapshots:
        forests, counts = slab_occupancy(dataset, snap, scratch, block_rows)
        if int(counts.sum(dtype=np.int64)) != n_halos[snap]:
            raise ConverterError(
                "{}: ForestIndex counts sum to {} but n_halos is {}".format(
                    dataset.snapshot_path(snap), int(counts.sum(dtype=np.int64)), n_halos[snap]
                )
            )
        forests_path, counts_path = slab_pair_paths(aggregate_dir, snap)
        pair_bytes += save_array(forests_path, forests) + save_array(counts_path, counts)
        pairs_total += int(forests.size)
        totals[forests] += counts
        # ascending slabs and a strict test: a tie keeps the lowest-numbered slab
        better = counts > max_occ[forests]
        max_occ[forests[better]] = counts[better]
        max_snap[forests[better]] = snap
        per_snapshot.append(
            {
                "snapshot": snap,
                "scale_factor": dataset.scale_factors[snap],
                "halos": n_halos[snap],
                "forests_present": int(forests.size),
                "heaviest_forest_rows": int(counts.max()) if counts.size else 0,
            }
        )
        if snap == widest:
            widest_record = {
                "snapshot": snap,
                "halos": n_halos[snap],
                "forests_present": int(forests.size),
                "top_shares": top_shares(counts, n_halos[snap]),
            }
        log(
            "occupancy: snapshot {} -- {} halos, {} forests".format(
                snap, n_halos[snap], forests.size
            )
        )
    del scratch

    forest_bytes = 0
    for name, array in (
        ("forest_totals", totals),
        ("forest_max_occupancy", max_occ),
        ("forest_max_snapshot", max_snap),
    ):
        forest_bytes += save_array(out / (name + ".npy"), array)

    forest_ids = dataset.forest_ids()
    largest = second = second_peak = None
    if n_forests:
        largest_index = int(np.argmax(totals))
        largest = _forest_record(largest_index, totals, max_occ, max_snap, forest_ids, n_halos)
        largest["share_of_all_halos"] = (
            largest["total_halos"] / dataset.total_halos if dataset.total_halos else None
        )
        second_index = _second_largest(totals, largest_index)
        if second_index is not None:
            second = _forest_record(second_index, totals, max_occ, max_snap, forest_ids, n_halos)
        peak_index = int(np.argmax(max_occ))
        runner_index = _second_largest(max_occ, peak_index)
        if runner_index is not None:
            second_peak = _forest_record(
                runner_index, totals, max_occ, max_snap, forest_ids, n_halos
            )

    ranked = sorted(dataset.snapshots, key=lambda snap: (-n_halos[snap], snap))
    summary = {
        "dataset": identity,
        "dataset_dir": str(dataset.directory.resolve()),
        "total_halos": dataset.total_halos,
        "n_forests_total": n_forests,
        "forests_with_halos": int(np.count_nonzero(totals)),
        "max_halo_rank_in_forest": dataset.max_halo_rank_in_forest,
        "widest_slab": widest_record,
        "second_widest_slab": (
            {"snapshot": ranked[1], "halos": n_halos[ranked[1]]} if len(ranked) > 1 else None
        ),
        "largest_forest": largest,
        "second_largest_forest": second,
        "second_highest_peak_forest": second_peak,
        "forests_exceeding": {
            str(threshold): int(np.count_nonzero(max_occ > threshold))
            for threshold in OCCUPANCY_THRESHOLDS
        },
        "per_snapshot": per_snapshot,
        "aggregates": {
            "slab_pairs": {
                "formula": "12 B per (slab, present forest) pair (int64 ForestIndex + int32 "
                "count), plus a 128 B .npy header per file; two files per slab",
                "pairs": pairs_total,
                "files": 2 * len(n_halos),
                "bytes": pair_bytes,
            },
            "per_forest": {
                "formula": "16 B x n_forests_total (int64 total, int32 maximum occupancy, "
                "int32 snapshot of the maximum), plus a 128 B .npy header per file",
                "n_forests_total": n_forests,
                "bytes": forest_bytes,
            },
        },
    }
    write_json(out / SUMMARY_NAME, summary)
    return summary
