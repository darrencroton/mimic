"""
Shared machinery of the manual batteries (``tests/manual/``): the rate battery, the distributed
identity gate and the snapshot-global battery.

``Battery`` is the base class of the three. It runs a command in its own process group under a
timeout and appends its output to one log (``run``), prints and logs every line the battery says
(``say``), keeps the PASS/FAIL marker bookkeeping (``marker``, ``fail``), builds a model and
simulation pair (``make``, ``build``), compares two runs through
``scripts/compare_cross_format_identity.py`` (``compare``), regenerates the caller's generated
code (``restore``) and wraps a body in the restore with SIGHUP/SIGINT/SIGQUIT/SIGTERM turned into
``SystemExit`` so that the restore runs on an interrupt too (``execute``). The module-level
helpers read and write the committed ``forest_blocks`` fixture's run files and outputs, which the
rate battery and the distributed gate both use.

Nothing here knows a battery's legs, markers or exit codes: each battery keeps its own.
"""

from __future__ import annotations

import os
import re
import shlex
import signal
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

import h5py
import yaml

from .fs import remove_tree

REPO_ROOT = Path(__file__).resolve().parents[2]

SIMULATION = "mini-millennium-horizontal"  # the package holding the forest_blocks fixture
RUN_FILE_DIR = Path("simulations") / SIMULATION / "_tests" / "input"
COMPARATOR = Path("scripts") / "compare_cross_format_identity.py"
NP_COUNTS = (1, 2, 3, 4, 8)
# Chunked legs: serial runs at each forest_chunks value, and (rank count, forest_chunks) MPI pairs.
# The fixture's peak weights [2, 1, 8, 2, 2, 3] (each forest's largest row count in any one slab)
# give two distinct non-trivial chunkings (G = 2, and every G >= 3), and idle chunks at G = 8.
CHUNK_COUNTS = (2, 3, 8)
NP_CHUNK_PAIRS = ((2, 2), (3, 3))

PARTITION_LINE = "Distributed horizontal partition:"
CHUNKED_PARTITION_LINE = "Chunked horizontal partition:"  # task 0's headline for a serial G > 1
PASSED_RE = re.compile(r"^PASSED: (\d+) galaxies", re.MULTILINE)
CREATED_RE = re.compile(
    r"^Created rows \(UniqueGalaxyID < 0\).*: \S+ (\d+), \S+ (\d+)$", re.MULTILINE
)
# Inherited state that must not reach the builds: make's recursion state, the test-build switch,
# and USE-MPI (an environment value would turn the serial reference into an MPI build).
MAKE_STATE = ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MIMIC_TEST_BUILD", "USE-MPI")
BUILD_TIMEOUT = 900
RESTORE_TIMEOUT = 300  # `make generate` alone; the battery has already stopped if this is reached
TIMEOUT_STATUS = 124  # the status run() reports for a command it had to kill
KILL_GRACE = 5  # seconds between SIGTERM and SIGKILL of a timed-out or interrupted group


class BatteryStopped(Exception):
    """A make or compile timed out: the battery records the failure and stops rather than repeat
    the hang."""


class LegStopped(Exception):
    """A launch or comparison of one leg timed out: its failure marker is recorded and the leg
    ends, so a hang costs one timeout per leg."""


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


def raise_on_signal(signum, _frame):
    raise SystemExit(128 + signum)


class Battery:
    """Base of the manual batteries: one log, one marker count, one failure list.

    A subclass sets ``default_timeout`` (seconds a ``run`` may take when its caller gives none;
    None for no limit) and may override ``run`` to inspect every command's output.
    """

    default_timeout: int | None = None

    def __init__(self, log_path: Path, mpirun: list[str] | None = None):
        self.log_path = log_path
        self.mpirun = mpirun or []
        self.failures: list[str] = []
        self.passes = 0
        self.env = {key: value for key, value in os.environ.items() if key not in MAKE_STATE}
        self.env["PATH"] = f"{Path(sys.executable).parent}{os.pathsep}{self.env.get('PATH', '')}"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("")

    # ------------------------------------------------------------------ output

    def log(self, text: str) -> None:
        with self.log_path.open("a") as handle:
            handle.write(text)

    def say(self, text: str = "") -> None:
        """Print one line of the battery's own output and append it to the log."""
        print(text, flush=True)
        self.log(text + "\n")

    def marker(self, ok: bool, name: str, detail: str = "") -> bool:
        """Emit one PASS or FAIL marker."""
        if ok:
            line = f"MIMIC_RESULT: PASS {name}"
            self.passes += 1
        else:
            line = f"MIMIC_RESULT: FAIL {name} -- {detail}"
            self.failures.append(f"{name}: {detail}")
        self.say(line)
        return ok

    def fail(self, message: str) -> None:
        """Record a failure that is not a marker."""
        self.say(f"FAIL: {message}")
        self.failures.append(message)

    # ------------------------------------------------------------------ commands

    def run(
        self,
        cmd: list[str],
        title: str,
        timeout: int | None = None,
        cwd: Path = REPO_ROOT,
        env: dict | None = None,
    ) -> tuple[int, str]:
        """Run one command in its own process group; log and return (status, output).

        The command runs in its own session, so it and every process it starts share one process
        group. A timeout is status 124 with the partial output, so a hang fails its step; on a
        timeout or an interrupt (the SystemExit raised for a signal) the whole group is
        terminated and reaped first, and an interrupt is then re-raised. The command runs from
        ``cwd`` (the repository root) with ``env`` (the battery's own).
        """
        timeout = self.default_timeout if timeout is None else timeout
        try:
            process = subprocess.Popen(
                cmd,
                cwd=cwd,
                env=self.env if env is None else env,
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

    def launch(
        self, ntask: int | None, run_file: Path, title: str, timeout: int | None = None
    ) -> tuple[int, str]:
        """Run ./mimic serially (ntask None) or under the MPI launcher at -np ntask."""
        prefix = [] if ntask is None else [*self.mpirun, "-np", str(ntask)]
        return self.run([*prefix, "./mimic", str(run_file)], title, timeout)

    def make(
        self, model: str, simulation: str, *targets: str, mpi: bool = False, test_build=False
    ) -> int:
        """One make of a pair; a timeout is recorded and stops the battery."""
        selectors = [f"MODEL={model}", f"SIMULATION={simulation}"]
        selectors.append(f"TEST_BUILD={'yes' if test_build else 'no'}")
        selectors.append("USE-MPI=yes" if mpi else "USE-MPI=")
        cmd = ["make", "--no-print-directory", *selectors, *targets]
        title = f"make {' '.join(selectors)} {' '.join(targets)}"
        status, _ = self.run(cmd, title, timeout=BUILD_TIMEOUT)
        if status == TIMEOUT_STATUS:
            # Every make stops the battery on a timeout: the same build would hang again for
            # every later pair, and the CI job's budget must leave room for the log step.
            self.marker(False, "build_timeout", f"{title} timed out after {BUILD_TIMEOUT} s")
            raise BatteryStopped(f"{title} timed out")
        return status

    def build(self, model: str, simulation: str, mpi: bool, test_build: bool, label: str) -> bool:
        """Generate and build one binary (non-MPI or USE-MPI=yes; production or test build);
        the marker is ``build_<serial|mpi>_<label>``."""
        pair = (model, simulation)
        status = self.make(*pair, "generate", "validate-build", mpi=mpi, test_build=test_build)
        if status == 0:
            jobs = f"-j{os.cpu_count() or 4}"
            status = self.make(*pair, jobs, "mimic", mpi=mpi, test_build=test_build)
        return self.marker(
            status == 0,
            f"build_{'mpi' if mpi else 'serial'}_{label}",
            f"build exited {status} (see {self.log_path})",
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
            self.fail(f"could not restore generated code for {caller} (see {self.log_path})")
        else:
            self.say(f"Generated code restored for {caller}; rebuild the executable with 'make'.")

    def execute(self, body) -> None:
        """Call ``body`` with the interrupt signals converted to SystemExit, a stop recorded as
        a failure rather than a crash, and the caller's generated code restored whatever happens."""
        for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
            signal.signal(signum, raise_on_signal)
        try:
            body()
        except BatteryStopped as stop:
            self.say(f"Battery stopped early: {stop}; its failure marker is recorded")
        finally:
            self.restore()

    def failed(self, target: str) -> bool:
        """Say the failure summary of ``target`` and return True when anything failed."""
        if not self.failures:
            return False
        self.say(f"FAIL: {target} ({len(self.failures)} failure(s); see {self.log_path})")
        for failure in self.failures:
            self.say(f"  {failure}")
        return True

    # ------------------------------------------------------------------ comparisons

    def compare(
        self,
        name: str,
        reference: Path,
        output: Path,
        left: str,
        right: str,
        created: bool = False,
        need: bool = False,
    ) -> None:
        """The comparator, with --compare-created when ``created``: identity, a non-vacuous count
        of galaxies compared and, when ``need``, created rows on both sides. A comparator timeout
        ends the leg after its marker."""
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
        want = read_row_order(reference_dir, base)
        got = read_row_order(run_dir, base)
        mismatched = sorted(
            key for key in want.keys() | got.keys() if want.get(key) != got.get(key)
        )
        self.marker(
            bool(want) and not mismatched,
            f"row_order_{name}",
            f"snapshots whose UniqueGalaxyID column differs in file order from the reference's "
            f"(or are missing on one side): {mismatched}; reference snapshots read: {len(want)}",
        )


# ---------------------------------------------------------------------- run files


def write_run_file(config: dict, run_dir: Path) -> Path:
    """Write the run file for one run into a fresh output directory and return its path."""
    if run_dir.exists():
        remove_tree(run_dir)
    run_dir.mkdir(parents=True)
    derived = dict(config)
    derived["output"] = dict(config["output"], output_directory=str(run_dir))
    run_file = run_dir / "run.yaml"
    run_file.write_text(yaml.safe_dump(derived, default_flow_style=False, sort_keys=False))
    return run_file


def load_run_file(name: str) -> dict:
    """Parse one of the fixture's run files under RUN_FILE_DIR."""
    return yaml.safe_load((REPO_ROOT / RUN_FILE_DIR / name).read_text())


def with_forest_chunks(config: dict, nchunk: int) -> dict:
    """A copy of a parsed run file with input.forest_chunks set (the original is not modified)."""
    derived = dict(config)
    derived["input"] = dict(config.get("input") or {}, forest_chunks=nchunk)
    return derived


def chunk_log_phrase(ntask: int, nchunk: int) -> str:
    """The part of task 0's partition headline naming the task and chunk counts, e.g.
    ``over 1 task in 2 chunks each`` or ``over 3 tasks in 3 chunks each``."""
    return f"over {ntask} task{'' if ntask == 1 else 's'} in {nchunk} chunks each"


# ---------------------------------------------------------------------- output readers


def read_row_order(run_dir: Path, base: str) -> dict[str, list[int]]:
    """Each snapshot's ``UniqueGalaxyID`` column in file order, keyed by snapshot number.

    A serial run holds one ``<base>_<snap>.hdf5`` per snapshot; an MPI run holds one
    ``<base>_<snap>_task<t>.hdf5`` per snapshot and task, whose columns are concatenated in task
    order (the zero-padded task suffix sorts that way). A partition holds one
    ``Snap<NNN>/Galaxies`` table; every such table in the file is read. A run directory without
    partitions gives an empty mapping.
    """
    pattern = re.compile(rf"{re.escape(base)}_(\d+)(?:_task\d+)?\.hdf5")
    order: dict[str, list[int]] = {}
    for path in sorted(run_dir.glob(f"{base}_*.hdf5")):
        match = pattern.fullmatch(path.name)
        if match is None:
            continue
        column = order.setdefault(match.group(1), [])
        with h5py.File(path, "r") as handle:
            for group in sorted(key for key in handle if key.startswith("Snap")):
                column.extend(int(uid) for uid in handle[group]["Galaxies"]["UniqueGalaxyID"])
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
    """What a battery reads from a master file, in one pass."""

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
