#!/usr/bin/env python3
"""
Byte identity of HEAD's galaxy output against a reference commit, on the real mini-Millennium data.

A manual acceptance test (tests/manual/ is never globbed into ``make tests``)::

    make tests-reference-identity REFERENCE_COMMIT=<hash>

It is the byte gate for any change that must not move a result: an optimisation, a build-flag
change, a refactor. ``REFERENCE_COMMIT`` is required (there is no pinned default: this is an
instrument, not a feature's own gate) and must be an ancestor of HEAD. Eight legs,
{halos-only, sage16} x {vertical, horizontal mini-Millennium} x {fixed, dynamic}: each is the
committed run file ``models/<model>/input/<model>_mini-millennium.yaml`` (or ``..._horizontal``)
with only ``output_directory`` redirected, and the dynamic leg is ``dynamic_variant()`` of it. The
reference and HEAD each run that file on the real data both packages' ``snapshots`` links point
to; the test fails, never skips, when a dataset is absent, because a gate whose only defence is
the data it verifies must not report success on an empty comparison.

Per leg, both runs must record the leg's model, simulation, commit and timestep scheme in
``RunProperties``; then the one output comparison shared with the snapshot-global battery
(``tests/framework/parity_gate.py``) must find the same files, HDF5 objects, master-file links,
attributes and ``metadata/`` files, byte-equal datasets, and per-``UniqueGalaxyID`` byte identity
of every Galaxies field through the unchanged ``scripts/compare_cross_format_identity.py`` in its
default mode. The only permitted differences are named provenance (``RunProperties/Version`` build
attributes, ``RunEndTime``, ``metadata/version_info.json``, worktree or scratch path prefixes).

The reference is built in a worktree cached per ``(commit, model, simulation)`` under
``output/snapshot-global-identity/worktrees/`` (the snapshot-global battery's cache and name
shape), verified a clean checkout. HEAD is built in one reusable worktree per pair
(``head__<model>__<simulation>``), detached to HEAD and rebuilt incrementally on every run, so
commits do not accumulate worktrees. Every worktree's ``snapshots`` links must resolve to the main
tree's targets. A worktree holding an untracked file under the runtime paths (it would be
compiled or run), or a HEAD worktree that is not a registered worktree of this repository, is
refused. Nothing is ever removed by this test (a clean rebuild of a HEAD worktree removes
only that worktree's own ``build/``, as ``make clean`` does).

Two optional environment variables, ``REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS`` and
``REFERENCE_IDENTITY_HEAD_EXTRA_LDFLAGS``, are passed as ``EXTRA_CFLAGS``/``EXTRA_LDFLAGS`` to the
HEAD build only and named in the log, so a candidate build flag can be gated against the
reference without committing it; any ``EXTRA_CFLAGS``/``EXTRA_LDFLAGS`` of the caller's own are
dropped from both builds. A HEAD worktree whose last build used other flags (or none) is cleaned
and rebuilt, so flags never leak between runs.

Runs, logs and ``evidence.json`` are archived under ``archive/reference-identity/<stamp>/``. The
console output is also written to ``build/reference_identity.log``. Every leg emits one
``MIMIC_RESULT:`` marker; any FAIL, ERROR, SKIP or NA marker fails the target.
"""

from __future__ import annotations

import datetime
import io
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import h5py
import yaml

TESTS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(TESTS_DIR))
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from framework import check_no_memory_leaks, run_test_suite  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    ERROR_CAP,
    RUNTIME_PATHS,
    WORKTREE_ROOT,
    OutputRun,
    assert_dataset_present,
    assert_untracked_free,
    attribute_signature,
    attribute_text,
    build_pair,
    cached_worktree,
    compare_outputs,
    dynamic_variant,
    git,
    head_worktree,
    point_output_at,
    read_run_file_output,
    resolve_reference,
    run_logged,
    runtime_tree_differences,
    snapshot_list_entries,
    worktree_env,
)

MODELS = ("halos-only", "sage16")
SCHEMES = ("fixed", "dynamic")
TREES = ("reference", "head")

#: The vertical and horizontal mini-Millennium packages, in leg order.
LAYOUTS = (("vertical", "mini-millennium"), ("horizontal", "mini-millennium-horizontal"))

#: Output location shared by every run, so the copied run YAML in ``metadata/`` of the two runs of
#: a leg is byte-identical.
RUN_FILE_NAME = "identity.yaml"
OUTPUT_DIRECTORY = "output/identity"

#: Environment variables carrying a candidate build's flags for HEAD only, and the make variables
#: they become.
HEAD_FLAG_VARIABLES = (
    ("REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS", "EXTRA_CFLAGS"),
    ("REFERENCE_IDENTITY_HEAD_EXTRA_LDFLAGS", "EXTRA_LDFLAGS"),
)

#: Where a HEAD worktree records the flags its ``build/`` was made with.
FLAGS_STAMP = Path("build") / ".reference_identity_flags"

#: Run-log lines worth keeping beside a leg's verdict: the dataset's population and the memory
#: profile's output-population and peak figures.
PROFILE_LINES = ("Peak process RSS", "Output population P", "Galaxy pool high-water G")

BUILD_TIMEOUT_S = 3600
RUN_TIMEOUT_S = 3600

ARCHIVE_ROOT = REPO_ROOT / "archive" / "reference-identity"
LOG_PATH = REPO_ROOT / "build" / "reference_identity.log"

STARTED = time.monotonic()


def log(message: str) -> None:
    """Print one progress line prefixed with elapsed wall time, unbuffered."""
    elapsed = int(time.monotonic() - STARTED)
    print(f"[reference {elapsed // 60:3d}m{elapsed % 60:02d}s] {message}", flush=True)


@dataclass(frozen=True)
class Leg:
    """One model on one layout's mini-Millennium package under one timestep scheme."""

    model: str
    layout: str
    simulation: str
    scheme: str

    @property
    def name(self) -> str:
        return f"{self.model}__{self.layout}__{self.scheme}".replace("-", "_")

    @property
    def run_file(self) -> str:
        """The committed run file this leg derives from, relative to the repository root."""
        return f"models/{self.model}/input/{self.model}_{self.simulation}.yaml"


LEGS = tuple(
    Leg(model, layout, simulation, scheme)
    for model in MODELS
    for layout, simulation in LAYOUTS
    for scheme in SCHEMES
)


# ---------------------------------------------------------------------------
# Inputs: the reference, the committed run files and the real data
# ---------------------------------------------------------------------------


def reference_request() -> str:
    """The required ``REFERENCE_COMMIT``, naming the reason when it is absent."""
    value = os.environ.get("REFERENCE_COMMIT") or ""
    if not value:
        raise AssertionError(
            "REFERENCE_COMMIT is required: the commit HEAD's output must reproduce byte for "
            "byte, e.g. `make tests-reference-identity REFERENCE_COMMIT=<hash>`"
        )
    return value


def check_selected_paths(reference: str, head: str) -> None:
    """Both commits carry every package and run file the legs select."""
    paths = [leg.run_file for leg in LEGS]
    paths += [f"models/{model}/model_properties.yaml" for model in MODELS]
    paths += [f"simulations/{simulation}/simulation_info.yaml" for _, simulation in LAYOUTS]
    for commit in (reference, head):
        for path in paths:
            if git("cat-file", "-e", f"{commit}:{path}", check=False).returncode != 0:
                raise AssertionError(f"{commit} lacks {path}, which the legs select")


def committed_run_file(leg: Leg, reference: str, head: str) -> str:
    """The leg's committed run file, which must be the same text at the reference and at HEAD."""
    texts = {commit: git("show", f"{commit}:{leg.run_file}").stdout for commit in (reference, head)}
    if texts[reference] != texts[head]:
        raise AssertionError(f"{leg.run_file} differs between the reference and HEAD")
    return texts[head]


def redirect_output(text: str, run_file: str) -> str:
    """``text`` with only ``output.output_directory`` set to the shared output location."""
    result, count = re.subn(
        r"^  output_directory: .*$",
        lambda _match: f"  output_directory: {OUTPUT_DIRECTORY}",
        text,
        flags=re.MULTILINE,
    )
    if count != 1:
        raise AssertionError(f"{run_file}: output_directory matched {count} time(s), expected 1")
    return result


def required_dataset_files(simulation: str) -> list[str]:
    """The data files a simulation package's ``snapshots`` directory must hold for the legs."""
    info = yaml.safe_load(
        (REPO_ROOT / "simulations" / simulation / "simulation_info.yaml").read_text()
    )
    section = info["input"]
    tree_name = section["tree_name"]
    if "%" in tree_name:  # horizontal: one file per a_list snapshot, plus the forest inventory
        listed = snapshot_list_entries(REPO_ROOT / section["snapshot_list_file"])
        return ["forests.h5", *(tree_name % number for number in range(len(listed)))]
    return [
        f"{tree_name}.{number}" for number in range(section["first_file"], section["last_file"] + 1)
    ]


def assert_snapshots_link(worktree: Path, simulation: str) -> None:
    """The worktree's ``snapshots`` link resolves to the main tree's target, else name both."""
    main = REPO_ROOT / "simulations" / simulation / "snapshots"
    link = worktree / "simulations" / simulation / "snapshots"
    target = link.resolve() if link.exists() else None
    if target != main.resolve():
        raise AssertionError(
            f"worktree {worktree}: simulations/{simulation}/snapshots resolves to "
            f"{target or 'nothing (no usable link)'}, the main tree's resolves to "
            f"{main.resolve()}; relink it to the main tree's target, or move the worktree "
            "directory to cold storage, run `git worktree prune`, and re-run"
        )


def first_line(error: BaseException) -> str:
    """The first line of an error's message, or its type name when it has none."""
    return str(error).splitlines()[0] if str(error) else type(error).__name__


def build_env(worktree: Path, model: str, simulation: str, flags: dict[str, str]) -> dict:
    """The worktree environment for a build: no ``EXTRA_*`` flags from the caller, then ``flags``.

    ``make tests-reference-identity EXTRA_CFLAGS=...`` (or an exported variable) would otherwise
    reach both builds while the log named no HEAD flags; only the ``REFERENCE_IDENTITY_HEAD_*``
    values may reach, and only HEAD.
    """
    env = worktree_env(worktree, model, simulation)
    for _, variable in HEAD_FLAG_VARIABLES:
        env.pop(variable, None)
    env.update(flags)
    return env


def head_flags() -> dict[str, str]:
    """The candidate build flags for HEAD from the environment (empty when none are set)."""
    flags = {variable: os.environ.get(source, "") for source, variable in HEAD_FLAG_VARIABLES}
    return {variable: value for variable, value in flags.items() if value}


def build_head_pair(
    worktree: Path, model: str, simulation: str, env: dict, logs: Path, key: str, flags: dict
) -> None:
    """Build a HEAD worktree incrementally, or from clean when its last build used other flags."""
    stamp = worktree / FLAGS_STAMP
    wanted = json.dumps(flags, sort_keys=True)
    if not stamp.is_file() or stamp.read_text() != wanted:
        run_logged(
            ["make", "clean"],
            worktree,
            env,
            logs / f"{key}-clean.log",
            f"{key}: make clean (build flags {wanted})",
            BUILD_TIMEOUT_S,
        )
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(wanted)
    build_pair(worktree, model, simulation, env, logs, key, BUILD_TIMEOUT_S)


# ---------------------------------------------------------------------------
# The identity run
# ---------------------------------------------------------------------------


@dataclass
class RunRecord:
    tree: str
    output: OutputRun
    elapsed_s: float
    attributes: dict
    profile: list[str]


class ReferenceIdentity:
    """State and stages of one identity run. Nothing it creates is ever deleted."""

    def __init__(self):
        self.root: Path | None = None
        self.commits: dict[str, str] = {}
        self.run_texts: dict[str, str] = {}
        self.worktrees: dict[tuple[str, str, str], Path] = {}
        self.verdicts: dict[str, str] = {}
        self.prerequisite_failure: str | None = None
        self.evidence: dict = {"legs": {}, "timings_s": {}}

    def scratch(self, *parts: str) -> Path:
        assert self.root is not None, "the resolution stage has not created the archive root"
        path = self.root.joinpath(*parts)
        path.mkdir(parents=True, exist_ok=True)
        return path

    def save_evidence(self) -> None:
        if self.root is not None:
            (self.root / "evidence.json").write_text(json.dumps(self.evidence, indent=2) + "\n")

    def prerequisite(self, stage: Callable[[], None]) -> None:
        """Run a stage the legs depend on, remembering why it failed for the legs to name."""
        try:
            stage()
        except BaseException as error:
            self.prerequisite_failure = self.prerequisite_failure or first_line(error)
            raise

    def test_reference_commit_resolution(self) -> None:
        """The reference is an ancestor of HEAD and the HEAD tree is what is built."""
        self.prerequisite(self.resolve)

    def resolve(self) -> None:
        reference, head = resolve_reference(reference_request(), (check_selected_paths,))
        log(f"HEAD: {head}")
        log(f"reference (REFERENCE_COMMIT): {reference}")
        edits = runtime_tree_differences()  # no exemption: any edit counts
        if edits:
            raise AssertionError(
                f"runtime paths {', '.join(RUNTIME_PATHS)} differ from HEAD (uncommitted): {edits}"
            )
        for leg in LEGS:
            text = committed_run_file(leg, reference, head)
            self.run_texts[leg.run_file] = redirect_output(text, leg.run_file)
        self.commits = {"reference": reference, "head": head}
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        self.root = ARCHIVE_ROOT / f"{stamp}-ref{reference[:8]}-head{head[:8]}"
        self.root.mkdir(parents=True, exist_ok=False)
        self.evidence.update(
            {
                "head": head,
                "reference": reference,
                "archive": str(self.root),
                "worktrees": str(WORKTREE_ROOT),
            }
        )
        self.save_evidence()
        log(f"archive (runs, logs, evidence): {self.root}")
        log(f"worktree cache: {WORKTREE_ROOT}")

    def test_builds_name_their_commits(self) -> None:
        """One worktree per tree x model x simulation, built at its commit."""
        self.prerequisite(self.build)

    def build(self) -> None:
        if not self.commits:
            raise AssertionError(f"prerequisite stage failed: {self.prerequisite_failure}")
        flags = head_flags()
        log(
            "HEAD build flags: "
            + (", ".join(f"{name}={value!r}" for name, value in flags.items()) or "none")
        )
        self.evidence["head_build_flags"] = flags
        logs = self.scratch("logs")
        for tree in TREES:
            commit = self.commits[tree]
            for model in MODELS:
                for _, simulation in LAYOUTS:
                    key = f"{tree}__{model}__{simulation}"
                    if tree == "reference":
                        worktree = cached_worktree(commit, model, simulation, log_line=log)
                        assert_untracked_free(worktree)
                    else:
                        worktree = head_worktree(commit, model, simulation, log_line=log)
                    started = time.monotonic()
                    if tree == "reference":
                        env = build_env(worktree, model, simulation, {})
                        build_pair(worktree, model, simulation, env, logs, key, BUILD_TIMEOUT_S)
                    else:
                        env = build_env(worktree, model, simulation, flags)
                        build_head_pair(worktree, model, simulation, env, logs, key, flags)
                    self.evidence["timings_s"][f"build:{key}"] = round(
                        time.monotonic() - started, 1
                    )
                    self.worktrees[(tree, model, simulation)] = worktree
        self.save_evidence()

    def execute(self, tree: str, leg: Leg, run_file: Path) -> RunRecord:
        """Run one executable from its worktree into its own archived output root."""
        worktree = self.worktrees[(tree, leg.model, leg.simulation)]
        output_root = self.scratch("runs", f"{tree}__{leg.name}")
        point_output_at(worktree, output_root)
        log_path = self.scratch("logs") / f"{tree}__{leg.name}-run.log"
        elapsed = run_logged(
            ["./mimic", str(run_file)],
            worktree,
            worktree_env(worktree, leg.model, leg.simulation),
            log_path,
            f"{tree} {leg.name}: mimic run",
            RUN_TIMEOUT_S,
        )
        text = log_path.read_text(errors="replace")
        if not check_no_memory_leaks(text):
            raise AssertionError(f"{tree} {leg.name}: the run reported a memory leak")
        profile = [
            line.strip() for line in text.splitlines() if line.strip().startswith(PROFILE_LINES)
        ]
        for line in profile:
            log(f"     | {tree}: {line}")
        basename = read_run_file_output(run_file)[1]
        directory = output_root / Path(OUTPUT_DIRECTORY).name
        master = directory / f"{basename}.hdf5"
        if not master.is_file():
            raise AssertionError(f"{tree} {leg.name}: no master file at {master}")
        attributes = {}
        with h5py.File(master, "r") as handle:
            for path, name in (
                ("RunProperties/Version", "git_commit"),
                ("RunProperties", "TimestepScheme"),
                ("RunProperties", "ModelName"),
                ("RunProperties", "SimulationName"),
            ):
                attributes[name] = attribute_text(attribute_signature(handle[path].attrs[name]))
        digest_file = worktree / "build" / "generated" / "property_hash.txt"
        if not digest_file.is_file():
            raise AssertionError(f"{tree} {leg.name}: no generated digest at {digest_file}")
        prefixes = {str(worktree), str(worktree.resolve()), str(output_root)}
        prefixes.add(str(output_root.resolve()))
        output = OutputRun(directory, tuple(prefixes), digest_file.read_text().strip(), basename)
        return RunRecord(tree, output, elapsed, attributes, profile)

    def check_provenance(self, record: RunRecord, leg: Leg) -> None:
        """The run came from the intended commit, model, simulation and timestepping scheme."""
        expected = {
            "git_commit": self.commits[record.tree],
            "TimestepScheme": leg.scheme,
            "ModelName": leg.model,
            "SimulationName": leg.simulation,
        }
        for key, value in expected.items():
            if record.attributes[key] != value:
                raise AssertionError(
                    f"{record.tree} {leg.name}: RunProperties {key} is "
                    f"{record.attributes[key]!r}, expected {value!r}"
                )

    def run_file_for(self, leg: Leg) -> Path:
        """The one run file both runs of a leg use: the committed file, output redirected."""
        fixed = self.scratch("run-files", leg.name, "fixed") / RUN_FILE_NAME
        fixed.write_text(self.run_texts[leg.run_file])
        if leg.scheme == "fixed":
            return fixed
        return dynamic_variant(fixed, self.scratch("run-files", leg.name, "dynamic"))

    def leg(self, leg: Leg) -> None:
        try:
            self.run_leg(leg)
        except BaseException as error:
            self.verdicts[leg.name] = f"FAIL: {first_line(error)}"
            raise
        finally:
            self.evidence["legs"].setdefault(leg.name, {})["verdict"] = self.verdicts.get(leg.name)
            self.save_evidence()

    def run_leg(self, leg: Leg) -> None:
        missing = [t for t in TREES if (t, leg.model, leg.simulation) not in self.worktrees]
        if missing:
            raise AssertionError(
                f"prerequisite stages failed (no {missing} build): {self.prerequisite_failure}"
            )
        assert_dataset_present(leg.simulation, required_dataset_files(leg.simulation))
        for tree in TREES:
            assert_snapshots_link(self.worktrees[(tree, leg.model, leg.simulation)], leg.simulation)
        run_file = self.run_file_for(leg)
        evidence = self.evidence["legs"].setdefault(leg.name, {})
        evidence.update({"model": leg.model, "simulation": leg.simulation, "scheme": leg.scheme})
        started = time.monotonic()
        records = {tree: self.execute(tree, leg, run_file) for tree in TREES}
        for record in records.values():
            self.check_provenance(record, leg)
        evidence["run_seconds"] = {t: round(r.elapsed_s, 1) for t, r in records.items()}
        evidence["profile"] = {t: r.profile for t, r in records.items()}
        report = compare_outputs(records["reference"].output, records["head"].output)
        evidence["compare"] = {**report.summary(), "error_lines": report.errors[:ERROR_CAP]}
        log(
            f"  {leg.name}: {report.galaxies} galaxies over {report.snapshots} snapshot(s), "
            f"{report.fields} fields, {report.links} external link(s), "
            f"{report.galaxy_differences} galaxy difference(s), {len(report.errors)} error(s); "
            f"permitted: {sorted(report.permitted)}"
        )
        evidence["elapsed_s"] = round(time.monotonic() - started, 1)
        if report.errors:
            for line in report.errors[:ERROR_CAP]:
                log(f"  {leg.name} FAILURE: {line}")
            if len(report.errors) > ERROR_CAP:
                log(f"  {leg.name}: {len(report.errors) - ERROR_CAP} further failure(s) not logged")
            raise AssertionError(
                f"{leg.name}: {len(report.errors)} failure(s); first: {report.errors[0]}"
            )
        self.verdicts[leg.name] = "PASS"

    def leg_stages(self) -> list[Callable[[], None]]:
        stages = []
        for leg in LEGS:

            def stage(chosen=leg):
                self.leg(chosen)

            stage.__name__ = f"test_leg_{leg.name}"
            stages.append(stage)
        return stages

    def test_verdicts(self) -> None:
        verdicts = {leg.name: self.verdicts.get(leg.name, "did not run") for leg in LEGS}
        for name, verdict in verdicts.items():
            log(f"  {name}: {verdict}")
        self.evidence["verdicts"] = verdicts
        self.evidence["total_elapsed_s"] = round(time.monotonic() - STARTED, 1)
        self.save_evidence()
        bad = [name for name, verdict in verdicts.items() if verdict != "PASS"]
        if bad:
            raise AssertionError(f"{len(bad)} of {len(LEGS)} legs did not pass: {bad}")
        log(f"all {len(LEGS)} legs passed; evidence: {self.root}/evidence.json")

    def stages(self) -> list[Callable[[], None]]:
        return [
            self.test_reference_commit_resolution,
            self.test_builds_name_their_commits,
            *self.leg_stages(),
            self.test_verdicts,
        ]


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
    """Run every stage; any FAIL, ERROR, SKIP or NA marker fails (a WARN is surfaced, not fatal)."""
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    console = sys.stdout
    with LOG_PATH.open("w") as handle:
        buffer = io.StringIO()
        sys.stdout = Tee(console, handle, buffer)
        try:
            # No abort_on_failure: once the builds succeed the legs are independent, each checks
            # its own prerequisites (naming the data it needs), and the verdict stage names every
            # leg's outcome. A failed resolution or build leaves the legs nothing to run.
            status = run_test_suite(ReferenceIdentity().stages(), "Reference identity")
            if re.search(r"^MIMIC_RESULT: (SKIP|NA)\b", buffer.getvalue(), flags=re.MULTILINE):
                print("FAIL: a stage was skipped or not applicable; every stage must run")
                status = 1
            if "MIMIC_RESULT: WARN" in buffer.getvalue():
                print("WARN: see the MIMIC_RESULT: WARN line(s) above (not a failure)")
            verdict = "PASS" if status == 0 else "FAIL"
            print(f"{verdict}: tests-reference-identity (log: {LOG_PATH})")
        finally:
            sys.stdout = console
    return status


if __name__ == "__main__":
    sys.exit(main())
