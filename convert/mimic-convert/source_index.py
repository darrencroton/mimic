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
one file -- that file's ordinal (its ``FileID``) and the forest's unit ordinal
in it. The unit ordinal is the dense rank of the forest among that file's
forests ordered by the file position of their first ``#tree`` marker, exactly
as ``ctrees_parser.plan_source_units`` numbers units (with markers F, F, G, F
in one file, F is unit 0 and G unit 1). A forest spanning files carries -1/-1,
as the version 3 sidecar does (``HORIZONTAL-HDF5-FORMAT.md``, V3 Forest
Sidecar). The ``FileID`` is the converter's file ordinal when the tree files are
given to it in ``FileID`` order.

Memory: the joined table is 28 B per tree (root, forest, offset int64; file id
int32) plus the loaders' transient copies; about 9 GB for 3 x 10^8 trees.
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
    file_ordinal: np.ndarray  # int64; -1 for a forest spanning files
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
        list_roots, list_forests = load_forests_list(forests_list)
        loc_roots, loc_files, loc_offsets, filenames = load_locations(locations)

        list_order = _sorted_unique(list_roots, str(forests_list))
        loc_order = _sorted_unique(loc_roots, str(locations))
        roots = list_roots[list_order]
        loc_sorted = loc_roots[loc_order]
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
        return cls(
            tree_roots=roots.astype(np.int64, copy=False),
            forest_ids=list_forests[list_order].astype(np.int64, copy=False),
            file_ids=loc_files[loc_order].astype(np.int64),
            offsets=loc_offsets[loc_order].astype(np.int64, copy=False),
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
        """Per-forest files, tree counts and sidecar ordinals (module docstring)."""
        # one entry per (file, forest) pair, keyed by the forest's first marker in the file
        by_file = np.lexsort((self.offsets, self.forest_ids, self.file_ids))
        files = self.file_ids[by_file]
        forests = self.forest_ids[by_file]
        offsets = self.offsets[by_file]
        pair_start = np.ones(files.size, dtype=bool)
        pair_start[1:] = (files[1:] != files[:-1]) | (forests[1:] != forests[:-1])
        pair_file = files[pair_start]
        pair_forest = forests[pair_start]
        pair_first = offsets[pair_start]

        # unit ordinal: dense rank of the pair within its file by first-marker offset
        by_marker = np.lexsort((pair_first, pair_file))
        marker_file = pair_file[by_marker]
        file_start = np.ones(marker_file.size, dtype=bool)
        file_start[1:] = marker_file[1:] != marker_file[:-1]
        position = np.arange(marker_file.size, dtype=np.int64)
        group_base = np.maximum.accumulate(np.where(file_start, position, 0))
        pair_unit = np.empty(marker_file.size, dtype=np.int64)
        pair_unit[by_marker] = position - group_base

        # per forest: its pairs, ascending file
        by_forest = np.lexsort((pair_file, pair_forest))
        pair_file = pair_file[by_forest]
        pair_forest = pair_forest[by_forest]
        pair_unit = pair_unit[by_forest]
        forest_ids, first_pair, n_files = np.unique(
            pair_forest, return_index=True, return_counts=True
        )
        file_starts = np.zeros(forest_ids.size + 1, dtype=np.int64)
        file_starts[1:] = np.cumsum(n_files)
        single = n_files == 1
        file_ordinal = np.where(single, pair_file[first_pair], -1).astype(np.int64)
        unit_ordinal = np.where(single, pair_unit[first_pair], -1).astype(np.int64)

        tree_forests, n_trees = np.unique(self.forest_ids, return_counts=True)
        if not np.array_equal(tree_forests, forest_ids):  # pragma: no cover - same input set
            raise ConverterError("forest tables disagree on the forest set")
        return ForestTable(
            forest_ids=forest_ids.astype(np.int64, copy=False),
            n_trees=n_trees.astype(np.int64),
            file_starts=file_starts,
            files=pair_file.astype(np.int64, copy=False),
            file_ordinal=file_ordinal,
            unit_ordinal=unit_ordinal,
        )
