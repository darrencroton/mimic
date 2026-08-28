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
from fixups import run_fixups  # noqa: E402
from hdf5_writer import run_write  # noqa: E402
from links import run_links  # noqa: E402
from scatter import Manifest, run_scatter, snapshot_scratch_name  # noqa: E402
from sort_index import index_name, run_sort, sorted_scratch_name  # noqa: E402
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
    """Plan Slice 8: deletion is bounded by re-run reachability. Once a later
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

    def _linked_workdir(self, name):
        """A workdir carried all the way to ``linked`` with consumption on, so
        every artifact sort skip-trusts has genuinely been deleted."""
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
        run_fixups(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=True,
        )
        run_links(workdir, consume_intermediates=True)
        return workdir, a_list, sim_info

    def test_rerun_after_a_consuming_links_run_is_a_skip_not_a_status_error(self):
        """Once ``links`` has run, every snapshot sits at ``linked`` and both of
        sort's own outputs are consumed. That used to abort with "unexpected
        status 'linked'; run scatter first" — advice that would itself have
        failed. It must be a skip naming what was consumed."""
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

    def test_flag_off_linked_status_still_refuses(self):
        """The ``linked`` skip exists only to keep a CONSUMED snapshot
        resumable. On a pipeline run entirely with the flag off, nothing was
        consumed and every artifact is on disk, so this stage must refuse a
        ``linked`` snapshot exactly as it did before the slice — that is the
        flag-off behaviour the contract says must not change."""
        root = self.root / "flag-off-linked"
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
        for entry in manifest.data["snapshots"].values():
            self.assertEqual("linked", entry["status"])
            for key in ("sorted_file", "index_file", "fixed_file", "links_file"):
                self.assertTrue(Path(entry[key]).exists())
        with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
            run_sort(workdir)
        with self.assertRaisesRegex(ConverterError, r"unexpected status 'linked'"):
            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)

    def _pipeline(self, name, fixups_on, links_on, write_on):
        """Run the whole pipeline to ``linked`` (and through ``write`` when it
        is enabled) with ``--consume-intermediates`` set independently at each
        stage."""
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
        run_fixups(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=fixups_on,
        )
        run_links(workdir, consume_intermediates=links_on)
        run_write(
            workdir,
            a_list_path=a_list,
            simulation_info_path=sim_info,
            consume_intermediates=write_on,
        )
        return workdir, a_list, sim_info

    def test_the_linked_gate_over_every_flag_combination(self):
        """The gate's complete truth table, not a sample of it.

        ``--consume-intermediates`` is an independent per-invocation flag on
        three stages, so there are eight ways an operator can set it, and the
        gate has to be right in all eight. Each stage marks a different manifest
        key — fixups takes ``sorted_file``, links takes every ``index_file``,
        the writer takes ``fixed_file`` and ``links_file`` — so a gate missing
        any key would still look correct in the combinations where some other
        stage happened to be on. With every flag off nothing is consumed and
        both stages must refuse a ``linked`` snapshot, exactly as they did
        before this slice; with any flag on both must skip, and sort's message
        must name precisely the artifacts that were actually taken.
        """
        phrases = {
            "sorted": "sorted snapshot scratch was consumed",
            "index": "snapshot id index was consumed",
            "fixed": "fixed snapshot scratch was consumed",
            "links": "snapshot links scratch was consumed",
        }
        for fixups_on in (False, True):
            for links_on in (False, True):
                for write_on in (False, True):
                    name = "gate-{:d}{:d}{:d}".format(fixups_on, links_on, write_on)
                    with self.subTest(fixups=fixups_on, links=links_on, write=write_on):
                        workdir, a_list, sim_info = self._pipeline(
                            name, fixups_on, links_on, write_on
                        )
                        manifest = Manifest.load_or_create(workdir)
                        for entry in manifest.data["snapshots"].values():
                            self.assertEqual("linked", entry["status"])

                        if not (fixups_on or links_on or write_on):
                            with self.assertRaisesRegex(
                                ConverterError, r"unexpected status 'linked'"
                            ):
                                run_sort(workdir)
                            with self.assertRaisesRegex(
                                ConverterError, r"unexpected status 'linked'"
                            ):
                                run_fixups(
                                    workdir,
                                    a_list_path=a_list,
                                    simulation_info_path=sim_info,
                                )
                            continue

                        expected = set()
                        if fixups_on:
                            expected.add("sorted")
                        if links_on:
                            expected.add("index")
                        if write_on:
                            expected |= {"fixed", "links"}
                        with capture_stderr() as captured:
                            run_sort(workdir)
                        for key, phrase in phrases.items():
                            if key in expected:
                                self.assertIn(phrase, captured.text)
                            else:
                                self.assertNotIn(phrase, captured.text)
                        # fixups skips too; it examines only the writer's two
                        # artifacts, so it names them only when write consumed
                        with capture_stderr() as captured:
                            run_fixups(workdir, a_list_path=a_list, simulation_info_path=sim_info)
                        for key in ("fixed", "links"):
                            if write_on:
                                self.assertIn(phrases[key], captured.text)
                            else:
                                self.assertNotIn(phrases[key], captured.text)

    def test_consumed_fixed_and_links_files_are_accepted_only_at_linked(self):
        """At ``linked`` the writer may have taken the fixed and links files, so
        both are accepted as consumed there."""
        workdir, a_list, sim_info = self._linked_workdir("linked-writer")
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
        """The permissive treatment must NOT reach ``fixed``. The writer is the
        fixed file's terminal consumer and runs only once every snapshot is
        linked, so at ``fixed`` that file must be present — accepting a
        consumption here would let the one deletion this slice must never make
        pass as a resumable skip."""
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


if __name__ == "__main__":
    unittest.main()
