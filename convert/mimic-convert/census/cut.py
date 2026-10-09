"""The decided table's cost: promotions, progenitor-order changes, affected
histories, and the partition with its pieces installed.

``cut`` evaluates exactly the decided table (``census/table.py``: every forest
cut at its z = 0 FoF groups, or, with a forest selection, only the selected
forests) over every forest the table gives more than one piece, the **split
forests**. It needs only completed ``occupancy``, ``trees`` and ``partition``
aggregates of the dataset, with a passed root correspondence:

1. the pieces, built and named exactly as the ``table`` subcommand builds them
   (the same :func:`table.decided_table`, the same fresh-id floor; both outputs
   carry the assignment's SHA-256, so they can be tied);
2. from the aggregates alone (one slab's tree pairs and occupancy pairs at a
   time): each piece's peak occupancy and its slab, and the driver's partition
   (``census/partition.py``) with the pieces installed -- under F6 an original
   forest keeps its ``ForestIndex`` (a split forest's largest piece among them)
   and the fresh pieces enumerate after every original forest in ascending
   fresh id -- giving every grid point's widest range per slab and overall and
   two memory figures at the stated bytes per resident halo: the **process**
   figure (the widest range; the per-rank ``retention_memory_ceiling_mb``
   guidance) and the **job** figure (the sum over tasks of each task's widest
   chunk over all slabs, since one laptop runs every rank at once and the
   ranks do not move through the slabs in step; :func:`laptop_rows`). Per
   laptop class, a row at a stated usable reserve says whether any grid
   point's job fits the class less the reserve, and the smallest that does;
3. one pass over the split forests' rows of every slab (:func:`severance_pass`):

   - **promotions**: per slab, the members whose FoF central lies in another
     piece (each becomes a central; no host is invented), the groups that lose
     members, and the **(group, piece) remnants**: a piece's members of one
     group whose central is in another piece, each member its own central,
     with the remnants of two or more members (members that stay together in
     one piece but no longer share a group) counted separately. **No
     promotion occurs at the final snapshot**: the table keeps every z = 0
     group whole, and a promotion there is refused as an error;
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
     recomputed one (``stored_chain_check``, whose mismatches must be 0 for
     the prediction to stand). The key reads the post-fix-up hosts, which is
     exact for Consistent-Trees-derived datasets only;
   - the **affected-history bracket** for ``sage16`` and ``halos-only``: the
     seeds (promoted halos, the centrals of groups that lose members, and
     descendants whose progenitor chain changes) and every halo on a seed's
     descendant path, propagated slab by slab (``dependent_halos``), and the
     upper bound, every halo of a piece holding a seed
     (``upper_bound_halos``): a piece without one keeps its topology and its
     relative order. HOD and SHAM are stated as predicted mechanisms (decision
     7), not counted: the identities their draws and tie-breaks key on are
     recomputed in every split forest, promoted halos change host and
     centrality, and SHAM's global ranking can move tied assignments outside
     the split forests.

The halos **re-labelled** are also counted: those of fresh pieces (new forest
id), every halo of a split forest (``HaloRankInForest`` recomputed), and every
halo whose ``SourceHaloID`` prefix moves. ``SourceHaloID`` is recomputed for
every row of every forest from the first split one on, but under F1 its prefix
is the halos of every lower ``ForestIndex``, so the prefix moves for every
halo of those forests except the first split forest's kept piece, which keeps
its ``ForestIndex`` after unchanged forests (only its ranks move).

**Retained-row discovery.** The pass finds the rows whose further columns the
chains need from the slab itself, in two steps per slab. First every split-forest
row's ``FirstHaloInFOFgroup`` and ``Descendant`` are read into two dense int32
row arrays of the slab's length (-1 for a row outside the split forests),
alongside the slab's labels, and the promotions found block by block. Then the
**retained rows** are the progenitors of every descendant with a promoted
progenitor (the rows whose ``Descendant`` names one, found from the dense
array) and the FoF centrals of those progenitors (from the other dense array);
``MostBoundID``, ``M_Crit200`` and ``NextProgenitor`` are read over them in
windows of at most ``block_rows`` rows (:func:`read_at_rows`). A promoted halo
and its siblings share their descendant's tree, so every retained row is a
split-forest row and nothing is re-read.

**Reads.** Per slab: ``ForestIndex`` in full blocks (8 B x ``n_halos(s)``),
``FirstHaloInFOFgroup`` and ``Descendant`` over each block's span of
split-forest rows (``2 w x span(s)``), and ``MostBoundID``, ``M_Crit200`` and
``NextProgenitor`` over each window of retained rows (``(12 + w) x
window(s)``), with ``w`` the link width (4 B in version 2, 8 B in version 3);
``span(s)`` and ``window(s)`` are at most ``n_halos(s)``. The final slab's
``FirstHaloInFOFgroup`` and ``ForestIndex`` are read once more by the table's
construction. The bytes read are measured and recorded per column in
``summary.json`` (``io``).

**Memory**, with ``n`` the trees, ``F`` = ``n_forests_total``, ``t`` the split
forests' trees, ``p`` their pieces and ``P`` = ``F`` + fresh pieces (per-unit
costs measured with ``tracemalloc`` on the micro-Uchuu census; Shin-Uchuu
figures with ``n`` = 315,004,242 and ``F`` = 166,547,771):

- for the whole subcommand: the census roots (8 B x n, 2.5 GB), the dense
  tree-to-local map (4 B x n, 1.3 GB), the sidecar ``ForestID``, the centrals
  and the halos per forest (24 B x F, 4.0 GB), the split-forest mask (1 B x F),
  the pieces as :func:`table.decided_table` leaves them (about **57 B x p + 8 B
  x t**) and the per-piece state (the peak, its slab, the widest slab's count,
  the seed flag and the fresh order, about **33 B x p**); the construction
  itself peaks as ``census/table.py`` states;
- step 2, from the aggregates: one slab's tree pairs and occupancy pairs with
  their gathers, about **19 B per present tree and 12 B per present forest**,
  two per-piece vectors (**16 B x p**), and the dense partition weights of the
  widest slab (**8 B x P**, transient); each grid point holds its forest cuts
  and range peaks;
- per slab of the pass (arrays of one slab's length): the labels and the two
  dense row arrays, **12 B x n_halos(s)** (6.2 GB at the widest slab's
  519,342,987 rows), and the descendant mask of the next slab, **1 B x
  n_halos(s + 1)**; one block's columns, gathers and transients, about **100 B
  x block_rows** (0.42 GB at the default 2^22); the promotions, about **50 B
  each** while they are joined; the retained rows, about **34 B each**, at
  most ``n_halos(s)``; the dependents carried into the slab, 8 B each; and the
  chain working memory of :func:`chain_changes`, about **29 B per affected
  sibling** while the sibling index is built and **200 B x (block_rows + g_max
  - 1)** per batch for the slab's largest progenitor group of ``g_max`` records.

The figures the run measured are recorded in ``summary.json`` (``memory``: the
peak resident set after each phase, and each slab's rows read and retained
rows) beside these formulas. The driver's transient startup weights, 8 B x
``n_forests_total`` before and after the cut, are stated there too. No tree x
snapshot or forest x snapshot matrix is held.

Aggregates: ``<aggregate>/cut/summary.json`` (``cut-restricted/`` for a forest
selection), a few kilobytes plus about 10 integers per grid point and slab.
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
    load_array,
    require_completed,
    write_json,
)
from census.occupancy import load_slab_pairs, occupancy_dir  # noqa: E402
from census.partition import (  # noqa: E402
    DEFAULT_NTASKS,
    PARTITION_DIR,
    partition_cut,
    range_rows,
    slab_prefix,
)
from census.table import (  # noqa: E402
    DecidedTable,
    ReadMeter,
    assignment_digest,
    check_central_rows,
    decided_table,
    output_dir,
    peak_rss_bytes,
    piece_entry,
    piece_slab_counts,
    refuse_stale,
    require_all_reach_final,
    require_census,
    scope_record,
)
from census.trees import load_labels, load_roots, trees_dir  # noqa: E402
from errors import ConverterError  # noqa: E402
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402

CUT_DIR = "cut"

#: The chunk counts per task the cut's grid applies when none are given.
CUT_NCHUNKS = (1, 2, 4, 8, 16, 32, 64, 128)

#: Bytes per resident halo of a chunked horizontal run (the chunked streaming record).
DEFAULT_BYTES_PER_HALO = 1100

#: Laptop memory classes, GiB.
DEFAULT_LAPTOP_GIB = (16, 32, 64)

#: The mass column: native float32 Mvir (Msun/h) for Consistent-Trees-derived datasets.
MASS_COLUMN = "M_Crit200"

#: Pieces and examples quoted per list.
N_LARGEST = 10
N_EXAMPLES = 5

_GIB = 1 << 30
_INT32_MAX = int(np.iinfo(np.int32).max)
_KEY_SHIFT = 31

#: What each per-snapshot count means, recorded with the counts.
SEVERANCE_DEFINITIONS = {
    "promoted_halos": "members whose FoF central lies in another piece of the decided table; "
    "each becomes a central",
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
    "rows_read": "split-forest rows of the slab, read into the dense row arrays",
    "retained_rows": "progenitors of descendants with a promoted progenitor, and their FoF "
    "centrals: the rows whose MostBoundID, M_Crit200 and NextProgenitor are read",
}

#: The predicted HOD and SHAM mechanisms (decision 7), stated rather than counted.
HOD_SHAM_MECHANISMS = (
    "identity recomputation: ForestIndex, HaloRankInForest and UniqueGalaxyID are recomputed "
    "for every halo of a split forest, and HOD's draws and SHAM's tie-breaks key on them",
    "host and centrality changes: a promoted halo becomes a central, so HOD populates it as a "
    "host and SHAM ranks it as a central; its former group loses it",
    "SHAM's global ranking: abundance matching ranks every halo of a snapshot together, so a "
    "changed centrality or tie order can move tied assignments outside the split forests",
)


def _quiet(_message: str) -> None:
    pass


def cut_dir(aggregate_dir, selection: Optional[np.ndarray] = None) -> Path:
    return output_dir(aggregate_dir, CUT_DIR, selection)


# ---- generic helpers ---------------------------------------------------------------


def drain(parts: List[np.ndarray], dtype=np.int64) -> np.ndarray:
    """Concatenate ``parts`` and empty the list, so the parts can be released."""
    whole = np.concatenate(parts) if parts else np.zeros(0, dtype=dtype)
    parts.clear()
    return whole


def in_sorted(haystack: np.ndarray, needles: np.ndarray) -> np.ndarray:
    """Whether each needle occurs in the ascending ``haystack``."""
    if haystack.size == 0:
        return np.zeros(np.shape(needles), dtype=bool)
    position = np.minimum(np.searchsorted(haystack, needles), haystack.size - 1)
    return haystack[position] == needles


def pair_keys(first: np.ndarray, second: np.ndarray) -> np.ndarray:
    """One int64 per pair, ordered as (first, second): both are below 2^31."""
    return (np.asarray(first, dtype=np.int64) << _KEY_SHIFT) | np.asarray(second, dtype=np.int64)


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
    selected: np.ndarray,
    names: Sequence[str],
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
):
    """Yield ``(rows, {name: values})`` for the rows of one slab whose
    ``ForestIndex`` is marked in ``selected`` (a boolean per ``ForestIndex``),
    block by block: the ``ForestIndex`` block is read whole, the named columns
    only over the span of the block's matching rows, from the first to the
    last. In a version 3 slab that span is the selected forests' rows when
    they are adjacent in ``ForestIndex``, otherwise their enclosing span; in a
    version 2 slab it can be the whole block. It is bounded by ``block_rows``
    either way. Every byte read is added to ``meter`` when one is given.

    Raises:
        ConverterError: for a ``ForestIndex`` outside ``[0, selected.size)``.
    """
    for start, block in dataset.iter_column(snap, "ForestIndex", block_rows):
        if meter is not None:
            meter.add({"ForestIndex": block})
        forest = block.astype(np.int64, copy=False)
        if forest.size and (int(forest.min()) < 0 or int(forest.max()) >= selected.size):
            raise ConverterError(
                "{}: ForestIndex outside [0, {}) in rows [{}, {})".format(
                    dataset.snapshot_path(snap), selected.size, start, start + forest.size
                )
            )
        picked = np.flatnonzero(selected[forest])
        if picked.size == 0:
            continue
        low, high = int(picked[0]), int(picked[-1]) + 1
        columns = read_span(dataset, snap, names, start + low, start + high, meter)
        yield start + picked.astype(np.int64), {name: columns[name][picked - low] for name in names}


def read_at_rows(
    dataset: HorizontalDataset,
    snap: int,
    names: Sequence[str],
    rows: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
) -> Dict[str, np.ndarray]:
    """The named columns at ``rows`` (ascending and unique), read in windows
    spanning at most ``block_rows`` rows each, so no window exceeds a block and
    the windows never overlap: at most the slab's columns are read once."""
    parts: Dict[str, List[np.ndarray]] = {name: [] for name in names}
    at = 0
    while at < rows.size:
        low = int(rows[at])
        end = int(np.searchsorted(rows, low + block_rows, "left"))
        high = int(rows[end - 1]) + 1
        columns = read_span(dataset, snap, names, low, high, meter)
        offset = rows[at:end] - low
        for name in names:
            parts[name].append(columns[name][offset])
        at = end
    return {name: drain(values) for name, values in parts.items()}


def check_descendant_rows(descendant: np.ndarray, n_next: int, path) -> np.ndarray:
    """``Descendant`` values as int64, refusing one that is neither -1 nor a
    row of the next slab."""
    descendant = descendant.astype(np.int64, copy=False)
    bad = (descendant < -1) | (descendant >= n_next)
    if bad.any():
        raise ConverterError(
            "{}: {} Descendant value(s) neither -1 nor in [0, {}) (e.g. {})".format(
                path,
                int(np.count_nonzero(bad)),
                n_next,
                descendant[np.flatnonzero(bad)[:N_EXAMPLES]].tolist(),
            )
        )
    return descendant


# ---- inputs -----------------------------------------------------------------------

#: The file the cut subcommand writes in its output directory.
CUT_OUTPUTS = (SUMMARY_NAME,)


def prepare_cut(dataset: HorizontalDataset, aggregate_dir, selection=None) -> Dict:
    """Every refusal that needs no pass result, before anything is loaded or
    written: the directory's dataset, the completed ``occupancy``, ``trees``
    and ``partition`` of that dataset, a passed root correspondence, every
    tree reaching the final snapshot, slabs whose rows int32 row indices hold,
    and an output directory holding nothing the run would not rewrite.

    Raises:
        ConverterError: naming the first refusal.
    """
    identity, trees = require_census(dataset, aggregate_dir)
    occupancy = require_completed(
        aggregate_dir, occupancy_dir(aggregate_dir), "occupancy", identity, ("widest_slab",)
    )
    partition = require_completed(
        aggregate_dir, Path(aggregate_dir) / PARTITION_DIR, "partition", identity, ("floor",)
    )
    if occupancy["widest_slab"] is None:
        raise ConverterError("{}: the dataset has no halo to cut".format(aggregate_dir))
    wide = [snap for snap, rows in enumerate(dataset.n_halos) if rows > _INT32_MAX]
    if wide:
        raise ConverterError(
            "{}: snapshot(s) {} hold more rows than int32 row indices hold".format(
                dataset.directory, wide[:N_EXAMPLES]
            )
        )
    require_all_reach_final(dataset, aggregate_dir)
    refuse_stale(cut_dir(aggregate_dir, selection), CUT_OUTPUTS)
    return {"identity": identity, "occupancy": occupancy, "trees": trees, "partition": partition}


# ---- the pieces' state ---------------------------------------------------------------


@dataclass
class CutState:
    """The decided table's pieces and the pass's accumulators."""

    table: DecidedTable
    peak: np.ndarray = field(init=False)  # per piece, its largest slab count
    peak_snapshot: np.ndarray = field(init=False)  # the lowest-numbered slab of the peak
    seeded: np.ndarray = field(init=False)  # per piece, whether it holds a seed
    fresh_order: np.ndarray = field(init=False)  # the fresh pieces, in ascending fresh id
    widest_counts: Optional[np.ndarray] = None  # per piece, its rows in the widest slab
    current: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=np.int64))
    current_seeds: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=np.int64))
    following: List[np.ndarray] = field(default_factory=list)
    following_seeds: List[np.ndarray] = field(default_factory=list)
    per_snapshot: List[Dict] = field(default_factory=list)

    def __post_init__(self):
        pieces = self.table.pieces
        n_pieces = pieces["id"].size
        self.peak = np.zeros(n_pieces, dtype=np.int64)
        self.peak_snapshot = np.full(n_pieces, -1, dtype=np.int64)
        self.seeded = np.zeros(n_pieces, dtype=bool)
        # sorted once here, not per slab: the installed partition appends fresh pieces in this order
        fresh = np.flatnonzero(~pieces["keeps_id"])
        self.fresh_order = fresh[np.argsort(pieces["id"][fresh], kind="stable")]

    @property
    def pieces(self) -> Dict[str, np.ndarray]:
        return self.table.pieces

    @property
    def tree_piece(self) -> np.ndarray:
        return self.table.tree_piece


def install_pieces(
    forests: np.ndarray,
    counts: np.ndarray,
    state: CutState,
    per_piece: np.ndarray,
    n_forests: int,
    catalogue_max: int,
) -> Tuple[np.ndarray, np.ndarray]:
    """One slab's per-forest rows after the cut: each split forest's count is
    its id-keeping piece's, and each fresh piece is appended at ``ForestIndex``
    ``n_forests + (id - catalogue_max - 1)`` (ascending fresh id, in the order
    :class:`CutState` sorted once). Empty entries are dropped; the result
    ascends."""
    forests = np.asarray(forests, dtype=np.int64)
    counts = np.asarray(counts, dtype=np.int64).copy()
    pieces = state.pieces
    keeping = np.flatnonzero(pieces["keeps_id"])
    at = np.searchsorted(forests, pieces["forest_index"][keeping])
    present = at < forests.size
    present[present] = forests[at[present]] == pieces["forest_index"][keeping][present]
    counts[at[present]] = per_piece[keeping[present]]
    fresh = state.fresh_order[per_piece[state.fresh_order] > 0]
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

#: Columns the pass reads for every split-forest row of a block.
PASS_COLUMNS = ("FirstHaloInFOFgroup", "Descendant")

#: Columns it reads over the windows of retained rows.
RETAINED_COLUMNS = ("MostBoundID", MASS_COLUMN, "NextProgenitor")


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
            raise ConverterError("{}: a {} is not among the retained rows".format(path, what))
        return at


def retained_rows(
    dataset: HorizontalDataset,
    snap: int,
    affected: np.ndarray,
    central_rows: np.ndarray,
    descendant_rows: np.ndarray,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    meter: Optional[ReadMeter] = None,
) -> Retained:
    """The retained-row discovery (module docstring): the progenitors of the
    ``affected`` descendants (rows of the next slab, ascending) and their FoF
    centrals, with ``MostBoundID``, ``M_Crit200`` and ``NextProgenitor`` read
    over windows of them."""
    if affected.size == 0:
        rows = np.zeros(0, dtype=np.int64)
    else:
        hit = np.zeros(dataset.n_halos[snap + 1], dtype=bool)
        hit[affected] = True
        parts = []
        for start in range(0, descendant_rows.size, block_rows):
            block = descendant_rows[start : start + block_rows]
            linked = np.flatnonzero(block >= 0)
            parts.append(start + linked[hit[block[linked]]])
        del hit
        siblings = drain(parts)
        rows = np.union1d(siblings, central_rows[siblings].astype(np.int64))
        del siblings
    extra = read_at_rows(dataset, snap, RETAINED_COLUMNS, rows, block_rows, meter)
    empty = rows.size == 0
    return Retained(
        row=rows.astype(np.int32),
        descendant=descendant_rows[rows],
        central=central_rows[rows],
        next_progenitor=(
            np.zeros(0, dtype=np.int32) if empty else extra["NextProgenitor"].astype(np.int32)
        ),
        most_bound_id=(
            np.zeros(0, dtype=np.int64) if empty else extra["MostBoundID"].astype(np.int64)
        ),
        mass=np.zeros(0, dtype=np.float32) if empty else extra[MASS_COLUMN].astype(np.float32),
    )


def chain_changes(
    snap: int,
    promoted: np.ndarray,
    affected: np.ndarray,
    kept: Retained,
    path,
    batch_rows: int = DEFAULT_BLOCK_ROWS,
) -> Tuple[np.ndarray, int, Dict]:
    """The descendants (rows of the next slab) whose progenitor chain changes,
    how many change their first progenitor, and the stored-chain check over
    every affected descendant with two or more progenitors.

    Working memory (measured with ``tracemalloc``): the slab's affected
    siblings are first found and sorted by descendant into one int32 index
    over the retained rows, about **29 B per affected sibling** while it is
    built and 4 B each while it is held across the batches; the chains are
    then worked in batches of whole descendants, each starting at the first
    group at or after a multiple of ``batch_rows`` records, so a batch holds at
    most ``batch_rows + g_max - 1`` records for the slab's largest progenitor
    group of ``g_max`` records, at about **200 B per record**."""
    check = {"descendants": 0, "mismatches": 0, "examples": []}
    if affected.size == 0:
        return np.zeros(0, dtype=np.int64), 0, check
    # the affected descendants' progenitors, found block by block
    found = []
    for start in range(0, kept.row.size, batch_rows):
        part = kept.descendant[start : start + batch_rows].astype(np.int64)
        found.append(start + np.flatnonzero(in_sorted(affected, part)))
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
    changed: List[np.ndarray] = []
    head_changed = 0
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
        moved = in_sorted(promoted, rows)
        if not moved.any():
            continue
        after = chain_positions(
            groups, np.where(moved, ids, upid), np.where(moved, -1, pid), ids, mass
        )
        changed.append(np.unique(groups[before != after]))
        head_changed += int(np.unique(groups[(before == 0) & (after != 0)]).size)
    return drain(changed), head_changed, check


def severance_pass(
    dataset: HorizontalDataset,
    aggregate_dir,
    state: CutState,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
    meter: Optional[ReadMeter] = None,
) -> Dict:
    """Step 3 (module docstring): fills ``state.per_snapshot`` and the seeded
    pieces; returns the stored-chain check.

    Slabs are visited in ascending order so that dependents propagate forward
    along ``Descendant``. Snapshot N's row counts its promotions, groups and
    seeds, and in ``progenitor_order_changed`` the descendants at N + 1 whose
    chain over N's progenitors changes (they are seeds of N + 1).

    Raises:
        ConverterError: on a promotion at the final snapshot, a dependent
            outside the split forests' rows, or a row or label inconsistency.
    """
    table = state.table
    selected = np.zeros(dataset.n_forests_total, dtype=bool)
    selected[table.split] = True
    check = {"descendants": 0, "mismatches": 0, "examples": []}
    last = dataset.snapshots[-1]
    for snap in dataset.snapshots:
        path = dataset.snapshot_path(snap)
        n_rows = dataset.n_halos[snap]
        n_next = dataset.n_halos[snap + 1] if snap != last else 0
        labels = load_labels(aggregate_dir, snap, mmap=False)
        if labels.size != n_rows:
            raise ConverterError(
                "{}: {} labels for {} halos; the trees aggregates are stale".format(
                    path, labels.size, n_rows
                )
            )
        state.current = np.unique(drain(state.following + [state.current[:0]]))
        state.current_seeds = np.unique(drain(state.following_seeds + [state.current[:0]]))
        state.following, state.following_seeds = [], []
        # the dense row arrays: -1 marks a row outside the split forests
        central_rows = np.full(n_rows, -1, dtype=np.int32)
        descendant_rows = np.full(n_rows, -1, dtype=np.int32)
        parts: List[Tuple[np.ndarray, ...]] = []
        rows_read = 0
        for rows, values in iter_forest_rows(
            dataset, snap, selected, PASS_COLUMNS, block_rows, meter
        ):
            central = check_central_rows(values["FirstHaloInFOFgroup"], n_rows, path)
            desc = check_descendant_rows(values["Descendant"], n_next, path)
            central_rows[rows] = central
            descendant_rows[rows] = desc
            own = table.local_of[labels[rows]]
            host = table.local_of[labels[central]]
            if (own < 0).any() or (host < 0).any():
                raise ConverterError(
                    "{}: a split-forest halo, or its FoF central, is labelled with a tree outside "
                    "the split forests".format(path)
                )
            own_piece = table.tree_piece[own]
            host_piece = table.tree_piece[host]
            moved = (central != rows) & (own_piece != host_piece)
            if moved.any():
                parts.append(
                    (
                        rows[moved].astype(np.int32),
                        central[moved].astype(np.int32),
                        own_piece[moved],
                        host_piece[moved],
                    )
                )
            rows_read += int(rows.size)
        del labels

        outside = state.current[central_rows[state.current] < 0]
        if outside.size:
            raise ConverterError(
                "{}: {} dependent row(s) are not among the split forests' rows (e.g. {})".format(
                    path, outside.size, outside[:N_EXAMPLES].tolist()
                )
            )
        if parts:
            promoted, central_p, own_p, host_p = (
                np.concatenate(column).astype(np.int64) for column in zip(*parts)
            )
        else:
            promoted = central_p = own_p = host_p = np.zeros(0, dtype=np.int64)
        del parts
        if snap == last and promoted.size:
            raise ConverterError(
                "{}: {} halo(s) promoted at the final snapshot (e.g. rows {}); the decided table "
                "keeps every z = 0 FoF group whole".format(
                    path, promoted.size, promoted[:N_EXAMPLES].tolist()
                )
            )
        losing = np.unique(central_p)
        remnant_members = np.unique(pair_keys(central_p, own_p), return_counts=True)[1]
        state.seeded[own_p] = True
        state.seeded[host_p] = True
        seeds = np.unique(np.concatenate([promoted, losing, state.current_seeds]))
        affected_rows = np.unique(np.concatenate([state.current, seeds]))
        for onward in (
            descendant_rows[state.current],
            descendant_rows[promoted],
            descendant_rows[losing],
        ):
            state.following.append(onward[onward >= 0].astype(np.int64))
        affected = np.unique(descendant_rows[promoted]).astype(np.int64)
        affected = affected[affected >= 0]
        kept = retained_rows(
            dataset, snap, affected, central_rows, descendant_rows, block_rows, meter
        )
        del central_rows, descendant_rows
        changed, head_changed, slab_check = chain_changes(
            snap, promoted, affected, kept, path, block_rows
        )
        check["descendants"] += slab_check["descendants"]
        check["mismatches"] += slab_check["mismatches"]
        check["examples"].extend(slab_check["examples"][: N_EXAMPLES - len(check["examples"])])
        state.following.append(changed)
        state.following_seeds.append(changed)
        state.per_snapshot.append(
            {
                "snapshot": snap,
                "promoted_halos": int(promoted.size),
                "groups_losing_members": int(losing.size),
                "groups_central_leaves_members_stay": int(remnant_members.size),
                "remnants_with_several_members": int(np.count_nonzero(remnant_members >= 2)),
                "seed_halos": int(seeds.size),
                "affected_halos": int(affected_rows.size),
                "progenitor_order_changed": int(changed.size),
                "first_progenitor_changed": int(head_changed),
                "rows_read": rows_read,
                "retained_rows": int(kept.row.size),
            }
        )
        del kept
        log(
            "cut: snapshot {} evaluated ({} promoted) at {}".format(
                snap, promoted.size, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            )
        )
    return check


# ---- partition with the pieces installed ------------------------------------------


def partition_points(
    aggregate_dir,
    state: CutState,
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
    weights = np.zeros(n_forests + state.table.n_fresh, dtype=np.int64)
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
    range's largest slab so far. The slab's prefix sums are computed once for
    every point."""
    prefix = slab_prefix(counts)
    for point in points:
        rows = range_rows(forests, counts, point["forest_cuts"], prefix)
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


def reserve_bytes(reserve_gib: float) -> int:
    """The usable reserve in bytes (GiB of 2^30 bytes, rounded to a byte)."""
    return int(round(float(reserve_gib) * _GIB))


def laptop_rows(
    points: List[Dict],
    bytes_per_halo: int,
    laptop_gib: Sequence[int],
    reserve_gib: float,
) -> List[Dict]:
    """Per laptop class, whether any grid point's **job** fits the class's
    memory less the stated reserve at ``bytes_per_halo``, and the smallest such
    point (fewest ranges, then fewest tasks).

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
    reserve = reserve_bytes(reserve_gib)
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
            int(gib) for gib in laptop_gib if point["job_bytes"] <= int(gib) * _GIB - reserve
        ]
    rows = []
    for gib in laptop_gib:
        usable = int(gib) * _GIB - reserve
        fitting = [p for p in points if p["job_bytes"] <= usable]
        best = min(fitting, key=lambda p: (p["ntask"] * p["nchunk"], p["ntask"]), default=None)
        rows.append(
            {
                "class_gib": int(gib),
                "reserve_gib": float(reserve_gib),
                "usable_bytes": usable,
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


# ---- the subcommand ------------------------------------------------------------------


def run_cut(
    dataset: HorizontalDataset,
    aggregate_dir,
    selection: Optional[np.ndarray] = None,
    ntasks: Sequence[int] = DEFAULT_NTASKS,
    nchunks: Sequence[int] = CUT_NCHUNKS,
    bytes_per_halo: int = DEFAULT_BYTES_PER_HALO,
    laptop_gib: Sequence[int] = DEFAULT_LAPTOP_GIB,
    reserve_gib: float = 0.0,
    block_rows: int = DEFAULT_BLOCK_ROWS,
    log: Callable[[str], None] = _quiet,
) -> Dict:
    """Evaluate the decided table (module docstring) and write the summary;
    returns it.

    Raises:
        ConverterError: on any :func:`prepare_cut` refusal, a reserve that
            leaves a class no usable memory, a refusal of the construction or
            of the pass.
    """
    if reserve_bytes(reserve_gib) < 0 or any(
        reserve_bytes(reserve_gib) >= int(gib) * _GIB for gib in laptop_gib
    ):
        raise ConverterError(
            "a reserve of {} GiB leaves a laptop class among {} GiB no usable memory".format(
                reserve_gib, list(laptop_gib)
            )
        )
    inputs = prepare_cut(dataset, aggregate_dir, selection)
    memory = {"after_prepare": peak_rss_bytes()}
    meter = ReadMeter()
    identity = inputs["identity"]
    n_forests = dataset.n_forests_total
    forest_ids = dataset.forest_ids()
    roots = load_roots(aggregate_dir)
    tree_forest = load_array(trees_dir(aggregate_dir) / "tree_forest.npy")
    tree_totals = load_array(trees_dir(aggregate_dir) / "tree_totals.npy", mmap=True)
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
    digest = assignment_digest(table, roots)
    catalogue_max = table.fresh_id_floor
    state = CutState(table=table)
    memory["after_pieces"] = peak_rss_bytes()
    log(
        "cut: {} split forest(s), {} piece(s) ({} fresh)".format(
            table.split.size, table.n_pieces, table.n_fresh
        )
    )

    # aggregates only: piece peaks and the partition with the pieces installed
    widest = int(inputs["occupancy"]["widest_slab"]["snapshot"])
    state.widest_counts = piece_slab_counts(aggregate_dir, widest, table)
    points = partition_points(
        aggregate_dir, state, widest, n_forests, catalogue_max, ntasks, nchunks
    )
    for snap in dataset.snapshots:
        per_piece = piece_slab_counts(aggregate_dir, snap, table)
        # ascending slabs and a strict test: a tie keeps the lowest-numbered slab
        better = per_piece > state.peak
        state.peak[better] = per_piece[better]
        state.peak_snapshot[better] = snap
        forests_s, counts_s = load_slab_pairs(aggregate_dir, snap)
        installed = install_pieces(forests_s, counts_s, state, per_piece, n_forests, catalogue_max)
        apply_points(points, snap, *installed)
        del per_piece, forests_s, counts_s, installed
    memory["after_aggregates"] = peak_rss_bytes()
    log("cut: aggregates applied; the pass begins")

    check = severance_pass(dataset, aggregate_dir, state, block_rows, log, meter)
    memory["after_pass"] = peak_rss_bytes()

    pieces = state.pieces
    fresh = ~pieces["keeps_id"]
    first_split = int(table.split[0]) if table.split.size else None
    forest_totals = load_array(occupancy_dir(aggregate_dir) / "forest_totals.npy", mmap=True)
    rows = state.per_snapshot
    by_halos = np.lexsort((pieces["smallest_root_id"], -pieces["halos"]))[:N_LARGEST]
    by_peak = np.lexsort((pieces["smallest_root_id"], -state.peak))[:N_LARGEST]
    largest_split = table.split[
        np.lexsort((table.split, -table.forest_halos[table.split]))[:N_LARGEST]
    ]
    per_forest = np.bincount(pieces["forest_index"], minlength=n_forests)
    keeping = np.flatnonzero(~fresh)
    # each listed forest's id-keeping piece
    kept_of = {
        int(f): int(keeping[np.argmax(pieces["forest_index"][keeping] == f)]) for f in largest_split
    }
    peak_args = (state.peak, state.peak_snapshot)
    uncut = inputs["partition"]["floor"]
    # SourceHaloID is recomputed from the first split forest on; its prefix moves
    # for all of those halos but the first split forest's kept piece (F1)
    recomputed = shifted = 0
    if first_split is not None:
        recomputed = int(np.asarray(forest_totals[first_split:]).sum())
        first_kept = (pieces["forest_index"] == first_split) & pieces["keeps_id"]
        shifted = recomputed - int(pieces["halos"][first_kept].sum())
    classes = laptop_rows(points, bytes_per_halo, laptop_gib, reserve_gib)
    rank_recomputed = int(pieces["halos"].sum())
    summary = {
        "dataset": identity,
        "dataset_dir": str(dataset.directory.resolve()),
        **scope_record(selection, forest_ids),
        "assignment_sha256": digest,
        "root_correspondence": inputs["trees"]["root_correspondence"]["verdict"],
        "fresh_id_floor": {
            "value": catalogue_max,
            "note": "fresh piece ids start at value + 1, the dataset sidecar's largest forest "
            "id, as the table subcommand names them",
        },
        "pieces": {
            "split_forests": int(table.split.size),
            "count": table.n_pieces,
            "fresh": table.n_fresh,
            "single_tree": int(np.count_nonzero(pieces["trees"] == 1)),
            "forests_after": n_forests + table.n_fresh,
            "largest": [piece_entry(table, int(at), *peak_args) for at in by_halos],
            "highest_peak": [piece_entry(table, int(at), *peak_args) for at in by_peak],
            "largest_split_forests": [
                {
                    "forest_index": int(f),
                    "forest_id": int(forest_ids[f]),
                    "halos": int(table.forest_halos[f]),
                    "pieces": int(per_forest[f]),
                    "kept_piece": piece_entry(table, kept_of[int(f)], *peak_args),
                }
                for f in largest_split
            ],
        },
        "stored_chain_check": check,
        "severance": {
            "promoted_halos": sum(r["promoted_halos"] for r in rows),
            "promoted_at_final_snapshot": rows[-1]["promoted_halos"] if rows else 0,
            "groups_losing_members": sum(r["groups_losing_members"] for r in rows),
            "groups_central_leaves_members_stay": sum(
                r["groups_central_leaves_members_stay"] for r in rows
            ),
            "remnants_with_several_members": sum(r["remnants_with_several_members"] for r in rows),
            "per_snapshot": rows,
            "definitions": SEVERANCE_DEFINITIONS,
        },
        "progenitor_order": {
            "descendants_changed": sum(r["progenitor_order_changed"] for r in rows),
            "first_progenitor_changed": sum(r["first_progenitor_changed"] for r in rows),
        },
        "relabelled": {
            "forest_id_changed_halos": int(pieces["halos"][fresh].sum()),
            "rank_recomputed_halos": rank_recomputed,
            "source_halo_id_shifted_halos": shifted,
            "source_halo_id_recomputed_halos": recomputed,
        },
        "predicted_effects": {
            "sage16_halos_only": {
                "seed_halos": sum(r["seed_halos"] for r in rows),
                "dependent_halos": sum(r["affected_halos"] for r in rows),
                "upper_bound_halos": int(pieces["halos"][state.seeded].sum()),
                "definition": "seeds are the promoted halos, the centrals of groups that lose "
                "members and the descendants whose progenitor chain changes; dependent_halos "
                "counts the seeds and every halo on a seed's descendant path; upper_bound_halos "
                "counts every halo of a piece holding a seed. Input-topology counts, not a "
                "prediction of how many galaxies differ",
            },
            "hod_sham": {
                "identity_recomputed_halos": rank_recomputed,
                "mechanisms": list(HOD_SHAM_MECHANISMS),
                "note": "predicted mechanisms, not counts of differing galaxies; their "
                "differences are measured on a subset, not predicted here",
            },
        },
        "partition": {
            "bytes_per_halo": int(bytes_per_halo),
            "reserve_gib": float(reserve_gib),
            "grid": points,
            "laptop_classes": classes,
            "uncut_floor": {
                "rows": uncut["max_rows"],
                "snapshot": uncut["snapshot"],
                "note": "the uncut dataset's floor, from the partition summary",
            },
        },
        "io": meter.record(
            "per slab, 8 B x n_halos(s) of ForestIndex, plus 2 x link width x the split forests' "
            "row span(s) of FirstHaloInFOFgroup and Descendant, plus (8 + 4 + link width) B x "
            "the windows of retained rows of MostBoundID, M_Crit200 and NextProgenitor; plus "
            "the final slab's FirstHaloInFOFgroup and ForestIndex for the table's construction; "
            "link width 4 B in version 2, 8 B in version 3"
        ),
        "memory": {
            "peak_rss_bytes_after": memory,
            "widest_pass_slab": {
                "snapshot": int(np.argmax(dataset.n_halos)),
                "dense_row_bytes": 12 * max(dataset.n_halos),
                "formula": "12 B x n_halos(s): the labels and the two dense int32 row arrays",
            },
            "startup_weights": {
                "formula": "8 B x n_forests_total: the driver's dense weight vector at startup "
                "(transient)",
                "uncut_bytes": 8 * n_forests,
                "cut_bytes": 8 * (n_forests + table.n_fresh),
            },
            "note": "the process's peak resident set (ru_maxrss) after each phase; the module "
            "docstring states each phase's formula, and per_snapshot records each slab's rows "
            "read and retained rows",
        },
        "aggregates": {
            "summary": {
                "formula": "a few kilobytes plus about 10 integers per grid point and slab",
                "bytes": 0,
            }
        },
    }
    out = begin(cut_dir(aggregate_dir, selection))
    # the summary records its own size: rewrite until the recorded figure is the file's
    path = out / SUMMARY_NAME
    record = summary["aggregates"]["summary"]
    for _attempt in range(8):
        write_json(path, summary)
        size = path.stat().st_size
        if size == record["bytes"]:
            break
        record["bytes"] = size
    return summary
