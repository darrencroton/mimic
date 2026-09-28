"""Whole-unit structural validation and batch assembly shared by the prelinked
adapters (``lhalo_binary`` and ``consistent_trees_hdf5``).

Both formats store their five merger links as unit-local row indices, so both
owe the same structural guarantee before a row may enter a batch (C1/C3): every
link is ``-1`` or a row of its own unit, ``Descendant`` points strictly forward
in time, the progenitor chains are exactly the reciprocal of ``Descendant``,
and every FoF group is same-snapshot, centrally rooted and acyclic.
:func:`validate_tree` checks all of it and **rejects, never repairs**: a
structurally invalid unit raises a ``ConverterError`` naming the unit and the
first offending row, and nothing is edited to make it conform. A forward gap
(a ``Descendant`` spanning more than one snapshot) is legal and survives as an
exact source-key edge.

These rules are not expressible chunk-locally, so validation is the one
whole-unit term of each prelinked adapter's memory model (C4). The contract
that keeps it bounded is **budget before allocation**: an adapter calls
:func:`check_validation_budget` for a unit's size *before* allocating the
unit's :data:`TOPOLOGY_COLUMNS`, runs :func:`validate_tree` while they are
live, and releases them before emission begins. :class:`BatchBuilder` then
packs the emitted chunks into canonical batches of at most ``max_rows`` rows,
and :func:`check_emission_budget` bounds that emission buffer before the
first batch is built.

The ASCII adapter shares none of this: Consistent-Trees ASCII stores no links,
and its topology is reconstructed by its own preparation stages.
"""

from typing import Dict, List, Optional, Tuple

import numpy as np
from column_schema import EXTRA_TYPES, IDENTITY_FIELDS, CanonicalSchema, ConverterError

from .base import LINK_FIELDS, NULL_LINK, CanonicalBatch, check_budget

__all__ = [
    "TOPOLOGY_COLUMNS",
    "VALIDATION_BYTES_PER_HALO",
    "CANONICAL_KEY_BYTES_PER_ROW",
    "validation_budget_bytes",
    "check_validation_budget",
    "canonical_row_bytes",
    "check_emission_budget",
    "validate_tree",
    "BatchBuilder",
]

#: The columns whole-unit structural validation needs: the five stored links
#: plus the snapshot they are interpreted against. :func:`validate_tree`
#: expects each as a one-dimensional int64 array of the unit's length, held as
#: int64 rather than the source's width so that index arithmetic, ``bincount``
#: and the chain walks cannot overflow or silently re-cast mid-expression.
TOPOLOGY_COLUMNS: Tuple[str, ...] = LINK_FIELDS + ("SnapNum",)

#: Peak bytes per halo of the whole per-unit validation path, which is what the
#: budget check must bound. The six retained :data:`TOPOLOGY_COLUMNS` are only
#: 48 of these; :func:`validate_tree` and its helpers then allocate whole-unit
#: scratch -- the ``rows`` index array, the boolean masks, their fancy-indexed
#: int64 copies (``heads``, ``owners``, ``descendant[heads]`` and their FoF
#: counterparts) and several ``bincount`` results -- while those columns are
#: still live.
#:
#: Measured with ``tracemalloc`` around **the retained columns plus**
#: :func:`validate_tree` -- both, because the columns stay live across the
#: validation and measuring only the scratch would understate the peak by more
#: than half. Taken at n = 20k, 40k, 100k and 400k halos over three
#: deliberately different tree shapes; the per-halo figure was flat in n to
#: within 0.6% in every case, so this is a slope, not a two-point
#: extrapolation:
#:
#:     linear chain (every halo has both a descendant and a
#:                   FirstProgenitor)                        119.0 B/halo
#:     wide (one progenitor each, large FoF groups)            98.0 B/halo
#:     dense sibling chains (64-member progenitor/FoF chains) 109.3 B/halo
#:
#: The linear shape is the worst because it is the one that makes
#: ``has_first`` and ``has_descendant`` dense *simultaneously*, so the
#: FirstProgenitor block's three int64 copies are all full length. The
#: forests-HDF5 path re-measures at the same 119.0 B/halo worst case. 160 is
#: that worst case plus a 1.34x margin, because a budget that refuses work is
#: safe and one that accepts work it cannot hold is the defect this figure
#: exists to prevent. The margin costs nothing real: the largest tree in any
#: shipped L-Halo package (397,280 halos, mini-Uchuu) needs 63.6 MB of the
#: 2 GiB default.
#:
#: ``BudgetAccountingTests`` in ``tests/test_lhalo_adapter.py`` and
#: ``tests/test_ctrees_hdf5_adapter.py`` re-measure the worst shape through
#: each adapter's real read path and fail if it exceeds this constant, so the
#: figure is self-policing rather than a number that silently rots as numpy's
#: temporaries change.
VALIDATION_BYTES_PER_HALO = 160


def validation_budget_bytes(n_halos: int, read_buffer_bytes: int, base_bytes: int) -> int:
    """The bytes :func:`check_validation_budget` requires for one unit.

    ``n_halos * VALIDATION_BYTES_PER_HALO`` plus the adapter's bounded
    ``read_buffer_bytes`` plus its fixed ``base_bytes`` addend. Exposed so an
    adapter can ask whether an optional, larger read buffer would still fit
    before choosing it, using exactly the arithmetic the refusal applies.
    """
    return n_halos * VALIDATION_BYTES_PER_HALO + read_buffer_bytes + base_bytes


def check_validation_budget(
    n_halos: int,
    read_buffer_bytes: int,
    base_bytes: int,
    memory_budget_bytes: int,
    context: str,
) -> None:
    """Refuse one unit's structural validation before its columns are allocated.

    The required figure is :func:`validation_budget_bytes`, compared against
    the whole ``memory_budget_bytes`` ceiling. The caller must invoke this
    *before* allocating the unit's :data:`TOPOLOGY_COLUMNS`, so an over-budget
    unit is refused rather than allocated.

    ``base_bytes`` is each adapter's own measured constant, because the two
    read paths differ in fixed overhead, and both routes declare one. The
    forests-HDF5 route passes ``ctrees_hdf5.TOPOLOGY_BASE_BYTES``: its reads go
    through h5py dataset handles and hyperslab selections, with a measured
    fixed peak of 6.4-7.4 KB for forests of 1-10 halos. The L-Halo route passes
    ``lhalo_binary.TOPOLOGY_BASE_BYTES``: it reads raw fixed-size records
    through one open file handle, with a smaller measured fixed peak of about
    2.7 KB at one halo. Without the addend either route would declare a small
    unit's figure below its real peak. A zero addend is omitted from the
    refusal message.

    Args:
        n_halos: Rows in the unit about to be validated.
        read_buffer_bytes: The adapter's transient read buffer for this unit.
        base_bytes: The adapter's fixed addend.
        memory_budget_bytes: The configured ceiling.
        context: Names the unit in the refusal.

    Raises:
        ConverterError: The unit's validation would exceed the ceiling.
    """
    if base_bytes:
        detail = "{} B/halo plus a {}-byte read buffer and a {}-byte base".format(
            VALIDATION_BYTES_PER_HALO, read_buffer_bytes, base_bytes
        )
    else:
        detail = "{} B/halo plus a {}-byte read buffer".format(
            VALIDATION_BYTES_PER_HALO, read_buffer_bytes
        )
    check_budget(
        validation_budget_bytes(n_halos, read_buffer_bytes, base_bytes),
        memory_budget_bytes,
        "{}: structural validation of {} halos ({})".format(context, n_halos, detail),
        "raise memory_budget_bytes",
    )


#: Bytes one canonical batch row holds whatever the schema: the three int64
#: identity columns, the three int64 coordinate columns
#: (``base.identity_columns``) and the five int64 links.
CANONICAL_KEY_BYTES_PER_ROW = 8 * (len(IDENTITY_FIELDS) + 3 + len(LINK_FIELDS))


def canonical_row_bytes(schema: CanonicalSchema) -> int:
    """Bytes one canonical batch row holds under ``schema``.

    :data:`CANONICAL_KEY_BYTES_PER_ROW` plus one element of every payload
    field and every declared extra, at their declared widths. The format's
    payload is 64 bytes on both prelinked routes, so a schema without extras
    comes to 152 B/row.
    """
    return (
        CANONICAL_KEY_BYTES_PER_ROW
        + sum(EXTRA_TYPES[field.type].itemsize for field in schema.payload_fields)
        + sum(EXTRA_TYPES[extra.type].itemsize for extra in schema.extra_fields)
    )


def check_emission_budget(
    schema: CanonicalSchema,
    max_rows: int,
    n_halos: int,
    raw_bytes_per_row: int,
    memory_budget_bytes: int,
) -> None:
    """Refuse a batch size whose emission buffer cannot fit, before any batch is built.

    Per batch a prelinked adapter holds the raw source values of the rows it
    has just read (``raw_bytes_per_row``: the record width on L-Halo, the
    summed width of every dataset read per row on forests-HDF5) and the
    canonical columns built from them (:func:`canonical_row_bytes`), and
    :meth:`BatchBuilder.take` concatenates the accumulated chunks while they
    are still referenced. The term is therefore doubled::

        2 * min(max_rows, n_halos) * (raw_bytes_per_row + canonical_row_bytes)

    ``n_halos`` is the inventory's total: no batch holds more rows than the
    source has, just as the ASCII route charges at most one snapshot's rows.
    The doubling also covers the per-chunk conversion temporaries (the
    forests-HDF5 float64 Spin normalisation is the largest).

    Raises:
        ConverterError: The emission buffer would exceed the ceiling.
    """
    rows = min(max_rows, n_halos)
    canonical = canonical_row_bytes(schema)
    check_budget(
        2 * rows * (raw_bytes_per_row + canonical),
        memory_budget_bytes,
        "emission (batches of {} rows, {} B/row raw read plus {} B/row canonical columns, "
        "doubled at concatenation)".format(rows, raw_bytes_per_row, canonical),
        "lower --ingest-max-rows or raise memory_budget_bytes",
    )


# ==========================================================================
# Per-unit structural validation
# ==========================================================================


def _rows_of(mask: np.ndarray) -> np.ndarray:
    """The row indices a boolean mask selects, in ascending order."""
    return np.asarray(mask).nonzero()[0]


def _first_row(mask: np.ndarray) -> int:
    """The first row a boolean mask selects. Callers check ``any()`` first."""
    return int(_rows_of(mask)[0])


def validate_tree(
    columns: Dict[str, np.ndarray], n_halos: int, context: str, max_snapshot: Optional[int]
) -> None:
    """Reject a structurally invalid unit (C1).

    ``columns`` maps every name in :data:`TOPOLOGY_COLUMNS` to an int64 array
    of ``n_halos`` unit-local values. ``SnapNum`` must be non-negative and, when
    ``max_snapshot`` is given, at most ``max_snapshot``; without it only
    non-negativity is enforced. Every error names ``context`` and, where one
    exists, the first offending row.

    Every rule was checked against all four shipped L-Halo datasets --
    mini-Millennium (8/8 files), micro-Uchuu (4/4), and four files each of
    Millennium and mini-Uchuu, 75.4 M halos in total -- and against all
    440,651 real micro-Uchuu forests-HDF5 forests (22,580,924 halos), with zero
    violations, so none of them fails valid source data.

    A **forward gap is not malformed**: ``Descendant`` must point strictly
    forward, and a span of 2 is as legal as a span of 1. mini-Millennium's
    29,291 gaps are the acceptance case.

    Raises:
        ConverterError: The unit violates a structural rule.
    """
    descendant = columns["Descendant"]
    first_progenitor = columns["FirstProgenitor"]
    next_progenitor = columns["NextProgenitor"]
    fof_central = columns["FirstHaloInFOFgroup"]
    next_in_fof = columns["NextHaloInFOFgroup"]
    snapshot = columns["SnapNum"]
    rows = np.arange(n_halos, dtype=np.int64)

    _validate_ranges(columns, n_halos, context)
    _validate_snapshots(snapshot, context, max_snapshot)
    _validate_descendants(descendant, snapshot, context)
    _validate_progenitors(descendant, first_progenitor, next_progenitor, rows, n_halos, context)
    _validate_fof(fof_central, next_in_fof, snapshot, rows, n_halos, context)


def _validate_ranges(columns: Dict[str, np.ndarray], n_halos: int, context: str) -> None:
    """Every link is ``-1`` or a row of this unit.

    ``FirstHaloInFOFgroup`` is the exception with no null: a central
    self-references (C3), so the whole column must be a real row.
    """
    for name in LINK_FIELDS:
        values = columns[name]
        floor = 0 if name == "FirstHaloInFOFgroup" else NULL_LINK
        below = values < floor
        if bool(below.any()):
            row = _first_row(below)
            if name == "FirstHaloInFOFgroup":
                raise ConverterError(
                    "{}, row {}: FirstHaloInFOFgroup is {}, but it is never null -- a central "
                    "self-references".format(context, row, int(values[row]))
                )
            raise ConverterError(
                "{}, row {}: {} is {}; only -1 is the null sentinel".format(
                    context, row, name, int(values[row])
                )
            )
        above = values >= n_halos
        if bool(above.any()):
            row = _first_row(above)
            raise ConverterError(
                "{}, row {}: {} points to row {}, outside this {}-halo tree".format(
                    context, row, name, int(values[row]), n_halos
                )
            )


def _validate_snapshots(snapshot: np.ndarray, context: str, max_snapshot: Optional[int]) -> None:
    below = snapshot < 0
    if bool(below.any()):
        row = _first_row(below)
        raise ConverterError(
            "{}, row {}: SnapNum is {}, which is negative".format(context, row, int(snapshot[row]))
        )
    if max_snapshot is None:
        return
    above = snapshot > max_snapshot
    if bool(above.any()):
        row = _first_row(above)
        raise ConverterError(
            "{}, row {}: SnapNum is {}, outside the a_list's range [0, {}]".format(
                context, row, int(snapshot[row]), max_snapshot
            )
        )


def _validate_descendants(descendant: np.ndarray, snapshot: np.ndarray, context: str) -> None:
    """``Descendant`` points strictly forward in time; gaps are legal."""
    linked = descendant >= 0
    if not bool(linked.any()):
        return
    span = snapshot[descendant[linked]] - snapshot[linked]
    bad = span < 1
    if bool(bad.any()):
        row = int(_rows_of(linked)[_first_row(bad)])
        target = int(descendant[row])
        raise ConverterError(
            "{}, row {}: Descendant points to row {} at snapshot {}, not forward of snapshot {}; "
            "a forward gap is legal, a non-forward link is not".format(
                context, row, target, int(snapshot[target]), int(snapshot[row])
            )
        )


def _validate_progenitors(
    descendant: np.ndarray,
    first_progenitor: np.ndarray,
    next_progenitor: np.ndarray,
    rows: np.ndarray,
    n_halos: int,
    context: str,
) -> None:
    """The progenitor chains must be exactly the reciprocal of ``Descendant``.

    Four independent properties, because no three of them imply the fourth:
    each ``FirstProgenitor`` names a halo that names it back; each
    ``NextProgenitor`` sibling shares its owner's descendant; every halo with a
    descendant is named exactly once across all chains; and the chains actually
    reach all of them. The last is not redundant -- an in-degree of exactly one
    is equally satisfied by a chain plus a disjoint cycle, which is precisely
    the corruption a reachability count catches.

    Two rules C3 states are enforced *transitively* here rather than tested
    again, because each is a consequence of the rules above and a separate
    branch for it would be unreachable:

    - **"FirstProgenitor points backwards."** ``_validate_descendants`` has
      already proved ``SnapNum[Descendant[x]] > SnapNum[x]`` for every linked
      ``x``, and reciprocity proves ``Descendant[FirstProgenitor[h]] == h``.
      Substituting gives ``SnapNum[h] > SnapNum[FirstProgenitor[h]]``.
    - **A chain head implies a progenitor.** Reciprocity makes
      ``FirstProgenitor[h]`` a halo whose ``Descendant`` is ``h``, so ``h``
      cannot simultaneously have a head and no progenitors.

    That is why no snapshot column is taken here.
    """
    has_first = first_progenitor >= 0
    if bool(has_first.any()):
        heads = first_progenitor[has_first]
        owners = rows[has_first]
        broken = descendant[heads] != owners
        if bool(broken.any()):
            position = _first_row(broken)
            owner, head = int(owners[position]), int(heads[position])
            raise ConverterError(
                "{}, row {}: FirstProgenitor is row {}, but that halo's Descendant is {}, not {} "
                "-- the progenitor round trip is inconsistent".format(
                    context, owner, head, int(descendant[head]), owner
                )
            )

    has_next = next_progenitor >= 0
    if bool(has_next.any()):
        siblings = next_progenitor[has_next]
        owners = rows[has_next]
        orphaned = descendant[owners] < 0
        if bool(orphaned.any()):
            owner = int(owners[_first_row(orphaned)])
            raise ConverterError(
                "{}, row {}: NextProgenitor is set but the halo has no Descendant, so there is no "
                "chain for it to belong to".format(context, owner)
            )
        mismatched = descendant[siblings] != descendant[owners]
        if bool(mismatched.any()):
            position = _first_row(mismatched)
            owner, sibling = int(owners[position]), int(siblings[position])
            raise ConverterError(
                "{}, row {}: NextProgenitor row {} descends to {}, but this halo descends to {}; "
                "siblings must name the same descendant".format(
                    context, owner, sibling, int(descendant[sibling]), int(descendant[owner])
                )
            )

    has_descendant = descendant >= 0
    progenitor_counts = np.bincount(descendant[has_descendant], minlength=n_halos)
    missing_head = (progenitor_counts > 0) & ~has_first
    if bool(missing_head.any()):
        row = _first_row(missing_head)
        raise ConverterError(
            "{}, row {}: {} halo(s) name this one as their Descendant, but FirstProgenitor is "
            "-1".format(context, row, int(progenitor_counts[row]))
        )

    in_degree = np.bincount(first_progenitor[has_first], minlength=n_halos) + np.bincount(
        next_progenitor[has_next], minlength=n_halos
    )
    wrong = in_degree != has_descendant.astype(np.int64)
    if bool(wrong.any()):
        row = _first_row(wrong)
        raise ConverterError(
            "{}, row {}: this halo is named by {} progenitor pointer(s) but should be named by {} "
            "-- the progenitor chains double-count or drop it".format(
                context, row, int(in_degree[row]), int(has_descendant[row])
            )
        )

    reached = _walk_chain(first_progenitor[has_first], next_progenitor, n_halos)
    expected = int(has_descendant.sum())
    if reached != expected:
        raise ConverterError(
            "{}: the NextProgenitor chains reach {} of {} halos that have a Descendant -- the "
            "unreached halos form a cycle".format(context, reached, expected)
        )


def _validate_fof(
    fof_central: np.ndarray,
    next_in_fof: np.ndarray,
    snapshot: np.ndarray,
    rows: np.ndarray,
    n_halos: int,
    context: str,
) -> None:
    """FoF groups are same-snapshot, centrally rooted and acyclic (C1/C3)."""
    wrong_snapshot = snapshot[fof_central] != snapshot
    if bool(wrong_snapshot.any()):
        row = _first_row(wrong_snapshot)
        central = int(fof_central[row])
        raise ConverterError(
            "{}, row {}: FirstHaloInFOFgroup is row {} at snapshot {}, but this halo is at "
            "snapshot {}; FoF links stay in the current snapshot".format(
                context, row, central, int(snapshot[central]), int(snapshot[row])
            )
        )
    not_self = fof_central[fof_central] != fof_central
    if bool(not_self.any()):
        row = _first_row(not_self)
        central = int(fof_central[row])
        raise ConverterError(
            "{}, row {}: FirstHaloInFOFgroup is row {}, but that halo's own FirstHaloInFOFgroup "
            "is {} -- a central must self-reference".format(
                context, row, central, int(fof_central[central])
            )
        )

    has_next = next_in_fof >= 0
    if bool(has_next.any()):
        members = next_in_fof[has_next]
        owners = rows[has_next]
        foreign = fof_central[members] != fof_central[owners]
        if bool(foreign.any()):
            position = _first_row(foreign)
            owner, member = int(owners[position]), int(members[position])
            raise ConverterError(
                "{}, row {}: NextHaloInFOFgroup row {} belongs to group {}, not this halo's "
                "group {}".format(
                    context, owner, member, int(fof_central[member]), int(fof_central[owner])
                )
            )

    is_central = fof_central == rows
    in_degree = np.bincount(next_in_fof[has_next], minlength=n_halos)
    expected = (~is_central).astype(np.int64)
    wrong = in_degree != expected
    if bool(wrong.any()):
        row = _first_row(wrong)
        raise ConverterError(
            "{}, row {}: this halo is named by {} NextHaloInFOFgroup pointer(s) but should be "
            "named by {} -- a central is never a chain target and a satellite is named "
            "once".format(context, row, int(in_degree[row]), int(expected[row]))
        )

    reached = _walk_chain(rows[is_central], next_in_fof, n_halos)
    if reached != n_halos:
        raise ConverterError(
            "{}: the NextHaloInFOFgroup chains reach {} of {} halos from their centrals -- the "
            "unreached halos form a cycle".format(context, reached, n_halos)
        )


def _walk_chain(heads: np.ndarray, successor: np.ndarray, n_halos: int) -> int:
    """Count the halos reachable from ``heads`` along ``successor``.

    A frontier walk rather than a per-node loop: every node has at most one
    successor, so the frontier never grows and the number of iterations is the
    longest chain, not the halo count. Real sources make that cheap -- the
    deepest chain measured across all four L-Halo datasets is 3,817
    (mini-Uchuu FoF) against 46 M halos in that sample.

    Callers run this only after proving in-degree is exactly one, which means
    a cycle is necessarily disjoint from the reachable set and can never be
    entered. The ``visited > n_halos`` guard is insurance against a future
    caller reordering those checks, not a live path.
    """
    frontier = heads[heads >= 0]
    visited = 0
    while frontier.size:
        visited += int(frontier.size)
        if visited > n_halos:  # pragma: no cover - unreachable after the in-degree check
            raise ConverterError("chain walk revisited a halo; the links contain a cycle")
        following = successor[frontier]
        frontier = following[following >= 0]
    return visited


# ==========================================================================
# Batch assembly
# ==========================================================================


class BatchBuilder:
    """Accumulates chunk columns until ``max_rows`` rows are ready.

    Holds at most ``max_rows`` rows: the caller sizes each chunk against the
    remaining space (``max_rows - n_rows``), so the builder never overshoots and
    never buffers a whole unit. Every batch :meth:`take` hands over has passed
    ``CanonicalBatch.validate()``.
    """

    _GROUPS = ("identity", "coordinates", "links", "payload", "extras")

    def __init__(self, schema: CanonicalSchema, max_rows: int):
        self.schema = schema
        self.max_rows = max_rows
        self.n_rows = 0
        self._parts: Dict[str, Dict[str, List[np.ndarray]]] = {group: {} for group in self._GROUPS}

    def add(self, columns: Dict[str, Dict[str, np.ndarray]]) -> None:
        """Append one chunk, given as the five canonical column groups."""
        added = None
        for group in self._GROUPS:
            for name, values in columns[group].items():
                self._parts[group].setdefault(name, []).append(values)
                added = values.shape[0] if added is None else added
        self.n_rows += 0 if added is None else added

    def take(self) -> CanonicalBatch:
        """Concatenate and hand over the accumulated rows, then reset."""
        groups = {
            group: {
                name: (parts[0] if len(parts) == 1 else np.concatenate(parts))
                for name, parts in self._parts[group].items()
            }
            for group in self._GROUPS
        }
        batch = CanonicalBatch(schema=self.schema, **groups)
        batch.validate()
        self._parts = {group: {} for group in self._GROUPS}
        self.n_rows = 0
        return batch
