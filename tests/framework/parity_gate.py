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
calling ``ParityGate(PACKAGE).run(title)``; a gate with a stage of its own
subclasses ``ParityGate``. Usage::

    make MODEL=halos-only SIMULATION=<horizontal package> tests-scientific

A missing dataset, a dataset that is not the pinned conversion, or a leg that
does not run **fails** the gate: it never skips, because a gate that reports
success having compared nothing is worse than no gate.

Stages, in order (one MIMIC_RESULT marker each):

1. ``stage_preconditions`` -- both datasets resolve, with named paths; the
   vertical side's effective file range is the one the package pins; enough
   free space; HEAD is captured once and used for every worktree.
2. ``stage_dataset_provenance`` -- every snapshot file the a_list names carries
   the pinned header (format version, links_adjacent, snapshot number, source
   format and column-mapping digest); the files hold the pinned halo and forest
   counts and the pinned gapped-descendant census; and the ``forests.h5``
   ``SourceFileOrdinal`` inventory (or, for a gate that pins its conversion
   manifest, the manifest's source dependencies) names exactly the vertical
   side's source files.
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

A package naming a horizontal dataset of its own (``GatePackage.horizontal_dataset``)
has that dataset used by every stage in place of the package's ``snapshots``:
stage 1 checks its presence and its snapshot list, stage 2 its provenance,
stage 3 the horizontal run file's ``dataset_variant``, and each leg runs that
variant and checks that the horizontal run read the named dataset.

A package naming a vertical dataset of its own (``GatePackage.vertical_dataset``,
an L-Halo conversion of the horizontal dataset) has it used in the same way on the
vertical side: stage 1 checks that it holds the files of the pinned range and its
conversion manifest, stage 2 proves the inventory from that manifest, stage 3
diffs each horizontal run file against the **committed** vertical one and checks
the vertical run file's ``vertical_dataset_variant`` separately, and each leg runs
that variant and checks that the vertical run read the named dataset.

Worktrees and scratch outputs are removed on every exit path, including an
interrupt; an interrupt first records a FAIL marker for the stage in flight.
"""

from __future__ import annotations

import atexit
import contextlib
import fcntl
import hashlib
import io
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
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

from .fs import remove_tree  # noqa: E402
from .markers import result_fail  # noqa: E402
from .runner import run_test_suite  # noqa: E402

#: Vertical readers whose tree files are numbered ``<tree_name>.<N>``; any other
#: reader names a single index file unless the package lists its files.
NUMBERED_TREE_FILE_READERS = ("lhalo_binary",)

#: The conversion inventory: one row per forest, naming the source file each
#: forest came from.
FORESTS_FILE = "forests.h5"
SOURCE_FILE_ORDINAL = "SourceFileOrdinal"
SOURCE_UNIT_ORDINAL = "SourceUnitOrdinal"
FOREST_ID = "ForestID"

#: The conversion manifest's dependency role for a bulk source file the
#: converter read (convert/mimic-convert/conversion_manifest.py SOURCE_ROLE).
MANIFEST_SOURCE_ROLE = "source"

#: The converter directory, whose source_index.py reads the Consistent-Trees
#: index files for the manifest-proved inventory.
CONVERTER_DIR = REPO_ROOT / "convert" / "mimic-convert"

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

#: The further keys the horizontal run file sets when the gate names a dataset
#: other than the package's own (``GatePackage.horizontal_dataset``), and only then.
DATASET_OVERRIDE_KEYS = ("input.simulation_dir", "input.snapshot_list_file")

#: The only functional keys the vertical range override may change.
RANGE_KEYS = ("input.first_file", "input.last_file")

#: The keys the vertical run file gains when the gate names a vertical dataset
#: other than the package's own (``GatePackage.vertical_dataset``), and only then.
VERTICAL_DATASET_KEYS = ("input.simulation_dir", *RANGE_KEYS)

#: The conversion manifest an L-Halo conversion of a horizontal dataset carries
#: beside its files (convert/mimic-convert/vertical/writer.py MANIFEST_NAME,
#: MANIFEST_FORMAT and MANIFEST_VERSION).
VERTICAL_MANIFEST = "manifest.json"
VERTICAL_MANIFEST_FORMAT = ("mimic-vertical-conversion-manifest", 1)

#: Rows of ``forests.h5`` ``ForestID`` hashed at a time (8 MiB as int64).
FOREST_ID_BLOCK_ROWS = 1 << 20

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
        source_format: The vertical package's ``input.tree_type``, also the
            expected header ``source_format`` unless ``header_source_format``
            names another.
        header_source_format: The expected header ``source_format`` when it is
            not the vertical package's tree type, as for a vertical dataset
            converted from the horizontal one; None means ``source_format``.
        column_mapping_sha256: The pinned conversion digest, the expected header
            ``column_mapping_sha256``.
        links_adjacent, halos, forests, gapped_descendants, max_descendant_span:
            The pinned dataset census.
        required_free_bytes: Free space the worktrees and runs need.
        models: Models compared, in the order their legs run.
        schemes: Timestep schemes compared per model.
        vertical_dataset_files: Files a non-numbered reader needs from its
            dataset directory; when empty they are derived from the reader.
        require_halos_only_before_sage16: Fail every sage16 leg's prerequisite
            unless all halos-only legs passed.
        manifest: The dataset's conversion manifest. When given, the sidecar
            stage proves the conversion inventory from the manifest's source
            dependencies rather than inferring it from ``SourceFileOrdinal``,
            which for a Consistent-Trees ASCII conversion is the tree file's
            position in the converter's order and -1 for a forest spanning files.
        horizontal_dataset: (directory, snapshot list) of a horizontal dataset
            to gate in place of the package's own ``snapshots`` and a_list. The
            horizontal side then runs ``dataset_variant`` of its committed run
            file, which also sets ``input.simulation_dir`` and
            ``input.snapshot_list_file`` to exactly these paths
            (DATASET_OVERRIDE_KEYS), and records its package's declared file
            range rather than ``file_range``, since it reads the files the
            snapshot list names.
        vertical_dataset: (directory, first_file, last_file) of a vertical
            dataset converted from the horizontal one, to gate in place of the
            vertical package's own ``snapshots``; ``file_range`` must be this
            range. The vertical side then runs ``vertical_dataset_variant`` of
            its committed run file, which sets VERTICAL_DATASET_KEYS to exactly
            these values, and the inventory is proved from the directory's
            conversion manifest (VERTICAL_MANIFEST), so ``manifest`` must be None.
    """

    vertical: str
    horizontal: str
    alist: str
    evidence: str
    file_range: tuple[int, int]
    override_vertical_range: bool
    format_version: int
    source_format: str
    column_mapping_sha256: str
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
    manifest: Path | None = None
    horizontal_dataset: tuple[Path, Path] | None = None
    vertical_dataset: tuple[Path, int, int] | None = None
    header_source_format: str | None = None
    build_timeout_s: int = 3600
    run_timeout_s: int = 8 * 3600
    compare_timeout_s: int = 2 * 3600
    git_timeout_s: int = 600

    @property
    def legs(self) -> tuple[tuple[str, str], ...]:
        """Every (model, scheme) leg, in the order it runs and is reported."""
        return tuple((model, scheme) for model in self.models for scheme in self.schemes)

    def horizontal_snapshots(self) -> Path:
        """The horizontal dataset directory: the override's, else the package's ``snapshots``."""
        if self.horizontal_dataset is not None:
            return Path(self.horizontal_dataset[0])
        return package_path(self.horizontal, "snapshots")

    def horizontal_snapshot_list(self) -> Path:
        """The horizontal dataset's snapshot list: the override's, else the package's a_list."""
        if self.horizontal_dataset is not None:
            return Path(self.horizontal_dataset[1])
        return package_path(self.horizontal, self.alist)

    def vertical_snapshots(self) -> Path:
        """The vertical dataset directory: the override's, else the package's ``snapshots``."""
        if self.vertical_dataset is not None:
            return Path(self.vertical_dataset[0])
        return package_path(self.vertical, "snapshots")


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
    vertical_path: Path,
    horizontal_path: Path,
    horizontal_simulation: str,
    dataset_override: tuple[Path, Path] | None = None,
) -> None:
    """The horizontal run file differs from the effective vertical one in exactly two keys.

    Checked on the parsed YAML (the keys must be exactly AUTHORIZED_KEY_CHANGES)
    and on the comment-free lines (exactly two, ``name: <horizontal>`` then an
    ``output_directory:`` assignment), since comments carry no functional weight.

    With ``dataset_override`` (directory, snapshot list), and only then, the
    horizontal file must also set DATASET_OVERRIDE_KEYS to exactly those paths:
    each may be added (with the ``input:`` line opening its section, when the
    vertical file has none) or changed, and nothing else may. Their lines are
    set aside before the line check, which then applies unchanged.

    Raises AssertionError naming every other difference.
    """
    left_text = vertical_path.read_text()
    right_text = horizontal_path.read_text()

    left_flat = flatten_keys(yaml.safe_load(left_text) or {})
    right_flat = flatten_keys(yaml.safe_load(right_text) or {})
    override_keys = DATASET_OVERRIDE_KEYS if dataset_override is not None else ()
    only_vertical = set(left_flat) - set(right_flat)
    only_horizontal = set(right_flat) - set(left_flat) - set(override_keys)
    if only_vertical or only_horizontal:
        raise AssertionError(
            f"{horizontal_path} and {vertical_path} do not carry the same keys: "
            f"only vertical: {sorted(only_vertical)}; only horizontal: {sorted(only_horizontal)}"
        )
    allowed = sorted((*AUTHORIZED_KEY_CHANGES, *override_keys))
    changed_keys = sorted(key for key in right_flat if left_flat.get(key) != right_flat[key])
    if changed_keys != allowed:
        detail = "\n".join(
            f"    {key}: {left_flat.get(key)!r} -> {right_flat.get(key)!r}"
            for key in sorted(set(changed_keys) | set(allowed))
        )
        raise AssertionError(
            f"{horizontal_path} differs from {vertical_path} in functional key(s) "
            f"{changed_keys}, expected exactly {allowed}:\n{detail}"
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
    for key, expected in zip(override_keys, dataset_override or ()):
        if right_flat[key] != str(expected):
            raise AssertionError(
                f"{horizontal_path}: {key} is {right_flat[key]!r}, the gate names {str(expected)!r}"
            )

    left_body = functional_lines(left_text)
    right_body = functional_lines(right_text)
    if override_keys:
        leaves = {key.split(".", 1)[1] for key in override_keys}
        bodies = []
        for body in (left_body, right_body):
            kept, section = [], None
            for line in body:
                if not line[0].isspace():
                    section = line.split(":", 1)[0].strip()
                elif section == "input" and line.strip().split(":", 1)[0] in leaves:
                    continue
                kept.append(line)
            bodies.append(kept)
        left_body, right_body = bodies
        # An input section the override alone opened is set aside with it.
        if "input:" in right_body and "input:" not in left_body:
            at = right_body.index("input:")
            if at + 1 == len(right_body) or not right_body[at + 1][0].isspace():
                del right_body[at]
    if len(left_body) != len(right_body):
        raise AssertionError(
            f"{horizontal_path.name} has {len(right_body)} functional line(s), "
            f"{vertical_path.name} has {len(left_body)}"
            + (f", beyond its {list(override_keys)} lines" if override_keys else "")
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


def dataset_variant(run_file: Path, scratch: Path, dataset: tuple[Path, Path]) -> Path:
    """Write ``run_file`` with an input section pointing at ``dataset`` (directory, a_list).

    The section is appended, so the file must not have one of its own; the
    result is verified to be the given file plus exactly DATASET_OVERRIDE_KEYS.
    Paths are written as double-quoted YAML scalars (a JSON string is one), so a
    name holding ``#`` or ``: `` reaches the run file intact.
    """
    text = run_file.read_text()
    if "input" in (yaml.safe_load(text) or {}):
        raise AssertionError(
            f"{run_file} has an input section of its own; the dataset variant opens one"
        )
    directory, snapshot_list = dataset
    variant = scratch / f"{run_file.stem}_dataset.yaml"
    variant.write_text(
        text
        + ("" if text.endswith("\n") else "\n")
        + "input:\n"
        + f"  simulation_dir: {json.dumps(str(directory), ensure_ascii=False)}\n"
        + f"  snapshot_list_file: {json.dumps(str(snapshot_list), ensure_ascii=False)}\n"
    )
    base = flatten_keys(yaml.safe_load(text) or {})
    written = flatten_keys(yaml.safe_load(variant.read_text()) or {})
    added = {key: written[key] for key in set(written) - set(base)}
    if {k: v for k, v in written.items() if k in base} != base or added != {
        key: str(value) for key, value in zip(DATASET_OVERRIDE_KEYS, dataset)
    }:
        raise AssertionError(f"{variant} is not {run_file} plus exactly {DATASET_OVERRIDE_KEYS}")
    return variant


def vertical_dataset_variant(run_file: Path, scratch: Path, dataset: tuple[Path, int, int]) -> Path:
    """Write ``run_file`` with an input section reading ``dataset`` (directory, first, last).

    The vertical counterpart of ``dataset_variant``: the section is appended, so
    the file must not have one of its own, and the result is verified to be the
    given file plus exactly VERTICAL_DATASET_KEYS set to those values. The
    directory is written as a double-quoted YAML scalar, as ``dataset_variant``
    writes its paths.
    """
    text = run_file.read_text()
    if "input" in (yaml.safe_load(text) or {}):
        raise AssertionError(
            f"{run_file} has an input section of its own; the vertical dataset variant opens one"
        )
    directory, first, last = dataset
    variant = scratch / f"{run_file.stem}_dataset.yaml"
    variant.write_text(
        text
        + ("" if text.endswith("\n") else "\n")
        + "input:\n"
        + f"  simulation_dir: {json.dumps(str(directory), ensure_ascii=False)}\n"
        + f"  first_file: {first}\n  last_file: {last}\n"
    )
    base = flatten_keys(yaml.safe_load(text) or {})
    written = flatten_keys(yaml.safe_load(variant.read_text()) or {})
    added = {key: written[key] for key in set(written) - set(base)}
    if {k: v for k, v in written.items() if k in base} != base or added != dict(
        zip(VERTICAL_DATASET_KEYS, (str(directory), int(first), int(last)))
    ):
        raise AssertionError(f"{variant} is not {run_file} plus exactly {VERTICAL_DATASET_KEYS}")
    return variant


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


# --------------------------------------------------------------------------
# Worktree services (shared with the manual identity tests under tests/manual/)
# --------------------------------------------------------------------------


def run_logged(cmd, cwd, env, log_path: Path, what: str, timeout: int, show_tail: int = 0) -> float:
    """Run a command with its output captured to a file; fail loudly on error or timeout.

    Returns the elapsed wall time in seconds.
    """
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
    return elapsed


def build_pair(
    worktree: Path, model: str, simulation: str, env: dict, logs: Path, key: str, timeout: int
) -> None:
    """Run ``make generate`` then ``make`` for one model/simulation pair inside a worktree."""
    selectors = [f"MODEL={model}", f"SIMULATION={simulation}"]
    run_logged(
        ["make", *selectors, "generate"],
        worktree,
        env,
        logs / f"{key}-generate.log",
        f"{key}: make generate",
        timeout,
    )
    run_logged(
        ["make", *selectors, f"-j{os.cpu_count() or 4}"],
        worktree,
        env,
        logs / f"{key}-build.log",
        f"{key}: make",
        timeout,
    )
    if not (worktree / "mimic").is_file():
        raise AssertionError(f"{key}: build produced no executable at {worktree / 'mimic'}")


def point_output_at(worktree: Path, output_root: Path) -> None:
    """Point the worktree's ``output`` at a per-run scratch root.

    Run files name a relative output_directory, so redirecting ``output`` selects a scratch
    root while the run file stays byte for byte the one tested. Only a symlink is replaced: a real
    ``output`` directory or file may hold output of its own, so it is refused, never removed.
    """
    link = worktree / "output"
    if link.exists() and not link.is_symlink():
        raise AssertionError(
            f"{link} is a real directory or file, not a symlink this harness made, so it is not "
            "replaced; move it to cold storage and re-run"
        )
    if link.is_symlink():
        link.unlink()
    link.symlink_to(output_root)


def assert_dataset_present(package: str, required_files, directory: Path | None = None) -> Path:
    """Fail -- never skip -- when a package's machine-local dataset is absent.

    ``directory`` names a dataset the gate uses in place of the package's own
    ``snapshots`` (``GatePackage.horizontal_dataset``); it is checked the same way.
    """
    link = package_path(package, "snapshots") if directory is None else Path(directory)
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


def assert_manifest_inventory(manifest: Path, sidecar: Path, vertical_data: Path) -> None:
    """The sidecar stage's inventory proof from a dataset's conversion manifest.

    ``vertical_data`` is the vertical side's Consistent-Trees dataset directory,
    holding ``forests.list`` and ``locations.dat``.

    The manifest's source dependencies must name exactly the tree files the
    vertical reader opens, which for Consistent-Trees ASCII are the files
    ``locations.dat`` lists. Each forest held in one file must carry a
    ``SourceFileOrdinal`` inside that set (the converter numbers its source files
    from 0 in the order it read them) and a ``SourceUnitOrdinal`` of its own;
    a forest marked -1/-1 must be one the index files place in more than one
    file (``source_index.py``). A converted forest's ordinals say nothing about
    the vertical side's file range, so this replaces the inference from
    ``SourceFileOrdinal`` rather than adding to it.

    The ordinal check is a range check: it proves every single-file ordinal
    names one of the manifest's source files, not that it names the forest's own
    file. The manifest records its dependencies sorted by path, without the
    order the converter numbered them in, so the file an ordinal stands for is
    not recoverable from it.

    Memory: ``SourceIndex`` over the vertical side's index files, about 80 B per
    tree at its peak (``source_index.py``), and the sidecar's three columns.
    """
    manifest = Path(manifest)
    try:
        document = json.loads(manifest.read_text())
        dependencies = document["sources"]["dependencies"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise AssertionError(f"{manifest}: not a readable conversion manifest ({error})") from None
    sources = [
        Path(record["path"]).name
        for record in dependencies
        if MANIFEST_SOURCE_ROLE in record.get("roles", ())
    ]
    if not sources or len(set(sources)) != len(sources):
        raise AssertionError(
            f"{manifest}: {len(sources)} source dependencies, {len(set(sources))} distinct "
            f"names; the inventory needs at least one, each named once"
        )

    if str(CONVERTER_DIR) not in sys.path:
        sys.path.insert(0, str(CONVERTER_DIR))
    from source_index import SourceIndex

    index = SourceIndex.load(vertical_data / "forests.list", vertical_data / "locations.dat")
    listed = set(index.filenames.values())
    if set(sources) != listed:
        only_manifest = sorted(set(sources) - listed)
        only_index = sorted(listed - set(sources))
        raise AssertionError(
            f"{manifest}: its source dependencies are not the vertical side's tree files: "
            f"{len(only_manifest)} only in the manifest (e.g. {only_manifest[:3]}), "
            f"{len(only_index)} only in {vertical_data / 'locations.dat'} "
            f"(e.g. {only_index[:3]})"
        )

    with h5py.File(sidecar, "r") as handle:
        forest_ids = numpy.asarray(handle[FOREST_ID][()], dtype=numpy.int64)
        files = numpy.asarray(handle[SOURCE_FILE_ORDINAL][()], dtype=numpy.int64)
        units = numpy.asarray(handle[SOURCE_UNIT_ORDINAL][()], dtype=numpy.int64)
    spanning = files == -1
    if not numpy.array_equal(spanning, units == -1):
        row = int(numpy.flatnonzero(spanning != (units == -1))[0])
        raise AssertionError(
            f"{sidecar}: row {row} (ForestID {int(forest_ids[row])}) carries ordinals "
            f"{int(files[row])}/{int(units[row])}; a forest is in one file (both set) or "
            f"spans files (both -1)"
        )
    outside = ~spanning & ((files < 0) | (files >= len(sources)) | (units < 0))
    if outside.any():
        row = int(numpy.flatnonzero(outside)[0])
        raise AssertionError(
            f"{sidecar}: {int(outside.sum())} single-file row(s) outside the manifest's "
            f"{len(sources)} source files (e.g. ForestID {int(forest_ids[row])} with ordinals "
            f"{int(files[row])}/{int(units[row])})"
        )
    table = index.forest_table()
    multi_file = table.forest_ids[table.file_id == -1]
    unexplained = forest_ids[spanning][~numpy.isin(forest_ids[spanning], multi_file)]
    if unexplained.size:
        raise AssertionError(
            f"{sidecar}: {unexplained.size} forest(s) marked -1/-1 that the index files hold "
            f"in one file (e.g. ForestID {unexplained[:3].tolist()})"
        )
    log(
        f"  conversion inventory from {manifest}: {len(sources)} source files, exactly the tree "
        f"files {vertical_data / 'locations.dat'} lists; {int((~spanning).sum())} single-file "
        f"forests inside them, {int(spanning.sum())} -1/-1 forests of the "
        f"{multi_file.size} the index files place in several files"
    )


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


# --------------------------------------------------------------------------
# Identity-gate services (shared with tests/manual/test_snapshot_disabled_identity.py and
# tests/manual/test_reference_identity.py): reference resolution, cached worktrees, and the
# per-run output comparison
# --------------------------------------------------------------------------

#: Everything a worktree builds or reads, for the uncommitted-edit check.
RUNTIME_PATHS = ("src", "scripts", "models", "simulations", "Makefile")

#: The gates' worktree cache, under the repository's gitignored ``output/`` (created if absent):
#: the documented cache location and clean-up procedure assume it, so no fallback to the system
#: tmp directory.
WORKTREE_ROOT = REPO_ROOT / "output" / "snapshot-global-identity" / "worktrees"

GIT_TIMEOUT_S = 600

#: Datasets the contract names for byte equality, relative to ``RunProperties``.
NAMED_DATASETS = ("Parameters", "FieldMetadata", "EnabledModules", "EventContracts", "Redshifts")

#: ``RunProperties/Version`` attributes that necessarily differ between two builds.
VERSION_ATTRIBUTES = ("version", "git_commit", "git_branch", "git_date", "build_date")

#: ``RunProperties`` attribute that necessarily differs between two runs.
RUN_END_TIME = "RunEndTime"

#: Errors kept per comparison in the evidence and logged per failed leg; the count is exact.
ERROR_CAP = 200

#: ``dataset_delta(leaf, left, right)``: the label of a permitted difference between two datasets.
DatasetDelta = Callable[[str, numpy.ndarray, numpy.ndarray], "str | None"]

#: ``metadata_delta(rel, left, right, base, other, report)``: True when it judged the file itself.
MetadataDelta = Callable[..., bool]


def git(*args: str, check: bool = True, cwd: Path = REPO_ROOT) -> subprocess.CompletedProcess:
    """Run git in ``cwd`` (the main tree by default); a failure is an AssertionError."""
    completed = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=GIT_TIMEOUT_S
    )
    if check and completed.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {completed.stderr.strip()}")
    return completed


def resolve_reference(
    value: str,
    commit_checks: tuple[Callable[[str, str], None], ...] = (),
    repo: Path = REPO_ROOT,
) -> tuple[str, str]:
    """(reference, HEAD) once ``value`` names an ancestor of HEAD and every check passes.

    ``commit_checks`` are the caller's own tests of the pair (each called with the reference and
    HEAD hashes, raising AssertionError), run after the ancestry test.
    """
    completed = git("rev-parse", "--verify", f"{value}^{{commit}}", check=False, cwd=repo)
    if completed.returncode != 0:
        raise AssertionError(f"reference {value!r} is not a commit in this repository")
    reference, head = completed.stdout.strip(), git("rev-parse", "HEAD", cwd=repo).stdout.strip()
    if git("merge-base", "--is-ancestor", reference, head, check=False, cwd=repo).returncode != 0:
        raise AssertionError(f"reference {reference} is not an ancestor of HEAD {head}")
    for check in commit_checks:
        check(reference, head)
    return reference, head


def _nul_separated(completed: subprocess.CompletedProcess) -> list[str]:
    """The paths of a ``-z`` git listing, so a path holding whitespace stays one entry."""
    return [path for path in completed.stdout.split("\0") if path]


def runtime_tree_differences(
    paths: tuple[str, ...] = RUNTIME_PATHS,
    exempt: Callable[[str], bool] | None = None,
    repo: Path = REPO_ROOT,
) -> list[str]:
    """Uncommitted or untracked edits to anything the worktrees build and run from.

    The legs build HEAD, so such an edit would not be tested. Every edit under ``paths`` counts
    unless ``exempt(path)`` is true: a caller that tolerates some paths (the battery tolerates
    module test envelopes) says so itself.
    """
    changed = git("diff", "--name-only", "-z", "HEAD", "--", *paths, cwd=repo)
    untracked = git("ls-files", "-z", "--others", "--exclude-standard", "--", *paths, cwd=repo)
    found = _nul_separated(changed) + _nul_separated(untracked)
    return [path for path in found if exempt is None or not exempt(path)]


def worktree_env(worktree: Path, model: str, simulation: str) -> dict:
    """The gates' worktree environment, minus the caller's make state and extra build flags.

    ``make TEST_BUILD=yes tests-snapshot-global-identity`` would otherwise reach both worktree
    builds through MAKEFLAGS, and ``EXTRA_CFLAGS``/``EXTRA_LDFLAGS`` of the caller's own would
    reach them through the environment, so the evidence would describe a build other than the
    one claimed. A caller that gates a candidate flag adds it back explicitly.
    """
    env = ParityGate.worktree_env(worktree, model, simulation)
    for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "EXTRA_CFLAGS", "EXTRA_LDFLAGS"):
        env.pop(key, None)
    return env


#: The Makefile's build-mode markers, relative to a worktree, by the flag variable each records:
#: ``.last_compile_mode`` is a prerequisite of every object and ``.last_exec_mode`` of the link,
#: and each is rewritten only when its content changes, so a flag change rebuilds and relinks.
BUILD_MODE_MARKERS = {
    "EXTRA_CFLAGS": Path("build") / ".last_compile_mode",
    "EXTRA_LDFLAGS": Path("build") / ".last_exec_mode",
}


def assert_build_flags(worktree: Path, flags: dict[str, str]) -> dict[str, str]:
    """The worktree's last build recorded exactly ``flags`` (an absent variable is empty).

    Reads the Makefile's own markers, which also record ``CC``, ``USE-HDF5`` and ``USE-MPI``, and
    returns each marker's full content keyed by its path relative to the worktree, for the
    evidence.
    """
    contents = {}
    for variable, marker in BUILD_MODE_MARKERS.items():
        path = worktree / marker
        if not path.is_file():
            raise AssertionError(f"no build mode marker at {path}: the build did not complete")
        text = path.read_text()
        recorded = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
        # A marker written by a Makefile older than the variable's line records it as absent,
        # which means the build received no such flag.
        if recorded.get(variable, "") != flags.get(variable, ""):
            raise AssertionError(
                f"build mode marker {path} records {variable}={recorded.get(variable, '')!r}, "
                f"not {flags.get(variable, '')!r}; it reads {text.splitlines()}"
            )
        contents[marker.as_posix()] = text
    return contents


@contextlib.contextmanager
def worktree_root_lock(root: Path = WORKTREE_ROOT):
    """Hold an exclusive lock on the shared worktree root for a whole gate or battery run.

    The identity gate and the battery share ``root``, its cached worktrees and HEAD worktrees, and
    each repoints a worktree's ``output`` per run, so two runs at once would build and run in each
    other's checkouts. The lock is not waited for: a second run is refused, naming the root.
    """
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".lock").open("a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise AssertionError(
                f"another identity gate or battery is using {root}; wait for it"
            ) from None
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def assert_clean_checkout(path: Path, commit: str) -> None:
    """The worktree at ``path`` is at ``commit`` with no tracked edit."""
    head = git("rev-parse", "HEAD", cwd=path, check=False)
    dirty = git("diff", "--quiet", "HEAD", cwd=path, check=False).returncode
    if head.returncode != 0 or head.stdout.strip() != commit or dirty != 0:
        raise AssertionError(
            f"FATAL: cached worktree {path} is not a clean checkout of {commit} "
            f"(HEAD {head.stdout.strip() or 'unreadable'}, tracked edits: {dirty != 0}); it is not "
            "reused. Move it to cold storage, run `git worktree prune`, and re-run."
        )


def _add_missing_worktree(
    path: Path, commit: str, log_line: Callable[[str], None], repo: Path
) -> bool:
    """Add a detached worktree of ``repo`` at ``path`` on ``commit`` when nothing is there.

    Returns True when it created one. A dangling symlink counts as present, so it is refused by
    ``assert_own_worktree`` rather than written through.
    """
    if path.exists() or path.is_symlink():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    git("worktree", "add", "--detach", str(path), commit, cwd=repo)
    ParityGate.link_machine_local(path)
    log_line(f"  created worktree {path}")
    return True


def cached_worktree(
    commit: str,
    model: str,
    simulation: str,
    root: Path = WORKTREE_ROOT,
    log_line: Callable[[str], None] = log,
    repo: Path = REPO_ROOT,
) -> Path:
    """The worktree for one commit and pair, created only when missing and never removed."""
    path = root / f"{commit[:12]}__{model}__{simulation}"
    if not _add_missing_worktree(path, commit, log_line, repo):
        log_line(f"  reusing worktree {path}")
    assert_own_worktree(path, root, repo=repo)
    assert_clean_checkout(path, commit)
    return path


def assert_untracked_free(worktree: Path, paths: tuple[str, ...] = RUNTIME_PATHS) -> None:
    """The worktree holds no untracked, non-ignored file under ``paths``.

    The Makefile compiles every source it finds under ``src/`` and ``models/``, so such a file
    would be built into the executable although the checkout reads as clean.
    """
    listing = git("ls-files", "-z", "--others", "--exclude-standard", "--", *paths, cwd=worktree)
    found = _nul_separated(listing)
    if found:
        raise AssertionError(
            f"FATAL: worktree {worktree} holds untracked file(s) under {', '.join(paths)}: "
            f"{found}; they would be built or run, so the executable would not be its commit's. "
            "Move them to cold storage and re-run."
        )


#: ``_worktree_problem``'s finding for the main checkout, whose remedy is never to move it.
MAIN_CHECKOUT = "it is the main checkout, not a linked worktree"


def _worktree_problem(path: Path, root: Path, repo: Path) -> str | None:
    """Why ``path`` is not a linked worktree of ``repo`` under ``root``, or None when it is."""
    resolved = path.resolve()
    if path.is_symlink():
        return f"it is a symlink (to {resolved})"
    if not path.is_dir():
        return "it is not a directory"
    if not resolved.is_relative_to(root.resolve()):
        return f"it is not under the worktree root {root.resolve()}"
    listing = git("worktree", "list", "--porcelain", cwd=repo).stdout.splitlines()
    registered = {
        Path(line[len("worktree ") :]).resolve() for line in listing if line.startswith("worktree ")
    }
    if resolved not in registered:
        return f"`git worktree list` of {repo} does not name it"
    top = git("rev-parse", "--show-toplevel", cwd=path, check=False)
    if top.returncode != 0 or Path(top.stdout.strip()).resolve() != resolved:
        found = top.stdout.strip() if top.returncode == 0 else "unreadable"
        return f"its git top level is {found}, not itself"
    ours = (repo / git("rev-parse", "--git-common-dir", cwd=repo).stdout.strip()).resolve()
    common = git("rev-parse", "--git-common-dir", cwd=path, check=False)
    if common.returncode != 0 or (path / common.stdout.strip()).resolve() != ours:
        found = common.stdout.strip() if common.returncode == 0 else "unreadable"
        return f"its common git dir is {found}, not {ours}"
    git_dir = git("rev-parse", "--absolute-git-dir", cwd=path, check=False)
    if git_dir.returncode != 0 or Path(git_dir.stdout.strip()).resolve() == ours:
        return MAIN_CHECKOUT
    return None


def assert_own_worktree(path: Path, root: Path = WORKTREE_ROOT, repo: Path = REPO_ROOT) -> None:
    """``path`` is a linked worktree of ``repo`` under ``root``, before anything uses it.

    Checked in order: ``path`` is not a symlink and is a directory; it resolves under ``root``;
    ``git worktree list --porcelain`` of ``repo`` names it (both sides resolved); git run in it
    reports it as its own top level (git discovers upward, so a stray directory would act on an
    enclosing repository) with ``repo``'s common git dir; and its own git dir
    (``--absolute-git-dir``) is not that common dir, so it is a linked worktree and never the
    main checkout.
    """
    problem = _worktree_problem(path, root, repo)
    if problem is not None:
        remedy = (
            "Never move the main checkout: choose a worktree root that does not lead to it."
            if problem == MAIN_CHECKOUT
            else "Move it to cold storage, run `git worktree prune`, and re-run."
        )
        raise AssertionError(
            f"FATAL: worktree {path} is not a linked worktree of {repo} under {root}: {problem}; "
            f"it is not used. {remedy}"
        )


def head_worktree(
    head: str,
    model: str,
    simulation: str,
    root: Path = WORKTREE_ROOT,
    log_line: Callable[[str], None] = log,
    repo: Path = REPO_ROOT,
) -> Path:
    """The one reusable worktree for a pair, detached to ``head`` on every call.

    Unlike ``cached_worktree`` it is keyed by pair, not commit, so successive HEADs share it and
    commits do not accumulate worktrees; it is never removed. It must be a linked worktree of
    this repository under ``root``, and a tracked edit or an untracked source in it is refused,
    before the checkout and after it, rather than carried onto the new commit or built.
    """
    path = root / f"head__{model}__{simulation}"
    created = _add_missing_worktree(path, head, log_line, repo)
    assert_own_worktree(path, root, repo=repo)
    if not created:
        status = git("diff", "--quiet", "HEAD", cwd=path, check=False)
        if status.returncode != 0:
            reason = (
                "has tracked edits"
                if status.returncode == 1
                else f"is unreadable as a git worktree ({status.stderr.strip() or 'no message'})"
            )
            raise AssertionError(
                f"FATAL: HEAD worktree {path} {reason}; it is not reused. Move it to cold "
                "storage, run `git worktree prune`, and re-run."
            )
        assert_untracked_free(path)
        git("checkout", "--detach", head, cwd=path)
        log_line(f"  reusing worktree {path}, detached to {head}")
    assert_clean_checkout(path, head)
    assert_untracked_free(path)
    return path


@dataclass(frozen=True)
class OutputRun:
    """A finished run's output directory, the path prefixes its files may embed, and the
    property-metadata digest its build generated (``build/generated/property_hash.txt`` of
    the producing worktree), which its ``output_schema.json`` must carry as ``source_md5``.
    ``basename`` is the run's ``output_filename``, the stem of its master and partition files."""

    directory: Path
    prefixes: tuple[str, ...]
    source_md5: str | None = None
    basename: str = "model"

    def normalized(self, text: str) -> str:
        for prefix in sorted(self.prefixes, key=len, reverse=True):
            text = text.replace(prefix, "<PREFIX>")
        return text

    def embeds_prefix(self, text: str) -> bool:
        return any(prefix in text for prefix in self.prefixes)


@dataclass
class Report:
    """The outcome of one comparison: failures, named permitted differences and counts."""

    errors: list[str] = field(default_factory=list)
    permitted: list[str] = field(default_factory=list)
    datasets_compared: list[str] = field(default_factory=list)
    files: int = 0
    links: int = 0
    snapshots: int = 0
    galaxies: int = 0
    fields: int = 0
    galaxy_differences: int = 0

    def error(self, message: str) -> None:
        self.errors.append(message)

    def allow(self, message: str) -> None:
        if message not in self.permitted:
            self.permitted.append(message)

    def summary(self) -> dict:
        return {
            "errors": len(self.errors),
            "permitted_differences": sorted(self.permitted),
            "named_datasets_compared": sorted(set(self.datasets_compared)),
            "files": self.files,
            "master_links": self.links,
            "output_snapshots": self.snapshots,
            "galaxies": self.galaxies,
            "fields": self.fields,
            "galaxy_differences": self.galaxy_differences,
        }


def relative_files(root: Path) -> list[str]:
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


def attribute_signature(value, stored=None) -> tuple:
    """(dtype, shape, bytes) of an attribute; strings carry their HDF5 encoding.

    With ``stored`` (the attribute's HDF5 id) the dtype and shape are the stored ones: a scalar
    fixed-length string otherwise comes back sized to its content.
    """
    array = numpy.asarray(value)
    shape = tuple(stored.shape) if stored is not None else array.shape
    info = h5py.check_string_dtype(stored.dtype) if stored is not None else None
    encoding = f":{info.encoding}" if info is not None else ""
    if array.dtype.kind == "O":
        items = [item.encode() if isinstance(item, str) else bytes(item) for item in array.ravel()]
        return ("O" + encoding, shape, b"\0".join(items))
    dtype = stored.dtype.str if stored is not None else array.dtype.str
    return (dtype + encoding, shape, array.tobytes())


def attribute_text(signature: tuple) -> str:
    """The attribute's text when it is a string, else empty (for path-prefix reasoning)."""
    dtype, _shape, payload = signature
    if dtype.startswith(("O", "|S", "<U")):
        return payload.decode(errors="replace").rstrip("\0")
    return ""


def dtype_signature(dtype: numpy.dtype):
    """A dtype's full layout: for a compound, its size and each field's name, base type, shape
    and offset, so field-by-field byte equality means the same record."""
    if not dtype.names:
        return dtype.str
    fields = []
    for name in dtype.names:
        item, offset = dtype.fields[name][:2]
        base, shape = item.subdtype if item.subdtype is not None else (item, ())
        fields.append((name, base.str, tuple(shape), offset))
    return (dtype.itemsize, tuple(fields))


def canonical_bytes(array: numpy.ndarray) -> bytes:
    """Dataset content as bytes, field by field so compound padding is never compared."""
    if array.dtype.names:
        return b"".join(canonical_bytes(array[name]) for name in array.dtype.names)
    if array.dtype.kind == "O":
        return b"\0".join(
            item.encode() if isinstance(item, str) else bytes(item) for item in array.ravel()
        )
    return numpy.ascontiguousarray(array).tobytes()


def objects_of(handle: h5py.File) -> dict[str, h5py.HLObject]:
    found: dict[str, h5py.HLObject] = {"/": handle["/"]}
    handle.visititems(lambda name, obj: found.__setitem__(name, obj))
    return found


def links_of(handle: h5py.File) -> dict[str, tuple]:
    """Every link by name: its type, and for soft and external links its target."""
    found: dict[str, tuple] = {}

    def visit(name, link):
        found[name] = (
            type(link).__name__,
            getattr(link, "filename", ""),
            getattr(link, "path", ""),
        )

    handle.visititems_links(visit)
    return found


def permitted_attribute(path: str, name: str) -> str | None:
    """The label of an attribute that may differ in value (never in dtype or shape), or None."""
    if path == "RunProperties/Version" and name in VERSION_ATTRIBUTES:
        return f"RunProperties/Version@{name}"
    if path == "RunProperties" and name == RUN_END_TIME:
        return f"RunProperties@{RUN_END_TIME}"
    return None


def compare_attributes(where: str, path: str, a, b, base: OutputRun, other: OutputRun, report):
    names_a, names_b = set(a.attrs), set(b.attrs)
    if names_a != names_b:
        report.error(
            f"{where}:{path}: attribute names differ (only baseline {sorted(names_a - names_b)}, "
            f"only candidate {sorted(names_b - names_a)})"
        )
    for name in sorted(names_a & names_b):
        left = attribute_signature(a.attrs[name], a.attrs.get_id(name))
        right = attribute_signature(b.attrs[name], b.attrs.get_id(name))
        if left == right:
            continue
        same_type = left[:2] == right[:2]
        label = permitted_attribute(path, name)
        if label is not None:
            if same_type:
                report.allow(label)
            else:
                report.error(
                    f"{where}:{path}@{name}: permitted attribute changed dtype or shape "
                    f"({left[:2]} != {right[:2]}); only its value may differ"
                )
            continue
        text_a, text_b = attribute_text(left), attribute_text(right)
        if (
            path == "RunProperties"
            and same_type
            and text_a
            and text_b
            and base.embeds_prefix(text_a)
            and other.embeds_prefix(text_b)
            and base.normalized(text_a) == other.normalized(text_b)
        ):
            report.allow(f"path-valued attribute {path}@{name} (path prefix only)")
            continue
        report.error(
            f"{where}:{path}@{name}: {left[:2]} {left[2][:60]!r} != {right[:2]} {right[2][:60]!r}"
        )


def compare_hdf5_file(
    rel: str,
    base: OutputRun,
    other: OutputRun,
    report: Report,
    dataset_delta: DatasetDelta | None = None,
    strict_galaxies: bool = False,
) -> None:
    """Links, objects, attributes and dataset bytes of one HDF5 file pair.

    ``dataset_delta(leaf, left, right)`` names a permitted difference between two non-Galaxies
    datasets (``leaf`` is the path without any ``RunProperties/`` prefix), or returns None.
    ``Galaxies`` datasets are compared per ID by ``compare_galaxies``; with ``strict_galaxies``
    their bytes must also be equal here, so row order inside a partition is checked too. Dataset
    creation properties (chunking, compression, fill value) are not compared.
    """
    with h5py.File(base.directory / rel, "r") as fa, h5py.File(other.directory / rel, "r") as fb:
        links_a, links_b = links_of(fa), links_of(fb)
        report.links += sum(1 for kind, _, _ in links_a.values() if kind == "ExternalLink")
        for name in sorted(set(links_a) | set(links_b)):
            if links_a.get(name) != links_b.get(name):
                report.error(
                    f"{rel}:{name}: external link or link type differs "
                    f"({links_a.get(name)} != {links_b.get(name)})"
                )
        objects_a, objects_b = objects_of(fa), objects_of(fb)
        if set(objects_a) != set(objects_b):
            report.error(
                f"{rel}: HDF5 objects differ (only baseline "
                f"{sorted(set(objects_a) - set(objects_b))}, only candidate "
                f"{sorted(set(objects_b) - set(objects_a))})"
            )
        for path in sorted(set(objects_a) & set(objects_b)):
            left, right = objects_a[path], objects_b[path]
            if isinstance(left, h5py.Dataset) != isinstance(right, h5py.Dataset):
                report.error(f"{rel}:{path}: one side is a group, the other a dataset")
                continue
            compare_attributes(rel, path, left, right, base, other, report)
            if not isinstance(left, h5py.Dataset):
                continue
            if dtype_signature(left.dtype) != dtype_signature(right.dtype):
                report.error(f"{rel}:{path}: dataset dtype differs")
            elif left.shape != right.shape:
                report.error(f"{rel}:{path}: shape {left.shape} != {right.shape}")
            elif path.endswith("/Galaxies"):  # compared per ID; strictly, row order too
                if strict_galaxies and canonical_bytes(left[()]) != canonical_bytes(right[()]):
                    report.error(f"{rel}:{path}: Galaxies bytes differ (row order or content)")
            else:
                leaf = path.removeprefix("RunProperties/")
                if path.startswith("RunProperties/") and leaf in NAMED_DATASETS:
                    report.datasets_compared.append(leaf)
                if canonical_bytes(left[()]) != canonical_bytes(right[()]):
                    label = dataset_delta(leaf, left[()], right[()]) if dataset_delta else None
                    if label is not None:
                        report.allow(label)
                    else:
                        report.error(f"{rel}:{path}: dataset bytes differ")


#: The comparator's ``--compare-created`` pairing: tree and created rows (``UniqueGalaxyID < 0``)
#: alike by id. Created ids are driver-specific, and both runs of every comparison here come from
#: one driver and layout, the case the comparator documents for it.
CREATED_BY_ID = comparator.Matching("id", "id")


def compare_galaxies(base: OutputRun, other: OutputRun, report: Report) -> None:
    """Per-ID byte identity of tree and created rows through the cross-format comparator.

    Its report lines are errors; ``report.galaxies`` counts the rows it actually compared.
    """
    left = comparator.scan_run(str(base.directory / base.basename))
    right = comparator.scan_run(str(other.directory / other.basename))
    labels = ("baseline", "candidate")

    def captured(call) -> tuple:
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            value = call()
        return value, " | ".join(line.strip() for line in buffer.getvalue().splitlines())

    for label, index in zip(labels, (left, right)):
        failures, text = captured(
            lambda i=index, n=label: comparator.report_run_duplicates(n, i, ERROR_CAP)
        )
        if failures:
            report.error(f"galaxies: {text}")
            report.galaxy_differences += failures
    if report.galaxy_differences:
        return
    if left.signature != right.signature:
        report.error("galaxies: Galaxies record schema differs")
        return
    if left.snapshots != right.snapshots:
        report.error(
            f"galaxies: output snapshots differ (only baseline "
            f"{sorted(left.snapshots - right.snapshots)}, only candidate "
            f"{sorted(right.snapshots - left.snapshots)})"
        )
    shared = sorted(left.snapshots & right.snapshots)
    report.snapshots, report.fields = len(shared), len(left.dtype.names)
    for snap in shared:
        records_left = comparator.read_snapshot(left, snap)
        records_right = comparator.read_snapshot(right, snap)
        result, text = captured(
            lambda s=snap, a=records_left, b=records_right: comparator.compare_snapshot(
                s, a, b, labels, ERROR_CAP, CREATED_BY_ID
            )
        )
        if result.failures:
            report.error(f"galaxies: {text}")
            report.galaxy_differences += result.failures
        else:
            report.galaxies += result.tree_rows + result.created_compared


def compare_metadata_file(
    rel: str,
    base: OutputRun,
    other: OutputRun,
    report: Report,
    metadata_delta: MetadataDelta | None = None,
):
    """One metadata file pair: identical, a named provenance or path-prefix difference, or an error.

    ``metadata_delta(rel, left, right, base, other, report)`` may judge a file itself (recording
    its own allowance or error) and return True, ahead of the identical-bytes and path-prefix
    rules.
    """
    left, right = (base.directory / rel).read_bytes(), (other.directory / rel).read_bytes()
    if rel == "metadata/version_info.json":
        if left != right:
            report.allow(rel)
        for label, raw in (("baseline", left), ("candidate", right)):
            if not isinstance(json.loads(raw), dict):
                report.error(f"{rel}: {label} is not a JSON object")
        return
    if metadata_delta is not None and metadata_delta(rel, left, right, base, other, report):
        return
    if left == right:
        return
    text_left, text_right = left.decode(errors="replace"), right.decode(errors="replace")
    if (
        base.embeds_prefix(text_left)
        and other.embeds_prefix(text_right)
        and base.normalized(text_left) == other.normalized(text_right)
    ):
        report.allow(f"{rel}: path prefix only")
    else:
        report.error(f"{rel}: content differs beyond the path prefix")


def compare_outputs(
    base: OutputRun,
    other: OutputRun,
    dataset_delta: DatasetDelta | None = None,
    metadata_delta: MetadataDelta | None = None,
    check_shared: Callable[[list[str], Report], None] | None = None,
    strict_galaxies: bool = False,
) -> Report:
    """Compare a candidate run with the baseline; every failure is in ``Report.errors``.

    With no hook, only the named provenance attributes and the path prefixes may differ.
    ``check_shared`` sees the files both runs wrote, right after the file-set comparison.
    ``strict_galaxies`` also requires byte-equal ``Galaxies`` datasets (see
    ``compare_hdf5_file``). HDF5 creation properties (chunking, compression, fill value) are not
    compared.
    """
    report = Report()
    files_left, files_right = relative_files(base.directory), relative_files(other.directory)
    if files_left != files_right:
        report.error(
            f"file sets differ (only baseline {sorted(set(files_left) - set(files_right))}, "
            f"only candidate {sorted(set(files_right) - set(files_left))})"
        )
    shared = sorted(set(files_left) & set(files_right))
    report.files = len(shared)
    if check_shared is not None:
        check_shared(shared, report)
    for rel in shared:
        if rel.endswith(".hdf5"):
            compare_hdf5_file(rel, base, other, report, dataset_delta, strict_galaxies)
        elif rel.startswith("metadata/"):
            compare_metadata_file(rel, base, other, report, metadata_delta)
        elif (base.directory / rel).read_bytes() != (other.directory / rel).read_bytes():
            report.error(f"{rel}: content differs")
    try:
        compare_galaxies(base, other, report)
    except comparator.ComparisonError as error:
        report.error(f"galaxy comparison could not read an output: {error}")
    if report.galaxies <= 0 and not report.errors:
        report.error("no galaxies compared")
    return report


class Tee(io.TextIOBase):
    """Write to every stream at once (the console, a log file, a capture buffer)."""

    def __init__(self, *streams):
        self.streams = streams

    def write(self, text: str) -> int:
        for stream in self.streams:
            stream.write(text)
            stream.flush()
        return len(text)

    def flush(self) -> None:
        for stream in self.streams:
            stream.flush()


def run_manual_gate(stages: list[Callable], title: str, log_path: Path, target: str) -> int:
    """Run a manual gate's stages with the console also written to ``log_path``.

    Returns 0 only when every stage passed: any FAIL, ERROR, SKIP or NA marker fails (a WARN is
    surfaced, not fatal). There is no ``abort_on_failure``: once the builds succeed the legs are
    independent, each checks its own prerequisites, and the verdict stage names every leg's
    outcome; a failed resolution or build leaves the legs nothing to run.
    """
    log_path.parent.mkdir(parents=True, exist_ok=True)
    console = sys.stdout
    with log_path.open("w") as handle:
        buffer = io.StringIO()
        sys.stdout = Tee(console, handle, buffer)
        try:
            status = run_test_suite(stages, title)
            if re.search(r"^MIMIC_RESULT: (SKIP|NA)\b", buffer.getvalue(), flags=re.MULTILINE):
                print("FAIL: a stage was skipped or not applicable; every stage must run")
                status = 1
            if "MIMIC_RESULT: WARN" in buffer.getvalue():
                print("WARN: see the MIMIC_RESULT: WARN line(s) above (not a failure)")
            verdict = "PASS" if status == 0 else "FAIL"
            print(f"{verdict}: {target} (log: {log_path})")
        finally:
            sys.stdout = console
    return status


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
            remove_tree(self.scratch)

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
        """Run a command with its output captured to a file (see the module-level run_logged)."""
        run_logged(cmd, cwd, env, log_path, what, timeout, show_tail)

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
        """The vertical run file the gate executes: committed, its range override, or its
        vertical dataset variant when the package names a vertical dataset."""
        if self.package.vertical_dataset is not None:
            return vertical_dataset_variant(run_file, scratch, self.package.vertical_dataset)
        if self.package.override_vertical_range:
            return range_override_variant(run_file, scratch, self.package.file_range)
        return run_file

    def worktree_dataset(self, worktree: Path) -> tuple[Path, Path]:
        """The named horizontal dataset as a worktree's run reads it: a snapshot list inside
        the repository is the worktree's own copy, as every other input of a HEAD-built run is."""
        directory, snapshot_list = (Path(path) for path in self.package.horizontal_dataset)
        if snapshot_list.is_relative_to(REPO_ROOT):
            snapshot_list = worktree / snapshot_list.relative_to(REPO_ROOT)
        return directory, snapshot_list

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
        if pkg.vertical_dataset is not None:
            held = tuple(int(value) for value in pkg.vertical_dataset[1:])
            if tuple(pkg.file_range) != held:
                raise AssertionError(
                    f"file_range {pkg.file_range} is not the range {held} the named vertical "
                    f"dataset {pkg.vertical_dataset[0]} holds"
                )
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
        """The snapshot files the horizontal dataset's snapshot list names."""
        alist = self.package.horizontal_snapshot_list()
        if not alist.is_file():
            raise AssertionError(f"horizontal snapshot list is missing: {alist}")
        return [f"snapshot_{index:03d}.h5" for index in range(len(snapshot_list_entries(alist)))]

    def stage_preconditions(self) -> None:
        """Both datasets resolve, with named paths on failure; HEAD is captured once."""
        pkg = self.package
        banner(f"Stage 1: preconditions ({pkg.horizontal}; {pkg.evidence})")
        if pkg.vertical_dataset is not None and pkg.manifest is not None:
            raise AssertionError(
                f"the package pins both a conversion manifest ({pkg.manifest}) and a vertical "
                f"dataset ({pkg.vertical_dataset[0]}): the first proves the inventory of a "
                f"conversion from the vertical side, the second of one from the horizontal side"
            )

        inventory = self.read_vertical_inventory()
        if pkg.vertical_dataset is None:
            vertical_data = assert_dataset_present(pkg.vertical, inventory["files"])
            named = ""
        else:
            vertical_data = assert_dataset_present(
                pkg.vertical, (*inventory["files"], VERTICAL_MANIFEST), pkg.vertical_snapshots()
            )
            named = f"the named dataset (with its {VERTICAL_MANIFEST}) "
        log(
            f"  {pkg.vertical}: {named}{vertical_data} -> {vertical_data.resolve()} "
            f"({len(inventory['files'])} source file(s): {', '.join(inventory['files'])})"
        )
        committed = ", ".join(
            f"{model} {first}-{last}"
            for model, (first, last) in inventory["committed_ranges"].items()
        )
        if pkg.vertical_dataset is not None:
            how = ", set with the named dataset in a scratch copy"
        elif pkg.override_vertical_range:
            how = ", overridden in a scratch copy"
        else:
            how = ""
        log(
            f"  file range: {pkg.file_range[0]}-{pkg.file_range[1]} on both sides (committed "
            f"vertical run file range {committed}{how})"
        )

        required = self.expected_snapshot_files()
        horizontal_data = assert_dataset_present(
            pkg.horizontal, (*required, FORESTS_FILE), pkg.horizontal_snapshots()
        )
        named = "the named dataset " if pkg.horizontal_dataset is not None else ""
        log(
            f"  {pkg.horizontal}: {named}{horizontal_data} -> {horizontal_data.resolve()} "
            f"({len(required)} snapshot files from {pkg.horizontal_snapshot_list()}, plus "
            f"{FORESTS_FILE})"
        )
        if pkg.manifest is not None:
            if not Path(pkg.manifest).is_file():
                raise AssertionError(f"the conversion manifest {pkg.manifest} is not a file")
            log(f"  conversion manifest: {pkg.manifest}")

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

        directory = pkg.horizontal_snapshots()
        names = sorted(
            entry.name for entry in directory.iterdir() if SNAPSHOT_FILE_RE.match(entry.name)
        )
        expected_names = self.expected_snapshot_files()
        if names != expected_names:
            raise AssertionError(
                f"{directory}: snapshot files {names[:3]}...{names[-3:]} ({len(names)}) are not "
                f"exactly the {len(expected_names)} the a_list names"
            )

        header_format = (
            pkg.source_format if pkg.header_source_format is None else pkg.header_source_format
        )
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
                    "source_format": (decoded(header["source_format"]), header_format),
                    "column_mapping_sha256": (
                        decoded(header["column_mapping_sha256"]),
                        pkg.column_mapping_sha256,
                    ),
                }
                for attribute, (found, expected) in checks.items():
                    if found != expected:
                        raise AssertionError(
                            f"{path}: header {attribute} is {found!r}, expected {expected!r}; "
                            f"this is not the conversion the gate certifies"
                        )
                n_halos = int(header["n_halos"])
                forests_total.add(int(header["n_forests_total"]))
                descendant_snapshot = numpy.asarray(handle["halos/DescendantSnapshot"][()])
            halos += n_halos
            populated += int(n_halos > 0)
            if descendant_snapshot.shape != (n_halos,):
                raise AssertionError(
                    f"{path}: DescendantSnapshot has shape {descendant_snapshot.shape}, "
                    f"header n_halos is {n_halos}"
                )
            spans = descendant_snapshot[descendant_snapshot >= 0].astype(numpy.int64) - snapshot
            gapped += int((spans > 1).sum())
            if spans.size:
                max_span = max(max_span, int(spans.max()))

        log(
            f"  {len(names)} version {pkg.format_version} files ({populated} populated) "
            f"from {header_format}, column_mapping_sha256 {pkg.column_mapping_sha256}, "
            f"links_adjacent {pkg.links_adjacent}"
        )
        if len(forests_total) != 1:
            raise AssertionError(
                f"{directory}: header n_forests_total varies across files: {forests_total}"
            )
        forests = forests_total.pop()
        log(f"  {halos} halos, {gapped} gapped descendant links, longest span {max_span}")
        measured = (halos, forests, gapped, max_span)
        expected = (pkg.halos, pkg.forests, pkg.gapped_descendants, pkg.max_descendant_span)
        what = "(halos, forests, gapped descendant links, longest span)"
        if measured != expected:
            raise AssertionError(
                f"{directory}: {what} is {measured}, expected {expected} for the pinned "
                f"conversion of files {pkg.file_range[0]}-{pkg.file_range[1]}"
            )

        with h5py.File(directory / FORESTS_FILE, "r") as handle:
            rows = numpy.asarray(handle[SOURCE_FILE_ORDINAL][()])
        if rows.size != pkg.forests:
            raise AssertionError(
                f"{directory / FORESTS_FILE}: {rows.size} forests, expected {pkg.forests}"
            )

        if pkg.vertical_dataset is not None:
            # The ordinals name the files the horizontal dataset was converted
            # from, not the vertical files converted from it: the vertical
            # dataset's own manifest proves what those hold.
            self._prove_inventory_from_manifest(directory, forests, halos)
            self.done.add("dataset")
            return

        if pkg.manifest is not None:
            if pkg.source_format != "consistent_trees_ascii":
                raise AssertionError(
                    f"the manifest-proved inventory reads the Consistent-Trees index files; "
                    f"{pkg.vertical} reads {pkg.source_format}"
                )
            assert_manifest_inventory(
                pkg.manifest, directory / FORESTS_FILE, package_path(pkg.vertical, "snapshots")
            )
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

    def _prove_inventory_from_manifest(self, directory: Path, forests: int, halos: int) -> None:
        """Prove the vertical side reads exactly the files converted from this dataset.

        The vertical dataset's ``manifest.json`` must record the conversion of exactly
        this horizontal dataset (the SHA-256 of its ``forests.h5`` ForestID column, its
        column mapping, forest total and halo total) into exactly the files the vertical
        side reads, tiling forests 0..n-1 in ForestIndex order. Anything else is an
        AssertionError naming the manifest.

        Args:
            directory: The horizontal dataset's snapshot directory, holding ``forests.h5``.
            forests: The forest total the dataset's snapshot headers agree on.
            halos: The halo total summed over the dataset's snapshot files.
        """
        pkg = self.package
        inventory = self.vertical_inventory
        manifest = pkg.vertical_snapshots() / VERTICAL_MANIFEST
        identity = ("forest_id_sha256", "column_mapping_sha256", "n_forests_total")
        try:
            document = json.loads(manifest.read_text())
            found_format = (document["format"], document["version"])
            recorded = {name: document["dataset"][name] for name in (*identity, "total_halos")}
            entries = [
                (
                    str(entry["name"]),
                    int(entry["first_forest"]),
                    int(entry["last_forest"]),
                    int(entry["n_halos"]),
                )
                for entry in document["files"]
            ]
            planned_files = int(document["plan"]["n_files"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise AssertionError(
                f"{manifest}: not a readable vertical conversion manifest ({error})"
            ) from None
        if found_format != VERTICAL_MANIFEST_FORMAT:
            raise AssertionError(
                f"{manifest}: format {found_format}, expected {VERTICAL_MANIFEST_FORMAT}"
            )

        digest = hashlib.sha256()
        with h5py.File(directory / FORESTS_FILE, "r") as handle:
            column = handle[FOREST_ID]
            for start in range(0, int(column.shape[0]), FOREST_ID_BLOCK_ROWS):
                block = column[start : start + FOREST_ID_BLOCK_ROWS]
                digest.update(numpy.ascontiguousarray(block, dtype="<i8").tobytes())
        horizontal_identity = {
            "forest_id_sha256": digest.hexdigest(),
            "column_mapping_sha256": pkg.column_mapping_sha256,
            "n_forests_total": forests,
            "total_halos": halos,
        }
        differing = [
            name for name in horizontal_identity if recorded[name] != horizontal_identity[name]
        ]
        if differing:
            raise AssertionError(
                f"{manifest}: records the conversion of another dataset: "
                + "; ".join(
                    f"{name} {recorded[name]!r}, the horizontal dataset's is "
                    f"{horizontal_identity[name]!r}"
                    for name in differing
                )
            )

        names = [entry[0] for entry in entries]
        if names != inventory["files"] or len(entries) != planned_files:
            raise AssertionError(
                f"{manifest}: records {len(names)} complete file(s) of a {planned_files}-file "
                f"plan ({', '.join(names[:3])}{', ...' if len(names) > 3 else ''}); the "
                f"vertical side reads exactly {len(inventory['files'])}, "
                f"{inventory['files'][0]}..{inventory['files'][-1]}"
            )
        starts = [entry[1] for entry in entries]
        stops = [entry[2] + 1 for entry in entries]
        if (
            starts != [0, *stops[:-1]]
            or stops[-1] != forests
            or sum(entry[3] for entry in entries) != halos
        ):
            raise AssertionError(
                f"{manifest}: its files' forest ranges and halo counts do not hold the "
                f"dataset's {forests} forests and {halos} halos in ForestIndex order"
            )
        log(
            f"  conversion inventory from {manifest}: the conversion of this dataset "
            f"({', '.join(horizontal_identity)} equal), into exactly the vertical side's "
            f"{len(names)} files {names[0]}..{names[-1]}, holding forests 0..{forests - 1} "
            f"in order"
        )

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
            # The horizontal file is diffed against the vertical file the
            # vertical side runs, except that a named vertical dataset's keys
            # are the vertical side's alone: its variant is verified as the
            # committed file plus exactly those keys, and the horizontal file
            # is diffed against the committed one.
            base, what = effective, "the effective vertical run file"
            if pkg.vertical_dataset is not None:
                base, what = vertical_path, f"the committed {vertical_path.name}"
                log(
                    f"  {model}: vertical side runs {effective.name} = {vertical_path.name} "
                    f"plus exactly {list(VERTICAL_DATASET_KEYS)} set to {pkg.vertical_dataset}"
                )
            elif effective != vertical_path:
                log(
                    f"  {model}: vertical side runs {effective.name} = {vertical_path.name} "
                    f"with only {list(RANGE_KEYS)} set to {pkg.file_range}"
                )
            assert_horizontal_run_file_diff(base, horizontal_path, pkg.horizontal)
            log(
                f"  {model}: {horizontal_path.name} differs from {what} in exactly "
                f"{list(AUTHORIZED_KEY_CHANGES)}"
            )
            if pkg.horizontal_dataset is not None:
                scratch = self.scratch_dir("run-files", "check", model)
                variant = dataset_variant(horizontal_path, scratch, pkg.horizontal_dataset)
                assert_horizontal_run_file_diff(
                    base, variant, pkg.horizontal, pkg.horizontal_dataset
                )
                log(
                    f"  {model}: its dataset variant differs from {what} in exactly "
                    f"{[*AUTHORIZED_KEY_CHANGES, *DATASET_OVERRIDE_KEYS]}"
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
        build_pair(worktree, model, simulation, env, logs, key, pkg.build_timeout_s)
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

        output_root = self.scratch_dir("runs", key)
        point_output_at(worktree, output_root)

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
        elif self.package.horizontal_dataset is not None:
            run_file = dataset_variant(run_file, scratch, self.worktree_dataset(worktree))
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
        main_directory = pkg.vertical_snapshots()
        if directory.resolve() != main_directory.resolve():
            raise AssertionError(
                f"{vertical_run.master}: the vertical run read {directory} "
                f"(-> {directory.resolve()}), not the dataset stage 1 checked "
                f"({main_directory.resolve()})"
            )
        _, strings, integers, _, _ = horizontal_run.properties()
        horizontal_range = (integers.get("FirstFile"), integers.get("LastFile"))
        expected_range, source = tuple(pkg.file_range), "the package pins"
        if pkg.horizontal_dataset is not None:
            # The named dataset's files come from its snapshot list; the run
            # records the range its package declares.
            info = yaml.safe_load(package_path(pkg.horizontal, "simulation_info.yaml").read_text())
            expected_range = (int(info["input"]["first_file"]), int(info["input"]["last_file"]))
            source = f"the {pkg.horizontal} package declares"
            read = horizontal_run.worktree / strings.get("SimulationDir", "")
            if read.resolve() != pkg.horizontal_snapshots().resolve():
                raise AssertionError(
                    f"{horizontal_run.master}: the horizontal run read {read.resolve()}, not the "
                    f"named dataset {pkg.horizontal_snapshots().resolve()}"
                )
            snapshot_list = self.worktree_dataset(horizontal_run.worktree)[1]
            if strings.get("FileWithSnapList") != str(snapshot_list):
                raise AssertionError(
                    f"{horizontal_run.master}: the horizontal run read the snapshot list "
                    f"{strings.get('FileWithSnapList')!r}, not its worktree's "
                    f"{str(snapshot_list)!r}"
                )
        if horizontal_range != expected_range:
            raise AssertionError(
                f"{horizontal_run.master}: the horizontal run recorded (FirstFile, LastFile) "
                f"{horizontal_range}, {source} {expected_range}"
            )
        log(
            f"  vertical run read {inventory['files'][0]}..{inventory['files'][-1]} from "
            f"{directory.resolve()}; the horizontal run read "
            f"{pkg.horizontal_snapshots().resolve()} and records FirstFile-LastFile "
            f"{expected_range[0]}-{expected_range[1]}"
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
        if status == 1:
            raise AssertionError(
                f"{leg}: cross-format identity comparison failed (exit 1); every "
                f"divergence is listed by snapshot, field and example id in the output above"
            )
        if status != 0:
            raise AssertionError(
                f"{leg}: the comparator could not compare the runs (exit {status}: 2 is an input "
                f"error, 3 a failure of the comparator itself); its error is in the output above"
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
