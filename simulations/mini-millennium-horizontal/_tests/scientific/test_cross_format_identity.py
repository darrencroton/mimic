#!/usr/bin/env python3
"""
Gapped mini-Millennium parity gate: vertical L-Halo against horizontal version 3.

The acceptance gate of the general horizontal runtime plan
(docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md, Slice 6). The
same real mini-Millennium merger trees, read through the vertical driver
(``mini-millennium``, L-Halo binary ``trees_063.0``-``trees_063.7``) and through
the horizontal driver (``mini-millennium-horizontal``, a version 3 conversion of
those same eight files carrying 29,291 gapped descendant links), must produce,
for every output snapshot, identical ``UniqueGalaxyID`` sets and per-id
**byte-identical** fields. There is no tolerance, no field exclusion and no
sampling anywhere in it.

It is a manual, dataset-present operation, registered only when the selected
package is ``mini-millennium-horizontal``::

    make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific

``MODEL=`` selects only the ambient tier build. One invocation runs all four
parity legs itself -- ``halos-only``/fixed, ``halos-only``/dynamic,
``sage16``/fixed and ``sage16``/dynamic -- from executables it builds in
isolated worktrees at HEAD. A missing dataset, a dataset that is not the real
complete conversion, or a leg that does not run **fails** the gate: it never
skips, because a gate that reports success having compared nothing is worse
than no gate.

Stages, in order (one MIMIC_RESULT marker each):

1. Preconditions -- both datasets resolve, with named paths.
2. Dataset provenance -- every snapshot file is version 3 from ``lhalo_binary``
   under the pinned ``column_mapping_sha256``, with ``links_adjacent = 0``; the
   files hold 1,533,122 halos and 29,291 gapped descendant links; and the
   conversion inventory (``forests.h5``'s ``SourceFileOrdinal``) names exactly
   the tree files the vertical package reads.
3. Run files and comparator -- the comparator script matches its committed
   HEAD copy byte for byte, as do all four run files; and each horizontal run
   file differs from its vertical counterpart in exactly ``simulation.name``
   and ``output.output_directory`` (plus the leading header comment).
4. Builds -- one detached git worktree per ``{halos-only, sage16} x {vertical,
   horizontal}`` pair at HEAD. The ambient tier build is never touched.
5-8. Parity legs -- ``halos-only`` then ``sage16``, each under fixed and then
   dynamic timesteps. Every leg runs regardless of the others' outcome, so each
   divergence is reported rather than hidden behind the first one. A leg runs
   both orderings from the worktrees' own run files, confirms the vertical run
   read the inventory's tree files, checks the structural preflight
   equalities, compares with ``scripts/compare_cross_format_identity.py``
   (unchanged; it reports every divergence by snapshot, field and example id),
   and then checks that each run wrote exactly the requested snapshots, none
   empty, and that the comparator compared every record the runs wrote.
9. Leg verdicts -- names each of the four legs' verdict and fails unless all
   four passed. A leg that did not run is a failure.

Worktrees and scratch outputs are removed on every exit path; the run logs are
kept only in the gate's own output.
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
# the script for the run-vs-run comparison and imports its helpers only to read
# the record schema the same way it does. Both uses read the working-tree copy,
# so stage 3 (and every comparison) requires it to match HEAD byte for byte: a
# dirty comparator must never certify parity on the gate's behalf.
import compare_cross_format_identity as comparator  # noqa: E402
from framework import run_test_suite  # noqa: E402  (path set up above)

#: The vertical package and the horizontal package holding the same
#: mini-Millennium merger trees in two on-disk formats.
VERTICAL_SIMULATION = "mini-millennium"
HORIZONTAL_SIMULATION = "mini-millennium-horizontal"

#: Models compared, in the order they run.
MODELS = ("halos-only", "sage16")

#: Timestep schemes compared. "fixed" uses the committed run file unchanged;
#: "dynamic" adds exactly one line to it.
SCHEMES = ("fixed", "dynamic")

#: The four parity legs, in the order they run and are reported.
LEGS = tuple((model, scheme) for model in MODELS for scheme in SCHEMES)

#: What makes the horizontal dataset the real complete conversion rather than a
#: fixture or a partial one. The figures are the converter's own report for all
#: eight trees_063.* files with simulations/mini-millennium/converter_columns.yaml
#: (docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md); the digest is the
#: column_mapping_sha256 that profile produces.
EXPECTED_FORMAT_VERSION = 3
EXPECTED_SOURCE_FORMAT = "lhalo_binary"
EXPECTED_COLUMN_MAPPING_SHA256 = "5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1"
EXPECTED_HALOS = 1_533_122
EXPECTED_GAPPED_DESCENDANTS = 29_291
EXPECTED_MAX_DESCENDANT_SPAN = 2

#: The conversion inventory: one row per forest, naming the source file it came from.
FORESTS_FILE = "forests.h5"
SOURCE_FILE_ORDINAL = "SourceFileOrdinal"

#: Free space the harness needs for four builds and eight mini-Millennium runs.
REQUIRED_FREE_BYTES = 10 * 1024**3

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

#: The only functional keys the horizontal run file may change relative to its
#: vertical counterpart. `simulation.name` selects the package under test;
#: `output.output_directory` keeps the two runs from writing over each other.
AUTHORIZED_KEY_CHANGES = ("simulation.name", "output.output_directory")

#: The committed comparator the gate runs and imports; pinned to HEAD.
COMPARATOR_SCRIPT = REPO_ROOT / "scripts" / "compare_cross_format_identity.py"

#: The run-file key the dynamic legs add. Mimic's parameter reader takes the
#: first matching key, so a committed run file already carrying it would make a
#: "dynamic" leg silently run whatever that first value says.
TIMESTEP_SCHEME_KEY = "TimestepScheme"

SNAP_GROUP_RE = re.compile(r"^Snap(\d+)$")
FILE_GROUP_RE = re.compile(r"^File(\d+)$")
SNAPSHOT_FILE_RE = re.compile(r"^snapshot_(\d{3})\.h5$")

COMPARATOR_PASS_RE = re.compile(
    r"^PASSED: (\d+) galaxies over (\d+) output snapshot\(s\) are bitwise identical "
    r"in all (\d+) field"
)


# --------------------------------------------------------------------------
# Progress reporting
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
        self.done: set[str] = set()
        #: The vertical package's tree files, from its simulation_info.yaml.
        self.vertical_inventory: dict | None = None
        #: (model, scheme) -> "PASS" or the first line of the leg's failure.
        self.verdicts: dict[tuple[str, str], str] = {}

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


def decoded(value) -> str:
    """An HDF5 string attribute as a Python str, whatever h5py returned for it."""
    value = numpy.asarray(value).ravel()[0]
    return value.decode() if isinstance(value, bytes) else str(value)


# --------------------------------------------------------------------------
# Stage 1: preconditions
# --------------------------------------------------------------------------


def package_path(package: str, *parts: str) -> Path:
    return REPO_ROOT.joinpath("simulations", package, *parts)


def snapshot_list_entries(path: Path) -> list[str]:
    """Return the scale factors listed in an a_list file."""
    return [line.strip() for line in path.read_text().splitlines() if line.strip()]


def assert_dataset_present(package: str, required_files) -> Path:
    """Fail -- never skip -- when a package's machine-local dataset is absent."""
    link = package_path(package, "snapshots")
    if not link.exists():
        target = os.readlink(link) if link.is_symlink() else "(no symlink)"
        raise AssertionError(
            f"{package} dataset is not available: {link} does not resolve "
            f"(symlink target: {target}). The gate requires the real mini-Millennium "
            f"trees and their version 3 conversion; it fails rather than skipping."
        )
    if not link.is_dir():
        raise AssertionError(f"{package} dataset path is not a directory: {link}")
    for name in required_files:
        entry = link / name
        if not entry.exists():
            raise AssertionError(f"{package} dataset is incomplete: missing {entry}")
    return link


def read_vertical_inventory() -> dict:
    """The tree files the vertical package reads, as its simulation_info.yaml names them."""
    info_path = package_path(VERTICAL_SIMULATION, "simulation_info.yaml")
    info = (yaml.safe_load(info_path.read_text()) or {}).get("input") or {}
    for key in ("tree_type", "tree_name", "first_file", "last_file"):
        if key not in info:
            raise AssertionError(f"{info_path}: input.{key} is not declared")
    if info["tree_type"] != EXPECTED_SOURCE_FORMAT:
        raise AssertionError(
            f"{info_path}: input.tree_type is {info['tree_type']!r}; the gate compares "
            f"against the {EXPECTED_SOURCE_FORMAT} vertical reader"
        )
    first, last = int(info["first_file"]), int(info["last_file"])
    if last < first:
        raise AssertionError(f"{info_path}: last_file {last} is before first_file {first}")
    tree_name = str(info["tree_name"])
    return {
        "tree_name": tree_name,
        "first_file": first,
        "last_file": last,
        "files": [f"{tree_name}.{index}" for index in range(first, last + 1)],
    }


def scratch_parent() -> Path:
    """The repository's machine-local, gitignored `output/` if present, else the system tmp."""
    output = REPO_ROOT / "output"
    return output if output.is_dir() else Path(tempfile.gettempdir())


def stage_preconditions():
    """Both mini-Millennium datasets resolve, with named paths on failure."""
    banner("Stage 1: preconditions")

    inventory = read_vertical_inventory()
    vertical_data = assert_dataset_present(VERTICAL_SIMULATION, inventory["files"])
    log(
        f"  {VERTICAL_SIMULATION}: {vertical_data} -> {vertical_data.resolve()} "
        f"({len(inventory['files'])} tree files: {', '.join(inventory['files'])})"
    )

    # The snapshot files the reader needs are the ones the package's a_list
    # names, one per entry, plus the conversion inventory.
    alist = package_path(HORIZONTAL_SIMULATION, "mini-millennium.a_list")
    if not alist.is_file():
        raise AssertionError(f"horizontal package a_list is missing: {alist}")
    entries = snapshot_list_entries(alist)
    required = tuple(f"snapshot_{index:03d}.h5" for index in range(len(entries)))
    horizontal_data = assert_dataset_present(HORIZONTAL_SIMULATION, (*required, FORESTS_FILE))
    log(
        f"  {HORIZONTAL_SIMULATION}: {horizontal_data} -> {horizontal_data.resolve()} "
        f"({len(required)} snapshot files from {len(entries)} a_list entries, plus {FORESTS_FILE})"
    )

    parent = scratch_parent()
    usage = shutil.disk_usage(parent)
    if usage.free < REQUIRED_FREE_BYTES:
        raise AssertionError(
            f"{parent}: {usage.free / 1024**3:.1f} GiB free, the gate needs at least "
            f"{REQUIRED_FREE_BYTES / 1024**3:.0f} GiB for its worktrees and runs"
        )

    GATE.scratch = Path(tempfile.mkdtemp(prefix="mimic-gapped-parity-gate-", dir=parent))
    GATE.vertical_inventory = inventory
    log(f"  scratch: {GATE.scratch}")
    log(f"  HEAD: {git('rev-parse', 'HEAD')}")
    GATE.done.add("preconditions")


# --------------------------------------------------------------------------
# Stage 2: dataset provenance
# --------------------------------------------------------------------------


def stage_dataset_provenance():
    """The horizontal dataset is the real complete gapped conversion of the vertical files."""
    banner("Stage 2: horizontal dataset provenance")
    GATE.require("preconditions", "not inspecting the dataset")

    directory = package_path(HORIZONTAL_SIMULATION, "snapshots")
    names = sorted(
        entry.name for entry in directory.iterdir() if SNAPSHOT_FILE_RE.match(entry.name)
    )
    expected_names = [
        f"snapshot_{index:03d}.h5"
        for index in range(
            len(
                snapshot_list_entries(package_path(HORIZONTAL_SIMULATION, "mini-millennium.a_list"))
            )
        )
    ]
    if names != expected_names:
        raise AssertionError(
            f"{directory}: snapshot files {names[:3]}...{names[-3:]} ({len(names)}) are not "
            f"exactly the {len(expected_names)} the a_list names"
        )

    halos = 0
    gapped = 0
    max_span = 0
    populated = 0
    for name in names:
        path = directory / name
        snapshot = int(SNAPSHOT_FILE_RE.match(name).group(1))
        with h5py.File(path, "r") as handle:
            header = handle["header"].attrs
            checks = {
                "format_version": (int(header["format_version"]), EXPECTED_FORMAT_VERSION),
                "source_format": (decoded(header["source_format"]), EXPECTED_SOURCE_FORMAT),
                "column_mapping_sha256": (
                    decoded(header["column_mapping_sha256"]),
                    EXPECTED_COLUMN_MAPPING_SHA256,
                ),
                "links_adjacent": (int(header["links_adjacent"]), 0),
                "snapshot_number": (int(header["snapshot_number"]), snapshot),
            }
            for attribute, (found, expected) in checks.items():
                if found != expected:
                    raise AssertionError(
                        f"{path}: header {attribute} is {found!r}, expected {expected!r}; this is "
                        f"not the version 3 conversion the gate certifies"
                    )
            n_halos = int(header["n_halos"])
            descendant_snapshot = numpy.asarray(handle["halos/DescendantSnapshot"][()])
        if descendant_snapshot.shape != (n_halos,):
            raise AssertionError(
                f"{path}: DescendantSnapshot has shape {descendant_snapshot.shape}, "
                f"header n_halos is {n_halos}"
            )
        halos += n_halos
        populated += int(n_halos > 0)
        linked = descendant_snapshot[descendant_snapshot >= 0].astype(numpy.int64)
        spans = linked - snapshot
        gapped += int((spans > 1).sum())
        if spans.size:
            max_span = max(max_span, int(spans.max()))

    log(
        f"  {len(names)} version 3 files ({populated} populated) from {EXPECTED_SOURCE_FORMAT}, "
        f"column_mapping_sha256 {EXPECTED_COLUMN_MAPPING_SHA256}, links_adjacent 0"
    )
    log(f"  {halos} halos, {gapped} gapped descendant links, longest descendant span {max_span}")
    measured = (halos, gapped, max_span)
    expected = (EXPECTED_HALOS, EXPECTED_GAPPED_DESCENDANTS, EXPECTED_MAX_DESCENDANT_SPAN)
    if measured != expected:
        raise AssertionError(
            f"{directory}: (halos, gapped descendant links, longest span) is {measured}, "
            f"expected {expected} for the real complete eight-file conversion"
        )

    # The conversion inventory must name exactly the vertical package's files, or
    # the two sides are not the same trees.
    inventory = GATE.vertical_inventory
    with h5py.File(directory / FORESTS_FILE, "r") as handle:
        ordinals = numpy.asarray(handle[SOURCE_FILE_ORDINAL][()])
    converted = sorted(int(value) for value in numpy.unique(ordinals))
    read = list(range(inventory["first_file"], inventory["last_file"] + 1))
    if converted != read:
        raise AssertionError(
            f"{directory / FORESTS_FILE}: the conversion inventory names source files "
            f"{converted}, the vertical package reads {inventory['tree_name']}.{read}"
        )
    log(
        f"  conversion inventory: {ordinals.size} forests from source files {converted}, "
        f"exactly the vertical package's {inventory['files'][0]}..{inventory['files'][-1]}"
    )
    GATE.done.add("dataset")


# --------------------------------------------------------------------------
# Stage 3: run files
# --------------------------------------------------------------------------


def committed_run_file(model: str, simulation: str) -> Path:
    return REPO_ROOT / "models" / model / "input" / f"{model}_{simulation}.yaml"


def assert_matches_head(path: Path) -> None:
    """Fail when a working-tree file the gate relies on differs from its HEAD copy.

    Used for the run files and for the comparator. The worktrees are pinned to
    HEAD and execute their own run-file copies, so a dirty working-tree run file
    would never reach the runs -- but it would still be the file this stage
    inspected. The comparator is run and imported from the working tree, so a
    dirty copy would decide the verdict while the record says it is unchanged.
    """
    relative = path.relative_to(REPO_ROOT).as_posix()
    completed = subprocess.run(
        ["git", "show", f"HEAD:{relative}"], cwd=REPO_ROOT, capture_output=True
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"{path} is not committed at HEAD: git show HEAD:{relative} failed "
            f"({completed.stderr.decode(errors='replace').strip()})"
        )
    if path.read_bytes() != completed.stdout:
        raise AssertionError(
            f"{path} differs from its committed copy at HEAD. This gate certifies committed "
            f"run files and the committed comparator against HEAD-built executables; commit "
            f"or revert the file and re-run."
        )


def split_leading_header(lines: list[str]) -> tuple[list[str], list[str]]:
    """Split a run file into its leading comment/blank block and the rest."""
    index = 0
    while index < len(lines) and (
        not lines[index].strip() or lines[index].lstrip().startswith("#")
    ):
        index += 1
    return lines[:index], lines[index:]


def flatten_keys(node, prefix: str = "") -> dict[str, object]:
    """Flatten a parsed run file to `section.key` -> value, lists kept whole."""
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


def assert_horizontal_run_file_diff(vertical_path: Path, horizontal_path: Path) -> None:
    """The horizontal run file differs from the vertical one in exactly two keys.

    Checked functionally on the parsed YAML and textually on the lines below
    the leading header comment, which is the only part allowed to differ
    beyond the two keys.
    """
    left_text = vertical_path.read_text()
    right_text = horizontal_path.read_text()

    left_flat = flatten_keys(yaml.safe_load(left_text))
    right_flat = flatten_keys(yaml.safe_load(right_text))
    if set(left_flat) != set(right_flat):
        raise AssertionError(
            f"{horizontal_path} and {vertical_path} do not carry the same keys: "
            f"only vertical: {sorted(set(left_flat) - set(right_flat))}; "
            f"only horizontal: {sorted(set(right_flat) - set(left_flat))}"
        )
    changed_keys = sorted(key for key in left_flat if left_flat[key] != right_flat[key])
    if changed_keys != sorted(AUTHORIZED_KEY_CHANGES):
        detail = "\n".join(
            f"    {key}: {left_flat[key]!r} -> {right_flat[key]!r}" for key in changed_keys
        )
        raise AssertionError(
            f"{horizontal_path} differs from {vertical_path} in functional key(s) "
            f"{changed_keys}, expected exactly {sorted(AUTHORIZED_KEY_CHANGES)}:\n{detail}"
        )
    if right_flat["simulation.name"] != HORIZONTAL_SIMULATION:
        raise AssertionError(
            f"{horizontal_path}: simulation.name is {right_flat['simulation.name']!r}, "
            f"expected {HORIZONTAL_SIMULATION!r}"
        )
    if not str(right_flat["output.output_directory"]).endswith(f"-{HORIZONTAL_SIMULATION}"):
        raise AssertionError(
            f"{horizontal_path}: output.output_directory is "
            f"{right_flat['output.output_directory']!r}, which does not name a "
            f"{HORIZONTAL_SIMULATION} output directory, so the two runs would share one"
        )

    left_header, left_body = split_leading_header(left_text.splitlines())
    right_header, right_body = split_leading_header(right_text.splitlines())
    if len(left_body) != len(right_body):
        raise AssertionError(
            f"{horizontal_path.name} has {len(right_body)} line(s) below its header comment, "
            f"{vertical_path.name} has {len(left_body)}"
        )
    changed = [index for index in range(len(left_body)) if left_body[index] != right_body[index]]
    if len(changed) != 2:
        detail = "\n".join(
            f"    line {index + 1 + len(right_header)}: "
            f"{left_body[index]!r} -> {right_body[index]!r}"
            for index in changed
        )
        raise AssertionError(
            f"{horizontal_path} differs from {vertical_path} in {len(changed)} line(s) below "
            f"the leading header comment, expected exactly 2:\n{detail}"
        )
    name_line, output_line = (right_body[index].strip() for index in changed)
    if name_line != f"name: {HORIZONTAL_SIMULATION}":
        raise AssertionError(
            f"{horizontal_path}: first changed line is {name_line!r}, "
            f"expected 'name: {HORIZONTAL_SIMULATION}'"
        )
    if not output_line.startswith("output_directory:"):
        raise AssertionError(
            f"{horizontal_path}: second changed line is {output_line!r}, "
            f"expected an output_directory: assignment"
        )
    if left_header != right_header:
        log(
            f"  {horizontal_path.name}: leading header comment differs from "
            f"{vertical_path.name} (carries no functional weight; permitted)"
        )


def stage_run_file_diffs():
    """The comparator is HEAD's; each horizontal run file differs in exactly two keys."""
    banner("Stage 3: committed comparator and run-file diffs")
    GATE.require("dataset", "not checking run files")

    assert_matches_head(COMPARATOR_SCRIPT)
    log(f"  {COMPARATOR_SCRIPT.relative_to(REPO_ROOT)} matches its committed HEAD copy")

    for model in MODELS:
        vertical_path = committed_run_file(model, VERTICAL_SIMULATION)
        horizontal_path = committed_run_file(model, HORIZONTAL_SIMULATION)
        for path in (vertical_path, horizontal_path):
            if not path.is_file():
                raise AssertionError(f"run file is missing: {path}")
            assert_matches_head(path)
        log(f"  {model}: both run files match their committed HEAD copies")
        assert_horizontal_run_file_diff(vertical_path, horizontal_path)
        log(
            f"  {model}: {horizontal_path.name} differs from {vertical_path.name} in exactly "
            f"{list(AUTHORIZED_KEY_CHANGES)}"
        )

    GATE.done.add("run-files")


# --------------------------------------------------------------------------
# Stage 4: isolated per-pair builds
# --------------------------------------------------------------------------


def link_machine_local(worktree: Path) -> None:
    """Recreate the main tree's machine-local symlinks inside a worktree.

    The dataset directories and the Python virtual environment are gitignored, so
    a fresh worktree has neither. Both are recreated from the main tree's
    resolved targets.
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


def pair_key(model: str, simulation: str) -> str:
    return f"{model}__{simulation}"


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
    if not (worktree / "mimic").is_file():
        raise AssertionError(f"{key}: build produced no executable at {worktree / 'mimic'}")
    return worktree


def stage_build_worktrees():
    """One isolated worktree build per model x ordering pair, at the current HEAD."""
    banner("Stage 4: isolated per-pair builds at HEAD")
    GATE.require("run-files", "not building")

    head = git("rev-parse", "HEAD")
    for model in MODELS:
        for simulation in (VERTICAL_SIMULATION, HORIZONTAL_SIMULATION):
            build_worktree(pair_key(model, simulation), head, model, simulation)

    GATE.done.add("builds")


# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------


def read_run_file_output(run_file: Path) -> tuple[str, str]:
    """Return (output_directory, output_filename) as the run file declares them."""
    config = yaml.safe_load(run_file.read_text()) or {}
    output = config.get("output") or {}
    directory = output.get("output_directory")
    filename = output.get("output_filename")
    if directory is None:
        raise AssertionError(f"{run_file}: output.output_directory not found")
    if filename is None:
        raise AssertionError(f"{run_file}: output.output_filename not found")
    return str(directory), str(filename)


def dynamic_variant(run_file: Path, scratch: Path) -> Path:
    """Write the committed run file with `TimestepScheme: dynamic` added after SubSteps.

    Verified to be the committed file plus exactly that one line, so the two
    runs of a scheme pair are otherwise byte-identical. The committed file must
    carry no TimestepScheme key of its own: the parameter reader takes the first
    matching key, so an existing one would silently decide the dynamic leg.
    """
    committed = yaml.safe_load(run_file.read_text()) or {}
    if TIMESTEP_SCHEME_KEY in flatten_keys(committed) or any(
        line.strip().startswith(f"{TIMESTEP_SCHEME_KEY}:")
        for line in run_file.read_text().splitlines()
    ):
        raise AssertionError(
            f"{run_file} already declares {TIMESTEP_SCHEME_KEY}; the reader takes the first "
            f"matching key, so the dynamic leg could silently run another scheme"
        )
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

    base = run_file.read_text().splitlines()
    written = variant.read_text().splitlines()
    inserted = [
        index
        for index in range(len(written))
        if written[:index] + written[index + 1 :] == base
        and written[index].strip() == "TimestepScheme: dynamic"
    ]
    if len(written) != len(base) + 1 or not inserted:
        raise AssertionError(
            f"{variant} is not {run_file} with exactly one 'TimestepScheme: dynamic' line added"
        )
    parsed = flatten_keys(yaml.safe_load(variant.read_text()) or {})
    if parsed.get(TIMESTEP_SCHEME_KEY) != "dynamic":
        raise AssertionError(
            f"{variant}: {TIMESTEP_SCHEME_KEY} parses as {parsed.get(TIMESTEP_SCHEME_KEY)!r}, "
            f"expected 'dynamic'"
        )
    return variant


def execute_run(model: str, simulation: str, scheme: str) -> RunOutput:
    """Run one model x ordering x scheme into its own scratch output directory."""
    key = f"{model}__{simulation}__{scheme}"
    worktree = GATE.worktrees[pair_key(model, simulation)]

    # The worktree's OWN copy of the run file, never the working tree's: the
    # executable is pinned to HEAD, so its input must be too.
    worktree_run_file = worktree / committed_run_file(model, simulation).relative_to(REPO_ROOT)
    if not worktree_run_file.is_file():
        raise AssertionError(f"{key}: the HEAD worktree has no run file at {worktree_run_file}")

    if scheme == "fixed":
        run_file = worktree_run_file
    else:
        run_file = dynamic_variant(worktree_run_file, GATE.scratch_dir("run-files", key))

    declared_directory, basename = read_run_file_output(run_file)

    # The run file's output_directory is relative to the working directory, so a
    # per-run scratch root is selected by pointing the worktree's `output` at it,
    # keeping the committed run file byte-for-byte the one being tested.
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
        strings = {
            name: decoded(value)
            for name, value in group.attrs.items()
            if numpy.asarray(value).dtype.kind in "SUO"
        }
        integers = {
            name: int(numpy.asarray(value).ravel()[0])
            for name, value in group.attrs.items()
            if numpy.asarray(value).dtype.kind in "iu"
        }
        redshifts = numpy.array(group["Redshifts"][()])
        field_metadata = numpy.array(group["FieldMetadata"][()])
    return attrs, strings, integers, redshifts, field_metadata


def assert_vertical_read_inventory(vertical_run: RunOutput) -> None:
    """The vertical run read exactly the tree files the conversion inventory names.

    Stage 2 established that the inventory names the vertical package's declared
    files; this checks the run itself recorded reading that file range, from
    that tree name, out of a directory resolving to the same data.
    """
    inventory = GATE.vertical_inventory
    _, strings, integers, _, _ = run_properties(vertical_run.master)
    recorded = (
        strings.get("TreeName"),
        integers.get("FirstFile"),
        integers.get("LastFile"),
    )
    expected = (inventory["tree_name"], inventory["first_file"], inventory["last_file"])
    if recorded != expected:
        raise AssertionError(
            f"{vertical_run.master}: the vertical run recorded (TreeName, FirstFile, LastFile) "
            f"{recorded}, the conversion inventory names {expected}"
        )
    directory = vertical_run.worktree / strings.get("SimulationDir", "")
    main_directory = package_path(VERTICAL_SIMULATION, "snapshots")
    if directory.resolve() != main_directory.resolve():
        raise AssertionError(
            f"{vertical_run.master}: the vertical run read {directory} "
            f"(-> {directory.resolve()}), not the dataset the inventory was checked "
            f"against ({main_directory.resolve()})"
        )
    log(
        f"  vertical run read {inventory['files'][0]}..{inventory['files'][-1]} from "
        f"{directory.resolve()}, the files the conversion inventory names"
    )


def assert_alist_byte_equal(left: Path, right: Path) -> None:
    """The two packages' snapshot lists must be the same bytes."""
    for path in (left, right):
        if not path.is_file():
            raise AssertionError(f"snapshot list is missing: {path}")
    if left.read_bytes() != right.read_bytes():
        raise AssertionError(f"snapshot lists differ:\n  {left}\n  {right}")


def alist_used_by(run: RunOutput) -> Path:
    """The snapshot list the run recorded as its input, inside its own worktree."""
    _, strings, _, _, _ = run_properties(run.master)
    if "FileWithSnapList" not in strings:
        raise AssertionError(f"{run.master}: no FileWithSnapList attribute")
    return run.worktree / strings["FileWithSnapList"]


def record_signature(run: RunOutput):
    """The Galaxies record schema of a run, from its first partition."""
    partitions = run.partitions()
    if not partitions:
        raise AssertionError(f"{run.key}: no partition files next to {run.master}")
    with h5py.File(partitions[0], "r") as handle:
        for name in handle:
            if SNAP_GROUP_RE.match(name) and "Galaxies" in handle[name]:
                return comparator.schema_signature(handle[f"{name}/Galaxies"].dtype)
    raise AssertionError(f"{partitions[0]}: no Snap###/Galaxies dataset")


def structural_preflight(vertical_run: RunOutput, horizontal_run: RunOutput) -> int:
    """Exact equality of everything the two runs must agree on before comparison.

    Returns the field count, derived independently of the comparator.
    """
    assert_alist_byte_equal(alist_used_by(vertical_run), alist_used_by(horizontal_run))
    log("  preflight: snapshot lists byte-equal")

    left_attrs, _, _, left_z, left_fields = run_properties(vertical_run.master)
    right_attrs, _, _, right_z, right_fields = run_properties(horizontal_run.master)

    if (left_z.dtype, left_z.shape) != (right_z.dtype, right_z.shape) or (
        left_z.tobytes() != right_z.tobytes()
    ):
        raise AssertionError(
            f"recorded Redshifts differ: {left_z.dtype}{left_z.shape} vs "
            f"{right_z.dtype}{right_z.shape}, or their bytes"
        )
    log(f"  preflight: {left_z.size} recorded redshifts exactly equal")

    for name in PREFLIGHT_ATTRS:
        for attrs, run in ((left_attrs, vertical_run), (right_attrs, horizontal_run)):
            if name not in attrs:
                raise AssertionError(f"{run.master}: RunProperties has no {name} attribute")
        if left_attrs[name] != right_attrs[name]:
            raise AssertionError(
                f"RunProperties/{name} differs between the runs: "
                f"{left_attrs[name]} vs {right_attrs[name]}"
            )
    log(f"  preflight: {', '.join(PREFLIGHT_ATTRS)} exactly equal")

    # Field names, their order and their units must match exactly. Descriptions
    # are package-owned prose and are reported, not compared: nothing below is
    # derived from one.
    if left_fields.dtype != right_fields.dtype or left_fields.shape != right_fields.shape:
        raise AssertionError(
            f"FieldMetadata tables differ in shape or columns: "
            f"{left_fields.dtype}{left_fields.shape} vs {right_fields.dtype}{right_fields.shape}"
        )
    left_names = [row["field_name"] for row in left_fields]
    right_names = [row["field_name"] for row in right_fields]
    if left_names != right_names:
        raise AssertionError(
            f"FieldMetadata field names or their order differ:\n"
            f"  vertical:   {left_names}\n  horizontal: {right_names}"
        )
    unit_mismatches = [
        f"{row['field_name'].decode()}: {row['units']!r} vs {right_fields[index]['units']!r}"
        for index, row in enumerate(left_fields)
        if row["units"] != right_fields[index]["units"]
    ]
    if unit_mismatches:
        raise AssertionError(f"FieldMetadata units differ: {unit_mismatches}")
    for index, row in enumerate(left_fields):
        if row["description"] != right_fields[index]["description"]:
            log(
                f"  preflight: note -- {row['field_name'].decode()} carries a package-specific "
                f"description on each side (prose only, not compared)"
            )

    left_signature = record_signature(vertical_run)
    right_signature = record_signature(horizontal_run)
    if left_signature != right_signature:
        raise AssertionError(
            f"Galaxies record schemas differ:\n  vertical:   {left_signature}\n"
            f"  horizontal: {right_signature}"
        )
    fields = len(left_fields)
    if fields != len(left_signature):
        raise AssertionError(
            f"FieldMetadata lists {fields} fields but the record carries {len(left_signature)}"
        )
    schema_fields = json.loads(vertical_run.schema_path().read_text())["fields"]
    if len(schema_fields) != fields:
        raise AssertionError(
            f"{vertical_run.schema_path()} lists {len(schema_fields)} fields, "
            f"FieldMetadata lists {fields}"
        )
    log(f"  preflight: FieldMetadata and record schema identical ({fields} fields, in order)")
    return fields


def requested_snapshots(run: RunOutput) -> set[int]:
    """The output snapshots the run file under test asks for."""
    config = yaml.safe_load(run.run_file.read_text())
    selected = (config.get("output") or {}).get("snapshot_list")
    if not selected:
        raise AssertionError(f"{run.run_file}: no output.snapshot_list to compare against")
    return {int(entry) for entry in selected}


def recorded_records(run: RunOutput) -> dict[int, int]:
    """Galaxies per output snapshot, from the master's TotHalosPerSnap attributes.

    Read independently of the comparator, so the count it reports can be checked
    against a number it had no part in producing.
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


def assert_snapshot_coverage(run: RunOutput, counts: dict[int, int], expected: set[int]) -> None:
    """One run wrote exactly the requested output snapshots, none of them empty."""
    written = set(counts)
    if written != expected:
        raise AssertionError(
            f"{run.key}: wrote output snapshots {sorted(written)}, its run file requests "
            f"{sorted(expected)} (missing {sorted(expected - written)}, "
            f"unexpected {sorted(written - expected)})"
        )
    empty = sorted(snap for snap, count in counts.items() if count <= 0)
    if empty:
        raise AssertionError(
            f"{run.key}: output snapshot(s) {empty} hold no galaxies; an empty snapshot "
            f"compares equal to an empty snapshot and is not evidence of anything"
        )


# --------------------------------------------------------------------------
# Comparison
# --------------------------------------------------------------------------


def run_comparator(leg: str, vertical_run: RunOutput, horizontal_run: RunOutput):
    """Run the committed comparator, logging its whole report. Returns (exit, stdout)."""
    # Re-checked at every use, not only in stage 3: the verdict is the script's.
    assert_matches_head(COMPARATOR_SCRIPT)
    command = [
        sys.executable,
        str(COMPARATOR_SCRIPT),
        vertical_run.spec,
        horizontal_run.spec,
        "--left-label",
        "vertical",
        "--right-label",
        "horizontal",
    ]
    log(f"  comparing {leg}: {' '.join(command[1:])}")
    completed = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True)
    for line in completed.stdout.splitlines():
        log(f"     | {line}")
    for line in completed.stderr.splitlines():
        log(f"     ! {line}")
    return completed.returncode, completed.stdout


def parity_leg(model: str, scheme: str) -> None:
    """One parity leg: both runs, preflight, the comparator, then coverage checks.

    The comparator runs before the count checks so that any divergence -- an
    id-set difference as much as a field difference -- is reported by snapshot,
    field and example id, rather than stopped at a count mismatch that says only
    that the runs disagree.
    """
    leg = f"{model}/{scheme}"
    banner(f"Parity leg: {leg}")
    GATE.require("builds", f"leg {leg} did not run")

    vertical_run = execute_run(model, VERTICAL_SIMULATION, scheme)
    assert_vertical_read_inventory(vertical_run)
    horizontal_run = execute_run(model, HORIZONTAL_SIMULATION, scheme)
    fields = structural_preflight(vertical_run, horizontal_run)

    status, stdout = run_comparator(leg, vertical_run, horizontal_run)
    if status != 0:
        raise AssertionError(
            f"{leg}: cross-format identity comparison failed (exit {status}); every divergence "
            f"is listed by snapshot, field and example id in the comparator output above"
        )
    match = None
    for line in stdout.splitlines():
        match = COMPARATOR_PASS_RE.match(line.strip()) or match
    if match is None:
        raise AssertionError(f"{leg}: the comparator exited 0 without a PASSED summary line")
    compared_records, compared_snapshots, compared_fields = (int(value) for value in match.groups())

    vertical_counts = recorded_records(vertical_run)
    horizontal_counts = recorded_records(horizontal_run)
    if vertical_counts != horizontal_counts:
        raise AssertionError(
            f"{leg}: the two runs record different galaxy counts per output snapshot:\n"
            f"  vertical:   {vertical_counts}\n  horizontal: {horizontal_counts}"
        )
    expected = requested_snapshots(vertical_run)
    if requested_snapshots(horizontal_run) != expected:
        raise AssertionError(f"{leg}: the two run files request different output snapshots")
    assert_snapshot_coverage(vertical_run, vertical_counts, expected)
    assert_snapshot_coverage(horizontal_run, horizontal_counts, expected)

    records = sum(vertical_counts.values())
    if (compared_records, compared_snapshots, compared_fields) != (records, len(expected), fields):
        raise AssertionError(
            f"{leg}: the comparator compared {compared_records} galaxies over "
            f"{compared_snapshots} snapshots in {compared_fields} fields; the runs record "
            f"{records} galaxies over {len(expected)} requested snapshots in {fields} fields"
        )

    # One horizontal partition per requested snapshot is the horizontal writer's
    # contract; asserted so the count is evidence rather than an observation.
    horizontal_partitions = len(horizontal_run.partitions())
    if horizontal_partitions != len(expected):
        raise AssertionError(
            f"{leg}: the horizontal run wrote {horizontal_partitions} partition file(s) for "
            f"{len(expected)} requested output snapshot(s)"
        )
    log(
        f"  PASS {leg}: {compared_records} galaxies, {compared_fields} fields, "
        f"{compared_snapshots} output snapshots {sorted(expected)} compared bitwise "
        f"(vertical {len(vertical_run.partitions())} partition(s) vs horizontal "
        f"{horizontal_partitions} partition(s))"
    )


def run_leg(model: str, scheme: str) -> None:
    """Run one leg and record its verdict, whatever the outcome."""
    try:
        parity_leg(model, scheme)
    except BaseException as error:
        first = str(error).splitlines()[0] if str(error) else type(error).__name__
        GATE.verdicts[(model, scheme)] = f"FAIL: {first}"
        raise
    GATE.verdicts[(model, scheme)] = "PASS"


def stage_halos_only_fixed():
    """halos-only, fixed timesteps: the driver alone, compared bitwise."""
    run_leg("halos-only", "fixed")


def stage_halos_only_dynamic():
    """halos-only, dynamic timesteps."""
    run_leg("halos-only", "dynamic")


def stage_sage16_fixed():
    """sage16, fixed timesteps: the full physics pipeline, compared bitwise."""
    run_leg("sage16", "fixed")


def stage_sage16_dynamic():
    """sage16, dynamic timesteps."""
    run_leg("sage16", "dynamic")


def stage_leg_verdicts():
    """Name every leg's verdict; the gate passes only if all four legs passed."""
    banner("Leg verdicts")
    for model, scheme in LEGS:
        verdict = GATE.verdicts.get((model, scheme), "FAIL: leg did not run")
        log(f"  LEG {model}/{scheme}: {verdict}")
    failed = [
        f"{model}/{scheme}"
        for model, scheme in LEGS
        if GATE.verdicts.get((model, scheme)) != "PASS"
    ]
    if failed:
        raise AssertionError(
            f"{len(failed)} of {len(LEGS)} parity leg(s) did not pass: {', '.join(failed)}"
        )
    log(f"  all {len(LEGS)} parity legs PASS")


# --------------------------------------------------------------------------


STAGES = [
    stage_preconditions,
    stage_dataset_provenance,
    stage_run_file_diffs,
    stage_build_worktrees,
    stage_halos_only_fixed,
    stage_halos_only_dynamic,
    stage_sage16_fixed,
    stage_sage16_dynamic,
    stage_leg_verdicts,
]


def main() -> int:
    atexit.register(cleanup)
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, _cleanup_on_signal)
    try:
        # No abort_on_failure: an aborted stage is reported as SKIP, and a leg
        # that does not run must FAIL. A setup failure instead makes every later
        # stage fail its own GATE.require() at once, and each leg runs whatever
        # the outcome of the others, so every divergence is reported.
        return run_test_suite(
            STAGES,
            "Gapped mini-Millennium parity gate (test_cross_format_identity.py)",
        )
    finally:
        cleanup()


if __name__ == "__main__":
    sys.exit(main())
