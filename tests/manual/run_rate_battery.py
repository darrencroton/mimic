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
another MPI whose ``mpicc``/``mpirun`` are on PATH), HDF5 and libyaml. The integrator cost driver
is not part of the battery: ``tests/manual/run_rate_cost.py`` runs it.

Every leg runs ``halos-only`` with the four rate fixtures (``test_rate_supply``,
``test_rate_formation``, ``test_rate_feedback``, ``test_rate_stripping``) in one rate phase and
``test_rate_jump`` in a discrete phase after it, test builds only, with the modules and
parameters of ``simulations/mini-millennium-horizontal/_tests/input/forest_blocks_rate.yaml``.
The legs:

``cross-driver``
    The real mini-Millennium data in both layouts (``mini-millennium`` through the vertical
    driver, ``mini-millennium-horizontal`` through the horizontal one; the committed
    ``halos-only`` run files with the rate modules added), under the explicit scheme with fixed
    and with dynamic time-stepping, under the implicit scheme with fixed time-stepping and under
    the explicit scheme with fixed time-stepping and the fixtures' rate coefficients scaled by
    1e3 (the stiff case): four pairs of runs. Each pair must give per-``UniqueGalaxyID`` byte
    identity of every output field through ``scripts/compare_cross_format_identity.py`` (tree
    rows only: a created record's id differs between drivers), identical step statistics (every
    attribute and dataset of the master file's ``RunProperties/RateIntegration``), and, for every
    FoF group (an output snapshot and a ``UniqueCentralGalaxyID``), the same sequence of tree
    rows in the output under both drivers. The stiff pair must also have rejected steps and a
    largest K_max above the explicit scheme's positivity cap in both runs, read from their
    statistics, so the cap and the retry rule ran through dispatch. Then the horizontal driver,
    explicit scheme, fixed time-stepping, runs the rate modules in two more orders (the reverse of
    the configured order and one other): each must equal the configured order's run in every
    ``Galaxies`` field byte for byte, in row order, in step statistics and in every
    ``RunProperties`` dataset and attribute but ``EnabledModules``, ``RunEndTime`` and the
    ``Version`` group's build attributes, which record the configured order and the run. The
    fixture's rows are too few for a missing canonical order to show (a rounding difference
    almost never surfaces in 9 to 13 rows a snapshot); the 204,974 real rows show it. Needs the
    real data under ``simulations/*/snapshots`` and fails, never skips, without it: the legs
    that need none (``--offline``) leave this one out.
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
    order's byte for byte, field by field and in row order, with identical step statistics and
    identical ``RunProperties`` datasets and attributes except ``EnabledModules``, ``RunEndTime``
    and the ``Version`` group's build attributes, which record the configured order and the run;
    the leg asserts that ``EnabledModules`` does record each order, so the runs differ in the
    configuration only. On 9 to 13 rows a snapshot this leg cannot see a missing canonical order
    (the cross-driver leg's reordered real-data runs can); it proves the fixture's laws and the
    comparison's mechanics.
``record-creation``
    The same fixture with ``test_fixture`` creating records in ``pre_timestep``, ahead of the
    rate phase: a serial run must complete with created rows, and ``-np 1, 2, 3, 4, 8`` must
    match it through the comparator with ``--compare-created`` (both sides must report created
    rows), in row order and in step statistics. The fixture gives every created record
    ``RateFixtureHot`` and ``RateFixtureCold`` of a fifth of the star-formation threshold
    (``TestFixtureCreatedContent``, a declared change to ``rate_fixture_mass``; small enough that
    no star forms and ``test_rate_jump`` does not merge the rows away), the serial reference
    must hold created rows with non-zero fixture state, and the comparison covers them, so the
    leg proves the rate pass orders and writes back created rows with content identically across
    task counts. One driver only: a created record's ``UniqueGalaxyID`` differs between drivers.

Every leg first asserts that the seven fixture properties (``RateFixture*``) are finite,
non-negative and non-zero in some rows of its reference output, so no comparison is made over
zeros. Each leg reports its compared cases against the count its design implies (the cross-driver
leg's four pairs and two reordered runs make six). The battery builds
its own test-build pairs (``halos-only`` on ``mini-millennium``, then on
``mini-millennium-horizontal`` without and with ``USE-MPI=yes``), and any FAIL, ERROR, SKIP or NA
marker in a run's output fails it; it has no legitimate skip. Every build and launch runs in its
own process group under a timeout, so a hang fails the battery instead of stalling it. The whole
output (every command's, every marker and every line the battery prints) goes to
``build/rate_tests.log``; run outputs go to ``output/rate-battery/`` (replaced on
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
import sys
from pathlib import Path
from typing import Iterator, NamedTuple

import h5py
import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import check_no_memory_leaks  # noqa: E402
from framework.battery import (  # noqa: E402
    CHUNK_COUNTS,
    CHUNKED_PARTITION_LINE,
    NP_CHUNK_PAIRS,
    NP_COUNTS,
    PARTITION_LINE,
    SIMULATION,
    TIMEOUT_STATUS,
    Battery,
    LegStopped,
    chunk_log_phrase,
    expected_layout,
    load_run_file,
    partition_files,
    read_master,
    with_forest_chunks,
    write_run_file,
)
from framework.fs import remove_tree  # noqa: E402
from framework.rate_fixture import (  # noqa: E402
    POSITIVITY_CAP,
    STATISTICS,
    STIFF_FACTOR,
    load_rate_fixture,
    read_step_extremes,
    scale_coefficients,
)

MODEL = "halos-only"
VERTICAL_SIMULATION = "mini-millennium"
HORIZONTAL_SIMULATION = SIMULATION  # the package holding the forest_blocks fixture
RATE_RUN_FILE = "forest_blocks_rate.yaml"
WORK_ROOT = REPO_ROOT / "output" / "rate-battery"
LOG_PATH = REPO_ROOT / "build" / "rate_tests.log"
RATE_PHASE = "rates"
JUMP_MODULE = "test_rate_jump"
RATE_MODULES, RATE_PARAMS = load_rate_fixture()
CREATOR = "test_fixture"
# RateFixtureHot and RateFixtureCold of every created record: a fifth of the star-formation
# threshold, so the two reservoirs together stay below it, no star forms on a created row, and
# test_rate_jump (which merges satellites holding stars) leaves the created rows in the output.
CREATED_CONTENT = 0.2 * RATE_PARAMS["TestRateFormationThreshold"]
CREATOR_PARAMS = {
    "TestFixtureDummyParameter": 0.25,
    "TestFixtureEnableLogging": 0,
    "TestFixtureCreateRecords": 1,
    "TestFixtureCreatedContent": CREATED_CONTENT,
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
ENABLED_MODULES = "RunProperties/EnabledModules"

LEGS = ("cross-driver", "task-chunk", "permutation", "record-creation")
OFFLINE_LEGS = ("task-chunk", "permutation", "record-creation")
SCHEMES = ("explicit", "implicit")
ORDERS = tuple(itertools.permutations(RATE_MODULES))
# The real-data orders: the configured one, its reverse (every pair of modules swapped) and a
# third that swaps three of the six pairs (supply with formation, supply with stripping,
# feedback with stripping) and keeps the other three.
REAL_ORDERS = (
    RATE_MODULES,
    RATE_MODULES[::-1],
    (RATE_MODULES[1], RATE_MODULES[3], RATE_MODULES[0], RATE_MODULES[2]),
)
# The RunProperties entries (read_run_properties keys) two runs of different module order may
# differ in: the configured order, the end time of the run and the Version group's attributes.
RUN_PROPERTIES_EXCLUDED = (
    "RunProperties/EnabledModules",
    "RunProperties@RunEndTime",
    "RunProperties/Version@",
)


class CrossCase(NamedTuple):
    """One pair of runs of the cross-driver leg."""

    scheme: str
    timestep: str
    scale: float = 1.0  # the factor on the fixtures' rate coefficients; STIFF_FACTOR is stiff

    @property
    def name(self) -> str:
        return f"{self.scheme}_{self.timestep}" + ("_stiff" if self.scale != 1.0 else "")


CROSS_CASES = (
    CrossCase("explicit", "fixed"),
    CrossCase("explicit", "dynamic"),
    CrossCase("implicit", "fixed"),
    CrossCase("explicit", "fixed", STIFF_FACTOR),
)
# The compared cases each leg must report, from the legs' own designs: a leg that compared fewer
# (a run that failed and was skipped, a case dropped from a loop) fails on the count as well.
EXPECTED_CASES = {
    "cross-driver": len(CROSS_CASES) + len(REAL_ORDERS) - 1,
    "task-chunk": len(NP_COUNTS) + len(CHUNK_COUNTS) + len(NP_CHUNK_PAIRS),
    "permutation": len(SCHEMES) * (len(ORDERS) - 1),
    "record-creation": len(NP_COUNTS),
}
REAL_RUN_FILES = {
    "vertical": Path("models") / MODEL / "input" / f"{MODEL}_{VERTICAL_SIMULATION}.yaml",
    "horizontal": Path("models") / MODEL / "input" / f"{MODEL}_{HORIZONTAL_SIMULATION}.yaml",
}

BAD_MARKER_RE = re.compile(r"^MIMIC_RESULT: (FAIL|ERROR|SKIP|NA)\b.*$", re.MULTILINE)
RUN_TIMEOUT = 120  # fixture runs take well under a second; a hang must cost little
REAL_RUN_TIMEOUT = 1800  # a real-data run takes seconds; a hang is bounded, not waited out


class Statistics(NamedTuple):
    """What the battery reads from a master file's ``RunProperties/RateIntegration`` group."""

    entries: dict[str, bytes]  # every attribute ("@name") and dataset, as raw bytes
    segments: int  # StepTotals.segments
    scheme: str  # the recorded scheme attribute


class RateBattery(Battery):
    """Runs the legs in build order, appending every command's output to one log."""

    default_timeout = RUN_TIMEOUT

    def __init__(self, mpirun: list[str], legs: tuple[str, ...]):
        super().__init__(LOG_PATH, mpirun)
        self.legs = legs
        self.cases = {leg: 0 for leg in LEGS}
        self.real_outputs: dict[tuple[str, str], Path] = {}
        self.references: dict[str, Path] = {}

    # ------------------------------------------------------------------ plumbing

    def run(self, cmd: list[str], title: str, timeout: int | None = None, **kwargs):
        """Run one command (``Battery.run``); a FAIL, ERROR, SKIP or NA marker in its output
        fails the battery."""
        status, output = super().run(cmd, title, timeout, **kwargs)
        for bad in BAD_MARKER_RE.findall(output):
            self.marker(False, "no_bad_markers", f"{title} reported a {bad} marker")
        return status, output

    def build_pair(self, simulation: str, mpi: bool) -> bool:
        """Build one test-build binary of ``halos-only`` on a simulation package."""
        return self.build(MODEL, simulation, mpi, True, simulation)

    @contextlib.contextmanager
    def case(self, leg: str) -> Iterator[None]:
        """A compared case: it counts for its leg when none of its markers failed."""
        before = len(self.failures)
        yield
        if len(self.failures) == before:
            self.cases[leg] += 1

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

    def check_layout(self, name, run_dir, config, serial_totals, output, ntask, nchunk) -> None:
        """-np 1 keeps the unsuffixed partition names and logs no partition line; more tasks
        write one partition per task and snapshot with the serial run's galaxy counts and
        resolving master links, and task 0 logs a partition headline naming the tasks (and the
        chunks, for a chunked run)."""
        base = config["output"]["output_filename"]
        snapshots = sorted(int(snap) for snap in config["output"]["snapshot_list"])
        want_files, want_groups = expected_layout(base, snapshots, ntask)
        found = partition_files(run_dir, base)
        master = read_master(run_dir / f"{base}.hdf5", snapshots)
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
                PARTITION_LINE not in output,
                f"partition_log_{name}",
                "a partition line was logged",
            )
            return
        headline = CHUNKED_PARTITION_LINE
        if ntask > 1:
            headline = f"task 0: {PARTITION_LINE}"
        phrase = f"over {ntask} tasks"
        if nchunk is not None:
            phrase = chunk_log_phrase(ntask, nchunk)
        line = next((text for text in output.splitlines() if headline in text), "")
        self.marker(
            phrase in line,
            f"partition_log_{name}",
            f"task 0's partition headline {headline!r} with {phrase!r} is missing (got {line!r})",
        )

    # ------------------------------------------------------------------ cross-driver leg

    def real_runs(self, layout: str) -> None:
        """The cross-driver cases on one layout of the real mini-Millennium data."""
        self.say(f"--- cross-driver runs: {layout} mini-Millennium")
        base = real_config(layout)
        for case in CROSS_CASES:
            run_dir = WORK_ROOT / "cross" / case.name / layout
            config = rate_config(base, scheme=case.scheme, timestep=case.timestep, scale=case.scale)
            status, output = self.launch(
                None,
                write_run_file(config, run_dir),
                f"{layout} {case.name}",
                REAL_RUN_TIMEOUT,
            )
            if self.run_marker(status, output, f"run_{layout}_{case.name}"):
                self.real_outputs[(layout, case.name)] = run_dir

    def cross_driver_compare(self) -> None:
        """Per case: the fixture assertion, the comparator, the FoF groups' row order and the
        step statistics, vertical against horizontal."""
        self.say("--- cross-driver comparison")
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
        if case.scale != 1.0:
            for layout, run_dir in (("vertical", vertical), ("horizontal", horizontal)):
                rejected, k_max = read_step_extremes(run_dir / f"{base}.hdf5")
                self.marker(
                    rejected > 0 and k_max > POSITIVITY_CAP,
                    f"stiff_nonvacuous_{layout}_{name}",
                    f"{rejected} rejected steps, largest K_max {k_max:.4g}; the case needs "
                    f"rejections and a K_max above the positivity cap {POSITIVITY_CAP}",
                )

    def real_permutations(self) -> None:
        """The horizontal real-data run in the REAL_ORDERS other than the configured one, each
        compared with the configured order's run (the explicit fixed case's)."""
        self.say("--- cross-driver: module orders on the real data")
        reference = self.real_outputs.get(("horizontal", CROSS_CASES[0].name))
        base = real_config("horizontal")
        for index, order in enumerate(REAL_ORDERS[1:], start=1):
            name = f"real_order{index}"
            run_dir = WORK_ROOT / "cross" / f"order{index}" / "horizontal"
            config = rate_config(base, scheme="explicit", order=order)
            status, output = self.launch(
                None,
                write_run_file(config, run_dir),
                f"horizontal {' '.join(order)}",
                REAL_RUN_TIMEOUT,
            )
            if not self.run_marker(status, output, f"run_{name}"):
                continue
            if reference is None:
                self.marker(False, f"permuted_{name}", "the configured order's run failed")
                continue
            with self.case("cross-driver"):
                self.check_permuted(name, reference, run_dir, "halos", order, "explicit")

    # ------------------------------------------------------------------ task-chunk and creation

    def serial_reference(self, key: str, config: dict) -> None:
        """The non-MPI serial run every other run of a key is compared with, and its fixture
        assertion."""
        run_dir = WORK_ROOT / key / "serial"
        status, output = self.launch(None, write_run_file(config, run_dir), f"{key} serial")
        if not self.run_marker(status, output, f"serial_run_{key}"):
            raise LegStopped(f"serial_run_{key}")
        base = config["output"]["output_filename"]
        self.check_fixture_nonzero(key, run_dir, base)
        self.references[key] = run_dir

    def check_created_rows(self, key: str, config: dict) -> None:
        """The serial reference holds created rows (UniqueGalaxyID < 0), so the created-record
        path is never compared vacuously, and some created rows carry non-zero fixture state
        (test_fixture's TestFixtureCreatedContent), so the comparison sees their content."""
        base = config["output"]["output_filename"]
        rows = [table for _, _, table in galaxy_tables(self.references[key], base)]
        created = sum(int(np.count_nonzero(table["UniqueGalaxyID"] < 0)) for table in rows)
        with_state = 0
        for table in rows:
            made = table[table["UniqueGalaxyID"] < 0]
            nonzero = np.zeros(len(made), dtype=bool)
            for field in FIXTURE_FIELDS:
                nonzero |= made[field] != 0.0
            with_state += int(np.count_nonzero(nonzero))
        self.say(
            f"  created rows in the {key} reference: {created}, {with_state} with non-zero "
            "fixture state"
        )
        self.marker(created > 0, f"created_rows_{key}", f"{created} created rows in the reference")
        self.marker(
            with_state > 0,
            f"created_rows_with_content_{key}",
            f"{with_state} of {created} created rows in the reference have non-zero fixture state",
        )

    def chunked_serial(self, config: dict) -> None:
        """Serial runs at every CHUNK_COUNTS value against the serial forest_chunks: 1 run."""
        self.say("--- task-chunk: serial chunked runs")
        for nchunk in CHUNK_COUNTS:
            run_dir = WORK_ROOT / "blocks" / f"chunks{nchunk}"
            derived = with_forest_chunks(config, nchunk)
            status, output = self.launch(
                None,
                write_run_file(derived, run_dir),
                f"blocks serial at forest_chunks {nchunk}",
            )
            if self.run_marker(status, output, f"serial_run_blocks_chunks{nchunk}"):
                with self.case("task-chunk"):
                    self.check_run("blocks", f"chunks{nchunk}", config, run_dir, output, 1, nchunk)

    def mpi_runs(self, leg: str, key: str, config: dict, with_chunks: bool) -> None:
        """MPI runs of one key at every NP_COUNTS rank count and, for the task-chunk leg, every
        NP_CHUNK_PAIRS pair."""
        self.say(f"--- {leg}: MPI runs")
        pairs = [(ntask, None) for ntask in NP_COUNTS]
        pairs += list(NP_CHUNK_PAIRS) if with_chunks else []
        for ntask, nchunk in pairs:
            label = f"np{ntask}" if nchunk is None else f"np{ntask}_chunks{nchunk}"
            run_dir = WORK_ROOT / key / label
            derived = config if nchunk is None else with_forest_chunks(config, nchunk)
            title = f"{key} at -np {ntask}" + (f" with forest_chunks {nchunk}" if nchunk else "")
            status, output = self.launch(ntask, write_run_file(derived, run_dir), title)
            if self.run_marker(status, output, f"mpi_run_{key}_{label}"):
                with self.case(leg):
                    self.check_run(key, label, config, run_dir, output, ntask, nchunk)

    def check_run(self, key, label, config, run_dir, output, ntask, nchunk) -> None:
        """One run against its key's serial reference: layout, comparator with --compare-created,
        row order and step statistics."""
        reference_dir = self.references[key]
        base = config["output"]["output_filename"]
        name = f"{key}_{label}"
        serial_totals = read_master(
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
            self.say(f"--- permutation: {scheme} scheme")
            reference_dir = None
            recorded = set()
            for index, order in enumerate(ORDERS):
                run_dir = WORK_ROOT / "permutation" / scheme / f"order{index:02d}"
                name = f"{scheme}_order{index:02d}"
                status, output = self.launch(
                    None,
                    write_run_file(rate_config(config, scheme=scheme, order=order), run_dir),
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
        the same step statistics, every RunProperties entry equal to the first order's but those
        in RUN_PROPERTIES_EXCLUDED, and EnabledModules recording this run's configured order."""
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
        want = read_run_properties(reference_dir / f"{base}.hdf5")
        got = read_run_properties(run_dir / f"{base}.hdf5")
        differing = sorted(key for key in want.keys() | got.keys() if want.get(key) != got.get(key))
        self.marker(
            bool(want) and not differing,
            f"run_properties_{name}",
            f"RunProperties entries that differ from the first order's (or are missing): "
            f"{differing[:4]} ({len(differing)} in all); entries read: {len(want)}",
        )

    # ------------------------------------------------------------------ the battery

    def guarded(self, leg: str, step) -> None:
        """Run one leg's step; a launch that timed out has recorded its failure, and ends it."""
        if leg in self.legs:
            try:
                step()
            except LegStopped as stop:
                self.say(f"Leg {leg} stopped early: {stop}; its failure marker is recorded")

    def run_all(self) -> None:
        """Build each pair once and run, in turn, the legs that use it."""
        rate_file = load_run_file(RATE_RUN_FILE)
        blocks = rate_config(rate_file, scheme="explicit")
        created = rate_config(rate_file, scheme="explicit", creation=True)

        if "cross-driver" in self.legs and self.build_pair(VERTICAL_SIMULATION, mpi=False):
            self.guarded("cross-driver", lambda: self.real_runs("vertical"))
        if not self.build_pair(HORIZONTAL_SIMULATION, mpi=False):
            return
        self.guarded("cross-driver", lambda: self.real_runs("horizontal"))
        if "cross-driver" in self.legs:
            self.guarded("cross-driver", self.cross_driver_compare)
            self.guarded("cross-driver", self.real_permutations)

        def blocks_serial():
            self.say("--- task-chunk: serial reference")
            self.serial_reference("blocks", blocks)
            self.chunked_serial(blocks)

        def creation_serial():
            self.say("--- record-creation: serial reference")
            self.serial_reference("created", created)
            self.check_created_rows("created", created)

        self.guarded("task-chunk", blocks_serial)
        self.guarded("permutation", lambda: self.permutation(blocks))
        self.guarded("record-creation", creation_serial)

        if not (set(self.legs) & {"task-chunk", "record-creation"}):
            return
        if not self.build_pair(HORIZONTAL_SIMULATION, mpi=True):
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
            self.say(f"  {leg}: {self.cases[leg]}/{expected} cases")


# ---------------------------------------------------------------------- run files


def rate_config(
    config: dict,
    scheme: str,
    timestep: str = "fixed",
    order: tuple[str, ...] | None = None,
    creation: bool = False,
    scale: float = 1.0,
) -> dict:
    """A copy of a parsed run file whose rate phase runs under ``scheme``.

    ``timestep`` ``dynamic`` sets ``TimestepScheme: dynamic`` (fixed leaves the key absent, as the
    committed run files do); ``order`` lists the rate modules in the rate phase in that order;
    ``creation`` puts ``test_fixture`` creating records in ``pre_timestep``, ahead of the phase;
    ``scale`` multiplies the fixtures' rate coefficients.
    """
    derived = copy.deepcopy(config)
    modules = derived["modules"]
    modules["parameters"] = scale_coefficients(modules["parameters"], scale)
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
    config["modules"] = copy.deepcopy(load_run_file(RATE_RUN_FILE)["modules"])
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


def read_run_properties(master: Path) -> dict[str, bytes]:
    """Every attribute (``<group path>@<name>``) and dataset (its path) under the master file's
    ``RunProperties``, as raw bytes, but those in RUN_PROPERTIES_EXCLUDED."""
    entries: dict[str, bytes] = {}
    with h5py.File(master, "r") as handle:
        group = handle["RunProperties"]
        found = {"RunProperties": group}
        group.visititems(lambda path, obj: found.update({f"RunProperties/{path}": obj}))
        for path, obj in found.items():
            if isinstance(obj, h5py.Dataset):
                entries[path] = np.asarray(obj[()]).tobytes()
            for name, value in obj.attrs.items():
                entries[f"{path}@{name}"] = np.asarray(value).tobytes()
    return {
        key: value for key, value in entries.items() if not key.startswith(RUN_PROPERTIES_EXCLUDED)
    }


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

    if WORK_ROOT.exists():
        remove_tree(WORK_ROOT)
    mpirun = shlex.split(os.environ.get("MPIRUN", "mpirun"))
    battery = RateBattery(mpirun, legs)
    battery.say(f"Legs: {', '.join(legs)}; launcher: {shlex.join(mpirun)}; log: {LOG_PATH}")

    def steps():
        battery.run_all()
        battery.check_case_counts()

    battery.execute(steps)

    if battery.failed("tests-rate"):
        return 1
    summary = ", ".join(f"{leg} {battery.cases[leg]} cases" for leg in legs)
    if args.only:
        battery.say(
            f"PARTIAL: tests-rate (legs {', '.join(legs)} only; {battery.passes} checks passed, "
            f"but a filtered run is not the gate; log: {LOG_PATH})"
        )
        return 3
    mode = " --offline (cross-driver leg not run)" if args.offline else ""
    battery.say(
        f"PASS: tests-rate{mode} ({battery.passes} checks over {summary}; every leg asserted "
        f"non-zero fixture properties; no skips; log: {LOG_PATH})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
