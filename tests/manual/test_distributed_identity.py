#!/usr/bin/env python3
"""
Gate the distributed horizontal driver on per-UniqueGalaxyID identity with the serial run.

Usage::

    MODEL=<caller model> SIMULATION=<caller simulation> [MPIRUN="mpirun --oversubscribe"] \\
        python tests/manual/test_distributed_identity.py [--only halos-only|sage16|sham|hod]

``make tests-distributed`` runs it. ``--only`` (repeatable) is a development aid: a filtered run
is not the gate, so it ends with a ``PARTIAL:`` summary and exit status 3 even when every check
it ran passed. ``MPIRUN`` is the launcher command (default ``mpirun``;
CI uses ``mpirun --oversubscribe``), split with shell quoting rules and given ``-np <N>``.
Needs Open MPI (or another MPI whose ``mpicc``/``mpirun`` are on PATH), HDF5 and libyaml; needs
no real dataset.

Steps, each reported as ``MIMIC_RESULT:`` markers and gated:

1. The MPI control test ``tests/mpi/test_snapshot_collectives_mpi.c``: compiled with
   ``mpicc -DMPI -O2`` and the house warning set against ``src/core/snapshot_collectives.c`` and
   the util sources only (none includes ``yaml.h``, so no libyaml flags), after refreshing
   ``build/generated/git_version.h`` (the test file stubs the running-callback accessor as NONE;
   ``module_registry.c`` is not linked), and run at ``-np 3`` under a timeout; all
   ``CONTROL_CASES`` cases must pass, so a case removed from the test fails the gate.
2. For each of ``halos-only``, ``sage16``, ``sham`` and ``hod`` on ``mini-millennium-horizontal``
   (the committed ``forest_blocks`` fixture, six forests over seven gapped snapshots, through
   ``simulations/mini-millennium-horizontal/_tests/input/forest_blocks_<model>.yaml``):
   build the non-MPI binary and run the fixture serially (for ``sham`` the serial ``SHAM audit``
   lines must each show ``assigned >= 1`` and ``masked >= 1``); build with ``USE-MPI=yes`` and run
   under ``$MPIRUN -np N`` for N in 1, 2, 3, 4, 8. At ``-np 1`` the output must use today's
   unsuffixed partition names (``<base>_<snap>.hdf5``, master groups ``File<snap>``) and the log
   must hold no partition line. At every other count the master must hold, for every requested
   snapshot, exactly ``File<snap>_task<t>`` for t in [0, N) whose ``TotHalosPerSnap`` sum to the
   serial master's, every expected partition file ``<base>_<snap>_task<t>.hdf5`` must exist and
   every master ``Galaxies`` external link must resolve, and the log must hold task 0's
   ``Distributed horizontal partition`` line naming N tasks. Every leg is checked for vacuity: the
   serial master must hold halos, and every count's comparison must report ``PASSED: N galaxies``
   with N > 0. Every count's output must pass
   ``scripts/compare_cross_format_identity.py --compare-created`` against the serial output, and
   for ``hod`` the comparator must report created rows (``UniqueGalaxyID < 0``) on both sides, so
   the created-record path is never compared vacuously. Both binaries of a leg are production
   builds (``TEST_BUILD=no``); the non-MPI one is built with ``USE-MPI=`` given explicitly and any
   ``USE-MPI`` in the environment removed, so the serial reference is non-MPI whatever the caller
   exported (the Makefile enables MPI for any non-empty value).
3. Chunked legs (``input.forest_chunks``, set by editing the parsed run file) for ``halos-only``,
   ``sage16`` and ``hod``: with the non-MPI binary, serial runs at ``forest_chunks`` 2, 3 and 8
   (run directories ``chunks<G>``), and with the MPI binary, ``-np 2`` at ``forest_chunks: 2`` and
   ``-np 3`` at ``forest_chunks: 3`` (``np2_chunks2``, ``np3_chunks3``). Each is compared with
   the serial ``forest_chunks: 1`` run through the comparator with the same identity,
   ``galaxies_compared_`` and (for ``hod``) ``created_rows_compared_`` markers as the rank
   counts; each serial chunked run must also hold, in every partition file, the reference's
   ``UniqueGalaxyID`` column in the same file order (``row_order_``); each MPI chunked run gets the
   task-layout checks above. Every chunked run's task 0 partition headline must name its task and
   chunk counts (``Chunked horizontal partition: ... over 1 task in G chunks each`` serially,
   ``Distributed horizontal partition: ... over N tasks in G chunks each`` under MPI;
   ``chunk_log_``), so a run that swept unchunked fails. The shipped ``hod`` fixture run file
   configures the snapshot audit, which chunking refuses, so ``hod``'s chunked legs run
   ``forest_blocks_hod_chunked.yaml`` (the same run without ``modules.post_snapshot``, so the
   full-halo creation path is gated with created rows on both sides) against a serial reference
   of that variant (``chunked_reference``, itself checked for halos). For ``sham`` and the
   shipped ``hod`` run file, one serial launch at ``forest_chunks: 2`` must fail at configuration
   with the snapshot-scope refusal (``never holds a whole snapshot`` and ``needs forest_chunks:
   1``) and never log ``Opened horizontal run`` (``chunked_refused_``).
4. The version 2 refusal: ``halos-only`` on the version 2 fixture
   ``simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/`` under ``-np 2`` must fail at
   startup with the format_version 2 message.

Every build and launch runs in its own session (process group) under a timeout (``RUN_TIMEOUT``
for a run, short because the fixture runs take seconds; ``BUILD_TIMEOUT`` for a build). The first
launch or comparison of a leg that times out is recorded as that leg's failure and ends the leg
(no further build or launch for that model, chunked legs included), so a collective deadlock costs
one timeout per model rather than one per rank count or chunk count; a build that times out is
recorded and ends the whole gate, since the same build would hang again for every later model and
the job's budget must leave room for the log step. On a timeout or an
interrupt the whole group (``mpirun`` and its ranks, or ``make`` and its compilers) gets SIGTERM
and, after a short grace, SIGKILL whether or not its leader has exited, and is reaped before the
gate continues or restores the generated code, so a hang fails the gate instead of stalling it or
leaving ranks behind. Any FAIL, ERROR or SKIP marker fails the run; the gate has no legitimate skip
(a missing ``mpicc`` or ``mpirun`` is a failure). The whole output goes to
``build/distributed_tests.log``; run outputs go to ``output/distributed-identity/gate/`` (replaced
on each run).

The caller's generated code (``MODEL``/``SIMULATION`` from the environment, else the Makefile
defaults) is regenerated in a ``finally`` block, and SIGHUP/SIGINT/SIGQUIT/SIGTERM are converted
to SystemExit so that block runs on an interrupt too. A failed restore fails the run. The
executable is left the last leg's MPI build: rebuild it with ``make``.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

import h5py
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
SIMULATION = "mini-millennium-horizontal"
MODELS = ("halos-only", "sage16", "sham", "hod")
NP_COUNTS = (1, 2, 3, 4, 8)
# Chunked legs: serial runs at each forest_chunks value, and (rank count, forest_chunks) MPI pairs.
# The fixture's widest-slab weights [2, 1, 7, 2, 2, 3] give two distinct non-trivial chunkings
# (G = 2, and every G >= 3), and idle chunks at G = 8.
CHUNK_COUNTS = (2, 3, 8)
NP_CHUNK_PAIRS = ((2, 2), (3, 3))
# Models with chunked legs; models whose fixture run file is refused under chunking (it configures
# modules.post_snapshot); and the variant run file whose chunked legs stand in for a refused one
# (hod without its audit, so its full-halo creation path is gated). sham has no chunkable form.
CHUNKED_MODELS = ("halos-only", "sage16", "hod")
CHUNK_REFUSED_MODELS = ("sham", "hod")
CHUNKED_RUN_FILE = {"hod": "forest_blocks_hod_chunked.yaml"}
RUN_FILE_DIR = Path("simulations") / SIMULATION / "_tests" / "input"
WORK_ROOT = REPO_ROOT / "output" / "distributed-identity" / "gate"
LOG_PATH = REPO_ROOT / "build" / "distributed_tests.log"
COMPARATOR = Path("scripts") / "compare_cross_format_identity.py"

CONTROL_SOURCE = Path("tests") / "mpi" / "test_snapshot_collectives_mpi.c"
CONTROL_BINARY = REPO_ROOT / "build" / "test_snapshot_collectives_mpi"
CONTROL_TASKS = 3
# The TEST_RUN cases of tests/mpi/test_snapshot_collectives_mpi.c. A constant rather than a count
# of the source's TEST_RUN( substrings (which also matches comments) or of the summary line (which
# reports whatever ran): a case dropped from the test then fails the gate instead of passing it.
CONTROL_CASES = 9
GIT_VERSION_H = REPO_ROOT / "build" / "generated" / "git_version.h"
# The house warning set (AGENTS.md: compile clean under these on Clang and GCC).
HOUSE_WARNINGS = ["-Wall", "-Wextra", "-Wshadow", "-Wformat-security", "-Wundef"]
CONTROL_SOURCES = [REPO_ROOT / "src" / "core" / "snapshot_collectives.c"] + sorted(
    (REPO_ROOT / "src" / "util").glob("*.c"), key=str
)
CONTROL_INCLUDES = [
    "src",
    "src/include",
    "src/include/generated",
    "src/util",
    "src/core",
    "src/io",
    "src/module_system",
    "tests",
    "build/generated",
]

V2_MODEL = "halos-only"
V2_SIMULATION = "micro-uchuu-ascii-horizontal"
V2_DATA = Path("simulations") / V2_SIMULATION / "_tests" / "data" / "generic"
V2_A_LIST = V2_DATA / "micro-uchuu-fixture.a_list"
V2_MESSAGE = "this is a format_version 2 dataset"

PARTITION_LINE = "Distributed horizontal partition:"
CHUNKED_PARTITION_LINE = "Chunked horizontal partition:"  # task 0's headline for a serial G > 1
CHUNK_REFUSAL_MESSAGES = ("never holds a whole snapshot", "needs forest_chunks: 1")
OPENED_LINE = "Opened horizontal run"
PASSED_RE = re.compile(r"^PASSED: (\d+) galaxies", re.MULTILINE)
CREATED_RE = re.compile(
    r"^Created rows \(UniqueGalaxyID < 0\).*: \S+ (\d+), \S+ (\d+)$", re.MULTILINE
)
SHAM_AUDIT_RE = re.compile(r"SHAM audit z=\S+ candidates=(\d+) assigned=(\d+) masked=(\d+)")
MARKER_RE = re.compile(r"^MIMIC_RESULT: (PASS|WARN|FAIL|ERROR|SKIP)\b.*$", re.MULTILINE)
# Inherited state that must not reach the builds: make's recursion state, the test-build switch,
# and USE-MPI (an environment value would turn the serial reference into an MPI build).
MAKE_STATE = ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MIMIC_TEST_BUILD", "USE-MPI")
BUILD_TIMEOUT = 900
RESTORE_TIMEOUT = 300  # `make generate` alone; the gate has already stopped if this is reached late
RUN_TIMEOUT = 60  # fixture runs take seconds; a hang must cost little of the CI job's budget
TIMEOUT_STATUS = 124  # the status run() reports for a command it had to kill
KILL_GRACE = 5  # seconds between SIGTERM and SIGKILL of a timed-out or interrupted group


class GateStopped(Exception):
    """A make or compile timed out: the gate records the failure and stops rather than repeat the hang."""


class LegStopped(Exception):
    """A launch or comparison of one model's leg timed out: its failure marker is recorded and the
    leg ends, so a hang costs one RUN_TIMEOUT per model."""


class Gate:
    """Runs the steps in sequence, appending every command's output to one log."""

    def __init__(self, mpirun: list[str]):
        self.mpirun = mpirun
        self.failures: list[str] = []
        self.passes = 0
        self.env = {key: value for key, value in os.environ.items() if key not in MAKE_STATE}
        self.env["PATH"] = f"{Path(sys.executable).parent}{os.pathsep}{self.env.get('PATH', '')}"

    # ------------------------------------------------------------------ plumbing

    def log(self, text: str) -> None:
        with LOG_PATH.open("a") as handle:
            handle.write(text)

    def run(self, cmd: list[str], title: str, timeout: int = RUN_TIMEOUT) -> tuple[int, str]:
        """Run one command from the repository root; log and return (status, output).

        The command runs in its own session, so it and every process it starts share one
        process group. A timeout is status 124 with the partial output, so a hang fails its
        step; on a timeout or an interrupt (the SystemExit raised for a signal) the whole group
        is terminated and reaped first, and an interrupt is then re-raised.
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
            output = terminate_group(process) + f"\n[timed out after {timeout} s; group killed]\n"
            status = TIMEOUT_STATUS
        except BaseException:
            partial = terminate_group(process)
            self.log(f"=== {title} (interrupted; group killed)\n$ {shlex.join(cmd)}\n{partial}\n")
            raise
        self.log(f"=== {title} (exit {status})\n$ {shlex.join(cmd)}\n{output}\n")
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

    def make(self, model: str, simulation: str, *targets: str, mpi: bool = False) -> int:
        selectors = [f"MODEL={model}", f"SIMULATION={simulation}", "TEST_BUILD=no"]
        selectors.append("USE-MPI=yes" if mpi else "USE-MPI=")
        cmd = ["make", "--no-print-directory", *selectors, *targets]
        title = f"make {' '.join(selectors)} {' '.join(targets)}"
        status, _ = self.run(cmd, title, timeout=BUILD_TIMEOUT)
        if status == TIMEOUT_STATUS:
            # Every make of the gate stops it on a timeout: the same build would hang again for
            # every later model, and the CI job's budget must leave room for the log step.
            self.marker(False, "build_timeout", f"{title} timed out after {BUILD_TIMEOUT} s")
            raise GateStopped(f"{title} timed out")
        return status

    def build(self, model: str, simulation: str, mpi: bool) -> bool:
        """Generate and build one production binary (non-MPI or USE-MPI=yes)."""
        status = self.make(model, simulation, "generate", "validate-build")
        if status == 0:
            status = self.make(model, simulation, f"-j{os.cpu_count() or 4}", "mimic", mpi=mpi)
        kind = "mpi" if mpi else "serial"
        return self.marker(
            status == 0,
            f"build_{kind}_{model}_{simulation}",
            f"build exited {status} (see {LOG_PATH})",
        )

    def launch(self, ntask: int | None, run_file: Path, title: str) -> tuple[int, str]:
        """Run ./mimic serially (ntask None) or under the MPI launcher at -np ntask."""
        prefix = [] if ntask is None else [*self.mpirun, "-np", str(ntask)]
        return self.run([*prefix, "./mimic", str(run_file)], title)

    # ------------------------------------------------------------------ steps

    def control_test(self) -> None:
        """Compile and run the MPI control test of the collectives at -np 3."""
        print("--- MPI control test", flush=True)
        if self.make("halos-only", SIMULATION, "generate") != 0:
            self.marker(False, "mpi_control_test_build", "make generate failed")
            return
        # version.c and run_log.c include git_version.h, which `make generate` does not write
        # and a clean tree lacks; refresh it with the project's generator, as run_tests.sh does.
        status, _ = self.run(
            ["scripts/generate_git_version.sh", str(GIT_VERSION_H)], "generate git_version.h"
        )
        if status != 0:
            self.marker(False, "mpi_control_test_build", f"generate_git_version.sh exited {status}")
            return
        cmd = ["mpicc", "-DMPI", "-O2", *HOUSE_WARNINGS]
        cmd += [f"-I{path}" for path in CONTROL_INCLUDES]
        cmd += [str(REPO_ROOT / CONTROL_SOURCE), *map(str, CONTROL_SOURCES)]
        cmd += ["-lm", "-o", str(CONTROL_BINARY)]
        status, _ = self.run(cmd, "compile the MPI control test", timeout=BUILD_TIMEOUT)
        if not self.marker(status == 0, "mpi_control_test_build", f"mpicc exited {status}"):
            if status == TIMEOUT_STATUS:
                raise GateStopped("the MPI control test compile timed out")
            return
        status, output = self.run(
            [*self.mpirun, "-np", str(CONTROL_TASKS), str(CONTROL_BINARY)],
            f"MPI control test at -np {CONTROL_TASKS}",
        )
        markers = [m.group(1) for m in MARKER_RE.finditer(output)]
        passed = markers.count("PASS")
        bad = len(markers) - passed
        self.marker(
            status == 0 and bad == 0 and passed == CONTROL_CASES,
            "mpi_control_test",
            f"exit {status}, {passed}/{CONTROL_CASES} cases passed, "
            f"{bad} FAIL/ERROR/SKIP/WARN markers",
        )

    def leg(self, model: str) -> None:
        """Serial reference, the chunked serial runs, then the MPI build at every rank count and
        every chunked rank count, for one model. The first launch or comparison that times out
        ends the leg (LegStopped), with no further build or launch for the model."""
        print(f"--- {model} x {SIMULATION}", flush=True)
        try:
            self.leg_steps(model)
        except LegStopped:
            pass

    def leg_steps(self, model: str) -> None:
        config = load_run_file(f"forest_blocks_{model}.yaml")
        base = config["output"]["output_filename"]
        snapshots = sorted(int(snap) for snap in config["output"]["snapshot_list"])

        if not self.build(model, SIMULATION, mpi=False):
            return
        serial_dir = WORK_ROOT / model / "serial"
        status, output = self.launch(None, write_run_file(config, serial_dir), f"{model} serial")
        if not self.marker(status == 0, f"serial_run_{model}", f"exit {status}"):
            return
        if model == "sham":
            self.check_sham_audit(output)
        serial_totals = read_master(serial_dir / f"{base}.hdf5", snapshots).totals
        serial_halos = sum(serial_totals.values())
        self.marker(
            serial_halos > 0,
            f"serial_halos_{model}",
            f"the serial master's TotHalosPerSnap sums to {serial_halos}; "
            "every later comparison would be vacuous",
        )
        if model in CHUNK_REFUSED_MODELS:
            self.chunked_refusal(model, config)
        chunked = None
        if model in CHUNKED_MODELS:
            chunked = self.chunked_serial(model, config, serial_dir, serial_totals)

        if not self.build(model, SIMULATION, mpi=True):
            return
        for ntask in NP_COUNTS:
            run_dir = WORK_ROOT / model / f"np{ntask}"
            run_file = write_run_file(config, run_dir)
            status, output = self.launch(ntask, run_file, f"{model} at -np {ntask}")
            name = f"{model}_np{ntask}"
            if not self.run_marker(status, f"mpi_run_{name}"):
                continue
            if ntask == 1:
                self.check_serial_layout(name, run_dir, base, snapshots, output)
            else:
                self.check_task_layout(name, run_dir, base, snapshots, ntask, serial_totals, output)
            self.compare(model, name, serial_dir / base, run_dir / base)
        if chunked is not None:
            self.chunked_mpi(model, *chunked)

    def run_marker(self, status: int, name: str) -> bool:
        """The PASS/FAIL marker for one launch's exit status; a timeout also ends the leg (a hang
        repeats at every later launch of the model)."""
        ok = self.marker(status == 0, name, f"exit {status}")
        if status == TIMEOUT_STATUS:
            raise LegStopped(name)
        return ok

    def chunked_serial(self, model, config, serial_dir, serial_totals):
        """Serial runs at every CHUNK_COUNTS value against the serial forest_chunks: 1 run.

        Each must be identical per UniqueGalaxyID and hold every partition's rows in the
        reference's order. A model whose fixture run file is refused under chunking runs its
        variant instead, against its own serial reference (a non-vacuous one). Returns the
        (config, reference directory, reference totals) the chunked MPI runs compare with, or None
        when the variant's reference failed and the chunked legs are recorded as not run.
        """
        if model in CHUNKED_RUN_FILE:
            config = load_run_file(CHUNKED_RUN_FILE[model])
            base = config["output"]["output_filename"]
            snapshots = sorted(int(snap) for snap in config["output"]["snapshot_list"])
            serial_dir = WORK_ROOT / model / "chunked_reference"
            status, _ = self.launch(
                None, write_run_file(config, serial_dir), f"{model} chunked variant serial"
            )
            if not self.run_marker(status, f"serial_run_{model}_chunked"):
                return None
            serial_totals = read_master(serial_dir / f"{base}.hdf5", snapshots).totals
            serial_halos = sum(serial_totals.values())
            self.marker(
                serial_halos > 0,
                f"serial_halos_{model}_chunked",
                f"the variant's serial master's TotHalosPerSnap sums to {serial_halos}; "
                "every chunked comparison would be vacuous",
            )
        base = config["output"]["output_filename"]
        reference_order = read_row_order(serial_dir, base)
        for nchunk in CHUNK_COUNTS:
            run_dir = WORK_ROOT / model / f"chunks{nchunk}"
            run_file = write_run_file(with_forest_chunks(config, nchunk), run_dir)
            status, output = self.launch(
                None, run_file, f"{model} serial at forest_chunks {nchunk}"
            )
            name = f"{model}_chunks{nchunk}"
            if not self.run_marker(status, f"serial_run_{name}"):
                continue
            self.check_chunk_log(name, output, CHUNKED_PARTITION_LINE, 1, nchunk)
            self.compare(model, name, serial_dir / base, run_dir / base)
            order = read_row_order(run_dir, base)
            mismatched = sorted(
                key
                for key in reference_order.keys() | order.keys()
                if reference_order.get(key) != order.get(key)
            )
            self.marker(
                bool(reference_order) and not mismatched,
                f"row_order_{name}",
                f"partitions whose UniqueGalaxyID column differs in file order from the serial "
                f"forest_chunks: 1 run (or are missing on one side): {mismatched}; reference "
                f"partitions read: {len(reference_order)}",
            )
        return config, serial_dir, serial_totals

    def chunked_mpi(self, model, config, serial_dir, serial_totals) -> None:
        """MPI runs at each NP_CHUNK_PAIRS (rank count, forest_chunks) against the serial
        forest_chunks: 1 run, with the task-layout checks."""
        base = config["output"]["output_filename"]
        snapshots = sorted(int(snap) for snap in config["output"]["snapshot_list"])
        for ntask, nchunk in NP_CHUNK_PAIRS:
            run_dir = WORK_ROOT / model / f"np{ntask}_chunks{nchunk}"
            run_file = write_run_file(with_forest_chunks(config, nchunk), run_dir)
            status, output = self.launch(
                ntask, run_file, f"{model} at -np {ntask} with forest_chunks {nchunk}"
            )
            name = f"{model}_np{ntask}_chunks{nchunk}"
            if not self.run_marker(status, f"mpi_run_{name}"):
                continue
            self.check_task_layout(
                name, run_dir, base, snapshots, ntask, serial_totals, output, nchunk=nchunk
            )
            self.compare(model, name, serial_dir / base, run_dir / base)

    def chunked_refusal(self, model: str, config: dict) -> None:
        """The fixture run file at forest_chunks: 2 fails at configuration, before the run opens."""
        run_dir = WORK_ROOT / model / "chunked-refusal"
        run_file = write_run_file(with_forest_chunks(config, 2), run_dir)
        status, output = self.launch(None, run_file, f"{model} chunked refusal at forest_chunks 2")
        self.marker(
            status not in (0, TIMEOUT_STATUS)
            and all(message in output for message in CHUNK_REFUSAL_MESSAGES)
            and OPENED_LINE not in output,
            f"chunked_refused_{model}",
            f"exit {status}; expected a configuration failure naming "
            f"{' and '.join(map(repr, CHUNK_REFUSAL_MESSAGES))} before '{OPENED_LINE}'",
        )
        if status == TIMEOUT_STATUS:
            raise LegStopped(f"chunked_refused_{model}")

    def compare(self, model: str, name: str, reference: Path, output: Path) -> None:
        """Compare one run with its serial reference: the identity, non-vacuity and (for hod)
        created-row markers. A comparator timeout ends the leg after its marker."""
        label = name.removeprefix(f"{model}_")
        status, report = self.run(
            [
                sys.executable,
                str(COMPARATOR),
                str(reference),
                str(output),
                "--left-label",
                "serial",
                "--right-label",
                label,
                "--compare-created",
            ],
            f"compare {model} serial with {label}",
        )
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
        if model == "hod":
            created = CREATED_RE.search(report)
            counts = tuple(map(int, created.groups())) if created else None
            self.marker(
                counts is not None and min(counts) > 0,
                f"created_rows_compared_{name}",
                f"comparator reported created rows (serial, {label}) = {counts}",
            )

    def check_sham_audit(self, output: str) -> None:
        audits = [tuple(map(int, m.groups())) for m in SHAM_AUDIT_RE.finditer(output)]
        ok = bool(audits) and all(assigned >= 1 and masked >= 1 for _, assigned, masked in audits)
        self.marker(
            ok,
            "serial_sham_audit_assigns_and_masks",
            f"SHAM audit (candidates, assigned, masked) lines: {audits}",
        )

    def check_serial_layout(self, name, run_dir, base, snapshots, output) -> None:
        """-np 1: today's unsuffixed partition names and no partition log."""
        want_files, want_groups = expected_layout(base, snapshots, 1)
        found = partition_files(run_dir, base)
        groups = read_master(run_dir / f"{base}.hdf5", snapshots).groups
        self.marker(
            found == want_files and groups == want_groups,
            f"layout_{name}",
            f"partitions {sorted(found)}, master groups {groups}",
        )
        self.marker(
            PARTITION_LINE not in output, f"no_partition_log_{name}", "a partition line was logged"
        )

    def check_task_layout(
        self, name, run_dir, base, snapshots, ntask, serial_totals, output, nchunk=None
    ):
        """-np N > 1: every partition file, N resolving task groups per snapshot summing to the
        serial count, and the log line; with nchunk, also task 0's headline naming the chunks."""
        want_files, want_groups = expected_layout(base, snapshots, ntask)
        found = partition_files(run_dir, base)
        master = read_master(run_dir / f"{base}.hdf5", snapshots)
        self.marker(
            found == want_files,
            f"partition_files_{name}",
            f"missing {sorted(want_files - found)}, unexpected {sorted(found - want_files)}",
        )
        self.marker(
            master.groups == want_groups and master.totals == serial_totals,
            f"layout_{name}",
            f"master groups {master.groups}, TotHalosPerSnap sums {master.totals} "
            f"vs serial {serial_totals}",
        )
        self.marker(
            not master.dangling and bool(master.groups),
            f"master_links_{name}",
            f"unresolvable master Galaxies links: {master.dangling}",
        )
        line = next(
            (text for text in output.splitlines() if f"task 0: {PARTITION_LINE}" in text), ""
        )
        self.marker(
            f"over {ntask} tasks" in line,
            f"partition_log_{name}",
            f"task 0's partition line naming {ntask} tasks is missing (got {line!r})",
        )
        if nchunk is not None:
            self.check_chunk_log(name, output, f"task 0: {PARTITION_LINE}", ntask, nchunk)

    def check_chunk_log(self, name, output, headline, ntask, nchunk) -> None:
        """Task 0's partition headline names the task count and the chunk count, so a run that
        parsed forest_chunks but swept unchunked fails (an unchunked run's headline names no
        chunks, and a serial unchunked run logs none)."""
        phrase = chunk_log_phrase(ntask, nchunk)
        line = next((text for text in output.splitlines() if headline in text), "")
        self.marker(
            phrase in line,
            f"chunk_log_{name}",
            f"task 0's partition headline {headline!r} with {phrase!r} is missing (got {line!r})",
        )

    def version_2_refusal(self) -> None:
        """halos-only on the version 2 fixture under -np 2 fails at startup with the message."""
        print(f"--- {V2_MODEL} x {V2_SIMULATION} (version 2 refusal)", flush=True)
        if not self.build(V2_MODEL, V2_SIMULATION, mpi=True):
            return
        base_file = REPO_ROOT / "models" / V2_MODEL / "input" / f"{V2_MODEL}_{V2_SIMULATION}.yaml"
        config = yaml.safe_load(base_file.read_text())
        config.setdefault("input", {}).update(
            {"simulation_dir": str(V2_DATA), "snapshot_list_file": str(V2_A_LIST)}
        )
        last = sum(1 for line in (REPO_ROOT / V2_A_LIST).read_text().splitlines() if line.strip())
        config["output"]["snapshot_list"] = [last - 1]
        run_file = write_run_file(config, WORK_ROOT / "version-2-refusal")
        status, output = self.launch(2, run_file, "version 2 refusal at -np 2")
        self.marker(
            status not in (0, TIMEOUT_STATUS)
            and V2_MESSAGE in output
            and PARTITION_LINE not in output,
            "version_2_refused_at_np2",
            f"exit {status}; expected a startup failure naming '{V2_MESSAGE}'",
        )

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


def terminate_group(process: subprocess.Popen) -> str:
    """SIGTERM the process's group, SIGKILL it after KILL_GRACE seconds, and reap the leader.

    The SIGKILL is sent whether or not the leader has exited by then, so a descendant that
    ignores SIGTERM and has closed its streams cannot outlive the group (the group id stays
    valid while any member lives). Returns whatever output the group wrote before it ended.
    """
    output = None
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        output, _ = process.communicate(timeout=KILL_GRACE)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if output is None:
        output, _ = process.communicate()
    process.wait()
    return output or ""


def write_run_file(config: dict, run_dir: Path) -> Path:
    """Write the run file for one run into a fresh output directory and return its path."""
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    derived = dict(config)
    derived["output"] = dict(config["output"], output_directory=str(run_dir))
    run_file = run_dir / "run.yaml"
    run_file.write_text(yaml.safe_dump(derived, default_flow_style=False, sort_keys=False))
    return run_file


def chunk_log_phrase(ntask: int, nchunk: int) -> str:
    """The part of task 0's partition headline naming the task and chunk counts, e.g.
    ``over 1 task in 2 chunks each`` or ``over 3 tasks in 3 chunks each``."""
    return f"over {ntask} task{'' if ntask == 1 else 's'} in {nchunk} chunks each"


def load_run_file(name: str) -> dict:
    """Parse one of the fixture's run files under RUN_FILE_DIR."""
    return yaml.safe_load((REPO_ROOT / RUN_FILE_DIR / name).read_text())


def with_forest_chunks(config: dict, nchunk: int) -> dict:
    """A copy of a parsed run file with input.forest_chunks set (the original is not modified)."""
    derived = dict(config)
    derived["input"] = dict(config.get("input") or {}, forest_chunks=nchunk)
    return derived


def read_row_order(run_dir: Path, base: str) -> dict[str, list[int]]:
    """Each partition file's ``UniqueGalaxyID`` column in file order, keyed by file name.

    A partition holds one ``Snap<NNN>/Galaxies`` table; every such table in the file is read.
    A run directory without partitions gives an empty mapping.
    """
    order: dict[str, list[int]] = {}
    for path in sorted(run_dir.glob(f"{base}_*.hdf5")):
        with h5py.File(path, "r") as handle:
            column: list[int] = []
            for group in sorted(key for key in handle if key.startswith("Snap")):
                column.extend(int(uid) for uid in handle[group]["Galaxies"]["UniqueGalaxyID"])
            order[path.name] = column
    return order


def expected_layout(
    base: str, snapshots: list[int], ntask: int
) -> tuple[set[str], dict[int, list[str]]]:
    """The partition file names and master File* groups a run at -np ntask must produce.

    -np 1 keeps today's unsuffixed names (``<base>_<snap>.hdf5``, group ``File<snap>``); more
    tasks write one ``<base>_<snap>_task<t>.hdf5`` per snapshot and task, group
    ``File<snap>_task<t>``, a task holding no galaxies included.
    """
    suffixes = [""] if ntask == 1 else [f"_task{task:03d}" for task in range(ntask)]
    files = {f"{base}_{snap:03d}{suffix}.hdf5" for snap in snapshots for suffix in suffixes}
    groups = {snap: [f"File{snap:03d}{suffix}" for suffix in suffixes] for snap in snapshots}
    return files, groups


def partition_files(run_dir: Path, base: str) -> set[str]:
    """The partition file names a run wrote (the master ``<base>.hdf5`` is not one)."""
    return {path.name for path in run_dir.glob(f"{base}_*.hdf5")}


class MasterSummary(NamedTuple):
    """What the gate reads from a master file, in one pass."""

    groups: dict[int, list[str]]  # snapshot -> sorted File* group names
    totals: dict[int, int]  # snapshot -> TotHalosPerSnap summed over the File* groups
    dangling: list[str]  # Snap*/File*/Galaxies paths whose external link does not resolve


def read_master(master: Path, snapshots: list[int]) -> MasterSummary:
    """Open a master file once and summarise each requested snapshot.

    A missing master gives an empty summary, which fails every comparison made against it. A
    master ``Galaxies`` link resolves when h5py can open the partition dataset it names; a
    dangling external link makes ``handle.get`` return None.
    """
    groups: dict[int, list[str]] = {}
    totals: dict[int, int] = {}
    dangling: list[str] = []
    if not master.exists():
        return MasterSummary(groups, totals, dangling)
    with h5py.File(master, "r") as handle:
        for snap in snapshots:
            group = handle.get(f"Snap{snap:03d}", {})
            names = sorted(key for key in group if key.startswith("File"))
            groups[snap] = names
            totals[snap] = sum(int(group[key].attrs["TotHalosPerSnap"][0]) for key in names)
            for key in names:
                link = f"Snap{snap:03d}/{key}/Galaxies"
                try:
                    resolved = handle.get(link) is not None
                except OSError:
                    resolved = False
                if not resolved:
                    dangling.append(link)
    return MasterSummary(groups, totals, dangling)


def raise_on_signal(signum, _frame):
    raise SystemExit(128 + signum)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--only", choices=MODELS, action="append")
    args = parser.parse_args(argv)
    models = [model for model in MODELS if not args.only or model in args.only]

    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    LOG_PATH.write_text("")
    if WORK_ROOT.exists():
        shutil.rmtree(WORK_ROOT)
    mpirun = shlex.split(os.environ.get("MPIRUN", "mpirun"))
    gate = Gate(mpirun)
    print(f"Launcher: {shlex.join(mpirun)}; log: {LOG_PATH}", flush=True)
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
        signal.signal(signum, raise_on_signal)
    try:
        gate.control_test()
        for model in models:
            gate.leg(model)
        gate.version_2_refusal()
    except GateStopped as stop:
        print(f"Gate stopped early: {stop}; its failure marker is recorded", flush=True)
    finally:
        gate.restore()

    if gate.failures:
        print(f"FAIL: tests-distributed ({len(gate.failures)} failure(s); see {LOG_PATH})")
        for failure in gate.failures:
            print(f"  {failure}")
        return 1
    if args.only:
        print(
            f"PARTIAL: tests-distributed (models {', '.join(models)} only; {gate.passes} checks "
            f"passed, but a filtered run is not the gate; log: {LOG_PATH})"
        )
        return 3
    pairs = ", ".join(f"-np {ntask} x {nchunk}" for ntask, nchunk in NP_CHUNK_PAIRS)
    print(
        f"PASS: tests-distributed ({gate.passes} checks: MPI control test, "
        f"{len(models)} model(s) at -np {', '.join(map(str, NP_COUNTS))}, "
        f"chunked legs ({', '.join(CHUNKED_MODELS)}) at forest_chunks "
        f"{', '.join(map(str, CHUNK_COUNTS))} serial and {pairs}, "
        f"chunked refusal ({', '.join(CHUNK_REFUSED_MODELS)}), version 2 refusal; "
        f"no skips; log: {LOG_PATH})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
