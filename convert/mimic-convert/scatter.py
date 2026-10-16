"""Phase 0 pre-pass and Phase 1 scatter for the ctrees -> horizontal-HDF5 converter.

Phase 0 streams ``forests.list`` into a sorted tree-root-id -> forest-id map and
assigns the dense run-scoped ForestIndex by ascending ctrees forest id, the
same enumeration order the vertical Consistent-Trees ASCII reader uses.
Phase 1 scatters ctrees ASCII files into per-snapshot scratch binaries in the
frozen record dtype, with a resume manifest, per-forest max-snapshot
aggregates, and observed (SnapNum, scale) pairs cross-validated against the
canonical a_list.

Cleanup discipline: the converter never deletes source
data. Deletion is restricted to manifest-owned intermediates located under the
workdir; ``remove_intermediate`` refuses anything else. The sort stage
consumes the unsorted snapshot scratch through
``Manifest.consume_intermediates``, which routes every deletion through that
same guard.

**Scratch layout**. A workdir holds one
``ctrees_parser.ScratchLayout`` for its whole life: the frozen 108-byte legacy
record, used when no schema is given, or the extended record of the canonical
ASCII bridge (source coordinates plus declared extras), which the version 3
route always uses. ``run_scatter`` fixes it from its ``schema`` argument when
the manifest is created; every later stage derives it from the manifest, and
any caller that names a different layout -- including a caller naming no
schema against an extended workdir -- is refused before anything is mutated.
An extended workdir records a dtype tag the frozen ``DTYPE_TAG`` comparison of
older code rejects, so a converter that predates it can never resume it.

Every scratch file handle this module opens is buffered explicitly at
:data:`SCRATCH_BUFFER_BYTES`, in the serial and the pooled scatter alike, so
the buffer never scales with a filesystem's ``st_blksize`` across the up to
one-handle-per-snapshot a source file can hold open.

**A declared cut table** (extended layout only; ``run_scatter``'s
``cut_table_path``) changes nothing scatter itself produces: ``forests.list``
stays the partition every record's ``forest_id`` is joined from, because the
fix-up stage resolves hosts under it. Scatter checks the table's roots against
``forests.list`` before any file is read, binds the table's md5 into the
workdir's provenance, and records each file's per-tree row counts
(:func:`tree_rows_name`). Once the fix-up stage has resolved every host,
:func:`install_cut` recomputes the forest table and the per-file units
sidecars under the table, replacing the original partition's; the per-forest
maximum-snapshot aggregate, an input of the fix-up stage's own guard, keeps
the original partition.
"""

import hashlib
import json
import os
import sys
import time
from dataclasses import dataclass
from multiprocessing import Pool
from pathlib import Path
from types import SimpleNamespace
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import (  # noqa: E402
    DTYPE_TAG,
    LEGACY_LAYOUT,
    ConverterError,
    CtreesFileParser,
    ScratchLayout,
    plan_source_units,
)

MANIFEST_NAME = "manifest.json"
#: Bumped 1 -> 2 when fix_flybys was removed: the frozen record dtypes are
#: unchanged (no new columns), so DTYPE_TAG/FIXED_DTYPE_TAG cannot detect
#: this change on their own. A workdir manifested under version 1 may hold
#: fixed/linked snapshots with the pre-removal MostBoundID sign convention;
#: resuming into it would skip re-verifying those snapshots (fix_one_snapshot
#: trusts recorded checksums, not content semantics) and let a stale
#: intermediate reach an emitted dataset. Refusing the resume outright forces a
#: fresh workdir instead.
MANIFEST_VERSION = 2
#: absolute tolerance for observed scale vs canonical a_list entry
A_LIST_ATOL = 1e-4
#: default scatter manifest save policy: bounds the worst-case
#: re-scatter after an unclean interruption to at most this many completed
#: source files. Measurements put the parent-side
#: inter-completion interval at production scale at ~40 s (serial, 4.23 GB
#: average source file at ~105 MB/s) to ~108.5 s (pooled, 39.0 MB/s scatter
#: aggregate throughput) -- any finite default well below that would make
#: the time arm fire on every completion and silently reproduce the
#: per-file-save cost the count arm avoids. The time arm is therefore
#: disabled by default (infinite) and the count arm alone bounds worst-case
#: re-work; an operator who wants a time-boxed ceiling can still pass
#: save_every_seconds explicitly.
DEFAULT_SAVE_EVERY_N_FILES = 25
DEFAULT_SAVE_EVERY_SECONDS = float("inf")

#: Explicit buffer size of every scratch file handle this module opens (8192
#: bytes, pooled execution included). Default buffering follows the
#: filesystem's st_blksize, which on Lustre and some APFS volumes is large
#: enough that one handle per snapshot costs far more than intended.
SCRATCH_BUFFER_BYTES = 8192

#: source-file states. ``completed`` is the only one ever written into the
#: manifest: it means this source file's scatter is on the record. ``pending``
#: (present and still to scatter) is classified per run from the frozen
#: inventory plus what is on disk right now, and is deliberately never
#: persisted.
SOURCE_COMPLETED = "completed"
SOURCE_PENDING = "pending"


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def snapshot_scratch_name(snap: int) -> str:
    return "snap_{:03d}.bin".format(snap)


def worker_scratch_name(snap: int, src_index: int) -> str:
    return "snap_{:03d}.src_{}.bin".format(snap, src_index)


def source_units_name(src_index: int) -> str:
    """Per-source ``[n_units, 2]`` (forest id, halo count) sidecar of an
    extended-layout scatter: that file's canonical source units."""
    return "units_src_{}.npy".format(src_index)


def tree_rows_name(src_index: int) -> str:
    """Per-source ``[n_trees]`` row counts of a scatter under a cut table, one
    per ``#tree`` marker in file order (aligned with ``roots_src_N.npy``)."""
    return "tree_rows_src_{}.npy".format(src_index)


def open_scratch(path, mode: str = "wb"):
    """Open one scratch file with the explicit :data:`SCRATCH_BUFFER_BYTES`
    buffer; the single place this module opens a scratch handle for writing."""
    return open(path, mode, buffering=SCRATCH_BUFFER_BYTES)


def id_checksum(ids: np.ndarray, running: int = 0) -> int:
    """Order-independent checksum over int64 ids: XOR of their uint64 views."""
    if ids.size:
        running ^= int(np.bitwise_xor.reduce(ids.astype(np.int64, copy=False).view(np.uint64)))
    return running


def file_md5(path, blocksize: int = 8 * 1024 * 1024) -> str:
    """Streamed md5 of a file's contents (intermediate ownership checksum)."""
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(blocksize)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


# ---------------------------------------------------------------------------
# Phase 0: forests.list map and dense ForestIndex
# ---------------------------------------------------------------------------


@dataclass
class ForestMap:
    """Sorted tree-root-id -> forest-id map plus the dense ForestIndex table."""

    tree_root_ids: np.ndarray  # int64, sorted ascending
    forest_ids: np.ndarray  # int64, aligned with tree_root_ids
    unique_forest_ids: np.ndarray  # int64, sorted ascending; position = ForestIndex
    #: md5 of the exact bytes the map was parsed from (identity binding)
    md5: str = ""

    def lookup_forest_ids(self, roots: np.ndarray) -> np.ndarray:
        """Map tree root ids to ctrees forest ids; abort on unknown roots."""
        pos = np.searchsorted(self.tree_root_ids, roots)
        pos_clipped = np.minimum(pos, len(self.tree_root_ids) - 1)
        bad = (pos >= len(self.tree_root_ids)) | (self.tree_root_ids[pos_clipped] != roots)
        if np.any(bad):
            examples = np.asarray(roots)[bad][:5].tolist()
            raise ConverterError(
                "{} tree root id(s) not present in forests.list; examples: {}".format(
                    int(bad.sum()), examples
                )
            )
        return self.forest_ids[pos_clipped]

    def forest_index_table(self) -> np.ndarray:
        """ForestID sidecar table data: position = dense ForestIndex,
        value = ctrees forest id (ascending)."""
        return self.unique_forest_ids


def load_forests_list(path) -> ForestMap:
    """Stream forests.list ('#TreeRootID ForestID' header) into a ForestMap.
    The recorded md5 is computed from the same byte stream being parsed."""
    path = Path(path)
    roots: List[int] = []
    forests: List[int] = []
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            digest.update(raw)
            line = raw.strip()
            if not line or line.startswith(b"#"):
                continue
            parts = line.split()
            if len(parts) != 2:
                raise ConverterError(
                    "{}:{}: expected 'TreeRootID ForestID', got: {!r}".format(
                        path, lineno, line[:80].decode("utf-8", "replace")
                    )
                )
            try:
                roots.append(int(parts[0]))
                forests.append(int(parts[1]))
            except ValueError:
                raise ConverterError(
                    "{}:{}: non-integer forests.list row: {!r}".format(
                        path, lineno, line[:80].decode("utf-8", "replace")
                    )
                )
    if not roots:
        raise ConverterError("{}: no forest rows found".format(path))
    root_arr = np.asarray(roots, dtype=np.int64)
    forest_arr = np.asarray(forests, dtype=np.int64)
    order = np.argsort(root_arr, kind="stable")
    root_arr = root_arr[order]
    forest_arr = forest_arr[order]
    dup = np.nonzero(root_arr[1:] == root_arr[:-1])[0]
    if dup.size:
        raise ConverterError(
            "{}: duplicate TreeRootID entries; examples: {}".format(
                path, root_arr[dup][:5].tolist()
            )
        )
    return ForestMap(
        tree_root_ids=root_arr,
        forest_ids=forest_arr,
        unique_forest_ids=np.unique(forest_arr),
        md5=digest.hexdigest(),
    )


def validate_root_coverage(observed_roots: np.ndarray, forest_map: ForestMap) -> None:
    """Enforce one-to-one coverage between observed #tree roots and forests.list."""
    observed = np.sort(np.asarray(observed_roots, dtype=np.int64))
    dup = np.nonzero(observed[1:] == observed[:-1])[0]
    if dup.size:
        raise ConverterError(
            "duplicate #tree root id(s) observed across input files; examples: {}".format(
                observed[dup][:5].tolist()
            )
        )
    listed = forest_map.tree_root_ids
    missing_from_list = np.setdiff1d(observed, listed, assume_unique=True)
    if missing_from_list.size:
        raise ConverterError(
            "{} observed #tree root(s) missing from forests.list; examples: {}".format(
                missing_from_list.size, missing_from_list[:5].tolist()
            )
        )
    missing_from_data = np.setdiff1d(listed, observed, assume_unique=True)
    if missing_from_data.size:
        raise ConverterError(
            "{} forests.list root(s) never observed in the data; examples: {}".format(
                missing_from_data.size, missing_from_data[:5].tolist()
            )
        )


def check_cut_roots(cut_map: ForestMap, forest_map: ForestMap, cut_table_path) -> None:
    """A cut table maps exactly ``forests.list``'s tree roots, each once (the
    loader refuses a repeated root): the check that needs no halo, made
    before any source file is read. Its other invariants need the trees'
    halo totals and are checked by :func:`load_cut_table`."""
    if np.array_equal(cut_map.tree_root_ids, forest_map.tree_root_ids):
        return
    missing = np.setdiff1d(forest_map.tree_root_ids, cut_map.tree_root_ids, assume_unique=True)
    extra = np.setdiff1d(cut_map.tree_root_ids, forest_map.tree_root_ids, assume_unique=True)
    raise ConverterError(
        "{}: cut table misses {} forests.list tree root(s) (e.g. {}) and lists {} root(s) "
        "forests.list does not (e.g. {})".format(
            cut_table_path, missing.size, missing[:5].tolist(), extra.size, extra[:5].tolist()
        )
    )


# ---------------------------------------------------------------------------
# a_list
# ---------------------------------------------------------------------------


def load_a_list(path) -> Tuple[np.ndarray, str]:
    """Load the canonical a_list: exactly one scale factor per non-comment
    line, index = SnapNum. Returns (values, md5-of-the-parsed-bytes)."""
    path = Path(path)
    values: List[float] = []
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, start=1):
            digest.update(raw)
            text = raw.decode("utf-8", "replace").strip()
            if not text or text.startswith("#"):
                continue
            tokens = text.split()
            if len(tokens) != 1:
                raise ConverterError(
                    "{}:{}: a_list lines carry exactly one scale factor, got {!r}".format(
                        path, lineno, text[:40]
                    )
                )
            try:
                values.append(float(tokens[0]))
            except ValueError:
                raise ConverterError(
                    "{}:{}: non-numeric a_list line: {!r}".format(path, lineno, text[:40])
                )
    if not values:
        raise ConverterError("{}: empty a_list".format(path))
    a_list = np.asarray(values, dtype=np.float64)
    bad = ~np.isfinite(a_list)
    if bad.any():
        raise ConverterError(
            "{}: non-finite a_list entr{} at snapshot(s) {}".format(
                path, "y" if bad.sum() == 1 else "ies", np.nonzero(bad)[0][:5].tolist()
            )
        )
    return a_list, digest.hexdigest()


def validate_observed_pairs(pairs, a_list: np.ndarray, source: str) -> None:
    """Abort on any observed (SnapNum, scale) pair unknown to the a_list."""
    for snap, scale in sorted(pairs):
        if snap < 0 or snap >= len(a_list):
            raise ConverterError(
                "{}: observed snapshot {} outside a_list range [0, {})".format(
                    source, snap, len(a_list)
                )
            )
        if abs(scale - a_list[snap]) > A_LIST_ATOL:
            raise ConverterError(
                "{}: observed (SnapNum={}, scale={}) does not match a_list[{}]={} "
                "within atol {}".format(source, snap, scale, snap, a_list[snap], A_LIST_ATOL)
            )


# ---------------------------------------------------------------------------
# Resume manifest
# ---------------------------------------------------------------------------


class Manifest:
    """JSON resume manifest under the workdir; every intermediate the converter
    creates is recorded here, and cleanup refuses paths it does not own."""

    def __init__(self, workdir, layout: ScratchLayout = LEGACY_LAYOUT):
        self.workdir = Path(workdir).resolve()
        self.path = self.workdir / MANIFEST_NAME
        self.layout = layout
        self.data = {
            "manifest_version": MANIFEST_VERSION,
            "dtype_tag": layout.dtype_tag,
            "source_files": {},
            "intermediates": {},
            "snapshots": {},
            "observed_pairs": [],
            "provenance": {},
        }
        if layout.is_extended:
            # absent from a legacy manifest, so the legacy JSON is unchanged
            self.data["scratch_layout"] = layout.to_record()

    @classmethod
    def load_or_create(cls, workdir, layout: Optional[ScratchLayout] = None) -> "Manifest":
        """Load the workdir's manifest, or start a new one in memory.

        ``layout`` None derives the scratch layout from an existing manifest
        (every stage after scatter); a given ``layout`` must equal the
        recorded one exactly -- dtype tag and schema digest -- and is what a
        new manifest is created with. Nothing is written here.
        """
        manifest = cls(workdir, layout if layout is not None else LEGACY_LAYOUT)
        if manifest.path.exists():
            with open(manifest.path) as handle:
                data = json.load(handle)
            if data.get("manifest_version") != MANIFEST_VERSION:
                raise ConverterError(
                    "{}: manifest version {} != supported {}".format(
                        manifest.path, data.get("manifest_version"), MANIFEST_VERSION
                    )
                )
            if "scratch_layout" in data:
                recorded = ScratchLayout.from_record(data["scratch_layout"], str(manifest.path))
                if data.get("dtype_tag") != recorded.dtype_tag:
                    raise ConverterError(
                        "{}: manifest dtype tag {!r} disagrees with its own scratch layout "
                        "record ({!r}) — refusing to resume".format(
                            manifest.path, data.get("dtype_tag"), recorded.dtype_tag
                        )
                    )
            elif data.get("dtype_tag") != DTYPE_TAG:
                raise ConverterError(
                    "{}: manifest dtype tag mismatch (manifest: {!r}; current: {!r}); "
                    "the scratch record dtype is frozen — refusing to resume".format(
                        manifest.path, data.get("dtype_tag"), DTYPE_TAG
                    )
                )
            else:
                recorded = LEGACY_LAYOUT
            if layout is not None and layout != recorded:
                raise ConverterError(
                    "{}: this workdir holds {} scratch records (tag {!r}, schema {}), not the "
                    "requested {} layout (tag {!r}, schema {}) — a different field selection is "
                    "a different conversion; refusing to resume, use a fresh workdir".format(
                        manifest.path,
                        "extended" if recorded.is_extended else "legacy",
                        recorded.dtype_tag,
                        recorded.schema_digest,
                        "extended" if layout.is_extended else "legacy",
                        layout.dtype_tag,
                        layout.schema_digest,
                    )
                )
            manifest.layout = recorded
            manifest.data = data
        return manifest

    def save(self) -> None:
        tmp = self.path.with_suffix(".json.tmp")
        with open(tmp, "w") as handle:
            json.dump(self.data, handle, indent=2, sort_keys=True)
        os.replace(tmp, self.path)

    # -- intermediates ------------------------------------------------------

    def register_intermediate(
        self, path, kind: str, rows: Optional[int] = None, dtype_tag: Optional[str] = None
    ) -> None:
        """Record an intermediate's absolute path and content checksum — the
        frozen ownership contract every later deletion is verified against.
        Scratch files holding structured records carry the workdir's
        ``manifest.layout.dtype_tag`` (the frozen ``DTYPE_TAG`` for the legacy
        layout); other binaries record their own dtype."""
        resolved = Path(path).resolve()
        entry = {"kind": kind, "status": "present", "md5": file_md5(resolved)}
        if rows is not None:
            entry["rows"] = rows
        if dtype_tag is not None:
            entry["dtype_tag"] = dtype_tag
        self.data["intermediates"][str(resolved)] = entry

    def remove_intermediate(self, path) -> None:
        """Verify-then-delete under the containment guard:
        manifest-owned, inside the workdir, and content unchanged since
        registration — anything else is refused."""
        resolved = Path(path).resolve()
        key = str(resolved)
        entry = self.data["intermediates"].get(key)
        if entry is None or entry.get("status") != "present":
            raise ConverterError(
                "refusing to delete {}: not a manifest-owned intermediate".format(resolved)
            )
        if self.workdir not in resolved.parents:
            raise ConverterError(
                "refusing to delete {}: outside the workdir {}".format(resolved, self.workdir)
            )
        checksum = file_md5(resolved)
        if checksum != entry.get("md5"):
            raise ConverterError(
                "refusing to delete {}: content checksum {} != registered {}".format(
                    resolved, checksum, entry.get("md5")
                )
            )
        resolved.unlink()
        entry["status"] = "removed"

    def is_consumed(self, path) -> bool:
        """True when this path is a registered intermediate the manifest
        records as already removed.

        That is a deliberate consumption by a later stage, not a missing
        artifact, and the difference is what lets a skip-trust path skip
        instead of failing on a stat or a checksum."""
        entry = self.data["intermediates"].get(str(Path(path).resolve()))
        return entry is not None and entry.get("status") == "removed"

    def consume_intermediates(self, paths, *, delete: bool) -> List[Path]:
        """Finish the delete-after-verify protocol for intermediates whose
        terminal consumer is done with them.

        Callers reach this with the successor artifact already re-read,
        verified, registered and saved -- the predecessor half of the
        verify-before-consume protocol. Each path is resolved to exactly one of
        four outcomes:

        * no manifest entry, or an entry already recorded ``removed`` -- nothing
          to do, which is what makes a re-run idempotent;
        * registered ``present`` but absent on disk -- a crash landed between
          the unlink and the manifest save, so the record converges on
          ``removed``. That happens whether or not ``delete`` is set: the bytes
          are gone already and the only open question is whether the manifest
          says so;
        * registered ``present``, on disk, ``delete`` set -- deleted through
          :meth:`remove_intermediate`, which is the only path to an unlink;
        * registered ``present``, on disk, ``delete`` clear -- retained.

        The manifest is saved once, at the end, iff anything changed. Returns
        the paths whose bytes are gone as a result -- deleted here, or deleted
        by an interrupted earlier call and only now recorded.
        """
        removed: List[Path] = []
        for path in paths:
            resolved = Path(path).resolve()
            entry = self.data["intermediates"].get(str(resolved))
            if entry is None or entry.get("status") != "present":
                continue
            if not resolved.exists():
                entry["status"] = "removed"
            elif delete:
                self.remove_intermediate(resolved)
            else:
                continue
            removed.append(resolved)
        if removed:
            self.save()
        return removed

    def verify_intermediate(self, path, what: str) -> dict:
        """Verify a registered intermediate before consuming or skip-trusting
        it: manifest-owned, still on disk, and content checksum unchanged."""
        resolved = Path(path).resolve()
        entry = self.data["intermediates"].get(str(resolved))
        if entry is None or entry.get("status") != "present":
            raise ConverterError(
                "{}: {} is not a manifest-owned intermediate".format(resolved, what)
            )
        if not resolved.exists():
            raise ConverterError(
                "{}: {} is recorded in the manifest but missing on disk".format(resolved, what)
            )
        checksum = file_md5(resolved)
        if checksum != entry.get("md5"):
            raise ConverterError(
                "{}: {} content checksum {} != registered {} — refusing to use it".format(
                    resolved, what, checksum, entry.get("md5")
                )
            )
        return entry

    # -- source files -------------------------------------------------------

    def source_entry(self, path) -> Optional[dict]:
        return self.data["source_files"].get(str(Path(path).resolve()))

    def source_completed(self, path) -> bool:
        """True when this source file's scatter is already on the record and
        needs no re-scatter.

        A ``completed`` entry requires the bytes on disk to be the bytes that
        were scattered, which is why this stats the path — callers must
        therefore only ask about a ``completed`` entry whose file is present
        (see ``classify_source``, which owns that decision).
        """
        entry = self.source_entry(path)
        if entry is None:
            return False
        status = entry.get("status")
        if status != SOURCE_COMPLETED:
            return False
        stat = Path(path).stat()
        return entry.get("size") == stat.st_size and entry.get("mtime_ns") == stat.st_mtime_ns

    def classify_source(self, path) -> str:
        """Classify one entry of the frozen source inventory for this run.

        Returns ``SOURCE_COMPLETED`` or ``SOURCE_PENDING`` (present and still
        to scatter), and raises for every state that must not be silently
        absorbed:

        * a source file with no bytes on disk is refused;
        * a ``completed`` entry whose bytes are gone is refused, because
          nothing then verifies that the intermediates it produced are still
          what was scattered.

        A ``completed`` entry whose bytes are present but whose size/mtime
        differ becomes pending, and the "changed after snapshots were
        finalized" guard in ``run_scatter`` rejects it, so silent substitution
        stays impossible.
        """
        candidate = Path(path)
        entry = self.source_entry(candidate)
        status = None if entry is None else entry.get("status")
        present = candidate.exists()
        if status == SOURCE_COMPLETED:
            if not present:
                raise ConverterError(
                    "completed source file is missing from disk: {} — nothing can verify "
                    "that the intermediates it produced are still intact; restore the file "
                    "or use a fresh workdir".format(candidate)
                )
            if self.source_completed(candidate):
                return SOURCE_COMPLETED
            return SOURCE_PENDING
        if present:
            return SOURCE_PENDING
        raise ConverterError("input tree file does not exist: {}".format(candidate))

    def source_intermediates(self, entry: dict) -> List[Tuple[Path, str]]:
        """Every intermediate one completed source entry produced, derived from
        the entry itself rather than guessed from the scratch directory
        listing: the two per-source sidecars, the source-units sidecar when the
        entry records one (an extended-layout scatter; every SourceHaloID and
        source coordinate of the file derives from it), the tree-rows sidecar
        when the entry records one (a scatter under a cut table), plus one
        worker-scratch file per snapshot the file contributed rows to."""
        scratch_dir = self.workdir / "scratch"
        src_index = entry["src_index"]
        owned = [
            (scratch_dir / "roots_src_{}.npy".format(src_index), "observed-roots sidecar"),
            (scratch_dir / "forest_max_src_{}.npy".format(src_index), "forest-max-snap sidecar"),
        ]
        if "source_units" in entry:
            owned.append((scratch_dir / source_units_name(src_index), "source-units sidecar"))
        if "tree_rows" in entry:
            owned.append((scratch_dir / tree_rows_name(src_index), "tree-rows sidecar"))
        for snap_str in sorted(entry["per_snapshot_counts"], key=int):
            owned.append(
                (
                    scratch_dir / worker_scratch_name(int(snap_str), src_index),
                    "worker scratch for snapshot {}".format(snap_str),
                )
            )
        return owned


def verify_or_consumed(manifest: Manifest, path, what: str, consumed: List[str]) -> None:
    """Verify one skip-trusted artifact unless the manifest records it as
    deliberately consumed by a later stage.

    A consumed artifact is a registered intermediate the pipeline itself
    removed through :meth:`Manifest.consume_intermediates`, not a missing
    file: a skip-trust path that re-verifies one has to skip and name what was
    consumed rather than fail on a stat or a checksum. Anything the manifest
    still records as present is verified exactly as before. The sort stage's
    skip-trust path (``sort_index.sort_one_snapshot``) uses it at the statuses
    where a consumption can already have happened. ``consumed`` accumulates
    one ready-to-log phrase per consumed artifact, so the caller decides how
    to report the skip.
    """
    if manifest.is_consumed(path):
        consumed.append("its {} was consumed by a later stage ({})".format(what, path))
        return
    manifest.verify_intermediate(path, what)


# ---------------------------------------------------------------------------
# Phase 1: scatter
# ---------------------------------------------------------------------------


@dataclass
class FileScatterResult:
    """Per-source-file scatter outcome recorded into the manifest."""

    path: str
    src_index: int
    pre_count: int
    parsed_count: int
    size: int
    mtime_ns: int
    md5: str
    per_snapshot_counts: Dict[int, int]
    per_snapshot_checksums: Dict[int, int]
    observed_pairs: List[Tuple[int, float]]
    observed_roots: np.ndarray
    forest_max_snap: Dict[int, int]
    worker_files: Dict[int, str]
    #: extended layout only: ``[n_units, 2]`` (forest id, halo count)
    source_units: Optional[np.ndarray] = None
    #: extended layout only: rows per ``#tree`` marker, in file order
    tree_rows: Optional[np.ndarray] = None


def _update_forest_max(acc: Dict[int, int], forest_ids: np.ndarray, snaps: np.ndarray) -> None:
    order = np.lexsort((snaps, forest_ids))
    fsorted = forest_ids[order]
    ssorted = snaps[order]
    starts = np.nonzero(np.r_[True, fsorted[1:] != fsorted[:-1]])[0]
    group_forests = fsorted[starts]
    group_max = np.maximum.reduceat(ssorted, starts)
    for forest, snap in zip(group_forests.tolist(), group_max.tolist()):
        if acc.get(forest, -1) < snap:
            acc[forest] = snap


def scatter_one_file(
    path,
    src_index: int,
    scratch_dir,
    forest_map: ForestMap,
    chunksize: int,
    schema=None,
) -> FileScatterResult:
    """Scatter one ctrees file into per-snapshot worker binaries.

    Worker files are opened with 'wb' so a re-run after a crash truncates
    partial output instead of appending to it, and with the explicit
    :data:`SCRATCH_BUFFER_BYTES` buffer.

    With a ``schema`` the records are in its extended scratch layout and carry
    each row's canonical source coordinate: ``src_index`` is the file ordinal,
    and the units come from :func:`ctrees_parser.plan_source_units` over this
    file's pre-scan. The parsed rows' unit attribution is checked against that
    plan before the result is returned.
    """
    path = Path(path)
    scratch_dir = Path(scratch_dir)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    parser = CtreesFileParser(path, chunksize=chunksize, schema=schema)

    if (
        parser.prescan.declared_tree_count is not None
        and parser.prescan.declared_tree_count != len(parser.prescan.tree_root_ids)
    ):
        raise ConverterError(
            "{}: declared tree count {} != {} '#tree' markers observed".format(
                path, parser.prescan.declared_tree_count, len(parser.prescan.tree_root_ids)
            )
        )

    tree_forest_ids = forest_map.lookup_forest_ids(parser.prescan.tree_root_ids)
    # sorted view for the per-chunk join (markers arrive in file order)
    root_order = np.argsort(parser.prescan.tree_root_ids, kind="stable")
    sorted_roots = parser.prescan.tree_root_ids[root_order]
    sorted_forests = tree_forest_ids[root_order]
    extended = parser.scratch_layout.is_extended
    if extended:
        parser.unit_plan = plan_source_units(parser.prescan, tree_forest_ids, src_index)
        unit_rows = np.zeros(parser.unit_plan.unit_counts.size, dtype=np.int64)

    handles = {}
    counts: Dict[int, int] = {}
    checksums: Dict[int, int] = {}
    forest_max: Dict[int, int] = {}
    try:
        for records in parser.chunks():
            # join forest_id from the Phase 0 map via the file's own tree list
            pos = np.searchsorted(sorted_roots, records["tree_root_id"])
            records["forest_id"] = sorted_forests[pos]
            _update_forest_max(forest_max, records["forest_id"], records["snap"])
            if extended:
                _check_unit_rows(records, parser.unit_plan, unit_rows, path)
            for snap in np.unique(records["snap"]).tolist():
                part = records[records["snap"] == snap]
                if snap not in handles:
                    handles[snap] = open_scratch(scratch_dir / worker_scratch_name(snap, src_index))
                handles[snap].write(part.tobytes())
                counts[snap] = counts.get(snap, 0) + len(part)
                checksums[snap] = id_checksum(part["id"], checksums.get(snap, 0))
    finally:
        for handle in handles.values():
            handle.close()

    total = sum(counts.values())
    if total != parser.prescan.n_rows:
        raise ConverterError(
            "{}: scattered row total {} != independent pre-count {}".format(
                path, total, parser.prescan.n_rows
            )
        )
    if extended and not np.array_equal(unit_rows, parser.unit_plan.unit_counts):
        raise ConverterError(
            "{}: parsed rows per source unit {} disagree with the pre-scan plan {}".format(
                path, unit_rows.tolist()[:10], parser.unit_plan.unit_counts.tolist()[:10]
            )
        )
    # the source must not have changed between the pre-scan and the pandas
    # pass — marker attribution and checksum would describe a different file
    stat = path.stat()
    if stat.st_size != parser.prescan.size or stat.st_mtime_ns != parser.prescan.mtime_ns:
        raise ConverterError(
            "{}: source file changed between pre-scan and parse "
            "(size {} -> {}, mtime_ns {} -> {})".format(
                path, parser.prescan.size, stat.st_size, parser.prescan.mtime_ns, stat.st_mtime_ns
            )
        )
    return FileScatterResult(
        path=str(path.resolve()),
        src_index=src_index,
        pre_count=parser.prescan.n_rows,
        parsed_count=parser.result.n_rows_parsed,
        size=parser.prescan.size,
        mtime_ns=parser.prescan.mtime_ns,
        md5=parser.prescan.md5,
        per_snapshot_counts=counts,
        per_snapshot_checksums=checksums,
        observed_pairs=sorted(parser.result.observed_pairs),
        observed_roots=parser.prescan.tree_root_ids.copy(),
        forest_max_snap=forest_max,
        worker_files={
            snap: str((scratch_dir / worker_scratch_name(snap, src_index)).resolve())
            for snap in counts
        },
        source_units=parser.unit_plan.table() if extended else None,
        tree_rows=(
            np.diff(np.r_[parser.prescan.tree_start_rows, np.int64(parser.prescan.n_rows)])
            if extended
            else None
        ),
    )


def _check_unit_rows(records: np.ndarray, plan, unit_rows: np.ndarray, path: Path) -> None:
    """Accumulate one chunk's rows per source unit, and check every row's
    coordinate lies inside its unit and its forest is the unit's forest --
    the parsed attribution agreeing with the pre-scan plan it came from."""
    units = records["src_unit_ordinal"]
    n_units = plan.unit_counts.size
    bad = (units < 0) | (units >= n_units)
    if not bad.any():
        rows = records["src_row_ordinal"]
        bad = (rows < 0) | (rows >= plan.unit_counts[units])
        bad |= plan.unit_forest_ids[units] != records["forest_id"]
    if bad.any():
        row = int(np.nonzero(bad)[0][0])
        raise ConverterError(
            "{}: record id {} has source coordinate (unit {}, row {}) outside the pre-scan "
            "plan of {} unit(s)".format(
                path,
                int(records["id"][row]),
                int(units[row]),
                int(records["src_row_ordinal"][row]),
                n_units,
            )
        )
    unit_rows += np.bincount(units, minlength=n_units)


#: per-worker-process forest map: set once by ``_init_scatter_worker``
#: rather than pickled into every task's argument tuple. ``None`` in the
#: parent process, in any process that never ran the pool initializer, and
#: in a worker whose independent load disagreed with the parent's (see
#: ``_worker_init_error`` below).
_worker_forest_map: Optional[ForestMap] = None

#: set by ``_init_scatter_worker`` instead of ``_worker_forest_map`` when
#: this worker's own load does not match the parent's; ``_scatter_worker``
#: turns this into a deterministic ``ConverterError`` on the next task.
_worker_init_error: Optional[str] = None


def _init_scatter_worker(forests_list_path: str, expected_md5: str) -> None:
    """``Pool`` initializer: load the forest map once per worker process,
    and bind it to the parent's identity.

    The start method is ``spawn`` on this host (the platform default since
    Python 3.8, not set anywhere in this package), so worker globals are not
    fork-inherited and each worker must load its own copy; the same
    initializer works unchanged under a ``fork`` default. Loading from
    ``forests_list_path`` introduces no new on-disk representation.

    Before this initializer existed, the parent loaded ``forests.list``
    exactly once and pickled that one object to every worker, so divergence
    between the parent's and a worker's view of the map was impossible by
    construction. Now there are N+1 independent loads of the same path at
    N+1 different moments, which opens a window for the file to change
    between them; ``ForestMap.md5`` is the identity binding
    ``load_forests_list`` already computes for exactly this purpose, and
    this initializer is required to consult it rather than trust an
    unconditioned independent load.

    This function is total: no exception may escape it, for any reason. An
    uncaught exception inside a ``Pool`` initializer does not fail the pool
    promptly — CPython keeps replacing the dead worker instead — so letting
    ANY loader failure (not just an md5 mismatch on an otherwise-parseable
    file, but a truncated, emptied, unlinked or unreadable
    ``forests.list`` too) escape here would turn a rare data-integrity risk
    into a silent respawn-loop hang on a multi-day run, with
    ``imap_unordered`` never receiving either a result or an error. Every
    failure mode — a mismatch or any exception ``load_forests_list`` raises
    — is therefore recorded in ``_worker_init_error`` instead, and surfaced
    deterministically by ``_scatter_worker`` on its next task, where an
    exception propagates to the parent through the normal ``Pool`` result
    path. Do not narrow the ``except Exception`` below to specific loader
    exceptions: that would silently reopen the hang for any failure mode
    not on the narrowed list.
    """
    global _worker_forest_map, _worker_init_error
    # reset both globals unconditionally at entry, before any work: this
    # worker process must not depend on starting from pristine module state
    # (e.g. under worker reuse or a fork default, where prior globals could
    # otherwise linger)
    _worker_forest_map = None
    _worker_init_error = None
    try:
        loaded = load_forests_list(forests_list_path)
        if loaded.md5 != expected_md5:
            _worker_init_error = (
                "{}: worker-loaded forests.list content (md5 {}) does not match the "
                "parent's (md5 {}) — the file changed between the parent's load and this "
                "worker's independent load; refusing to scatter with a possibly divergent "
                "forest map".format(forests_list_path, loaded.md5, expected_md5)
            )
            return
        _worker_forest_map = loaded
    except Exception as exc:  # noqa: BLE001 -- deliberately total, see docstring
        _worker_init_error = "{}: worker failed to load forests.list: {!r}".format(
            forests_list_path, exc
        )


def _scatter_worker(args: Tuple) -> FileScatterResult:
    """Pool task: ``(path, src_index, scratch_dir, chunksize, schema)``.

    ``schema`` is the extended layout's schema, or ``None`` for the legacy
    layout; it travels with the task (a small frozen object) so a pooled
    worker scatters into the same layout, through the same
    :func:`scatter_one_file`, as the serial path.
    """
    path, src_index, scratch_dir, chunksize, schema = args
    if _worker_forest_map is None:
        raise ConverterError(
            _worker_init_error
            or "worker forest map was never initialized — _init_scatter_worker did not "
            "run or did not set it before this task started"
        )
    return scatter_one_file(
        path, src_index, scratch_dir, _worker_forest_map, chunksize, schema=schema
    )


def run_scatter(
    tree_files: Sequence,
    forests_list_path,
    a_list_path,
    workdir,
    pool_size: int = 1,
    chunksize: int = 1_000_000,
    simulation_info_path=None,
    save_every_n_files: int = DEFAULT_SAVE_EVERY_N_FILES,
    save_every_seconds: float = DEFAULT_SAVE_EVERY_SECONDS,
    schema=None,
    cut_table_path=None,
) -> Manifest:
    """Phase 0 + Phase 1: map, scatter, concat, aggregates, manifest.

    ``schema`` (default None: the frozen legacy scratch layout) is a
    ``consistent_trees_ascii`` canonical schema; it selects the columns and
    fixes the workdir's extended scratch layout (see the module docstring).
    A resume must pass the same schema, or none for a legacy workdir.

    ``cut_table_path`` (extended layout only; default None, which changes
    nothing) names a cut forest table in ``forests.list`` shape. Before any
    source file is read, its tree roots must be exactly ``forests.list``'s;
    its md5 is then bound into the provenance (a resume must name the same
    table, or none when none was named), and every scattered file's per-tree
    row counts are recorded (:func:`tree_rows_name`) for the table's
    invariant check and :func:`install_cut`. The records themselves keep the
    original forest ids. Loading the table costs what loading ``forests.list``
    does (the parse's Python lists, then 24 B per tree of sorted int64
    columns), and it is released before any file is scattered.

    Re-running skips source files whose manifest entry is completed and whose
    size/mtime still match. Per-file conservation (independent pre-count ==
    scattered rows) is enforced before a completion is recorded.

    Completed source entries accumulate in the in-memory manifest and are
    persisted on a bounded-interval policy (``save_every_n_files`` and
    ``save_every_seconds``, whichever is reached first) rather than after
    every source file, plus one unconditional save once the dispatch loop
    finishes and before ``_finalize_scatter`` runs. ``save_every_seconds``
    defaults to infinite (disabled): at production scale the parent sees a
    completion only every ~40-108 s (measured), so
    any finite default in that range would make the time arm fire on every
    completion and silently reproduce the per-file-save cost this policy
    exists to remove — only ``save_every_n_files`` (default 25) bounds the
    default worst-case re-work. Pass ``save_every_seconds`` explicitly for a
    time-boxed durability ceiling. Worker-scratch and sidecar artifacts are
    always durably written before they are registered, so an interruption
    between saves leaves at most unsaved-but-written artifacts on disk —
    never a manifest entry naming something absent. A re-scatter of the
    owning source file overwrites those deterministically.

    Every source file must be on disk: a missing one is refused before any
    metadata is loaded.
    """
    workdir = Path(workdir).resolve()
    scratch_dir = workdir / "scratch"
    workdir.mkdir(parents=True, exist_ok=True)
    scratch_dir.mkdir(parents=True, exist_ok=True)

    tree_files = [Path(p) for p in tree_files]
    # deliberately ahead of the metadata loads and the provenance guards, so a
    # missing input fails with exactly this message before anything else is
    # checked
    for path in tree_files:
        if not path.exists():
            raise ConverterError("input tree file does not exist: {}".format(path))

    a_list, a_list_md5 = load_a_list(a_list_path)
    forest_map = load_forests_list(forests_list_path)
    cut_identity = None
    if cut_table_path is not None:
        if schema is None:
            raise ConverterError(
                "a cut table is a canonical ASCII route option; the legacy layout takes none"
            )
        cut_map = load_forests_list(cut_table_path)
        check_cut_roots(cut_map, forest_map, cut_table_path)
        cut_identity = {"path": str(Path(cut_table_path).resolve()), "md5": cut_map.md5}
        del cut_map

    layout = LEGACY_LAYOUT if schema is None else ScratchLayout.from_schema(schema)
    manifest = Manifest.load_or_create(workdir, layout=layout)

    # bind the manifest to its input identities: a resumed run must see the
    # same canonical metadata content and the same ordered source set it was
    # started with — anything else is a different conversion, not a resume.
    # Both digests come from the exact bytes the loaders parsed.
    identities = {
        "a_list": {"path": str(Path(a_list_path).resolve()), "md5": a_list_md5},
        "forests_list": {
            "path": str(Path(forests_list_path).resolve()),
            "md5": forest_map.md5,
        },
        "source_files": [str(p.resolve()) for p in tree_files],
    }
    recorded = manifest.data["provenance"]
    for key in ("a_list", "forests_list"):
        if key in recorded and recorded[key].get("md5") != identities[key]["md5"]:
            raise ConverterError(
                "{} content changed since this workdir was created ({} != {}); "
                "refusing to resume — use a fresh workdir".format(
                    key, identities[key]["md5"], recorded[key].get("md5")
                )
            )
    if "source_files" in recorded and recorded["source_files"] != identities["source_files"]:
        raise ConverterError(
            "source file set/order changed since this workdir was created; "
            "refusing to resume — use a fresh workdir"
        )
    if recorded:
        # an existing workdir: the cut table (or its absence) is part of the
        # conversion, never something a resume adds, drops or swaps
        recorded_md5 = (recorded.get("cut_table") or {}).get("md5")
        given_md5 = None if cut_identity is None else cut_identity["md5"]
        if recorded_md5 != given_md5:
            raise ConverterError(
                "cut table md5 {} differs from the {} this workdir was created with; refusing "
                "to resume — use a fresh workdir".format(given_md5, recorded_md5)
            )
    if cut_identity is not None:
        identities["cut_table"] = cut_identity
    manifest.data["provenance"].update(identities)
    if simulation_info_path is not None:
        # immutable once recorded: the fix-up stage derives Len from this
        # file's particle mass, so silently replacing the recorded identity
        # could mix particle-mass provenance across snapshots
        sim_md5 = file_md5(simulation_info_path)
        recorded_info = recorded.get("simulation_info")
        if recorded_info is not None and recorded_info.get("md5") != sim_md5:
            raise ConverterError(
                "simulation_info content changed since this workdir was created ({} != {}); "
                "refusing to resume — use a fresh workdir".format(sim_md5, recorded_info.get("md5"))
            )
        if recorded_info is None:
            manifest.data["provenance"]["simulation_info"] = {
                "path": str(Path(simulation_info_path).resolve()),
                "md5": sim_md5,
            }

    # classify the whole frozen inventory: which entries still need scattering
    pending = []
    for i, path in enumerate(tree_files):
        if manifest.classify_source(path) == SOURCE_PENDING:
            pending.append((path, i))
    if pending and manifest.data["snapshots"]:
        raise ConverterError(
            "source file(s) changed after snapshots were finalized ({} pending: {}); "
            "downstream snapshot products would be stale — refusing to resume, "
            "use a fresh workdir".format(len(pending), [str(p) for p, _ in pending[:3]])
        )

    # bounded-interval manifest persistence: a full manifest.json
    # rewrite after every completed source file is quadratic in file count,
    # so only a policy-bounded number of completions accumulate in memory
    # between saves. The worker/sidecar artifacts a completion registers are
    # always durably written before record() is called, so a save deferred
    # this way never lets the manifest name something absent from disk.
    save_every_n_files = max(1, int(save_every_n_files))
    pending_since_save = 0
    last_save_monotonic = time.monotonic()

    def maybe_save_manifest() -> None:
        nonlocal pending_since_save, last_save_monotonic
        pending_since_save += 1
        elapsed = time.monotonic() - last_save_monotonic
        if pending_since_save >= save_every_n_files or elapsed >= save_every_seconds:
            manifest.save()
            pending_since_save = 0
            last_save_monotonic = time.monotonic()

    def record(result: FileScatterResult) -> None:
        validate_observed_pairs(result.observed_pairs, a_list, result.path)
        roots_file = scratch_dir / "roots_src_{}.npy".format(result.src_index)
        np.save(roots_file, result.observed_roots)
        forest_max_file = scratch_dir / "forest_max_src_{}.npy".format(result.src_index)
        forest_max_arr = np.array(sorted(result.forest_max_snap.items()), dtype=np.int64).reshape(
            -1, 2
        )
        np.save(forest_max_file, forest_max_arr)
        for snap, worker_path in result.worker_files.items():
            manifest.register_intermediate(
                worker_path,
                "worker-scratch",
                rows=result.per_snapshot_counts[snap],
                dtype_tag=layout.dtype_tag,
            )
        manifest.register_intermediate(roots_file, "observed-roots")
        manifest.register_intermediate(forest_max_file, "forest-max-snap")
        if result.source_units is not None:
            units_file = scratch_dir / source_units_name(result.src_index)
            np.save(units_file, result.source_units)
            manifest.register_intermediate(
                units_file, "source-units", rows=int(result.source_units.shape[0])
            )
        if cut_identity is not None:
            tree_rows_file = scratch_dir / tree_rows_name(result.src_index)
            np.save(tree_rows_file, result.tree_rows)
            manifest.register_intermediate(
                tree_rows_file, "tree-rows", rows=int(result.tree_rows.size)
            )
        manifest.data["source_files"][result.path] = {
            "src_index": result.src_index,
            "size": result.size,
            "mtime_ns": result.mtime_ns,
            "md5": result.md5,
            "pre_count": result.pre_count,
            "parsed_count": result.parsed_count,
            "dtype_tag": layout.dtype_tag,
            "per_snapshot_counts": {str(k): v for k, v in result.per_snapshot_counts.items()},
            "per_snapshot_checksums": {str(k): v for k, v in result.per_snapshot_checksums.items()},
            "observed_pairs": [[snap, scale] for snap, scale in result.observed_pairs],
            "status": "completed",
        }
        if result.source_units is not None:
            # absent from a legacy entry, so the legacy JSON is unchanged; it
            # is what names the sidecar among the entry's intermediates
            # (source_intermediates)
            manifest.data["source_files"][result.path]["source_units"] = int(
                result.source_units.shape[0]
            )
        if cut_identity is not None:
            manifest.data["source_files"][result.path]["tree_rows"] = int(result.tree_rows.size)
        maybe_save_manifest()

    if pool_size <= 1 or len(pending) <= 1:
        for path, src_index in pending:
            record(
                scatter_one_file(path, src_index, scratch_dir, forest_map, chunksize, schema=schema)
            )
    else:
        # the forest map itself is never in this argument tuple: each
        # worker loads its own copy once, in _init_scatter_worker, instead of
        # the whole ForestMap being pickled into every task. forest_map.md5
        # travels alongside the path so each worker's independent load can
        # be bound to the parent's identity rather than trusted blind.
        args = [(path, i, scratch_dir, chunksize, schema) for path, i in pending]
        with Pool(
            processes=min(pool_size, len(pending)),
            initializer=_init_scatter_worker,
            initargs=(forests_list_path, forest_map.md5),
        ) as pool:
            for result in pool.imap_unordered(_scatter_worker, args):
                record(result)

    # Save once the dispatch loop finishes cleanly, before the finalize pass
    # reads source_files back out of the manifest.
    if pending:
        manifest.save()

    _finalize_scatter(manifest, tree_files, forest_map, scratch_dir)
    return manifest


def _finalize_scatter(
    manifest: Manifest, tree_files: Sequence[Path], forest_map: ForestMap, scratch_dir: Path
) -> None:
    """Coverage validation, per-snapshot concat, aggregate merge, sidecar table."""
    entries = []
    for path in tree_files:
        entry = manifest.source_entry(path)
        status = None if entry is None else entry.get("status")
        if status != SOURCE_COMPLETED:
            raise ConverterError(
                "source file incomplete after scatter: {} (status {!r}, expected "
                "{!r})".format(path, status, SOURCE_COMPLETED)
            )
        entries.append(entry)

    all_roots = []
    for entry in entries:
        roots_file = scratch_dir / "roots_src_{}.npy".format(entry["src_index"])
        manifest.verify_intermediate(roots_file, "observed-roots sidecar")
        all_roots.append(np.load(roots_file))
    validate_root_coverage(np.concatenate(all_roots), forest_map)

    def write_table_once(path: Path, kind: str, build) -> None:
        """Registered tables are never silently overwritten: an existing entry
        is verified (tampering aborts) and kept; only a fresh path is built."""
        existing = manifest.data["intermediates"].get(str(path.resolve()))
        if existing is not None and existing.get("status") == "present":
            manifest.verify_intermediate(path, kind)
            return
        np.save(path, build())
        manifest.register_intermediate(path, kind)

    # merge per-forest max-snapshot aggregates (scientific input to Phase 3)
    def build_forest_max() -> np.ndarray:
        merged: Dict[int, int] = {}
        for entry in entries:
            sidecar = scratch_dir / "forest_max_src_{}.npy".format(entry["src_index"])
            manifest.verify_intermediate(sidecar, "forest-max-snap sidecar")
            for forest, snap in np.load(sidecar).tolist():
                if merged.get(forest, -1) < snap:
                    merged[forest] = snap
        return np.array(sorted(merged.items()), dtype=np.int64).reshape(-1, 2)

    write_table_once(
        Path(manifest.workdir) / "forest_max_snap.npy", "forest-max-snap-merged", build_forest_max
    )

    # ForestID sidecar table data (emitted as forests.h5 by the writer). Under
    # a cut table, once every snapshot is fixed the table belongs to
    # install_cut, which may have been interrupted between replacing it and
    # saving the manifest; install_cut itself verifies or completes it.
    recorded_snapshots = manifest.data["snapshots"].values()
    installing = (
        manifest.data["provenance"].get("cut_table") is not None
        and bool(recorded_snapshots)
        and all(entry.get("status") == "fixed" for entry in recorded_snapshots)
    )
    if not installing:
        write_table_once(
            Path(manifest.workdir) / "forest_index_table.npy",
            "forest-index-table",
            forest_map.forest_index_table,
        )

    # observed pairs union
    pairs = set()
    for entry in entries:
        pairs.update((int(s), float(a)) for s, a in entry["observed_pairs"])
    manifest.data["observed_pairs"] = [[snap, scale] for snap, scale in sorted(pairs)]

    # per-snapshot concat in ascending src_index order
    snapshots: Dict[int, List[dict]] = {}
    for entry in sorted(entries, key=lambda e: e["src_index"]):
        for snap_str, count in entry["per_snapshot_counts"].items():
            snapshots.setdefault(int(snap_str), []).append(
                {
                    "src_index": entry["src_index"],
                    "count": count,
                    "checksum": entry["per_snapshot_checksums"][snap_str],
                }
            )

    for snap, parts in sorted(snapshots.items()):
        snap_entry = manifest.data["snapshots"].get(str(snap))
        target = scratch_dir / snapshot_scratch_name(snap)
        if snap_entry and snap_entry.get("status") == "fixed":
            # a snapshot the fix-up stage already completed: verify
            # all downstream artifacts and finish any interrupted cleanup.
            manifest.verify_intermediate(snap_entry["sorted_file"], "sorted snapshot scratch")
            manifest.verify_intermediate(snap_entry["index_file"], "snapshot id index")
            manifest.verify_intermediate(snap_entry["fixed_file"], "fixed snapshot scratch")
            _retry_worker_cleanup(manifest, scratch_dir, snap, parts)
            manifest.save()
            continue
        if snap_entry and snap_entry.get("status") == "sorted":
            # a crash between concat-status save and worker deletion can leave
            # a snapshot that was later sorted with workers still on disk —
            # verify the sorted artifacts and finish the interrupted cleanup.
            manifest.verify_intermediate(snap_entry["sorted_file"], "sorted snapshot scratch")
            manifest.verify_intermediate(snap_entry["index_file"], "snapshot id index")
            _retry_worker_cleanup(manifest, scratch_dir, snap, parts)
            manifest.save()
            continue
        if snap_entry and snap_entry.get("status") == "concatenated":
            # skip-trusting a prior concat requires verifying it, then
            # retrying any cleanup a crash may have left unfinished
            manifest.verify_intermediate(target, "concatenated snapshot scratch")
            _retry_worker_cleanup(manifest, scratch_dir, snap, parts)
            manifest.save()
            continue
        expected_rows = sum(p["count"] for p in parts)
        expected_checksum = 0
        for p in parts:
            expected_checksum ^= p["checksum"]
        rows_written = 0
        checksum = 0
        with open_scratch(target) as out:
            for p in parts:
                worker_path = scratch_dir / worker_scratch_name(snap, p["src_index"])
                worker_entry = manifest.verify_intermediate(worker_path, "worker scratch input")
                if worker_entry.get("dtype_tag") != manifest.layout.dtype_tag:
                    raise ConverterError(
                        "{}: worker scratch dtype tag {!r} != this workdir's {!r}".format(
                            worker_path, worker_entry.get("dtype_tag"), manifest.layout.dtype_tag
                        )
                    )
                data = np.fromfile(worker_path, dtype=manifest.layout.dtype)
                if len(data) != p["count"]:
                    raise ConverterError(
                        "{}: worker file has {} rows, manifest records {}".format(
                            worker_path, len(data), p["count"]
                        )
                    )
                out.write(data.tobytes())
                rows_written += len(data)
                checksum = id_checksum(data["id"], checksum)
        if rows_written != expected_rows or checksum != expected_checksum:
            raise ConverterError(
                "snapshot {}: concat mismatch (rows {} vs {}, checksum {} vs {})".format(
                    snap, rows_written, expected_rows, checksum, expected_checksum
                )
            )
        manifest.register_intermediate(
            target, "snapshot-scratch", rows=rows_written, dtype_tag=manifest.layout.dtype_tag
        )
        manifest.data["snapshots"][str(snap)] = {
            "rows": rows_written,
            "id_checksum": checksum,
            "scratch_file": str(target.resolve()),
            "status": "concatenated",
        }
        manifest.save()
        for p in parts:
            manifest.remove_intermediate(scratch_dir / worker_scratch_name(snap, p["src_index"]))
        manifest.save()


def _retry_worker_cleanup(manifest: Manifest, scratch_dir: Path, snap: int, parts) -> None:
    """Finish worker-file deletion a crash may have interrupted. A registered
    entry whose file is already gone is the intended end state of a cleanup
    that crashed between unlink and manifest save — record it as removed."""
    for p in parts:
        worker_path = (scratch_dir / worker_scratch_name(snap, p["src_index"])).resolve()
        entry = manifest.data["intermediates"].get(str(worker_path))
        if entry is None or entry.get("status") != "present":
            continue
        if worker_path.exists():
            manifest.remove_intermediate(worker_path)
        else:
            entry["status"] = "removed"


# ---------------------------------------------------------------------------
# A declared cut table
# ---------------------------------------------------------------------------


def _scattered_trees(manifest: Manifest) -> List[Tuple[int, np.ndarray, np.ndarray]]:
    """``(src_index, roots, rows)`` per source file, in inventory order: each
    ``#tree`` marker's root and row count in file order, from the verified
    observed-roots and tree-rows sidecars. A file's rows must sum to its
    parsed count."""
    scratch_dir = Path(manifest.workdir) / "scratch"
    trees = []
    for path in manifest.data["provenance"]["source_files"]:
        entry = manifest.source_entry(path)
        if entry is None or entry.get("status") != SOURCE_COMPLETED or "tree_rows" not in entry:
            raise ConverterError(
                "{}: source file {} has no scatter recorded under the cut table".format(
                    manifest.path, path
                )
            )
        src_index = int(entry["src_index"])
        roots_file = scratch_dir / "roots_src_{}.npy".format(src_index)
        rows_file = scratch_dir / tree_rows_name(src_index)
        manifest.verify_intermediate(roots_file, "observed-roots sidecar")
        manifest.verify_intermediate(rows_file, "tree-rows sidecar")
        roots = np.load(roots_file)
        rows = np.load(rows_file)
        if roots.shape != rows.shape or int(rows.sum()) != int(entry["parsed_count"]):
            raise ConverterError(
                "{}: {} tree row count(s) summing to {} for {} tree root(s) and {} parsed "
                "halo(s)".format(
                    rows_file, rows.size, int(rows.sum()), roots.size, entry["parsed_count"]
                )
            )
        trees.append((src_index, roots, rows))
    return trees


def load_cut_table(manifest: Manifest, forests_list_path, cut_table_path) -> ForestMap:
    """The workdir's cut table, every invariant checked, as the map the fix-up
    stage and :func:`install_cut` relabel from.

    The table must be the one the provenance binds, and ``forests.list`` the
    original partition it binds. The invariants are the census's
    (``census.cut_table.read_cut_table``): every tree root once, a piece holds
    trees of one forest, an uncut forest keeps its id, the largest piece of a
    cut forest keeps its id (by halo totals over every snapshot, ties by the
    smallest root), and fresh ids ascend contiguously from the catalogue's
    maximum in descending piece size. The totals are the scattered per-tree
    row counts, so this runs after scatter and before any later stage.

    Memory: the census reader's parse and check, the two maps (24 B per tree
    each), and the per-file roots and row counts with their concatenations,
    the sort order and the ordered totals (48 B per tree), all resident
    together. Measured with ``tracemalloc`` around this call on synthetic
    two-file workdirs of one-halo trees: a peak of 197 B per tree at 100,000
    and at 200,000 trees (19.7 and 39.4 MB), plus the constant 8 MiB block
    the md5 reads use.
    """
    from census.cut_table import read_cut_table

    provenance = manifest.data["provenance"]
    forest_map = load_forests_list(forests_list_path)
    recorded_cut = provenance.get("cut_table") or {}
    if forest_map.md5 != provenance.get("forests_list", {}).get("md5"):
        raise ConverterError(
            "{}: forests.list md5 {} is not the one this workdir was scattered under".format(
                forests_list_path, forest_map.md5
            )
        )
    cut_md5 = file_md5(cut_table_path)
    if cut_md5 != recorded_cut.get("md5"):
        raise ConverterError(
            "{}: cut table md5 {} is not the {} this workdir was scattered under".format(
                cut_table_path, cut_md5, recorded_cut.get("md5")
            )
        )
    trees = _scattered_trees(manifest)
    roots = np.concatenate([tree_roots for _src, tree_roots, _rows in trees])
    rows = np.concatenate([tree_rows for _src, _roots, tree_rows in trees])
    order = np.argsort(roots, kind="stable")
    if not np.array_equal(roots[order], forest_map.tree_root_ids):
        raise ConverterError(  # pragma: no cover - finalize's coverage check refuses it first
            "{}: the scattered tree roots are not forests.list's".format(manifest.path)
        )
    table_roots, table_ids = read_cut_table(
        cut_table_path, forest_map.tree_root_ids, forest_map.forest_ids, rows[order]
    )
    return ForestMap(
        tree_root_ids=table_roots,
        forest_ids=table_ids,
        unique_forest_ids=np.unique(table_ids),
        md5=recorded_cut["md5"],
    )


def _install_table(manifest: Manifest, path: Path, kind: str, table: np.ndarray, **record):
    """Replace one registered table with ``table``, atomically. A file that
    already holds ``table`` (the identity cut, or a resumed install) is kept
    and only re-registered if a crash came between its replacement and the
    manifest save; anything else must be the registered original."""
    path = Path(path)
    if path.exists():
        current = np.load(path)
        if current.dtype == table.dtype and np.array_equal(current, table):
            registered = manifest.data["intermediates"].get(str(path.resolve()), {})
            if registered.get("md5") != file_md5(path):
                manifest.register_intermediate(path, kind, **record)
            return
    manifest.verify_intermediate(path, kind)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as handle:
        np.save(handle, table)
    os.replace(tmp, path)
    manifest.register_intermediate(path, kind, **record)


def install_cut(manifest: Manifest, cut_map: ForestMap) -> None:
    """Recompute the forest table and every units sidecar under the cut table,
    once the fix-up stage has resolved every host under the original
    partition and relabelled every record's ``forest_id`` through its
    ``tree_root_id``.

    ``forest_index_table.npy`` becomes the table's ascending forest ids, so a
    fresh piece enumerates after every original forest. Each file's
    ``units_src_N.npy`` becomes its pieces as units: a piece's ordinal is its
    rank among the file's pieces by its first ``#tree`` marker in the file
    (``ctrees_parser.plan_source_units`` over the recorded marker roots and
    tree row counts), its count the piece's rows in the file. The original
    partition's versions are replaced, never kept beside these, so the link
    stage and the adapter read the cut partition only. The fixed scratch
    records keep the source coordinates scatter stamped under the original
    partition's units; nothing reads them after scatter's own attribution
    check. Idempotent: a resumed install keeps what it already wrote, and an
    install interrupted between a table's replacement and the manifest save
    is completed here (``_finalize_scatter`` leaves the forest table to this
    function once every snapshot is fixed).

    Memory: one file's roots, row counts, starts and forest ids (32 B per tree
    of the file) and its unit plan, besides the cut map.
    """
    snapshots = manifest.data["snapshots"]
    unfixed = sorted(int(s) for s, e in snapshots.items() if e.get("status") != "fixed")
    if unfixed:
        raise ConverterError(
            "{}: snapshot(s) {} are not fixed; the cut is installed once every host is "
            "resolved".format(manifest.path, unfixed[:5])
        )
    scratch_dir = Path(manifest.workdir) / "scratch"
    for src_index, roots, rows in _scattered_trees(manifest):
        starts = np.cumsum(rows) - rows
        markers = SimpleNamespace(
            path=str(scratch_dir / tree_rows_name(src_index)),
            tree_start_rows=starts,
            n_rows=int(rows.sum()),
        )
        plan = plan_source_units(markers, cut_map.lookup_forest_ids(roots), src_index)
        table = plan.table()
        _install_table(
            manifest,
            scratch_dir / source_units_name(src_index),
            "source-units",
            table,
            rows=int(table.shape[0]),
        )
    _install_table(
        manifest,
        Path(manifest.workdir) / "forest_index_table.npy",
        "forest-index-table",
        cut_map.forest_index_table(),
    )
    manifest.save()
