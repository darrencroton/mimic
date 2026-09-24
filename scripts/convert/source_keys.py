"""Source-key joins and independent topology closure checks for the bounded
transpose (Slice 6 of the converter generalisation plan,
docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md, contracts
C1/C3/C4).

Canonical adapter batches carry every link as the target's ``SourceHaloID``
(``adapters/base.py``). A v3 file needs the target's *snapshot-local row* and,
for the three progenitor/descendant links, its *snapshot*. This module turns
one into the other without any catalog-sized id dictionary, id array or
whole-forest allocation, and checks the resulting topology on the way.

**Global positions.** ``transpose.py`` sorts every halo into snapshot-major,
ascending-``SourceHaloID`` order. A halo's *global position* ``gp`` is its
index in that order; :class:`SnapshotLayout` holds the per-snapshot counts and
their int64 prefix sums, so ``gp`` converts to ``(snapshot, row)`` and back
with a binary search over O(number of snapshots) values. Every position,
offset and count is int64 (or an exact Python int while it is being summed),
and a sum that would leave int64 aborts before anything is narrowed.

**One stream, one pass.** For every halo, :func:`build_join_records` emits a
*map entry* keyed by its own ``SourceHaloID`` and one *request* per non-null
link, keyed by the link's target ``SourceHaloID``. Externally sorted by
``(key, tag, gp)``, each halo's map entry is immediately followed by every
request that targets it, so :class:`SourceKeyJoin` resolves all five link
kinds and runs every closure check in a single streaming pass, carrying only
the group that straddles a block boundary. A request with no map entry in
front of it names a halo this conversion does not contain; two map entries
with one key are a duplicated source key.

**Closure checks** (independent of any adapter's own validation, and of the
writer's; each is a rejection, never a repair):

- every link target exists and lies in the owner's ``ForestIndex``;
- ``Descendant`` points to a strictly later snapshot and ``FirstProgenitor``
  to a strictly earlier one; FoF links stay in the owner's snapshot;
- ``FirstProgenitor`` round-trips (its target's ``Descendant`` is the owner);
- every ``NextProgenitor`` target names the owner's own descendant, which must
  exist -- descendant-relative, never owner-relative, so a sibling may sit at
  an earlier, equal or later snapshot than its owner (v3 invariant 2);
- a ``FirstHaloInFOFgroup`` target is a central (self-referencing), and every
  ``NextHaloInFOFgroup`` target names the owner's central;
- **coverage**: a halo with a descendant is reached exactly once -- as its
  descendant's ``FirstProgenitor`` or as one sibling's ``NextProgenitor`` --
  and a halo without one is reached by neither; a non-central is reached by
  exactly one ``NextHaloInFOFgroup`` and a central by none;
- **acyclicity** of both singly linked chains, by external pointer jumping
  (:func:`verify_chains_acyclic`).

Coverage plus acyclicity is what makes each chain *exactly* its group: with
every in-degree at most one, a group decomposes into simple paths and cycles,
the only in-degree-zero member is the head, so there is one path, it starts at
the head, and acyclicity leaves nothing outside it. Nothing here reorders a
chain -- the checks read the source's chains, and the transpose remaps them.

numpy + stdlib only.
"""

from dataclasses import dataclass, field
from typing import Callable, Dict, Mapping, Optional, Sequence, Tuple

import numpy as np
from adapters.base import INT64_MAX, LINK_FIELDS, NULL_LINK
from column_schema import ConverterError
from rank_sort import KeyedSorter

__all__ = [
    "CHAIN_DTYPE",
    "CHAIN_KEY",
    "CHAIN_LINKS",
    "CHAIN_ROUND_SCRATCH_BYTES_PER_RECORD",
    "INT32_MAX",
    "JOIN_BUILD_BYTES_PER_ROW",
    "JOIN_DTYPE",
    "JOIN_KEY",
    "JOIN_SCRATCH_BYTES_PER_RECORD",
    "LINK_TAG",
    "RESOLVED_DTYPE",
    "RESOLVED_KEY",
    "JoinStats",
    "SnapshotLayout",
    "SourceKeyJoin",
    "build_join_records",
    "validate_snapshots",
    "verify_chains_acyclic",
]

INT32_MAX = int(np.iinfo(np.int32).max)

#: Tag of a halo's own map entry. It sorts before every request for the same
#: key, which is what lets the join see a target before the links into it.
TAG_MAP_ENTRY = 0

#: Tag of a request made through each link field: 1..5 in the fixed v3 order.
LINK_TAG: Dict[str, int] = {name: index + 1 for index, name in enumerate(LINK_FIELDS)}
_TAG_NAME: Dict[int, str] = {tag: name for name, tag in LINK_TAG.items()}
_TAG_DESCENDANT = LINK_TAG["Descendant"]
_TAG_FIRST_PROGENITOR = LINK_TAG["FirstProgenitor"]
_TAG_NEXT_PROGENITOR = LINK_TAG["NextProgenitor"]
_TAG_FIRST_FOF = LINK_TAG["FirstHaloInFOFgroup"]
_TAG_NEXT_FOF = LINK_TAG["NextHaloInFOFgroup"]

#: The join stream's record.
#:
#: - map entry (``tag == 0``): ``key`` = the halo's SourceHaloID, ``gp`` its
#:   global position, ``forest`` its ForestIndex, ``a`` its own Descendant
#:   source key and ``b`` its own FirstHaloInFOFgroup source key;
#: - request (``tag`` = :data:`LINK_TAG`): ``key`` = the target SourceHaloID,
#:   ``gp``/``forest`` the owner's, ``a`` the value the target must carry for
#:   the link to close (see :func:`build_join_records`) and ``b`` the owner's
#:   SourceHaloID, for diagnostics.
#:
#: ``(key, tag, gp)`` is a total order: one map entry per halo, and at most one
#: request per owner and link kind.
JOIN_DTYPE = np.dtype(
    [
        ("key", "<i8"),
        ("tag", "<i8"),
        ("gp", "<i8"),
        ("forest", "<i8"),
        ("a", "<i8"),
        ("b", "<i8"),
    ]
)
JOIN_KEY = ("key", "tag", "gp")

#: One resolved link: the owner's global position, the link kind (index into
#: ``LINK_FIELDS``) and the target's global position. ``(owner_gp, kind)`` is
#: unique, so it is a total order.
RESOLVED_DTYPE = np.dtype([("owner_gp", "<i8"), ("kind", "<i8"), ("target_gp", "<i8")])
RESOLVED_KEY = ("owner_gp", "kind")

#: The two singly linked chains whose acyclicity needs checking. The other
#: three links cannot close a cycle: ``Descendant`` strictly advances the
#: snapshot, ``FirstProgenitor`` strictly retreats it, and a
#: ``FirstHaloInFOFgroup`` target is a self-referencing central.
CHAIN_LINKS: Tuple[str, ...] = ("NextProgenitor", "NextHaloInFOFgroup")
_CHAIN_OF_TAG = {LINK_TAG[name]: index for index, name in enumerate(CHAIN_LINKS)}

#: A pointer-jumping record. ``tag == 0`` is a node ``key`` with current
#: successor ``other``; ``tag == 1`` asks for the successor of ``key`` on
#: behalf of node ``other``. Sorting on all four fields puts each node's
#: successor in front of every question about it.
CHAIN_DTYPE = np.dtype([("chain", "<i8"), ("key", "<i8"), ("tag", "<i8"), ("other", "<i8")])
CHAIN_KEY = ("chain", "key", "tag", "other")

#: Working bytes :func:`build_join_records` holds per input row: up to six join
#: records (one map entry, five requests) at 48 B, the position ramp, the five
#: non-null masks, and the largest boolean-indexed temporary alive at once.
JOIN_BUILD_BYTES_PER_ROW = 6 * JOIN_DTYPE.itemsize + 8 + 5 + 3 * 8

#: Working bytes :meth:`SourceKeyJoin.consume` holds per join record: the
#: tag/group/mask arrays over the block, the per-request copies and derived
#: positions, and its two outputs (a 24 B resolved record and, for a chain
#: link, two 32 B pointer-jumping records).
JOIN_SCRATCH_BYTES_PER_RECORD = 3 * 8 + 4 + 12 * 8 + 4 + RESOLVED_DTYPE.itemsize + 2 * 32

#: Working bytes one pointer-jumping round's consumer holds per record: the
#: running last-node index, the question subset and its gathered successors,
#: the match mask, and the next round's two records per surviving node.
CHAIN_ROUND_SCRATCH_BYTES_PER_RECORD = 8 * 8 + 2 + 2 * CHAIN_DTYPE.itemsize


# ==========================================================================
# Snapshot layout
# ==========================================================================


def validate_snapshots(snapshots: Sequence[int]) -> Tuple[int, ...]:
    """Check the ordered a_list snapshot numbers a transpose emits.

    Every entry is an integer (``bool`` excluded: it is a subclass of
    ``int``), non-negative, representable in the int32 target-snapshot columns
    (C3), and strictly ascending. An empty list is refused: there is nothing
    to emit a snapshot into.
    """
    try:
        values = list(snapshots)
    except TypeError:
        raise ConverterError(
            "snapshots must be a sequence of snapshot numbers, got {!r}".format(
                type(snapshots).__name__
            )
        ) from None
    if not values:
        raise ConverterError("snapshots is empty: a transpose needs at least one snapshot")
    checked = []
    for value in values:
        if isinstance(value, bool) or not isinstance(value, (int, np.integer)):
            raise ConverterError(
                "snapshot number {!r} is not an integer; it would be rounded into a different "
                "snapshot".format(value)
            )
        value = int(value)
        if not 0 <= value <= INT32_MAX:
            raise ConverterError(
                "snapshot number {} is outside [0, {}], the range of the int32 target-snapshot "
                "columns; it would be narrowed".format(value, INT32_MAX)
            )
        if checked and value <= checked[-1]:
            raise ConverterError(
                "snapshot numbers must be strictly ascending: {} follows {}".format(
                    value, checked[-1]
                )
            )
        checked.append(value)
    return tuple(checked)


class SnapshotLayout:
    """Where each snapshot's rows sit in the snapshot-major global order.

    ``offsets[p]`` is the global position of row 0 of the ``p``-th snapshot,
    and ``offsets[-1]`` the total. The sums are formed in exact Python ints and
    refused if they leave int64, so no offset is ever a wrapped value.
    """

    def __init__(self, snapshots: Sequence[int], counts: Sequence[int]):
        self.snapshots_tuple = validate_snapshots(snapshots)
        counts = [int(count) for count in counts]
        if len(counts) != len(self.snapshots_tuple):
            raise ConverterError(
                "{} snapshot counts for {} snapshots".format(len(counts), len(self.snapshots_tuple))
            )
        offsets = [0]
        for snapshot, count in zip(self.snapshots_tuple, counts):
            if count < 0:
                raise ConverterError("snapshot {} has a negative count {}".format(snapshot, count))
            if offsets[-1] > INT64_MAX - count:
                raise ConverterError(
                    "the global row count overflows int64 at snapshot {} ({} rows already, {} "
                    "more)".format(snapshot, offsets[-1], count)
                )
            offsets.append(offsets[-1] + count)
        self.total: int = offsets[-1]
        self.snapshots = np.asarray(self.snapshots_tuple, dtype=np.int64)
        self.counts = np.asarray(counts, dtype=np.int64)
        self.offsets = np.asarray(offsets, dtype=np.int64)

    def positions(self, gp: np.ndarray) -> np.ndarray:
        """Snapshot *position* (index into ``snapshots``) of each global position.

        The largest ``p`` with ``offsets[p] <= gp``: an empty snapshot shares
        its offset with the next one, and ``side="right"`` steps past it.
        """
        return np.searchsorted(self.offsets, gp, side="right") - 1

    def locate(self, gp: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """``(snapshot number, snapshot-local row)`` of each global position, int64."""
        position = self.positions(gp)
        return self.snapshots[position], gp - self.offsets[position]

    def describe(self, gp: int) -> str:
        gp = int(gp)
        if not 0 <= gp < self.total:
            return "global position {} (outside the {} converted rows)".format(gp, self.total)
        position = int(np.searchsorted(self.offsets, gp, side="right")) - 1
        return "snapshot {} row {}".format(
            int(self.snapshots[position]), gp - int(self.offsets[position])
        )


# ==========================================================================
# Join records
# ==========================================================================


def build_join_records(
    source_halo_ids: np.ndarray,
    forest_index: np.ndarray,
    link_keys: Mapping[str, np.ndarray],
    first_gp: int,
) -> np.ndarray:
    """The map entries and link requests for a run of consecutive rows.

    Row ``i`` has global position ``first_gp + i``. For a request, ``a`` is
    what the target must carry for the link to close:

    - ``FirstProgenitor``: the owner's own SourceHaloID (the target's
      Descendant must come back to it);
    - ``NextProgenitor``: the owner's Descendant key (siblings share it);
    - ``NextHaloInFOFgroup``: the owner's FirstHaloInFOFgroup key (members
      share a central);
    - ``Descendant`` and ``FirstHaloInFOFgroup``: nothing (``-1``); their
      checks need only the target itself.

    One owner-side rule is enforced here, because only the owner can see it:
    a ``NextProgenitor`` on a halo with no ``Descendant`` has no descendant to
    be a sibling under.
    """
    n_rows = int(source_halo_ids.size)
    first_gp = int(first_gp)
    if first_gp < 0 or first_gp > INT64_MAX - n_rows:
        raise ConverterError(
            "global positions {} + {} rows overflow int64".format(first_gp, n_rows)
        )
    descendant = link_keys["Descendant"]
    next_progenitor = link_keys["NextProgenitor"]
    central = link_keys["FirstHaloInFOFgroup"]
    orphan = np.flatnonzero((next_progenitor != NULL_LINK) & (descendant == NULL_LINK))
    if orphan.size:
        row = int(orphan[0])
        raise ConverterError(
            "SourceHaloID {} has NextProgenitor {} but no Descendant: a sibling progenitor "
            "must share a descendant".format(int(source_halo_ids[row]), int(next_progenitor[row]))
        )

    expectations = {
        "Descendant": None,
        "FirstProgenitor": source_halo_ids,
        "NextProgenitor": descendant,
        "FirstHaloInFOFgroup": None,
        "NextHaloInFOFgroup": central,
    }
    masks = {name: link_keys[name] != NULL_LINK for name in LINK_FIELDS}
    n_requests = sum(int(np.count_nonzero(mask)) for mask in masks.values())
    records = np.empty(n_rows + n_requests, dtype=JOIN_DTYPE)
    positions = np.arange(first_gp, first_gp + n_rows, dtype=np.int64)

    entries = records[:n_rows]
    entries["key"] = source_halo_ids
    entries["tag"] = TAG_MAP_ENTRY
    entries["gp"] = positions
    entries["forest"] = forest_index
    entries["a"] = descendant
    entries["b"] = central

    at = n_rows
    for name in LINK_FIELDS:
        mask = masks[name]
        count = int(np.count_nonzero(mask))
        if not count:
            continue
        segment = records[at : at + count]
        segment["key"] = link_keys[name][mask]
        segment["tag"] = LINK_TAG[name]
        segment["gp"] = positions[mask]
        segment["forest"] = forest_index[mask]
        expected = expectations[name]
        segment["a"] = NULL_LINK if expected is None else expected[mask]
        segment["b"] = source_halo_ids[mask]
        at += count
    return records


# ==========================================================================
# The join
# ==========================================================================


@dataclass
class JoinStats:
    """What one join pass saw. ``descendant_span`` is measured in a_list
    *positions*, so an adjacent descendant has span 1 and a gap span >= 2."""

    n_halos: int = 0
    n_links: Dict[str, int] = field(default_factory=lambda: {name: 0 for name in LINK_FIELDS})
    n_gapped_descendants: int = 0
    max_descendant_span: int = 0
    n_chain_edges: int = 0


@dataclass(frozen=True)
class _Requests:
    """One block's link requests, column by column; ``group`` indexes the
    block's :class:`_Targets` table."""

    group: np.ndarray
    key: np.ndarray
    tag: np.ndarray
    gp: np.ndarray
    forest: np.ndarray
    expect: np.ndarray
    owner: np.ndarray


@dataclass(frozen=True)
class _Targets:
    """One block's group table: row 0 is the carried-in group (or a sentinel),
    then every map entry that opens in the block."""

    key: np.ndarray
    gp: np.ndarray
    forest: np.ndarray
    desc: np.ndarray
    central: np.ndarray


class _Group:
    """The one join group that may straddle a block boundary: a target's map
    entry and the in-degree counts gathered for it so far."""

    __slots__ = ("key", "gp", "forest", "desc", "central", "n_fp", "n_np", "n_nfof")

    def __init__(self, key, gp, forest, desc, central, n_fp=0, n_np=0, n_nfof=0):
        self.key = int(key)
        self.gp = int(gp)
        self.forest = int(forest)
        self.desc = int(desc)
        self.central = int(central)
        self.n_fp = int(n_fp)
        self.n_np = int(n_np)
        self.n_nfof = int(n_nfof)


class SourceKeyJoin:
    """Consumes the ``JOIN_KEY``-sorted join stream block by block.

    Every resolved link goes to ``resolved_sink`` (a :class:`KeyedSorter` of
    :data:`RESOLVED_DTYPE`) and every ``NextProgenitor``/``NextHaloInFOFgroup``
    edge to ``chain_sink`` (of :data:`CHAIN_DTYPE`), as the first
    pointer-jumping round's nodes and questions. Blocks are views the caller
    may reuse after :meth:`consume` returns; nothing here keeps a reference.
    """

    def __init__(self, layout: SnapshotLayout, resolved_sink: KeyedSorter, chain_sink: KeyedSorter):
        self.layout = layout
        self.resolved_sink = resolved_sink
        self.chain_sink = chain_sink
        self.stats = JoinStats()
        self._open: Optional[_Group] = None
        self._finished = False

    # ---- helpers ------------------------------------------------------------

    def _fail(self, message: str) -> None:
        raise ConverterError("source-key join: " + message)

    def _request_context(self, owner_id: int, owner_gp: int, tag: int, target: int) -> str:
        return "{} of SourceHaloID {} ({}) -> SourceHaloID {}".format(
            _TAG_NAME[tag], owner_id, self.layout.describe(owner_gp), target
        )

    # ---- one block -----------------------------------------------------------

    def consume(self, block: np.ndarray) -> None:
        if self._finished:
            raise ConverterError("source-key join: block consumed after finish()")
        if block.dtype != JOIN_DTYPE:
            raise ConverterError("source-key join: expected {} records".format(JOIN_DTYPE))
        if not block.size:
            return
        key = block["key"]
        tag = block["tag"]
        is_entry = tag == TAG_MAP_ENTRY
        entry_rows = np.flatnonzero(is_entry)
        n_entries = int(entry_rows.size)

        # Group table: row 0 is the group carried in from the previous block
        # (or a sentinel whose key 0 no halo has), rows 1.. the map entries
        # that open in this block, in stream order.
        g_key = np.empty(n_entries + 1, dtype=np.int64)
        g_gp = np.empty(n_entries + 1, dtype=np.int64)
        g_forest = np.empty(n_entries + 1, dtype=np.int64)
        g_desc = np.empty(n_entries + 1, dtype=np.int64)
        g_central = np.empty(n_entries + 1, dtype=np.int64)
        carried = self._open
        if carried is None:
            g_key[0], g_gp[0], g_forest[0], g_desc[0], g_central[0] = 0, -1, -1, NULL_LINK, 0
        else:
            g_key[0], g_gp[0], g_forest[0] = carried.key, carried.gp, carried.forest
            g_desc[0], g_central[0] = carried.desc, carried.central
        g_key[1:] = key[entry_rows]
        g_gp[1:] = block["gp"][entry_rows]
        g_forest[1:] = block["forest"][entry_rows]
        g_desc[1:] = block["a"][entry_rows]
        g_central[1:] = block["b"][entry_rows]

        duplicated = np.flatnonzero(g_key[1:] == g_key[:-1])
        if duplicated.size:
            second = int(duplicated[0]) + 1
            self._fail(
                "SourceHaloID {} is carried by two halos ({} and {}); source keys must be "
                "unique".format(
                    int(g_key[second]),
                    self.layout.describe(g_gp[second - 1]),
                    self.layout.describe(g_gp[second]),
                )
            )

        group = np.cumsum(is_entry)
        requests = np.flatnonzero(~is_entry)
        r_group = group[requests]
        r_key = key[requests]
        r_tag = tag[requests]
        r_gp = block["gp"][requests]
        r_expect = block["a"][requests]
        r_owner = block["b"][requests]

        self._check_requests(
            _Requests(r_group, r_key, r_tag, r_gp, block["forest"][requests], r_expect, r_owner),
            _Targets(g_key, g_gp, g_forest, g_desc, g_central),
        )

        # In-degree through the three links that must reach a halo at most
        # once; the carried group's earlier counts are folded into row 0.
        n_fp = np.bincount(r_group[r_tag == _TAG_FIRST_PROGENITOR], minlength=n_entries + 1)
        n_np = np.bincount(r_group[r_tag == _TAG_NEXT_PROGENITOR], minlength=n_entries + 1)
        n_nfof = np.bincount(r_group[r_tag == _TAG_NEXT_FOF], minlength=n_entries + 1)
        if carried is not None:
            n_fp[0] += carried.n_fp
            n_np[0] += carried.n_np
            n_nfof[0] += carried.n_nfof

        # Every group but the last is complete: a later map entry has opened.
        # The sentinel row 0 is skipped when nothing was carried in.
        start = 0 if carried is not None else 1
        if n_entries:
            closing = slice(start, n_entries)
            self._check_coverage(
                g_key[closing],
                g_gp[closing],
                g_desc[closing],
                g_central[closing],
                n_fp[closing],
                n_np[closing],
                n_nfof[closing],
            )
        last = n_entries
        if n_entries or carried is not None:
            self._open = _Group(
                g_key[last],
                g_gp[last],
                g_forest[last],
                g_desc[last],
                g_central[last],
                n_fp[last],
                n_np[last],
                n_nfof[last],
            )

        self._emit(r_group, r_tag, r_gp, g_gp)
        self.stats.n_halos += n_entries

    def _check_requests(self, requests: "_Requests", targets: "_Targets") -> None:
        if not requests.group.size:
            return

        def first(mask: np.ndarray) -> Optional[int]:
            hits = np.flatnonzero(mask)
            return int(hits[0]) if hits.size else None

        def context(index: int) -> str:
            return self._request_context(
                int(requests.owner[index]),
                int(requests.gp[index]),
                int(requests.tag[index]),
                int(requests.key[index]),
            )

        group = requests.group
        missing = first(requests.key != targets.key[group])
        if missing is not None:
            self._fail("{}: the target is not a halo of this conversion".format(context(missing)))
        target_forest = targets.forest[group]
        crossing = first(requests.forest != target_forest)
        if crossing is not None:
            self._fail(
                "{}: the target is in ForestIndex {}, the owner in {}; links never cross "
                "forests".format(
                    context(crossing), int(target_forest[crossing]), int(requests.forest[crossing])
                )
            )

        target_gp = targets.gp[group]
        owner_position = self.layout.positions(requests.gp)
        target_position = self.layout.positions(target_gp)
        target_desc = targets.desc[group]
        target_central = targets.central[group]
        tag = requests.tag

        is_desc = tag == _TAG_DESCENDANT
        is_fp = tag == _TAG_FIRST_PROGENITOR
        is_np = tag == _TAG_NEXT_PROGENITOR
        is_ffof = tag == _TAG_FIRST_FOF
        is_nfof = tag == _TAG_NEXT_FOF

        def where(index: int) -> str:
            return self.layout.describe(target_gp[index])

        def descendant_of(index: int) -> str:
            return "SourceHaloID {}".format(int(target_desc[index]))

        def central_of(index: int) -> str:
            return "SourceHaloID {}".format(int(target_central[index]))

        checks = (
            (
                is_desc & (target_position <= owner_position),
                "a Descendant must be in a strictly later snapshot (target at {})",
                where,
            ),
            (
                is_fp & (target_position >= owner_position),
                "a FirstProgenitor must be in a strictly earlier snapshot (target at {})",
                where,
            ),
            (
                is_fp & (target_desc != requests.owner),
                "the FirstProgenitor does not round-trip: its Descendant is {}",
                descendant_of,
            ),
            (
                is_np & (target_desc != requests.expect),
                "a NextProgenitor must name the owner's descendant; the target's Descendant is {}",
                descendant_of,
            ),
            (
                (is_ffof | is_nfof) & (target_position != owner_position),
                "FoF links stay in the owner's snapshot (target at {})",
                where,
            ),
            (
                is_ffof & (target_central != requests.key),
                "a FirstHaloInFOFgroup target must be a central, but its own "
                "FirstHaloInFOFgroup is {}",
                central_of,
            ),
            (
                is_nfof & (target_central != requests.expect),
                "a NextHaloInFOFgroup target must share the owner's central; its "
                "FirstHaloInFOFgroup is {}",
                central_of,
            ),
        )
        for mask, template, detail in checks:
            bad = first(mask)
            if bad is not None:
                self._fail("{}: {}".format(context(bad), template.format(detail(bad))))

        if bool(np.any(is_desc)):
            span = target_position[is_desc] - owner_position[is_desc]
            self.stats.n_gapped_descendants += int(np.count_nonzero(span > 1))
            self.stats.max_descendant_span = max(self.stats.max_descendant_span, int(span.max()))

    def _check_coverage(
        self,
        keys: np.ndarray,
        gps: np.ndarray,
        desc: np.ndarray,
        central: np.ndarray,
        n_fp: np.ndarray,
        n_np: np.ndarray,
        n_nfof: np.ndarray,
    ) -> None:
        if not keys.size:
            return
        reached = n_fp + n_np
        has_descendant = desc != NULL_LINK
        is_central = central == keys
        rules = (
            (
                n_fp > 1,
                "is the FirstProgenitor of {} halos; a halo is at most one descendant's main "
                "progenitor",
                n_fp,
            ),
            (n_np > 1, "is the NextProgenitor of {} halos; a sibling chain cannot fork", n_np),
            (
                n_nfof > 1,
                "is the NextHaloInFOFgroup of {} halos; a FoF chain cannot fork",
                n_nfof,
            ),
            (
                has_descendant & (reached != 1),
                "has a Descendant but is reached by {} progenitor-chain link(s); it must be its "
                "descendant's FirstProgenitor or exactly one sibling's NextProgenitor",
                reached,
            ),
            (
                ~has_descendant & (reached != 0),
                "has no Descendant but is reached by {} progenitor-chain link(s)",
                reached,
            ),
            (
                is_central & (n_nfof != 0),
                "is a FoF central but is reached by {} NextHaloInFOFgroup link(s); a chain "
                "starts at its central",
                n_nfof,
            ),
            (
                ~is_central & (n_nfof != 1),
                "is a FoF member but is reached by {} NextHaloInFOFgroup link(s); it must be in "
                "its central's chain exactly once",
                n_nfof,
            ),
        )
        for mask, template, detail in rules:
            hits = np.flatnonzero(mask)
            if hits.size:
                index = int(hits[0])
                self._fail(
                    "SourceHaloID {} ({}) {}".format(
                        int(keys[index]),
                        self.layout.describe(gps[index]),
                        template.format(int(detail[index])),
                    )
                )

    def _emit(
        self, r_group: np.ndarray, r_tag: np.ndarray, r_gp: np.ndarray, g_gp: np.ndarray
    ) -> None:
        n_requests = int(r_group.size)
        if not n_requests:
            return
        target_gp = g_gp[r_group]
        resolved = np.empty(n_requests, dtype=RESOLVED_DTYPE)
        resolved["owner_gp"] = r_gp
        resolved["kind"] = r_tag - 1
        resolved["target_gp"] = target_gp
        self.resolved_sink.add(resolved)
        del resolved
        for name in LINK_FIELDS:
            self.stats.n_links[name] += int(np.count_nonzero(r_tag == LINK_TAG[name]))

        for tag, chain in _CHAIN_OF_TAG.items():
            edge = r_tag == tag
            count = int(np.count_nonzero(edge))
            if not count:
                continue
            records = np.empty(2 * count, dtype=CHAIN_DTYPE)
            nodes, questions = records[:count], records[count:]
            nodes["chain"] = chain
            nodes["key"] = r_gp[edge]
            nodes["tag"] = 0
            nodes["other"] = target_gp[edge]
            questions["chain"] = chain
            questions["key"] = target_gp[edge]
            questions["tag"] = 1
            questions["other"] = r_gp[edge]
            self.chain_sink.add(records)
            self.stats.n_chain_edges += count

    # ---- end of stream -------------------------------------------------------

    def finish(self) -> JoinStats:
        """Close the last open group and return the pass's statistics."""
        if self._finished:
            return self.stats
        self._finished = True
        carried = self._open
        if carried is not None:
            self._check_coverage(
                np.asarray([carried.key], dtype=np.int64),
                np.asarray([carried.gp], dtype=np.int64),
                np.asarray([carried.desc], dtype=np.int64),
                np.asarray([carried.central], dtype=np.int64),
                np.asarray([carried.n_fp], dtype=np.int64),
                np.asarray([carried.n_np], dtype=np.int64),
                np.asarray([carried.n_nfof], dtype=np.int64),
            )
        self._open = None
        return self.stats


# ==========================================================================
# Chain acyclicity by external pointer jumping
# ==========================================================================


def verify_chains_acyclic(
    first_round: KeyedSorter,
    n_edges: int,
    layout: SnapshotLayout,
    *,
    merge_budget_bytes: int,
    new_round: Callable[[], KeyedSorter],
) -> int:
    """Prove every ``NextProgenitor`` and ``NextHaloInFOFgroup`` chain ends.

    ``first_round`` holds, for each of the ``n_edges`` chain links ``x -> s``,
    a node record ``(chain, x, 0, s)`` and a question ``(chain, s, 1, x)``.
    Each round sorts them, answers every question from the node in front of it
    -- ``x``'s successor becomes ``s``'s successor, i.e. twice as far ahead --
    and keeps only the nodes whose new successor still has one. A node ``d``
    links from the end of an acyclic chain leaves after ``ceil(log2 d)``
    rounds, so every acyclic node has left after ``n_edges.bit_length()``
    rounds; a node on a cycle never leaves. Rounds cost one bounded external
    sort each, over a shrinking set, and nothing is ever held per chain or per
    forest.

    Returns the number of rounds run. Closes ``first_round`` and every sorter
    ``new_round`` makes, on every path.
    """
    sorter = first_round
    active = int(n_edges)
    limit = active.bit_length()
    rounds = 0
    try:
        while active:
            # sealed first, so the finished round holds nothing while the next
            # round's generation buffers are allocated
            sorter.seal()
            upcoming = new_round()
            try:
                active, example = _jump_once(sorter, upcoming, merge_budget_bytes)
            except BaseException:
                upcoming.close()
                raise
            sorter.close()
            sorter = upcoming
            rounds += 1
            if active and rounds >= limit:
                chain, gp = example
                raise ConverterError(
                    "chain check: the {} chain through {} never terminates after {} "
                    "pointer-jumping rounds over {} link(s); the chain is cyclic".format(
                        CHAIN_LINKS[chain], layout.describe(gp), rounds, n_edges
                    )
                )
        return rounds
    finally:
        sorter.close()


def _jump_once(
    sorter: KeyedSorter, upcoming: KeyedSorter, merge_budget_bytes: int
) -> Tuple[int, Tuple[int, int]]:
    """One pointer-jumping round; returns the surviving node count and one
    surviving ``(chain, node)`` for diagnostics."""
    survivors = 0
    example = (-1, -1)
    carry_chain, carry_key, carry_other = -1, -1, -1
    blocks = sorter.sorted_blocks(
        budget_bytes=merge_budget_bytes,
        consumer_bytes_per_record=CHAIN_ROUND_SCRATCH_BYTES_PER_RECORD,
    )
    try:
        for block in blocks:
            chain = block["chain"]
            key = block["key"]
            other = block["other"]
            is_node = block["tag"] == 0
            last_node = np.where(is_node, np.arange(block.size), -1)
            np.maximum.accumulate(last_node, out=last_node)
            questions = np.flatnonzero(~is_node)
            if questions.size:
                source = last_node[questions]
                inside = source >= 0
                safe = np.maximum(source, 0)
                node_chain = np.where(inside, chain[safe], carry_chain)
                node_key = np.where(inside, key[safe], carry_key)
                node_other = np.where(inside, other[safe], carry_other)
                matched = (node_chain == chain[questions]) & (node_key == key[questions])
                count = int(np.count_nonzero(matched))
                if count:
                    asker = other[questions][matched]
                    asker_chain = chain[questions][matched]
                    successor = node_other[matched]
                    records = np.empty(2 * count, dtype=CHAIN_DTYPE)
                    nodes, asks = records[:count], records[count:]
                    nodes["chain"] = asker_chain
                    nodes["key"] = asker
                    nodes["tag"] = 0
                    nodes["other"] = successor
                    asks["chain"] = asker_chain
                    asks["key"] = successor
                    asks["tag"] = 1
                    asks["other"] = asker
                    if not survivors:
                        example = (int(asker_chain[0]), int(asker[0]))
                    upcoming.add(records)
                    survivors += count
            node_rows = np.flatnonzero(is_node)
            if node_rows.size:
                tail = int(node_rows[-1])
                carry_chain, carry_key, carry_other = (
                    int(chain[tail]),
                    int(key[tail]),
                    int(other[tail]),
                )
    finally:
        blocks.close()
    return survivors, example
