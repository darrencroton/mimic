#!/usr/bin/env python3
"""
micro-Uchuu L-Halo Binary Reader Smoke Test

Validates: lhalo_binary reader can load the micro-Uchuu L-Halo binary files
(4 files, ~2.2 GB) and produce sensible halo output.

Skips automatically when:
  - SIMULATION != micro-uchuu-lhalo (wrong compiled package)
  - snapshots/ symlink is missing (data not yet available locally)
  - Mimic executable is not built

Run with:
  python3 simulations/micro-uchuu-lhalo/_tests/integration/test_reader_smoke.py
or (when the compilation is set up):
  make SIMULATION=micro-uchuu-lhalo MODEL=halos-only tests-integration
"""

import sys
from pathlib import Path


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
SIM_DIR = REPO_ROOT / "simulations" / "micro-uchuu-lhalo"
PARAM_FILE = REPO_ROOT / "models" / "halos-only" / "input" / "halos-only_micro-uchuu-lhalo.yaml"

sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import MIMIC_EXE, TestSkipped, compiled_simulation, run_mimic, run_test_suite


def _require_simulation():
    sim = compiled_simulation()
    if sim != "micro-uchuu-lhalo":
        raise TestSkipped(f"compiled simulation is {sim!r}, not micro-uchuu-lhalo")


def _require_snapshots():
    snapshots = SIM_DIR / "snapshots"
    if not snapshots.exists():
        raise TestSkipped(
            f"snapshots/ not present — create symlink: ln -s /fred/oz214/simulations/uchuu/U100/lhalo-binary-mergertree {snapshots}"
        )


def _require_exe():
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built — run: make SIMULATION=micro-uchuu-lhalo")


def test_lhalo_reader_loads():
    """Run halos-only on snapshot 49 (z=0) via the L-Halo binary reader; expect exit 0."""
    _require_simulation()
    _require_snapshots()
    _require_exe()

    rc, stdout, stderr = run_mimic(PARAM_FILE)
    assert rc == 0, f"Mimic exited {rc}\nstdout:\n{stdout}\nstderr:\n{stderr}"


def test_lhalo_reader_halo_count():
    """Output must contain a non-trivial number of halos at z=0."""
    _require_simulation()
    _require_snapshots()
    _require_exe()

    import h5py

    out_dir = REPO_ROOT / "output" / "halos-only-micro-uchuu-lhalo"
    output_files = sorted(out_dir.glob("halos_z0.000_*"))
    assert output_files, f"No output files found in {out_dir}"

    total_halos = 0
    for f in output_files:
        with h5py.File(f, "r") as hf:
            if "Halos" in hf:
                total_halos += hf["Halos/HaloMass"].shape[0]

    # micro-Uchuu has ~440k forests; at z=0 expect hundreds of thousands of halos
    assert total_halos > 100_000, f"Unexpectedly low halo count: {total_halos}"


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_lhalo_reader_loads, test_lhalo_reader_halo_count],
            "micro-Uchuu L-Halo binary reader smoke (test_reader_smoke.py)",
        )
    )
