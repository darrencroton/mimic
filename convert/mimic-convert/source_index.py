"""The Consistent-Trees ASCII index files: ``forests.list`` and ``locations.dat``.

``forests.list`` maps every tree root id to its forest id (``#TreeRootID
ForestID``); ``locations.dat`` maps every tree root id to the file holding its
``#tree`` block and the byte offset of the block's first data row
(``#TreeRootID FileID Offset Filename``). Both are streamed by the loaders
``subset.py`` already owns (:func:`subset.load_forests_list`,
:func:`subset.load_locations`) and joined here into one table sorted by tree
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

``ForestTable.file_ordinal`` is the ``locations.dat`` ``FileID``, **not** the
sidecar's ``SourceFileOrdinal``. The converter numbers tree files in the order
they are given to it, and on real Shin-Uchuu data a lexically sorted file list
differs from ``FileID`` order (``tree_0_0_10.dat`` is ``FileID`` 10 but sorts
before ``tree_0_0_2.dat``). A caller comparing with a sidecar must map each
``FileID`` through that conversion's tree-file order; this module offers no such
mapping. The unit ordinal needs no mapping: it is a rank within one file.

Memory (measured with ``tracemalloc``), for Shin-Uchuu's 315,004,242 trees:

- the joined table keeps **32 B per tree** (root, forest, file id and offset,
  int64), about 10.1 GB;
- :meth:`SourceIndex.load` peaks at about **79 B per tree** (about 24.9 GB),
  most of it the shared loaders' parse buffers;
- :meth:`SourceIndex.forest_table` adds, on top of the table, at most about
  **57 B per tree** while it runs (about 18.0 GB; measured with as many
  (file, forest) pairs as trees, its worst case) and returns about 25 B per
  pair, which is at most 25 B per tree (about 7.9 GB).
"""

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from errors import ConverterError  # noqa: E402
from subset import load_forests_list, load_locations  # noqa: E402


def _examples(values: np.ndarray, limit: int = 5) -> list:
    return np.asarray(values)[:limit].tolist()


def _sorted_unique(values: np.ndarray, what: str) -> np.ndarray:
    """``values``' ascending order (stable), refusing any duplicate."""
    order = np.argsort(values, kind="stable")
    ordered = values[order]
    if ordered.size > 1:
        repeated = np.nonzero(ordered[1:] == ordered[:-1])[0]
        if repeated.size:
            raise ConverterError(
                "{}: {} tree root id(s) listed more than once; examples: {}".format(
                    what, repeated.size, _examples(ordered[repeated])
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
    file_ordinal: np.ndarray  # int64 locations.dat FileID (not SourceFileOrdinal); -1 if spanning
    unit_ordinal: np.ndarray  # int64; -1 for a forest spanning files

    def files_of(self, position: int) -> np.ndarray:
        """The sorted ``FileID`` set of the forest at ``position``."""
        return self.files[self.file_starts[position] : self.file_starts[position + 1]]


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
            ConverterError: on a duplicated root in either file, or a root
                listed by one file and not the other.
            subset.SubsetError: on a malformed row (from the shared loaders).
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
                    _examples(only_list),
                    only_loc.size,
                    locations,
                    _examples(only_loc),
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
        file_ordinal = np.where(single, pair_file[first_pair], -1).astype(np.int64)
        unit_ordinal = np.where(single, pair_unit[first_pair], -1).astype(np.int64)
        return ForestTable(
            forest_ids=forest_ids.astype(np.int64, copy=False),
            n_trees=tree_counts.astype(np.int64, copy=False),
            file_starts=file_starts,
            files=pair_file.astype(np.int64, copy=False),
            file_ordinal=file_ordinal,
            unit_ordinal=unit_ordinal,
        )
