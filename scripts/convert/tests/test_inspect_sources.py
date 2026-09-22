"""Slice 1 unit tests: read-only L-Halo binary and Consistent-Trees
forests-HDF5 source inspection -- header/count validation, link-span
scanning, reachability reporting, and the inspect_sources.py CLI.

L-Halo binary fixtures under tests/data/source_formats/ were generated with
struct.pack against adapters.source_inventory.LHALO_FIELDS (the shipped
104-byte record order); see the git history of this file for the generator
script if fixtures need regenerating.
"""

import io
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import h5py
import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import inspect_sources  # noqa: E402
from adapters import source_inventory as si  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402

DATA_DIR = Path(__file__).parent / "data" / "source_formats"


class LHaloHeaderTests(unittest.TestCase):
    def test_valid_header_two_trees(self):
        header = si.read_lhalo_header(DATA_DIR / "valid_two_trees.bin", byte_order="<")
        self.assertEqual(header.ntrees, 2)
        self.assertEqual(header.total_halos, 5)
        self.assertEqual(list(header.tree_halo_counts), [3, 2])
        self.assertEqual(header.header_bytes, 8 + 4 * 2)
        self.assertEqual(header.expected_size, header.file_size)

    def test_valid_big_endian_header(self):
        header = si.read_lhalo_header(DATA_DIR / "valid_big_endian.bin", byte_order=">")
        self.assertEqual(header.ntrees, 2)
        self.assertEqual(header.total_halos, 5)

    def test_wrong_byte_order_is_detected_as_corrupt(self):
        # Reading a little-endian file as if it were big-endian must not
        # silently succeed: the byte-swapped Ntrees is nonsensical.
        with self.assertRaises(ConverterError):
            si.read_lhalo_header(DATA_DIR / "valid_two_trees.bin", byte_order=">")

    def test_truncated_header_fails(self):
        with self.assertRaises(ConverterError):
            si.read_lhalo_header(DATA_DIR / "truncated_header.bin")

    def test_truncated_count_table_fails(self):
        with self.assertRaises(ConverterError):
            si.read_lhalo_header(DATA_DIR / "truncated_counts.bin")

    def test_negative_ntrees_fails(self):
        with self.assertRaises(ConverterError):
            si.read_lhalo_header(DATA_DIR / "negative_counts.bin")

    def test_bad_count_total_fails(self):
        with self.assertRaises(ConverterError):
            si.read_lhalo_header(DATA_DIR / "bad_count_total.bin")

    def test_truncated_payload_fails_at_header_stage(self):
        # File length disagrees with header-implied size even before any
        # payload record is read.
        with self.assertRaises(ConverterError):
            si.read_lhalo_header(DATA_DIR / "truncated_payload.bin")

    def test_invalid_byte_order_argument_rejected(self):
        with self.assertRaises(ConverterError):
            si.lhalo_record_dtype("=")


class LHaloLinkScanTests(unittest.TestCase):
    def test_link_span_and_gap_counting(self):
        header = si.read_lhalo_header(DATA_DIR / "valid_two_trees.bin")
        summary = si.scan_lhalo_file(header)
        # tree0: halo0(snap0)->halo1(snap2) span 2 (gap); halo1(snap2)->halo2(snap3) span 1 (adjacent)
        # tree1: two roots, no links
        self.assertEqual(summary.non_null_descendant_links, 2)
        self.assertEqual(summary.forward_adjacent_links, 1)
        self.assertEqual(summary.forward_gap_links, 1)
        self.assertEqual(summary.max_span, 2)
        self.assertEqual(summary.non_forward_or_zero_span, 0)
        self.assertEqual(summary.snapshot_halo_counts, {0: 1, 1: 2, 2: 1, 3: 1})

    def test_big_endian_scan_matches_little_endian_content(self):
        header = si.read_lhalo_header(DATA_DIR / "valid_big_endian.bin", byte_order=">")
        summary = si.scan_lhalo_file(header)
        self.assertEqual(summary.non_null_descendant_links, 2)
        self.assertEqual(summary.forward_gap_links, 1)
        self.assertEqual(summary.max_span, 2)

    def test_out_of_tree_descendant_fails(self):
        # Build a standalone corrupt file: tree with a Descendant >= tree size.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out_of_tree.bin"
            dtype = si.lhalo_record_dtype("<")
            rec = np.zeros(1, dtype=dtype)
            rec["Descendant"] = 5  # only 1 halo in this tree -> out of range
            rec["SnapNum"] = 0
            with open(path, "wb") as f:
                f.write(struct.pack("<ii", 1, 1))
                f.write(struct.pack("<i", 1))
                f.write(rec.tobytes())
            corrupt_header = si.read_lhalo_header(path)
            with self.assertRaises(ConverterError):
                si.scan_lhalo_file(corrupt_header)

    def test_mini_millennium_reproduces_measured_totals(self):
        """Reproduces the plan's measured mini-Millennium totals exactly:
        29,585 trees, 1,533,122 halos, 1,495,274 non-null descendant links,
        29,291 forward gaps, maximum span 2, across all 8 local files."""
        repo_root = Path(__file__).parents[3]
        sim_dir = repo_root / "simulations" / "mini-millennium" / "snapshots"
        files = sorted(sim_dir.glob("trees_063.*"))
        if len(files) != 8:
            self.skipTest("real mini-Millennium data (8 trees_063.* files) not present locally")
        total_trees = 0
        total_halos = 0
        combined = si.LinkSpanSummary()
        for path in files:
            header = si.read_lhalo_header(path)
            total_trees += header.ntrees
            total_halos += header.total_halos
            summary = si.scan_lhalo_file(header)
            combined.non_null_descendant_links += summary.non_null_descendant_links
            combined.forward_gap_links += summary.forward_gap_links
            combined.max_span = max(combined.max_span, summary.max_span)
        self.assertEqual(total_trees, 29585)
        self.assertEqual(total_halos, 1533122)
        self.assertEqual(combined.non_null_descendant_links, 1495274)
        self.assertEqual(combined.forward_gap_links, 29291)
        self.assertEqual(combined.max_span, 2)


def _write_ctrees_hdf5_fixture(
    path, forests, snap_field="Snap_num", snap_dtype="i8", extra_group=None
):
    """Write a minimal single-file forests-HDF5 fixture (no external links):
    `forests` is a list of (descendant, snap) row lists per forest, packed
    contiguously with a matching ForestInfo table."""
    offsets = []
    counts = []
    offset = 0
    all_desc = []
    all_snap = []
    for rows in forests:
        counts.append(len(rows))
        offsets.append(offset)
        offset += len(rows)
        for desc, snap in rows:
            all_desc.append(desc)
            all_snap.append(snap)

    forest_info_dtype = np.dtype(
        [
            ("ForestID", "<i8"),
            ("ForestHalosOffset", "<i8"),
            ("ForestNhalos", "<i8"),
            ("ForestNtrees", "<i8"),
        ]
    )
    forest_info = np.zeros(len(forests), dtype=forest_info_dtype)
    forest_info["ForestID"] = np.arange(len(forests))
    forest_info["ForestHalosOffset"] = offsets
    forest_info["ForestNhalos"] = counts
    forest_info["ForestNtrees"] = 1

    with h5py.File(path, "w") as f:
        f.attrs["Nfiles"] = 1
        f.attrs["TotNforests"] = len(forests)
        group = f.create_group("File0")
        group.create_dataset("ForestInfo", data=forest_info)
        forests_group = group.create_group("Forests")
        forests_group.create_dataset("Descendant", data=np.array(all_desc, dtype="<i8"))
        forests_group.create_dataset(snap_field, data=np.array(all_snap, dtype=snap_dtype))
        if extra_group == "vector_field":
            forests_group.create_dataset("Mvir", data=np.ones(len(all_desc), dtype="<f8"))


class CtreesHDF5InspectionTests(unittest.TestCase):
    def test_valid_fixture_snap_num_int(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "info.h5"
            _write_ctrees_hdf5_fixture(
                path,
                forests=[[(1, 0), (-1, 2)], [(-1, 1)]],
                snap_field="Snap_num",
            )
            root_attrs, files = si.inspect_ctrees_hdf5_source(path)
            self.assertEqual(root_attrs["TotNforests"], 2)
            self.assertEqual(len(files), 1)
            f0 = files[0]
            self.assertTrue(f0.reachable)
            self.assertEqual(f0.n_forests, 2)
            self.assertEqual(f0.n_halos, 3)
            self.assertEqual(f0.link_summary.non_null_descendant_links, 1)
            self.assertEqual(f0.link_summary.forward_gap_links, 1)
            self.assertEqual(f0.link_summary.max_span, 2)

    def test_valid_fixture_snap_idx_float(self):
        # Newer forests-HDF5 packages spell the snapshot column Snap_idx and
        # store it as float64 (src/io/vertical/read_ctrees_hdf5.c:178-179).
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "info.h5"
            _write_ctrees_hdf5_fixture(
                path,
                forests=[[(1, 0), (-1, 1)]],
                snap_field="Snap_idx",
                snap_dtype="<f8",
            )
            root_attrs, files = si.inspect_ctrees_hdf5_source(path)
            f0 = files[0]
            self.assertEqual(f0.link_summary.non_null_descendant_links, 1)
            self.assertEqual(f0.link_summary.forward_adjacent_links, 1)

    def test_non_integral_snap_idx_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "info.h5"
            _write_ctrees_hdf5_fixture(
                path, forests=[[(-1, 0)]], snap_field="Snap_idx", snap_dtype="<f8"
            )
            with h5py.File(path, "r+") as f:
                f["File0/Forests/Snap_idx"][0] = 0.5
            with self.assertRaises(ConverterError):
                si.inspect_ctrees_hdf5_source(path)

    def test_missing_snap_field_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "info.h5"
            with h5py.File(path, "w") as f:
                f.attrs["Nfiles"] = 1
                f.attrs["TotNforests"] = 1
                group = f.create_group("File0")
                forest_info_dtype = np.dtype(
                    [
                        ("ForestID", "<i8"),
                        ("ForestHalosOffset", "<i8"),
                        ("ForestNhalos", "<i8"),
                        ("ForestNtrees", "<i8"),
                    ]
                )
                info = np.zeros(1, dtype=forest_info_dtype)
                info["ForestNhalos"] = 1
                group.create_dataset("ForestInfo", data=info)
                fg = group.create_group("Forests")
                fg.create_dataset("Descendant", data=np.array([-1], dtype="<i8"))
            with self.assertRaises(ConverterError):
                si.inspect_ctrees_hdf5_source(path)

    def test_out_of_forest_descendant_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "info.h5"
            _write_ctrees_hdf5_fixture(path, forests=[[(5, 0)]], snap_field="Snap_num")
            with self.assertRaises(ConverterError):
                si.inspect_ctrees_hdf5_source(path)

    def test_missing_info_file_fails(self):
        with self.assertRaises(ConverterError):
            si.inspect_ctrees_hdf5_source("/nonexistent/path/info.h5")

    def test_missing_h5py_dependency_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "info.h5"
            _write_ctrees_hdf5_fixture(path, forests=[[(-1, 0)]])
            with mock.patch.object(si, "h5py", None):
                with self.assertRaises(si.MissingDependencyError):
                    si.inspect_ctrees_hdf5_source(path)

    def test_real_full_uchuu_fixture_is_reachable_and_inspectable(self):
        repo_root = Path(__file__).parents[3]
        fixture = repo_root / "simulations" / "uchuu" / "_tests" / "data" / "mergertree_info.h5"
        if not fixture.exists():
            self.skipTest("full-Uchuu external-link fixture not present")
        root_attrs, files = si.inspect_ctrees_hdf5_source(fixture)
        self.assertEqual(root_attrs["TotNforests"], 3)
        self.assertEqual(len(files), 1)
        self.assertEqual(files[0].link_type, "ExternalLink")
        self.assertTrue(files[0].reachable)
        self.assertEqual(files[0].n_forests, 3)

    def test_real_micro_uchuu_hdf5_reproduces_known_totals(self):
        repo_root = Path(__file__).parents[3]
        info_path = (
            repo_root
            / "simulations"
            / "micro-uchuu-hdf5"
            / "snapshots"
            / "MicroUchuu_mergertree_info.h5"
        )
        if not info_path.exists():
            self.skipTest("real micro-Uchuu forests-HDF5 dataset not present locally")
        root_attrs, files = si.inspect_ctrees_hdf5_source(info_path)
        self.assertEqual(root_attrs["TotNhalos"], 22580924)
        self.assertEqual(root_attrs["TotNforests"], 440651)
        self.assertEqual(files[0].n_forests, 440651)
        self.assertEqual(files[0].n_halos, 22580924)


class ReachabilityTests(unittest.TestCase):
    def _write_sim_info(self, tmp, **overrides):
        data = {
            "input": {
                "first_file": 0,
                "last_file": 1,
                "tree_name": "trees_test",
                "tree_type": "lhalo_binary",
                "simulation_dir": str(tmp / "snapshots"),
                "snapshot_list_file": str(tmp / "a_list"),
            }
        }
        data["input"].update(overrides)
        path = tmp / "simulation_info.yaml"
        with open(path, "w") as f:
            yaml.safe_dump(data, f)
        return path

    def test_lhalo_reachability_partial_present(self):
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            snap_dir = tmp / "snapshots"
            snap_dir.mkdir()
            (snap_dir / "trees_test.0").write_bytes(b"x")
            sim_info_path = self._write_sim_info(tmp, last_file=2)
            sim_info = si.load_simulation_info(sim_info_path)
            reach = si.check_lhalo_reachability(sim_info)
            self.assertTrue(reach.exists)
            self.assertEqual(reach.present_file_count, 1)
            self.assertEqual(reach.declared_file_count, 3)
            self.assertTrue(reach.notes)

    def test_lhalo_reachability_absent_directory_not_dead_symlink(self):
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            sim_info_path = self._write_sim_info(tmp, simulation_dir=str(tmp / "does-not-exist"))
            sim_info = si.load_simulation_info(sim_info_path)
            reach = si.check_lhalo_reachability(sim_info)
            self.assertFalse(reach.exists)
            self.assertEqual(reach.present_file_count, 0)
            self.assertIn("simulation_dir does not exist", reach.notes)

    def test_load_simulation_info_missing_key_fails(self):
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            path = tmp / "simulation_info.yaml"
            with open(path, "w") as f:
                yaml.safe_dump({"input": {"first_file": 0}}, f)
            with self.assertRaises(ConverterError):
                si.load_simulation_info(path)

    def test_real_uchuu_snapshots_absent(self):
        repo_root = Path(__file__).parents[3]
        self.assertFalse((repo_root / "simulations" / "uchuu" / "snapshots").exists())
        sim_info = si.load_simulation_info(
            repo_root / "simulations" / "uchuu" / "simulation_info.yaml"
        )
        reach = si.check_hdf5_reachability(sim_info)
        self.assertFalse(reach.exists)
        self.assertIn("simulation_dir does not exist", reach.notes)


class InspectSourcesCLITests(unittest.TestCase):
    def test_source_format_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            snap_dir = tmp / "snapshots"
            snap_dir.mkdir()
            sim_info_path = tmp / "simulation_info.yaml"
            with open(sim_info_path, "w") as f:
                yaml.safe_dump(
                    {
                        "input": {
                            "first_file": 0,
                            "last_file": 0,
                            "tree_name": "trees_test",
                            "tree_type": "lhalo_binary",
                            "simulation_dir": str(snap_dir),
                            "snapshot_list_file": str(tmp / "a_list"),
                        }
                    },
                    f,
                )
            with self.assertRaises(ConverterError):
                inspect_sources.inspect_one(
                    "consistent_trees_hdf5", sim_info_path, tmp / "a_list", "little", True
                )

    def test_survey_declares_route_for_all_five_named_packages(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = inspect_sources.main(["survey", "--reachability-only"])
        self.assertEqual(rc, 0)
        self.assertEqual(
            set(inspect_sources.NAMED_PACKAGES),
            {
                "mini-millennium",
                "millennium",
                "micro-uchuu",
                "mini-uchuu",
                "uchuu",
            },
        )
        for name, adapter in (
            ("mini-millennium", "lhalo_binary"),
            ("millennium", "lhalo_binary"),
            ("micro-uchuu", "lhalo_binary"),
            ("mini-uchuu", "lhalo_binary"),
            ("uchuu", "ctrees_hdf5"),
        ):
            self.assertEqual(
                inspect_sources.ADAPTER_ROUTES.get(
                    si.load_simulation_info(
                        Path(inspect_sources._repo_root()) / inspect_sources.NAMED_PACKAGES[name]
                    ).tree_type
                ),
                adapter,
            )

    def test_inspect_mini_millennium_matches_measured_totals(self):
        repo_root = inspect_sources._repo_root()
        sim_info_path = repo_root / "simulations" / "mini-millennium" / "simulation_info.yaml"
        if not (
            repo_root / "simulations" / "mini-millennium" / "snapshots" / "trees_063.0"
        ).exists():
            self.skipTest("real mini-Millennium data not present locally")
        report = inspect_sources.inspect_one(
            "lhalo_binary",
            sim_info_path,
            repo_root / "simulations" / "mini-millennium" / "mini-millennium.a_list",
            "little",
            True,
        )
        self.assertEqual(report["total_trees"], 29585)
        self.assertEqual(report["total_halos"], 1533122)
        self.assertEqual(report["combined_link_summary"]["non_null_descendant_links"], 1495274)
        self.assertEqual(report["combined_link_summary"]["forward_gap_links"], 29291)
        self.assertEqual(report["combined_link_summary"]["max_span"], 2)
        self.assertEqual(len(report["files"]), 8)


if __name__ == "__main__":
    unittest.main()
