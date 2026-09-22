"""Read-only source inspection helpers (Slice 1 of the converter generalisation
plan, docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md).

Pure inspection: every function here opens sources for reading only, and none
writes into a source directory. Used by scripts/convert/inspect_sources.py.

L-Halo binary layout is the shipped 104-byte record (src/include/generated/
raw_halo_defs.h -> struct RawHalo), with an explicit caller-supplied byte
order -- never numpy's native ('=') packing, which would silently follow host
architecture instead of the file's actual endianness.

Consistent-Trees forests-HDF5 links are forest-local row indices (verified
against src/io/vertical/read_ctrees_hdf5.c's CT_ASSIGN_LINK bounds check:
-1 <= link < nhalos, where nhalos is the per-forest halo count), so link-span
scanning resolves them through each forest's ForestHalosOffset before
comparing snapshot numbers.
"""

import shutil
import socket
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import yaml

try:
    import h5py
except ImportError:  # pragma: no cover - exercised via MissingDependencyError path
    h5py = None

from ctrees_parser import ConverterError

__all__ = [
    "ConverterError",
    "MissingDependencyError",
    "LHALO_FIELDS",
    "LHALO_RECORD_BYTES",
    "lhalo_record_dtype",
    "LHaloHeader",
    "read_lhalo_header",
    "LinkSpanSummary",
    "scan_lhalo_file",
    "HDF5FileLinkage",
    "inspect_ctrees_hdf5_source",
    "SimulationInfo",
    "load_simulation_info",
    "SourceReachability",
    "check_lhalo_reachability",
    "check_hdf5_reachability",
    "host_identity",
    "free_space_bytes",
]


class MissingDependencyError(RuntimeError):
    """A required optional dependency (h5py) is not installed."""


# --------------------------------------------------------------------------
# L-Halo binary
# --------------------------------------------------------------------------

#: (field name, numpy kind, shape) in on-disk order, matching struct RawHalo
#: exactly (src/include/generated/raw_halo_defs.h). Scalar fields carry shape
#: ().
LHALO_FIELDS: Tuple[Tuple[str, str, Tuple[int, ...]], ...] = (
    ("Descendant", "i4", ()),
    ("FirstProgenitor", "i4", ()),
    ("NextProgenitor", "i4", ()),
    ("FirstHaloInFOFgroup", "i4", ()),
    ("NextHaloInFOFgroup", "i4", ()),
    ("Len", "i4", ()),
    ("M_Mean200", "f4", ()),
    ("M_Crit200", "f4", ()),
    ("M_TopHat", "f4", ()),
    ("Pos", "f4", (3,)),
    ("Vel", "f4", (3,)),
    ("VelDisp", "f4", ()),
    ("Vmax", "f4", ()),
    ("Spin", "f4", (3,)),
    ("MostBoundID", "i8", ()),
    ("SnapNum", "i4", ()),
    ("FileNr", "i4", ()),
    ("SubhaloIndex", "i4", ()),
    ("SubHalfMass", "f4", ()),
)

#: The five stored local links C1/C2 require the binary adapter to preserve.
LHALO_LINK_FIELDS = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)

_LHALO_FIELD_NAMES = frozenset(name for name, _kind, _shape in LHALO_FIELDS)
if not set(LHALO_LINK_FIELDS) <= _LHALO_FIELD_NAMES:  # pragma: no cover - defends future edits
    raise AssertionError(
        "LHALO_LINK_FIELDS names not present in LHALO_FIELDS: {}".format(
            set(LHALO_LINK_FIELDS) - _LHALO_FIELD_NAMES
        )
    )

LHALO_RECORD_BYTES = 104


def lhalo_record_dtype(byte_order: str) -> np.dtype:
    """Explicit structured dtype for the shipped L-Halo record.

    `byte_order` is '<' (little) or '>' (big) and is always supplied by the
    caller; it is never inferred from host architecture."""
    if byte_order not in ("<", ">"):
        raise ConverterError("byte_order must be '<' or '>', got {!r}".format(byte_order))
    descr = []
    for name, kind, shape in LHALO_FIELDS:
        fmt = byte_order + kind
        descr.append((name, fmt, shape) if shape else (name, fmt))
    dtype = np.dtype(descr)
    if dtype.itemsize != LHALO_RECORD_BYTES:
        raise ConverterError(
            "LHALO_FIELDS itemsize {} does not match the shipped {}-byte record".format(
                dtype.itemsize, LHALO_RECORD_BYTES
            )
        )
    return dtype


@dataclass
class LHaloHeader:
    path: Path
    byte_order: str
    ntrees: int
    tree_halo_counts: np.ndarray
    total_halos: int
    header_bytes: int
    file_size: int
    expected_size: int


def read_lhalo_header(path, byte_order: str = "<") -> LHaloHeader:
    """Read and validate the Ntrees/totNHalos/per-tree-count header.

    Validates the count table sums to totNHalos and that the file's actual
    byte length matches the header-implied size exactly (header + totNHalos *
    104 bytes). Raises ConverterError on any truncation or count mismatch."""
    path = Path(path)
    int_dtype = np.dtype(byte_order + "i4")
    file_size = path.stat().st_size
    with open(path, "rb") as handle:
        head = handle.read(8)
        if len(head) != 8:
            raise ConverterError(
                "{}: truncated header (need 8 bytes for Ntrees/totNHalos, got {})".format(
                    path, len(head)
                )
            )
        ntrees_arr = np.frombuffer(head, dtype=int_dtype, count=2)
        ntrees = int(ntrees_arr[0])
        total_halos = int(ntrees_arr[1])
        if ntrees < 0 or total_halos < 0:
            raise ConverterError(
                "{}: negative header counts (Ntrees={}, totNHalos={})".format(
                    path, ntrees, total_halos
                )
            )
        # Sanity-check the count-table size against the already-measured file
        # size before reading it: an opposite-endian or corrupted header can
        # otherwise request a multi-GiB read (reproducible today by reading a
        # real little-endian file as big-endian) before any size check exists.
        if 8 + 4 * ntrees > file_size:
            raise ConverterError(
                "{}: header claims {} trees (count table alone would need {} bytes), "
                "but the file is only {} bytes -- corrupt or opposite-endian header".format(
                    path, ntrees, 4 * ntrees, file_size
                )
            )
        counts_bytes = handle.read(4 * ntrees)
        if len(counts_bytes) != 4 * ntrees:
            raise ConverterError(
                "{}: truncated tree-count table (need {} bytes for {} trees, got {})".format(
                    path, 4 * ntrees, ntrees, len(counts_bytes)
                )
            )
        tree_halo_counts = np.frombuffer(counts_bytes, dtype=int_dtype).astype(np.int64)
    if np.any(tree_halo_counts < 0):
        bad_index = int(np.argmax(tree_halo_counts < 0))
        raise ConverterError(
            "{}: tree {} has a negative halo count ({}) -- the total can still sum "
            "correctly against a compensating positive entry elsewhere, so this must be "
            "checked per-tree, not just against the header total".format(
                path, bad_index, int(tree_halo_counts[bad_index])
            )
        )
    counted_total = int(tree_halo_counts.sum())
    if counted_total != total_halos:
        raise ConverterError(
            "{}: per-tree counts sum to {}, header totNHalos={} -- bad count total".format(
                path, counted_total, total_halos
            )
        )
    header_bytes = 8 + 4 * ntrees
    expected_size = header_bytes + total_halos * LHALO_RECORD_BYTES
    if file_size != expected_size:
        raise ConverterError(
            "{}: file is {} bytes, header implies {} bytes ({} header + {} halos x {} bytes) "
            "-- truncated or trailing data".format(
                path, file_size, expected_size, header_bytes, total_halos, LHALO_RECORD_BYTES
            )
        )
    return LHaloHeader(
        path=path,
        byte_order=byte_order,
        ntrees=ntrees,
        tree_halo_counts=tree_halo_counts,
        total_halos=total_halos,
        header_bytes=header_bytes,
        file_size=file_size,
        expected_size=expected_size,
    )


@dataclass
class LinkSpanSummary:
    """Descendant link-span evidence: span = SnapNum[target] - SnapNum[source].

    A valid forward gap (span > 1) is counted, not rejected -- only
    out-of-tree indices and non-forward links are flagged as anomalies.
    `max_span` is the maximum *forward-gap* span specifically (the largest
    value among spans > 1, i.e. `gaps.max()` in the scan); it is 0 for a
    gap-free source with millions of adjacent (span == 1) links, not the
    maximum span among all links.
    `mostboundid_min`/`mostboundid_max` are identity bounds across every halo
    scanned, not just linked ones (L-Halo route only; unset for forests-HDF5,
    which carries no MostBoundID-equivalent single field)."""

    non_null_descendant_links: int = 0
    forward_adjacent_links: int = 0
    forward_gap_links: int = 0
    max_span: int = 0  # maximum forward-gap span (see docstring); 0 if gap-free
    non_forward_or_zero_span: int = 0
    snapshot_halo_counts: Dict[int, int] = field(default_factory=dict)
    mostboundid_min: Optional[int] = None
    mostboundid_max: Optional[int] = None


def scan_lhalo_file(header: LHaloHeader, max_snapshot: Optional[int] = None) -> LinkSpanSummary:
    """Tree-by-tree scan of one already-header-validated L-Halo file.

    Reads one tree's contiguous record block at a time (bounded by the
    largest tree in the file, not the whole file) and vectorises the
    Descendant/SnapNum comparison with numpy. `max_snapshot`, when supplied
    (from the caller's --a-list, i.e. len(a_list) - 1), bounds valid SnapNum
    values to [0, max_snapshot]; without it, no upper bound is enforced."""
    summary = LinkSpanSummary()
    dtype = lhalo_record_dtype(header.byte_order)
    with open(header.path, "rb") as handle:
        handle.seek(header.header_bytes)
        for tree_index in range(header.ntrees):
            n = int(header.tree_halo_counts[tree_index])
            if n == 0:
                continue
            raw = handle.read(n * LHALO_RECORD_BYTES)
            if len(raw) != n * LHALO_RECORD_BYTES:
                raise ConverterError(
                    "{}: truncated halo payload in tree {} (need {} bytes, got {})".format(
                        header.path, tree_index, n * LHALO_RECORD_BYTES, len(raw)
                    )
                )
            records = np.frombuffer(raw, dtype=dtype)
            desc = records["Descendant"]
            snap = records["SnapNum"].astype(np.int64)
            most_bound_id = records["MostBoundID"]

            if max_snapshot is not None and (np.any(snap < 0) or np.any(snap > max_snapshot)):
                raise ConverterError(
                    "{}: tree {} has a SnapNum outside [0, {}] (the a_list's snapshot "
                    "range)".format(header.path, tree_index, max_snapshot)
                )

            for s, c in zip(*np.unique(records["SnapNum"], return_counts=True)):
                key = int(s)
                summary.snapshot_halo_counts[key] = summary.snapshot_halo_counts.get(key, 0) + int(
                    c
                )
            tree_min = int(most_bound_id.min())
            tree_max = int(most_bound_id.max())
            summary.mostboundid_min = (
                tree_min
                if summary.mostboundid_min is None
                else min(summary.mostboundid_min, tree_min)
            )
            summary.mostboundid_max = (
                tree_max
                if summary.mostboundid_max is None
                else max(summary.mostboundid_max, tree_max)
            )

            # Only -1 is the null sentinel (matches the C reader's
            # CT_ASSIGN_LINK, which accepts exactly [-1, nhalos)); anything
            # more negative is structurally invalid, not "no link".
            if np.any(desc < -1):
                raise ConverterError(
                    "{}: tree {} has a Descendant value below -1 (not the -1 null sentinel)".format(
                        header.path, tree_index
                    )
                )
            valid = desc >= 0
            if valid.any():
                targets = desc[valid]
                if np.any(targets >= n):
                    raise ConverterError(
                        "{}: tree {} has an out-of-tree Descendant index".format(
                            header.path, tree_index
                        )
                    )
                span = snap[targets] - snap[valid]
                summary.non_null_descendant_links += int(valid.sum())
                summary.non_forward_or_zero_span += int(np.count_nonzero(span < 1))
                summary.forward_adjacent_links += int(np.count_nonzero(span == 1))
                gaps = span[span > 1]
                summary.forward_gap_links += int(gaps.size)
                if gaps.size:
                    summary.max_span = max(summary.max_span, int(gaps.max()))
    return summary


# --------------------------------------------------------------------------
# Consistent-Trees forests-HDF5
# --------------------------------------------------------------------------


@dataclass
class HDF5FileLinkage:
    name: str
    link_type: str
    reachable: bool
    error: Optional[str] = None
    n_forests: Optional[int] = None
    n_halos: Optional[int] = None
    max_forest_nhalos: Optional[int] = None
    fields: Optional[Dict[str, Dict]] = None
    link_summary: Optional[LinkSpanSummary] = None


def _attr_to_python(value):
    return value.item() if hasattr(value, "item") else value


def inspect_ctrees_hdf5_source(
    info_path,
    scan_links: bool = True,
    chunk_rows: int = 4_000_000,
    max_snapshot: Optional[int] = None,
):
    """Inspect a Consistent-Trees forests-HDF5 info file: root attrs, every
    FileN group's ForestInfo/Forests contents, and (optionally) a full
    link-span scan resolved through each forest's ForestHalosOffset.
    `max_snapshot`, when supplied (from the caller's --a-list, i.e.
    len(a_list) - 1), bounds the snapshot column's valid values to
    [0, max_snapshot] in place of [0, INT_MAX].

    Raises MissingDependencyError if h5py is not installed, and ConverterError
    if the info file cannot be opened at all."""
    if h5py is None:
        raise MissingDependencyError("h5py is required to inspect consistent_trees_hdf5 sources")
    info_path = Path(info_path)
    if not info_path.exists():
        raise ConverterError("{}: forests-HDF5 info file does not exist".format(info_path))
    try:
        handle = h5py.File(info_path, "r")
    except OSError as exc:
        raise ConverterError("{}: failed to open as HDF5: {}".format(info_path, exc)) from exc

    files: List[HDF5FileLinkage] = []
    with handle as f:
        root_attrs = {k: _attr_to_python(v) for k, v in f.attrs.items()}
        for key in sorted(f.keys()):
            link = f.get(key, getlink=True)
            link_type = type(link).__name__
            try:
                group = f[key]
                forest_info = group["ForestInfo"][:]
                forests_group = group["Forests"]
            except Exception as exc:
                # Bare Exception, not an enumerated tuple: this is untrusted
                # per-file HDF5 structure (this slice's own declared risky
                # surface), and no exception type here should abort every
                # other FileN group's already-gathered results just because
                # one file is malformed.
                files.append(
                    HDF5FileLinkage(name=key, link_type=link_type, reachable=False, error=str(exc))
                )
                continue

            context = "{} File {}".format(info_path, key)
            _require_forest_info_fields(forest_info, context)
            _require_dataset(forests_group, "Descendant", context)
            snap_field = _resolve_snap_field(forests_group, context)
            _require_dataset(forests_group, snap_field, context)

            try:
                fields = {
                    fname: {
                        "dtype": str(forests_group[fname].dtype),
                        "shape": list(forests_group[fname].shape),
                        "is_virtual": bool(forests_group[fname].is_virtual),
                    }
                    for fname in forests_group.keys()
                }
            except Exception as exc:
                # A dangling soft link or malformed subgroup inside Forests/
                # surfaces here as whatever HDF5/h5py itself raises (e.g. a
                # bare KeyError with no file/field context) once dereferenced
                # by .dtype/.shape/.is_virtual -- treat it the same as the
                # group-access failure above: this file is unreachable, the
                # others are not affected.
                files.append(
                    HDF5FileLinkage(name=key, link_type=link_type, reachable=False, error=str(exc))
                )
                continue
            n_forests = int(forest_info.shape[0])
            n_halos = int(forest_info["ForestNhalos"].sum()) if n_forests else 0
            max_forest_nhalos = int(forest_info["ForestNhalos"].max()) if n_forests else None

            # Structural ForestInfo validation is O(n_forests), not
            # O(n_halos), so it always runs -- unlike the payload-intensive
            # span computation below, it is not something --no-link-scan
            # should be able to skip past on a real, supported path.
            # Unconditional even when n_forests == 0: every internal check is
            # itself sized-guarded and reduces to comparing the two dataset
            # extents (0 declared halos means both must actually be empty) --
            # previously gating this whole call on `if n_forests:` let a
            # zero-forest ForestInfo bypass the extent-agreement check
            # entirely, silently accepting mismatched Descendant/snapshot
            # dataset lengths.
            _validate_forest_info_offsets(
                forest_info["ForestHalosOffset"],
                forest_info["ForestNhalos"],
                forests_group["Descendant"].shape[0],
                forests_group[snap_field].shape[0],
            )

            link_summary = None
            if scan_links and n_halos:
                link_summary = _scan_ctrees_hdf5_links(
                    forest_info, forests_group, chunk_rows, snap_field, max_snapshot
                )

            files.append(
                HDF5FileLinkage(
                    name=key,
                    link_type=link_type,
                    reachable=True,
                    n_forests=n_forests,
                    n_halos=n_halos,
                    max_forest_nhalos=max_forest_nhalos,
                    fields=fields,
                    link_summary=link_summary,
                )
            )
    return root_attrs, files


#: The snapshot column carries either spelling and either an integer or an
#: integral-float dtype (src/io/vertical/read_ctrees_hdf5.c:178-179 --
#: "Snap_num" (older) or "Snap_idx" (newer), snap_field_is_double). Resolved
#: dynamically per file, exactly as the C reader does, rather than assumed.
_SNAP_FIELD_SPELLINGS = ("Snap_num", "Snap_idx")


def _resolve_snap_field(forests_group, context: str = "Forests/") -> str:
    for name in _SNAP_FIELD_SPELLINGS:
        if name in forests_group:
            return name
    raise ConverterError(
        "{}: has neither {} -- cannot resolve the snapshot column".format(
            context, " nor ".join(_SNAP_FIELD_SPELLINGS)
        )
    )


#: ForestInfo compound-dtype fields this tool reads directly.
_FOREST_INFO_REQUIRED_FIELDS = ("ForestHalosOffset", "ForestNhalos")


def _require_forest_info_fields(forest_info: np.ndarray, context: str) -> None:
    """Raise ConverterError with file context instead of letting a malformed
    ForestInfo compound dtype missing a required field surface as a bare
    ValueError ("no field of name ...") once indexed by name -- mirrors
    _require_dataset's existing pattern for the Forests/ group."""
    names = forest_info.dtype.names or ()
    missing = [name for name in _FOREST_INFO_REQUIRED_FIELDS if name not in names]
    if missing:
        raise ConverterError(
            "{}: ForestInfo is missing required column(s) {}".format(context, missing)
        )


def _require_dataset(forests_group, name: str, context: str) -> None:
    """Raise ConverterError with file/field context instead of letting a
    missing required dataset, one with the wrong rank, or one that exists by
    name but cannot actually be dereferenced surface as a bare
    KeyError/IndexError/AttributeError -- mirrors _resolve_snap_field's
    existing treatment of the (also required, just ambiguously-named)
    snapshot column. `name in forests_group` only checks membership, not
    dereferenceability: a dangling h5py.SoftLink passes that check but
    raises KeyError on `forests_group[name]`, and an ExternalLink resolving
    to a group rather than a dataset raises AttributeError on `.ndim` --
    both reachable from a malformed real file and previously escaped as raw
    exceptions instead of a per-file ConverterError."""
    if name not in forests_group:
        raise ConverterError(
            "{}: Forests/ is missing the required dataset '{}'".format(context, name)
        )
    try:
        ndim = forests_group[name].ndim
    except Exception as exc:
        raise ConverterError(
            "{}: Forests/{} exists by name but cannot be opened as a dataset: {}".format(
                context, name, exc
            )
        ) from exc
    if ndim != 1:
        raise ConverterError(
            "{}: Forests/{} has rank {} -- expected a 1-D, one-row-per-halo array".format(
                context, name, ndim
            )
        )


def _validate_forest_info_offsets(
    offsets: np.ndarray, counts: np.ndarray, dataset_len: int, snap_len: Optional[int] = None
) -> None:
    """Structural sanity check on ForestInfo's offset/count table: counts and
    offsets non-negative, counts below the int32 index limit (matches the
    real C reader's validate_forestinfo_cache_row_ctrees_hdf5:
    forestnhalos >= 0, < INT_MAX, foresthalosoffset >= 0), forests
    non-overlapping and in ascending row order, and the ForestNhalos sum
    agreeing exactly with the Forests dataset length(s) actually present
    (previously only checked inside the link-scan path, which --no-link-scan
    could skip past entirely -- see steer-attempt-3 item 1's
    ForestNhalos=[-1, 1] regression). Not full parity with the C reader's
    per-forest-read validation (that also checks a UniqueGalaxyIDMultiplier
    bound this read-only tool has no reason to know) -- just enough that
    corrupted ForestInfo metadata cannot silently mis-resolve link targets or
    report misleading counts."""
    if counts.size and np.any(counts < 0):
        bad_index = int(np.argmax(counts < 0))
        raise ConverterError(
            "ForestInfo row {} has a negative ForestNhalos ({}) -- the total can still sum "
            "to something plausible against a compensating positive entry elsewhere".format(
                bad_index, int(counts[bad_index])
            )
        )
    if counts.size and np.any(counts >= np.iinfo(np.int32).max):
        raise ConverterError(
            "ForestInfo has a ForestNhalos at or above the int32 index limit ({})".format(
                np.iinfo(np.int32).max
            )
        )
    if offsets.size and np.any(offsets < 0):
        raise ConverterError("ForestInfo has a negative ForestHalosOffset")
    if offsets.size > 1:
        # Forest i's range must end at or before forest i+1's start; this
        # catches both overlap and out-of-order rows in one check. Safe now
        # that counts are known non-negative (offsets are then implied
        # non-decreasing by this same inequality, by induction).
        ends = offsets[:-1] + counts[:-1]
        if np.any(ends > offsets[1:]):
            raise ConverterError(
                "ForestInfo has overlapping or out-of-order ForestHalosOffset/ForestNhalos rows"
            )
    if offsets.size:
        last_end = int(offsets[-1]) + int(counts[-1])
        if last_end > dataset_len:
            raise ConverterError(
                "ForestInfo's last forest range ({}) exceeds the Forests dataset length ({})".format(
                    last_end, dataset_len
                )
            )
    total = int(counts.sum())
    if total != dataset_len:
        raise ConverterError(
            "ForestInfo's ForestNhalos sum ({}) disagrees with the Forests/Descendant "
            "dataset length ({})".format(total, dataset_len)
        )
    if snap_len is not None and total != snap_len:
        raise ConverterError(
            "ForestInfo's ForestNhalos sum ({}) disagrees with the resolved snapshot "
            "column's dataset length ({})".format(total, snap_len)
        )


def _scan_ctrees_hdf5_links(
    forest_info,
    forests_group,
    chunk_rows: int,
    snap_field: str,
    max_snapshot: Optional[int] = None,
) -> LinkSpanSummary:
    """Resolve forest-local Descendant indices to a global row via each
    forest's ForestHalosOffset, then compare snapshot numbers like the binary
    scan. `snap_field` is resolved once by the caller (inspect_ctrees_hdf5_source),
    which also runs _validate_forest_info_offsets unconditionally -- this
    function's own work is exactly the part --no-link-scan is meant to skip.
    `max_snapshot`, when supplied, bounds the snapshot range to
    [0, max_snapshot] in place of [0, INT_MAX]."""
    offsets = forest_info["ForestHalosOffset"]
    counts = forest_info["ForestNhalos"]
    total = int(counts.sum())
    row_offset = np.repeat(offsets, counts)
    if row_offset.shape[0] != total:
        raise ConverterError("ForestInfo offsets/counts do not sum to the declared halo total")

    snap_ds = forests_group[snap_field]
    desc_ds = forests_group["Descendant"]
    if snap_ds.shape[0] != total or desc_ds.shape[0] != total:
        raise ConverterError(
            "Forests/{} or Forests/Descendant length disagrees with ForestInfo's "
            "declared halo total".format(snap_field)
        )

    # The snapshot column is read whole (one value per halo) so target-snapshot
    # lookups can use arbitrary-order fancy indexing; h5py datasets require
    # increasing-order indices, plain numpy arrays do not.
    raw_snap = snap_ds[:]
    if np.issubdtype(raw_snap.dtype, np.floating):
        # Exact floor(v) == v, matching the C reader's own integral-float
        # snapshot check (CT_ASSIGN_SNAP_DOUBLE) -- not a tolerant
        # np.allclose, which would accept non-integral values once |v| gets
        # into the tens of thousands under numpy's default rtol=1e-5.
        if not np.all(np.floor(raw_snap) == raw_snap):
            raise ConverterError(
                "Forests/{} carries non-integral values -- not a valid integral-float "
                "snapshot column".format(snap_field)
            )
        all_snap = raw_snap.astype(np.int64)
    else:
        all_snap = raw_snap.astype(np.int64)
    # Range check on both paths, matching the C reader's CT_ASSIGN_SNAP_INT/
    # CT_ASSIGN_SNAP_DOUBLE (read_ctrees_hdf5.c:486-511): v >= 0 and
    # v <= LastSnapshotNr (here, max_snapshot derived from the caller's
    # --a-list: len(a_list) - 1). Falls back to INT_MAX only when no a_list
    # bound was supplied.
    upper_bound = max_snapshot if max_snapshot is not None else np.iinfo(np.int32).max
    if np.any(all_snap < 0) or np.any(all_snap > upper_bound):
        raise ConverterError(
            "Forests/{} has a snapshot value outside [0, {}]".format(snap_field, upper_bound)
        )
    forest_count_for_row = np.repeat(counts, counts)

    summary = LinkSpanSummary()
    for s, c in zip(*np.unique(all_snap, return_counts=True)):
        summary.snapshot_halo_counts[int(s)] = int(c)

    for start in range(0, total, chunk_rows):
        end = min(start + chunk_rows, total)
        desc = desc_ds[start:end]
        snap = all_snap[start:end]
        local_offset = row_offset[start:end]

        # Only -1 is the null sentinel (matches read_ctrees_hdf5.c's
        # CT_ASSIGN_LINK, which accepts exactly [-1, nhalos)).
        if np.any(desc < -1):
            raise ConverterError(
                "forests-HDF5 chunk has a Descendant value below -1 (not the -1 null sentinel)"
            )
        valid = desc >= 0
        if not valid.any():
            continue
        local_targets = desc[valid]
        if np.any(local_targets >= forest_count_for_row[start:end][valid]):
            raise ConverterError("forests-HDF5 chunk has an out-of-forest Descendant index")
        global_targets = local_offset[valid] + local_targets
        target_snap = all_snap[global_targets]
        span = target_snap - snap[valid]
        summary.non_null_descendant_links += int(valid.sum())
        summary.non_forward_or_zero_span += int(np.count_nonzero(span < 1))
        summary.forward_adjacent_links += int(np.count_nonzero(span == 1))
        gaps = span[span > 1]
        summary.forward_gap_links += int(gaps.size)
        if gaps.size:
            summary.max_span = max(summary.max_span, int(gaps.max()))
    return summary


# --------------------------------------------------------------------------
# Simulation metadata / reachability
# --------------------------------------------------------------------------


@dataclass
class SimulationInfo:
    path: Path
    first_file: int
    last_file: int
    tree_name: str
    tree_type: str
    simulation_dir: str
    snapshot_list_file: str
    raw: Dict


def load_simulation_info(path) -> SimulationInfo:
    path = Path(path)
    with open(path, "r") as handle:
        try:
            data = yaml.safe_load(handle)
        except yaml.YAMLError as exc:
            raise ConverterError("{}: invalid YAML: {}".format(path, exc)) from exc
    try:
        input_section = data["input"]
        return SimulationInfo(
            path=path,
            first_file=int(input_section["first_file"]),
            last_file=int(input_section["last_file"]),
            tree_name=input_section["tree_name"],
            tree_type=input_section["tree_type"],
            simulation_dir=input_section["simulation_dir"],
            snapshot_list_file=input_section["snapshot_list_file"],
            raw=data,
        )
    except (KeyError, TypeError) as exc:
        raise ConverterError("{}: missing required input.* key: {}".format(path, exc)) from exc


@dataclass
class SourceReachability:
    """`free_bytes_on_volume` is always the **source** volume's free space
    (the volume `simulation_dir` lives on) -- never the volume any later
    conversion would write to, which may be a different mount entirely (see
    docs/dev/MIMIC-CONVERTER-SOURCE-INVENTORY.md's Section 2 for the
    output-volume figure and why the two are reported separately). A
    consumer of this JSON field alone, without that doc's prose, should not
    read it as write-target capacity."""

    simulation_dir: str
    exists: bool
    host: str
    declared_first_file: int
    declared_last_file: int
    declared_file_count: int
    present_files: List[str]
    present_file_count: int
    total_bytes: int
    free_bytes_on_volume: Optional[int]
    notes: List[str] = field(default_factory=list)


def host_identity() -> str:
    return socket.gethostname()


def free_space_bytes(path) -> Optional[int]:
    """Free space on the volume containing `path`, or `None` if `path` itself
    does not exist. Deliberately does not walk up to a parent directory: a
    reachability report's "free space" figure is about the requested source
    path, and silently substituting an ancestor's volume would report a real
    number for a path that is not actually there -- not reproducible once the
    absent path's ancestor chain differs between runs/hosts."""
    if not Path(path).exists():
        return None
    return shutil.disk_usage(path).free


def _resolve_binary_paths(sim_info: SimulationInfo) -> List[Path]:
    base = Path(sim_info.simulation_dir)
    return [
        base / "{}.{}".format(sim_info.tree_name, n)
        for n in range(sim_info.first_file, sim_info.last_file + 1)
    ]


def check_lhalo_reachability(sim_info: SimulationInfo) -> SourceReachability:
    base = Path(sim_info.simulation_dir)
    exists = base.exists()
    declared_count = sim_info.last_file - sim_info.first_file + 1
    present: List[Path] = []
    total_bytes = 0
    if exists:
        for candidate in _resolve_binary_paths(sim_info):
            if candidate.exists():
                present.append(candidate)
                total_bytes += candidate.stat().st_size
    notes = []
    if exists and len(present) != declared_count:
        notes.append(
            "{} of {} declared files present (first_file={}, last_file={})".format(
                len(present), declared_count, sim_info.first_file, sim_info.last_file
            )
        )
    if not exists:
        notes.append("simulation_dir does not exist")
    return SourceReachability(
        simulation_dir=str(base),
        exists=exists,
        host=host_identity(),
        declared_first_file=sim_info.first_file,
        declared_last_file=sim_info.last_file,
        declared_file_count=declared_count,
        present_files=[str(p) for p in present],
        present_file_count=len(present),
        total_bytes=total_bytes,
        free_bytes_on_volume=free_space_bytes(base),
        notes=notes,
    )


def check_hdf5_reachability(sim_info: SimulationInfo) -> SourceReachability:
    base = Path(sim_info.simulation_dir)
    exists = base.exists()
    info_path = base / sim_info.tree_name
    notes = []
    total_bytes = 0
    present: List[Path] = []
    # declared_count counts the same units as present_file_count (info file +
    # external-link data files), derived from the info file's own `Nfiles`
    # attribute when readable. first_file/last_file are a *different* count
    # for this route (L-Halo partition numbering, not forests-HDF5 file
    # groups) and are kept only as a documented fallback with a note, so the
    # two figures are never silently compared as if they were the same unit.
    declared_count = None
    if exists and info_path.exists():
        present.append(info_path)
        total_bytes += info_path.stat().st_size
        if h5py is not None:
            try:
                with h5py.File(info_path, "r") as f:
                    n_files_attr = f.attrs.get("Nfiles")
                    if n_files_attr is not None:
                        declared_count = 1 + int(n_files_attr)
                    for key in f.keys():
                        link = f.get(key, getlink=True)
                        if isinstance(link, h5py.ExternalLink):
                            target = (info_path.parent / link.filename).resolve()
                            if target.exists():
                                present.append(target)
                                total_bytes += target.stat().st_size
                            else:
                                notes.append(
                                    "{}: external link target {} is absent".format(key, target)
                                )
            except OSError as exc:
                notes.append("failed to enumerate external links: {}".format(exc))
        else:
            notes.append("h5py not installed: external-link targets not enumerated")
    elif exists:
        notes.append("info file {} does not exist under simulation_dir".format(sim_info.tree_name))
    else:
        notes.append("simulation_dir does not exist")
    if declared_count is None:
        declared_count = sim_info.last_file - sim_info.first_file + 1
        notes.append(
            "declared_file_count derived from first_file/last_file ({}), not the info file's "
            "Nfiles attribute -- not directly comparable to present_file_count (info file + "
            "external-link targets) for this route".format(declared_count)
        )
    return SourceReachability(
        simulation_dir=str(base),
        exists=exists,
        host=host_identity(),
        declared_first_file=sim_info.first_file,
        declared_last_file=sim_info.last_file,
        declared_file_count=declared_count,
        present_files=[str(p) for p in present],
        present_file_count=len(present),
        total_bytes=total_bytes,
        free_bytes_on_volume=free_space_bytes(base),
        notes=notes,
    )
