#!/usr/bin/env python3
"""
Run the rate battery: the rate mode's identity legs across drivers, tasks, chunks and module order.

Usage::

    MODEL=<caller model> SIMULATION=<caller simulation> [MPIRUN="mpirun --oversubscribe"] \\
        python tests/manual/run_rate_battery.py [--offline | --only LEG ...]

``make tests-rate`` runs every leg. ``make tests-rate RATE_BATTERY_ARGS=--offline`` runs the legs
that need no external data (CI's step). ``--only`` (repeatable) is a development aid: a filtered
run is not the gate, so it ends with a ``PARTIAL:`` summary and exit status 3 even when every
check it ran passed. ``MPIRUN`` is the launcher command (default ``mpirun``; CI uses ``mpirun
--oversubscribe``), split with shell quoting rules and given ``-np <N>``. Needs Open MPI (or
another MPI whose ``mpicc``/``mpirun`` are on PATH), HDF5 and libyaml.

Every leg runs ``halos-only`` with the four rate fixtures (``test_rate_supply``,
``test_rate_formation``, ``test_rate_feedback``, ``test_rate_stripping``) in one rate phase and
``test_rate_jump`` in a discrete phase after it, test builds only, with the modules and
parameters of ``simulations/mini-millennium-horizontal/_tests/input/forest_blocks_rate.yaml``.
The legs:

``cross-driver``
    The real mini-Millennium data in both layouts (``mini-millennium`` through the vertical
    driver, ``mini-millennium-horizontal`` through the horizontal one; the committed
    ``halos-only`` run files with the rate modules added), under the explicit scheme with fixed
    and with dynamic time-stepping and under the implicit scheme with fixed time-stepping: three
    pairs of runs. Each pair must give per-``UniqueGalaxyID`` byte identity of every output field
    through ``scripts/compare_cross_format_identity.py`` (tree rows only: a created record's id
    differs between drivers), identical step statistics (every attribute and dataset of the
    master file's ``RunProperties/RateIntegration``), and, for every FoF group (an output
    snapshot and a ``UniqueCentralGalaxyID``), the same sequence of tree rows in the output under
    both drivers. Needs the real data under ``simulations/*/snapshots`` and fails, never skips,
    without it.
``task-chunk``
    The committed ``forest_blocks`` fixture (the rate run file above), a non-MPI serial run
    against ``$MPIRUN -np N`` for N in 1, 2, 3, 4, 8, against ``input.forest_chunks`` 2, 3 and 8
    serially, and against ``-np 2`` with 2 chunks and ``-np 3`` with 3 chunks (the counts of
    ``tests/manual/test_distributed_identity.py``). Each must pass the comparator with
    ``--compare-created``, hold the serial run's ``UniqueGalaxyID`` column in the same file order
    snapshot by snapshot (an MPI run's task partitions concatenated in task order), have the
    distributed layout, and record step statistics identical to the serial run's.
``permutation``
    The same fixture on one binary, the four rate modules listed in every one of their 24 orders
    in the run file, under each scheme. Every run's ``Galaxies`` datasets must equal the first
    order's byte for byte, field by field and in row order, with identical step statistics; the
    comparison leaves out ``EnabledModules`` and the run-file copy, which record the configured
    order, and the leg asserts that ``EnabledModules`` does record each order, so the runs differ
    in the configuration only.
``record-creation``
    The same fixture with ``test_fixture`` creating records in ``pre_timestep``, ahead of the
    rate phase: a serial run must complete with created rows, and ``-np 1, 2, 3, 4, 8`` must
    match it through the comparator with ``--compare-created`` (both sides must report created
    rows), in row order and in step statistics. One driver only: a created record's
    ``UniqueGalaxyID`` differs between drivers.

Every leg first asserts that the seven fixture properties (``RateFixture*``) are finite,
non-negative and non-zero in some rows of its reference output, so no comparison is made over
zeros. Each leg reports its compared cases against the count its design implies. The battery builds
its own test-build pairs (``halos-only`` on ``mini-millennium``, then on
``mini-millennium-horizontal`` without and with ``USE-MPI=yes``), and any FAIL, ERROR, SKIP or NA
marker in a run's output fails it; it has no legitimate skip. Every build and launch runs in its
own process group under a timeout, so a hang fails the battery instead of stalling it. The whole
output goes to ``build/rate_tests.log``; run outputs go to ``output/rate-battery/`` (replaced on
each run).

The caller's generated code (``MODEL``/``SIMULATION`` from the environment, else the Makefile
defaults) is regenerated in a ``finally`` block, and SIGHUP/SIGINT/SIGQUIT/SIGTERM are converted
to SystemExit so that block runs on an interrupt too. A failed restore fails the run. The
executable is left the last leg's test build: rebuild it with ``make``.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import itertools
import os
import re
import shlex
import signal
import subprocess
import sys
from pathlib import Path
from typing import Iterator, NamedTuple

import h5py
import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_distributed_identity as distributed  # noqa: E402
from framework import check_no_memory_leaks  # noqa: E402
from framework.fs import remove_tree  # noqa: E402

MODEL = "halos-only"
VERTICAL_SIMULATION = "mini-millennium"
HORIZONTAL_SIMULATION = distributed.SIMULATION  # the package holding the forest_blocks fixture
RATE_RUN_FILE = "forest_blocks_rate.yaml"
WORK_ROOT = REPO_ROOT / "output" / "rate-battery"
LOG_PATH = REPO_ROOT / "build" / "rate_tests.log"
COMPARATOR = distributed.COMPARATOR

RATE_PHASE = "rates"
JUMP_MODULE = "test_rate_jump"
RATE_MODULES = (
    "test_rate_supply",
    "test_rate_formation",
    "test_rate_feedback",
    "test_rate_stripping",
)
CREATOR = "test_fixture"
CREATOR_PARAMS = {
    "TestFixtureDummyParameter": 0.25,
    "TestFixtureEnableLogging": 0,
    "TestFixtureCreateRecords": 1,
}
FIXTURE_FIELDS = (
    "RateFixtureHot",
    "RateFixtureCold",
    "RateFixtureStars",
    "RateFixtureHotTracer",
    "RateFixtureColdTracer",
    "RateFixtureStarsTracer",
    "RateFixtureFormed",
)
STATISTICS = "RunProperties/RateIntegration"
ENABLED_MODULES = "RunProperties/EnabledModules"

LEGS = ("cross-driver", "task-chunk", "permutation", "record-creation")
OFFLINE_LEGS = ("task-chunk", "permutation", "record-creation")
SCHEMES = ("explicit", "implicit")
NP_COUNTS = distributed.NP_COUNTS
CHUNK_COUNTS = distributed.CHUNK_COUNTS
NP_CHUNK_PAIRS = distributed.NP_CHUNK_PAIRS
ORDERS = tuple(itertools.permutations(RATE_MODULES))


class CrossCase(NamedTuple):
    """One pair of runs of the cross-driver leg."""

    scheme: str
    timestep: str

    @property
    def name(self) -> str:
        return f"{self.scheme}_{self.timestep}"


CROSS_CASES = (
    CrossCase("explicit", "fixed"),
    CrossCase("explicit", "dynamic"),
    CrossCase("implicit", "fixed"),
)
# The compared cases each leg must report, from the legs' own designs: a leg that compared fewer
# (a run that failed and was skipped, a case dropped from a loop) fails on the count as well.
EXPECTED_CASES = {
    "cross-driver": len(CROSS_CASES),
    "task-chunk": len(NP_COUNTS) + len(CHUNK_COUNTS) + len(NP_CHUNK_PAIRS),
    "permutation": len(SCHEMES) * (len(ORDERS) - 1),
    "record-creation": len(NP_COUNTS),
}
REAL_RUN_FILES = {
    "vertical": Path("models") / MODEL / "input" / f"{MODEL}_{VERTICAL_SIMULATION}.yaml",
    "horizontal": Path("models") / MODEL / "input" / f"{MODEL}_{HORIZONTAL_SIMULATION}.yaml",
}

BAD_MARKER_RE = re.compile(r"^MIMIC_RESULT: (FAIL|ERROR|SKIP|NA)\b.*$", re.MULTILINE)
PASSED_RE = distributed.PASSED_RE
CREATED_RE = distributed.CREATED_RE
MAKE_STATE = distributed.MAKE_STATE
BUILD_TIMEOUT = distributed.BUILD_TIMEOUT
RESTORE_TIMEOUT = distributed.RESTORE_TIMEOUT
TIMEOUT_STATUS = distributed.TIMEOUT_STATUS
RUN_TIMEOUT = 120  # fixture runs take well under a second; a hang must cost little
REAL_RUN_TIMEOUT = 1800  # a real-data run takes seconds; a hang is bounded, not waited out


class BatteryStopped(Exception):
    """A make timed out: the battery records the failure and stops rather than repeat the hang."""


class LegStopped(Exception):
    """A launch or comparison timed out: its failure marker is recorded and the leg ends."""


class Statistics(NamedTuple):
    """What the battery reads from a master file's ``RunProperties/RateIntegration`` group."""

    entries: dict[str, bytes]  # every attribute ("@name") and dataset, as raw bytes
    segments: int  # StepTotals.segments
    scheme: str  # the recorded scheme attribute


class Battery:
    """Runs the legs in build order, appending every command's output to one log."""

    def __init__(self, mpirun: list[str], legs: tuple[str, ...]):
        self.mpirun = mpirun
        self.legs = legs
        self.failures: list[str] = []
        self.passes = 0
        self.cases = {leg: 0 for leg in LEGS}
        self.env = {key: value for key, value in os.environ.items() if key not in MAKE_STATE}
        self.env["PATH"] = f"{Path(sys.executable).parent}{os.pathsep}{self.env.get('PATH', '')}"
        self.real_outputs: dict[tuple[str, str], Path] = {}
        self.references: dict[str, Path] = {}

    # ------------------------------------------------------------------ plumbing

    def log(self, text: str) -> None:
        with LOG_PATH.open("a") as handle:
            handle.write(text)

    def run(self, cmd: list[str], title: str, timeout: int = RUN_TIMEOUT) -> tuple[int, str]:
        """Run one command from the repository root in its own process group; log and return
        (status, output).

        A timeout is status 124 with the partial output, after the whole group is terminated and
        reaped; an interrupt terminates the group too and is re-raised. A FAIL, ERROR, SKIP or NA
        marker in the output fails the battery.
        """
        try:
            process = subprocess.Popen(
                cmd,
                cwd=REPO_ROOT,
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
        except OSError as error:
            status, output = 127, f"[could not run {cmd[0]}: {error}]\n"
            self.log(f"=== {title} (exit {status})\n$ {shlex.join(cmd)}\n{output}\n")
            return status, output
        try:
            output, _ = process.communicate(timeout=timeout)
            status = process.returncode
        except subprocess.TimeoutExpired:
            output = distributed.terminate_group(process)
            output += f"\n[timed out after {timeout} s; group killed]\n"
            status = TIMEOUT_STATUS
        except BaseException:
            partial = distributed.terminate_group(process)
            self.log(f"=== {title} (interrupted; group killed)\n$ {shlex.join(cmd)}\n{partial}\n")
            raise
        self.log(f"=== {title} (exit {status})\n$ {shlex.join(cmd)}\n{output}\n")
        for bad in BAD_MARKER_RE.findall(output):
            self.marker(False, "no_bad_markers", f"{title} reported a {bad} marker")
        return status, output

    def marker(self, ok: bool, name: str, detail: str = "") -> bool:
        """Emit one PASS or FAIL marker, to stdout and the log."""
        if ok:
            line = f"MIMIC_RESULT: PASS {name}"
            self.passes += 1
        else:
            line = f"MIMIC_RESULT: FAIL {name} -- {detail}"
            self.failures.append(f"{name}: {detail}")
        print(line, flush=True)
        self.log(line + "\n")
        return ok

    @contextlib.contextmanager
    def case(self, leg: str) -> Iterator[None]:
        """A compared case: it counts for its leg when none of its markers failed."""
        before = len(self.failures)
        yield
        if len(self.failures) == before:
            self.cases[leg] += 1

    def build(self, simulation: str, mpi: bool) -> bool:
        """Generate and build one test-build binary (non-MPI, or USE-MPI=yes)."""
        selectors = [f"MODEL={MODEL}", f"SIMULATION={simulation}", "TEST_BUILD=yes"]
        selectors.append("USE-MPI=yes" if mpi else "USE-MPI=")
        status = 0
        for targets in (["generate", "validate-build"], [f"-j{os.cpu_count() or 4}", "mimic"]):
            if status != 0:
                break
            title = f"make {' '.join(selectors)} {' '.join(targets)}"
            cmd = ["make", "--no-print-directory", *selectors, *targets]
            status, _ = self.run(cmd, title, timeout=BUILD_TIMEOUT)
            if status == TIMEOUT_STATUS:
                self.marker(False, "build_timeout", f"{title} timed out after {BUILD_TIMEOUT} s")
                raise BatteryStopped(f"{title} timed out")
        kind = "mpi" if mpi else "serial"
        return self.marker(
            status == 0, f"build_{kind}_{simulation}", f"build exited {status} (see {LOG_PATH})"
        )

    def launch(
        self, ntask: int | None, run_file: Path, title: str, timeout: int = RUN_TIMEOUT
    ) -> tuple[int, str]:
        """Run ./mimic serially (ntask None) or under the MPI launcher at -np ntask."""
        prefix = [] if ntask is None else [*self.mpirun, "-np", str(ntask)]
        return self.run([*prefix, "./mimic", str(run_file)], title, timeout)

    def run_marker(self, status: int, output: str, name: str) -> bool:
        """The PASS/FAIL marker for one launch: exit 0 and no leak report. A timeout also ends
        the leg (a hang repeats at every later launch)."""
        leaked = not check_no_memory_leaks(output)
        ok = self.marker(
            status == 0 and not leaked, name, f"exit {status}, memory leak reported: {leaked}"
        )
        if status == TIMEOUT_STATUS:
            raise LegStopped(name)
        return ok

    def restore(self) -> None:
        """Regenerate the caller's generated code; a failure fails the run."""
        selectors = [
            f"{key}={os.environ[key]}" for key in ("MODEL", "SIMULATION") if key in os.environ
        ]
        cmd = ["make", "--no-print-directory", *selectors, "generate"]
        status, _ = self.run(cmd, "restore generated code", timeout=RESTORE_TIMEOUT)
        caller = " ".join(selectors) or "the Makefile defaults"
        if status != 0:
            self.failures.append(f"could not restore generated code for {caller}")
            print(f"FAIL: could not restore generated code for {caller} (see {LOG_PATH})")
        else:
            print(f"Generated code restored for {caller}; rebuild the executable with 'make'.")

    # ------------------------------------------------------------------ shared checks

    def check_fixture_nonzero(self, label: str, run_dir: Path, base: str) -> None:
        """The seven fixture properties are finite and non-negative everywhere and non-zero in
        some rows, so no comparison of the leg is made over zeros."""
        tables = [table for _, _, table in galaxy_tables(run_dir, base)]
        rows = np.concatenate(tables) if tables else None
        problems = []
        for field in (FIXTURE_FIELDS if rows is not None else ()):
            values = np.asarray(rows[field], dtype=np.float64)
            if not np.isfinite(values).all() or (values < 0.0).any():
                problems.append(f"{field} is negative or non-finite")
            elif np.count_nonzero(values) == 0:
                problems.append(f"{field} is zero on all {len(values)} rows")
        count = 0 if rows is None else len(rows)
        self.marker(
            count > 0 and not problems,
            f"fixture_nonzero_{label}",
            f"{count} rows read; {'; '.join(problems) or 'no rows'}",
        )

    def check_statistics(self, label: str, reference: Path, master: Path, scheme: str) -> None:
        """The master's step statistics equal the reference's byte for byte (every attribute and
        dataset of RunProperties/RateIntegration), the run recorded the scheme it was given, and
        the statistics are not vacuous (some segment was integrated)."""
        want, got = read_statistics(reference), read_statistics(master)
        differing = sorted(
            name
            for name in want.entries.keys() | got.entries.keys()
            if want.entries.get(name) != got.entries.get(name)
        )
        self.marker(
            not differing and bool(want.entries),
            f"statistics_{label}",
            f"RateIntegration entries that differ from the reference's (or are missing): "
            f"{differing}; reference entries read: {len(want.entries)}",
        )
        self.marker(
            got.segments > 0 and got.scheme == scheme,
            f"statistics_nonvacuous_{label}",
            f"{got.segments} segments recorded under scheme {got.scheme!r}, expected {scheme!r}",
        )

    def compare(
        self, name, reference: Path, output: Path, left: str, right: str, created=False, need=False
    ):
        """The comparator, with --compare-created when ``created``: identity, a non-vacuous count
        of galaxies compared and, when ``need``, created rows on both sides."""
        cmd = [sys.executable, str(COMPARATOR), str(reference), str(output)]
        cmd += ["--left-label", left, "--right-label", right]
        if created:
            cmd.append("--compare-created")
        status, report = self.run(cmd, f"compare {left} with {right}")
        self.marker(status == 0, f"identity_{name}", f"comparator exited {status}")
        if status == TIMEOUT_STATUS:
            raise LegStopped(f"identity_{name}")
        passed = PASSED_RE.search(report)
        compared = int(passed.group(1)) if passed else None
        self.marker(
            compared is not None and compared > 0,
            f"galaxies_compared_{name}",
            f"comparator reported {compared} galaxies compared (PASSED: N galaxies)",
        )
        if need:
            found = CREATED_RE.search(report)
            counts = tuple(map(int, found.groups())) if found else None
            self.marker(
                counts is not None and min(counts) > 0,
                f"created_rows_compared_{name}",
                f"comparator reported created rows ({left}, {right}) = {counts}",
            )

    def check_row_order(self, name: str, reference_dir: Path, run_dir: Path, base: str) -> None:
        """Every snapshot's UniqueGalaxyID column, in file order (an MPI run's task partitions
        concatenated in task order), equals the reference's; the comparator matches by id and
        cannot see order."""
        want = distributed.read_row_order(reference_dir, base)
        got = distributed.read_row_order(run_dir, base)
        mismatched = sorted(
            key for key in want.keys() | got.keys() if want.get(key) != got.get(key)
        )
        self.marker(
            bool(want) and not mismatched,
            f"row_order_{name}",
            f"snapshots whose UniqueGalaxyID column differs in file order from the reference's "
            f"(or are missing on one side): {mismatched}; reference snapshots read: {len(want)}",
        )

    def check_layout(self, name, run_dir, config, serial_totals, output, ntask, nchunk) -> None:
        """-np 1 keeps the unsuffixed partition names and logs no partition line; more tasks
        write one partition per task and snapshot with the serial run's galaxy counts and
        resolving master links, and task 0 logs a partition headline naming the tasks (and the
        chunks, for a chunked run)."""
        base = config["output"]["output_filename"]
        snapshots = sorted(int(snap) for snap in config["output"]["snapshot_list"])
        want_files, want_groups = distributed.expected_layout(base, snapshots, ntask)
        found = distributed.partition_files(run_dir, base)
        master = distributed.read_master(run_dir / f"{base}.hdf5", snapshots)
        self.marker(
            found == want_files and master.groups == want_groups,
            f"layout_{name}",
            f"missing {sorted(want_files - found)}, unexpected {sorted(found - want_files)}, "
            f"master groups {master.groups}",
        )
        self.marker(
            master.totals == serial_totals and not master.dangling and bool(master.groups),
            f"master_{name}",
            f"TotHalosPerSnap sums {master.totals} vs serial {serial_totals}; "
            f"unresolvable Galaxies links {master.dangling}",
        )
        if ntask == 1 and nchunk is None:
            self.marker(
                distributed.PARTITION_LINE not in output,
                f"partition_log_{name}",
                "a partition line was logged",
            )
            return
        headline = distributed.CHUNKED_PARTITION_LINE
        if ntask > 1:
            headline = f"task 0: {distributed.PARTITION_LINE}"
        phrase = f"over {ntask} tasks"
        if nchunk is not None:
            phrase = distributed.chunk_log_phrase(ntask, nchunk)
        line = next((text for text in output.splitlines() if headline in text), "")
        self.marker(
            phrase in line,
            f"partition_log_{name}",
            f"task 0's partition headline {headline!r} with {phrase!r} is missing (got {line!r})",
        )

    # ------------------------------------------------------------------ cross-driver leg

    def real_runs(self, layout: str) -> None:
        """The three cross-driver cases on one layout of the real mini-Millennium data."""
        print(f"--- cross-driver runs: {layout} mini-Millennium", flush=True)
        base = real_config(layout)
        for case in CROSS_CASES:
            run_dir = WORK_ROOT / "cross" / case.name / layout
            config = rate_config(base, scheme=case.scheme, timestep=case.timestep)
            status, output = self.launch(
                None,
                distributed.write_run_file(config, run_dir),
                f"{layout} {case.name}",
                REAL_RUN_TIMEOUT,
            )
            if self.run_marker(status, output, f"run_{layout}_{case.name}"):
                self.real_outputs[(layout, case.name)] = run_dir

    def cross_driver_compare(self) -> None:
        """Per case: the fixture assertion, the comparator, the FoF groups' row order and the
        step statistics, vertical against horizontal."""
        print("--- cross-driver comparison", flush=True)
        for case in CROSS_CASES:
            vertical = self.real_outputs.get(("vertical", case.name))
            horizontal = self.real_outputs.get(("horizontal", case.name))
            if vertical is None or horizontal is None:
                self.marker(False, f"cross_driver_{case.name}", "a run of the pair failed")
                continue
            with self.case("cross-driver"):
                self.cross_driver_case(case, vertical, horizontal)

    def cross_driver_case(self, case: CrossCase, vertical: Path, horizontal: Path) -> None:
        base, name = "halos", case.name
        for layout, run_dir in (("vertical", vertical), ("horizontal", horizontal)):
            self.check_fixture_nonzero(f"{layout}_{name}", run_dir, base)
            recorded = read_timestep_scheme(run_dir / f"{base}.hdf5")
            self.marker(
                recorded == case.timestep,
                f"timestep_scheme_{layout}_{name}",
                f"RunProperties/TimestepScheme is {recorded!r}, the case is {case.timestep!r}",
            )
        self.compare(f"cross_{name}", vertical / base, horizontal / base, "vertical", "horizontal")
        groups = {
            "vertical": fof_groups(vertical, base),
            "horizontal": fof_groups(horizontal, base),
        }
        multi = sum(1 for rows in groups["vertical"].values() if len(rows) > 1)
        differing = sorted(
            key
            for key in groups["vertical"].keys() | groups["horizontal"].keys()
            if groups["vertical"].get(key) != groups["horizontal"].get(key)
        )
        self.marker(
            multi > 0 and not differing,
            f"fof_row_order_{name}",
            f"{len(groups['vertical'])} FoF groups read, {multi} with more than one tree row; "
            f"{len(differing)} differ in row sequence between the drivers (first: {differing[:3]})",
        )
        self.check_statistics(
            f"cross_{name}", vertical / f"{base}.hdf5", horizontal / f"{base}.hdf5", case.scheme
        )

    # ------------------------------------------------------------------ task-chunk and creation

    def serial_reference(self, key: str, config: dict) -> None:
        """The non-MPI serial run every other run of a key is compared with, and its fixture
        assertion."""
        run_dir = WORK_ROOT / key / "serial"
        status, output = self.launch(
            None, distributed.write_run_file(config, run_dir), f"{key} serial"
        )
        if not self.run_marker(status, output, f"serial_run_{key}"):
            raise LegStopped(f"serial_run_{key}")
        base = config["output"]["output_filename"]
        self.check_fixture_nonzero(key, run_dir, base)
        self.references[key] = run_dir

    def check_created_rows(self, key: str, config: dict) -> None:
        """The serial reference holds created rows (UniqueGalaxyID < 0), so the created-record
        path is never compared vacuously."""
        base = config["output"]["output_filename"]
        rows = [table for _, _, table in galaxy_tables(self.references[key], base)]
        created = sum(int(np.count_nonzero(table["UniqueGalaxyID"] < 0)) for table in rows)
        self.marker(created > 0, f"created_rows_{key}", f"{created} created rows in the reference")

    def chunked_serial(self, config: dict) -> None:
        """Serial runs at every CHUNK_COUNTS value against the serial forest_chunks: 1 run."""
        print("--- task-chunk: serial chunked runs", flush=True)
        for nchunk in CHUNK_COUNTS:
            run_dir = WORK_ROOT / "blocks" / f"chunks{nchunk}"
            derived = distributed.with_forest_chunks(config, nchunk)
            status, output = self.launch(
                None,
                distributed.write_run_file(derived, run_dir),
                f"blocks serial at forest_chunks {nchunk}",
            )
            if self.run_marker(status, output, f"serial_run_blocks_chunks{nchunk}"):
                with self.case("task-chunk"):
                    self.check_run("blocks", f"chunks{nchunk}", config, run_dir, output, 1, nchunk)

    def mpi_runs(self, leg: str, key: str, config: dict, with_chunks: bool) -> None:
        """MPI runs of one key at every NP_COUNTS rank count and, for the task-chunk leg, every
        NP_CHUNK_PAIRS pair."""
        print(f"--- {leg}: MPI runs", flush=True)
        pairs = [(ntask, None) for ntask in NP_COUNTS]
        pairs += list(NP_CHUNK_PAIRS) if with_chunks else []
        for ntask, nchunk in pairs:
            label = f"np{ntask}" if nchunk is None else f"np{ntask}_chunks{nchunk}"
            run_dir = WORK_ROOT / key / label
            derived = config if nchunk is None else distributed.with_forest_chunks(config, nchunk)
            title = f"{key} at -np {ntask}" + (f" with forest_chunks {nchunk}" if nchunk else "")
            status, output = self.launch(ntask, distributed.write_run_file(derived, run_dir), title)
            if self.run_marker(status, output, f"mpi_run_{key}_{label}"):
                with self.case(leg):
                    self.check_run(key, label, config, run_dir, output, ntask, nchunk)

    def check_run(self, key, label, config, run_dir, output, ntask, nchunk) -> None:
        """One run against its key's serial reference: layout, comparator with --compare-created,
        row order and step statistics."""
        reference_dir = self.references[key]
        base = config["output"]["output_filename"]
        name = f"{key}_{label}"
        serial_totals = distributed.read_master(
            reference_dir / f"{base}.hdf5",
            sorted(int(snap) for snap in config["output"]["snapshot_list"]),
        ).totals
        self.check_layout(name, run_dir, config, serial_totals, output, ntask, nchunk)
        self.compare(
            name, reference_dir / base, run_dir / base, "serial", label, True, key == "created"
        )
        self.check_row_order(name, reference_dir, run_dir, base)
        self.check_statistics(
            name,
            reference_dir / f"{base}.hdf5",
            run_dir / f"{base}.hdf5",
            config["modules"]["rate_integration"]["scheme"],
        )

    # ------------------------------------------------------------------ permutation leg

    def permutation(self, config: dict) -> None:
        """Every order of the four rate modules, under each scheme, on one binary."""
        base = config["output"]["output_filename"]
        for scheme in SCHEMES:
            print(f"--- permutation: {scheme} scheme", flush=True)
            reference_dir = None
            recorded = set()
            for index, order in enumerate(ORDERS):
                run_dir = WORK_ROOT / "permutation" / scheme / f"order{index:02d}"
                name = f"{scheme}_order{index:02d}"
                status, output = self.launch(
                    None,
                    distributed.write_run_file(
                        rate_config(config, scheme=scheme, order=order), run_dir
                    ),
                    f"permutation {scheme} {' '.join(order)}",
                )
                if not self.run_marker(status, output, f"run_permutation_{name}"):
                    if index == 0:
                        break  # no reference to compare the rest with
                    continue
                recorded.add(tuple(read_enabled_modules(run_dir / f"{base}.hdf5")))
                if index == 0:
                    reference_dir = run_dir
                    self.check_fixture_nonzero(f"permutation_{scheme}", run_dir, base)
                    continue
                with self.case("permutation"):
                    self.check_permuted(name, reference_dir, run_dir, base, order, scheme)
            else:
                self.marker(
                    len(recorded) == len(ORDERS),
                    f"permutation_orders_distinct_{scheme}",
                    f"{len(recorded)} distinct recorded EnabledModules orders over "
                    f"{len(ORDERS)} runs, so the runs did not all differ in configured order",
                )

    def check_permuted(self, name, reference_dir, run_dir, base, order, scheme) -> None:
        """Galaxies datasets byte-identical to the first order's (field by field, in row order),
        the same step statistics, and EnabledModules recording this run's configured order."""
        recorded = tuple(read_enabled_modules(run_dir / f"{base}.hdf5"))
        configured = (*order, JUMP_MODULE)
        self.marker(
            recorded == configured,
            f"enabled_modules_{name}",
            f"EnabledModules records {recorded}, the run file lists {configured}",
        )
        want, got = galaxy_bytes(reference_dir, base), galaxy_bytes(run_dir, base)
        differing = sorted(key for key in want.keys() | got.keys() if want.get(key) != got.get(key))
        self.marker(
            bool(want) and not differing,
            f"galaxies_bytes_{name}",
            f"Galaxies fields that differ from the first order's (file, group, field): "
            f"{differing[:4]} ({len(differing)} in all); datasets read: {len(want)}",
        )
        self.check_row_order(name, reference_dir, run_dir, base)
        self.check_statistics(
            name, reference_dir / f"{base}.hdf5", run_dir / f"{base}.hdf5", scheme
        )

    # ------------------------------------------------------------------ the battery

    def guarded(self, leg: str, step) -> None:
        """Run one leg's step; a launch that timed out has recorded its failure, and ends it."""
        if leg in self.legs:
            try:
                step()
            except LegStopped as stop:
                print(f"Leg {leg} stopped early: {stop}; its failure marker is recorded")

    def run_all(self) -> None:
        """Build each pair once and run, in turn, the legs that use it."""
        rate_file = distributed.load_run_file(RATE_RUN_FILE)
        blocks = rate_config(rate_file, scheme="explicit")
        created = rate_config(rate_file, scheme="explicit", creation=True)

        if "cross-driver" in self.legs and self.build(VERTICAL_SIMULATION, mpi=False):
            self.guarded("cross-driver", lambda: self.real_runs("vertical"))
        if not self.build(HORIZONTAL_SIMULATION, mpi=False):
            return
        self.guarded("cross-driver", lambda: self.real_runs("horizontal"))
        if "cross-driver" in self.legs:
            self.guarded("cross-driver", self.cross_driver_compare)

        def blocks_serial():
            print("--- task-chunk: serial reference", flush=True)
            self.serial_reference("blocks", blocks)
            self.chunked_serial(blocks)

        def creation_serial():
            print("--- record-creation: serial reference", flush=True)
            self.serial_reference("created", created)
            self.check_created_rows("created", created)

        self.guarded("task-chunk", blocks_serial)
        self.guarded("permutation", lambda: self.permutation(blocks))
        self.guarded("record-creation", creation_serial)

        if not (set(self.legs) & {"task-chunk", "record-creation"}):
            return
        if not self.build(HORIZONTAL_SIMULATION, mpi=True):
            return
        if "blocks" in self.references:
            self.guarded("task-chunk", lambda: self.mpi_runs("task-chunk", "blocks", blocks, True))
        if "created" in self.references:
            self.guarded(
                "record-creation",
                lambda: self.mpi_runs("record-creation", "created", created, False),
            )

    def check_case_counts(self) -> None:
        """Each selected leg compared exactly the cases its design implies."""
        for leg in self.legs:
            expected = EXPECTED_CASES[leg]
            self.marker(
                self.cases[leg] == expected,
                f"cases_{leg.replace('-', '_')}",
                f"{self.cases[leg]} of {expected} cases passed every check",
            )
            print(f"  {leg}: {self.cases[leg]}/{expected} cases", flush=True)


# ---------------------------------------------------------------------- run files


def rate_config(
    config: dict,
    scheme: str,
    timestep: str = "fixed",
    order: tuple[str, ...] | None = None,
    creation: bool = False,
) -> dict:
    """A copy of a parsed run file whose rate phase runs under ``scheme``.

    ``timestep`` ``dynamic`` sets ``TimestepScheme: dynamic`` (fixed leaves the key absent, as the
    committed run files do); ``order`` lists the rate modules in the rate phase in that order;
    ``creation`` puts ``test_fixture`` creating records in ``pre_timestep``, ahead of the phase.
    """
    derived = copy.deepcopy(config)
    modules = derived["modules"]
    if timestep == "dynamic":
        derived["TimestepScheme"] = "dynamic"
    modules["rate_integration"] = {"scheme": scheme}
    if order is not None:
        modules["phases"][RATE_PHASE] = [{name: "process_rates"} for name in order]
    if creation:
        modules["parameters"].update(CREATOR_PARAMS)
        derived["modules"] = {"pre_timestep": [{CREATOR: "process_full_halo"}], **modules}
    return derived


def real_config(layout: str) -> dict:
    """The committed ``halos-only`` run file of one mini-Millennium layout, with the rate
    fixtures' modules and parameters (``forest_blocks_rate.yaml``'s) in place of its own."""
    config = yaml.safe_load((REPO_ROOT / REAL_RUN_FILES[layout]).read_text())
    config["modules"] = copy.deepcopy(distributed.load_run_file(RATE_RUN_FILE)["modules"])
    return config


# ---------------------------------------------------------------------- output readers


def galaxy_tables(run_dir: Path, base: str) -> list[tuple[str, str, np.ndarray]]:
    """Every ``Galaxies`` table of a run, as (file name, snapshot group, rows).

    Files come in ascending (number, task) order and groups in name order, so a vertical run's
    files and a distributed run's task partitions are read in the order a serial run writes them.
    """
    pattern = re.compile(rf"{re.escape(base)}_(\d+)(?:_task(\d+))?\.hdf5")
    found = []
    for path in run_dir.glob(f"{base}_*.hdf5"):
        match = pattern.fullmatch(path.name)
        if match is not None:
            found.append((int(match.group(1)), int(match.group(2) or -1), path))
    tables = []
    for _, _, path in sorted(found):
        with h5py.File(path, "r") as handle:
            for group in sorted(key for key in handle if key.startswith("Snap")):
                if "Galaxies" in handle[group]:
                    tables.append((path.name, group, handle[group]["Galaxies"][:]))
    return tables


def galaxy_bytes(run_dir: Path, base: str) -> dict[tuple[str, str, str], bytes]:
    """The raw bytes of every field of every ``Galaxies`` table, keyed (file, group, field)."""
    return {
        (name, group, field): np.ascontiguousarray(table[field]).tobytes()
        for name, group, table in galaxy_tables(run_dir, base)
        for field in table.dtype.names
    }


def fof_groups(run_dir: Path, base: str) -> dict[tuple[str, int], tuple[int, ...]]:
    """Each FoF group's tree rows in output order: (snapshot group, ``UniqueCentralGalaxyID``)
    to the ``UniqueGalaxyID`` sequence. Created rows (negative ids) are left out: their ids
    differ between drivers."""
    groups: dict[tuple[str, int], list[int]] = {}
    for _, group, table in galaxy_tables(run_dir, base):
        for uid, central in zip(
            table["UniqueGalaxyID"].tolist(), table["UniqueCentralGalaxyID"].tolist(), strict=True
        ):
            if uid > 0:
                groups.setdefault((group, central), []).append(uid)
    return {key: tuple(rows) for key, rows in groups.items()}


def attribute_text(value) -> str:
    """A string attribute's text; RunProperties strings are one-element fixed-width arrays."""
    value = np.asarray(value).ravel()[0]
    return value.decode() if isinstance(value, bytes) else str(value)


def read_statistics(master: Path) -> Statistics:
    """The master file's ``RunProperties/RateIntegration``, or an empty reading without it."""
    with h5py.File(master, "r") as handle:
        if STATISTICS not in handle:
            return Statistics({}, 0, "")
        group = handle[STATISTICS]
        entries = {f"@{name}": np.asarray(value).tobytes() for name, value in group.attrs.items()}
        for name in group:
            entries[name] = np.asarray(group[name][()]).tobytes()
        segments = int(group["StepTotals"][0]["segments"])
        return Statistics(entries, segments, attribute_text(group.attrs["scheme"]))


def read_timestep_scheme(master: Path) -> str:
    with h5py.File(master, "r") as handle:
        return attribute_text(handle["RunProperties"].attrs["TimestepScheme"])


def read_enabled_modules(master: Path) -> list[str]:
    """The module names of ``RunProperties/EnabledModules``, in configured order."""
    with h5py.File(master, "r") as handle:
        return [row[0].decode() for row in handle[ENABLED_MODULES][:]]


# ---------------------------------------------------------------------- driver


def raise_on_signal(signum, _frame):
    raise SystemExit(128 + signum)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--only", choices=LEGS, action="append")
    group.add_argument("--offline", action="store_true", help="the legs needing no external data")
    args = parser.parse_args(argv)
    legs = (
        OFFLINE_LEGS
        if args.offline
        else tuple(leg for leg in LEGS if not args.only or leg in args.only)
    )

    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    LOG_PATH.write_text("")
    if WORK_ROOT.exists():
        remove_tree(WORK_ROOT)
    mpirun = shlex.split(os.environ.get("MPIRUN", "mpirun"))
    battery = Battery(mpirun, legs)
    print(f"Legs: {', '.join(legs)}; launcher: {shlex.join(mpirun)}; log: {LOG_PATH}", flush=True)
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
        signal.signal(signum, raise_on_signal)
    try:
        battery.run_all()
        battery.check_case_counts()
    except BatteryStopped as stop:
        print(f"Battery stopped early: {stop}; its failure marker is recorded", flush=True)
    finally:
        battery.restore()

    if battery.failures:
        print(f"FAIL: tests-rate ({len(battery.failures)} failure(s); see {LOG_PATH})")
        for failure in battery.failures:
            print(f"  {failure}")
        return 1
    summary = ", ".join(f"{leg} {battery.cases[leg]} cases" for leg in legs)
    if legs not in (LEGS, OFFLINE_LEGS):
        print(
            f"PARTIAL: tests-rate (legs {', '.join(legs)} only; {battery.passes} checks passed, "
            f"but a filtered run is not the gate; log: {LOG_PATH})"
        )
        return 3
    print(
        f"PASS: tests-rate ({battery.passes} checks over {summary}; every leg asserted non-zero "
        f"fixture properties; no skips; log: {LOG_PATH})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
