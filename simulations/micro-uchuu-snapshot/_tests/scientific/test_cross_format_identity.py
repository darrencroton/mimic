#!/usr/bin/env python3
"""
Cross-format identity gate for the snapshot-ordered driver.

The same simulation, read through two different drivers, must produce the same
galaxies. This harness runs micro-Uchuu through the tree-ordered driver
(``micro-uchuu-ascii``, Consistent-Trees ASCII) and through the snapshot-ordered
driver (``micro-uchuu-snapshot``, snapshot HDF5) and requires, for every output
snapshot, identical ``UniqueGalaxyID`` sets and per-id **byte-identical** fields.
There is no tolerance, no field exclusion and no sampling anywhere in it.

It is a manual, dataset-present operation: both micro-Uchuu datasets are
machine-local and large, and the gate builds four executables and performs nine
full runs, so it takes hours. It is registered only when the selected package is
``micro-uchuu-snapshot``::

    make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific

A missing dataset **fails** the gate with the path it looked for. It never skips:
a gate that quietly reports success because it found nothing to compare is worse
than no gate, and this one is the whole evidentiary basis of the snapshot driver.

Stages, in order (one MIMIC_RESULT marker each):

1. Preconditions -- both datasets resolve, with named paths.
2. Run-file diffs -- each committed snapshot run file differs from its ascii
   counterpart in exactly the two authorized functional keys
   (``simulation.name`` and ``output.output_directory``), plus at most the
   leading header comment block, which carries no functional weight. All four
   run files must also match their committed HEAD copies, so a dirty working
   tree is reported rather than certified.
3. Builds -- one detached git worktree per ``{halos-only, sage16} x {ascii,
   snapshot}`` pair at the current HEAD, each built on its own. The ambient tier
   build in the main tree is never touched.
4-7. Identity -- ``halos-only`` first (fixed timesteps, then dynamic), then
   ``sage16``, which does not start until **both** halos-only stages have
   passed: a divergence in either is a driver bug and is reported before the
   sage16 cost is paid, and a ``halos-only`` pass alone is not the gate. Each
   stage runs both orderings from the worktrees' own run files, checks the
   preflight equalities -- including that each run wrote exactly the output
   snapshots its run file requests, none of them empty -- and compares with
   ``scripts/compare_cross_format_identity.py``, which aggregates every partition
   of each run (the tree side writes five; comparing one file would silently
   compare a fifth of the run).
8. Tree-path preservation -- the same tree-ordered run built from the pre-Phase-5
   baseline commit, whose galaxy records must be byte-identical to HEAD's and
   whose HDF5 metadata must differ in exactly the four permitted deltas beyond
   five provenance attributes that carry no scientific content.

Worktrees and scratch outputs are removed on every exit path.
"""

from __future__ import annotations

import atexit
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import h5py
import numpy
import yaml


def find_repo_root(start: Path) -> Path:
    """Find the Mimic repository root from this file's location."""
    for candidate in [start, *start.parents]:
        if (candidate / "Makefile").is_file() and (candidate / "tests" / "framework").is_dir():
            return candidate
    raise RuntimeError(f"Could not find Mimic repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

# The comparison algorithm has exactly one implementation. The gate shells out to
# the script for the run-vs-run comparison and imports these two helpers so its
# own byte comparisons (the preservation stage) use the same definitions.
import compare_cross_format_identity as comparator  # noqa: E402
from framework import run_test_suite  # noqa: E402  (path set up above)

#: The tree-ordered package and the snapshot-ordered package holding the same
#: micro-Uchuu simulation in two on-disk formats.
ASCII_SIMULATION = "micro-uchuu-ascii"
SNAPSHOT_SIMULATION = "micro-uchuu-snapshot"

#: Models compared, in the order they run. halos-only is the driver-only case and
#: is cheap; sage16 adds the full physics pipeline and is not paid for until
#: halos-only has passed.
MODELS = ("halos-only", "sage16")

#: Timestep schemes compared. "fixed" uses the committed run file unchanged;
#: "dynamic" adds exactly one line to it.
SCHEMES = ("fixed", "dynamic")

#: Files the Consistent-Trees ASCII reader needs from its dataset directory.
ASCII_DATASET_FILES = ("forests.list", "locations.dat", "tree_0_0_0.dat")

#: The pre-Phase-5 baseline: the commit this phase's plan was frozen against,
#: before any snapshot-driver work. The preservation stage builds it to show the
#: tree-ordered path still produces the same galaxy records.
BASELINE_COMMIT = "ae22d278"

#: HDF5 attributes that legitimately differ between two builds/runs of the same
#: code and carry no scientific content, mapped to the object paths where they
#: legitimately live. Reported separately, never counted as a delta, and never
#: silently dropped -- but excused only where they belong: the same name
#: appearing anywhere else is a real difference, and excusing it by name alone
#: would let a genuine metadata change hide behind a provenance label.
PROVENANCE_ATTR_PATHS = {
    "git_commit": ("/RunProperties/Version",),
    "git_branch": ("/RunProperties/Version",),
    "git_date": ("/RunProperties/Version",),
    "build_date": ("/RunProperties/Version",),
    "RunEndTime": ("/RunProperties",),
}
EXCLUDED_PROVENANCE_ATTRS = tuple(PROVENANCE_ATTR_PATHS)

#: The four permitted HDF5 metadata deltas between the pre-Phase-5 baseline and
#: HEAD. Every one of them must be observed; anything else is a failure.
PERMITTED_DELTAS = (
    "UniqueGalaxyIDMultiplier attribute",
    "TotHalosPerSnap int64",
    "UniqueGalaxyID description",
    "hdf5_format_version 1.2",
)

#: Attributes required to be exactly equal between the two runs of a pair before
#: their records are compared. A pair that disagrees here is not two views of one
#: simulation, and a record comparison over it would be meaningless.
PREFLIGHT_ATTRS = (
    "BoxSize",
    "PartMass",
    "Omega",
    "OmegaLambda",
    "Hubble_h",
    "UniqueGalaxyIDMultiplier",
)

#: Free space the harness needs for nine full micro-Uchuu runs plus five builds.
REQUIRED_FREE_BYTES = 20 * 1024**3

SNAP_GROUP_RE = re.compile(r"^Snap(\d+)$")
FILE_GROUP_RE = re.compile(r"^File(\d+)$")

#: Object paths a permitted delta is allowed to occur at. `TotHalosPerSnap`
#: lives on the master's per-partition groups and on each partition's own
#: Galaxies dataset; the other three live at one fixed path each. Binding the
#: classification to the path (and, below, to the exact before/after transition)
#: is what keeps "exactly four permitted deltas" meaning four *specific*
#: changes rather than four attribute names that may change in any way anywhere.
VERSION_GROUP_PATH = "/RunProperties/Version"
RUN_PROPERTIES_PATH = "/RunProperties"
FIELD_METADATA_PATH = "/RunProperties/FieldMetadata"
MASTER_SNAP_FILE_RE = re.compile(r"^/Snap\d+/File\d+$")
PARTITION_SNAP_GALAXIES_RE = re.compile(r"^/Snap\d+/Galaxies$")

#: The exact transitions the permitted deltas must show. A widening that lands
#: on a different width, or a version that moves anywhere other than 1.1 -> 1.2,
#: is an unclassified difference and fails the stage.
TOTHALOS_DTYPE_TRANSITION_RE = re.compile(r"^dtype int32(\(.*?\)) -> int64(\(.*?\))$")
FORMAT_VERSION_TRANSITION_RE = re.compile(r"^value \[b'1\.1'\] -> \[b'1\.2'\]$")
COMPARATOR_PASS_RE = re.compile(
    r"^PASSED: (\d+) galaxies over (\d+) output snapshot\(s\) are bitwise identical "
    r"in all (\d+) field"
)


# --------------------------------------------------------------------------
# Progress reporting. A multi-hour run has to be observable while it runs, so
# every stage and every long step announces itself and its elapsed time.
# --------------------------------------------------------------------------


_STARTED = time.monotonic()


def log(message: str) -> None:
    """Print one progress line, prefixed with elapsed wall time, unbuffered."""
    elapsed = time.monotonic() - _STARTED
    print(f"[gate {int(elapsed) // 60:3d}m{int(elapsed) % 60:02d}s] {message}", flush=True)


def banner(title: str) -> None:
    print(flush=True)
    log("=" * 70)
    log(title)
    log("=" * 70)


# --------------------------------------------------------------------------
# Harness state and cleanup
# --------------------------------------------------------------------------


class Gate:
    """Everything the stages build up, and everything cleanup has to remove."""

    def __init__(self):
        self.scratch: Path | None = None
        self.worktrees: dict[str, Path] = {}
        self.runs: dict[str, "RunOutput"] = {}
        self.done: set[str] = set()

    def require(self, stage_name: str, what: str) -> None:
        """Fail this stage when an earlier one it depends on did not complete."""
        if stage_name not in self.done:
            raise AssertionError(f"prerequisite stage '{stage_name}' did not complete; {what}")

    def scratch_dir(self, *parts: str) -> Path:
        assert self.scratch is not None, "scratch directory not created"
        path = self.scratch.joinpath(*parts)
        path.mkdir(parents=True, exist_ok=True)
        return path


GATE = Gate()


class RunOutput:
    """One completed Mimic run: where its files are and what they claim."""

    def __init__(self, key, directory, basename, worktree, run_file):
        self.key = key
        self.directory = directory
        self.basename = basename
        self.worktree = worktree
        self.run_file = run_file

    @property
    def master(self) -> Path:
        return self.directory / f"{self.basename}.hdf5"

    @property
    def spec(self) -> str:
        """The <directory>/<output_filename> pair the comparator takes."""
        return str(self.directory / self.basename)

    def partitions(self) -> list[Path]:
        return comparator.partition_files(self.spec)

    def schema_path(self) -> Path:
        return self.directory / "metadata" / "output_schema.json"


def cleanup() -> None:
    """Remove every worktree and scratch directory. Safe to call twice."""
    for key, path in list(GATE.worktrees.items()):
        try:
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(path)],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
            )
        finally:
            GATE.worktrees.pop(key, None)
    subprocess.run(["git", "worktree", "prune"], cwd=REPO_ROOT, check=False, capture_output=True)
    if GATE.scratch is not None and GATE.scratch.exists():
        shutil.rmtree(GATE.scratch, ignore_errors=True)


def _cleanup_on_signal(signum, _frame):
    # Raise through SystemExit so the atexit handler still runs, then die with
    # the conventional status for the signal.
    raise SystemExit(128 + signum)


# --------------------------------------------------------------------------
# Subprocess helpers
# --------------------------------------------------------------------------


def tail(path: Path, lines: int = 40) -> str:
    try:
        content = path.read_text(errors="replace").splitlines()
    except OSError as error:
        return f"(log {path} unreadable: {error})"
    return "\n".join(content[-lines:])


def run_logged(cmd, cwd, env, log_path: Path, what: str, show_tail: int = 0) -> None:
    """Run a command with its output captured to a file; fail loudly on error."""
    log(f"  -> {what}")
    started = time.monotonic()
    with log_path.open("wb") as handle:
        completed = subprocess.run(cmd, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT)
    elapsed = time.monotonic() - started
    if completed.returncode != 0:
        raise AssertionError(
            f"{what} failed with exit status {completed.returncode}\n"
            f"  command: {' '.join(str(part) for part in cmd)}\n"
            f"  cwd: {cwd}\n"
            f"  log: {log_path}\n--- last lines ---\n{tail(log_path)}"
        )
    log(f"     done in {elapsed:.0f}s (log: {log_path})")
    if show_tail:
        for line in tail(log_path, show_tail).splitlines():
            log(f"     | {line}")


def git(*args: str) -> str:
    completed = subprocess.run(
        ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True, check=False
    )
    if completed.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


# --------------------------------------------------------------------------
# Preconditions
# --------------------------------------------------------------------------


def snapshot_list_entries(path: Path) -> list[str]:
    """Return the scale factors listed in an a_list file."""
    return [line.strip() for line in path.read_text().splitlines() if line.strip()]


def assert_dataset_present(package: str, required_files) -> Path:
    """Fail -- never skip -- when a package's machine-local dataset is absent."""
    link = REPO_ROOT / "simulations" / package / "snapshots"
    if not link.exists():
        target = os.readlink(link) if link.is_symlink() else "(no symlink)"
        raise AssertionError(
            f"{package} dataset is not available: {link} does not resolve "
            f"(symlink target: {target}). The gate requires both micro-Uchuu datasets; "
            f"it fails rather than skipping."
        )
    if not link.is_dir():
        raise AssertionError(f"{package} dataset path is not a directory: {link}")
    for name in required_files:
        entry = link / name
        if not entry.exists():
            raise AssertionError(f"{package} dataset is incomplete: missing {entry}")
    return link


def scratch_parent() -> Path:
    """Where the gate's worktrees and nine run outputs go.

    The repository's machine-local `output/` when it exists -- it is gitignored,
    it is where run output belongs, and on a typical install it is the volume
    with room for it. Otherwise the system temporary directory.
    """
    output = REPO_ROOT / "output"
    return output if output.is_dir() else Path(tempfile.gettempdir())


def assert_free_space(parent: Path) -> None:
    usage = shutil.disk_usage(parent)
    if usage.free < REQUIRED_FREE_BYTES:
        raise AssertionError(
            f"{parent}: {usage.free / 1024**3:.1f} GiB free, the gate needs at "
            f"least {REQUIRED_FREE_BYTES / 1024**3:.0f} GiB for its worktrees and nine runs"
        )


def stage_preconditions():
    """Both micro-Uchuu datasets resolve, with named paths on failure."""
    banner("Stage 1: preconditions")

    ascii_data = assert_dataset_present(ASCII_SIMULATION, ASCII_DATASET_FILES)
    log(f"  {ASCII_SIMULATION}: {ascii_data} -> {ascii_data.resolve()}")

    # The snapshot files the reader needs are the ones the package's a_list names,
    # one per entry: snapshot_%03d.h5 for index 0..len(a_list)-1.
    alist = REPO_ROOT / "simulations" / SNAPSHOT_SIMULATION / "micro-uchuu.a_list"
    if not alist.is_file():
        raise AssertionError(f"snapshot package a_list is missing: {alist}")
    entries = snapshot_list_entries(alist)
    required = tuple(f"snapshot_{index:03d}.h5" for index in range(len(entries)))
    snapshot_data = assert_dataset_present(SNAPSHOT_SIMULATION, required)
    log(
        f"  {SNAPSHOT_SIMULATION}: {snapshot_data} -> {snapshot_data.resolve()} "
        f"({len(required)} snapshot files, from {len(entries)} a_list entries)"
    )

    parent = scratch_parent()
    assert_free_space(parent)

    GATE.scratch = Path(tempfile.mkdtemp(prefix="mimic-cross-format-gate-", dir=parent))
    log(f"  scratch: {GATE.scratch}")
    log(f"  HEAD: {git('rev-parse', 'HEAD')}")
    GATE.done.add("preconditions")


# --------------------------------------------------------------------------
# Run files
# --------------------------------------------------------------------------


def committed_run_file(model: str, simulation: str) -> Path:
    return REPO_ROOT / "models" / model / "input" / f"{model}_{simulation}.yaml"


def head_bytes(path: Path) -> bytes:
    """The content of `path` as HEAD holds it, independent of the working tree."""
    relative = path.relative_to(REPO_ROOT).as_posix()
    completed = subprocess.run(
        ["git", "show", f"HEAD:{relative}"], cwd=REPO_ROOT, capture_output=True
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"{path} is not committed at HEAD: git show HEAD:{relative} failed "
            f"({completed.stderr.decode(errors='replace').strip()}). The gate certifies "
            f"committed run files and builds worktrees at HEAD, so an uncommitted run file "
            f"cannot be part of it."
        )
    return completed.stdout


def assert_matches_head(path: Path) -> None:
    """Fail when a run file in the working tree differs from its HEAD copy.

    The worktrees are pinned to HEAD and execute their own copies, so a dirty
    working-tree run file would never reach the runs -- but it would still be
    the file stage 2 inspected. Reporting the divergence keeps the gate's PASS
    a statement about a self-contained HEAD, rather than about whatever happens
    to be on disk.
    """
    if path.read_bytes() != head_bytes(path):
        raise AssertionError(
            f"{path} differs from its committed copy at HEAD. This gate certifies committed "
            f"run files against HEAD-built executables; commit or revert the file and re-run."
        )


def split_leading_header(lines: list[str]) -> tuple[list[str], list[str]]:
    """Split a run file into its leading header comment block and the rest.

    The header block is the maximal prefix of comment and blank lines. It carries
    no functional weight: it is the only part of the file the two run files are
    allowed to differ in beyond the two authorized keys, because an identical
    header would force the snapshot run file to describe itself as
    Consistent-Trees ASCII.
    """
    index = 0
    while index < len(lines) and (
        not lines[index].strip() or lines[index].lstrip().startswith("#")
    ):
        index += 1
    return lines[:index], lines[index:]


def flatten_keys(node, prefix: str = "") -> dict[str, object]:
    """Flatten a parsed run file to `section.key` -> value, lists kept whole.

    Lists (module phase orders, snapshot lists) are compared as single values:
    reordering one is a functional change and must be seen as such, not as a
    set of independently-numbered differences that could be miscounted.
    """
    if not isinstance(node, dict):
        return {prefix: node}
    flat: dict[str, object] = {}
    for key, value in node.items():
        path = f"{prefix}.{key}" if prefix else str(key)
        if isinstance(value, dict) and value:
            flat.update(flatten_keys(value, path))
        else:
            flat[path] = value
    return flat


#: The only functional keys the snapshot run file may change relative to its
#: ascii counterpart. `simulation.name` selects the package under test;
#: `output.output_directory` keeps the two runs from writing over each other.
#: Anything else -- a different substep count, snapshot list, output format or
#: module order -- would make a comparison of their outputs meaningless.
AUTHORIZED_KEY_CHANGES = ("simulation.name", "output.output_directory")


def assert_snapshot_run_file_diff(ascii_path: Path, snapshot_path: Path) -> None:
    """The snapshot run file differs from the ascii one in exactly two keys.

    Checked twice over, because this assertion is what lets the identity
    comparison mean anything at all:

    1. **Functionally**, on the parsed YAML: exactly `AUTHORIZED_KEY_CHANGES`
       differ, and nothing is added or removed.
    2. **Textually**, on the lines below the leading header comment block:
       exactly two differ, and they are those same two keys. The leading header
       block is exempt (and only it -- a comment anywhere further down is a third
       differing line and fails). The textual check catches what a parse cannot:
       a body comment, a reordered file, a whitespace change.
    """
    left_text = ascii_path.read_text()
    right_text = snapshot_path.read_text()

    left_flat = flatten_keys(yaml.safe_load(left_text))
    right_flat = flatten_keys(yaml.safe_load(right_text))
    if set(left_flat) != set(right_flat):
        only_ascii = sorted(set(left_flat) - set(right_flat))
        only_snapshot = sorted(set(right_flat) - set(left_flat))
        raise AssertionError(
            f"{snapshot_path} and {ascii_path} do not carry the same keys: "
            f"only in {ascii_path.name}: {only_ascii}; only in {snapshot_path.name}: "
            f"{only_snapshot}"
        )
    changed_keys = sorted(key for key in left_flat if left_flat[key] != right_flat[key])
    if changed_keys != sorted(AUTHORIZED_KEY_CHANGES):
        detail = "\n".join(
            f"    {key}: {left_flat[key]!r} -> {right_flat[key]!r}" for key in changed_keys
        )
        raise AssertionError(
            f"{snapshot_path} differs from {ascii_path} in functional key(s) {changed_keys}, "
            f"expected exactly {sorted(AUTHORIZED_KEY_CHANGES)}:\n{detail}"
        )
    if right_flat["simulation.name"] != SNAPSHOT_SIMULATION:
        raise AssertionError(
            f"{snapshot_path}: simulation.name is {right_flat['simulation.name']!r}, "
            f"expected {SNAPSHOT_SIMULATION!r}"
        )
    if not str(right_flat["output.output_directory"]).endswith(f"-{SNAPSHOT_SIMULATION}"):
        raise AssertionError(
            f"{snapshot_path}: output.output_directory is "
            f"{right_flat['output.output_directory']!r}, which does not name a "
            f"{SNAPSHOT_SIMULATION} output directory, so the two runs would share one"
        )

    left_header, left_body = split_leading_header(left_text.splitlines())
    right_header, right_body = split_leading_header(right_text.splitlines())
    if len(left_body) != len(right_body):
        raise AssertionError(
            f"{snapshot_path.name} has {len(right_body)} line(s) below its header comment, "
            f"{ascii_path.name} has {len(left_body)}: the two run files must differ in exactly "
            f"the two authorized keys, not in their shape"
        )
    changed = [index for index in range(len(left_body)) if left_body[index] != right_body[index]]
    if len(changed) != 2:
        detail = "\n".join(
            f"    line {index + 1 + len(right_header)}: "
            f"{left_body[index]!r} -> {right_body[index]!r}"
            for index in changed
        )
        raise AssertionError(
            f"{snapshot_path} differs from {ascii_path} in {len(changed)} line(s) below the "
            f"leading header comment, expected exactly 2:\n{detail}"
        )
    name_line, output_line = (right_body[index].strip() for index in changed)
    if name_line != f"name: {SNAPSHOT_SIMULATION}":
        raise AssertionError(
            f"{snapshot_path}: first changed line is {name_line!r}, "
            f"expected 'name: {SNAPSHOT_SIMULATION}'"
        )
    if not output_line.startswith("output_directory:"):
        raise AssertionError(
            f"{snapshot_path}: second changed line is {output_line!r}, "
            f"expected an output_directory: assignment"
        )
    if left_header != right_header:
        log(
            f"  {snapshot_path.name}: leading header comment differs from "
            f"{ascii_path.name} (carries no functional weight; permitted)"
        )


def assert_dynamic_variant(base_path: Path, variant_path: Path) -> None:
    """The dynamic variant is the committed file plus exactly one line."""
    base = base_path.read_text().splitlines()
    variant = variant_path.read_text().splitlines()
    if len(variant) != len(base) + 1:
        raise AssertionError(
            f"{variant_path} has {len(variant)} lines, expected {len(base) + 1} "
            f"(the committed file plus one TimestepScheme line)"
        )
    inserted = [
        index for index in range(len(variant)) if variant[:index] + variant[index + 1 :] == base
    ]
    if not inserted:
        raise AssertionError(
            f"{variant_path} is not {base_path} with a single line inserted; the two runs of a "
            f"scheme pair must otherwise be byte-identical"
        )
    added = variant[inserted[0]].strip()
    if added != "TimestepScheme: dynamic":
        raise AssertionError(
            f"{variant_path}: inserted line is {added!r}, not 'TimestepScheme: dynamic'"
        )


def stage_run_file_diffs():
    """Each committed snapshot run file differs in exactly the two authorized keys."""
    banner("Stage 2: committed run-file diffs")
    GATE.require("preconditions", "not checking run files")

    for model in MODELS:
        ascii_path = committed_run_file(model, ASCII_SIMULATION)
        snapshot_path = committed_run_file(model, SNAPSHOT_SIMULATION)
        for path in (ascii_path, snapshot_path):
            if not path.is_file():
                raise AssertionError(f"run file is missing: {path}")
            # Both sides, before anything is built or run: a dirty run file on
            # either side would make the two-key relative diff pass while the
            # HEAD-built executables consumed something else.
            assert_matches_head(path)
        log(f"  {model}: both run files match their committed HEAD copies")
        assert_snapshot_run_file_diff(ascii_path, snapshot_path)
        log(
            f"  {model}: {snapshot_path.name} differs from {ascii_path.name} in exactly "
            f"{list(AUTHORIZED_KEY_CHANGES)}"
        )

    GATE.done.add("run-files")


# --------------------------------------------------------------------------
# Isolated per-pair builds
# --------------------------------------------------------------------------


def link_machine_local(worktree: Path) -> None:
    """Recreate the main tree's machine-local symlinks inside a worktree.

    The dataset directories and the Python virtual environment are gitignored, so
    a fresh worktree has neither. Both are recreated from the main tree's
    resolved targets: the datasets because the runs read them, the venv because
    the Makefile's generators need PyYAML.
    """
    for package in sorted((REPO_ROOT / "simulations").iterdir()):
        source = package / "snapshots"
        if not source.is_symlink():
            continue
        destination = worktree / "simulations" / package.name / "snapshots"
        if not destination.parent.is_dir():
            continue
        target = source.resolve()
        if not target.exists():
            continue
        destination.symlink_to(target)

    venv = REPO_ROOT / "mimic_venv"
    if venv.is_dir():
        (worktree / "mimic_venv").symlink_to(venv)


def worktree_env(worktree: Path, model: str, simulation: str) -> dict:
    """Environment for make/mimic inside a worktree: venv on PATH, no test build."""
    env = dict(os.environ)
    # The gate compares production executables. The ambient scientific tier is a
    # TEST_BUILD, which carries the framework's fixture modules and their
    # test-only properties; inheriting that would compare a different record.
    env.pop("MIMIC_TEST_BUILD", None)
    env.pop("TEST_BUILD", None)
    env.pop("SIM", None)
    env["MODEL"] = model
    env["SIMULATION"] = simulation
    env["VIRTUAL_ENV"] = str(worktree / "mimic_venv")
    env["PATH"] = f"{worktree / 'mimic_venv' / 'bin'}{os.pathsep}{env.get('PATH', '')}"
    return env


def build_worktree(key: str, commit: str, model: str, simulation: str) -> Path:
    """Create a detached worktree at `commit` and build one MODEL/SIMULATION pair."""
    worktree = GATE.scratch_dir("worktrees") / key
    logs = GATE.scratch_dir("logs")
    log(f"  worktree {key}: {model} x {simulation} at {commit}")
    subprocess.run(
        ["git", "worktree", "add", "--detach", str(worktree), commit],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    GATE.worktrees[key] = worktree
    link_machine_local(worktree)

    env = worktree_env(worktree, model, simulation)
    selectors = [f"MODEL={model}", f"SIMULATION={simulation}"]
    run_logged(
        ["make", *selectors, "generate"],
        worktree,
        env,
        logs / f"{key}-generate.log",
        f"{key}: make generate",
    )
    run_logged(
        ["make", *selectors, f"-j{os.cpu_count() or 4}"],
        worktree,
        env,
        logs / f"{key}-build.log",
        f"{key}: make",
    )

    executable = worktree / "mimic"
    if not executable.is_file():
        raise AssertionError(f"{key}: build produced no executable at {executable}")
    return worktree


def pair_key(model: str, simulation: str) -> str:
    return f"{model}__{simulation}"


def stage_build_worktrees():
    """One isolated worktree build per model x ordering pair, at the current HEAD."""
    banner("Stage 3: isolated per-pair builds at HEAD")
    GATE.require("run-files", "not building")

    head = git("rev-parse", "HEAD")
    for model in MODELS:
        for simulation in (ASCII_SIMULATION, SNAPSHOT_SIMULATION):
            build_worktree(pair_key(model, simulation), head, model, simulation)

    GATE.done.add("builds")


# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------


def read_run_file_output(run_file: Path) -> tuple[str, str]:
    """Return (output_directory, output_filename) as the run file declares them."""
    directory = filename = None
    for line in run_file.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("output_directory:"):
            directory = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("output_filename:"):
            filename = stripped.split(":", 1)[1].strip()
    if directory is None or filename is None:
        raise AssertionError(f"{run_file}: output_directory/output_filename not found")
    return directory, filename


def dynamic_variant(run_file: Path, scratch: Path) -> Path:
    """Write the committed run file with `TimestepScheme: dynamic` added."""
    lines = run_file.read_text().splitlines(keepends=True)
    out = []
    inserted = False
    for line in lines:
        out.append(line)
        if not inserted and line.strip().startswith("SubSteps:"):
            out.append("TimestepScheme: dynamic\n")
            inserted = True
    if not inserted:
        raise AssertionError(f"{run_file}: no SubSteps line to place TimestepScheme after")
    variant = scratch / f"{run_file.stem}_dynamic.yaml"
    variant.write_text("".join(out))
    assert_dynamic_variant(run_file, variant)
    return variant


def execute_run(model: str, simulation: str, scheme: str) -> RunOutput:
    """Run one model x ordering x scheme into its own scratch output directory."""
    key = f"{model}__{simulation}__{scheme}"
    worktree = GATE.worktrees[pair_key(model, simulation)]

    # The worktree's OWN copy of the run file, never the working tree's. The
    # executable is pinned to HEAD, so its input must be too: reading the
    # ambient copy would let a dirty tree be certified by a HEAD build.
    worktree_run_file = worktree / committed_run_file(model, simulation).relative_to(REPO_ROOT)
    if not worktree_run_file.is_file():
        raise AssertionError(f"{key}: the HEAD worktree has no run file at {worktree_run_file}")

    if scheme == "fixed":
        run_file = worktree_run_file
    else:
        run_file = dynamic_variant(worktree_run_file, GATE.scratch_dir("run-files"))

    declared_directory, basename = read_run_file_output(run_file)

    # The run file's output_directory is relative to the working directory, so a
    # per-run scratch root is selected by pointing the worktree's `output` at it.
    # That keeps the committed run file byte-for-byte the one being tested while
    # still giving every one of the nine runs its own directory.
    output_root = GATE.scratch_dir("runs", key)
    link = worktree / "output"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(output_root)

    env = worktree_env(worktree, model, simulation)
    log(f"  run {key}: {run_file}")
    run_logged(
        ["./mimic", str(run_file)],
        worktree,
        env,
        GATE.scratch_dir("logs") / f"{key}-run.log",
        f"{key}: mimic run",
        show_tail=3,
    )

    directory = output_root / Path(declared_directory).name
    if not directory.is_dir():
        raise AssertionError(f"{key}: run produced no output directory at {directory}")
    run = RunOutput(key, directory, basename, worktree, run_file)
    if not run.master.is_file():
        raise AssertionError(f"{key}: run produced no master file at {run.master}")
    GATE.runs[key] = run
    log(f"     {len(run.partitions())} partition file(s) + master {run.master.name}")
    return run


# --------------------------------------------------------------------------
# Preflight equality
# --------------------------------------------------------------------------


def attr_value(raw) -> tuple[str, tuple, bytes]:
    """Return an attribute as (dtype, shape, raw bytes) for exact comparison."""
    array = numpy.asarray(raw)
    return str(array.dtype), array.shape, array.tobytes()


def run_properties(master: Path):
    with h5py.File(master, "r") as handle:
        group = handle["RunProperties"]
        attrs = {name: attr_value(value) for name, value in group.attrs.items()}
        redshifts = numpy.array(group["Redshifts"][()])
        field_metadata = numpy.array(group["FieldMetadata"][()])
    return attrs, redshifts, field_metadata


def assert_alist_byte_equal(left: Path, right: Path) -> None:
    """The two packages' snapshot lists must be the same bytes.

    Deliberately stricter than the runtime check the reader performs: the runtime
    compares scale factors with a tolerance, while two packages claiming to hold
    the same simulation have no business disagreeing about its snapshot list at
    all.
    """
    for path in (left, right):
        if not path.is_file():
            raise AssertionError(f"snapshot list is missing: {path}")
    left_bytes, right_bytes = left.read_bytes(), right.read_bytes()
    if left_bytes != right_bytes:
        left_lines = left_bytes.decode(errors="replace").splitlines()
        right_lines = right_bytes.decode(errors="replace").splitlines()
        differing = [
            f"    line {index + 1}: {a!r} != {b!r}"
            for index, (a, b) in enumerate(zip(left_lines, right_lines))
            if a != b
        ]
        raise AssertionError(
            f"snapshot lists differ:\n  {left}\n  {right}\n"
            + (
                "\n".join(differing[:10])
                or f"    (line counts {len(left_lines)} vs {len(right_lines)})"
            )
        )


def requested_snapshots(run: RunOutput) -> set[int]:
    """The output snapshots the run file under test actually asks for.

    The gate's counts are only evidence if they cover the whole run. Reading the
    request from the run file gives an expectation the runs had no part in
    producing, so a shared output-selection or writer regression that drops the
    same snapshot from both sides cannot pass by agreeing with itself.
    """
    config = yaml.safe_load(run.run_file.read_text())
    selected = (config.get("output") or {}).get("snapshot_list")
    if not selected:
        raise AssertionError(f"{run.run_file}: no output.snapshot_list to compare against")
    return {int(entry) for entry in selected}


def assert_snapshot_coverage(run: RunOutput, counts: dict[int, int], expected: set[int]) -> None:
    """One run wrote exactly the requested output snapshots, none of them empty.

    Exactly: a missing snapshot means the comparison covers part of the run, and
    an extra one means the run is not the run the file describes. Non-empty:
    an empty snapshot compares equal to an empty snapshot, so a writer that
    emitted nothing on both sides would otherwise be indistinguishable from a
    run that agreed everywhere.
    """
    written = set(counts)
    if written != expected:
        raise AssertionError(
            f"{run.key}: wrote output snapshots {sorted(written)}, its run file requests "
            f"{sorted(expected)} (missing {sorted(expected - written)}, "
            f"unexpected {sorted(written - expected)}); the comparison would cover only "
            f"part of the run"
        )
    empty = sorted(snap for snap, count in counts.items() if count <= 0)
    if empty:
        raise AssertionError(
            f"{run.key}: output snapshot(s) {empty} hold no galaxies; an empty snapshot "
            f"compares equal to an empty snapshot and is not evidence of anything"
        )


def alist_used_by(run: RunOutput) -> Path:
    """The snapshot list the run recorded as its input, inside its own worktree."""
    attrs, _, _ = run_properties(run.master)
    if "FileWithSnapList" not in attrs:
        raise AssertionError(f"{run.master}: no FileWithSnapList attribute")
    with h5py.File(run.master, "r") as handle:
        recorded = handle["RunProperties"].attrs["FileWithSnapList"]
    relative = numpy.asarray(recorded).ravel()[0]
    if isinstance(relative, bytes):
        relative = relative.decode()
    return run.worktree / relative


def preflight(tree_run: RunOutput, snapshot_run: RunOutput) -> tuple[int, int]:
    """Exact equality of everything the two runs must agree on before comparison.

    Returns (records, fields) derived independently of the comparator.
    """
    assert_alist_byte_equal(alist_used_by(tree_run), alist_used_by(snapshot_run))
    log("  preflight: snapshot lists byte-equal")

    tree_attrs, tree_z, tree_fields = run_properties(tree_run.master)
    snap_attrs, snap_z, snap_fields = run_properties(snapshot_run.master)

    if (tree_z.dtype, tree_z.shape) != (snap_z.dtype, snap_z.shape):
        raise AssertionError(
            f"recorded Redshifts differ in type/shape: {tree_z.dtype}{tree_z.shape} vs "
            f"{snap_z.dtype}{snap_z.shape}"
        )
    if tree_z.tobytes() != snap_z.tobytes():
        bad = [i for i in range(tree_z.size) if tree_z[i] != snap_z[i]]
        raise AssertionError(
            f"recorded Redshifts differ at {len(bad)} of {tree_z.size} entries, first at "
            f"index {bad[0]}: {tree_z[bad[0]]!r} vs {snap_z[bad[0]]!r}"
        )

    # The scale-factor table the run derives from those redshifts. Equal
    # redshifts make this equal by construction; it is asserted anyway because it
    # is the table the physics actually integrates over, and deriving it here
    # would catch a redshift table that compared equal only as bytes.
    tree_a = 1.0 / (1.0 + tree_z)
    snap_a = 1.0 / (1.0 + snap_z)
    if tree_a.tobytes() != snap_a.tobytes():
        bad = [i for i in range(tree_a.size) if tree_a[i] != snap_a[i]]
        raise AssertionError(
            f"derived scale factors 1/(1+z) differ at {len(bad)} entries, first at "
            f"index {bad[0]}: {tree_a[bad[0]]!r} vs {snap_a[bad[0]]!r}"
        )
    log(f"  preflight: {tree_z.size} recorded redshifts and derived scale factors exactly equal")

    for name in PREFLIGHT_ATTRS:
        for attrs, run in ((tree_attrs, tree_run), (snap_attrs, snapshot_run)):
            if name not in attrs:
                raise AssertionError(f"{run.master}: RunProperties has no {name} attribute")
        if tree_attrs[name] != snap_attrs[name]:
            raise AssertionError(
                f"RunProperties/{name} differs between the runs: "
                f"{tree_attrs[name]} vs {snap_attrs[name]}"
            )
    log(f"  preflight: {', '.join(PREFLIGHT_ATTRS)} exactly equal")

    # Field names, their order and their units must match exactly. Descriptions
    # must not: they are prose owned by each simulation package, and the two
    # packages legitimately describe the same quantity's provenance differently
    # (micro-uchuu-ascii's Spin is "applied by reader before bridging", the
    # snapshot package's is "applied by the producer before emission"). Nothing
    # compared below is derived from a description, so a difference there is
    # reported and does not fail the pair.
    if tree_fields.dtype != snap_fields.dtype or tree_fields.shape != snap_fields.shape:
        raise AssertionError(
            f"FieldMetadata tables differ in shape or columns: "
            f"{tree_fields.dtype}{tree_fields.shape} vs {snap_fields.dtype}{snap_fields.shape}"
        )
    tree_names = [row["field_name"] for row in tree_fields]
    snap_names = [row["field_name"] for row in snap_fields]
    if tree_names != snap_names:
        raise AssertionError(
            f"FieldMetadata field names or their order differ:\n"
            f"  tree: {tree_names}\n  snap: {snap_names}"
        )
    unit_mismatches = [
        f"{row['field_name'].decode()}: {row['units']!r} vs {snap_fields[index]['units']!r}"
        for index, row in enumerate(tree_fields)
        if row["units"] != snap_fields[index]["units"]
    ]
    if unit_mismatches:
        raise AssertionError(f"FieldMetadata units differ: {unit_mismatches}")
    for index, row in enumerate(tree_fields):
        if row["description"] != snap_fields[index]["description"]:
            log(
                f"  preflight: note -- {row['field_name'].decode()} carries a package-specific "
                f"description on each side (prose only, not compared)"
            )

    tree_signature = record_signature(tree_run)
    snap_signature = record_signature(snapshot_run)
    if tree_signature != snap_signature:
        raise AssertionError(
            f"Galaxies record schemas differ:\n  tree: {tree_signature}\n  snap: {snap_signature}"
        )
    log(
        f"  preflight: FieldMetadata and record schema identical "
        f"({len(tree_fields)} fields, in order)"
    )

    fields = len(tree_fields)
    if fields != len(tree_signature):
        raise AssertionError(
            f"FieldMetadata lists {fields} fields but the record carries {len(tree_signature)}"
        )
    schema_fields = json.loads(tree_run.schema_path().read_text())["fields"]
    if len(schema_fields) != fields:
        raise AssertionError(
            f"{tree_run.schema_path()} lists {len(schema_fields)} fields, "
            f"FieldMetadata lists {fields}"
        )

    tree_records = recorded_records(tree_run)
    snap_records = recorded_records(snapshot_run)
    if tree_records != snap_records:
        raise AssertionError(
            f"the two runs record different galaxy counts per output snapshot:\n"
            f"  tree: {tree_records}\n  snap: {snap_records}"
        )

    # Equal counts on both sides say the two runs agree; they do not say the runs
    # wrote what was asked for. Both of those must hold, or a regression that
    # drops the same snapshot -- or empties it -- on both sides passes the gate
    # having compared a subset, or nothing at all.
    expected = requested_snapshots(tree_run)
    snapshot_expected = requested_snapshots(snapshot_run)
    if expected != snapshot_expected:
        raise AssertionError(
            f"the two run files request different output snapshots: "
            f"{sorted(expected)} vs {sorted(snapshot_expected)}"
        )
    log(f"  preflight: run files request output snapshots {sorted(expected)}")
    assert_snapshot_coverage(tree_run, tree_records, expected)
    assert_snapshot_coverage(snapshot_run, snap_records, expected)

    total = sum(tree_records.values())
    if total <= 0:
        raise AssertionError(f"{tree_run.key}: the runs recorded no galaxies at all")
    log(
        f"  preflight: {total} galaxies over {len(tree_records)} output "
        f"snapshots recorded on both sides, all {len(expected)} requested and none empty"
    )
    return total, fields


def record_signature(run: RunOutput):
    """The Galaxies record schema of a run, from its first partition."""
    partition = run.partitions()[0]
    with h5py.File(partition, "r") as handle:
        for name in handle:
            if SNAP_GROUP_RE.match(name) and "Galaxies" in handle[name]:
                return comparator.schema_signature(handle[f"{name}/Galaxies"].dtype)
    raise AssertionError(f"{partition}: no Snap###/Galaxies dataset")


def recorded_records(run: RunOutput) -> dict[int, int]:
    """Galaxies per output snapshot, as the run's own master file records them.

    Read from the master's per-partition TotHalosPerSnap attributes rather than
    from the comparator, so the count the comparison reports can be checked
    against a number the comparator had no part in producing.
    """
    counts: dict[int, int] = {}
    with h5py.File(run.master, "r") as handle:
        for name in handle:
            match = SNAP_GROUP_RE.match(name)
            if match is None:
                continue
            total = 0
            for child in handle[name]:
                if FILE_GROUP_RE.match(child) is None:
                    continue
                attrs = handle[f"{name}/{child}"].attrs
                if "TotHalosPerSnap" not in attrs:
                    raise AssertionError(f"{run.master}: {name}/{child} has no TotHalosPerSnap")
                total += int(numpy.asarray(attrs["TotHalosPerSnap"]).ravel()[0])
            counts[int(match.group(1))] = total
    if not counts:
        raise AssertionError(f"{run.master}: no Snap### groups")
    return counts


# --------------------------------------------------------------------------
# Comparison
# --------------------------------------------------------------------------


def compare_pair(
    model: str, scheme: str, tree_run: RunOutput, snapshot_run: RunOutput, records: int, fields: int
) -> None:
    """Run the committed comparator over one pair and check what it compared."""
    command = [
        sys.executable,
        str(REPO_ROOT / "scripts" / "compare_cross_format_identity.py"),
        tree_run.spec,
        snapshot_run.spec,
        "--left-label",
        "tree-ordered",
        "--right-label",
        "snapshot-ordered",
    ]
    log(f"  comparing {model}/{scheme}: {' '.join(command[1:])}")
    started = time.monotonic()
    completed = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True)
    elapsed = time.monotonic() - started
    for line in completed.stdout.splitlines():
        log(f"     | {line}")
    for line in completed.stderr.splitlines():
        log(f"     ! {line}")
    if completed.returncode != 0:
        raise AssertionError(
            f"{model}/{scheme}: cross-format identity comparison failed "
            f"(exit {completed.returncode}); see the comparator output above"
        )

    match = None
    for line in completed.stdout.splitlines():
        match = COMPARATOR_PASS_RE.match(line.strip()) or match
    if match is None:
        raise AssertionError(
            f"{model}/{scheme}: the comparator exited 0 without a PASSED summary line; "
            f"its output cannot be taken as evidence"
        )

    compared_records, compared_snapshots, compared_fields = (int(value) for value in match.groups())
    expected_snapshots = len(recorded_records(tree_run))
    if compared_records <= 0:
        raise AssertionError(
            f"{model}/{scheme}: the comparator reported success over {compared_records} "
            f"galaxies; a comparison of nothing is not a pass"
        )
    if compared_records != records:
        raise AssertionError(
            f"{model}/{scheme}: the comparator compared {compared_records} galaxies but the two "
            f"runs record {records}; a partition or snapshot was not compared"
        )
    if compared_snapshots != expected_snapshots:
        raise AssertionError(
            f"{model}/{scheme}: the comparator compared {compared_snapshots} output snapshots, "
            f"the runs wrote {expected_snapshots}"
        )
    if compared_fields != fields:
        raise AssertionError(
            f"{model}/{scheme}: the comparator compared {compared_fields} fields, the record "
            f"schema and output_schema.json carry {fields}"
        )

    tree_partitions = len(tree_run.partitions())
    snapshot_partitions = len(snapshot_run.partitions())
    if tree_partitions < 2:
        raise AssertionError(
            f"{model}/{scheme}: the tree-ordered run wrote {tree_partitions} partition file(s); "
            f"micro-uchuu-ascii must write several, or partition aggregation is untested"
        )
    log(
        f"  PASS {model}/{scheme}: {compared_records} records, {compared_fields} fields, "
        f"{compared_snapshots} output snapshots compared bitwise "
        f"(tree-ordered {tree_partitions} partition(s) vs snapshot-ordered "
        f"{snapshot_partitions} partition(s), {elapsed:.0f}s)"
    )


def identity_stage(model: str, scheme: str) -> None:
    banner(f"Identity gate: {model}, {scheme} timesteps")
    GATE.require("builds", f"not running {model}/{scheme}")

    tree_run = execute_run(model, ASCII_SIMULATION, scheme)
    snapshot_run = execute_run(model, SNAPSHOT_SIMULATION, scheme)
    records, fields = preflight(tree_run, snapshot_run)
    compare_pair(model, scheme, tree_run, snapshot_run, records, fields)
    GATE.done.add(f"identity:{model}:{scheme}")


def stage_halos_only_fixed():
    """halos-only, fixed timesteps: the driver alone, compared bitwise."""
    identity_stage("halos-only", "fixed")


def stage_halos_only_dynamic():
    """halos-only, dynamic timesteps."""
    identity_stage("halos-only", "dynamic")


def require_halos_only_complete() -> None:
    """Both halos-only identity stages must pass before any sage16 run starts.

    Either divergence is a driver bug, and the contract is that a driver bug is
    reported before the sage16 cost is paid -- so a dynamic failure must stop the
    fixed sage16 run just as surely as a fixed failure stops it.
    """
    for scheme in SCHEMES:
        GATE.require(
            f"identity:halos-only:{scheme}",
            "a halos-only divergence is a driver bug and is reported before the sage16 "
            "cost is paid; neither sage16 stage runs until both halos-only stages pass",
        )


def stage_sage16_fixed():
    """sage16, fixed timesteps: the full physics pipeline, compared bitwise."""
    require_halos_only_complete()
    identity_stage("sage16", "fixed")


def stage_sage16_dynamic():
    """sage16, dynamic timesteps."""
    require_halos_only_complete()
    identity_stage("sage16", "dynamic")


# --------------------------------------------------------------------------
# Tree-path preservation against the pre-Phase-5 baseline
# --------------------------------------------------------------------------


def h5_structure(path: Path) -> dict:
    """Walk one HDF5 file: every object, attribute, link and non-record payload.

    Galaxies datasets are recorded by schema and shape only; their records are
    compared separately and byte-wise. Everything else -- including the payloads
    of Redshifts and FieldMetadata -- is captured as raw bytes, because this walk
    is the metadata delta check and a delta hidden in a payload is still a delta.
    """
    structure: dict = {}

    def attrs_of(obj):
        return {name: attr_value(value) for name, value in obj.attrs.items()}

    def walk(group, prefix):
        entry = {"kind": "group", "attrs": attrs_of(group), "links": {}}
        structure[prefix or "/"] = entry
        for key in sorted(group.keys()):
            link = group.get(key, getlink=True)
            child_path = f"{prefix}/{key}" if prefix else f"/{key}"
            if isinstance(link, h5py.ExternalLink):
                entry["links"][key] = ("external", link.filename, link.path)
                continue
            if isinstance(link, h5py.SoftLink):
                entry["links"][key] = ("soft", link.path)
                continue
            entry["links"][key] = ("hard",)
            child = group[key]
            if isinstance(child, h5py.Group):
                walk(child, child_path)
            else:
                record = {
                    "kind": "dataset",
                    "attrs": attrs_of(child),
                    "dtype": str(child.dtype),
                    "shape": child.shape,
                }
                if key != "Galaxies":
                    record["payload"] = numpy.array(child[()]).tobytes()
                structure[child_path] = record

    with h5py.File(path, "r") as handle:
        walk(handle, "")
    return structure


class Delta:
    """One metadata difference, with enough context to classify or report it."""

    def __init__(self, where, objpath, item, detail):
        self.where = where
        self.objpath = objpath
        self.item = item
        self.detail = detail

    def __str__(self):
        return f"{self.where}:{self.objpath} {self.item}: {self.detail}"


def diff_structures(where: str, left: dict, right: dict) -> list[Delta]:
    """Every difference between two walked files, unclassified."""
    deltas: list[Delta] = []
    for objpath in sorted(set(left) | set(right)):
        if objpath not in right:
            deltas.append(Delta(where, objpath, "object", "present only in the baseline"))
            continue
        if objpath not in left:
            deltas.append(Delta(where, objpath, "object", "present only at HEAD"))
            continue
        before, after = left[objpath], right[objpath]
        if before["kind"] != after["kind"]:
            deltas.append(Delta(where, objpath, "kind", f"{before['kind']} -> {after['kind']}"))
            continue

        for name in sorted(set(before["attrs"]) | set(after["attrs"])):
            if name not in after["attrs"]:
                deltas.append(Delta(where, objpath, f"attr {name}", "removed"))
                continue
            if name not in before["attrs"]:
                deltas.append(Delta(where, objpath, f"attr {name}", "added"))
                continue
            old_dtype, old_shape, old_bytes = before["attrs"][name]
            new_dtype, new_shape, new_bytes = after["attrs"][name]
            if (old_dtype, old_shape) != (new_dtype, new_shape):
                deltas.append(
                    Delta(
                        where,
                        objpath,
                        f"attr {name}",
                        f"dtype {old_dtype}{old_shape} -> {new_dtype}{new_shape}",
                    )
                )
                old_value = numpy.frombuffer(old_bytes, dtype=old_dtype)
                new_value = numpy.frombuffer(new_bytes, dtype=new_dtype)
                if old_value.tolist() != new_value.tolist():
                    deltas.append(
                        Delta(
                            where,
                            objpath,
                            f"attr {name}",
                            f"value {old_value.tolist()} -> {new_value.tolist()}",
                        )
                    )
            elif old_bytes != new_bytes:
                old_value = numpy.frombuffer(old_bytes, dtype=old_dtype)
                new_value = numpy.frombuffer(new_bytes, dtype=new_dtype)
                deltas.append(
                    Delta(
                        where,
                        objpath,
                        f"attr {name}",
                        f"value {old_value.tolist()} -> {new_value.tolist()}",
                    )
                )

        if before["kind"] == "group":
            if before["links"] != after["links"]:
                deltas.append(
                    Delta(where, objpath, "links", f"{before['links']} -> {after['links']}")
                )
            continue

        if before["dtype"] != after["dtype"]:
            deltas.append(
                Delta(where, objpath, "dataset dtype", f"{before['dtype']} -> {after['dtype']}")
            )
        if before["shape"] != after["shape"]:
            deltas.append(
                Delta(where, objpath, "dataset shape", f"{before['shape']} -> {after['shape']}")
            )
        if "payload" in before and before.get("payload") != after.get("payload"):
            deltas.append(Delta(where, objpath, "dataset payload", "differs"))
    return deltas


def field_metadata_delta(where, objpath, baseline: Path, head: Path) -> list[Delta]:
    """Explain a FieldMetadata payload difference row by row and column by column."""
    with h5py.File(baseline, "r") as handle:
        before = numpy.array(handle[objpath.lstrip("/")][()])
    with h5py.File(head, "r") as handle:
        after = numpy.array(handle[objpath.lstrip("/")][()])
    if before.shape != after.shape:
        return [Delta(where, objpath, "FieldMetadata", f"{before.shape} -> {after.shape} rows")]

    deltas = []
    columns = before.dtype.names
    for index in range(before.shape[0]):
        for column in columns:
            if before[index][column] == after[index][column]:
                continue
            name = before[index]["field_name"].decode()
            deltas.append(Delta(where, objpath, f"FieldMetadata {name}.{column}", "differs"))
    return deltas


def classify(delta: Delta) -> str | None:
    """Map one difference to a permitted delta, to provenance, or to None.

    Every acceptance below is bound to three things at once: the attribute (or
    metadata column), the object path it occurred at, and the exact before/after
    transition. Matching on the name alone would accept a version string moving
    to any value, a counter widening to any width, or a provenance name changing
    at a path where it does not belong -- all of which are real metadata changes
    that the Definition of Done's "exactly four permitted deltas" excludes.
    Anything unmatched returns None and fails the stage as unclassified.
    """
    item = delta.item
    if item.startswith("attr "):
        name = item.split(" ", 1)[1]

        provenance_paths = PROVENANCE_ATTR_PATHS.get(name)
        if provenance_paths is not None:
            return "provenance" if delta.objpath in provenance_paths else None

        if name == "UniqueGalaxyIDMultiplier":
            if delta.objpath == RUN_PROPERTIES_PATH and delta.detail == "added":
                return "UniqueGalaxyIDMultiplier attribute"
            return None

        if name == "TotHalosPerSnap":
            at_expected_path = MASTER_SNAP_FILE_RE.match(delta.objpath) or (
                PARTITION_SNAP_GALAXIES_RE.match(delta.objpath)
            )
            transition = TOTHALOS_DTYPE_TRANSITION_RE.match(delta.detail)
            # Same shape on both sides: this delta is a widening, not a reshape.
            if at_expected_path and transition and transition.group(1) == transition.group(2):
                return "TotHalosPerSnap int64"
            return None

        if name == "hdf5_format_version":
            if delta.objpath == VERSION_GROUP_PATH and FORMAT_VERSION_TRANSITION_RE.match(
                delta.detail
            ):
                return "hdf5_format_version 1.2"
            return None

    if item == "FieldMetadata UniqueGalaxyID.description":
        return "UniqueGalaxyID description" if delta.objpath == FIELD_METADATA_PATH else None
    return None


def assert_records_byte_identical(baseline: RunOutput, head: RunOutput) -> int:
    """Every galaxy record of every snapshot of every partition, field by field.

    Compared per field rather than per whole record: the compound type carries
    padding between fields (160-byte record over 152 bytes of fields), and those
    padding bytes are not written by either run, so a whole-record comparison
    would test uninitialised memory.
    """
    baseline_files = baseline.partitions()
    head_files = head.partitions()
    if [path.name for path in baseline_files] != [path.name for path in head_files]:
        raise AssertionError(
            f"partition file sets differ: {[p.name for p in baseline_files]} vs "
            f"{[p.name for p in head_files]}"
        )

    total = 0
    for before_path, after_path in zip(baseline_files, head_files):
        with h5py.File(before_path, "r") as before, h5py.File(after_path, "r") as after:
            snaps = sorted(name for name in before if SNAP_GROUP_RE.match(name))
            after_snaps = sorted(name for name in after if SNAP_GROUP_RE.match(name))
            if snaps != after_snaps:
                raise AssertionError(
                    f"{before_path.name}: snapshot groups {snaps} vs {after_snaps}"
                )
            for name in snaps:
                left = before[f"{name}/Galaxies"]
                right = after[f"{name}/Galaxies"]
                left_signature = comparator.schema_signature(left.dtype)
                right_signature = comparator.schema_signature(right.dtype)
                if left_signature != right_signature:
                    raise AssertionError(
                        f"{before_path.name}/{name}: record schema changed:\n"
                        f"  baseline: {left_signature}\n  HEAD:     {right_signature}"
                    )
                if left.shape != right.shape:
                    raise AssertionError(
                        f"{before_path.name}/{name}: {left.shape[0]} records at the baseline, "
                        f"{right.shape[0]} at HEAD"
                    )
                left_records = left[()]
                right_records = right[()]
                for field in left_records.dtype.names:
                    left_bytes = comparator.field_bytes(left_records[field])
                    right_bytes = comparator.field_bytes(right_records[field])
                    if not numpy.array_equal(left_bytes, right_bytes):
                        differing = int((left_bytes != right_bytes).any(axis=1).sum())
                        raise AssertionError(
                            f"{before_path.name}/{name}: field {field} differs from the "
                            f"pre-Phase-5 baseline in {differing} of {left.shape[0]} records"
                        )
                total += int(left.shape[0])
                del left_records, right_records
    return total


def assert_output_schema_delta(baseline: RunOutput, head: RunOutput) -> None:
    """The run-local output schema differs in exactly the description and source_md5."""
    before = json.loads(baseline.schema_path().read_text())
    after = json.loads(head.schema_path().read_text())

    differences = []

    def compare(left, right, path):
        if isinstance(left, dict) and isinstance(right, dict):
            for key in sorted(set(left) | set(right)):
                if key not in left or key not in right:
                    differences.append(f"{path}.{key} (present on one side only)")
                    continue
                compare(left[key], right[key], f"{path}.{key}")
        elif isinstance(left, list) and isinstance(right, list):
            if len(left) != len(right):
                differences.append(f"{path} (list length {len(left)} vs {len(right)})")
                return
            for index, (a, b) in enumerate(zip(left, right)):
                label = a.get("name", index) if isinstance(a, dict) else index
                compare(a, b, f"{path}[{label}]")
        elif left != right:
            differences.append(path)

    compare(before, after, "")

    expected = {".source_md5", ".fields[UniqueGalaxyID].description"}
    if set(differences) != expected:
        raise AssertionError(
            f"{head.schema_path()} differs from the pre-Phase-5 baseline in "
            f"{sorted(differences)}, expected exactly {sorted(expected)}"
        )
    log(f"  output_schema.json differs in exactly {sorted(expected)}")


def stage_tree_path_preservation():
    """The tree-ordered path still produces the pre-Phase-5 baseline's galaxies."""
    banner(f"Stage 8: tree-path preservation against {BASELINE_COMMIT}")
    GATE.require("identity:halos-only:fixed", "no HEAD tree-ordered run to compare against")

    head_run = GATE.runs[f"halos-only__{ASCII_SIMULATION}__fixed"]

    worktree = build_worktree("baseline", BASELINE_COMMIT, "halos-only", ASCII_SIMULATION)
    # Each side runs its own worktree's copy; they are required to be identical
    # bytes, so the two runs provably share one input without either of them
    # reaching into the working tree.
    baseline_committed = worktree / committed_run_file("halos-only", ASCII_SIMULATION).relative_to(
        REPO_ROOT
    )
    if baseline_committed.read_bytes() != head_run.run_file.read_bytes():
        raise AssertionError(
            f"the run file changed since {BASELINE_COMMIT}: {baseline_committed} differs from "
            f"{head_run.run_file}; the two runs would not share an input"
        )

    output_root = GATE.scratch_dir("runs", "baseline")
    link = worktree / "output"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(output_root)
    env = worktree_env(worktree, "halos-only", ASCII_SIMULATION)
    run_logged(
        ["./mimic", str(baseline_committed)],
        worktree,
        env,
        GATE.scratch_dir("logs") / "baseline-run.log",
        f"baseline ({BASELINE_COMMIT}): mimic run",
        show_tail=3,
    )
    declared_directory, basename = read_run_file_output(baseline_committed)
    baseline_run = RunOutput(
        "baseline",
        output_root / Path(declared_directory).name,
        basename,
        worktree,
        baseline_committed,
    )
    if not baseline_run.master.is_file():
        raise AssertionError(f"baseline run produced no master at {baseline_run.master}")

    records = assert_records_byte_identical(baseline_run, head_run)
    log(f"  {records} galaxy records byte-identical to the {BASELINE_COMMIT} baseline")

    files = [(baseline_run.master, head_run.master)] + list(
        zip(baseline_run.partitions(), head_run.partitions())
    )
    deltas: list[Delta] = []
    for before_path, after_path in files:
        where = after_path.name
        for delta in diff_structures(where, h5_structure(before_path), h5_structure(after_path)):
            if delta.item == "dataset payload" and delta.objpath.endswith("FieldMetadata"):
                deltas.extend(field_metadata_delta(where, delta.objpath, before_path, after_path))
            else:
                deltas.append(delta)

    observed: dict[str, int] = {name: 0 for name in PERMITTED_DELTAS}
    provenance = 0
    unclassified = []
    for delta in deltas:
        kind = classify(delta)
        if kind == "provenance":
            provenance += 1
        elif kind in observed:
            observed[kind] += 1
        else:
            unclassified.append(delta)

    log(
        f"  {len(files)} HDF5 files walked; {provenance} excluded provenance attribute difference(s)"
    )
    for name, count in observed.items():
        log(f"    delta observed: {name} ({count} occurrence(s))")
    if unclassified:
        detail = "\n".join(f"    {delta}" for delta in unclassified[:20])
        raise AssertionError(
            f"{len(unclassified)} HDF5 metadata difference(s) beyond the four permitted deltas "
            f"and the five excluded provenance attributes:\n{detail}"
        )
    missing = [name for name, count in observed.items() if count == 0]
    if missing:
        raise AssertionError(
            f"permitted delta(s) never observed: {missing}; the evidence does not show the "
            f"schema change actually reaching the output"
        )

    assert_permitted_delta_values(baseline_run, head_run)
    assert_output_schema_delta(baseline_run, head_run)
    GATE.done.add("preservation")


def assert_permitted_delta_values(baseline: RunOutput, head: RunOutput) -> None:
    """Pin what each permitted delta changed from and to, not merely that it moved."""
    with h5py.File(baseline.master, "r") as before, h5py.File(head.master, "r") as after:
        old_version = numpy.asarray(before["RunProperties/Version"].attrs["hdf5_format_version"])
        new_version = numpy.asarray(after["RunProperties/Version"].attrs["hdf5_format_version"])
        if old_version.ravel()[0] != b"1.1" or new_version.ravel()[0] != b"1.2":
            raise AssertionError(
                f"hdf5_format_version moved {old_version.ravel()[0]!r} -> "
                f"{new_version.ravel()[0]!r}, expected b'1.1' -> b'1.2'"
            )
        if "UniqueGalaxyIDMultiplier" in before["RunProperties"].attrs:
            raise AssertionError("the pre-Phase-5 baseline already records a multiplier attribute")
        multiplier = int(numpy.asarray(after["RunProperties"].attrs["UniqueGalaxyIDMultiplier"])[0])
        if multiplier <= 0:
            raise AssertionError(f"UniqueGalaxyIDMultiplier is {multiplier}")

        snap = sorted(name for name in before if SNAP_GROUP_RE.match(name))[0]
        old_total = before[f"{snap}/File000"].attrs["TotHalosPerSnap"]
        new_total = after[f"{snap}/File000"].attrs["TotHalosPerSnap"]
        if numpy.asarray(old_total).dtype != numpy.dtype("int32"):
            raise AssertionError(
                f"baseline TotHalosPerSnap dtype is {numpy.asarray(old_total).dtype}"
            )
        if numpy.asarray(new_total).dtype != numpy.dtype("int64"):
            raise AssertionError(f"HEAD TotHalosPerSnap dtype is {numpy.asarray(new_total).dtype}")
        if int(numpy.asarray(old_total)[0]) != int(numpy.asarray(new_total)[0]):
            raise AssertionError("TotHalosPerSnap changed value, not only width")

        old_rows = numpy.array(before["RunProperties/FieldMetadata"][()])
        new_rows = numpy.array(after["RunProperties/FieldMetadata"][()])
        row = [index for index, r in enumerate(old_rows) if r["field_name"] == b"UniqueGalaxyID"]
        if not row:
            raise AssertionError("no UniqueGalaxyID row in FieldMetadata")
        old_description = old_rows[row[0]]["description"]
        new_description = new_rows[row[0]]["description"]
        if old_description == new_description:
            raise AssertionError("the UniqueGalaxyID description did not change")
        if b"UniqueGalaxyIDMultiplier" not in new_description:
            raise AssertionError(
                f"the new UniqueGalaxyID description does not name the provenance attribute: "
                f"{new_description!r}"
            )
    log("  all four permitted deltas observed with their expected before/after values")


# --------------------------------------------------------------------------


STAGES = [
    stage_preconditions,
    stage_run_file_diffs,
    stage_build_worktrees,
    stage_halos_only_fixed,
    stage_halos_only_dynamic,
    stage_sage16_fixed,
    stage_sage16_dynamic,
    stage_tree_path_preservation,
]


def main() -> int:
    atexit.register(cleanup)
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, _cleanup_on_signal)
    try:
        return run_test_suite(STAGES, "Cross-format identity gate (test_cross_format_identity.py)")
    finally:
        cleanup()


if __name__ == "__main__":
    sys.exit(main())
