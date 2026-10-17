#!/usr/bin/env python3
"""
shin-uchuu-lhalo fixture identity, halo by halo.

Proves that the committed worked_ascii_lhalo fixture says, through Mimic's own
lhalo_binary reader, exactly what the version 3 dataset it was converted from says:
every halo, every link and every payload bit. The reader's view comes from the
reference dump tool (tests/unit/tools/dump_ctrees_topology.c, built by
tests/unit/tools/build_topology_dump.sh into a temporary directory of its own for
the compiled model and this package), run in --source-payload mode through a
scratch run file naming the fixture. The comparison is the generalisation
acceptance harness's `compare`
(convert/mimic-convert/tests/run_generalisation_acceptance.py) against
simulations/shin-uchuu/_tests/data/worked_ascii, the fixture's source, whose
header names the Consistent-Trees ASCII route it was itself converted from.

The build regenerates property and module code for the compiled pair, which is
the code already in place, so it is run in the same generation mode (test build
or production) the ambient generated code was made in, and the test fails if that
code changed.

Skips unless SIMULATION=shin-uchuu-lhalo, or when h5py is not installed.

Run with:
  make MODEL=halos-only SIMULATION=shin-uchuu-lhalo generate
  MODEL=halos-only SIMULATION=shin-uchuu-lhalo \\
      python3 simulations/shin-uchuu-lhalo/_tests/integration/test_fixture_halo_identity.py
or (registered):
  make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-integration
"""

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    TestSkipped,
    compiled_model,
    compiled_simulation,
    run_test_suite,
)

PACKAGE = "shin-uchuu-lhalo"
FIXTURE = REPO_ROOT / "simulations" / PACKAGE / "_tests" / "data" / "worked_ascii_lhalo"
FIXTURE_FILES = (0, 1)
SOURCE_DATASET = REPO_ROOT / "simulations" / "shin-uchuu" / "_tests" / "data" / "worked_ascii"
BUILD_SCRIPT = REPO_ROOT / "tests" / "unit" / "tools" / "build_topology_dump.sh"
HARNESS = REPO_ROOT / "convert" / "mimic-convert" / "tests" / "run_generalisation_acceptance.py"

#: The generated code the dump build regenerates for the compiled pair.
GENERATED_DIRS = (
    REPO_ROOT / "src" / "include" / "generated",
    REPO_ROOT / "src" / "module_system" / "generated",
    REPO_ROOT / "tests" / "generated",
)
#: Lists, among its sources, the fixture properties only a test build generates.
PROPERTY_MANIFEST = REPO_ROOT / "tests" / "generated" / "property_ranges.json"
TEST_BUILD_PROPERTIES = "src/module_system/test_fixture/test_properties.yaml"


def generated_state():
    """SHA-256 of every generated file, by path."""
    return {
        path: hashlib.sha256(path.read_bytes()).hexdigest()
        for directory in GENERATED_DIRS
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def ambient_is_test_build():
    """Whether the ambient generated code was made by a test build."""
    sources = json.loads(PROPERTY_MANIFEST.read_text())["_metadata"]["source_files"]
    return TEST_BUILD_PROPERTIES in sources


def run_file(path, model, output_dir):
    """A run file naming the fixture's two files and its snapshot list."""
    lines = [
        f"model:\n  name: {model}\n",
        f"simulation:\n  name: {PACKAGE}\n",
        "input:\n",
        "  tree_name: shin-uchuu-cut\n",
        f"  simulation_dir: {FIXTURE}\n",
        f"  first_file: {FIXTURE_FILES[0]}\n",
        f"  last_file: {FIXTURE_FILES[-1]}\n",
        f"  snapshot_list_file: {FIXTURE / 'worked_ascii.a_list'}\n",
        "output:\n",
        "  output_filename: halos\n",
        f"  output_directory: {output_dir}\n",
        "  output_format: binary\n",
        "  snapshot_list: [5]\n",
        "SubSteps: 1\n",
        "modules:\n  parameters: {}\n",
    ]
    path.write_text("".join(lines))
    return path


def run(argv, env=None):
    result = subprocess.run(
        [str(arg) for arg in argv], cwd=REPO_ROOT, env=env, capture_output=True, text=True
    )
    return result.returncode, result.stdout + result.stderr


def test_fixture_matches_its_source_halo_by_halo():
    """The lhalo_binary reading of the fixture equals its source dataset in every check."""
    simulation = compiled_simulation()
    if simulation != PACKAGE:
        raise TestSkipped(f"compiled simulation is {simulation!r}, not {PACKAGE}")
    if importlib.util.find_spec("h5py") is None:
        raise TestSkipped("h5py is not installed; the comparison reads the source dataset with it")

    model = compiled_model()
    work = Path(tempfile.mkdtemp(prefix="shin_uchuu_lhalo_identity_"))
    try:
        env = dict(
            os.environ,
            MODEL=model,
            SIMULATION=PACKAGE,
            TOPOLOGY_DUMP_BUILD_DIR=str(work / "tool"),
        )
        env.pop("SIM", None)
        env.pop("MIMIC_TEST_BUILD", None)
        if ambient_is_test_build():
            env["MIMIC_TEST_BUILD"] = "1"
        before = generated_state()
        code, output = run(["bash", BUILD_SCRIPT], env=env)
        assert code == 0, f"dump tool build failed ({code}):\n{output[-3000:]}"
        assert generated_state() == before, "building the dump tool changed the generated code"

        record = work / "record.json"
        dump = work / "fixture.dump"
        code, output = run(
            [
                sys.executable,
                HARNESS,
                "dump",
                "--record",
                record,
                "--tool",
                work / "tool" / "dump_ctrees_topology",
                "--run-file",
                run_file(work / "run.yaml", model, work / "output"),
                "--out",
                dump,
            ]
        )
        assert code == 0, f"source dump failed ({code}):\n{output[-3000:]}"

        report = work / "report.json"
        code, output = run(
            [
                sys.executable,
                HARNESS,
                "compare",
                "--record",
                record,
                "--dataset",
                SOURCE_DATASET,
                "--source-format",
                "lhalo_binary",
                "--dataset-source-format",
                "consistent_trees_ascii",
                "--dump",
                dump,
                "--spill-dir",
                work,
                "--report",
                report,
            ]
        )
        result = json.loads(report.read_text()) if report.is_file() else {}
        assert code == 0 and result.get("verdict") == "PASS", (
            f"compare exited {code} with verdict {result.get('verdict')!r}, failed checks "
            f"{result.get('failed_checks')}:\n{output[-3000:]}"
        )
        print(f"  compare PASS: {result.get('matched_rows')} halos matched")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_fixture_matches_its_source_halo_by_halo],
            "shin-uchuu-lhalo fixture halo identity (test_fixture_halo_identity.py)",
        )
    )
