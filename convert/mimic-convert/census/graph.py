"""The effective FoF co-membership graph between effective descendant trees.

**Definition.** Nodes are the effective descendant trees (``census/trees.py``)
of the forests named (default: the forest with the largest total). In every
slab, a FoF group's central is the row its members' ``FirstHaloInFOFgroup``
points to (a snapshot-local index), and a member whose tree label differs from
its central's joins the **undirected pair** (min, max) of the two root
ordinals. Groups are the effective (post-fix-up) ones the dataset records; the
raw ``pid``/``upid`` graph is not available without the source. A pair is
deduplicated within the slab, so it counts one snapshot however many members
join it and in whichever direction (a pair whose host switches between
snapshots is one pair), and carries the slab, the member count and the
members' ``Mvir`` sum. The mass column is ``M_Crit200``, which for a
Consistent-Trees-derived dataset is native float32 ``Mvir`` in Msun/h
(``HORIZONTAL-HDF5-FORMAT.md``); sums are float64, computed with
``np.add.reduceat`` over the members in row order. That is deterministic for a
fixed NumPy build and independent of the block size, but ``reduceat`` does not
promise strictly sequential addition, so the last bit of a sum may differ
between builds, and an ``m`` threshold placed exactly on a sum is not portable
across them.

**Edge lists.** One pass over the named forests' rows of every slab, with the
slab's labels mapped: ``ForestIndex`` is read in bounded blocks and
``FirstHaloInFOFgroup`` and ``M_Crit200`` only over each block's span of the
named forests' rows (for a version 3 dataset, exactly those rows when the
named forests are adjacent in ``ForestIndex``, else their enclosing span).
``graph`` is refused before it writes anything when the ``trees`` summary
records halos whose ``ForestIndex`` is not their tree's forest
(``forest_mismatch_halos``): such rows would be skipped and their pairs lost. Each slab's
pairs are written sorted by (lo, hi) as ``graph/slab_NNN_edges.npy``. The
per-slab lists are kept beside the merged records: they give the per-snapshot
promotions of a cut (``census/cut.py``). Bytes read per slab: ``8 x
n_halos(s)`` for ``ForestIndex`` plus ``(w_link + 4) x span(s)`` for
``FirstHaloInFOFgroup`` and ``M_Crit200``, where ``w_link`` is the link width
(4 B in version 2, 8 B in version 3) and ``span(s)`` the rows from the first to
the last named-forest row of each block (in version 3 the forests' rows when
they are adjacent, else their enclosing span). The bytes actually read are
measured and recorded in ``summary.json`` (``io``).

**Merged records.** The per-slab lists are merged by an external k-way merge
(:func:`merge_slab_edges`): each slab contributes a bounded window, every key at
or below the smallest window end is reduced, and the windows advance, so at
most ``block_rows`` entries are resident whatever the slab count. Each pair's
record is its distinct-snapshot count, halo count, mass sum (per-slab sums
added in slab order) and first and last slab, written as ``graph/pairs.npy``,
sorted by (lo, hi).

**Components.** The components of the complete graph (every pair kept) are a
census result, never assumed to be one: a forest the effective relations
already leave in several components is separable without severing anything,
and the summary says so. ``graph/components.npy`` holds each named-forest
tree's component, the local index of its smallest tree (``census/rules.py``).

Aggregates under ``<aggregate>/graph/`` (sizes also in ``summary.json``):

- ``slab_NNN_edges.npy`` (int32 lo, int32 hi, int32 members, float64 mass):
  **20 B per (slab, pair)**, ``20 x sum over slabs of the pairs present``,
  plus a 192 B header per file; at most 20 B per cross-tree member.
- ``pairs.npy`` (int32 lo, hi, snapshots, first, last; int64 halos; float64
  mass): **36 B per pair**, ``36 x distinct pairs`` plus a 256 B header; the
  distinct pairs are at most the per-slab pairs summed over slabs.
- ``forest_trees.npy`` (int32 root ordinals, ascending; position is the local
  tree index) and ``components.npy`` (int32): **8 B per named-forest tree**,
  about 0.84 GB for the super-forest's 104,845,278 trees.

Resident memory, with ``n`` the number of trees, ``t`` the named forests'
trees and ``x`` a slab's cross-tree members (measured with ``tracemalloc``):

- for the whole subcommand: the roots and the per-tree forest array (16 B x n,
  about 5.0 GB at Shin-Uchuu's 315,004,242 trees) and the named-forest tree
  list with its forests and totals (20 B x t, about 2.1 GB for the
  super-forest's 104,845,278 trees);
- per slab: the block's columns, label gathers and transients, about **100 B
  x block_rows** (0.4 GB at the default 2^22); and the slab's cross-tree
  contributions (int64 key and float32 mass, kept float32 until the
  reduction), about **40 B per member plus 27 B per distinct pair of the
  slab** at the reduction's peak (12 B per member while they accumulate).
  This per-slab accumulation is proportional to the named forests' rows in
  one slab (PM ruling DD10): ``x`` is at most the named forests' rows in the
  slab, so its worst case at Shin-Uchuu scale is the super-forest's peak
  occupancy of 333,663,215 rows at snapshot 31, about **22.4 GB** if every one
  were a cross-tree member of a distinct pair; the mandated per-slab edge list
  is of the same order (20 B per pair, at most one pair per member). The
  ``per_snapshot`` rows of ``summary.json`` record each slab's members and
  pairs, so the real figure follows from them;
- the merge, about **170 B per window entry**, ``block_rows`` entries in all
  (0.7 GB at the default); the components, 8 B x t plus about 40 B per edge of
  a batch of ``block_rows`` pairs (about 1.0 GB for the super-forest).

The subcommand's peak at Shin-Uchuu scale is therefore about 7.5 GB plus the
per-slab term: about **30 GB** in the worst case (every super-forest row of
snapshot 31 a cross-tree member of its own pair), and about 9 GB when a few per
cent of a slab's rows are. No array of a slab's length is held beyond the column block,
the mapped labels and the cross-tree contributions above, and no tree x
snapshot matrix.
"""

import os
import sys
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.aggregate import (  # noqa: E402
    SUMMARY_NAME,
    begin,
    bind,
    load_array,
    require_summary,
    save_array,
    slab_stem,
    write_json,
)
from census.rules import COMPLETE, Rule, component_count, union_find  # noqa: E402
from census.trees import load_labels, load_roots, trees_dir  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402

GRAPH_DIR = "graph"

#: The mass column: native float32 Mvir (Msun/h) for Consistent-Trees-derived datasets.
MASS_COLUMN = "M_Crit200"

#: One slab's pair: root ordinals (lo < hi), member count and members' mass sum.
EDGE_DTYPE = np.dtype([("lo", "<i4"), ("hi", "<i4"), ("halos", "<i4"), ("mass", "<f8")])

#: One merged pair record.
PAIR_DTYPE = np.dtype(
    [
        ("lo", "<i4"),
        ("hi", "<i4"),
        ("snapshots", "<i4"),
        ("first", "<i4"),
        ("last", "<i4"),
        ("halos", "<i8"),
        ("mass", "<f8"),
    ]
)

#: Number of largest components quoted per forest.
N_LARGEST = 10

_INT32_MAX = int(np.iinfo(np.int32).max)
_KEY_SHIFT = 31


def _quiet(_message: str) -> None:
    pass


def graph_dir(aggregate_dir) -> Path:
    return Path(aggregate_dir) / GRAPH_DIR


def edge_path(aggregate_dir, snap: int) -> Path:
    return graph_dir(aggregate_dir) / (slab_stem(snap) + "_edges.npy")


def load_slab_edges(aggregate_dir, snap: int, mmap: bool = True) -> np.ndarray:
    """One slab's pairs (:data:`EDGE_DTYPE`), sorted by (lo, hi)."""
    return load_array(edge_path(aggregate_dir, snap), mmap)


def load_pairs(aggregate_dir, mmap: bool = True) -> np.ndarray:
    """The merged pair records (:data:`PAIR_DTYPE`), sorted by (lo, hi)."""
    return load_array(graph_dir(aggregate_dir) / "pairs.npy", mmap)


def load_forest_trees(aggregate_dir) -> np.ndarray:
    """The named forests' root ordinals, ascending; position is the local index."""
    return load_array(graph_dir(aggregate_dir) / "forest_trees.npy")


def load_components(aggregate_dir) -> np.ndarray:
    """Each named-forest tree's complete-graph component (its smallest local index)."""
    return load_array(graph_dir(aggregate_dir) / "components.npy")


def pair_keys(lo: np.ndarray, hi: np.ndarray) -> np.ndarray:
    """One int64 per pair, ordered as (lo, hi): root ordinals are below 2^31."""
    return (np.asarray(lo, dtype=np.int64) << _KEY_SHIFT) | np.asarray(hi, dtype=np.int64)


def in_sorted(haystack: np.ndarray, needles: np.ndarray) -> np.ndarray:
    """Whether each needle occurs in the ascending ``haystack``."""
    if haystack.size == 0:
        return np.zeros(np.shape(needles), dtype=bool)
    position = np.minimum(np.searchsorted(haystack, needles), haystack.size - 1)
    return haystack[position] == needles


# ---- bounded reads of the named forests' rows ------------------------------------


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


def read_span(
    dataset: HorizontalDataset,
    snap: int,
    names: Sequence[str],
    start: int,
    stop: int,
    meter: Optional[ReadMeter] = None,
) -> Dict[str, np.ndarray]:
    """:meth:`HorizontalDataset.read_rows`, metered."""
    columns = dataset.read_rows(snap, names, start, stop) if names else {}
    if meter is not None:
        meter.add(columns)
    return columns


def iter_forest_rows(
    dataset: HorizontalDataset,
    snap: int,
    forests: np.ndarray,
    names: Sequence[str],
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
) -> Iterator[Tuple[np.ndarray, Dict[str, np.ndarray]]]:
    """Yield ``(rows, {name: values})`` for the rows of one slab whose
    ``ForestIndex`` is in ``forests`` (ascending), block by block: the
    ``ForestIndex`` block is read whole, the named columns only over the span
    of the block's matching rows, from the first to the last. In a version 3
    slab that span is exactly the named forests' rows only when the forests
    are adjacent in ``ForestIndex``; otherwise it is their enclosing span,
    which includes the rows of the forests between them, and in a version 2
    slab it can be the whole block. It is bounded by ``block_rows`` either way.
    ``ForestIndex`` is always among the values. Every byte read is added to
    ``meter`` when one is given."""
    forests = np.asarray(forests, dtype=np.int64)
    for start, block in dataset.iter_column(snap, "ForestIndex", block_rows):
        if meter is not None:
            meter.add({"ForestIndex": block})
        if forests.size == 1:
            picked = np.flatnonzero(block == forests[0])
        else:
            picked = np.flatnonzero(in_sorted(forests, block.astype(np.int64, copy=False)))
        if picked.size == 0:
            continue
        low, high = int(picked[0]), int(picked[-1]) + 1
        columns = read_span(dataset, snap, names, start + low, start + high, meter)
        values = {name: columns[name][picked - low] for name in names}
        values["ForestIndex"] = block[picked].astype(np.int64, copy=False)
        yield start + picked.astype(np.int64), values


def check_central_rows(central: np.ndarray, n_rows: int, path) -> np.ndarray:
    """``FirstHaloInFOFgroup`` values as int64, refusing one outside the slab."""
    central = central.astype(np.int64, copy=False)
    if central.size and (int(central.min()) < 0 or int(central.max()) >= n_rows):
        raise ConverterError(
            "{}: FirstHaloInFOFgroup outside [0, {}) (min {}, max {})".format(
                path, n_rows, int(central.min()), int(central.max())
            )
        )
    return central


def check_label_forests(
    tree_forest: np.ndarray,
    member_labels: np.ndarray,
    central_labels: np.ndarray,
    forest_index: np.ndarray,
    path,
) -> None:
    """Refuse a halo whose tree, or whose FoF central's tree, is not in the
    halo's own forest: the converter forbids both, and a pair across forests
    would put a tree in two forests' graphs."""
    for labels, what in ((member_labels, "tree"), (central_labels, "FoF central's tree")):
        wrong = tree_forest[labels] != forest_index
        if wrong.any():
            at = int(np.argmax(wrong))
            raise ConverterError(
                "{}: {} halo(s) whose {} lies in another forest (e.g. ForestIndex {}, tree "
                "forest {})".format(
                    path,
                    int(np.count_nonzero(wrong)),
                    what,
                    int(forest_index[at]),
                    int(tree_forest[labels[at]]),
                )
            )


# ---- per-slab edge lists ------------------------------------------------------


def reduce_contributions(key_parts: List[np.ndarray], mass_parts: List[np.ndarray]) -> np.ndarray:
    """Pairs from per-member ``(key, mass)`` contributions in row order: one
    :data:`EDGE_DTYPE` record per distinct key, ascending, its members counted
    and their masses (float32, widened here) summed in float64 in row order
    with ``np.add.reduceat``. The two part lists are emptied as they are
    concatenated, and each array is released as soon as its sorted copy
    exists, so the peak is about 35 B per member (module docstring)."""
    keys = drain(key_parts)
    mass = drain(mass_parts)
    if keys.size == 0:
        return np.zeros(0, dtype=EDGE_DTYPE)
    order = np.argsort(keys, kind="stable")
    keys = keys[order]
    mass = mass[order]
    del order
    starts = np.flatnonzero(np.r_[True, keys[1:] != keys[:-1]])
    edges = np.zeros(starts.size, dtype=EDGE_DTYPE)
    first = keys[starts]
    edges["lo"] = first >> _KEY_SHIFT
    edges["hi"] = first & ((1 << _KEY_SHIFT) - 1)
    edges["halos"] = np.diff(np.r_[starts, keys.size])
    del keys, first
    edges["mass"] = np.add.reduceat(mass.astype(np.float64), starts)
    return edges


def slab_edges(
    dataset: HorizontalDataset,
    snap: int,
    labels: np.ndarray,
    tree_forest: np.ndarray,
    forests: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
) -> Tuple[np.ndarray, int]:
    """One slab's pairs and its cross-tree member count (module docstring)."""
    path = dataset.snapshot_path(snap)
    n_rows = dataset.n_halos[snap]
    key_parts: List[np.ndarray] = []
    mass_parts: List[np.ndarray] = []
    for rows, values in iter_forest_rows(
        dataset, snap, forests, ("FirstHaloInFOFgroup", MASS_COLUMN), block_rows, meter
    ):
        central = check_central_rows(values["FirstHaloInFOFgroup"], n_rows, path)
        own = np.asarray(labels[rows], dtype=np.int64)
        host = np.asarray(labels[central], dtype=np.int64)
        check_label_forests(tree_forest, own, host, values["ForestIndex"], path)
        cross = (central != rows) & (own != host)
        if not cross.any():
            continue
        own, host = own[cross], host[cross]
        key_parts.append(pair_keys(np.minimum(own, host), np.maximum(own, host)))
        mass_parts.append(values[MASS_COLUMN][cross].astype(np.float32))
    if not key_parts:
        return np.zeros(0, dtype=EDGE_DTYPE), 0
    members = sum(part.size for part in key_parts)
    return reduce_contributions(key_parts, mass_parts), members


def drain(parts: List[np.ndarray]) -> np.ndarray:
    """Concatenate ``parts`` and empty the list, so the parts can be released."""
    whole = np.concatenate(parts) if parts else np.zeros(0, dtype=np.int64)
    parts.clear()
    return whole


# ---- the external merge -------------------------------------------------------


class NpyAppender:
    """Append records to an ``.npy`` file whose length is known only at the
    end: records go to a raw scratch file, which :meth:`finish` copies, in
    bounded chunks, behind an ``.npy`` header and renames into place."""

    def __init__(self, path, dtype: np.dtype, copy_rows: int = 1 << 20):
        self.path = Path(path)
        self.dtype = np.dtype(dtype)
        self.copy_rows = int(copy_rows)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._raw = self.path.with_name(self.path.name + ".raw.tmp")
        self._handle = open(self._raw, "wb")
        self.count = 0

    def append(self, records: np.ndarray) -> None:
        self._handle.write(np.ascontiguousarray(records, dtype=self.dtype).tobytes())
        self.count += int(records.size)

    def finish(self) -> int:
        """Write the ``.npy`` file; returns its size."""
        self._handle.close()
        tmp = self.path.with_name(self.path.name + ".tmp")
        if self.count == 0:
            with open(tmp, "wb") as handle:
                np.save(handle, np.zeros(0, dtype=self.dtype), allow_pickle=False)
        else:
            target = np.lib.format.open_memmap(
                tmp, mode="w+", dtype=self.dtype, shape=(self.count,)
            )
            for start in range(0, self.count, self.copy_rows):
                rows = min(self.copy_rows, self.count - start)
                target[start : start + rows] = np.fromfile(
                    self._raw, dtype=self.dtype, count=rows, offset=start * self.dtype.itemsize
                )
            target.flush()
            del target
        os.replace(str(tmp), str(self.path))
        os.remove(self._raw)
        return self.path.stat().st_size


def merge_slab_edges(
    sources: Sequence[Tuple[int, np.ndarray]], out_path, window_rows: int = DEFAULT_BLOCK_ROWS
) -> Tuple[int, int]:
    """Merge per-slab pair lists into per-pair records (module docstring).

    Args:
        sources: ``(snapshot, edges)`` in ascending snapshot, each sorted by
            key with unique keys (mapped arrays are read window by window).
        window_rows: the entries resident across all windows.

    Returns:
        ``(pairs, bytes)`` written to ``out_path``.
    """
    active = [[snap, edges, 0] for snap, edges in sources if edges.size]
    writer = NpyAppender(out_path, PAIR_DTYPE)
    window = max(1, int(window_rows) // max(1, len(active)))
    while active:
        windows = []
        cutoff = None
        for _snap, edges, at in active:
            part = edges[at : at + window]
            keys = pair_keys(part["lo"], part["hi"])
            windows.append((part, keys))
            if at + window < edges.size and (cutoff is None or int(keys[-1]) < cutoff):
                cutoff = int(keys[-1])
        key_parts, snap_parts, halo_parts, mass_parts = [], [], [], []
        for entry, (part, keys) in zip(active, windows):
            taken = keys.size if cutoff is None else int(np.searchsorted(keys, cutoff, "right"))
            key_parts.append(keys[:taken])
            snap_parts.append(np.full(taken, entry[0], dtype=np.int32))
            halo_parts.append(part["halos"][:taken].astype(np.int64))
            mass_parts.append(part["mass"][:taken].astype(np.float64))
            entry[2] += taken
        active = [entry for entry in active if entry[2] < entry[1].size]
        keys = np.concatenate(key_parts)
        snaps = np.concatenate(snap_parts)
        halos = np.concatenate(halo_parts)
        mass = np.concatenate(mass_parts)
        del key_parts, snap_parts, halo_parts, mass_parts, windows
        if keys.size == 0:  # pragma: no cover - the window ending at the cutoff yields
            continue
        order = np.lexsort((snaps, keys))
        keys, snaps, halos, mass = keys[order], snaps[order], halos[order], mass[order]
        del order
        starts = np.flatnonzero(np.r_[True, keys[1:] != keys[:-1]])
        ends = np.r_[starts[1:], keys.size]
        records = np.zeros(starts.size, dtype=PAIR_DTYPE)
        records["lo"] = keys[starts] >> _KEY_SHIFT
        records["hi"] = keys[starts] & ((1 << _KEY_SHIFT) - 1)
        records["snapshots"] = ends - starts
        records["first"] = snaps[starts]
        records["last"] = snaps[ends - 1]
        records["halos"] = np.add.reduceat(halos, starts)
        records["mass"] = np.add.reduceat(mass, starts)
        writer.append(records)
    return writer.count, writer.finish()


# ---- components ----------------------------------------------------------------


def local_index(forest_trees: np.ndarray, ordinals: np.ndarray) -> np.ndarray:
    """Position of each root ordinal in the ascending ``forest_trees``.

    Raises:
        ConverterError: for an ordinal that is not a named-forest tree.
    """
    ordinals = np.asarray(ordinals, dtype=np.int64)
    position = np.searchsorted(forest_trees, ordinals)
    clipped = np.minimum(position, max(forest_trees.size - 1, 0))
    if ordinals.size and (forest_trees.size == 0 or (forest_trees[clipped] != ordinals).any()):
        raise ConverterError("a pair names a tree outside the named forests")
    return position.astype(np.int64)


def kept_pair_batches(
    pairs: np.ndarray,
    forest_trees: np.ndarray,
    rule: Rule,
    batch_rows: int = DEFAULT_BLOCK_ROWS,
) -> Callable[[], Iterator[Tuple[np.ndarray, np.ndarray]]]:
    """A restartable stream of the local endpoints of the pairs ``rule`` keeps."""

    def batches() -> Iterator[Tuple[np.ndarray, np.ndarray]]:
        for start in range(0, pairs.size, batch_rows):
            part = pairs[start : start + batch_rows]
            kept = rule.keep(part["snapshots"], part["halos"], part["mass"])
            yield (
                local_index(forest_trees, part["lo"][kept]),
                local_index(forest_trees, part["hi"][kept]),
            )

    return batches


def rule_components(
    pairs: np.ndarray,
    forest_trees: np.ndarray,
    rule: Rule,
    batch_rows: int = DEFAULT_BLOCK_ROWS,
) -> np.ndarray:
    """Each named-forest tree's component under ``rule``: its smallest local index."""
    return union_find(forest_trees.size, kept_pair_batches(pairs, forest_trees, rule, batch_rows))


def component_table(components: np.ndarray, local_totals: np.ndarray) -> Dict[str, np.ndarray]:
    """Per component, ascending by its root (smallest local index): the root,
    its tree count and its halo total."""
    roots, inverse, trees = np.unique(components, return_inverse=True, return_counts=True)
    halos = np.zeros(roots.size, dtype=np.int64)
    np.add.at(halos, inverse, local_totals)
    return {"root": roots.astype(np.int64), "trees": trees.astype(np.int64), "halos": halos}


def describe_components(
    components: np.ndarray,
    forest_trees: np.ndarray,
    local_forest: np.ndarray,
    local_totals: np.ndarray,
    roots: np.ndarray,
    forest_ids: np.ndarray,
) -> List[Dict]:
    """Per named forest: its component count, single-tree components and the
    largest components by halos (ties by the smallest root id)."""
    table = component_table(components, local_totals)
    component_forest = local_forest[table["root"]]
    records = []
    for forest in np.unique(local_forest):
        mine = np.flatnonzero(component_forest == forest)
        ranked = mine[np.lexsort((table["root"][mine], -table["halos"][mine]))]
        records.append(
            {
                "forest_index": int(forest),
                "forest_id": int(forest_ids[forest]),
                "trees": int(table["trees"][mine].sum()),
                "total_halos": int(table["halos"][mine].sum()),
                "components": int(mine.size),
                "separable_without_severing": bool(mine.size > 1),
                "single_tree_components": int(np.count_nonzero(table["trees"][mine] == 1)),
                "largest": [
                    {
                        "smallest_root_id": int(roots[forest_trees[table["root"][at]]]),
                        "trees": int(table["trees"][at]),
                        "halos": int(table["halos"][at]),
                    }
                    for at in ranked[:N_LARGEST]
                ],
            }
        )
    return records


# ---- the subcommand ----------------------------------------------------------------


def require_completed(aggregate_dir, directory, what: str, identity: dict, keys=()) -> dict:
    """A completed subcommand's summary, produced from this directory's dataset."""
    summary = require_summary(directory, what, tuple(keys) + ("dataset",))
    if summary["dataset"] != identity:
        raise ConverterError(
            "{}: the {} summary was not produced from this directory's dataset".format(
                aggregate_dir, what
            )
        )
    return summary


def named_forests(
    forest_indices: Optional[Sequence[int]], trees_summary: dict, n_forests: int
) -> np.ndarray:
    """The forests to census, ascending and unique: those given, else the
    forest with the largest total (from the ``trees`` summary).

    Raises:
        ConverterError: for a ``ForestIndex`` outside ``[0, n_forests)``, or no
            forest to default to.
    """
    if not forest_indices:
        largest = trees_summary.get("largest_forest")
        if not largest:
            raise ConverterError("no forest to census: the trees summary names no largest forest")
        forest_indices = [largest["forest_index"]]
    forests = np.unique(np.asarray([int(f) for f in forest_indices], dtype=np.int64))
    bad = forests[(forests < 0) | (forests >= n_forests)]
    if bad.size:
        raise ConverterError("ForestIndex {} outside [0, {})".format(bad.tolist(), n_forests))
    return forests


def run_graph(
    dataset: HorizontalDataset,
    aggregate_dir,
    forest_indices: Optional[Sequence[int]] = None,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
) -> Dict:
    """Build the per-slab edge lists, merge them, and find the complete
    graph's components; returns the summary.

    Raises:
        ConverterError: without a completed ``trees`` of this dataset, for a
            named forest out of range, or on a refusal of the pass.
    """
    identity = dataset.identity()
    aggregate_dir = bind(aggregate_dir, identity)
    trees_summary = require_completed(
        aggregate_dir,
        trees_dir(aggregate_dir),
        "trees",
        identity,
        ("largest_forest", "forest_mismatch_halos"),
    )
    mismatched = int(trees_summary["forest_mismatch_halos"])
    if mismatched:
        # a named-forest tree whose rows carry another ForestIndex would be read
        # only partly, silently losing the pairs through those rows
        raise ConverterError(
            "{}: the trees summary records {} halo(s) whose ForestIndex is not their tree's "
            "forest; the converter forbids this and the graph would miss pairs through them".format(
                aggregate_dir, mismatched
            )
        )
    forests = named_forests(forest_indices, trees_summary, dataset.n_forests_total)
    out = graph_dir(aggregate_dir)
    begin(out)

    roots = load_roots(aggregate_dir)
    tree_forest = load_array(trees_dir(aggregate_dir) / "tree_forest.npy")
    tree_totals = load_array(trees_dir(aggregate_dir) / "tree_totals.npy", mmap=True)
    forest_trees = np.flatnonzero(in_sorted(forests, tree_forest)).astype(np.int32)
    local_forest = tree_forest[forest_trees]
    local_totals = np.asarray(tree_totals[forest_trees], dtype=np.int64)
    tree_bytes = save_array(out / "forest_trees.npy", forest_trees)
    log("graph: {} forest(s), {} trees".format(forests.size, forest_trees.size))

    per_snapshot = []
    edge_bytes = edges_total = 0
    meter = ReadMeter()
    for snap in dataset.snapshots:
        edges, members = slab_edges(
            dataset,
            snap,
            load_labels(aggregate_dir, snap),
            tree_forest,
            forests,
            block_rows,
            meter,
        )
        edge_bytes += save_array(edge_path(aggregate_dir, snap), edges)
        edges_total += int(edges.size)
        per_snapshot.append(
            {"snapshot": snap, "cross_tree_members": members, "pairs": int(edges.size)}
        )
        log(
            "graph: snapshot {} -- {} cross-tree members, {} pairs".format(
                snap, members, edges.size
            )
        )
    del tree_forest

    sources = [(snap, load_slab_edges(aggregate_dir, snap)) for snap in dataset.snapshots]
    n_pairs, pair_bytes = merge_slab_edges(sources, out / "pairs.npy", block_rows)
    del sources
    pairs = load_pairs(aggregate_dir)
    log("graph: {} distinct pairs".format(n_pairs))

    components = rule_components(pairs, forest_trees, COMPLETE, block_rows)
    component_bytes = save_array(out / "components.npy", components.astype(np.int32))
    forest_ids = dataset.forest_ids()
    per_forest = describe_components(
        components, forest_trees, local_forest, local_totals, roots, forest_ids
    )
    summary = {
        "dataset": identity,
        "dataset_dir": str(dataset.directory.resolve()),
        "forests": [int(f) for f in forests],
        "mass_column": MASS_COLUMN,
        "mass_note": "pair masses sum M_Crit200, native float32 Mvir in Msun/h for a "
        "Consistent-Trees-derived dataset, with np.add.reduceat in row order: deterministic "
        "for a fixed NumPy build; an m threshold exactly on a sum is not portable across builds",
        "io": meter.record(
            "8 B x n_halos(s) of ForestIndex plus (link width + 4 B) x the named forests' row "
            "span(s) of FirstHaloInFOFgroup and M_Crit200, summed over slabs"
        ),
        "n_forest_trees": int(forest_trees.size),
        "per_snapshot": per_snapshot,
        "pairs": {
            "count": n_pairs,
            "max_snapshots": int(pairs["snapshots"].max()) if n_pairs else 0,
            "halos": int(pairs["halos"].sum()) if n_pairs else 0,
        },
        "complete_graph": {
            "rule": COMPLETE.name,
            "components": component_count(components),
            "per_forest": per_forest,
            "note": "the components of the complete effective graph (every pair kept): a "
            "forest with more than one is separable without severing any relation",
        },
        "aggregates": {
            "slab_edges": {
                "formula": "20 B per (slab, pair) (int32 lo, int32 hi, int32 members, float64 "
                "mass), plus a 192 B .npy header per file; one file per slab",
                "pairs": edges_total,
                "bytes": edge_bytes,
            },
            "pairs": {
                "formula": "36 B per distinct pair (int32 lo, hi, snapshots, first, last; int64 "
                "halos; float64 mass), plus a 256 B .npy header",
                "pairs": n_pairs,
                "bytes": pair_bytes,
            },
            "forest_trees": {
                "formula": "8 B per named-forest tree (int32 root ordinal, int32 component), "
                "plus a 128 B .npy header per file",
                "trees": int(forest_trees.size),
                "bytes": tree_bytes + component_bytes,
            },
        },
    }
    write_json(out / SUMMARY_NAME, summary)
    return summary
