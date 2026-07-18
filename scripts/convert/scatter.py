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
workdir; ``remove_intermediate`` refuses anything else.
"""

import hashlib
import json
import os
import sys
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
    prescan_file,
)

MANIFEST_NAME = "manifest.json"
MANIFEST_VERSION = 1
#: absolute tolerance for observed scale vs canonical a_list entry
A_LIST_ATOL = 1e-4


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
        entry = self.source_entry(path)
        if entry is None or entry.get("status") != "completed":
            return False
        stat = Path(path).stat()
        return entry.get("size") == stat.st_size and entry.get("mtime_ns") == stat.st_mtime_ns


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


def _scatter_worker(args) -> FileScatterResult:
    path, src_index, scratch_dir, forest_map, chunksize = args
    return scatter_one_file(path, src_index, scratch_dir, forest_map, chunksize)


def run_scatter(
    tree_files: Sequence,
    forests_list_path,
    a_list_path,
    workdir,
    pool_size: int = 1,
    chunksize: int = 1_000_000,
    simulation_info_path=None,
) -> Manifest:
    """Phase 0 + Phase 1: map, scatter, concat, aggregates, manifest.

    Re-running skips source files whose manifest entry is completed and whose
    size/mtime still match. Per-file conservation (independent pre-count ==
    scattered rows) is enforced before a completion is recorded.
    """
    workdir = Path(workdir).resolve()
    scratch_dir = workdir / "scratch"
    workdir.mkdir(parents=True, exist_ok=True)
    scratch_dir.mkdir(parents=True, exist_ok=True)

    tree_files = [Path(p) for p in tree_files]
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
        # recorded for provenance only; not consumed until later slices
        manifest.data["provenance"]["simulation_info"] = {
            "path": str(Path(simulation_info_path).resolve()),
            "md5": file_md5(simulation_info_path),
        }

    pending = [
        (path, i) for i, path in enumerate(tree_files) if not manifest.source_completed(path)
    ]
    if pending and manifest.data["snapshots"]:
        raise ConverterError(
            "source file(s) changed after snapshots were finalized ({} pending: {}); "
            "downstream snapshot products would be stale — refusing to resume, "
            "use a fresh workdir".format(len(pending), [str(p) for p, _ in pending[:3]])
        )

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
        manifest.save()

    if pool_size <= 1 or len(pending) <= 1:
        for path, src_index in pending:
            record(scatter_one_file(path, src_index, scratch_dir, forest_map, chunksize))
    else:
        args = [(path, i, scratch_dir, forest_map, chunksize) for path, i in pending]
        with Pool(processes=min(pool_size, len(pending))) as pool:
            for result in pool.imap_unordered(_scatter_worker, args):
                record(result)

    _finalize_scatter(manifest, tree_files, forest_map, scratch_dir)
    return manifest


def _finalize_scatter(
    manifest: Manifest, tree_files: Sequence[Path], forest_map: ForestMap, scratch_dir: Path
) -> None:
    """Coverage validation, per-snapshot concat, aggregate merge, sidecar table."""
    entries = []
    for path in tree_files:
        entry = manifest.source_entry(path)
        if entry is None or entry.get("status") != "completed":
            raise ConverterError("source file incomplete after scatter: {}".format(path))
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
        if snap_entry and snap_entry.get("status") == "sorted":
            # a crash between concat-status save and worker deletion can leave
            # a snapshot that was later sorted with workers still on disk —
            # verify the sorted artifacts and finish the interrupted cleanup
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
