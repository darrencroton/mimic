"""Consistent-Trees forests-HDF5 source adapter (contracts C1/C3/C4 of
docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

Streams the uchuutools "forests-HDF5" packaging of Consistent-Trees output into
the canonical batches ``adapters/base.py`` defines, without an ASCII
intermediate. It reads only: every HDF5 file is opened ``"r"`` and no path is
created, so there is no write set to enumerate.

**Two organisations, one reader.** Both shipped layouts put ``Nfiles`` on the
root and one ``File<N>`` group per input file, each holding a ``ForestInfo``
compound table and a ``Forests/`` group of one-dimensional per-halo datasets.
They differ only in where ``File<N>`` physically lives:

- *full Uchuu* (``simulations/uchuu``): ``File<N>`` is an HDF5 ``ExternalLink``
  from the small info file to ``/`` of a sibling ``mergertree_<N>.h5`` data
  file. The real micro-Uchuu dataset is organised the same way.
- *micro-Uchuu fixture* (``simulations/micro-uchuu-hdf5/_tests``): ``File<N>``
  is an ordinary group inside the one file.

The adapter walks both identically, hop by hop, so neither is special-cased.

**Conventions follow the vertical C reader exactly**
(``src/io/vertical/read_ctrees_hdf5.c`` and the shared
``apply_ctrees_value_conventions`` in ``read_ctrees_ascii.c``), in the same
order, and are the ctrees ones -- never the L-Halo ones:

1. ``Mvir``, ``x/y/z``, ``vx/vy/vz``, ``vrms``, ``vmax`` and ``Jx/Jy/Jz`` are
   stored float64 and are narrowed to float32 on read, exactly as the C
   reader's assignment into ``struct halo_data``'s ``float`` members does.
2. ``Spin`` is then the float32 J multiplied -- not divided -- by the float64
   reciprocal of the float32 mass, and narrowed back to float32:
   ``(float)((double)J_f32 * (1.0 / (double)Mvir_f32))``. A zero mass leaves
   the float32 J unnormalised, as the C reader does.
3. ``Len`` is ``round((double)Mvir_f32 * 1e-10 / particle_mass)`` with C's
   round-half-away-from-zero, evaluated left to right. A value that is not
   finite, is negative or exceeds ``INT_MAX`` is fatal in the C reader, and is
   rejected here with the source row named.
4. ``M_Crit200`` is the float32 ``Mvir`` in native ``Msun/h``; no unit
   arithmetic is applied (the payload declaration in ``column_schema`` says
   so, and the C reader leaves it to generated accessors).
5. ``MostBoundID`` is the stored ``id``, carried as signed int64 data.
6. ``SnapNum`` comes from ``Snap_num`` or ``Snap_idx``, integer or integral
   float, checked exactly as ``CT_ASSIGN_SNAP_INT``/``CT_ASSIGN_SNAP_DOUBLE``
   check it: finite, ``floor(v) == v``, in ``[0, INT_MAX]`` and, when an a_list
   bound is supplied, at most ``max_snapshot``.

The five merger links are stored int64 **forest-local** row indices and are
checked against ``[-1, nhalos)`` of their own forest, as ``CT_ASSIGN_LINK``
checks them, and then against every structural rule the L-Halo adapter
enforces (see below). A stored link is re-expressed as its target's
``SourceHaloID``; its value and its chain order are never re-derived.

**Identity** follows the C reader's enumeration (C1): ``ForestIndex`` is the
file-prefix ``ForestInfo`` row number -- the cumulative ``Nforests`` over the
preceding requested files plus the row, which is exactly how
``prepare_run_ctrees_hdf5_state`` builds ``first_forest_in_file`` and
``stage_range_ctrees_hdf5`` maps a forest to ``(file, row)``.
``HaloRankInForest`` is the original within-forest row index. The coordinate is
``(N, ForestInfo row, row)`` where ``N`` is the source's own ``File<N>`` number,
never a compacted position. The sidecar ``ForestID`` is the source's own
``ForestInfo.ForestID`` (see :meth:`CTreesHDF5Adapter.iter_forests`).

**Dependencies are pinned, never assumed (C1, C4).** Every object the
conversion reads is reached through an explicit walk that records the physical
file backing it, and the whole set is pinned by
``source_inventory.pin_source_file`` before any row is accepted. Along the way:

- an ``ExternalLink`` whose target is not present *beside the file that
  declares it* fails before HDF5 is asked to dereference it. HDF5 would
  otherwise fall back to ``HDF5_EXT_PREFIX`` or the current directory and
  could open a different file of the same name; if it resolves somewhere other
  than the declared location anyway, that fails too;
- a virtual dataset (VDS) fails outright. No reachable source uses one (C1:
  "No VDS requirement"), and a VDS whose source files are absent reads as fill
  values -- precisely the plausible-looking substitute C1 forbids;
- a dataset whose raw data lives in HDF5 "external storage" files fails for
  the same reason, and so does a contiguous or chunked dataset whose storage
  was never allocated, which HDF5 would also silently read as fill values;
- soft and user-defined links on the read path fail: neither shipped
  organisation uses one, and a soft link can route through an external file
  that the walk would then not see as a hop.

``iter_batches`` re-pins every dependency before streaming and fails if any
file's size, modification time or inode has changed since inventory.

**Bounds (C4).** All offsets, counts, row indices and remapping keys are int64
throughout; nothing is narrowed to int32. Four terms scale with something
other than a constant, and all four are named:

1. *Dataset reads* are bounded. When a file's ``ForestInfo`` lists its
   forests in storage order -- every real source does -- consecutive forests
   are read together through one *read window* of at most
   :data:`TOPOLOGY_READ_CHUNK_ROWS` rows per selected dataset, and every
   forest wholly inside it is validated and emitted from those arrays, so
   each dataset is read once per window instead of twice per forest. The
   window's rows are sized from the budget (see ``_window_rows``). A forest
   larger than the window, and every forest of a file not in storage order,
   is read per forest instead: the topology pass reads at most
   :data:`TOPOLOGY_READ_CHUNK_ROWS` rows and emission at most the caller's
   ``max_rows`` rows of each dataset at a time, so a forest larger than one
   batch is read and emitted across several. No forest's payload is ever
   materialised whole beyond the window; the C reader's fallback of reading a
   super-forest into one whole-forest buffer is deliberately not reproduced.
2. *Per-forest structural validation* holds the forest's five links plus
   ``SnapNum`` as int64, and the validator's scratch, while it runs:
   ``topology.VALIDATION_BYTES_PER_HALO`` per halo (re-measured on this path
   at 119.0 B/halo, the same worst case as the L-Halo path) plus the bounded
   read buffer -- the read window, or the per-forest topology chunk -- and the
   :data:`TOPOLOGY_BASE_BYTES` constant. It is the one whole-forest term,
   checked against the budget **before** allocation and released before
   emission begins. A forest too large for the configured budget is refused,
   never allocated.
3. *The emission buffer* holds one batch: the raw values read for up to
   ``max_rows`` rows and the canonical columns built from them, doubled while
   ``topology.BatchBuilder`` concatenates the accumulated chunks. It is
   checked once, before the first batch, by ``topology.check_emission_budget``.
4. *The inventory* is O(forest count), the explicitly budgeted term C4
   permits, checked per file before that file's ``ForestInfo`` is read.

Each term is checked against the whole ``memory_budget_bytes`` ceiling before
its own allocation, as in the L-Halo adapter; it bounds working buffers, not
interpreter RSS or HDF5's own library caches (C4).

**Deliberate differences from the C reader.** Each of these is a tightening:

- ``particle_mass`` must be positive. The C reader silently sets ``Len = 0``
  when ``PartMass <= 0``; a lossless converter refuses to invent a count.
- ``ForestInfo`` must tile its datasets exactly -- the non-empty forests
  partition ``[0, extent)`` with no overlap or gap, and the counts sum to the
  extent. The C reader checks only that each slab lies inside the extent,
  which would let two overlapping forests emit the same halos twice.
- Two ``File<N>`` (or ``Forests``) groups resolving to the same HDF5 object
  fail, for the same reason.
- Optional ``Nhalos`` (per file) and ``TotNforests``/``TotNhalos`` (root, when
  the whole file set is requested) attributes are cross-checked when present.
- Both snapshot spellings present in one file is ambiguous and fails
  (C2: exactly one alias per role per file); the C reader silently prefers
  ``Snap_num``.
- Every required dataset must be exactly little-endian int64 or float64 of
  the class its role needs. The C reader reads raw bytes with the file type
  as the memory type and reinterprets them as ``double``/``int64_t``, so a
  big-endian or differently-typed dataset has no correct reference
  interpretation to reproduce; it fails instead of being guessed.
- A non-finite payload value, a finite value that overflows float32 on
  narrowing, and a ``Spin`` whose ``J / Mvir`` product overflows float32 are
  all rejected with the source row named. The C reader's only finiteness
  checks are on the snapshot and the derived ``Len``; it would silently store
  the others as NaN or +/-inf.

One difference is a loosening: the snapshot spelling is resolved **per
file**. The C reader detects ``Snap_num``/``Snap_idx`` once, on the first
requested file, and reuses that name for every later one, so a file set that
mixes the two spellings fails there and converts here.

**Not reproduced**, because they are properties of the compiled runtime rather
than of the source: the per-forest ``nhalos < INT_MAX`` limit (v3 links are
int64 by design and this route must not inherit it), the
``UniqueGalaxyIDMultiplier`` bound, and the cosmology agreement against the
compiled simulation package.
"""

import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple

import numpy as np
from column_schema import (
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
from .source_inventory import MissingDependencyError, SourceFileIdentity, pin_source_file
from .topology import (
    TOPOLOGY_COLUMNS,
    VALIDATION_BYTES_PER_HALO,
    BatchBuilder,
    check_emission_budget,
    check_validation_budget,
    validate_tree,
)

try:
    import h5py
except ImportError:  # pragma: no cover - exercised via MissingDependencyError
    h5py = None

__all__ = [
    "ConverterError",
    "MissingDependencyError",
    "TOPOLOGY_COLUMNS",
    "TOPOLOGY_READ_CHUNK_ROWS",
    "TOPOLOGY_READ_BUFFER_BYTES_PER_ROW",
    "TOPOLOGY_BASE_BYTES",
    "INVENTORY_BYTES_PER_UNIT",
    "INVENTORY_BASE_BYTES",
    "DEFAULT_MEMORY_BUDGET_BYTES",
    "LEN_MASS_SCALE",
    "ForestRecord",
    "HDF5Dependency",
    "ctrees_payload",
    "convert_snapshots",
    "validate_forest_table",
    "CTreesHDF5Adapter",
]

INT32_MAX = int(np.iinfo(np.int32).max)

#: The factor the C reader applies to the native ``Mvir`` (Msun/h) before
#: dividing by the particle mass (1e10 Msun/h) to estimate ``Len``.
LEN_MASS_SCALE = 1e-10

#: Rows per dataset read while gathering the topology columns, and the largest
#: read window. Fixed rather than taken from ``max_rows`` so the transient read
#: buffer stays O(1) in the forest size, exactly as in the L-Halo adapter.
TOPOLOGY_READ_CHUNK_ROWS = 65536

#: Transient bytes per row of one topology read chunk, beyond the retained
#: columns: the slice h5py returns for one dataset plus the snapshot check's
#: temporaries while that slice is live. Measured with ``tracemalloc`` around
#: the real ``_read_forest_topology`` on chain-shaped forests of 20,000 to
#: 400,000 halos: 18.08 B/row on an integral-float snapshot column (the float
#: path's ``floor`` copy and masks make it the worst case) and 17.08 on an
#: integer one, flat in n. 32 is that plus a ~1.8x margin.
#: ``BudgetAccountingTests`` re-measures it.
TOPOLOGY_READ_BUFFER_BYTES_PER_ROW = 32

#: The constant part of the topology path's peak -- array headers, the column
#: dict, and the validator's small fixed scratch -- which neither per-halo nor
#: per-row figure can express. Measured at 6.4-7.4 KB for forests of 1-10
#: halos and a residual of 14.0 KB at 100 halos, above the linear terms;
#: 64 KiB covers that with more than 4x room. Without it a one-halo forest's
#: budget would be declared as 192 bytes against a measured 7.5 KB. The read
#: window path is measured against the same constant.
TOPOLOGY_BASE_BYTES = 64 * 1024

#: Peak bytes per forest of the complete ``_build_inventory()`` path -- the
#: ``SourceUnit`` per forest, ``SourceInventory``'s tuple/prefix/index
#: structures, and the three retained int64 ``ForestInfo`` columns plus the
#: transient compound read. Measured with ``tracemalloc`` around the real
#: ``inventory()`` call, net of the one-forest base below: between 428 and
#: 528 B/unit over synthetic catalogs of 1,000 to 200,000 forests (worst at
#: 24,000, where list growth falls unfavourably, as the L-Halo adapter also
#: found), and 479 B/unit on real micro-Uchuu's 440,651 forests (211,064,650
#: bytes). 704 is the 528 worst case plus a ~1.33x margin.
#: ``BudgetAccountingTests`` re-measures the real path and fails if it exceeds
#: this constant.
INVENTORY_BYTES_PER_UNIT = 704

#: The constant part of the inventory peak (HDF5/h5py handles, attribute
#: reads, schema resolution), which no per-unit figure can express. Measured
#: at 30,389 bytes for a one-forest source; 256 KiB is ~8x that.
INVENTORY_BASE_BYTES = 256 * 1024

#: Default ceiling for the budgeted terms. Holds real micro-Uchuu's 440,651-
#: forest inventory (~310 MB) and its largest forest (350,075 halos, ~56 MB of
#: validation) with ample room, while refusing an absurd request loudly
#: instead of paging.
DEFAULT_MEMORY_BUDGET_BYTES = 2 * 1024**3

#: Role -> the exact stored dtype(s) the C reader reads correctly. The C
#: reader requires an 8-byte element for every field and reads the bytes as
#: ``int64_t`` or ``double``; see the module docstring for why anything else
#: fails rather than being converted.
_INT64_LE = np.dtype("<i8")
_FLOAT64_LE = np.dtype("<f8")
_FLOAT_ROLES = ("Mvir", "x", "y", "z", "vx", "vy", "vz", "Jx", "Jy", "Jz", "vrms", "vmax")
_ROLE_DTYPES: Dict[str, Tuple[np.dtype, ...]] = {
    **{role: (_INT64_LE,) for role in LINK_FIELDS},
    **{role: (_FLOAT64_LE,) for role in _FLOAT_ROLES},
    "id": (_INT64_LE,),
    "snap": (_INT64_LE, _FLOAT64_LE),
}

#: The ctrees payload this adapter computes, by name. ``column_schema``
#: declares the same nine for ``consistent_trees_hdf5``; the constructor checks
#: that the two agree rather than trusting it.
_PAYLOAD_NAMES = frozenset(
    ("Len", "SnapNum", "M_Crit200", "Pos", "Vel", "Spin", "VelDisp", "Vmax", "MostBoundID")
)

#: ``ForestInfo`` members the adapter reads, and the record size the C reader
#: requires (``struct ctrees_forestinfo``: four int64).
_FOREST_INFO_FIELDS = ("ForestID", "ForestHalosOffset", "ForestNhalos")
_FOREST_INFO_ITEMSIZE = 32

#: Errors h5py raises for an unreadable, dangling or malformed object. Named
#: rather than a bare ``Exception``: this is a read path, but a programming
#: error inside the adapter should still surface as itself.
_H5_ERRORS = (KeyError, OSError, RuntimeError, ValueError, TypeError)


# ==========================================================================
# Public records
# ==========================================================================


@dataclass(frozen=True)
class ForestRecord:
    """One inventory forest, in emission order, for the ``forests.h5`` sidecar."""

    forest_index: int
    forest_id: int
    source_file_ordinal: int
    unit_ordinal: int
    n_halos: int


@dataclass(frozen=True)
class HDF5Dependency:
    """One physical file the conversion reads, and the objects read from it.

    ``objects`` are logical paths from the info file's root (``/File0``,
    ``/File0/Forests/Mvir``), so a reader of the provenance can see which
    object each file backs, not merely that the file was touched.
    """

    identity: SourceFileIdentity
    objects: Tuple[str, ...]


# ==========================================================================
# Pure conversion helpers (independent of any open file)
# ==========================================================================


def validate_forest_table(
    offsets: np.ndarray, counts: np.ndarray, halo_extent: int, context: str
) -> None:
    """Reject a ``ForestInfo`` table that does not tile its datasets exactly.

    Checks what the C reader checks per forest -- non-negative count and
    offset, slab inside the extent -- and then the partition property it does
    not: the non-empty forests cover ``[0, halo_extent)`` exactly once, in any
    row order. All arithmetic is int64; nothing here assumes a forest or an
    offset fits int32.
    """
    offsets = np.asarray(offsets, dtype=np.int64)
    counts = np.asarray(counts, dtype=np.int64)
    if offsets.shape != counts.shape or offsets.ndim != 1:
        raise ConverterError(
            "{}: ForestInfo offset/count columns disagree in shape".format(context)
        )
    if halo_extent < 0:  # pragma: no cover - a dataset extent is never negative
        raise ConverterError("{}: negative dataset extent {}".format(context, halo_extent))
    negative = counts < 0
    if bool(negative.any()):
        row = int(np.flatnonzero(negative)[0])
        raise ConverterError(
            "{}: ForestInfo row {} has a negative ForestNhalos ({})".format(
                context, row, int(counts[row])
            )
        )
    negative = offsets < 0
    if bool(negative.any()):
        row = int(np.flatnonzero(negative)[0])
        raise ConverterError(
            "{}: ForestInfo row {} has a negative ForestHalosOffset ({})".format(
                context, row, int(offsets[row])
            )
        )
    # offset <= extent and count <= extent - offset, the C reader's own
    # overflow-free form of "the slab lies inside the dataset".
    outside = (offsets > halo_extent) | (counts > halo_extent - np.minimum(offsets, halo_extent))
    if bool(outside.any()):
        row = int(np.flatnonzero(outside)[0])
        raise ConverterError(
            "{}: ForestInfo row {} requests halo slab [{}, {}+{}) but the Forests datasets have "
            "{} rows".format(
                context, row, int(offsets[row]), int(offsets[row]), int(counts[row]), halo_extent
            )
        )
    # Summed as Python integers, so the total is exact however many forests
    # there are -- an int64 accumulator could wrap before the comparison.
    total = sum(counts.tolist())
    if total != halo_extent:
        raise ConverterError(
            "{}: ForestInfo's ForestNhalos sum to {} but the Forests datasets have {} rows".format(
                context, total, halo_extent
            )
        )
    nonempty = np.flatnonzero(counts > 0)
    if nonempty.size:
        order = nonempty[np.argsort(offsets[nonempty], kind="stable")]
        starts = offsets[order]
        ends = starts + counts[order]
        if int(starts[0]) != 0:
            raise ConverterError(
                "{}: no ForestInfo forest starts at row 0 of the Forests datasets (the first "
                "starts at {})".format(context, int(starts[0]))
            )
        broken = ends[:-1] != starts[1:]
        if bool(broken.any()):
            position = int(np.flatnonzero(broken)[0])
            raise ConverterError(
                "{}: ForestInfo rows {} and {} leave a gap or overlap at Forests row {} (one ends "
                "at {}, the next starts at {}); forests must tile the datasets exactly".format(
                    context,
                    int(order[position]),
                    int(order[position + 1]),
                    int(ends[position]),
                    int(ends[position]),
                    int(starts[position + 1]),
                )
            )


def convert_snapshots(
    raw: np.ndarray, max_snapshot: Optional[int], context: str, first_row: int
) -> np.ndarray:
    """Check and convert a slice of the snapshot column to int64.

    Mirrors ``CT_ASSIGN_SNAP_INT`` and ``CT_ASSIGN_SNAP_DOUBLE``: an integer
    column must lie in ``[0, INT_MAX]``; a float column must also be finite
    and exactly integral (``floor(v) == v``, never a tolerance). When an a_list
    bound is supplied it caps both, as ``LastSnapshotNr`` does. ``first_row``
    is the forest-local row of ``raw[0]``, so the error names the source row.
    """
    upper = INT32_MAX if max_snapshot is None else min(INT32_MAX, max_snapshot)
    if raw.dtype.kind == "f":
        with np.errstate(invalid="ignore"):
            bad = ~np.isfinite(raw) | (np.floor(raw) != raw) | (raw < 0) | (raw > upper)
        if bool(bad.any()):
            row = int(np.flatnonzero(bad)[0])
            raise ConverterError(
                "{}, row {}: snapshot value {!r} is not an integer in [0, {}]".format(
                    context, first_row + row, float(raw[row]), upper
                )
            )
        return raw.astype(np.int64)
    if raw.dtype.kind != "i":  # pragma: no cover - dtype is checked at inventory
        raise ConverterError("{}: snapshot column has dtype {}".format(context, raw.dtype))
    bad = (raw < 0) | (raw > upper)
    if bool(bad.any()):
        row = int(np.flatnonzero(bad)[0])
        raise ConverterError(
            "{}, row {}: snapshot value {} is outside [0, {}]".format(
                context, first_row + row, int(raw[row]), upper
            )
        )
    return raw.astype(np.int64)


def _narrow_to_float32(values: np.ndarray, role: str, context: str, first_row: int) -> np.ndarray:
    """The C reader's ``double`` -> ``float`` assignment, refusing what it cannot hold.

    A non-finite source value and a finite value beyond float32's range are
    both rejected with the source row named. Finite underflow is the IEEE
    round-to-nearest cast C performs, and signed zero survives.
    """
    finite = np.isfinite(values)
    if not bool(finite.all()):
        row = int(np.flatnonzero(~finite)[0])
        raise ConverterError(
            "{}, row {}: {} holds the non-finite value {!r}; NaN and infinity are not valid "
            "payload".format(context, first_row + row, role, float(values[row]))
        )
    with np.errstate(over="ignore"):
        narrowed = values.astype(np.float32)
    overflow = ~np.isfinite(narrowed)
    if bool(overflow.any()):
        row = int(np.flatnonzero(overflow)[0])
        raise ConverterError(
            "{}, row {}: {} value {!r} overflows float32".format(
                context, first_row + row, role, float(values[row])
            )
        )
    return narrowed


def _round_half_away_from_zero(values: np.ndarray) -> np.ndarray:
    """C ``round()`` for non-negative finite doubles, exactly.

    ``floor(v + 0.5)`` is not exact -- it rounds 0.49999999999999994 up -- and
    ``np.rint`` rounds half to even. ``v - floor(v)`` is exact for any double
    below 2**52, which every value reaching here is (at most ``INT_MAX``).
    """
    floor = np.floor(values)
    return floor + ((values - floor) >= 0.5)


def ctrees_payload(
    raw: Dict[str, np.ndarray],
    particle_mass: float,
    snapshots: np.ndarray,
    context: str,
    first_row: int,
) -> Dict[str, np.ndarray]:
    """Apply the ctrees value conventions to one chunk of stored columns.

    ``raw`` holds the float64 value roles and the int64 ``id`` by role name;
    ``snapshots`` is the already-checked int64 snapshot column. The order is
    the C reader's (see the module docstring): narrow to float32, normalise
    Spin on the float32 mass, derive Len from the float32 mass. Every returned
    array owns its memory, so a batch never keeps a caller's read buffer
    alive.
    """
    mass = _narrow_to_float32(raw["Mvir"], "Mvir", context, first_row)
    position = np.stack(
        [_narrow_to_float32(raw[role], role, context, first_row) for role in ("x", "y", "z")],
        axis=1,
    )
    velocity = np.stack(
        [_narrow_to_float32(raw[role], role, context, first_row) for role in ("vx", "vy", "vz")],
        axis=1,
    )
    angular = np.stack(
        [_narrow_to_float32(raw[role], role, context, first_row) for role in ("Jx", "Jy", "Jz")],
        axis=1,
    )
    dispersion = _narrow_to_float32(raw["vrms"], "vrms", context, first_row)
    vmax = _narrow_to_float32(raw["vmax"], "vmax", context, first_row)

    mass64 = mass.astype(np.float64)
    nonzero = mass != 0.0
    reciprocal = np.ones_like(mass64)
    reciprocal[nonzero] = 1.0 / mass64[nonzero]
    with np.errstate(over="ignore"):
        normalised = (angular.astype(np.float64) * reciprocal[:, None]).astype(np.float32)
    spin = np.where(nonzero[:, None], normalised, angular)
    bad = ~np.isfinite(spin)
    if bool(bad.any()):
        row = int(np.flatnonzero(bad.any(axis=1))[0])
        raise ConverterError(
            "{}, row {}: Spin = J / Mvir overflows float32 (J = {}, Mvir = {!r})".format(
                context, first_row + row, angular[row].tolist(), float(mass[row])
            )
        )

    particles = mass64 * LEN_MASS_SCALE / particle_mass
    bad = ~np.isfinite(particles) | (particles < 0.0) | (particles > float(INT32_MAX))
    if bool(bad.any()):
        row = int(np.flatnonzero(bad)[0])
        raise ConverterError(
            "{}, row {}: derived particle count {!r} (Mvir = {!r}, particle_mass = {!r}) is not "
            "in [0, {}]; the vertical reader treats this as fatal".format(
                context,
                first_row + row,
                float(particles[row]),
                float(mass[row]),
                particle_mass,
                INT32_MAX,
            )
        )
    length = _round_half_away_from_zero(particles).astype(np.int32)

    return {
        "Len": length,
        "SnapNum": snapshots.astype(np.int32),
        "M_Crit200": mass,
        "Pos": position,
        "Vel": velocity,
        "Spin": spin,
        "VelDisp": dispersion,
        "Vmax": vmax,
        "MostBoundID": np.array(raw["id"], dtype=np.int64),
    }


# ==========================================================================
# Dependency walk
# ==========================================================================


class _DependencyWalk:
    """Records which physical file backs each object the conversion reads."""

    def __init__(self):
        self._objects: Dict[str, List[str]] = {}

    def record(self, filename: str, logical: str) -> None:
        resolved = os.path.realpath(filename)
        self._objects.setdefault(resolved, []).append(logical)

    @property
    def paths(self) -> Tuple[str, ...]:
        return tuple(sorted(self._objects))

    def pin(self) -> Tuple[HDF5Dependency, ...]:
        return tuple(
            HDF5Dependency(identity=pin_source_file(path), objects=tuple(self._objects[path]))
            for path in self.paths
        )


def _open_member(parent, name: str, logical: str, walk: _DependencyWalk):
    """Dereference one hop of the read path, checking its link first.

    ``logical`` is the path from the info file's root, used for provenance
    and error messages. The link is inspected *before* it is followed, so an
    external target that is missing where it is declared fails without HDF5
    ever searching elsewhere for a file of the same name.
    """
    try:
        link = parent.get(name, getlink=True)
    except _H5_ERRORS as exc:
        raise ConverterError("{}: cannot inspect the link: {}".format(logical, exc)) from exc
    if link is None:
        raise ConverterError("{}: does not exist".format(logical))

    expected_target: Optional[Path] = None
    if isinstance(link, h5py.ExternalLink):
        declared = Path(link.filename)
        linking_file = Path(parent.file.filename)
        expected_target = declared if declared.is_absolute() else linking_file.parent / declared
        if not expected_target.is_file():
            raise ConverterError(
                "{}: external link to {!r} is unresolved -- {} is absent; a missing backing "
                "file fails before any row is accepted".format(
                    logical, link.filename, expected_target
                )
            )
    elif isinstance(link, h5py.SoftLink):
        raise ConverterError(
            "{}: is a soft link to {!r}; no supported forests-HDF5 organisation uses one, and "
            "its target's physical backing would not be visible to the dependency walk".format(
                logical, link.path
            )
        )
    elif not isinstance(link, h5py.HardLink):
        raise ConverterError(
            "{}: is a {} link, which is not supported".format(logical, type(link).__name__)
        )

    try:
        obj = parent[name]
        filename = obj.file.filename
    except _H5_ERRORS as exc:
        raise ConverterError("{}: cannot be opened: {}".format(logical, exc)) from exc
    if expected_target is not None:
        try:
            same = os.path.samefile(expected_target, filename)
        except OSError as exc:
            raise ConverterError(
                "{}: cannot compare the resolved file {} with {}: {}".format(
                    logical, filename, expected_target, exc
                )
            ) from exc
        if not same:
            raise ConverterError(
                "{}: external link resolved to {}, not the declared {}; refusing a file HDF5 "
                "found elsewhere".format(logical, filename, expected_target)
            )
    walk.record(filename, logical)
    return obj


def _check_storage(dataset, logical: str) -> None:
    """Refuse a dataset whose values would come from anywhere but stored data.

    Virtual datasets, raw "external storage" and never-allocated storage all
    read as fill values without error when their data is absent, which is the
    plausible-looking substitute C1 forbids.
    """
    try:
        is_virtual = bool(dataset.is_virtual)
        plist = dataset.id.get_create_plist()
        layout = plist.get_layout()
        external = plist.get_external_count()
    except _H5_ERRORS as exc:
        raise ConverterError("{}: cannot inspect storage: {}".format(logical, exc)) from exc
    if is_virtual or layout == h5py.h5d.VIRTUAL:
        raise ConverterError(
            "{}: is a virtual dataset; absent virtual source data would read as fill values, and "
            "no supported forests-HDF5 organisation uses VDS".format(logical)
        )
    if external:
        raise ConverterError(
            "{}: stores its raw data in {} HDF5 external file(s), which are not supported".format(
                logical, external
            )
        )
    if not dataset.size:
        return
    if layout == h5py.h5d.CONTIGUOUS:
        needed = int(dataset.size) * int(dataset.dtype.itemsize)
        stored = int(dataset.id.get_storage_size())
        if stored < needed:
            raise ConverterError(
                "{}: only {} of {} bytes of storage are allocated; unwritten data would read as "
                "fill values".format(logical, stored, needed)
            )
    elif layout == h5py.h5d.CHUNKED:
        expected = 1
        for extent, chunk in zip(dataset.shape, dataset.chunks):
            expected *= -(-int(extent) // int(chunk))
        allocated = int(dataset.id.get_num_chunks())
        if allocated != expected:
            raise ConverterError(
                "{}: only {} of {} chunks are allocated; unwritten chunks would read as fill "
                "values".format(logical, allocated, expected)
            )
    elif layout != h5py.h5d.COMPACT:  # pragma: no cover - HDF5 has no fifth layout
        raise ConverterError("{}: unsupported storage layout {}".format(logical, layout))


def _int_attribute(obj, name: str, logical: str, itemsize: int = 8, required: bool = True):
    """Read one scalar integer attribute, checked the way the C reader sizes it."""
    try:
        present = name in obj.attrs
        value = obj.attrs[name] if present else None
    except _H5_ERRORS as exc:
        raise ConverterError(
            "{}: cannot read attribute {!r}: {}".format(logical, name, exc)
        ) from exc
    if not present:
        if required:
            raise ConverterError("{}: missing required attribute {!r}".format(logical, name))
        return None
    array = np.asarray(value)
    if (
        array.shape not in ((), (1,))
        or array.dtype.kind not in "iub"
        or array.dtype.itemsize != itemsize
    ):
        raise ConverterError(
            "{}: attribute {!r} must be a scalar {}-byte integer, got {!r} ({})".format(
                logical, name, itemsize, value, array.dtype
            )
        )
    if array.dtype.kind == "u" and itemsize == 8 and int(array.reshape(-1)[0]) > INT64_MAX:
        raise ConverterError("{}: attribute {!r} exceeds int64".format(logical, name))
    return int(array.reshape(-1)[0])


# ==========================================================================
# The adapter
# ==========================================================================


@dataclass
class _FilePlan:
    """Everything inventory learned about one ``File<N>`` group."""

    ordinal: int
    forest_base: int
    halo_extent: int
    offsets: np.ndarray
    counts: np.ndarray
    forest_ids: np.ndarray
    roles: Dict[str, str]
    extras: Dict[str, Tuple[Tuple[str, Optional[int]], ...]]
    dtypes: Dict[str, np.dtype]


@dataclass
class _ReadWindow:
    """One read window: every selected dataset's values over ``[low, high)``.

    ``arrays`` is keyed like :func:`_chunk_reads`. ``read_buffer_bytes`` is
    what the window is charged as a forest's validation read buffer.
    """

    low: int
    high: int
    arrays: Dict[Tuple[str, Optional[int]], np.ndarray]
    read_buffer_bytes: int

    def rows(self, first: int, count: int) -> Dict[Tuple[str, Optional[int]], np.ndarray]:
        """Views of the window's values for dataset rows ``[first, first + count)``."""
        low = first - self.low
        return {key: values[low : low + count] for key, values in self.arrays.items()}


def _chunk_reads(plan: _FilePlan) -> Tuple[Tuple[str, Optional[int]], ...]:
    """The distinct ``(dataset, component)`` reads one chunk of ``plan`` needs.

    Every required role's dataset whole (component ``None``) and every extra
    source component, each once even when a role and an extra share a
    dataset, in first-use order.
    """
    keys: Dict[Tuple[str, Optional[int]], None] = {}
    for spelling in plan.roles.values():
        keys[(spelling, None)] = None
    for components in plan.extras.values():
        for spelling, component in components:
            keys[(spelling, component)] = None
    return tuple(keys)


def _window_bytes_per_row(plan: _FilePlan) -> int:
    """Bytes one read-window row costs: the stored element width of every
    :func:`_chunk_reads` read, plus the per-row transient allowance the
    topology read already carries (:data:`TOPOLOGY_READ_BUFFER_BYTES_PER_ROW`),
    which covers the snapshot check's temporaries when a window-held forest's
    ``SnapNum`` is converted."""
    stored = sum(int(plan.dtypes[spelling].itemsize) for spelling, _component in _chunk_reads(plan))
    return stored + TOPOLOGY_READ_BUFFER_BYTES_PER_ROW


def _in_storage_order(plan: _FilePlan) -> bool:
    """Whether ``plan``'s non-empty forests sit in the datasets in row order.

    ``validate_forest_table`` has already proved the non-empty forests tile
    ``[0, halo_extent)`` exactly, so strictly ascending offsets in row order
    mean each forest starts where the previous one ends, and a run of
    consecutive rows is one contiguous slab.
    """
    offsets = plan.offsets[plan.counts > 0]
    return bool(np.all(offsets[1:] > offsets[:-1]))


class CTreesHDF5Adapter(SourceAdapter):
    """Streams Consistent-Trees forests-HDF5 forests into canonical batches.

    ``info_path`` is the file a run names as ``tree_name`` under
    ``simulation_dir``. ``first_file``/``last_file`` select the ``File<N>``
    groups exactly as the run's ``input.first_file``/``input.last_file`` do,
    and every one of them must exist (C1: never narrow silently).
    ``particle_mass`` is the package's ``simulation.particle_mass`` in
    ``1e10 Msun/h``, the value the C reader's ``Len`` convention divides by.
    ``max_snapshot`` is ``len(a_list) - 1`` when the caller has an a_list.
    """

    source_format = "consistent_trees_hdf5"

    def __init__(
        self,
        schema: CanonicalSchema,
        info_path,
        *,
        first_file: int,
        last_file: int,
        particle_mass: float,
        max_snapshot: Optional[int] = None,
        memory_budget_bytes: int = DEFAULT_MEMORY_BUDGET_BYTES,
    ):
        if h5py is None:  # pragma: no cover - h5py is a pinned dependency
            raise MissingDependencyError("h5py is required to read consistent_trees_hdf5 sources")
        if schema.source_format != self.source_format:
            raise ConverterError(
                "CTreesHDF5Adapter needs a {!r} schema, got {!r}".format(
                    self.source_format, schema.source_format
                )
            )
        declared = {field.name for field in schema.payload_fields}
        if declared != _PAYLOAD_NAMES:  # pragma: no cover - column_schema fixes the set
            raise ConverterError(
                "the consistent_trees_hdf5 schema declares payload {}, but this adapter computes "
                "{}".format(sorted(declared), sorted(_PAYLOAD_NAMES))
            )
        first_file = require_integer(first_file, "first_file", "it selects recorded identity")
        last_file = require_integer(last_file, "last_file", "it selects recorded identity")
        if not 0 <= first_file <= last_file:
            raise ConverterError(
                "need 0 <= first_file <= last_file, got first_file={} last_file={}".format(
                    first_file, last_file
                )
            )
        if isinstance(particle_mass, bool) or not isinstance(
            particle_mass, (int, float, np.integer, np.floating)
        ):
            raise ConverterError(
                "particle_mass must be a number in 1e10 Msun/h, got {!r}".format(particle_mass)
            )
        particle_mass = float(particle_mass)
        if not math.isfinite(particle_mass) or particle_mass <= 0.0:
            raise ConverterError(
                "particle_mass must be positive and finite, got {!r}; the vertical reader would "
                "silently set every Len to 0, which a lossless conversion refuses".format(
                    particle_mass
                )
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
        if max_snapshot is not None:
            max_snapshot = require_integer(
                max_snapshot, "max_snapshot", "a fractional bound would be truncated"
            )
            if max_snapshot < 0:
                raise ConverterError(
                    "max_snapshot must be non-negative, got {}".format(max_snapshot)
                )
        try:
            info_path = Path(info_path)
        except TypeError as exc:
            raise ConverterError(
                "info_path {!r} is not a filesystem path ({})".format(info_path, exc)
            ) from exc
        if not info_path.is_file():
            raise ConverterError(
                "forests-HDF5 info file {} is missing or not a regular file".format(info_path)
            )

        self.schema = schema
        self.info_path = info_path
        self.first_file = first_file
        self.last_file = last_file
        self.particle_mass = particle_mass
        self.max_snapshot = max_snapshot
        self.memory_budget_bytes = memory_budget_bytes
        self._plans: Tuple[_FilePlan, ...] = ()
        self._dependencies: Tuple[HDF5Dependency, ...] = ()
        self._inventory: Optional[SourceInventory] = None

    # ---- public views ----------------------------------------------------

    def inventory(self) -> SourceInventory:
        """Walk and validate every requested file, pin its dependencies and
        build the complete ordered inventory.

        Every structural and dependency check happens here, so nothing
        ``iter_batches`` emits was read from a source that had not already
        passed them.
        """
        if self._inventory is None:
            self._plans, self._dependencies, self._inventory = self._build_inventory()
        return self._inventory

    def dependencies(self) -> Tuple[HDF5Dependency, ...]:
        """The pinned physical files, sorted by path, for provenance."""
        self.inventory()
        return self._dependencies

    def iter_forests(self) -> Iterator[ForestRecord]:
        """Every inventory forest in emission order, for the sidecar.

        O(1) per forest from the inventory's retained ``ForestInfo`` columns;
        nothing is re-read.
        """
        self.inventory()
        for plan in self._plans:
            for unit in range(plan.counts.shape[0]):
                yield ForestRecord(
                    forest_index=plan.forest_base + unit,
                    forest_id=int(plan.forest_ids[unit]),
                    source_file_ordinal=plan.ordinal,
                    unit_ordinal=unit,
                    n_halos=int(plan.counts[unit]),
                )

    # ---- inventory -------------------------------------------------------

    def _open_info(self):
        try:
            return h5py.File(self.info_path, "r")
        except _H5_ERRORS as exc:
            raise ConverterError(
                "cannot open forests-HDF5 info file {}: {}".format(self.info_path, exc)
            ) from exc

    def _build_inventory(self):
        walk = _DependencyWalk()
        plans: List[_FilePlan] = []
        units: List[SourceUnit] = []
        forest_base = 0
        total_halos = 0
        with self._open_info() as handle:
            walk.record(handle.filename, "/")
            nfiles = _int_attribute(handle, "Nfiles", "/")
            if self.last_file >= nfiles:
                raise ConverterError(
                    "{}: requested files [{}, {}] but Nfiles is {} (valid File<N> are 0..{}); a "
                    "missing requested file fails rather than narrowing".format(
                        self.info_path, self.first_file, self.last_file, nfiles, nfiles - 1
                    )
                )
            seen: Dict[object, str] = {}
            for ordinal in range(self.first_file, self.last_file + 1):
                plan = self._inspect_file(handle, ordinal, forest_base, len(units), walk, seen)
                for unit in range(plan.counts.shape[0]):
                    units.append(
                        SourceUnit(
                            source_file_ordinal=ordinal,
                            unit_ordinal=unit,
                            n_halos=int(plan.counts[unit]),
                        )
                    )
                n_forests = int(plan.counts.shape[0])
                if n_forests > INT64_MAX - forest_base:  # pragma: no cover - needs 2**63 forests
                    raise ConverterError("file-prefix forest number overflows int64")
                forest_base += n_forests
                total_halos += plan.halo_extent
                plans.append(plan)
            if self.first_file == 0 and self.last_file == nfiles - 1:
                for name, actual in (("TotNforests", forest_base), ("TotNhalos", total_halos)):
                    declared = _int_attribute(handle, name, "/", required=False)
                    if declared is not None and declared != actual:
                        raise ConverterError(
                            "{}: root attribute {} is {}, but the File<N> groups hold {}".format(
                                self.info_path, name, declared, actual
                            )
                        )
        inventory = SourceInventory(units)
        return tuple(plans), walk.pin(), inventory

    def _inspect_file(
        self,
        handle,
        ordinal: int,
        forest_base: int,
        units_so_far: int,
        walk: _DependencyWalk,
        seen: Dict[object, str],
    ) -> _FilePlan:
        """Validate one ``File<N>`` group and everything read from it."""
        file_path = "/File{}".format(ordinal)
        group = _open_member(handle, "File{}".format(ordinal), file_path, walk)
        if not isinstance(group, h5py.Group):
            raise ConverterError("{}: is not a group".format(file_path))
        self._claim(group, file_path, seen)

        n_forests = _int_attribute(group, "Nforests", file_path)
        if n_forests < 1:
            raise ConverterError(
                "{}: Nforests is {}; the vertical reader requires at least one forest per "
                "file".format(file_path, n_forests)
            )
        contiguous = _int_attribute(group, "contiguous-halo-props", file_path, itemsize=1)
        if not contiguous:
            raise ConverterError(
                "{}: contiguous-halo-props is false; the array-of-structs layout is not "
                "supported".format(file_path)
            )

        info_path = file_path + "/ForestInfo"
        forest_info = _open_member(group, "ForestInfo", info_path, walk)
        offsets, counts, forest_ids = self._read_forest_info(
            forest_info, info_path, n_forests, units_so_far
        )

        forests_path = file_path + "/Forests"
        forests = _open_member(group, "Forests", forests_path, walk)
        if not isinstance(forests, h5py.Group):
            raise ConverterError("{}: is not a group".format(forests_path))
        self._claim(forests, forests_path, seen)
        try:
            available = list(forests.keys())
        except _H5_ERRORS as exc:
            raise ConverterError("{}: cannot list datasets: {}".format(forests_path, exc)) from exc
        roles = resolve_required_columns(self.schema, available)
        extras = resolve_extra_sources(self.schema, available)

        dtypes: Dict[str, np.dtype] = {}
        extent: Optional[int] = None
        needed = set(roles.values())
        for components in extras.values():
            needed.update(spelling for spelling, _component in components)
        for spelling in sorted(needed):
            logical = "{}/{}".format(forests_path, spelling)
            dataset = _open_member(forests, spelling, logical, walk)
            if not isinstance(dataset, h5py.Dataset):
                raise ConverterError("{}: is not a dataset".format(logical))
            _check_storage(dataset, logical)
            rows = int(dataset.shape[0]) if dataset.ndim else None
            if rows is None or (extent is not None and rows != extent):
                raise ConverterError(
                    "{}: has shape {}, but the Forests datasets must share one per-halo extent "
                    "({})".format(logical, dataset.shape, extent)
                )
            extent = rows
            dtypes[spelling] = dataset.dtype
            self._check_selected_dataset(
                spelling, logical, dataset.dtype, dataset.shape, roles, extras
            )
        if extent is None:  # pragma: no cover - the required role set is never empty
            raise ConverterError("{}: no dataset was selected".format(forests_path))

        validate_forest_table(offsets, counts, extent, info_path)
        declared_halos = _int_attribute(group, "Nhalos", file_path, required=False)
        if declared_halos is not None and declared_halos != extent:
            raise ConverterError(
                "{}: Nhalos attribute is {}, but ForestInfo and the Forests datasets hold "
                "{}".format(file_path, declared_halos, extent)
            )
        return _FilePlan(
            ordinal=ordinal,
            forest_base=forest_base,
            halo_extent=extent,
            offsets=offsets,
            counts=counts,
            forest_ids=forest_ids,
            roles=roles,
            extras=extras,
            dtypes=dtypes,
        )

    @staticmethod
    def _claim(group, logical: str, seen: Dict[object, str]) -> None:
        """Refuse two logical paths that reach the same HDF5 group.

        Two ``File<N>`` external links to one data file would otherwise emit
        every one of its forests twice under different identities -- the dual
        of the missing-file case C1 already refuses.

        Keyed by the physical file's ``(st_dev, st_ino)`` and the object's
        address in it -- not by ``hash(group.id)``, which folds in HDF5's
        per-open file serial number and so changes when an externally linked
        file is closed and reopened between two hops.
        """
        try:
            status = os.stat(group.file.filename)
            key = (status.st_dev, status.st_ino, h5py.h5o.get_info(group.id).addr)
        except _H5_ERRORS as exc:  # pragma: no cover - the group was just opened
            raise ConverterError("{}: cannot identify: {}".format(logical, exc)) from exc
        if key in seen:
            raise ConverterError(
                "{} is the same HDF5 object as {}; converting both would emit its forests "
                "twice".format(logical, seen[key])
            )
        seen[key] = logical

    def _read_forest_info(self, dataset, logical: str, n_forests: int, units_so_far: int):
        if not isinstance(dataset, h5py.Dataset):
            raise ConverterError("{}: is not a dataset".format(logical))
        _check_storage(dataset, logical)
        dtype = dataset.dtype
        names = dtype.names or ()
        missing = [name for name in _FOREST_INFO_FIELDS if name not in names]
        if missing:
            raise ConverterError("{}: compound type lacks member(s) {}".format(logical, missing))
        if dtype.itemsize != _FOREST_INFO_ITEMSIZE:
            raise ConverterError(
                "{}: record is {} bytes but the vertical reader requires {} (4 x int64)".format(
                    logical, dtype.itemsize, _FOREST_INFO_ITEMSIZE
                )
            )
        for name in _FOREST_INFO_FIELDS:
            member = dtype.fields[name][0]
            if member.kind != "i" or member.shape:
                raise ConverterError(
                    "{}: member {} is {}, not a signed integer".format(logical, name, member)
                )
        if dataset.ndim != 1:
            raise ConverterError("{}: must be one-dimensional".format(logical))
        if int(dataset.shape[0]) != n_forests:
            raise ConverterError(
                "{}: holds {} rows but Nforests is {}".format(logical, dataset.shape[0], n_forests)
            )
        # Checked before the read, from the extent alone: the read and every
        # structure built from it are the O(forest count) term C4 budgets.
        check_budget(
            (units_so_far + n_forests) * INVENTORY_BYTES_PER_UNIT + INVENTORY_BASE_BYTES,
            self.memory_budget_bytes,
            "inventory of {} forests ({} B/unit plus a {}-byte base)".format(
                units_so_far + n_forests, INVENTORY_BYTES_PER_UNIT, INVENTORY_BASE_BYTES
            ),
            "raise memory_budget_bytes, or convert a narrower file range",
        )
        try:
            table = dataset.fields(list(_FOREST_INFO_FIELDS))[()]
        except _H5_ERRORS as exc:
            raise ConverterError("{}: cannot be read: {}".format(logical, exc)) from exc
        columns = tuple(
            np.ascontiguousarray(table[name], dtype=np.int64) for name in _FOREST_INFO_FIELDS
        )
        del table
        forest_ids, offsets, counts = columns
        return offsets, counts, forest_ids

    def _check_selected_dataset(
        self,
        spelling: str,
        logical: str,
        dtype: np.dtype,
        shape: Tuple[int, ...],
        roles: Dict[str, str],
        extras: Dict[str, Tuple[Tuple[str, Optional[int]], ...]],
    ) -> None:
        """Check one dataset against every use the schema makes of it.

        A required role must be exactly the dtype the C reader reads, and
        one-dimensional. An extra component must match its declared element
        type exactly (no int-through-float, no silent narrowing), and its
        arity must match the reference: ``{field}`` needs a 1-D dataset,
        ``{field, component}`` an ``[N, 3]`` one.
        """
        for role, resolved in roles.items():
            if resolved != spelling:
                continue
            if len(shape) != 1:
                raise ConverterError(
                    "{}: required column {!r} must be one-dimensional, got shape {}".format(
                        logical, role, shape
                    )
                )
            if dtype not in _ROLE_DTYPES[role]:
                raise ConverterError(
                    "{}: required column {!r} is stored as {}, but the vertical reader reads it "
                    "as {}".format(
                        logical,
                        role,
                        dtype.str,
                        " or ".join(allowed.str for allowed in _ROLE_DTYPES[role]),
                    )
                )
        for extra in self.schema.extra_fields:
            for position, (resolved, component) in enumerate(extras[extra.name]):
                if resolved != spelling:
                    continue
                what = "{}: extra field {!r} source[{}]".format(logical, extra.name, position)
                if component is None and len(shape) != 1:
                    raise ConverterError(
                        "{} names a stored scalar, but the dataset has shape {}".format(what, shape)
                    )
                if component is not None and (len(shape) != 2 or shape[1] != 3):
                    raise ConverterError(
                        "{} names component {} of a stored vector, but the dataset has shape "
                        "{}".format(what, component, shape)
                    )
                wanted = np.dtype("<" + np.dtype(extra.spec.numpy_dtype).str[1:])
                if dtype != wanted:
                    raise ConverterError(
                        "{} stores {}, but the extra is declared {} ({}); integers never pass "
                        "through floating point and nothing is narrowed silently".format(
                            what, dtype.str, extra.type, wanted.str
                        )
                    )

    # ---- batch streaming -------------------------------------------------

    def _verify_dependencies(self) -> None:
        """Fail if any pinned file changed between inventory and streaming."""
        for dependency in self._dependencies:
            current = pin_source_file(dependency.identity.path)
            if current != dependency.identity:
                raise ConverterError(
                    "source dependency {} changed after inventory (was {}, now {}); restart the "
                    "conversion from a fresh inventory".format(
                        dependency.identity.path, dependency.identity, current
                    )
                )

    def _reopen(self, handle, plan: _FilePlan) -> Dict[str, object]:
        """Re-walk one file's read path and return its selected datasets.

        The walk repeats every link check, and the files it touches must be
        exactly those inventory pinned.
        """
        walk = _DependencyWalk()
        file_path = "/File{}".format(plan.ordinal)
        group = _open_member(handle, "File{}".format(plan.ordinal), file_path, walk)
        forests = _open_member(group, "Forests", file_path + "/Forests", walk)
        datasets = {}
        for spelling, dtype in plan.dtypes.items():
            logical = "{}/Forests/{}".format(file_path, spelling)
            dataset = _open_member(forests, spelling, logical, walk)
            if (
                not isinstance(dataset, h5py.Dataset)
                or dataset.dtype != dtype
                or int(dataset.shape[0]) != plan.halo_extent
            ):
                raise ConverterError("{}: changed after inventory".format(logical))
            datasets[spelling] = dataset
        pinned = {dependency.identity.path for dependency in self._dependencies}
        stray = [path for path in walk.paths if path not in pinned]
        if stray:
            raise ConverterError(
                "{}: now resolves through unpinned file(s) {}".format(file_path, stray)
            )
        return datasets

    def iter_batches(self, max_rows: int) -> Iterator[CanonicalBatch]:
        """Stream canonical batches of at most ``max_rows`` rows.

        Forests are emitted in ``ForestInfo`` row order within ascending file
        number -- the inventory's order, so ``SourceHaloID`` ascends across
        every batch. Each forest is structurally validated in full before any
        of its rows enter a batch.

        The emission buffer is budget-checked once, before any file is
        reopened, with :meth:`_raw_bytes_per_row` as the raw per-row term (see
        ``topology.check_emission_budget``). A file in storage order is read
        through read windows (:meth:`_read_window`); a forest no window holds
        is read per forest. Both routes emit identical batches.
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
            self._raw_bytes_per_row(),
            self.memory_budget_bytes,
        )
        self._verify_dependencies()
        builder = BatchBuilder(self.schema, max_rows)
        with self._open_info() as handle:
            for plan in self._plans:
                datasets = self._reopen(handle, plan)
                window_rows = self._window_rows(plan) if _in_storage_order(plan) else 0
                nonempty = np.flatnonzero(plan.counts > 0)
                ends = plan.offsets[nonempty] + plan.counts[nonempty]
                window: Optional[_ReadWindow] = None
                position = -1
                for unit in range(plan.counts.shape[0]):
                    n_halos = int(plan.counts[unit])
                    if n_halos == 0:
                        continue
                    position += 1
                    offset = int(plan.offsets[unit])
                    context = "{}: File{} forest row {} (ForestID {})".format(
                        self.info_path, plan.ordinal, unit, int(plan.forest_ids[unit])
                    )
                    if window_rows and (window is None or offset >= window.high):
                        # Released before the next window is read, so two
                        # windows never coexist.
                        window = None
                        window = self._read_window(
                            datasets, plan, nonempty, ends, position, window_rows
                        )
                    held = window is not None and offset + n_halos <= window.high
                    if held:
                        topology = self._window_topology(window, plan, offset, n_halos, context)
                    else:
                        topology = self._read_forest_topology(
                            datasets, plan, offset, n_halos, context
                        )
                    validate_tree(topology, n_halos, context, self.max_snapshot)
                    # Released before emission, so the one whole-forest term
                    # never overlaps the output buffers.
                    del topology

                    base_id = inventory.base_id(plan.ordinal, unit)
                    start = 0
                    while start < n_halos:
                        count = min(max_rows - builder.n_rows, n_halos - start)
                        if held:
                            raw = window.rows(offset + start, count)
                            chunk = self._convert(
                                raw, plan, unit, n_halos, base_id, start, count, context
                            )
                            del raw
                        else:
                            chunk = self._columns(
                                datasets,
                                plan,
                                unit,
                                offset,
                                n_halos,
                                base_id,
                                start,
                                count,
                                context,
                            )
                        builder.add(chunk)
                        del chunk
                        start += count
                        if builder.n_rows == max_rows:
                            yield builder.take()
                window = None
        if builder.n_rows:
            yield builder.take()

    def _raw_bytes_per_row(self) -> int:
        """Bytes of raw source values one emitted row reads.

        Every required role is an 8-byte dataset (``_ROLE_DTYPES``) read once
        per chunk, and every extra component is read at its declared element
        width, so the figure follows from the schema alone: 152 B/row without
        extras.
        """
        return 8 * len(_ROLE_DTYPES) + sum(
            extra.spec.itemsize for extra in self.schema.extra_fields
        )

    def _window_rows(self, plan: _FilePlan) -> int:
        """The largest read window this file's forests may use, in rows.

        A window of ``R`` rows holds one array per selected dataset read
        (:func:`_chunk_reads`), :func:`_window_bytes_per_row` bytes per row in
        all. Any forest validated from it has at most ``R`` halos, so its
        validation figure is at most ``R * VALIDATION_BYTES_PER_HALO + R *
        window_bytes_per_row + TOPOLOGY_BASE_BYTES``. Choosing::

            R = min(TOPOLOGY_READ_CHUNK_ROWS,
                    (memory_budget_bytes - TOPOLOGY_BASE_BYTES)
                    // (VALIDATION_BYTES_PER_HALO + window_bytes_per_row))

        therefore keeps every window-served forest inside the budget, with the
        window charged as that forest's read buffer. 0 means the budget cannot
        afford a window, and every forest of the file is read per forest.
        """
        per_row = VALIDATION_BYTES_PER_HALO + _window_bytes_per_row(plan)
        affordable = (self.memory_budget_bytes - TOPOLOGY_BASE_BYTES) // per_row
        return int(max(0, min(TOPOLOGY_READ_CHUNK_ROWS, affordable)))

    def _read_window(
        self,
        datasets,
        plan: _FilePlan,
        nonempty: np.ndarray,
        ends: np.ndarray,
        position: int,
        window_rows: int,
    ) -> Optional[_ReadWindow]:
        """Read the window that starts at the ``position``-th non-empty forest.

        ``nonempty`` lists the file's non-empty ``ForestInfo`` rows in row
        order and ``ends`` their exclusive end rows, which ascend because the
        file is in storage order. The window extends over every following
        forest that still fits in ``window_rows`` rows, and reads each selected
        dataset once over that span. Returns ``None`` when the starting forest
        alone is larger than ``window_rows``.
        """
        low = int(plan.offsets[nonempty[position]])
        last = int(np.searchsorted(ends, low + window_rows, side="right")) - 1
        if last < position:
            return None
        high = int(ends[last])
        context = "{}: File{} ForestInfo rows {}-{} (read window)".format(
            self.info_path, plan.ordinal, int(nonempty[position]), int(nonempty[last])
        )
        arrays = {
            key: self._read(datasets[key[0]], low, high, key[1], context)
            for key in _chunk_reads(plan)
        }
        return _ReadWindow(
            low=low,
            high=high,
            arrays=arrays,
            read_buffer_bytes=(high - low) * _window_bytes_per_row(plan),
        )

    def _window_topology(
        self, window: _ReadWindow, plan: _FilePlan, offset: int, n_halos: int, context: str
    ) -> Dict[str, np.ndarray]:
        """One window-held forest's links and snapshots as int64.

        The links are views of the window; only ``SnapNum`` is converted.
        Budget-checked like the per-forest route, with the window as the read
        buffer, which ``_window_rows`` guarantees fits.
        """
        check_validation_budget(
            n_halos,
            window.read_buffer_bytes,
            TOPOLOGY_BASE_BYTES,
            self.memory_budget_bytes,
            context,
        )
        rows = window.rows(offset, n_halos)
        columns = {
            name: np.asarray(rows[(plan.roles[name], None)], dtype=np.int64) for name in LINK_FIELDS
        }
        columns["SnapNum"] = convert_snapshots(
            rows[(plan.roles["snap"], None)], self.max_snapshot, context, 0
        )
        return columns

    @staticmethod
    def _read(dataset, low: int, high: int, component: Optional[int], context: str) -> np.ndarray:
        """One bounded hyperslab read, failures named."""
        try:
            if component is None:
                values = dataset[low:high]
            else:
                values = dataset[low:high, component]
        except _H5_ERRORS as exc:
            raise ConverterError(
                "{}: reading {}[{}:{}] failed: {}".format(context, dataset.name, low, high, exc)
            ) from exc
        return np.asarray(values)

    def _read_forest_topology(
        self, datasets, plan: _FilePlan, offset: int, n_halos: int, context: str
    ) -> Dict[str, np.ndarray]:
        """Gather one forest's links and snapshots as int64, reading in chunks.

        The columns are whole-forest; the reads are not. Budget-checked before
        the allocation, covering the validator's scratch as well.
        """
        read_buffer_bytes = min(TOPOLOGY_READ_CHUNK_ROWS, n_halos) * (
            TOPOLOGY_READ_BUFFER_BYTES_PER_ROW
        )
        check_validation_budget(
            n_halos, read_buffer_bytes, TOPOLOGY_BASE_BYTES, self.memory_budget_bytes, context
        )
        columns = {name: np.empty(n_halos, dtype=np.int64) for name in TOPOLOGY_COLUMNS}
        snap = plan.roles["snap"]
        start = 0
        while start < n_halos:
            count = min(TOPOLOGY_READ_CHUNK_ROWS, n_halos - start)
            low = offset + start
            for name in LINK_FIELDS:
                columns[name][start : start + count] = self._read(
                    datasets[plan.roles[name]], low, low + count, None, context
                )
            raw = self._read(datasets[snap], low, low + count, None, context)
            columns["SnapNum"][start : start + count] = convert_snapshots(
                raw, self.max_snapshot, context, start
            )
            del raw
            start += count
        return columns

    def _columns(
        self,
        datasets,
        plan: _FilePlan,
        unit: int,
        offset: int,
        n_halos: int,
        base_id: int,
        start: int,
        count: int,
        context: str,
    ) -> Dict[str, Dict[str, np.ndarray]]:
        """Read and convert one bounded chunk of one forest."""
        low = offset + start
        raw = {
            key: self._read(datasets[key[0]], low, low + count, key[1], context)
            for key in _chunk_reads(plan)
        }
        return self._convert(raw, plan, unit, n_halos, base_id, start, count, context)

    def _convert(
        self,
        raw: Dict[Tuple[str, Optional[int]], np.ndarray],
        plan: _FilePlan,
        unit: int,
        n_halos: int,
        base_id: int,
        start: int,
        count: int,
        context: str,
    ) -> Dict[str, Dict[str, np.ndarray]]:
        """Convert one chunk's raw dataset values into the canonical groups.

        ``raw`` maps each :func:`_chunk_reads` key to the chunk's ``count``
        stored values, whether read for this chunk or sliced from a read
        window. Every returned array owns its memory.
        """
        identity, coordinates = identity_columns(
            base_id, plan.forest_base + unit, plan.ordinal, unit, start, count
        )

        links = {}
        for name in LINK_FIELDS:
            local = raw[(plan.roles[name], None)]
            # Re-checked chunk-locally: the topology pass validated these
            # rows, and a value outside the forest now would mean the source
            # changed underneath the conversion.
            if bool(np.any((local < NULL_LINK) | (local >= n_halos))):
                raise ConverterError(
                    "{}: {} changed between the validation and emission reads".format(context, name)
                )
            links[name] = np.where(local >= 0, base_id + local, NULL_LINK).astype(np.int64)

        roles = {role: raw[(plan.roles[role], None)] for role in _FLOAT_ROLES + ("id",)}
        snapshots = convert_snapshots(
            raw[(plan.roles["snap"], None)], self.max_snapshot, context, start
        )
        payload = ctrees_payload(roles, self.particle_mass, snapshots, context, start)
        del roles

        extras = {}
        for extra in self.schema.extra_fields:
            spec = extra.spec
            parts = []
            for spelling, component in plan.extras[extra.name]:
                values = raw[(spelling, component)]
                if values.dtype.kind == "f" and not bool(np.all(np.isfinite(values))):
                    row = int(np.flatnonzero(~np.isfinite(values))[0])
                    raise ConverterError(
                        "{}, row {}: extra {!r} source {!r} holds the non-finite value {!r}; NaN "
                        "and infinity are not valid payload".format(
                            context, start + row, extra.name, spelling, float(values[row])
                        )
                    )
                parts.append(np.array(values, dtype=spec.numpy_dtype))
            extras[extra.name] = parts[0] if spec.n_components == 1 else np.stack(parts, axis=1)

        return {
            "identity": identity,
            "coordinates": coordinates,
            "links": links,
            "payload": payload,
            "extras": extras,
        }
