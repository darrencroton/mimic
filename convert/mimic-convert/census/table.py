"""The decided cut table: every forest cut at its z = 0 FoF groups (F6, revision 5).

**The rule.** Every tree joins the forest of its terminal halo's FoF group at
the dataset's final snapshot. A group is identified by the tree of its z = 0
central, the row its members' ``FirstHaloInFOFgroup`` points to, so a tree's
**piece** is the tree of its terminal halo's central. A forest whose trees end
in one group is unchanged; a forest ending in several (a **split forest**) is
cut into exactly its groups, named under F6 (``census/cut_table.py``: the
largest piece keeps the forest's id, every other one takes a fresh id above
the catalogue's maximum in descending size, ties by the smallest tree root id).
This is the partition that cuts every co-membership ended before the final
snapshot and keeps every one present at it: no halo is promoted at the final
snapshot, every z = 0 FoF group of the input is preserved, and no tree is
split. "Ended" is fixed by the dataset's final snapshot (its scale factor is
recorded); it is a dataset definition, not a claim about any encounter.

The rule's scope is catalogues in which every tree reaches the final snapshot,
as Consistent-Trees guarantees: a census holding a tree whose root lies in an
earlier slab is refused before anything is loaded or written, naming the count
(:func:`require_all_reach_final`). Then every tree has exactly one halo in the
final slab, its terminal halo, and the final slab's rows and the trees are in
one-to-one correspondence through the ``trees`` labels.

**Construction** (:func:`z0_central_trees`, :func:`decided_table`): one bounded
pass over the final slab's ``FirstHaloInFOFgroup`` and ``ForestIndex`` with its
labels resident. Every central reference is validated: in range, the row it
names is its own central (self-central), and that row's tree is in the
member's original forest. Each tree's piece key is its central's tree; the
z = 0 centrals are counted per forest. A **forest selection** (``--forest-index``)
splits only the named forests: every other forest keeps its identity
assignment, and the record says so (a restricted table is never labelled
whole-simulation). The pieces of the split forests are named, and for every
split forest the piece count is asserted equal to its z = 0 centrals.

**The** ``table`` **subcommand** (:func:`run_table`) writes, under
``<aggregate>/table/`` (``table-restricted/`` for a selection), the complete
table in ``forests.list`` shape, its streamed JSON record (dataset identity,
index files' md5, the rule stated as "the z = 0 FoF groups of every forest",
the pieces of the split forests with their peak occupancy and slab, and the
table's md5) and ``summary.json`` last. It is written only when the ``trees``
root correspondence passed and the index files given pass the census's index
check (:func:`check_index`); the invariants are checked on write
(:func:`cut_table.check_table`) and on read (:func:`cut_table.read_cut_table`).
Each piece's peak comes from the aggregates alone, one slab's tree pairs at a
time (:func:`piece_slab_counts`).

**Reads.** ``FirstHaloInFOFgroup`` and ``ForestIndex`` of the final slab, once:
``(w_link + 8) x n_halos(final)`` bytes, with ``w_link`` 4 B in version 2 and
8 B in version 3, measured into ``summary.json`` (``io``). The labels of the
final slab (4 B per halo) and every slab's tree pairs (8 B per present tree)
are read from the aggregates.

**Memory**, with ``n`` the trees (equal to the final slab's halos), ``F`` =
``n_forests_total``, ``t`` the split forests' trees and ``p`` their pieces
(per-unit costs measured with ``tracemalloc`` on the micro-Uchuu census;
Shin-Uchuu figures at ``n`` = 315,004,242, ``F`` = 166,547,771):

- the census arrays held throughout: roots and tree forests, **16 B x n**
  (5.0 GB; the tree forests are released once the pieces are built), the
  sidecar ``ForestID`` (8 B x F, 1.3 GB), the tree totals mapped;
- the z = 0 groups (:func:`z0_central_trees`): the final slab's labels and each
  tree's central tree (**8 B x n**), the self-central masks (**2 B x n**), the
  centrals per forest (**8 B x F**), and one block's columns and gathers, about
  **30 B x block_rows**: about 4.6 GB;
- the pieces (:func:`decided_table`): the split mask (1 B x F), the halos per
  forest (8 B x F), the local tree list and its inverse map (**4 B x t + 4 B x
  n**), and :func:`cut_table.name_pieces` at about **110 B x t** at its peak
  (measured with every tree its own piece, its worst case), falling to about
  **57 B x p + 4 B x t** held (the piece columns and each tree's piece);
- the peaks (:func:`piece_slab_counts`): one slab's tree pairs and their
  gathers, about **19 B per present tree**, and two per-piece vectors (**16 B x
  p**), with the peaks themselves (**16 B x p**) held;
- the index files, loaded by the caller before the run: 79 B per index tree
  at the load's peak, then their roots and forest ids, 16 B per tree;
- while the table is checked and written: ``census/cut_table.py``'s check, about
  **96 B per catalogue tree** at its peak, the table's ids and the trees'
  totals, 16 B per tree; the record, 8 B per piece and one chunk of 2^16
  pieces as Python values and text, about 7 MB.

The figures the run measured are in ``summary.json`` (``memory``: the peak
resident set after each phase) beside these formulas.
"""

import hashlib
import os
import resource
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.aggregate import (  # noqa: E402
    SUMMARY_NAME,
    begin,
    bound_identity,
    load_array,
    require_completed,
    write_json,
)
from census.cut_table import (  # noqa: E402
    check_table,
    name_pieces,
    table_ids,
    write_table,
    write_table_record,
)
from census.trees import load_labels, load_roots, load_slab_tree_pairs, trees_dir  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402

TABLE_DIR = "table"

#: Appended to a subcommand's output directory when it runs on a forest selection.
RESTRICTED_SUFFIX = "-restricted"

#: The decided rule, as the table record states it.
RULE_TEXT = "the z = 0 FoF groups of every forest"

#: The same rule over a forest selection.
RESTRICTED_RULE_TEXT = (
    "the z = 0 FoF groups of the selected forests; every other forest keeps its identity assignment"
)

#: Columns of the final slab the construction reads.
FINAL_COLUMNS = ("FirstHaloInFOFgroup", "ForestIndex")

#: Examples quoted per reported count, and entries per list of largest items.
N_EXAMPLES = 5
N_LARGEST = 10

_INT32_MAX = int(np.iinfo(np.int32).max)
_HASH_ROWS = 1 << 22


def _quiet(_message: str) -> None:
    pass


def output_dir(aggregate_dir, name: str, selection: Optional[np.ndarray]) -> Path:
    """A subcommand's output directory: ``<aggregate>/<name>``, or
    ``<aggregate>/<name>-restricted`` for a forest selection."""
    return Path(aggregate_dir) / (name if selection is None else name + RESTRICTED_SUFFIX)


def table_dir(aggregate_dir, selection: Optional[np.ndarray] = None) -> Path:
    return output_dir(aggregate_dir, TABLE_DIR, selection)


# ---- measurement ---------------------------------------------------------------


class ReadMeter:
    """Bytes of ``/halos`` columns read, per column: the arrays the dataset
    returned, which is what the census asked HDF5 for (HDF5 reads whole
    chunks, so the device may move more)."""

    def __init__(self):
        self.per_column: Dict[str, int] = {}

    def add(self, columns: Dict[str, np.ndarray]) -> None:
        for name, values in columns.items():
            self.per_column[name] = self.per_column.get(name, 0) + int(values.nbytes)

    @property
    def total(self) -> int:
        return sum(self.per_column.values())

    def record(self, formula: str) -> Dict:
        return {
            "formula": formula,
            "bytes_read": self.total,
            "per_column": dict(sorted(self.per_column.items())),
        }


def peak_rss_bytes() -> int:
    """The process's peak resident set so far, in bytes (``ru_maxrss`` is in
    bytes on macOS and in KiB on Linux)."""
    peak = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    return peak if sys.platform == "darwin" else peak * 1024


# ---- inputs and refusals -----------------------------------------------------------


def selected_forests(forest_indices: Optional[Sequence[int]], n_forests: int):
    """The forest selection, ascending and unique, or ``None`` for the whole
    simulation.

    Raises:
        ConverterError: for a ``ForestIndex`` outside ``[0, n_forests)``.
    """
    if not forest_indices:
        return None
    forests = np.unique(np.asarray([int(f) for f in forest_indices], dtype=np.int64))
    bad = forests[(forests < 0) | (forests >= n_forests)]
    if bad.size:
        raise ConverterError("ForestIndex {} outside [0, {})".format(bad.tolist(), n_forests))
    return forests


def scope_record(selection: Optional[np.ndarray], forest_ids: np.ndarray) -> Dict:
    """What a table or cut covers: the whole simulation, or a selection."""
    if selection is None:
        return {"scope": "whole simulation", "selection": None}
    return {
        "scope": "restricted to the selected forests",
        "selection": {
            "forest_index": [int(f) for f in selection],
            "forest_id": [int(forest_ids[f]) for f in selection],
        },
    }


def require_census(dataset: HorizontalDataset, aggregate_dir) -> Tuple[Dict, Dict]:
    """The directory's dataset identity and its completed ``trees`` summary,
    with a passed root correspondence and no halo outside its tree's forest.

    Raises:
        ConverterError: naming the first refusal.
    """
    identity = bound_identity(aggregate_dir)
    if dataset.identity() != identity:
        raise ConverterError(
            "{}: produced from a different dataset (identity differs)".format(aggregate_dir)
        )
    trees = require_completed(
        aggregate_dir,
        trees_dir(aggregate_dir),
        "trees",
        identity,
        ("root_correspondence", "forest_mismatch_halos", "n_trees"),
    )
    verdict = trees["root_correspondence"].get("verdict")
    if verdict != "pass":
        raise ConverterError(
            "{}: the trees root correspondence verdict is {!r}; the decided table needs it to "
            "pass".format(aggregate_dir, verdict)
        )
    mismatched = int(trees["forest_mismatch_halos"])
    if mismatched:
        raise ConverterError(
            "{}: the trees summary records {} halo(s) whose ForestIndex is not their tree's "
            "forest; the converter forbids this".format(aggregate_dir, mismatched)
        )
    return identity, trees


def require_all_reach_final(dataset: HorizontalDataset, aggregate_dir) -> int:
    """Refuse a census holding a tree that ends before the final snapshot
    (F6: the rule's scope); returns the tree count.

    Raises:
        ConverterError: naming how many trees end early, with example root ids.
    """
    last = dataset.snapshots[-1] if dataset.snapshots else -1
    root_snapshot = load_array(trees_dir(aggregate_dir) / "tree_root_snapshot.npy")
    early = np.flatnonzero(root_snapshot != last)
    if early.size:
        roots = load_roots(aggregate_dir)
        raise ConverterError(
            "{}: {} tree(s) end before the final snapshot {} (e.g. roots {}); the decided table "
            "covers only catalogues in which every tree reaches the final snapshot".format(
                aggregate_dir, early.size, last, roots[early[:N_EXAMPLES]].tolist()
            )
        )
    return int(root_snapshot.size)


def stale_outputs(directory, written: Sequence[str]) -> List[str]:
    """Entries of an output directory a run would not rewrite. A
    ``<file>.tmp`` left by an interrupted write of a file the run rewrites is
    replaced by that atomic write, so it is not counted."""
    directory = Path(directory)
    if not directory.is_dir():
        return []
    rewritten = set(written) | {name + ".tmp" for name in written}
    return sorted(entry.name for entry in directory.iterdir() if entry.name not in rewritten)


def refuse_stale(directory, written: Sequence[str]) -> None:
    """Refuse an output directory holding anything a run would not rewrite.

    Raises:
        ConverterError: naming the directory and examples.
    """
    stale = stale_outputs(directory, written)
    if stale:
        raise ConverterError(
            "{}: {} entr(y/ies) this run would not rewrite (e.g. {}); remove them by hand or "
            "use a fresh aggregate directory".format(directory, len(stale), stale[:N_EXAMPLES])
        )


def check_index(
    index_roots: np.ndarray,
    index_forest_ids: np.ndarray,
    roots: np.ndarray,
    tree_forest: np.ndarray,
    forest_ids: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
) -> Dict:
    """Whether the index files given to this run describe this census: the
    root sets must be equal and every root's ``forests.list`` forest id must
    be the sidecar ``ForestID`` of its tree's forest -- the ``trees``
    correspondence, applied to the index files in hand.

    Returns:
        ``{"verdict": "pass" | "fail", "message", ...counts and examples}``;
        the caller refuses a table on ``fail``.
    """
    if index_roots.size != roots.size or not np.array_equal(index_roots, roots):
        missing = np.setdiff1d(roots, index_roots, assume_unique=True)
        extra = np.setdiff1d(index_roots, roots, assume_unique=True)
        return {
            "verdict": "fail",
            "census_roots_missing": {
                "count": int(missing.size),
                "examples": missing[:N_EXAMPLES].tolist(),
            },
            "index_roots_without_tree": {
                "count": int(extra.size),
                "examples": extra[:N_EXAMPLES].tolist(),
            },
            "message": "the index files do not list this census's tree roots: {} census root(s) "
            "missing (e.g. {}), {} root(s) the census has no tree for (e.g. {})".format(
                missing.size, missing[:N_EXAMPLES].tolist(), extra.size, extra[:N_EXAMPLES].tolist()
            ),
        }
    wrong = 0
    examples: List[int] = []
    for start in range(0, roots.size, block_rows):
        stop = min(start + block_rows, roots.size)
        sidecar = forest_ids[np.asarray(tree_forest[start:stop], dtype=np.int64)]
        differs = np.flatnonzero(np.asarray(index_forest_ids[start:stop]) != sidecar)
        wrong += int(differs.size)
        examples.extend(roots[start + differs[: N_EXAMPLES - len(examples)]].tolist())
    record = {
        "verdict": "fail" if wrong else "pass",
        "roots_in_another_forest": {"count": wrong, "examples": examples},
    }
    if wrong:
        record["message"] = (
            "the index files give {} tree root(s) a forest other than the dataset's (e.g. roots "
            "{}); they are not the index files this census describes".format(wrong, examples)
        )
    return record


# ---- the z = 0 FoF groups ---------------------------------------------------------


def check_central_rows(central: np.ndarray, n_rows: int, path) -> np.ndarray:
    """``FirstHaloInFOFgroup`` values as int64, refusing one outside the slab."""
    central = central.astype(np.int64, copy=False)
    if central.size and (int(central.min()) < 0 or int(central.max()) >= n_rows):
        bad = np.flatnonzero((central < 0) | (central >= n_rows))
        raise ConverterError(
            "{}: {} FirstHaloInFOFgroup value(s) outside [0, {}) (e.g. {})".format(
                path, bad.size, n_rows, central[bad[:N_EXAMPLES]].tolist()
            )
        )
    return central


def z0_central_trees(
    dataset: HorizontalDataset,
    aggregate_dir,
    tree_forest: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
) -> Tuple[np.ndarray, np.ndarray]:
    """Each tree's z = 0 central tree, and the z = 0 centrals per forest.

    One bounded pass over the final slab's ``FirstHaloInFOFgroup`` and
    ``ForestIndex`` with its labels resident; every central reference is
    validated before anything is returned.

    Returns:
        ``(central_tree, centrals)``: int32 root ordinal per root ordinal, and
        int64 per ``ForestIndex``.

    Raises:
        ConverterError: for a final slab that does not hold exactly one halo per
            tree, a reference outside the slab, one naming a row that is not
            its own central, or one whose row lies in another forest.
    """
    last = dataset.snapshots[-1]
    n_rows = dataset.n_halos[last]
    n_trees = int(tree_forest.size)
    path = dataset.snapshot_path(last)
    if n_rows != n_trees:
        raise ConverterError(
            "{}: the final slab holds {} halos but the census has {} trees; with every tree "
            "reaching the final snapshot they are one to one".format(path, n_rows, n_trees)
        )
    if n_rows > _INT32_MAX:
        raise ConverterError("{}: more rows than int32 row indices hold".format(path))
    labels = load_labels(aggregate_dir, last, mmap=False)
    if labels.size != n_rows:
        raise ConverterError(
            "{}: {} labels for {} halos; the trees aggregates are stale".format(
                path, labels.size, n_rows
            )
        )
    central_tree = np.full(n_trees, -1, dtype=np.int32)
    is_central = np.zeros(n_rows, dtype=bool)
    referenced = np.zeros(n_rows, dtype=bool)
    centrals = np.zeros(dataset.n_forests_total, dtype=np.int64)
    foreign = 0
    foreign_example = None
    for start, block in dataset.iter_columns(last, FINAL_COLUMNS, block_rows):
        if meter is not None:
            meter.add(block)
        central = check_central_rows(block["FirstHaloInFOFgroup"], n_rows, path)
        forest = block["ForestIndex"].astype(np.int64, copy=False)
        stop = start + central.size
        own = labels[start:stop]
        host = labels[central]
        wrong = np.flatnonzero(tree_forest[host] != forest)
        if wrong.size:
            foreign += int(wrong.size)
            if foreign_example is None:
                at = int(wrong[0])
                foreign_example = (start + at, int(forest[at]), int(central[at]))
        central_tree[own] = host
        own_central = central == np.arange(start, stop, dtype=np.int64)
        is_central[start:stop] = own_central
        referenced[central] = True
        np.add.at(centrals, forest[own_central], 1)
    if foreign:
        row, forest, target = foreign_example
        raise ConverterError(
            "{}: {} z = 0 halo(s) whose FirstHaloInFOFgroup names a row of another forest (e.g. "
            "row {} of ForestIndex {} names row {})".format(path, foreign, row, forest, target)
        )
    not_self = np.flatnonzero(referenced & ~is_central)
    if not_self.size:
        raise ConverterError(
            "{}: {} row(s) named as a FoF central by FirstHaloInFOFgroup are not their own central "
            "(e.g. rows {})".format(path, not_self.size, not_self[:N_EXAMPLES].tolist())
        )
    if n_trees and int(central_tree.min()) < 0:
        raise ConverterError(
            "{}: the final slab's labels do not name every tree once; the trees aggregates are "
            "stale".format(path)
        )
    return central_tree, centrals


@dataclass
class DecidedTable:
    """The decided table's pieces over its split forests."""

    selection: Optional[np.ndarray]  # the forest selection, None for the whole simulation
    split: np.ndarray  # ForestIndex of every split forest, ascending
    local: np.ndarray  # int32 root ordinals of the split forests' trees, ascending
    local_of: np.ndarray  # int32 local index per root ordinal, -1 outside the split forests
    tree_piece: np.ndarray  # int32 piece index per local tree
    pieces: Dict[str, np.ndarray]  # cut_table.name_pieces' per-piece columns
    centrals: np.ndarray  # int64 z = 0 centrals per ForestIndex
    forest_halos: np.ndarray  # int64 halos per ForestIndex over all slabs
    fresh_id_floor: int

    @property
    def n_pieces(self) -> int:
        return int(self.pieces["id"].size)

    @property
    def n_fresh(self) -> int:
        return int(np.count_nonzero(~self.pieces["keeps_id"]))


def decided_table(
    dataset: HorizontalDataset,
    aggregate_dir,
    roots: np.ndarray,
    tree_forest: np.ndarray,
    tree_totals: np.ndarray,
    forest_ids: np.ndarray,
    selection: Optional[np.ndarray] = None,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
) -> DecidedTable:
    """Build the decided table's pieces (module docstring). Fresh ids start
    above the sidecar's largest ``ForestID``, which, when the index check
    passes, is the index files' largest too.

    Raises:
        ConverterError: as :func:`z0_central_trees`, or when a split forest's
            piece count is not its z = 0 central count.
    """
    central_tree, centrals = z0_central_trees(
        dataset, aggregate_dir, tree_forest, block_rows, meter
    )
    n_forests = dataset.n_forests_total
    split_mask = centrals > 1
    if selection is not None:
        chosen = np.zeros(n_forests, dtype=bool)
        chosen[selection] = True
        split_mask &= chosen
    split = np.flatnonzero(split_mask)
    local = np.flatnonzero(split_mask[tree_forest]).astype(np.int32)
    del split_mask
    local_of = np.full(roots.size, -1, dtype=np.int32)
    local_of[local] = np.arange(local.size, dtype=np.int32)
    components = local_of[central_tree[local]]
    del central_tree
    if components.size and int(components.min()) < 0:  # pragma: no cover - forests checked above
        raise ConverterError("a z = 0 central's tree lies outside its member's split forest")
    fresh_id_floor = int(forest_ids.max()) if forest_ids.size else 0
    pieces = name_pieces(
        components,
        tree_forest[local],
        np.asarray(tree_totals[local], dtype=np.int64),
        roots[local],
        forest_ids,
        fresh_id_floor,
    )
    del components
    tree_piece = pieces.pop("piece").astype(np.int32)
    per_forest = np.bincount(pieces["forest_index"], minlength=n_forests)[split]
    wrong = np.flatnonzero(per_forest != centrals[split])
    if wrong.size:
        at = int(wrong[0])
        raise ConverterError(
            "{} split forest(s) whose piece count is not their z = 0 central count (e.g. "
            "ForestIndex {}: {} pieces, {} centrals)".format(
                wrong.size, int(split[at]), int(per_forest[at]), int(centrals[split[at]])
            )
        )
    forest_halos = np.zeros(n_forests, dtype=np.int64)
    for start in range(0, roots.size, _HASH_ROWS):
        stop = min(start + _HASH_ROWS, roots.size)
        np.add.at(
            forest_halos,
            np.asarray(tree_forest[start:stop], dtype=np.int64),
            np.asarray(tree_totals[start:stop], dtype=np.int64),
        )
    return DecidedTable(
        selection=selection,
        split=split,
        local=local,
        local_of=local_of,
        tree_piece=tree_piece,
        pieces=pieces,
        centrals=centrals,
        forest_halos=forest_halos,
        fresh_id_floor=fresh_id_floor,
    )


def assignment_digest(table: DecidedTable, roots: np.ndarray) -> str:
    """SHA-256 of every split-forest tree's (root id, piece id) as little-endian
    int64 pairs in ascending root: the table's assignment, independent of
    where it is written, so the ``table`` and ``cut`` outputs can be tied."""
    digest = hashlib.sha256()
    ids = table.pieces["id"]
    for start in range(0, table.local.size, _HASH_ROWS):
        part = table.local[start : start + _HASH_ROWS]
        pairs = np.empty((part.size, 2), dtype="<i8")
        pairs[:, 0] = roots[part]
        pairs[:, 1] = ids[table.tree_piece[start : start + _HASH_ROWS]]
        digest.update(pairs.tobytes())
    return digest.hexdigest()


def group_counts(table: DecidedTable, n_final_halos: int, forest_ids: np.ndarray) -> Dict:
    """The z = 0 FoF-group counts the table is built on: in all, in the forest
    with the largest total, and in the other split forests."""
    centrals = table.centrals
    split = table.split
    largest = int(np.argmax(table.forest_halos)) if table.forest_halos.size else -1
    others = split[split != largest]
    by_groups = split[np.lexsort((split, -centrals[split]))][:N_LARGEST]
    return {
        "z0_halos": int(n_final_halos),
        "z0_groups": int(centrals.sum()),
        "forests_before": int(centrals.size),
        "forests_after": int(centrals.size) + table.n_fresh,
        "split_forests": {
            "count": int(split.size),
            "groups": int(centrals[split].sum()),
            "extra_groups": int(centrals[split].sum() - split.size),
            "halos": int(table.forest_halos[split].sum()),
        },
        "largest_forest": (
            None
            if largest < 0
            else {
                "forest_index": largest,
                "forest_id": int(forest_ids[largest]),
                "halos": int(table.forest_halos[largest]),
                "groups": int(centrals[largest]),
                "split": bool(np.isin(largest, split)),
            }
        ),
        "other_split_forests": {
            "note": "the split forests other than the forest with the largest total",
            "count": int(others.size),
            "extra_groups": int(centrals[others].sum() - others.size),
            "halos": int(table.forest_halos[others].sum()),
        },
        "most_groups": [
            {
                "forest_index": int(f),
                "forest_id": int(forest_ids[f]),
                "groups": int(centrals[f]),
                "halos": int(table.forest_halos[f]),
            }
            for f in by_groups
        ],
        "forests_with_one_group": int(np.count_nonzero(centrals == 1)),
    }


# ---- per-piece slab counts -------------------------------------------------------------


def piece_slab_counts(aggregate_dir, snap: int, table: DecidedTable) -> np.ndarray:
    """One slab's halos per piece, from its tree pairs (the aggregates alone)."""
    ordinals, counts = load_slab_tree_pairs(aggregate_dir, snap)
    local = table.local_of[ordinals]
    named = local >= 0
    return np.bincount(
        table.tree_piece[local[named]],
        weights=counts[named].astype(np.float64),  # exact: a slab's counts are below 2^31
        minlength=table.n_pieces,
    ).astype(np.int64)


def piece_peaks(
    dataset: HorizontalDataset,
    aggregate_dir,
    table: DecidedTable,
    log: Callable[[str], None] = _quiet,
) -> Tuple[np.ndarray, np.ndarray]:
    """Each piece's largest slab count and the lowest-numbered slab of it."""
    peak = np.zeros(table.n_pieces, dtype=np.int64)
    peak_snapshot = np.full(table.n_pieces, -1, dtype=np.int64)
    for snap in dataset.snapshots:
        per_piece = piece_slab_counts(aggregate_dir, snap, table)
        # ascending slabs and a strict test: a tie keeps the lowest-numbered slab
        better = per_piece > peak
        peak[better] = per_piece[better]
        peak_snapshot[better] = snap
        log("table: snapshot {} piece counts applied".format(snap))
    return peak, peak_snapshot


def piece_entry(table: DecidedTable, at: int, peak=None, peak_snapshot=None) -> Dict:
    pieces = table.pieces
    entry = {
        "id": int(pieces["id"][at]),
        "forest_index": int(pieces["forest_index"][at]),
        "forest_id": int(pieces["forest_id"][at]),
        "trees": int(pieces["trees"][at]),
        "halos": int(pieces["halos"][at]),
        "smallest_root_id": int(pieces["smallest_root_id"][at]),
        "keeps_id": bool(pieces["keeps_id"][at]),
    }
    if peak is not None:
        entry["peak_occupancy"] = int(peak[at])
        entry["peak_snapshot"] = int(peak_snapshot[at])
    return entry


# ---- the subcommand ----------------------------------------------------------------

#: The files the table subcommand writes in its output directory.
TABLE_OUTPUTS = ("forests.list", "record.json", SUMMARY_NAME)


def prepare_table(dataset: HorizontalDataset, aggregate_dir, selection=None) -> Dict:
    """Every refusal that needs no index file and no pass, before anything is
    loaded or written: the directory's dataset, a completed ``trees`` with a
    passed correspondence, every tree reaching the final snapshot, and an
    output directory holding nothing the run would not rewrite.

    Raises:
        ConverterError: naming the first refusal.
    """
    identity, trees = require_census(dataset, aggregate_dir)
    n_trees = require_all_reach_final(dataset, aggregate_dir)
    refuse_stale(table_dir(aggregate_dir, selection), TABLE_OUTPUTS)
    return {"identity": identity, "trees": trees, "n_trees": n_trees}


def run_table(
    dataset: HorizontalDataset,
    aggregate_dir,
    index_roots: np.ndarray,
    index_forest_ids: np.ndarray,
    index_files: Dict,
    selection: Optional[np.ndarray] = None,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
    prepared: Optional[Dict] = None,
) -> Dict:
    """Build the decided table and write it, its record and the summary.

    ``index_roots`` and ``index_forest_ids`` are the index files' tree roots
    (ascending) and their forest ids; ``index_files`` is the record of the
    files (paths, sizes and md5) the table record carries. ``prepared`` is
    :func:`prepare_table`'s result when the caller has already run it (before
    loading the index files); otherwise it is run here.

    Raises:
        ConverterError: on any :func:`prepare_table` refusal, index files that
            do not describe this census, a refusal of the construction, or a
            table invariant; each before the output directory is touched.
    """
    inputs = prepared if prepared is not None else prepare_table(dataset, aggregate_dir, selection)
    memory = {"after_loading_index_files": peak_rss_bytes()}
    meter = ReadMeter()
    roots = load_roots(aggregate_dir)
    tree_forest = load_array(trees_dir(aggregate_dir) / "tree_forest.npy")
    tree_totals = load_array(trees_dir(aggregate_dir) / "tree_totals.npy", mmap=True)
    forest_ids = dataset.forest_ids()
    index = check_index(index_roots, index_forest_ids, roots, tree_forest, forest_ids, block_rows)
    if index["verdict"] != "pass":
        raise ConverterError("{}; no cut table is written".format(index["message"]))

    log("table: z = 0 FoF groups of snapshot {}".format(dataset.snapshots[-1]))
    table = decided_table(
        dataset,
        aggregate_dir,
        roots,
        tree_forest,
        tree_totals,
        forest_ids,
        selection,
        block_rows,
        meter,
    )
    del tree_forest
    memory["after_pieces"] = peak_rss_bytes()
    log(
        "table: {} split forest(s), {} piece(s) ({} fresh)".format(
            table.split.size, table.n_pieces, table.n_fresh
        )
    )
    peak, peak_snapshot = piece_peaks(dataset, aggregate_dir, table, log)
    memory["after_peaks"] = peak_rss_bytes()

    ids = table_ids(index_forest_ids, table.local, table.pieces["id"][table.tree_piece])
    checked = check_table(index_roots, ids, index_roots, index_forest_ids, np.asarray(tree_totals))
    out = begin(table_dir(aggregate_dir, selection))
    path = out / "forests.list"
    md5, table_bytes = write_table(path, index_roots, ids)
    del ids
    memory["after_table"] = peak_rss_bytes()
    log("table: {} written, md5 {}".format(path, md5))
    last = dataset.snapshots[-1]
    scope = scope_record(selection, forest_ids)
    digest = assignment_digest(table, roots)
    record_table = {
        "path": str(path.resolve()),
        "md5": md5,
        "bytes": table_bytes,
        "rows": int(index_roots.size),
    }
    rule = {
        "text": RULE_TEXT if selection is None else RESTRICTED_RULE_TEXT,
        "final_snapshot": int(last),
        "final_scale_factor": dataset.scale_factors[last],
        "equivalence": "the partition that cuts every co-membership ended before the final "
        "snapshot and keeps every one present at it",
        **scope,
    }
    header = {
        "dataset": inputs["identity"],
        "index_files": index_files,
        "rule": rule,
        "forests": {
            "split": int(table.split.size),
            "unchanged": int(dataset.n_forests_total - table.split.size),
            "pieces_listed": "every piece of every split forest; every other forest keeps its id "
            "and is not listed",
        },
        "fresh_id_floor": table.fresh_id_floor,
        "assignment_sha256": digest,
        "table": record_table,
    }
    record_bytes = write_table_record(
        out / "record.json",
        header,
        dict(table.pieces, peak_occupancy=peak, peak_snapshot=peak_snapshot),
    )
    memory["after_record"] = peak_rss_bytes()
    pieces = table.pieces
    by_halos = np.lexsort((pieces["smallest_root_id"], -pieces["halos"]))[:N_LARGEST]
    by_peak = np.lexsort((pieces["smallest_root_id"], -peak))[:N_LARGEST]
    summary = {
        "dataset": inputs["identity"],
        "dataset_dir": str(dataset.directory.resolve()),
        **scope,
        "rule": rule,
        "index_files": index_files,
        "root_correspondence": inputs["trees"]["root_correspondence"]["verdict"],
        "index_check": index,
        "checks": {
            "trees_ending_before_the_final_snapshot": 0,
            "final_slab_central_references": "validated: in range, self-central, in the "
            "member's original forest",
            "piece_count_equals_z0_centrals": "asserted for every split forest",
            "table_invariants": checked,
        },
        "groups": group_counts(table, dataset.n_halos[last], forest_ids),
        "pieces": {
            "count": table.n_pieces,
            "fresh": table.n_fresh,
            "single_tree": int(np.count_nonzero(pieces["trees"] == 1)),
            "largest": [piece_entry(table, int(at), peak, peak_snapshot) for at in by_halos],
            "highest_peak": [piece_entry(table, int(at), peak, peak_snapshot) for at in by_peak],
        },
        "fresh_id_floor": {
            "value": table.fresh_id_floor,
            "index_max_forest_id": int(index_forest_ids.max()) if index_forest_ids.size else 0,
            "dataset_max_forest_id": int(forest_ids.max()) if forest_ids.size else 0,
            "note": "fresh piece ids start at value + 1, the dataset sidecar's largest forest "
            "id, which the passed index check makes the index files' largest too",
        },
        "assignment_sha256": digest,
        "table": record_table,
        "io": meter.record(
            "(link width + 8 B) x n_halos(final) of FirstHaloInFOFgroup and ForestIndex; link "
            "width 4 B in version 2, 8 B in version 3"
        ),
        "memory": {
            "peak_rss_bytes_after": memory,
            "note": "the process's peak resident set (ru_maxrss) after each phase; the module "
            "docstring states each phase's formula",
        },
        "aggregates": {
            "table": {
                "formula": "one forests.list row per catalogue tree, '<root id> <forest id>\\n', "
                "after a 21 B header (about 24 B per tree at Shin-Uchuu's id widths)",
                "bytes": table_bytes,
            },
            "record": {
                "formula": "a header of a few kilobytes plus, per piece of a split forest, the "
                "sum over its six columns of (decimal digits + 1) bytes (34.2 B per piece measured "
                "at Shin-Uchuu's id widths); written in chunks of 2^16 pieces: 8 B per piece "
                "resident and one chunk, about 7 MB",
                "pieces": table.n_pieces,
                "bytes": record_bytes,
            },
        },
    }
    write_json(out / SUMMARY_NAME, summary)
    return summary
