#!/usr/bin/env python3
"""
micro-Uchuu HDF5 chunked output regression tests.

Validates that the consistent_trees_hdf5 reader emits output chunks independent
of the task partition model and preserves galaxy data across chunk sizes.
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
    load_binary_halos,
    run_mimic_fresh,
    run_test_suite,
    simulation_input_file,
)

SNAP_GROUP = "Snap049"
FIXTURE_NFORESTS = 3


def _require_ready():
    sim = compiled_simulation()
    if sim != "micro-uchuu-hdf5":
        raise TestSkipped(f"compiled simulation is {sim!r}, not micro-uchuu-hdf5")
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built -- run: make SIMULATION=micro-uchuu-hdf5")


def _make_param(ref_name, temp_dir, output_format, forests_per_file, output_name):
    with simulation_input_file(ref_name).open() as handle:
        config = yaml.safe_load(handle)

    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)
    config["output"]["output_directory"] = str(output_dir)
    config["output"]["output_format"] = output_format
    config["output"]["forests_per_file"] = forests_per_file

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


def _load_binary_union(output_dir):
    rows = []
    files = sorted(output_dir.glob("model_z*_*"))
    for output_file in files:
        halos, _ = load_binary_halos(output_file)
        rows.append(halos)
    if not rows:
        raise AssertionError(f"No binary chunk files found in {output_dir}")
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


def test_chunked_hdf5_and_binary_match_single_chunk_hdf5():
    _require_ready()

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_ctrees_chunks_"))
    try:
        baseline_param, baseline_dir = _make_param(
            "test_hdf5.yaml", temp_dir, "hdf5", 1000000, "baseline_hdf5"
        )
        chunked_hdf5_param, chunked_hdf5_dir = _make_param(
            "test_hdf5.yaml", temp_dir, "hdf5", 1, "chunked_hdf5"
        )
        chunked_binary_param, chunked_binary_dir = _make_param(
            "test_binary.yaml", temp_dir, "binary", 1, "chunked_binary"
        )

        run_mimic_fresh(baseline_param)
        baseline, baseline_files = _load_hdf5_union(baseline_dir)
        assert len(baseline_files) == 1, f"baseline should be one chunk, got {baseline_files}"

        run_mimic_fresh(chunked_hdf5_param)
        chunked_hdf5, hdf5_files = _load_hdf5_union(chunked_hdf5_dir)
        assert (
            len(hdf5_files) == FIXTURE_NFORESTS
        ), f"expected one HDF5 chunk per fixture forest, got {hdf5_files}"
        _assert_same_records(chunked_hdf5, baseline)

        run_mimic_fresh(chunked_binary_param)
        chunked_binary, binary_files = _load_binary_union(chunked_binary_dir)
        assert (
            len(binary_files) == FIXTURE_NFORESTS
        ), f"expected one binary chunk per fixture forest, got {binary_files}"
        _assert_same_records(chunked_binary, baseline)
    finally:
        shutil.rmtree(temp_dir)


if __name__ == "__main__":
    sys.exit(
        run_test_suite(
            [test_chunked_hdf5_and_binary_match_single_chunk_hdf5],
            "micro-Uchuu HDF5 chunked output (test_chunked_output.py)",
        )
    )
