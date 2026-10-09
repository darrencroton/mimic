"""The output workdir, the scratch directory and the restart state.

**The output workdir** is converter-shaped so that ``convert_trees.py
validate`` and ``report`` bind to it through their existing entry points:

- ``manifest.json``: a generic conversion manifest
  (``conversion_manifest.ConversionManifest``) created before the first
  output byte, pinning every input artefact (the a_list, metadata, profile,
  index files and conversion record by content, the input dataset's files by
  path, size, mtime and inode) and freezing the rewriter's configuration; its
  ``ingest``, ``transpose`` and ``write`` stages are completed at the end with
  the counts the battery checks (per-snapshot counts, the halo total, the link
  counts, zero gapped descendants, a maximum span of 1, ``links_adjacent`` 1),
  the ``write`` stage registering ``dataset/`` and every file in it by SHA-256;
- ``dataset/``: the snapshot files and ``forests.h5`` and nothing else;
- beside the manifest: ``tree_inventory.npy``, ``rewrite_state.json`` (the
  restart state below) and ``rewrite_summary.json``.

**The scratch directory** holds pass 0's results and pass 1's per-slab remap
entries and checkpoints, each written to a temporary name, fsynced and
renamed, and each recorded with its SHA-256 in the restart state before
anything depends on it. Only the slabs the next step needs are kept.

**The restart state** records which slabs are written and verified (with
each file's SHA-256), which scratch files exist (with theirs), and the
manifest's configuration digest. ``--resume`` loads the manifest, requires
the newly prepared configuration to equal the recorded one, re-pins every
dependency (``ConversionManifest.verify_dependencies``: a changed input --
even one of the same size -- is refused), re-verifies the scratch files it
continues from, and continues after the last verified slab; a completed run
is re-verified and exits.
"""

import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Mapping, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from conversion_manifest import (  # noqa: E402
    MANIFEST_NAME,
    ConversionManifest,
    canonical_json,
    classify_manifest,
    sha256_file,
)
from errors import ConverterError  # noqa: E402
from rewriter import (  # noqa: E402
    DATASET_DIR,
    REWRITER_VERSION,
    STATE_NAME,
    SUMMARY_NAME,
    TREE_INVENTORY_NAME,
)
from rewriter.inputs import Prepared  # noqa: E402
from rewriter.passes import SlabRemap  # noqa: E402

STATE_VERSION = 1
SCRATCH_BINDING_NAME = "rewriter_binding.json"

STATUS_RUNNING = "running"
STATUS_COMPLETE = "complete"

_REMAP_PARTS = ("perm", "forest", "rank", "sid")
_CHECKPOINT_PARTS = ("running", "tree_counts", "labels")
_PASS0_PARTS = ("totals", "roots", "root_snapshot")


def _fsync_directory(path: Path) -> None:
    try:
        descriptor = os.open(str(path), os.O_RDONLY)
    except OSError:  # pragma: no cover - platforms without directory fds
        return
    try:
        os.fsync(descriptor)
    except OSError:  # pragma: no cover - filesystems refusing directory fsync
        pass
    finally:
        os.close(descriptor)


def write_json_atomic(path: Path, document: Mapping) -> None:
    """Write JSON to a sibling temporary file, fsync, rename, fsync the
    directory: a crash leaves the previous file or this one."""
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w") as handle:
        handle.write(json.dumps(document, indent=2, sort_keys=True, allow_nan=False) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)
    _fsync_directory(path.parent)


def read_json(path: Path, what: str) -> dict:
    try:
        with open(path) as handle:
            document = json.load(handle)
    except (OSError, ValueError) as exc:
        raise ConverterError("{}: {} is not readable JSON ({})".format(path, what, exc)) from exc
    if not isinstance(document, dict):
        raise ConverterError("{}: {} is not a JSON object".format(path, what))
    return document


def save_array_atomic(path: Path, array: np.ndarray) -> str:
    """``np.save`` to a temporary name, fsync, rename; returns the SHA-256."""
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "wb") as handle:
        np.save(handle, np.ascontiguousarray(array))
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)
    _fsync_directory(path.parent)
    return sha256_file(path)


def load_array(path: Path, sha256: str, what: str) -> np.ndarray:
    """Load an ``.npy`` whose content SHA-256 must be the recorded one."""
    if not path.is_file():
        raise ConverterError("{}: {} is missing; the run cannot resume from it".format(path, what))
    held = sha256_file(path)
    if held != sha256:
        raise ConverterError(
            "{}: {} has SHA-256 {}, the restart state recorded {}; refusing to resume from "
            "it".format(path, what, held, sha256)
        )
    return np.load(path)


# ---------------------------------------------------------------------------
# Scratch
# ---------------------------------------------------------------------------


class Scratch:
    """The scratch directory's files, each saved atomically and reloaded only
    against its recorded SHA-256."""

    def __init__(self, directory: Path):
        self.directory = Path(directory)

    @property
    def binding_path(self) -> Path:
        return self.directory / SCRATCH_BINDING_NAME

    def check_fresh(self) -> None:
        """A fresh run needs an empty (or absent) scratch directory."""
        if self.directory.exists():
            if not self.directory.is_dir():
                raise ConverterError("--scratch {}: not a directory".format(self.directory))
            if any(self.directory.iterdir()):
                raise ConverterError(
                    "--scratch {}: not empty; a fresh run needs an empty scratch directory "
                    "(use --resume to continue a run)".format(self.directory)
                )

    def begin(self, binding: Mapping, resume: bool) -> None:
        """Create a fresh run's scratch with its binding file (the output and
        the configuration digest); a resume needs the scratch of this very
        run, which that file names."""
        if resume:
            if not self.binding_path.is_file():
                raise ConverterError(
                    "--scratch {}: no {}; it is not the scratch of the run being resumed".format(
                        self.directory, SCRATCH_BINDING_NAME
                    )
                )
            held = read_json(self.binding_path, "scratch binding")
            if canonical_json(held) != canonical_json(dict(binding)):
                raise ConverterError(
                    "--scratch {}: bound to another run (output {}, configuration {}); this run "
                    "is output {}, configuration {}".format(
                        self.directory,
                        held.get("output"),
                        held.get("configuration_sha256"),
                        binding["output"],
                        binding["configuration_sha256"],
                    )
                )
            return
        self.check_fresh()
        self.directory.mkdir(parents=True, exist_ok=True)
        write_json_atomic(self.binding_path, dict(binding))

    def _path(self, name: str) -> Path:
        return self.directory / name

    def save(self, name: str, array: np.ndarray) -> str:
        return save_array_atomic(self._path(name), array)

    def load(self, name: str, sha256: str) -> np.ndarray:
        return load_array(self._path(name), sha256, name)

    def remove(self, names) -> None:
        for name in names:
            path = self._path(name)
            if path.exists():
                path.unlink()

    # ---- pass 0 ----
    @staticmethod
    def pass0_names() -> Dict[str, str]:
        return {part: "pass0_{}.npy".format(part) for part in _PASS0_PARTS}

    # ---- remap entries ----
    @staticmethod
    def remap_names(snap: int) -> Dict[str, str]:
        return {part: "remap_{:03d}_{}.npy".format(snap, part) for part in _REMAP_PARTS}

    def save_remap(self, remap: SlabRemap) -> Dict[str, str]:
        names = self.remap_names(remap.snap)
        return {part: self.save(names[part], getattr(remap, part)) for part in _REMAP_PARTS}

    def load_remap(self, snap: int, shas: Mapping[str, str]) -> SlabRemap:
        names = self.remap_names(snap)
        parts = {part: self.load(names[part], shas[part]) for part in _REMAP_PARTS}
        return SlabRemap(snap=snap, **parts)

    def drop_remap(self, snap: int) -> None:
        self.remove(self.remap_names(snap).values())

    # ---- pass 1 checkpoints ----
    @staticmethod
    def checkpoint_names(snap: int) -> Dict[str, str]:
        return {part: "checkpoint_{:03d}_{}.npy".format(snap, part) for part in _CHECKPOINT_PARTS}

    def save_checkpoint(
        self, snap: int, running: np.ndarray, tree_counts: np.ndarray, labels: np.ndarray
    ) -> Dict[str, str]:
        names = self.checkpoint_names(snap)
        return {
            "running": self.save(names["running"], running),
            "tree_counts": self.save(names["tree_counts"], tree_counts),
            "labels": self.save(names["labels"], labels),
        }

    def load_checkpoint(self, snap: int, shas: Mapping[str, str]):
        names = self.checkpoint_names(snap)
        return tuple(self.load(names[part], shas[part]) for part in _CHECKPOINT_PARTS)

    def drop_checkpoint(self, snap: int) -> None:
        self.remove(self.checkpoint_names(snap).values())


# ---------------------------------------------------------------------------
# The restart state
# ---------------------------------------------------------------------------


class RestartState:
    """``rewrite_state.json``: progress and the digests it depends on."""

    def __init__(self, path: Path, data: dict):
        self.path = Path(path)
        self.data = data

    @classmethod
    def create(cls, path: Path, operation: str, configuration_sha256: str, n_snapshots: int):
        data = {
            "state_version": STATE_VERSION,
            "rewriter_version": REWRITER_VERSION,
            "operation": operation,
            "configuration_sha256": configuration_sha256,
            "n_snapshots": int(n_snapshots),
            "status": STATUS_RUNNING,
            "pass0": None,
            "sidecar": None,
            "verified": {},
            "remaps": {},
            "checkpoints": {},
            "peak_store_bytes": 0,
        }
        state = cls(path, data)
        state.save()
        return state

    @classmethod
    def load(cls, path: Path, operation: str, configuration_sha256: str, n_snapshots: int):
        if not path.is_file():
            raise ConverterError(
                "{}: no restart state; this output holds no run to resume".format(path)
            )
        data = read_json(path, "restart state")
        for key, wanted in (
            ("state_version", STATE_VERSION),
            ("operation", operation),
            ("configuration_sha256", configuration_sha256),
            ("n_snapshots", int(n_snapshots)),
        ):
            if data.get(key) != wanted:
                raise ConverterError(
                    "{}: restart state {} is {!r}, this run's is {!r}; refusing to resume".format(
                        path, key, data.get(key), wanted
                    )
                )
        return cls(path, data)

    def save(self) -> None:
        write_json_atomic(self.path, self.data)

    @property
    def complete(self) -> bool:
        return self.data["status"] == STATUS_COMPLETE

    def verified_snapshots(self) -> List[int]:
        return sorted(int(key) for key in self.data["verified"])

    def mark_verified(self, snap: int, path: Path) -> str:
        digest = sha256_file(path)
        self.data["verified"][str(snap)] = {"sha256": digest, "bytes": int(path.stat().st_size)}
        return digest

    def lowest_verified(self) -> Optional[int]:
        """The last verified slab in descending order, which must be the
        bottom of a contiguous run from the top; anything else is refused."""
        verified = self.verified_snapshots()
        if not verified:
            return None
        top = self.data["n_snapshots"] - 1
        if verified != list(range(verified[0], top + 1)):
            raise ConverterError(
                "{}: verified snapshots {} are not contiguous from the last ({}); refusing to "
                "resume".format(self.path, verified, top)
            )
        return verified[0]


# ---------------------------------------------------------------------------
# The manifest
# ---------------------------------------------------------------------------


def inventory_record(totals: np.ndarray) -> Dict[str, object]:
    """The source inventory record as ``conversion_manifest.inventory_record``
    writes it for the Consistent-Trees ASCII route: one unit per forest,
    keyed ``(0, ForestIndex)``, no physical units (``files`` empty), every
    unit selected; computed over the whole array rather than unit by unit."""
    n = int(totals.size)
    units = hashlib.sha256()
    selected = hashlib.sha256()
    for start in range(0, n, 65536):
        stop = min(start + 65536, n)
        block = np.zeros((stop - start, 3), dtype="<i8")
        block[:, 1] = np.arange(start, stop, dtype=np.int64)
        block[:, 2] = totals[start:stop]
        units.update(block.tobytes())
        selected.update(np.ascontiguousarray(block[:, :2]).tobytes())
    total = int(totals.sum())
    return {
        "n_units": n,
        "total_halos": total,
        "units_sha256": units.hexdigest(),
        "n_selected_units": n,
        "selected_halos": total,
        "selected_sha256": selected.hexdigest(),
        "files": [],
    }


def tuning(prepared: Prepared) -> Dict[str, object]:
    return {"block_rows": int(prepared.request.block_rows)}


def check_fresh_output(output_dir: Path) -> None:
    """A fresh run needs an absent or empty output directory."""
    if classify_manifest(output_dir) != "absent":
        raise ConverterError(
            "--output {}: already holds a {}; use --resume to continue it or another "
            "directory".format(output_dir / MANIFEST_NAME, MANIFEST_NAME)
        )
    if output_dir.exists():
        if not output_dir.is_dir():
            raise ConverterError("--output {}: not a directory".format(output_dir))
        if any(output_dir.iterdir()):
            raise ConverterError(
                "--output {}: not empty; a fresh run needs an absent or empty output "
                "directory".format(output_dir)
            )


def create_manifest(prepared: Prepared, output_dir: Path) -> ConversionManifest:
    """A fresh workdir's manifest: configuration frozen, dependencies pinned,
    every stage pending."""
    check_fresh_output(output_dir)
    return ConversionManifest.create(
        output_dir, prepared.configuration, prepared.pins, None, tuning(prepared)
    )


def open_manifest(prepared: Prepared, output_dir: Path) -> ConversionManifest:
    """The manifest of the run being resumed: the same configuration, every
    dependency unchanged."""
    manifest = ConversionManifest.load(output_dir)
    if canonical_json(manifest.configuration) != canonical_json(prepared.configuration):
        raise ConverterError(
            "{}: this run was started with another configuration ({}); refusing to resume it "
            "with different arguments".format(manifest.path, manifest.data["configuration_sha256"])
        )
    manifest.verify_dependencies()
    return manifest


def finish_manifest(
    manifest: ConversionManifest,
    prepared: Prepared,
    counts: List[int],
    n_links: Mapping[str, int],
    store_peak_bytes: int,
    files: List[Path],
) -> None:
    """Complete the three stages with the rewriter's results and register
    every dataset file. Idempotent: a stage already complete is left alone."""
    total = int(sum(counts))
    if not manifest.is_complete("ingest"):
        manifest.begin_attempt("ingest")
        manifest.complete_stage(
            "ingest", {"n_rows": total, "snapshot_counts": [int(c) for c in counts], "n_chunks": 0}
        )
    if not manifest.is_complete("transpose"):
        manifest.begin_attempt("transpose")
        manifest.complete_stage(
            "transpose",
            {
                "total_halos": total,
                "n_links": {name: int(value) for name, value in n_links.items()},
                "n_gapped_descendants": 0,
                "max_descendant_span": 1,
                "links_adjacent": 1,
                "chain_rounds": 0,
                "peak_resident_bytes": int(store_peak_bytes),
                "peak_spill_bytes": 0,
                "sort_runs": {},
                "sort_merge_passes": {},
            },
        )
    if not manifest.is_complete("write"):
        manifest.begin_attempt("write", DATASET_DIR)
        for path in files:
            manifest.register_artifact(manifest.relative(path), "write", "write-output")
        manifest.complete_stage(
            "write",
            {
                "writer": {
                    "writer": "horizontal-hdf5-rewriter",
                    "rewriter_version": REWRITER_VERSION,
                    "operation": prepared.operation,
                    "format_version": 3,
                    "simulation_info_sha256": sha256_file(prepared.request.simulation_info),
                    "header": dict(prepared.metadata),
                },
                "n_files": len(files),
            },
        )


def state_path(output_dir: Path) -> Path:
    return Path(output_dir) / STATE_NAME


def summary_path(output_dir: Path) -> Path:
    return Path(output_dir) / SUMMARY_NAME


def inventory_path(output_dir: Path) -> Path:
    return Path(output_dir) / TREE_INVENTORY_NAME


def dataset_dir(output_dir: Path) -> Path:
    return Path(output_dir) / DATASET_DIR
