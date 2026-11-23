#!/usr/bin/env python3
"""
Run ``scripts/benchmark_mimic.sh`` over a matrix of configurations and tabulate the results.

The matrix is model x simulation x output mode x MPI rank count. Each point is one call of
``benchmark_mimic.sh``, which clean-builds Mimic in this checkout (``make clean``, then
``make``), times one run of the executable it built, and writes its JSON and run log, so that
script stays the one place a benchmark is measured. Runs are sequential, one at a time; nothing
else should build or test in this checkout while a matrix runs.

Two kinds of input are supported, and any other is refused before anything runs: L-Halo binary
trees under the vertical driver, and horizontal HDF5 (version 3) datasets under the horizontal
driver. For each point the wrapper writes a run file derived from the shipped
``models/<model>/input/<model>_<simulation>.yaml``, changing only:

* ``input.first_file`` and ``input.last_file`` (L-Halo binary only): the whole box (the
  simulation package's own range) by default, or the ``--files`` range;
* ``output.snapshot_list``: the last snapshot of the package's ``a_list`` (``final``) or every
  snapshot (``all``, an empty list);
* ``output.output_directory``: ``<scratch>/<run_id>``, which must not exist beforehand; the
  wrapper reads the run's output there and then removes that directory, and nothing else.

Everything one invocation measures lands in ``benchmarks/matrix_<label>_<stamp>/``: a
``summary.csv`` with one row per run, rewritten after every run so that an interruption loses
nothing already measured, and per run its run file, the wrapped script's JSON, run log and
console output, and the run's ``metadata/`` record. A run is ``ok`` only when the run succeeded
and every measurement below was read; otherwise its ``status`` says why, and the matrix goes on.

Where each summary column comes from (a quantity that cannot be read is an empty cell):

* ``benchmark_mimic.sh``'s JSON: ``timestamp``, ``host``, ``hw_model``, ``cpu_cores`` (logical),
  ``os_version``, ``architecture``, ``make_flags``, ``extra_cflags``, ``extra_ldflags``,
  ``runtime_flags``, ``ranks``, ``wall_time_s`` (the whole timed process, ``/usr/bin/time``),
  ``time_max_rss_mb`` and ``output_bytes`` (the per-file outputs, without the master file and
  ``metadata/``). ``time_max_rss_mb`` is ``/usr/bin/time``'s maximum resident set size
  (MB = 2^20 B) of what it launched: in a serial run, the Mimic process; under MPI, whatever
  its accounting of the launcher reaches, which is neither a given rank's peak nor the job's
  total. The job's total memory under MPI is not measured.
* The run log beside that JSON: ``mimic_peak_rss_rank0_gb``, the ``Peak process RSS`` of Mimic's
  run memory profile (rank 0 only, GB = 1e9 B). This is the figure to compare across runs.
* The run's output, read before it is removed: ``galaxies_written`` (records over every output
  snapshot), ``galaxies_final_snapshot`` and ``output_snapshots``.
* The input: ``halos_processed``, the sum of the ``totNHalos`` headers of the L-Halo binary files
  in the range, or of the ``n_halos`` header of every ``snapshot_NNN.h5`` of a horizontal
  dataset (each driver processes every halo of the input it reads).
* This host: ``cpu_model``, ``cpu_performance_cores`` (macOS), ``memory_installed_bytes``;
  ``mimic_commit`` and ``mimic_dirty``, sampled before the wrapper writes anything; and
  ``compiler``, the ``Compiler:`` of ``make info`` under the build's own make flags, with its
  ``--version``.
* The plan: ``label``, ``model``, ``simulation``, ``tree_type``, ``driver``, ``first_file``,
  ``last_file``, ``whole_box``, ``output_mode``, ``run_file_overrides``; and derived,
  ``halos_per_second`` = ``halos_processed`` / ``wall_time_s``.

Usage (from the repository root, with the venv's Python, which has PyYAML and h5py):
    mimic_venv/bin/python3 scripts/benchmark_matrix.py --label studio --dry-run
    mimic_venv/bin/python3 scripts/benchmark_matrix.py --label studio --ranks "1 4 8 16"
    mimic_venv/bin/python3 scripts/benchmark_matrix.py --label laptop --files 0:63

Exit status: 0 when every run succeeded, 1 when any run failed (the others still run and are
tabulated), 2 when the matrix is refused before anything runs.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import json
import os
import platform
import re
import shlex
import shutil
import socket
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCHMARK_SCRIPT = REPO_ROOT / "scripts" / "benchmark_mimic.sh"

DEFAULT_MODELS = ["halos-only", "sage16"]
DEFAULT_SIMULATIONS = ["millennium", "mini-uchuu"]
OUTPUT_MODES = ("final", "all")

# The two inputs the matrix supports: (tree_type, processing_order).
VERTICAL_LHALO = ("lhalo_binary", "vertical")
HORIZONTAL_HDF5 = ("horizontal_hdf5", "horizontal")

PEAK_RSS_PATTERN = re.compile(r"Peak process RSS: ([0-9.]+) GB")

SUMMARY_COLUMNS = [
    "label",
    "run_id",
    "status",
    "timestamp",
    "host",
    "hw_model",
    "cpu_model",
    "cpu_cores",
    "cpu_performance_cores",
    "memory_installed_bytes",
    "os_version",
    "architecture",
    "mimic_commit",
    "mimic_dirty",
    "compiler",
    "make_flags",
    "extra_cflags",
    "extra_ldflags",
    "runtime_flags",
    "model",
    "simulation",
    "tree_type",
    "driver",
    "first_file",
    "last_file",
    "whole_box",
    "output_mode",
    "output_snapshots",
    "ranks",
    "wall_time_s",
    "time_max_rss_mb",
    "mimic_peak_rss_rank0_gb",
    "halos_processed",
    "galaxies_written",
    "galaxies_final_snapshot",
    "output_bytes",
    "halos_per_second",
    "run_file_overrides",
    "run_dir",
]


class MatrixRefused(Exception):
    """The matrix cannot run as declared; raised before anything is built or run."""


# --------------------------------------------------------------------------------------------
# Run-file text handling: every scalar is kept as its written text, so nothing the wrapper does
# not set changes (``6.0e11`` stays ``6.0e11``). Comments are not kept; Mimic does not read them.
# --------------------------------------------------------------------------------------------


class _TextDumper(yaml.BaseDumper):
    """A dumper with no implicit typing, so plain text scalars are written back unquoted.

    Aliases are off: the loader shares equal key strings, which would otherwise be written as
    anchors (``&id001 name``) rather than as the text the shipped file holds.
    """

    def ignore_aliases(self, data: Any) -> bool:
        return True


_TextDumper.add_representer(
    str, lambda dumper, value: dumper.represent_scalar("tag:yaml.org,2002:str", value)
)
_TextDumper.add_representer(
    dict, lambda dumper, value: dumper.represent_mapping("tag:yaml.org,2002:map", value.items())
)
_TextDumper.add_representer(
    list, lambda dumper, value: dumper.represent_sequence("tag:yaml.org,2002:seq", value)
)


def load_text_yaml(path: Path) -> dict:
    """Read a YAML mapping with every scalar kept as its written text."""
    document = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    if not isinstance(document, dict):
        raise MatrixRefused(f"{rel(path)} is not a YAML mapping")
    return document


def dump_text_yaml(document: dict) -> str:
    """Write a document read by :func:`load_text_yaml`, keeping key order and scalar text."""
    return yaml.dump(
        document, Dumper=_TextDumper, sort_keys=False, default_flow_style=False, width=4096
    )


def rel(path: Path) -> str:
    """A path relative to the repository root when it lies inside it."""
    for candidate in (Path(path), Path(path).resolve()):
        try:
            return str(candidate.relative_to(REPO_ROOT))
        except ValueError:
            pass
    return str(path)


def repo_path(value: str) -> Path:
    """Resolve a run-file path the way Mimic does when run from the repository root."""
    path = Path(value)
    return path if path.is_absolute() else REPO_ROOT / path


def count_snapshots(a_list: Path) -> int:
    """Count an ``a_list``'s scale factors as Mimic's reader does: blank and ``#`` lines skipped."""
    lines = (line.strip() for line in a_list.read_text().splitlines())
    return sum(1 for line in lines if line and not line.startswith("#"))


# --------------------------------------------------------------------------------------------
# Planning
# --------------------------------------------------------------------------------------------


@dataclass
class PairPlan:
    """One model and simulation package pair, resolved from the shipped files."""

    model: str
    simulation: str
    run_file: Path
    template: dict
    tree_type: str
    driver: str
    first_file: int | None
    last_file: int | None
    whole_box: bool
    final_snapshot: int
    output_format: str
    output_filename: str
    input_files: list[Path] = field(default_factory=list)


@dataclass
class RunPlan:
    """One point of the matrix."""

    pair: PairPlan
    output_mode: str
    ranks: int

    @property
    def run_id(self) -> str:
        return f"{self.pair.model}_{self.pair.simulation}_{self.output_mode}_np{self.ranks}"


def parse_file_range(text: str | None) -> tuple[int, int] | None:
    """Parse ``FIRST:LAST`` into two non-negative integers with FIRST <= LAST."""
    if text is None:
        return None
    match = re.fullmatch(r"\s*(\d+)\s*:\s*(\d+)\s*", text)
    if not match or int(match.group(1)) > int(match.group(2)):
        raise MatrixRefused(f"--files must be FIRST:LAST with FIRST <= LAST, got {text!r}")
    return int(match.group(1)), int(match.group(2))


def split_words(values: list[str]) -> list[str]:
    """Flatten arguments given as separate words or as one quoted list, dropping repeats."""
    words = [word for value in values for word in re.split(r"[\s,]+", value.strip()) if word]
    return list(dict.fromkeys(words))


def plan_pair(
    model: str, simulation: str, file_range: tuple[int, int] | None, problems: list[str]
) -> PairPlan | None:
    """Resolve one pair from its shipped run file and simulation package, or record why not."""
    name = f"{model} x {simulation}"
    model_dir = REPO_ROOT / "models" / model
    simulation_dir = REPO_ROOT / "simulations" / simulation
    run_file = model_dir / "input" / f"{model}_{simulation}.yaml"
    if not model_dir.is_dir():
        problems.append(f"{name}: model package {rel(model_dir)}/ is not present")
        return None
    if not (simulation_dir / "simulation_info.yaml").is_file():
        problems.append(f"{name}: simulation package {rel(simulation_dir)}/ is not present")
        return None
    if not run_file.is_file():
        problems.append(f"{name}: no shipped run file {rel(run_file)}")
        return None

    template = load_text_yaml(run_file)
    if (template.get("simulation") or {}).get("config"):
        problems.append(
            f"{name}: {rel(run_file)} sets simulation.config, which replaces the package's own "
            f"inputs; the matrix does not support it"
        )
        return None
    package_input = dict(load_text_yaml(simulation_dir / "simulation_info.yaml").get("input") or {})
    # Mimic's precedence: the simulation package's input section, then the run file's.
    input_config = {**package_input, **(template.get("input") or {})}
    output_config = template.get("output") or {}
    kind = (
        input_config.get("tree_type", "lhalo_binary"),
        input_config.get("processing_order", "vertical"),
    )
    if kind not in (VERTICAL_LHALO, HORIZONTAL_HDF5):
        problems.append(
            f"{name}: tree_type {kind[0]!r} under the {kind[1]} driver is not supported "
            f"(only lhalo_binary, vertical, and horizontal_hdf5, horizontal)"
        )
        return None

    if kind == VERTICAL_LHALO:
        package_range = (int(package_input["first_file"]), int(package_input["last_file"]))
        first_file, last_file = file_range or package_range
        whole_box = (first_file, last_file) == package_range
    elif file_range is not None:
        problems.append(f"{name}: --files does not apply, a horizontal dataset is read whole")
        return None
    else:
        first_file = last_file = None
        whole_box = True

    a_list = repo_path(input_config.get("snapshot_list_file", ""))
    if not a_list.is_file():
        problems.append(f"{name}: snapshot list {rel(a_list)} is not present")
        return None
    snapshot_count = count_snapshots(a_list)

    pair = PairPlan(
        model=model,
        simulation=simulation,
        run_file=run_file,
        template=template,
        tree_type=kind[0],
        driver=kind[1],
        first_file=first_file,
        last_file=last_file,
        whole_box=whole_box,
        final_snapshot=snapshot_count - 1,
        output_format=output_config.get("output_format", "binary"),
        output_filename=output_config.get("output_filename", "model"),
    )

    data_dir = repo_path(input_config.get("simulation_dir", ""))
    if not data_dir.is_dir():
        problems.append(f"{name}: tree directory {rel(data_dir)} (the snapshots link) is absent")
        return pair
    # The vertical driver skips a missing tree file and still exits 0, so check them all here.
    tree_name = input_config["tree_name"]
    if kind == VERTICAL_LHALO:
        numbers = range(first_file, last_file + 1)
        pair.input_files = [data_dir / f"{tree_name}.{n}" for n in numbers]
    else:
        pair.input_files = [data_dir / (tree_name % n) for n in range(snapshot_count)]
    missing = [path for path in pair.input_files if not path.is_file()]
    if missing:
        more = f" and {len(missing) - 8} more" if len(missing) > 8 else ""
        problems.append(
            f"{name}: {len(missing)} of {len(pair.input_files)} input files missing from "
            f"{rel(data_dir)}: {', '.join(path.name for path in missing[:8])}{more}"
        )
    return pair


def derive_run_file(run: RunPlan, output_directory: Path) -> tuple[dict, str]:
    """The run file for one point, and a record of exactly what differs from the shipped one."""
    pair = run.pair
    document = json.loads(json.dumps(pair.template))  # deep copy of nested dicts and lists
    overrides = []
    if pair.first_file is not None:
        document.setdefault("input", {})
        document["input"]["first_file"] = str(pair.first_file)
        document["input"]["last_file"] = str(pair.last_file)
        overrides.append(f"input.first_file={pair.first_file}")
        overrides.append(f"input.last_file={pair.last_file}")
    snapshot_list = [str(pair.final_snapshot)] if run.output_mode == "final" else []
    document.setdefault("output", {})
    document["output"]["snapshot_list"] = snapshot_list
    document["output"]["output_directory"] = str(output_directory)
    overrides.append(f"output.snapshot_list=[{', '.join(snapshot_list)}]")
    overrides.append(f"output.output_directory={output_directory}")
    return document, "; ".join(overrides)


def make_flags_for(ranks: int) -> str:
    """The MAKE_FLAGS a point builds with: the caller's own, plus USE-MPI=yes under MPI."""
    make_flags = os.environ.get("MAKE_FLAGS", "").strip()
    return f"{make_flags} USE-MPI=yes".strip() if ranks > 1 else make_flags


def run_environment(run: RunPlan, mpirun: str, results_dir: Path) -> dict[str, str]:
    """The environment variables of ``benchmark_mimic.sh`` that this point sets."""
    return {
        "BENCHMARK_DIR": str(results_dir),
        "MAKE_FLAGS": make_flags_for(run.ranks),
        "MPI_RUN_COMMAND": f"{mpirun} -np {run.ranks}" if run.ranks > 1 else "",
    }


# --------------------------------------------------------------------------------------------
# Measurement readers
# --------------------------------------------------------------------------------------------


def count_input_halos(pair: PairPlan) -> int:
    """Sum the halo counts the input files record in their own headers."""
    total = 0
    if pair.tree_type == "lhalo_binary":
        for path in pair.input_files:
            with open(path, "rb") as handle:
                _, halos = struct.unpack("=ii", handle.read(8))  # Ntrees, totNHalos
            total += halos
    else:
        import h5py

        for path in pair.input_files:
            with h5py.File(path, "r") as handle:
                total += int(handle["header"].attrs["n_halos"])
    return total


def count_output_galaxies(output_dir: Path, pair: PairPlan) -> tuple[int, int, int]:
    """Count the galaxy records a run wrote.

    Returns:
        (records over every output snapshot, records at the latest output snapshot,
        number of output snapshots), read from the per-file outputs themselves.
    """
    per_snapshot: dict[float, int] = {}
    if pair.output_format == "hdf5":
        import h5py

        for path in sorted(output_dir.glob(f"{pair.output_filename}_*.hdf5")):
            with h5py.File(path, "r") as handle:
                for name in handle:
                    if name.startswith("Snap") and "Galaxies" in handle[name]:
                        snapshot = int(name[4:])
                        count = handle[name]["Galaxies"].shape[0]
                        per_snapshot[snapshot] = per_snapshot.get(snapshot, 0) + count
        latest = max(per_snapshot) if per_snapshot else None
    else:
        pattern = re.compile(rf"{re.escape(pair.output_filename)}_z([0-9.]+)_\d+$")
        for path in sorted(output_dir.iterdir()):
            match = pattern.match(path.name)
            if match:
                with open(path, "rb") as handle:
                    _, galaxies = struct.unpack("=ii", handle.read(8))  # ntrees, ngalaxies
                redshift = float(match.group(1))
                per_snapshot[redshift] = per_snapshot.get(redshift, 0) + galaxies
        latest = min(per_snapshot) if per_snapshot else None  # lowest redshift
    if latest is None:
        raise ValueError(f"no galaxy output found in {output_dir}")
    return sum(per_snapshot.values()), per_snapshot[latest], len(per_snapshot)


def command_output(command: list[str]) -> str:
    """The stripped stdout of a command, or '' when it cannot be run."""
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def host_facts() -> dict[str, str]:
    """Hardware facts the wrapped script's JSON does not carry, and the checkout's Git state."""
    facts = {"cpu_model": "", "cpu_performance_cores": "", "memory_installed_bytes": ""}
    if platform.system() == "Darwin":
        facts["cpu_model"] = command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
        facts["cpu_performance_cores"] = command_output(
            ["sysctl", "-n", "hw.perflevel0.physicalcpu"]
        )
        facts["memory_installed_bytes"] = command_output(["sysctl", "-n", "hw.memsize"])
    elif platform.system() == "Linux":
        try:
            cpuinfo = Path("/proc/cpuinfo").read_text()
            match = re.search(r"^model name\s*:\s*(.+)$", cpuinfo, re.MULTILINE)
            facts["cpu_model"] = match.group(1).strip() if match else ""
            meminfo = Path("/proc/meminfo").read_text()
            match = re.search(r"^MemTotal:\s*(\d+) kB", meminfo, re.MULTILINE)
            facts["memory_installed_bytes"] = str(int(match.group(1)) * 1024) if match else ""
        except OSError:
            pass
    git = ["git", "-C", str(REPO_ROOT)]
    facts["mimic_commit"] = command_output(git + ["rev-parse", "--short", "HEAD"])
    facts["mimic_dirty"] = "true" if command_output(git + ["status", "--porcelain"]) else "false"
    return facts


def compiler_identity(ranks: int) -> str:
    """The compiler ``make`` selects under this point's make flags, with its version line."""
    command = ["make", "-s", "-C", str(REPO_ROOT), "info"] + shlex.split(make_flags_for(ranks))
    match = re.search(r"^Compiler:\s*(\S+)", command_output(command), re.MULTILINE)
    if not match:
        return ""
    version = command_output([match.group(1), "--version"]).splitlines()
    return f"{match.group(1)}: {version[0]}" if version else match.group(1)


# --------------------------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------------------------


def read_measurements(run: RunPlan, row: dict, run_dir: Path, output_dir: Path, cache: dict):
    """Fill a run's row from its receipt, log, output and input; raise on anything missing."""
    pair = run.pair
    results = sorted(run_dir.glob("baseline_*.json"))
    if len(results) != 1:
        raise ValueError(f"{len(results)} result JSON files in {rel(run_dir)}, expected 1")
    result = json.loads(results[0].read_text())
    system, configuration = result["system"], result["configuration"]
    performance = result["overall_performance"]
    row.update(
        timestamp=result["timestamp"],
        host=system["hostname"],
        hw_model=system["hw_model"],
        cpu_cores=system["cpu_count"],
        os_version=system["os_version"],
        architecture=system["architecture"],
        make_flags=configuration["build_flags"],
        extra_cflags=configuration["extra_cflags"],
        extra_ldflags=configuration["extra_ldflags"],
        runtime_flags=configuration["runtime_flags"],
        ranks=result["test_case"]["num_processes"],
        wall_time_s=performance["wall_time_seconds"],
        time_max_rss_mb=performance["max_memory_mb"],
        output_bytes=performance["total_output_size_bytes"],
    )
    match = PEAK_RSS_PATTERN.search(Path(performance["run_log"]).read_text(errors="replace"))
    if not match:
        raise ValueError("no 'Peak process RSS' line in the run log")
    row["mimic_peak_rss_rank0_gb"] = match.group(1)

    written, final, snapshots = count_output_galaxies(output_dir, pair)
    row.update(galaxies_written=written, galaxies_final_snapshot=final, output_snapshots=snapshots)
    if (output_dir / "metadata").is_dir():
        shutil.copytree(output_dir / "metadata", run_dir / "metadata")

    key = (pair.simulation, pair.first_file, pair.last_file)
    if key not in cache:
        cache[key] = count_input_halos(pair)
    row["halos_processed"] = cache[key]
    row["halos_per_second"] = f"{cache[key] / float(row['wall_time_s']):.1f}"


def measure_run(
    run: RunPlan, args: argparse.Namespace, paths: dict[str, Path], facts: dict, cache: dict
) -> dict[str, Any]:
    """Run one point through ``benchmark_mimic.sh`` and return its summary row, ok or failed."""
    pair = run.pair
    run_dir = paths["results"] / "runs" / run.run_id
    run_dir.mkdir(parents=True)
    output_dir = paths["scratch"] / run.run_id
    document, overrides = derive_run_file(run, output_dir)
    run_file = run_dir / "run.yaml"
    run_file.write_text(dump_text_yaml(document))

    row: dict[str, Any] = {column: "" for column in SUMMARY_COLUMNS}
    row.update(
        label=args.label,
        run_id=run.run_id,
        cpu_model=facts["cpu_model"],
        cpu_performance_cores=facts["cpu_performance_cores"],
        memory_installed_bytes=facts["memory_installed_bytes"],
        mimic_commit=facts["mimic_commit"],
        mimic_dirty=facts["mimic_dirty"],
        compiler=facts["compiler_mpi" if run.ranks > 1 else "compiler_serial"],
        model=pair.model,
        simulation=pair.simulation,
        tree_type=pair.tree_type,
        driver=pair.driver,
        first_file="" if pair.first_file is None else pair.first_file,
        last_file="" if pair.last_file is None else pair.last_file,
        whole_box=str(pair.whole_box).lower(),
        output_mode=run.output_mode,
        ranks=run.ranks,
        run_file_overrides=overrides,
        run_dir=rel(run_dir),
    )
    print(f"  overrides: {overrides}", flush=True)
    env = {**os.environ, **run_environment(run, args.mpirun, run_dir)}
    try:
        with open(run_dir / "benchmark.out", "w") as console:
            status = subprocess.run(
                [str(BENCHMARK_SCRIPT), "--param-file", str(run_file)],
                cwd=REPO_ROOT,
                env=env,
                stdout=console,
                stderr=console,
            ).returncode
        if status != 0:
            row["status"] = f"failed: benchmark_mimic.sh exit {status}"
        else:
            read_measurements(run, row, run_dir, output_dir, cache)
            row["status"] = "ok"
    except Exception as error:  # a failed row with its reason, and the matrix goes on
        row["status"] = f"failed: {type(error).__name__}: {error}"
    finally:
        # The run's output is this wrapper's own scratch: measured above, then removed.
        if output_dir.is_dir():
            shutil.rmtree(output_dir)
    return row


def write_summary(path: Path, rows: list[dict]) -> None:
    """Rewrite the whole summary table, replacing the previous file in one step."""
    partial = path.with_suffix(".csv.partial")
    with open(partial, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    os.replace(partial, path)


def describe(runs: list[RunPlan], args, paths: dict[str, Path]) -> None:
    """Print the matrix and, per point, the overrides and the command that would run."""
    print(f"Benchmark matrix '{args.label}': {len(runs)} runs, sequential, one at a time.")
    print(f"Results:  {rel(paths['results'])}/")
    print(f"Scratch:  {paths['scratch']}/ (each run's output, removed once measured)")
    print(f"Each run clean-builds Mimic in {REPO_ROOT}; build or test nothing else there.")
    print()
    print(f"{'#':>3}  {'model':<12} {'simulation':<28} {'files':<9} {'output':<6} ranks")
    for index, run in enumerate(runs, 1):
        pair = run.pair
        files = "dataset" if pair.first_file is None else f"{pair.first_file}:{pair.last_file}"
        print(
            f"{index:>3}  {pair.model:<12} {pair.simulation:<28} {files:<9} "
            f"{run.output_mode:<6} {run.ranks}"
        )
    print()
    for index, run in enumerate(runs, 1):
        run_dir = paths["results"] / "runs" / run.run_id
        _, overrides = derive_run_file(run, paths["scratch"] / run.run_id)
        env = run_environment(run, args.mpirun, run_dir)
        env_text = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
        print(f"[{index}] {run.run_id} from {rel(run.pair.run_file)}")
        print(f"    overrides: {overrides}")
        print(
            f"    command: {env_text} {rel(BENCHMARK_SCRIPT)} --param-file "
            f"{shlex.quote(str(run_dir / 'run.yaml'))}"
        )


def build_matrix(args: argparse.Namespace, scratch: Path) -> list[RunPlan]:
    """Resolve and check every point; raise :class:`MatrixRefused` listing every problem."""
    problems: list[str] = []
    outputs = split_words(args.outputs)
    for mode in outputs:
        if mode not in OUTPUT_MODES:
            problems.append(f"unknown output mode {mode!r} (choose from {', '.join(OUTPUT_MODES)})")
    ranks: list[int] = []
    for word in split_words(args.ranks):
        if not word.isdigit() or int(word) < 1:
            problems.append(f"rank count {word!r} is not a positive integer")
        else:
            ranks.append(int(word))
    if os.environ.get("MIMIC_EXECUTABLE"):
        problems.append("MIMIC_EXECUTABLE is set; the matrix times the executable it builds")
    if "USE-MPI" in os.environ.get("MAKE_FLAGS", ""):
        problems.append("MAKE_FLAGS sets USE-MPI; the wrapper sets it from the rank count")
    if os.environ.get("MAKEFLAGS"):
        problems.append("MAKEFLAGS is set; unset it so each build is the one the wrapper asks for")
    if any(rank > 1 for rank in ranks) and shutil.which(args.mpirun.split()[0]) is None:
        problems.append(f"rank counts above 1 need {args.mpirun.split()[0]!r} on PATH")
    try:
        file_range = parse_file_range(args.files)
    except MatrixRefused as error:
        problems.append(str(error))
        file_range = None

    pairs = []
    for model in split_words(args.models):
        for simulation in split_words(args.simulations):
            pair = plan_pair(model, simulation, file_range, problems)
            if pair is not None:
                pairs.append(pair)
    try:
        import h5py  # noqa: F401
    except ImportError:
        problems.append("h5py is needed to read HDF5 files; run with mimic_venv/bin/python3")
    runs = [RunPlan(pair, mode, rank) for pair in pairs for mode in outputs for rank in ranks]
    for run in runs:
        if (scratch / run.run_id).exists():
            problems.append(f"scratch run directory {scratch / run.run_id} already exists")
    if problems:
        raise MatrixRefused("\n".join(f"  - {problem}" for problem in problems))
    return runs


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run scripts/benchmark_mimic.sh over model x simulation x output mode x rank count, "
            "sequentially, one run at a time, and write one summary table per invocation."
        ),
        epilog=(
            "Lists may be given as separate words or as one quoted string. Rank count 1 is a "
            "serial build run without mpirun; a count N above 1 builds with USE-MPI=yes and runs "
            "under '<mpirun> -np N'. MAKE_FLAGS (without USE-MPI), EXTRA_CFLAGS, EXTRA_LDFLAGS "
            "and MIMIC_FLAGS pass through to benchmark_mimic.sh. Each run clean-builds Mimic in "
            "this checkout, so build or test nothing else there while the matrix runs."
        ),
    )
    parser.add_argument(
        "--label",
        default=socket.gethostname().split(".")[0],
        help="name of the machine or run set, recorded in every row "
        "(default: the short host name)",
    )
    parser.add_argument(
        "--models",
        nargs="+",
        metavar="MODEL",
        default=DEFAULT_MODELS,
        help=f"model packages (default: {' '.join(DEFAULT_MODELS)})",
    )
    parser.add_argument(
        "--simulations",
        nargs="+",
        metavar="SIMULATION",
        default=DEFAULT_SIMULATIONS,
        help=f"simulation packages, L-Halo binary or horizontal HDF5 "
        f"(default: {' '.join(DEFAULT_SIMULATIONS)})",
    )
    parser.add_argument(
        "--outputs",
        nargs="+",
        metavar="MODE",
        default=list(OUTPUT_MODES),
        help="output modes: 'final' writes the last snapshot only, 'all' every "
        "snapshot (default: final all)",
    )
    parser.add_argument(
        "--ranks",
        nargs="+",
        metavar="N",
        default=["1"],
        help='MPI rank counts, e.g. --ranks "1 4 8 16" (default: 1)',
    )
    parser.add_argument(
        "--files",
        metavar="FIRST:LAST",
        help="L-Halo tree-file range for a machine holding part of a simulation; "
        "recorded in every row (default: the whole box)",
    )
    parser.add_argument(
        "--mpirun",
        metavar="COMMAND",
        default="mpirun",
        help="MPI launcher; '-np N' is appended (default: %(default)s)",
    )
    parser.add_argument(
        "--scratch",
        type=Path,
        metavar="DIR",
        help="where run output is written and removed once measured; needs room for one "
        "run's output (default: output/benchmark-matrix-<stamp>/)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="check and print the matrix and the commands; build and run nothing",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.label = re.sub(r"[^A-Za-z0-9._-]+", "-", args.label).strip("-") or "matrix"
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    paths = {
        "results": REPO_ROOT / "benchmarks" / f"matrix_{args.label}_{stamp}",
        "scratch": (args.scratch or REPO_ROOT / "output" / f"benchmark-matrix-{stamp}").resolve(),
    }
    try:
        runs = build_matrix(args, paths["scratch"])
    except MatrixRefused as refusal:
        print(f"Refused before anything ran:\n{refusal}", file=sys.stderr)
        return 2

    describe(runs, args, paths)
    if args.dry_run:
        return 0

    facts = host_facts()  # Git state first, before this wrapper writes anything
    facts["compiler_serial"] = compiler_identity(1)
    facts["compiler_mpi"] = compiler_identity(2) if any(r.ranks > 1 for r in runs) else ""
    created_scratch = not paths["scratch"].exists()
    paths["results"].mkdir(parents=True)
    paths["scratch"].mkdir(parents=True, exist_ok=True)
    summary = paths["results"] / "summary.csv"
    rows: list[dict] = []
    write_summary(summary, rows)

    halos_cache: dict = {}
    try:
        for index, run in enumerate(runs, 1):
            started = f"{datetime.datetime.now():%H:%M:%S}"
            print(f"\n[{index}/{len(runs)}] {run.run_id} (started {started})")
            row = measure_run(run, args, paths, facts, halos_cache)
            rows.append(row)
            write_summary(summary, rows)
            print(
                f"  {row['status']}: wall {row['wall_time_s'] or '-'} s, galaxies "
                f"{row['galaxies_written'] or '-'}, halos/s {row['halos_per_second'] or '-'}"
            )
            if row["status"] != "ok":
                print(f"  see {rel(paths['results'] / 'runs' / run.run_id / 'benchmark.out')}")
    except KeyboardInterrupt:
        print(f"\nInterrupted: {rel(summary)} holds the {len(rows)} runs finished before it")
        return 130

    if created_scratch:
        try:
            paths["scratch"].rmdir()  # created by this invocation and emptied run by run
        except OSError:
            print(f"Scratch directory {paths['scratch']} is not empty and was left in place")
    failures = sum(1 for row in rows if row["status"] != "ok")
    print(f"\nSummary: {rel(summary)} ({len(rows) - failures} ok, {failures} failed)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
