#!/usr/bin/env python3
"""
Self-test for the sage26 T1 parity comparator (models/sage26/instrument/compare_sage26.py).

Validates: the T1 comparator meets every synthetic control's expectation (the instrument
           README's Controls table, S0-S7c): each control's verdict, exit status, printed
           verdict line, failed thresholds, per-snapshot counts and facts, as the control's own
           expect.json states them, checked by the instrument's own run_control.py
Tolerance: none; every expectation is exact (verdict, exit status, printed verdict line, failed
           thresholds, per-snapshot counts, facts)

A parity figure is only trustworthy if the comparator can report a difference, so this runs
the instrument's synthetic controls: inputs built from literal tables so that the comparator's
outcome is known in advance. It needs no SAGE26 or Mimic run and no T1 reference. Every
expectation comes from the controls' own expect.json; this test adds none of its own.

The 17 reference-derived controls (G1, G1s63, B1-B10c, G2) are not run here. They need the T1
reference, which lives outside the repository, so they stay with run_controls.sh, run by hand as
the instrument README says.

The instrument is protected: it is only run, never changed. This test writes nothing under it.
It sets sys.dont_write_bytecode and gives every child process PYTHONDONTWRITEBYTECODE=1, builds
the controls in a new temporary directory under $TMPDIR (removed when the suite ends), and its
last case checks that the tracked instrument's files are byte-identical to those listed at
import, with no __pycache__ or *.pyc beside them.

Instrument directory: the tracked models/sage26/instrument/ unless SAGE26_INSTRUMENT_DIR names
another directory. The variable exists only for the self-test's mutation table, which points it
at a disposable copy of the instrument; the test tier never sets it. Every instrument script
(make_fixture.py, alter_map.py, run_control.py, run_controls.sh, compare_sage26.py) is taken
from that directory, and the directory is printed as the first line. The instrument-unchanged
case always checks the tracked directory, whatever the variable names.

Test cases:
  - test_control_set: make_fixture.py and alter_map.py each exit 0, and the control directories
    they build are exactly the 23 synthetic controls listed in SYNTHETIC_CONTROLS, which also
    equal the S-prefixed names of run_controls.sh's CONTROLS (so a synthetic control added to
    the instrument without this test fails here)
  - test_control_<name>, one per synthetic control (for example test_control_S3b):
    run_control.py on that control exits 0 and prints "result: MET"
  - test_instrument_unchanged: the SHA-256 of every file under the tracked instrument equals
    its value at import, and no __pycache__ or *.pyc exists there

Run: python3 models/sage26/modules/_tests/test_scientific_compare_sage26.py
     (or make MODEL=sage26 SIMULATION=mini-millennium tests-scientific)
"""

import sys

# The instrument's scripts must leave no bytecode beside their sources; children get the
# matching environment variable in CHILD_ENV.
sys.dont_write_bytecode = True

import hashlib  # noqa: E402  (bytecode writing disabled above)
import os  # noqa: E402
import re  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import time  # noqa: E402
from pathlib import Path  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import run_test_suite  # noqa: E402  (path set up above)

#: The tracked instrument, checked by test_instrument_unchanged whatever the variable says.
TRACKED_INSTRUMENT = REPO_ROOT / "models" / "sage26" / "instrument"

#: The environment variable naming a disposable instrument copy (mutation table only).
INSTRUMENT_ENV = "SAGE26_INSTRUMENT_DIR"

#: The 23 synthetic controls: make_fixture.py builds S0-S6 and S0e, alter_map.py S7a-S7c.
SYNTHETIC_CONTROLS = (
    "S0 S0e S1a S1b S1c S1d S1e S1f S1g S2a S2b S3a S3b S3c S4a S4b S5a S5b S5c S6 S7a S7b S7c"
).split()

#: Environment of every child process: no bytecode caches beside the instrument's sources.
CHILD_ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")


def instrument_dir():
    """The instrument directory to run: SAGE26_INSTRUMENT_DIR if set, else the tracked one."""
    named = os.environ.get(INSTRUMENT_ENV, "")
    if named:
        return Path(named).resolve(), f"from {INSTRUMENT_ENV}"
    return TRACKED_INSTRUMENT.resolve(), "tracked"


INSTRUMENT, INSTRUMENT_SOURCE = instrument_dir()
CONTROLS_DIR = INSTRUMENT / "controls"
COMPARATOR = INSTRUMENT / "compare_sage26.py"
MAKE_FIXTURE = CONTROLS_DIR / "make_fixture.py"
ALTER_MAP = CONTROLS_DIR / "alter_map.py"
RUN_CONTROL = CONTROLS_DIR / "run_control.py"
RUN_CONTROLS_SH = CONTROLS_DIR / "run_controls.sh"


def instrument_listing(root):
    """``{relative path: SHA-256}`` for every file under ``root``, and any bytecode found.

    Returns ``(digests, bytecode)``; ``bytecode`` lists every ``__pycache__`` directory and
    ``*.pyc`` file under ``root``, relative to it.
    """
    digests, bytecode = {}, []
    for directory, subdirs, files in os.walk(root):
        subdirs.sort()
        for name in subdirs:
            if name == "__pycache__":
                bytecode.append(os.path.relpath(os.path.join(directory, name), root))
        for name in sorted(files):
            path = os.path.join(directory, name)
            rel = os.path.relpath(path, root)
            if name.endswith(".pyc"):
                bytecode.append(rel)
            with open(path, "rb") as handle:
                digests[rel] = hashlib.sha256(handle.read()).hexdigest()
    return digests, bytecode


#: The tracked instrument as listed at import, before any instrument script runs.
LISTING_AT_IMPORT = instrument_listing(TRACKED_INSTRUMENT)


def run_child(argv, cwd):
    """Run ``argv`` with CHILD_ENV in ``cwd``; return ``(exit status, combined output)``."""
    proc = subprocess.run(
        argv,
        cwd=cwd,
        env=CHILD_ENV,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    return proc.returncode, proc.stdout


class Fixture:
    """The controls built once, in a temporary directory, for every case of the suite."""

    def __init__(self):
        self.out = None  # the temporary directory holding the control directories
        self.steps = None  # [(script name, exit status, output)] once built

    def built(self):
        """Build the controls on first use; return the list of build steps run."""
        if self.steps is None:
            self.steps = []
            for script in (MAKE_FIXTURE, ALTER_MAP):
                argv = [sys.executable, str(script), "--out", self.out]
                status, output = run_child(argv + ["--comparator", str(COMPARATOR)], self.out)
                self.steps.append((script.name, status, output))
                if status != 0:
                    break  # alter_map.py needs S0, which a failed make_fixture.py may lack
        return self.steps

    def build_failure(self):
        """A message naming the first build step that did not exit 0, or None."""
        steps = self.built()
        for name, status, output in steps:
            if status != 0:
                return f"{name} exited {status}:\n{output}"
        if len(steps) != 2:
            return "the controls were not fully built"
        return None


FIXTURE = Fixture()


def run_controls_synthetic_names():
    """The S-prefixed names of the CONTROLS assignment in run_controls.sh, in order."""
    text = RUN_CONTROLS_SH.read_text(encoding="utf-8")
    assignments = re.findall(r'^CONTROLS="([^"]*)"', text, flags=re.MULTILINE)
    assert (
        len(assignments) == 1
    ), f"{RUN_CONTROLS_SH} holds {len(assignments)} CONTROLS assignments, expected 1"
    return [name for name in assignments[0].split() if name.startswith("S")]


def test_control_set():
    failure = FIXTURE.build_failure()
    if failure:
        print(failure)
        raise AssertionError(failure)
    for name, _, output in FIXTURE.built():
        print(f"{name}: exit 0, {len(output.splitlines())} lines printed")
    built = sorted(p.name for p in Path(FIXTURE.out).iterdir() if p.is_dir())
    assert set(built) == set(SYNTHETIC_CONTROLS), (
        f"control directories built differ from SYNTHETIC_CONTROLS: "
        f"only built {sorted(set(built) - set(SYNTHETIC_CONTROLS))}, "
        f"never built {sorted(set(SYNTHETIC_CONTROLS) - set(built))}"
    )
    listed = run_controls_synthetic_names()
    assert listed == SYNTHETIC_CONTROLS, (
        f"run_controls.sh's synthetic controls {listed} differ from SYNTHETIC_CONTROLS "
        f"{SYNTHETIC_CONTROLS}"
    )
    print(f"{len(built)} control directories built, equal to run_controls.sh's S controls")


def make_control_case(control):
    """A case running run_control.py on ``control``, named ``test_control_<control>``."""

    def case():
        failure = FIXTURE.build_failure()
        assert failure is None, f"{control}: controls not built ({failure.splitlines()[0]})"
        control_dir = os.path.join(FIXTURE.out, control)
        assert os.path.isfile(
            os.path.join(control_dir, "expect.json")
        ), f"{control}: not built (no {control}/expect.json)"
        status, output = run_child([sys.executable, str(RUN_CONTROL), control_dir], FIXTURE.out)
        lines = output.splitlines()
        results = [line.strip() for line in lines if line.strip().startswith("result:")]
        if status != 0 or results != ["result: MET"]:
            print("\n".join(lines))
            raise AssertionError(
                f"{control}: run_control.py exited {status}, printed {results or 'no result line'}"
            )
        print(lines[0] if lines else f"{control}: no output")

    case.__name__ = f"test_control_{control}"
    case.__doc__ = f"run_control.py on {control} exits 0 and prints 'result: MET'."
    return case


def test_instrument_unchanged():
    digests_before, bytecode_before = LISTING_AT_IMPORT
    digests_after, bytecode_after = instrument_listing(TRACKED_INSTRUMENT)
    assert not bytecode_before, f"bytecode under the instrument at import: {bytecode_before}"
    assert not bytecode_after, f"bytecode under the instrument after the suite: {bytecode_after}"
    added = sorted(set(digests_after) - set(digests_before))
    removed = sorted(set(digests_before) - set(digests_after))
    changed = sorted(
        rel
        for rel in set(digests_before) & set(digests_after)
        if digests_before[rel] != digests_after[rel]
    )
    assert not (added or removed or changed), (
        f"the tracked instrument changed during the suite: added {added}, removed {removed}, "
        f"changed {changed}"
    )
    print(f"{len(digests_after)} files under {TRACKED_INSTRUMENT}, every SHA-256 unchanged")


def main():
    print(f"instrument directory: {INSTRUMENT} ({INSTRUMENT_SOURCE})")
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="sage26-compare-selftest-") as out:
        FIXTURE.out = out
        status = run_test_suite(
            [test_control_set]
            + [make_control_case(control) for control in SYNTHETIC_CONTROLS]
            + [test_instrument_unchanged],
            "sage26 T1 comparator controls (test_scientific_compare_sage26.py)",
        )
    print(f"suite wall time: {time.monotonic() - started:.1f} s")
    return status


if __name__ == "__main__":
    sys.exit(main())
