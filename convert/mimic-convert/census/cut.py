"""Candidate cuts of the named forests: tables, severed relations, predicted
effects and the partition with the pieces installed.

For every candidate rule (``census/rules.py``) over the co-membership graph
(``census/graph.py``), ``cut``:

1. finds the rule's components and names its pieces under F6
   (``census/cut_table.py``), keeping the compact per-tree assignment
   ``cut/rules/<rule>/assignment.npy`` (each named-forest tree's piece id,
   aligned with ``graph/forest_trees.npy``);
2. from the aggregates alone (one slab's tree pairs, occupancy pairs and edge
   list at a time): each piece's peak occupancy and its slab, the promotions
   each slab's edge list predicts, and the driver's partition
   (``census/partition.py``) with the piece sizes installed -- under F6 an
   original forest keeps its ``ForestIndex`` (the cut forest's largest piece
   among them) and the fresh pieces enumerate after every original forest in
   ascending fresh id -- giving every grid point's widest range per slab and
   overall and two memory figures at the stated bytes per resident halo: the
   **process** figure (the widest range; the per-rank
   ``retention_memory_ceiling_mb`` guidance) and the **job** figure (the sum
   over tasks of each task's widest chunk over all slabs, since one laptop
   runs every rank at once and the ranks do not move through the slabs in
   step; :func:`laptop_rows`). Per laptop class, a row says whether any grid
   point's job fits and the smallest that does;
3. from one further pass over the named forests' rows of every slab with the
   labels mapped (the census's fourth read of those columns, bounded like the
   others; every rule is evaluated in the same pass):

   - **severed co-memberships**: the pairs the rule drops and, of those, the
     pairs whose trees fall in different pieces (severed); per slab, the halos
     **promoted** to central (members whose central lies in another piece,
     F6), the groups that lose members, and the **groups whose central leaves
     while members stay**, counted as (group, piece) remnants: a piece's
     members of one group whose central is in another piece, each of which
     becomes its own central (no host is invented), and of those the remnants
     holding two or more members (members that stay together in one piece but
     no longer share a group). The promotions are checked against those the
     slab's edge list predicts;
   - **progenitor-order changes** under F4: per slab, the encounter order of a
     descendant's progenitors is ascending (upid, pid, id), upid being the FoF
     central's ``MostBoundID`` (``FirstHaloInFOFgroup``'s row) and pid -1 for a
     central, else the central's id; a promoted halo becomes (id, -1, id). The
     chain is the reference incremental-insertion loop with ``Mvir``
     (``M_Crit200``, float32): the first progenitor encountered starts it and
     each later one moves to the front if strictly more massive than the
     current head, else joins the tail. Only descendants with a promoted
     progenitor can change; for each, the chain before the cut is also
     rebuilt from the dataset's ``NextProgenitor`` links and compared with the
     recomputed one (``stored_chain_mismatches``, which must be 0 for the
     prediction to stand);
   - the **predicted model effects** (decision 7). ``sage16`` and
     ``halos-only``: the seeds (promoted halos, the centrals of groups that
     lose members, and descendants whose progenitor chain changes) and their
     dependents along descendant links -- every halo on a seed's descendant
     path, propagated slab by slab -- with an upper bound, every halo of a
     piece holding a seed (a piece without one keeps its topology and its
     relative order). HOD and SHAM: every halo of every forest the rule cuts,
     because their draws and tie-breaks key on ids the cut changes.

The halos **re-labelled** are also counted: those of fresh pieces (new forest
id), every halo of a cut forest (``HaloRankInForest`` recomputed), and every
halo whose ``SourceHaloID`` prefix moves (every forest from the first cut one).

A complete table in ``forests.list`` shape and its JSON record are written
only for the rules asked for (``materialise``), only when the ``trees`` root
correspondence verdict passed, and only when the index files given to the run
describe the census (:func:`check_index`); they live under
``cut/rules/<rule>/`` as ``forests.list`` and ``record.json``. Without a table
to write, a failed index check does not stop the run (PM ruling DD12): it is
recorded in the summary (``index_check``) and the census explores every rule.
Fresh piece ids start above one floor used for naming, the partition and the
summary (``fresh_id_floor``): the larger of the supplied index's and the
dataset sidecar's largest forest id, so a fresh piece never takes an existing
forest's id. When the index check passed the two are equal.

**Reads.** Each slab is read once by the pass: ``ForestIndex`` in full
blocks, ``FirstHaloInFOFgroup`` and ``Descendant`` over each block's span of
named-forest rows, and ``MostBoundID``, ``M_Crit200`` and ``NextProgenitor``
over each block's span of **retained rows**, the rows of the trees a severed
edge of the slab joins and of the trees they share a group with
(:func:`touched_trees`). Those rows supply every progenitor record, FoF
central id and losing central's descendant the pass needs, so nothing is
re-read. Bytes read per slab: ``8 x n_halos(s) + 2 w x span(s) + (12 + w) x
retained_span(s)``, with ``w`` the link width (4 B in version 2, 8 B in
version 3), ``span(s)`` the rows from the first to the last named-forest row of
each block (in version 3 the forests' rows when they are adjacent in
``ForestIndex``, else their enclosing span) and ``retained_span(s)`` the same
over the retained rows. The bytes read are measured and recorded per column in
``summary.json`` (``io``).

**Memory**, with ``n`` all trees, ``t`` the named forests' trees, ``F`` the
forests, ``R`` the rules and ``p`` a rule's pieces (per-unit costs measured
with ``tracemalloc``):

- for the whole subcommand: the index files' roots and forest ids (16 B per
  catalogue tree; :meth:`source_index.SourceIndex.load` peaks at about 79 B per
  tree before anything else is loaded, 24.9 GB at Shin-Uchuu's 315,004,242
  trees), the census roots (8 B x n), the dense tree to local-index map (4 B x
  n) and the named-forest tree arrays (28 B x t): about **11.7 GB** at
  Shin-Uchuu scale with the super-forest's 104,845,278 trees; per rule, the
  piece of each tree (4 B x t, 0.42 GB) and about 82 B per piece; the
  union-find of ``census/rules.py`` (about 1.0 GB, transient) and the dense
  partition weights (8 B x (F + fresh pieces), 1.3 GB, transient);
- per slab of step 2 (:func:`aggregate_slab`, from the aggregates alone):
  about **37 B per pair** of the slab's edge list (the list, its local
  endpoints and every rule's piece gathers), **14 B per present tree** and 12
  B per occupancy pair, all released when the slab is done, before the pass
  begins. Pairs per slab are at most its cross-tree members, so at worst
  (every super-forest row of snapshot 31 a member of its own pair) about 12.3
  GB;
- per slab of the pass (PM ruling DD10: arrays proportional to the named
  forests' rows in one slab, stated here with their worst case): the block's
  columns, label gathers and transients, about **114 B x block_rows** (0.48
  GB at the default 2^22) for one rule and 17 B x block_rows more per further
  rule; the slab's edge list and :func:`touched_trees`' gathers, about **38 B
  per pair** (20 B for the list, 18 B for its endpoints and masks over pairs),
  with its two tree masks, **2 B per named-forest tree** (0.21 GB for the
  super-forest), the edge terms at worst about 12.7 GB as above; the retained
  rows, about **34 B each**, at most the named forests' rows of the slab, so at
  worst the super-forest's 333,663,215 rows at snapshot 31, **11.3 GB**; each
  promotion with its share of the chain work and of the dependents'
  bookkeeping, about **165 B** as measured with groups of one or two
  progenitors, promotions being at most the slab's cross-tree members
  (``graph``'s ``per_snapshot`` records them); the descendant sets being
  propagated, 8 B per affected halo; and the chain working memory of
  :func:`_chain_changes`, stated on its own because it grows with the slab's
  affected siblings ``a`` (the progenitors of descendants with a promoted
  progenitor) and with its largest progenitor group ``g_max``: the sorted
  sibling index, about **29 B x a** while it is built (``a`` is at most the
  retained rows, so at worst 9.7 GB at snapshot 31), and one batch's working
  arrays, about **200 B x (block_rows + g_max - 1)** (0.84 GB at the default
  2^22 plus 200 B per progenitor of the largest group beyond it; the
  theoretical worst case, one descendant with every one of snapshot 31's
  333,663,215 super-forest rows as progenitors, would be 67 GB, while a
  descendant's progenitors in a real merger tree number in the thousands at
  most);
- while a table is checked and written: ``census/cut_table.py``'s 50 B per
  catalogue tree and the table's ids, 8 B per tree: **18.3 GB**. Nothing of
  the pass or of step 2 is held by then.

The subcommand's peak at Shin-Uchuu scale is the largest of the index load
(24.9 GB), step 2 (11.7 GB + R x (0.42 GB + 82 B x p) + 1.3 GB + the slab
terms), the pass (11.7 GB + R x (0.42 GB + 82 B x p) + 0.7 GB + the edge,
retained-row and promotion terms) and a table (11.7 GB + R x (0.42 GB + 82 B x
p) + 18.3 GB). With eight rules of 10^7 pieces each, that is about 40 GB while
a table is written and about 22 GB plus the per-slab terms during the pass:
with as many pairs and retained rows as a few per cent of snapshot 31's
super-forest rows, a few GB more; in the worst case, every one of those rows a
cross-tree member of its own pair and retained, about 46 GB before the
promotions, plus 165 B per promotion (another 55 GB if every one of them were
promoted) and the chain working memory (up to 9.7 GB for the sibling index,
and 0.84 GB per batch plus 200 B per progenitor of the largest group beyond
``block_rows``). Every per-slab term follows from the aggregates Slice 9 records
(``graph``'s ``per_snapshot`` members and pairs), so the real peak can be
computed before the pass runs. The figures above were measured with
``tracemalloc`` on synthetic slabs of 2^20 rows and synthetic graphs of 2 x
10^6 pairs over 10^6 trees. No tree x snapshot or forest x snapshot matrix is
held.

Aggregates under ``<aggregate>/cut/``: per rule ``assignment.npy`` (**8 B x
t**, about 0.84 GB per rule for the super-forest), and for a materialised
rule ``forests.list`` (the catalogue's rows, about 24 B per tree) and
``record.json`` (``census/cut_table.py``: about 40 B per piece, streamed);
``summary.json`` (a few kilobytes per rule plus about 10 integers per rule,
grid point and slab).
"""

import os
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.aggregate import (  # noqa: E402
    SUMMARY_NAME,
    begin,
    bound_identity,
    load_array,
    save_array,
    write_json,
)
from census.cut_table import (  # noqa: E402
    check_table,
    name_pieces,
    table_ids,
    write_table,
    write_table_record,
)
from census.graph import (  # noqa: E402
    MASS_COLUMN,
    ReadMeter,
    check_central_rows,
    drain,
    graph_dir,
    in_sorted,
    in_sorted_window,
    iter_forest_rows,
    load_forest_trees,
    load_pairs,
    load_slab_edges,
    local_index,
    pair_keys,
    read_span,
    require_completed,
    rule_components,
)
from census.occupancy import load_slab_pairs, occupancy_dir  # noqa: E402
from census.partition import (  # noqa: E402
    DEFAULT_NCHUNKS,
    DEFAULT_NTASKS,
    partition_cut,
    range_rows,
)
from census.rules import Rule  # noqa: E402
from census.trees import load_labels, load_roots, load_slab_tree_pairs, trees_dir  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402

CUT_DIR = "cut"
RULES_DIR = "rules"

#: Bytes per resident halo of a chunked horizontal run (the chunked streaming record).
DEFAULT_BYTES_PER_HALO = 1100

#: Laptop memory classes, GiB.
DEFAULT_LAPTOP_GIB = (16, 32, 64)

#: Pieces and examples quoted per list.
N_LARGEST = 10
N_EXAMPLES = 5

_GIB = 1 << 30

#: What each per-snapshot severance count means, recorded with the counts.
SEVERANCE_DEFINITIONS = {
    "promoted_halos": "members whose FoF central lies in another piece; each becomes a central",
    "groups_losing_members": "FoF groups with at least one promoted member",
    "groups_central_leaves_members_stay": "(group, piece) remnants: a piece's members of one "
    "group whose central lies in another piece; each member becomes its own central",
    "remnants_with_several_members": "of those remnants, the ones holding two or more members: "
    "members that stay together in one piece but no longer share a group",
    "seed_halos": "promoted halos, centrals of groups losing members, and descendants whose "
    "progenitor chain changed over the previous slab",
    "affected_halos": "seeds and every halo on a seed's descendant path, in this slab",
    "progenitor_order_changed": "descendants at the next snapshot whose progenitor chain over "
    "this slab changes",
    "first_progenitor_changed": "of those, the descendants whose first progenitor changes",
}


def _quiet(_message: str) -> None:
    pass


def cut_dir(aggregate_dir) -> Path:
    return Path(aggregate_dir) / CUT_DIR


def rule_dir(aggregate_dir, rule: Rule) -> Path:
    return cut_dir(aggregate_dir) / RULES_DIR / rule.name


def load_assignment(aggregate_dir, rule: Rule) -> np.ndarray:
    """A rule's piece id for each named-forest tree, aligned with ``forest_trees``."""
    return load_array(rule_dir(aggregate_dir, rule) / "assignment.npy")


# ---- inputs ------------------------------------------------------------------


def prepare_cut(
    dataset: HorizontalDataset,
    aggregate_dir,
    rules: Sequence[Rule],
    materialise: Sequence[Rule] = (),
) -> Dict:
    """Every refusal that needs no pass result, before anything is loaded or
    written: the rules, the materialised subset, the directory's dataset, and
    the completed ``occupancy``, ``trees`` and ``graph`` of that dataset, with
    a passed root correspondence when a table is to be written.

    A run whose ``cut/rules/`` holds output it would not rewrite (a rule not
    among this run's, or a table and record of a rule this run does not
    materialise) is refused too, so that no earlier run's output survives
    beside this run's summary; such output is removed by hand.

    Raises:
        ConverterError: naming the first refusal.
    """
    if not rules:
        raise ConverterError("cut needs at least one rule")
    names = [rule.name for rule in rules]
    repeated = sorted({name for name in names if names.count(name) > 1})
    if repeated:
        raise ConverterError("rule(s) given more than once: {}".format(repeated))
    unknown = sorted({rule.name for rule in materialise} - set(names))
    if unknown:
        raise ConverterError("materialised rule(s) not among the rules: {}".format(unknown))
    identity = bound_identity(aggregate_dir)
    if dataset.identity() != identity:
        raise ConverterError(
            "{}: produced from a different dataset (identity differs)".format(aggregate_dir)
        )
    occupancy = require_completed(
        aggregate_dir, occupancy_dir(aggregate_dir), "occupancy", identity, ("widest_slab",)
    )
    trees = require_completed(
        aggregate_dir, trees_dir(aggregate_dir), "trees", identity, ("root_correspondence",)
    )
    graph = require_completed(
        aggregate_dir, graph_dir(aggregate_dir), "graph", identity, ("forests",)
    )
    verdict = trees["root_correspondence"].get("verdict")
    if materialise and verdict != "pass":
        raise ConverterError(
            "{}: the trees root correspondence verdict is {!r}; a cut table is written only "
            "when it passed".format(aggregate_dir, verdict)
        )
    if occupancy["widest_slab"] is None:
        raise ConverterError("{}: the dataset has no halo to cut".format(aggregate_dir))
    stale = stale_outputs(aggregate_dir, rules, materialise)
    if stale:
        raise ConverterError(
            "{}: {} output(s) of an earlier cut run this run would not rewrite (e.g. {}); "
            "remove them by hand or use a fresh aggregate directory".format(
                cut_dir(aggregate_dir) / RULES_DIR, len(stale), stale[:N_EXAMPLES]
            )
        )
    return {
        "identity": identity,
        "occupancy": occupancy,
        "trees": trees,
        "graph": graph,
        "forests": np.asarray(graph["forests"], dtype=np.int64),
    }


def stale_outputs(aggregate_dir, rules: Sequence[Rule], materialise: Sequence[Rule]) -> List[str]:
    """The entries of ``cut/rules/`` a run of ``rules`` would not rewrite,
    relative to that directory. A ``<file>.tmp`` left by an interrupted write
    of a file this run rewrites is not counted: the atomic write replaces it.
    """
    base = cut_dir(aggregate_dir) / RULES_DIR
    if not base.is_dir():
        return []
    written = {rule.name: {"assignment.npy"} for rule in rules}
    for rule in materialise:
        written[rule.name] |= {"forests.list", "record.json"}
    stale = []
    for entry in sorted(base.iterdir()):
        if entry.name not in written or not entry.is_dir():
            stale.append(entry.name)
            continue
        # an interrupted write's temporary of a file this run rewrites is
        # replaced by that atomic write, so it is not stale
        rewritten = written[entry.name] | {name + ".tmp" for name in written[entry.name]}
        stale.extend(
            "{}/{}".format(entry.name, item.name)
            for item in sorted(entry.iterdir())
            if item.name not in rewritten
        )
    return stale


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
        the caller refuses a table on ``fail`` and records it otherwise (PM
        ruling DD12: exploration proceeds whatever the correspondence).
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


# ---- per-rule state ------------------------------------------------------------


@dataclass
class RuleState:
    """One rule's pieces and the pass's accumulators."""

    rule: Rule
    pieces: Dict[str, np.ndarray]
    tree_piece: np.ndarray  # piece index per local tree
    peak: np.ndarray = field(init=False)  # per piece, its largest slab count
    peak_snapshot: np.ndarray = field(init=False)  # the lowest-numbered slab of the peak
    seeded: np.ndarray = field(init=False)  # per piece, whether it holds a seed
    widest_counts: Optional[np.ndarray] = None  # per piece, its rows in the widest slab
    current: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=np.int64))
    current_seeds: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=np.int64))
    following: List[np.ndarray] = field(default_factory=list)
    following_seeds: List[np.ndarray] = field(default_factory=list)
    per_snapshot: List[Dict] = field(default_factory=list)

    def __post_init__(self):
        n_pieces = self.pieces["id"].size
        self.peak = np.zeros(n_pieces, dtype=np.int64)
        self.peak_snapshot = np.full(n_pieces, -1, dtype=np.int64)
        self.seeded = np.zeros(n_pieces, dtype=bool)


def piece_counts(states: Sequence[RuleState], local: np.ndarray, counts: np.ndarray) -> List:
    """Per rule, one slab's halos per piece from its present trees' counts."""
    out = []
    weights = np.asarray(counts, dtype=np.float64)  # exact: a slab's counts are below 2^31
    for state in states:
        out.append(
            np.bincount(
                state.tree_piece[local], weights=weights, minlength=state.pieces["id"].size
            ).astype(np.int64)
        )
    return out


def slab_local_counts(
    aggregate_dir, snap: int, local_of: np.ndarray
) -> Tuple[np.ndarray, np.ndarray]:
    """One slab's named-forest trees (local indices) and their counts."""
    ordinals, counts = load_slab_tree_pairs(aggregate_dir, snap)
    local = local_of[ordinals]
    named = local >= 0
    return local[named].astype(np.int64), counts[named]


def install_pieces(
    forests: np.ndarray,
    counts: np.ndarray,
    state: RuleState,
    per_piece: np.ndarray,
    n_forests: int,
    catalogue_max: int,
) -> Tuple[np.ndarray, np.ndarray]:
    """One slab's per-forest rows after the cut: each named forest's count is
    its id-keeping piece's, and each fresh piece is appended at ``ForestIndex``
    ``n_forests + (id - catalogue_max - 1)`` (ascending fresh id). Empty
    entries are dropped; the result ascends."""
    forests = np.asarray(forests, dtype=np.int64)
    counts = np.asarray(counts, dtype=np.int64).copy()
    pieces = state.pieces
    keeping = np.flatnonzero(pieces["keeps_id"])
    at = np.searchsorted(forests, pieces["forest_index"][keeping])
    present = at < forests.size
    present[present] = forests[at[present]] == pieces["forest_index"][keeping][present]
    counts[at[present]] = per_piece[keeping[present]]
    fresh = np.flatnonzero(~pieces["keeps_id"] & (per_piece > 0))
    fresh = fresh[np.argsort(pieces["id"][fresh], kind="stable")]
    nonzero = counts > 0
    return (
        np.concatenate([forests[nonzero], n_forests + (pieces["id"][fresh] - catalogue_max - 1)]),
        np.concatenate([counts[nonzero], per_piece[fresh]]),
    )


# ---- progenitor chains (F4) -----------------------------------------------------


def chain_order(encounter: Sequence[int], mass: np.ndarray) -> List[int]:
    """The reference incremental-insertion chain over progenitors visited in
    ``encounter`` order: the first starts the chain, each later one becomes
    the head if strictly more massive than the current head, else the tail
    (``links.build_progenitor_links``). Kept as the reference that
    :func:`chain_positions` is tested against."""
    encounter = [int(i) for i in encounter]
    chain = deque(encounter[:1])
    head = encounter[0]
    for item in encounter[1:]:
        if mass[head] < mass[item]:
            chain.appendleft(item)
            head = item
        else:
            chain.append(item)
    return list(chain)


def chain_positions(
    groups: np.ndarray, upid: np.ndarray, pid: np.ndarray, ids: np.ndarray, mass: np.ndarray
) -> np.ndarray:
    """Every progenitor's position in its descendant's chain, for all
    descendants at once (``groups`` is each record's descendant).

    The insertion loop moves a progenitor to the front exactly when it is a
    strict prefix maximum of mass in encounter order (ascending (upid, pid,
    id)), and appends every other one at the tail. So the chain is the strict
    prefix maxima in reverse encounter order, then the rest in encounter
    order, and its head is the last strict prefix maximum. Masses become
    dense ranks offset by the group's ordinal, so one ``maximum.accumulate``
    over all groups gives each group's running maximum.

    Returns:
        int64 positions (0 is the first progenitor), aligned with the inputs.
    """
    groups = np.asarray(groups, dtype=np.int64)
    positions = np.zeros(groups.size, dtype=np.int64)
    if groups.size == 0:
        return positions
    order = np.lexsort((ids, pid, upid, groups))
    grouped = groups[order]
    first = np.r_[True, grouped[1:] != grouped[:-1]]
    group_no = np.cumsum(first) - 1
    rank = np.unique(np.asarray(mass)[order], return_inverse=True)[1].reshape(-1)
    levels = int(rank.max()) + 1
    value = group_no * levels + rank  # below 2^63: groups and levels are below 2^31
    running = np.maximum.accumulate(value)
    record = first | (value > np.r_[-1, running[:-1]])
    starts = np.flatnonzero(first)
    records_before = np.cumsum(record) - record
    others_before = np.cumsum(~record) - ~record
    n_records = np.add.reduceat(record.astype(np.int64), starts)[group_no]
    record_at = records_before - records_before[starts][group_no]
    other_at = others_before - others_before[starts][group_no]
    positions[order] = np.where(record, n_records - 1 - record_at, n_records + other_at)
    return positions


def stored_chain_mismatches(
    groups: np.ndarray, rows: np.ndarray, next_rows: np.ndarray, positions: np.ndarray
) -> np.ndarray:
    """The groups whose stored ``NextProgenitor`` links are not the chain
    ``positions`` describe: in a matching group every progenitor links to the
    next one in the chain and the last links to -1."""
    order = np.lexsort((positions, groups))
    grouped = groups[order]
    chained = rows[order]
    continues = np.r_[grouped[1:] == grouped[:-1], False]
    expected = np.where(continues, np.r_[chained[1:], -1], -1)
    return np.unique(grouped[np.asarray(next_rows)[order] != expected])


# ---- the pass ------------------------------------------------------------------

#: Columns the pass reads for every named-forest row of a block.
PASS_COLUMNS = ("FirstHaloInFOFgroup", "Descendant")

#: Columns it reads, over the block's span of retained rows, for the rows of
#: trees touched by a severance in the slab.
RETAINED_COLUMNS = ("MostBoundID", MASS_COLUMN, "NextProgenitor")

_INT32_MAX = int(np.iinfo(np.int32).max)


def edge_locals(
    edges: np.ndarray, local_of: np.ndarray, snap: Optional[int] = None
) -> Tuple[np.ndarray, np.ndarray]:
    """The local tree indices of an edge list's two endpoints.

    Raises:
        ConverterError: for an endpoint outside the named forests, whose -1
            would otherwise index the last tree's piece.
    """
    lo = local_of[edges["lo"]]
    hi = local_of[edges["hi"]]
    outside = (lo < 0) | (hi < 0)
    if outside.any():
        at = int(np.argmax(outside))
        raise ConverterError(
            "{}edge list names {} pair(s) with a tree outside the named forests (e.g. root "
            "ordinals ({}, {})); the graph aggregates are stale".format(
                "" if snap is None else "snapshot {}: ".format(snap),
                int(np.count_nonzero(outside)),
                int(edges["lo"][at]),
                int(edges["hi"][at]),
            )
        )
    return lo, hi


def touched_trees(
    edges: np.ndarray, local_of: np.ndarray, states: Sequence[RuleState], n_local: int
) -> Tuple[np.ndarray, np.ndarray]:
    """From one slab's edge list: the trees an edge severed under any rule
    joins (``severed``), and those trees with every tree they share a group
    with in the slab (``retained``), as masks over the local trees.

    A promoted halo and its siblings (the other progenitors of its
    descendant) lie in a severed tree, so the chain records needed are the
    severed trees' rows; a central whose group loses members lies in a
    severed tree too; and a sibling's FoF central lies in its own tree or in a
    tree it shares an edge with, so the retained trees' rows hold every
    central the chain keys need.
    """
    severed = np.zeros(n_local, dtype=bool)
    retained = np.zeros(n_local, dtype=bool)
    if edges.size == 0:
        return severed, retained
    lo, hi = edge_locals(edges, local_of)
    cut_any = np.zeros(edges.size, dtype=bool)
    for state in states:
        cut_any |= state.tree_piece[lo] != state.tree_piece[hi]
    severed[lo[cut_any]] = True
    severed[hi[cut_any]] = True
    near = severed[lo] | severed[hi]
    retained[:] = severed
    retained[lo[near]] = True
    retained[hi[near]] = True
    return severed, retained


@dataclass
class Retained:
    """One slab's retained rows (ascending), with what the chains need."""

    row: np.ndarray  # int32
    descendant: np.ndarray  # int32, -1 when none
    central: np.ndarray  # int32 FirstHaloInFOFgroup
    next_progenitor: np.ndarray  # int32
    most_bound_id: np.ndarray  # int64
    mass: np.ndarray  # float32 M_Crit200

    def lookup(self, rows: np.ndarray, what: str, path) -> np.ndarray:
        """Positions of ``rows`` among the retained rows.

        Raises:
            ConverterError: for a row that was not retained.
        """
        rows = np.asarray(rows, dtype=np.int64)
        at = np.minimum(np.searchsorted(self.row, rows), max(self.row.size - 1, 0))
        if rows.size and (self.row.size == 0 or (self.row[at] != rows).any()):
            raise ConverterError(
                "{}: a {} is not among the rows of the touched trees".format(path, what)
            )
        return at


def _chain_changes(
    snap: int,
    states: Sequence[RuleState],
    promoted: Sequence[np.ndarray],
    affected: Sequence[np.ndarray],
    kept: Retained,
    path,
    batch_rows: int = DEFAULT_BLOCK_ROWS,
) -> Tuple[List[Dict], Dict]:
    """Per rule, the descendants (rows of the next slab) whose progenitor chain
    changes and how many change their first progenitor, and the stored-chain
    check over every affected descendant with two or more progenitors.

    Working memory (measured with ``tracemalloc``): the slab's affected
    siblings are first found and sorted by descendant into one int32 index
    over the retained rows, about **29 B per affected sibling** while it is
    built and 4 B each while it is held across the batches; the chains are
    then worked in batches of whole descendants, each starting at the first
    group at or after a multiple of ``batch_rows`` records, so a batch holds at
    most ``batch_rows + g_max - 1`` records for the slab's largest progenitor
    group of ``g_max`` records, at about **200 B per record**. The batch spans
    more than ``batch_rows`` records only when one descendant's progenitors
    cross a batch boundary, and the whole slab's affected siblings only when
    they all belong to one descendant."""
    changed: List[List[np.ndarray]] = [[] for _state in states]
    head_changed = [0] * len(states)
    check = {"descendants": 0, "mismatches": 0, "examples": []}
    union = np.unique(np.concatenate(affected)) if affected else np.zeros(0, dtype=np.int64)
    if union.size == 0:
        return [{"changed": np.zeros(0, dtype=np.int64), "head_changed": 0} for _s in states], check
    # the affected descendants' progenitors, found block by block
    found = []
    for start in range(0, kept.row.size, batch_rows):
        part = kept.descendant[start : start + batch_rows].astype(np.int64)
        found.append(start + np.flatnonzero(in_sorted(union, part)))
    picked = drain(found).astype(np.int32)  # positions among the retained rows
    picked = picked[np.argsort(kept.descendant[picked], kind="stable")]
    grouped = kept.descendant[picked]
    starts = np.flatnonzero(np.r_[True, grouped[1:] != grouped[:-1]]) if picked.size else picked
    sizes = np.diff(np.r_[starts, picked.size])
    several = np.repeat(sizes >= 2, sizes)
    picked, grouped = picked[several], grouped[several]
    del starts, sizes, several
    starts = np.flatnonzero(np.r_[True, grouped[1:] != grouped[:-1]]) if picked.size else picked
    # each batch starts at the first group starting at or after a multiple of batch_rows
    edges = np.r_[starts, picked.size]
    bounds = np.unique(
        np.r_[edges[np.searchsorted(starts, np.arange(0, picked.size, batch_rows))], picked.size]
    )
    del grouped, starts, edges
    for low, high in zip(bounds[:-1], bounds[1:]):
        batch = picked[low:high]
        groups = kept.descendant[batch].astype(np.int64)
        rows = kept.row[batch].astype(np.int64)
        central = kept.central[batch].astype(np.int64)
        ids = kept.most_bound_id[batch]
        mass = kept.mass[batch]
        satellite = central != rows
        upid = ids.copy()
        upid[satellite] = kept.most_bound_id[
            kept.lookup(central[satellite], "progenitor's FoF central", path)
        ]
        pid = np.where(satellite, upid, -1)
        before = chain_positions(groups, upid, pid, ids, mass)
        mismatched = stored_chain_mismatches(
            groups, rows, kept.next_progenitor[batch].astype(np.int64), before
        )
        check["descendants"] += int(np.unique(groups).size)
        check["mismatches"] += int(mismatched.size)
        check["examples"].extend(
            {"snapshot": snap + 1, "descendant_row": int(row)}
            for row in mismatched[: N_EXAMPLES - len(check["examples"])]
        )
        for at in range(len(states)):
            moved = in_sorted(promoted[at], rows)
            if not moved.any():
                continue
            after = chain_positions(
                groups, np.where(moved, ids, upid), np.where(moved, -1, pid), ids, mass
            )
            changed[at].append(np.unique(groups[before != after]))
            head_changed[at] += int(np.unique(groups[(before == 0) & (after != 0)]).size)
    results = [
        {
            "changed": (np.concatenate(parts) if parts else np.zeros(0, dtype=np.int64)),
            "head_changed": heads,
        }
        for parts, heads in zip(changed, head_changed)
    ]
    return results, check


def severance_pass(
    dataset: HorizontalDataset,
    aggregate_dir,
    forests: np.ndarray,
    local_of: np.ndarray,
    states: Sequence[RuleState],
    predicted: Sequence[Sequence[int]],
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
    meter: Optional[ReadMeter] = None,
) -> Dict:
    """The fourth read (module docstring, step 3): fills each state's
    ``per_snapshot`` rows and ``seeded`` pieces; returns the stored-chain check.

    Slabs are visited in ascending order so that dependents propagate forward
    along ``Descendant``. Snapshot N's row counts its promotions, groups and
    seeds, and in ``progenitor_order_changed`` the descendants at N + 1 whose
    chain over N's progenitors changes (they are seeds of N + 1). Each slab is
    read once: every named-forest row's ``FirstHaloInFOFgroup`` and
    ``Descendant``, and the retained rows' (:func:`touched_trees`) three
    further columns over each block's span of them; nothing is re-read.

    Raises:
        ConverterError: on a promotion count that disagrees with the slab's
            edge list, a dependent outside the named forests' rows, or a row
            or label inconsistency.
    """
    check = {"descendants": 0, "mismatches": 0, "examples": []}
    n_local = int(np.count_nonzero(local_of >= 0))
    for snap in dataset.snapshots:
        path = dataset.snapshot_path(snap)
        if dataset.n_halos[snap] > _INT32_MAX:
            raise ConverterError("{}: more rows than int32 row indices hold".format(path))
        labels = load_labels(aggregate_dir, snap)
        try:
            _severed, retain = touched_trees(
                load_slab_edges(aggregate_dir, snap, mmap=False), local_of, states, n_local
            )
        except ConverterError as exc:
            raise ConverterError("{}: {}".format(path, exc)) from exc
        for state in states:
            state.current = np.unique(np.concatenate(state.following + [state.current[:0]]))
            state.current_seeds = np.unique(
                np.concatenate(state.following_seeds + [state.current[:0]])
            )
            state.following, state.following_seeds = [], []
        parts = [[] for _state in states]
        found = [0] * len(states)
        kept_parts: Dict[str, List[np.ndarray]] = {name: [] for name in Retained.__annotations__}
        for rows, values in iter_forest_rows(
            dataset, snap, forests, PASS_COLUMNS, block_rows, meter
        ):
            central = check_central_rows(values["FirstHaloInFOFgroup"], dataset.n_halos[snap], path)
            desc = values["Descendant"].astype(np.int64)
            own = local_of[np.asarray(labels[rows])]
            host = local_of[np.asarray(labels[central])]
            if (own < 0).any() or (host < 0).any():
                raise ConverterError(
                    "{}: a named-forest halo, or its FoF central, is labelled with a tree outside "
                    "the named forests".format(path)
                )
            member = central != rows
            for at, state in enumerate(states):
                own_piece = state.tree_piece[own]
                host_piece = state.tree_piece[host]
                moved = member & (own_piece != host_piece)
                if moved.any():
                    parts[at].append(
                        (
                            rows[moved].astype(np.int32),
                            central[moved].astype(np.int32),
                            desc[moved].astype(np.int32),
                            own_piece[moved].astype(np.int32),
                            host_piece[moved].astype(np.int32),
                        )
                    )
                if state.current.size:
                    # rows ascend: search only the dependents within the block's row range
                    hit = in_sorted_window(state.current, rows)
                    found[at] += int(np.count_nonzero(hit))
                    onward = desc[hit]
                    state.following.append(onward[onward >= 0])
            keep = np.flatnonzero(retain[own])
            if keep.size:
                low, high = int(rows[keep[0]]), int(rows[keep[-1]]) + 1
                extra = read_span(dataset, snap, RETAINED_COLUMNS, low, high, meter)
                offset = rows[keep] - low
                kept_parts["row"].append(rows[keep].astype(np.int32))
                kept_parts["descendant"].append(desc[keep].astype(np.int32))
                kept_parts["central"].append(central[keep].astype(np.int32))
                kept_parts["next_progenitor"].append(
                    extra["NextProgenitor"][offset].astype(np.int32)
                )
                kept_parts["most_bound_id"].append(extra["MostBoundID"][offset].astype(np.int64))
                kept_parts["mass"].append(extra[MASS_COLUMN][offset].astype(np.float32))
        # each field's parts released as it is joined
        kept = Retained(**{name: drain(values) for name, values in kept_parts.items()})
        del kept_parts

        promoted, affected = [], []
        for at, state in enumerate(states):
            if found[at] != state.current.size:
                raise ConverterError(
                    "{}: {} dependent row(s) are not among the named forests' rows".format(
                        path, state.current.size - found[at]
                    )
                )
            if parts[at]:
                rows_p, central_p, desc_p, own_p, host_p = (
                    np.concatenate(column).astype(np.int64) for column in zip(*parts[at])
                )
            else:
                rows_p = central_p = desc_p = own_p = host_p = np.zeros(0, dtype=np.int64)
            parts[at] = None
            if rows_p.size != predicted[at][snap]:
                raise ConverterError(
                    "{}: rule {} promotes {} halo(s) but the slab's edge list predicts {}; the "
                    "graph aggregates are stale".format(
                        path, state.rule.name, rows_p.size, predicted[at][snap]
                    )
                )
            losing = np.unique(central_p)
            remnant_members = np.unique(pair_keys(central_p, own_p), return_counts=True)[1]
            state.seeded[own_p] = True
            state.seeded[host_p] = True
            seeds = np.unique(np.concatenate([rows_p, losing, state.current_seeds]))
            affected_rows = np.unique(np.concatenate([state.current, seeds]))
            state.following.append(desc_p[desc_p >= 0])
            onward = kept.descendant[kept.lookup(losing, "central losing members", path)]
            state.following.append(onward[onward >= 0].astype(np.int64))
            state.per_snapshot.append(
                {
                    "snapshot": snap,
                    "promoted_halos": int(rows_p.size),
                    "groups_losing_members": int(losing.size),
                    "groups_central_leaves_members_stay": int(remnant_members.size),
                    "remnants_with_several_members": int(np.count_nonzero(remnant_members >= 2)),
                    "seed_halos": int(seeds.size),
                    "affected_halos": int(affected_rows.size),
                }
            )
            promoted.append(rows_p)
            affected.append(np.unique(desc_p[desc_p >= 0]))

        changes, slab_check = _chain_changes(
            snap, states, promoted, affected, kept, path, block_rows
        )
        del kept
        check["descendants"] += slab_check["descendants"]
        check["mismatches"] += slab_check["mismatches"]
        check["examples"].extend(slab_check["examples"][: N_EXAMPLES - len(check["examples"])])
        for state, change in zip(states, changes):
            changed = change["changed"]
            state.following.append(changed)
            state.following_seeds.append(changed)
            state.per_snapshot[-1]["progenitor_order_changed"] = int(changed.size)
            state.per_snapshot[-1]["first_progenitor_changed"] = int(change["head_changed"])
        log(
            "cut: snapshot {} evaluated for {} rule(s) at {}".format(
                snap, len(states), time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            )
        )
    return check


# ---- partition with the pieces installed ------------------------------------------


def aggregate_slab(
    aggregate_dir,
    snap: int,
    local_of: np.ndarray,
    states: Sequence[RuleState],
    points: Sequence[List[Dict]],
    predicted: Sequence[List[int]],
    n_forests: int,
    catalogue_max: int,
) -> None:
    """One slab of step 2 (module docstring), from the aggregates alone: every
    rule's piece peaks, the promotions the slab's edge list predicts, and the
    partition grid's ranges with the pieces installed. Its buffers (the slab's
    tree pairs, occupancy pairs and edge list with their gathers) are released
    on return, before the pass."""
    local, counts = slab_local_counts(aggregate_dir, snap, local_of)
    edges = load_slab_edges(aggregate_dir, snap, mmap=False)
    edge_lo, edge_hi = edge_locals(edges, local_of, snap)
    forests_s, counts_s = load_slab_pairs(aggregate_dir, snap)
    for at, (state, per_piece) in enumerate(zip(states, piece_counts(states, local, counts))):
        # ascending slabs and a strict test: a tie keeps the lowest-numbered slab
        better = per_piece > state.peak
        state.peak[better] = per_piece[better]
        state.peak_snapshot[better] = snap
        severed = state.tree_piece[edge_lo] != state.tree_piece[edge_hi]
        predicted[at].append(int(edges["halos"][severed].sum()))
        installed = install_pieces(forests_s, counts_s, state, per_piece, n_forests, catalogue_max)
        apply_points(points[at], snap, *installed)


def partition_points(
    aggregate_dir,
    state: RuleState,
    widest: int,
    n_forests: int,
    catalogue_max: int,
    ntasks: Sequence[int],
    nchunks: Sequence[int],
) -> List[Dict]:
    """Each grid point's forest cuts with the widest slab's installed counts as weights."""
    forests, counts = load_slab_pairs(aggregate_dir, widest)
    new_forests, new_counts = install_pieces(
        forests, counts, state, state.widest_counts, n_forests, catalogue_max
    )
    weights = np.zeros(n_forests + int(np.count_nonzero(~state.pieces["keeps_id"])), dtype=np.int64)
    weights[new_forests] = new_counts
    points = []
    for ntask in ntasks:
        for nchunk in nchunks:
            points.append(
                {
                    "ntask": int(ntask),
                    "nchunk": int(nchunk),
                    "forest_cuts": partition_cut(weights, ntask, nchunk),
                    "widest_rows_per_snapshot": [],
                    "widest": {"snapshot": None, "range": None, "rows": -1},
                    "range_peak_rows": [0] * (int(ntask) * int(nchunk)),
                }
            )
    return points


def apply_points(points: List[Dict], snap: int, forests: np.ndarray, counts: np.ndarray) -> None:
    """Record every grid point's widest range in one slab (a tie keeps the
    lowest-numbered slab, as slabs are applied in ascending order) and each
    range's largest slab so far."""
    for point in points:
        rows = range_rows(forests, counts, point["forest_cuts"])
        point["range_peak_rows"] = np.maximum(point["range_peak_rows"], rows).tolist()
        at = int(np.argmax(rows)) if rows.size else 0
        widest = int(rows[at]) if rows.size else 0
        point["widest_rows_per_snapshot"].append(widest)
        if widest > point["widest"]["rows"]:
            point["widest"] = {
                "snapshot": snap,
                "range": at,
                "task": at // point["nchunk"],
                "chunk": at % point["nchunk"],
                "rows": widest,
            }


def laptop_rows(points: List[Dict], bytes_per_halo: int, laptop_gib: Sequence[int]) -> List[Dict]:
    """Per laptop class, whether any grid point's **job** fits its whole memory
    at ``bytes_per_halo``, and the smallest such point (fewest ranges, then
    fewest tasks).

    On one laptop the ranks of an ``mpirun`` job run at once, each holding its
    own chunk, so a class is judged on the job's memory, not one process's
    (``docs/USER-GUIDE.md``: ranks cut per-process memory but not the job's
    total). The driver's ranks do not synchronise during the sweep -- each
    sweeps its chunks through every snapshot on its own, with no collective
    between the startup broadcast and the end -- so two ranks can be at their
    widest slabs at the same moment, and the per-slab sum over ranks is not a
    safe bound. The job figure is therefore the conservative one: the sum over
    tasks of each task's widest chunk over all slabs (``task_widest_rows``).
    The per-process figure (the widest range over all slabs) is kept as the
    per-rank ``retention_memory_ceiling_mb`` guidance (MB of 1024^2 bytes, as
    the driver reads it)."""
    for point in points:
        peaks = np.asarray(point.pop("range_peak_rows"), dtype=np.int64).reshape(
            point["ntask"], point["nchunk"]
        )
        point["task_widest_rows"] = peaks.max(axis=1).tolist()
        point["job_rows"] = int(peaks.max(axis=1).sum())
        point["process_bytes"] = point["widest"]["rows"] * int(bytes_per_halo)
        point["job_bytes"] = point["job_rows"] * int(bytes_per_halo)
        point["retention_memory_ceiling_mb"] = -(-point["process_bytes"] // (1 << 20))
        point["fits_gib"] = [
            int(gib) for gib in laptop_gib if point["job_bytes"] <= int(gib) * _GIB
        ]
    rows = []
    for gib in laptop_gib:
        budget = int(gib) * _GIB
        fitting = [p for p in points if p["job_bytes"] <= budget]
        best = min(fitting, key=lambda p: (p["ntask"] * p["nchunk"], p["ntask"]), default=None)
        rows.append(
            {
                "class_gib": int(gib),
                "budget_bytes": budget,
                "feasible": best is not None,
                "smallest_fitting_point": (
                    None
                    if best is None
                    else {
                        "ntask": best["ntask"],
                        "nchunk": best["nchunk"],
                        "widest_rows": best["widest"]["rows"],
                        "job_rows": best["job_rows"],
                        "process_bytes": best["process_bytes"],
                        "job_bytes": best["job_bytes"],
                        "retention_memory_ceiling_mb": best["retention_memory_ceiling_mb"],
                    }
                ),
            }
        )
    return rows


# ---- summaries --------------------------------------------------------------------


def _piece_entry(pieces: Dict[str, np.ndarray], state: RuleState, at: int) -> Dict:
    return {
        "id": int(pieces["id"][at]),
        "forest_id": int(pieces["forest_id"][at]),
        "trees": int(pieces["trees"][at]),
        "halos": int(pieces["halos"][at]),
        "smallest_root_id": int(pieces["smallest_root_id"][at]),
        "peak_occupancy": int(state.peak[at]),
        "peak_snapshot": int(state.peak_snapshot[at]),
    }


def describe_pieces(state: RuleState, forest_ids: np.ndarray) -> List[Dict]:
    """Per named forest: whether the rule cuts it, its pieces, the piece keeping
    its id, and the largest fresh pieces."""
    pieces = state.pieces
    records = []
    for forest in np.unique(pieces["forest_index"]):
        mine = np.flatnonzero(pieces["forest_index"] == forest)
        kept = mine[pieces["keeps_id"][mine]]
        fresh = mine[~pieces["keeps_id"][mine]]
        fresh = fresh[np.argsort(pieces["id"][fresh], kind="stable")]
        records.append(
            {
                "forest_index": int(forest),
                "forest_id": int(forest_ids[forest]),
                "cut": bool(mine.size > 1),
                "pieces": int(mine.size),
                "single_tree_pieces": int(np.count_nonzero(pieces["trees"][mine] == 1)),
                "kept_piece": _piece_entry(pieces, state, int(kept[0])),
                "largest_fresh_pieces": [
                    _piece_entry(pieces, state, int(at)) for at in fresh[:N_LARGEST]
                ],
            }
        )
    return records


def edge_statistics(
    pairs: np.ndarray, forest_trees: np.ndarray, state: RuleState, block_rows: int
) -> Dict:
    """The pairs a rule keeps and drops, and the dropped pairs whose trees fall
    in different pieces (severed)."""
    totals = {
        name: {"pairs": 0, "halos": 0, "mass": 0.0, "max_snapshots": 0}
        for name in ("kept", "dropped", "severed")
    }
    for start in range(0, pairs.size, block_rows):
        part = pairs[start : start + block_rows]
        kept = state.rule.keep(part["snapshots"], part["halos"], part["mass"])
        piece_lo = state.tree_piece[local_index(forest_trees, part["lo"])]
        piece_hi = state.tree_piece[local_index(forest_trees, part["hi"])]
        for name, mask in (
            ("kept", kept),
            ("dropped", ~kept),
            ("severed", piece_lo != piece_hi),
        ):
            if not mask.any():
                continue
            totals[name]["pairs"] += int(np.count_nonzero(mask))
            totals[name]["halos"] += int(part["halos"][mask].sum())
            totals[name]["mass"] += float(part["mass"][mask].sum())
            totals[name]["max_snapshots"] = max(
                totals[name]["max_snapshots"], int(part["snapshots"][mask].max())
            )
    return totals


# ---- the subcommand ------------------------------------------------------------------


def run_cut(
    dataset: HorizontalDataset,
    aggregate_dir,
    index_roots: np.ndarray,
    index_forest_ids: np.ndarray,
    index_files: Dict,
    rules: Sequence[Rule],
    materialise: Sequence[Rule] = (),
    ntasks: Sequence[int] = DEFAULT_NTASKS,
    nchunks: Sequence[int] = DEFAULT_NCHUNKS,
    bytes_per_halo: int = DEFAULT_BYTES_PER_HALO,
    laptop_gib: Sequence[int] = DEFAULT_LAPTOP_GIB,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
) -> Dict:
    """Evaluate every rule (module docstring) and write the aggregates and,
    for the materialised rules, the tables and records; returns the summary.

    ``index_roots`` and ``index_forest_ids`` are the index files' tree roots
    (ascending) and their forest ids (:class:`source_index.SourceIndex`'s
    ``tree_roots`` and ``forest_ids``); ``index_files`` is the record of the
    files (paths, sizes and md5, computed once by the caller) that every
    table record carries.

    Raises:
        ConverterError: on any :func:`prepare_cut` refusal, index files that
            do not describe this census when a table is to be written
            (:func:`check_index`, before any output is touched; otherwise the
            check is recorded), a table invariant, or a refusal of the pass.
    """
    inputs = prepare_cut(dataset, aggregate_dir, rules, materialise)
    if index_roots.size == 0:
        raise ConverterError("the index files list no tree")
    identity = inputs["identity"]
    forests = inputs["forests"]
    forest_ids = dataset.forest_ids()
    roots = load_roots(aggregate_dir)
    tree_forest = load_array(trees_dir(aggregate_dir) / "tree_forest.npy", mmap=True)
    tree_totals = load_array(trees_dir(aggregate_dir) / "tree_totals.npy", mmap=True)
    index_check = check_index(
        index_roots, index_forest_ids, roots, tree_forest, forest_ids, block_rows
    )
    if materialise and index_check["verdict"] != "pass":
        raise ConverterError("{}; no cut table is written".format(index_check["message"]))
    if index_check["verdict"] != "pass":
        log("cut: index check failed, exploring only: {}".format(index_check["message"]))
    out = begin(cut_dir(aggregate_dir))
    n_forests = dataset.n_forests_total
    # one fresh-id floor for naming, installing and the summary: above every
    # id of the supplied index and of the dataset, so a fresh piece can never
    # take an existing forest's id even when the index check failed (equal
    # when it passed)
    catalogue_max = max(int(index_forest_ids.max()), int(forest_ids.max()))
    forest_trees = load_forest_trees(aggregate_dir)
    local_forest = np.asarray(tree_forest[forest_trees], dtype=np.int64)
    local_totals = np.asarray(tree_totals[forest_trees], dtype=np.int64)
    local_roots = roots[forest_trees]
    local_of = np.full(roots.size, -1, dtype=np.int32)
    local_of[forest_trees] = np.arange(forest_trees.size, dtype=np.int32)
    pairs = load_pairs(aggregate_dir)

    states: List[RuleState] = []
    assignment_bytes = 0
    for rule in rules:
        components = rule_components(pairs, forest_trees, rule, block_rows)
        pieces = name_pieces(
            components, local_forest, local_totals, local_roots, forest_ids, catalogue_max
        )
        del components
        state = RuleState(rule=rule, pieces=pieces, tree_piece=pieces.pop("piece").astype(np.int32))
        assignment_bytes += save_array(
            rule_dir(aggregate_dir, rule) / "assignment.npy", pieces["id"][state.tree_piece]
        )
        states.append(state)
        log("cut: rule {} -- {} piece(s)".format(rule.name, pieces["id"].size))

    # aggregates only: piece peaks, predicted promotions, the partition
    widest = int(inputs["occupancy"]["widest_slab"]["snapshot"])
    local, counts = slab_local_counts(aggregate_dir, widest, local_of)
    for state, per_piece in zip(states, piece_counts(states, local, counts)):
        state.widest_counts = per_piece
    points = [
        partition_points(aggregate_dir, s, widest, n_forests, catalogue_max, ntasks, nchunks)
        for s in states
    ]
    predicted = [[] for _state in states]
    for snap in dataset.snapshots:
        aggregate_slab(
            aggregate_dir, snap, local_of, states, points, predicted, n_forests, catalogue_max
        )

    meter = ReadMeter()
    check = severance_pass(
        dataset, aggregate_dir, forests, local_of, states, predicted, block_rows, log, meter
    )

    sizes = {"assignments": assignment_bytes, "tables": 0, "records": 0}
    tables = {}
    named = {
        "forest_index": [int(f) for f in forests],
        "forest_id": [int(forest_ids[f]) for f in forests],
        "graph_forests": [int(f) for f in inputs["graph"]["forests"]],
    }
    if materialise:
        for state in states:
            if state.rule.name not in {rule.name for rule in materialise}:
                continue
            ids = table_ids(index_forest_ids, forest_trees, state.pieces["id"][state.tree_piece])
            check_table(index_roots, ids, index_roots, index_forest_ids, tree_totals)
            directory = rule_dir(aggregate_dir, state.rule)
            md5, table_bytes = write_table(directory / "forests.list", index_roots, ids)
            del ids
            table = {
                "path": str((directory / "forests.list").resolve()),
                "md5": md5,
                "bytes": table_bytes,
                "rows": int(index_roots.size),
            }
            header = {
                "dataset": identity,
                "index_files": index_files,
                "rule": state.rule.record(),
                "forests": named,
                "table": table,
            }
            pieces = dict(
                state.pieces, peak_occupancy=state.peak, peak_snapshot=state.peak_snapshot
            )
            sizes["records"] += write_table_record(directory / "record.json", header, pieces)
            sizes["tables"] += table_bytes
            tables[state.rule.name] = table
            log("cut: table for rule {} written, md5 {}".format(state.rule.name, md5))

    forest_totals = load_array(occupancy_dir(aggregate_dir) / "forest_totals.npy", mmap=True)
    rule_summaries = []
    for state, rule_points in zip(states, points):
        pieces = state.pieces
        cut_forests = np.unique(pieces["forest_index"][~pieces["keeps_id"]])
        in_cut = np.isin(pieces["forest_index"], cut_forests)
        fresh = ~pieces["keeps_id"]
        first_cut = int(cut_forests[0]) if cut_forests.size else None
        rows = state.per_snapshot
        rule_summaries.append(
            {
                "rule": state.rule.record(),
                "components": int(pieces["id"].size),
                "pieces": {
                    "count": int(pieces["id"].size),
                    "fresh": int(np.count_nonzero(fresh)),
                    "per_forest": describe_pieces(state, forest_ids),
                },
                "edges": edge_statistics(pairs, forest_trees, state, block_rows),
                "severance": {
                    "promoted_halos": sum(r["promoted_halos"] for r in rows),
                    "groups_losing_members": sum(r["groups_losing_members"] for r in rows),
                    "groups_central_leaves_members_stay": sum(
                        r["groups_central_leaves_members_stay"] for r in rows
                    ),
                    "remnants_with_several_members": sum(
                        r["remnants_with_several_members"] for r in rows
                    ),
                    "per_snapshot": rows,
                    "definitions": SEVERANCE_DEFINITIONS,
                },
                "progenitor_order": {
                    "descendants_changed": sum(r["progenitor_order_changed"] for r in rows),
                    "first_progenitor_changed": sum(r["first_progenitor_changed"] for r in rows),
                },
                "relabelled": {
                    "forest_id_changed_halos": int(pieces["halos"][fresh].sum()),
                    "rank_recomputed_halos": int(pieces["halos"][in_cut].sum()),
                    "source_halo_id_shifted_halos": (
                        0 if first_cut is None else int(np.asarray(forest_totals[first_cut:]).sum())
                    ),
                },
                "predicted_effects": {
                    "sage16_halos_only": {
                        "seed_halos": sum(r["seed_halos"] for r in rows),
                        "dependent_halos": sum(r["affected_halos"] for r in rows),
                        "upper_bound_halos": int(pieces["halos"][state.seeded].sum()),
                        "definition": "seeds are the promoted halos, the centrals of groups that "
                        "lose members and the descendants whose progenitor chain changes; "
                        "dependents are every halo on a seed's descendant path, seeds included; "
                        "the upper bound is every halo of a piece holding a seed",
                    },
                    "hod_sham": {
                        "halos": int(pieces["halos"][in_cut].sum()),
                        "definition": "every halo of every forest the rule cuts: the ids their "
                        "draws and tie-breaks key on change",
                    },
                },
                "partition": {
                    "bytes_per_halo": int(bytes_per_halo),
                    "grid": rule_points,
                    "laptop_classes": laptop_rows(rule_points, bytes_per_halo, laptop_gib),
                },
                "table": tables.get(state.rule.name),
            }
        )

    summary = {
        "dataset": identity,
        "dataset_dir": str(dataset.directory.resolve()),
        "forests": [int(f) for f in forests],
        "named_forests": named,
        "index_files": index_files,
        "catalogue_max_forest_id": catalogue_max,
        "fresh_id_floor": {
            "value": catalogue_max,
            "index_max_forest_id": int(index_forest_ids.max()),
            "dataset_max_forest_id": int(forest_ids.max()),
            "note": "fresh piece ids start at value + 1: the larger of the supplied index's and "
            "the dataset sidecar's largest forest id",
        },
        "root_correspondence": inputs["trees"]["root_correspondence"]["verdict"],
        "index_check": index_check,
        "stored_chain_check": check,
        "io": meter.record(
            "per slab, 8 B x n_halos(s) of ForestIndex, plus 2 x link width x the named forests' "
            "row span(s) of FirstHaloInFOFgroup and Descendant, plus (8 + 4 + link width) B x the "
            "retained rows' span(s) of MostBoundID, M_Crit200 and NextProgenitor; link width 4 B "
            "in version 2, 8 B in version 3"
        ),
        "rules": rule_summaries,
        "aggregates": {
            "assignments": {
                "formula": "8 B per named-forest tree per rule (int64 piece id), plus a 128 B "
                ".npy header per file",
                "trees": int(forest_trees.size),
                "rules": len(states),
                "bytes": sizes["assignments"],
            },
            "tables": {
                "formula": "one forests.list-shaped row per catalogue tree for each materialised "
                "rule (about 24 B per tree at Shin-Uchuu's id widths)",
                "materialised": sorted(tables),
                "bytes": sizes["tables"],
            },
            "records": {
                "formula": "per materialised rule, a header of a few kilobytes plus, per piece, "
                "the sum over its six columns of (decimal digits + 1) bytes, about 40 B per piece "
                "at Shin-Uchuu's id widths; written in chunks of 2^16 pieces, about 16 B per "
                "piece resident",
                "pieces": int(sum(s.pieces["id"].size for s in states if s.rule.name in tables)),
                "bytes": sizes["records"],
            },
        },
    }
    write_json(out / SUMMARY_NAME, summary)
    return summary
