"""The rewriter's orchestration: :func:`run_rewrite` runs one operation.

The passes are the package docstring's; this module sequences them, keeps
the remap window to slabs N+1, N and N-1, checkpoints pass 1 after every
slab, records each verified slab in the restart state before any recovery
file is removed, and finishes the manifest, the per-tree inventory and the
summary. On ``--resume`` it continues after the last verified slab from the
scratch files the state names (a surplus or already-removed recovery file is
tolerated), and on a completed run it re-verifies the registered outputs and
the inventory and returns.

What stays resident through passes 1 and 2 is deliberately small: the index,
its forest table, the sidecar columns, the forest totals and the inventory
are released once pass 0, the sidecar and the inventory checkpoint are done
(the inventory is reloaded from the scratch at the end); the baseline is then
the forest bases and running counts, the roots and tree counts, and one or
two slabs' labels (package docstring).

For a cut (``rewriter.cut``) pass 0 continues with the labelling pre-pass
and the table's installation, every refusal of which happens before the
manifest, the scratch and any output exist; the installed table replaces the
census's totals, bases and rank bound, the prepared sidecar and the forest
count, and its per-tree totals are checkpointed so that a resume reinstalls
the same table (bound by its md5 in the configuration). The passes then keep
the per-tree forest and the split masks resident, and the finalisation checks
the labels' tree counts against the pre-pass, writes ``cut_pieces.npy`` and
records the cut in the summary and the manifest.
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
from rewriter import DATASET_DIR, OPERATION_CUT, OPERATION_NOOP  # noqa: E402
from rewriter.cut import (  # noqa: E402
    CutPlan,
    install_table,
    label_tree_totals,
    per_snapshot_rows,
)
from rewriter.inputs import (  # noqa: E402
    MEMORY_FORMULAS,
    Prepared,
    RewriteRequest,
    check_header_reproduced,
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
    CUT_COUNT_DEFINITIONS,
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
    cut_pieces_path,
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


def _check_recorded_file(path: Path, record: Optional[dict], what: str) -> None:
    if record is None:
        raise ConverterError("{}: the restart state records no {}".format(path, what))
    if not path.is_file():
        raise ConverterError("{}: the {} the state records is missing".format(path, what))
    if path.stat().st_size != int(record["bytes"]) or sha256_file(path) != record["sha256"]:
        raise ConverterError("{}: differs from the {} the state records".format(path, what))


def _reverify_complete(manifest, state: RestartState, output_dir: Path, log) -> dict:
    """A completed run: every registered output and the inventory present
    with their recorded checksums; returns the recorded summary."""
    manifest.require_complete("write")
    manifest.verify_stage_artifacts("write")
    for snap, record in state.data["verified"].items():
        _check_recorded_file(
            dataset_dir(output_dir) / snapshot_h5_name(int(snap)), record, "verified slab"
        )
    _check_recorded_file(dataset_dir(output_dir) / SIDECAR_NAME, state.data["sidecar"], "sidecar")
    _check_recorded_file(inventory_path(output_dir), state.data.get("inventory"), "tree inventory")
    if state.data.get("cut_pieces") is not None:
        _check_recorded_file(cut_pieces_path(output_dir), state.data["cut_pieces"], "cut pieces")
    log("resume: the run is complete; every output re-verified")
    return read_json(summary_path(output_dir), "summary")


class _Run:
    """One run's mutable progress."""

    def __init__(
        self,
        prepared: Prepared,
        output_dir: Path,
        scratch: Scratch,
        log,
        plan: Optional[CutPlan] = None,
    ):
        self.prepared = prepared
        self.output_dir = output_dir
        self.dataset_out = dataset_dir(output_dir)
        self.scratch = scratch
        self.log = log
        self.plan = plan
        self.cut = None if plan is None else plan.slab()
        self.remaps: Dict[int, SlabRemap] = {}
        self.store_peak = 0
        self.written_this_run = 0

    # ---- the remap window and the recovery files ----

    def hold(self, remap: SlabRemap) -> None:
        self.remaps[remap.snap] = remap
        resident = sum(entry.nbytes for entry in self.remaps.values())
        self.store_peak = max(self.store_peak, resident)

    def drop(self, snap: int, state: RestartState) -> None:
        """Release one slab's remap entry and remove its files; idempotent."""
        self.remaps.pop(snap, None)
        self.scratch.drop_remap(snap)
        state.data["remaps"].pop(str(snap), None)

    def drop_checkpoint(self, snap: int, state: RestartState) -> None:
        """Remove one pass 1 checkpoint; idempotent."""
        self.scratch.drop_checkpoint(snap)
        state.data["checkpoints"].pop(str(snap), None)


def run_rewrite(request: RewriteRequest, log: Callable[[str], None] = _log) -> dict:
    """Run one operation end to end (module docstring); returns the summary."""
    started = time.time()
    prepared = prepare(request)
    input_dir, output_dir, scratch_dir = check_paths(request)
    dataset = prepared.dataset
    configuration_sha256 = canonical_sha256(prepared.configuration)
    n_snapshots = prepared.n_snapshots
    block_rows = int(request.block_rows)
    identity_from_input = request.operation == OPERATION_NOOP
    cutting = request.operation == OPERATION_CUT
    plan: Optional[CutPlan] = None
    scratch = Scratch(scratch_dir)
    binding = {"output": str(output_dir), "configuration_sha256": configuration_sha256}
    filenames = dict(prepared.index.filenames)
    if request.resume:
        manifest = open_manifest(prepared, output_dir)
        state = RestartState.load(
            state_path(output_dir), request.operation, configuration_sha256, n_snapshots
        )
        if state.complete:
            return _reverify_complete(manifest, state, output_dir, log)
        scratch.begin(binding, resume=True)
        census = _reload_pass0(state, scratch, log)
        if cutting:
            tree_totals = scratch.load(
                scratch.pass0_names()["tree_totals"], state.data["pass0"]["sha256"]["tree_totals"]
            )
            plan = _install(prepared, census, tree_totals, log)
            del tree_totals
    else:
        # every refusal, pass 0's root correspondence and the cut table's
        # invariants included, before the first write: the output and scratch
        # are only checked here, and created once pass 0 has passed
        check_fresh_output(output_dir)
        scratch.check_fresh()
        census = run_pass0(dataset, prepared.index, prepared.sidecar["ForestID"], block_rows, log)
        tree_totals = None
        if cutting:
            tree_totals = label_tree_totals(dataset, census.roots, block_rows, log)
            plan = _install(prepared, census, tree_totals, log)
        manifest = create_manifest(prepared, output_dir)
        # a dependency that moved while pass 0 read it is caught here
        manifest.verify_dependencies()
        state = RestartState.create(
            state_path(output_dir), request.operation, configuration_sha256, n_snapshots
        )
        scratch.begin(binding, resume=False)
        _checkpoint_pass0(census, state, scratch, tree_totals)
        del tree_totals
        if plan is None:
            inventory = tree_inventory(
                prepared.index, prepared.forest_table, prepared.file_ordinals
            )
        else:
            inventory = tree_inventory(plan.index, plan.forest_table, prepared.file_ordinals)
        state.data["pass0"]["inventory_sha256"] = scratch.save(
            scratch.pass0_names()["inventory"], inventory
        )
        state.save()
        del inventory
    run = _Run(prepared, output_dir, scratch, log, plan)
    run.dataset_out.mkdir(parents=True, exist_ok=True)
    manifest.require_inventory(inventory_record(census.totals))
    manifest.save()

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
        state.data["sidecar"] = {
            "sha256": sha256_file(sidecar_path),
            "bytes": sidecar_path.stat().st_size,
            "n_forests": n_forests,
        }
        state.save()
        log("sidecar: {} forest(s) written and verified".format(n_forests))
    # the whole-input tables are no longer needed: release them before the
    # slab passes (package docstring, the resident baseline)
    prepared.release_tables()
    census.totals = None
    if plan is not None:
        plan.release_tables()

    # ---- passes 1 and 2 ----
    top = n_snapshots - 1
    lowest = state.lowest_verified()
    _clean_unverified(run.dataset_out, state.verified_snapshots(), log)
    n_forests = prepared.n_forests
    n_trees = census.roots.size
    if lowest is None:
        running = np.zeros(n_forests, dtype=np.int64)
        tree_counts = np.zeros(n_trees, dtype=np.int64)
        labels = _pass1(run, census, state, top, running, None, tree_counts, identity_from_input)
        pending = top
    elif lowest == 0:
        pending = None
        running, tree_counts, labels = _load_checkpoint(run, state, 0)
        _prune_recovery(run, state, keep_remaps=(), keep_checkpoints=(0,))
    else:
        pending = lowest - 1
        running, tree_counts, labels = _load_checkpoint(run, state, pending)
        for snap in (lowest, pending):
            run.hold(scratch.load_remap(snap, state.data["remaps"][str(snap)]))
        _prune_recovery(run, state, keep_remaps=(lowest, pending), keep_checkpoints=(pending,))
        log(
            "resume: continuing after snapshot {} with pass 1 done through {}".format(
                lowest, pending
            )
        )
    while pending is not None:
        if pending - 1 >= 0:
            labels = _pass1(
                run, census, state, pending - 1, running, labels, tree_counts, identity_from_input
            )
        _pass2(run, census, state, pending, block_rows, identity_from_input)
        _release_after(run, state, pending)
        pending = pending - 1 if pending > 0 else None
    del running, labels
    for snap in list(run.remaps):
        run.drop(snap, state)
    state.save()

    # ---- the inventory, the manifest and the summary ----
    if int(tree_counts.sum()) != dataset.total_halos:
        raise ConverterError(
            "the tree labels count {} halos but the input holds {}".format(
                int(tree_counts.sum()), dataset.total_halos
            )
        )
    if plan is not None and not np.array_equal(tree_counts, plan.tree_totals):
        raise ConverterError(
            "the tree labels of pass 1 count {} tree(s) differently from the labelling "
            "pre-pass the table was checked against".format(
                int(np.count_nonzero(tree_counts != plan.tree_totals))
            )
        )
    inventory = scratch.load(
        scratch.pass0_names()["inventory"], state.data["pass0"]["inventory_sha256"]
    )
    inventory["n_rows"] = tree_counts
    del tree_counts
    save_array_atomic(inventory_path(output_dir), inventory)
    state.data["inventory"] = {
        "sha256": sha256_file(inventory_path(output_dir)),
        "bytes": inventory_path(output_dir).stat().st_size,
    }
    state.save()
    conservation = check_conservation(inventory, filenames, prepared.parsed_counts)
    del inventory
    counts = [int(c) for c in dataset.n_halos]
    n_links: Dict[str, int] = {}
    for record in state.data["verified"].values():
        for name, value in record["n_links"].items():
            n_links[name] = n_links.get(name, 0) + int(value)
    cut_record = None
    if plan is not None:
        save_array_atomic(cut_pieces_path(output_dir), plan.pieces)
        state.data["cut_pieces"] = {
            "sha256": sha256_file(cut_pieces_path(output_dir)),
            "bytes": cut_pieces_path(output_dir).stat().st_size,
        }
        state.save()
        cut_record = {
            "table": {
                "path": prepared.configuration["adapter"]["parameters"]["cut_table"],
                "md5": prepared.cut_table_md5,
                "sha256": prepared.configuration["adapter"]["parameters"]["cut_table_sha256"],
            },
            "record": prepared.configuration["adapter"]["parameters"]["cut_table_record"],
            "rule": None if prepared.cut_record is None else prepared.cut_record["rule"],
            "record_dataset": (
                None if prepared.cut_record is None else prepared.cut_record["dataset"]
            ),
            **plan.record(),
            "per_snapshot": per_snapshot_rows(state.data["cut_per_snapshot"]),
            "definitions": dict(CUT_COUNT_DEFINITIONS),
            "pieces_file": {
                "name": cut_pieces_path(output_dir).name,
                **state.data["cut_pieces"],
            },
        }
    files = sorted(run.dataset_out.iterdir())
    finish_manifest(
        manifest,
        prepared,
        counts,
        n_links,
        int(state.data["peak_store_bytes"]),
        files,
        cut_record,
    )
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
        "memory": {
            "formulas": MEMORY_FORMULAS,
            "planned_peak_bytes": prepared.configuration["transpose_budget_bytes"],
            "peak_rss_bytes": _peak_rss_bytes(),
        },
        "block_rows": block_rows,
        "resumed": bool(request.resume),
        "slabs_written_this_run": run.written_this_run,
        "elapsed_seconds": round(time.time() - started, 3),
    }
    if request.operation == OPERATION_NOOP:
        summary["byte_comparison"] = _byte_comparison(dataset, run.dataset_out)
    if cut_record is not None:
        summary["cut"] = cut_record
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
    if recorded is None or "inventory_sha256" not in recorded:
        raise ConverterError(
            "{}: no complete pass 0 checkpoint; the run cannot resume (start it afresh)".format(
                state.path
            )
        )
    totals = scratch.load(names["totals"], recorded["sha256"]["totals"])
    roots = scratch.load(names["roots"], recorded["sha256"]["roots"])
    sid_base, max_rank = source_bases(totals)
    log("pass 0: reloaded from the scratch checkpoints")
    return Census(
        totals=totals,
        sid_base=sid_base,
        max_rank=max_rank,
        roots=roots,
        correspondence=dict(recorded["correspondence"]),
    )


def _checkpoint_pass0(
    census: Census, state: RestartState, scratch: Scratch, tree_totals: Optional[np.ndarray]
) -> None:
    names = scratch.pass0_names()
    shas = {
        "totals": scratch.save(names["totals"], census.totals),
        "roots": scratch.save(names["roots"], census.roots),
    }
    if tree_totals is not None:
        shas["tree_totals"] = scratch.save(names["tree_totals"], tree_totals)
    state.data["pass0"] = {"sha256": shas, "correspondence": census.correspondence}
    state.save()


def _install(prepared: Prepared, census: Census, tree_totals: np.ndarray, log) -> CutPlan:
    """Install the cut table and let it replace the input's census: the
    output's totals, bases, rank bound, sidecar and forest count."""
    plan = install_table(prepared, census, tree_totals, log)
    census.totals = plan.totals
    census.sid_base = plan.sid_base
    census.max_rank = plan.max_rank
    prepared.sidecar = plan.sidecar
    prepared.n_forests_out = plan.n_forests
    return plan


def _load_checkpoint(run: _Run, state: RestartState, snap: int):
    recorded = state.data["checkpoints"].get(str(snap))
    if recorded is None:
        raise ConverterError(
            "{}: no pass 1 checkpoint for snapshot {}; the run cannot resume (start it afresh "
            "in an empty output and scratch)".format(state.path, snap)
        )
    return run.scratch.load_checkpoint(snap, recorded)


def _prune_recovery(run: _Run, state: RestartState, keep_remaps, keep_checkpoints) -> None:
    """Drop every recovery entry a resume does not continue from: entries
    whose files an interrupted cleanup already removed, and files whose
    removal it never reached."""
    for snap in [int(key) for key in state.data["remaps"] if int(key) not in keep_remaps]:
        run.drop(snap, state)
    for snap in [int(key) for key in state.data["checkpoints"] if int(key) not in keep_checkpoints]:
        run.drop_checkpoint(snap, state)
    state.save()


def _pass1(
    run: _Run,
    census: Census,
    state: RestartState,
    snap: int,
    running: np.ndarray,
    labels_next: Optional[np.ndarray],
    tree_counts: np.ndarray,
    identity_from_input: bool,
) -> np.ndarray:
    remap, labels, cut_counts = rank_slab(
        run.prepared.dataset,
        snap,
        census.sid_base,
        running,
        census.roots,
        labels_next,
        tree_counts,
        identity_from_input,
        run.cut,
        run.remaps.get(snap + 1),
    )
    run.hold(remap)
    if run.cut is not None:
        state.data["cut_per_snapshot"][str(snap)] = cut_counts
    state.data["remaps"][str(snap)] = run.scratch.save_remap(remap)
    state.data["checkpoints"][str(snap)] = run.scratch.save_checkpoint(
        snap, running, tree_counts, labels
    )
    state.data["peak_store_bytes"] = max(int(state.data["peak_store_bytes"]), run.store_peak)
    state.save()
    run.log("pass 1: snapshot {} ranked ({} rows)".format(snap, remap.n))
    return labels


def _pass2(
    run: _Run,
    census: Census,
    state: RestartState,
    snap: int,
    block_rows: int,
    identity_from_input: bool,
) -> None:
    """Write the slab to its temporary name, verify that file, rename it, and
    persist the verified progress before any recovery file is touched."""
    prepared = run.prepared
    window = Window(
        own=run.remaps[snap], above=run.remaps.get(snap + 1), below=run.remaps.get(snap - 1)
    )
    columns = SlabColumns(
        prepared.dataset, snap, window, prepared.table, identity_from_input, run.cut
    )
    header = output_header(prepared, snap, window.own.n, census.max_rank)
    if identity_from_input:
        check_header_reproduced(prepared.dataset, snap, header)
    path = run.dataset_out / snapshot_h5_name(snap)
    tmp = run.dataset_out / (snapshot_h5_name(snap) + ".tmp")
    if tmp.exists():
        tmp.unlink()
    if path.exists():
        path.unlink()
    n_links = write_slab(columns, tmp, header, prepared.declarations, block_rows)
    verify_slab(tmp, columns, header, prepared.declarations, census.sid_base, block_rows)
    os.replace(tmp, path)
    digest = state.mark_verified(snap, path)
    state.data["verified"][str(snap)]["n_links"] = n_links
    state.save()
    run.written_this_run += 1
    run.log(
        "pass 2: snapshot {} written and verified ({} rows, sha256 {})".format(
            snap, window.own.n, digest[:12]
        )
    )


def _release_after(run: _Run, state: RestartState, pending: int) -> None:
    """After slab ``pending`` is verified and recorded: the remap entry of
    slab ``pending + 1`` and, while an earlier checkpoint exists, pass 1's
    checkpoint of ``pending`` are no longer needed (a resume continues from
    the remap entries of ``pending`` and ``pending - 1`` and the checkpoint
    of ``pending - 1``). Every step is idempotent and the state is saved last,
    so an interruption anywhere here resumes cleanly."""
    run.drop(pending + 1, state)
    if pending > 0:
        run.drop_checkpoint(pending, state)
    state.save()


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
