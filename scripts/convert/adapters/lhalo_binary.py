"""Lossless L-Halo binary source adapter (Slice 3 of the converter
generalisation plan, docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md,
contracts C1/C3/C4).

Streams the shipped fixed-record L-Halo tree files -- mini-Millennium,
Millennium, mini-Uchuu and micro-Uchuu all ship the same 19-field, 104-byte
record -- into the canonical batches ``adapters/base.py`` defines. It reads
only; it opens no file for writing and creates no path, so Slice 1's
source-overwrite protection has no write set to enumerate here.

**What "lossless" means on this route.** The adapter preserves what the source
stores and converts nothing:

- ``Len`` is the source's supplied particle count, never re-derived from mass.
- ``M_Crit200`` stays ``float32`` in ``1e10 Msun/h``. A round trip through
  ``Msun/h`` and back would destroy bit parity (C3), so no unit arithmetic of
  any kind is applied.
- ``Spin`` is already specific angular momentum in this format and is **not**
  renormalised by ``J/Mvir`` the way the Consistent-Trees route's producer
  does.
- ``MostBoundID`` is carried as signed int64 catalog data. Real sources carry
  both duplicates (845,103 of them in mini-Millennium) and negative values
  (mini-Uchuu), so it is never treated as a key -- ``SourceHaloID`` is.
- No Consistent-Trees host fixup runs, no source is repaired, and no phantom
  halo is inserted to close a gap.

The only value transformation between disk and batch is **byte-order
normalisation**: the record is read through the profile's declared byte order
and the emitted arrays are native-endian, because ``CanonicalBatch`` compares
against native dtypes. That is a re-encoding of the same value, not a cast --
``column_schema.build_schema`` has already proved, at freeze time, that every
required role and every selected extra matches its source field's stored
element type exactly, so no widening, narrowing or int-through-float step can
occur here.

**Identity** follows the vertical reader's own enumeration (C1), which was
read off the C driver rather than assumed:

- ``ForestIndex`` is the file-prefix tree number -- the cumulative tree count
  over the preceding files of this inventory plus the tree's index within its
  file. That is exactly ``GlobalForestOffset + unit`` in
  ``src/core/build_model.c``'s ``make_unique_galaxy_id()``, where
  ``src/core/vertical_driver.c``'s ``build_partition_file_offsets()`` builds
  the per-file prefix.
- ``HaloRankInForest`` is the original within-tree row index (``halonr``),
  never a re-sorted one.
- ``SourceHaloID`` is the inventory's prefix sum over ``(file, tree)`` and is
  distinct from ``MostBoundID`` by construction.

**Bounds (C4).** Three terms scale with something other than a constant, and
all three are named rather than left implicit:

1. *Record reads* are bounded by the caller's ``max_rows``. A tree larger than
   one batch is read and emitted across several batches; a tree is never
   materialised whole as records.
2. *Per-tree validation* -- the five links plus ``SnapNum`` as int64, **plus
   the transient scratch the structural checks allocate while those columns
   are still live** -- peaks at :data:`VALIDATION_BYTES_PER_HALO` bytes per
   halo for the tree currently being validated. Structural validation
   (reciprocal chains, cycles, FoF membership) is not expressible
   chunk-locally, so this term is deliberate, budgeted, and checked *before*
   allocation. The retained columns are only 48 of those bytes; counting just
   them would understate the real peak by more than half, so the budget uses
   the measured whole-path figure instead.
3. *The inventory* is O(tree count), which C4 permits as an explicitly
   budgeted term. It is the contract's own mandated structure: C1 defines
   ``SourceHaloID`` as a prefix sum over the complete ordered inventory, so it
   cannot be streamed away. Measured at ~464 bytes per unit peak during
   construction (``tracemalloc`` around ``SourceInventory``), which is the
   figure :data:`INVENTORY_BYTES_PER_UNIT` carries and the budget check uses.

Both budgeted terms fail loudly against ``memory_budget_bytes`` before the
allocation rather than after it. A full 512-file Millennium inventory needs
roughly 6.6 GB by that measure and will refuse to build under the default
budget: that refusal is the honest answer, and the operator raises the budget
deliberately.

**Validation is rejection, never repair.** Every structural rule enforced here
was first checked against all four shipped datasets by an independent
hand-built ``numpy`` dtype, with zero violations, so none of them is an
invented gate that real data trips. A valid forward gap is *not* malformed and
survives as an exact source-key edge (C1); mini-Millennium's 29,291 of them are
the acceptance case.
"""

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

import numpy as np
from column_schema import (
    EXTRA_TYPES,
    CanonicalSchema,
    ConverterError,
    resolve_extra_sources,
    resolve_required_columns,
)

from .base import (
    INT64_MAX,
    LINK_FIELDS,
    NULL_LINK,
    CanonicalBatch,
    SourceAdapter,
    SourceInventory,
    SourceUnit,
)
from .source_inventory import LHaloHeader, read_lhalo_header

__all__ = [
    "ConverterError",
    "TOPOLOGY_COLUMNS",
    "TOPOLOGY_COLUMN_BYTES_PER_HALO",
    "VALIDATION_BYTES_PER_HALO",
    "INVENTORY_BYTES_PER_UNIT",
    "DEFAULT_MEMORY_BUDGET_BYTES",
    "LHaloBinaryAdapter",
]


#: The columns whole-tree structural validation needs: the five stored links
#: plus the snapshot they are interpreted against.
TOPOLOGY_COLUMNS: Tuple[str, ...] = LINK_FIELDS + ("SnapNum",)

#: Bytes the six retained topology columns occupy per halo. Held as int64
#: rather than the source's int32 so that index arithmetic, ``bincount`` and
#: the chain walks below cannot overflow or silently re-cast mid-expression.
#: This is **not** the budget figure: see :data:`VALIDATION_BYTES_PER_HALO`.
TOPOLOGY_COLUMN_BYTES_PER_HALO = 8 * len(TOPOLOGY_COLUMNS)

#: Peak bytes per halo of the whole per-tree validation path, which is what
#: the budget check must bound. The six retained columns above are only 48 of
#: these; ``_validate_tree`` and its helpers then allocate whole-tree scratch
#: -- the ``rows`` index array, the boolean masks, their fancy-indexed int64
#: copies (``heads``, ``owners``, ``descendant[heads]`` and their FoF
#: counterparts) and several ``bincount`` results -- while those columns are
#: still live.
#:
#: Measured with ``tracemalloc`` around **the retained columns plus**
#: ``_validate_tree`` -- both, because the columns stay live across the
#: validation and measuring only the scratch is the same half-accounting that
#: made the previous figure wrong. Taken at n = 20k, 40k, 100k and 400k halos
#: over three deliberately different tree shapes; the per-halo figure was flat
#: in n to within 0.6% in every case, so this is a slope, not a two-point
#: extrapolation:
#:
#:     linear chain (every halo has both a descendant and a
#:                   FirstProgenitor)                        119.0 B/halo
#:     wide (one progenitor each, large FoF groups)            98.0 B/halo
#:     dense sibling chains (64-member progenitor/FoF chains) 109.3 B/halo
#:
#: The linear shape is the worst because it is the one that makes
#: ``has_first`` and ``has_descendant`` dense *simultaneously*, so the
#: FirstProgenitor block's three int64 copies are all full length. 160 is that
#: 119.1 worst case plus a 1.34x margin, because a budget that refuses work is
#: safe and one that accepts work it cannot hold is the defect this figure
#: exists to prevent. The margin costs nothing real: the largest tree in any
#: shipped package (397,280 halos, mini-Uchuu) needs 63.6 MB of the 2 GiB
#: default.
#:
#: ``BudgetAccountingTests`` in the test module re-measures all three shapes
#: and fails if any exceeds this constant, so the figure is self-policing
#: rather than a number that silently rots as numpy's temporaries change.
VALIDATION_BYTES_PER_HALO = 160

#: Peak bytes per inventory unit during ``SourceInventory`` construction,
#: measured with ``tracemalloc`` at 50k and 200k units (463.7 and 462.5
#: bytes/unit respectively; the resident figure afterwards is ~341). The peak
#: is the figure the budget check must use: it is what actually has to fit.
INVENTORY_BYTES_PER_UNIT = 464

#: Default ceiling for the two budgeted terms above. Chosen to hold every
#: inventory this plan's L-Halo sources actually present -- micro-Uchuu's
#: 440,651 trees need ~204 MB, mini-Millennium's 29,585 need ~14 MB -- while
#: refusing a full 512-file Millennium (~6.6 GB) loudly instead of paging.
DEFAULT_MEMORY_BUDGET_BYTES = 2 * 1024**3


@dataclass(frozen=True)
class _SourceFile:
    """One inventory file: its declared ordinal, path and validated header."""

    ordinal: int
    path: Path
    header: LHaloHeader
    forest_base: int  # cumulative tree count over the preceding files


class LHaloBinaryAdapter(SourceAdapter):
    """Streams L-Halo binary trees into canonical batches.

    ``sources`` is an ordered sequence of ``(source_file_ordinal, path)``
    pairs. The ordinal is explicit rather than inferred, because it is durable
    identity: a conversion of files 4-7 must record 4, 5, 6, 7 and not pretend
    they are 0-3. ``source_inventory.lhalo_file_paths()`` produces the pairs
    for a package's declared file range; passing them by hand is equally
    valid, and is what the tests do.

    ``max_snapshot`` is ``len(a_list) - 1`` when the caller has an a_list, and
    bounds valid ``SnapNum`` values to ``[0, max_snapshot]``. Without it only
    non-negativity is enforced, and the adapter says so rather than inventing
    an upper bound.
    """

    source_format = "lhalo_binary"

    def __init__(
        self,
        schema: CanonicalSchema,
        sources: Sequence[Tuple[int, object]],
        *,
        max_snapshot: Optional[int] = None,
        memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    ):
        if schema.source_format != self.source_format:
            raise ConverterError(
                "LHaloBinaryAdapter needs a {!r} schema, got {!r}".format(
                    self.source_format, schema.source_format
                )
            )
        if schema.source_layout is None:  # pragma: no cover - build_schema guarantees it
            raise ConverterError("a lhalo_binary schema always carries a frozen source layout")
        if memory_budget_bytes <= 0:
            raise ConverterError(
                "memory_budget_bytes must be positive, got {}".format(memory_budget_bytes)
            )
        if max_snapshot is not None and max_snapshot < 0:
            raise ConverterError("max_snapshot must be non-negative, got {}".format(max_snapshot))

        self.schema = schema
        self.layout = schema.source_layout
        self.max_snapshot = max_snapshot
        self.memory_budget_bytes = int(memory_budget_bytes)

        self._record_dtype = np.dtype(self.layout.numpy_dtype_spec())
        if self._record_dtype.itemsize != self.layout.itemsize:  # pragma: no cover - defensive
            raise ConverterError(
                "frozen source layout claims a {}-byte record but its dtype is {} bytes".format(
                    self.layout.itemsize, self._record_dtype.itemsize
                )
            )

        # Resolve through the shared Slice 2 API rather than reimplementing
        # alias matching: the layout's entry names are exactly the source
        # field names a binary profile may reference.
        available = [entry.name for entry in self.layout.entries]
        self._roles = resolve_required_columns(schema, available)
        self._extras = resolve_extra_sources(schema, available)
        self._sources = self._resolve_sources(sources)
        self._files: Tuple[_SourceFile, ...] = ()
        self._inventory: Optional[SourceInventory] = None

    # ---- construction helpers -------------------------------------------

    def _resolve_sources(
        self, sources: Sequence[Tuple[int, object]]
    ) -> Tuple[Tuple[int, Path], ...]:
        """Check the requested file list before anything is opened.

        A missing requested file is fatal (C1): silently narrowing a
        conversion to the files that happen to be present is the failure mode
        the plan's data-availability section names explicitly.

        Every malformed shape leaves here as this module's named
        ``ConverterError``, never as a raw ``TypeError``/``ValueError``. The
        ordinal is *type*-checked rather than coerced: ``int(4.9)`` would
        silently convert a caller's mistake into file 4, and durable identity
        is the last thing that should be quietly rounded. ``bool`` is
        excluded explicitly because it is a subclass of ``int``, so ``True``
        would otherwise pass as ordinal 1.
        """
        resolved: List[Tuple[int, Path]] = []
        previous: Optional[int] = None
        for position, entry in enumerate(sources):
            try:
                ordinal, path = entry
            except (TypeError, ValueError):
                raise ConverterError(
                    "sources[{}] must be a (source_file_ordinal, path) pair, got {!r}".format(
                        position, entry
                    )
                ) from None
            if isinstance(ordinal, bool) or not isinstance(ordinal, (int, np.integer)):
                raise ConverterError(
                    "sources[{}]: source_file_ordinal must be an integer, got {!r}; it is "
                    "recorded identity and is never coerced".format(position, ordinal)
                )
            ordinal = int(ordinal)
            if ordinal < 0:
                raise ConverterError(
                    "sources[{}]: source_file_ordinal must be non-negative, got {}".format(
                        position, ordinal
                    )
                )
            if previous is not None and ordinal <= previous:
                raise ConverterError(
                    "sources must be in ascending source_file_ordinal order: {} follows {} at "
                    "position {}".format(ordinal, previous, position)
                )
            previous = ordinal
            try:
                path = Path(path)
            except TypeError as exc:
                raise ConverterError(
                    "sources[{}]: {!r} is not a filesystem path ({})".format(position, path, exc)
                ) from exc
            if not path.exists():
                raise ConverterError(
                    "requested source file {} (ordinal {}) is missing; a conversion fails rather "
                    "than narrowing to the files that are present".format(path, ordinal)
                )
            if not path.is_file():
                raise ConverterError(
                    "requested source file {} (ordinal {}) is not a regular file".format(
                        path, ordinal
                    )
                )
            resolved.append((ordinal, path))
        if not resolved:
            raise ConverterError(
                "no source files requested; an empty conversion is a caller error, not an empty "
                "result"
            )
        return tuple(resolved)

    # ---- inventory -------------------------------------------------------

    def inventory(self) -> SourceInventory:
        """Read every file's count header and build the complete ordered
        inventory.

        Cached: the headers are read once, and ``iter_batches`` reuses both the
        inventory and the validated headers rather than re-reading them.
        """
        if self._inventory is None:
            self._files, self._inventory = self._build_inventory()
        return self._inventory

    def _build_inventory(self) -> Tuple[Tuple[_SourceFile, ...], SourceInventory]:
        byte_order = self.layout.numpy_byte_order
        files: List[_SourceFile] = []
        units: List[SourceUnit] = []
        forest_base = 0
        for ordinal, path in self._sources:
            header = read_lhalo_header(
                path, byte_order=byte_order, record_bytes=self.layout.itemsize
            )
            self._check_budget(
                (len(units) + header.ntrees) * INVENTORY_BYTES_PER_UNIT,
                "inventory of {} trees".format(len(units) + header.ntrees),
                "raise memory_budget_bytes, or convert a narrower file range",
            )
            files.append(
                _SourceFile(ordinal=ordinal, path=path, header=header, forest_base=forest_base)
            )
            for tree_ordinal in range(header.ntrees):
                units.append(
                    SourceUnit(
                        source_file_ordinal=ordinal,
                        unit_ordinal=tree_ordinal,
                        n_halos=int(header.tree_halo_counts[tree_ordinal]),
                    )
                )
            # Mirrors build_partition_file_offsets()'s own overflow guard: the
            # file-prefix tree number is the vertical reader's forest identity,
            # and it may not wrap.
            if header.ntrees > INT64_MAX - forest_base:
                raise ConverterError(
                    "{}: file-prefix tree number overflows int64 after {} trees".format(
                        path, forest_base
                    )
                )
            forest_base += header.ntrees
        return tuple(files), SourceInventory(units)

    def _check_budget(self, required_bytes: int, what: str, remedy: str) -> None:
        """Refuse an over-budget allocation *before* making it (C4)."""
        if required_bytes > self.memory_budget_bytes:
            raise ConverterError(
                "{} needs {} bytes, above the configured memory budget of {} bytes; {}".format(
                    what, required_bytes, self.memory_budget_bytes, remedy
                )
            )

    # ---- batch streaming -------------------------------------------------

    def iter_batches(self, max_rows: int) -> Iterator[CanonicalBatch]:
        """Stream canonical batches of at most ``max_rows`` rows.

        A tree is validated in full before any of its rows enter a batch, so a
        structurally invalid tree fails before it can be emitted. Batches span
        tree and file boundaries freely: ``SourceHaloID`` ascends across the
        whole inventory, so a batch that ends mid-tree is still strictly
        increasing, and packing them keeps a catalog of 29,585 small trees from
        producing 29,585 tiny batches.
        """
        if max_rows < 1:
            raise ConverterError("max_rows must be at least 1, got {}".format(max_rows))
        inventory = self.inventory()
        builder = _BatchBuilder(self.schema, max_rows)

        for source in self._files:
            header = source.header
            with open(source.path, "rb") as handle:
                offset = header.header_bytes
                for tree_ordinal in range(header.ntrees):
                    n_halos = int(header.tree_halo_counts[tree_ordinal])
                    context = "{}: tree {}".format(source.path, tree_ordinal)
                    if n_halos == 0:
                        # A zero-halo tree is a legal inventory unit with no
                        # rows. It consumes a SourceHaloID range of length
                        # zero, which the prefix sums already handle.
                        continue
                    topology = self._read_tree_topology(handle, offset, n_halos, max_rows, context)
                    _validate_tree(topology, n_halos, context, self.max_snapshot)

                    base_id = inventory.base_id(source.ordinal, tree_ordinal)
                    forest_index = source.forest_base + tree_ordinal
                    handle.seek(offset)
                    start = 0
                    while start < n_halos:
                        count = min(max_rows - builder.n_rows, n_halos - start)
                        records = self._read_records(handle, count, context)
                        builder.add(
                            self._columns(
                                records, source, tree_ordinal, base_id, forest_index, start, context
                            )
                        )
                        start += count
                        if builder.n_rows == max_rows:
                            yield builder.take()
                    offset += n_halos * self.layout.itemsize
        if builder.n_rows:
            yield builder.take()

    def _read_records(self, handle, count: int, context: str) -> np.ndarray:
        """Read exactly ``count`` records from the current file position."""
        want = count * self.layout.itemsize
        raw = handle.read(want)
        if len(raw) != want:
            raise ConverterError(
                "{}: truncated halo payload (need {} bytes for {} records, got {})".format(
                    context, want, count, len(raw)
                )
            )
        return np.frombuffer(raw, dtype=self._record_dtype, count=count)

    def _read_tree_topology(
        self, handle, offset: int, n_halos: int, max_rows: int, context: str
    ) -> Dict[str, np.ndarray]:
        """Gather one tree's link and snapshot columns, reading in chunks.

        The columns are whole-tree; the *reads* are not. This is the one
        deliberate whole-tree term, and it is budget-checked before the
        allocation rather than after it.

        The check covers the *whole* validation path, not just the six
        columns allocated here: ``_validate_tree`` runs while they are live
        and allocates more whole-tree scratch of its own, so budgeting the
        columns alone would let an operator's configured ceiling be exceeded
        by more than 2x a few lines later.
        """
        self._check_budget(
            n_halos * VALIDATION_BYTES_PER_HALO,
            "{}: structural validation of {} halos".format(context, n_halos),
            "raise memory_budget_bytes",
        )
        columns = {name: np.empty(n_halos, dtype=np.int64) for name in TOPOLOGY_COLUMNS}
        handle.seek(offset)
        start = 0
        while start < n_halos:
            count = min(max_rows, n_halos - start)
            records = self._read_records(handle, count, context)
            for name in TOPOLOGY_COLUMNS:
                columns[name][start : start + count] = records[self._roles[name]]
            start += count
        return columns

    # ---- column construction --------------------------------------------

    def _columns(
        self,
        records: np.ndarray,
        source: _SourceFile,
        tree_ordinal: int,
        base_id: int,
        forest_index: int,
        start: int,
        context: str,
    ) -> Dict[str, Dict[str, np.ndarray]]:
        """Turn one chunk of records into the five canonical column groups."""
        n_rows = records.shape[0]
        rows = np.arange(start, start + n_rows, dtype=np.int64)

        identity = {
            "SourceHaloID": base_id + rows,
            "ForestIndex": np.full(n_rows, forest_index, dtype=np.int64),
            "HaloRankInForest": rows.copy(),
        }
        coordinates = {
            "source_file_ordinal": np.full(n_rows, source.ordinal, dtype=np.int64),
            "unit_ordinal": np.full(n_rows, tree_ordinal, dtype=np.int64),
            "row_ordinal": rows.copy(),
        }
        # A stored link is a within-tree row index; the canonical form is the
        # target's SourceHaloID, so the whole tree's ids shift by one base.
        # Only -1 is null, and _validate_tree has already refused anything
        # else negative or out of tree.
        links = {}
        for name in LINK_FIELDS:
            local = records[self._roles[name]].astype(np.int64)
            links[name] = np.where(local >= 0, base_id + local, NULL_LINK)

        payload = {}
        for field in self.schema.payload_fields:
            spec = EXTRA_TYPES[field.type]
            values = self._native(records[self._roles[field.name]], spec.numpy_dtype)
            self._reject_non_finite(values, field.name, start, context)
            payload[field.name] = values

        extras = {}
        for extra in self.schema.extra_fields:
            spec = extra.spec
            components = []
            for spelling, component in self._extras[extra.name]:
                column = records[spelling]
                components.append(column if component is None else column[:, component])
            if len(components) != spec.n_components:  # pragma: no cover - the parser guarantees it
                raise ConverterError(
                    "extra field {!r} declares {} source component(s) for a {}-component "
                    "type".format(extra.name, len(components), spec.n_components)
                )
            if spec.n_components == 1:
                values = self._native(components[0], spec.numpy_dtype)
            else:
                values = np.stack(
                    [self._native(part, spec.numpy_dtype) for part in components], axis=1
                )
            self._reject_non_finite(values, extra.name, start, context)
            extras[extra.name] = values

        self._reject_invalid_payload(payload, start, context)
        return {
            "identity": identity,
            "coordinates": coordinates,
            "links": links,
            "payload": payload,
            "extras": extras,
        }

    @staticmethod
    def _native(values: np.ndarray, numpy_dtype: str) -> np.ndarray:
        """Re-encode a source-endian column as a native-endian copy.

        Not a cast: ``build_schema`` has already proved the stored element type
        equals the declared one for every role and every extra, so this only
        changes byte order. ``CanonicalBatch`` compares against native dtypes
        and would otherwise reject a big-endian source outright (see the note
        in ``adapters/base.py``).
        """
        return np.ascontiguousarray(values.astype(numpy_dtype))

    @staticmethod
    def _reject_non_finite(values: np.ndarray, name: str, start: int, context: str) -> None:
        """Refuse NaN/infinity at the adapter's own cast boundary (C2).

        ``CanonicalBatch.validate()`` catches these too, but only by batch-local
        index. Catching them here names the source row, which is what an
        operator needs to find the offending record on disk. Signed zero and
        negative mass sentinels are finite and pass untouched -- C1 keeps
        negative masses as data, not parse errors.
        """
        if values.dtype.kind != "f" or not values.size:
            return
        bad = ~np.isfinite(values)
        if not bool(bad.any()):
            return
        first = tuple(int(axis[0]) for axis in np.asarray(bad).nonzero())
        raise ConverterError(
            "{}, row {}: field {!r} holds the non-finite value {}; NaN and infinity are not "
            "valid payload".format(context, start + first[0], name, values[first])
        )

    @staticmethod
    def _reject_invalid_payload(payload: Dict[str, np.ndarray], start: int, context: str) -> None:
        """Refuse the two payload values v3 declares impossible (C3).

        Rejected at the source row, not repaired: the adapter never edits a
        value to make it conform. ``M_Crit200`` is deliberately *not* checked
        -- a negative mass sentinel is data under the existing core policy.
        """
        for name in ("Len", "SnapNum"):
            values = payload.get(name)
            if values is None or not values.size:
                continue
            if int(values.min()) < 0:
                row = int(np.argmin(values))
                raise ConverterError(
                    "{}, row {}: {} is {}, which the general format does not permit".format(
                        context, start + row, name, int(values[row])
                    )
                )


# ==========================================================================
# Per-tree structural validation
# ==========================================================================


def _rows_of(mask: np.ndarray) -> np.ndarray:
    """The row indices a boolean mask selects, in ascending order."""
    return np.asarray(mask).nonzero()[0]


def _first_row(mask: np.ndarray) -> int:
    """The first row a boolean mask selects. Callers check ``any()`` first."""
    return int(_rows_of(mask)[0])


def _validate_tree(
    columns: Dict[str, np.ndarray], n_halos: int, context: str, max_snapshot: Optional[int]
) -> None:
    """Reject a structurally invalid tree (C1).

    Every rule below was checked against all four shipped L-Halo datasets --
    mini-Millennium (8/8 files), micro-Uchuu (4/4), and four files each of
    Millennium and mini-Uchuu, 75.4 M halos in total -- with zero violations
    before it was made a gate, so none of them fails valid source data.

    A **forward gap is not malformed**: ``Descendant`` must point strictly
    forward, and a span of 2 is as legal as a span of 1. mini-Millennium's
    29,291 gaps are the reason this route exists.
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
    _validate_progenitors(
        descendant, first_progenitor, next_progenitor, snapshot, rows, n_halos, context
    )
    _validate_fof(fof_central, next_in_fof, snapshot, rows, n_halos, context)


def _validate_ranges(columns: Dict[str, np.ndarray], n_halos: int, context: str) -> None:
    """Every link is ``-1`` or a row of this tree.

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
    snapshot: np.ndarray,
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

    ``snapshot`` is still a parameter: the caller passes the whole column set,
    and dropping it here would only move the argument list out of step with
    the other validators.
    """
    del snapshot  # enforced transitively; see the docstring
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
    deepest chain measured across all four datasets is 3,817 (mini-Uchuu FoF)
    against 46 M halos in that sample.

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


class _BatchBuilder:
    """Accumulates chunk columns until ``max_rows`` rows are ready.

    Holds at most ``max_rows`` rows: the caller sizes each chunk against the
    remaining space, so the builder never overshoots and never buffers a whole
    tree.
    """

    _GROUPS = ("identity", "coordinates", "links", "payload", "extras")

    def __init__(self, schema: CanonicalSchema, max_rows: int):
        self.schema = schema
        self.max_rows = max_rows
        self.n_rows = 0
        self._parts: Dict[str, Dict[str, List[np.ndarray]]] = {group: {} for group in self._GROUPS}

    def add(self, columns: Dict[str, Dict[str, np.ndarray]]) -> None:
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
