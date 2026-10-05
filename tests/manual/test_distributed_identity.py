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
3. The version 2 refusal: ``halos-only`` on the version 2 fixture
   ``simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/`` under ``-np 2`` must fail at
   startup with the format_version 2 message.

Every build and launch runs in its own session (process group) under a timeout (``RUN_TIMEOUT``
for a run, short because the fixture runs take seconds; ``BUILD_TIMEOUT`` for a build). The first
launch of a leg that times out is recorded as that leg's failure and ends the leg, so a collective
deadlock costs one timeout per model rather than one per rank count; a build that times out is
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
        """Serial reference, then the MPI build at every rank count, for one model."""
        print(f"--- {model} x {SIMULATION}", flush=True)
        base_file = REPO_ROOT / RUN_FILE_DIR / f"forest_blocks_{model}.yaml"
        config = yaml.safe_load(base_file.read_text())
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

        if not self.build(model, SIMULATION, mpi=True):
            return
        for ntask in NP_COUNTS:
            run_dir = WORK_ROOT / model / f"np{ntask}"
            run_file = write_run_file(config, run_dir)
            status, output = self.launch(ntask, run_file, f"{model} at -np {ntask}")
            name = f"{model}_np{ntask}"
            if not self.marker(status == 0, f"mpi_run_{name}", f"exit {status}"):
                if status == TIMEOUT_STATUS:
                    break  # a hang repeats at every count; the recorded failure ends the leg
                continue
            if ntask == 1:
                self.check_serial_layout(name, run_dir, base, snapshots, output)
            else:
                self.check_task_layout(name, run_dir, base, snapshots, ntask, serial_totals, output)
            status, report = self.run(
                [
                    sys.executable,
                    str(COMPARATOR),
                    str(serial_dir / base),
                    str(run_dir / base),
                    "--left-label",
                    "serial",
                    "--right-label",
                    f"np{ntask}",
                    "--compare-created",
                ],
                f"compare {model} serial with -np {ntask}",
            )
            self.marker(status == 0, f"identity_{name}", f"comparator exited {status}")
            if status == TIMEOUT_STATUS:
                break
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
                    f"comparator reported created rows (serial, np{ntask}) = {counts}",
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

    def check_task_layout(self, name, run_dir, base, snapshots, ntask, serial_totals, output):
        """-np N > 1: every partition file, N resolving task groups per snapshot summing to the
        serial count, and the log line."""
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
    print(
        f"PASS: tests-distributed ({gate.passes} checks: MPI control test, "
        f"{len(models)} model(s) at -np {', '.join(map(str, NP_COUNTS))}, version 2 refusal; "
        f"no skips; log: {LOG_PATH})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
