"""Unit tests: sort determinism, duplicate-id abort, verify-then-delete.

``TestExtendedSort`` checks that extended records (source coordinates and
declared extras) travel with their id through the sort, and that a scratch file
of a different layout is refused."""

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
from fixups import run_fixups  # noqa: E402
from links import run_links  # noqa: E402
from scatter import Manifest, run_scatter, snapshot_scratch_name  # noqa: E402
from sort_index import (  # noqa: E402
    CONSUMABLE_KEYS,
    index_name,
    run_sort,
    sorted_scratch_name,
)
from test_fixups import capture_stderr  # noqa: E402


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


class TestSortSkipsConsumedArtifacts(unittest.TestCase):
    """deletion is bounded by re-run reachability. Once a later
    stage has consumed an artifact this stage's skip-trust path verifies, that
    path has to skip and name what was consumed — not fail on a stat or a
    checksum on a file the pipeline deliberately deleted."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def _sorted_workdir(self):
        workdir = make_scattered_workdir(self.root)
        run_sort(workdir)
        return workdir

    def _fixed_workdir(self, name):
        """A workdir at status ``fixed`` — the earliest status at which either
        of this stage's outputs can legitimately have been consumed."""
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
        )
        run_sort(workdir)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        return workdir

    def _consume(self, workdir, key):
        """Consume one snapshot-5 artifact exactly as a later stage would."""
        manifest = Manifest.load_or_create(workdir)
        path = manifest.data["snapshots"]["5"][key]
        self.assertEqual([Path(path)], manifest.consume_intermediates([path], delete=True))
        self.assertFalse(Path(path).exists())
        return path

    def test_consumed_sorted_file_is_a_skip_naming_it(self):
        workdir = self._fixed_workdir("consumed-sorted")
        path = self._consume(workdir, "sorted_file")
        with capture_stderr() as captured:
            run_sort(workdir)
        self.assertIn("snapshot 5 is already fixed", captured.text)
        self.assertIn("sorted snapshot scratch was consumed", captured.text)
        self.assertIn(path, captured.text)

    def test_consumed_index_file_is_a_skip_naming_it(self):
        workdir = self._fixed_workdir("consumed-index")
        path = self._consume(workdir, "index_file")
        with capture_stderr() as captured:
            run_sort(workdir)
        self.assertIn("snapshot id index was consumed", captured.text)
        self.assertIn(path, captured.text)

    def test_a_merely_missing_artifact_is_still_a_hard_failure(self):
        """The skip is granted only to a RECORDED consumption: a sorted file
        that simply disappeared is the failure it has always been."""
        workdir = self._sorted_workdir()
        manifest = Manifest.load_or_create(workdir)
        Path(manifest.data["snapshots"]["5"]["sorted_file"]).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_sort(workdir)

    def _linked_workdir(self, name, consumed_keys=("sorted_file", "index_file")):
        """A workdir carried all the way to ``linked``, then with every
        snapshot's ``consumed_keys`` artifacts consumed through the manifest's
        guard, so every artifact sort skip-trusts under those keys has
        genuinely been deleted."""
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
        )
        run_sort(workdir)
        run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
        run_links(workdir)
        manifest = Manifest.load_or_create(workdir)
        paths = [
            entry[key] for entry in manifest.data["snapshots"].values() for key in consumed_keys
        ]
        self.assertEqual(
            [Path(p) for p in paths], manifest.consume_intermediates(paths, delete=True)
        )
        return workdir, a_list, sim_info

    def test_rerun_after_linking_with_consumed_outputs_is_a_skip_not_a_status_error(self):
        """Once every snapshot sits at ``linked`` with both of sort's own
        outputs consumed, a re-run must not abort with "unexpected status
        'linked'; run scatter first" -- advice that would itself fail. It must
        be a skip naming what was consumed."""
        workdir, _, _ = self._linked_workdir("linked-skip")
        manifest = Manifest.load_or_create(workdir)
        for entry in manifest.data["snapshots"].values():
            self.assertEqual("linked", entry["status"])
        with capture_stderr() as captured:
            run_sort(workdir)
        for snap, entry in sorted(manifest.data["snapshots"].items(), key=lambda kv: int(kv[0])):
            self.assertIn("sort: snapshot {} is already linked".format(snap), captured.text)
            self.assertIn(entry["sorted_file"], captured.text)
            self.assertIn(entry["index_file"], captured.text)

    def test_linked_status_with_nothing_consumed_refuses(self):
        """The ``linked`` skip exists only to keep a snapshot with consumed
        artifacts resumable. With nothing consumed and every artifact on disk,
        sort refuses a ``linked`` snapshot, and so does the fix-up stage."""
        workdir, a_list, sim_info = self._linked_workdir("nothing-consumed", consumed_keys=())
        manifest = Manifest.load_or_create(workdir)
        for entry in manifest.data["snapshots"].values():
            self.assertEqual("linked", entry["status"])
            for key in ("sorted_file", "index_file", "fixed_file", "links_file"):
                self.assertTrue(Path(entry[key]).exists())
        with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
            run_sort(workdir)
        with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_the_linked_gate_over_every_consumed_key_combination(self):
        """The gate's complete truth table, not a sample of it.

        The gate (``sort_index.consumption_recorded``) reads four manifest
        keys -- ``sorted_file``, ``index_file``, ``fixed_file`` and
        ``links_file`` -- so a gate missing any key would still look correct in
        the combinations where another key happened to be consumed. With
        nothing consumed sort must refuse a ``linked`` snapshot; with any key
        consumed it must skip, and its message must name precisely the
        artifacts that were actually taken. The fix-up stage never skips a
        ``linked`` snapshot, whatever was consumed.
        """
        phrases = {
            "sorted_file": "sorted snapshot scratch was consumed",
            "index_file": "snapshot id index was consumed",
            "fixed_file": "fixed snapshot scratch was consumed",
            "links_file": "snapshot links scratch was consumed",
        }
        for mask in range(1 << len(CONSUMABLE_KEYS)):
            keys = tuple(key for bit, key in enumerate(CONSUMABLE_KEYS) if mask & (1 << bit))
            with self.subTest(consumed=keys):
                workdir, a_list, sim_info = self._linked_workdir(
                    "gate-{:02d}".format(mask), consumed_keys=keys
                )
                if not keys:
                    with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
                        run_sort(workdir)
                else:
                    with capture_stderr() as captured:
                        run_sort(workdir)
                    for key, phrase in phrases.items():
                        if key in keys:
                            self.assertIn(phrase, captured.text)
                        else:
                            self.assertNotIn(phrase, captured.text)
                with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
                    run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def test_consumed_fixed_and_links_files_are_accepted_only_at_linked(self):
        """At ``linked`` the fixed and links files are past every stage that
        reads them, so both are accepted as consumed there."""
        workdir, a_list, sim_info = self._linked_workdir("linked-fixed-and-links")
        manifest = Manifest.load_or_create(workdir)
        entry = manifest.data["snapshots"]["5"]
        self.assertEqual(
            [Path(entry["fixed_file"]), Path(entry["links_file"])],
            manifest.consume_intermediates([entry["fixed_file"], entry["links_file"]], delete=True),
        )
        with capture_stderr() as captured:
            run_sort(workdir)
        self.assertIn("fixed snapshot scratch was consumed", captured.text)
        self.assertIn("snapshot links scratch was consumed", captured.text)

    def test_a_missing_fixed_file_at_status_fixed_is_still_a_hard_error(self):
        """The permissive treatment must NOT reach ``fixed``: the link stage
        still has to read the fixed file, so at ``fixed`` it must be present —
        accepting a consumption here would let a premature deletion pass as a
        resumable skip."""
        workdir = make_scattered_workdir(self.root)
        run_sort(workdir)
        run_fixups(
            workdir,
            a_list_path=self.root / "test.a_list",
            simulation_info_path=fixtures.write_simulation_info(self.root / "simulation_info.yaml"),
        )
        manifest = Manifest.load_or_create(workdir)
        self.assertEqual("fixed", manifest.data["snapshots"]["5"]["status"])
        Path(manifest.data["snapshots"]["5"]["fixed_file"]).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_sort(workdir)

    def test_a_consumed_fixed_file_at_status_fixed_is_still_a_hard_error(self):
        """Same boundary, reached the other way: even a manifest that RECORDS
        the fixed file as removed must not buy a skip at ``fixed``."""
        workdir = make_scattered_workdir(self.root)
        run_sort(workdir)
        run_fixups(
            workdir,
            a_list_path=self.root / "test.a_list",
            simulation_info_path=fixtures.write_simulation_info(self.root / "simulation_info.yaml"),
        )
        manifest = Manifest.load_or_create(workdir)
        entry = manifest.data["snapshots"]["5"]
        manifest.consume_intermediates([entry["fixed_file"]], delete=True)
        with self.assertRaisesRegex(ConverterError, "not a manifest-owned intermediate"):
            run_sort(workdir)

    def test_consumption_at_status_sorted_is_refused_not_skipped(self):
        """The rule the other way round: an artifact is accepted as consumed
        only where its consumer has provably run. At ``sorted`` neither of this
        stage's outputs can have been taken — fixups saves ``fixed`` before it
        removes the sorted file, and links refuses to start until every snapshot
        is at least ``fixed`` — so a manifest claiming otherwise describes a
        premature deletion and must be refused, not skipped."""
        for key in ("sorted_file", "index_file"):
            with self.subTest(key=key):
                root = self.root / key
                root.mkdir()
                workdir = make_scattered_workdir(root)
                run_sort(workdir)
                manifest = Manifest.load_or_create(workdir)
                manifest.consume_intermediates([manifest.data["snapshots"]["5"][key]], delete=True)
                with self.assertRaisesRegex(ConverterError, "not a manifest-owned intermediate"):
                    run_sort(workdir)

    def test_tampered_artifact_is_still_refused(self):
        workdir = self._sorted_workdir()
        manifest = Manifest.load_or_create(workdir)
        path = Path(manifest.data["snapshots"]["5"]["sorted_file"])
        with open(path, "r+b") as handle:
            handle.write(b"\x00\x01\x02\x03")
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            run_sort(workdir)


class TestExtendedSort(unittest.TestCase):
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

    def test_extras_and_coordinates_travel_with_their_id(self):
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
        self.manifest.data["intermediates"][entry["scratch_file"]]["dtype_tag"] = DTYPE_TAG
        self.manifest.save()
        with self.assertRaisesRegex(ConverterError, "refusing to sort"):
            with capture_stderr():
                run_sort(self.workdir, snapshots=[5])


if __name__ == "__main__":
    unittest.main()
