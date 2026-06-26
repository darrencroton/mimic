#!/usr/bin/env python3
"""Integration coverage for run-scoped UniqueGalaxyID encoding."""

import shutil
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    MIMIC_EXE,
    TEST_DATA_DIR,
    TestSkipped,
    core_input_file,
    create_test_param_file,
    load_binary_halos,
    resolve_sim_config_path,
    run_mimic,
    run_mimic_fresh,
    run_test_suite,
)

TREE_MUL_FAC = 1_000_000_000
OLD_FILE_STRIDE = 1_000_000_000_000_000


def _tree_type_for_param(param_file):
    with Path(param_file).open() as handle:
        run_config = yaml.safe_load(handle)
    sim_config_path = resolve_sim_config_path(run_config["simulation"]["config"], param_file)
    with sim_config_path.open() as handle:
        sim_config = yaml.safe_load(handle)
    return sim_config["input"]["tree_type"]


def _numeric_partition_key(path):
    return int(path.name.rsplit("_", 1)[1])


def test_selected_simulation_unique_ids_are_unique():
    """The selected simulation's binary output must not contain duplicate real galaxy IDs."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    param_file, output_dir, temp_dir = create_test_param_file(
        "uniquegalid_selected_simulation",
        output_format="binary",
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert (
            returncode == 0
        ), f"Mimic execution failed (rc={returncode})\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"

        output_files = sorted(output_dir.glob("model_z*_*"), key=_numeric_partition_key)
        assert output_files, f"No binary output partitions found in {output_dir}"

        seen = set()
        total_ids = 0
        for output_file in output_files:
            halos, _ = load_binary_halos(output_file)
            ids = halos.UniqueGalaxyID.astype(np.int64, copy=False)
            assert len(ids) > 0, f"{output_file} should contain output halos"
            assert np.all(ids > 0), "UniqueGalaxyID 0 is reserved as a sentinel"

            unique_ids = np.unique(ids)
            assert len(unique_ids) == len(ids), f"Duplicate UniqueGalaxyID values in {output_file}"

            overlap = seen.intersection(int(value) for value in unique_ids)
            assert (
                not overlap
            ), f"Duplicate UniqueGalaxyID values across partitions: {sorted(overlap)[:5]}"
            seen.update(int(value) for value in unique_ids)
            total_ids += len(ids)

        assert (
            len(seen) == total_ids
        ), "Global UniqueGalaxyID set size should match output row count"
    finally:
        shutil.rmtree(temp_dir)


def _load_lhalo_binary_bytes(source_path):
    data = source_path.read_bytes()
    if len(data) < 8:
        raise ValueError(f"L-Halo fixture is too small: {source_path}")

    ntrees, total_halos = struct.unpack_from("=ii", data, 0)
    if ntrees < 3:
        raise ValueError(f"Need at least three source trees, found {ntrees}")

    counts_offset = 8
    counts_size = 4 * ntrees
    counts = list(struct.unpack_from(f"={ntrees}i", data, counts_offset))
    if sum(counts) != total_halos:
        raise ValueError(f"Header total {total_halos} does not match per-tree sum {sum(counts)}")

    data_offset = counts_offset + counts_size
    payload_size = len(data) - data_offset
    if payload_size % total_halos != 0:
        raise ValueError(f"Cannot infer RawHalo byte size from fixture {source_path}")

    return {
        "data": data,
        "counts": counts,
        "data_offset": data_offset,
        "record_size": payload_size // total_halos,
    }


def _write_lhalo_partition(fixture, dest_path, tree_indices):
    counts = fixture["counts"]
    selected_counts = [counts[index] for index in tree_indices]
    total_halos = sum(selected_counts)

    halo_starts = [0]
    for count in counts[:-1]:
        halo_starts.append(halo_starts[-1] + count)

    with dest_path.open("wb") as handle:
        handle.write(struct.pack("=ii", len(tree_indices), total_halos))
        handle.write(struct.pack(f"={len(selected_counts)}i", *selected_counts))
        for index in tree_indices:
            start = fixture["data_offset"] + halo_starts[index] * fixture["record_size"]
            size = counts[index] * fixture["record_size"]
            handle.write(fixture["data"][start : start + size])

    return len(tree_indices)


def _create_two_file_lhalo_fixture(input_dir):
    input_dir.mkdir(parents=True, exist_ok=True)
    fixture = _load_lhalo_binary_bytes(TEST_DATA_DIR / "input" / "trees_063.0")
    file0_tree_count = _write_lhalo_partition(fixture, input_dir / "trees_063.0", [0, 1])
    _write_lhalo_partition(fixture, input_dir / "trees_063.1", [2])
    return file0_tree_count


def _rewrite_simulation_config(param_file, input_dir):
    with param_file.open() as handle:
        run_config = yaml.safe_load(handle)

    sim_config_path = resolve_sim_config_path(run_config["simulation"]["config"], param_file)

    with sim_config_path.open() as handle:
        sim_config = yaml.safe_load(handle)

    sim_config["input"]["first_file"] = 0
    sim_config["input"]["last_file"] = 1
    sim_config["input"]["tree_name"] = "trees_063"
    sim_config["input"]["tree_type"] = "lhalo_binary"
    sim_config["input"]["simulation_dir"] = str(input_dir)

    with sim_config_path.open("w") as handle:
        yaml.dump(sim_config, handle, default_flow_style=False, sort_keys=False)

    run_config.setdefault("input", {})
    run_config["input"]["first_file"] = 0
    run_config["input"]["last_file"] = 1
    with param_file.open("w") as handle:
        yaml.dump(run_config, handle, default_flow_style=False, sort_keys=False)


def test_lhalo_two_file_prefix_offset_encoding():
    """File 1 IDs use file 0's tree-count prefix, not the old file-number stride."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    if _tree_type_for_param(core_input_file("test_binary.yaml")) != "lhalo_binary":
        raise TestSkipped("L-Halo prefix-offset fixture requires a lhalo_binary build")

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_unique_id_"))
    try:
        input_dir = temp_dir / "input"
        file0_tree_count = _create_two_file_lhalo_fixture(input_dir)
        assert file0_tree_count == 2, "test fixture must distinguish prefix offset from file id"

        param_file, output_dir, _ = create_test_param_file(
            "uniquegalid_two_file",
            first_file=0,
            last_file=1,
            temp_dir=temp_dir,
            output_format="binary",
        )
        _rewrite_simulation_config(param_file, input_dir)

        output0 = output_dir / "model_z0.000_0"
        output1 = output_dir / "model_z0.000_1"
        run_mimic_fresh(param_file, output1)

        assert output0.exists(), f"Missing output for file 0: {output0}"
        assert output1.exists(), f"Missing output for file 1: {output1}"

        halos0, _ = load_binary_halos(output0)
        halos1, _ = load_binary_halos(output1)
        assert len(halos0) > 0, "file 0 output must exercise the first partition"
        assert len(halos1) > 0, "file 1 output must exercise the second partition"

        forest_numbers0 = set(
            int(value) - 1 for value in np.unique(halos0.UniqueGalaxyID // TREE_MUL_FAC)
        )
        forest_numbers1 = set(
            int(value) - 1 for value in np.unique(halos1.UniqueGalaxyID // TREE_MUL_FAC)
        )

        assert forest_numbers0 == {
            0,
            1,
        }, f"file 0 should contain forests 0 and 1, got {forest_numbers0}"
        assert forest_numbers1 == {
            file0_tree_count
        }, f"file 1 should start at global forest {file0_tree_count}, got {forest_numbers1}"
        assert np.all(
            halos1.UniqueGalaxyID < OLD_FILE_STRIDE
        ), "file 1 IDs should not include the old file-number stride"
    finally:
        shutil.rmtree(temp_dir)


def main():
    return run_test_suite(
        [
            test_selected_simulation_unique_ids_are_unique,
            test_lhalo_two_file_prefix_offset_encoding,
        ],
        "UniqueGalaxyID Encoding (test_unique_galaxy_id_encoding.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
