#!/usr/bin/env python3
"""
micro-Uchuu ASCII Reader Smoke Test

Validates: consistent_trees_ascii reader can load the ASCII fixture
(forests.list + locations.dat + tree_0_0_0.dat) and produce sensible halo output.

Skips automatically when:
  - SIMULATION != micro-uchuu-ascii (wrong compiled package)
  - Mimic executable is not built

Run with:
  python3 simulations/micro-uchuu-ascii/_tests/integration/test_reader_smoke.py
or (when the compilation is set up):
  make SIMULATION=micro-uchuu-ascii MODEL=halos-only tests-integration
"""

import sys
from pathlib import Path


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
OUTPUT_FILE = REPO_ROOT / "tests" / "data" / "output" / "hdf5" / "model_000.hdf5"
Z0_SNAPSHOT_GROUP = "Snap049"
EXPECTED_Z0_HALOS = 3  # fixture: 3 forests, each contributing one snap49 halo

sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    MIMIC_EXE,
    TestSkipped,
    compiled_simulation,
    run_mimic_fresh,
    run_test_suite,
    simulation_input_file,
)


def _require_simulation():
    sim = compiled_simulation()
    if sim != "micro-uchuu-ascii":
        raise TestSkipped(f"compiled simulation is {sim!r}, not micro-uchuu-ascii")


def _require_exe():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built — run: make SIMULATION=micro-uchuu-ascii")


def test_ascii_reader_loads():
    """Run halos-only on the tiny ASCII fixture; expect exit 0."""
    _require_simulation()
    _require_exe()

    run_mimic_fresh(simulation_input_file("test_hdf5.yaml"), OUTPUT_FILE)


def test_ascii_reader_halo_count():
    """Output must contain the expected tiny-fixture halos at z=0."""
    _require_simulation()
    _require_exe()

    import h5py

    run_mimic_fresh(simulation_input_file("test_hdf5.yaml"), OUTPUT_FILE)
    with h5py.File(OUTPUT_FILE, "r") as hf:
        assert Z0_SNAPSHOT_GROUP in hf, f"{Z0_SNAPSHOT_GROUP} missing from {OUTPUT_FILE}"
        galaxies = hf[Z0_SNAPSHOT_GROUP].get("Galaxies")
        assert galaxies is not None, f"{Z0_SNAPSHOT_GROUP}/Galaxies missing from {OUTPUT_FILE}"
        assert (
            galaxies.shape[0] == EXPECTED_Z0_HALOS
        ), f"Expected {EXPECTED_Z0_HALOS} output halos, found {galaxies.shape[0]}"


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_ascii_reader_loads, test_ascii_reader_halo_count],
            "micro-Uchuu ASCII reader smoke (test_reader_smoke.py)",
        )
    )
