"""Shared harness for the per-package cross-format parity gates.

A parity gate runs the same real merger trees through the vertical driver (the
source format's own reader, ``GatePackage.vertical``) and through the
horizontal driver (``GatePackage.horizontal``, a conversion of exactly the files
the vertical side reads), and requires, for every output snapshot, identical
``UniqueGalaxyID`` sets and per-id **byte-identical** fields. There is no
tolerance, no field exclusion and no sampling of records anywhere in it. Parity
is promised against the package's **own** source format only.

Each ``simulations/<package>/_tests/scientific/test_cross_format_identity.py``
is a thin file: a ``GatePackage`` naming what the package pins, and a ``main()``
calling ``ParityGate(PACKAGE).run(title)``. The version 2 gate subclasses
``ParityGate`` to append its vertical-path preservation stage. Usage::

    make MODEL=halos-only SIMULATION=<horizontal package> tests-scientific

A missing dataset, a dataset that is not the pinned conversion, or a leg that
does not run **fails** the gate: it never skips, because a gate that reports
success having compared nothing is worse than no gate.

Stages, in order (one MIMIC_RESULT marker each):

1. ``stage_preconditions`` -- both datasets resolve, with named paths; the
   vertical side's effective file range is the one the package pins; enough
   free space; HEAD is captured once and used for every worktree.
2. ``stage_dataset_provenance`` -- every snapshot file the a_list names carries
   the pinned header (format version, links_adjacent, snapshot number, and for
   version 3 the source format and column-mapping digest); the files hold the
   pinned halo and forest counts; for version 3 also the gapped-descendant
   census and the ``forests.h5`` ``SourceFileOrdinal`` inventory naming exactly
   the vertical side's source files.
3. ``stage_run_files`` -- the comparator and every committed run file match
   HEAD byte for byte; the vertical range override (when the package needs one)
   differs from its run file in exactly ``input.first_file``/``last_file``; the
   horizontal run file differs from the effective vertical one in exactly
   ``simulation.name`` and ``output.output_directory``.
4. ``stage_builds`` -- one detached worktree per model x {vertical, horizontal}
   at HEAD, built with ``make generate`` then ``make``. The ambient tier build
   is never touched.
5. One stage per (model, scheme) leg, e.g. ``stage_halos_only_fixed``. Each leg
   runs both sides from the worktrees' own run files, asserts that both runs
   recorded ``RunProperties/TimestepScheme`` equal to the leg's scheme and the
   pinned source file range, checks the structural preflight equalities, runs
   the horizontal worktree's copy of ``scripts/compare_cross_format_identity.py``
   (it reports every divergence by snapshot, field and example id), and then
   checks snapshot coverage and that the comparator compared every record the
   runs wrote. Legs run whatever the others' outcome, except that with
   ``require_halos_only_before_sage16`` a sage16 leg fails its prerequisite
   unless every halos-only leg passed.
6. ``stage_leg_verdicts`` -- names every leg's verdict and fails unless all
   passed. A leg that did not run is a failure.

Worktrees and scratch outputs are removed on every exit path, including an
interrupt; an interrupt first records a FAIL marker for the stage in flight.
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
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import h5py
import numpy
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

# The comparison algorithm has exactly one implementation. The gate runs the
# horizontal worktree's copy of the script for every verdict and imports the
# working-tree copy only for partition_files and schema_signature, so the
# working-tree copy is required to match HEAD before every use: a dirty
# comparator must never shape the evidence on the gate's behalf.
import compare_cross_format_identity as comparator  # noqa: E402

from .markers import result_fail  # noqa: E402
from .runner import run_test_suite  # noqa: E402

#: Vertical readers whose tree files are numbered ``<tree_name>.<N>``; any other
#: reader names a single index file unless the package lists its files.
NUMBERED_TREE_FILE_READERS = ("lhalo_binary",)

#: The conversion inventory: one row per forest (version 3 also names the source
#: file each forest came from).
FORESTS_FILE = "forests.h5"
SOURCE_FILE_ORDINAL = "SourceFileOrdinal"
FOREST_ID = "ForestID"

#: Attributes required to be exactly equal between the two runs of a leg before
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

#: The only functional keys the horizontal run file may change relative to the
#: effective vertical one. ``simulation.name`` selects the package under test;
#: ``output.output_directory`` keeps the two runs from writing over each other.
AUTHORIZED_KEY_CHANGES = ("simulation.name", "output.output_directory")

#: The only functional keys the vertical range override may change.
RANGE_KEYS = ("input.first_file", "input.last_file")

#: The committed comparator; pinned to HEAD.
COMPARATOR_RELATIVE = Path("scripts") / "compare_cross_format_identity.py"
COMPARATOR_SCRIPT = REPO_ROOT / COMPARATOR_RELATIVE

#: The run-file key the dynamic legs add. Mimic's parameter reader takes the
#: first matching key, so a run file already carrying it would make a "dynamic"
#: leg silently run whatever that first value says.
TIMESTEP_SCHEME_KEY = "TimestepScheme"

SNAP_GROUP_RE = re.compile(r"^Snap(\d+)$")
FILE_GROUP_RE = re.compile(r"^File(\d+)$")
SNAPSHOT_FILE_RE = re.compile(r"^snapshot_(\d{3})\.h5$")

COMPARATOR_PASS_RE = re.compile(
    r"^PASSED: (\d+) galaxies over (\d+) output snapshot\(s\) are bitwise identical "
    r"in all (\d+) field"
)


# --------------------------------------------------------------------------
# Package description
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class GatePackage:
    """Everything one package's parity gate pins.

    Args:
        vertical: The vertical simulation package holding the source trees.
        horizontal: The horizontal package holding their conversion.
        alist: The horizontal package's snapshot-list filename.
        evidence: One sentence stating what this gate's data covers.
        file_range: (first_file, last_file) both sides must read.
        override_vertical_range: True when ``file_range`` differs from the
            committed vertical run file's effective range, so the vertical side
            runs a scratch copy overriding exactly the two range keys.
        format_version: The horizontal dataset's ``header/format_version``.
        source_format: The vertical package's ``input.tree_type``; for version
            3 also the expected header ``source_format``.
        column_mapping_sha256: The pinned conversion digest (None for version 2,
            whose files carry none).
        links_adjacent, halos, forests, gapped_descendants, max_descendant_span:
            The pinned dataset census.
        required_free_bytes: Free space the worktrees and runs need.
        models: Models compared, in the order their legs run.
        schemes: Timestep schemes compared per model.
        vertical_dataset_files: Files a non-numbered reader needs from its
            dataset directory; when empty they are derived from the reader.
        require_halos_only_before_sage16: Fail every sage16 leg's prerequisite
            unless all halos-only legs passed.
    """

    vertical: str
    horizontal: str
    alist: str
    evidence: str
    file_range: tuple[int, int]
    override_vertical_range: bool
    format_version: int
    source_format: str
    column_mapping_sha256: str | None
    links_adjacent: int
    halos: int
    forests: int
    gapped_descendants: int
    max_descendant_span: int
    required_free_bytes: int
    models: tuple[str, ...] = ("halos-only",)
    schemes: tuple[str, ...] = ("fixed", "dynamic")
    vertical_dataset_files: tuple[str, ...] = ()
    require_halos_only_before_sage16: bool = False
    build_timeout_s: int = 3600
    run_timeout_s: int = 8 * 3600
    compare_timeout_s: int = 2 * 3600
    git_timeout_s: int = 600

    @property
    def legs(self) -> tuple[tuple[str, str], ...]:
        """Every (model, scheme) leg, in the order it runs and is reported."""
        return tuple((model, scheme) for model in self.models for scheme in self.schemes)


# --------------------------------------------------------------------------
# Progress reporting
# --------------------------------------------------------------------------


_STARTED = time.monotonic()


def log(message: str) -> None:
    """Print one progress line, prefixed with elapsed wall time, unbuffered."""
    elapsed = int(time.monotonic() - _STARTED)
    print(f"[gate {elapsed // 60:3d}m{elapsed % 60:02d}s] {message}", flush=True)


def banner(title: str) -> None:
    print(flush=True)
    log("=" * 70)
    log(title)
    log("=" * 70)


# --------------------------------------------------------------------------
# Pure run-file helpers (unit-tested by tests/integration/test_parity_gate_helpers.py)
# --------------------------------------------------------------------------


def flatten_keys(node, prefix: str = "") -> dict[str, object]:
    """Flatten a parsed run file to ``section.key`` -> value, lists kept whole."""
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


def _parsed(path: Path) -> dict[str, object]:
    return flatten_keys(yaml.safe_load(path.read_text()) or {})


def functional_lines(text: str) -> list[str]:
    """A run file's non-blank lines with every comment removed.

    Comments carry no functional weight, so the textual diff checks compare
    only what the parameter reader sees.
    """
    lines = []
    for line in text.splitlines():
        stripped = line.split("#", 1)[0].rstrip()
        if stripped.strip():
            lines.append(stripped)
    return lines


def run_file_range(run_file: Path) -> tuple[int, int] | None:
    """The run file's own input.first_file/last_file, or None if it declares neither."""
    flat = _parsed(run_file)
    present = [key for key in RANGE_KEYS if key in flat]
    if not present:
        return None
    if len(present) != len(RANGE_KEYS):
        raise AssertionError(f"{run_file}: declares {present} but not both of {list(RANGE_KEYS)}")
    return int(flat["input.first_file"]), int(flat["input.last_file"])


def range_override_variant(run_file: Path, scratch: Path, file_range: tuple[int, int]) -> Path:
    """Write ``run_file`` with its input range set to ``file_range``, into ``scratch``.

    Verified to differ from the given file in exactly the input.first_file and
    input.last_file values, so the vertical side is otherwise the committed
    configuration. The file must already declare both keys: the variant only
    rewrites their values, and refuses (AssertionError) otherwise.
    """
    if run_file_range(run_file) is None:
        raise AssertionError(f"{run_file}: declares no input range for the override to rewrite")
    first, last = file_range
    replacements = {"first_file": first, "last_file": last}
    out = []
    in_input = False
    rewritten = set()
    for line in run_file.read_text().splitlines(keepends=True):
        if line and not line[0].isspace() and not line.startswith("#"):
            in_input = line.split(":", 1)[0].strip() == "input"
        key = line.strip().split(":", 1)[0]
        if in_input and key in replacements and line.startswith("  ") and key not in rewritten:
            indent = line[: len(line) - len(line.lstrip())]
            out.append(f"{indent}{key}: {replacements[key]}\n")
            rewritten.add(key)
        else:
            out.append(line)
    if rewritten != set(replacements):
        raise AssertionError(
            f"{run_file}: could not rewrite {sorted(set(replacements) - rewritten)}"
        )
    variant = scratch / f"{run_file.stem}_files_{first}-{last}.yaml"
    variant.write_text("".join(out))

    base, written = _parsed(run_file), _parsed(variant)
    changed = sorted(key for key in set(base) | set(written) if base.get(key) != written.get(key))
    if set(base) != set(written) or not set(changed) <= set(RANGE_KEYS):
        raise AssertionError(
            f"{variant} differs from {run_file} in {changed}, not only {list(RANGE_KEYS)}"
        )
    if run_file_range(variant) != tuple(file_range):
        raise AssertionError(
            f"{variant}: input range is {run_file_range(variant)}, not {tuple(file_range)}"
        )
    left, right = functional_lines(run_file.read_text()), functional_lines(variant.read_text())
    line_changes = [index for index in range(len(left)) if left[index] != right[index]]
    if len(left) != len(right) or len(line_changes) > len(RANGE_KEYS):
        raise AssertionError(f"{variant} is not {run_file} with only its range lines rewritten")
    return variant


def dynamic_variant(run_file: Path, scratch: Path) -> Path:
    """Write ``run_file`` with ``TimestepScheme: dynamic`` added after SubSteps.

    Verified to be the given file plus exactly that one line, so the two runs of
    a leg are otherwise byte-identical. The file must carry no TimestepScheme
    key of its own: the parameter reader takes the first matching key, so an
    existing one would silently decide the dynamic leg (AssertionError).
    """
    text = run_file.read_text()
    if TIMESTEP_SCHEME_KEY in _parsed(run_file) or any(
        line.strip().startswith(f"{TIMESTEP_SCHEME_KEY}:") for line in text.splitlines()
    ):
        raise AssertionError(
            f"{run_file} already declares {TIMESTEP_SCHEME_KEY}; the reader takes the first "
            f"matching key, so the dynamic leg could silently run another scheme"
        )
    out = []
    inserted = False
    for line in text.splitlines(keepends=True):
        out.append(line)
        if not inserted and line.strip().startswith("SubSteps:"):
            out.append(f"{TIMESTEP_SCHEME_KEY}: dynamic\n")
            inserted = True
    if not inserted:
        raise AssertionError(f"{run_file}: no SubSteps line to place TimestepScheme after")
    variant = scratch / f"{run_file.stem}_dynamic.yaml"
    variant.write_text("".join(out))

    base = text.splitlines()
    written = variant.read_text().splitlines()
    added = [
        index
        for index in range(len(written))
        if written[:index] + written[index + 1 :] == base
        and written[index].strip() == f"{TIMESTEP_SCHEME_KEY}: dynamic"
    ]
    if len(written) != len(base) + 1 or not added:
        raise AssertionError(
            f"{variant} is not {run_file} with exactly one 'TimestepScheme: dynamic' line added"
        )
    if _parsed(variant).get(TIMESTEP_SCHEME_KEY) != "dynamic":
        raise AssertionError(f"{variant}: {TIMESTEP_SCHEME_KEY} does not parse as 'dynamic'")
    return variant


def assert_horizontal_run_file_diff(
    vertical_path: Path, horizontal_path: Path, horizontal_simulation: str
) -> None:
    """The horizontal run file differs from the effective vertical one in exactly two keys.

    Checked on the parsed YAML (the keys must be exactly AUTHORIZED_KEY_CHANGES)
    and on the comment-free lines (exactly two, ``name: <horizontal>`` then an
    ``output_directory:`` assignment), since comments carry no functional weight.
    Raises AssertionError naming every other difference.
    """
    left_text = vertical_path.read_text()
    right_text = horizontal_path.read_text()

    left_flat = flatten_keys(yaml.safe_load(left_text) or {})
    right_flat = flatten_keys(yaml.safe_load(right_text) or {})
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
    if right_flat["simulation.name"] != horizontal_simulation:
        raise AssertionError(
            f"{horizontal_path}: simulation.name is {right_flat['simulation.name']!r}, "
            f"expected {horizontal_simulation!r}"
        )
    if not str(right_flat["output.output_directory"]).endswith(f"-{horizontal_simulation}"):
        raise AssertionError(
            f"{horizontal_path}: output.output_directory is "
            f"{right_flat['output.output_directory']!r}, which does not name a "
            f"{horizontal_simulation} output directory, so the two runs would share one"
        )

    left_body = functional_lines(left_text)
    right_body = functional_lines(right_text)
    if len(left_body) != len(right_body):
        raise AssertionError(
            f"{horizontal_path.name} has {len(right_body)} functional line(s), "
            f"{vertical_path.name} has {len(left_body)}"
        )
    changed = [index for index in range(len(left_body)) if left_body[index] != right_body[index]]
    if len(changed) != 2:
        detail = "\n".join(
            f"    {left_body[index]!r} -> {right_body[index]!r}" for index in changed
        )
        raise AssertionError(
            f"{horizontal_path} differs from {vertical_path} in {len(changed)} functional "
            f"line(s), expected exactly 2:\n{detail}"
        )
    name_line, output_line = (right_body[index].strip() for index in changed)
    if name_line != f"name: {horizontal_simulation}":
        raise AssertionError(
            f"{horizontal_path}: first changed line is {name_line!r}, "
            f"expected 'name: {horizontal_simulation}'"
        )
    if not output_line.startswith("output_directory:"):
        raise AssertionError(
            f"{horizontal_path}: second changed line is {output_line!r}, "
            f"expected an output_directory: assignment"
        )


# --------------------------------------------------------------------------
# Small readers
# --------------------------------------------------------------------------


def package_path(package: str, *parts: str) -> Path:
    return REPO_ROOT.joinpath("simulations", package, *parts)


def snapshot_list_entries(path: Path) -> list[str]:
    """Return the scale factors listed in an a_list file."""
    return [line.strip() for line in path.read_text().splitlines() if line.strip()]


def decoded(value) -> str:
    """An HDF5 string attribute as a Python str, whatever h5py returned for it."""
    value = numpy.asarray(value).ravel()[0]
    return value.decode() if isinstance(value, bytes) else str(value)


def tail(path: Path, lines: int = 40) -> str:
    try:
        content = path.read_text(errors="replace").splitlines()
    except OSError as error:
        return f"(log {path} unreadable: {error})"
    return "\n".join(content[-lines:])


def attr_value(raw) -> tuple[str, tuple, bytes]:
    """Return an attribute as (dtype, shape, raw bytes) for exact comparison."""
    array = numpy.asarray(raw)
    return str(array.dtype), array.shape, array.tobytes()


def read_run_file_output(run_file: Path) -> tuple[str, str]:
    """Return (output_directory, output_filename) as the run file declares them."""
    output = (yaml.safe_load(run_file.read_text()) or {}).get("output") or {}
    directory = output.get("output_directory")
    filename = output.get("output_filename")
    if directory is None:
        raise AssertionError(f"{run_file}: output.output_directory not found")
    if filename is None:
        raise AssertionError(f"{run_file}: output.output_filename not found")
    return str(directory), str(filename)


def scratch_parent() -> Path:
    """The repository's machine-local, gitignored ``output/`` if present, else the system tmp."""
    output = REPO_ROOT / "output"
    return output if output.is_dir() else Path(tempfile.gettempdir())


def assert_dataset_present(package: str, required_files) -> Path:
    """Fail -- never skip -- when a package's machine-local dataset is absent."""
    link = package_path(package, "snapshots")
    if not link.exists():
        target = os.readlink(link) if link.is_symlink() else "(no symlink)"
        raise AssertionError(
            f"{package} dataset is not available: {link} does not resolve "
            f"(symlink target: {target}). The gate requires the real source trees and "
            f"their conversion; it fails rather than skipping."
        )
    if not link.is_dir():
        raise AssertionError(f"{package} dataset path is not a directory: {link}")
    for name in required_files:
        entry = link / name
        if not entry.exists():
            raise AssertionError(f"{package} dataset is incomplete: missing {entry}")
    return link


class RunOutput:
    """One completed Mimic run: where its files are and what they claim."""

    def __init__(self, key: str, directory: Path, basename: str, worktree: Path, run_file: Path):
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

    def properties(self):
        """(attrs as raw triples, string attrs, integer attrs, Redshifts, FieldMetadata)."""
        with h5py.File(self.master, "r") as handle:
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

    def requested_snapshots(self) -> set[int]:
        """The output snapshots the run file under test asks for."""
        selected = ((yaml.safe_load(self.run_file.read_text()) or {}).get("output") or {}).get(
            "snapshot_list"
        )
        if not selected:
            raise AssertionError(f"{self.run_file}: no output.snapshot_list to compare against")
        return {int(entry) for entry in selected}

    def recorded_records(self) -> dict[int, int]:
        """Galaxies per output snapshot, from the master's TotHalosPerSnap attributes.

        Read independently of the comparator, so the count it reports can be
        checked against a number it had no part in producing.
        """
        counts: dict[int, int] = {}
        with h5py.File(self.master, "r") as handle:
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
                        raise AssertionError(
                            f"{self.master}: {name}/{child} has no TotHalosPerSnap"
                        )
                    total += int(numpy.asarray(attrs["TotHalosPerSnap"]).ravel()[0])
                counts[int(match.group(1))] = total
        if not counts:
            raise AssertionError(f"{self.master}: no Snap### groups")
        return counts

    def record_signature(self):
        """The Galaxies record schema of the run, from its first partition."""
        partitions = self.partitions()
        if not partitions:
            raise AssertionError(f"{self.key}: no partition files next to {self.master}")
        with h5py.File(partitions[0], "r") as handle:
            for name in handle:
                if SNAP_GROUP_RE.match(name) and "Galaxies" in handle[name]:
                    return comparator.schema_signature(handle[f"{name}/Galaxies"].dtype)
        raise AssertionError(f"{partitions[0]}: no Snap###/Galaxies dataset")


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


def _stage_name(model: str, scheme: str) -> str:
    return f"stage_{model.replace('-', '_')}_{scheme}"


# --------------------------------------------------------------------------
# The gate
# --------------------------------------------------------------------------


class ParityGate:
    """One package's parity gate: its stages, its state, and its cleanup.

    A context manager: leaving it removes every worktree and the scratch
    directory. ``run()`` enters it itself.
    """

    def __init__(self, package: GatePackage):
        self.package = package
        self.scratch: Path | None = None
        self.worktrees: dict[str, Path] = {}
        self.done: set[str] = set()
        #: (model, scheme) -> "PASS" or "FAIL: <first line of the failure>".
        self.verdicts: dict[tuple[str, str], str] = {}
        #: run key -> RunOutput, for stages that reuse an earlier run.
        self.runs: dict[str, RunOutput] = {}
        #: The commit every HEAD worktree is built at, captured once.
        self.head: str | None = None
        self.vertical_inventory: dict | None = None
        #: The ``__name__`` of the stage in flight, for the interrupt marker.
        self.current_stage: str | None = None

    # ---- lifecycle -------------------------------------------------------

    def __enter__(self) -> "ParityGate":
        return self

    def __exit__(self, *_exc) -> bool:
        self.cleanup()
        return False

    def cleanup(self) -> None:
        """Remove every worktree and the scratch directory. Safe to call twice."""
        for key, path in list(self.worktrees.items()):
            try:
                subprocess.run(
                    ["git", "worktree", "remove", "--force", str(path)],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    timeout=self.package.git_timeout_s,
                )
            except subprocess.TimeoutExpired:
                pass
            finally:
                self.worktrees.pop(key, None)
        try:
            subprocess.run(
                ["git", "worktree", "prune"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                timeout=self.package.git_timeout_s,
            )
        except subprocess.TimeoutExpired:
            pass
        if self.scratch is not None and self.scratch.exists():
            shutil.rmtree(self.scratch, ignore_errors=True)

    @staticmethod
    def _raise_on_signal(signum, _frame):
        # Raise through SystemExit so cleanup still runs, then exit with the
        # conventional status for the signal.
        raise SystemExit(128 + signum)

    def stage(self, function: Callable[[], None], name: str | None = None) -> Callable[[], None]:
        """Wrap a stage so the stage in flight is known; ``name`` becomes its marker name."""

        def tracked():
            self.current_stage = tracked.__name__
            try:
                function()
            except SystemExit:
                # Left set: run() records the interrupted stage's FAIL marker.
                raise
            except BaseException:
                self.current_stage = None
                raise
            self.current_stage = None

        tracked.__name__ = name or function.__name__
        return tracked

    def stages(self) -> list[Callable[[], None]]:
        """The ordered stages ``run()`` hands to run_test_suite."""
        legs = [
            self.stage(lambda m=model, s=scheme: self.leg(m, s), _stage_name(model, scheme))
            for model, scheme in self.package.legs
        ]
        return [
            self.stage(self.stage_preconditions),
            self.stage(self.stage_dataset_provenance),
            self.stage(self.stage_run_files),
            self.stage(self.stage_builds),
            *legs,
            self.stage(self.stage_leg_verdicts),
        ]

    def run(self, title: str) -> int:
        """Run every stage with markers; clean up on every exit path. Returns the exit code."""
        atexit.register(self.cleanup)
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(signum, self._raise_on_signal)
        with self:
            try:
                # No abort_on_failure: an aborted stage is reported as SKIP, and a
                # leg that does not run must FAIL. A setup failure instead makes
                # every later stage fail its own require() at once, and each leg
                # runs whatever the others' outcome, so every divergence is reported.
                return run_test_suite(self.stages(), title)
            except SystemExit as interrupt:
                if self.current_stage is not None:
                    result_fail(self.current_stage, f"interrupted (exit status {interrupt.code})")
                raise

    # ---- small services --------------------------------------------------

    def require(self, stage_name: str, what: str) -> None:
        """Fail this stage when an earlier one it depends on did not complete."""
        if stage_name not in self.done:
            raise AssertionError(f"prerequisite stage '{stage_name}' did not complete; {what}")

    def scratch_dir(self, *parts: str) -> Path:
        assert self.scratch is not None, "scratch directory not created"
        path = self.scratch.joinpath(*parts)
        path.mkdir(parents=True, exist_ok=True)
        return path

    def git(self, *args: str) -> str:
        try:
            completed = subprocess.run(
                ["git", *args],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                timeout=self.package.git_timeout_s,
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError(
                f"git {' '.join(args)} timed out after {error.timeout}s"
            ) from error
        if completed.returncode != 0:
            raise AssertionError(f"git {' '.join(args)} failed: {completed.stderr.strip()}")
        return completed.stdout.strip()

    def run_logged(
        self, cmd, cwd, env, log_path: Path, what: str, timeout: int, show_tail: int = 0
    ) -> None:
        """Run a command with its output captured to a file; fail loudly on error or timeout."""
        log(f"  -> {what}")
        started = time.monotonic()
        with log_path.open("wb") as handle:
            try:
                completed = subprocess.run(
                    cmd, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT, timeout=timeout
                )
            except subprocess.TimeoutExpired:
                raise AssertionError(
                    f"{what} timed out after {timeout}s\n  log: {log_path}\n"
                    f"--- last lines ---\n{tail(log_path)}"
                ) from None
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

    def head_bytes(self, path: Path) -> bytes:
        """The committed bytes of a repository file at ``self.head``."""
        relative = path.relative_to(REPO_ROOT).as_posix()
        try:
            completed = subprocess.run(
                ["git", "show", f"{self.head}:{relative}"],
                cwd=REPO_ROOT,
                capture_output=True,
                timeout=self.package.git_timeout_s,
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError(
                f"git show {self.head}:{relative} timed out after {error.timeout}s"
            ) from error
        if completed.returncode != 0:
            raise AssertionError(
                f"{path} is not committed at {self.head}: git show failed "
                f"({completed.stderr.decode(errors='replace').strip()})"
            )
        return completed.stdout

    def assert_matches_head(self, path: Path) -> None:
        """Fail when a working-tree file the gate relies on differs from its HEAD copy.

        Used for the run files and the comparator. The worktrees execute their
        own run-file copies, so a dirty working-tree run file would never reach
        the runs -- but it would still be the file stage 3 inspected. The
        working-tree comparator is imported for its record helpers, so a dirty
        copy would shape the evidence while the record says it is unchanged.
        """
        if path.read_bytes() != self.head_bytes(path):
            raise AssertionError(
                f"{path} differs from its committed copy at HEAD ({self.head}). This gate "
                f"certifies committed run files and the committed comparator against HEAD-built "
                f"executables; commit or revert the file and re-run."
            )

    @staticmethod
    def committed_run_file(model: str, simulation: str) -> Path:
        return REPO_ROOT / "models" / model / "input" / f"{model}_{simulation}.yaml"

    def effective_vertical_run_file(self, run_file: Path, scratch: Path) -> Path:
        """The vertical run file the gate executes: committed, or its range override."""
        if self.package.override_vertical_range:
            return range_override_variant(run_file, scratch, self.package.file_range)
        return run_file

    # ---- stage 1 ---------------------------------------------------------

    def read_vertical_inventory(self) -> dict:
        """The source files the vertical side reads, from its effective run configuration."""
        pkg = self.package
        info_path = package_path(pkg.vertical, "simulation_info.yaml")
        info = (yaml.safe_load(info_path.read_text()) or {}).get("input") or {}
        for key in ("tree_type", "tree_name", "first_file", "last_file"):
            if key not in info:
                raise AssertionError(f"{info_path}: input.{key} is not declared")
        if info["tree_type"] != pkg.source_format:
            raise AssertionError(
                f"{info_path}: input.tree_type is {info['tree_type']!r}; the gate compares "
                f"against the {pkg.source_format} vertical reader"
            )
        first, last = pkg.file_range
        if last < first:
            raise AssertionError(f"file_range {pkg.file_range}: last_file is before first_file")
        committed_ranges = {}
        for model in pkg.models:
            run_file = self.committed_run_file(model, pkg.vertical)
            committed = run_file_range(run_file)
            if committed is None:
                committed = (int(info["first_file"]), int(info["last_file"]))
            if pkg.override_vertical_range == (committed == tuple(pkg.file_range)):
                raise AssertionError(
                    f"{run_file}: effective range is {committed}; the package pins "
                    f"{pkg.file_range} with override_vertical_range="
                    f"{pkg.override_vertical_range}, which would make the override either "
                    f"missing or a no-op"
                )
            committed_ranges[model] = committed
        tree_name = str(info["tree_name"])
        if pkg.vertical_dataset_files:
            files = list(pkg.vertical_dataset_files)
        elif info["tree_type"] in NUMBERED_TREE_FILE_READERS:
            files = [f"{tree_name}.{index}" for index in range(first, last + 1)]
        else:
            files = [tree_name]
        return {
            "tree_name": tree_name,
            "first_file": first,
            "last_file": last,
            "committed_ranges": committed_ranges,
            "files": files,
        }

    def expected_snapshot_files(self) -> list[str]:
        alist = package_path(self.package.horizontal, self.package.alist)
        if not alist.is_file():
            raise AssertionError(f"horizontal package a_list is missing: {alist}")
        return [f"snapshot_{index:03d}.h5" for index in range(len(snapshot_list_entries(alist)))]

    def stage_preconditions(self) -> None:
        """Both datasets resolve, with named paths on failure; HEAD is captured once."""
        pkg = self.package
        banner(f"Stage 1: preconditions ({pkg.horizontal}; {pkg.evidence})")

        inventory = self.read_vertical_inventory()
        vertical_data = assert_dataset_present(pkg.vertical, inventory["files"])
        log(
            f"  {pkg.vertical}: {vertical_data} -> {vertical_data.resolve()} "
            f"({len(inventory['files'])} source file(s): {', '.join(inventory['files'])})"
        )
        committed = ", ".join(
            f"{model} {first}-{last}"
            for model, (first, last) in inventory["committed_ranges"].items()
        )
        log(
            f"  file range: {pkg.file_range[0]}-{pkg.file_range[1]} on both sides (committed "
            f"vertical run file range {committed}"
            f"{', overridden in a scratch copy' if pkg.override_vertical_range else ''})"
        )

        required = self.expected_snapshot_files()
        horizontal_data = assert_dataset_present(pkg.horizontal, (*required, FORESTS_FILE))
        log(
            f"  {pkg.horizontal}: {horizontal_data} -> {horizontal_data.resolve()} "
            f"({len(required)} snapshot files from the a_list, plus {FORESTS_FILE})"
        )

        parent = scratch_parent()
        usage = shutil.disk_usage(parent)
        if usage.free < pkg.required_free_bytes:
            raise AssertionError(
                f"{parent}: {usage.free / 1024**3:.1f} GiB free, the gate needs at least "
                f"{pkg.required_free_bytes / 1024**3:.0f} GiB for its worktrees and runs"
            )

        self.head = self.git("rev-parse", "HEAD")
        self.scratch = Path(tempfile.mkdtemp(prefix=f"mimic-{pkg.horizontal}-gate-", dir=parent))
        self.vertical_inventory = inventory
        log(f"  scratch: {self.scratch}")
        log(f"  HEAD: {self.head} (every worktree is built at this commit)")
        self.done.add("preconditions")

    # ---- stage 2 ---------------------------------------------------------

    def stage_dataset_provenance(self) -> None:
        """The horizontal dataset is the pinned conversion of the vertical side's files."""
        pkg = self.package
        banner(f"Stage 2: horizontal dataset provenance (format version {pkg.format_version})")
        self.require("preconditions", "not inspecting the dataset")

        directory = package_path(pkg.horizontal, "snapshots")
        names = sorted(
            entry.name for entry in directory.iterdir() if SNAPSHOT_FILE_RE.match(entry.name)
        )
        expected_names = self.expected_snapshot_files()
        if names != expected_names:
            raise AssertionError(
                f"{directory}: snapshot files {names[:3]}...{names[-3:]} ({len(names)}) are not "
                f"exactly the {len(expected_names)} the a_list names"
            )

        # Version 2 has no /schema, no source_format or digest attribute and no
        # target-snapshot link columns, and its forests.h5 holds only ForestID:
        # the gapped-descendant census and the source-file inventory below exist
        # only for version 3. Version 2 is adjacent-only by construction.
        version3 = pkg.format_version == 3
        halos = gapped = max_span = populated = 0
        forests_total = set()
        for name in names:
            path = directory / name
            snapshot = int(SNAPSHOT_FILE_RE.match(name).group(1))
            with h5py.File(path, "r") as handle:
                header = handle["header"].attrs
                checks = {
                    "format_version": (int(header["format_version"]), pkg.format_version),
                    "links_adjacent": (int(header["links_adjacent"]), pkg.links_adjacent),
                    "snapshot_number": (int(header["snapshot_number"]), snapshot),
                }
                if version3:
                    checks["source_format"] = (decoded(header["source_format"]), pkg.source_format)
                    checks["column_mapping_sha256"] = (
                        decoded(header["column_mapping_sha256"]),
                        pkg.column_mapping_sha256,
                    )
                for attribute, (found, expected) in checks.items():
                    if found != expected:
                        raise AssertionError(
                            f"{path}: header {attribute} is {found!r}, expected {expected!r}; "
                            f"this is not the conversion the gate certifies"
                        )
                n_halos = int(header["n_halos"])
                forests_total.add(int(header["n_forests_total"]))
                descendant_snapshot = (
                    numpy.asarray(handle["halos/DescendantSnapshot"][()]) if version3 else None
                )
            halos += n_halos
            populated += int(n_halos > 0)
            if descendant_snapshot is None:
                continue
            if descendant_snapshot.shape != (n_halos,):
                raise AssertionError(
                    f"{path}: DescendantSnapshot has shape {descendant_snapshot.shape}, "
                    f"header n_halos is {n_halos}"
                )
            spans = descendant_snapshot[descendant_snapshot >= 0].astype(numpy.int64) - snapshot
            gapped += int((spans > 1).sum())
            if spans.size:
                max_span = max(max_span, int(spans.max()))

        identity = (
            f"from {pkg.source_format}, column_mapping_sha256 {pkg.column_mapping_sha256}, "
            if version3
            else ""
        )
        log(
            f"  {len(names)} version {pkg.format_version} files ({populated} populated) "
            f"{identity}links_adjacent {pkg.links_adjacent}"
        )
        if len(forests_total) != 1:
            raise AssertionError(
                f"{directory}: header n_forests_total varies across files: {forests_total}"
            )
        forests = forests_total.pop()
        if version3:
            log(f"  {halos} halos, {gapped} gapped descendant links, longest span {max_span}")
            measured = (halos, forests, gapped, max_span)
            expected = (pkg.halos, pkg.forests, pkg.gapped_descendants, pkg.max_descendant_span)
            what = "(halos, forests, gapped descendant links, longest span)"
        else:
            log(f"  {halos} halos, n_forests_total {forests} (no gap census in version 2)")
            measured, expected, what = (
                (halos, forests),
                (pkg.halos, pkg.forests),
                "(halos, forests)",
            )
        if measured != expected:
            raise AssertionError(
                f"{directory}: {what} is {measured}, expected {expected} for the pinned "
                f"conversion of files {pkg.file_range[0]}-{pkg.file_range[1]}"
            )

        with h5py.File(directory / FORESTS_FILE, "r") as handle:
            if version3:
                rows = numpy.asarray(handle[SOURCE_FILE_ORDINAL][()])
            else:
                rows = numpy.asarray(handle[FOREST_ID][()])
        if rows.size != pkg.forests:
            raise AssertionError(
                f"{directory / FORESTS_FILE}: {rows.size} forests, expected {pkg.forests}"
            )
        if not version3:
            log(f"  {FORESTS_FILE}: {rows.size} {FOREST_ID} rows, the pinned forest count")
            self.done.add("dataset")
            return

        # The conversion inventory must name exactly the vertical side's files,
        # or the two sides are not the same trees.
        inventory = self.vertical_inventory
        converted = sorted(int(value) for value in numpy.unique(rows))
        read = list(range(inventory["first_file"], inventory["last_file"] + 1))
        if converted != read:
            raise AssertionError(
                f"{directory / FORESTS_FILE}: the conversion inventory names source files "
                f"{converted}, the vertical side reads {inventory['tree_name']} files {read}"
            )
        log(
            f"  conversion inventory: {rows.size} forests from source files {converted}, "
            f"exactly the vertical side's {inventory['files'][0]}..{inventory['files'][-1]}"
        )
        self.done.add("dataset")

    # ---- stage 3 ---------------------------------------------------------

    def stage_run_files(self) -> None:
        """The comparator and run files are HEAD's; each horizontal file differs in two keys."""
        pkg = self.package
        banner("Stage 3: committed comparator and run-file diffs")
        self.require("dataset", "not checking run files")

        self.assert_matches_head(COMPARATOR_SCRIPT)
        log(f"  {COMPARATOR_RELATIVE} matches its committed HEAD copy")

        for model in pkg.models:
            vertical_path = self.committed_run_file(model, pkg.vertical)
            horizontal_path = self.committed_run_file(model, pkg.horizontal)
            for path in (vertical_path, horizontal_path):
                if not path.is_file():
                    raise AssertionError(f"run file is missing: {path}")
                self.assert_matches_head(path)
            log(f"  {model}: both run files match their committed HEAD copies")

            effective = self.effective_vertical_run_file(
                vertical_path, self.scratch_dir("run-files", "check", model)
            )
            if effective != vertical_path:
                log(
                    f"  {model}: vertical side runs {effective.name} = {vertical_path.name} "
                    f"with only {list(RANGE_KEYS)} set to {pkg.file_range}"
                )
            assert_horizontal_run_file_diff(effective, horizontal_path, pkg.horizontal)
            log(
                f"  {model}: {horizontal_path.name} differs from the effective vertical run "
                f"file in exactly {list(AUTHORIZED_KEY_CHANGES)}"
            )
        self.done.add("run-files")

    # ---- stage 4 ---------------------------------------------------------

    @staticmethod
    def link_machine_local(worktree: Path) -> None:
        """Recreate the main tree's machine-local symlinks inside a worktree.

        The dataset directories and the Python virtual environment are
        gitignored, so a fresh worktree has neither. Both are recreated from the
        main tree's resolved targets.
        """
        for package in sorted((REPO_ROOT / "simulations").iterdir()):
            source = package / "snapshots"
            if not source.is_symlink():
                continue
            destination = worktree / "simulations" / package.name / "snapshots"
            if not destination.parent.is_dir():
                continue
            target = source.resolve()
            if target.exists():
                destination.symlink_to(target)
        venv = REPO_ROOT / "mimic_venv"
        if venv.is_dir():
            (worktree / "mimic_venv").symlink_to(venv)

    @staticmethod
    def worktree_env(worktree: Path, model: str, simulation: str) -> dict:
        """Environment for make/mimic inside a worktree: venv on PATH, no test build."""
        env = dict(os.environ)
        # The gate compares production executables. The ambient scientific tier
        # is a TEST_BUILD, which carries the framework's fixture modules and
        # their test-only properties; inheriting it would compare another record.
        env.pop("MIMIC_TEST_BUILD", None)
        env.pop("TEST_BUILD", None)
        env.pop("SIM", None)
        env["MODEL"] = model
        env["SIMULATION"] = simulation
        env["VIRTUAL_ENV"] = str(worktree / "mimic_venv")
        env["PATH"] = f"{worktree / 'mimic_venv' / 'bin'}{os.pathsep}{env.get('PATH', '')}"
        return env

    @staticmethod
    def pair_key(model: str, simulation: str) -> str:
        return f"{model}__{simulation}"

    def build_worktree(self, key: str, commit: str, model: str, simulation: str) -> Path:
        """Create a detached worktree at ``commit`` and build one model/simulation pair."""
        pkg = self.package
        worktree = self.scratch_dir("worktrees") / key
        logs = self.scratch_dir("logs")
        log(f"  worktree {key}: {model} x {simulation} at {commit}")
        try:
            completed = subprocess.run(
                ["git", "worktree", "add", "--detach", str(worktree), commit],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                timeout=pkg.git_timeout_s,
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError(
                f"git worktree add {key} timed out after {error.timeout}s"
            ) from error
        if completed.returncode != 0:
            raise AssertionError(
                f"git worktree add {worktree} {commit} failed (exit {completed.returncode}): "
                f"{completed.stderr.strip()}"
            )
        self.worktrees[key] = worktree
        self.link_machine_local(worktree)

        env = self.worktree_env(worktree, model, simulation)
        selectors = [f"MODEL={model}", f"SIMULATION={simulation}"]
        self.run_logged(
            ["make", *selectors, "generate"],
            worktree,
            env,
            logs / f"{key}-generate.log",
            f"{key}: make generate",
            timeout=pkg.build_timeout_s,
        )
        self.run_logged(
            ["make", *selectors, f"-j{os.cpu_count() or 4}"],
            worktree,
            env,
            logs / f"{key}-build.log",
            f"{key}: make",
            timeout=pkg.build_timeout_s,
        )
        if not (worktree / "mimic").is_file():
            raise AssertionError(f"{key}: build produced no executable at {worktree / 'mimic'}")
        return worktree

    def stage_builds(self) -> None:
        """One isolated worktree build per model x ordering, at the captured HEAD."""
        pkg = self.package
        banner(f"Stage 4: isolated per-pair builds at {self.head}")
        self.require("run-files", "not building")
        for model in pkg.models:
            for simulation in (pkg.vertical, pkg.horizontal):
                self.build_worktree(self.pair_key(model, simulation), self.head, model, simulation)
        self.done.add("builds")

    # ---- running ---------------------------------------------------------

    def run_in_worktree(
        self, key: str, worktree: Path, run_file: Path, model: str, simulation: str
    ) -> RunOutput:
        """Run ``run_file`` with the worktree's executable into its own scratch output root."""
        declared_directory, basename = read_run_file_output(run_file)

        # The run file's output_directory is relative to the working directory,
        # so a per-run scratch root is selected by pointing the worktree's
        # `output` at it, keeping the run file byte for byte the one tested.
        output_root = self.scratch_dir("runs", key)
        link = worktree / "output"
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(output_root)

        log(f"  run {key}: {run_file}")
        self.run_logged(
            ["./mimic", str(run_file)],
            worktree,
            self.worktree_env(worktree, model, simulation),
            self.scratch_dir("logs") / f"{key}-run.log",
            f"{key}: mimic run",
            timeout=self.package.run_timeout_s,
            show_tail=3,
        )
        directory = output_root / Path(declared_directory).name
        if not directory.is_dir():
            raise AssertionError(f"{key}: run produced no output directory at {directory}")
        run = RunOutput(key, directory, basename, worktree, run_file)
        if not run.master.is_file():
            raise AssertionError(f"{key}: run produced no master file at {run.master}")
        self.runs[key] = run
        log(f"     {len(run.partitions())} partition file(s) + master {run.master.name}")
        return run

    def execute_run(self, model: str, simulation: str, scheme: str) -> RunOutput:
        """Run one model x ordering x scheme from the HEAD worktree's own run file."""
        key = f"{model}__{simulation}__{scheme}"
        worktree = self.worktrees[self.pair_key(model, simulation)]

        # The worktree's OWN copy of the run file, never the working tree's: the
        # executable is pinned to HEAD, so its input must be too.
        worktree_run_file = worktree / self.committed_run_file(model, simulation).relative_to(
            REPO_ROOT
        )
        if not worktree_run_file.is_file():
            raise AssertionError(f"{key}: the HEAD worktree has no run file at {worktree_run_file}")

        scratch = self.scratch_dir("run-files", key)
        run_file = worktree_run_file
        if simulation == self.package.vertical:
            run_file = self.effective_vertical_run_file(worktree_run_file, scratch)
        if scheme == "dynamic":
            run_file = dynamic_variant(run_file, scratch)
        return self.run_in_worktree(key, worktree, run_file, model, simulation)

    # ---- leg checks ------------------------------------------------------

    def assert_timestep_scheme(self, scheme: str, *runs: RunOutput) -> None:
        """Every run recorded the scheme its leg is named for (RunProperties/TimestepScheme)."""
        for run in runs:
            recorded = run.properties()[1].get(TIMESTEP_SCHEME_KEY)
            if recorded != scheme:
                raise AssertionError(
                    f"{run.master}: RunProperties/{TIMESTEP_SCHEME_KEY} is {recorded!r}, the leg "
                    f"is {scheme!r}; the run did not execute the scheme it is certified for"
                )
        log(f"  {TIMESTEP_SCHEME_KEY}: both runs record {scheme!r}")

    def assert_recorded_file_ranges(self, vertical_run: RunOutput, horizontal_run: RunOutput):
        """Both runs recorded the pinned source file range, and the vertical run read it.

        Stage 2 established what the dataset was converted from; this checks
        that the vertical run itself recorded reading that range, from that
        tree name, out of a directory resolving to the same data, and that the
        horizontal run's metadata names the same range.
        """
        pkg = self.package
        inventory = self.vertical_inventory
        _, strings, integers, _, _ = vertical_run.properties()
        recorded = (strings.get("TreeName"), integers.get("FirstFile"), integers.get("LastFile"))
        expected = (inventory["tree_name"], inventory["first_file"], inventory["last_file"])
        if recorded != expected:
            raise AssertionError(
                f"{vertical_run.master}: the vertical run recorded (TreeName, FirstFile, "
                f"LastFile) {recorded}, the package pins {expected}"
            )
        directory = vertical_run.worktree / strings.get("SimulationDir", "")
        main_directory = package_path(pkg.vertical, "snapshots")
        if directory.resolve() != main_directory.resolve():
            raise AssertionError(
                f"{vertical_run.master}: the vertical run read {directory} "
                f"(-> {directory.resolve()}), not the dataset stage 1 checked "
                f"({main_directory.resolve()})"
            )
        horizontal_integers = horizontal_run.properties()[2]
        horizontal_range = (
            horizontal_integers.get("FirstFile"),
            horizontal_integers.get("LastFile"),
        )
        if horizontal_range != tuple(pkg.file_range):
            raise AssertionError(
                f"{horizontal_run.master}: the horizontal run recorded (FirstFile, LastFile) "
                f"{horizontal_range}, the package pins {pkg.file_range}"
            )
        log(
            f"  vertical run read {inventory['files'][0]}..{inventory['files'][-1]} from "
            f"{directory.resolve()}; both runs record FirstFile-LastFile "
            f"{pkg.file_range[0]}-{pkg.file_range[1]}"
        )

    @staticmethod
    def structural_preflight(vertical_run: RunOutput, horizontal_run: RunOutput) -> int:
        """Exact equality of everything the two runs must agree on before comparison.

        Returns the field count, derived independently of the comparator.
        """
        alists = []
        for run in (vertical_run, horizontal_run):
            strings = run.properties()[1]
            if "FileWithSnapList" not in strings:
                raise AssertionError(f"{run.master}: no FileWithSnapList attribute")
            path = run.worktree / strings["FileWithSnapList"]
            if not path.is_file():
                raise AssertionError(f"snapshot list is missing: {path}")
            alists.append(path)
        if alists[0].read_bytes() != alists[1].read_bytes():
            raise AssertionError(f"snapshot lists differ:\n  {alists[0]}\n  {alists[1]}")
        log("  preflight: snapshot lists byte-equal")

        left_attrs, _, _, left_z, left_fields = vertical_run.properties()
        right_attrs, _, _, right_z, right_fields = horizontal_run.properties()
        if (left_z.dtype, left_z.shape) != (right_z.dtype, right_z.shape) or (
            left_z.tobytes() != right_z.tobytes()
        ):
            raise AssertionError(
                f"recorded Redshifts differ: {left_z.dtype}{left_z.shape} vs "
                f"{right_z.dtype}{right_z.shape}, or their bytes"
            )
        # The scale-factor table the physics integrates over; equal by
        # construction from equal redshifts, asserted because it is what matters.
        if (1.0 / (1.0 + left_z)).tobytes() != (1.0 / (1.0 + right_z)).tobytes():
            raise AssertionError("derived scale factors 1/(1+z) differ between the runs")
        log(f"  preflight: {left_z.size} recorded redshifts and derived scale factors equal")

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

        # Field names, their order and their units must match exactly.
        # Descriptions are package-owned prose and are reported, not compared:
        # nothing below is derived from one.
        if left_fields.dtype != right_fields.dtype or left_fields.shape != right_fields.shape:
            raise AssertionError(
                f"FieldMetadata tables differ in shape or columns: "
                f"{left_fields.dtype}{left_fields.shape} vs "
                f"{right_fields.dtype}{right_fields.shape}"
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
                    f"  preflight: note -- {row['field_name'].decode()} carries a "
                    f"package-specific description on each side (prose only, not compared)"
                )

        left_signature = vertical_run.record_signature()
        right_signature = horizontal_run.record_signature()
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

    def run_comparator(self, leg: str, vertical_run: RunOutput, horizontal_run: RunOutput):
        """Run the horizontal worktree's comparator, logging its whole report.

        Returns (exit status, stdout). The working-tree copy is re-checked
        against HEAD at every use, and the worktree copy against it, so the
        script deciding the verdict is provably HEAD's.
        """
        self.assert_matches_head(COMPARATOR_SCRIPT)
        script = horizontal_run.worktree / COMPARATOR_RELATIVE
        if script.read_bytes() != COMPARATOR_SCRIPT.read_bytes():
            raise AssertionError(f"{script} differs from the HEAD comparator")
        command = [
            sys.executable,
            str(script),
            vertical_run.spec,
            horizontal_run.spec,
            "--left-label",
            "vertical",
            "--right-label",
            "horizontal",
        ]
        log(f"  comparing {leg}: {' '.join(command[1:])}")
        try:
            completed = subprocess.run(
                command,
                cwd=horizontal_run.worktree,
                capture_output=True,
                text=True,
                timeout=self.package.compare_timeout_s,
            )
        except subprocess.TimeoutExpired as error:
            raise AssertionError(
                f"{leg}: the comparator timed out after {error.timeout}s"
            ) from error
        for line in completed.stdout.splitlines():
            log(f"     | {line}")
        for line in completed.stderr.splitlines():
            log(f"     ! {line}")
        return completed.returncode, completed.stdout

    def check_leg(
        self, model: str, scheme: str, vertical_run: RunOutput, horizontal_run: RunOutput
    ) -> None:
        """Package-specific extra evidence for one leg; a subclass hook, empty here."""

    def parity_leg(self, model: str, scheme: str) -> None:
        """One parity leg: both runs, guards, preflight, the comparator, then coverage.

        The comparator runs before the count checks so that any divergence -- an
        id-set difference as much as a field difference -- is reported by
        snapshot, field and example id, rather than stopped at a count mismatch
        that says only that the runs disagree.
        """
        pkg = self.package
        leg = f"{model}/{scheme}"
        banner(f"Parity leg: {leg}")
        self.require("builds", f"leg {leg} did not run")
        if pkg.require_halos_only_before_sage16 and model == "sage16":
            # A halos-only divergence is a driver bug, reported before the
            # sage16 cost is paid; either halos-only failure blocks both sage16 legs.
            blocked = [s for s in pkg.schemes if self.verdicts.get(("halos-only", s)) != "PASS"]
            if blocked:
                raise AssertionError(
                    f"prerequisite: halos-only leg(s) {blocked} did not pass; a halos-only "
                    f"divergence is a driver bug and is reported before the sage16 cost is paid"
                )

        vertical_run = self.execute_run(model, pkg.vertical, scheme)
        horizontal_run = self.execute_run(model, pkg.horizontal, scheme)
        self.assert_timestep_scheme(scheme, vertical_run, horizontal_run)
        self.assert_recorded_file_ranges(vertical_run, horizontal_run)
        fields = self.structural_preflight(vertical_run, horizontal_run)

        status, stdout = self.run_comparator(leg, vertical_run, horizontal_run)
        if status != 0:
            raise AssertionError(
                f"{leg}: cross-format identity comparison failed (exit {status}); every "
                f"divergence is listed by snapshot, field and example id in the output above"
            )
        match = None
        for line in stdout.splitlines():
            match = COMPARATOR_PASS_RE.match(line.strip()) or match
        if match is None:
            raise AssertionError(f"{leg}: the comparator exited 0 without a PASSED summary line")
        compared_records, compared_snapshots, compared_fields = (int(v) for v in match.groups())

        vertical_counts = vertical_run.recorded_records()
        horizontal_counts = horizontal_run.recorded_records()
        if vertical_counts != horizontal_counts:
            raise AssertionError(
                f"{leg}: the two runs record different galaxy counts per output snapshot:\n"
                f"  vertical:   {vertical_counts}\n  horizontal: {horizontal_counts}"
            )
        expected = vertical_run.requested_snapshots()
        if horizontal_run.requested_snapshots() != expected:
            raise AssertionError(f"{leg}: the two run files request different output snapshots")
        assert_snapshot_coverage(vertical_run, vertical_counts, expected)
        assert_snapshot_coverage(horizontal_run, horizontal_counts, expected)

        records = sum(vertical_counts.values())
        if (compared_records, compared_snapshots, compared_fields) != (
            records,
            len(expected),
            fields,
        ):
            raise AssertionError(
                f"{leg}: the comparator compared {compared_records} galaxies over "
                f"{compared_snapshots} snapshots in {compared_fields} fields; the runs record "
                f"{records} galaxies over {len(expected)} requested snapshots in {fields} fields"
            )

        # One horizontal partition per requested snapshot is the horizontal
        # writer's contract; asserted so the count is evidence, not observation.
        horizontal_partitions = len(horizontal_run.partitions())
        if horizontal_partitions != len(expected):
            raise AssertionError(
                f"{leg}: the horizontal run wrote {horizontal_partitions} partition file(s) for "
                f"{len(expected)} requested output snapshot(s)"
            )
        self.check_leg(model, scheme, vertical_run, horizontal_run)
        per_snapshot = ", ".join(f"{snap}: {vertical_counts[snap]}" for snap in sorted(expected))
        log(
            f"  PASS {leg}: {compared_records} galaxies, {compared_fields} fields, "
            f"{compared_snapshots} output snapshots compared bitwise "
            f"(vertical {len(vertical_run.partitions())} partition(s) vs horizontal "
            f"{horizontal_partitions} partition(s)); per snapshot {{{per_snapshot}}}"
        )

    def leg(self, model: str, scheme: str) -> None:
        """Run one leg and record its verdict, whatever the outcome."""
        try:
            self.parity_leg(model, scheme)
        except BaseException as error:
            first = str(error).splitlines()[0] if str(error) else type(error).__name__
            self.verdicts[(model, scheme)] = f"FAIL: {first}"
            raise
        self.verdicts[(model, scheme)] = "PASS"
        self.done.add(f"leg:{model}:{scheme}")

    def stage_leg_verdicts(self) -> None:
        """Name every leg's verdict; the gate passes only if every leg passed."""
        pkg = self.package
        banner(f"Leg verdicts ({pkg.horizontal} against {pkg.vertical})")
        for model, scheme in pkg.legs:
            verdict = self.verdicts.get((model, scheme), "FAIL: leg did not run")
            log(f"  LEG {model}/{scheme}: {verdict}")
        failed = [f"{m}/{s}" for m, s in pkg.legs if self.verdicts.get((m, s)) != "PASS"]
        if failed:
            raise AssertionError(
                f"{len(failed)} of {len(pkg.legs)} parity leg(s) did not pass: {', '.join(failed)}"
            )
        log(f"  all {len(pkg.legs)} parity legs PASS")
