#!/usr/bin/env python3
"""
Gate the distributed horizontal driver on per-UniqueGalaxyID identity with the serial run.

Usage::

    MODEL=<caller model> SIMULATION=<caller simulation> [MPIRUN="mpirun --oversubscribe"] \\
        python tests/manual/test_distributed_identity.py [--only halos-only|sage16|sham|hod]

``make tests-distributed`` runs it. ``MPIRUN`` is the launcher command (default ``mpirun``;
CI uses ``mpirun --oversubscribe``), split with shell quoting rules and given ``-np <N>``.
Needs Open MPI (or another MPI whose ``mpicc``/``mpirun`` are on PATH), HDF5 and libyaml; needs
no real dataset.

Steps, each reported as ``MIMIC_RESULT:`` markers and gated:

1. The MPI control test ``tests/mpi/test_snapshot_collectives_mpi.c``: compiled with
   ``mpicc -DMPI`` against ``src/core/snapshot_collectives.c`` and the util sources only (the
   test file stubs the running-callback accessor as NONE; ``module_registry.c`` is not linked)
   and run at ``-np 3`` under a timeout; every case it declares (``TEST_RUN(``) must pass.
2. For each of ``halos-only``, ``sage16``, ``sham`` and ``hod`` on ``mini-millennium-horizontal``
   (the committed ``forest_blocks`` fixture, six forests over seven gapped snapshots, through
   ``simulations/mini-millennium-horizontal/_tests/input/forest_blocks_<model>.yaml``):
   build the non-MPI binary and run the fixture serially (for ``sham`` the serial ``SHAM audit``
   lines must each show ``assigned >= 1`` and ``masked >= 1``); build with ``USE-MPI=yes`` and run
   under ``$MPIRUN -np N`` for N in 1, 2, 3, 4, 8. At ``-np 1`` the output must use today's
   unsuffixed partition names (``<base>_<snap>.hdf5``, master groups ``File<snap>``) and the log
   must hold no partition line. At every other count the master must hold, for every requested
   snapshot, exactly ``File<snap>_task<t>`` for t in [0, N) whose ``TotHalosPerSnap`` sum to the
   serial master's, and the log must hold task 0's ``Distributed horizontal partition`` line
   naming N tasks. Every count's output must pass
   ``scripts/compare_cross_format_identity.py --compare-created`` against the serial output.
   Both binaries of a leg are production builds (``TEST_BUILD=no``).
3. The version 2 refusal: ``halos-only`` on the version 2 fixture
   ``simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/`` under ``-np 2`` must fail at
   startup with the format_version 2 message.

Every launch runs under a timeout, so a hang fails the gate instead of stalling it. Any FAIL,
ERROR or SKIP marker fails the run; the gate has no legitimate skip (a missing ``mpicc`` or
``mpirun`` is a failure). The whole output goes to ``build/distributed_tests.log``; run outputs go
to ``output/distributed-identity/gate/`` (replaced on each run).

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
CONTROL_SOURCES = [Path("src/core/snapshot_collectives.c")] + sorted(
    Path("src/util").glob("*.c"), key=str
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
SHAM_AUDIT_RE = re.compile(r"SHAM audit z=\S+ candidates=(\d+) assigned=(\d+) masked=(\d+)")
MARKER_RE = re.compile(r"^MIMIC_RESULT: (PASS|WARN|FAIL|ERROR|SKIP)\b.*$", re.MULTILINE)
MAKE_STATE = ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MIMIC_TEST_BUILD")
BUILD_TIMEOUT = 900
RUN_TIMEOUT = 300


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

        A timeout is status 124 with the partial output, so a hang fails its step.
        """
        try:
            completed = subprocess.run(
                cmd,
                cwd=REPO_ROOT,
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
            )
            status, output = completed.returncode, completed.stdout
        except subprocess.TimeoutExpired as expired:
            partial = expired.stdout or ""
            if isinstance(partial, bytes):
                partial = partial.decode(errors="replace")
            status, output = 124, partial + f"\n[timed out after {timeout} s]\n"
        except OSError as error:
            status, output = 127, f"[could not run {cmd[0]}: {error}]\n"
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
        if mpi:
            selectors.append("USE-MPI=yes")
        cmd = ["make", "--no-print-directory", *selectors, *targets]
        title = f"make {' '.join(selectors)} {' '.join(targets)}"
        status, _ = self.run(cmd, title, timeout=BUILD_TIMEOUT)
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
        cmd = ["mpicc", "-DMPI", "-Wall", "-Wextra", "-Wshadow"]
        cmd += [f"-I{path}" for path in CONTROL_INCLUDES]
        cmd += pkg_config("--cflags")
        cmd += [str(CONTROL_SOURCE), *map(str, CONTROL_SOURCES), "-lm", *pkg_config("--libs")]
        cmd += ["-o", str(CONTROL_BINARY)]
        status, _ = self.run(cmd, "compile the MPI control test", timeout=BUILD_TIMEOUT)
        if not self.marker(status == 0, "mpi_control_test_build", f"mpicc exited {status}"):
            return
        status, output = self.run(
            [*self.mpirun, "-np", str(CONTROL_TASKS), str(CONTROL_BINARY)],
            f"MPI control test at -np {CONTROL_TASKS}",
        )
        markers = [m.group(1) for m in MARKER_RE.finditer(output)]
        declared = (REPO_ROOT / CONTROL_SOURCE).read_text().count("TEST_RUN(")
        passed = markers.count("PASS")
        bad = len(markers) - passed
        self.marker(
            status == 0 and bad == 0 and passed == declared,
            "mpi_control_test",
            f"exit {status}, {passed}/{declared} cases passed, {bad} FAIL/ERROR/SKIP/WARN markers",
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
        serial_totals = master_totals(serial_dir / f"{base}.hdf5", snapshots)

        if not self.build(model, SIMULATION, mpi=True):
            return
        for ntask in NP_COUNTS:
            run_dir = WORK_ROOT / model / f"np{ntask}"
            run_file = write_run_file(config, run_dir)
            status, output = self.launch(ntask, run_file, f"{model} at -np {ntask}")
            name = f"{model}_np{ntask}"
            if not self.marker(status == 0, f"mpi_run_{name}", f"exit {status}"):
                continue
            if ntask == 1:
                self.check_serial_layout(name, run_dir, base, snapshots, output)
            else:
                self.check_task_layout(name, run_dir, base, snapshots, ntask, serial_totals, output)
            status, _ = self.run(
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
        expected = {f"{base}_{snap:03d}.hdf5" for snap in snapshots}
        found = {path.name for path in run_dir.glob(f"{base}_*.hdf5")}
        groups = master_groups(run_dir / f"{base}.hdf5", snapshots)
        want_groups = {snap: [f"File{snap:03d}"] for snap in snapshots}
        self.marker(
            found == expected and groups == want_groups,
            f"layout_{name}",
            f"partitions {sorted(found)}, master groups {groups}",
        )
        self.marker(
            PARTITION_LINE not in output, f"no_partition_log_{name}", "a partition line was logged"
        )

    def check_task_layout(self, name, run_dir, base, snapshots, ntask, serial_totals, output):
        """-np N > 1: N task groups per snapshot summing to the serial count, and the log line."""
        groups = master_groups(run_dir / f"{base}.hdf5", snapshots)
        want_groups = {
            snap: [f"File{snap:03d}_task{t:03d}" for t in range(ntask)] for snap in snapshots
        }
        totals = master_totals(run_dir / f"{base}.hdf5", snapshots)
        self.marker(
            groups == want_groups and totals == serial_totals,
            f"layout_{name}",
            f"master groups {groups}, TotHalosPerSnap sums {totals} vs serial {serial_totals}",
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
            status not in (0, 124) and V2_MESSAGE in output and PARTITION_LINE not in output,
            "version_2_refused_at_np2",
            f"exit {status}; expected a startup failure naming '{V2_MESSAGE}'",
        )

    def restore(self) -> None:
        """Regenerate the caller's generated code; a failure fails the run."""
        selectors = [
            f"{key}={os.environ[key]}" for key in ("MODEL", "SIMULATION") if key in os.environ
        ]
        cmd = ["make", "--no-print-directory", *selectors, "generate"]
        status, _ = self.run(cmd, "restore generated code", timeout=BUILD_TIMEOUT)
        caller = " ".join(selectors) or "the Makefile defaults"
        if status != 0:
            self.failures.append(f"could not restore generated code for {caller}")
            print(f"FAIL: could not restore generated code for {caller} (see {LOG_PATH})")
        else:
            print(f"Generated code restored for {caller}; rebuild the executable with 'make'.")


def pkg_config(flag: str) -> list[str]:
    """libyaml's compile or link flags, falling back to -lyaml without pkg-config."""
    try:
        out = subprocess.run(
            ["pkg-config", flag, "yaml-0.1"], capture_output=True, text=True, check=True
        ).stdout
        return shlex.split(out)
    except (OSError, subprocess.CalledProcessError):
        return ["-lyaml"] if flag == "--libs" else []


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


def master_groups(master: Path, snapshots: list[int]) -> dict[int, list[str]]:
    """The File* group names under each requested Snap group of a master file."""
    if not master.exists():
        return {}
    with h5py.File(master, "r") as handle:
        return {
            snap: sorted(key for key in handle.get(f"Snap{snap:03d}", {}) if key.startswith("File"))
            for snap in snapshots
        }


def master_totals(master: Path, snapshots: list[int]) -> dict[int, int]:
    """Each requested snapshot's TotHalosPerSnap summed over the master's File* groups."""
    if not master.exists():
        return {}
    totals = {}
    with h5py.File(master, "r") as handle:
        for snap in snapshots:
            group = handle.get(f"Snap{snap:03d}", {})
            totals[snap] = sum(
                int(group[key].attrs["TotHalosPerSnap"][0])
                for key in group
                if key.startswith("File")
            )
    return totals


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
    finally:
        gate.restore()

    if gate.failures:
        print(f"FAIL: tests-distributed ({len(gate.failures)} failure(s); see {LOG_PATH})")
        for failure in gate.failures:
            print(f"  {failure}")
        return 1
    print(
        f"PASS: tests-distributed ({gate.passes} checks: MPI control test, "
        f"{len(models)} model(s) at -np {', '.join(map(str, NP_COUNTS))}, version 2 refusal; "
        f"no skips; log: {LOG_PATH})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
