"""Detect the flyby-demotion signature in an L-Halo binary source or a version 3 horizontal dataset.

**What the signature is.** sage-model's ``fix_flybys``, applied when some
L-Halo files were written, makes every FoF central but the most massive at a
forest's last snapshot a satellite of the survivor and negates its
``MostBoundID``. A converter that carries such a source through unchanged
publishes demoted centrals as if they were the catalogue's own satellites.
For an L-Halo source the signature is therefore defined as a halo that

- has ``MostBoundID < 0``,
- is not its own FoF central (its ``FirstHaloInFOFgroup`` is not its own
  tree-local index), and
- lies at its tree's last snapshot (the largest ``SnapNum`` among that tree's
  halos; an L-Halo tree is a whole forest).

A negative ``MostBoundID`` on its own stays legal data: on a central, or on a
satellite below the tree's last snapshot, it is not the signature. The check
is a converter policy, not a rule of the horizontal format.

**The pre-scan** (:func:`prescan_lhalo`) reads ``SnapNum``,
``FirstHaloInFOFgroup`` and ``MostBoundID`` from every tree of the L-Halo
files an ingest will read and counts signature halos. Tree boundaries come
from ``read_lhalo_header``; each field's byte offset and dtype, the byte order
and the record stride come from the frozen source layout, never from
constants. Each tree is read in chunks of at most ``chunk_rows`` records, and
the only per-tree state is one count of candidate halos per snapshot
(``max_snapshot + 1`` entries), so the count at the tree's last snapshot is
known when the tree ends without holding the tree: a 126-million-halo forest
costs one chunk of records and one snapshot-sized array. A ``SnapNum``
outside ``[0, max_snapshot]`` -- the bound the L-Halo adapter enforces --
is refused naming the file and tree.

The pre-scan is separate from the streamed ingest: it reads the source once
more, read-only, and changes nothing the adapter does.

**A horizontal dataset** (version 3, the vertical writer's input) cannot
cheaply give each forest's last snapshot, so its signature is defined on the
dataset's **last snapshot file holding halos**: a halo there with
``MostBoundID < 0`` that is not its own FoF central (its
``FirstHaloInFOFgroup``, a row index in the same snapshot, is not its own
row). Where every tree is rooted at the final snapshot, as in the Uchuu
datasets, the two definitions agree; a forest ending at an earlier snapshot
is not checked in a horizontal dataset. :func:`count_horizontal_signature`
reads only those two columns of that one file, through
``HorizontalDataset.iter_columns`` in blocks of at most ``block_rows`` rows
(by default that iterator's own, ``DEFAULT_BLOCK_ROWS``, 2^22), never a
whole column; a file lacking either column is refused by that iterator's
unknown-column :class:`ConverterError`. (``horizontal_dataset`` is not
imported here: it imports the pipeline, which imports this module.)
"""

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Mapping, Optional, Sequence, Tuple

import numpy as np
from adapters.lhalo_binary import TOPOLOGY_READ_CHUNK_ROWS
from adapters.source_inventory import read_lhalo_header
from column_schema import CanonicalSchema, ConverterError, SourceLayout, resolve_required_columns

__all__ = [
    "SIGNATURE_ROLES",
    "FlybyScan",
    "HorizontalFlybyScan",
    "HORIZONTAL_SIGNATURE_COLUMNS",
    "count_horizontal_signature",
    "count_tree_signature",
    "prescan_lhalo",
    "unchecked_record",
]

#: The three required roles the signature is defined on.
SIGNATURE_ROLES = ("SnapNum", "FirstHaloInFOFgroup", "MostBoundID")

#: The two ``/halos`` columns the horizontal check reads.
HORIZONTAL_SIGNATURE_COLUMNS = ("MostBoundID", "FirstHaloInFOFgroup")


@dataclass(frozen=True)
class FlybyScan:
    """What :func:`prescan_lhalo` found.

    ``first_file_ordinal``/``first_file_path`` name the first source file (in
    inventory order) holding a signature halo, and are ``None`` when
    ``halos`` is 0.
    """

    halos: int
    trees: int
    first_file_ordinal: Optional[int]
    first_file_path: Optional[str]

    def record(self, allowed: bool) -> Dict[str, object]:
        """The ingest stage result's ``flyby_signature`` entry. ``allowed`` is
        recorded true only when the override was given *and* there was
        something to allow."""
        return {
            "checked": True,
            "halos": int(self.halos),
            "trees": int(self.trees),
            "allowed": bool(allowed) and self.halos > 0,
        }


def unchecked_record(source_format: str) -> Dict[str, object]:
    """The ``flyby_signature`` entry for a source format the check does not
    apply to (the Consistent-Trees routes, whose ``id`` Mimic never negates)."""
    return {"checked": False, "reason": "source format {}".format(source_format)}


# ==========================================================================
# The signature on one tree
# ==========================================================================


def _candidates(
    snap_num: np.ndarray, first_halo_in_fof: np.ndarray, most_bound_id: np.ndarray, start: int
) -> np.ndarray:
    """Rows of one chunk of a tree, starting at tree-local row ``start``,
    that carry a negative ``MostBoundID`` and are not their own FoF central;
    the snapshot test is applied once the tree's last snapshot is known."""
    local = np.arange(start, start + snap_num.shape[0], dtype=np.int64)
    return (np.asarray(most_bound_id) < 0) & (
        np.asarray(first_halo_in_fof, dtype=np.int64) != local
    )


def count_tree_signature(
    snap_num: Sequence[int], first_halo_in_fof: Sequence[int], most_bound_id: Sequence[int]
) -> int:
    """Signature halos in one whole tree, from its ``SnapNum``,
    ``FirstHaloInFOFgroup`` and ``MostBoundID`` columns in tree-local row
    order. An empty tree has none."""
    snap_num = np.asarray(snap_num, dtype=np.int64)
    first_halo_in_fof = np.asarray(first_halo_in_fof, dtype=np.int64)
    most_bound_id = np.asarray(most_bound_id, dtype=np.int64)
    if not snap_num.shape == first_halo_in_fof.shape == most_bound_id.shape:
        raise ConverterError(
            "count_tree_signature needs three columns of one length, got {}, {} and {}".format(
                snap_num.shape, first_halo_in_fof.shape, most_bound_id.shape
            )
        )
    if snap_num.size == 0:
        return 0
    last_snapshot = int(snap_num.max())
    candidates = _candidates(snap_num, first_halo_in_fof, most_bound_id, 0)
    return int(np.count_nonzero(candidates & (snap_num == last_snapshot)))


# ==========================================================================
# The pre-scan
# ==========================================================================


def _signature_dtype(layout: SourceLayout, schema: CanonicalSchema) -> Tuple[np.dtype, Dict]:
    """A record dtype viewing only the three signature fields at their
    declared offsets, in the declared byte order and stride, and the map from
    role to the source's own field name."""
    available = [entry.name for entry in layout.entries]
    roles = resolve_required_columns(schema, available)
    entries = {entry.name: entry for entry in layout.entries}
    names, formats, offsets = [], [], []
    for role in SIGNATURE_ROLES:
        entry = entries[roles[role]]
        base = np.dtype(entry.numpy_dtype)
        if entry.n_components != 1 or base.kind not in "iu":
            raise ConverterError(
                "the source layout declares {} ({}) as {} x {}; the flyby-signature pre-scan "
                "reads it as one integer".format(
                    role, entry.name, entry.n_components, entry.numpy_dtype
                )
            )
        names.append(entry.name)
        formats.append(base.newbyteorder(layout.numpy_byte_order))
        offsets.append(entry.offset)
    dtype = np.dtype(
        {"names": names, "formats": formats, "offsets": offsets, "itemsize": layout.itemsize}
    )
    return dtype, {role: roles[role] for role in SIGNATURE_ROLES}


def _new_snapshot_counts(n_snapshots: int) -> np.ndarray:
    """The pre-scan's only per-tree state: one candidate count per snapshot."""
    return np.zeros(n_snapshots, dtype=np.int64)


def _read_records(handle, count: int, dtype: np.dtype, context: str) -> np.ndarray:
    """Read exactly ``count`` records from the current file position."""
    want = count * dtype.itemsize
    try:
        raw = handle.read(want)
    except OSError as exc:
        raise ConverterError(
            "{}: reading {} bytes for the flyby-signature pre-scan failed: {}".format(
                context, want, exc
            )
        ) from exc
    if len(raw) != want:
        raise ConverterError(
            "{}: truncated halo payload (need {} bytes for {} records, got {})".format(
                context, want, count, len(raw)
            )
        )
    return np.frombuffer(raw, dtype=dtype, count=count)


def _scan_file(
    ordinal: int,
    path: Path,
    layout: SourceLayout,
    dtype: np.dtype,
    fields: Mapping[str, str],
    max_snapshot: int,
    chunk_rows: int,
) -> Tuple[int, int]:
    """Signature halos and affected trees in one source file."""
    try:
        header = read_lhalo_header(
            path, byte_order=layout.numpy_byte_order, record_bytes=layout.itemsize
        )
        handle = open(path, "rb")
    except OSError as exc:
        raise ConverterError(
            "cannot read source file {} (ordinal {}) for the flyby-signature pre-scan: {}".format(
                path, ordinal, exc
            )
        ) from exc
    n_snapshots = max_snapshot + 1
    counts = _new_snapshot_counts(n_snapshots)
    halos = 0
    trees = 0
    with handle:
        try:
            handle.seek(header.header_bytes)
        except OSError as exc:
            raise ConverterError(
                "{} (ordinal {}): cannot seek to byte {}: {}".format(
                    path, ordinal, header.header_bytes, exc
                )
            ) from exc
        for tree in range(header.ntrees):
            n_halos = int(header.tree_halo_counts[tree])
            if n_halos == 0:
                continue
            context = "{} (ordinal {}): tree {}".format(path, ordinal, tree)
            last_snapshot = -1
            dirty = False
            start = 0
            while start < n_halos:
                count = min(chunk_rows, n_halos - start)
                records = _read_records(handle, count, dtype, context)
                snaps = records[fields["SnapNum"]].astype(np.int64)
                outside = (snaps < 0) | (snaps > max_snapshot)
                if bool(outside.any()):
                    row = start + int(np.argmax(outside))
                    raise ConverterError(
                        "{}, row {}: SnapNum is {}, outside [0, {}]; the flyby-signature "
                        "pre-scan refuses the source".format(
                            context, row, int(snaps[row - start]), max_snapshot
                        )
                    )
                last_snapshot = max(last_snapshot, int(snaps.max()))
                candidates = _candidates(
                    snaps,
                    records[fields["FirstHaloInFOFgroup"]],
                    records[fields["MostBoundID"]],
                    start,
                )
                if bool(candidates.any()):
                    counts += np.bincount(snaps[candidates], minlength=n_snapshots)
                    dirty = True
                records = None
                start += count
            if dirty:
                found = int(counts[last_snapshot])
                if found:
                    halos += found
                    trees += 1
                counts[:] = 0
    return halos, trees


def prescan_lhalo(
    sources: Sequence[Sequence],
    layout: SourceLayout,
    schema: CanonicalSchema,
    max_snapshot: int,
    *,
    chunk_rows: int = TOPOLOGY_READ_CHUNK_ROWS,
) -> FlybyScan:
    """Count flyby-signature halos over every tree of an L-Halo source.

    ``sources`` is the ordered ``(ordinal, path)`` list the adapter is built
    from; ``layout`` and ``schema`` are the bound adapter's. ``max_snapshot``
    bounds ``SnapNum`` as the adapter does. Read-only; see the module
    docstring for the memory bound.
    """
    if isinstance(chunk_rows, bool) or not isinstance(chunk_rows, int) or chunk_rows < 1:
        raise ConverterError("chunk_rows must be a positive integer, got {!r}".format(chunk_rows))
    if isinstance(max_snapshot, bool) or not isinstance(max_snapshot, int) or max_snapshot < 0:
        raise ConverterError(
            "max_snapshot must be a non-negative integer, got {!r}".format(max_snapshot)
        )
    if schema.source_format != "lhalo_binary":
        raise ConverterError(
            "the flyby-signature pre-scan reads lhalo_binary sources, not {!r}".format(
                schema.source_format
            )
        )
    dtype, fields = _signature_dtype(layout, schema)
    halos = 0
    trees = 0
    first: Tuple[Optional[int], Optional[str]] = (None, None)
    for ordinal, path in sources:
        file_halos, file_trees = _scan_file(
            int(ordinal), Path(path), layout, dtype, fields, max_snapshot, chunk_rows
        )
        if file_halos and first[0] is None:
            first = (int(ordinal), str(path))
        halos += file_halos
        trees += file_trees
    return FlybyScan(
        halos=halos, trees=trees, first_file_ordinal=first[0], first_file_path=first[1]
    )


# ==========================================================================
# The horizontal check
# ==========================================================================


@dataclass(frozen=True)
class HorizontalFlybyScan:
    """What :func:`count_horizontal_signature` found: ``halos`` signature
    halos in snapshot ``snapshot``, the dataset's last snapshot holding halos
    (``None``, with ``halos`` 0, when no snapshot holds any)."""

    halos: int
    snapshot: Optional[int]


def count_horizontal_signature(dataset, *, block_rows: Optional[int] = None) -> HorizontalFlybyScan:
    """Count flyby-signature halos in a version 3 horizontal dataset's last
    snapshot holding halos.

    ``dataset`` is an open ``horizontal_dataset.HorizontalDataset``. Reads
    that snapshot's ``MostBoundID`` and ``FirstHaloInFOFgroup`` in blocks of
    at most ``block_rows`` rows (``None``: ``iter_columns``'s default; see the
    module docstring); every other snapshot is left unread. Read-only.

    Raises:
        ConverterError: as ``HorizontalDataset.iter_columns``, for a file
            lacking either column or a non-positive ``block_rows``.
    """
    n_halos = dataset.n_halos
    holding = [snap for snap in dataset.snapshots if n_halos[snap]]
    if not holding:
        return HorizontalFlybyScan(halos=0, snapshot=None)
    snapshot = holding[-1]
    mbid_name, fof_name = HORIZONTAL_SIGNATURE_COLUMNS
    halos = 0
    blocks = (
        dataset.iter_columns(snapshot, HORIZONTAL_SIGNATURE_COLUMNS)
        if block_rows is None
        else dataset.iter_columns(snapshot, HORIZONTAL_SIGNATURE_COLUMNS, block_rows)
    )
    for start, columns in blocks:
        most_bound_id = np.asarray(columns[mbid_name])
        first_halo_in_fof = np.asarray(columns[fof_name], dtype=np.int64)
        rows = np.arange(start, start + most_bound_id.shape[0], dtype=np.int64)
        halos += int(np.count_nonzero((most_bound_id < 0) & (first_halo_in_fof != rows)))
        columns = most_bound_id = first_halo_in_fof = rows = None
    return HorizontalFlybyScan(halos=halos, snapshot=snapshot)
