#!/usr/bin/env python3
"""
Disabled-mode identity of the snapshot-global feature against its pre-feature reference.

A manual acceptance test (tests/manual/ is never globbed into ``make tests``)::

    make tests-snapshot-global-identity [REFERENCE_COMMIT=<hash>]

It proves that, with ``modules.post_snapshot`` absent or an explicit empty list, HEAD produces
what the pinned pre-feature reference commit produces. Per leg, on committed fixtures only: a
baseline run of the reference build, a feature-absent run of the HEAD build on the very same run
file, and a feature-empty run with ``post_snapshot: []`` added under ``modules:``. The matrix is
{halos-only, sage16} x {vertical mini-Millennium, version 2 horizontal, gapped version 3
horizontal} x {fixed, dynamic}: twelve legs, each derived from the shipped
``models/sage16/input/sage16_mini-millennium.yaml`` with only its selectors substituted.

Each feature leg must match the baseline: per-ID byte identity of every Galaxies field (through
the unchanged scripts/compare_cross_format_identity.py), the same files, HDF5 objects, master-file
links and attributes, byte-equal datasets and ``metadata/output_schema.json``. The only permitted
differences are named provenance (``RunProperties/Version`` build attributes, ``RunEndTime``,
``metadata/version_info.json``, worktree or scratch path prefixes), in the feature-empty leg the
one added run-YAML line, and one pinned content delta against the current reference: the
``UniqueGalaxyID`` description text (``PINNED_DESCRIPTION_FIELD`` below), which is bound to its
exact before and after wording. A mutation self-check proves the comparator rejects each defect.

Build worktrees are cached by commit under ``output/snapshot-global-identity/worktrees/`` (the
gates' scratch parent); runs, logs, mutations and ``evidence.json`` are archived under
``archive/snapshot-global-identity/<stamp>/``. Nothing is ever removed by this test. The
console output is also written to ``build/snapshot_global_identity.log``.

Contract: Slice 4 of the snapshot-global modules implementation plan (docs/dev, at acceptance).
"""

from __future__ import annotations

import contextlib
import datetime
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

import h5py
import numpy
import yaml

TESTS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(TESTS_DIR))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

# The per-ID Galaxies comparison is the one cross-format implementation, imported unchanged.
import compare_cross_format_identity as comparator  # noqa: E402
from framework import check_no_memory_leaks, run_test_suite  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    ParityGate,
    build_pair,
    point_output_at,
    run_logged,
)

#: The pre-feature reference: the last commit before the snapshot-global feature touched a
#: runtime path. Re-anchor procedure: only when a validated, deliberate change to the shipped
#: template or the fixtures makes the old reference unusable, pick the new commit, confirm it
#: is an ancestor of HEAD that `git grep -E "$FEATURE_SYMBOLS" <commit> -- src scripts models`
#: does not match, run this test with REFERENCE_COMMIT=<new> until it passes, then replace
#: this full 40-character hash, record the move (old -> new, date, reason) here, and reset the
#: pinned description delta below (a fresh reference carries the current description).
REFERENCE_COMMIT_DEFAULT = "501bac12f654d9622b797bc9b26c536e5385aca2"

FEATURE_PATHS = ("src", "scripts", "models")
FEATURE_SYMBOLS = r"process_snapshot|PROCESSING_MODE_SNAPSHOT|SnapshotContext|post_snapshot"

#: Everything a leg's worktree builds or reads, for the uncommitted-edit check.
RUNTIME_PATHS = (*FEATURE_PATHS, "simulations", "Makefile")

#: The shipped SAGE run file every leg derives from.
TEMPLATE = "models/sage16/input/sage16_mini-millennium.yaml"

#: Run-file name and output location shared by every run, so the copied run YAML in
#: ``metadata/`` of the baseline and feature-absent runs is byte-identical.
RUN_FILE_NAME = "identity.yaml"
OUTPUT_DIRECTORY = "output/identity"
OUTPUT_BASENAME = "model"
MASTER = f"{OUTPUT_BASENAME}.hdf5"

MODELS = ("halos-only", "sage16")
SCHEMES = ("fixed", "dynamic")
TREES = ("reference", "feature")

#: Datasets the contract names for byte equality, relative to ``RunProperties``.
NAMED_DATASETS = ("Parameters", "FieldMetadata", "EnabledModules", "EventContracts", "Redshifts")

#: ``RunProperties/Version`` attributes that necessarily differ between two builds.
VERSION_ATTRIBUTES = ("version", "git_commit", "git_branch", "git_date", "build_date")

#: ``RunProperties`` attribute that necessarily differs between two runs.
RUN_END_TIME = "RunEndTime"

#: The modules line and the one line the feature-empty leg adds after it.
MODULES_LINE = b"\nmodules:\n"
EMPTY_LIST_LINE = b"  post_snapshot: []\n"
EMPTY_LIST_LABEL = f"metadata/{RUN_FILE_NAME}: added 'post_snapshot: []' line"

#: The one pinned content delta against the current reference, recorded 2026-10-04: the core
#: UniqueGalaxyID property's description gained the created-record namespace (strictly negative
#: IDs), so every HDF5 file's FieldMetadata row and the run-local output_schema.json field carry
#: the AFTER text where the reference carries the BEFORE text, and output_schema.json's
#: ``source_md5`` (the digest of the property metadata) follows it. Nothing else about the
#: property, the table or the schema may differ; both texts are bound exactly.
PINNED_DESCRIPTION_FIELD = "UniqueGalaxyID"
UNIQUE_ID_DESCRIPTION_BEFORE = (
    "Persistent run-scoped unique galaxy identifier across all snapshots "
    "(creation_halonr + multiplier * (forestnr_global + 1), where multiplier is "
    "simulation.unique_galaxy_id_multiplier, default 10^9, provenance attribute "
    "UniqueGalaxyIDMultiplier)"
)
UNIQUE_ID_DESCRIPTION_AFTER = (
    "Persistent run-scoped unique galaxy ID, creation_halonr + multiplier * "
    "(forestnr_global + 1) for tree rows (multiplier = simulation.unique_galaxy_id_multiplier, "
    "default 10^9) and strictly negative -(1 + ordinal + 1024 * host_key) for created records"
)
DESCRIPTION_LABEL = "RunProperties/FieldMetadata: pinned UniqueGalaxyID description"
SCHEMA_DESCRIPTION_LABEL = (
    "metadata/output_schema.json: pinned UniqueGalaxyID description and its source_md5"
)

#: Errors kept per comparison in evidence.json and logged per failed leg; the count is exact.
ERROR_CAP = 200

#: Worktree registrations of this test above which a clean-up reminder is raised.
REGISTRATION_WARNING = 50

BUILD_TIMEOUT_S = 3600
RUN_TIMEOUT_S = 3600
GIT_TIMEOUT_S = 600

# Always under the repository's gitignored output/ (created if absent): the documented cache
# location and clean-up procedure assume it, so no fallback to the system tmp directory.
WORKTREE_ROOT = REPO_ROOT / "output" / "snapshot-global-identity" / "worktrees"
ARCHIVE_ROOT = REPO_ROOT / "archive" / "snapshot-global-identity"
LOG_PATH = REPO_ROOT / "build" / "snapshot_global_identity.log"

STARTED = time.monotonic()


def log(message: str) -> None:
    """Print one progress line prefixed with elapsed wall time, unbuffered."""
    elapsed = int(time.monotonic() - STARTED)
    print(f"[identity {elapsed // 60:3d}m{elapsed % 60:02d}s] {message}", flush=True)


# ---------------------------------------------------------------------------
# Fixtures, git and the reference commit
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Fixture:
    """One committed input route: the simulation package and the config naming its data."""

    name: str
    simulation: str
    config: str


FIXTURES = (
    Fixture("default", "mini-millennium", "tests/data/test_simulation.yaml"),
    Fixture(
        "v2",
        "micro-uchuu-ascii-horizontal",
        "simulations/micro-uchuu-ascii-horizontal/_tests/input/test_simulation.yaml",
    ),
    Fixture(
        "v3-gapped",
        "mini-millennium-horizontal",
        "simulations/mini-millennium-horizontal/_tests/input/test_simulation.yaml",
    ),
)


def leg_name(model: str, fixture: Fixture, scheme: str) -> str:
    return f"{model}__{fixture.name}__{scheme}".replace("-", "_")


def git(*args: str, check: bool = True, cwd: Path = REPO_ROOT) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=GIT_TIMEOUT_S
    )
    if check and completed.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {completed.stderr.strip()}")
    return completed


def has_feature_symbols(commit: str) -> bool:
    """True when the commit's src/, scripts/ or models/ name any feature symbol."""
    completed = git("grep", "-q", "-E", FEATURE_SYMBOLS, commit, "--", *FEATURE_PATHS, check=False)
    if completed.returncode not in (0, 1):
        raise AssertionError(f"git grep at {commit} failed: {completed.stderr.strip()}")
    return completed.returncode == 0


def required_paths(commit: str) -> list[str]:
    """Every path the legs select by name or through a fixture config, at ``commit``."""
    paths = [TEMPLATE, *(f"models/{model}/model_properties.yaml" for model in MODELS)]
    for fixture in FIXTURES:
        paths += [f"simulations/{fixture.simulation}/simulation_info.yaml", fixture.config]
        text = git("show", f"{commit}:{fixture.config}").stdout
        section = (yaml.safe_load(text) or {}).get("input") or {}
        paths += [
            os.path.normpath(section[key]) for key in ("simulation_dir", "snapshot_list_file")
        ]
    return paths


def resolve_reference() -> tuple[str, str, str]:
    """(reference, HEAD, source) after every check that protects the result."""
    explicit = os.environ.get("REFERENCE_COMMIT") or None
    value = explicit or REFERENCE_COMMIT_DEFAULT
    completed = git("rev-parse", "--verify", f"{value}^{{commit}}", check=False)
    if completed.returncode != 0:
        raise AssertionError(f"reference {value!r} is not a commit in this repository")
    reference, head = completed.stdout.strip(), git("rev-parse", "HEAD").stdout.strip()
    if git("merge-base", "--is-ancestor", reference, head, check=False).returncode != 0:
        raise AssertionError(f"reference {reference} is not an ancestor of HEAD {head}")
    if has_feature_symbols(reference):
        raise AssertionError(f"reference {reference} already contains a feature symbol")
    if not has_feature_symbols(head):
        raise AssertionError(f"HEAD {head} contains no feature symbol; it is not a feature tree")
    for commit in (reference, head):
        for path in required_paths(commit):
            if git("cat-file", "-e", f"{commit}:{path}", check=False).returncode != 0:
                raise AssertionError(f"{commit} lacks {path}, which the legs select")
    return reference, head, "REFERENCE_COMMIT" if explicit else "pinned default"


def runtime_tree_differences() -> list[str]:
    """Uncommitted or untracked edits to anything the worktrees build and run from.

    The legs build HEAD, so such an edit would not be tested. Module test envelopes are excluded.
    """
    changed = git("diff", "--name-only", "HEAD", "--", *RUNTIME_PATHS).stdout.split()
    untracked = git(
        "ls-files", "--others", "--exclude-standard", "--", *RUNTIME_PATHS
    ).stdout.split()
    return [
        path
        for path in changed + untracked
        if not (path.startswith("models/") and "/_tests/" in path)
    ]


def worktree_env(worktree: Path, model: str, simulation: str) -> dict:
    """The gates' worktree environment, minus the caller's make state.

    ``make TEST_BUILD=yes tests-snapshot-global-identity`` would otherwise reach both worktree
    builds through MAKEFLAGS, and the evidence would describe a build other than the one claimed.
    """
    env = ParityGate.worktree_env(worktree, model, simulation)
    for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL"):
        env.pop(key, None)
    return env


def cached_worktree(commit: str, model: str, simulation: str) -> Path:
    """The worktree for one commit and pair, created only when missing and never removed."""
    path = WORKTREE_ROOT / f"{commit[:12]}__{model}__{simulation}"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        git("worktree", "add", "--detach", str(path), commit)
        ParityGate.link_machine_local(path)
        log(f"  created worktree {path}")
    else:
        log(f"  reusing worktree {path}")
    head = git("rev-parse", "HEAD", cwd=path, check=False)
    dirty = git("diff", "--quiet", "HEAD", cwd=path, check=False).returncode
    if head.returncode != 0 or head.stdout.strip() != commit or dirty != 0:
        raise AssertionError(
            f"FATAL: cached worktree {path} is not a clean checkout of {commit} "
            f"(HEAD {head.stdout.strip() or 'unreadable'}, tracked edits: {dirty != 0}); it is not "
            "reused. Move it to cold storage, run `git worktree prune`, and re-run."
        )
    return path


def registration_warning() -> str | None:
    """A clean-up reminder when this test's worktree registrations pile up."""
    listing = git("worktree", "list", "--porcelain").stdout.splitlines()
    count = sum(
        1 for line in listing if line.startswith("worktree ") and "snapshot-global-identity" in line
    )
    log(f"  {count} snapshot-global-identity worktree registration(s)")
    if count <= REGISTRATION_WARNING:
        return None
    return (
        f"{count} snapshot-global-identity worktree registrations (more than "
        f"{REGISTRATION_WARNING}): move old directories under {WORKTREE_ROOT} to cold storage, "
        "then run `git worktree prune`"
    )


# ---------------------------------------------------------------------------
# Run files
# ---------------------------------------------------------------------------


def substitute_once(text: str, pattern: str, replacement: str, what: str) -> str:
    result, count = re.subn(pattern, lambda _match: replacement, text, flags=re.MULTILINE)
    if count != 1:
        raise AssertionError(f"shipped run file: {what} matched {count} time(s), expected 1")
    return result


def with_empty_list(raw: bytes) -> bytes:
    """``raw`` with ``post_snapshot: []`` added as the first key under ``modules:``."""
    return raw.replace(MODULES_LINE, MODULES_LINE + EMPTY_LIST_LINE, 1)


def run_file_text(template: str, model: str, fixture: Fixture, scheme: str) -> str:
    """The leg's run file: the shipped SAGE file with only its selectors changed.

    The ``modules`` mapping and every parameter stay verbatim; halos-only replaces the mapping
    with an empty pipeline.
    """
    text = template
    for pattern, replacement, what in (
        (r"^model:\n  name: sage16\n", f"model:\n  name: {model}\n", "model"),
        (
            r"^simulation:\n  name: mini-millennium\n",
            f"simulation:\n  name: {fixture.simulation}\n  config: {fixture.config}\n",
            "simulation",
        ),
        (r"^  output_directory: .*$", f"  output_directory: {OUTPUT_DIRECTORY}", "output"),
        (r"^  snapshot_list: .*$", "  snapshot_list: []", "snapshots"),
        (
            r"^SubSteps: 10\n",
            f"SubSteps: 10\nMaxDynamicSubsteps: 200\nTimestepScheme: {scheme}\n",
            "substeps",
        ),
    ):
        text = substitute_once(text, pattern, replacement, what)
    if text.encode().count(MODULES_LINE) != 1:
        raise AssertionError("shipped run file does not carry exactly one modules: line")
    if model == "halos-only":
        text = text[: text.index("\nmodules:\n") + 1] + "modules:\n  phases: {}\n  parameters: {}\n"
    return text


def check_run_file(text: str, shipped: dict, model: str) -> None:
    """Nothing but the selectors and the two scheme keys changed, and no post_snapshot key."""
    parsed = yaml.safe_load(text)
    assert "post_snapshot" not in parsed["modules"], "the baseline run file must not carry the key"
    expected_keys = set(shipped) | {"MaxDynamicSubsteps", "TimestepScheme"}
    keys_message = f"top-level keys {sorted(parsed)} != {sorted(expected_keys)}"
    assert set(parsed) == expected_keys, keys_message
    if model == "sage16":
        assert parsed["modules"] == shipped["modules"], "the SAGE modules mapping was altered"
    else:
        assert parsed["modules"] == {"phases": {}, "parameters": {}}


# ---------------------------------------------------------------------------
# Output comparison
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class OutputRun:
    """A finished run's output directory, the path prefixes its files may embed, and the
    property-metadata digest its build generated (``build/generated/property_hash.txt`` of
    the producing worktree), which its ``output_schema.json`` must carry as ``source_md5``."""

    directory: Path
    prefixes: tuple[str, ...]
    source_md5: str | None = None

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


def pinned_description_delta(left: numpy.ndarray, right: numpy.ndarray) -> bool:
    """Two FieldMetadata tables that differ only in the pinned description cell (before -> after)."""
    if left.dtype != right.dtype or left.shape != right.shape or left.dtype.names is None:
        return False
    if set(left.dtype.names) != {"field_name", "units", "description"}:
        return False
    for column in ("field_name", "units"):
        if canonical_bytes(left[column]) != canonical_bytes(right[column]):
            return False
    changed = [i for i in range(len(left)) if left["description"][i] != right["description"][i]]
    if len(changed) != 1:
        return False
    row = changed[0]
    return (
        left["field_name"][row].rstrip(b"\0") == PINNED_DESCRIPTION_FIELD.encode()
        and left["description"][row].rstrip(b"\0") == UNIQUE_ID_DESCRIPTION_BEFORE.encode()
        and right["description"][row].rstrip(b"\0") == UNIQUE_ID_DESCRIPTION_AFTER.encode()
    )


def pinned_schema_delta(left: bytes, right: bytes, base: OutputRun, other: OutputRun) -> bool:
    """Two output_schema.json texts that differ only in the pinned description and source_md5.

    ``source_md5`` is the digest of the generator's inputs, so it moves with the description;
    each side's value must be the digest its own build generated, never an arbitrary one.
    """
    try:
        before, after = json.loads(left), json.loads(right)
    except ValueError:
        return False
    if not isinstance(before, dict) or not isinstance(after, dict):
        return False
    for schema, run in ((before, base), (after, other)):
        if run.source_md5 is None or schema.get("source_md5") != run.source_md5:
            return False
    pinned = [
        [f for f in schema.get("fields", []) if f.get("name") == PINNED_DESCRIPTION_FIELD]
        for schema in (before, after)
    ]
    if len(pinned[0]) != 1 or len(pinned[1]) != 1:
        return False
    if (
        pinned[0][0].get("description") != UNIQUE_ID_DESCRIPTION_BEFORE
        or pinned[1][0].get("description") != UNIQUE_ID_DESCRIPTION_AFTER
    ):
        return False
    pinned[0][0]["description"] = pinned[1][0]["description"]
    before["source_md5"] = after.get("source_md5")
    return before == after


def compare_hdf5_file(rel: str, base: OutputRun, other: OutputRun, report: Report) -> None:
    """Links, objects, attributes and non-Galaxies dataset bytes of one HDF5 file pair."""
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
            elif not path.endswith("/Galaxies"):  # Galaxies are compared per ID
                leaf = path.removeprefix("RunProperties/")
                if path.startswith("RunProperties/") and leaf in NAMED_DATASETS:
                    report.datasets_compared.append(leaf)
                if canonical_bytes(left[()]) != canonical_bytes(right[()]):
                    if leaf == "FieldMetadata" and pinned_description_delta(left[()], right[()]):
                        report.allow(DESCRIPTION_LABEL)
                    else:
                        report.error(f"{rel}:{path}: dataset bytes differ")


def compare_galaxies(base: OutputRun, other: OutputRun, report: Report) -> None:
    """Per-ID byte identity through the cross-format comparator; its report lines are errors."""
    left = comparator.scan_run(str(base.directory / OUTPUT_BASENAME))
    right = comparator.scan_run(str(other.directory / OUTPUT_BASENAME))
    labels = ("baseline", "candidate")

    def captured(call) -> tuple[int, str]:
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            failures = call()
        return failures, " | ".join(line.strip() for line in buffer.getvalue().splitlines())

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
        failures, text = captured(
            lambda s=snap, a=records_left, b=records_right: comparator.compare_snapshot(
                s, a, b, labels, ERROR_CAP
            )
        )
        if failures:
            report.error(f"galaxies: {text}")
            report.galaxy_differences += failures
        else:
            report.galaxies += records_left.size


def compare_metadata_file(rel: str, base: OutputRun, other: OutputRun, empty_leg: bool, report):
    left, right = (base.directory / rel).read_bytes(), (other.directory / rel).read_bytes()
    if rel == "metadata/version_info.json":
        if left != right:
            report.allow(rel)
        for label, raw in (("baseline", left), ("candidate", right)):
            if not isinstance(json.loads(raw), dict):
                report.error(f"{rel}: {label} is not a JSON object")
        return
    if rel == f"metadata/{RUN_FILE_NAME}" and empty_leg:
        if MODULES_LINE in left and right == with_empty_list(left):
            report.allow(EMPTY_LIST_LABEL)
        else:
            report.error(f"{rel}: differs from the baseline in more than the added empty list")
        return
    if left == right:
        return
    if rel == "metadata/output_schema.json":
        if pinned_schema_delta(left, right, base, other):
            report.allow(SCHEMA_DESCRIPTION_LABEL)
        else:
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
            compare_hdf5_file(rel, base, other, report)
        elif rel.startswith("metadata/"):
            compare_metadata_file(rel, base, other, empty_leg, report)
        elif (base.directory / rel).read_bytes() != (other.directory / rel).read_bytes():
            report.error(f"{rel}: content differs")
    try:
        compare_galaxies(base, other, report)
    except comparator.ComparisonError as error:
        report.error(f"galaxy comparison could not read an output: {error}")
    if report.galaxies <= 0 and not report.errors:
        report.error("no galaxies compared")
    return report


# ---------------------------------------------------------------------------
# The identity run
# ---------------------------------------------------------------------------


@dataclass
class RunRecord:
    tree: str
    variant: str
    output: OutputRun
    elapsed_s: float
    attributes: dict


class Identity:
    """State and stages of one identity run. Nothing it creates is ever deleted."""

    def __init__(self):
        self.root: Path | None = None
        self.commits: dict[str, str] = {}
        self.worktrees: dict[tuple[str, str, str], Path] = {}
        self.template: str | None = None
        self.verdicts: dict[str, str] = {}
        self.mutation_source: OutputRun | None = None
        self.evidence: dict = {"legs": {}, "timings_s": {}}

    def scratch(self, *parts: str) -> Path:
        assert self.root is not None, "the reference stage has not created the archive root"
        path = self.root.joinpath(*parts)
        path.mkdir(parents=True, exist_ok=True)
        return path

    def save_evidence(self) -> None:
        if self.root is not None:
            (self.root / "evidence.json").write_text(json.dumps(self.evidence, indent=2) + "\n")

    def test_reference_commit_resolution(self) -> None:
        """The reference is a valid pre-feature ancestor and the HEAD tree is what is built."""
        reference, head, source = resolve_reference()
        log(f"HEAD (feature tree): {head}")
        log(f"reference ({source}): {reference}")
        edits = runtime_tree_differences()
        if edits:
            raise AssertionError(f"runtime paths differ from HEAD (uncommitted): {edits}")
        shipped = {
            commit: git("show", f"{commit}:{TEMPLATE}").stdout for commit in (reference, head)
        }
        if shipped[reference] != shipped[head]:
            raise AssertionError(f"{TEMPLATE} differs between the reference and the feature tree")
        self.template = shipped[head]
        self.commits = {"reference": reference, "feature": head}
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        self.root = ARCHIVE_ROOT / f"{stamp}-ref{reference[:8]}-head{head[:8]}"
        self.root.mkdir(parents=True, exist_ok=False)
        self.evidence.update(
            {
                "head": head,
                "reference": reference,
                "reference_source": source,
                "archive": str(self.root),
                "worktrees": str(WORKTREE_ROOT),
            }
        )
        self.save_evidence()
        log(f"archive (runs, logs, evidence): {self.root}")
        log(f"worktree cache: {WORKTREE_ROOT}")

    def test_builds_name_their_commits(self) -> str | None:
        """One cached worktree per tree x model x simulation, built incrementally at its commit."""
        if not self.commits:
            raise AssertionError("prerequisite stage (reference) did not complete")
        logs = self.scratch("logs")
        for tree in TREES:
            commit = self.commits[tree]
            for model in MODELS:
                for simulation in (fixture.simulation for fixture in FIXTURES):
                    worktree = cached_worktree(commit, model, simulation)
                    key = f"{tree}__{model}__{simulation}"
                    started = time.monotonic()
                    env = worktree_env(worktree, model, simulation)
                    build_pair(worktree, model, simulation, env, logs, key, BUILD_TIMEOUT_S)
                    self.evidence["timings_s"][f"build:{key}"] = round(
                        time.monotonic() - started, 1
                    )
                    self.worktrees[(tree, model, simulation)] = worktree
        self.save_evidence()
        return registration_warning()

    def execute(self, tree: str, model: str, fixture: Fixture, leg: str, variant: str, run_file):
        """Run one executable from its worktree into its own archived output root."""
        worktree = self.worktrees[(tree, model, fixture.simulation)]
        output_root = self.scratch("runs", f"{tree}__{leg}__{variant}")
        point_output_at(worktree, output_root)
        log_path = self.scratch("logs") / f"{tree}__{leg}__{variant}-run.log"
        elapsed = run_logged(
            ["./mimic", str(run_file)],
            worktree,
            worktree_env(worktree, model, fixture.simulation),
            log_path,
            f"{tree}/{variant} {leg}: mimic run",
            RUN_TIMEOUT_S,
        )
        if not check_no_memory_leaks(log_path.read_text(errors="replace")):
            raise AssertionError(f"{tree}/{variant} {leg}: the run reported a memory leak")
        directory = output_root / Path(OUTPUT_DIRECTORY).name
        prefixes = {str(worktree), str(worktree.resolve()), str(output_root)}
        prefixes.add(str(output_root.resolve()))
        if not (directory / MASTER).is_file():
            raise AssertionError(f"{tree}/{variant} {leg}: no master file at {directory / MASTER}")
        attributes = {}
        with h5py.File(directory / MASTER, "r") as handle:
            for path, name in (
                ("RunProperties/Version", "git_commit"),
                ("RunProperties", "TimestepScheme"),
                ("RunProperties", "ModelName"),
                ("RunProperties", "SimulationName"),
            ):
                attributes[name] = attribute_text(attribute_signature(handle[path].attrs[name]))
        digest_file = worktree / "build" / "generated" / "property_hash.txt"
        if not digest_file.is_file():
            raise AssertionError(f"{tree}/{variant} {leg}: no generated digest at {digest_file}")
        output = OutputRun(directory, tuple(prefixes), digest_file.read_text().strip())
        return RunRecord(tree, variant, output, elapsed, attributes)

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
        try:
            self.run_leg(name, model, fixture, scheme)
        except BaseException as error:
            first = str(error).splitlines()[0] if str(error) else type(error).__name__
            self.verdicts[name] = f"FAIL: {first}"
            raise
        finally:
            self.evidence["legs"].setdefault(name, {})["verdict"] = self.verdicts.get(name)
            self.save_evidence()

    def run_leg(self, name: str, model: str, fixture: Fixture, scheme: str) -> None:
        missing = [
            tree for tree in TREES if (tree, model, fixture.simulation) not in self.worktrees
        ]
        if self.template is None or missing:
            raise AssertionError(f"prerequisite stages did not complete (no {missing} build)")
        text = run_file_text(self.template, model, fixture, scheme)
        check_run_file(text, yaml.safe_load(self.template), model)
        files = {}
        for variant, body in (("absent", text), ("empty", with_empty_list(text.encode()).decode())):
            files[variant] = self.scratch("run-files", name, variant) / RUN_FILE_NAME
            files[variant].write_text(body)
        assert yaml.safe_load(files["empty"].read_text())["modules"]["post_snapshot"] == []
        evidence = self.evidence["legs"].setdefault(name, {})
        evidence.update({"model": model, "fixture": fixture.name, "scheme": scheme})
        started = time.monotonic()
        baseline = self.execute("reference", model, fixture, name, "baseline", files["absent"])
        absent = self.execute("feature", model, fixture, name, "absent", files["absent"])
        empty = self.execute("feature", model, fixture, name, "empty", files["empty"])
        for record in (baseline, absent, empty):
            self.check_provenance(record, model, fixture, scheme)
        evidence["run_seconds"] = {
            r.variant: round(r.elapsed_s, 1) for r in (baseline, absent, empty)
        }
        if (model, fixture.name, scheme) == ("sage16", "v2", "fixed"):
            self.mutation_source = absent.output
        failures = []
        for record, empty_leg in ((absent, False), (empty, True)):
            report = compare_outputs(baseline.output, record.output, empty_leg=empty_leg)
            evidence[f"compare_{record.variant}"] = {
                **report.summary(),
                "error_lines": report.errors[:ERROR_CAP],
            }
            log(
                f"  {name} {record.variant}: {report.galaxies} galaxies over {report.snapshots} "
                f"snapshot(s), {report.fields} fields, {report.links} external link(s), "
                f"{report.galaxy_differences} galaxy difference(s), {len(report.errors)} "
                f"error(s); permitted: {sorted(report.permitted)}"
            )
            failures += [f"{record.variant}: {error}" for error in report.errors]
        evidence["elapsed_s"] = round(time.monotonic() - started, 1)
        if failures:
            for line in failures[:ERROR_CAP]:
                log(f"  {name} FAILURE: {line}")
            if len(failures) > ERROR_CAP:
                log(f"  {name}: {len(failures) - ERROR_CAP} further failure(s) not logged")
            raise AssertionError(f"{name}: {len(failures)} failure(s); first: {failures[0]}")
        self.verdicts[name] = "PASS"

    def leg_stages(self) -> list[Callable[[], None]]:
        stages = []
        for model in MODELS:
            for fixture in FIXTURES:
                for scheme in SCHEMES:

                    def stage(m=model, f=fixture, s=scheme):
                        self.leg(m, f, s)

                    stage.__name__ = f"test_leg_{leg_name(model, fixture, scheme)}"
                    stages.append(stage)
        return stages

    def test_comparator_rejects_mutations(self) -> None:
        """The comparator fails on every injected defect and accepts only the permitted ones."""
        if self.mutation_source is None:
            raise AssertionError("the sage16 v2 fixed leg produced no output to mutate")
        count = run_mutation_checks(self.mutation_source, self.scratch("mutations"))
        self.evidence["mutation_cases"] = count
        self.save_evidence()

    def test_leg_verdicts(self) -> None:
        expected = [leg_name(m, f, s) for m in MODELS for f in FIXTURES for s in SCHEMES]
        verdicts = {name: self.verdicts.get(name, "did not run") for name in expected}
        for name, verdict in verdicts.items():
            log(f"  {name}: {verdict}")
        self.evidence["verdicts"] = verdicts
        self.evidence["total_elapsed_s"] = round(time.monotonic() - STARTED, 1)
        self.save_evidence()
        bad = [name for name, verdict in verdicts.items() if verdict != "PASS"]
        if bad:
            raise AssertionError(f"{len(bad)} of {len(expected)} legs did not pass: {bad}")
        log(f"all {len(expected)} legs passed; evidence: {self.root}/evidence.json")

    def stages(self) -> list[Callable[[], None]]:
        return [
            self.test_reference_commit_resolution,
            self.test_builds_name_their_commits,
            *self.leg_stages(),
            self.test_comparator_rejects_mutations,
            self.test_leg_verdicts,
        ]


# ---------------------------------------------------------------------------
# Comparator self-check (mutations of real output)
# ---------------------------------------------------------------------------

#: A change applied to fresh (base, candidate) copies of the mutation source.
Change = Callable[[OutputRun, OutputRun], None]


def snapshot_partition(run: OutputRun, snap: int) -> Path:
    return run.directory / f"{OUTPUT_BASENAME}_{snap:03d}.hdf5"


def with_master(run: OutputRun, change: Callable[[h5py.File], None]) -> None:
    """Apply ``change`` to a run's master file, opened for writing."""
    with h5py.File(run.directory / MASTER, "r+") as handle:
        change(handle)


def recreate_dataset(handle: h5py.File, name: str, data, dtype=None) -> None:
    """Replace a dataset with ``data``, keeping its attributes and their stored dtypes."""
    dataset = handle[name]
    attributes = [
        (key, dataset.attrs[key], dataset.attrs.get_id(key).dtype) for key in dataset.attrs
    ]
    del handle[name]
    created = handle.create_dataset(name, data=data, dtype=dtype)
    for key, value, stored in attributes:
        created.attrs.create(key, data=value, dtype=stored)


def rewrite_galaxies(run: OutputRun, snap: int, transform) -> None:
    """Replace a snapshot's Galaxies dataset with ``transform(rows)``."""
    with h5py.File(snapshot_partition(run, snap), "r+") as handle:
        name = f"Snap{snap:03d}/Galaxies"
        recreate_dataset(handle, name, transform(handle[name][()]))


def on_master(change: Callable[[h5py.File], None]) -> Change:
    return lambda _base, run: with_master(run, change)


def on_galaxies(snap: int, transform) -> Change:
    return lambda _base, run: rewrite_galaxies(run, snap, transform)


def on_text(rel: str, edit: Callable[[str], str]) -> Change:
    def change(_base: OutputRun, run: OutputRun) -> None:
        path = run.directory / rel
        path.write_text(edit(path.read_text()))

    return change


def set_attribute(path: str, name: str, value: bytes, dtype=None, count: int = 1):
    """A master-file change giving an attribute a new value (by default, same dtype and shape)."""

    def change(handle):
        width = dtype or handle[path].attrs.get_id(name).dtype
        handle[path].attrs.create(name, data=numpy.array([value] * count, dtype=width), dtype=width)

    return change


def alter_dataset(name: str):
    """A master-file change altering the first byte, or first field, of a named dataset."""

    def change(handle):
        data = handle[f"RunProperties/{name}"]
        values = data[()]
        if values.dtype.names:
            column = values[values.dtype.names[0]]
            head = column[0]
            if isinstance(head, (bytes, str)):
                column[0] = head[:0] + (b"X" if isinstance(head, bytes) else "X") + head[1:]
            else:
                column[0] = head + 1
        else:
            values.view(numpy.uint8).reshape(-1)[0] ^= 1
        data[...] = values

    return change


def relayout(handle: h5py.File) -> None:
    """Rewrite FieldMetadata with identical values but a different record layout."""
    name = "RunProperties/FieldMetadata"
    values = handle[name][()]
    old = values.dtype
    layout = numpy.dtype(
        {
            "names": list(old.names),
            "formats": [old.fields[item][0] for item in old.names],
            "offsets": [old.fields[item][1] + 8 for item in old.names],
            "itemsize": old.itemsize + 16,
        }
    )
    moved = numpy.zeros(values.shape, dtype=layout)
    for item in old.names:
        moved[item] = values[item]
    recreate_dataset(handle, name, moved, dtype=layout)
    stored = handle[name][()]
    assert all(stored[item].tobytes() == values[item].tobytes() for item in old.names)
    assert stored.dtype != old, "the relayout produced the same dtype"


def retarget_link(handle: h5py.File) -> None:
    """Point the first external Galaxies link at the second one's target."""
    links = sorted((n, t) for n, t in links_of(handle).items() if t[0] == "ExternalLink")
    if len(links) < 2:
        raise AssertionError(f"the master file has {len(links)} external link(s); need two")
    (name, _), (_, (_, filename, path)) = links[0], links[1]
    del handle[name]
    handle[name] = h5py.ExternalLink(filename, path)


def path_note(where: str, widths=(1024, 1024)) -> Change:
    """Both runs gain a path-valued string attribute embedding their own prefix."""

    def change(base: OutputRun, run: OutputRun) -> None:
        for target, width in zip((base, run), widths):
            note = f"{target.prefixes[0]}/x".encode()
            with_master(target, set_attribute(where, "PathNote", note, dtype=f"S{width}"))

    return change


def prefixed_metadata(base: OutputRun, run: OutputRun) -> None:
    """Both runs' copied simulation config gains a line embedding their own prefix."""
    for target in (base, run):
        path = target.directory / "metadata" / "test_simulation.yaml"
        path.write_text(path.read_text() + f"# built in {target.prefixes[0]}/x\n")


def provenance(_base: OutputRun, run: OutputRun) -> None:
    with_master(run, set_attribute("RunProperties/Version", "git_commit", b"f" * 40))
    with_master(run, set_attribute("RunProperties", RUN_END_TIME, b"1999-01-01T00:00:00"))
    info = run.directory / "metadata" / "version_info.json"
    info.write_text(info.read_text().replace('"run_date"', '"run_date_changed"'))


def field_metadata_rows(handle: h5py.File, field_name: str) -> list[int]:
    """Row indices of ``field_name`` in a file's RunProperties/FieldMetadata table."""
    values = handle["RunProperties/FieldMetadata"][()]
    return [
        i
        for i in range(len(values))
        if values["field_name"][i].rstrip(b"\0") == field_name.encode()
    ]


def set_description(field_name: str, text: str) -> Callable[[h5py.File], None]:
    """A master-file change giving one FieldMetadata row's description a new text."""

    def change(handle):
        rows = field_metadata_rows(handle, field_name)
        if len(rows) != 1:
            raise AssertionError(f"FieldMetadata holds {len(rows)} row(s) named {field_name}")
        values = handle["RunProperties/FieldMetadata"][()]
        values["description"][rows[0]] = text.encode()
        handle["RunProperties/FieldMetadata"][...] = values

    return change


def pinned_description_on_base(base: OutputRun, _run: OutputRun) -> None:
    """The baseline copy carries the reference's description: the one pinned delta. Both copies
    keep the source digest their build generated, which the comparison binds each side to."""
    for path in sorted(base.directory.glob("*.hdf5")):
        with h5py.File(path, "r+") as handle:
            if "RunProperties/FieldMetadata" in handle:
                set_description(PINNED_DESCRIPTION_FIELD, UNIQUE_ID_DESCRIPTION_BEFORE)(handle)
    schema_path = base.directory / "metadata" / "output_schema.json"
    schema = json.loads(schema_path.read_text())
    pinned = [f for f in schema["fields"] if f["name"] == PINNED_DESCRIPTION_FIELD]
    if len(pinned) != 1:
        raise AssertionError(f"output_schema.json names {len(pinned)} {PINNED_DESCRIPTION_FIELD}")
    pinned[0]["description"] = UNIQUE_ID_DESCRIPTION_BEFORE
    schema_path.write_text(json.dumps(schema, indent=2) + "\n")


def pinned_description_wrong_digest(base: OutputRun, run: OutputRun) -> None:
    """The pinned delta beside a candidate digest that is not the one its build generated."""
    pinned_description_on_base(base, run)
    schema_path = run.directory / "metadata" / "output_schema.json"
    schema = json.loads(schema_path.read_text())
    schema["source_md5"] = "f" * 32
    schema_path.write_text(json.dumps(schema, indent=2) + "\n")


def other_description_changed(handle: h5py.File) -> None:
    """A description change on a field other than the pinned one."""
    values = handle["RunProperties/FieldMetadata"][()]
    others = [
        i
        for i in range(len(values))
        if values["field_name"][i].rstrip(b"\0") != PINNED_DESCRIPTION_FIELD.encode()
    ]
    if not others:
        raise AssertionError("FieldMetadata has no field other than the pinned one")
    values["description"][others[0]] = b"X"
    handle["RunProperties/FieldMetadata"][...] = values


def unchanged(_base: OutputRun, _run: OutputRun) -> None:
    pass


@dataclass(frozen=True)
class Case:
    """One self-check row. With ``needle`` the comparison must fail with an error containing it
    (and name no permitted label containing ``unpermitted``); without, it must pass and name
    every label in ``permitted``."""

    name: str
    change: Change
    needle: str | None = None
    empty_leg: bool = False
    permitted: tuple[str, ...] = ()
    unpermitted: str = ""


def galaxy_rows(source: OutputRun) -> dict[int, numpy.ndarray]:
    """The UniqueGalaxyID column of every output snapshot of a run."""
    index = comparator.scan_run(str(source.directory / OUTPUT_BASENAME))
    return {
        snap: comparator.read_snapshot(index, snap, field=comparator.ID_FIELD)
        for snap in sorted(index.snapshots)
    }


def mutation_cases(source: OutputRun) -> list[Case]:
    """Every mutation and control, built from the source output's own snapshots and datasets."""
    ids = galaxy_rows(source)
    populated = [snap for snap, column in ids.items() if len(column) >= 2]
    if not populated:
        raise AssertionError("the mutation source has no snapshot with two galaxies")
    first = populated[0]
    # A galaxy moved to a snapshot that lacks its ID (IDs persist across snapshots, so a move
    # onto a snapshot already holding it would be reported as a duplicate instead).
    moves = [
        (a, b, row)
        for a, b in zip(populated, populated[1:])
        for row in range(len(ids[a]))
        if ids[a][row] not in set(ids[b])
    ]
    if not moves:
        raise AssertionError("no galaxy ID is absent from the next populated snapshot")
    origin, target, moved_row = moves[0]

    def duplicate(rows):
        rows = rows.copy()
        rows[comparator.ID_FIELD][1] = rows[comparator.ID_FIELD][0]
        return rows

    def perturb(rows):
        rows = rows.copy()
        column = rows["Mvir"].copy()
        column.view(numpy.uint8)[0] ^= 1
        rows["Mvir"] = column
        return rows

    def swap(rows):
        rows = rows.copy()
        for name in rows.dtype.names:
            if name != comparator.ID_FIELD:
                rows[name][[0, 1]] = rows[name][[1, 0]]
        return rows

    def wrong_snapshot(_base: OutputRun, run: OutputRun) -> None:
        with h5py.File(snapshot_partition(run, origin), "r") as handle:
            row = handle[f"Snap{origin:03d}/Galaxies"][()][moved_row : moved_row + 1]
        rewrite_galaxies(run, origin, lambda rows: numpy.delete(rows, moved_row))
        rewrite_galaxies(run, target, lambda rows: numpy.concatenate([rows, row]))

    def remove_metadata(_base: OutputRun, run: OutputRun) -> None:
        (run.directory / "metadata" / "test_simulation.yaml").unlink()

    run_yaml, prop, ver = f"metadata/{RUN_FILE_NAME}", "RunProperties", "RunProperties/Version"
    commit, end_time, sim_yaml = b"f" * 40, b"1999-01-01T00:00:00", "metadata/test_simulation.yaml"
    add_empty = on_text(run_yaml, lambda text: with_empty_list(text.encode()).decode())
    substeps = on_text(run_yaml, lambda text: text.replace("SubSteps: 10", "SubSteps: 11"))
    schema = on_text("metadata/output_schema.json", lambda text: text.replace('"', "'", 1))
    md5_only = on_text(
        "metadata/output_schema.json",
        lambda text: re.sub(r'"source_md5": "[0-9a-f]+"', '"source_md5": "' + "0" * 32 + '"', text),
    )
    bytes_needle = f"{prop}/FieldMetadata: dataset bytes differ"
    sim_changed = on_text(sim_yaml, lambda text: text + "# changed\n")
    extra = on_master(lambda h: h[prop].create_dataset("EventContractsExtra", data=numpy.zeros(1)))
    substeps_attr = on_master(lambda h: h[prop].attrs.__setitem__("SubSteps", 11))
    added_attr = on_master(lambda h: h[prop].attrs.__setitem__("Extra", 1))
    model_name = on_master(set_attribute(prop, "ModelName", b"sage16x", "S7"))
    commit_dtype = on_master(set_attribute(ver, "git_commit", commit, "S64"))
    commit_shape = on_master(set_attribute(ver, "git_commit", commit, count=2))
    end_dtype = on_master(set_attribute(prop, RUN_END_TIME, end_time, "S19"))
    provenance_labels = (
        f"{ver}@git_commit",
        f"{prop}@{RUN_END_TIME}",
        "metadata/version_info.json",
    )
    label = f"path-valued attribute {prop}@PathNote (path prefix only)"
    note_ver, note_prop, prefix_only = (
        f"{ver}@PathNote",
        f"{prop}@PathNote",
        f"{sim_yaml}: path prefix only",
    )
    narrow_note, relaid = path_note(prop, (1024, 512)), on_master(relayout)
    layout_needle = f"{prop}/FieldMetadata: dataset dtype differs"
    more, only_value = "more than the added empty list", "only its value may differ"
    rows = [
        ("an untouched copy", unchanged, None),
        ("a dataset rewritten with identical rows", on_galaxies(first, lambda rows: rows), None),
        ("a dropped ID", on_galaxies(first, lambda rows: rows[:-1]), "UniqueGalaxyID sets differ"),
        ("a duplicated ID", on_galaxies(first, duplicate), "duplicated UniqueGalaxyID"),
        ("a field perturbed by one bit", on_galaxies(first, perturb), "Mvir: 1 record(s) differ"),
        ("payloads swapped between two IDs", on_galaxies(first, swap), "record(s) differ"),
        ("a galaxy in the wrong snapshot", wrong_snapshot, f"Snap{origin:03d}: UniqueGalaxyID"),
    ]
    with h5py.File(source.directory / MASTER, "r") as handle:
        sizes = {n: handle[f"{prop}/{n}"].size for n in NAMED_DATASETS if f"{prop}/{n}" in handle}
    for name, size in sizes.items():
        delete = on_master(lambda h, n=name: h.__delitem__(f"{prop}/{n}"))
        rows.append((f"missing dataset {name}", delete, "HDF5 objects differ"))
        if size:
            needle = f"{prop}/{name}: dataset bytes differ"
            rows.append((f"altered dataset {name}", on_master(alter_dataset(name)), needle))
        else:
            log(f"  dataset {name} is empty here; no byte to alter")
    rows += [
        ("an extra dataset", extra, "HDF5 objects differ"),
        ("an altered RunProperties attribute", substeps_attr, f"{prop}@SubSteps"),
        ("an altered non-permitted string attribute", model_name, f"{prop}@ModelName"),
        ("an added attribute", added_attr, "attribute names differ"),
        ("an external link retargeted", on_master(retarget_link), "external link or link type"),
        ("an altered output schema", schema, "output schema differs"),
        ("source_md5 changed alone", md5_only, "output schema differs"),
        (
            "the pinned UniqueGalaxyID description delta",
            pinned_description_on_base,
            None,
            False,
            (DESCRIPTION_LABEL, SCHEMA_DESCRIPTION_LABEL),
        ),
        (
            "the pinned description beside a digest its build did not generate",
            pinned_description_wrong_digest,
            "output schema differs",
        ),
        (
            "a description changed on another field",
            on_master(other_description_changed),
            bytes_needle,
        ),
        (
            "the pinned field described with other text",
            on_master(set_description(PINNED_DESCRIPTION_FIELD, "X")),
            bytes_needle,
        ),
        ("a metadata file changed beyond its path prefix", sim_changed, "beyond the path prefix"),
        ("a missing metadata file", remove_metadata, "file sets differ"),
        ("permitted provenance differences", provenance, None, False, provenance_labels),
        ("a permitted attribute with a changed dtype", commit_dtype, only_value),
        ("a permitted attribute with a changed shape", commit_shape, only_value),
        ("RunEndTime with a changed dtype", end_dtype, only_value),
        ("a compound dataset with identical values but a different layout", relaid, layout_needle),
        ("a path-prefix-only metadata difference", prefixed_metadata, None, False, (prefix_only,)),
        ("a path-prefix string attribute on RunProperties", path_note(prop), None, False, (label,)),
        ("a path-prefix string attribute on RunProperties/Version", path_note(ver), note_ver),
        (
            "a path-prefix attribute with a changed dtype",
            narrow_note,
            note_prop,
            False,
            (),
            "PathNote",
        ),
        (
            "the added post_snapshot: [] line (empty-list leg)",
            add_empty,
            None,
            True,
            (EMPTY_LIST_LABEL,),
        ),
        ("the added empty list outside the empty-list leg", add_empty, "beyond the path prefix"),
        ("an empty-list leg without the added line", unchanged, more, True),
        ("another run-file change in the empty-list leg", substeps, more, True),
    ]
    return [Case(*row) for row in rows]


def run_mutation_checks(source: OutputRun, scratch: Path) -> int:
    """Prove each assertion of the comparator fails on the defect it exists to catch.

    Every case compares fresh copies of the source output, each with its own path-prefix token.
    Returns the number of cases that behaved as required.
    """
    cases = mutation_cases(source)
    for number, case in enumerate(cases):
        runs = []
        for role in ("base", "candidate"):
            directory = scratch / f"{number:02d}_{role}"
            shutil.copytree(source.directory, directory)
            runs.append(OutputRun(directory, (f"{directory}_prefix",), source.source_md5))
        case.change(*runs)
        report = compare_outputs(*runs, empty_leg=case.empty_leg)
        if case.needle is None:
            missing = [label for label in case.permitted if label not in report.permitted]
            if report.errors or missing:
                raise AssertionError(
                    f"control {case.name!r}: errors {report.errors[:4]}, unnamed {missing}"
                )
            log(f"  control accepted: {case.name} (permitted: {sorted(report.permitted)})")
            continue
        if not any(case.needle in error for error in report.errors):
            raise AssertionError(
                f"mutation {case.name!r} not rejected with {case.needle!r}: {report.errors[:4]}"
            )
        if case.unpermitted and any(case.unpermitted in label for label in report.permitted):
            raise AssertionError(f"mutation {case.name!r} was also named as permitted")
        log(f"  mutation rejected: {case.name} ({len(report.errors)} error(s))")
    log(f"comparator self-check: {len(cases)} mutation/control cases behaved as required")
    return len(cases)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


class Tee(io.TextIOBase):
    """Write to the console and the log file at once."""

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


def main() -> int:
    """Run every stage; fail on any FAIL, ERROR or SKIP marker (a WARN is surfaced, not fatal)."""
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    console = sys.stdout
    with LOG_PATH.open("w") as handle:
        buffer = io.StringIO()
        sys.stdout = Tee(console, handle, buffer)
        try:
            # No abort_on_failure: once the builds succeed the legs are independent, each
            # checks its own prerequisites, and the verdict stage names every leg's outcome.
            status = run_test_suite(Identity().stages(), "Snapshot-global disabled-mode identity")
            if re.search(r"^MIMIC_RESULT: SKIP", buffer.getvalue(), flags=re.MULTILINE):
                print("FAIL: a stage was skipped; this test has no legitimate skip")
                status = 1
            if "MIMIC_RESULT: WARN" in buffer.getvalue():
                print("WARN: see the MIMIC_RESULT: WARN line(s) above (not a failure)")
            verdict = "PASS" if status == 0 else "FAIL"
            print(f"{verdict}: tests-snapshot-global-identity (log: {LOG_PATH})")
        finally:
            sys.stdout = console
    return status


if __name__ == "__main__":
    sys.exit(main())
