"""Capture: run a design's evaluations against Mimic, verify each, and record counts.

``capture`` is the only stage that runs Mimic, and a capture is made once: it refuses an output
directory that exists and writes nothing outside it. Before any run, :func:`prepare` reads every
input once (the spec, the constraint table, the run-file template, the simulation's
``simulation_info.yaml``, Mimic's two plotting helpers and the executable), hashes those bytes and
parses from them, checks that the template can carry the design, and checks the tree files by
``input.tree_type``, compared case-insensitively as Mimic does (:func:`tree_files`): for
``lhalo_binary``, a per-file reader that skips a missing file with a warning, every tree file of
the run's range must exist and each one's size is recorded; for ``consistent_trees_ascii``,
``consistent_trees_hdf5`` and ``horizontal_hdf5``, which open one dataset and stop with a fatal
error when it is missing, only the tree directory is recorded; any other ``tree_type`` (``lhalo_hdf5``
is a per-file reader the bench does not check) is refused, because a partial box could pass.
After the last run the hashed files are hashed again (``./mimic`` through its symlink, if it is
one) and the tree directory and files looked at again; any difference makes the capture exit 2
(:func:`changed_inputs`). The design is the spec's
(``--sets`` names its sets) or a CSV made elsewhere (``--design``, checked by
:func:`mimic_bench.design.parse_design`; ``--sets`` then filters by label, all rows when omitted).
For each row it

1. writes a run file (:mod:`mimic_bench.runfile`) into ``<out>/runs/<id>/``;
2. runs ``./mimic -q <run file>`` with the Mimic root as working directory, in its own process
   group, stdout and stderr in ``<out>/runs/<id>/mimic.log`` and a timeout (``--timeout-s``,
   default :data:`RUN_TIMEOUT_S` seconds), the output going to ``<out>/runs/<id>/output/``;
3. verifies the run from what it recorded (:func:`verify_run`), because Mimic accepts a run file
   with a misspelled key; and
4. counts galaxies in every candidate bin (:mod:`mimic_bench.statistics`).

Workers are threads, each waiting on one subprocess: process pools need a system call that an OS
sandbox may deny, and the work is in the subprocess anyway. SIGINT (Ctrl-C) and SIGTERM stop the
capture: no queued run starts, the running ``./mimic`` process groups are terminated, and every
file below is still written, the runs marked ``not run: interrupted`` or ``interrupted``, with
``"interrupted": true`` in ``context.json`` and exit status 2.

A capture whose exit status is not 0 is not a measurement: :func:`capture_outcome_error` is the
check ``select-outputs`` and ``analyse`` share.

The output directory then holds ``context.json`` (provenance, including the request hash of
:mod:`mimic_bench.provenance`), ``design.csv``, ``runs.csv`` (per run: set, exit status, wall
time, verified, rows at the snapshot, and the failure if any), ``outputs.csv`` (per run: the
count in every candidate bin, columns named by :func:`~mimic_bench.statistics.output_label`),
``bins.csv`` (per column: the bin's converted centre and edges, :data:`BINS_COLUMNS`), the run
directories, and the two manifests of :mod:`mimic_bench.manifest`.

:func:`select_from_capture` reads a finished pilot capture and applies the spec's output
selection rule; it writes nothing.

Usage:
    status = run_capture(spec_path, mimic_root, ["pilot"], out, workers=8)
"""

from __future__ import annotations

import csv
import datetime
import hashlib
import json
import math
import os
import platform
import signal
import socket
import stat
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Mapping, Sequence

import h5py
import numpy as np
import yaml
from mimic_bench.constraints import convert_constraint
from mimic_bench.design import (
    DesignRow,
    build_design,
    design_columns,
    parse_design,
    write_design,
)
from mimic_bench.manifest import (
    check_manifest,
    read_csv_rows,
    read_json_object,
    write_manifests,
)
from mimic_bench.provenance import (
    SpecInputs,
    library_versions,
    package_identity,
    read_spec_inputs,
    request_mismatch,
    sha256_bytes,
)
from mimic_bench.runfile import (
    FIRST_FILE_PATH,
    LAST_FILE_PATH,
    OUTPUT_FILENAME_PATH,
    PARAMETERS_PATH,
    SUBSTEPS_PATH,
    RunFileError,
    apply_overrides,
    build_run_file,
    check_template,
    dump_run_file,
    get_path,
    parse_template,
)
from mimic_bench.spec import SET_NAMES, Spec
from mimic_bench.statistics import (
    BinSelection,
    MimicHelpers,
    StatisticError,
    UnsupportedUnitError,
    bin_counts,
    candidate_bins,
    galaxy_log_masses,
    load_mimic_helpers,
    output_label,
    select_outputs,
)

RUN_TIMEOUT_S = 120.0
"""Default seconds one evaluation may take before it is killed and counted as failed."""

EXECUTABLE = "mimic"
"""The Mimic executable, relative to the Mimic root."""

PER_FILE_TREE_TYPE = "lhalo_binary"
"""The ``input.tree_type`` whose reader reads one ``<tree_name>.<n>`` file per number and skips a
missing file with a warning, and whose files the bench checks. Mimic's file extension for it is
empty."""

DATASET_TREE_TYPES = frozenset(
    {"consistent_trees_ascii", "consistent_trees_hdf5", "horizontal_hdf5"}
)
"""The ``input.tree_type`` values whose reader opens one dataset and stops with a fatal error when
it is missing, so the bench records only the tree directory for them. Any ``tree_type`` in neither
this set nor :data:`PER_FILE_TREE_TYPE` is refused: ``lhalo_hdf5``, for one, is a per-file reader
that skips a missing file with a warning, and the bench does not check its files."""

PILOT_REL_TOL = 1e-12
"""Relative tolerance on a pilot design value in a capture against the spec's, so a last-bit
difference in ``log10`` or ``pow`` between machines does not refuse a valid pilot."""

COMPLETION_MARKER = Path("metadata") / "version_info.json"
"""Written last by a successful run, relative to its output directory."""

RUNS_COLUMNS = ("id", "set", "exit_status", "wall_time_s", "verified", "rows", "failure")
BINS_COLUMNS = (
    "label",
    "property",
    "snapshot",
    "bin_width_dex",
    "centre_log10_msun",
    "low_log10_msun",
    "high_log10_msun",
)
"""``bins.csv``: per ``outputs.csv`` column, the statistic and the bin a galaxy counts in when
``low <= log10(mass / Msun) < high``; the centre and edges are empty when the runs reported no
single ``Hubble_h``."""

CAPTURE_FILES = ("context.json", "design.csv", "runs.csv", "outputs.csv")
"""The capture files a later stage reads, each of which the top-level manifest must list."""

INTERRUPTED = "interrupted"

EXIT_OK = 0
EXIT_ERROR = 2


class CaptureError(ValueError):
    """A capture that cannot start, or a capture directory that cannot be read."""


@dataclass(frozen=True)
class Prepared:
    """Everything a capture checks and reads before its first run.

    Attributes:
        inputs: The spec and constraint table, parsed from the bytes hashed.
        mimic_root: The Mimic checkout, absolute.
        template: The run-file template as read with ``BaseLoader``.
        first_file: The simulation's first tree file.
        last_file: The simulation's last tree file.
        helpers: Mimic's reader and unit conversion.
        files: Path and SHA-256 of each Mimic file read (executable, template,
            ``simulation_info.yaml`` and the helpers), by role.
        tree_directory: The directory the runs read their tree files from, as the run file
            names it under the Mimic root.
        trees: The tree directory and, for ``lhalo_binary`` trees, the tree files as
            :func:`tree_state` found them before the first run.
    """

    inputs: SpecInputs
    mimic_root: Path
    template: dict
    first_file: int
    last_file: int
    helpers: MimicHelpers
    files: Mapping[str, Mapping[str, str]]
    tree_directory: Path
    trees: Mapping[str, Any]

    @property
    def spec(self) -> Spec:
        """The campaign spec."""
        return self.inputs.spec


@dataclass(frozen=True)
class ExpectedRun:
    """What a verified run must have recorded.

    Attributes:
        parameter_names: The template's ``modules.parameters`` names, in order.
        parameter_values: The text each parameter carried in the run file, by name.
        model: The spec's model.
        simulation: The spec's simulation.
        first_file: The run file's first tree file.
        last_file: The run file's last tree file.
        substeps: The run file's ``SubSteps``.
        snapshot: The spec's snapshot.
        output_filename: The run file's ``output.output_filename``.
        run_file_md5: The MD5 of the run file's bytes, which Mimic records.
    """

    parameter_names: tuple[str, ...]
    parameter_values: Mapping[str, str]
    model: str
    simulation: str
    first_file: int
    last_file: int
    substeps: int
    snapshot: int
    output_filename: str
    run_file_md5: str


@dataclass(frozen=True)
class Verification:
    """The outcome of :func:`verify_run`.

    Attributes:
        verified: True only when every check held.
        failures: One message per check that failed.
        version: The Mimic version the run reported, when it reported one.
        commit: The Mimic commit the run reported, when it reported one.
        box_size: The run's ``BoxSize``, when readable.
        hubble_h: The run's ``Hubble_h``, when readable.
    """

    verified: bool
    failures: tuple[str, ...]
    version: str | None = None
    commit: str | None = None
    box_size: float | None = None
    hubble_h: float | None = None


@dataclass
class RunRecord:
    """One evaluation's outcome, as ``runs.csv`` and ``outputs.csv`` record it."""

    identifier: str
    set_name: str
    exit_status: int | None = None
    wall_time_s: float | None = None
    verified: bool = False
    rows: int | None = None
    counts: np.ndarray | None = None
    failures: list[str] = field(default_factory=list)
    verification: Verification | None = None
    fatal: str | None = None


@dataclass(frozen=True)
class CaptureStatus:
    """What a capture's records say as a whole (:func:`capture_status`).

    ``version``, ``commit``, ``box_size`` and ``hubble_h`` are the one value every run that
    reported one reported, ``None`` when none did, or the sorted distinct values when they differ.
    """

    exit_status: int
    verified: int
    fatal: str | None
    version: Any
    commit: Any
    box_size: Any
    hubble_h: Any


def _utc_now() -> str:
    """Return the current UTC time in ISO 8601."""
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


def _integer_text(text: Any, path: str) -> int:
    """Parse an integer from a YAML scalar, raising :class:`CaptureError` naming ``path``."""
    try:
        return int(text)
    except (TypeError, ValueError) as exc:
        raise CaptureError(f"{path}: {text!r} is not an integer") from exc


def _read(path: Path, what: str) -> bytes:
    """Read a file's bytes, raising :class:`CaptureError` naming it."""
    try:
        return path.read_bytes()
    except OSError as exc:
        raise CaptureError(f"{path}: cannot read {what} ({exc.strerror})") from exc


def prepare(spec_path: Path, mimic_root: Path) -> Prepared:
    """Read, hash and check everything a capture needs, before any run and without writing.

    Args:
        spec_path: The campaign spec.
        mimic_root: The Mimic checkout.

    Returns:
        The checked inputs.

    Raises:
        SpecError: If the spec is invalid.
        ConstraintError: If the constraint table is invalid.
        RunFileError: If the template cannot carry the spec's design (see
            :func:`mimic_bench.runfile.check_template`), or its tree-file range is not the
            simulation's whole range.
        StatisticError: If Mimic's helper modules cannot be imported.
        CaptureError: If the checkout has no executable, the simulation's file range is
            unreadable, the run's ``tree_type`` is one whose tree files the bench does not check,
            or, for ``lhalo_binary`` trees, a tree file of the run's range is absent.
    """
    inputs = read_spec_inputs(spec_path)
    spec = inputs.spec
    mimic_root = Path(mimic_root).resolve()
    executable = mimic_root / EXECUTABLE
    if not (executable.is_file() and os.access(executable, os.X_OK)):
        raise CaptureError(f"{executable}: no Mimic executable (build Mimic first)")
    template_path = mimic_root / spec.run_file.template
    template_bytes = _read(template_path, "the run-file template")
    template = parse_template(template_bytes, template_path)
    check_template(template, spec)
    info_path = mimic_root / "simulations" / spec.simulation / "simulation_info.yaml"
    info_bytes = _read(info_path, "the simulation's metadata")
    try:
        info = yaml.load(info_bytes.decode("utf-8"), Loader=yaml.BaseLoader)
        first_file = _integer_text(info["input"]["first_file"], "input.first_file")
        last_file = _integer_text(info["input"]["last_file"], "input.last_file")
    except (UnicodeDecodeError, yaml.YAMLError, KeyError, TypeError, CaptureError) as exc:
        raise CaptureError(f"{info_path}: cannot read the simulation's file range ({exc})") from exc
    for path, whole in ((FIRST_FILE_PATH, first_file), (LAST_FILE_PATH, last_file)):
        try:
            found = get_path(template, path)
        except RunFileError:
            continue
        if _integer_text(found, ".".join(path)) != whole:
            raise RunFileError(
                f"{'.'.join(path)}: the template reads {found!r}, but the simulation's files are "
                f"{first_file} to {last_file} ({info_path}); a capture must read them all, since "
                "the statistic's volume is the whole box (BoxSize^3)"
            )
    run_input = apply_overrides(template, spec).get("input")
    tree_directory, tree_names = tree_files(
        mimic_root, {**info["input"], **(run_input if isinstance(run_input, dict) else {})}
    )
    trees = tree_state(tree_directory, tree_names)
    missing = [name for name, found in trees["files"].items() if found is None]
    if missing:
        raise CaptureError(
            f"{tree_directory / missing[0]}: {len(missing)} of the {len(tree_names)} tree files of "
            "the run's range are absent; a capture must read them all, since the statistic's "
            "volume is the whole box (BoxSize^3)"
        )
    helpers = load_mimic_helpers(mimic_root)
    files = {
        "executable": (executable, _read(executable, "the Mimic executable")),
        "template": (template_path, template_bytes),
        "simulation_info": (info_path, info_bytes),
    }
    recorded = {
        role: {"path": _relative(path, mimic_root), "sha256": sha256_bytes(data)}
        for role, (path, data) in files.items()
    }
    for name, path in helpers.paths.items():
        recorded[f"helper:{name}"] = {
            "path": _relative(path, mimic_root),
            "sha256": helpers.sha256[name],
        }
    return Prepared(
        inputs,
        mimic_root,
        template,
        first_file,
        last_file,
        helpers,
        recorded,
        tree_directory,
        trees,
    )


def tree_files(mimic_root: Path, input_section: Mapping[str, Any]) -> tuple[Path, list[str]]:
    """Return the directory and names of the tree files a run reads, as Mimic names them.

    ``tree_type`` is compared case-insensitively, as Mimic does. ``lhalo_binary`` is a per-file
    reader: it reads ``<simulation_dir>/<tree_name>.<n>`` for n from ``first_file`` to
    ``last_file``, skipping a missing file with a warning, so those names are returned for the
    caller to check. ``consistent_trees_ascii``, ``consistent_trees_hdf5`` and ``horizontal_hdf5``
    open one dataset and stop with a fatal error when it is missing, so their names are empty.
    Any other ``tree_type``, ``lhalo_hdf5`` included, is refused: the bench does not check that
    reader's tree files, so a capture of a partial box could pass. A relative ``simulation_dir``
    is relative to the Mimic root, the runs' working directory.

    Args:
        mimic_root: The Mimic checkout.
        input_section: The run's ``input`` keys: the simulation's ``simulation_info.yaml``
            values, with the run file's over them.

    Raises:
        CaptureError: If a key is missing, the ``tree_type`` is one whose tree files the bench
            does not check or, for ``lhalo_binary``, the file range is not two integers.
    """
    try:
        directory = Path(mimic_root) / str(input_section["simulation_dir"])
        tree_type = str(input_section["tree_type"])
        kind = tree_type.lower()
        if kind in DATASET_TREE_TYPES:
            return directory, []
        if kind != PER_FILE_TREE_TYPE:
            raise CaptureError(
                f"input.tree_type {tree_type!r}: the bench does not check that reader's tree "
                f"files; it checks {PER_FILE_TREE_TYPE} and accepts "
                f"{', '.join(sorted(DATASET_TREE_TYPES))}"
            )
        tree_name = str(input_section["tree_name"])
    except KeyError as exc:
        raise CaptureError(
            f"input.{exc.args[0]}: set by neither the simulation nor the run file"
        ) from exc
    first = _integer_text(input_section.get("first_file"), "input.first_file")
    last = _integer_text(input_section.get("last_file"), "input.last_file")
    return directory, [f"{tree_name}.{n}" for n in range(first, last + 1)]


def tree_state(directory: Path, names: Sequence[str]) -> dict[str, Any]:
    """Return what the tree files are now: the directory with every link resolved, and per file
    name its ``[size in bytes, modification time in ns]``, or ``None`` when it is not a file."""
    files: dict[str, list[int] | None] = {}
    for name in names:
        try:
            status = (directory / name).stat()
        except OSError:
            status = None
        regular = status is not None and stat.S_ISREG(status.st_mode)
        files[name] = [status.st_size, status.st_mtime_ns] if regular else None
    return {"directory": str(directory.resolve()), "files": files}


def changed_inputs(prepared: Prepared) -> list[str]:
    """Return the inputs that differ from what :func:`prepare` found: each hashed file whose
    SHA-256 differs or that cannot be read, and the tree directory or each tree file whose
    resolved directory, size or modification time differs. The executable is read at the path
    that is launched, ``<mimic_root>/mimic``, so a symlink retargeted since the start counts."""
    changed = []
    for role, recorded in prepared.files.items():
        path = prepared.mimic_root / (EXECUTABLE if role == "executable" else recorded["path"])
        try:
            same = sha256_bytes(path.read_bytes()) == recorded["sha256"]
        except OSError:
            same = False
        if not same:
            changed.append(str(path))
    now = tree_state(prepared.tree_directory, list(prepared.trees["files"]))
    if now["directory"] != prepared.trees["directory"]:
        changed.append(str(prepared.tree_directory))
    changed.extend(
        str(prepared.tree_directory / name)
        for name, found in prepared.trees["files"].items()
        if now["files"][name] != found
    )
    return changed


def _relative(path: Path, root: Path) -> str:
    """``path`` relative to ``root`` when it lies inside it, else absolute."""
    path = Path(path).resolve()
    return str(path.relative_to(root)) if path.is_relative_to(root) else str(path)


def expected_run(
    prepared: Prepared, run_document: Mapping[str, Any], run_file_md5: str
) -> ExpectedRun:
    """Return what a run of ``run_document`` must record to count as verified.

    Parameter names come from the template, in its order; their values, ``SubSteps`` and the
    tree-file range (the simulation's when the run file has no ``input`` range) from the run file.
    """
    template_parameters = get_path(prepared.template, PARAMETERS_PATH)
    run_parameters = get_path(run_document, PARAMETERS_PATH)
    first_file, last_file = prepared.first_file, prepared.last_file
    if isinstance(run_document.get("input"), dict):
        first_file = _integer_text(
            run_document["input"].get("first_file", first_file), "input.first_file"
        )
        last_file = _integer_text(
            run_document["input"].get("last_file", last_file), "input.last_file"
        )
    return ExpectedRun(
        parameter_names=tuple(template_parameters),
        parameter_values={name: run_parameters[name] for name in template_parameters},
        model=prepared.spec.model,
        simulation=prepared.spec.simulation,
        first_file=first_file,
        last_file=last_file,
        substeps=_integer_text(get_path(run_document, SUBSTEPS_PATH), "SubSteps"),
        snapshot=prepared.spec.statistic.snapshot,
        output_filename=get_path(run_document, OUTPUT_FILENAME_PATH),
        run_file_md5=run_file_md5,
    )


def _attribute(attributes: h5py.AttributeManager, name: str) -> Any:
    """Return a ``RunProperties`` attribute as a Python scalar (bytes decoded), or KeyError."""
    value = attributes[name]
    if isinstance(value, np.ndarray):
        if value.size != 1:
            raise KeyError(f"{name} has {value.size} values")
        value = value.reshape(-1)[0]
    if isinstance(value, bytes):
        return value.decode()
    if isinstance(value, np.generic):
        return value.item()
    return value


def _values_match(recorded: str, carried: str) -> bool:
    """True when a recorded parameter value parses to the float the run file carried.

    A carried value that is not a number must be recorded as the same text.
    """
    try:
        carried_float = float(carried)
    except ValueError:
        return recorded == carried
    try:
        return float(recorded) == carried_float
    except ValueError:
        return False


def verify_run(
    output_directory: Path, exit_status: int | None, expected: ExpectedRun
) -> Verification:
    """Verify one finished run from what it recorded.

    A run is verified only when all hold: exit status 0; ``metadata/version_info.json`` exists and
    its ``parameters.parameter_md5_checksum`` is the MD5 of the run file written (so Mimic read
    those bytes); the master file's ``RunProperties/Parameters`` has exactly the expected
    parameter names in order, each value parsing to the float the run file carried;
    ``ModelName`` and ``SimulationName`` equal the spec's; ``FirstFile``, ``LastFile`` and
    ``SubSteps`` equal the run file's; and the spec's snapshot is present.

    Args:
        output_directory: The run's output directory.
        exit_status: The run's exit status, or None when it did not finish.
        expected: What the run must have recorded.

    Returns:
        The verification, with every failed check listed and, where readable, the version,
        commit, ``BoxSize`` and ``Hubble_h`` the run reported.
    """
    output_directory = Path(output_directory)
    failures: list[str] = []
    version = commit = None
    box_size = hubble_h = None
    if exit_status != 0:
        failures.append(f"exit status {exit_status}")
    marker = output_directory / COMPLETION_MARKER
    if not marker.is_file():
        failures.append(f"{COMPLETION_MARKER} is missing")
    else:
        try:
            info = json.loads(marker.read_text())
            version, commit = info.get("version"), info.get("git_commit")
            checksum = (info.get("parameters") or {}).get("parameter_md5_checksum")
            if checksum != expected.run_file_md5:
                failures.append(
                    f"{COMPLETION_MARKER} parameter_md5_checksum is {checksum!r}, the run file's "
                    f"MD5 is {expected.run_file_md5!r}"
                )
        except (OSError, ValueError, AttributeError) as exc:
            failures.append(f"{COMPLETION_MARKER} is unreadable ({exc})")
    master = output_directory / f"{expected.output_filename}.hdf5"
    try:
        with h5py.File(master, "r") as handle:
            properties = handle["RunProperties"]
            attributes = properties.attrs
            box_size = float(_attribute(attributes, "BoxSize"))
            hubble_h = float(_attribute(attributes, "Hubble_h"))
            table = properties["Parameters"][()]
            names = tuple(entry["param_name"].decode() for entry in table)
            values = {entry["param_name"].decode(): entry["value"].decode() for entry in table}
            if names != expected.parameter_names:
                failures.append(
                    f"RunProperties/Parameters names {list(names)} differ from the template's "
                    f"{list(expected.parameter_names)}"
                )
            else:
                for name in names:
                    if not _values_match(values[name], expected.parameter_values[name]):
                        failures.append(
                            f"parameter {name} recorded {values[name]!r}, the run file carried "
                            f"{expected.parameter_values[name]!r}"
                        )
            for name, wanted in (
                ("ModelName", expected.model),
                ("SimulationName", expected.simulation),
                ("FirstFile", expected.first_file),
                ("LastFile", expected.last_file),
                ("SubSteps", expected.substeps),
            ):
                found = _attribute(attributes, name)
                if found != wanted:
                    failures.append(f"RunProperties {name} is {found!r}, expected {wanted!r}")
            if f"Snap{expected.snapshot:03d}" not in handle:
                failures.append(f"snapshot {expected.snapshot} is not in {master.name}")
    except (OSError, KeyError, ValueError, TypeError) as exc:
        failures.append(f"{master.name}: cannot read RunProperties ({exc})")
    return Verification(not failures, tuple(failures), version, commit, box_size, hubble_h)


class CaptureRunner:
    """Starts the capture's ``./mimic`` processes and stops them all on request.

    Each process gets its own process group, so :meth:`halt` with ``kill`` terminates whatever it
    started too. Once halted, :meth:`start` starts nothing; the lock makes "check, then start"
    atomic against a halt. The lock is re-entrant because :meth:`halt` runs in a signal handler,
    which may interrupt the main thread inside :meth:`halt` itself.
    """

    def __init__(self) -> None:
        """A runner that has started nothing and is not halted."""
        self._lock = threading.RLock()
        self._running: set[subprocess.Popen] = set()
        self.reason: str | None = None
        self.interrupted = False

    def start(self, arguments: Sequence[str], **options: Any) -> subprocess.Popen | None:
        """Start a process in a new session, or return ``None`` when the runner is halted."""
        with self._lock:
            if self.reason is not None:
                return None
            process = subprocess.Popen(arguments, start_new_session=True, **options)
            self._running.add(process)
            return process

    def finished(self, process: subprocess.Popen) -> None:
        """Forget a process that has been waited for."""
        with self._lock:
            self._running.discard(process)

    def halt(self, reason: str, *, kill: bool) -> None:
        """Start no more runs; with ``kill``, also terminate every running process group.

        Args:
            reason: Why, recorded on every run not started (the first reason given stands).
            kill: True for an interrupt, which also marks the capture interrupted.
        """
        with self._lock:
            self.reason = self.reason or reason
            if kill:
                self.interrupted = True
                for process in list(self._running):
                    _signal_group(process, signal.SIGTERM)


def _signal_group(process: subprocess.Popen, signum: int) -> None:
    """Send a signal to a process's group, ignoring a group that has already gone."""
    try:
        os.killpg(process.pid, signum)
    except OSError:
        pass


def run_evaluation(
    prepared: Prepared,
    row: DesignRow,
    runs_directory: Path,
    timeout_s: float = RUN_TIMEOUT_S,
    runner: CaptureRunner | None = None,
) -> RunRecord:
    """Write one row's run file, run Mimic on it, verify the run and count its galaxies.

    Args:
        prepared: The capture's checked inputs.
        row: The design row.
        runs_directory: ``<out>/runs``; the row's directory is created inside it and must not
            exist.
        timeout_s: Seconds before the run is killed.
        runner: The capture's runner; a new one when ``None``.

    Returns:
        The run's record. A run that fails verification has no counts. A run not started
        because the runner is halted, or killed by an interrupt, records why. A property whose
        unit label Mimic cannot convert sets ``fatal``, which stops the capture.
    """
    runner = runner or CaptureRunner()
    record = RunRecord(row.identifier, row.set_name)
    if runner.reason is not None:
        record.failures.append(f"not run: {runner.reason}")
        return record
    run_directory = runs_directory / row.identifier
    run_directory.mkdir()
    output_directory = run_directory / "output"
    document = build_run_file(prepared.template, prepared.spec, row.values, output_directory)
    run_file = run_directory / "run.yaml"
    run_bytes = dump_run_file(document).encode("utf-8")
    run_file.write_bytes(run_bytes)
    expected = expected_run(
        prepared, document, hashlib.md5(run_bytes, usedforsecurity=False).hexdigest()
    )
    started = time.monotonic()
    with (run_directory / "mimic.log").open("wb") as log:
        process = runner.start(
            [f"./{EXECUTABLE}", "-q", str(run_file.resolve())],
            cwd=prepared.mimic_root,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        if process is None:
            record.failures.append(f"not run: {runner.reason}")
            return record
        try:
            record.exit_status = process.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            _signal_group(process, signal.SIGKILL)
            process.wait()
            record.failures.append(f"timed out after {timeout_s} s")
        finally:
            runner.finished(process)
    record.wall_time_s = time.monotonic() - started
    if runner.interrupted and record.exit_status != 0:
        record.failures.append(INTERRUPTED)
        return record
    verification = verify_run(output_directory, record.exit_status, expected)
    record.verification = verification
    record.verified = verification.verified
    record.failures.extend(verification.failures)
    if not verification.verified:
        return record
    statistic = prepared.spec.statistic
    try:
        masses, rows = galaxy_log_masses(
            prepared.helpers,
            output_directory / f"{expected.output_filename}.hdf5",
            statistic.property,
            statistic.snapshot,
            verification.hubble_h,
        )
    except UnsupportedUnitError as exc:
        record.fatal = str(exc)
        record.failures.append(str(exc))
        return record
    except StatisticError as exc:
        record.failures.append(str(exc))
        return record
    converted = convert_constraint(
        prepared.inputs.constraint, prepared.spec.constraint.conversion, verification.hubble_h
    )
    record.rows = rows
    record.counts = bin_counts(masses, *candidate_bins(converted.centre, statistic.bin_width_dex))
    return record


def select_rows(
    rows: Sequence[DesignRow], set_names: Sequence[str] | None, allowed: Sequence[str]
) -> list[DesignRow]:
    """Return the design rows whose set is named, in design order (every row when ``None``).

    Args:
        rows: The design.
        set_names: The sets to run, or ``None`` for every row.
        allowed: The set labels that exist.

    Raises:
        CaptureError: If a name is not a set, or the selection is empty.
    """
    if set_names is not None:
        unknown = [name for name in set_names if name not in allowed]
        if not set_names or unknown:
            raise CaptureError(
                f"--sets: {unknown or 'nothing'} named; the design's sets are {', '.join(allowed)}"
            )
        rows = [row for row in rows if row.set_name in set_names]
    if not rows:
        raise CaptureError("the design has no rows to run")
    return list(rows)


def _distinct(values: Sequence[Any]) -> Any:
    """Return the one distinct value, None when there is none, or the sorted list of several."""
    distinct = sorted(set(values), key=repr)
    if not distinct:
        return None
    return distinct[0] if len(distinct) == 1 else distinct


def capture_status(records: Sequence[RunRecord]) -> CaptureStatus:
    """Summarise a capture's records: its exit status and what its runs reported as a whole.

    The status is 0 when every run verified and was counted with no failure, and every run that
    reported one reported the same Mimic version and commit, ``BoxSize`` and ``Hubble_h``; it is
    2 otherwise. A pure function of the records.
    """
    reported = [r.verification for r in records if r.verification is not None]
    summary = {
        name: _distinct([getattr(v, name) for v in reported if getattr(v, name) is not None])
        for name in ("version", "commit", "box_size", "hubble_h")
    }
    consistent = not any(isinstance(value, list) for value in summary.values())
    counted = all(r.verified and r.counts is not None and not r.failures for r in records)
    fatal = next((r.fatal for r in records if r.fatal), None)
    return CaptureStatus(
        exit_status=EXIT_OK if records and counted and consistent and not fatal else EXIT_ERROR,
        verified=sum(1 for r in records if r.verified),
        fatal=fatal,
        **summary,
    )


def run_all(
    prepared: Prepared,
    rows: Sequence[DesignRow],
    runs_directory: Path,
    workers: int,
    timeout_s: float,
    runner: CaptureRunner,
) -> dict[str, RunRecord]:
    """Run every row on ``workers`` threads; a fatal record halts the runs not yet started.

    Returns:
        A record per row, by identifier; an exception in a run is that run's failure.
    """
    records: dict[str, RunRecord] = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(run_evaluation, prepared, row, runs_directory, timeout_s, runner): row
            for row in rows
        }
        for future in as_completed(futures):
            row = futures[future]
            try:
                record = future.result()
            except Exception as exc:  # recorded as that run's failure; the capture goes on
                record = RunRecord(row.identifier, row.set_name, failures=[f"error: {exc!r}"])
            records[row.identifier] = record
            if record.fatal:
                runner.halt(f"capture stopped ({record.fatal})", kill=False)
    return records


def _write_csv(path: Path, header: Sequence[str], lines: Sequence[Sequence[Any]]) -> None:
    """Write a CSV file with ``\\n`` line endings."""
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(lines)


def write_records(
    out: Path,
    prepared: Prepared,
    rows: Sequence[DesignRow],
    records: Mapping[str, RunRecord],
    hubble_h: Any,
) -> None:
    """Write ``runs.csv``, ``outputs.csv`` and ``bins.csv``, rows in design order.

    Args:
        out: The capture directory.
        prepared: The capture's inputs.
        rows: The design rows run.
        records: Each row's record, by identifier.
        hubble_h: The runs' one ``Hubble_h``, which places the bins; anything else leaves the
            centres and edges empty.
    """
    _write_csv(
        out / "runs.csv",
        RUNS_COLUMNS,
        [
            [
                r.identifier,
                r.set_name,
                "" if r.exit_status is None else r.exit_status,
                "" if r.wall_time_s is None else f"{r.wall_time_s:.3f}",
                "true" if r.verified else "false",
                "" if r.rows is None else r.rows,
                "; ".join(r.failures),
            ]
            for r in (records[row.identifier] for row in rows)
        ],
    )
    table = prepared.inputs.constraint
    labels = [output_label(value) for value in table.log10_mass]
    _write_csv(
        out / "outputs.csv",
        ["id", *labels],
        [
            [row.identifier, *([""] * len(labels) if counts is None else [int(c) for c in counts])]
            for row, counts in ((row, records[row.identifier].counts) for row in rows)
        ],
    )
    statistic = prepared.spec.statistic
    edges: list[Sequence[Any]] = [[""] * len(labels)] * 3
    if isinstance(hubble_h, float):
        centres = convert_constraint(table, prepared.spec.constraint.conversion, hubble_h).centre
        edges = [centres, *candidate_bins(centres, statistic.bin_width_dex)]
    _write_csv(
        out / "bins.csv",
        BINS_COLUMNS,
        [
            [
                label,
                statistic.property,
                statistic.snapshot,
                repr(statistic.bin_width_dex),
                *(cell if cell == "" else repr(float(cell)) for cell in cells),
            ]
            for label, *cells in zip(labels, *edges, strict=True)
        ],
    )


def start_capture(
    spec_path: Path,
    mimic_root: Path,
    set_names: Sequence[str] | None,
    out: Path,
    workers: int,
    timeout_s: float,
    design_path: Path | None,
) -> tuple[Prepared, list[DesignRow]]:
    """Check the arguments and every input, then make the capture directory and its design.

    Returns:
        The checked inputs and the design rows to run, written to ``<out>/design.csv``.

    Raises:
        CaptureError, DesignError, SpecError, ConstraintError, RunFileError, StatisticError: If
            the capture cannot start; nothing has been written.
    """
    out = Path(out)
    if out.exists() or out.is_symlink():
        raise CaptureError(f"{out}: already exists; a capture is made once, into a new directory")
    if workers < 1:
        raise CaptureError(f"--workers: must be a positive integer, found {workers}")
    if not timeout_s > 0:
        raise CaptureError(f"--timeout-s: must be positive, found {timeout_s}")
    prepared = prepare(spec_path, mimic_root)
    if design_path is None:
        if set_names is None:
            raise CaptureError("--sets: name the spec's design sets to run")
        rows = select_rows(build_design(prepared.spec), set_names, SET_NAMES)
    else:
        design = parse_design(_read(Path(design_path), "the design"), design_path, prepared.spec)
        labels = list(dict.fromkeys(row.set_name for row in design))
        rows = select_rows(design, set_names, labels)
    (out / "runs").mkdir(parents=True)
    write_design(rows, prepared.spec, out / "design.csv")
    return prepared, rows


def write_context(
    out: Path,
    header: Mapping[str, Any],
    prepared: Prepared,
    runs: int,
    status: CaptureStatus,
    interrupted: bool,
    exit_status: int,
) -> None:
    """Write ``context.json``: ``header`` (what was asked and who ran it), then the inputs' hashes,
    the request hash, the design's source and hash, what Mimic reported, the tree files' directory
    and sizes, and the outcome."""
    design_path = header["arguments"]["design"]
    context = {
        **header,
        "request_sha256": prepared.inputs.request_sha256,
        "spec": {
            "path": str(prepared.inputs.spec_path),
            "sha256": prepared.inputs.spec_sha256,
        },
        "constraint": {
            "path": str(prepared.inputs.constraint_path),
            "sha256": prepared.inputs.constraint_sha256,
        },
        "design": {
            "source": "spec" if design_path is None else design_path,
            "sha256": sha256_bytes((out / "design.csv").read_bytes()),
        },
        "mimic": {
            "root": str(prepared.mimic_root),
            "version": status.version,
            "commit": status.commit,
            "box_size": status.box_size,
            "hubble_h": status.hubble_h,
            "files_sha256": prepared.files,
            "trees": {
                "directory": prepared.trees["directory"],
                "files": [
                    {"name": name, "size_bytes": found[0]}
                    for name, found in prepared.trees["files"].items()
                ],
            },
        },
        "ended_utc": _utc_now(),
        "runs": runs,
        "verified": status.verified,
        "fatal": status.fatal,
        "interrupted": interrupted,
        "exit_status": exit_status,
    }
    (out / "context.json").write_text(json.dumps(context, indent=2) + "\n")


def run_capture(
    spec_path: Path,
    mimic_root: Path,
    set_names: Sequence[str] | None,
    out: Path,
    workers: int = 8,
    timeout_s: float = RUN_TIMEOUT_S,
    design_path: Path | None = None,
) -> int:
    """Run a capture and return its exit status.

    Nothing is written when the output directory exists or a check before the first run fails
    (:func:`start_capture`). Otherwise every file is written even when runs fail or the capture
    is interrupted (SIGINT or SIGTERM, handled while this runs in the main thread), and the
    failures are recorded.

    Args:
        spec_path: The campaign spec.
        mimic_root: The Mimic checkout.
        set_names: The design sets to run; ``None`` (only with ``design_path``) for every row.
        out: The capture directory; it must not exist.
        workers: The number of runs at once.
        timeout_s: Seconds one run may take.
        design_path: A design CSV to run instead of the spec's design.

    Returns:
        0 when :func:`capture_status` is 0, no input changed during the capture
        (:func:`changed_inputs`, recorded as ``fatal``) and it was not interrupted; 2 otherwise.

    Raises:
        CaptureError, DesignError, SpecError, ConstraintError, RunFileError, StatisticError: If
            the capture cannot start; nothing has been written.
    """
    out = Path(out)
    header = {
        "command_line": list(getattr(sys, "orig_argv", sys.argv)),
        "arguments": {
            "spec": str(spec_path),
            "design": None if design_path is None else str(design_path),
            "sets": None if set_names is None else list(set_names),
            "out": str(out),
            "workers": workers,
            "timeout_s": timeout_s,
        },
        "package": package_identity(),
        "libraries": library_versions(),
        "python": sys.version,
        "host": {"name": socket.gethostname(), "platform": platform.platform()},
        "started_utc": _utc_now(),
    }
    prepared, rows = start_capture(
        spec_path, mimic_root, set_names, out, workers, timeout_s, design_path
    )
    runner = CaptureRunner()
    previous = {}
    if threading.current_thread() is threading.main_thread():
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous[signum] = signal.signal(signum, lambda *_: runner.halt(INTERRUPTED, kill=True))
    try:
        records = run_all(prepared, rows, out / "runs", workers, timeout_s, runner)
        status = capture_status([records[row.identifier] for row in rows])
        changed = changed_inputs(prepared)
        if changed:
            message = f"{', '.join(changed)} changed during the capture"
            fatal = f"{status.fatal}; {message}" if status.fatal else message
            status = replace(status, exit_status=EXIT_ERROR, fatal=fatal)
        interrupted = runner.interrupted
        exit_status = EXIT_ERROR if interrupted else status.exit_status
        write_records(out, prepared, rows, records, status.hubble_h)
        write_context(out, header, prepared, len(rows), status, interrupted, exit_status)
        write_manifests(out)
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)
    return exit_status


def capture_outcome_error(context: Mapping[str, Any], path: Path) -> str | None:
    """Return why a capture is not a measurement, or ``None`` when its exit status is 0.

    ``context.json`` records exit status 2 when a run failed, the capture was interrupted, an
    input changed during it, or its runs reported more than one Mimic version or commit,
    ``BoxSize`` or ``Hubble_h``.

    Args:
        context: The capture's ``context.json``.
        path: The ``context.json`` file, named in the message.
    """
    status = context.get("exit_status")
    if type(status) is int and status == EXIT_OK:
        return None
    return (
        f"{path}: the capture's exit status is {status!r}, not 0 (fatal: {context.get('fatal')!r}, "
        f"interrupted: {context.get('interrupted')!r}); a failed capture is not a measurement"
    )


def select_from_capture(inputs: SpecInputs, capture: Path) -> list[BinSelection]:
    """Apply the spec's output selection rule to a finished pilot capture, writing nothing.

    The capture's top-level manifest is checked, its request must be the spec's, every run must
    have verified and its exit status must be 0 (:func:`capture_outcome_error`). Its ``pilot``
    rows in ``design.csv`` must be, row for row, the pilot ``design`` writes from the spec:
    identifiers and set labels equal, each value equal to a relative tolerance of 1e-12, and each must have a record in ``runs.csv``: the rule reads the default row
    first, then the corners, then the Latin-hypercube runs, as the spec lays the set out.

    Args:
        inputs: The spec and constraint table (:func:`~mimic_bench.provenance.read_spec_inputs`).
        capture: The capture directory.

    Returns:
        The rule's reading of every candidate bin.

    Raises:
        CaptureError: If the capture's request differs, a run did not verify, the capture
            failed, its pilot rows are not the spec's, or a pilot row lacks a record or a count.
        ManifestError: If the capture is incomplete or altered.
    """
    capture = Path(capture)
    spec = inputs.spec
    files = check_manifest(capture, CAPTURE_FILES)
    context = read_json_object(files["context.json"], capture / "context.json")
    mismatch = request_mismatch(context, inputs, capture / "context.json")
    if mismatch:
        raise CaptureError(mismatch)
    runs = read_csv_rows(files["runs.csv"], capture / "runs.csv", RUNS_COLUMNS)[1:]
    failed = [run[0] for run in runs if run[4] != "true" or run[6]]
    if failed:
        raise CaptureError(f"{capture}: runs {failed} did not verify; the capture is unusable")
    outcome = capture_outcome_error(context, capture / "context.json")
    if outcome:
        raise CaptureError(outcome)
    hubble_h = (context.get("mimic") or {}).get("hubble_h")
    if not isinstance(hubble_h, (int, float)):
        raise CaptureError(f"{capture / 'context.json'}: no single Hubble_h, found {hubble_h!r}")

    pilot = spec.design.sets["pilot"]
    if not pilot.include_defaults:
        raise CaptureError("design.sets.pilot.include_defaults: the rule needs the default row")
    leading = 1 + (2 ** len(spec.parameters) if pilot.include_corners else 0)
    expected = [row for row in build_design(spec) if row.set_name == "pilot"]
    design = read_csv_rows(files["design.csv"], capture / "design.csv", design_columns(spec))
    pilot_rows = [row for row in design[1:] if row[1] == "pilot"]
    if not _same_pilot(pilot_rows, expected):
        raise CaptureError(
            f"{capture / 'design.csv'}: its {len(pilot_rows)} pilot rows are not the "
            f"{len(expected)} rows design writes for the spec's pilot set; the rule reads the "
            "spec's pilot layout"
        )
    recorded = {run[0] for run in runs}
    unrecorded = [row[0] for row in pilot_rows if row[0] not in recorded]
    if unrecorded:
        raise CaptureError(
            f"{capture / 'runs.csv'}: {len(unrecorded)} pilot row(s) have no record, first "
            f"{unrecorded[0]}"
        )

    table = inputs.constraint
    labels = [output_label(value) for value in table.log10_mass]
    outputs = read_csv_rows(files["outputs.csv"], capture / "outputs.csv")
    by_id = {row[0]: dict(zip(outputs[0], row, strict=True)) for row in outputs[1:]}
    try:
        counts = np.array(
            [[int(by_id[row[0]][label]) for label in labels] for row in pilot_rows],
            dtype=np.int64,
        )
    except (KeyError, ValueError) as exc:
        raise CaptureError(
            f"{capture / 'outputs.csv'}: missing or non-integer count ({exc})"
        ) from exc
    converted = convert_constraint(table, spec.constraint.conversion, float(hubble_h))
    return select_outputs(
        labels=table.log10_mass,
        centres=converted.centre,
        error_not_below_value=converted.error_not_below_value,
        latin_hypercube_counts=counts[leading:],
        default_counts=counts[0],
        rule=spec.outputs.selection_rule,
    )


def _same_pilot(rows: Sequence[Sequence[str]], expected: Sequence[DesignRow]) -> bool:
    """Whether the capture's pilot ``rows`` are ``expected``: identifier and set label equal,
    each value equal to a relative tolerance of :data:`PILOT_REL_TOL`."""
    if len(rows) != len(expected):
        return False
    for row, wanted in zip(rows, expected, strict=True):
        if len(row) != 2 + len(wanted.values) or tuple(row[:2]) != (
            wanted.identifier,
            wanted.set_name,
        ):
            return False
        try:
            values = [float(cell) for cell in row[2:]]
        except ValueError:
            return False
        if not all(
            math.isclose(value, want, rel_tol=PILOT_REL_TOL, abs_tol=0.0)
            for value, want in zip(values, wanted.values, strict=True)
        ):
            return False
    return True


def format_selection(selections: Sequence[BinSelection]) -> str:
    """Format a selection as text: the core and exploratory lists, then one line per bin."""
    core = [s.label for s in selections if s.status == "core"]
    exploratory = [s.label for s in selections if s.status == "exploratory"]
    lines = [
        f"core: [{', '.join(repr(v) for v in core)}]",
        f"exploratory: [{', '.join(repr(v) for v in exploratory)}]",
        "",
        f"{'log10_mass':>10} {'centre':>8} {'err>=val':>8} {'default':>7} {'min_lh':>6} "
        f"{'lh_meet':>7}  status       reason",
    ]
    for s in selections:
        lines.append(
            f"{s.label:>10.2f} {s.centre:>8.4f} {str(s.error_not_below_value).lower():>8} "
            f"{s.default_count:>7d} {s.min_count:>6d} {s.runs_meeting:>7d}  {s.status:<12} "
            f"{s.reason}"
        )
    return "\n".join(lines) + "\n"
