"""Consistent-Trees ASCII source adapter: the canonical bridge over the existing
ASCII topology preparation (contracts C1/C2/C4 of
docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

**Why a bridge rather than a reader.** Consistent-Trees ASCII stores no merger
links: the vertical reader reconstructs them (``ctrees_utils.c``
``assign_mergertree_indices``), with ``fix_upid()`` host fix-ups, a literal
incremental-insertion progenitor loop and its own tie order. The converter
already reproduces that reconstruction bit for bit in its per-snapshot stages
(``scatter`` -> ``sort_index`` -> ``fixups`` -> ``links``), and C1/C4 keep it:
"ASCII topology reconstruction remains its own adapter-specific preparation".
So this adapter does not re-derive anything. :func:`prepare_workdir` runs those
stages in the *extended* scratch layout, which carries every row's canonical
source coordinate and the profile's declared extras through them unchanged,
and :class:`CTreesAsciiAdapter` then re-expresses the prepared records as
canonical batches. The legacy ASCII-to-v2 route is untouched: it runs the same
stages in the frozen legacy layout, and its writer refuses an extended workdir.

**Source coordinates (C1).** A *unit* is the part of one ASCII forest that one
file carries -- every ``#tree`` block of that file whose root the forest owns --
numbered in order of the forest's first ``#tree`` marker in the file. A row's
``row_ordinal`` is its position among its unit's rows in file order; the file
ordinal is the file's position in the ordered source inventory. This is the
source's own order, and nothing is sorted to derive it
(``ctrees_parser.plan_source_units``). ``SourceHaloID`` prefix-sums unit halo
counts in ascending ``(file, unit)`` order. A forest whose trees lie in
several files is several units; :meth:`CTreesAsciiAdapter.iter_forests` then
reports -1 ordinals for it, as C3 prescribes, and the per-file unit sidecars
the manifest owns retain its full membership.

**Identity is both kinds at once.** ``SourceHaloID`` and the coordinate are the
canonical keys; ``ForestIndex`` and ``HaloRankInForest`` are the existing ASCII
identity -- the dense ascending-forest-id enumeration and the post-fix-up
reference vertical order -- exactly as the link stage computed them.

**Values** are those the preparation fixed: float64 parse then float32
rounding, ``Len`` from ``round(Mvir * 1e-10 / particle_mass)``, ``Spin`` the
J/Mvir specific angular momentum (raw J where ``Mvir == 0``), ``M_Crit200`` the
native float32 ``Mvir`` in Msun/h, ``MostBoundID`` the ctrees ``id``. A declared
extra carries its own parse of its source column, before any convention, so an
extra that selects ``Jx``/``Jy``/``Jz`` keeps the raw catalog J.

**Links** are the link stage's snapshot-local rows re-expressed as target
``SourceHaloID``: ``Descendant`` resolves against snapshot N+1, ``FirstProgenitor``
against N-1, and ``NextProgenitor`` and both FoF links within N -- ASCII
topology is adjacent by construction, because the fix-up stage still rejects a
non-adjacent source descendant rather than inventing a prescription for one.
Chain order is the link stage's, untouched.

**Emission order.** Snapshot by snapshot, ascending, each snapshot in ascending
``SourceHaloID``, and no batch spans two snapshots, so every batch is strictly
increasing as ``CanonicalBatch`` requires. ``SourceHaloID`` does *not* ascend
across batches -- unlike the two prelinked adapters, whose emission follows the
inventory -- because the preparation is snapshot-partitioned; the transpose
partitions by snapshot anyway. After the last batch the adapter proves every
id in ``[1, total]`` was emitted exactly once.

**Bounds (C4).** ASCII keeps its existing per-snapshot bounds; nothing is
catalog-sized except the two budgeted terms C4 permits, each checked against
``memory_budget_bytes`` before it is allocated:

1. *The inventory*, O(unit count): :data:`INVENTORY_BYTES_PER_UNIT` per unit.
2. *The coverage bitset*, one bit per halo, the same exact structure the link
   stage's identity verification uses.

Per snapshot the resident working set is three ``SourceHaloID`` columns (the
snapshot and both neighbours) plus the snapshot's sort permutation, 8 bytes
per halo each, plus one batch's gathered rows (:func:`batch_term_bytes`, which
never charges more rows than the snapshot holds). Fixed and links records are
memory-mapped, never loaded whole, and each snapshot's are checksum-verified
once per pass: the mapping verified while the snapshot is the upcoming
neighbour is the one its own batches are gathered from. A snapshot too large
for the budget is refused, never allocated.
"""

import os
from pathlib import Path
from typing import Dict, Iterator, List, NamedTuple, Optional, Sequence, Tuple

import numpy as np
from column_schema import EXTRA_TYPES, CanonicalSchema, ConverterError
from ctrees_parser import DEFAULT_CHUNKSIZE, EXTRA_FIELD_PREFIX, ScratchLayout
from fixups import fixed_layout, run_fixups
from links import (
    DEFAULT_RANK_BUDGET_BYTES,
    LINKS_DTYPE_TAG,
    LINKS_RECORD_DTYPE,
    run_links,
)
from scatter import (
    SOURCE_SATISFIED,
    Manifest,
    file_md5,
    load_a_list,
    load_forests_list,
    run_scatter,
    source_units_name,
)
from sort_index import run_sort

from .base import (
    LINK_FIELDS,
    NULL_LINK,
    CanonicalBatch,
    SourceAdapter,
    SourceInventory,
    SourceUnit,
    check_budget,
    require_integer,
)
from .ctrees_hdf5 import ForestRecord

__all__ = [
    "ConverterError",
    "INVENTORY_BYTES_PER_UNIT",
    "INVENTORY_BASE_BYTES",
    "SNAPSHOT_BYTES_PER_HALO",
    "SOURCE_ID_READ_ROWS",
    "DEFAULT_MEMORY_BUDGET_BYTES",
    "CTreesAsciiAdapter",
    "batch_term_bytes",
    "prepare_workdir",
]

#: Peak bytes per inventory unit of the whole :meth:`CTreesAsciiAdapter.inventory`
#: path: the ``SourceInventory`` (a ``SourceUnit`` object, a prefix-sum int and
#: an index-dict entry per unit), the per-file sidecar tables and this
#: adapter's own int64 unit columns, all live together while it is built.
#:
#: Measured with ``tracemalloc`` around the real ``inventory()`` call on
#: prepared two-file synthetic workdirs of one halo per unit (whole path, not
#: just ``SourceInventory``):
#:
#:     n =  5,000 units    1,724 B/unit   (the base term below dominates)
#:     n = 24,000 units      520 B/unit   <- worst per-unit slope observed
#:     n = 60,000 units      461 B/unit
#:
#: 704 is that 520 plus a 1.35x margin, for the reason the L-Halo adapter
#: gives: a budget that refuses work is safe, one that accepts work it cannot
#: hold is the defect. ``BudgetAccountingTests`` re-measures the real path and
#: fails if ``INVENTORY_BASE_BYTES + n * INVENTORY_BYTES_PER_UNIT`` is ever
#: exceeded.
INVENTORY_BYTES_PER_UNIT = 704

#: The constant part of the inventory peak: ``scatter.file_md5`` reads in
#: 8 MiB blocks while every sidecar is verified, which is the whole measured
#: peak at two units (8,540,544 bytes). Budgeted as its own addend, as in the
#: other adapters, rather than folded into the per-unit figure; 9 MiB covers
#: that block with room for the manifest's own parse.
INVENTORY_BASE_BYTES = 9 * 1024 * 1024

#: Resident bytes per halo of one snapshot's emission, counted rather than
#: estimated: the previous, current and next snapshots' ``SourceHaloID``
#: columns are each charged at 8 bytes per halo of *their own* snapshot, and
#: this figure is the current snapshot's other term -- its int64 sort
#: permutation (8) plus the sorted copy used for the strict-order check (8).
SNAPSHOT_BYTES_PER_HALO = 16

#: Rows per bounded read when deriving a snapshot's ``SourceHaloID`` column from
#: its memory-mapped fixed records, so the three coordinate temporaries stay
#: O(1) in the snapshot size.
SOURCE_ID_READ_ROWS = 65536

#: Bytes the ``SourceHaloID`` derivation holds per row of one bounded read:
#: three int64 coordinates, the global unit index and the gathered base.
SOURCE_ID_READ_BYTES_PER_ROW = 5 * 8

#: Default ceiling, matching the other adapters and the link stage.
DEFAULT_MEMORY_BUDGET_BYTES = 2 * 1024**3

#: Payload names this adapter fills; column_schema fixes the set per format.
_PAYLOAD_NAMES = frozenset(
    ("Len", "SnapNum", "M_Crit200", "Pos", "Vel", "Spin", "VelDisp", "Vmax", "MostBoundID")
)

#: Canonical columns one batch row holds besides the gathered records:
#: identity (3 x int64), coordinates (3 x int64), links (5 x int64) and the
#: 64-byte ctrees payload (Len, SnapNum, M_Crit200, Pos, Vel, Spin, VelDisp,
#: Vmax, MostBoundID).
_BATCH_CANONICAL_BYTES_PER_ROW = 3 * 8 + 3 * 8 + 5 * 8 + 64


def batch_term_bytes(schema: CanonicalSchema, max_rows: int, n_rows: Optional[int] = None) -> int:
    """Bytes :meth:`CTreesAsciiAdapter.iter_batches` charges for one batch of a
    snapshot of ``n_rows`` halos, as one addend of that snapshot's budget check.

    A batch holds ``min(max_rows, n_rows)`` rows, since no batch spans two
    snapshots. Each row is charged its gathered fixed record (the schema's
    extended fixed layout), its links record, the canonical identity,
    coordinate, link and payload columns built from them, and one element of
    every declared extra; the total is doubled as headroom for the conversion
    temporaries a batch's assembly creates. The function is pure: it reads
    only the schema's declared widths, so a caller can evaluate it before any
    preparation has run.

    Args:
        schema: A ``consistent_trees_ascii`` schema; its extras set the widths.
        max_rows: The batch-size cap passed to ``iter_batches``.
        n_rows: The snapshot's halo count, or ``None`` to charge a full batch
            of ``max_rows`` -- the worst case, which over-refuses a small
            catalog (664 MiB for the default profile at ``1 << 20`` rows).

    Returns:
        The batch term in bytes.

    Raises:
        ConverterError: ``schema`` is not a ``consistent_trees_ascii`` schema,
            ``max_rows`` is not an integer of at least 1, or ``n_rows`` is not
            ``None`` or a non-negative integer.
    """
    max_rows = require_integer(
        max_rows,
        "max_rows",
        "a fractional batch size would be truncated, and True would silently mean 1",
        minimum=1,
    )
    if n_rows is None:
        rows = max_rows
    else:
        n_rows = require_integer(
            n_rows, "n_rows", "a snapshot holds a whole number of halos", minimum=0
        )
        rows = min(max_rows, n_rows)
    fixed_dtype, _fixed_tag = fixed_layout(ScratchLayout.from_schema(schema))
    row_bytes = (
        fixed_dtype.itemsize
        + LINKS_RECORD_DTYPE.itemsize
        + _BATCH_CANONICAL_BYTES_PER_ROW
        + sum(EXTRA_TYPES[extra.type].itemsize for extra in schema.extra_fields)
    )
    return 2 * rows * row_bytes


class _MappedSnapshot(NamedTuple):
    """One snapshot's verified, memory-mapped scratch and its ``SourceHaloID``
    column, carried from the iteration that verified it to the one that
    emits it."""

    snap: int
    fixed: np.ndarray
    links: np.ndarray
    ids: np.ndarray


def prepare_workdir(
    schema: CanonicalSchema,
    tree_files: Sequence,
    forests_list_path,
    a_list_path,
    simulation_info_path,
    workdir,
    *,
    pool_size: int = 1,
    chunksize: int = DEFAULT_CHUNKSIZE,
    rank_budget_bytes: int = DEFAULT_RANK_BUDGET_BYTES,
    max_rows: Optional[int] = None,
    memory_budget_bytes: Optional[int] = None,
) -> Manifest:
    """Run the existing ASCII topology preparation in the schema's extended
    scratch layout: scatter, sort, fix-ups and links. Returns the workdir
    manifest.

    No intermediate is consumed: the adapter reads the fixed and links scratch
    this leaves behind. The a_list, simulation_info and ordered source files
    are bound into the manifest as on the legacy route, and the schema's
    layout (tag and digest) is too, so a resume with any other selection --
    or with none -- is refused before anything is mutated.

    **Early batch-term refusal.** Given ``max_rows`` and
    ``memory_budget_bytes`` (the batch size and the adapter ceiling the
    ingest will read with), the largest snapshot's batch term
    (:func:`batch_term_bytes`) is checked against the budget as soon as
    scatter has counted every snapshot, before sort, fix-ups or links run:
    a snapshot :meth:`CTreesAsciiAdapter.iter_batches` would refuse for its
    batch term is refused here instead, with the same message and remedy,
    and the scatter output is kept for a resume under a larger budget. The
    two are given together or not at all; without them no early check is
    made and ``iter_batches`` remains the only one.

    **Resume** follows the stages' own rules, which this does not change: each
    stage skip-trusts the snapshots it already finished, but scatter, sort and
    fix-ups refuse a snapshot the link stage has already linked. So once any
    snapshot is linked, only the link stage is re-run -- after the recorded
    input identities (ordered source files, forests.list, a_list,
    simulation_info) are checked against the arguments, since scatter, which
    would otherwise check them, is not re-entered.
    """
    if schema.source_format != CTreesAsciiAdapter.source_format:
        raise ConverterError(
            "prepare_workdir needs a {!r} schema, got {!r}".format(
                CTreesAsciiAdapter.source_format, schema.source_format
            )
        )
    if (max_rows is None) != (memory_budget_bytes is None):
        raise ConverterError(
            "prepare_workdir needs max_rows and memory_budget_bytes together, got max_rows={!r} "
            "and memory_budget_bytes={!r}".format(max_rows, memory_budget_bytes)
        )
    if max_rows is not None:
        max_rows = require_integer(
            max_rows,
            "max_rows",
            "a fractional batch size would be truncated, and True would silently mean 1",
            minimum=1,
        )
        memory_budget_bytes = require_integer(
            memory_budget_bytes,
            "memory_budget_bytes",
            "a fractional ceiling would be truncated into a different budget than asked for",
            minimum=1,
        )
    manifest = Manifest.load_or_create(workdir, layout=ScratchLayout.from_schema(schema))
    if manifest.path.exists() and any(
        entry.get("status") == "linked" for entry in manifest.data["snapshots"].values()
    ):
        _check_recorded_inputs(
            manifest, tree_files, forests_list_path, a_list_path, simulation_info_path
        )
        return run_links(workdir, budget_bytes=rank_budget_bytes)
    scattered = run_scatter(
        tree_files=tree_files,
        forests_list_path=forests_list_path,
        a_list_path=a_list_path,
        workdir=workdir,
        pool_size=pool_size,
        chunksize=chunksize,
        simulation_info_path=simulation_info_path,
        schema=schema,
    )
    if max_rows is not None:
        _check_largest_batch_term(schema, scattered, max_rows, memory_budget_bytes)
    run_sort(workdir)
    run_fixups(workdir, a_list_path=a_list_path, simulation_info_path=simulation_info_path)
    return run_links(workdir, budget_bytes=rank_budget_bytes)


def _check_largest_batch_term(
    schema: CanonicalSchema, manifest: Manifest, max_rows: int, memory_budget_bytes: int
) -> None:
    """Refuse the largest scattered snapshot's batch term before any later
    stage runs. The row counts are the ones ``iter_batches`` reads, recorded
    by scatter's concatenation; the lowest-numbered snapshot of the largest
    count is the one named."""
    rows_of = {int(snap): int(entry["rows"]) for snap, entry in manifest.data["snapshots"].items()}
    if not rows_of:
        return
    largest = max(rows_of.values())
    snap = min(snap for snap, n_rows in rows_of.items() if n_rows == largest)
    check_budget(
        batch_term_bytes(schema, max_rows, largest),
        memory_budget_bytes,
        "snapshot {} ({} halos, batches of {})".format(snap, largest, max_rows),
        "raise memory_budget_bytes or lower max_rows",
    )


def _check_recorded_inputs(
    manifest: Manifest, tree_files, forests_list_path, a_list_path, simulation_info_path
) -> None:
    """The identity binding scatter applies on resume, for a resume that does
    not re-enter scatter: the same ordered sources, each still the bytes that
    were scattered, and the same metadata bytes.

    Source content is judged by scatter's own rule, ``Manifest.classify_source``
    (recorded size and mtime_ns, a ``consumed`` entry satisfying it without a
    stat), and a changed source is refused with the message ``run_scatter``
    gives the same case on the legacy route -- so silent substitution stays
    impossible whether or not scatter is re-entered.
    """
    provenance = manifest.data["provenance"]
    checks = (
        (
            "source_files",
            [str(Path(p).resolve()) for p in tree_files],
            provenance.get("source_files"),
        ),
        (
            "forests_list",
            load_forests_list(forests_list_path).md5,
            provenance.get("forests_list", {}).get("md5"),
        ),
        ("a_list", load_a_list(a_list_path)[1], provenance.get("a_list", {}).get("md5")),
        (
            "simulation_info",
            file_md5(simulation_info_path),
            provenance.get("simulation_info", {}).get("md5"),
        ),
    )
    for what, given, recorded in checks:
        if given != recorded:
            raise ConverterError(
                "{}: {} differs from the one this workdir was prepared with; refusing to "
                "resume -- use a fresh workdir".format(manifest.path, what)
            )
    changed = [
        Path(path) for path in tree_files if manifest.classify_source(path) not in SOURCE_SATISFIED
    ]
    if changed:
        raise ConverterError(
            "source file(s) changed after snapshots were finalized ({} pending: {}); "
            "downstream snapshot products would be stale — refusing to resume, "
            "use a fresh workdir".format(len(changed), [str(p) for p in changed[:3]])
        )


class CTreesAsciiAdapter(SourceAdapter):
    """Canonical batches from a workdir that :func:`prepare_workdir` completed.

    ``schema`` must be the one the workdir was prepared with: its digest is
    compared with the manifest's recorded layout, so a same-width schema that
    differs in any declared semantics is refused rather than read.
    """

    source_format = "consistent_trees_ascii"

    def __init__(
        self,
        schema: CanonicalSchema,
        workdir,
        *,
        memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    ):
        if schema.source_format != self.source_format:
            raise ConverterError(
                "CTreesAsciiAdapter needs a {!r} schema, got {!r}".format(
                    self.source_format, schema.source_format
                )
            )
        declared = {field.name for field in schema.payload_fields}
        if declared != _PAYLOAD_NAMES:  # pragma: no cover - column_schema fixes the set
            raise ConverterError(
                "the consistent_trees_ascii schema declares payload {}, but this adapter fills "
                "{}".format(sorted(declared), sorted(_PAYLOAD_NAMES))
            )
        memory_budget_bytes = require_integer(
            memory_budget_bytes,
            "memory_budget_bytes",
            "a fractional ceiling would be truncated into a different budget than asked for",
        )
        if memory_budget_bytes <= 0:
            raise ConverterError(
                "memory_budget_bytes must be positive, got {}".format(memory_budget_bytes)
            )
        try:
            workdir = Path(workdir)
        except TypeError as exc:
            raise ConverterError(
                "workdir {!r} is not a filesystem path ({})".format(workdir, exc)
            ) from exc
        self.schema = schema
        self.workdir = workdir
        self.memory_budget_bytes = memory_budget_bytes
        self.layout = ScratchLayout.from_schema(schema)
        self._manifest: Optional[Manifest] = None
        self._inventory: Optional[SourceInventory] = None
        #: per inventory unit, flattened in (file, unit) order
        self._unit_forest_ids = np.zeros(0, dtype=np.int64)
        self._unit_counts = np.zeros(0, dtype=np.int64)
        self._unit_bases = np.zeros(0, dtype=np.int64)
        #: index of each file's first unit in the flattened columns, plus the total
        self._file_unit_offsets = np.zeros(1, dtype=np.int64)

    # ---- public views ----------------------------------------------------

    def inventory(self) -> SourceInventory:
        """The ordered unit inventory, from the manifest-owned per-file unit
        sidecars. Every source file of the recorded inventory must have a
        completed scatter and a verified sidecar (C1: never narrow silently),
        and the units must account for every prepared halo."""
        if self._inventory is None:
            self._inventory = self._build_inventory()
        return self._inventory

    def iter_forests(self) -> Iterator[ForestRecord]:
        """Every forest in ``ForestIndex`` order, for the ``forests.h5`` sidecar.

        ``forest_id`` is the original ctrees forest id. A forest whose halos
        lie in exactly one unit reports that unit's file and unit ordinals; a
        forest spanning files reports -1 for both (C3), its membership being
        the unit sidecars'. ``n_halos`` is the forest's total. O(units +
        forests) from the inventory columns and the verified forest table.
        """
        self.inventory()
        manifest = self._manifest
        table_path = Path(manifest.workdir) / "forest_index_table.npy"
        manifest.verify_intermediate(table_path, "forest index table")
        # sized from the memory-mapped header, and checked, before anything is
        # materialised (C4: fail before allocation)
        header = np.load(table_path, mmap_mode="r")
        if header.dtype != np.dtype("<i8") or header.ndim != 1:
            raise ConverterError(
                "{}: forest index table must be int64 [n_forests], got {} {}".format(
                    table_path, header.dtype, header.shape
                )
            )
        # the table plus the two searchsorted bounds and the per-forest totals
        # (4 x 8 B per forest), and the six per-unit views below (6 x 8 B per unit)
        check_budget(
            header.nbytes * 4 + self._unit_counts.nbytes * 6,
            self.memory_budget_bytes,
            "the forest sidecar view ({} forest(s), {} unit(s))".format(
                header.shape[0], self._unit_counts.size
            ),
            "raise memory_budget_bytes",
        )
        forest_table = np.array(header)
        del header
        n_units_per_file = np.diff(self._file_unit_offsets)
        file_of_unit = np.repeat(np.arange(n_units_per_file.size, dtype=np.int64), n_units_per_file)
        unit_of_unit = np.arange(self._unit_counts.size, dtype=np.int64) - np.repeat(
            self._file_unit_offsets[:-1], n_units_per_file
        )
        populated = self._unit_counts > 0
        forests = self._unit_forest_ids[populated]
        order = np.argsort(forests, kind="stable")
        forests = forests[order]
        counts = self._unit_counts[populated][order]
        files = file_of_unit[populated][order]
        units = unit_of_unit[populated][order]
        low = np.searchsorted(forests, forest_table, side="left")
        high = np.searchsorted(forests, forest_table, side="right")
        # the table is ascending and unique, so the ranges are disjoint and in
        # order; they cover every populated unit iff their lengths sum to it
        if forest_table.size == 0 or np.any(high == low) or int((high - low).sum()) != forests.size:
            raise ConverterError(
                "{}: the forest index table and the source-unit sidecars disagree about which "
                "forests carry halos".format(manifest.path)
            )
        totals = np.add.reduceat(counts, low)
        for forest_index in range(forest_table.size):
            lo, hi = int(low[forest_index]), int(high[forest_index])
            single = hi - lo == 1
            yield ForestRecord(
                forest_index=forest_index,
                forest_id=int(forest_table[forest_index]),
                source_file_ordinal=int(files[lo]) if single else -1,
                unit_ordinal=int(units[lo]) if single else -1,
                n_halos=int(totals[forest_index]),
            )

    def iter_batches(self, max_rows: int) -> Iterator[CanonicalBatch]:
        """Stream canonical batches of at most ``max_rows`` rows (see the
        module docstring for their order and for what is resident)."""
        max_rows = require_integer(
            max_rows,
            "max_rows",
            "a fractional batch size would be truncated, and True would silently mean 1",
            minimum=1,
        )
        inventory = self.inventory()
        manifest = self._manifest
        snaps = sorted(int(s) for s in manifest.data["snapshots"])
        recorded = set(snaps)
        fixed_dtype, _fixed_tag = fixed_layout(manifest.layout)
        rows_of = {snap: int(manifest.data["snapshots"][str(snap)]["rows"]) for snap in snaps}

        total = inventory.total_halos
        check_budget(
            (total + 7) // 8,
            self.memory_budget_bytes,
            "the SourceHaloID coverage bitset (1 bit per halo, {} halos)".format(total),
            "raise memory_budget_bytes; the bitset is what proves every id is emitted once",
        )
        claimed = np.zeros((total + 7) // 8, dtype=np.uint8)

        emitted = 0
        previous: Optional[Tuple[int, np.ndarray]] = None
        upcoming: Optional[_MappedSnapshot] = None
        for snap in snaps:
            n_rows = rows_of[snap]
            n_prev = rows_of.get(snap - 1, 0) if snap - 1 in recorded else 0
            n_next = rows_of.get(snap + 1, 0) if snap + 1 in recorded else 0
            check_budget(
                8 * (n_prev + n_rows + n_next)
                + SNAPSHOT_BYTES_PER_HALO * n_rows
                + SOURCE_ID_READ_ROWS * SOURCE_ID_READ_BYTES_PER_ROW
                + batch_term_bytes(self.schema, max_rows, n_rows),
                self.memory_budget_bytes,
                "snapshot {} ({} halos, neighbours {} and {}, batches of {})".format(
                    snap, n_rows, n_prev, n_next, max_rows
                ),
                "raise memory_budget_bytes or lower max_rows",
            )
            if upcoming is not None and upcoming.snap == snap:
                fixed, links, current = upcoming.fixed, upcoming.links, upcoming.ids
            else:
                fixed, links = self._open_snapshot(snap, fixed_dtype)
                current = self._source_ids(fixed, snap)
            prev_ids = previous[1] if previous is not None and previous[0] == snap - 1 else None
            upcoming = None
            if snap + 1 in recorded:
                next_fixed, next_links = self._open_snapshot(snap + 1, fixed_dtype)
                upcoming = _MappedSnapshot(
                    snap + 1, next_fixed, next_links, self._source_ids(next_fixed, snap + 1)
                )
                del next_fixed, next_links
            next_ids = upcoming.ids if upcoming is not None else None

            order = np.argsort(current, kind="stable")
            ascending = current[order]
            if n_rows > 1 and not bool(np.all(ascending[1:] > ascending[:-1])):
                at = int(np.nonzero(ascending[1:] <= ascending[:-1])[0][0])
                raise ConverterError(
                    "snapshot {}: SourceHaloID {} occurs more than once -- two rows claim one "
                    "source coordinate".format(snap, int(ascending[at]))
                )
            del ascending
            for start in range(0, n_rows, max_rows):
                rows = order[start : start + max_rows]
                batch = self._batch(
                    snap, fixed[rows], links[rows], current[rows], prev_ids, current, next_ids
                )
                self._claim(claimed, batch.identity["SourceHaloID"], total)
                yield batch
            emitted += n_rows
            del order, fixed, links
            previous = (snap, current)

        if emitted != total:
            raise ConverterError(
                "{}: emitted {} halo(s) but the source inventory holds {}".format(
                    manifest.path, emitted, total
                )
            )
        # every row claimed a distinct in-range id and the counts agree, so
        # [1, total] is covered exactly; the bitset says so independently
        full, tail = divmod(total, 8)
        if not (
            bool(np.all(claimed[:full] == 0xFF))
            and (tail == 0 or int(claimed[full]) == (1 << tail) - 1)
        ):
            raise ConverterError(
                "{}: SourceHaloID coverage of [1, {}] is incomplete".format(manifest.path, total)
            )

    # ---- inventory -------------------------------------------------------

    def _load_manifest(self) -> Manifest:
        manifest_path = self.workdir / "manifest.json"
        if not manifest_path.is_file():
            raise ConverterError(
                "{}: no manifest; run prepare_workdir with this schema first".format(self.workdir)
            )
        manifest = Manifest.load_or_create(self.workdir)
        if manifest.layout != self.layout:
            raise ConverterError(
                "{}: prepared with scratch layout {!r} (schema {}), not the one this schema "
                "selects ({!r}, schema {}) -- refusing to read it as this "
                "schema".format(
                    manifest.path,
                    manifest.layout.dtype_tag,
                    manifest.layout.schema_digest,
                    self.layout.dtype_tag,
                    self.layout.schema_digest,
                )
            )
        snapshots = manifest.data["snapshots"]
        if not snapshots:
            raise ConverterError("{}: the manifest records no snapshots".format(manifest.path))
        unfinished = sorted(
            int(snap) for snap, entry in snapshots.items() if entry.get("status") != "linked"
        )
        if unfinished:
            raise ConverterError(
                "{}: snapshot(s) {} are not linked yet; finish prepare_workdir first".format(
                    manifest.path, unfinished[:5]
                )
            )
        return manifest

    def _build_inventory(self) -> SourceInventory:
        manifest = self._load_manifest()
        source_files = manifest.data["provenance"].get("source_files")
        if not source_files:
            raise ConverterError(
                "{}: no ordered source inventory is recorded".format(manifest.path)
            )
        scratch_dir = Path(manifest.workdir) / "scratch"
        tables: List[np.ndarray] = []
        n_units = 0
        for ordinal, path in enumerate(source_files):
            entry = manifest.source_entry(path)
            status = None if entry is None else entry.get("status")
            if status not in SOURCE_SATISFIED:
                raise ConverterError(
                    "{}: source file {} has scatter status {!r}; every inventory file must be "
                    "scattered -- a conversion never narrows silently".format(
                        manifest.path, path, status
                    )
                )
            if entry.get("src_index") != ordinal:
                raise ConverterError(
                    "{}: source file {} records src_index {!r} at inventory position {}".format(
                        manifest.path, path, entry.get("src_index"), ordinal
                    )
                )
            units_path = scratch_dir / source_units_name(ordinal)
            manifest.verify_intermediate(units_path, "source-units sidecar")
            header = np.load(units_path, mmap_mode="r")
            if header.dtype != np.dtype("<i8") or header.ndim != 2 or header.shape[1] != 2:
                raise ConverterError(
                    "{}: source-units sidecar must be int64 [n_units, 2], got {} {}".format(
                        units_path, header.dtype, header.shape
                    )
                )
            n_units += int(header.shape[0])
            check_budget(
                INVENTORY_BASE_BYTES + n_units * INVENTORY_BYTES_PER_UNIT,
                self.memory_budget_bytes,
                "the source inventory ({} unit(s) through file {})".format(n_units, ordinal),
                "raise memory_budget_bytes",
            )
            table = np.array(header)
            del header
            if table.size and int(table[:, 1].min()) < 0:
                raise ConverterError("{}: negative unit halo count".format(units_path))
            if int(table[:, 1].sum()) != int(entry.get("parsed_count", -1)):
                raise ConverterError(
                    "{}: units hold {} halo(s), the scatter parsed {}".format(
                        units_path, int(table[:, 1].sum()), entry.get("parsed_count")
                    )
                )
            tables.append(table)

        units = [
            SourceUnit(source_file_ordinal=ordinal, unit_ordinal=unit, n_halos=int(count))
            for ordinal, table in enumerate(tables)
            for unit, count in enumerate(table[:, 1].tolist())
        ]
        inventory = SourceInventory(units)
        prepared = sum(int(entry["rows"]) for entry in manifest.data["snapshots"].values())
        if inventory.total_halos != prepared:
            raise ConverterError(
                "{}: the source inventory holds {} halo(s), the prepared snapshots {}".format(
                    manifest.path, inventory.total_halos, prepared
                )
            )
        stacked = np.concatenate(tables) if tables else np.zeros((0, 2), dtype=np.int64)
        self._unit_forest_ids = np.ascontiguousarray(stacked[:, 0])
        self._unit_counts = np.ascontiguousarray(stacked[:, 1])
        # the emitted ids are the inventory's own bases, unit for unit: both
        # the stacked tables and ``inventory.units`` are in (file, unit) order
        self._unit_bases = np.fromiter(
            (inventory.base_id(u.source_file_ordinal, u.unit_ordinal) for u in inventory.units),
            dtype=np.int64,
            count=len(inventory.units),
        )
        self._file_unit_offsets = np.r_[
            np.int64(0), np.cumsum([table.shape[0] for table in tables], dtype=np.int64)
        ]
        self._manifest = manifest
        return inventory

    # ---- snapshots -------------------------------------------------------

    def _open_snapshot(self, snap: int, fixed_dtype: np.dtype) -> Tuple[np.ndarray, np.ndarray]:
        """Verify one snapshot's fixed and links scratch and memory-map both."""
        manifest = self._manifest
        entry = manifest.data["snapshots"][str(snap)]
        _dtype, fixed_tag = fixed_layout(manifest.layout)
        mapped = []
        for key, what, dtype, tag in (
            ("fixed_file", "fixed snapshot scratch", fixed_dtype, fixed_tag),
            ("links_file", "snapshot links scratch", LINKS_RECORD_DTYPE, LINKS_DTYPE_TAG),
        ):
            path = entry.get(key)
            if path is None:
                raise ConverterError(
                    "snapshot {}: no {} is recorded".format(snap, key.replace("_", " "))
                )
            meta = manifest.verify_intermediate(path, what)
            if meta.get("dtype_tag") != tag:
                raise ConverterError(
                    "{}: {} dtype tag {!r} != expected {!r}".format(
                        path, what, meta.get("dtype_tag"), tag
                    )
                )
            n_rows = int(entry["rows"])
            if os.path.getsize(path) != n_rows * dtype.itemsize:
                raise ConverterError(
                    "{}: {} is {} bytes, expected {} rows x {} bytes".format(
                        path, what, os.path.getsize(path), n_rows, dtype.itemsize
                    )
                )
            if n_rows == 0:  # pragma: no cover - a recorded snapshot has rows
                mapped.append(np.zeros(0, dtype=dtype))
            else:
                mapped.append(np.memmap(path, dtype=dtype, mode="r", shape=(n_rows,)))
        return mapped[0], mapped[1]

    def _source_ids(self, fixed: np.ndarray, snap: int) -> np.ndarray:
        """``SourceHaloID`` of every row of one snapshot, in slab order, derived
        in bounded reads from the recorded coordinates; every coordinate is
        checked against the inventory it must lie in."""
        n_files = self._file_unit_offsets.size - 1
        out = np.empty(fixed.shape[0], dtype=np.int64)
        for start in range(0, fixed.shape[0], SOURCE_ID_READ_ROWS):
            block = fixed[start : start + SOURCE_ID_READ_ROWS]
            files = np.asarray(block["src_file_ordinal"], dtype=np.int64)
            units = np.asarray(block["src_unit_ordinal"], dtype=np.int64)
            rows = np.asarray(block["src_row_ordinal"], dtype=np.int64)
            # each test indexes with values the previous one proved in range
            bad = (files < 0) | (files >= n_files)
            if not bad.any():
                bad = (units < 0) | (units >= np.diff(self._file_unit_offsets)[files])
            flat = np.zeros(0, dtype=np.int64)
            if not bad.any():
                flat = self._file_unit_offsets[files] + units
                bad = (rows < 0) | (rows >= self._unit_counts[flat])
            if bad.any():
                at = int(np.nonzero(bad)[0][0])
                raise ConverterError(
                    "snapshot {}: halo id {} has source coordinate ({}, {}, {}) outside the "
                    "source inventory".format(
                        snap, int(block["id"][at]), int(files[at]), int(units[at]), int(rows[at])
                    )
                )
            out[start : start + block.shape[0]] = self._unit_bases[flat] + rows
        return out

    @staticmethod
    def _claim(claimed: np.ndarray, ids: np.ndarray, total: int) -> None:
        """Mark one batch's ids in the coverage bitset, refusing a repeat."""
        slots = ids - 1
        if ids.size and (int(slots.min()) < 0 or int(slots.max()) >= total):
            # unreachable through _source_ids, which bounds every id first
            raise ConverterError("SourceHaloID outside [1, {}] in a batch".format(total))
        byte = slots >> 3
        mask = (np.int64(1) << (slots & 7)).astype(np.uint8)
        if bool(np.any(claimed[byte] & mask)):
            first = int(ids[np.nonzero(claimed[byte] & mask)[0][0]])
            raise ConverterError("SourceHaloID {} was emitted twice across snapshots".format(first))
        # a batch's ids are strictly increasing, so equal bytes are adjacent;
        # OR each byte's bits together before the store
        starts = np.nonzero(np.r_[True, byte[1:] != byte[:-1]])[0]
        claimed[byte[starts]] |= np.bitwise_or.reduceat(mask, starts)

    def _batch(
        self,
        snap: int,
        fixed: np.ndarray,
        links: np.ndarray,
        ids: np.ndarray,
        prev_ids: Optional[np.ndarray],
        current_ids: np.ndarray,
        next_ids: Optional[np.ndarray],
    ) -> CanonicalBatch:
        """One batch: gathered rows of one snapshot, in ascending SourceHaloID."""
        n = ids.size
        if n and not bool(np.all(fixed["snap"] == snap)):
            raise ConverterError(
                "snapshot {}: fixed scratch holds a record of snapshot {}".format(
                    snap, int(fixed["snap"][np.nonzero(fixed["snap"] != snap)[0][0]])
                )
            )
        targets = {
            "Descendant": (next_ids, snap + 1),
            "FirstProgenitor": (prev_ids, snap - 1),
            "NextProgenitor": (current_ids, snap),
            "FirstHaloInFOFgroup": (current_ids, snap),
            "NextHaloInFOFgroup": (current_ids, snap),
        }
        link_columns: Dict[str, np.ndarray] = {}
        for name in LINK_FIELDS:
            target_ids, target_snap = targets[name]
            link_columns[name] = _translate_link(
                np.asarray(links[name], dtype=np.int64), target_ids, name, snap, target_snap
            )
        payload = {
            "Len": np.ascontiguousarray(fixed["Len"], dtype=np.int32),
            "SnapNum": np.full(n, snap, dtype=np.int32),
            "M_Crit200": np.ascontiguousarray(fixed["Mvir"], dtype=np.float32),
            "Pos": np.column_stack((fixed["X"], fixed["Y"], fixed["Z"])).astype(np.float32),
            "Vel": np.column_stack((fixed["VX"], fixed["VY"], fixed["VZ"])).astype(np.float32),
            "Spin": np.column_stack((fixed["Jx"], fixed["Jy"], fixed["Jz"])).astype(np.float32),
            "VelDisp": np.ascontiguousarray(fixed["vrms"], dtype=np.float32),
            "Vmax": np.ascontiguousarray(fixed["vmax"], dtype=np.float32),
            "MostBoundID": np.ascontiguousarray(fixed["MostBoundID"], dtype=np.int64),
        }
        extras = {
            extra.name: np.ascontiguousarray(
                fixed[EXTRA_FIELD_PREFIX + extra.name], dtype=EXTRA_TYPES[extra.type].numpy_dtype
            )
            for extra in self.schema.extra_fields
        }
        batch = CanonicalBatch(
            schema=self.schema,
            identity={
                "SourceHaloID": np.ascontiguousarray(ids, dtype=np.int64),
                "ForestIndex": np.ascontiguousarray(links["ForestIndex"], dtype=np.int64),
                "HaloRankInForest": np.ascontiguousarray(links["HaloRankInForest"], dtype=np.int64),
            },
            coordinates={
                "source_file_ordinal": np.ascontiguousarray(
                    fixed["src_file_ordinal"], dtype=np.int64
                ),
                "unit_ordinal": np.ascontiguousarray(fixed["src_unit_ordinal"], dtype=np.int64),
                "row_ordinal": np.ascontiguousarray(fixed["src_row_ordinal"], dtype=np.int64),
            },
            links=link_columns,
            payload=payload,
            extras=extras,
        )
        batch.validate()
        return batch


def _translate_link(
    rows: np.ndarray,
    target_ids: Optional[np.ndarray],
    name: str,
    snap: int,
    target_snap: int,
) -> np.ndarray:
    """One link column: snapshot-local target rows -> target ``SourceHaloID``.

    ``-1`` stays ``-1``; any other negative row, a row past the target
    snapshot's end, or a link into a snapshot with no prepared records is a
    corrupt preparation and aborts.
    """
    out = np.full(rows.size, NULL_LINK, dtype=np.int64)
    has = rows != NULL_LINK
    if not has.any():
        return out
    n_target = 0 if target_ids is None else target_ids.size
    bad = has & ((rows < 0) | (rows >= n_target))
    if bad.any():
        at = int(np.nonzero(bad)[0][0])
        raise ConverterError(
            "snapshot {}: {} row {} does not exist at snapshot {} ({} halos)".format(
                snap, name, int(rows[at]), target_snap, n_target
            )
        )
    out[has] = target_ids[rows[has]]
    return out
