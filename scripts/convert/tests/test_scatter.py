"""Slice 3 unit tests: Phase 0 map, root coverage, ForestIndex order, scatter
conservation, manifest resume, aggregates, cleanup containment guard."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
from ctrees_parser import DTYPE_TAG, RECORD_DTYPE, ConverterError, CtreesFileParser  # noqa: E402
from scatter import (  # noqa: E402
    Manifest,
    load_a_list,
    load_forests_list,
    run_scatter,
    snapshot_scratch_name,
    validate_observed_pairs,
    validate_root_coverage,
    worker_scratch_name,
)


class KilledMidScatter(ConverterError):
    """Sentinel raised by the crashing parser below."""


class CrashAfterFirstChunkParser(CtreesFileParser):
    """Writes exactly one chunk's worth of scatter output, then dies —
    simulating a kill mid-scatter with partial worker binaries on disk."""

    def chunks(self):
        gen = super().chunks()
        yield next(gen)
        raise KilledMidScatter("simulated kill mid-scatter")


class TouchSourceParser(CtreesFileParser):
    """Appends to the source file after the parse completes — simulating a
    source mutated between the pre-scan and the pandas pass."""

    def chunks(self):
        yield from super().chunks()
        with open(self.path, "ab") as handle:
            handle.write(b"# touched after parse\n")


def _flip_last_byte(path: Path) -> None:
    data = bytearray(path.read_bytes())
    data[-1] ^= 0xFF
    path.write_bytes(bytes(data))


class ScatterEnv:
    """One synthetic two-file scatter setup in a temp directory."""

    def __init__(self, root: Path):
        self.root = root
        self.workdir = root / "workdir"
        self.forests = fixtures.standard_forests()
        split = 3  # file 0: forests 100/200/400; file 1: forests 500/600
        self.file_forests = [self.forests[:split], self.forests[split:]]
        self.tree_files = []
        for i, group in enumerate(self.file_forests):
            path = fixtures.write_ctrees_file(
                root / "tree_{}.dat".format(i), fixtures.all_trees(group)
            )
            self.tree_files.append(path)
        self.forests_list = fixtures.write_forests_list(root / "forests.list", self.forests)
        self.a_list = fixtures.write_a_list(root / "test.a_list")
        self.sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")

    def run(self, **kwargs):
        return run_scatter(
            tree_files=self.tree_files,
            forests_list_path=self.forests_list,
            a_list_path=self.a_list,
            workdir=self.workdir,
            simulation_info_path=self.sim_info,
            **kwargs,
        )

    def expected_snapshot_counts(self):
        counts = {}
        for forest in self.forests:
            for tree in forest.trees:
                for halo in tree.halos:
                    counts[halo.snap] = counts.get(halo.snap, 0) + 1
        return counts


class TestForestMap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_map_and_dense_index_order(self):
        forests = [
            fixtures.ForestSpec(forest_id=50, trees=[fixtures.TreeSpec(root_id=7)]),
            fixtures.ForestSpec(forest_id=10, trees=[fixtures.TreeSpec(root_id=3)]),
            fixtures.ForestSpec(
                forest_id=30, trees=[fixtures.TreeSpec(root_id=9), fixtures.TreeSpec(root_id=1)]
            ),
        ]
        path = fixtures.write_forests_list(self.dir / "forests.list", forests)
        forest_map = load_forests_list(path)
        np.testing.assert_array_equal(
            forest_map.lookup_forest_ids(np.array([7, 3, 9, 1])), [50, 10, 30, 30]
        )
        # dense ForestIndex enumeration: ascending ctrees forest id
        np.testing.assert_array_equal(forest_map.forest_index_table(), [10, 30, 50])

    def test_unknown_root_aborts(self):
        path = fixtures.write_forests_list(
            self.dir / "forests.list",
            [fixtures.ForestSpec(forest_id=1, trees=[fixtures.TreeSpec(root_id=5)])],
        )
        forest_map = load_forests_list(path)
        with self.assertRaisesRegex(ConverterError, "not present in forests.list"):
            forest_map.lookup_forest_ids(np.array([6]))

    def test_duplicate_root_in_list_aborts(self):
        path = self.dir / "forests.list"
        path.write_text("#TreeRootID ForestID\n5 1\n5 2\n")
        with self.assertRaisesRegex(ConverterError, "duplicate TreeRootID"):
            load_forests_list(path)

    def test_malformed_list_row_aborts(self):
        path = self.dir / "forests.list"
        path.write_text("#TreeRootID ForestID\n5\n")
        with self.assertRaisesRegex(ConverterError, "TreeRootID ForestID"):
            load_forests_list(path)

    def test_root_coverage_validation(self):
        path = fixtures.write_forests_list(
            self.dir / "forests.list",
            [
                fixtures.ForestSpec(
                    forest_id=1, trees=[fixtures.TreeSpec(root_id=5), fixtures.TreeSpec(root_id=6)]
                )
            ],
        )
        forest_map = load_forests_list(path)
        validate_root_coverage(np.array([5, 6]), forest_map)
        with self.assertRaisesRegex(ConverterError, "missing from forests.list"):
            validate_root_coverage(np.array([5, 6, 7]), forest_map)
        with self.assertRaisesRegex(ConverterError, "never observed"):
            validate_root_coverage(np.array([5]), forest_map)
        with self.assertRaisesRegex(ConverterError, "duplicate #tree root"):
            validate_root_coverage(np.array([5, 5, 6]), forest_map)


class TestALists(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_load_and_validate(self):
        path = fixtures.write_a_list(self.dir / "a_list")
        a_list, md5 = load_a_list(path)
        np.testing.assert_allclose(a_list, fixtures.A_LIST)
        self.assertEqual(len(md5), 32)  # digest of the parsed bytes
        validate_observed_pairs({(0, 0.5), (5, 1.00005)}, a_list, "test")

    def test_unknown_snapshot_aborts(self):
        a_list, _ = load_a_list(fixtures.write_a_list(self.dir / "a_list"))
        with self.assertRaisesRegex(ConverterError, "outside a_list range"):
            validate_observed_pairs({(6, 1.1)}, a_list, "test")

    def test_scale_mismatch_aborts(self):
        a_list, _ = load_a_list(fixtures.write_a_list(self.dir / "a_list"))
        with self.assertRaisesRegex(ConverterError, "does not match a_list"):
            validate_observed_pairs({(5, 0.9995)}, a_list, "test")

    def test_multi_token_a_list_line_aborts(self):
        path = self.dir / "a_list"
        path.write_text("0.5\n0.6 garbage\n1.0\n")
        with self.assertRaisesRegex(ConverterError, "exactly one scale factor"):
            load_a_list(path)


class TestScatter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.env = ScatterEnv(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_conservation_and_forest_join(self):
        manifest = self.env.run()
        scratch = self.env.workdir / "scratch"
        expected = self.env.expected_snapshot_counts()
        total_rows = 0
        for snap, count in expected.items():
            data = np.fromfile(scratch / snapshot_scratch_name(snap), dtype=RECORD_DTYPE)
            self.assertEqual(len(data), count)
            total_rows += len(data)
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)
            # forest_id joined from the Phase 0 map for every record
            for forest in self.env.forests:
                for tree in forest.trees:
                    tree_snaps = {h.snap for h in tree.halos}
                    mask = data["tree_root_id"] == tree.root_id
                    if snap in tree_snaps:
                        self.assertTrue(mask.any(), "tree {} missing".format(tree.root_id))
                    self.assertTrue(np.all(data["forest_id"][mask] == forest.forest_id))
        n_halos = sum(len(t.halos) for f in self.env.forests for t in f.trees)
        self.assertEqual(total_rows, n_halos)
        pre_counts = sum(entry["pre_count"] for entry in manifest.data["source_files"].values())
        self.assertEqual(pre_counts, n_halos)

    def test_observed_pairs_and_aggregates(self):
        manifest = self.env.run()
        pairs = {(s, a) for s, a in manifest.data["observed_pairs"]}
        expected_snaps = set(self.env.expected_snapshot_counts())
        self.assertEqual({s for s, _ in pairs}, expected_snaps)
        for snap, scale in pairs:
            self.assertAlmostEqual(scale, fixtures.A_LIST[snap], places=5)

        forest_max = np.load(self.env.workdir / "forest_max_snap.npy")
        expected_max = {}
        for forest in self.env.forests:
            for tree in forest.trees:
                for halo in tree.halos:
                    key = forest.forest_id
                    expected_max[key] = max(expected_max.get(key, -1), halo.snap)
        self.assertEqual(
            {int(f): int(s) for f, s in forest_max},
            expected_max,
        )
        table = np.load(self.env.workdir / "forest_index_table.npy")
        np.testing.assert_array_equal(table, sorted(expected_max))

    def test_worker_files_removed_after_concat(self):
        manifest = self.env.run()
        scratch = self.env.workdir / "scratch"
        leftovers = list(scratch.glob("snap_*.src_*.bin"))
        self.assertEqual(leftovers, [])
        removed = [
            k
            for k, v in manifest.data["intermediates"].items()
            if v["kind"] == "worker-scratch" and v["status"] == "removed"
        ]
        self.assertTrue(removed)

    def test_every_scratch_entry_records_frozen_dtype_tag(self):
        manifest = self.env.run()
        record_entries = [
            v
            for v in manifest.data["intermediates"].values()
            if v["kind"] in ("worker-scratch", "snapshot-scratch")
        ]
        self.assertTrue(record_entries)
        for entry in record_entries:
            self.assertEqual(entry["dtype_tag"], DTYPE_TAG)

    def test_missing_input_file_aborts(self):
        self.env.tree_files.append(Path(self.tmp.name) / "missing.dat")
        with self.assertRaisesRegex(ConverterError, "does not exist"):
            self.env.run()

    def test_root_not_in_forests_list_aborts(self):
        extra = fixtures.ForestSpec(
            forest_id=999,
            trees=[
                fixtures.TreeSpec(
                    root_id=9990, halos=[fixtures.HaloSpec(halo_id=9990, snap=5, mvir=1e11)]
                )
            ],
        )
        self.env.tree_files.append(
            fixtures.write_ctrees_file(Path(self.tmp.name) / "extra.dat", extra.trees)
        )
        with self.assertRaisesRegex(ConverterError, "not present in forests.list"):
            self.env.run()

    def test_forest_never_observed_aborts(self):
        ghost = fixtures.ForestSpec(forest_id=999, trees=[fixtures.TreeSpec(root_id=9990)])
        fixtures.write_forests_list(self.env.forests_list, self.env.forests + [ghost])
        with self.assertRaisesRegex(ConverterError, "never observed"):
            self.env.run()

    def test_wrong_declared_tree_count_aborts(self):
        group = self.env.file_forests[0]
        fixtures.write_ctrees_file(
            self.env.tree_files[0],
            fixtures.all_trees(group),
            tree_count=len(fixtures.all_trees(group)) + 1,
        )
        with self.assertRaisesRegex(ConverterError, "declared tree count"):
            self.env.run()

    def test_a_list_mismatch_aborts(self):
        fixtures.write_a_list(self.env.a_list, [0.5, 0.6, 0.7, 0.8, 0.9, 0.95])
        with self.assertRaisesRegex(ConverterError, "does not match a_list"):
            self.env.run()

    def test_resume_after_crash_skips_completed(self):
        # simulate a crash: file 1 is malformed, so the run aborts after file 0
        good_text = self.env.tree_files[1].read_text()
        lines = good_text.splitlines()
        first_data = next(
            i
            for i, l in enumerate(lines[1:], start=1)
            if not l.startswith("#") and len(l.split()) > 1
        )
        lines[first_data] = lines[first_data].replace(lines[first_data].split()[8], "notanumber", 1)
        self.env.tree_files[1].write_text("\n".join(lines) + "\n")
        with self.assertRaisesRegex(ConverterError, "malformed"):
            self.env.run()

        manifest = Manifest.load_or_create(self.env.workdir)
        file0 = str(self.env.tree_files[0].resolve())
        self.assertEqual(manifest.data["source_files"][file0]["status"], "completed")

        marker = self.env.workdir / "scratch" / "roots_src_0.npy"
        mtime_before = marker.stat().st_mtime_ns

        # repair file 1 and resume: file 0 must be skipped, totals must be right
        self.env.tree_files[1].write_text(good_text)
        manifest = self.env.run()
        self.assertEqual(marker.stat().st_mtime_ns, mtime_before)
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)

    def test_kill_mid_scatter_leaves_partials_and_rerun_recovers(self):
        # crash after the first 2-row chunk: partial worker binaries on disk,
        # no completed manifest entry for the killed file
        with mock.patch("scatter.CtreesFileParser", CrashAfterFirstChunkParser):
            with self.assertRaisesRegex(ConverterError, "simulated kill"):
                self.env.run(chunksize=2)
        scratch = self.env.workdir / "scratch"
        partials = list(scratch.glob("snap_*.src_*.bin"))
        self.assertTrue(partials, "expected partial worker binaries after the kill")
        manifest = Manifest.load_or_create(self.env.workdir)
        self.assertEqual(manifest.data["source_files"], {})

        # re-run: 'wb' truncation must absorb the partial files; totals exact
        manifest = self.env.run(chunksize=2)
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            entry = manifest.data["snapshots"][str(snap)]
            self.assertEqual(entry["rows"], count)
            data = np.fromfile(scratch / snapshot_scratch_name(snap), dtype=RECORD_DTYPE)
            self.assertEqual(len(data), count)

    def test_changed_a_list_content_refuses_resume(self):
        self.env.run()
        fixtures.write_a_list(self.env.a_list, [0.5, 0.6, 0.7, 0.8, 0.9, 0.99995])
        with self.assertRaisesRegex(ConverterError, "a_list content changed"):
            self.env.run()

    def test_changed_forests_list_content_refuses_resume(self):
        self.env.run()
        with open(self.env.forests_list, "a") as handle:
            handle.write("# annotated later\n")
        with self.assertRaisesRegex(ConverterError, "forests_list content changed"):
            self.env.run()

    def test_changed_source_set_refuses_resume(self):
        self.env.run()
        extra = fixtures.ForestSpec(
            forest_id=999,
            trees=[
                fixtures.TreeSpec(
                    root_id=9990, halos=[fixtures.HaloSpec(halo_id=9990, snap=5, mvir=1e11)]
                )
            ],
        )
        self.env.tree_files.append(
            fixtures.write_ctrees_file(Path(self.tmp.name) / "extra.dat", extra.trees)
        )
        with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
            self.env.run()

    def test_changed_source_after_finalize_refuses_resume(self):
        self.env.run()
        with open(self.env.tree_files[0], "a") as handle:
            handle.write("#tree 10199\n")  # content change -> size/mtime change
        with self.assertRaisesRegex(ConverterError, "after snapshots were finalized"):
            self.env.run()

    def test_tampered_concat_target_detected_on_resume(self):
        manifest = self.env.run()
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        _flip_last_byte(Path(manifest.data["snapshots"][snap]["scratch_file"]))
        with self.assertRaisesRegex(ConverterError, "checksum"):
            self.env.run()

    def test_tampered_worker_file_detected_before_concat(self):
        good_text = self._crash_between_files()
        # tamper a non-id byte of one registered worker file (last field is
        # forest_id, so the id checksum alone would not notice)
        worker = next((self.env.workdir / "scratch").glob("snap_*.src_0.bin"))
        _flip_last_byte(worker)
        self.env.tree_files[1].write_text(good_text)
        with self.assertRaisesRegex(ConverterError, "checksum"):
            self.env.run()

    def _crash_between_files(self):
        """Abort after file 0 completes: its sidecars and worker files persist."""
        good_text = self.env.tree_files[1].read_text()
        lines = good_text.splitlines()
        first_data = next(
            i
            for i, l in enumerate(lines[1:], start=1)
            if not l.startswith("#") and len(l.split()) > 1
        )
        tokens = lines[first_data].split()
        tokens[8] = "notanumber"
        lines[first_data] = " ".join(tokens)
        self.env.tree_files[1].write_text("\n".join(lines) + "\n")
        with self.assertRaisesRegex(ConverterError, "malformed"):
            self.env.run()
        return good_text

    def test_tampered_roots_sidecar_detected_on_resume(self):
        good_text = self._crash_between_files()
        _flip_last_byte(self.env.workdir / "scratch" / "roots_src_0.npy")
        self.env.tree_files[1].write_text(good_text)
        with self.assertRaisesRegex(ConverterError, "observed-roots sidecar.*checksum"):
            self.env.run()

    def test_tampered_forest_max_sidecar_detected_on_resume(self):
        good_text = self._crash_between_files()
        _flip_last_byte(self.env.workdir / "scratch" / "forest_max_src_0.npy")
        self.env.tree_files[1].write_text(good_text)
        with self.assertRaisesRegex(ConverterError, "forest-max-snap sidecar.*checksum"):
            self.env.run()

    def test_tampered_merged_table_detected_on_resume(self):
        self.env.run()
        _flip_last_byte(self.env.workdir / "forest_max_snap.npy")
        with self.assertRaisesRegex(ConverterError, "forest-max-snap-merged.*checksum"):
            self.env.run()

    def test_registered_tables_not_silently_rebuilt(self):
        self.env.run()
        table = self.env.workdir / "forest_index_table.npy"
        mtime_before = table.stat().st_mtime_ns
        self.env.run()
        self.assertEqual(table.stat().st_mtime_ns, mtime_before)

    def test_interrupted_worker_cleanup_recovers_on_resume(self):
        # simulate a crash between worker unlink and manifest save: the entry
        # says present, the file is gone — the retry path records it removed
        manifest = self.env.run()
        key, entry = next(
            (k, v)
            for k, v in manifest.data["intermediates"].items()
            if v["kind"] == "worker-scratch" and v["status"] == "removed"
        )
        entry["status"] = "present"
        manifest.save()
        manifest = self.env.run()
        self.assertEqual(manifest.data["intermediates"][key]["status"], "removed")

    def test_sorted_branch_verifies_artifacts_on_scatter_resume(self):
        from sort_index import run_sort

        manifest = self.env.run()
        run_sort(self.env.workdir)
        manifest = Manifest.load_or_create(self.env.workdir)
        snap = sorted(manifest.data["snapshots"], key=int)[0]
        Path(manifest.data["snapshots"][snap]["sorted_file"]).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            self.env.run()

    def test_source_mutation_between_prescan_and_parse_aborts(self):
        with mock.patch("scatter.CtreesFileParser", TouchSourceParser):
            with self.assertRaisesRegex(ConverterError, "changed between pre-scan and parse"):
                self.env.run()

    def test_nan_a_list_aborts(self):
        (Path(self.tmp.name) / "bad.a_list").write_text("0.5\nnan\n1.0\n")
        self.env.a_list = Path(self.tmp.name) / "bad.a_list"
        with self.assertRaisesRegex(ConverterError, "non-finite a_list"):
            self.env.run()

    def test_dtype_tag_mismatch_refuses_resume(self):
        self.env.run()
        manifest_path = self.env.workdir / "manifest.json"
        data = json.loads(manifest_path.read_text())
        data["dtype_tag"] = "something-else"
        manifest_path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ConverterError, "dtype tag mismatch"):
            self.env.run()

    def test_pool_scatter_matches_serial(self):
        manifest = self.env.run(pool_size=2)
        scratch = self.env.workdir / "scratch"
        serial_root = Path(self.tmp.name) / "serial"
        serial_root.mkdir()
        serial_env = ScatterEnv(serial_root)
        serial_env.run(pool_size=1)
        for snap in self.env.expected_snapshot_counts():
            a = (scratch / snapshot_scratch_name(snap)).read_bytes()
            b = (serial_env.workdir / "scratch" / snapshot_scratch_name(snap)).read_bytes()
            self.assertEqual(a, b)
        self.assertTrue(manifest.data["snapshots"])


class TestCleanupContainment(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.workdir = self.dir / "workdir"
        self.workdir.mkdir()
        self.manifest = Manifest.load_or_create(self.workdir)

    def tearDown(self):
        self.tmp.cleanup()

    def test_unregistered_path_refused(self):
        victim = self.workdir / "innocent.bin"
        victim.write_bytes(b"data")
        with self.assertRaisesRegex(ConverterError, "not a manifest-owned"):
            self.manifest.remove_intermediate(victim)
        self.assertTrue(victim.exists())

    def test_outside_workdir_refused(self):
        outside = self.dir / "outside.bin"
        outside.write_bytes(b"data")
        # even a registered path outside the workdir must be refused
        self.manifest.register_intermediate(outside, "worker-scratch")
        with self.assertRaisesRegex(ConverterError, "outside the workdir"):
            self.manifest.remove_intermediate(outside)
        self.assertTrue(outside.exists())

    def test_registered_inside_workdir_deleted_once(self):
        target = self.workdir / "scratch.bin"
        target.write_bytes(b"data")
        self.manifest.register_intermediate(target, "worker-scratch")
        self.manifest.remove_intermediate(target)
        self.assertFalse(target.exists())
        with self.assertRaisesRegex(ConverterError, "not a manifest-owned"):
            self.manifest.remove_intermediate(target)

    def test_registration_records_content_checksum(self):
        target = self.workdir / "scratch.bin"
        target.write_bytes(b"data")
        self.manifest.register_intermediate(target, "worker-scratch")
        entry = self.manifest.data["intermediates"][str(target.resolve())]
        self.assertEqual(entry["md5"], "8d777f385d3dfec8815d20f7496026dc")  # md5("data")

    def test_tampered_intermediate_refused(self):
        target = self.workdir / "scratch.bin"
        target.write_bytes(b"data")
        self.manifest.register_intermediate(target, "worker-scratch")
        target.write_bytes(b"tampered")
        with self.assertRaisesRegex(ConverterError, "checksum"):
            self.manifest.remove_intermediate(target)
        self.assertTrue(target.exists())


if __name__ == "__main__":
    unittest.main()
