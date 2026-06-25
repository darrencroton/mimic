#!/usr/bin/env python3
"""
micro-Uchuu ASCII chunked output regression tests.

Validates that the consistent_trees_ascii reader requires an explicit
forests_per_file setting and emits deterministic forest-count chunks.
"""

import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np
import yaml


def find_repo_root(start):
    for parent in [start, *start.parents]:
        if (parent / "tests").is_dir() and (parent / "src").is_dir():
            return parent
    raise RuntimeError(f"Could not find repository root from {start}")


REPO_ROOT = find_repo_root(Path(__file__).resolve())
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TestSkipped,
    compiled_simulation,
    run_mimic,
    run_mimic_fresh,
    run_test_suite,
    simulation_input_file,
)

SNAP_GROUP = "Snap049"
FIXTURE_NFORESTS = 3


def _require_ready():
    sim = compiled_simulation()
    if sim != "micro-uchuu-ascii":
        raise TestSkipped(f"compiled simulation is {sim!r}, not micro-uchuu-ascii")
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built -- run: make SIMULATION=micro-uchuu-ascii")


def _make_param(temp_dir, output_name, forests_per_file=None):
    with simulation_input_file("test_hdf5.yaml").open() as handle:
        config = yaml.safe_load(handle)

    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)
    config["output"]["output_directory"] = str(output_dir)
    if forests_per_file is not None:
        config["output"]["forests_per_file"] = forests_per_file
    else:
        config["output"].pop("forests_per_file", None)

    param_file = Path(temp_dir) / f"{output_name}.yaml"
    with param_file.open("w") as handle:
        yaml.dump(config, handle, default_flow_style=False, sort_keys=False)

    return param_file, output_dir


def _load_hdf5_union(output_dir):
    import h5py

    rows = []
    files = sorted(output_dir.glob("model_*.hdf5"))
    for output_file in files:
        with h5py.File(output_file, "r") as handle:
            galaxies = handle[f"{SNAP_GROUP}/Galaxies"][:]
            rows.append(galaxies)
    if not rows:
        raise AssertionError(f"No HDF5 chunk files found in {output_dir}")
    return np.concatenate(rows), files


def _canonical(records):
    order = np.argsort(records["UniqueGalaxyID"])
    records = records[order]
    fields = ["UniqueGalaxyID", "MostBoundID", "SnapNum", "Mvir", "Len"]
    return {field: records[field] for field in fields}


def _assert_same_records(actual, expected):
    assert np.all(actual["UniqueGalaxyID"] != 0), "chunked output must not emit sentinel ID 0"
    actual_data = _canonical(actual)
    expected_data = _canonical(expected)
    for field, expected_values in expected_data.items():
        actual_values = actual_data[field]
        if np.issubdtype(expected_values.dtype, np.floating):
            np.testing.assert_allclose(actual_values, expected_values)
        else:
            np.testing.assert_array_equal(actual_values, expected_values)


def _assert_master_links(output_dir, chunk_count):
    import h5py

    master_file = output_dir / "model.hdf5"
    assert master_file.exists(), f"HDF5 master file not created: {master_file}"
    with h5py.File(master_file, "r") as handle:
        assert SNAP_GROUP in handle, f"{SNAP_GROUP} missing from {master_file}"
        snap_group = handle[SNAP_GROUP]
        for chunk_id in range(chunk_count):
            file_group_name = f"File{chunk_id:03d}"
            assert file_group_name in snap_group, f"{SNAP_GROUP}/{file_group_name} missing"
            file_group_path = f"{SNAP_GROUP}/{file_group_name}"
            for dataset_name in ("Galaxies", "TreeHalosPerSnap"):
                link_path = f"{file_group_path}/{dataset_name}"
                link = handle.get(link_path, getlink=True)
                assert isinstance(link, h5py.ExternalLink), f"{link_path} is not an external link"


def test_ascii_requires_forests_per_file():
    _require_ready()

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_ascii_chunks_"))
    try:
        param_file, _output_dir = _make_param(temp_dir, "missing_forests_per_file")
        returncode, stdout, stderr = run_mimic(param_file)
        combined = f"{stdout}\n{stderr}"
        assert returncode != 0, "ASCII run without forests_per_file should fail"
        assert "consistent_trees_ascii requires output.forests_per_file > 0" in combined, combined
    finally:
        shutil.rmtree(temp_dir)


def test_ascii_chunked_hdf5_matches_single_chunk_hdf5():
    _require_ready()

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_ascii_chunks_"))
    try:
        baseline_param, baseline_dir = _make_param(temp_dir, "baseline_hdf5", 1000000)
        chunked_param, chunked_dir = _make_param(temp_dir, "chunked_hdf5", 1)

        run_mimic_fresh(baseline_param)
        baseline, baseline_files = _load_hdf5_union(baseline_dir)
        assert len(baseline_files) == 1, f"baseline should be one chunk, got {baseline_files}"

        run_mimic_fresh(chunked_param)
        chunked, chunked_files = _load_hdf5_union(chunked_dir)
        assert (
            len(chunked_files) == FIXTURE_NFORESTS
        ), f"expected one HDF5 chunk per fixture forest, got {chunked_files}"
        _assert_master_links(chunked_dir, FIXTURE_NFORESTS)
        _assert_same_records(chunked, baseline)
    finally:
        shutil.rmtree(temp_dir)


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [
                test_ascii_requires_forests_per_file,
                test_ascii_chunked_hdf5_matches_single_chunk_hdf5,
            ],
            "micro-Uchuu ASCII chunked output (test_ascii_chunks.py)",
        )
    )
