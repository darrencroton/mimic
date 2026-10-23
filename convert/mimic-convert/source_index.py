"""The Consistent-Trees ASCII index files: ``forests.list`` and ``locations.dat``.

``forests.list`` maps every tree root id to its forest id (``#TreeRootID
ForestID``); ``locations.dat`` maps every tree root id to the file holding its
``#tree`` block and the byte offset of the block's first data row
(``#TreeRootID FileID Offset Filename``). Both are streamed by the loaders defined here
(:func:`load_forests_list`, :func:`load_locations`) and joined into one table sorted by tree
root, which :class:`SourceIndex` holds as aligned int64 arrays.

The two files must list exactly the same tree roots, each once; a root present
in one and not the other is refused, as is a duplicate.

:meth:`SourceIndex.forest_table` derives, per forest in ascending forest id
(which is the ``ForestIndex`` order the ASCII route enumerates,
``scatter.py`` ``ForestMap.unique_forest_ids``): the files holding its trees
(sorted, as a CSR pair of arrays), its tree count, and -- for a forest held in
one file -- that file's ``FileID`` and the forest's unit ordinal in it. The unit ordinal is the dense rank of the forest among that file's
forests ordered by the file position of their first ``#tree`` marker, exactly
as ``ctrees_parser.plan_source_units`` numbers units (with markers F, F, G, F
in one file, F is unit 0 and G unit 1). A forest spanning files carries -1/-1,
as the version 3 sidecar does (``HORIZONTAL-HDF5-FORMAT.md``, V3 Forest
Sidecar).

``ForestTable.file_id`` is the ``locations.dat`` ``FileID``, **not** the
sidecar's ``SourceFileOrdinal``. The converter numbers tree files in the order
they are given to it, and on real Shin-Uchuu data a lexically sorted file list
differs from ``FileID`` order (``tree_0_0_10.dat`` is ``FileID`` 10 but sorts
before ``tree_0_0_2.dat``). A caller comparing with a sidecar must map each
``FileID`` through that conversion's tree-file order
(:func:`rewriter.inputs.sidecar_file_ordinals` derives it from a version 3
sidecar's own ordinals, and refuses an inconsistent mapping); this module
offers no such mapping. The unit ordinal needs no mapping: it is a rank within
one file. :meth:`SourceIndex.marker_ordinals` gives the matching per-tree rank
of each ``#tree`` marker within its file.

Memory (measured with ``tracemalloc``), with ``n_trees`` the index's trees,
``n_forests`` its forests and ``n_pairs`` its distinct (file, forest) pairs
(Shin-Uchuu: 315,004,242 trees, 166,547,771 forests, about 320 million pairs):

- the joined table keeps **32 B x n_trees** (root, forest, file id and
  offset, int64), about 10.1 GB;
- :meth:`SourceIndex.load` peaks at about **79 B x n_trees** (about 24.9 GB),
  most of it the shared loaders' parse buffers;
- :meth:`SourceIndex.forest_table` returns **40 B x n_forests + 8 B x n_pairs**
  (five int64 arrays per forest plus the CSR file list), about 9.2 GB at
  Shin-Uchuu. While it runs, the total it holds above the joined table --
  its workspace and that result together -- peaks at about **57 B x n_trees**
  at Shin-Uchuu's ratio (about 18.0 GB) and about **81 B x n_trees** when
  forests are as many as trees (every forest one tree, so n_forests = n_pairs
  = n_trees), the measured worst case;
- :meth:`SourceIndex.marker_ordinals` holds a transient of about **33 B x
  n_trees** (the sort order, the sorted file ids, a one-byte run mask, the
  ranks and the result) and returns 8 B x n_trees.
"""

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from census.common import examples  # noqa: E402
from errors import ConverterError  # noqa: E402

#: Rows accumulated in Python lists before flushing into numpy arrays.
_PARSE_BLOCK_ROWS = 1_000_000


# ---------------------------------------------------------------------------
# Index-file loaders (streaming, numpy-only)
# ---------------------------------------------------------------------------


def load_forests_list(path) -> Tuple[np.ndarray, np.ndarray]:
    """Stream ``forests.list`` into ``(tree_root_id, forest_id)`` int64 arrays."""
    path = Path(path)
    root_blocks: List[np.ndarray] = []
    forest_blocks: List[np.ndarray] = []
    roots: List[int] = []
    forests: List[int] = []

    def flush() -> None:
        if roots:
            root_blocks.append(np.asarray(roots, dtype=np.int64))
            forest_blocks.append(np.asarray(forests, dtype=np.int64))
            del roots[:]
            del forests[:]

    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            if not raw.strip() or raw[:1] == b"#":
                continue
            parts = raw.split()
            if len(parts) != 2:
                raise ConverterError(
                    "{}:{}: expected 'TreeRootID ForestID', got: {!r}".format(
                        path, lineno, raw[:80]
                    )
                )
            try:
                roots.append(int(parts[0]))
                forests.append(int(parts[1]))
            except ValueError as exc:
                raise ConverterError(
                    "{}:{}: non-integer forests.list row: {!r}".format(path, lineno, raw[:80])
                ) from exc
            if len(roots) >= _PARSE_BLOCK_ROWS:
                flush()
    flush()
    if not root_blocks:
        raise ConverterError("{}: no forest rows found".format(path))
    return np.concatenate(root_blocks), np.concatenate(forest_blocks)


def load_locations(path) -> Tuple[np.ndarray, np.ndarray, np.ndarray, Dict[int, str]]:
    """Stream ``locations.dat`` into ``(tree_root_id, file_id, offset)`` plus the
    ``file_id -> filename`` map.

    The filename is checked on every row rather than only on a file id's first
    appearance: a file id that changes name mid-file is corrupt input, and it
    would silently mis-target the extractor.
    """
    path = Path(path)
    root_blocks: List[np.ndarray] = []
    file_blocks: List[np.ndarray] = []
    offset_blocks: List[np.ndarray] = []
    roots: List[int] = []
    files: List[int] = []
    offsets: List[int] = []
    filemap: Dict[int, str] = {}

    def flush() -> None:
        if roots:
            root_blocks.append(np.asarray(roots, dtype=np.int64))
            file_blocks.append(np.asarray(files, dtype=np.int32))
            offset_blocks.append(np.asarray(offsets, dtype=np.int64))
            del roots[:]
            del files[:]
            del offsets[:]

    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            if not raw.strip() or raw[:1] == b"#":
                continue
            parts = raw.split()
            if len(parts) != 4:
                raise ConverterError(
                    "{}:{}: expected 'TreeRootID FileID Offset Filename', got: {!r}".format(
                        path, lineno, raw[:80]
                    )
                )
            try:
                root = int(parts[0])
                file_id = int(parts[1])
                offset = int(parts[2])
            except ValueError as exc:
                raise ConverterError(
                    "{}:{}: non-integer locations.dat row: {!r}".format(path, lineno, raw[:80])
                ) from exc
            if file_id < 0:
                raise ConverterError("{}:{}: negative FileID {}".format(path, lineno, file_id))
            if offset < 0:
                raise ConverterError("{}:{}: negative offset {}".format(path, lineno, offset))
            name = parts[3].decode("utf-8", "replace")
            known = filemap.get(file_id)
            if known is None:
                filemap[file_id] = name
            elif known != name:
                raise ConverterError(
                    "{}:{}: FileID {} maps to both {!r} and {!r}".format(
                        path, lineno, file_id, known, name
                    )
                )
            roots.append(root)
            files.append(file_id)
            offsets.append(offset)
            if len(roots) >= _PARSE_BLOCK_ROWS:
                flush()
    flush()
    if not root_blocks:
        raise ConverterError("{}: no location rows found".format(path))
    return (
        np.concatenate(root_blocks),
        np.concatenate(file_blocks),
        np.concatenate(offset_blocks),
        filemap,
    )


def _sorted_unique(values: np.ndarray, what: str) -> np.ndarray:
    """``values``' ascending order (stable), refusing any duplicate."""
    order = np.argsort(values, kind="stable")
    ordered = values[order]
    if ordered.size > 1:
        repeated = np.nonzero(ordered[1:] == ordered[:-1])[0]
        if repeated.size:
            raise ConverterError(
                "{}: {} tree root id(s) listed more than once; examples: {}".format(
                    what, repeated.size, examples(ordered[repeated])
                )
            )
    return order


@dataclass
class ForestTable:
    """Per forest, in ascending forest id.

    ``file_starts``/``files`` are a CSR pair: forest ``i``'s files are
    ``files[file_starts[i]:file_starts[i + 1]]``, ascending ``FileID``.
    """

    forest_ids: np.ndarray  # int64, ascending
    n_trees: np.ndarray  # int64
    file_starts: np.ndarray  # int64, length n_forests + 1
    files: np.ndarray  # int64
    file_id: np.ndarray  # int64 locations.dat FileID (not SourceFileOrdinal); -1 if spanning
    unit_ordinal: np.ndarray  # int64; -1 for a forest spanning files


@dataclass
class SourceIndex:
    """``forests.list`` joined with ``locations.dat``, sorted by tree root."""

    tree_roots: np.ndarray  # int64, strictly ascending
    forest_ids: np.ndarray  # int64, aligned with tree_roots
    file_ids: np.ndarray  # int64, aligned with tree_roots (locations.dat FileID)
    offsets: np.ndarray  # int64, aligned (first data row of the #tree block)
    filenames: Dict[int, str]  # FileID -> file name as locations.dat records it

    @classmethod
    def load(cls, forests_list, locations) -> "SourceIndex":
        """Parse both index files and join them on the tree root id.

        Raises:
            ConverterError: on a malformed row, a duplicated root in either
                file, or a root listed by one file and not the other.
        """
        forests_list, locations = Path(forests_list), Path(locations)
        # one file at a time, each sorted and its unsorted arrays released
        # before the next is parsed, which bounds the load's transient peak
        list_roots, list_forests = load_forests_list(forests_list)
        order = _sorted_unique(list_roots, str(forests_list))
        roots = list_roots[order]
        del list_roots
        forest_ids = list_forests[order]
        del list_forests, order

        loc_roots, loc_files, loc_offsets, filenames = load_locations(locations)
        order = _sorted_unique(loc_roots, str(locations))
        loc_sorted = loc_roots[order]
        del loc_roots
        if roots.size != loc_sorted.size or not np.array_equal(roots, loc_sorted):
            only_list = np.setdiff1d(roots, loc_sorted, assume_unique=True)
            only_loc = np.setdiff1d(loc_sorted, roots, assume_unique=True)
            raise ConverterError(
                "index files disagree: {} tree root(s) only in {} (e.g. {}), {} only in {} "
                "(e.g. {})".format(
                    only_list.size,
                    forests_list,
                    examples(only_list),
                    only_loc.size,
                    locations,
                    examples(only_loc),
                )
            )
        del loc_sorted
        file_ids = loc_files[order].astype(np.int64)
        del loc_files
        offsets = loc_offsets[order].astype(np.int64, copy=False)
        del loc_offsets, order
        return cls(
            tree_roots=roots.astype(np.int64, copy=False),
            forest_ids=forest_ids.astype(np.int64, copy=False),
            file_ids=file_ids,
            offsets=offsets,
            filenames=dict(filenames),
        )

    @property
    def n_trees(self) -> int:
        return int(self.tree_roots.size)

    def find(self, roots: np.ndarray) -> np.ndarray:
        """Position of each root in :attr:`tree_roots`, or -1 where absent."""
        roots = np.asarray(roots, dtype=np.int64)
        if self.tree_roots.size == 0:
            return np.full(roots.shape, -1, dtype=np.int64)
        position = np.searchsorted(self.tree_roots, roots)
        clipped = np.minimum(position, self.tree_roots.size - 1)
        return np.where(self.tree_roots[clipped] == roots, clipped, -1).astype(np.int64)

    def marker_ordinals(self) -> np.ndarray:
        """Per tree (aligned with :attr:`tree_roots`), the dense rank of its
        ``#tree`` marker within its file by byte offset: the order
        ``ctrees_parser.plan_source_units`` encounters the file's trees.
        Transient about 33 B per tree (module docstring)."""
        by_marker = np.lexsort((self.offsets, self.file_ids))
        files = self.file_ids[by_marker]
        file_start = np.ones(files.size, dtype=bool)
        np.not_equal(files[1:], files[:-1], out=file_start[1:])
        del files
        rank = np.arange(file_start.size, dtype=np.int64)
        rank -= np.maximum.accumulate(np.where(file_start, rank, 0))
        del file_start
        ordinals = np.empty(rank.size, dtype=np.int64)
        ordinals[by_marker] = rank
        return ordinals

    def forest_table(self) -> ForestTable:
        """Per-forest files, tree counts and sidecar ordinals (module docstring).

        Three phases, each releasing its workspace before the next: per tree,
        the (file, forest) pairs and their first-marker offsets; per pair, the
        unit ordinal within the file; per forest, the CSR file sets and the
        ordinals of the forests held in one file.
        """
        # phase 1, per tree: one entry per (file, forest) pair, with its tree
        # count and the offset of its first marker (offsets ascend within a pair)
        by_file = np.lexsort((self.offsets, self.forest_ids, self.file_ids))
        n_trees = by_file.size
        pair_start = np.ones(n_trees, dtype=bool)
        files = self.file_ids[by_file]
        np.not_equal(files[1:], files[:-1], out=pair_start[1:])
        forests = self.forest_ids[by_file]
        pair_start[1:] |= forests[1:] != forests[:-1]
        first = np.flatnonzero(pair_start)
        del pair_start
        pair_file = files[first]
        del files
        pair_forest = forests[first]
        del forests
        pair_first = self.offsets[by_file[first]]
        del by_file
        pair_trees = np.diff(np.append(first, n_trees))
        del first

        # phase 2, per pair: unit ordinal = dense rank of the pair within its
        # file by first-marker offset
        by_marker = np.lexsort((pair_first, pair_file))
        del pair_first
        marker_file = pair_file[by_marker]
        file_start = np.ones(marker_file.size, dtype=bool)
        np.not_equal(marker_file[1:], marker_file[:-1], out=file_start[1:])
        del marker_file
        rank = np.arange(file_start.size, dtype=np.int64)
        rank -= np.maximum.accumulate(np.where(file_start, rank, 0))
        del file_start
        pair_unit = np.empty(rank.size, dtype=np.int64)
        pair_unit[by_marker] = rank
        del by_marker, rank

        # phase 3, per forest: its pairs in ascending file (pair_forest is then
        # sorted, so each forest's pairs are one run)
        by_forest = np.lexsort((pair_file, pair_forest))
        pair_forest = pair_forest[by_forest]
        run_start = np.ones(pair_forest.size, dtype=bool)
        np.not_equal(pair_forest[1:], pair_forest[:-1], out=run_start[1:])
        first_pair = np.flatnonzero(run_start)
        del run_start
        forest_ids = pair_forest[first_pair]
        del pair_forest
        n_files = np.diff(np.append(first_pair, by_forest.size))
        pair_trees = pair_trees[by_forest]
        tree_counts = (
            np.add.reduceat(pair_trees, first_pair)
            if first_pair.size
            else np.zeros(0, dtype=np.int64)
        )
        del pair_trees
        pair_file = pair_file[by_forest]
        pair_unit = pair_unit[by_forest]
        del by_forest
        file_starts = np.zeros(forest_ids.size + 1, dtype=np.int64)
        np.cumsum(n_files, out=file_starts[1:])
        single = n_files == 1
        file_id = np.where(single, pair_file[first_pair], -1).astype(np.int64)
        unit_ordinal = np.where(single, pair_unit[first_pair], -1).astype(np.int64)
        return ForestTable(
            forest_ids=forest_ids.astype(np.int64, copy=False),
            n_trees=tree_counts.astype(np.int64, copy=False),
            file_starts=file_starts,
            files=pair_file.astype(np.int64, copy=False),
            file_id=file_id,
            unit_ordinal=unit_ordinal,
        )
