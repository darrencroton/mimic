"""Passes 1 and 2: per-slab ranking and labelling, writing and verification.

**Pass 1** (:func:`rank_slab`) computes, for one slab in descending snapshot
order, the reference row order and identities from the input's own columns:

- the key is (``ForestIndex`` ascending, upid ascending, pid ascending,
  ``MostBoundID`` ascending), where upid is the FoF central's ``MostBoundID``
  (the ``FirstHaloInFOFgroup`` row's) and pid is -1 for a central, else upid
  -- the post-fix-up reference key restricted to one snapshot, whose
  descending-snapshot term is the pass order itself. Within a slab every
  ``MostBoundID`` is distinct, so the key is total; a repeated key is refused;
- ``HaloRankInForest`` continues each forest's running count over the slabs
  already processed (higher snapshots), ``SourceHaloID`` is the forest's base
  plus the rank, and ``ForestIndex`` is carried. The computed rank must equal
  the input's ``HaloRankInForest`` for every row, which proves the derivation
  on the input rather than assuming it;
- every halo is labelled with its tree: a terminal halo (``Descendant`` -1)
  with the ordinal of its own ``MostBoundID`` among the roots, every other
  with its descendant's label in the next slab. The per-tree counts
  accumulate the inventory's ``n_rows``.

Under a cut (:class:`CutSlab`) the labels come first and each halo's forest is
its tree's piece (``CutSlab.tree_forest``). Every halo whose FoF central's
piece differs from its own is promoted: it takes a central's key (upid its own
id, pid -1) and becomes a group of one. The rank derivation is proved against
the input only over the forests the table leaves uncut, since the others are
recomputed. The progenitor chains over the slab are then recomputed by the
reference insertion loop (``census.cut.chain_positions``: progenitors of one
descendant in ascending (upid, pid, id) encounter order, the first starts the
chain, each later one moves to the front if strictly more massive in
``M_Crit200`` than the current head, else joins the tail) over the new keys;
the same loop over the input's own keys must reproduce the input's stored
``NextProgenitor`` and ``FirstProgenitor``, refused otherwise, since the
rewriter could not then reproduce the uncut forests' chains either. The new
``NextProgenitor`` of this slab and ``FirstProgenitor`` of the next are kept in
the remap entry, and the slab's promotions and chain changes are counted.

**Pass 2** (:func:`write_slab`, :func:`verify_slab`) produces one output slab
from the input slab and the remap entries of slabs N+1, N and N-1: every
column gathered into the new order, every link remapped through its target
slab's inverse permutation (``Descendant`` into N+1, ``FirstProgenitor`` into
N-1, ``NextProgenitor`` and the FoF links into N) with its companion snapshot
column -1 iff the link is -1, the identities from the remap store, and
``SnapNum`` the slab's number. The file is written under a temporary name,
:func:`verify_slab` re-reads that temporary file, and the caller renames it to
its final name only on success. The verification checks:

- the object set, header attributes, dataset layouts and ``/schema`` the
  format fixes;
- every column against the input's under the permutation;
- every link's target row carries the ``MostBoundID`` the input's target row
  carried (through the target slab's permutation), FoF centrals stay
  self-referencing, and the companion columns name the target slab;
- ``SnapNum``, ``ForestIndex`` non-decreasing, ranks dense within each
  forest's run, and ``SourceHaloID`` the forest base plus the rank.

Under a cut the FoF chains are the runs of the new order (each group its
central then its members in ascending id, a promoted halo a group of one) and
the progenitor links come from the remap entries. The verification then
proves, for every row, that ``Descendant`` carries the ``MostBoundID`` it
carried in the input (the logical relation is unchanged) and that the FoF
central lies in the row's own forest; for every row of a forest the table
leaves uncut, that every link's target carries the input target's
``MostBoundID``, the null pattern is the input's and ``ForestIndex`` and
``HaloRankInForest`` are the input's (so ``UniqueGalaxyID`` is unchanged);
for every promoted halo, that it is its own group's first halo and last
member; and for the recomputed chains, that every ``FirstHaloInFOFgroup``
is a central whose members form one run, that the ``NextHaloInFOFgroup``
links chain exactly the members of each group, and that ``FirstProgenitor``
and ``NextProgenitor`` together chain exactly the progenitors of every
descendant, each progenitor once.

Memory per slab is stated in the package docstring; every array here is
int64-indexed and released as soon as the next step no longer needs it.
"""

import os
import sys
from dataclasses import dataclass
from typing import Dict, Mapping, Optional, Sequence, Tuple

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from census.cut import MASS_COLUMN, chain_positions  # noqa: E402
from column_schema import PayloadField  # noqa: E402
from errors import ConverterError  # noqa: E402
from hdf5_writer_v3 import (  # noqa: E402
    CHUNK_1D,
    HEADER_ATTRS,
    V3_SCHEMA_ATTRS,
    V3_SIDECAR_DATASETS,
    V3_STRING_ATTRS,
    V3_WRITE_BLOCK_ROWS,
    write_v3_sidecar,
    write_v3_snapshot_columns,
)
from horizontal_dataset import HorizontalDataset  # noqa: E402

#: Each link, its companion snapshot column (None for the FoF links) and the
#: offset of its target slab from the owner's.
LINKS: Tuple[Tuple[str, Optional[str], int], ...] = (
    ("Descendant", "DescendantSnapshot", 1),
    ("FirstProgenitor", "FirstProgenitorSnapshot", -1),
    ("NextProgenitor", "NextProgenitorSnapshot", 0),
    ("FirstHaloInFOFgroup", None, 0),
    ("NextHaloInFOFgroup", None, 0),
)
_LINK_NAMES = tuple(name for name, _companion, _offset in LINKS)
_LINK_INFO = {name: (companion, offset) for name, companion, offset in LINKS}
_COMPANIONS = {companion: name for name, companion, _offset in LINKS if companion is not None}
_IDENTITY = ("SourceHaloID", "ForestIndex", "HaloRankInForest")

#: Columns pass 1 reads from the input slab (a cut also reads ``M_Crit200``
#: and ``NextProgenitor``, and the next slab's ``FirstProgenitor``).
PASS1_COLUMNS = (
    "ForestIndex",
    "FirstHaloInFOFgroup",
    "MostBoundID",
    "HaloRankInForest",
    "Descendant",
)

#: What each per-snapshot cut count means, recorded with the counts (the
#: census's definitions of the same quantities).
CUT_COUNT_DEFINITIONS = {
    "promoted_halos": "members whose FoF central lies in another forest of the cut table; "
    "each becomes a central",
    "groups_losing_members": "FoF groups with at least one promoted member",
    "groups_central_leaves_members_stay": "(group, forest) remnants: a forest's members of one "
    "group whose central lies in another forest; each member becomes its own central",
    "remnants_with_several_members": "of those remnants, the ones holding two or more members: "
    "members that stay together in one forest but no longer share a group",
    "progenitor_order_changed": "descendants at the next snapshot whose progenitor chain over "
    "this slab changes",
    "first_progenitor_changed": "of those, the descendants whose first progenitor changes",
    "descendants_checked": "descendants at the next snapshot whose stored progenitor chain was "
    "reproduced by the reference insertion loop over the input's own keys",
}


def _examples(values, limit: int = 5) -> list:
    return list(values)[:limit]


def _rows(path, rows) -> str:
    return "{} rows {}".format(path, _examples(np.asarray(rows).tolist()))


# ---------------------------------------------------------------------------
# Pass 1
# ---------------------------------------------------------------------------


@dataclass
class SlabRemap:
    """One slab's remap entry: its new row order and identities (32 B per
    row). ``perm[new_row]`` is the input row now at ``new_row``. Under a cut
    it also holds the recomputed progenitor chains: this slab's
    ``NextProgenitor`` in the new order and the next slab's ``FirstProgenitor``
    in its new order (both naming this slab's new rows), 8 B per row each."""

    snap: int
    perm: np.ndarray  # int64 [n]
    forest: np.ndarray  # int64 [n], new order
    rank: np.ndarray  # int64 [n], new order
    sid: np.ndarray  # int64 [n], new order
    next_prog: Optional[np.ndarray] = None  # int64 [n], new order (cut)
    first_prog_up: Optional[np.ndarray] = None  # int64 [n of slab N+1], its new order (cut)

    @property
    def n(self) -> int:
        return int(self.perm.size)

    @property
    def nbytes(self) -> int:
        total = self.perm.nbytes + self.forest.nbytes + self.rank.nbytes + self.sid.nbytes
        for extra in (self.next_prog, self.first_prog_up):
            if extra is not None:
                total += extra.nbytes
        return int(total)

    def inverse(self) -> np.ndarray:
        """``inverse[old_row]`` = the new row of input row ``old_row``."""
        inverse = np.empty(self.n, dtype=np.int64)
        inverse[self.perm] = np.arange(self.n, dtype=np.int64)
        return inverse


def _int64(dataset: HorizontalDataset, snap: int, name: str) -> np.ndarray:
    return np.ascontiguousarray(dataset.read_column(snap, name), dtype=np.int64)


@dataclass
class CutSlab:
    """What the slab passes need of an installed cut table."""

    tree_forest: np.ndarray  # int64 [n_trees]: each root ordinal's output ForestIndex
    tree_original: np.ndarray  # int64 [n_trees]: each root ordinal's input ForestIndex
    split_old: np.ndarray  # bool [input forests]: the forests the table cuts
    split_new: np.ndarray  # bool [output forests]: pieces of a cut forest (kept or fresh)


def tree_labels(
    path, desc: np.ndarray, mbid: np.ndarray, roots: np.ndarray, labels_next: Optional[np.ndarray]
) -> np.ndarray:
    """Each halo's tree as a root ordinal: a terminal halo's own ``MostBoundID``
    among ``roots``, every other halo its descendant's label in the next slab
    (``labels_next``, in that slab's input row order). Works on a whole slab
    or on one block of it, since only ``desc`` and ``mbid`` are the block's."""
    n = int(desc.size)
    terminal = desc == -1
    labels = np.empty(n, dtype=np.int32)
    if terminal.any():
        ids = mbid[terminal]
        position = np.searchsorted(roots, ids)
        position = np.minimum(position, max(roots.size - 1, 0))
        unknown = roots.size == 0 or roots[position] != ids
        if np.any(unknown):
            raise ConverterError(
                "{}: {} terminal halo(s) whose MostBoundID is not a root; {}".format(
                    path, int(np.sum(unknown)), _rows(path, np.flatnonzero(terminal)[unknown])
                )
            )
        del ids, unknown
        labels[terminal] = position
        del position
    linked = ~terminal
    if linked.any():
        if labels_next is None:
            raise ConverterError(
                "{}: {} halo(s) of the last slab carry a Descendant; {}".format(
                    path, int(linked.sum()), _rows(path, np.flatnonzero(linked))
                )
            )
        targets = desc[linked]
        bad = (targets < 0) | (targets >= labels_next.size)
        if bad.any():
            raise ConverterError(
                "{}: {} Descendant value(s) outside [-1, {}); {}".format(
                    path, int(bad.sum()), labels_next.size, _rows(path, np.flatnonzero(linked)[bad])
                )
            )
        labels[linked] = labels_next[targets]
        del targets, bad
    return labels


def rank_slab(
    dataset: HorizontalDataset,
    snap: int,
    sid_base: np.ndarray,
    running: np.ndarray,
    roots: np.ndarray,
    labels_next: Optional[np.ndarray],
    tree_counts: np.ndarray,
    identity_from_input: bool = False,
    cut: Optional[CutSlab] = None,
    above: Optional[SlabRemap] = None,
) -> Tuple[SlabRemap, np.ndarray, Dict[str, int]]:
    """Pass 1 for one slab (module docstring). ``running`` and ``tree_counts``
    are advanced in place; ``labels_next`` is the next slab's labels in its
    input row order (``None`` for the last slab). With ``identity_from_input``
    (the no-op) the derived order must be the input's own: the permutation
    the identity and the derived ``ForestIndex``, ``HaloRankInForest`` and
    ``SourceHaloID`` the stored columns, refused otherwise naming the first
    row. With ``cut`` (and ``above``, the next slab's remap entry) the slab is
    ranked and chained under the table. Returns the remap entry, this slab's
    labels in input row order, and the cut's per-slab counts
    (:data:`CUT_COUNT_DEFINITIONS`; empty without a cut)."""
    path = dataset.snapshot_path(snap)
    n = dataset.n_halos[snap]
    if n == 0:
        empty = np.zeros(0, dtype=np.int64)
        remap = SlabRemap(snap=snap, perm=empty, forest=empty, rank=empty, sid=empty)
        counts: Dict[str, int] = {}
        if cut is not None:
            remap.next_prog = empty.copy()
            remap.first_prog_up = np.full(0 if above is None else above.n, -1, dtype=np.int64)
            counts = {name: 0 for name in CUT_COUNT_DEFINITIONS}
        return remap, np.zeros(0, dtype=np.int32), counts
    forest = _int64(dataset, snap, "ForestIndex")
    fhif = _int64(dataset, snap, "FirstHaloInFOFgroup")
    bad = (fhif < 0) | (fhif >= n)
    if bad.any():
        raise ConverterError(
            "{}: {} FirstHaloInFOFgroup value(s) outside [0, {}); {}".format(
                path, int(bad.sum()), n, _rows(path, np.flatnonzero(bad))
            )
        )
    not_central = fhif[fhif] != fhif
    if not_central.any():
        raise ConverterError(
            "{}: {} halo(s) whose FoF central is not self-referencing; {}".format(
                path, int(not_central.sum()), _rows(path, np.flatnonzero(not_central))
            )
        )
    del not_central
    mbid = _int64(dataset, snap, "MostBoundID")
    rows = np.arange(n, dtype=np.int64)
    upid = mbid[fhif]
    pid = np.where(fhif == rows, -1, upid)
    labels = None
    changed = None
    if cut is not None:
        desc = _int64(dataset, snap, "Descendant")
        labels = tree_labels(path, desc, mbid, roots, labels_next)
        # every row's stored forest must be its tree's: a row stored under
        # another forest cannot be re-homed, cut or not
        wrong = cut.tree_original[labels] != forest
        if wrong.any():
            raise ConverterError(
                "{}: {} halo(s) whose stored ForestIndex is not their tree's forest; examples "
                "(row, stored, tree's): {}".format(
                    path,
                    int(wrong.sum()),
                    [
                        (int(w), int(forest[w]), int(cut.tree_original[labels[w]]))
                        for w in np.flatnonzero(wrong)[:5]
                    ],
                )
            )
        del wrong
        key_forest = cut.tree_forest[labels]
        changed = cut.split_old[forest]
        foreign = forest[fhif] != forest
        if foreign.any():
            raise ConverterError(
                "{}: {} halo(s) whose FoF central lies in another forest of the input; hosts are "
                "resolved under the input's own partition, which must hold them; {}".format(
                    path, int(foreign.sum()), _rows(path, np.flatnonzero(foreign))
                )
            )
        del foreign
        promoted = (fhif != rows) & (key_forest[fhif] != key_forest)
        upid_old, pid_old = upid, pid
        upid = np.where(promoted, mbid, upid_old)
        pid = np.where((fhif == rows) | promoted, -1, upid)
        del rows
    else:
        key_forest = forest
        del fhif, rows
    perm = np.lexsort((mbid, pid, upid, key_forest))
    if identity_from_input:
        moved = perm != np.arange(n, dtype=np.int64)
        if moved.any():
            first = int(np.flatnonzero(moved)[0])
            raise ConverterError(
                "{}: the stored row order is not the (ForestIndex, HaloRankInForest) order the "
                "input's own columns derive: {} row(s) move, the first at row {} (input row {} "
                "belongs there); refusing to reproduce it".format(
                    path, int(moved.sum()), first, int(perm[first])
                )
            )
        del moved
    sorted_forest = key_forest[perm]
    del forest
    if cut is None:
        del key_forest  # the same array as forest
    if n > 1:
        same = sorted_forest[1:] == sorted_forest[:-1]
        for column in (upid, pid, mbid):
            ordered = column[perm]
            same &= ordered[1:] == ordered[:-1]
            del ordered
        if same.any():
            where = perm[1:][same]
            raise ConverterError(
                "{}: {} row(s) repeat another row's (ForestIndex, upid, pid, MostBoundID) key; "
                "the reference order needs distinct ids within a slab; {}".format(
                    path, int(same.sum()), _rows(path, where)
                )
            )
        del same
    if cut is None:
        del upid, pid
    starts = np.flatnonzero(np.r_[True, sorted_forest[1:] != sorted_forest[:-1]])
    lengths = np.diff(np.append(starts, n))
    run_forests = sorted_forest[starts]
    local = np.arange(n, dtype=np.int64) - np.repeat(starts, lengths)
    rank = np.repeat(running[run_forests], lengths) + local
    del local
    running[run_forests] += lengths
    sid = sid_base[sorted_forest] + rank
    rank_in = _int64(dataset, snap, "HaloRankInForest")[perm]
    bad = rank_in != rank
    if changed is not None:
        # the cut forests' ranks are recomputed; the derivation is proved on the others
        bad &= ~changed[perm]
    if bad.any():
        where = np.flatnonzero(bad)[:5]
        raise ConverterError(
            "{}: {} row(s){} whose HaloRankInForest differs from the reference rank derived from "
            "the input columns; examples (input row, held, derived): {}".format(
                path,
                int(bad.sum()),
                " of uncut forests" if cut is not None else "",
                [(int(perm[w]), int(rank_in[w]), int(rank[w])) for w in where],
            )
        )
    del rank_in, bad
    if identity_from_input:
        sid_in = _int64(dataset, snap, "SourceHaloID")
        bad = sid_in != sid
        if bad.any():
            first = int(np.flatnonzero(bad)[0])
            raise ConverterError(
                "{}: {} row(s) whose stored SourceHaloID differs from the forest base plus the "
                "derived rank, the first at row {} (stored {}, derived {}); refusing to "
                "reproduce it".format(
                    path, int(bad.sum()), first, int(sid_in[first]), int(sid[first])
                )
            )
        del sid_in, bad
    remap = SlabRemap(snap=snap, perm=perm, forest=sorted_forest, rank=rank, sid=sid)
    counts: Dict[str, int] = {}
    if cut is None:
        desc = _int64(dataset, snap, "Descendant")
        labels = tree_labels(path, desc, mbid, roots, labels_next)
        del desc, mbid
    else:
        counts = _cut_chains(
            dataset,
            snap,
            remap,
            key_forest,
            desc,
            mbid,
            fhif,
            upid,
            pid,
            upid_old,
            pid_old,
            promoted,
            above,
        )
    present, present_counts = np.unique(labels, return_counts=True)
    tree_counts[present] += present_counts
    return remap, labels, counts


def _cut_chains(
    dataset: HorizontalDataset,
    snap: int,
    remap: SlabRemap,
    forest_new: np.ndarray,
    desc: np.ndarray,
    mbid: np.ndarray,
    fhif: np.ndarray,
    upid: np.ndarray,
    pid: np.ndarray,
    upid_old: np.ndarray,
    pid_old: np.ndarray,
    promoted: np.ndarray,
    above: Optional[SlabRemap],
) -> Dict[str, int]:
    """The cut's progenitor chains over one slab (module docstring), into
    ``remap.next_prog`` and ``remap.first_prog_up``; returns the slab's
    counts. Every array is in input row order unless said otherwise."""
    path = dataset.snapshot_path(snap)
    n = remap.n
    n_next = 0 if above is None else above.n
    counts: Dict[str, int] = {
        "promoted_halos": int(np.count_nonzero(promoted)),
        "groups_losing_members": int(np.unique(fhif[promoted]).size),
        "groups_central_leaves_members_stay": 0,
        "remnants_with_several_members": 0,
        "progenitor_order_changed": 0,
        "first_progenitor_changed": 0,
        "descendants_checked": 0,
    }
    if promoted.any():
        remnants = np.unique(
            np.stack((fhif[promoted], forest_new[promoted]), axis=1), axis=0, return_counts=True
        )[1]
        counts["groups_central_leaves_members_stay"] = int(remnants.size)
        counts["remnants_with_several_members"] = int(np.count_nonzero(remnants >= 2))
        del remnants
    del fhif, promoted
    inverse = remap.inverse()
    next_prog = np.full(n, -1, dtype=np.int64)
    first_prog_up = np.full(n_next, -1, dtype=np.int64)
    prog = np.flatnonzero(desc != -1)
    if prog.size:
        # tree_labels has refused a Descendant on the last slab or out of range
        inverse_next = above.inverse()
        targets = desc[prog]
        group_size = np.bincount(targets, minlength=n_next)[targets]
        before = np.zeros(prog.size, dtype=np.int64)
        after = np.zeros(prog.size, dtype=np.int64)
        multi = group_size >= 2
        del group_size
        if multi.any():
            mass = np.ascontiguousarray(dataset.read_column(snap, MASS_COLUMN), dtype=np.float32)
            m = prog[multi]
            g = targets[multi]
            ids = mbid[m]
            masses = mass[m]
            del mass
            after[multi] = chain_positions(g, upid[m], pid[m], ids, masses)
            del upid, pid
            before[multi] = chain_positions(g, upid_old[m], pid_old[m], ids, masses)
            del upid_old, pid_old, ids, masses, m, g
        del multi, mbid
        # the input's stored chains must be the loop's over the input's own keys
        order = np.lexsort((before, targets))
        grouped = targets[order]
        chained = prog[order]
        continues = np.r_[grouped[1:] == grouped[:-1], False]
        starts = np.r_[True, grouped[1:] != grouped[:-1]]
        expected_next = np.where(continues, np.r_[chained[1:], -1], -1)
        stored_next = _int64(dataset, snap, "NextProgenitor")[chained]
        stored_first = _int64(dataset, snap + 1, "FirstProgenitor")
        has_progenitor = np.zeros(n_next, dtype=bool)
        has_progenitor[targets] = True
        bad_next = stored_next != expected_next
        bad_first = stored_first[grouped[starts]] != chained[starts]
        bad_none = (stored_first != -1) & ~has_progenitor
        if bad_next.any() or bad_first.any() or bad_none.any():
            descendants = np.unique(
                np.concatenate(
                    [grouped[bad_next], grouped[starts][bad_first], np.flatnonzero(bad_none)]
                )
            )
            raise ConverterError(
                "{}: the input's progenitor chains into snapshot {} are not the reference "
                "insertion order over the input's own keys for {} descendant(s) ({} NextProgenitor "
                "link(s) and {} FirstProgenitor link(s) differ); the cut recomputes chains by that "
                "order and cannot reproduce these; descendant rows of snapshot {}: {}".format(
                    path,
                    snap + 1,
                    descendants.size,
                    int(bad_next.sum()),
                    int(bad_first.sum() + bad_none.sum()),
                    snap + 1,
                    _examples(descendants.tolist()),
                )
            )
        counts["descendants_checked"] = int(np.count_nonzero(starts))
        del stored_next, stored_first, has_progenitor, bad_next, bad_first, bad_none
        del order, grouped, chained, continues, starts, expected_next
        # the output chains, in the new rows of this slab and the next
        order = np.lexsort((after, targets))
        grouped = targets[order]
        new_rows = inverse[prog[order]]
        continues = np.r_[grouped[1:] == grouped[:-1], False]
        starts = np.r_[True, grouped[1:] != grouped[:-1]]
        next_prog[new_rows] = np.where(continues, np.r_[new_rows[1:], -1], -1)
        first_prog_up[inverse_next[grouped[starts]]] = new_rows[starts]
        moved = before != after
        counts["progenitor_order_changed"] = int(np.unique(targets[moved]).size)
        counts["first_progenitor_changed"] = int(
            np.unique(targets[(before == 0) & (after != 0)]).size
        )
        del order, grouped, new_rows, continues, starts, moved, before, after, inverse_next
    del inverse
    remap.next_prog = next_prog
    remap.first_prog_up = first_prog_up
    return counts


# ---------------------------------------------------------------------------
# Pass 2
# ---------------------------------------------------------------------------


@dataclass
class Window:
    """The remap entries pass 2 resolves one slab's links through."""

    own: SlabRemap
    above: Optional[SlabRemap]  # slab N+1, the Descendant target; None for the last
    below: Optional[SlabRemap]  # slab N-1, the FirstProgenitor target; None for the first

    def target(self, offset: int) -> Optional[SlabRemap]:
        return {1: self.above, 0: self.own, -1: self.below}[offset]


class SlabColumns:
    """The output columns of one slab, computed on demand from the input slab
    and the window: one column resident at a time, the target slab's inverse
    permutation while a link is remapped, and the null masks of the links.
    With ``identity_from_input`` the identity columns are the input's own
    (which pass 1 has proved equal to the derived ones). With ``cut`` the FoF
    links are built from the new order (:meth:`fof`) and the progenitor links
    come from the remap entries; ``Descendant`` is remapped as always."""

    def __init__(
        self,
        dataset: HorizontalDataset,
        snap: int,
        window: Window,
        table: Mapping[str, Tuple[np.dtype, bool]],
        identity_from_input: bool = False,
        cut: Optional[CutSlab] = None,
    ):
        self.dataset = dataset
        self.snap = snap
        self.window = window
        self.table = table
        self.identity_from_input = identity_from_input
        self.cut = cut
        self.path = dataset.snapshot_path(snap)
        self.n = window.own.n
        self.null: Dict[str, np.ndarray] = {}
        self.n_links: Dict[str, int] = {}
        self._fof: Optional[Tuple[np.ndarray, np.ndarray, np.ndarray]] = None

    def old_link(self, name: str) -> np.ndarray:
        """The input's link column, gathered into the new row order (so each
        value is still an input row of the target slab)."""
        return _int64(self.dataset, self.snap, name)[self.window.own.perm]

    def fof(self) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
        """The cut's FoF chains in the new order: each row's central (its
        input central's new row, or itself when promoted, that is when the
        central's forest differs from its own), the next member (the next row
        while it shares the central, else -1) and the promotion mask. The new
        order sorts each forest by (central's id, -1 for a central else that
        id, own id), so a group is one run headed by its central; a run that
        is not is refused."""
        if self._fof is not None:
            return self._fof
        own = self.window.own
        n = self.n
        old = self.old_link("FirstHaloInFOFgroup")
        bad = (old < 0) | (old >= n)
        if bad.any():
            raise ConverterError(
                "{}: {} FirstHaloInFOFgroup value(s) outside [0, {}); {}".format(
                    self.path, int(bad.sum()), n, _rows(self.path, own.perm[bad])
                )
            )
        inverse = own.inverse()
        central = inverse[old]
        del inverse, old
        rows = np.arange(n, dtype=np.int64)
        promoted = (central != rows) & (own.forest[central] != own.forest)
        central[promoted] = rows[promoted]
        starts = np.r_[True, central[1:] != central[:-1]] if n else np.zeros(0, dtype=bool)
        heads = central[starts]
        if not np.array_equal(heads, np.flatnonzero(starts)) or np.unique(heads).size != heads.size:
            raise ConverterError(
                "{}: the new row order does not place each FoF central at the head of one run "
                "of its members".format(self.path)
            )
        del heads
        same_next = np.r_[central[1:] == central[:-1], False] if n else np.zeros(0, dtype=bool)
        next_fof = np.where(same_next, rows + 1, -1)
        del same_next, starts, rows
        self._fof = (central, next_fof, promoted)
        return self._fof

    def link(self, name: str) -> np.ndarray:
        companion, offset = _LINK_INFO[name]
        old = self.old_link(name)
        target = self.window.target(offset)
        null = old == -1
        if name == "FirstHaloInFOFgroup" and null.any():
            raise ConverterError(
                "{}: {} FirstHaloInFOFgroup value(s) of -1; {}".format(
                    self.path, int(null.sum()), _rows(self.path, self.window.own.perm[null])
                )
            )
        n_target = 0 if target is None else target.n
        bad = ~null & ((old < 0) | (old >= n_target))
        if bad.any():
            raise ConverterError(
                "{}: {} {} value(s) outside [-1, {}) of snapshot {}; {}".format(
                    self.path,
                    int(bad.sum()),
                    name,
                    n_target,
                    self.snap + offset,
                    _rows(self.path, self.window.own.perm[bad]),
                )
            )
        if companion is not None and self.dataset.format_version >= 3:
            held = _int64(self.dataset, self.snap, companion)[self.window.own.perm]
            expected = np.where(null, -1, self.snap + offset)
            bad = held != expected
            if bad.any():
                raise ConverterError(
                    "{}: {} {} value(s) that are not -1 iff {} is -1 and otherwise snapshot "
                    "{}; {}".format(
                        self.path,
                        int(bad.sum()),
                        companion,
                        name,
                        self.snap + offset,
                        _rows(self.path, self.window.own.perm[bad]),
                    )
                )
            del held, expected
        if self.cut is not None and name != "Descendant":
            new = self._cut_link(name, target)
            null = new == -1
        else:
            new = np.full(self.n, -1, dtype=np.int64)
            if target is not None and (~null).any():
                inverse = target.inverse()
                new[~null] = inverse[old[~null]]
                del inverse
        self.null[name] = null
        self.n_links[name] = int(np.count_nonzero(~null))
        return new

    def _cut_link(self, name: str, target: Optional[SlabRemap]) -> np.ndarray:
        """A recomputed link under the cut (class docstring)."""
        if name == "FirstProgenitor":
            if target is None:
                return np.full(self.n, -1, dtype=np.int64)
            held = target.first_prog_up
        elif name == "NextProgenitor":
            held = self.window.own.next_prog
        else:
            central, next_fof, _promoted = self.fof()
            held = central if name == "FirstHaloInFOFgroup" else next_fof
        if held is None or held.size != self.n:
            raise ConverterError(
                "{}: the remap store holds no {} for the cut ({} value(s) for {} rows)".format(
                    self.path, name, None if held is None else held.size, self.n
                )
            )
        return np.ascontiguousarray(held, dtype=np.int64)

    def companion(self, name: str) -> np.ndarray:
        link = _COMPANIONS[name]
        if link not in self.null:
            self.link(link)
        offset = _LINK_INFO[link][1]
        return np.where(self.null[link], -1, self.snap + offset).astype(np.int32)

    def payload(self, name: str) -> np.ndarray:
        dtype, _is_vec = self.table[name]
        values = self.dataset.read_column(self.snap, name)
        if name == "SnapNum":
            bad = np.asarray(values, dtype=np.int64) != self.snap
            if bad.any():
                raise ConverterError(
                    "{}: {} SnapNum value(s) other than {}; {}".format(
                        self.path, int(bad.sum()), self.snap, _rows(self.path, np.flatnonzero(bad))
                    )
                )
        return np.ascontiguousarray(values[self.window.own.perm], dtype=dtype)

    def identity(self, name: str) -> np.ndarray:
        if self.identity_from_input:
            return _int64(self.dataset, self.snap, name)
        return {
            "SourceHaloID": self.window.own.sid,
            "ForestIndex": self.window.own.forest,
            "HaloRankInForest": self.window.own.rank,
        }[name]

    def __call__(self, name: str) -> np.ndarray:
        if name in _LINK_NAMES:
            return self.link(name)
        if name in _COMPANIONS:
            return self.companion(name)
        if name in _IDENTITY:
            return self.identity(name)
        return self.payload(name)


def write_slab(
    columns: SlabColumns,
    path,
    header: Mapping[str, object],
    declarations: Sequence[PayloadField],
    block_rows: int,
) -> Dict[str, int]:
    """Write one output slab to ``path`` (a temporary name; the caller renames
    it after :func:`verify_slab`). Returns the non-null count per link."""
    write_v3_snapshot_columns(
        path,
        header,
        columns.table,
        declarations,
        columns.n,
        columns,
        block_rows=block_rows,
        track_times=False,
    )
    return dict(columns.n_links)


def _require_hard_links(group, path) -> None:
    for name in group:
        link = group.get(name, getlink=True)
        if not isinstance(link, h5py.HardLink):
            raise ConverterError(
                "{}: {}/{} is a {}, not a physically present object".format(
                    path, group.name.rstrip("/"), name, type(link).__name__
                )
            )


def _check_layout(dataset, dtype: np.dtype, is_vec: bool, n_rows: int, path) -> None:
    shape = (n_rows, 3) if is_vec else (n_rows,)
    problems = []
    if dataset.dtype.str != dtype.str:
        problems.append("dtype {} != {}".format(dataset.dtype.str, dtype.str))
    if dataset.shape != shape:
        problems.append("shape {} != {}".format(dataset.shape, shape))
    chunks = dataset.chunks
    if (
        chunks is None
        or len(chunks) != len(shape)
        or not 1 <= chunks[0] <= CHUNK_1D[0]
        or (is_vec and chunks[1] != 3)
    ):
        problems.append("chunks {} (rows must be in [1, {}])".format(chunks, CHUNK_1D[0]))
    if dataset.compression is not None or dataset.shuffle or dataset.fletcher32:
        problems.append("filtered")
    if dataset.scaleoffset is not None:
        problems.append("scale-offset")
    if dataset.external or dataset.is_virtual:
        problems.append("not stored in this file")
    if dataset.attrs.keys():
        problems.append("carries attributes {}".format(sorted(dataset.attrs.keys())))
    if problems:
        raise ConverterError("{}: {}: {}".format(path, dataset.name, "; ".join(problems)))


def _check_structure(
    handle,
    path,
    header: Mapping[str, object],
    table: Mapping[str, Tuple[np.dtype, bool]],
    declarations: Sequence[PayloadField],
    n_halos: int,
) -> None:
    if set(handle.keys()) != {"header", "halos", "schema"} or handle.attrs.keys():
        raise ConverterError(
            "{}: root holds {} (attributes {}), not exactly /header, /halos, /schema".format(
                path, sorted(handle.keys()), sorted(handle.attrs.keys())
            )
        )
    _require_hard_links(handle, path)
    attrs = handle["header"].attrs
    expected_names = set(HEADER_ATTRS) | set(V3_STRING_ATTRS)
    if set(attrs.keys()) != expected_names:
        raise ConverterError(
            "{}: header attributes {} != {}".format(
                path, sorted(attrs.keys()), sorted(expected_names)
            )
        )
    for name, dtype in HEADER_ATTRS.items():
        stored = np.asarray(attrs[name])
        if stored.dtype != np.dtype(dtype) or stored.shape != () or stored != header[name]:
            raise ConverterError(
                "{}: header {} is {!r} ({}), expected {!r}".format(
                    path, name, stored, stored.dtype, header[name]
                )
            )
    for name, size in V3_STRING_ATTRS.items():
        kind = attrs.get_id(name).get_type()
        if (
            not isinstance(kind, h5py.h5t.TypeStringID)
            or kind.is_variable_str()
            or kind.get_size() != size
            or kind.get_cset() != h5py.h5t.CSET_ASCII
        ):
            raise ConverterError(
                "{}: header {} is not {}-byte fixed ASCII".format(path, name, size)
            )
        if attrs[name] != str(header[name]).encode("ascii"):
            raise ConverterError(
                "{}: header {} is {!r}, expected {!r}".format(path, name, attrs[name], header[name])
            )
    halos = handle["halos"]
    if set(halos.keys()) != set(table) or halos.attrs.keys():
        raise ConverterError(
            "{}: /halos holds {} (attributes {}), expected exactly {}".format(
                path, sorted(halos.keys()), sorted(halos.attrs.keys()), sorted(table)
            )
        )
    _require_hard_links(halos, path)
    for name, (dtype, is_vec) in table.items():
        _check_layout(halos[name], dtype, is_vec, n_halos, path)
    schema_group = handle["schema"]
    wanted = {declaration.name: declaration for declaration in declarations}
    if set(schema_group.keys()) != set(wanted) or schema_group.attrs.keys():
        raise ConverterError(
            "{}: /schema declares {}, expected exactly {}".format(
                path, sorted(schema_group.keys()), sorted(wanted)
            )
        )
    _require_hard_links(schema_group, path)
    for name, declaration in wanted.items():
        group = schema_group[name]
        if (
            not isinstance(group, h5py.Group)
            or len(group)
            or set(group.attrs.keys()) != set(V3_SCHEMA_ATTRS)
        ):
            raise ConverterError(
                "{}: /schema/{} must be a childless group with exactly {}".format(
                    path, name, V3_SCHEMA_ATTRS
                )
            )
        for key in V3_SCHEMA_ATTRS:
            kind = group.attrs.get_id(key).get_type()
            if (
                not isinstance(kind, h5py.h5t.TypeStringID)
                or not kind.is_variable_str()
                or kind.get_cset() != h5py.h5t.CSET_UTF8
                or group.attrs[key] != getattr(declaration, key)
            ):
                raise ConverterError(
                    "{}: /schema/{} {} is {!r}, expected UTF-8 {!r}".format(
                        path, name, key, group.attrs[key], getattr(declaration, key)
                    )
                )


def _read_blocks(dataset, n_rows: int, block_rows: int):
    for start in range(0, n_rows, block_rows):
        yield start, dataset[start : min(start + block_rows, n_rows)]


def _compare(written, expected: np.ndarray, block_rows: int, path, name: str) -> None:
    """A written dataset against its expected column, block by block, as bytes."""
    if written.shape != expected.shape:
        raise ConverterError(
            "{}: /halos/{} has shape {}, expected {}".format(
                path, name, written.shape, expected.shape
            )
        )
    for start, block in _read_blocks(written, expected.shape[0], block_rows):
        stop = start + block.shape[0]
        wanted = np.ascontiguousarray(expected[start:stop], dtype=expected.dtype)
        if block.tobytes() != wanted.tobytes():
            raise ConverterError(
                "{}: /halos/{} rows [{}, {}) differ from the input under the permutation".format(
                    path, name, start, stop
                )
            )


def verify_slab(
    path,
    columns: SlabColumns,
    header: Mapping[str, object],
    declarations: Sequence[PayloadField],
    sid_base: np.ndarray,
    block_rows: int,
) -> None:
    """Re-read one written slab and check everything the module docstring
    lists; raise on the first difference. The input ``MostBoundID`` of one
    target slab at a time is read for the link check (the links are taken in
    target order, so each slab's column is read once)."""
    snap = columns.snap
    own = columns.window.own
    n = own.n
    dataset = columns.dataset
    cut = columns.cut
    # under a cut: the rows of forests the table changes, and the promoted rows
    touched = None if cut is None else cut.split_new[own.forest]
    promoted = None if cut is None else columns.fof()[2]
    with h5py.File(path, "r") as handle:
        _check_structure(handle, path, header, columns.table, declarations, n)
        halos = handle["halos"]
        for name in columns.table:
            expected = columns(name)
            _compare(halos[name], expected, block_rows, path, name)
            del expected
        if columns.identity_from_input:
            for name in _IDENTITY:
                if not np.array_equal(
                    np.asarray(halos[name][...], dtype=np.int64), _int64(dataset, snap, name)
                ):
                    raise ConverterError(
                        "{}: /halos/{} differs from the input's stored column".format(path, name)
                    )
        # links: the target row of every written link carries the MostBoundID
        # of the input's target row, through the target slab's permutation
        mbid_snap, mbid_target = None, None
        for name, companion, offset in sorted(LINKS, key=lambda link: link[2]):
            target = columns.window.target(offset)
            written = np.asarray(halos[name][...], dtype=np.int64)
            old = columns.old_link(name)
            null = old == -1
            # which rows must carry the input's own link target: every row,
            # or under a cut every row of an uncut forest (the recomputed
            # chains are proved structurally below) and, for the FoF central,
            # every row that is not promoted; Descendant keeps the input's
            # target everywhere and FirstProgenitor its null pattern (a halo
            # has a progenitor after the cut iff it had one)
            if cut is None or name == "Descendant":
                as_input = np.ones(n, dtype=bool)
            elif name == "FirstHaloInFOFgroup":
                as_input = ~promoted
            else:
                as_input = ~touched
            null_as_input = np.ones(n, dtype=bool) if name == "FirstProgenitor" else as_input
            if not np.array_equal((written == -1)[null_as_input], null[null_as_input]):
                raise ConverterError(
                    "{}: /halos/{} null rows differ from the input's".format(path, name)
                )
            compare = as_input & ~null
            if target is not None and compare.any():
                if mbid_snap != target.snap:
                    mbid_target = _int64(dataset, target.snap, "MostBoundID")
                    mbid_snap = target.snap
                actual = mbid_target[target.perm[written[compare]]]
                wanted = mbid_target[old[compare]]
                if not np.array_equal(actual, wanted):
                    bad = np.flatnonzero(compare)[actual != wanted]
                    raise ConverterError(
                        "{}: {} /halos/{} link(s) whose target row does not carry the input "
                        "target's MostBoundID; {}".format(path, bad.size, name, _rows(path, bad))
                    )
                del actual, wanted
            del compare, as_input, null_as_input
            if name == "FirstHaloInFOFgroup":
                rows = np.arange(n, dtype=np.int64)
                central_in = old == own.perm
                if promoted is not None:
                    central_in |= promoted
                if not np.array_equal(written == rows, central_in):
                    raise ConverterError(
                        "{}: /halos/FirstHaloInFOFgroup self-references differ from the "
                        "input's{}".format(path, " and the promotions" if cut is not None else "")
                    )
            if companion is not None:
                held = np.asarray(halos[companion][...], dtype=np.int64)
                expected = np.where(written == -1, -1, snap + offset)
                if not np.array_equal(held, expected):
                    raise ConverterError(
                        "{}: /halos/{} is not -1 iff {} is -1 and otherwise {}".format(
                            path, companion, name, snap + offset
                        )
                    )
            del written, old, null
        del mbid_target
        # identities and SnapNum
        snapnum = np.asarray(halos["SnapNum"][...], dtype=np.int64)
        if np.any(snapnum != snap):
            raise ConverterError("{}: /halos/SnapNum holds values other than {}".format(path, snap))
        forest = np.asarray(halos["ForestIndex"][...], dtype=np.int64)
        rank = np.asarray(halos["HaloRankInForest"][...], dtype=np.int64)
        sid = np.asarray(halos["SourceHaloID"][...], dtype=np.int64)
        if n > 1:
            if np.any(forest[1:] < forest[:-1]):
                raise ConverterError("{}: /halos/ForestIndex is not non-decreasing".format(path))
            same = forest[1:] == forest[:-1]
            if np.any(rank[1:][same] != rank[:-1][same] + 1):
                raise ConverterError(
                    "{}: /halos/HaloRankInForest is not dense within a forest's run".format(path)
                )
            if np.any(sid[1:] <= sid[:-1]):
                raise ConverterError(
                    "{}: /halos/SourceHaloID is not strictly ascending".format(path)
                )
        if np.any(sid != sid_base[forest] + rank):
            raise ConverterError(
                "{}: /halos/SourceHaloID is not the forest base plus the rank".format(path)
            )
        if (
            not np.array_equal(forest, own.forest)
            or not np.array_equal(rank, own.rank)
            or not np.array_equal(sid, own.sid)
        ):
            raise ConverterError("{}: identity columns differ from the remap store's".format(path))
        if cut is not None:
            _verify_cut(handle, path, columns, touched, promoted, forest, rank)


def _verify_cut(handle, path, columns: SlabColumns, touched, promoted, forest, rank) -> None:
    """The cut's structural proof over one written slab (module docstring):
    the uncut forests' identity, the promoted halos' groups, every central in
    its row's forest, and the recomputed FoF and progenitor chains covering
    each group and each descendant's progenitors exactly once."""
    halos = handle["halos"]
    own = columns.window.own
    dataset = columns.dataset
    snap = columns.snap
    n = own.n
    rows = np.arange(n, dtype=np.int64)
    untouched = ~touched
    for name, written in (("ForestIndex", forest), ("HaloRankInForest", rank)):
        held = _int64(dataset, snap, name)[own.perm]
        if not np.array_equal(written[untouched], held[untouched]):
            raise ConverterError(
                "{}: /halos/{} differs from the input's for {} row(s) of uncut forests".format(
                    path, name, int(np.count_nonzero(written[untouched] != held[untouched]))
                )
            )
        del held
    central = np.asarray(halos["FirstHaloInFOFgroup"][...], dtype=np.int64)
    next_fof = np.asarray(halos["NextHaloInFOFgroup"][...], dtype=np.int64)
    if promoted.any() and (
        np.any(central[promoted] != rows[promoted]) or np.any(next_fof[promoted] != -1)
    ):
        raise ConverterError(
            "{}: a promoted halo is not its own FoF group's first and only member".format(path)
        )
    if np.any(forest[central] != forest):
        raise ConverterError(
            "{}: {} halo(s) whose FoF central lies in another forest of the output".format(
                path, int(np.count_nonzero(forest[central] != forest))
            )
        )
    linked = next_fof != -1
    members = next_fof[linked]
    if (
        np.any(central[members] != central[linked])
        or np.unique(members).size != members.size
        or members.size != n - np.unique(central).size
    ):
        raise ConverterError(
            "{}: the NextHaloInFOFgroup links do not chain exactly the members of each FoF "
            "group".format(path)
        )
    del central, next_fof, linked, members
    # progenitor chains: the heads written into the next slab (its verified
    # FirstProgenitor) and this slab's NextProgenitor links name every
    # progenitor of a descendant exactly once, each in its descendant's chain
    desc = np.asarray(halos["Descendant"][...], dtype=np.int64)
    next_prog = np.asarray(halos["NextProgenitor"][...], dtype=np.int64)
    heads = own.first_prog_up if own.first_prog_up is not None else np.zeros(0, dtype=np.int64)
    head_rows = heads[heads != -1]
    linked = next_prog != -1
    chained = next_prog[linked]
    named = np.concatenate([head_rows, chained])
    if (
        np.any(desc[head_rows] != np.flatnonzero(heads != -1))
        or np.any(desc[chained] != desc[linked])
        or np.unique(named).size != named.size
        or named.size != int(np.count_nonzero(desc != -1))
    ):
        raise ConverterError(
            "{}: the FirstProgenitor and NextProgenitor links do not chain exactly the "
            "progenitors of each descendant".format(path)
        )
    del desc, next_prog, heads, head_rows, linked, chained, named
    # this slab's FirstProgenitor names, for every halo with one, a halo of the
    # previous slab whose input Descendant is this halo
    first = np.asarray(halos["FirstProgenitor"][...], dtype=np.int64)
    linked = first != -1
    below = columns.window.below
    if linked.any():
        if below is None:
            raise ConverterError("{}: FirstProgenitor links below the first slab".format(path))
        desc_below = _int64(dataset, snap - 1, "Descendant")[below.perm]
        inverse = own.inverse()
        if np.any(inverse[desc_below[first[linked]]] != rows[linked]):
            raise ConverterError(
                "{}: a FirstProgenitor target is not a progenitor of its halo".format(path)
            )
        del desc_below, inverse


# ---------------------------------------------------------------------------
# The sidecar
# ---------------------------------------------------------------------------


def _sidecar_blocks(sidecar: Mapping[str, np.ndarray], rows: int):
    n = int(sidecar["ForestID"].size)
    for start in range(0, n, rows):
        yield {
            name: np.ascontiguousarray(sidecar[name][start : start + rows], dtype="<i8")
            for name in V3_SIDECAR_DATASETS
        }


def write_sidecar(path, sidecar: Mapping[str, np.ndarray]) -> int:
    """``forests.h5`` through the writer's sidecar helper, in its block size,
    so the chunk shape is the converter's own."""
    return write_v3_sidecar(path, _sidecar_blocks(sidecar, V3_WRITE_BLOCK_ROWS))


def verify_sidecar(path, sidecar: Mapping[str, np.ndarray]) -> None:
    """Re-read ``forests.h5``: exactly the three int64 root datasets, the
    format's layout, and the expected values."""
    n = int(sidecar["ForestID"].size)
    with h5py.File(path, "r") as handle:
        if list(handle.keys()) != list(V3_SIDECAR_DATASETS) or handle.attrs.keys():
            raise ConverterError(
                "{}: holds {} (attributes {}), not exactly {}".format(
                    path, list(handle.keys()), sorted(handle.attrs.keys()), V3_SIDECAR_DATASETS
                )
            )
        _require_hard_links(handle, path)
        for name in V3_SIDECAR_DATASETS:
            _check_layout(handle[name], np.dtype("<i8"), False, n, path)
            if (
                handle[name][...].tobytes()
                != np.ascontiguousarray(sidecar[name], dtype="<i8").tobytes()
            ):
                raise ConverterError("{}: /{} differs from the expected values".format(path, name))
