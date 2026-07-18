"""Phase 2 per-snapshot sort and id index for the ctrees -> snapshot-HDF5 converter.

Per snapshot: load the concatenated scratch binary, assert within-snapshot id
uniqueness (abort with examples), sort by ascending id, write
``snap_NNN_sorted.bin`` and ``snap_NNN.idx`` (the sorted int64 id array), then
verify row count and id checksum against the manifest totals before deleting
the unsorted file under the Slice 3 cleanup discipline.

Snapshots are independent jobs; this implementation processes them serially,
which is sufficient at micro-Uchuu scale (parallelisation is a Shin-Uchuu
production concern).
"""

import os
import sys
from pathlib import Path
from typing import Optional, Sequence

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ctrees_parser import DTYPE_TAG, RECORD_DTYPE, ConverterError  # noqa: E402
from scatter import Manifest, id_checksum  # noqa: E402


def sorted_scratch_name(snap: int) -> str:
    return "snap_{:03d}_sorted.bin".format(snap)


def index_name(snap: int) -> str:
    return "snap_{:03d}.idx".format(snap)


def sort_one_snapshot(manifest: Manifest, snap: int) -> None:
    """Sort one snapshot's scratch binary and write its id index."""
    entry = manifest.data["snapshots"].get(str(snap))
    if entry is None:
        raise ConverterError("snapshot {}: no manifest entry; run scatter first".format(snap))
    if entry.get("status") == "sorted":
        # skip-trusting a prior sort requires verifying both artifacts and
        # retrying any unsorted-file cleanup a crash may have interrupted
        manifest.verify_intermediate(entry["sorted_file"], "sorted snapshot scratch")
        manifest.verify_intermediate(entry["index_file"], "snapshot id index")
        _retry_unsorted_cleanup(manifest, entry)
        manifest.save()
        return
    if entry.get("status") != "concatenated":
        raise ConverterError(
            "snapshot {}: unexpected status {!r}; run scatter first".format(
                snap, entry.get("status")
            )
        )

    scratch_path = Path(entry["scratch_file"])
    # verify the input's registered content checksum before consuming it —
    # the id checksum alone would miss corruption in non-id fields
    manifest.verify_intermediate(scratch_path, "unsorted snapshot scratch")
    records = np.fromfile(scratch_path, dtype=RECORD_DTYPE)
    if len(records) != entry["rows"]:
        raise ConverterError(
            "{}: has {} rows, manifest records {}".format(scratch_path, len(records), entry["rows"])
        )

    ids = records["id"]
    unique_ids, counts = np.unique(ids, return_counts=True)
    if len(unique_ids) != len(ids):
        examples = unique_ids[counts > 1][:5].tolist()
        raise ConverterError(
            "snapshot {}: {} duplicate halo id(s); examples: {}".format(
                snap, int((counts > 1).sum()), examples
            )
        )

    order = np.argsort(ids, kind="stable")
    records = records[order]

    sorted_path = scratch_path.parent / sorted_scratch_name(snap)
    idx_path = scratch_path.parent / index_name(snap)
    records.tofile(sorted_path)
    records["id"].astype(np.int64, copy=False).tofile(idx_path)

    # verify the sorted file against the manifest totals before any deletion
    reread = np.fromfile(sorted_path, dtype=RECORD_DTYPE)
    if len(reread) != entry["rows"]:
        raise ConverterError(
            "{}: sorted file has {} rows, manifest records {}".format(
                sorted_path, len(reread), entry["rows"]
            )
        )
    checksum = id_checksum(reread["id"])
    if checksum != entry["id_checksum"]:
        raise ConverterError(
            "{}: sorted-file id checksum {} != manifest checksum {}".format(
                sorted_path, checksum, entry["id_checksum"]
            )
        )
    index_ids = np.fromfile(idx_path, dtype=np.int64)
    if not np.array_equal(index_ids, reread["id"]):
        raise ConverterError("{}: index file does not match sorted ids".format(idx_path))

    manifest.register_intermediate(
        sorted_path, "snapshot-sorted", rows=int(len(reread)), dtype_tag=DTYPE_TAG
    )
    manifest.register_intermediate(
        idx_path, "snapshot-index", rows=int(len(index_ids)), dtype_tag="<i8"
    )
    entry["sorted_file"] = str(sorted_path.resolve())
    entry["index_file"] = str(idx_path.resolve())
    entry["status"] = "sorted"
    manifest.save()

    manifest.remove_intermediate(scratch_path)
    manifest.save()


def _retry_unsorted_cleanup(manifest: Manifest, entry: dict) -> None:
    """Finish unsorted-file deletion a crash may have interrupted. A registered
    entry whose file is already gone is the intended end state of a cleanup
    that crashed between unlink and manifest save — record it as removed."""
    unsorted_path = Path(entry["scratch_file"]).resolve()
    unsorted_entry = manifest.data["intermediates"].get(str(unsorted_path))
    if unsorted_entry is None or unsorted_entry.get("status") != "present":
        return
    if unsorted_path.exists():
        manifest.remove_intermediate(unsorted_path)
    else:
        unsorted_entry["status"] = "removed"


def run_sort(workdir, snapshots: Optional[Sequence[int]] = None) -> Manifest:
    """Sort every concatenated snapshot in the workdir (or the given subset)."""
    manifest = Manifest.load_or_create(workdir)
    if not manifest.path.exists():
        raise ConverterError("{}: no manifest found; run scatter first".format(workdir))
    if snapshots is None:
        snapshots = sorted(int(s) for s in manifest.data["snapshots"])
    for snap in snapshots:
        sort_one_snapshot(manifest, snap)
    return manifest
