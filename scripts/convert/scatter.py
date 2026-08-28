"""Phase 0 pre-pass and Phase 1 scatter for the ctrees -> snapshot-HDF5 converter.

Phase 0 streams ``forests.list`` into a sorted tree-root-id -> forest-id map and
assigns the dense run-scoped ForestIndex by ascending ctrees forest id (the
reference enumeration rule; see docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md Phase 0).
Phase 1 scatters ctrees ASCII files into per-snapshot scratch binaries in the
frozen record dtype, with a resume manifest, per-forest max-snapshot
aggregates, and observed (SnapNum, scale) pairs cross-validated against the
canonical a_list.

Cleanup discipline (plan review finding 8): the converter never deletes source
data. Deletion is restricted to manifest-owned intermediates located under the
workdir; ``remove_intermediate`` refuses anything else. The downstream stages
consume their predecessors through ``Manifest.consume_intermediates``, which is
opt-in per run (plan Slice 8) and routes every deletion through that same guard.
"""

import hashlib
import json
import os
import sys
import time
from dataclasses import dataclass
from multiprocessing import Pool
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import (  # noqa: E402
    DTYPE_TAG,
    RECORD_DTYPE,
    ConverterError,
    CtreesFileParser,
)

MANIFEST_NAME = "manifest.json"
MANIFEST_VERSION = 1
#: absolute tolerance for observed scale vs canonical a_list entry
A_LIST_ATOL = 1e-4
#: default scatter manifest save policy (item 7): bounds the worst-case
#: re-scatter after an unclean interruption to at most this many completed
#: source files. The plan's measured ground truth puts the parent-side
#: inter-completion interval at production scale at ~40 s (serial, 4.23 GB
#: average source file at ~105 MB/s) to ~108.5 s (pooled, 39.0 MB/s scatter
#: aggregate throughput) -- any finite default well below that would make
#: the time arm fire on every completion and silently reproduce the
#: per-file-save cost this slice removes. The time arm is therefore
#: disabled by default (infinite) and the count arm alone bounds worst-case
#: re-work; an operator who wants a time-boxed ceiling can still pass
#: save_every_seconds explicitly.
DEFAULT_SAVE_EVERY_N_FILES = 25
DEFAULT_SAVE_EVERY_SECONDS = float("inf")

#: source-file lifecycle states for the batched interleaved transfer (item 3).
#: ``completed`` and ``consumed`` are the only two that are ever written into
#: the manifest; ``pending`` and ``deferred`` are classified per run from the
#: frozen inventory plus what is on disk right now, and are deliberately never
#: persisted — a deferred entry becomes pending the moment its bytes arrive,
#: so recording it would create a stale state somebody has to remember to
#: clear. ``deferred`` (bytes not transferred yet) and ``consumed`` (bytes
#: released after a verified scatter) are the two different reasons a source
#: file can be legitimately absent; keeping them distinct is the whole point
#: of batch mode, because only one of them means "nothing was processed".
SOURCE_COMPLETED = "completed"
SOURCE_CONSUMED = "consumed"
SOURCE_DEFERRED = "deferred"
SOURCE_PENDING = "pending"
#: the two states that mean "this source file's scatter is on the record"
SOURCE_SATISFIED = (SOURCE_COMPLETED, SOURCE_CONSUMED)


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def snapshot_scratch_name(snap: int) -> str:
    return "snap_{:03d}.bin".format(snap)


def worker_scratch_name(snap: int, src_index: int) -> str:
    return "snap_{:03d}.src_{}.bin".format(snap, src_index)


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

    def __init__(self, workdir):
        self.workdir = Path(workdir).resolve()
        self.path = self.workdir / MANIFEST_NAME
        self.data = {
            "manifest_version": MANIFEST_VERSION,
            "dtype_tag": DTYPE_TAG,
            "source_files": {},
            "intermediates": {},
            "snapshots": {},
            "observed_pairs": [],
            "provenance": {},
        }

    @classmethod
    def load_or_create(cls, workdir) -> "Manifest":
        manifest = cls(workdir)
        if manifest.path.exists():
            with open(manifest.path) as handle:
                data = json.load(handle)
            if data.get("manifest_version") != MANIFEST_VERSION:
                raise ConverterError(
                    "{}: manifest version {} != supported {}".format(
                        manifest.path, data.get("manifest_version"), MANIFEST_VERSION
                    )
                )
            if data.get("dtype_tag") != DTYPE_TAG:
                raise ConverterError(
                    "{}: manifest dtype tag mismatch (manifest: {!r}; current: {!r}); "
                    "the scratch record dtype is frozen — refusing to resume".format(
                        manifest.path, data.get("dtype_tag"), DTYPE_TAG
                    )
                )
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
        Scratch files holding structured records must carry the frozen
        DTYPE_TAG (plan Slice 2: 'recorded in every scratch-file manifest
        entry'); other binaries record their own dtype."""
        resolved = Path(path).resolve()
        entry = {"kind": kind, "status": "present", "md5": file_md5(resolved)}
        if rows is not None:
            entry["rows"] = rows
        if dtype_tag is not None:
            entry["dtype_tag"] = dtype_tag
        self.data["intermediates"][str(resolved)] = entry

    def remove_intermediate(self, path) -> None:
        """Verify-then-delete under the containment guard (plan Slice 3):
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
        instead of failing on a stat or a checksum (plan Slice 8)."""
        entry = self.data["intermediates"].get(str(Path(path).resolve()))
        return entry is not None and entry.get("status") == "removed"

    def consume_intermediates(self, paths, *, delete: bool) -> List[Path]:
        """Finish the delete-after-verify protocol for intermediates whose
        terminal consumer is done with them (plan Slice 8 deletion table).

        Callers reach this in one of exactly two states: with the successor
        artifact already re-read, verified, registered and saved -- the
        predecessor half of the protocol stated in the plan's *Conventions* --
        or with an artifact that has no consumer at all, and so no successor to
        wait for (``links``' unreachable id indexes). Each path is resolved to
        exactly one of four outcomes:

        * no manifest entry, or an entry already recorded ``removed`` -- nothing
          to do, which is what makes a re-run idempotent;
        * registered ``present`` but absent on disk -- a crash landed between
          the unlink and the manifest save, so the record converges on
          ``removed``. That happens whether or not ``delete`` is set: the bytes
          are gone already and the only open question is whether the manifest
          says so;
        * registered ``present``, on disk, ``delete`` set -- deleted through
          :meth:`remove_intermediate`, which is the only path to an unlink;
        * registered ``present``, on disk, ``delete`` clear -- retained, so the
          flag-off workdir keeps every intermediate it keeps today.

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

        A ``consumed`` entry satisfies resume without touching the filesystem:
        its scatter completed, its intermediates were verified, and its bytes
        were then deliberately released, so there is nothing left to stat and
        re-scattering it could never be correct. A ``completed`` entry still
        requires the bytes on disk to be the bytes that were scattered, which
        is why this stats the path — callers must therefore only ask about a
        ``completed`` entry whose file is present (see ``classify_source``,
        which owns that decision).
        """
        entry = self.source_entry(path)
        if entry is None:
            return False
        status = entry.get("status")
        if status == SOURCE_CONSUMED:
            return True
        if status != SOURCE_COMPLETED:
            return False
        stat = Path(path).stat()
        return entry.get("size") == stat.st_size and entry.get("mtime_ns") == stat.st_mtime_ns

    def classify_source(self, path, batch_mode: bool = False) -> str:
        """Classify one entry of the frozen source inventory for this run.

        Returns one of ``SOURCE_COMPLETED``, ``SOURCE_CONSUMED``,
        ``SOURCE_PENDING`` (present and still to scatter) or
        ``SOURCE_DEFERRED`` (batch mode only: bytes not transferred yet), and
        raises for every state that must not be silently absorbed.

        The two absences batch mode exists to tell apart are decided here, and
        only from recorded state — never from a stat failure:

        * no recorded entry and no bytes on disk is **deferred**: this file has
          not been transferred yet. Outside batch mode it stays the hard error
          it has always been.
        * a ``consumed`` entry is a file whose bytes were released *after* an
          explicit, verified ``release``. It is never stat-ed and never
          re-scattered, in either mode.
        * a ``completed`` entry whose bytes are gone was deleted without going
          through ``release``, so nothing verified that its intermediates
          survived. That is refused rather than inferred to be consumed —
          consumption is an operator action, not a conclusion drawn from a
          missing file.

        A ``completed`` entry whose bytes are present but whose size/mtime
        differ is refused in batch mode. Outside batch mode it deliberately
        keeps the pre-existing behaviour — it becomes pending, and the
        "changed after snapshots were finalized" guard in ``run_scatter``
        rejects it — because that guard is unreachable in batch mode (a
        batch-mode scatter never finalizes, so ``snapshots`` stays empty for
        the whole cycle) and silent substitution has to stay impossible in
        both modes.
        """
        candidate = Path(path)
        entry = self.source_entry(candidate)
        status = None if entry is None else entry.get("status")
        if status == SOURCE_CONSUMED:
            return SOURCE_CONSUMED
        present = candidate.exists()
        if status == SOURCE_COMPLETED:
            if not present:
                raise ConverterError(
                    "completed source file is missing from disk and was never released: "
                    "{} — its bytes were deleted without 'release', so nothing verified "
                    "that the intermediates it produced are still intact; restore the file "
                    "or use a fresh workdir".format(candidate)
                )
            if self.source_completed(candidate):
                return SOURCE_COMPLETED
            if batch_mode:
                stat = candidate.stat()
                raise ConverterError(
                    "completed source file changed on disk: {} (size {} -> {}, mtime_ns "
                    "{} -> {}); the recorded scatter describes different bytes — refusing "
                    "to resume, use a fresh workdir".format(
                        candidate,
                        entry.get("size"),
                        stat.st_size,
                        entry.get("mtime_ns"),
                        stat.st_mtime_ns,
                    )
                )
            return SOURCE_PENDING
        if present:
            return SOURCE_PENDING
        if batch_mode:
            return SOURCE_DEFERRED
        raise ConverterError("input tree file does not exist: {}".format(candidate))

    def source_intermediates(self, entry: dict) -> List[Tuple[Path, str]]:
        """Every intermediate one completed source entry produced, derived from
        the entry itself rather than guessed from the scratch directory
        listing: the two per-source sidecars plus one worker-scratch file per
        snapshot the file contributed rows to."""
        scratch_dir = self.workdir / "scratch"
        src_index = entry["src_index"]
        owned = [
            (scratch_dir / "roots_src_{}.npy".format(src_index), "observed-roots sidecar"),
            (scratch_dir / "forest_max_src_{}.npy".format(src_index), "forest-max-snap sidecar"),
        ]
        for snap_str in sorted(entry["per_snapshot_counts"], key=int):
            owned.append(
                (
                    scratch_dir / worker_scratch_name(int(snap_str), src_index),
                    "worker scratch for snapshot {}".format(snap_str),
                )
            )
        return owned

    def mark_source_consumed(self, path) -> dict:
        """Record a completed source file as consumed: an explicit operator
        transition saying its bytes may now be released.

        This is the only way an entry becomes ``consumed``. It refuses
        anything that is not ``completed``, and it verifies every intermediate
        that entry produced *before* recording the transition — that
        verification is the whole value of the command, because it is what
        makes the operator's subsequent deletion of irreplaceable source bytes
        safe. The converter itself never deletes source data (see the module
        docstring); this records permission, nothing more.

        Because that verification is the whole value, it is never skipped: a
        source whose intermediates finalization has already deleted is
        **refused**, which makes releasing before finalizing a requirement
        rather than merely the documented habit. Releasing a source whose bytes
        the operator has already deleted still works — what must verify is the
        intermediates, not the source.
        """
        resolved = Path(path).resolve()
        entry = self.source_entry(resolved)
        if entry is None:
            raise ConverterError(
                "refusing to release {}: not a recorded source file of this conversion".format(
                    resolved
                )
            )
        status = entry.get("status")
        if status == SOURCE_CONSUMED:
            raise ConverterError(
                "refusing to release {}: already recorded as {!r} by an earlier release, so "
                "its bytes may already be deleted".format(resolved, SOURCE_CONSUMED)
            )
        if status != SOURCE_COMPLETED:
            raise ConverterError(
                "refusing to release {}: source entry status is {!r}, not {!r} — only a "
                "completed scatter may be released".format(resolved, status, SOURCE_COMPLETED)
            )
        if resolved.exists():
            stat = resolved.stat()
            if entry.get("size") != stat.st_size or entry.get("mtime_ns") != stat.st_mtime_ns:
                raise ConverterError(
                    "refusing to release {}: on-disk size/mtime ({}, {}) no longer match the "
                    "scattered source ({}, {}) — releasing would authorize deleting bytes "
                    "this conversion never processed".format(
                        resolved,
                        stat.st_size,
                        stat.st_mtime_ns,
                        entry.get("size"),
                        entry.get("mtime_ns"),
                    )
                )
        for candidate, what in self.source_intermediates(entry):
            registered = self.data["intermediates"].get(str(candidate.resolve()))
            if registered is None:
                raise ConverterError(
                    "refusing to release {}: {} {} was never registered as an "
                    "intermediate".format(resolved, what, candidate)
                )
            if registered.get("status") == "removed":
                # Finalization has already deleted this artifact, and nothing
                # on the release path can stand in for it. The rows moved into
                # the concatenated snapshot, which ``source_intermediates``
                # does not name and cannot name usefully: that artifact is
                # itself deleted by the sort stage (sort_index.py:116,129) and
                # superseded in turn by the sorted, fixed and link files, each
                # deleted by a different downstream module. Verifying "the
                # successor" from here would mean encoding every downstream
                # stage's artifact lifetime in the scatter module, and it would
                # drift the moment another slice changes one.
                #
                # So this is a refusal, not a skip. Releasing here would tell
                # the operator to delete irreplaceable source bytes while
                # nothing had verified the artifact that now holds those rows —
                # the exact opposite of what release exists for. Release a
                # batch before finalizing, which is the documented order.
                raise ConverterError(
                    "refusing to release {}: {} {} was already deleted by finalization, so "
                    "nothing on the release path can still verify the rows this source "
                    "contributed — release a batch BEFORE finalizing it, not after".format(
                        resolved, what, candidate
                    )
                )
            self.verify_intermediate(candidate, what)
        entry["status"] = SOURCE_CONSUMED
        return entry


def verify_or_consumed(manifest: Manifest, path, what: str, consumed: List[str]) -> None:
    """Verify one skip-trusted artifact unless the manifest records it as
    deliberately consumed by a later stage.

    A consumed artifact is the pipeline's own doing, not a missing file: with
    consumption enabled the fix-up stage removes ``snap_NNN_sorted.bin`` once
    the fixed output is registered, and the link stage removes ``snap_NNN.idx``
    once the snapshot below it is linked (plan Slice 8 deletion table). Any
    skip-trust path that re-verifies one of those has to skip and name what was
    consumed, not fail on a stat or a checksum — deletion is bounded by re-run
    reachability, and this is what keeps those paths reachable. Anything the
    manifest still records as present is verified exactly as before.

    It lives here rather than in the stage that first needed it because three
    modules now share it — ``sort_one_snapshot``'s skip-trust path and both of
    ``_finalize_scatter``'s — and a second formulation of a rule this load
    bearing is how the two would drift apart. ``consumed`` accumulates one
    ready-to-log phrase per consumed artifact, so the caller decides how to
    report the skip.
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
) -> FileScatterResult:
    """Scatter one ctrees file into per-snapshot worker binaries.

    Worker files are opened with 'wb' so a re-run after a crash truncates
    partial output instead of appending to it.
    """
    path = Path(path)
    scratch_dir = Path(scratch_dir)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    parser = CtreesFileParser(path, chunksize=chunksize)

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
            for snap in np.unique(records["snap"]).tolist():
                part = records[records["snap"] == snap]
                if snap not in handles:
                    handles[snap] = open(scratch_dir / worker_scratch_name(snap, src_index), "wb")
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
    )


#: per-worker-process forest map (item 4): set once by ``_init_scatter_worker``
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


def _scatter_worker(args: Tuple[Path, int, Path, int]) -> FileScatterResult:
    path, src_index, scratch_dir, chunksize = args
    if _worker_forest_map is None:
        raise ConverterError(
            _worker_init_error
            or "worker forest map was never initialized — _init_scatter_worker did not "
            "run or did not set it before this task started"
        )
    return scatter_one_file(path, src_index, scratch_dir, _worker_forest_map, chunksize)


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
    batch_mode: bool = False,
) -> Manifest:
    """Phase 0 + Phase 1: map, scatter, concat, aggregates, manifest.

    Re-running skips source files whose manifest entry is completed and whose
    size/mtime still match. Per-file conservation (independent pre-count ==
    scattered rows) is enforced before a completion is recorded.

    Completed source entries accumulate in the in-memory manifest and are
    persisted on a bounded-interval policy (``save_every_n_files`` and
    ``save_every_seconds``, whichever is reached first) rather than after
    every source file, plus one unconditional save once the dispatch loop
    finishes and before ``_finalize_scatter`` runs. ``save_every_seconds``
    defaults to infinite (disabled): at production scale the parent sees a
    completion only every ~40-108 s (the plan's measured ground truth), so
    any finite default in that range would make the time arm fire on every
    completion and silently reproduce the per-file-save cost this policy
    exists to remove — only ``save_every_n_files`` (default 25) bounds the
    default worst-case re-work. Pass ``save_every_seconds`` explicitly for a
    time-boxed durability ceiling. Worker-scratch and sidecar artifacts are
    always durably written before they are registered, so an interruption
    between saves leaves at most unsaved-but-written artifacts on disk —
    never a manifest entry naming something absent. A re-scatter of the
    owning source file overwrites those deterministically.

    ``batch_mode`` (default off) supports the interleaved consumptive
    transfer, where the source is never all local at once. ``tree_files``
    still carries the **complete** ordered inventory on every invocation —
    the operator passes all of it, not the subset currently on disk — so the
    frozen-source-set guard keeps comparing like with like and still refuses
    a genuinely different conversion. What changes is that an inventory entry
    whose bytes are absent is classified rather than rejected: not
    transferred yet is ``deferred`` and skipped for now, already scattered
    and explicitly released is ``consumed`` and satisfies resume. A
    batch-mode run therefore scatters whatever has arrived, reports how many
    entries remain deferred, and returns **without finalizing**; the operator
    finalizes explicitly with ``run_finalize`` once nothing is deferred.
    Outside batch mode nothing here changes, including the hard error for a
    missing source file.
    """
    workdir = Path(workdir).resolve()
    scratch_dir = workdir / "scratch"
    workdir.mkdir(parents=True, exist_ok=True)
    scratch_dir.mkdir(parents=True, exist_ok=True)

    tree_files = [Path(p) for p in tree_files]
    if not batch_mode:
        # unchanged non-batch behaviour, deliberately kept ahead of the
        # metadata loads and the provenance guards so a missing input still
        # fails with exactly this message before anything else is checked
        for path in tree_files:
            if not path.exists():
                raise ConverterError("input tree file does not exist: {}".format(path))

    a_list, a_list_md5 = load_a_list(a_list_path)
    forest_map = load_forests_list(forests_list_path)

    manifest = Manifest.load_or_create(workdir)

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

    # classify the whole frozen inventory: which entries still need scattering,
    # and (batch mode only) which have simply not been transferred yet
    pending = []
    deferred = []
    for i, path in enumerate(tree_files):
        state = manifest.classify_source(path, batch_mode=batch_mode)
        if state == SOURCE_PENDING:
            pending.append((path, i))
        elif state == SOURCE_DEFERRED:
            deferred.append(path)
    if pending and manifest.data["snapshots"]:
        raise ConverterError(
            "source file(s) changed after snapshots were finalized ({} pending: {}); "
            "downstream snapshot products would be stale — refusing to resume, "
            "use a fresh workdir".format(len(pending), [str(p) for p, _ in pending[:3]])
        )

    # bounded-interval manifest persistence (item 7): a full manifest.json
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
                dtype_tag=DTYPE_TAG,
            )
        manifest.register_intermediate(roots_file, "observed-roots")
        manifest.register_intermediate(forest_max_file, "forest-max-snap")
        manifest.data["source_files"][result.path] = {
            "src_index": result.src_index,
            "size": result.size,
            "mtime_ns": result.mtime_ns,
            "md5": result.md5,
            "pre_count": result.pre_count,
            "parsed_count": result.parsed_count,
            "dtype_tag": DTYPE_TAG,
            "per_snapshot_counts": {str(k): v for k, v in result.per_snapshot_counts.items()},
            "per_snapshot_checksums": {str(k): v for k, v in result.per_snapshot_checksums.items()},
            "observed_pairs": [[snap, scale] for snap, scale in result.observed_pairs],
            "status": "completed",
        }
        maybe_save_manifest()

    if pool_size <= 1 or len(pending) <= 1:
        for path, src_index in pending:
            record(scatter_one_file(path, src_index, scratch_dir, forest_map, chunksize))
    else:
        # the forest map itself is never in this argument tuple (item 4): each
        # worker loads its own copy once, in _init_scatter_worker, instead of
        # the whole ForestMap being pickled into every task. forest_map.md5
        # travels alongside the path so each worker's independent load can
        # be bound to the parent's identity rather than trusted blind.
        args = [(path, i, scratch_dir, chunksize) for path, i in pending]
        with Pool(
            processes=min(pool_size, len(pending)),
            initializer=_init_scatter_worker,
            initargs=(forests_list_path, forest_map.md5),
        ) as pool:
            for result in pool.imap_unordered(_scatter_worker, args):
                record(result)

    # Save once the dispatch loop finishes cleanly, before the finalize pass
    # reads source_files back out of the manifest.
    #
    # In batch mode this must happen even when nothing was scattered. A
    # batch-mode invocation issued before any bytes have arrived is legal and
    # ordinary — it is what a scripted transfer/scatter loop does on its first
    # iteration, and what an operator smoke-testing the pipeline does — and it
    # classifies every entry as deferred, so ``pending`` is empty. If
    # provenance stayed in memory then, the frozen inventory would never reach
    # disk and the next invocation would accept a different membership or a
    # different order: the anti-mixing guard would be absent at exactly the
    # moment it is supposed to be established, which is the worst possible
    # place for it to be absent on an irreplaceable multi-day run.
    #
    # This is one whole-manifest save per batch-mode INVOCATION, not per source
    # file. That distinction is the whole of item 7: a per-file rewrite is
    # quadratic in file count (38.2 KB per entry, 104.9 MB at 2,744 files) and
    # is what the save policy above exists to remove. One extra save per
    # invocation is negligible against it.
    if pending or batch_mode:
        manifest.save()

    if batch_mode:
        # A batch-mode run must not finalize, even when nothing is deferred:
        # _finalize_scatter deletes the worker intermediates that a later
        # release has to verify, so finalizing the moment the last batch
        # completes would make that batch impossible to release. Finalization
        # is therefore reachable only through run_finalize, which the operator
        # calls once every inventory entry is completed or consumed.
        _log(
            "scatter: batch mode — scattered {} file(s), {} of {} inventory "
            "entr{} still deferred; not finalizing".format(
                len(pending),
                len(deferred),
                len(tree_files),
                "y" if len(tree_files) == 1 else "ies",
            )
        )
        return manifest

    _finalize_scatter(manifest, tree_files, forest_map, scratch_dir)
    return manifest


def _log_consumed_skip(snap: int, consumed: List[str]) -> None:
    """Report a finalize skip over a snapshot whose downstream artifacts a
    later stage consumed, naming each one. Silent when nothing was consumed,
    which is every run that did not enable consumptive deletion."""
    if consumed:
        _log(
            "finalize: snapshot {} is already past concat and {} — skipping".format(
                snap, "; ".join(consumed)
            )
        )


def _finalize_scatter(
    manifest: Manifest, tree_files: Sequence[Path], forest_map: ForestMap, scratch_dir: Path
) -> None:
    """Coverage validation, per-snapshot concat, aggregate merge, sidecar table."""
    entries = []
    for path in tree_files:
        entry = manifest.source_entry(path)
        status = None if entry is None else entry.get("status")
        if status not in SOURCE_SATISFIED:
            # a consumed entry is as final as a completed one — its scatter is
            # on the record and everything read below comes from registered
            # intermediates, never from the source bytes, so releasing them
            # cannot make finalization less correct. Anything else, including a
            # deferred entry (no recorded status at all), is refused.
            raise ConverterError(
                "source file incomplete after scatter: {} (status {!r}, expected one "
                "of {})".format(path, status, list(SOURCE_SATISFIED))
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

    # ForestID sidecar table data (emitted as forests.h5 by the Slice 7 writer)
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
            # a snapshot the Slice 5 fix-up stage already completed: verify
            # all downstream artifacts and finish any interrupted cleanup.
            # The sorted file and the index are both in the Slice 8 deletion
            # table, so either may legitimately be gone by now; the fixed file
            # is not consumable while this status holds (the writer takes it
            # only once every snapshot is linked), so it is verified outright.
            consumed: List[str] = []
            verify_or_consumed(
                manifest, snap_entry["sorted_file"], "sorted snapshot scratch", consumed
            )
            verify_or_consumed(manifest, snap_entry["index_file"], "snapshot id index", consumed)
            manifest.verify_intermediate(snap_entry["fixed_file"], "fixed snapshot scratch")
            _retry_worker_cleanup(manifest, scratch_dir, snap, parts)
            manifest.save()
            _log_consumed_skip(snap, consumed)
            continue
        if snap_entry and snap_entry.get("status") == "sorted":
            # a crash between concat-status save and worker deletion can leave
            # a snapshot that was later sorted with workers still on disk —
            # verify the sorted artifacts and finish the interrupted cleanup.
            # Both are verified OUTRIGHT here, on the same rule the sort stage
            # applies at this status: neither can have been consumed yet, since
            # fixups saves ``fixed`` before it removes the sorted file and links
            # will not start until every snapshot is at least ``fixed``. A
            # manifest claiming otherwise describes a premature deletion.
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
        with open(target, "wb") as out:
            for p in parts:
                worker_path = scratch_dir / worker_scratch_name(snap, p["src_index"])
                manifest.verify_intermediate(worker_path, "worker scratch input")
                data = np.fromfile(worker_path, dtype=RECORD_DTYPE)
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
            target, "snapshot-scratch", rows=rows_written, dtype_tag=DTYPE_TAG
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
# Batch-mode operator actions (item 3)
# ---------------------------------------------------------------------------


def run_release(workdir, tree_files: Sequence) -> Manifest:
    """Record completed source files as consumed so their bytes may be deleted.

    This is the explicit operator action that makes deletion of irreplaceable
    source bytes safe: every named file must be ``completed``, and every
    intermediate its scatter produced must still verify against its registered
    checksum, before the transition is recorded. The converter never deletes
    source data itself — the deletion stays with the operator or the transfer
    script (see the module docstring).

    The whole invocation is atomic against the manifest on disk: every file is
    verified and transitioned in memory first and the manifest is saved once,
    so a refusal on any named file leaves the persisted manifest untouched.
    """
    workdir = Path(workdir).resolve()
    manifest = Manifest.load_or_create(workdir)
    released = []
    for path in tree_files:
        manifest.mark_source_consumed(path)
        released.append(Path(path).resolve())
    manifest.save()
    for path in released:
        _log(
            "release: {} recorded as {}; its bytes may now be deleted".format(path, SOURCE_CONSUMED)
        )
    return manifest


def run_finalize(workdir, forests_list_path) -> Manifest:
    """Explicit Phase 1 finalize for a batch-mode conversion.

    Outside batch mode ``run_scatter`` still finalizes automatically; this is
    the step that replaces that for a batched run, and it refuses to run while
    any inventory entry is still deferred. The inventory it checks is the
    frozen ordered source list already recorded in provenance — there is no
    new artifact and no second copy of the list to keep in step — and the
    forest map is re-loaded here because root-coverage validation needs it,
    bound to the identity recorded at first run exactly as ``run_scatter``
    binds it.
    """
    workdir = Path(workdir).resolve()
    scratch_dir = workdir / "scratch"
    manifest = Manifest.load_or_create(workdir)
    recorded = manifest.data["provenance"]
    inventory = recorded.get("source_files")
    if not inventory:
        raise ConverterError(
            "{}: no scatter provenance recorded in this workdir; there is nothing to "
            "finalize — run scatter first".format(workdir)
        )
    forest_map = load_forests_list(forests_list_path)
    recorded_forests = recorded.get("forests_list") or {}
    if recorded_forests.get("md5") != forest_map.md5:
        raise ConverterError(
            "forests_list content changed since this workdir was created ({} != {}); "
            "refusing to finalize — use a fresh workdir".format(
                forest_map.md5, recorded_forests.get("md5")
            )
        )
    tree_files = [Path(p) for p in inventory]
    deferred = [
        path
        for path in tree_files
        if manifest.classify_source(path, batch_mode=True) == SOURCE_DEFERRED
    ]
    if deferred:
        raise ConverterError(
            "refusing to finalize: {} of {} inventory entr{} {} still deferred (not "
            "transferred and not scattered); examples: {}".format(
                len(deferred),
                len(tree_files),
                "y" if len(tree_files) == 1 else "ies",
                "is" if len(deferred) == 1 else "are",
                [str(p) for p in deferred[:3]],
            )
        )
    _finalize_scatter(manifest, tree_files, forest_map, scratch_dir)
    manifest.save()
    _log(
        "finalize: {} inventory entr{} finalized".format(
            len(tree_files), "y" if len(tree_files) == 1 else "ies"
        )
    )
    return manifest
