"""The cut: a declared cut forest table installed over a version 3 input.

After pass 0 has given the trees' roots (and proved their correspondence with
the index files), the cut needs each tree's halo total, which only a
labelling of every halo with its tree gives: :func:`label_tree_totals` is one
descending pass over ``Descendant`` and ``MostBoundID`` (the same labelling
pass 1 repeats slab by slab), reading 16 B per halo and holding two slabs'
int32 labels. :func:`install_table` then reads the table through the census's
reader (``census.cut_table.read_cut_table``), which refuses every violated
invariant naming the root or id: every index tree root exactly once and no
other, a piece holds trees of one forest, an uncut forest keeps its id, the
largest piece keeps a cut forest's id (ties by the smallest root), fresh ids
above the catalogue's maximum in descending piece size, and contiguous from
that maximum plus one. Nothing has been written when a table is refused.

The installed table gives the output's enumeration and everything the passes
and the sidecar need:

- the output's forests are the input's, in their order, followed by the fresh
  ids ascending, so fresh id ``catalogue_max + 1 + k`` is ``ForestIndex``
  ``n_forests_total + k`` and every untouched forest keeps its
  ``ForestIndex`` (the enumeration the census's partition simulation installs);
- each root ordinal's output ``ForestIndex`` (``tree_forest``, 8 B per tree,
  resident through the passes), from which pass 1 labels every halo's forest;
- the output's per-forest totals, ``SourceHaloID`` bases and
  ``max_halo_rank_in_forest`` (``pass0.source_bases`` over the new totals),
  and the multiplier check against the output's bounds;
- the split forests (an input forest with more than one piece) and their
  pieces (id, original forest, trees, halos, which keeps the id), written at
  the end as ``cut_pieces.npy`` beside the manifest;
- the output sidecar: the index files re-read under the table's ids
  (``source_index.SourceIndex.forest_table``), so a piece held in one file
  carries that file's conversion ordinal and its rank among the file's units
  by first ``#tree`` marker, and a piece spanning files carries -1/-1;
- the per-tree inventory's forest columns, which name each tree's piece.

Memory: the ``cut_*`` entries of ``rewriter.inputs.MEMORY_FORMULAS``. The
pieces are written out and released before the slab passes, which keep
``tree_forest``, ``tree_original`` and ``tree_totals``, the output's bases and
forest ids, and the split masks.
"""

import os
import sys
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.cut_table import read_cut_table  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import HorizontalDataset  # noqa: E402
from rewriter.inputs import Prepared, check_multiplier, output_sidecar  # noqa: E402
from rewriter.pass0 import Census, source_bases  # noqa: E402
from rewriter.passes import CutSlab, tree_labels  # noqa: E402
from source_index import ForestTable, SourceIndex  # noqa: E402

#: One row per piece of a split forest (``cut_pieces.npy``), in ascending id.
PIECES_DTYPE = np.dtype(
    [
        ("id", "<i8"),
        ("forest_id", "<i8"),
        ("forest_index", "<i8"),
        ("original_forest_index", "<i8"),
        ("trees", "<i8"),
        ("halos", "<i8"),
        ("keeps_id", "<i8"),
    ]
)

#: Pieces quoted in the summary's list of the largest.
N_LARGEST = 10

#: The columns the labelling pre-pass reads.
LABEL_COLUMNS = ("Descendant", "MostBoundID")


@dataclass
class CutPlan:
    """An installed cut table: the output's enumeration and what the passes read."""

    table_ids: Optional[np.ndarray]  # int64 [n_trees]: the table's forest id per index tree
    tree_forest: np.ndarray  # int64 [n_trees]: each root ordinal's output ForestIndex
    tree_original: np.ndarray  # int64 [n_trees]: each root ordinal's input ForestIndex
    tree_totals: np.ndarray  # int64 [n_trees]: each tree's halos over every slab
    forest_ids: np.ndarray  # int64 [n_forests_out]: the output sidecar's ForestID
    totals: np.ndarray  # int64 [n_forests_out]: halos per output forest
    sid_base: np.ndarray  # int64 [n_forests_out]
    max_rank: int
    split_old: np.ndarray  # bool [n_forests_in]: the forests the table cuts
    split_new: np.ndarray  # bool [n_forests_out]: their pieces, kept or fresh
    pieces: Optional[np.ndarray]  # PIECES_DTYPE, ascending id; None once written out
    n_forests_in: int
    catalogue_max: int
    index: SourceIndex  # the index files under the table's ids
    forest_table: ForestTable  # its per-forest table
    sidecar: Dict[str, np.ndarray]

    @property
    def n_forests(self) -> int:
        return int(self.forest_ids.size)

    @property
    def n_fresh(self) -> int:
        return self.n_forests - self.n_forests_in

    def slab(self) -> CutSlab:
        return CutSlab(
            tree_forest=self.tree_forest,
            tree_original=self.tree_original,
            split_old=self.split_old,
            split_new=self.split_new,
        )

    def release_tables(self) -> None:
        """Drop what the slab passes do not read (the pieces once written)."""
        self.index = None
        self.forest_table = None
        self.sidecar = None
        self.totals = None
        self.pieces = None
        self.table_ids = None

    def record(self, pieces: np.ndarray) -> Dict[str, object]:
        """The cut's summary: the forests before and after, the split forests,
        the pieces (reloaded from ``cut_pieces.npy``) and the largest of them."""
        by_halos = np.lexsort((pieces["id"], -pieces["halos"]))[:N_LARGEST]
        return {
            "forests_before": int(self.n_forests_in),
            "forests_after": int(self.n_forests),
            "split_forests": int(np.count_nonzero(self.split_old)),
            "fresh_pieces": int(self.n_fresh),
            "catalogue_max_forest_id": int(self.catalogue_max),
            "first_fresh_forest_index": int(self.n_forests_in),
            "pieces": {
                "count": int(pieces.size),
                "listed": "every piece of every split forest, in ascending id, in cut_pieces.npy "
                "beside the manifest; every other forest keeps its id and is not listed",
                "largest": [
                    {name: int(pieces[name][at]) for name in PIECES_DTYPE.names} for at in by_halos
                ],
            },
        }


def label_tree_totals(
    dataset: HorizontalDataset, roots: np.ndarray, block_rows: int, log: Callable[[str], None]
) -> np.ndarray:
    """Each tree's halo total over every slab, from one descending labelling
    pass (module docstring); refused when the totals do not sum to the
    headers' halo count."""
    totals = np.zeros(roots.size, dtype=np.int64)
    labels_next = None
    for snap in reversed(dataset.snapshots):
        path = dataset.snapshot_path(snap)
        labels = np.empty(dataset.n_halos[snap], dtype=np.int32)
        for start, block in dataset.iter_columns(snap, LABEL_COLUMNS, block_rows):
            desc = np.asarray(block["Descendant"], dtype=np.int64)
            mbid = np.asarray(block["MostBoundID"], dtype=np.int64)
            labels[start : start + desc.size] = tree_labels(path, desc, mbid, roots, labels_next)
        present, counts = np.unique(labels, return_counts=True)
        totals[present] += counts
        labels_next = labels
        log("cut: snapshot {} labelled ({} halos)".format(snap, labels.size))
    if int(totals.sum()) != dataset.total_halos:
        raise ConverterError(
            "the tree totals sum to {} halos but the headers count {}".format(
                int(totals.sum()), dataset.total_halos
            )
        )
    return totals


def _pieces(
    table_ids: np.ndarray,
    tree_totals: np.ndarray,
    original_index: np.ndarray,
    split_old: np.ndarray,
    forest_ids_in: np.ndarray,
    forest_ids_out: np.ndarray,
) -> np.ndarray:
    """The split forests' pieces (module docstring), in ascending id."""
    in_split = split_old[original_index]
    ids = table_ids[in_split]
    order = np.argsort(ids, kind="stable")
    ids = ids[order]
    if ids.size == 0:
        return np.zeros(0, dtype=PIECES_DTYPE)
    starts = np.flatnonzero(np.r_[True, ids[1:] != ids[:-1]])
    pieces = np.empty(starts.size, dtype=PIECES_DTYPE)
    pieces["id"] = ids[starts]
    pieces["trees"] = np.diff(np.append(starts, ids.size))
    pieces["halos"] = np.add.reduceat(tree_totals[in_split][order], starts)
    pieces["original_forest_index"] = original_index[in_split][order][starts]
    pieces["forest_id"] = forest_ids_in[pieces["original_forest_index"]]
    pieces["forest_index"] = np.searchsorted(forest_ids_out, pieces["id"])
    pieces["keeps_id"] = pieces["id"] == pieces["forest_id"]
    return pieces


def install_table(
    prepared: Prepared, census: Census, tree_totals: np.ndarray, log: Callable[[str], None]
) -> CutPlan:
    """Read the cut table, check it, and derive the output's enumeration,
    totals, bases, bounds, pieces and sidecar (module docstring).

    Raises:
        ConverterError: for every violated table invariant, a multiplier that
            cannot encode the output's bounds, or an enumeration that is not
            the input's forests followed by the contiguous fresh ids.
    """
    index = prepared.index
    request = prepared.request
    if not np.array_equal(census.roots, index.tree_roots):
        raise ConverterError(
            "the trees' roots are not the index files' tree roots; the root correspondence "
            "must have passed before a table is installed"
        )
    forest_ids_in = np.ascontiguousarray(prepared.sidecar["ForestID"], dtype=np.int64)
    n_in = int(forest_ids_in.size)
    catalogue_max = int(forest_ids_in.max()) if n_in else 0
    log("cut: reading {}".format(request.cut_table))
    _roots, table_ids = read_cut_table(
        request.cut_table, index.tree_roots, index.forest_ids, tree_totals
    )
    del _roots
    cut_index = SourceIndex(
        tree_roots=index.tree_roots,
        forest_ids=table_ids,
        file_ids=index.file_ids,
        offsets=index.offsets,
        filenames=index.filenames,
    )
    forest_table = cut_index.forest_table()
    forest_ids_out = forest_table.forest_ids
    n_fresh = int(np.count_nonzero(forest_ids_out > catalogue_max))
    expected = np.concatenate(
        [forest_ids_in, catalogue_max + 1 + np.arange(n_fresh, dtype=np.int64)]
    )
    if not np.array_equal(forest_ids_out, expected):
        raise ConverterError(
            "{}: the table's forests are not the input's {} forests followed by {} fresh ids "
            "contiguous from {}".format(request.cut_table, n_in, n_fresh, catalogue_max + 1)
        )
    del expected
    tree_forest = np.searchsorted(forest_ids_out, table_ids).astype(np.int64)
    # exact: every total is an integer far below 2^53
    totals = np.bincount(
        tree_forest, weights=tree_totals.astype(np.float64), minlength=forest_ids_out.size
    ).astype(np.int64)
    if int(totals.sum()) != int(tree_totals.sum()):
        raise ConverterError("the output's forest totals do not sum to the trees' totals")
    sid_base, max_rank = source_bases(totals)
    check_multiplier(request, int(forest_ids_out.size), max_rank)
    original_index = np.searchsorted(forest_ids_in, index.forest_ids).astype(np.int64)
    split_old = np.zeros(n_in, dtype=bool)
    split_old[original_index[table_ids != index.forest_ids]] = True
    split_new = np.ones(forest_ids_out.size, dtype=bool)
    split_new[:n_in] = split_old
    pieces = _pieces(
        table_ids, tree_totals, original_index, split_old, forest_ids_in, forest_ids_out
    )
    # a piece held in one file needs that file's conversion ordinal, which a
    # version 3 input's sidecar gives only through a forest held in that file
    single_files = np.unique(forest_table.file_id[forest_table.file_id >= 0])
    no_ordinal = [
        int(f) for f in single_files.tolist() if prepared.file_ordinals.get(int(f), -1) < 0
    ]
    if no_ordinal:
        raise ConverterError(
            "{}: {} piece(s) held in one file whose FileID the input sidecar gives no "
            "conversion ordinal for ({}); every input forest of those files spans files, and the "
            "cut cannot infer the converter's file order".format(
                request.cut_table,
                int(np.count_nonzero(np.isin(forest_table.file_id, no_ordinal))),
                ", ".join(
                    "FileID {} ({})".format(f, index.filenames.get(f)) for f in no_ordinal[:5]
                ),
            )
        )
    del single_files, no_ordinal
    sidecar = output_sidecar(forest_table, prepared.file_ordinals, forest_ids_out)
    log(
        "cut: table installed -- {} forest(s) become {} ({} split, {} fresh piece(s)); "
        "max_halo_rank_in_forest {}".format(
            n_in, forest_ids_out.size, int(np.count_nonzero(split_old)), n_fresh, max_rank
        )
    )
    return CutPlan(
        table_ids=table_ids,
        tree_forest=tree_forest,
        tree_original=original_index,
        tree_totals=np.ascontiguousarray(tree_totals, dtype=np.int64),
        forest_ids=np.ascontiguousarray(forest_ids_out, dtype=np.int64),
        totals=totals,
        sid_base=sid_base,
        max_rank=max_rank,
        split_old=split_old,
        split_new=split_new,
        pieces=pieces,
        n_forests_in=n_in,
        catalogue_max=catalogue_max,
        index=cut_index,
        forest_table=forest_table,
        sidecar=sidecar,
    )


def per_snapshot_rows(per_snapshot: Dict[str, Dict[str, int]]) -> List[Dict[str, int]]:
    """The per-slab counts as the summary lists them, ascending by snapshot."""
    rows = []
    for key in sorted(per_snapshot, key=int):
        rows.append({"snapshot": int(key), **{k: int(v) for k, v in per_snapshot[key].items()}})
    return rows
