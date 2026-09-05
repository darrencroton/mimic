#!/usr/bin/env python3
"""Regression tests for chunked Mimic output consumers."""

import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
MIMIC_PLOT_DIR = HERE.parent
REPO_ROOT = MIMIC_PLOT_DIR.parents[1]
sys.path.insert(0, str(MIMIC_PLOT_DIR))
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import TestSkipped, run_test_suite

try:
    import h5py

    HAVE_H5PY = True
except ImportError:
    HAVE_H5PY = False


def _load_mimic_plot_module():
    spec = importlib.util.spec_from_file_location("mimic_plot", MIMIC_PLOT_DIR / "mimic-plot.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _halo_dtype():
    return np.dtype(
        {
            "names": ["Type", "Len", "Mvir"],
            "formats": [np.int32, np.int32, np.float32],
            "offsets": [0, 4, 8],
            "itemsize": 12,
        }
    )


def _write_schema(output_dir):
    metadata_dir = Path(output_dir) / "metadata"
    metadata_dir.mkdir()
    fields = [
        ("Type", "int32", 0, ""),
        ("Len", "int32", 4, ""),
        ("Mvir", "float32", 8, "1e10 Msun/h"),
    ]
    with (metadata_dir / "output_schema.json").open("w", encoding="utf-8") as handle:
        json.dump(
            {
                "schema_version": 1,
                "record": {"binary_record_size": 12},
                "fields": [
                    {
                        "name": name,
                        "numpy_type": dtype,
                        "shape": [],
                        "offset": offset,
                        "units": units,
                        "description": "",
                    }
                    for name, dtype, offset, units in fields
                ],
            },
            handle,
        )


def _write_binary_partition(path, value):
    record = np.array([(0, value, float(value))], dtype=_halo_dtype())
    with Path(path).open("wb") as handle:
        np.array([1], dtype=np.int32).tofile(handle)
        np.array([1], dtype=np.int32).tofile(handle)
        np.array([1], dtype=np.int32).tofile(handle)
        record.tofile(handle)


def _example_partition_sort_key(path):
    name = Path(path).name
    if name.endswith(".hdf5"):
        name = name[:-5]
    suffix = name.rsplit("_", 1)[-1]
    return (0, int(suffix)) if suffix.isdigit() else (1, str(path))


def _plot_params(tree_type, first_file=0, last_file=0, total_files=1):
    return {
        "Hubble_h": 0.7,
        "BoxSize": 10.0,
        "OutputFormat": "binary",
        "TreeType": tree_type,
        "FirstFile": first_file,
        "LastFile": last_file,
        "NumSimulationTreeFiles": last_file - first_file + 1,
        "SimulationTotalTreeFiles": total_files,
    }


def _write_hdf5_master(path, snapshot_num, records_per_group):
    """Write a master-format HDF5 file with one File subgroup per record value."""
    dtype = _halo_dtype()
    with h5py.File(path, "w") as handle:
        snap = handle.create_group(f"Snap{snapshot_num:03d}")
        for index, value in enumerate(records_per_group):
            data = np.array([(0, value, float(value))], dtype=dtype)
            snap.create_group(f"File{index}").create_dataset("Galaxies", data=data)


def _write_a_list(path, num_snapshots):
    """Write a strictly increasing expansion-factor list covering num_snapshots entries."""
    factors = [(index + 1) / float(num_snapshots) for index in range(num_snapshots)]
    Path(path).write_text("\n".join(f"{a:.6f}" for a in factors) + "\n", encoding="utf-8")


def test_binary_ctrees_discovers_chunk_ids_numerically():
    mimic_plot = _load_mimic_plot_module()
    with tempfile.TemporaryDirectory() as tmp:
        _write_schema(tmp)
        model_path = os.path.join(tmp, "model_z0.000")
        for chunk_id in (10, 0, 2):
            _write_binary_partition(f"{model_path}_{chunk_id}", chunk_id + 1)

        galaxies, volume, metadata = mimic_plot.read_data(
            model_path, 0, 0, params=_plot_params("consistent_trees_hdf5"), quiet=True
        )

        np.testing.assert_array_equal(galaxies.Len, np.array([1, 3, 11], dtype=np.int32))
        assert metadata["good_files"] == 1
        assert volume == 1000.0


def test_binary_lhalo_preserves_input_file_range():
    mimic_plot = _load_mimic_plot_module()
    with tempfile.TemporaryDirectory() as tmp:
        _write_schema(tmp)
        model_path = os.path.join(tmp, "model_z0.000")
        _write_binary_partition(f"{model_path}_0", 1)
        _write_binary_partition(f"{model_path}_2", 3)

        galaxies, _, metadata = mimic_plot.read_data(
            model_path, 0, 0, params=_plot_params("lhalo_binary"), quiet=True
        )

        np.testing.assert_array_equal(galaxies.Len, np.array([1], dtype=np.int32))
        assert metadata["good_files"] == 1


def test_binary_ctrees_volume_uses_input_range_not_chunk_count():
    mimic_plot = _load_mimic_plot_module()
    with tempfile.TemporaryDirectory() as tmp:
        _write_schema(tmp)
        model_path = os.path.join(tmp, "model_z0.000")
        for chunk_id in (0, 1000):
            _write_binary_partition(f"{model_path}_{chunk_id}", chunk_id + 1)

        galaxies, volume, metadata = mimic_plot.read_data(
            model_path,
            0,
            3,
            params=_plot_params("consistent_trees_hdf5", first_file=0, last_file=3, total_files=8),
            quiet=True,
        )

        assert len(galaxies) == 2
        assert metadata["good_files"] == 4
        assert volume == 500.0


def test_hdf5_master_file_groups_sort_numerically():
    if not HAVE_H5PY:
        raise TestSkipped("h5py not installed")

    import hdf5_reader

    dtype = _halo_dtype()
    with tempfile.TemporaryDirectory() as tmp:
        master_path = Path(tmp) / "model.hdf5"
        with h5py.File(master_path, "w") as handle:
            snap = handle.create_group("Snap063")
            for chunk_id in (10, 1000, 2):
                data = np.array([(0, chunk_id, float(chunk_id))], dtype=dtype)
                file_group = snap.create_group(f"File{chunk_id}")
                file_group.create_dataset("Galaxies", data=data)

        halos = hdf5_reader.read_hdf5_snapshot(master_path, 63)

        np.testing.assert_array_equal(halos["Len"], np.array([2, 10, 1000], dtype=np.int32))


def test_hdf5_reader_reads_only_requested_fields():
    """Evolution plots read a field subset; the reader must return just those fields."""
    if not HAVE_H5PY:
        raise TestSkipped("h5py not installed")

    import hdf5_reader

    with tempfile.TemporaryDirectory() as tmp:
        master_path = Path(tmp) / "model.hdf5"
        _write_hdf5_master(master_path, 63, (7, 9))

        subset = hdf5_reader.read_hdf5_snapshot(master_path, 63, fields=["Mvir"])
        full = hdf5_reader.read_hdf5_snapshot(master_path, 63)

        assert subset.dtype.names == ("Mvir",)
        assert subset.itemsize < full.itemsize
        np.testing.assert_array_equal(subset["Mvir"], np.array([7.0, 9.0], dtype=np.float32))
        np.testing.assert_array_equal(subset["Mvir"], full["Mvir"])
        assert full.dtype.names == ("Type", "Len", "Mvir")


def test_hdf5_reader_falls_back_to_full_record_for_unknown_field():
    """A field the output does not carry must degrade to a full read, not an empty one."""
    if not HAVE_H5PY:
        raise TestSkipped("h5py not installed")

    import hdf5_reader

    with tempfile.TemporaryDirectory() as tmp:
        master_path = Path(tmp) / "model.hdf5"
        _write_hdf5_master(master_path, 63, (7,))

        halos = hdf5_reader.read_hdf5_snapshot(master_path, 63, fields=["Mvir", "StellarMass"])

        assert halos.dtype.names == ("Type", "Len", "Mvir")


def test_hdf5_reader_reads_only_requested_fields_direct_format():
    """The non-master (individual-file) branch must also honour a field subset."""
    if not HAVE_H5PY:
        raise TestSkipped("h5py not installed")

    import hdf5_reader

    dtype = _halo_dtype()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "model_000.hdf5"
        with h5py.File(path, "w") as handle:
            snap = handle.create_group("Snap063")
            data = np.array([(0, 7, 7.0), (0, 9, 9.0)], dtype=dtype)
            snap.create_dataset("Galaxies", data=data)

        subset = hdf5_reader.read_hdf5_snapshot(path, 63, fields=["Mvir"])
        full = hdf5_reader.read_hdf5_snapshot(path, 63)

        assert subset.dtype.names == ("Mvir",)
        assert subset.itemsize < full.itemsize
        np.testing.assert_array_equal(subset["Mvir"], np.array([7.0, 9.0], dtype=np.float32))


def test_resolve_evolution_read_fields_narrows_by_selected_plots():
    """--plots= narrows the candidate set before the field union is computed."""
    mimic_plot = _load_mimic_plot_module()

    plot_modules = {"hmf_evolution": None, "smf_evolution": None}
    registry = {
        "hmf_evolution": ["Type", "Mvir"],
        "smf_evolution": ["StellarMass"],
    }

    # No --plots= restriction: union of every candidate plot's fields.
    fields = mimic_plot.resolve_evolution_read_fields(plot_modules, None, registry)
    assert fields == ["Mvir", "StellarMass", "Type"]

    # --plots=hmf_evolution: only that plot's fields.
    fields = mimic_plot.resolve_evolution_read_fields(plot_modules, {"hmf_evolution"}, registry)
    assert fields == ["Mvir", "Type"]


def test_resolve_evolution_read_fields_falls_back_when_registry_incomplete():
    """A candidate plot missing from the registry must force a full read (None)."""
    mimic_plot = _load_mimic_plot_module()

    plot_modules = {"hmf_evolution": None, "smf_evolution": None}
    incomplete_registry = {"hmf_evolution": ["Type", "Mvir"]}  # smf_evolution missing

    fields = mimic_plot.resolve_evolution_read_fields(plot_modules, None, incomplete_registry)
    assert fields is None

    # An empty registry (e.g. a model package with no EVOLUTION_PLOT_FIELDS at all)
    # must fall back the same way.
    fields = mimic_plot.resolve_evolution_read_fields(plot_modules, None, {})
    assert fields is None

    # No candidate plots at all (--plots= names only snapshot plots) also falls back.
    complete_registry = {"hmf_evolution": ["Type", "Mvir"], "smf_evolution": ["StellarMass"]}
    fields = mimic_plot.resolve_evolution_read_fields(
        plot_modules, {"halo_mass_function"}, complete_registry
    )
    assert fields is None


def test_read_data_hdf5_threads_field_subset():
    """read_data() must pass its field subset through to the HDF5 reader."""
    if not HAVE_H5PY:
        raise TestSkipped("h5py not installed")

    mimic_plot = _load_mimic_plot_module()
    with tempfile.TemporaryDirectory() as tmp:
        _write_schema(tmp)
        _write_hdf5_master(Path(tmp) / "model.hdf5", 1, (3, 5))
        a_list = Path(tmp) / "test.a_list"
        _write_a_list(a_list, 2)

        params = _plot_params("lhalo_binary")
        params.update(
            {
                "OutputFormat": "hdf5",
                "OutputFileBaseName": "model",
                "FileWithSnapList": str(a_list),
                "OutputSnapshots": [1],
            }
        )

        galaxies, _, metadata = mimic_plot.read_data(
            os.path.join(tmp, "model"), 0, 0, params=params, quiet=True, fields=["Mvir"]
        )

        assert galaxies.dtype.names == ("Mvir",)
        assert metadata["ngals"] == 2
        np.testing.assert_array_equal(galaxies.Mvir, np.array([3.0, 5.0], dtype=np.float32))

        full, _, _ = mimic_plot.read_data(
            os.path.join(tmp, "model"), 0, 0, params=params, quiet=True
        )

        assert full.dtype.names == ("Type", "Len", "Mvir")


def test_generated_example_partition_sort_handles_binary_redshift_names():
    template = (REPO_ROOT / "src/io/output/python_example.c").read_text(encoding="utf-8")
    assert "name = Path(path).name" in template
    assert 'if name.endswith(\\".hdf5\\"):' in template

    paths = [
        "/tmp/model_z0.000_10",
        "/tmp/model_z0.000_2",
        "/tmp/model_z0.000_1000",
    ]

    ordered = [Path(path).name for path in sorted(paths, key=_example_partition_sort_key)]

    assert ordered == ["model_z0.000_2", "model_z0.000_10", "model_z0.000_1000"]


def main():
    return run_test_suite(
        [
            test_binary_ctrees_discovers_chunk_ids_numerically,
            test_binary_lhalo_preserves_input_file_range,
            test_binary_ctrees_volume_uses_input_range_not_chunk_count,
            test_hdf5_master_file_groups_sort_numerically,
            test_hdf5_reader_reads_only_requested_fields,
            test_hdf5_reader_falls_back_to_full_record_for_unknown_field,
            test_hdf5_reader_reads_only_requested_fields_direct_format,
            test_resolve_evolution_read_fields_narrows_by_selected_plots,
            test_resolve_evolution_read_fields_falls_back_when_registry_incomplete,
            test_read_data_hdf5_threads_field_subset,
            test_generated_example_partition_sort_handles_binary_redshift_names,
        ],
        "Chunked Output Consumers (test_chunked_consumers.py)",
    )


if __name__ == "__main__":
    sys.exit(main())
