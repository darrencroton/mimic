#!/usr/bin/env python3
"""
Reference-topology dump harness format test.

Validates: tests/unit/tools/dump_ctrees_topology.c, built via
`make dump-ctrees-topology-tool`, loads the tiny micro-Uchuu ASCII fixture
(forests.list + locations.dat + tree_0_0_0.dat) through the production
consistent_trees_ascii reader and emits a well-formed, deterministic dump of
each halo's link fields (Descendant, FirstProgenitor, NextProgenitor,
FirstHaloInFOFgroup, NextHaloInFOFgroup) by stable ctrees id.

Skips automatically when:
  - SIMULATION != micro-uchuu-ascii (wrong compiled package)
  - the harness binary is not built

Run with:
  python3 simulations/micro-uchuu-ascii/_tests/integration/test_topology_dump_format.py
or (when the compilation is set up):
  make SIMULATION=micro-uchuu-ascii MODEL=halos-only tests-integration
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
HARNESS_EXE = REPO_ROOT / "tests" / "unit" / "tools" / "build" / "dump_ctrees_topology"
FIXTURE_SIMULATION_CONFIG = (
    REPO_ROOT / "simulations/micro-uchuu-ascii/_tests/input/test_simulation.yaml"
)
NA_SENTINEL = -(2**63)

sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    TestSkipped,
    compiled_simulation,
    run_test_suite,
    simulation_input_file,
)


def _require_ready():
    sim = compiled_simulation()
    if sim != "micro-uchuu-ascii":
        raise TestSkipped(f"compiled simulation is {sim!r}, not micro-uchuu-ascii")
    if not HARNESS_EXE.exists():
        raise TestSkipped(
            "topology dump harness not built — run: "
            "make MODEL=halos-only SIMULATION=micro-uchuu-ascii dump-ctrees-topology-tool"
        )


def _make_param(temp_dir):
    """A run file pointing at the tiny fixture data, not the production package."""
    with simulation_input_file("test_binary.yaml").open() as handle:
        config = yaml.safe_load(handle)

    config["simulation"]["config"] = str(FIXTURE_SIMULATION_CONFIG)
    output_dir = Path(temp_dir) / "output"
    output_dir.mkdir(parents=True, exist_ok=True)
    config["output"]["output_directory"] = str(output_dir)
    # consistent_trees_ascii requires this explicitly (no implicit default).
    config["output"]["forests_per_file"] = 100000

    param_file = Path(temp_dir) / "run.yaml"
    with param_file.open("w") as handle:
        yaml.dump(config, handle, default_flow_style=False, sort_keys=False)

    return param_file


def _run_harness(param_file, dump_path):
    completed = subprocess.run(
        [str(HARNESS_EXE), str(param_file), str(dump_path)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, (
        f"harness exited {completed.returncode}\nstdout: {completed.stdout}\n"
        f"stderr: {completed.stderr}"
    )
    return dump_path


class DumpRow:
    __slots__ = (
        "forestnr",
        "rank",
        "halo_id",
        "snapnum",
        "desc_id",
        "first_prog_id",
        "next_prog_id",
        "first_fof_id",
        "next_fof_id",
    )

    def __init__(self, fields):
        (
            self.forestnr,
            self.rank,
            self.halo_id,
            self.snapnum,
            self.desc_id,
            self.first_prog_id,
            self.next_prog_id,
            self.first_fof_id,
            self.next_fof_id,
        ) = (int(field) for field in fields)


def _parse_dump(path):
    """Three comment lines (format version, column names, NA sentinel value),
    then one whitespace-separated data row per halo. Raises AssertionError on
    any structural deviation — this is the format the test exists to pin."""
    lines = Path(path).read_text().splitlines()
    assert len(lines) >= 3, f"expected at least 3 header lines, got {len(lines)}"
    assert lines[0] == "# mimic-topology-dump v1", lines[0]
    assert lines[1].startswith("# forestnr rank id snapnum"), lines[1]
    assert lines[2] == f"# NA sentinel = {NA_SENTINEL} (no link)", lines[2]

    rows = [DumpRow(line.split()) for line in lines[3:]]
    return rows


def test_dump_format_and_fixture_topology():
    """The dump must reproduce the fixture's known topology exactly:
    3 forests, 4 halos; forest 0 has a snap49 central with a snap48
    progenitor; forests 1 and 2 each have one unlinked snap49 halo."""
    _require_ready()

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_topology_dump_"))
    try:
        param_file = _make_param(temp_dir)
        dump_path = Path(temp_dir) / "topology.dump"
        _run_harness(param_file, dump_path)
        rows = _parse_dump(dump_path)

        assert len(rows) == 4, f"expected 4 halos in the fixture, got {len(rows)}"
        by_id = {row.halo_id: row for row in rows}
        assert set(by_id) == {1000001, 1000011, 1000002, 1000003}

        forestnrs = {row.forestnr for row in rows}
        assert forestnrs == {0, 1, 2}, f"expected dense forest numbers 0..2, got {forestnrs}"

        central = by_id[1000001]
        progenitor = by_id[1000011]

        assert central.forestnr == progenitor.forestnr == 0
        assert central.snapnum == 49
        assert progenitor.snapnum == 48
        assert central.rank == 0 and progenitor.rank == 1
        assert central.desc_id == NA_SENTINEL, "root halo must have no descendant"
        assert central.first_prog_id == 1000011
        assert central.next_prog_id == NA_SENTINEL, "single progenitor has no sibling"
        assert central.first_fof_id == 1000001, "central is its own FoF group"
        assert central.next_fof_id == NA_SENTINEL, "sole FoF member has no next"

        assert progenitor.desc_id == 1000001
        assert progenitor.first_prog_id == NA_SENTINEL, "leaf halo has no progenitor"
        assert progenitor.next_prog_id == NA_SENTINEL
        assert progenitor.first_fof_id == 1000011
        assert progenitor.next_fof_id == NA_SENTINEL

        for halo_id, forestnr in ((1000002, 1), (1000003, 2)):
            row = by_id[halo_id]
            assert row.forestnr == forestnr
            assert row.rank == 0
            assert row.snapnum == 49
            assert row.desc_id == NA_SENTINEL
            assert row.first_prog_id == NA_SENTINEL
            assert row.next_prog_id == NA_SENTINEL
            assert row.first_fof_id == halo_id
            assert row.next_fof_id == NA_SENTINEL
    finally:
        shutil.rmtree(temp_dir)


def test_dump_is_deterministic():
    """Two independent runs over the same fixture must produce a byte-identical
    dump: this is the direct evidence a consumer compares chain order against,
    so its output must not depend on process-local ordering (e.g. malloc
    layout, hash iteration) between runs."""
    _require_ready()

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_topology_dump_determinism_"))
    try:
        param_file = _make_param(temp_dir)
        dump_a = Path(temp_dir) / "a.dump"
        dump_b = Path(temp_dir) / "b.dump"
        _run_harness(param_file, dump_a)
        _run_harness(param_file, dump_b)
        assert dump_a.read_bytes() == dump_b.read_bytes(), "dump is not deterministic"
    finally:
        shutil.rmtree(temp_dir)


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_dump_format_and_fixture_topology, test_dump_is_deterministic],
            "reference-topology dump format (test_topology_dump_format.py)",
        )
    )
