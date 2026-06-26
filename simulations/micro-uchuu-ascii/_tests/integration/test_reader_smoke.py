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

import shutil
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
Z0_SNAPSHOT_GROUP = "Snap049"
# Production micro-Uchuu ASCII has ~1.2M halos at Snap049; the smoke test checks
# for a meaningful lower bound rather than an exact count, since the count
# varies with the selected model's physics.
MIN_EXPECTED_Z0_HALOS = 10_000

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


def _make_param(temp_dir, output_name):
    with simulation_input_file("test_hdf5.yaml").open() as handle:
        config = yaml.safe_load(handle)

    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)
    config["output"]["output_directory"] = str(output_dir)
    # Keep this explicit so direct test runs still work if generated inputs are stale.
    config["output"]["forests_per_file"] = 1000000

    param_file = Path(temp_dir) / f"{output_name}.yaml"
    with param_file.open("w") as handle:
        yaml.dump(config, handle, default_flow_style=False, sort_keys=False)

    return param_file, output_dir / "model_000.hdf5"


def test_ascii_reader_loads():
    """Run against micro-Uchuu ASCII production data; expect exit 0."""
    _require_simulation()
    _require_exe()

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_ascii_smoke_"))
    try:
        param_file, output_file = _make_param(temp_dir, "load")
        run_mimic_fresh(param_file, output_file)
    finally:
        shutil.rmtree(temp_dir)


def test_ascii_reader_halo_count():
    """Output must contain a meaningful number of halos at z=0."""
    _require_simulation()
    _require_exe()

    import h5py

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_ascii_smoke_"))
    try:
        param_file, output_file = _make_param(temp_dir, "count")
        run_mimic_fresh(param_file, output_file)
        with h5py.File(output_file, "r") as hf:
            assert Z0_SNAPSHOT_GROUP in hf, f"{Z0_SNAPSHOT_GROUP} missing from {output_file}"
            galaxies = hf[Z0_SNAPSHOT_GROUP].get("Galaxies")
            assert galaxies is not None, f"{Z0_SNAPSHOT_GROUP}/Galaxies missing from {output_file}"
            assert (
                galaxies.shape[0] >= MIN_EXPECTED_Z0_HALOS
            ), f"Expected at least {MIN_EXPECTED_Z0_HALOS} output halos, found {galaxies.shape[0]}"
    finally:
        shutil.rmtree(temp_dir)


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_ascii_reader_loads, test_ascii_reader_halo_count],
            "micro-Uchuu ASCII reader smoke (test_reader_smoke.py)",
        )
    )
