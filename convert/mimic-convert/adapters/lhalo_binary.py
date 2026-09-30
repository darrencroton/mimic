"""Lossless L-Halo binary source adapter.

Streams the shipped fixed-record L-Halo tree files -- mini-Millennium,
Millennium, mini-Uchuu and micro-Uchuu all ship the same 19-field, 104-byte
record -- into the canonical batches ``adapters/base.py`` defines. It reads
only; it opens no file for writing and creates no path, so the converter's
source-overwrite protection has no write set to enumerate here.

**What "lossless" means on this route.** The adapter preserves what the source
stores and converts nothing:

- ``Len`` is the source's supplied particle count, never re-derived from mass.
- ``M_Crit200`` stays ``float32`` in ``1e10 Msun/h``. A round trip through
  ``Msun/h`` and back would destroy bit parity, so no unit arithmetic of
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

**Identity** follows the vertical reader's own enumeration, which was
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

**Bounds.** Four terms scale with something other than a constant, and
all four are named rather than left implicit:

1. *Record reads* are bounded. Emission reads at most the caller's
   ``max_rows`` records at a time, and the validation pass reads at most
   :data:`TOPOLOGY_READ_CHUNK_ROWS`, independent of ``max_rows``, so neither
   buffer scales with the tree. A tree larger than one batch is read and
   emitted across several batches; a tree is never materialised whole as
   records. Each tree's bytes are therefore read **twice** -- once to gather
   the topology and once to emit -- which is a deliberate trade: validating
   before emitting means a structurally invalid tree can never reach a batch,
   and the second pass hits the page cache the first one just warmed.
2. *Per-tree validation* -- the five links plus ``SnapNum`` as int64, **plus
   the transient scratch the structural checks allocate while those columns
   are still live** -- peaks at ``topology.VALIDATION_BYTES_PER_HALO`` bytes
   per halo for the tree currently being validated. Structural validation
   (reciprocal chains, cycles, FoF membership) is not expressible
   chunk-locally, so this term is deliberate, budgeted, and checked *before*
   allocation. The retained columns are only 48 of those bytes; counting just
   them would understate the real peak by more than half, so the budget uses
   the measured whole-path figure instead, **plus** the one bounded read
   buffer of term 1 and the :data:`TOPOLOGY_BASE_BYTES` fixed addend --
   constants, which a per-halo figure cannot express. The columns are
   **released as soon as validation returns**, before emission begins, so
   this term never overlaps the output buffers.
3. *The emission buffer* holds one batch: up to ``max_rows`` raw records and
   the canonical columns built from them, doubled while
   ``topology.BatchBuilder`` concatenates the accumulated chunks. It is
   checked once, before the first batch, by
   ``topology.check_emission_budget``.
4. *The inventory* is O(tree count), an explicitly budgeted term. It cannot
   be streamed away, because ``SourceHaloID`` is a prefix sum over the complete
   ordered inventory. Budgeted at :data:`INVENTORY_BYTES_PER_UNIT`
   (640) per tree plus a :data:`INVENTORY_BASE_BYTES` (256 KiB) constant,
   measured around the whole ``_build_inventory()`` path rather than around
   ``SourceInventory`` alone -- the path also retains a per-file header whose
   count table is a real int64 array, and at one tree the constant *is* the
   entire peak.

**What the budget does and does not bound.** Each named term above is checked
against the whole configured ``memory_budget_bytes`` ceiling **before its own
allocation**. The ceiling is deliberately *not* a running total: it does not
bound the sum of terms that happen to be resident at the same moment, and it
says nothing about interpreter RSS: ``--memory-budget-mb`` bounds merge/chunk
working buffers, not the entire Python interpreter RSS, and that is what makes
the check answerable before an allocation rather than after it. A caller who
needs a hard ceiling on total process memory needs a different mechanism than
this one.

Sized against the shipped packages, an inventory needs roughly 19 MB for
mini-Millennium's 29,585 trees and 282 MB for micro-Uchuu's 440,651. A full
512-file Millennium comes to about 9.1 GB (extrapolated from the 27,747 trees
per file measured across its 16 local files) and will refuse to build under
the default budget: that refusal is the honest answer, and the operator raises
the budget deliberately.

**Validation is rejection, never repair.** Every structural rule enforced here
was first checked against all four shipped datasets by an independent
hand-built ``numpy`` dtype, with zero violations, so none of them is an
invented gate that real data trips. A valid forward gap is *not* malformed and
survives as an exact source-key edge; mini-Millennium's 29,291 of them are
the acceptance case.
"""

import os
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
    check_budget,
    identity_columns,
    require_integer,
)
from .source_inventory import LHaloHeader, read_lhalo_header
from .topology import (
    TOPOLOGY_COLUMNS,
    BatchBuilder,
    check_emission_budget,
    check_validation_budget,
    validate_tree,
)

__all__ = [
    "ConverterError",
    "TOPOLOGY_COLUMNS",
    "TOPOLOGY_READ_CHUNK_ROWS",
    "TOPOLOGY_COLUMN_BYTES_PER_HALO",
    "TOPOLOGY_BASE_BYTES",
    "INVENTORY_BYTES_PER_UNIT",
    "INVENTORY_BASE_BYTES",
    "DEFAULT_MEMORY_BUDGET_BYTES",
    "LHaloBinaryAdapter",
]


#: Records per read while gathering those columns. Fixed rather than taken
#: from the caller's ``max_rows`` so the raw read buffer stays O(1) in the
#: tree size: this pass fills six int64 columns and gains nothing from a
#: larger buffer, while a caller batching a whole tree at once would
#: otherwise add ``itemsize`` bytes per halo (104 on the shipped record) to
#: the validation peak. 65536 records is 6.8 MB on that record.
TOPOLOGY_READ_CHUNK_ROWS = 65536

#: Bytes the six retained topology columns occupy per halo, held as int64
#: (see ``topology.TOPOLOGY_COLUMNS``). This is **not** the budget figure: see
#: ``topology.VALIDATION_BYTES_PER_HALO``, which also covers the validator's
#: scratch.
TOPOLOGY_COLUMN_BYTES_PER_HALO = 8 * len(TOPOLOGY_COLUMNS)

#: The constant part of the validation path's peak -- array headers, the
#: column dict, the check helpers' small fixed scratch -- which neither the
#: per-halo figure nor the read buffer can express. Measured with
#: ``tracemalloc`` around the complete real path (``_read_tree_topology``
#: through a real file handle, then ``topology.validate_tree``): a peak of
#: 2,597-2,674 bytes at one halo and 3,883 at ten, against 264 and 2,640
#: from the two linear terms, with a worst observed peak of 5,397 bytes (a
#: first-call spike on a seven-halo tree). The linear terms alone cover the
#: peak from about 20 halos up. 16 KiB is that worst peak with a ~3x margin.
#: ``BudgetAccountingTests`` re-measures the real path at small sizes and
#: fails if it exceeds the declared figure.
TOPOLOGY_BASE_BYTES = 16 * 1024

#: Peak bytes per inventory unit of the **complete** ``_build_inventory()``
#: path, which is what the budget check must bound.
#:
#: ``SourceInventory(units)`` alone is not the path: ``_build_inventory``
#: also accumulates a ``_SourceFile`` per file -- each retaining an
#: ``LHaloHeader`` whose ``tree_halo_counts`` is a real int64 array, 8 bytes
#: per tree -- and a ``SourceUnit`` per tree, and holds all of it while
#: ``SourceInventory`` builds its own tuple, prefix-sum tuple and index dict on
#: top. Measuring ``SourceInventory`` alone understates the real 8-file
#: mini-Millennium peak (14,500,184 bytes) by 5.6%, so the figure is measured
#: around the whole path.
#:
#: Measured with ``tracemalloc`` around the real ``inventory()`` call, over
#: synthetic catalogs from 1 to 200,000 trees and on real mini-Millennium.
#: Net of the base term below, the per-unit figure oscillates between roughly
#: 470 and 506 -- it is not smooth in ``n``, because a Python list holds both
#: its old and new storage during a resize, so where ``n`` falls relative to a
#: growth boundary moves the peak by up to 8%. Representative points:
#:
#:     n =   5,000 synthetic                454 B/unit
#:     n =  24,000 synthetic                506 B/unit   <- worst observed
#:     n =  29,585 real mini-Millennium     486 B/unit
#:     n = 200,000 synthetic                470 B/unit
#:
#: 640 is that 506 worst case plus a 1.26x margin, for the same reason
#: ``topology.VALIDATION_BYTES_PER_HALO`` carries one: a budget that refuses work
#: is safe, and one that accepts work it cannot hold is the defect the figure
#: exists to prevent. ``BudgetAccountingTests`` re-measures the real path and
#: fails if it exceeds this constant, so it cannot drift back open silently.
INVENTORY_BYTES_PER_UNIT = 640

#: The constant part of that peak, which no per-unit figure can express.
#:
#: ``read_lhalo_header`` opens the file with default buffering, so CPython
#: allocates the filesystem's block size -- 128 KiB on this host -- for the
#: duration of the read. At one tree that single buffer *is* the whole peak
#: (132,967 bytes measured), which is why folding it into the per-unit
#: constant would need an absurd 133,000 B/unit to stay honest for a small
#: catalog while overstating a large one by 200x. Budgeted as its own named
#: addend instead, exactly as the topology read buffer is. 256 KiB covers the
#: measured 133 KB with room for a host whose block size is larger.
INVENTORY_BASE_BYTES = 256 * 1024

#: Default ceiling for the budgeted terms above. Chosen to hold every
#: inventory the shipped L-Halo sources present -- micro-Uchuu's
#: 440,651 trees need ~282 MB, mini-Millennium's 29,585 need ~19 MB -- while
#: refusing a full 512-file Millennium (~9.1 GB, extrapolated from the 27,747
#: trees per file measured across its 16 local files) loudly instead of
#: paging.
DEFAULT_MEMORY_BUDGET_BYTES = 2 * 1024**3


def _peek_ntrees(path: Path, byte_order: str) -> Optional[int]:
    """Read just the leading ``Ntrees`` field, allocating nothing else.

    Exists so the inventory budget can be checked *before*
    ``read_lhalo_header`` performs its own O(ntrees) count-table read and
    int64 cast. It deliberately diagnoses nothing: a short, unreadable,
    negative or **implausible** header returns ``None`` and the caller
    proceeds to ``read_lhalo_header``, which owns every header error message
    and must stay the single place they are produced. Duplicating validation
    here would mean two sources of truth for the same malformed file.

    "Implausible" carries real weight, and is why the file's size is checked
    here as well as the count. Without it, a corrupt or opposite-endian
    header's absurd ``Ntrees`` reaches the budget check first and the operator
    is told to raise ``memory_budget_bytes`` -- a real mini-Millennium file
    read big-endian asks for 810 GB -- instead of being told the header is
    corrupt or the byte order is wrong. Both paths reject the file, so this is
    diagnosis quality rather than correctness, but a misdirected error message
    sends someone to change the wrong thing. Mirrors ``read_lhalo_header``'s
    own ``8 + 4 * ntrees > file_size`` guard, from a ``stat()`` that allocates
    nothing, so the real defect is named before any count table is read.

    **A failed probe is not the same as an implausible one.** Standing aside
    is right when the header is readable and wrong; it is not right when the
    probe itself could not run, because this function is the only thing that
    budget-checks the file and ``read_lhalo_header`` would then read the whole
    count table unbudgeted -- precisely what this mechanism exists to prevent,
    on precisely the flaky network and FUSE storage where a transient error is
    plausible. So an I/O error is retried once, and a second failure is left
    to propagate: the caller wraps this call and ``read_lhalo_header``
    together, so the error still arrives named with its path and ordinal
    rather than becoming a silent bypass here.
    """
    try:
        file_size, head = _read_header_probe(path)
    except OSError:
        # One retry, for the transient case that has already cleared. A second
        # failure propagates to the caller's wrapper, which names it.
        file_size, head = _read_header_probe(path)
    if len(head) != 4:
        return None
    ntrees = int(np.frombuffer(head, dtype=np.dtype(byte_order + "i4"), count=1)[0])
    if ntrees < 0 or 8 + 4 * ntrees > file_size:
        return None
    return ntrees


def _read_header_probe(path: Path) -> Tuple[int, bytes]:
    """The file's size and its leading 4 bytes, allocating nothing else.

    Unbuffered: a buffered handle would allocate the filesystem's block size
    up front -- 128 KiB on this host -- which is absurd overhead for a 4-byte
    probe whose whole purpose is to allocate nothing before the budget has
    been consulted. A short read is safe here: the caller treats fewer than
    four bytes as "not readable as a header" and defers, rather than
    diagnosing it.
    """
    file_size = path.stat().st_size
    with open(path, "rb", buffering=0) as handle:
        return file_size, handle.read(4)


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
        memory_budget_bytes = require_integer(
            memory_budget_bytes,
            "memory_budget_bytes",
            "a fractional ceiling would be truncated into a different budget than the caller "
            "asked for",
        )
        if memory_budget_bytes <= 0:
            raise ConverterError(
                "memory_budget_bytes must be positive, got {}".format(memory_budget_bytes)
            )
        if max_snapshot is not None:
            max_snapshot = require_integer(
                max_snapshot,
                "max_snapshot",
                "a fractional snapshot bound would be truncated into a different bound than the "
                "a_list declares",
            )
            if max_snapshot < 0:
                raise ConverterError(
                    "max_snapshot must be non-negative, got {}".format(max_snapshot)
                )

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

        # Resolve through column_schema's shared API rather than
        # reimplementing alias matching: the layout's entry names are exactly
        # the source field names a binary profile may reference.
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

        A missing requested file is fatal: silently narrowing a
        conversion to the files that happen to be present would produce a
        smaller dataset that looks complete.

        **A file may appear only once.** Distinct ordinals pointing at the
        same physical file would convert cleanly and emit every one of its
        trees twice, under two different ``SourceHaloID`` ranges -- silently
        doubling the catalog. That is the exact dual of the missing-file case
        this method already refuses: one silently narrows a conversion, the
        other silently widens it, and neither leaves a trace in the output.

        Sameness is decided by **filesystem identity** -- ``(st_dev, st_ino)``
        -- not by comparing path strings. ``Path.resolve()`` unifies a ``..``
        detour and a symlink, but it cannot unify two spellings that differ
        only in case on a case-insensitive volume (APFS, this repository's
        own), where ``trees_063.0`` and ``TREES_063.0`` resolve to different
        strings and the same inode; nor can it unify hard links, which have
        genuinely different names. The inode pair subsumes all four cases at
        once. Two *distinct* files with identical contents still have distinct
        inodes and remain legal: a package may hold byte-identical partitions,
        and refusing those would invent a rule the source does not have.

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
        claimed: Dict[Tuple[int, int], Tuple[int, Path]] = {}
        for position, entry in enumerate(sources):
            try:
                ordinal, path = entry
            except (TypeError, ValueError):
                raise ConverterError(
                    "sources[{}] must be a (source_file_ordinal, path) pair, got {!r}".format(
                        position, entry
                    )
                ) from None
            ordinal = require_integer(
                ordinal,
                "sources[{}]: source_file_ordinal".format(position),
                "it is recorded identity and is never coerced",
            )
            if not 0 <= ordinal <= INT64_MAX:
                # Bounded at both ends: the ordinal is written into an int64
                # coordinate column, so one above INT64_MAX would surface as a
                # raw numpy overflow at emission -- long after the caller could
                # tell which source entry caused it -- instead of the named
                # error every other malformed ordinal shape already gets.
                raise ConverterError(
                    "sources[{}]: source_file_ordinal must be between 0 and {}, got {}".format(
                        position, INT64_MAX, ordinal
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
            try:
                status = os.stat(path)
            except OSError as exc:
                raise ConverterError(
                    "requested source file {} (ordinal {}) cannot be inspected: {}".format(
                        path, ordinal, exc
                    )
                ) from exc
            identity = (status.st_dev, status.st_ino)
            if identity in claimed:
                first_ordinal, first_path = claimed[identity]
                raise ConverterError(
                    "source file {} (ordinal {}) is the same filesystem object as {} (ordinal "
                    "{}); converting it once per ordinal would emit every one of its trees twice "
                    "under different SourceHaloID ranges".format(
                        path, ordinal, first_path, first_ordinal
                    )
                )
            claimed[identity] = (ordinal, path)
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
            # Budget-check against the tree count *before* read_lhalo_header
            # runs, not after. That function reads the whole count table and
            # casts it to int64 unconditionally -- an O(ntrees) allocation of
            # its own -- so checking afterwards would let the allocation the
            # budget exists to gate happen first. The leading 8 bytes already
            # carry Ntrees, which is all the check needs.
            try:
                declared_trees = _peek_ntrees(path, byte_order)
                if declared_trees is not None:
                    check_budget(
                        (len(units) + declared_trees) * INVENTORY_BYTES_PER_UNIT
                        + INVENTORY_BASE_BYTES,
                        self.memory_budget_bytes,
                        "inventory of {} trees ({} B/unit plus a {}-byte base)".format(
                            len(units) + declared_trees,
                            INVENTORY_BYTES_PER_UNIT,
                            INVENTORY_BASE_BYTES,
                        ),
                        "raise memory_budget_bytes, or convert a narrower file range",
                    )
                header = read_lhalo_header(
                    path, byte_order=byte_order, record_bytes=self.layout.itemsize
                )
            except OSError as exc:
                # A source that vanished or became unreadable between
                # _resolve_sources' existence check and this read is a source
                # problem, and leaves here named like every other one.
                raise ConverterError(
                    "cannot read source file {} (ordinal {}): {}".format(path, ordinal, exc)
                ) from exc
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

    # ---- batch streaming -------------------------------------------------

    def iter_batches(self, max_rows: int) -> Iterator[CanonicalBatch]:
        """Stream canonical batches of at most ``max_rows`` rows.

        A tree is validated in full before any of its rows enter a batch, so a
        structurally invalid tree fails before it can be emitted. Batches span
        tree and file boundaries freely: ``SourceHaloID`` ascends across the
        whole inventory, so a batch that ends mid-tree is still strictly
        increasing, and packing them keeps a catalog of 29,585 small trees from
        producing 29,585 tiny batches.

        The emission buffer is budget-checked once, before any tree is read,
        with the record width as the raw per-row term (see
        ``topology.check_emission_budget``).
        """
        max_rows = require_integer(
            max_rows,
            "max_rows",
            "a fractional batch size would be truncated, and True would silently mean 1",
            minimum=1,
        )
        inventory = self.inventory()
        check_emission_budget(
            self.schema,
            max_rows,
            inventory.total_halos,
            self.layout.itemsize,
            self.memory_budget_bytes,
        )
        builder = BatchBuilder(self.schema, max_rows)

        for source in self._files:
            header = source.header
            try:
                handle = open(source.path, "rb")
            except OSError as exc:
                raise ConverterError(
                    "cannot open source file {} (ordinal {}) for conversion: {}".format(
                        source.path, source.ordinal, exc
                    )
                ) from exc
            with handle:
                offset = header.header_bytes
                for tree_ordinal in range(header.ntrees):
                    n_halos = int(header.tree_halo_counts[tree_ordinal])
                    context = "{}: tree {}".format(source.path, tree_ordinal)
                    if n_halos == 0:
                        # A zero-halo tree is a legal inventory unit with no
                        # rows. It consumes a SourceHaloID range of length
                        # zero, which the prefix sums already handle.
                        continue
                    topology = self._read_tree_topology(handle, offset, n_halos, context)
                    validate_tree(topology, n_halos, context, self.max_snapshot)
                    # Released before emission begins, not merely rebound on
                    # the next iteration. Nothing below reads it, and holding
                    # it would keep 48 B/halo of the tree resident through the
                    # whole chunked emission phase -- turning a term that is
                    # supposed to live only for the tree under validation into
                    # one that overlaps the output buffers as well.
                    del topology

                    base_id = inventory.base_id(source.ordinal, tree_ordinal)
                    forest_index = source.forest_base + tree_ordinal
                    self._seek(handle, offset, context)
                    start = 0
                    while start < n_halos:
                        count = min(max_rows - builder.n_rows, n_halos - start)
                        records = self._read_records(handle, count, context)
                        builder.add(
                            self._columns(
                                records, source, tree_ordinal, base_id, forest_index, start, context
                            )
                        )
                        # The chunk's values have been copied out; drop it
                        # before the next read allocates its replacement.
                        records = None
                        start += count
                        if builder.n_rows == max_rows:
                            yield builder.take()
                    offset += n_halos * self.layout.itemsize
        if builder.n_rows:
            yield builder.take()

    @staticmethod
    def _seek(handle, offset: int, context: str) -> None:
        """Seek, reporting a device failure the way every other one is reported.

        A conversion runs for a long time over sources that may live on
        external or network storage, so the handle can fail long after it
        opened successfully. That is still a source problem and leaves here
        named, with the tree that was being read.
        """
        try:
            handle.seek(offset)
        except OSError as exc:
            raise ConverterError(
                "{}: cannot seek to byte {} of the source: {}".format(context, offset, exc)
            ) from exc

    def _read_records(self, handle, count: int, context: str) -> np.ndarray:
        """Read exactly ``count`` records from the current file position."""
        want = count * self.layout.itemsize
        try:
            raw = handle.read(want)
        except OSError as exc:
            raise ConverterError(
                "{}: reading {} bytes from the source failed: {}".format(context, want, exc)
            ) from exc
        if len(raw) != want:
            raise ConverterError(
                "{}: truncated halo payload (need {} bytes for {} records, got {})".format(
                    context, want, count, len(raw)
                )
            )
        return np.frombuffer(raw, dtype=self._record_dtype, count=count)

    def _read_tree_topology(
        self, handle, offset: int, n_halos: int, context: str
    ) -> Dict[str, np.ndarray]:
        """Gather one tree's link and snapshot columns, reading in chunks.

        The columns are whole-tree; the *reads* are not. This is the one
        deliberate whole-tree term, and it is budget-checked before the
        allocation rather than after it.

        The check covers the *whole* validation path, not just the six
        columns allocated here: ``validate_tree`` runs while they are live
        and allocates more whole-tree scratch of its own, so budgeting the
        columns alone would let an operator's configured ceiling be exceeded
        by more than 2x a few lines later. :data:`TOPOLOGY_BASE_BYTES` covers
        the path's fixed peak, which dominates a tree of a few halos.

        The read chunk is :data:`TOPOLOGY_READ_CHUNK_ROWS`, deliberately not
        the caller's ``max_rows``. This pass only fills six columns and gains
        nothing from a larger buffer, whereas taking ``max_rows`` would make
        the raw read ``itemsize`` bytes per halo -- 104 on the shipped record,
        more than double the columns themselves -- whenever a caller sized its
        batches at or above the tree. That would put an O(n_halos) term in the
        peak that this constant is not meant to cover.
        """
        read_buffer_bytes = min(TOPOLOGY_READ_CHUNK_ROWS, n_halos) * self.layout.itemsize
        check_validation_budget(
            n_halos, read_buffer_bytes, TOPOLOGY_BASE_BYTES, self.memory_budget_bytes, context
        )
        columns = {name: np.empty(n_halos, dtype=np.int64) for name in TOPOLOGY_COLUMNS}
        self._seek(handle, offset, context)
        start = 0
        records = None
        while start < n_halos:
            count = min(TOPOLOGY_READ_CHUNK_ROWS, n_halos - start)
            # Released before the next read allocates, not after: rebinding
            # alone would hold two chunk buffers at once, a 13.6 MB constant
            # rather than the intended 6.8 MB.
            records = None
            records = self._read_records(handle, count, context)
            for name in TOPOLOGY_COLUMNS:
                columns[name][start : start + count] = records[self._roles[name]]
            start += count
        del records
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
        identity, coordinates = identity_columns(
            base_id, forest_index, source.ordinal, tree_ordinal, start, records.shape[0]
        )
        # A stored link is a within-tree row index; the canonical form is the
        # target's SourceHaloID, so the whole tree's ids shift by one base.
        # Only -1 is null, and validate_tree has already refused anything
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
        """Refuse NaN/infinity at the adapter's own cast boundary.

        ``CanonicalBatch.validate()`` catches these too, but only by batch-local
        index. Catching them here names the source row, which is what an
        operator needs to find the offending record on disk. Signed zero and
        negative mass sentinels are finite and pass untouched -- negative
        masses are data, not parse errors.
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
        """Refuse the two payload values v3 declares impossible.

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
