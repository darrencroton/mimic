#!/usr/bin/env python3
"""
Disabled-mode identity of the snapshot-global feature against its pre-feature reference.

Not an auto-discovered test: scripts/generate_test_registry.py globs only tests/unit,
tests/integration and tests/scientific, so this file under tests/manual/ never enters
``make tests``. Invoke it explicitly::

    make tests-snapshot-global-identity [REFERENCE_COMMIT=<hash>]

It proves that the snapshot-global feature, with ``modules.post_snapshot`` absent or an
explicit empty list, changes nothing a pre-feature build produces. Three builds' worth of
evidence per leg, on the committed fixtures only:

  baseline         the reference commit, run file WITHOUT ``modules.post_snapshot`` (the
                   reference rejects that key as unknown)
  feature-absent   HEAD, the very same run file
  feature-empty    HEAD, the same run file plus ``post_snapshot: []`` under ``modules:``

Both feature legs are compared with the baseline output. The matrix is
{halos-only, sage16} x {committed vertical mini-Millennium trees, committed version 2
horizontal fixture, committed gapped version 3 horizontal fixture} x {fixed, dynamic}: twelve
legs, each scheme against its own baseline. The SAGE pipeline and parameters are the
shipped ``models/sage16/input/sage16_mini-millennium.yaml`` (metal enrichment included). The
harness substitutes, and asserts it substituted, only: the model name, the simulation name and
config, ``output.output_directory``, ``output.snapshot_list`` (``[]``, all snapshots), and the
added ``MaxDynamicSubsteps`` and ``TimestepScheme`` keys; the ``modules`` mapping and every
parameter stay identical to the shipped file. Halos-only replaces only its ``modules`` mapping
with an empty pipeline.

Reference commit
----------------
``REFERENCE_COMMIT=<hash>`` or, without it, derived as the parent of the first commit,
walking ``git log --first-parent --reverse`` from the commit that last changed the plan, that
changes any path outside the planning surface (the plan, the development pathway, the two
planning records and ``docs/dev/snapshot-global-checks/``). No hash is embedded. Either way the
commit must be an ancestor of HEAD, contain no feature symbol under src/, scripts/ and
models/, and contain every simulation package and run-file selector the legs use. An explicit
value must equal the derived one or differ from it only in planning-surface paths.

What is compared
----------------
For each feature leg against the baseline, by an independent comparator in this file (the
existing cross-format comparator is neither modified nor weakened; no tolerance exists
anywhere here):

  * the same ID set and no duplicate ID at every output snapshot, and every Galaxies field
    byte-identical per ID (raw bytes, so NaN payloads and signed zeros count);
  * the same set of files, the same HDF5 objects, and the same attributes everywhere;
  * byte-equal ``RunProperties/Parameters``, ``FieldMetadata``, ``EnabledModules``,
    ``EventContracts`` and ``Redshifts`` datasets where present (field by field, so compound
    padding is not compared) and, for every other dataset, the same bytes;
  * a byte-identical ``metadata/output_schema.json``.

The only permitted differences, each named in the evidence when it occurs, are the provenance
records two builds and two runs necessarily differ in: the ``RunProperties/Version`` attributes
``version``, ``git_commit``, ``git_branch``, ``git_date`` and ``build_date``; the
``RunProperties`` attribute ``RunEndTime``; ``metadata/version_info.json``; path-valued string
attributes and copied ``metadata/`` files whose content differs only by the reference-worktree
or scratch-output path prefix; and, in the feature-empty leg only, the single added
``post_snapshot: []`` line of the copied run YAML.

Self-check
----------
``test_comparator_rejects_mutations`` copies real output and proves the comparator fails on a
dropped or duplicated ID, a perturbed field, a galaxy in the wrong snapshot, payloads swapped
between two IDs, a missing or extra dataset, altered datasets and attributes, and an altered
output schema, while accepting the permitted differences.

Scratch material (worktrees, run files, outputs, logs, evidence.json) lives under
``archive/snapshot-global-identity/<stamp>/`` and is never deleted. Needs the local Python
environment (h5py, numpy, PyYAML), git, make and a C compiler. ``--fixtures``, ``--models``
and ``--schemes`` run a development subset; a subset run reports SKIP for the stages it cannot
complete (the comparator self-check needs the sage16 v2 fixed leg, the verdicts need all twelve)
and so cannot satisfy the target.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import h5py
import numpy
import yaml

TESTS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(TESTS_DIR))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import compare_cross_format_identity as comparator  # noqa: E402
from framework import TestSkipped, check_no_memory_leaks, run_test_suite  # noqa: E402
from framework.parity_gate import ParityGate  # noqa: E402

PLAN = "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md"
PLANNING_FILES = frozenset(
    {
        PLAN,
        "docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md",
        "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-REVIEW.md",
        "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-CHECKS.md",
    }
)
PLANNING_DIRECTORY = "docs/dev/snapshot-global-checks/"

FEATURE_PATHS = ("src", "scripts", "models")
FEATURE_SYMBOLS = r"process_snapshot|PROCESSING_MODE_SNAPSHOT|SnapshotContext|post_snapshot"

#: The shipped SAGE run file every leg derives from.
TEMPLATE = "models/sage16/input/sage16_mini-millennium.yaml"

#: Run-file name and output location shared by every run. The copied run YAML in
#: ``metadata/`` takes its name from the run file, and the run file's relative
#: output_directory is redirected per run through the worktree's ``output`` symlink, so the
#: baseline and feature-absent copies are byte-identical.
RUN_FILE_NAME = "identity.yaml"
OUTPUT_DIRECTORY = "output/identity"
OUTPUT_BASENAME = "model"

MODELS = ("halos-only", "sage16")
SCHEMES = ("fixed", "dynamic")

#: Datasets the contract names for byte equality, relative to ``RunProperties``.
NAMED_DATASETS = ("Parameters", "FieldMetadata", "EnabledModules", "EventContracts", "Redshifts")

#: ``RunProperties/Version`` attributes that necessarily differ between two builds.
VERSION_ATTRIBUTES = ("version", "git_commit", "git_branch", "git_date", "build_date")

#: ``RunProperties`` attribute that necessarily differs between two runs.
RUN_END_TIME = "RunEndTime"

#: Added to the copied run YAML by the feature-empty leg, and nothing else.
EMPTY_LIST_LINE = "  post_snapshot: []"

BUILD_TIMEOUT_S = 3600
RUN_TIMEOUT_S = 3600
GIT_TIMEOUT_S = 600

TREES = ("reference", "feature")

STARTED = time.monotonic()


def log(message: str) -> None:
    """Print one progress line prefixed with elapsed wall time, unbuffered."""
    elapsed = int(time.monotonic() - STARTED)
    print(f"[identity {elapsed // 60:3d}m{elapsed % 60:02d}s] {message}", flush=True)


# ---------------------------------------------------------------------------
# Fixtures and legs
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Fixture:
    """One committed input route: the simulation package and the config naming its data."""

    name: str
    simulation: str
    config: str
    description: str


FIXTURES = (
    Fixture(
        "default",
        "mini-millennium",
        "tests/data/test_simulation.yaml",
        "committed mini-Millennium trees (L-Halo binary, vertical driver)",
    ),
    Fixture(
        "v2",
        "micro-uchuu-ascii-horizontal",
        "simulations/micro-uchuu-ascii-horizontal/_tests/input/test_simulation.yaml",
        "committed version 2 generic fixture (horizontal driver, adjacent links)",
    ),
    Fixture(
        "v3-gapped",
        "mini-millennium-horizontal",
        "simulations/mini-millennium-horizontal/_tests/input/test_simulation.yaml",
        "committed version 3 worked_graph fixture (horizontal driver, gapped links)",
    ),
)


def leg_name(model: str, fixture: Fixture, scheme: str) -> str:
    return f"{model}__{fixture.name}__{scheme}".replace("-", "_")


# ---------------------------------------------------------------------------
# git helpers and reference-commit resolution
# ---------------------------------------------------------------------------


def git(*args: str, check: bool = True, cwd: Path = REPO_ROOT) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        ["git", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=GIT_TIMEOUT_S,
    )
    if check and completed.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {completed.stderr.strip()}")
    return completed


def is_planning_path(path: str) -> bool:
    return path in PLANNING_FILES or path.startswith(PLANNING_DIRECTORY)


def non_planning_changes(older: str, newer: str) -> list[str]:
    """Paths outside the planning surface that differ between two commits."""
    changed = git("diff", "--name-only", older, newer).stdout.split()
    return [path for path in changed if not is_planning_path(path)]


def derive_reference_commit() -> str:
    """The parent of the first post-plan commit that changes a non-planning path."""
    plan_commit = git("log", "-1", "--format=%H", "--", PLAN).stdout.strip()
    if not plan_commit:
        raise AssertionError(f"{PLAN} has no commit to walk from")
    walk = git("rev-list", "--first-parent", "--reverse", f"{plan_commit}^..HEAD").stdout.split()
    for commit in walk:
        parents = git("rev-list", "--parents", "-n", "1", commit).stdout.split()[1:]
        if parents and non_planning_changes(parents[0], commit):
            return parents[0]
    raise AssertionError(
        f"no commit after {plan_commit} changes a path outside the planning surface"
    )


@dataclass
class Reference:
    commit: str
    derived: str
    explicit: str | None
    plan_commit: str
    head: str


def has_feature_symbols(commit: str) -> bool:
    """True when the commit's src/, scripts/ or models/ name any feature symbol."""
    completed = git("grep", "-q", "-E", FEATURE_SYMBOLS, commit, "--", *FEATURE_PATHS, check=False)
    if completed.returncode not in (0, 1):
        raise AssertionError(f"git grep at {commit} failed: {completed.stderr.strip()}")
    return completed.returncode == 0


def required_paths() -> list[str]:
    """Every path the legs select by name, which both trees must contain."""
    paths = [TEMPLATE]
    for model in MODELS:
        paths.append(f"models/{model}/model_properties.yaml")
    for fixture in FIXTURES:
        paths.append(f"simulations/{fixture.simulation}/simulation_info.yaml")
        paths.append(fixture.config)
    return paths


def selector_paths(commit: str) -> list[str]:
    """Input paths the fixtures' simulation configs name, which must exist at the commit."""
    paths = []
    for fixture in FIXTURES:
        text = git("show", f"{commit}:{fixture.config}").stdout
        section = (yaml.safe_load(text) or {}).get("input") or {}
        for key in ("simulation_dir", "snapshot_list_file"):
            paths.append(os.path.normpath(section[key]))
    return paths


def check_reference(commit: str, head: str) -> None:
    """Raise unless the commit is a valid pre-feature reference for this run."""
    if git("merge-base", "--is-ancestor", commit, head, check=False).returncode != 0:
        raise AssertionError(f"reference {commit} is not an ancestor of HEAD {head}")
    if has_feature_symbols(commit):
        raise AssertionError(f"reference {commit} already contains a feature symbol")
    for path in required_paths() + selector_paths(commit):
        if git("cat-file", "-e", f"{commit}:{path}", check=False).returncode != 0:
            raise AssertionError(f"reference {commit} lacks {path}, which the legs select")


def resolve_reference(explicit: str | None) -> Reference:
    head = git("rev-parse", "HEAD").stdout.strip()
    plan_commit = git("log", "-1", "--format=%H", "--", PLAN).stdout.strip()
    derived = derive_reference_commit()
    chosen = derived
    if explicit:
        completed = git("rev-parse", "--verify", f"{explicit}^{{commit}}", check=False)
        if completed.returncode != 0:
            raise AssertionError(f"REFERENCE_COMMIT {explicit!r} is not a commit")
        chosen = completed.stdout.strip()
        if chosen != derived:
            extra = non_planning_changes(chosen, derived)
            if extra:
                raise AssertionError(
                    f"REFERENCE_COMMIT {chosen} differs from the derived reference {derived} "
                    f"in non-planning paths: {extra[:10]}"
                )
    check_reference(chosen, head)
    for path in required_paths() + selector_paths(head):
        if git("cat-file", "-e", f"{head}:{path}", check=False).returncode != 0:
            raise AssertionError(f"HEAD {head} lacks {path}, which the legs select")
    return Reference(chosen, derived, explicit, plan_commit, head)


def runtime_tree_differences() -> list[str]:
    """Uncommitted edits to the feature tree's runtime paths (test envelopes excluded)."""
    changed = git("diff", "--name-only", "HEAD", "--", *FEATURE_PATHS).stdout.split()
    return [path for path in changed if "/_tests/" not in path]


# ---------------------------------------------------------------------------
# Run files
# ---------------------------------------------------------------------------


def substitute_once(text: str, pattern: str, replacement: str, what: str) -> str:
    result, count = re.subn(pattern, lambda _match: replacement, text, flags=re.MULTILINE)
    if count != 1:
        raise AssertionError(f"shipped run file: {what} matched {count} time(s), expected 1")
    return result


def run_file_text(template: str, model: str, fixture: Fixture, scheme: str) -> str:
    """The leg's run file: the shipped SAGE file with only its selectors changed.

    The ``modules`` mapping and every parameter of the SAGE run are kept verbatim; halos-only
    replaces the mapping with an empty pipeline. The result is parsed back and the claim
    checked, so a template edit that defeats a substitution fails loudly.
    """
    text = substitute_once(
        template, r"^model:\n  name: sage16\n", f"model:\n  name: {model}\n", "model"
    )
    text = substitute_once(
        text,
        r"^simulation:\n  name: mini-millennium\n",
        f"simulation:\n  name: {fixture.simulation}\n  config: {fixture.config}\n",
        "simulation",
    )
    text = substitute_once(
        text, r"^  output_directory: .*$", f"  output_directory: {OUTPUT_DIRECTORY}", "output"
    )
    text = substitute_once(text, r"^  snapshot_list: .*$", "  snapshot_list: []", "snapshots")
    text = substitute_once(
        text,
        r"^SubSteps: 10\n",
        f"SubSteps: 10\nMaxDynamicSubsteps: 200\nTimestepScheme: {scheme}\n",
        "substeps",
    )
    if model == "halos-only":
        start = text.index("\nmodules:\n")
        text = text[: start + 1] + "modules:\n  phases: {}\n  parameters: {}\n"

    parsed, shipped = yaml.safe_load(text), yaml.safe_load(template)
    assert parsed["model"] == {"name": model}
    assert parsed["simulation"] == {"name": fixture.simulation, "config": fixture.config}
    assert parsed["output"]["snapshot_list"] == []
    assert parsed["TimestepScheme"] == scheme and parsed["SubSteps"] == 10
    assert "post_snapshot" not in parsed["modules"], "the baseline run file must not carry the key"
    expected_keys = set(shipped) | {"MaxDynamicSubsteps", "TimestepScheme"}
    assert set(parsed) == expected_keys, (
        f"top-level keys {sorted(parsed)} are not the shipped file's plus the scheme keys "
        f"{sorted(expected_keys)}; a key was dropped or added"
    )
    if model == "sage16":
        assert parsed["modules"] == shipped["modules"], "the SAGE modules mapping was altered"
    else:
        assert parsed["modules"] == {"phases": {}, "parameters": {}}
    return text


def empty_list_text(text: str) -> str:
    """The run file with ``post_snapshot: []`` added as the first key under ``modules:``."""
    marker = "\nmodules:\n"
    if text.count(marker) != 1:
        raise AssertionError("run file does not carry exactly one modules: line")
    variant = text.replace(marker, f"{marker}{EMPTY_LIST_LINE}\n", 1)
    assert yaml.safe_load(variant)["modules"]["post_snapshot"] == []
    assert variant.splitlines() == sorted_insert(text.splitlines(), EMPTY_LIST_LINE)
    return variant


def sorted_insert(lines: list[str], added: str) -> list[str]:
    """``lines`` with ``added`` placed straight after the modules: line."""
    index = lines.index("modules:")
    return lines[: index + 1] + [added] + lines[index + 1 :]


# ---------------------------------------------------------------------------
# Output comparison
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class OutputRun:
    """A finished run's output directory and the path prefixes its files may embed."""

    directory: Path
    prefixes: tuple[str, ...]

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
    snapshots: int = 0
    galaxies: int = 0
    fields: int = 0
    id_mismatches: int = 0

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
            "output_snapshots": self.snapshots,
            "galaxies": self.galaxies,
            "fields": self.fields,
            "per_id_mismatches": self.id_mismatches,
        }


def relative_files(root: Path) -> list[str]:
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())


def attribute_signature(value) -> tuple:
    """(dtype, shape, bytes) of an attribute, with variable-length strings made canonical."""
    array = numpy.asarray(value)
    if array.dtype.kind == "O":
        items = [item.encode() if isinstance(item, str) else bytes(item) for item in array.ravel()]
        return ("O", array.shape, b"\0".join(items))
    return (array.dtype.str, array.shape, array.tobytes())


def attribute_text(signature: tuple) -> str:
    """The attribute's text when it is a string, else empty (for path-prefix reasoning)."""
    dtype, _shape, payload = signature
    if dtype == "O" or dtype.startswith(("|S", "<U")):
        return payload.decode(errors="replace").rstrip("\0")
    return ""


def dtype_signature(dtype: numpy.dtype):
    """The full layout of a dtype: for a compound, its record size and each field's name, base
    type, shape and byte offset in order; the type string for anything else.

    Dataset values are compared field by field (padding bytes legitimately differ between
    identical runs), so the layout is what makes "the same bytes" mean the same record.
    """
    if not dtype.names:
        return dtype.str
    fields = []
    for name in dtype.names:
        field, offset = dtype.fields[name][:2]
        base, shape = field.subdtype if field.subdtype is not None else (field, ())
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


def is_permitted_attribute(path: str, name: str) -> str | None:
    """The permitted-difference label of an attribute, or None when it must be identical.

    A permitted attribute may differ in value only: its dtype and shape are always compared.
    """
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
        left, right = attribute_signature(a.attrs[name]), attribute_signature(b.attrs[name])
        if left == right:
            continue
        shape_differs = left[:2] != right[:2]
        label = is_permitted_attribute(path, name)
        if label is not None and not shape_differs:
            report.allow(label)
            continue
        if label is not None:
            report.error(
                f"{where}:{path}@{name}: permitted attribute changed dtype or shape "
                f"({left[:2]} != {right[:2]}); only its value may differ"
            )
            continue
        text_a, text_b = attribute_text(left), attribute_text(right)
        if (
            path == "RunProperties"
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


def compare_hdf5_file(rel: str, a_path: Path, b_path: Path, base, other, report) -> None:
    """Structure, attributes and non-Galaxies dataset bytes of one HDF5 file pair."""
    with h5py.File(a_path, "r") as fa, h5py.File(b_path, "r") as fb:
        objects_a, objects_b = objects_of(fa), objects_of(fb)
        if set(objects_a) != set(objects_b):
            report.error(
                f"{rel}: HDF5 objects differ (only baseline {sorted(set(objects_a) - set(objects_b))}, "
                f"only candidate {sorted(set(objects_b) - set(objects_a))})"
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
                continue
            if left.shape != right.shape:
                report.error(f"{rel}:{path}: shape {left.shape} != {right.shape}")
                continue
            if path.endswith("/Galaxies"):
                continue  # compared per ID across all partitions
            same = canonical_bytes(left[()]) == canonical_bytes(right[()])
            leaf = path.removeprefix("RunProperties/")
            if path.startswith("RunProperties/") and leaf in NAMED_DATASETS:
                report.datasets_compared.append(leaf)
            if not same:
                report.error(f"{rel}:{path}: dataset bytes differ")


def load_galaxies(run: OutputRun) -> dict[int, numpy.ndarray]:
    """Every output snapshot's Galaxies rows, aggregated across the run's partitions."""
    pieces: dict[int, list[numpy.ndarray]] = {}
    for partition in comparator.partition_files(str(run.directory / OUTPUT_BASENAME)):
        with h5py.File(partition, "r") as handle:
            for name in handle:
                match = comparator.SNAP_GROUP_RE.match(name)
                if match and "Galaxies" in handle[name]:
                    pieces.setdefault(int(match.group(1)), []).append(handle[name]["Galaxies"][()])
    return {snap: numpy.concatenate(rows) for snap, rows in pieces.items()}


def field_bytes(rows: numpy.ndarray, name: str) -> numpy.ndarray:
    """A field as one row of raw bytes per record."""
    if len(rows) == 0:
        return numpy.zeros((0, 0), dtype=numpy.uint8)
    column = numpy.ascontiguousarray(rows[name])
    return column.view(numpy.uint8).reshape(len(rows), -1)


def compare_galaxies(base: OutputRun, other: OutputRun, report: Report) -> None:
    left_all, right_all = load_galaxies(base), load_galaxies(other)
    if set(left_all) != set(right_all):
        report.error(
            f"output snapshots differ (only baseline {sorted(set(left_all) - set(right_all))}, "
            f"only candidate {sorted(set(right_all) - set(left_all))})"
        )
    report.snapshots = len(set(left_all) & set(right_all))
    for snap in sorted(set(left_all) & set(right_all)):
        left, right = left_all[snap], right_all[snap]
        where = f"Snap{snap:03d}"
        if comparator.schema_signature(left.dtype) != comparator.schema_signature(right.dtype):
            report.error(f"{where}: Galaxies record schema differs")
            continue
        ids_left, ids_right = left[comparator.ID_FIELD], right[comparator.ID_FIELD]
        duplicates = False
        for label, ids in (("baseline", ids_left), ("candidate", ids_right)):
            if len(numpy.unique(ids)) != len(ids):
                report.error(f"{where}: duplicate UniqueGalaxyID in {label}")
                duplicates = True
        if duplicates:
            continue
        order_left, order_right = numpy.argsort(ids_left), numpy.argsort(ids_right)
        sorted_left, sorted_right = ids_left[order_left], ids_right[order_right]
        common = numpy.intersect1d(sorted_left, sorted_right)
        missing, added = len(sorted_left) - len(common), len(sorted_right) - len(common)
        if missing or added:
            report.error(
                f"{where}: ID sets differ ({missing} only in baseline, {added} only in candidate)"
            )
            report.id_mismatches += missing + added
        keep_left = order_left[numpy.isin(sorted_left, common)]
        keep_right = order_right[numpy.isin(sorted_right, common)]
        left, right = left[keep_left], right[keep_right]
        report.galaxies += len(common)
        report.fields = len(left.dtype.names)
        differing = numpy.zeros(len(common), dtype=bool)
        for name in left.dtype.names:
            changed = (field_bytes(left, name) != field_bytes(right, name)).any(axis=1)
            if changed.any():
                report.error(f"{where}: field {name} differs for {int(changed.sum())} ID(s)")
            differing |= changed
        report.id_mismatches += int(differing.sum())


def compare_metadata_file(rel: str, base: OutputRun, other: OutputRun, empty_leg: bool, report):
    left, right = (base.directory / rel).read_bytes(), (other.directory / rel).read_bytes()
    if rel == "metadata/version_info.json":
        report.allow("metadata/version_info.json")
        for label, raw in (("baseline", left), ("candidate", right)):
            if not isinstance(json.loads(raw), dict):
                report.error(f"{rel}: {label} is not a JSON object")
        return
    if rel == f"metadata/{RUN_FILE_NAME}" and empty_leg:
        lines_left = left.decode().splitlines()
        if right.decode().splitlines() == sorted_insert(lines_left, EMPTY_LIST_LINE):
            report.allow(f"{rel}: added '{EMPTY_LIST_LINE.strip()}' line")
        else:
            report.error(f"{rel}: differs from the baseline in more than the added empty list")
        return
    if left == right:
        return
    if rel == "metadata/output_schema.json":
        report.error(f"{rel}: output schema differs")
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


def compare_outputs(base: OutputRun, other: OutputRun, empty_leg: bool = False) -> Report:
    """Compare a candidate run with the baseline; every failure is in ``Report.errors``."""
    report = Report()
    files_left, files_right = relative_files(base.directory), relative_files(other.directory)
    if files_left != files_right:
        report.error(
            f"file sets differ (only baseline {sorted(set(files_left) - set(files_right))}, "
            f"only candidate {sorted(set(files_right) - set(files_left))})"
        )
    shared = sorted(set(files_left) & set(files_right))
    report.files = len(shared)
    if empty_leg and f"metadata/{RUN_FILE_NAME}" not in shared:
        report.error(f"the empty-list leg has no copied run YAML metadata/{RUN_FILE_NAME}")
    for rel in shared:
        if rel.endswith(".hdf5"):
            compare_hdf5_file(rel, base.directory / rel, other.directory / rel, base, other, report)
        elif rel.startswith("metadata/"):
            compare_metadata_file(rel, base, other, empty_leg, report)
        elif (base.directory / rel).read_bytes() != (other.directory / rel).read_bytes():
            report.error(f"{rel}: content differs")
    try:
        compare_galaxies(base, other, report)
    except comparator.ComparisonError as error:
        report.error(f"galaxy comparison could not read an output: {error}")
    return report


# ---------------------------------------------------------------------------
# The identity run
# ---------------------------------------------------------------------------


@dataclass
class RunRecord:
    tree: str
    variant: str
    output: OutputRun
    log_path: Path
    elapsed_s: float
    attributes: dict


class Identity:
    """State and stages of one identity run. Nothing it creates is ever deleted."""

    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.models = tuple(args.models)
        self.fixtures = tuple(fixture for fixture in FIXTURES if fixture.name in args.fixtures)
        self.schemes = tuple(args.schemes)
        self.partial = (
            set(self.models) != set(MODELS)
            or len(self.fixtures) != len(FIXTURES)
            or set(self.schemes) != set(SCHEMES)
        )
        self.reference: Reference | None = None
        self.root: Path | None = None
        self.worktrees: dict[tuple[str, str, str], Path] = {}
        self.commits: dict[str, str] = {}
        self.template: str | None = None
        self.verdicts: dict[str, str] = {}
        self.mutation_source: OutputRun | None = None
        self.evidence: dict = {"legs": {}, "timings_s": {}}

    # ---- services ---------------------------------------------------------

    def scratch(self, *parts: str) -> Path:
        assert self.root is not None, "the reference stage has not created the scratch root"
        path = self.root.joinpath(*parts)
        path.mkdir(parents=True, exist_ok=True)
        return path

    def save_evidence(self) -> None:
        if self.root is not None:
            (self.root / "evidence.json").write_text(json.dumps(self.evidence, indent=2) + "\n")

    def run_logged(
        self, cmd, cwd: Path, env: dict, log_path: Path, what: str, timeout: int
    ) -> float:
        log(f"  -> {what}")
        started = time.monotonic()
        with log_path.open("wb") as handle:
            try:
                completed = subprocess.run(
                    cmd, cwd=cwd, env=env, stdout=handle, stderr=subprocess.STDOUT, timeout=timeout
                )
            except subprocess.TimeoutExpired:
                raise AssertionError(
                    f"{what} timed out after {timeout}s (log: {log_path})"
                ) from None
        elapsed = time.monotonic() - started
        if completed.returncode != 0:
            tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-30:])
            raise AssertionError(
                f"{what} failed with exit status {completed.returncode} (log: {log_path})\n{tail}"
            )
        log(f"     done in {elapsed:.0f}s (log: {log_path})")
        return elapsed

    # ---- stage: reference -------------------------------------------------

    def test_reference_commit_resolution(self) -> None:
        """The reference commit is valid, pre-feature, and the feature tree is the HEAD tree."""
        explicit = os.environ.get("REFERENCE_COMMIT") or None
        self.reference = resolve_reference(explicit)
        ref = self.reference
        log(f"HEAD (feature tree): {ref.head}")
        log(f"plan last changed by: {ref.plan_commit}")
        log(f"derived reference: {ref.derived}")
        log(f"explicit REFERENCE_COMMIT: {ref.explicit or '(none)'}")
        log(f"resolved reference: {ref.commit}")
        if not has_feature_symbols(ref.head):
            raise AssertionError("HEAD contains no feature symbol; it is not a feature tree")
        edits = runtime_tree_differences()
        if edits:
            raise AssertionError(f"runtime paths differ from HEAD (uncommitted): {edits}")
        shipped_reference = git("show", f"{ref.commit}:{TEMPLATE}").stdout
        shipped_feature = git("show", f"{ref.head}:{TEMPLATE}").stdout
        if shipped_reference != shipped_feature:
            raise AssertionError(f"{TEMPLATE} differs between the reference and the feature tree")
        self.template = shipped_feature
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        self.root = (
            REPO_ROOT
            / "archive"
            / "snapshot-global-identity"
            / f"{stamp}-ref{ref.commit[:8]}-head{ref.head[:8]}"
        )
        self.root.mkdir(parents=True, exist_ok=False)
        self.commits = {"reference": ref.commit, "feature": ref.head}
        self.evidence.update(
            {
                "head": ref.head,
                "plan_commit": ref.plan_commit,
                "derived_reference": ref.derived,
                "explicit_reference": ref.explicit,
                "reference": ref.commit,
                "scratch": str(self.root),
                "partial_run": self.partial,
            }
        )
        self.save_evidence()
        log(f"scratch (archived, never deleted): {self.root}")

    def test_reference_resolution_rejects_bad_values(self) -> None:
        """An explicit value that is not the derived reference is refused for each defect."""
        ref = self.reference
        assert ref is not None, "reference resolution did not complete"
        refusals = {
            "a feature commit (HEAD)": ref.head,
            "a value that is not a commit": "0" * 40,
        }
        refusals["a commit older than the derived reference"] = self.older_with_changes(ref)
        for what, value in refusals.items():
            try:
                resolve_reference(value)
            except AssertionError as error:
                log(f"  refused {what}: {str(error).splitlines()[0][:110]}")
            else:
                raise AssertionError(f"REFERENCE_COMMIT naming {what} was accepted")

    @staticmethod
    def older_with_changes(ref: Reference) -> str:
        """An ancestor of the derived reference that differs from it outside the planning surface."""
        for commit in git("rev-list", "--first-parent", "-n", "50", ref.derived).stdout.split()[1:]:
            if non_planning_changes(commit, ref.derived):
                return commit
        raise AssertionError("no older commit with non-planning changes to use as a negative case")

    # ---- stage: builds ----------------------------------------------------

    def build_all(self) -> None:
        """One detached worktree per tree x model x simulation, each built at its commit."""
        assert self.reference is not None, "reference resolution did not complete"
        pairs = [(model, fixture.simulation) for model in self.models for fixture in self.fixtures]
        for tree in TREES:
            commit = self.commits[tree]
            for model, simulation in pairs:
                key = (tree, model, simulation)
                name = f"{tree}__{model}__{simulation}"
                worktree = self.scratch("worktrees") / name
                logs = self.scratch("logs")
                log(f"worktree {name} at {commit}")
                git("worktree", "add", "--detach", str(worktree), commit)
                self.worktrees[key] = worktree
                ParityGate.link_machine_local(worktree)
                head = git("rev-parse", "HEAD", cwd=worktree).stdout.strip()
                if head != commit:
                    raise AssertionError(f"{name}: worktree is at {head}, expected {commit}")
                env = ParityGate.worktree_env(worktree, model, simulation)
                selectors = [f"MODEL={model}", f"SIMULATION={simulation}"]
                started = time.monotonic()
                self.run_logged(
                    ["make", *selectors, "generate"],
                    worktree,
                    env,
                    logs / f"{name}-generate.log",
                    f"{name}: make generate",
                    BUILD_TIMEOUT_S,
                )
                self.run_logged(
                    ["make", *selectors, f"-j{os.cpu_count() or 4}"],
                    worktree,
                    env,
                    logs / f"{name}-build.log",
                    f"{name}: make",
                    BUILD_TIMEOUT_S,
                )
                if not (worktree / "mimic").is_file():
                    raise AssertionError(f"{name}: build produced no executable")
                self.evidence["timings_s"][f"build:{name}"] = round(time.monotonic() - started, 1)
        self.save_evidence()

    def test_builds_name_their_commits(self) -> None:
        assert self.reference is not None, "reference resolution did not complete"
        self.build_all()

    # ---- stage: legs ------------------------------------------------------

    def execute(
        self, tree: str, model: str, fixture: Fixture, leg: str, variant: str, run_file: Path
    ):
        """Run one executable from its worktree into its own scratch output root."""
        worktree = self.worktrees[(tree, model, fixture.simulation)]
        output_root = self.scratch("runs", f"{tree}__{leg}__{variant}")
        link = worktree / "output"
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(output_root)
        log_path = self.scratch("logs") / f"{tree}__{leg}__{variant}-run.log"
        elapsed = self.run_logged(
            ["./mimic", str(run_file)],
            worktree,
            ParityGate.worktree_env(worktree, model, fixture.simulation),
            log_path,
            f"{tree}/{variant} {leg}: mimic run",
            RUN_TIMEOUT_S,
        )
        text = log_path.read_text(errors="replace")
        if not check_no_memory_leaks(text):
            raise AssertionError(f"{tree}/{variant} {leg}: the run reported a memory leak")
        directory = output_root / Path(OUTPUT_DIRECTORY).name
        output = OutputRun(
            directory,
            tuple(
                {
                    str(worktree),
                    str(worktree.resolve()),
                    str(output_root),
                    str(output_root.resolve()),
                }
            ),
        )
        master = directory / f"{OUTPUT_BASENAME}.hdf5"
        if not master.is_file():
            raise AssertionError(f"{tree}/{variant} {leg}: no master file at {master}")
        with h5py.File(master, "r") as handle:
            properties = handle["RunProperties"]
            attributes = {
                "git_commit": attribute_text(
                    attribute_signature(properties["Version"].attrs["git_commit"])
                ),
                "version": attribute_text(
                    attribute_signature(properties["Version"].attrs["version"])
                ),
                "TimestepScheme": attribute_text(
                    attribute_signature(properties.attrs["TimestepScheme"])
                ),
                "ModelName": attribute_text(attribute_signature(properties.attrs["ModelName"])),
                "SimulationName": attribute_text(
                    attribute_signature(properties.attrs["SimulationName"])
                ),
            }
        return RunRecord(tree, variant, output, log_path, elapsed, attributes)

    def check_provenance(self, record: RunRecord, model, fixture, scheme) -> None:
        """The run came from the intended commit, model, simulation and timestepping scheme."""
        expected = {
            "git_commit": self.commits[record.tree],
            "TimestepScheme": scheme,
            "ModelName": model,
            "SimulationName": fixture.simulation,
        }
        for key, value in expected.items():
            if record.attributes[key] != value:
                raise AssertionError(
                    f"{record.tree}/{record.variant}: RunProperties {key} is "
                    f"{record.attributes[key]!r}, expected {value!r}"
                )

    def leg(self, model: str, fixture: Fixture, scheme: str) -> None:
        name = leg_name(model, fixture, scheme)
        if self.template is None or not self.worktrees:
            raise AssertionError("prerequisite stages (reference, builds) did not complete")
        text = run_file_text(self.template, model, fixture, scheme)
        files = {}
        for variant, body in (("absent", text), ("empty", empty_list_text(text))):
            directory = self.scratch("run-files", name, variant)
            files[variant] = directory / RUN_FILE_NAME
            files[variant].write_text(body)
        leg_evidence = self.evidence["legs"].setdefault(name, {})
        leg_evidence.update({"model": model, "fixture": fixture.name, "scheme": scheme})
        started = time.monotonic()
        baseline = self.execute("reference", model, fixture, name, "baseline", files["absent"])
        absent = self.execute("feature", model, fixture, name, "absent", files["absent"])
        empty = self.execute("feature", model, fixture, name, "empty", files["empty"])
        for record in (baseline, absent, empty):
            self.check_provenance(record, model, fixture, scheme)
        leg_evidence["commits"] = {
            "baseline": baseline.attributes["git_commit"],
            "feature": absent.attributes["git_commit"],
        }
        leg_evidence["run_seconds"] = {
            record.variant: round(record.elapsed_s, 1) for record in (baseline, absent, empty)
        }
        if fixture.name == "v2" and model == "sage16" and scheme == "fixed":
            self.mutation_source = absent.output
        failures = []
        for record, empty_leg in ((absent, False), (empty, True)):
            report = compare_outputs(baseline.output, record.output, empty_leg=empty_leg)
            leg_evidence[f"compare_{record.variant}"] = report.summary()
            log(
                f"  {name} {record.variant}: {report.galaxies} galaxies over {report.snapshots} "
                f"snapshot(s), {report.fields} fields, {report.id_mismatches} per-ID mismatches, "
                f"{len(report.errors)} error(s); permitted: {sorted(report.permitted)}"
            )
            failures.extend(f"{record.variant}: {error}" for error in report.errors)
            if report.galaxies <= 0 and not report.errors:
                failures.append(f"{record.variant}: no galaxies were compared")
        leg_evidence["elapsed_s"] = round(time.monotonic() - started, 1)
        self.verdicts[name] = "PASS" if not failures else f"FAIL: {failures[0]}"
        leg_evidence["verdict"] = self.verdicts[name]
        self.save_evidence()
        if failures:
            raise AssertionError(f"{name}: {len(failures)} failure(s); first: {failures[0]}")

    def leg_stages(self):
        stages = []
        for model in self.models:
            for fixture in self.fixtures:
                for scheme in self.schemes:

                    def stage(m=model, f=fixture, s=scheme):
                        self.leg(m, f, s)

                    stage.__name__ = f"test_leg_{leg_name(model, fixture, scheme)}"
                    stages.append(stage)
        return stages

    # ---- stage: comparator self-check ------------------------------------

    def test_comparator_rejects_mutations(self) -> None:
        """The comparator fails on every injected defect and accepts only the permitted ones."""
        if self.mutation_source is None:
            raise TestSkipped(
                "development subset: the sage16 v2 fixed leg did not run, so there is no real "
                "output to mutate"
            )
        run_mutation_checks(self.mutation_source, self.scratch("mutations"))

    # ---- stage: verdicts --------------------------------------------------

    def test_leg_verdicts(self) -> None:
        expected = [
            leg_name(m, f, s) for m in self.models for f in self.fixtures for s in self.schemes
        ]
        for name in expected:
            log(f"  {name}: {self.verdicts.get(name, 'did not run')}")
        bad = [name for name in expected if self.verdicts.get(name) != "PASS"]
        self.evidence["verdicts"] = {
            name: self.verdicts.get(name, "did not run") for name in expected
        }
        self.evidence["total_elapsed_s"] = round(time.monotonic() - STARTED, 1)
        self.save_evidence()
        if bad:
            raise AssertionError(f"{len(bad)} of {len(expected)} legs did not pass: {bad}")
        if self.partial:
            raise TestSkipped(
                f"development subset ({len(expected)} of {len(MODELS) * len(FIXTURES) * len(SCHEMES)} "
                "legs); the full matrix has not been run"
            )
        log(f"all {len(expected)} legs passed; evidence: {self.root}/evidence.json")

    def stages(self):
        return [
            self.test_reference_commit_resolution,
            self.test_reference_resolution_rejects_bad_values,
            self.test_builds_name_their_commits,
            *self.leg_stages(),
            self.test_comparator_rejects_mutations,
            self.test_leg_verdicts,
        ]


# ---------------------------------------------------------------------------
# Comparator self-check (mutations of real output)
# ---------------------------------------------------------------------------


def copy_run(source: OutputRun, destination: Path, token: str) -> OutputRun:
    """A copy of a run's output directory, with its own path prefix token."""
    shutil.copytree(source.directory, destination)
    return OutputRun(destination, (token,))


def snapshot_partition(run: OutputRun, snap: int) -> Path:
    return run.directory / f"{OUTPUT_BASENAME}_{snap:03d}.hdf5"


def rewrite_galaxies(path: Path, snap: int, transform) -> None:
    """Replace a snapshot's Galaxies dataset with ``transform(rows)``, keeping its attributes."""
    with h5py.File(path, "r+") as handle:
        name = f"Snap{snap:03d}/Galaxies"
        dataset = handle[name]
        rows = dataset[()]
        attributes = [
            (key, dataset.attrs[key], dataset.attrs.get_id(key).dtype) for key in dataset.attrs
        ]
        new_rows = transform(rows)
        del handle[name]
        created = handle.create_dataset(name, data=new_rows)
        for key, value, dtype in attributes:
            created.attrs.create(key, data=value, dtype=dtype)


def change_attribute_value(obj, name: str, value: bytes) -> None:
    """Give an attribute a new value, keeping its stored dtype and shape."""
    stored = obj.attrs.get_id(name)
    obj.attrs.create(name, data=numpy.array([value], dtype=stored.dtype), dtype=stored.dtype)


def relayout_dataset(path: Path, name: str) -> None:
    """Rewrite a compound dataset with identical values but a different record layout."""
    with h5py.File(path, "r+") as handle:
        dataset = handle[name]
        values = dataset[()]
        attributes = [
            (key, dataset.attrs[key], dataset.attrs.get_id(key).dtype) for key in dataset.attrs
        ]
        old = values.dtype
        layout = numpy.dtype(
            {
                "names": list(old.names),
                "formats": [old.fields[field][0] for field in old.names],
                "offsets": [old.fields[field][1] + 8 for field in old.names],
                "itemsize": old.itemsize + 16,
            }
        )
        moved = numpy.zeros(values.shape, dtype=layout)
        for field in old.names:
            moved[field] = values[field]
        del handle[name]
        created = handle.create_dataset(name, data=moved, dtype=layout)
        for key, value, dtype in attributes:
            created.attrs.create(key, data=value, dtype=dtype)


def populated_snapshots(run: OutputRun) -> list[int]:
    return sorted(snap for snap, rows in load_galaxies(run).items() if len(rows) >= 2)


def run_mutation_checks(source: OutputRun, scratch: Path) -> None:
    """Prove each assertion of the comparator fails on the defect it exists to catch."""
    base = copy_run(source, scratch / "base", str(scratch / "base_prefix"))
    snaps = populated_snapshots(base)
    if len(snaps) < 2:
        raise AssertionError(f"mutation source has fewer than two populated snapshots: {snaps}")
    first, second = snaps[0], snaps[1]
    results: list[str] = []

    def candidate(name: str) -> OutputRun:
        return copy_run(base, scratch / name, str(scratch / f"{name}_prefix"))

    def expect_failure(name: str, run: OutputRun, needle: str, **kwargs) -> None:
        report = compare_outputs(base, run, **kwargs)
        if not any(needle in error for error in report.errors):
            raise AssertionError(
                f"mutation {name!r} was not rejected with {needle!r}: errors {report.errors[:4]}"
            )
        results.append(name)
        log(f"  mutation rejected: {name} ({len(report.errors)} error(s))")

    def expect_accepted(name: str, run: OutputRun, expected_permitted: list[str], **kwargs) -> None:
        report = compare_outputs(base, run, **kwargs)
        if report.errors:
            raise AssertionError(f"control {name!r} was rejected: {report.errors[:4]}")
        missing = [label for label in expected_permitted if label not in report.permitted]
        if missing:
            raise AssertionError(f"control {name!r} did not name {missing}: {report.permitted}")
        results.append(name)
        log(f"  control accepted: {name} (permitted: {sorted(report.permitted)})")

    expect_accepted("an untouched copy", candidate("control"), [])
    rewritten = candidate("control_rewrite")
    rewrite_galaxies(snapshot_partition(rewritten, first), first, lambda rows: rows)
    expect_accepted("a dataset rewritten with identical rows", rewritten, [])

    dropped = candidate("drop_id")
    rewrite_galaxies(snapshot_partition(dropped, first), first, lambda rows: rows[:-1])
    expect_failure("a dropped ID", dropped, "ID sets differ")

    duplicated = candidate("duplicate_id")

    def duplicate(rows):
        rows = rows.copy()
        rows["UniqueGalaxyID"][1] = rows["UniqueGalaxyID"][0]
        return rows

    rewrite_galaxies(snapshot_partition(duplicated, first), first, duplicate)
    expect_failure("a duplicated ID", duplicated, "duplicate UniqueGalaxyID")

    perturbed = candidate("perturb_field")

    def perturb(rows):
        rows = rows.copy()
        column = rows["Mvir"].copy()
        column.view(numpy.uint8)[0] ^= 1
        rows["Mvir"] = column
        return rows

    rewrite_galaxies(snapshot_partition(perturbed, first), first, perturb)
    expect_failure("a field perturbed by one bit", perturbed, "field Mvir differs for 1 ID(s)")

    swapped = candidate("swap_payload")

    def swap(rows):
        rows = rows.copy()
        for name in rows.dtype.names:
            if name != "UniqueGalaxyID":
                rows[name][[0, 1]] = rows[name][[1, 0]]
        return rows

    rewrite_galaxies(snapshot_partition(swapped, first), first, swap)
    expect_failure("payloads swapped between two IDs", swapped, "differs for")

    moved = candidate("wrong_snapshot")
    with h5py.File(snapshot_partition(moved, first), "r") as handle:
        row = handle[f"Snap{first:03d}/Galaxies"][()][-1:]
    rewrite_galaxies(snapshot_partition(moved, first), first, lambda rows: rows[:-1])
    rewrite_galaxies(
        snapshot_partition(moved, second), second, lambda rows: numpy.concatenate([rows, row])
    )
    expect_failure("a galaxy in the wrong snapshot", moved, f"Snap{first:03d}: ID sets differ")

    master = "model.hdf5"
    for dataset in NAMED_DATASETS:
        with h5py.File(base.directory / master, "r") as handle:
            if f"RunProperties/{dataset}" not in handle:
                continue
        missing = candidate(f"missing_{dataset}")
        with h5py.File(missing.directory / master, "r+") as handle:
            del handle[f"RunProperties/{dataset}"]
        expect_failure(f"missing dataset {dataset}", missing, "HDF5 objects differ")

        altered = candidate(f"altered_{dataset}")
        with h5py.File(altered.directory / master, "r+") as handle:
            data = handle[f"RunProperties/{dataset}"]
            values = data[()]
            if values.size == 0:
                log(f"  dataset {dataset} is empty here; no byte to alter")
                continue
            if values.dtype.names:
                column = values[values.dtype.names[0]]
                head = column[0]
                if isinstance(head, bytes):
                    column[0] = b"X" + head[1:]
                elif isinstance(head, str):
                    column[0] = "X" + head[1:]
                else:
                    column[0] = head + 1
            else:
                values.view(numpy.uint8).reshape(-1)[0] ^= 1
            data[...] = values
        expect_failure(
            f"altered dataset {dataset}", altered, f"RunProperties/{dataset}: dataset bytes differ"
        )

    extra = candidate("extra_dataset")
    with h5py.File(extra.directory / master, "r+") as handle:
        handle["RunProperties"].create_dataset("EventContractsExtra", data=numpy.zeros(1))
    expect_failure("an extra dataset", extra, "HDF5 objects differ")

    attribute = candidate("attribute")
    with h5py.File(attribute.directory / master, "r+") as handle:
        handle["RunProperties"].attrs["SubSteps"] = 11
    expect_failure("an altered RunProperties attribute", attribute, "RunProperties@SubSteps")

    renamed = candidate("name_attribute")
    with h5py.File(renamed.directory / master, "r+") as handle:
        handle["RunProperties"].attrs.create("ModelName", data=b"sage16x", dtype="S7")
    expect_failure("an altered non-permitted string attribute", renamed, "RunProperties@ModelName")

    added = candidate("added_attribute")
    with h5py.File(added.directory / master, "r+") as handle:
        handle["RunProperties"].attrs["Extra"] = 1
    expect_failure("an added attribute", added, "attribute names differ")

    schema = candidate("schema")
    path = schema.directory / "metadata" / "output_schema.json"
    path.write_text(path.read_text().replace('"', "'", 1))
    expect_failure("an altered output schema", schema, "output schema differs")

    metadata = candidate("metadata")
    path = metadata.directory / "metadata" / "test_simulation.yaml"
    path.write_text(path.read_text() + "# changed\n")
    expect_failure(
        "a metadata file changed beyond its path prefix", metadata, "differs beyond the path prefix"
    )

    removed = candidate("file_set")
    (removed.directory / "metadata" / "test_simulation.yaml").unlink()
    expect_failure("a missing metadata file", removed, "file sets differ")

    # Permitted differences are accepted, and named.
    provenance = candidate("provenance")
    with h5py.File(provenance.directory / master, "r+") as handle:
        change_attribute_value(handle["RunProperties/Version"], "git_commit", b"f" * 40)
        change_attribute_value(handle["RunProperties"], RUN_END_TIME, b"1999-01-01T00:00:00")
    info = provenance.directory / "metadata" / "version_info.json"
    info.write_text(info.read_text().replace('"run_date"', '"run_date_changed"'))
    expect_accepted(
        "permitted provenance differences",
        provenance,
        [
            "RunProperties/Version@git_commit",
            f"RunProperties@{RUN_END_TIME}",
            "metadata/version_info.json",
        ],
    )

    for label, mutate in (
        (
            "a permitted attribute with a changed dtype",
            lambda handle: handle["RunProperties/Version"].attrs.create(
                "git_commit", data=numpy.array([b"f" * 40], dtype="S64"), dtype="S64"
            ),
        ),
        (
            "a permitted attribute with a changed shape",
            lambda handle: handle["RunProperties/Version"].attrs.create(
                "git_commit",
                data=numpy.array([b"f" * 40, b"x"], dtype="S128"),
                dtype=handle["RunProperties/Version"].attrs.get_id("git_commit").dtype,
            ),
        ),
        (
            "RunEndTime with a changed dtype",
            lambda handle: handle["RunProperties"].attrs.create(
                RUN_END_TIME, data=numpy.array([b"1999-01-01T00:00:00"], dtype="S19"), dtype="S19"
            ),
        ),
    ):
        mutated = candidate(f"attr_shape_{label.replace(' ', '_')}")
        with h5py.File(mutated.directory / master, "r+") as handle:
            mutate(handle)
        expect_failure(label, mutated, "only its value may differ")

    relaid = candidate("relayout")
    relayout_dataset(relaid.directory / master, "RunProperties/FieldMetadata")
    with h5py.File(relaid.directory / master, "r") as handle:
        moved = handle["RunProperties/FieldMetadata"][()]
    with h5py.File(base.directory / master, "r") as original:
        kept = original["RunProperties/FieldMetadata"][()]
    assert all(moved[f].tobytes() == kept[f].tobytes() for f in kept.dtype.names)
    assert moved.dtype != kept.dtype, "the relayout produced the same dtype"
    expect_failure(
        "a compound dataset with identical values but a different layout",
        relaid,
        "RunProperties/FieldMetadata: dataset dtype differs",
    )

    prefixed_base = copy_run(
        source, scratch / "prefixed_base", str(scratch / "prefixed_base_prefix")
    )
    prefixed = copy_run(source, scratch / "prefixed", str(scratch / "prefixed_prefix"))
    for run in (prefixed_base, prefixed):
        path = run.directory / "metadata" / "test_simulation.yaml"
        path.write_text(path.read_text() + f"# built in {run.prefixes[0]}/x\n")
    report = compare_outputs(prefixed_base, prefixed)
    if report.errors or "metadata/test_simulation.yaml: path prefix only" not in report.permitted:
        raise AssertionError(f"a path-prefix-only difference was not accepted: {report.errors[:3]}")
    results.append("a path-prefix-only metadata difference")
    log("  control accepted: a path-prefix-only metadata difference")

    # A path-valued string attribute is tolerated on RunProperties only.
    for where, accepted in (("RunProperties", True), ("RunProperties/Version", False)):
        for run in (prefixed_base, prefixed):
            with h5py.File(run.directory / master, "r+") as handle:
                handle[where].attrs.create(
                    "PathNote", data=f"{run.prefixes[0]}/x".encode(), dtype="S1024"
                )
        report = compare_outputs(prefixed_base, prefixed)
        label = "path-valued attribute RunProperties@PathNote (path prefix only)"
        if accepted and (
            label not in report.permitted or any("PathNote" in e for e in report.errors)
        ):
            raise AssertionError(
                f"a RunProperties path-prefix attribute was rejected: {report.errors[:3]}"
            )
        if not accepted and not any(f"{where}@PathNote" in e for e in report.errors):
            raise AssertionError(f"a path-prefix attribute on {where} was accepted")
        for run in (prefixed_base, prefixed):
            with h5py.File(run.directory / master, "r+") as handle:
                del handle[where].attrs["PathNote"]
        results.append(f"a path-prefix string attribute on {where}")
        log(
            f"  {'control accepted' if accepted else 'mutation rejected'}: path-prefix attribute on {where}"
        )

    # The empty-list line is accepted only in the explicit-empty leg, and only when present.
    listed = candidate("empty_list")
    yaml_path = listed.directory / "metadata" / RUN_FILE_NAME
    yaml_path.write_text(empty_list_text(yaml_path.read_text()))
    expect_accepted(
        "the added post_snapshot: [] line (empty-list leg)",
        listed,
        [f"metadata/{RUN_FILE_NAME}: added '{EMPTY_LIST_LINE.strip()}' line"],
        empty_leg=True,
    )
    expect_failure(
        "the added empty list outside the empty-list leg",
        listed,
        "content differs beyond the path prefix",
    )
    expect_failure(
        "an empty-list leg without the added line",
        candidate("empty_missing"),
        "more than the added empty list",
        empty_leg=True,
    )
    other_line = candidate("empty_other")
    yaml_path = other_line.directory / "metadata" / RUN_FILE_NAME
    yaml_path.write_text(yaml_path.read_text().replace("SubSteps: 10", "SubSteps: 11"))
    expect_failure(
        "another run-file change in the empty-list leg",
        other_line,
        "more than the added empty list",
        empty_leg=True,
    )

    log(f"comparator self-check: {len(results)} mutation/control cases behaved as required")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--models", nargs="+", choices=MODELS, default=list(MODELS))
    parser.add_argument(
        "--fixtures",
        nargs="+",
        choices=[f.name for f in FIXTURES],
        default=[f.name for f in FIXTURES],
    )
    parser.add_argument("--schemes", nargs="+", choices=SCHEMES, default=list(SCHEMES))
    return parser.parse_args(argv)


def main() -> int:
    identity = Identity(parse_args())
    # Ordered stages: each needs the one before it, so a failure stops the chain
    # (the remaining stages report SKIP, which fails the make target) rather than
    # cascading into errors that bury the cause.
    return run_test_suite(
        identity.stages(), "Snapshot-global disabled-mode identity", abort_on_failure=True
    )


if __name__ == "__main__":
    sys.exit(main())
