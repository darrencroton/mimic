"""The cut forest table: F6's naming, its invariants, and its record.

A **cut forest table** is a file in ``forests.list`` shape (``#TreeRootID
ForestID`` then one ``<tree root id> <forest id>`` row per tree) that maps
every tree root of the catalogue's index files to the forest it belongs to
after a cut. Its pieces come from a rule's components (``census/rules.py``):
each component of a named forest's co-membership graph is one **piece**.

Naming (:func:`name_pieces`), as the plan's frozen decision F6 binds it:

- a piece's size is its total halo count over all snapshots; ties are broken
  by the smallest tree root id;
- of a forest's pieces exactly one, the largest, keeps the original forest id,
  so a forest the rule does not cut keeps its id and is unchanged;
- every other piece receives a fresh id above the catalogue's maximum forest
  id, unique across the table, assigned in descending piece size with the same
  tie rule (over all cut forests together), so that every untouched forest
  keeps its ``ForestIndex`` and the fresh pieces enumerate after every original
  forest.

Invariants (:func:`check_table`), checked when a table is written and when one
is read (:func:`read_cut_table`), each refused with a ``ConverterError``:

1. the table parses as ``forests.list`` (a malformed row is refused);
2. every tree root of the index files appears exactly once, and no other root;
3. a piece (the trees sharing one id) holds trees of exactly one original
   forest;
4. an id at or below the catalogue's maximum is its trees' original forest id
   (a fresh id at or below the maximum is refused);
5. every original forest keeps exactly one piece with its own id;
6. the piece keeping a forest's id is its largest by the trees' halo totals
   (ties by the smallest root id), and fresh ids ascend in descending piece
   size with the same tie rule. The totals are a required input of both the
   check and the reader, so no table is accepted without this invariant.

A complete table holds a row per catalogue tree (about 7.5 GB of text at
Shin-Uchuu's 315,004,242 trees), so it is written only for the tables asked for
(the rehearsal's and the rule the owner chooses) and only when the ``trees``
root correspondence verdict passed; every exploratory rule keeps only its
compact per-tree assignment. Each table is accompanied by a JSON record
(:func:`write_table_record`): the input dataset identity, the index files'
md5 (computed once per run), the rule, the named forests, every piece (id,
original forest id, tree count, halo total, peak occupancy and its slab) and
the table's own md5. The per-piece columns are streamed to the file in chunks
of 2^16 pieces, never built as Python lists: the record costs about 16 B per
piece resident (the id order and one gathered chunk) and ``sum over the six
columns of (decimal digits + 1)`` bytes per piece on disk, about 40 B per
piece at Shin-Uchuu's id widths.

Resident memory while a table is checked and written, with ``n`` catalogue
trees: the index's 32 B x n, the table's ids and the trees' totals (16 B x n),
and the check's sort orders and per-piece arrays, about **50 B x n** at their
peak (measured with ``tracemalloc``): about 98 B x n in all, 31 GB at
Shin-Uchuu's 315,004,242 trees. Writing formats rows in chunks of 2^20.
"""

import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from errors import ConverterError  # noqa: E402
from subset import SubsetError, load_forests_list  # noqa: E402

#: The table's header line, as ``forests.list`` carries it.
TABLE_HEADER = "#TreeRootID ForestID\n"

#: Rows formatted per write.
WRITE_ROWS = 1 << 20

#: Bytes hashed per read.
HASH_BYTES = 8 << 20

#: The per-piece columns of a table record, in order.
RECORD_COLUMNS = ("id", "forest_id", "trees", "halos", "peak_occupancy", "peak_snapshot")

#: Pieces formatted per write of a record column.
RECORD_CHUNK = 1 << 16

_INT64_MAX = int(np.iinfo(np.int64).max)


def _examples(values, limit: int = 5) -> list:
    return np.asarray(values)[:limit].tolist()


def md5_file(path) -> str:
    """The md5 of a file, read in bounded chunks."""
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(HASH_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ---- naming ------------------------------------------------------------------


def name_pieces(
    components: np.ndarray,
    local_forest: np.ndarray,
    local_totals: np.ndarray,
    local_roots: np.ndarray,
    forest_ids: np.ndarray,
    catalogue_max: int,
) -> Dict[str, np.ndarray]:
    """Name the pieces of the named forests' components under F6.

    Args:
        components: each named-forest tree's component root (its smallest
            local index); local trees are in ascending root id.
        local_forest: each tree's ``ForestIndex``.
        local_totals: each tree's halo total over all snapshots.
        local_roots: each tree's root id (ascending).
        forest_ids: the catalogue forest id of every ``ForestIndex``.
        catalogue_max: the largest forest id of the catalogue.

    Returns:
        ``piece`` (each tree's piece index) and, per piece in ascending
        component root: ``root`` (local index of its smallest tree),
        ``forest_index``, ``forest_id`` (the original), ``id`` (the piece's
        id), ``trees``, ``halos``, ``smallest_root_id`` and ``keeps_id``.

    Raises:
        ConverterError: on a component joining trees of two forests, or fresh
            ids beyond int64.
    """
    roots, piece, trees = np.unique(components, return_inverse=True, return_counts=True)
    piece = piece.reshape(-1)
    halos = np.zeros(roots.size, dtype=np.int64)
    np.add.at(halos, piece, np.asarray(local_totals, dtype=np.int64))
    forest = np.asarray(local_forest, dtype=np.int64)[roots]
    if np.any(np.asarray(local_forest, dtype=np.int64) != forest[piece]):
        raise ConverterError("a component joins trees of two forests")
    smallest = np.asarray(local_roots, dtype=np.int64)[roots]
    # rank within each forest: largest first, ties by the smallest root id
    ranked = np.lexsort((smallest, -halos, forest))
    first_of_forest = np.r_[True, forest[ranked][1:] != forest[ranked][:-1]]
    keeps = np.zeros(roots.size, dtype=bool)
    keeps[ranked[first_of_forest]] = True
    ids = np.asarray(forest_ids, dtype=np.int64)[forest]
    fresh = np.flatnonzero(~keeps)
    if fresh.size:
        if int(catalogue_max) > _INT64_MAX - fresh.size:
            raise ConverterError(
                "{} fresh forest ids above {} exceed int64".format(fresh.size, catalogue_max)
            )
        order = fresh[np.lexsort((smallest[fresh], -halos[fresh]))]
        ids[order] = int(catalogue_max) + 1 + np.arange(order.size, dtype=np.int64)
    return {
        "piece": piece.astype(np.int64),
        "root": roots.astype(np.int64),
        "forest_index": forest,
        "forest_id": np.asarray(forest_ids, dtype=np.int64)[forest],
        "id": ids,
        "trees": trees.astype(np.int64),
        "halos": halos,
        "smallest_root_id": smallest,
        "keeps_id": keeps,
    }


def table_ids(
    index_forest_ids: np.ndarray, forest_trees: np.ndarray, local_piece_ids: np.ndarray
) -> np.ndarray:
    """The cut table's forest ids aligned with the index's ascending roots:
    the original ids, with each named-forest tree's replaced by its piece's.
    The census root ordinals are the index positions when the root
    correspondence passed."""
    ids = np.array(index_forest_ids, dtype=np.int64, copy=True)
    ids[np.asarray(forest_trees, dtype=np.int64)] = local_piece_ids
    return ids


# ---- invariants ----------------------------------------------------------------


def check_table(
    table_roots: np.ndarray,
    table_forest_ids: np.ndarray,
    index_roots: np.ndarray,
    index_forest_ids: np.ndarray,
    tree_totals: np.ndarray,
    what: str = "cut table",
) -> Dict:
    """Check a cut table against the catalogue's index (module docstring,
    invariants 2 to 6).

    Args:
        table_roots, table_forest_ids: the table's rows, in any order.
        index_roots: the index files' tree roots, ascending and unique.
        index_forest_ids: their original forest ids, aligned.
        tree_totals: each index tree's halo total, aligned (required: invariant
            6 depends on it).

    Returns:
        ``{"trees", "pieces", "cut_forests", "fresh_pieces"}``.

    Raises:
        ConverterError: naming the first invariant violated, with examples.
    """
    table_roots = np.asarray(table_roots, dtype=np.int64)
    table_forest_ids = np.asarray(table_forest_ids, dtype=np.int64)
    order = np.argsort(table_roots, kind="stable")
    roots = table_roots[order]
    if roots.size > 1:
        repeated = np.flatnonzero(roots[1:] == roots[:-1])
        if repeated.size:
            raise ConverterError(
                "{}: {} tree root(s) listed more than once; examples: {}".format(
                    what, repeated.size, _examples(roots[repeated])
                )
            )
    if roots.size != index_roots.size or not np.array_equal(roots, index_roots):
        missing = np.setdiff1d(index_roots, roots, assume_unique=True)
        extra = np.setdiff1d(roots, index_roots, assume_unique=True)
        raise ConverterError(
            "{}: {} tree root(s) of the index files missing (e.g. {}), {} not in the index "
            "files (e.g. {})".format(
                what, missing.size, _examples(missing), extra.size, _examples(extra)
            )
        )
    del roots
    ids = table_forest_ids[order]  # aligned with index_roots
    del order
    original = np.asarray(index_forest_ids, dtype=np.int64)
    catalogue_max = int(original.max()) if original.size else 0

    low_foreign = (ids <= catalogue_max) & (ids != original)
    if low_foreign.any():
        at = np.flatnonzero(low_foreign)
        raise ConverterError(
            "{}: {} tree(s) carry an id at or below the catalogue's maximum {} that is not their "
            "forest's own (e.g. root {} of forest {} given {})".format(
                what,
                at.size,
                catalogue_max,
                int(index_roots[at[0]]),
                int(original[at[0]]),
                int(ids[at[0]]),
            )
        )
    by_piece = np.lexsort((original, ids))
    piece_ids = ids[by_piece]
    starts = np.flatnonzero(np.r_[True, piece_ids[1:] != piece_ids[:-1]])
    ends = np.r_[starts[1:], piece_ids.size]
    piece_forest = original[by_piece][starts]
    mixed = original[by_piece][ends - 1] != piece_forest
    if mixed.any():
        raise ConverterError(
            "{}: {} piece(s) mix trees of several forests (e.g. id {})".format(
                what, int(np.count_nonzero(mixed)), int(piece_ids[starts[np.argmax(mixed)]])
            )
        )
    piece_id = piece_ids[starts]
    keeps = piece_id == piece_forest
    forests = np.unique(original)
    keeping = np.unique(piece_forest[keeps])
    if keeping.size != forests.size:
        lost = np.setdiff1d(forests, keeping, assume_unique=True)
        raise ConverterError(
            "{}: {} forest(s) keep no piece with their own id (e.g. {})".format(
                what, lost.size, _examples(lost)
            )
        )
    record = {
        "trees": int(ids.size),
        "pieces": int(piece_id.size),
        "cut_forests": int(np.unique(piece_forest[~keeps]).size),
        "fresh_pieces": int(np.count_nonzero(~keeps)),
    }
    halos = np.add.reduceat(np.asarray(tree_totals, dtype=np.int64)[by_piece], starts)
    smallest = np.minimum.reduceat(np.asarray(index_roots, dtype=np.int64)[by_piece], starts)
    del by_piece
    # per forest, its pieces ranked largest first (ties by the smallest root id)
    ranked = np.lexsort((smallest, -halos, piece_forest))
    first = np.r_[True, piece_forest[ranked][1:] != piece_forest[ranked][:-1]]
    wrong = ranked[first][~keeps[ranked[first]]]
    if wrong.size:
        raise ConverterError(
            "{}: {} forest(s) whose id is not kept by their largest piece (e.g. forest {})".format(
                what, wrong.size, int(piece_forest[wrong[0]])
            )
        )
    fresh = np.flatnonzero(~keeps)  # ascending fresh id
    if fresh.size > 1:
        size, root = halos[fresh], smallest[fresh]
        later_larger = (size[1:] > size[:-1]) | ((size[1:] == size[:-1]) & (root[1:] < root[:-1]))
        if later_larger.any():
            at = int(np.argmax(later_larger)) + 1
            raise ConverterError(
                "{}: fresh ids do not follow descending piece size (id {} holds {} halos after "
                "id {} with {})".format(
                    what,
                    int(piece_id[fresh[at]]),
                    int(size[at]),
                    int(piece_id[fresh[at - 1]]),
                    int(size[at - 1]),
                )
            )
    return record


# ---- writing and reading ---------------------------------------------------------


def write_table(path, roots: np.ndarray, forest_ids: np.ndarray) -> Tuple[str, int]:
    """Write a table in ``forests.list`` shape, atomically; returns its md5
    and size. The caller checks the invariants first (:func:`check_table`)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    digest = hashlib.md5()
    with open(tmp, "wb") as handle:
        header = TABLE_HEADER.encode("ascii")
        handle.write(header)
        digest.update(header)
        for start in range(0, roots.size, WRITE_ROWS):
            stop = min(start + WRITE_ROWS, roots.size)
            text = "".join(
                "{} {}\n".format(root, forest)
                for root, forest in zip(roots[start:stop].tolist(), forest_ids[start:stop].tolist())
            ).encode("ascii")
            handle.write(text)
            digest.update(text)
    os.replace(str(tmp), str(path))
    return digest.hexdigest(), path.stat().st_size


def read_cut_table(
    path,
    index_roots: np.ndarray,
    index_forest_ids: np.ndarray,
    tree_totals: np.ndarray,
) -> Tuple[np.ndarray, np.ndarray]:
    """Read a cut table and check every invariant against the index and the
    trees' halo totals (aligned with ``index_roots``).

    Returns:
        ``(roots, forest_ids)`` aligned with the ascending ``index_roots``.

    Raises:
        ConverterError: for a malformed table (a row that is not two integers,
            or an id outside int64) or any invariant violated.
    """
    try:
        roots, ids = load_forests_list(path)
    except (SubsetError, OverflowError) as exc:
        # an id beyond int64 overflows the shared loader's int64 arrays
        raise ConverterError("{}: malformed cut table ({})".format(path, exc)) from exc
    check_table(roots, ids, index_roots, index_forest_ids, tree_totals, what=str(path))
    order = np.argsort(roots, kind="stable")
    return roots[order], ids[order]


def write_table_record(path, header: Dict, pieces: Dict[str, np.ndarray]) -> int:
    """Write a table's JSON record atomically; returns its size.

    ``header`` holds the record's JSON-ready entries (dataset identity, index
    files, rule, forests, table); ``pieces`` holds the per-piece arrays named
    in :data:`RECORD_COLUMNS`, which are written as columns in ascending
    piece id, streamed in chunks of :data:`RECORD_CHUNK` (module docstring).
    """
    if "pieces" in header:
        raise ConverterError("a record's header may not carry its own pieces")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    order = np.argsort(np.asarray(pieces["id"]), kind="stable")
    with open(tmp, "w") as handle:
        handle.write("{\n")
        for key in sorted(header):
            handle.write(
                "  {}: {},\n".format(json.dumps(key), json.dumps(header[key], sort_keys=True))
            )
        handle.write('  "pieces": {\n')
        for at, name in enumerate(RECORD_COLUMNS):
            column = np.asarray(pieces[name])
            handle.write("    {}: [".format(json.dumps(name)))
            for start in range(0, order.size, RECORD_CHUNK):
                part = column[order[start : start + RECORD_CHUNK]].tolist()
                handle.write(("," if start else "") + ",".join(str(int(v)) for v in part))
            handle.write("]" + (",\n" if at + 1 < len(RECORD_COLUMNS) else "\n"))
        handle.write("  }\n}\n")
    os.replace(str(tmp), str(path))
    return path.stat().st_size
