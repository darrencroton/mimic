"""Unit tests: sort order, duplicate-id abort, verify-then-delete.

Every stage runs under a profile schema: the shipped default profile unless a
test names another. ``TestSortWithDeclaredExtras`` checks that declared extras
travel with their id through the sort, and that a scratch file of a different
layout is refused."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    capture_stderr,
    default_layout,
    default_schema,
    delete_through_the_guard,
)
from fixups import run_fixups  # noqa: E402
from links import run_links  # noqa: E402
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
        schema=default_schema(),
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
            data = np.fromfile(scratch / sorted_scratch_name(snap), dtype=default_layout().dtype)
            self.assertEqual(len(data), entry["rows"])
            self.assertTrue(np.all(np.diff(data["id"]) > 0))  # strictly ascending, unique
            index_ids = np.fromfile(scratch / index_name(snap), dtype=np.int64)
            np.testing.assert_array_equal(index_ids, data["id"])
            # verify-then-delete: unsorted scratch gone, and gone via the manifest
            unsorted = scratch / snapshot_scratch_name(snap)
            self.assertFalse(unsorted.exists())
            status = manifest.data["intermediates"][str(unsorted.resolve())]["status"]
            self.assertEqual(status, "removed")
            # every scratch-file entry carries the layout's dtype tag
            sorted_entry = manifest.data["intermediates"][
                str((scratch / sorted_scratch_name(snap)).resolve())
            ]
            self.assertEqual(sorted_entry["dtype_tag"], default_layout().dtype_tag)
            idx_entry = manifest.data["intermediates"][str((scratch / index_name(snap)).resolve())]
            self.assertEqual(idx_entry["dtype_tag"], "<i8")

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
        manifest = Manifest.load(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        manifest.data["snapshots"][snap]["id_checksum"] ^= 0xDEAD
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_sort(workdir)
        unsorted = Path(manifest.data["snapshots"][snap]["scratch_file"])
        self.assertTrue(unsorted.exists())

    def test_sort_preserves_record_bytes(self):
        workdir = make_scattered_workdir(self.root)
        manifest = Manifest.load(workdir)
        expected = {}
        for snap_str, entry in manifest.data["snapshots"].items():
            arr = np.fromfile(entry["scratch_file"], dtype=default_layout().dtype)
            expected[snap_str] = arr[np.argsort(arr["id"], kind="stable")].tobytes()
        run_sort(workdir)
        for snap_str, want in expected.items():
            got = (workdir / "scratch" / sorted_scratch_name(int(snap_str))).read_bytes()
            self.assertEqual(got, want)

    def test_tampered_unsorted_input_aborts(self):
        workdir = make_scattered_workdir(self.root)
        manifest = Manifest.load(workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        target = Path(manifest.data["snapshots"][snap]["scratch_file"])
        data = bytearray(target.read_bytes())
        data[-1] ^= 0xFF  # last field is forest_id: invisible to the id checksum
        target.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_sort(workdir)

    def test_sort_before_scatter_aborts(self):
        workdir = self.root / "empty-workdir"
        workdir.mkdir()
        with self.assertRaisesRegex(ConverterError, "no manifest"):
            run_sort(workdir)


class TestSortSkipTrust(unittest.TestCase):
    """A re-run over a snapshot this stage or the fix-up stage already carried
    forward verifies every artifact that exists at its status outright: one
    that is missing, tampered with, or recorded as removed by the manifest is
    a hard failure, never a skip. A ``linked`` snapshot is refused."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def _workdir_at(self, status, name):
        """A workdir whose snapshots are at ``sorted`` or ``fixed``."""
        if status == "fixed":
            return self._workdir_through(name)[0]
        root = self.root / name
        root.mkdir()
        workdir = make_scattered_workdir(root)
        run_sort(workdir)
        return workdir

    def _workdir_through(self, name, link=False):
        """A workdir carried to ``fixed`` (or to ``linked`` with ``link``)."""
        root = self.root / name
        root.mkdir()
        forests = fixtures.standard_forests()
        tree_file = fixtures.write_ctrees_file(root / "tree_0.dat", fixtures.all_trees(forests))
        forests_list = fixtures.write_forests_list(root / "forests.list", forests)
        a_list = fixtures.write_a_list(root / "test.a_list")
        sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")
        workdir = root / "workdir"
        run_scatter(
            tree_files=[tree_file],
            forests_list_path=forests_list,
            a_list_path=a_list,
            workdir=workdir,
            simulation_info_path=sim_info,
            schema=default_schema(),
        )
        run_sort(workdir)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        if link:
            run_links(workdir)
        return workdir, a_list, sim_info

    def _record_removed(self, workdir, keys, snap="5"):
        """Delete snapshot ``snap``'s ``keys`` artifacts through the manifest's
        guard, so the manifest records each one as removed."""
        manifest = Manifest.load(workdir)
        for key in keys:
            path = manifest.data["snapshots"][snap][key]
            delete_through_the_guard(manifest, path)
            self.assertFalse(Path(path).exists())

    def test_a_missing_or_tampered_artifact_is_a_hard_failure(self):
        """Re-running sort refuses a sorted or index file that is missing or has a
        flipped byte, and at ``fixed`` a missing fixed file, which the link stage
        still has to read."""
        cases = [
            ("sorted", "sorted_file", "missing"),
            ("sorted", "index_file", "missing"),
            ("sorted", "sorted_file", "tampered"),
            ("sorted", "index_file", "tampered"),
            ("fixed", "fixed_file", "missing"),
        ]
        for status, key, damage in cases:
            with self.subTest(status=status, key=key, damage=damage):
                workdir = self._workdir_at(status, "{}-{}-{}".format(status, key, damage))
                manifest = Manifest.load(workdir)
                snap = sorted(manifest.data["snapshots"], key=int)[0]
                self.assertEqual(status, manifest.data["snapshots"][snap]["status"])
                target = Path(manifest.data["snapshots"][snap][key])
                if damage == "missing":
                    target.unlink()
                    message = "missing on disk"
                else:
                    data = bytearray(target.read_bytes())
                    data[-1] ^= 0xFF
                    target.write_bytes(bytes(data))
                    message = "checksum"
                with self.assertRaisesRegex(ConverterError, message):
                    run_sort(workdir)

    def test_an_artifact_recorded_removed_is_refused_at_sorted_and_fixed(self):
        """Sort's own outputs, and at ``fixed`` the fixed file too, must still
        be on disk: a manifest recording one as removed describes a deletion
        no stage makes, and is refused rather than skipped."""
        cases = [
            ("sorted", "sorted_file"),
            ("sorted", "index_file"),
            ("fixed", "sorted_file"),
            ("fixed", "index_file"),
            ("fixed", "fixed_file"),
        ]
        for status, key in cases:
            with self.subTest(status=status, key=key):
                workdir = self._workdir_at(status, "{}-{}".format(status, key))
                manifest = Manifest.load(workdir)
                self.assertEqual(status, manifest.data["snapshots"]["5"]["status"])
                self._record_removed(workdir, [key])
                with self.assertRaisesRegex(ConverterError, "not a manifest-owned intermediate"):
                    run_sort(workdir)

    def test_a_fixed_rerun_verifies_and_keeps_every_artifact(self):
        workdir, _, _ = self._workdir_through("fixed-rerun")
        run_sort(workdir)
        manifest = Manifest.load(workdir)
        for entry in manifest.data["snapshots"].values():
            self.assertEqual("fixed", entry["status"])
            for key in ("sorted_file", "index_file", "fixed_file"):
                self.assertTrue(Path(entry[key]).exists())

    def test_a_linked_snapshot_is_refused_by_sort_and_fix_ups(self):
        """Once a snapshot is linked only the link stage is re-run. Sort and
        the fix-up stage refuse a ``linked`` snapshot whether every artifact is
        on disk or the manifest records them as removed. The removal is
        recorded on every snapshot, so the first snapshot either stage visits
        already has its artifacts gone: skip-trusting it instead of refusing
        would let the stage return without raising."""
        for name, removed in (("linked-intact", ()), ("linked-removed", LINKED_ARTIFACTS)):
            with self.subTest(removed=removed):
                workdir, a_list, sim_info = self._workdir_through(name, link=True)
                manifest = Manifest.load(workdir)
                for entry in manifest.data["snapshots"].values():
                    self.assertEqual("linked", entry["status"])
                if removed:
                    for snap in manifest.data["snapshots"]:
                        self._record_removed(workdir, removed, snap=snap)
                with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
                    run_sort(workdir)
                with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
                    run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)


#: Every per-snapshot artifact a ``linked`` snapshot records.
LINKED_ARTIFACTS = ("sorted_file", "index_file", "fixed_file", "links_file")


class TestSortWithDeclaredExtras(unittest.TestCase):
    def setUp(self):
        from column_schema import build_schema, load_column_map

        profile = Path(__file__).parent / "data" / "column_maps" / "ascii_all_extra_types.yaml"
        self.schema = build_schema(load_column_map(profile))
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        forests = fixtures.standard_forests()
        self.tree = fixtures.write_ctrees_file(root / "t.dat", fixtures.all_trees(forests))
        self.forests_list = fixtures.write_forests_list(root / "forests.list", forests)
        self.a_list = fixtures.write_a_list(root / "a")
        self.workdir = root / "w"
        with capture_stderr():
            self.manifest = run_scatter(
                [self.tree],
                self.forests_list,
                self.a_list,
                self.workdir,
                chunksize=2,
                schema=self.schema,
            )

    def tearDown(self):
        self.tmp.cleanup()

    def test_extras_travel_with_their_id(self):
        layout = self.manifest.layout
        before = {}
        for entry in self.manifest.data["snapshots"].values():
            for row in np.fromfile(entry["scratch_file"], dtype=layout.dtype):
                before[int(row["id"])] = row.tobytes()
        with capture_stderr():
            manifest = run_sort(self.workdir)
        after = {}
        for entry in manifest.data["snapshots"].values():
            meta = manifest.data["intermediates"][entry["sorted_file"]]
            self.assertEqual(meta["dtype_tag"], layout.dtype_tag)
            records = np.fromfile(entry["sorted_file"], dtype=layout.dtype)
            self.assertTrue(np.all(np.diff(records["id"]) > 0))
            for row in records:
                after[int(row["id"])] = row.tobytes()
        self.assertEqual(after, before)

    def test_scratch_of_another_layout_is_refused(self):
        entry = self.manifest.data["snapshots"]["5"]
        intermediate = self.manifest.data["intermediates"][entry["scratch_file"]]
        intermediate["dtype_tag"] = default_layout().dtype_tag
        self.manifest.save()
        with self.assertRaisesRegex(ConverterError, "refusing to sort"):
            with capture_stderr():
                run_sort(self.workdir, snapshots=[5])


if __name__ == "__main__":
    unittest.main()
