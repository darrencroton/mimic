"""The rewriter's orchestration: :func:`run_rewrite` runs one operation.

The passes are the package docstring's; this module sequences them, keeps
the remap window to slabs N+1, N and N-1, checkpoints pass 1 after every
slab, records each verified slab in the restart state, and finishes the
manifest, the per-tree inventory and the summary. On ``--resume`` it
continues after the last verified slab from the scratch files the state
names, and on a completed run it re-verifies the registered outputs and
returns.
"""

import os
import resource
import sys
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from conversion_manifest import canonical_sha256, sha256_file  # noqa: E402
from errors import ConverterError  # noqa: E402
from hdf5_writer import snapshot_h5_name  # noqa: E402
from hdf5_writer_v3 import SIDECAR_NAME  # noqa: E402
from rewriter import DATASET_DIR, OPERATION_NOOP  # noqa: E402
from rewriter.inputs import (  # noqa: E402
    Prepared,
    RewriteRequest,
    check_paths,
    output_header,
    prepare,
)
from rewriter.pass0 import (  # noqa: E402
    Census,
    check_conservation,
    run_pass0,
    source_bases,
    tree_inventory,
)
from rewriter.passes import (  # noqa: E402
    SlabColumns,
    SlabRemap,
    Window,
    rank_slab,
    verify_sidecar,
    verify_slab,
    write_sidecar,
    write_slab,
)
from rewriter.workdir import (  # noqa: E402
    STATUS_COMPLETE,
    RestartState,
    Scratch,
    check_fresh_output,
    create_manifest,
    dataset_dir,
    finish_manifest,
    inventory_path,
    inventory_record,
    open_manifest,
    read_json,
    save_array_atomic,
    state_path,
    summary_path,
    write_json_atomic,
)

#: Bounded-memory formulas, stated once for the summary and the docstring.
MEMORY_FORMULAS = {
    "pass0": "32 B x n_trees (index) + 40 B x n_forests + 8 B x n_pairs (forest table) + "
    "20 B x n_trees (roots) + 16 B x n_forests (totals, bases) + 56 B x n_trees (inventory); "
    "the index load peaks at 79 B x n_trees and the forest table at 81 B x n_trees",
    "pass1": "at most 56 B x n_rows (widest slab) + 4 B x (n_rows + n_next) (labels) + "
    "8 B x n_forests (running count) + 16 B x n_trees (roots, tree counts)",
    "remap_store": "32 B per row of each resident slab, at most three slabs (N+1, N, N-1)",
    "pass2": "at most 104 B x n_rows: three permutations and inverses (48 B), three slabs' "
    "MostBoundID (24 B), and while writing one input column and its gathered output (12 B "
    "each) or while verifying the written and input link, the compared target ids and the "
    "identity columns (56 B)",
}


def _log(message: str) -> None:
    print(message, file=sys.stderr)


def _peak_rss_bytes() -> int:
    """The process's maximum resident set so far (ru_maxrss is bytes on
    macOS and kibibytes on Linux)."""
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return int(peak) if sys.platform == "darwin" else int(peak) * 1024


def _clean_unverified(dataset_out: Path, verified: List[int], log) -> None:
    """Remove temporary files and snapshot files the state never verified (a
    crash between a rename and the state's save) before rewriting them."""
    if not dataset_out.is_dir():
        return
    keep = {snapshot_h5_name(snap) for snap in verified} | {SIDECAR_NAME}
    for entry in sorted(dataset_out.iterdir()):
        if entry.name in keep:
            continue
        if entry.is_file() and not entry.is_symlink():
            log("resume: removing unverified {}".format(entry))
            entry.unlink()
        else:
            raise ConverterError(
                "{}: unexpected entry {} in the dataset directory".format(dataset_out, entry.name)
            )


def _reverify_complete(manifest, state: RestartState, output_dir: Path, log) -> dict:
    """A completed run: every registered output present with its recorded
    checksum; returns the recorded summary."""
    manifest.require_complete("write")
    manifest.verify_stage_artifacts("write")
    for snap, record in state.data["verified"].items():
        path = dataset_dir(output_dir) / snapshot_h5_name(int(snap))
        if sha256_file(path) != record["sha256"]:
            raise ConverterError(
                "{}: differs from the verified file the state records".format(path)
            )
    sidecar = dataset_dir(output_dir) / SIDECAR_NAME
    if sha256_file(sidecar) != state.data["sidecar"]["sha256"]:
        raise ConverterError(
            "{}: differs from the verified sidecar the state records".format(sidecar)
        )
    log("resume: the run is complete; every output re-verified")
    return read_json(summary_path(output_dir), "summary")


class _Run:
    """One run's mutable progress."""

    def __init__(self, prepared: Prepared, output_dir: Path, scratch: Scratch, log):
        self.prepared = prepared
        self.output_dir = output_dir
        self.dataset_out = dataset_dir(output_dir)
        self.scratch = scratch
        self.log = log
        self.remaps: Dict[int, SlabRemap] = {}
        self.mbid: Dict[int, np.ndarray] = {}
        self.store_peak = 0
        self.written_this_run = 0

    # ---- the remap window ----

    def hold(self, remap: SlabRemap) -> None:
        self.remaps[remap.snap] = remap
        resident = sum(entry.nbytes for entry in self.remaps.values())
        self.store_peak = max(self.store_peak, resident)

    def drop(self, snap: int, state: RestartState) -> None:
        self.remaps.pop(snap, None)
        self.mbid.pop(snap, None)
        self.scratch.drop_remap(snap)
        state.data["remaps"].pop(str(snap), None)

    def mbid_of(self, snap: int) -> np.ndarray:
        if snap not in self.mbid:
            self.mbid[snap] = np.ascontiguousarray(
                self.prepared.dataset.read_column(snap, "MostBoundID"), dtype=np.int64
            )
        return self.mbid[snap]


def run_rewrite(request: RewriteRequest, log: Callable[[str], None] = _log) -> dict:
    """Run one operation end to end (module docstring); returns the summary."""
    started = time.time()
    prepared = prepare(request)
    input_dir, output_dir, scratch_dir = check_paths(request)
    dataset = prepared.dataset
    configuration_sha256 = canonical_sha256(prepared.configuration)
    n_snapshots = prepared.n_snapshots
    block_rows = int(request.block_rows)
    scratch = Scratch(scratch_dir)
    binding = {"output": str(output_dir), "configuration_sha256": configuration_sha256}
    if request.resume:
        manifest = open_manifest(prepared, output_dir)
        state = RestartState.load(
            state_path(output_dir), request.operation, configuration_sha256, n_snapshots
        )
        if state.complete:
            return _reverify_complete(manifest, state, output_dir, log)
        scratch.begin(binding, resume=True)
        census = _reload_pass0(state, scratch, log)
    else:
        # every refusal, pass 0's root correspondence included, before the
        # first write: the output and scratch are only checked here, and
        # created once pass 0 has passed
        check_fresh_output(output_dir)
        scratch.check_fresh()
        census = run_pass0(dataset, prepared.index, prepared.sidecar["ForestID"], block_rows, log)
        manifest = create_manifest(prepared, output_dir)
        # a dependency that moved while pass 0 read it is caught here
        manifest.verify_dependencies()
        state = RestartState.create(
            state_path(output_dir), request.operation, configuration_sha256, n_snapshots
        )
        scratch.begin(binding, resume=False)
        _checkpoint_pass0(census, state, scratch)
    run = _Run(prepared, output_dir, scratch, log)
    run.dataset_out.mkdir(parents=True, exist_ok=True)
    manifest.require_inventory(inventory_record(census.totals))
    manifest.save()
    inventory = tree_inventory(prepared.index, prepared.forest_table, prepared.file_ordinals)

    # ---- the sidecar ----
    sidecar_path = run.dataset_out / SIDECAR_NAME
    if (
        state.data["sidecar"] is not None
        and sidecar_path.is_file()
        and (sha256_file(sidecar_path) == state.data["sidecar"]["sha256"])
    ):
        log("sidecar: already written and verified")
    else:
        tmp = run.dataset_out / (SIDECAR_NAME + ".tmp")
        if tmp.exists():
            tmp.unlink()
        if sidecar_path.exists():
            sidecar_path.unlink()
        n_forests = write_sidecar(tmp, prepared.sidecar)
        verify_sidecar(tmp, prepared.sidecar)
        os.replace(tmp, sidecar_path)
        state.data["sidecar"] = {"sha256": sha256_file(sidecar_path), "n_forests": n_forests}
        state.save()
        log("sidecar: {} forest(s) written and verified".format(n_forests))

    # ---- passes 1 and 2 ----
    top = n_snapshots - 1
    lowest = state.lowest_verified()
    _clean_unverified(run.dataset_out, state.verified_snapshots(), log)
    n_forests = prepared.n_forests
    n_trees = census.roots.size
    if lowest is None:
        running = np.zeros(n_forests, dtype=np.int64)
        tree_counts = np.zeros(n_trees, dtype=np.int64)
        labels = _pass1(run, census, state, top, running, None, tree_counts)
        pending = top
    elif lowest == 0:
        pending = None
        running, tree_counts, labels = _load_checkpoint(run, state, 0)
    else:
        pending = lowest - 1
        running, tree_counts, labels = _load_checkpoint(run, state, pending)
        for snap in (lowest, pending):
            run.hold(scratch.load_remap(snap, state.data["remaps"][str(snap)]))
        log(
            "resume: continuing after snapshot {} with pass 1 done through {}".format(
                lowest, pending
            )
        )
    while pending is not None:
        if pending - 1 >= 0:
            labels = _pass1(run, census, state, pending - 1, running, labels, tree_counts)
        _pass2(run, census, state, pending, block_rows)
        run.drop(pending + 1, state)
        if pending > 0:
            # pass 1's latest checkpoint (pending - 1) is what a resume after
            # this slab continues from; the older one is no longer needed
            scratch.drop_checkpoint(pending)
            state.data["checkpoints"].pop(str(pending), None)
        state.save()
        pending = pending - 1 if pending > 0 else None
    del running
    for snap in list(run.remaps):
        run.drop(snap, state)

    # ---- the inventory, the manifest and the summary ----
    if int(tree_counts.sum()) != dataset.total_halos:
        raise ConverterError(
            "the tree labels count {} halos but the input holds {}".format(
                int(tree_counts.sum()), dataset.total_halos
            )
        )
    inventory["n_rows"] = tree_counts
    save_array_atomic(inventory_path(output_dir), inventory)
    conservation = check_conservation(inventory, prepared.index, prepared.parsed_counts)
    counts = [int(c) for c in dataset.n_halos]
    n_links: Dict[str, int] = {}
    for record in state.data["verified"].values():
        for name, value in record["n_links"].items():
            n_links[name] = n_links.get(name, 0) + int(value)
    files = sorted(run.dataset_out.iterdir())
    finish_manifest(manifest, prepared, counts, n_links, run.store_peak, files)
    for snap, record in state.data["verified"].items():
        registered = manifest.artifact("{}/{}".format(DATASET_DIR, snapshot_h5_name(int(snap))))
        if registered is None or registered["sha256"] != record["sha256"]:
            raise ConverterError(
                "{}: the registered checksum differs from the verified file's".format(
                    snapshot_h5_name(int(snap))
                )
            )
    summary = {
        "operation": request.operation,
        "rewriter_version": prepared.configuration["adapter"]["parameters"]["rewriter_version"],
        "input": str(input_dir),
        "output": str(output_dir),
        "scratch": str(scratch_dir),
        "configuration_sha256": configuration_sha256,
        "n_snapshots": n_snapshots,
        "n_forests_total": n_forests,
        "n_halos": dataset.total_halos,
        "n_trees": int(n_trees),
        "snapshot_counts": counts,
        "n_links": n_links,
        "max_halo_rank_in_forest": census.max_rank,
        "multiplier": int(request.multiplier),
        "tree_file_order": list(prepared.tree_file_order),
        "root_correspondence": census.correspondence,
        "conservation": conservation,
        "remap_store": {"peak_bytes": int(state.data["peak_store_bytes"])},
        "memory": {"formulas": MEMORY_FORMULAS, "peak_rss_bytes": _peak_rss_bytes()},
        "block_rows": block_rows,
        "resumed": bool(request.resume),
        "slabs_written_this_run": run.written_this_run,
        "elapsed_seconds": round(time.time() - started, 3),
    }
    if request.operation == OPERATION_NOOP:
        summary["byte_comparison"] = _byte_comparison(dataset, run.dataset_out)
    write_json_atomic(summary_path(output_dir), summary)
    state.data["status"] = STATUS_COMPLETE
    state.save()
    scratch.remove(sorted(entry.name for entry in scratch.directory.iterdir()))
    log(
        "{}: complete -- {} snapshot file(s), {} halos, {} forests in {}".format(
            request.operation, n_snapshots, dataset.total_halos, n_forests, run.dataset_out
        )
    )
    return summary


def _reload_pass0(state: RestartState, scratch: Scratch, log) -> Census:
    names = scratch.pass0_names()
    recorded = state.data["pass0"]
    if recorded is None:
        raise ConverterError(
            "{}: no pass 0 checkpoint; the run cannot resume (start it afresh)".format(state.path)
        )
    totals = scratch.load(names["totals"], recorded["sha256"]["totals"])
    roots = scratch.load(names["roots"], recorded["sha256"]["roots"])
    root_snapshot = scratch.load(names["root_snapshot"], recorded["sha256"]["root_snapshot"])
    sid_base, max_rank = source_bases(totals)
    log("pass 0: reloaded from the scratch checkpoints")
    return Census(
        totals=totals,
        sid_base=sid_base,
        max_rank=max_rank,
        roots=roots,
        root_snapshot=root_snapshot,
        correspondence=dict(recorded["correspondence"]),
    )


def _checkpoint_pass0(census: Census, state: RestartState, scratch: Scratch) -> None:
    names = scratch.pass0_names()
    shas = {
        "totals": scratch.save(names["totals"], census.totals),
        "roots": scratch.save(names["roots"], census.roots),
        "root_snapshot": scratch.save(names["root_snapshot"], census.root_snapshot),
    }
    state.data["pass0"] = {"sha256": shas, "correspondence": census.correspondence}
    state.save()


def _load_checkpoint(run: _Run, state: RestartState, snap: int):
    recorded = state.data["checkpoints"].get(str(snap))
    if recorded is None:
        raise ConverterError(
            "{}: no pass 1 checkpoint for snapshot {}; the run cannot resume (start it afresh "
            "in an empty output and scratch)".format(state.path, snap)
        )
    return run.scratch.load_checkpoint(snap, recorded)


def _pass1(
    run: _Run,
    census: Census,
    state: RestartState,
    snap: int,
    running: np.ndarray,
    labels_next: Optional[np.ndarray],
    tree_counts: np.ndarray,
) -> np.ndarray:
    remap, labels = rank_slab(
        run.prepared.dataset, snap, census.sid_base, running, census.roots, labels_next, tree_counts
    )
    run.hold(remap)
    state.data["remaps"][str(snap)] = run.scratch.save_remap(remap)
    state.data["checkpoints"][str(snap)] = run.scratch.save_checkpoint(
        snap, running, tree_counts, labels
    )
    state.data["peak_store_bytes"] = max(int(state.data["peak_store_bytes"]), run.store_peak)
    state.save()
    run.log("pass 1: snapshot {} ranked ({} rows)".format(snap, remap.n))
    return labels


def _pass2(run: _Run, census: Census, state: RestartState, snap: int, block_rows: int) -> None:
    prepared = run.prepared
    window = Window(
        own=run.remaps[snap], above=run.remaps.get(snap + 1), below=run.remaps.get(snap - 1)
    )
    columns = SlabColumns(prepared.dataset, snap, window, prepared.table)
    header = output_header(prepared, snap, window.own.n, census.max_rank)
    path = run.dataset_out / snapshot_h5_name(snap)
    tmp = run.dataset_out / (snapshot_h5_name(snap) + ".tmp")
    if tmp.exists():
        tmp.unlink()
    if path.exists():
        path.unlink()
    n_links = write_slab(columns, tmp, header, prepared.declarations, block_rows)
    os.replace(tmp, path)
    verify_slab(
        path, columns, header, prepared.declarations, census.sid_base, run.mbid_of, block_rows
    )
    digest = state.mark_verified(snap, path)
    state.data["verified"][str(snap)]["n_links"] = n_links
    run.written_this_run += 1
    run.log(
        "pass 2: snapshot {} written and verified ({} rows, sha256 {})".format(
            snap, window.own.n, digest[:12]
        )
    )


def _byte_comparison(dataset, dataset_out: Path) -> dict:
    """Per output file, whether its bytes equal the input file's: expected
    only when the input was itself written by the rewriter."""
    files = {}
    for snap in dataset.snapshots:
        name = snapshot_h5_name(snap)
        files[name] = sha256_file(dataset_out / name) == sha256_file(dataset.snapshot_path(snap))
    files[SIDECAR_NAME] = sha256_file(dataset_out / SIDECAR_NAME) == sha256_file(
        dataset.sidecar_path
    )
    return {
        "files": files,
        "all_equal": all(files.values()),
        "note": "byte equality is expected only when the input was itself written by the "
        "rewriter (same layout, no object timestamps); the value identity is verified either way",
    }
