"""Effective descendant trees: terminal-root labels for every halo, and tree occupancy.

An **effective descendant tree** is a component of the descendant graph rooted
at a terminal halo, one whose ``Descendant`` is -1, in any slab. Its root id is
the terminal halo's ``MostBoundID``. Whether these trees are Consistent-Trees'
physical ``#tree`` blocks is not established by construction; the root set is
therefore compared with the index files' tree roots (:func:`root_correspondence`)
and the verdict recorded, because a cut table keyed by tree root and the
rewriter's inventory both rely on it. A mismatch is reported, never repaired.

Two passes, both over bounded column blocks:

1. **Roots.** Every slab's ``Descendant``, ``MostBoundID`` and ``ForestIndex``
   are scanned; the terminal halos' ids become ``roots.npy``, a sorted int64
   array whose position is the tree's **root ordinal**, with each tree's forest
   and the slab its root lies in. A repeated terminal id is refused: it would
   make the ordinal ambiguous.
2. **Labels**, backward from the last slab with two slabs resident: a terminal
   halo takes its own root ordinal, every other halo its descendant's label
   (the descendant lies in the next slab, since ``links_adjacent`` is 1; a
   gapped dataset is refused). Each slab's labels are written as int32 root
   ordinals in the slab's row order; per slab the trees present and their
   counts are written as sparse pairs, and each tree's total and peak
   occupancy are accumulated.

Aggregates written under ``<aggregate>/trees/`` (sizes also in ``summary.json``):

- ``labels/slab_NNN.npy`` (int32): **4 B per halo**, ``4 x total halos`` plus a
  128 B header per file, about 90 GB at Shin-Uchuu scale (22,503,649,037
  halos). The labels live for the aggregate directory's lifetime, because the
  co-membership graph and the cut both read them; the directory is deleted by
  hand when the census is done.
- ``slab_NNN_trees.npy`` (int32 root ordinals, ascending) and
  ``slab_NNN_counts.npy`` (int32): **8 B per present tree per slab**,
  ``8 x sum over slabs of the trees present``, at most ``8 x sum over slabs of
  min(n_halos, n_trees)``.
- ``roots.npy`` (int64), ``tree_forest.npy`` (int64 ``ForestIndex``),
  ``tree_root_snapshot.npy`` (int32), ``tree_totals.npy`` (int64),
  ``tree_max_occupancy.npy`` (int32) and ``tree_max_snapshot.npy`` (int32, the
  lowest-numbered slab of the peak): **36 B x n_trees**.
- ``summary.json``: the tree count, the largest tree's total and peak, the root
  correspondence verdict, the conservation check, and the aggregate sizes.

Resident memory: the two label arrays (4 B x the two slabs' halos, about 4.2 GB
at the widest Shin-Uchuu pair), the per-tree arrays (36 B per tree) and one
per-tree count vector (8 B per tree) per slab, and the column blocks being read.
No other array of a slab's length and no tree x snapshot matrix is held.

The conservation check sums the tree totals per source file through
``locations.dat`` and, when a JSON carrying ``source_files`` with per-file
``parsed_count`` is given (a version 2 ``conversion_report.json``, or the ASCII
preparation's ``manifest.json``), compares each file's sum with its parsed
count. Agreement is necessary for the trees to be the ``#tree`` blocks, not
sufficient.
"""

import os
import sys
from pathlib import Path
from typing import Callable, Dict, Optional, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.aggregate import (  # noqa: E402
    SUMMARY_NAME,
    bind,
    load_array,
    read_json,
    save_array,
    slab_stem,
    write_json,
)
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402
from source_index import SourceIndex  # noqa: E402

TREES_DIR = "trees"
LABELS_DIR = "labels"

#: Columns the two passes read.
TREE_COLUMNS = ("Descendant", "MostBoundID", "ForestIndex")

#: Examples quoted per reported count.
N_EXAMPLES = 5

_INT32_MAX = int(np.iinfo(np.int32).max)


def _quiet(_message: str) -> None:
    pass


def trees_dir(aggregate_dir) -> Path:
    return Path(aggregate_dir) / TREES_DIR


def label_path(aggregate_dir, snap: int) -> Path:
    return trees_dir(aggregate_dir) / LABELS_DIR / (slab_stem(snap) + ".npy")


def load_labels(aggregate_dir, snap: int, mmap: bool = True) -> np.ndarray:
    """One slab's int32 root ordinals, in the slab's row order (mapped by default)."""
    return load_array(label_path(aggregate_dir, snap), mmap)


def slab_tree_pair_paths(aggregate_dir, snap: int) -> Tuple[Path, Path]:
    base = trees_dir(aggregate_dir)
    stem = slab_stem(snap)
    return base / (stem + "_trees.npy"), base / (stem + "_counts.npy")


def load_slab_tree_pairs(aggregate_dir, snap: int) -> Tuple[np.ndarray, np.ndarray]:
    """One slab's present root ordinals (int32, ascending) and their counts (int32)."""
    trees_path, counts_path = slab_tree_pair_paths(aggregate_dir, snap)
    return load_array(trees_path), load_array(counts_path)


def load_roots(aggregate_dir) -> np.ndarray:
    """The sorted terminal-root ids; position is the root ordinal."""
    return load_array(trees_dir(aggregate_dir) / "roots.npy")


def _examples(values) -> list:
    return np.asarray(values)[:N_EXAMPLES].tolist()


def _in_sorted(haystack: np.ndarray, needles: np.ndarray) -> np.ndarray:
    """Whether each needle occurs in the ascending ``haystack``."""
    if haystack.size == 0:
        return np.zeros(needles.shape, dtype=bool)
    position = np.minimum(np.searchsorted(haystack, needles), haystack.size - 1)
    return haystack[position] == needles


# ---- pass 1 ----------------------------------------------------------------


def enumerate_roots(
    dataset: HorizontalDataset, block_rows: int = DEFAULT_BLOCK_ROWS
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Every terminal halo's ``MostBoundID``, ``ForestIndex`` and slab.

    Returns:
        ``(roots, forests, snapshots)``: int64 ascending, int64 and int32,
        aligned; position is the root ordinal.

    Raises:
        ConverterError: on a repeated terminal id, or more roots than int32
            ordinals can number.
    """
    roots, forests, snaps = [], [], []
    for snap in dataset.snapshots:
        for _start, block in dataset.iter_columns(snap, TREE_COLUMNS, block_rows):
            terminal = block["Descendant"] == -1
            if terminal.any():
                roots.append(block["MostBoundID"][terminal].astype(np.int64))
                forests.append(block["ForestIndex"][terminal].astype(np.int64))
                snaps.append(np.full(int(terminal.sum()), snap, dtype=np.int32))
    if not roots:
        empty = np.zeros(0, dtype=np.int64)
        return empty, empty.copy(), np.zeros(0, dtype=np.int32)
    roots, forests, snaps = np.concatenate(roots), np.concatenate(forests), np.concatenate(snaps)
    order = np.argsort(roots, kind="stable")
    roots, forests, snaps = roots[order], forests[order], snaps[order]
    repeated = np.nonzero(roots[1:] == roots[:-1])[0]
    if repeated.size:
        raise ConverterError(
            "{}: {} terminal MostBoundID value(s) occur more than once, so root ordinals would "
            "be ambiguous; examples: {}".format(
                dataset.directory, repeated.size, _examples(roots[repeated])
            )
        )
    if roots.size > _INT32_MAX:
        raise ConverterError(
            "{}: {} terminal roots exceed int32 root ordinals".format(dataset.directory, roots.size)
        )
    return roots, forests, snaps


# ---- pass 2 ----------------------------------------------------------------


def _label_slab(
    dataset: HorizontalDataset,
    snap: int,
    roots: np.ndarray,
    tree_forest: np.ndarray,
    next_labels: Optional[np.ndarray],
    block_rows: int,
) -> Tuple[np.ndarray, int]:
    """One slab's labels, and the halos whose ``ForestIndex`` is not their tree's."""
    labels = np.empty(dataset.n_halos[snap], dtype=np.int32)
    forest_mismatches = 0
    path = dataset.snapshot_path(snap)
    for start, block in dataset.iter_columns(snap, TREE_COLUMNS, block_rows):
        descendant = block["Descendant"].astype(np.int64, copy=False)
        out = labels[start : start + descendant.size]
        terminal = descendant == -1
        if terminal.any():
            ids = block["MostBoundID"][terminal].astype(np.int64, copy=False)
            ordinal = np.searchsorted(roots, ids)
            if ordinal.size and (
                int(ordinal.max()) >= roots.size or not np.array_equal(roots[ordinal], ids)
            ):
                raise ConverterError(
                    "{}: a terminal halo is not among the enumerated roots; the dataset "
                    "changed during the census".format(path)
                )
            out[terminal] = ordinal
        linked = ~terminal
        if linked.any():
            target = descendant[linked]
            if next_labels is None:
                raise ConverterError(
                    "{}: the last slab holds {} non-null Descendant link(s)".format(
                        path, int(linked.sum())
                    )
                )
            if int(target.min()) < 0 or int(target.max()) >= next_labels.size:
                raise ConverterError(
                    "{}: Descendant outside [0, {}) of the next slab in rows [{}, {})".format(
                        path, next_labels.size, start, start + descendant.size
                    )
                )
            out[linked] = next_labels[target]
        forest_mismatches += int(np.count_nonzero(tree_forest[out] != block["ForestIndex"]))
    return labels, forest_mismatches


def label_trees(
    dataset: HorizontalDataset,
    aggregate_dir,
    roots: np.ndarray,
    tree_forest: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
) -> Dict:
    """Pass 2 (module docstring): write every slab's labels and tree pairs;
    returns the per-tree totals and peaks and the pass's counts and sizes."""
    n_trees = int(roots.size)
    totals = np.zeros(n_trees, dtype=np.int64)
    max_occ = np.zeros(n_trees, dtype=np.int32)
    max_snap = np.full(n_trees, -1, dtype=np.int32)
    label_bytes = pair_bytes = pairs_total = forest_mismatches = 0
    next_labels = None
    for snap in reversed(dataset.snapshots):
        labels, mismatches = _label_slab(dataset, snap, roots, tree_forest, next_labels, block_rows)
        forest_mismatches += mismatches
        label_bytes += save_array(label_path(aggregate_dir, snap), labels)
        counts = np.bincount(labels, minlength=n_trees) if n_trees else np.zeros(0, np.int64)
        present = np.flatnonzero(counts)
        trees_path, counts_path = slab_tree_pair_paths(aggregate_dir, snap)
        pair_bytes += save_array(trees_path, present.astype(np.int32))
        pair_bytes += save_array(counts_path, counts[present].astype(np.int32))
        pairs_total += int(present.size)
        totals += counts
        # descending slabs and a non-strict test: a tie moves to the lower-numbered slab
        better = (counts >= max_occ) & (counts > 0)
        max_occ[better] = counts[better]
        max_snap[better] = snap
        del counts, present, better
        next_labels = labels
        log("trees: snapshot {} -- {} halos labelled".format(snap, labels.size))
    return {
        "totals": totals,
        "max_occupancy": max_occ,
        "max_snapshot": max_snap,
        "forest_mismatch_halos": forest_mismatches,
        "label_bytes": label_bytes,
        "pair_bytes": pair_bytes,
        "pairs": pairs_total,
    }


# ---- checks ----------------------------------------------------------------


def root_correspondence(
    roots: np.ndarray,
    root_snapshot: np.ndarray,
    tree_forest: np.ndarray,
    forest_ids: np.ndarray,
    last_snapshot: int,
    index: Optional[SourceIndex] = None,
) -> Dict:
    """Compare the terminal roots with the index files' tree roots.

    The verdict is ``pass`` when the two root sets are equal and every root's
    forest (through the sidecar ``ForestID``) is the one ``forests.list``
    gives it, ``fail`` otherwise, and ``unchecked`` without index files. Roots
    outside the last slab are reported either way; they do not fail the
    verdict by themselves.
    """
    not_last = root_snapshot != last_snapshot
    record: Dict = {
        "terminal_roots": int(roots.size),
        "roots_not_in_last_snapshot": {
            "count": int(np.count_nonzero(not_last)),
            "examples": _examples(roots[not_last]),
        },
    }
    if index is None:
        record["verdict"] = "unchecked"
        return record
    position = index.find(roots)
    found = position >= 0
    listed = _in_sorted(roots, index.tree_roots)
    mismatched = np.zeros(roots.size, dtype=bool)
    mismatched[found] = index.forest_ids[position[found]] != forest_ids[tree_forest[found]]
    record["index_roots"] = index.n_trees
    record["roots_not_in_forests_list"] = {
        "count": int(np.count_nonzero(~found)),
        "examples": _examples(roots[~found]),
    }
    record["forests_list_roots_without_terminal_halo"] = {
        "count": int(np.count_nonzero(~listed)),
        "examples": _examples(index.tree_roots[~listed]),
    }
    record["roots_not_in_last_snapshot"]["in_forests_list"] = int(
        np.count_nonzero(not_last & found)
    )
    record["forest_mismatches"] = {
        "count": int(np.count_nonzero(mismatched)),
        "examples": _examples(roots[mismatched]),
    }
    failed = (
        record["roots_not_in_forests_list"]["count"]
        or record["forests_list_roots_without_terminal_halo"]["count"]
        or record["forest_mismatches"]["count"]
    )
    record["verdict"] = "fail" if failed else "pass"
    return record


def load_parsed_counts(path) -> Dict[str, int]:
    """Per-file ``parsed_count`` keyed by file name, from a JSON whose
    ``source_files`` maps each source path to a record carrying it.

    Raises:
        ConverterError: without such a mapping, or when two paths share a name.
    """
    path = Path(path)
    sources = read_json(path).get("source_files")
    if not isinstance(sources, dict) or not sources:
        raise ConverterError(
            "{}: no source_files with per-file parsed_count (expected a version 2 "
            "conversion_report.json or an ASCII preparation manifest.json)".format(path)
        )
    counts: Dict[str, int] = {}
    for source, entry in sources.items():
        name = os.path.basename(source)
        if name in counts:
            raise ConverterError("{}: two source files are named {}".format(path, name))
        if not isinstance(entry, dict) or "parsed_count" not in entry:
            raise ConverterError("{}: {} has no parsed_count".format(path, source))
        counts[name] = int(entry["parsed_count"])
    return counts


def conservation(
    roots: np.ndarray,
    tree_totals: np.ndarray,
    index: SourceIndex,
    report_path=None,
) -> Dict:
    """Per-file halo sums of the trees through ``locations.dat``, compared with
    the report's per-file parsed counts when one is given (module docstring)."""
    position = index.find(roots)
    found = position >= 0
    file_ids = sorted(index.filenames)
    sums = np.zeros((max(file_ids) + 1) if file_ids else 0, dtype=np.int64)
    np.add.at(sums, index.file_ids[position[found]], tree_totals[found])
    files = [
        {
            "file_id": file_id,
            "name": os.path.basename(index.filenames[file_id]),
            "census_halos": int(sums[file_id]),
        }
        for file_id in file_ids
    ]
    record: Dict = {
        "files": files,
        "unattributed_halos": int(tree_totals[~found].sum()),
    }
    if report_path is None:
        record["verdict"] = "unchecked"
        return record
    parsed = load_parsed_counts(report_path)
    names = {entry["name"] for entry in files}
    mismatched = 0
    for entry in files:
        entry["parsed_count"] = parsed.get(entry["name"])
        entry["agrees"] = entry["parsed_count"] == entry["census_halos"]
        mismatched += not entry["agrees"]
    record["report"] = str(Path(report_path).resolve())
    record["report_files_not_in_locations"] = sorted(set(parsed) - names)
    record["files_disagreeing"] = mismatched
    record["verdict"] = (
        "pass"
        if not mismatched
        and not record["report_files_not_in_locations"]
        and not record["unattributed_halos"]
        else "fail"
    )
    return record


# ---- the subcommand ----------------------------------------------------------


def run_trees(
    dataset: HorizontalDataset,
    aggregate_dir,
    index: Optional[SourceIndex] = None,
    report_path=None,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
) -> Dict:
    """Run both passes and the checks, write the aggregates; returns the summary.

    Raises:
        ConverterError: for a gapped dataset (``links_adjacent`` 0), a report
            without index files, or any refusal of the passes.
    """
    if report_path is not None and index is None:
        raise ConverterError("the conservation check needs forests.list and locations.dat")
    if dataset.links_adjacent != 1:
        raise ConverterError(
            "{}: links_adjacent is {}; the census labels adjacent descendant links only".format(
                dataset.directory, dataset.links_adjacent
            )
        )
    identity = dataset.identity()
    aggregate_dir = bind(aggregate_dir, identity)
    out = trees_dir(aggregate_dir)
    out.mkdir(parents=True, exist_ok=True)

    log("trees: enumerating terminal roots")
    roots, tree_forest, root_snapshot = enumerate_roots(dataset, block_rows)
    tree_bytes = save_array(out / "roots.npy", roots)
    tree_bytes += save_array(out / "tree_forest.npy", tree_forest)
    tree_bytes += save_array(out / "tree_root_snapshot.npy", root_snapshot)
    log("trees: {} terminal roots".format(roots.size))

    labelled = label_trees(dataset, aggregate_dir, roots, tree_forest, block_rows, log)
    totals = labelled["totals"]
    if int(totals.sum()) != dataset.total_halos:  # pragma: no cover - every row is labelled
        raise ConverterError("tree totals do not sum to the dataset's halos")
    tree_bytes += save_array(out / "tree_totals.npy", totals)
    tree_bytes += save_array(out / "tree_max_occupancy.npy", labelled["max_occupancy"])
    tree_bytes += save_array(out / "tree_max_snapshot.npy", labelled["max_snapshot"])

    forest_ids = dataset.forest_ids()
    largest_tree = largest_forest = None
    if roots.size:
        tree = int(np.argmax(totals))
        peak_snap = int(labelled["max_snapshot"][tree])
        largest_tree = {
            "root_ordinal": tree,
            "root_id": int(roots[tree]),
            "forest_index": int(tree_forest[tree]),
            "forest_id": int(forest_ids[tree_forest[tree]]),
            "root_snapshot": int(root_snapshot[tree]),
            "total_halos": int(totals[tree]),
            "peak_occupancy": int(labelled["max_occupancy"][tree]),
            "peak_snapshot": peak_snap,
        }
        forest_trees = np.bincount(tree_forest, minlength=dataset.n_forests_total)
        forest_halos = np.zeros(dataset.n_forests_total, dtype=np.int64)
        np.add.at(forest_halos, tree_forest, totals)
        forest = int(np.argmax(forest_halos))
        largest_forest = {
            "forest_index": forest,
            "forest_id": int(forest_ids[forest]),
            "trees": int(forest_trees[forest]),
            "total_halos": int(forest_halos[forest]),
        }
        del forest_trees, forest_halos

    summary = {
        "dataset": identity,
        "dataset_dir": str(dataset.directory.resolve()),
        "total_halos": dataset.total_halos,
        "n_trees": int(roots.size),
        "largest_tree": largest_tree,
        "largest_forest": largest_forest,
        "forest_mismatch_halos": labelled["forest_mismatch_halos"],
        "root_correspondence": root_correspondence(
            roots, root_snapshot, tree_forest, forest_ids, dataset.snapshots[-1], index
        ),
        "conservation": (
            None if index is None else conservation(roots, totals, index, report_path)
        ),
        "aggregates": {
            "labels": {
                "formula": "4 B x total halos (int32 root ordinal per halo, one file per slab in "
                "row order), plus a 128 B .npy header per file",
                "halos": dataset.total_halos,
                "bytes": labelled["label_bytes"],
            },
            "slab_tree_pairs": {
                "formula": "8 B per (slab, present tree) pair (int32 root ordinal + int32 "
                "count), at most 8 B x sum over slabs of min(n_halos, n_trees), plus a 128 B "
                ".npy header per file; two files per slab",
                "pairs": labelled["pairs"],
                "bytes": labelled["pair_bytes"],
            },
            "per_tree": {
                "formula": "36 B x n_trees (int64 root, int64 forest, int32 root snapshot, int64 "
                "total, int32 maximum occupancy, int32 snapshot of the maximum), plus a 128 B "
                ".npy header per file",
                "n_trees": int(roots.size),
                "bytes": tree_bytes,
            },
        },
    }
    write_json(out / SUMMARY_NAME, summary)
    return summary
