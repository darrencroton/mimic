#!/usr/bin/env python3
"""
Run the snapshot-global fixture battery and gate every step on its declared case count.

Usage::

    MODEL=<caller model> SIMULATION=<caller simulation> \\
        python tests/manual/run_snapshot_global_battery.py [--only sham|hod]

``make tests-snapshot-global`` runs every group; ``make tests-snapshot-global-sham`` and
``make tests-snapshot-global-hod`` run only the ``sham`` and ``hod`` groups. Each group builds its model/simulation pair as a test build, then runs its
declared tests by path:

  halos-only-v2  halos-only x micro-uchuu-ascii-horizontal: tests/integration/test_snapshot_phase.py,
                 the C unit test test_snapshot_module_contract and
                 tests/integration/test_snapshot_module_schema.py
  halos-only-v3  halos-only x mini-millennium-horizontal: tests/integration/test_snapshot_phase.py
  sham           sham x micro-uchuu-ascii-horizontal: the sham_global_rank C unit and Python
                 integration tests
  hod            hod x micro-uchuu-ascii-horizontal: the hod_populate C unit test and the fixture
                 cases of its Python integration test (the vertical mini-Millennium cases run in
                 the integration tier of a hod x mini-millennium build)

Model tests are not registered for horizontal packages, which is why the tests are invoked by
path. Marker policy, per step: a non-zero exit, any ``MIMIC_RESULT: FAIL``, ``ERROR`` or ``SKIP``
(these batteries have no legitimate skip), or a PASS-plus-WARN count different from the cases the
test file declares (``def test_`` or ``TEST_RUN(``) fails the step; a ``WARN`` is surfaced but not
fatal, as in ``make tests``. The whole output goes to one log under build/.

The caller's generated code (``MODEL``/``SIMULATION`` from the environment, else the Makefile
defaults) is regenerated in a ``finally`` block, and SIGHUP/SIGINT/SIGQUIT/SIGTERM are converted
to SystemExit so that block runs on an interrupt too; only SIGKILL can leave the test-build
selectors in place (``make generate`` with your selectors restores them). A failed restore fails
the run. The executable is left a TEST_BUILD binary: rebuild it with ``make``.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
UNIT_DIR = REPO_ROOT / "tests" / "unit"
SHAM_TESTS = Path("models/sham/modules/sham_global_rank/_tests")
HOD_TESTS = Path("models/hod/modules/hod_populate/_tests")
MARKER_RE = re.compile(r"^MIMIC_RESULT: (PASS|WARN|FAIL|ERROR|SKIP)\b.*$", re.MULTILINE)
MAKE_STATE = ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")


@dataclass(frozen=True)
class Test:
    """One declared test file: a C unit test (run through run_tests.sh) or a Python test."""

    label: str
    source: Path
    unit_name: str | None = None
    #: A Python test file shared between packages names its cases ``test_<cases>_*`` and takes
    #: ``--cases <cases>``; only that subset is run and counted, so the cases written for another
    #: package (which would report a configuration SKIP here) stay out of this group.
    cases: str | None = None

    def declared(self) -> int:
        text = (REPO_ROOT / self.source).read_text()
        if self.source.suffix == ".c":
            return text.count("TEST_RUN(")
        prefix = f"test_{self.cases}_" if self.cases else "test_"
        return len(re.findall(rf"^def {prefix}", text, flags=re.MULTILINE))

    def command(self) -> tuple[list[str], Path]:
        if self.source.suffix == ".c":
            return ["./run_tests.sh", self.unit_name or str(self.source)], UNIT_DIR
        extra = ["--cases", self.cases] if self.cases else []
        return [sys.executable, str(self.source), *extra], REPO_ROOT


@dataclass(frozen=True)
class Group:
    name: str
    model: str
    simulation: str
    tests: tuple[Test, ...]


PHASE_TEST = Path("tests/integration/test_snapshot_phase.py")
GROUPS = (
    Group(
        "halos-only-v2",
        "halos-only",
        "micro-uchuu-ascii-horizontal",
        (
            Test("post_snapshot phase tests on micro-uchuu-ascii-horizontal", PHASE_TEST),
            Test(
                "typed callback unit tests",
                Path("tests/unit/test_snapshot_module_contract.c"),
                "test_snapshot_module_contract",
            ),
            Test(
                "typed callback schema tests",
                Path("tests/integration/test_snapshot_module_schema.py"),
            ),
        ),
    ),
    Group(
        "halos-only-v3",
        "halos-only",
        "mini-millennium-horizontal",
        (Test("post_snapshot phase tests on mini-millennium-horizontal", PHASE_TEST),),
    ),
    Group(
        "sham",
        "sham",
        "micro-uchuu-ascii-horizontal",
        (
            Test("sham_global_rank unit tests", SHAM_TESTS / "test_unit_sham_global_rank.c"),
            Test(
                "sham_global_rank integration tests",
                SHAM_TESTS / "test_integration_sham_global_rank.py",
            ),
        ),
    ),
    Group(
        "hod",
        "hod",
        "micro-uchuu-ascii-horizontal",
        (
            Test("hod_populate unit tests", HOD_TESTS / "test_unit_hod_populate.c"),
            Test(
                "hod_populate integration tests",
                HOD_TESTS / "test_integration_hod_populate.py",
                cases="fixture",
            ),
        ),
    ),
)


class Battery:
    """Runs groups in sequence, appending every command's output to one log."""

    def __init__(self, log_path: Path):
        self.log_path = log_path
        self.failures: list[str] = []
        self.warnings: list[str] = []
        self.cases = 0

    def run(self, cmd: list[str], cwd: Path, env: dict, title: str) -> tuple[int, str]:
        """Run one command; its output goes to the log and is returned with the exit status."""
        completed = subprocess.run(
            cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        with self.log_path.open("a") as handle:
            handle.write(f"=== {title} (exit {completed.returncode})\n{completed.stdout}\n")
        return completed.returncode, completed.stdout

    def fail(self, message: str) -> None:
        print(f"FAIL: {message}", flush=True)
        self.failures.append(message)

    def check(self, label: str, status: int, output: str, declared: int) -> None:
        """Apply the marker policy to one step's output."""
        markers = [(m.group(1), m.group(0)) for m in MARKER_RE.finditer(output)]
        passed = sum(1 for kind, _ in markers if kind in ("PASS", "WARN"))
        bad = [line for kind, line in markers if kind in ("FAIL", "ERROR", "SKIP")]
        warned = [line for kind, line in markers if kind == "WARN"]
        if status != 0:
            self.fail(f"{label} exited {status}")
        if bad:
            self.fail(f"{label} reported FAIL, ERROR or SKIP cases:\n  " + "\n  ".join(bad))
        if passed != declared:
            self.fail(f"{label} ran {passed} passing cases, expected {declared}")
        for line in warned:
            print(f"WARN: {label}: {line}", flush=True)
        self.warnings += warned
        self.cases += passed
        print(f"  {label}: {passed}/{declared} cases", flush=True)

    def group(self, group: Group) -> None:
        print(f"--- {group.model} x {group.simulation}", flush=True)
        env = {key: value for key, value in os.environ.items() if key not in MAKE_STATE}
        env.update(MODEL=group.model, SIMULATION=group.simulation)
        env["PATH"] = f"{Path(sys.executable).parent}{os.pathsep}{env.get('PATH', '')}"
        selectors = [f"MODEL={group.model}", f"SIMULATION={group.simulation}", "TEST_BUILD=yes"]
        make = ["make", "--no-print-directory", *selectors]
        status, _ = self.run([*make, "generate", "validate-build"], REPO_ROOT, env, "generate")
        if status == 0:
            jobs = f"-j{os.cpu_count() or 4}"
            status, _ = self.run([*make, jobs, "mimic"], REPO_ROOT, env, "build")
        if status != 0:
            self.fail(f"build {group.model} x {group.simulation} exited {status}; tests not run")
            return
        for test in group.tests:
            cmd, cwd = test.command()
            status, output = self.run(cmd, cwd, env, test.label)
            self.check(test.label, status, output, test.declared())

    def restore(self) -> None:
        """Regenerate the caller's generated code; a failure fails the run."""
        selectors = [
            f"{key}={os.environ[key]}" for key in ("MODEL", "SIMULATION") if key in os.environ
        ]
        env = {key: value for key, value in os.environ.items() if key not in MAKE_STATE}
        cmd = ["make", "--no-print-directory", *selectors, "generate"]
        status, _ = self.run(cmd, REPO_ROOT, env, "restore generated code")
        caller = " ".join(selectors) or "the Makefile defaults"
        if status != 0:
            self.fail(f"could not restore generated code for {caller} (see {self.log_path})")
        else:
            print(f"Generated code restored for {caller}; rebuild the executable with 'make'.")


def raise_on_signal(signum, _frame):
    raise SystemExit(128 + signum)


def main(argv=None, groups=GROUPS) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--only", choices=[group.name for group in groups], action="append")
    args = parser.parse_args(argv)
    selected = [group for group in groups if not args.only or group.name in args.only]
    only = args.only[0] if args.only and len(args.only) == 1 else None
    target = "tests-snapshot-global" + (f"-{only}" if only in ("sham", "hod") else "")
    log_path = REPO_ROOT / "build" / f"{target.replace('tests-', '').replace('-', '_')}_tests.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("")
    battery = Battery(log_path)
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
        signal.signal(signum, raise_on_signal)
    try:
        for group in selected:
            battery.group(group)
    finally:
        battery.restore()
    if battery.failures:
        print(f"FAIL: {target} ({len(battery.failures)} failure(s); see {log_path})")
        return 1
    note = f", {len(battery.warnings)} warning(s) surfaced" if battery.warnings else ""
    print(
        f"PASS: {target} ({battery.cases} cases, every step gated on its declared count, "
        f"no skips{note}; log: {log_path})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
