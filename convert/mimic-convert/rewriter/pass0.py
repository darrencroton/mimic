"""Pass 0: the per-forest totals, the trees' roots and the per-tree inventory.

Everything the later passes need that is a property of the whole input, read
once in bounded blocks before any output is written:

- **per-forest totals** from a scan of every slab's ``ForestIndex`` (8 B per
  halo read), which give each forest's ``SourceHaloID`` base -- the prefix
  sum from 1 over ascending ``ForestIndex`` -- and ``max_halo_rank_in_forest``
  (the largest total less one, since ranks are dense per forest). Every
  forest must hold at least one halo and the prefix sum must fit int64;
- **the trees' roots**: every halo whose ``Descendant`` is -1, by its
  ``MostBoundID`` (``census.trees.enumerate_roots``: the ``Descendant`` column
  of every slab, plus ``MostBoundID`` and ``ForestIndex`` over the rows
  spanning its terminals). Their correspondence with the index files' tree
  roots (``census.trees.root_correspondence``: equal root sets, and each root
  in the forest ``forests.list`` gives it) is a prerequisite: a mismatch is
  refused, never repaired;
- **the per-tree inventory**: per index tree, in ascending root id, its
  forest, its ``locations.dat`` file, that file's ordinal in the conversion's
  tree-file order, the rank of its ``#tree`` marker within the file, and its
  row count, which pass 1 fills in as it labels halos with their trees, and
  whose per-file sums the summary records (:func:`conservation_record`).

Memory: the ``pass0`` entry of ``rewriter.inputs.MEMORY_FORMULAS``.
"""

import os
import sys
from dataclasses import dataclass
from typing import Callable, Dict, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.common import examples  # noqa: E402
from census.trees import enumerate_roots, root_correspondence  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from source_index import ForestTable, SourceIndex  # noqa: E402

#: One row per index tree, in ascending root id (``tree_inventory.npy``).
INVENTORY_DTYPE = np.dtype(
    [
        ("root", "<i8"),
        ("forest_id", "<i8"),
        ("forest_index", "<i8"),
        ("file_id", "<i8"),
        ("source_file_ordinal", "<i8"),
        ("marker_ordinal", "<i8"),
        ("n_rows", "<i8"),
    ]
)

_INT64_MAX = int(np.iinfo(np.int64).max)


@dataclass
class Census:
    """Pass 0's whole-input facts."""

    totals: np.ndarray  # int64 [n_forests]: halos per forest over every slab
    sid_base: np.ndarray  # int64 [n_forests]: SourceHaloID of each forest's rank 0
    max_rank: int
    roots: np.ndarray  # int64, ascending: the trees' terminal MostBoundID values
    correspondence: Dict[str, object]


def scan_forest_totals(
    dataset: HorizontalDataset, n_forests: int, block_rows: int, log: Callable[[str], None]
) -> np.ndarray:
    """Halos per forest over every slab, from bounded ``ForestIndex`` blocks;
    a value outside ``[0, n_forests)`` is refused."""
    totals = np.zeros(n_forests, dtype=np.int64)
    for snap in dataset.snapshots:
        for start, block in dataset.iter_column(snap, "ForestIndex", block_rows):
            block = np.asarray(block, dtype=np.int64)
            bad = (block < 0) | (block >= n_forests)
            if bad.any():
                rows = start + np.flatnonzero(bad)
                raise ConverterError(
                    "{}: {} ForestIndex value(s) outside [0, {}); rows {}".format(
                        dataset.snapshot_path(snap), int(bad.sum()), n_forests, examples(rows)
                    )
                )
            present, counts = np.unique(block, return_counts=True)
            totals[present] += counts
        log("pass 0: snapshot {} scanned ({} halos)".format(snap, dataset.n_halos[snap]))
    return totals


def source_bases(totals: np.ndarray) -> Tuple[np.ndarray, int]:
    """Each forest's ``SourceHaloID`` base (1 + the halos of every lower
    forest) and the dataset's maximum rank; refuses an empty forest and an
    int64 overflow of the prefix sum."""
    empty = np.flatnonzero(totals == 0)
    if empty.size:
        raise ConverterError(
            "{} forest(s) hold no halo (a Consistent-Trees forest always does); ForestIndex "
            "examples: {}".format(empty.size, examples(empty.tolist()))
        )
    prefix = np.cumsum(totals, dtype=np.int64)
    # a wrapped partial sum is smaller than its predecessor (every total is
    # positive and below 2^63), so one monotonic check catches the overflow
    if prefix.size and (int(prefix[0]) < 0 or np.any(prefix[1:] < prefix[:-1])):
        raise ConverterError("SourceHaloID assignment overflows int64 over the forest totals")
    if prefix.size and int(prefix[-1]) >= _INT64_MAX:
        raise ConverterError("SourceHaloID assignment overflows int64 at the last forest")
    sid_base = np.empty_like(prefix)
    sid_base[0] = 1
    sid_base[1:] = prefix[:-1] + 1
    max_rank = int(totals.max()) - 1 if totals.size else -1
    return sid_base, max_rank


def check_roots(
    dataset: HorizontalDataset,
    index: SourceIndex,
    forest_ids: np.ndarray,
    block_rows: int,
) -> Tuple[np.ndarray, np.ndarray, Dict[str, object]]:
    """The terminal halos as roots, and their correspondence with the index
    files' tree roots (refused unless it passes). Returns the ascending
    roots, their slabs and the correspondence record."""
    roots, forests, snaps = enumerate_roots(dataset, block_rows)
    record = root_correspondence(roots, snaps, forests, forest_ids, dataset.snapshots[-1], index)
    if record["verdict"] != "pass":
        raise ConverterError(
            "the input's terminal halos do not correspond to the index files' tree roots: {} "
            "terminal root(s) not in forests.list (e.g. {}), {} forests.list root(s) without a "
            "terminal halo (e.g. {}), {} root(s) whose forest differs (e.g. {}); refusing to "
            "rewrite (the correspondence is not repaired)".format(
                record["roots_not_in_forests_list"]["count"],
                record["roots_not_in_forests_list"]["examples"],
                record["forests_list_roots_without_terminal_halo"]["count"],
                record["forests_list_roots_without_terminal_halo"]["examples"],
                record["forest_mismatches"]["count"],
                record["forest_mismatches"]["examples"],
            )
        )
    return roots, snaps, record


def run_pass0(
    dataset: HorizontalDataset,
    index: SourceIndex,
    forest_ids: np.ndarray,
    block_rows: int,
    log: Callable[[str], None],
) -> Census:
    totals = scan_forest_totals(dataset, dataset.n_forests_total, block_rows, log)
    if int(totals.sum()) != dataset.total_halos:
        raise ConverterError(
            "the forest totals sum to {} halos but the headers count {}".format(
                int(totals.sum()), dataset.total_halos
            )
        )
    sid_base, max_rank = source_bases(totals)
    if max_rank != dataset.max_halo_rank_in_forest:
        raise ConverterError(
            "the largest forest holds {} halos, so max_halo_rank_in_forest is {}, but the "
            "input's header records {}".format(
                max_rank + 1, max_rank, dataset.max_halo_rank_in_forest
            )
        )
    roots, _root_snapshot, correspondence = check_roots(dataset, index, forest_ids, block_rows)
    log(
        "pass 0: {} forests, {} halos, {} trees; root correspondence {}".format(
            totals.size, dataset.total_halos, roots.size, correspondence["verdict"]
        )
    )
    return Census(
        totals=totals,
        sid_base=sid_base,
        max_rank=max_rank,
        roots=roots,
        correspondence=correspondence,
    )


def tree_inventory(
    index: SourceIndex, table: ForestTable, file_ordinals: Dict[int, int]
) -> np.ndarray:
    """The per-tree inventory with ``n_rows`` -1, to be filled by pass 1."""
    inventory = np.empty(index.n_trees, dtype=INVENTORY_DTYPE)
    inventory["root"] = index.tree_roots
    inventory["forest_id"] = index.forest_ids
    inventory["forest_index"] = np.searchsorted(table.forest_ids, index.forest_ids)
    inventory["file_id"] = index.file_ids
    keys = np.asarray(sorted(file_ordinals), dtype=np.int64)
    values = np.asarray([file_ordinals[key] for key in keys.tolist()], dtype=np.int64)
    inventory["source_file_ordinal"] = values[np.searchsorted(keys, index.file_ids)]
    inventory["marker_ordinal"] = index.marker_ordinals()
    inventory["n_rows"] = -1
    return inventory


def conservation_record(inventory: np.ndarray, filenames: Dict[int, str]) -> Dict[str, object]:
    """Per-file sums of the trees' rows, through the inventory's ``file_id``,
    with ``filenames`` the ``locations.dat`` ``FileID`` -> name map. The sums
    are recorded, not checked: a horizontal dataset carries no per-file
    parsed counts to check them against."""
    file_ids = sorted(filenames)
    present, inverse = np.unique(inventory["file_id"], return_inverse=True)
    sums = np.zeros(present.size, dtype=np.int64)
    np.add.at(sums, inverse, inventory["n_rows"])
    by_file = {int(file_id): int(total) for file_id, total in zip(present, sums)}
    files = [
        {
            "file_id": file_id,
            "name": os.path.basename(filenames[file_id]),
            "census_halos": by_file.get(file_id, 0),
        }
        for file_id in file_ids
    ]
    return {"files": files}
