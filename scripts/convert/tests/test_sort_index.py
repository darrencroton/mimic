"""Slice 4 unit tests: sort determinism, duplicate-id abort, verify-then-delete."""

import hashlib
import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
from ctrees_parser import DTYPE_TAG, RECORD_DTYPE, ConverterError  # noqa: E402
from scatter import Manifest, run_scatter, snapshot_scratch_name  # noqa: E402
from sort_index import index_name, run_sort, sorted_scratch_name  # noqa: E402


def make_scattered_workdir(root: Path, forests=None):
    forests = forests if forests is not None else fixtures.standard_forests()
    tree_file = fixtures.write_ctrees_file(root / "tree_0.dat", fixtures.all_trees(forests))
    forests_list = fixtures.write_forests_list(root / "forests.list", forests)
    a_list = fixtures.write_a_list(root / "test.a_list")
    workdir = root / "workdir"
    run_scatter(
        tree_files=[tree_file],
        forests_list_path=forests_list,
        a_list_path=a_list,
        workdir=workdir,
    )
    return workdir


class TestSortIndex(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_sorted_output_index_and_delete(self):
        workdir = make_scattered_workdir(self.root)
        manifest = run_sort(workdir)
        scratch = workdir / "scratch"
        for snap_str, entry in manifest.data["snapshots"].items():
            snap = int(snap_str)
            self.assertEqual(entry["status"], "sorted")
            data = np.fromfile(scratch / sorted_scratch_name(snap), dtype=RECORD_DTYPE)
            self.assertEqual(len(data), entry["rows"])
            self.assertTrue(np.all(np.diff(data["id"]) > 0))  # strictly ascending, unique
            index_ids = np.fromfile(scratch / index_name(snap), dtype=np.int64)
            np.testing.assert_array_equal(index_ids, data["id"])
            # verify-then-delete: unsorted scratch gone, and gone via the manifest
            unsorted = scratch / snapshot_scratch_name(snap)
            self.assertFalse(unsorted.exists())
            status = manifest.data["intermediates"][str(unsorted.resolve())]["status"]
            self.assertEqual(status, "removed")
            # every scratch-file entry carries the frozen dtype tag
            sorted_entry = manifest.data["intermediates"][
                str((scratch / sorted_scratch_name(snap)).resolve())
            ]
            self.assertEqual(sorted_entry["dtype_tag"], DTYPE_TAG)
            idx_entry = manifest.data["intermediates"][str((scratch / index_name(snap)).resolve())]
            self.assertEqual(idx_entry["dtype_tag"], "<i8")

    def test_sort_determinism_across_runs(self):
        digests = []
        for name in ("a", "b"):
            root = self.root / name
            root.mkdir()
            workdir = make_scattered_workdir(root)
            manifest = run_sort(workdir)
            digest = hashlib.md5()
            for snap_str in sorted(manifest.data["snapshots"], key=int):
                digest.update(
                    (workdir / "scratch" / sorted_scratch_name(int(snap_str))).read_bytes()
                )
            digests.append(digest.hexdigest())
        self.assertEqual(digests[0], digests[1])

    def test_rerun_is_idempotent(self):
        workdir = make_scattered_workdir(self.root)
        run_sort(workdir)
        manifest = run_sort(workdir)  # all snapshots already sorted: no-op
        for entry in manifest.data["snapshots"].values():
            self.assertEqual(entry["status"], "sorted")

    def test_duplicate_id_aborts_and_keeps_unsorted(self):
        # two trees carrying the same halo id at the same snapshot
        dup = [
            fixtures.ForestSpec(
                forest_id=1,
                trees=[
                    fixtures.TreeSpec(
                        root_id=11, halos=[fixtures.HaloSpec(halo_id=77, snap=5, mvir=1e11)]
                    ),
                    fixtures.TreeSpec(
                        root_id=12, halos=[fixtures.HaloSpec(halo_id=77, snap=5, mvir=2e11)]
                    ),
                ],
            )
        ]
        workdir = make_scattered_workdir(self.root, forests=dup)
        with self.assertRaisesRegex(ConverterError, "duplicate halo id.*77"):
            run_sort(workdir)
        self.assertTrue((workdir / "scratch" / snapshot_scratch_name(5)).exists())

    def test_checksum_mismatch_aborts_and_keeps_unsorted(self):
        workdir = make_scattered_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        manifest.data["snapshots"][snap]["id_checksum"] ^= 0xDEAD
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_sort(workdir)
        unsorted = Path(manifest.data["snapshots"][snap]["scratch_file"])
        self.assertTrue(unsorted.exists())

    def test_sort_preserves_record_bytes(self):
        workdir = make_scattered_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        expected = {}
        for snap_str, entry in manifest.data["snapshots"].items():
            arr = np.fromfile(entry["scratch_file"], dtype=RECORD_DTYPE)
            expected[snap_str] = arr[np.argsort(arr["id"], kind="stable")].tobytes()
        run_sort(workdir)
        for snap_str, want in expected.items():
            got = (workdir / "scratch" / sorted_scratch_name(int(snap_str))).read_bytes()
            self.assertEqual(got, want)

    def test_tampered_unsorted_input_aborts(self):
        workdir = make_scattered_workdir(self.root)
        manifest = Manifest.load_or_create(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        target = Path(manifest.data["snapshots"][snap]["scratch_file"])
        data = bytearray(target.read_bytes())
        data[-1] ^= 0xFF  # last field is forest_id: invisible to the id checksum
        target.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_sort(workdir)

    def test_missing_sorted_artifact_detected_on_rerun(self):
        workdir = make_scattered_workdir(self.root)
        manifest = run_sort(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        Path(manifest.data["snapshots"][snap]["sorted_file"]).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_sort(workdir)

    def test_tampered_sorted_artifact_detected_on_rerun(self):
        workdir = make_scattered_workdir(self.root)
        manifest = run_sort(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        target = Path(manifest.data["snapshots"][snap]["sorted_file"])
        data = bytearray(target.read_bytes())
        data[-1] ^= 0xFF
        target.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_sort(workdir)

    def test_missing_index_artifact_detected_on_rerun(self):
        workdir = make_scattered_workdir(self.root)
        manifest = run_sort(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        Path(manifest.data["snapshots"][snap]["index_file"]).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_sort(workdir)

    def test_tampered_index_artifact_detected_on_rerun(self):
        workdir = make_scattered_workdir(self.root)
        manifest = run_sort(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        target = Path(manifest.data["snapshots"][snap]["index_file"])
        data = bytearray(target.read_bytes())
        data[-1] ^= 0xFF
        target.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_sort(workdir)

    def test_sort_before_scatter_aborts(self):
        workdir = self.root / "empty-workdir"
        workdir.mkdir()
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            run_sort(workdir)


if __name__ == "__main__":
    unittest.main()
