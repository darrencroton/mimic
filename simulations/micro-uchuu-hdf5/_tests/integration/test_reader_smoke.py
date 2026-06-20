#!/usr/bin/env python3
"""
micro-Uchuu HDF5 Reader Smoke Test

Validates: consistent_trees_hdf5 reader can load the micro-Uchuu forests-HDF5
trees and produce sensible halo output.

Skips automatically when:
  - SIMULATION != micro-uchuu-hdf5 (wrong compiled package)
  - snapshots/ symlink is missing (data not yet available locally)
  - Mimic executable is not built

Run with:
  python3 simulations/micro-uchuu-hdf5/_tests/integration/test_reader_smoke.py
or (when the compilation is set up):
  make SIMULATION=micro-uchuu-hdf5 MODEL=halos-only tests-integration
"""

import sys
from pathlib import Path


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
SIM_DIR = REPO_ROOT / "simulations" / "micro-uchuu-hdf5"
PARAM_FILE = REPO_ROOT / "models" / "halos-only" / "input" / "halos-only_micro-uchuu-hdf5.yaml"
OUTPUT_DIR = REPO_ROOT / "output" / "halos-only-micro-uchuu-hdf5"
Z0_SNAPSHOT_GROUP = "Snap049"

sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import MIMIC_EXE, TestSkipped, compiled_simulation, run_mimic, run_test_suite


def _require_simulation():
    sim = compiled_simulation()
    if sim != "micro-uchuu-hdf5":
        raise TestSkipped(f"compiled simulation is {sim!r}, not micro-uchuu-hdf5")


def _require_snapshots():
    snapshots = SIM_DIR / "snapshots"
    if not snapshots.exists():
        raise TestSkipped(
            f"snapshots/ not present — create symlink: ln -s /fred/oz214/simulations/uchuu/U100/mergertrees {snapshots}"
        )


def _require_exe():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built — run: make SIMULATION=micro-uchuu-hdf5")


def test_hdf5_reader_loads():
    """Run halos-only on snapshot 49 (z=0) via the HDF5 reader; expect exit 0."""
    _require_simulation()
    _require_snapshots()
    _require_exe()

    rc, stdout, stderr = run_mimic(PARAM_FILE)
    assert rc == 0, f"Mimic exited {rc}\nstdout:\n{stdout}\nstderr:\n{stderr}"


def test_hdf5_reader_halo_count():
    """Output must contain a non-trivial number of halos at z=0."""
    _require_simulation()
    _require_snapshots()
    _require_exe()

    import h5py

    output_files = sorted(OUTPUT_DIR.glob("halos_*.hdf5"))
    output_files = [path for path in output_files if path.name != "halos.hdf5"]
    assert output_files, f"No output files found in {OUTPUT_DIR}"

    total_halos = 0
    for output_file in output_files:
        with h5py.File(output_file, "r") as hf:
            assert Z0_SNAPSHOT_GROUP in hf, f"{Z0_SNAPSHOT_GROUP} missing from {output_file}"
            galaxies = hf[Z0_SNAPSHOT_GROUP].get("Galaxies")
            assert galaxies is not None, f"{Z0_SNAPSHOT_GROUP}/Galaxies missing from {output_file}"
            total_halos += galaxies.shape[0]

    # micro-Uchuu has ~440k forests; at z=0 expect hundreds of thousands of halos
    assert total_halos > 100_000, f"Unexpectedly low halo count: {total_halos}"


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_hdf5_reader_loads, test_hdf5_reader_halo_count],
            "micro-Uchuu HDF5 reader smoke (test_reader_smoke.py)",
        )
    )
