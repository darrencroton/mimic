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
   overall, its implied memory at the stated bytes per resident halo, and per
   laptop class a row saying whether any grid point fits and the smallest that
   does;
3. from one further pass over the named forests' rows of every slab with the
   labels mapped (the census's fourth read of those columns, bounded like the
   others; every rule is evaluated in the same pass):

   - **severed co-memberships**: the pairs the rule drops and, of those, the
     pairs whose trees fall in different pieces (severed); per slab, the halos
     **promoted** to central (members whose central lies in another piece,
     F6), the groups that lose members, and the **groups whose central leaves
     while members stay**, counted as (group, piece) remnants: a piece's
     members of one group whose central is in another piece, each of which
     becomes its own central (no host is invented). The promotions are
     checked against those the slab's edge list predicts;
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
only for the rules asked for (``materialise``) and only when the ``trees``
root correspondence verdict passed; they live under ``cut/rules/<rule>/`` as
``forests.list`` and ``record.json``.

Resident memory, with ``n`` all trees, ``t`` the named forests' trees, ``F``
the forests, ``R`` the rules and ``p_r`` rule ``r``'s pieces: the dense tree to
local-index map (4 B x n, about 1.26 GB at Shin-Uchuu scale), the roots (8 B
x n, 2.5 GB), the named-forest tree arrays (about 32 B x t, 3.4 GB for the
super-forest) and per rule the piece of each tree (8 B x t) and about 50 B per
piece; the union-find of ``census/rules.py``; one slab's tree pairs,
occupancy pairs and edge list (8, 12 and 20 B per entry) with the dense
partition weights (8 B x (F + fresh pieces), 1.3 GB at Shin-Uchuu scale); in
the pass (measured with ``tracemalloc``), the block's columns, label gathers
and transients, about **115 B x block_rows** for one rule (0.48 GB at the
default 2^22) and 17 B x block_rows more per further rule (its two piece
gathers and mask), the promoted rows and the sibling records of the affected
descendants, about **100 B per promoted or sibling row**, and the descendant
sets being propagated (8 B per affected halo of the slab). The
:class:`source_index.SourceIndex` holds 32 B per catalogue tree throughout,
and while a table is checked and written ``census/cut_table.py`` adds about
66 B per catalogue tree.
No array of a slab's length is held beyond the column blocks and the mapped
labels, and no tree x snapshot or forest x snapshot matrix.

Aggregates under ``<aggregate>/cut/``: per rule ``assignment.npy`` (**8 B x
t**, about 0.84 GB per rule for the super-forest), and for a materialised
rule ``forests.list`` (the catalogue's rows, about 24 B per tree) and
``record.json`` (about 80 B per piece); ``summary.json`` (a few kilobytes per
rule plus about 10 integers per rule, grid point and slab).
"""

import os
import sys
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
    table_record,
    write_table,
)
from census.graph import (  # noqa: E402
    MASS_COLUMN,
    check_central_rows,
    graph_dir,
    in_sorted,
    iter_forest_rows,
    load_forest_trees,
    load_pairs,
    load_slab_edges,
    local_index,
    pair_keys,
    read_at,
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
from source_index import SourceIndex  # noqa: E402

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
    "group whose central lies in another piece",
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
    return {
        "identity": identity,
        "occupancy": occupancy,
        "trees": trees,
        "graph": graph,
        "forests": np.asarray(graph["forests"], dtype=np.int64),
    }


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
    (``links.build_progenitor_links``)."""
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


def stored_chain(rows: np.ndarray, next_rows: np.ndarray) -> Optional[List[int]]:
    """The positions of ``rows`` in the order their ``NextProgenitor`` links
    chain them, or ``None`` when the links do not form one chain over them."""
    position = {int(row): at for at, row in enumerate(rows)}
    targets = {int(target) for target in next_rows if int(target) != -1}
    heads = [at for at, row in enumerate(rows) if int(row) not in targets]
    if len(heads) != 1:
        return None
    chain = [heads[0]]
    while len(chain) <= rows.size:
        target = int(next_rows[chain[-1]])
        if target == -1:
            break
        if target not in position:
            return None
        chain.append(position[target])
    return chain if len(chain) == rows.size and len(set(chain)) == rows.size else None


def encounter(upid: np.ndarray, pid: np.ndarray, ids: np.ndarray) -> np.ndarray:
    """Positions in ascending (upid, pid, id)."""
    return np.lexsort((ids, pid, upid))


# ---- the pass ------------------------------------------------------------------


def _sibling_records(
    dataset: HorizontalDataset,
    snap: int,
    descendants: np.ndarray,
    span: Tuple[int, int],
    block_rows: int,
) -> Dict[str, np.ndarray]:
    """The rows of one slab whose ``Descendant`` is among ``descendants``
    (ascending), with the columns their chains need, read in bounded blocks
    over the named forests' row span."""
    names = ("MostBoundID", MASS_COLUMN, "FirstHaloInFOFgroup", "NextProgenitor")
    parts: Dict[str, List[np.ndarray]] = {name: [] for name in names + ("row", "Descendant")}
    low, high = span
    for start in range(low, high, block_rows):
        stop = min(start + block_rows, high)
        desc = dataset.read_rows(snap, ("Descendant",), start, stop)["Descendant"]
        hit = np.flatnonzero(in_sorted(descendants, desc.astype(np.int64, copy=False)))
        if hit.size == 0:
            continue
        first, last = int(hit[0]), int(hit[-1]) + 1
        columns = dataset.read_rows(snap, names, start + first, start + last)
        for name in names:
            parts[name].append(columns[name][hit - first])
        parts["row"].append(start + hit.astype(np.int64))
        parts["Descendant"].append(desc[hit].astype(np.int64))
    return {
        name: (np.concatenate(values) if values else np.zeros(0)) for name, values in parts.items()
    }


def _chain_changes(
    dataset: HorizontalDataset,
    snap: int,
    states: Sequence[RuleState],
    promoted: Sequence[np.ndarray],
    affected: Sequence[np.ndarray],
    span: Tuple[int, int],
    block_rows: int,
) -> Tuple[List[Dict], Dict]:
    """Per rule, the descendants (rows of the next slab) whose progenitor chain
    changes, and the stored-chain check over every affected descendant."""
    results = [{"changed": [], "head_changed": 0} for _state in states]
    check = {"descendants": 0, "mismatches": 0, "examples": []}
    union = np.unique(np.concatenate(affected)) if affected else np.zeros(0, dtype=np.int64)
    if union.size == 0:
        return results, check
    sib = _sibling_records(dataset, snap, union, span, block_rows)
    rows = sib["row"].astype(np.int64)  # the promoted rows themselves are always among them
    central = check_central_rows(
        sib["FirstHaloInFOFgroup"], dataset.n_halos[snap], dataset.snapshot_path(snap)
    )
    ids = sib["MostBoundID"].astype(np.int64)
    mass = sib[MASS_COLUMN]
    satellite = central != rows
    hosts = np.unique(central[satellite])
    host_ids = read_at(dataset, snap, "MostBoundID", hosts, block_rows).astype(np.int64)
    upid = ids.copy()
    upid[satellite] = host_ids[np.searchsorted(hosts, central[satellite])]
    pid = np.where(satellite, upid, -1)

    order = np.lexsort((rows, sib["Descendant"]))
    desc_sorted = sib["Descendant"][order]
    starts = np.flatnonzero(np.r_[True, desc_sorted[1:] != desc_sorted[:-1]])
    ends = np.r_[starts[1:], order.size]
    for begin_at, end_at in zip(starts, ends):
        group = order[begin_at:end_at]
        if group.size < 2:
            continue
        descendant = int(desc_sorted[begin_at])
        before = [int(group[i]) for i in encounter(upid[group], pid[group], ids[group])]
        chain_before = chain_order(before, mass)
        check["descendants"] += 1
        stored = stored_chain(rows[group], sib["NextProgenitor"][group].astype(np.int64))
        if stored is None or [int(group[i]) for i in stored] != chain_before:
            check["mismatches"] += 1
            if len(check["examples"]) < N_EXAMPLES:
                check["examples"].append({"snapshot": snap + 1, "descendant_row": descendant})
        for state_at in range(len(states)):
            if not in_sorted(affected[state_at], np.array([descendant]))[0]:
                continue
            moved = in_sorted(promoted[state_at], rows[group])
            if not moved.any():  # pragma: no cover - an affected descendant has one
                continue
            new_upid = np.where(moved, ids[group], upid[group])
            new_pid = np.where(moved, -1, pid[group])
            after = [int(group[i]) for i in encounter(new_upid, new_pid, ids[group])]
            chain_after = chain_order(after, mass)
            if chain_after != chain_before:
                results[state_at]["changed"].append(descendant)
                results[state_at]["head_changed"] += int(chain_after[0] != chain_before[0])
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
) -> Dict:
    """The fourth read (module docstring, step 3): fills each state's
    ``per_snapshot`` rows and ``seeded`` pieces; returns the stored-chain check.

    Slabs are visited in ascending order so that dependents propagate forward
    along ``Descendant``. Snapshot N's row counts its promotions, groups and
    seeds, and in ``progenitor_order_changed`` the descendants at N + 1 whose
    chain over N's progenitors changes (they are seeds of N + 1).

    Raises:
        ConverterError: on a promotion count that disagrees with the slab's
            edge list, a dependent outside the named forests' rows, or a row
            or label inconsistency.
    """
    check = {"descendants": 0, "mismatches": 0, "examples": []}
    for snap in dataset.snapshots:
        path = dataset.snapshot_path(snap)
        labels = load_labels(aggregate_dir, snap)
        for state in states:
            state.current = np.unique(np.concatenate(state.following + [state.current[:0]]))
            state.current_seeds = np.unique(
                np.concatenate(state.following_seeds + [state.current[:0]])
            )
            state.following, state.following_seeds = [], []
        parts = [[] for _state in states]
        found = [0] * len(states)
        span = [None, None]
        for rows, values in iter_forest_rows(
            dataset, snap, forests, ("FirstHaloInFOFgroup", "Descendant"), block_rows
        ):
            span[0] = int(rows[0]) if span[0] is None else span[0]
            span[1] = int(rows[-1]) + 1
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
                            rows[moved],
                            central[moved],
                            desc[moved],
                            own_piece[moved],
                            host_piece[moved],
                        )
                    )
                if state.current.size:
                    hit = in_sorted(state.current, rows)
                    found[at] += int(np.count_nonzero(hit))
                    onward = desc[hit]
                    state.following.append(onward[onward >= 0])

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
                    np.concatenate(column) for column in zip(*parts[at])
                )
            else:
                rows_p = central_p = desc_p = own_p = host_p = np.zeros(0, dtype=np.int64)
            if rows_p.size != predicted[at][snap]:
                raise ConverterError(
                    "{}: rule {} promotes {} halo(s) but the slab's edge list predicts {}; the "
                    "graph aggregates are stale".format(
                        path, state.rule.name, rows_p.size, predicted[at][snap]
                    )
                )
            losing = np.unique(central_p)
            remnants = np.unique(pair_keys(central_p, own_p)).size if rows_p.size else 0
            state.seeded[own_p] = True
            state.seeded[host_p] = True
            seeds = np.unique(np.concatenate([rows_p, losing, state.current_seeds]))
            affected_rows = np.unique(np.concatenate([state.current, seeds]))
            state.following.append(desc_p[desc_p >= 0])
            if losing.size:
                onward = read_at(dataset, snap, "Descendant", losing, block_rows).astype(np.int64)
                state.following.append(onward[onward >= 0])
            state.per_snapshot.append(
                {
                    "snapshot": snap,
                    "promoted_halos": int(rows_p.size),
                    "groups_losing_members": int(losing.size),
                    "groups_central_leaves_members_stay": int(remnants),
                    "seed_halos": int(seeds.size),
                    "affected_halos": int(affected_rows.size),
                }
            )
            promoted.append(rows_p)
            affected.append(np.unique(desc_p[desc_p >= 0]))

        if span[0] is not None:
            changes, slab_check = _chain_changes(
                dataset, snap, states, promoted, affected, (span[0], span[1]), block_rows
            )
        else:
            changes = [{"changed": [], "head_changed": 0} for _state in states]
            slab_check = {"descendants": 0, "mismatches": 0, "examples": []}
        check["descendants"] += slab_check["descendants"]
        check["mismatches"] += slab_check["mismatches"]
        check["examples"].extend(slab_check["examples"][: N_EXAMPLES - len(check["examples"])])
        for state, change in zip(states, changes):
            changed = np.asarray(change["changed"], dtype=np.int64)
            state.following.append(changed)
            state.following_seeds.append(changed)
            state.per_snapshot[-1]["progenitor_order_changed"] = int(changed.size)
            state.per_snapshot[-1]["first_progenitor_changed"] = int(change["head_changed"])
        log("cut: snapshot {} evaluated for {} rule(s)".format(snap, len(states)))
    return check


# ---- partition with the pieces installed ------------------------------------------


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
                }
            )
    return points


def apply_points(points: List[Dict], snap: int, forests: np.ndarray, counts: np.ndarray) -> None:
    """Record every grid point's widest range in one slab (a tie keeps the
    lowest-numbered slab, as slabs are applied in ascending order)."""
    for point in points:
        rows = range_rows(forests, counts, point["forest_cuts"])
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
    """Per laptop class, whether any grid point's widest range fits its whole
    memory at ``bytes_per_halo``, and the smallest such point (fewest ranges,
    then fewest tasks)."""
    for point in points:
        point["implied_bytes"] = point["widest"]["rows"] * int(bytes_per_halo)
        point["fits_gib"] = [
            int(gib) for gib in laptop_gib if point["implied_bytes"] <= int(gib) * _GIB
        ]
    rows = []
    for gib in laptop_gib:
        budget = int(gib) * _GIB
        fitting = [p for p in points if p["implied_bytes"] <= budget]
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
                        "implied_bytes": best["implied_bytes"],
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
    index: SourceIndex,
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

    ``index_files`` is the record of the index files (paths, sizes and md5,
    computed once by the caller) that every table record carries.

    Raises:
        ConverterError: on any :func:`prepare_cut` refusal, an index whose
            roots are not the census's when a table is written, a table
            invariant, or a refusal of the pass.
    """
    inputs = prepare_cut(dataset, aggregate_dir, rules, materialise)
    if index.n_trees == 0:
        raise ConverterError("the index files list no tree")
    identity = inputs["identity"]
    forests = inputs["forests"]
    out = begin(cut_dir(aggregate_dir))
    n_forests = dataset.n_forests_total
    catalogue_max = int(index.forest_ids.max())
    forest_ids = dataset.forest_ids()

    roots = load_roots(aggregate_dir)
    tree_forest = load_array(trees_dir(aggregate_dir) / "tree_forest.npy", mmap=True)
    tree_totals = load_array(trees_dir(aggregate_dir) / "tree_totals.npy", mmap=True)
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
        state = RuleState(rule=rule, pieces=pieces, tree_piece=pieces.pop("piece"))
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
        local, counts = slab_local_counts(aggregate_dir, snap, local_of)
        edges = load_slab_edges(aggregate_dir, snap, mmap=False)
        edge_lo = local_of[edges["lo"]]
        edge_hi = local_of[edges["hi"]]
        forests_s, counts_s = load_slab_pairs(aggregate_dir, snap)
        for at, (state, per_piece) in enumerate(zip(states, piece_counts(states, local, counts))):
            # ascending slabs and a strict test: a tie keeps the lowest-numbered slab
            better = per_piece > state.peak
            state.peak[better] = per_piece[better]
            state.peak_snapshot[better] = snap
            severed = state.tree_piece[edge_lo] != state.tree_piece[edge_hi]
            predicted[at].append(int(edges["halos"][severed].sum()))
            installed = install_pieces(
                forests_s, counts_s, state, per_piece, n_forests, catalogue_max
            )
            apply_points(points[at], snap, *installed)

    check = severance_pass(
        dataset, aggregate_dir, forests, local_of, states, predicted, block_rows, log
    )

    sizes = {"assignments": assignment_bytes, "tables": 0, "records": 0}
    tables = {}
    if materialise:
        if not np.array_equal(roots, index.tree_roots):
            raise ConverterError(
                "{}: the census's tree roots are not the index files' roots; a cut table is "
                "written only for the index files the root correspondence passed against".format(
                    aggregate_dir
                )
            )
        all_totals = np.asarray(tree_totals, dtype=np.int64)
        for state in states:
            if state.rule.name not in {rule.name for rule in materialise}:
                continue
            ids = table_ids(index.forest_ids, forest_trees, state.pieces["id"][state.tree_piece])
            check_table(index.tree_roots, ids, index.tree_roots, index.forest_ids, all_totals)
            directory = rule_dir(aggregate_dir, state.rule)
            md5, table_bytes = write_table(directory / "forests.list", index.tree_roots, ids)
            del ids
            table = {
                "path": str((directory / "forests.list").resolve()),
                "md5": md5,
                "bytes": table_bytes,
                "rows": index.n_trees,
            }
            pieces = dict(
                state.pieces, peak_occupancy=state.peak, peak_snapshot=state.peak_snapshot
            )
            record = table_record(identity, index_files, state.rule.record(), pieces, table)
            write_json(directory / "record.json", record)
            sizes["tables"] += table_bytes
            sizes["records"] += (directory / "record.json").stat().st_size
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
        "index_files": index_files,
        "catalogue_max_forest_id": catalogue_max,
        "root_correspondence": inputs["trees"]["root_correspondence"]["verdict"],
        "stored_chain_check": check,
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
                "rule (about 24 B per tree at Shin-Uchuu's id widths) and a JSON record of "
                "about 80 B per piece",
                "materialised": sorted(tables),
                "bytes": sizes["tables"] + sizes["records"],
            },
        },
    }
    write_json(out / SUMMARY_NAME, summary)
    return summary
