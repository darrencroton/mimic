"""Slice 3 unit tests: Phase 0 map, root coverage, ForestIndex order, scatter
conservation, manifest resume, aggregates, cleanup containment guard."""

import json
import multiprocessing as mp
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
import scatter  # noqa: E402
from ctrees_parser import DTYPE_TAG, RECORD_DTYPE, ConverterError, CtreesFileParser  # noqa: E402
from scatter import (  # noqa: E402
    Manifest,
    load_a_list,
    load_forests_list,
    run_scatter,
    snapshot_scratch_name,
    validate_observed_pairs,
    validate_root_coverage,
)


def _mp_worker_init_and_count(forests_list_path, counter, barrier) -> None:
    """Test-only Pool initializer: run the real per-worker loader, record
    that this worker process ran it, then rendezvous with the parent and
    every sibling worker. Must be module-level so it can be pickled by
    reference under the ``spawn`` start method.

    The barrier is the difference between a deterministic count and a race:
    without it, ``Pool.map`` can satisfy every task from whichever worker(s)
    finish spawning first, and the context manager's ``terminate()`` on exit
    can kill a slower sibling before it ever reaches this initializer —
    undercounting workers that were requested but never needed. Waiting here
    forces all requested workers to complete initialization before the
    parent submits any task.
    """
    scatter._init_scatter_worker(forests_list_path)
    with counter.get_lock():
        counter.value += 1
    barrier.wait()


def _mp_lookup_task(root_id: int) -> int:
    """Test-only pool task: look up one root id via the worker-global map
    ``_init_scatter_worker`` set, proving the map is actually usable from
    inside the worker process rather than merely loaded and discarded."""
    return int(scatter._worker_forest_map.lookup_forest_ids(np.array([root_id]))[0])


class KilledMidScatter(ConverterError):
    """Sentinel raised by the crashing parser below."""


class CrashAfterFirstChunkParser(CtreesFileParser):
    """Writes exactly one chunk's worth of scatter output, then dies —
    simulating a kill mid-scatter with partial worker binaries on disk."""

    def chunks(self):
        gen = super().chunks()
        yield next(gen)
        raise KilledMidScatter("simulated kill mid-scatter")


class CrashOnNamedFileParser(CtreesFileParser):
    """Raises before producing any scatter output for one named source file,
    without ever touching that file's bytes or mtime — unlike editing the
    source text to force a 'malformed' abort, this keeps a later
    byte-identical manifest comparison across the simulated crash valid."""

    crash_on_filename = None

    def chunks(self):
        if self.crash_on_filename is not None and self.path.name == self.crash_on_filename:
            raise KilledMidScatter(
                "simulated crash before scattering {}".format(self.crash_on_filename)
            )
        yield from super().chunks()


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


def _split_forests(forests, n_files: int):
    """Split forests into n_files contiguous, non-empty groups, as evenly as
    possible with any remainder in the earliest groups. For n_files=2 over
    the 5 standard_forests() entries this reproduces the original 3/2 split
    (file 0: forests 100/200/400; file 1: forests 500/600) exactly."""
    n = len(forests)
    base, extra = divmod(n, n_files)
    groups = []
    start = 0
    for i in range(n_files):
        size = base + (1 if i < extra else 0)
        groups.append(forests[start : start + size])
        start += size
    return groups


class ScatterEnv:
    """One synthetic multi-file scatter setup in a temp directory (2 files by
    default; pass n_files for a larger fixture)."""

    def __init__(self, root: Path, n_files: int = 2):
        self.root = root
        self.workdir = root / "workdir"
        self.forests = fixtures.standard_forests()
        self.file_forests = _split_forests(self.forests, n_files)
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
        # simulate a crash: file 1 is malformed, so the run aborts after file 0.
        # save_every_n_files=1 forces the pre-batching per-file-save behaviour
        # so file 0's completion is guaranteed to reach disk before the crash —
        # the scenario this test is actually about (skip a *saved* completion)
        # is independent of the save-policy knob under test elsewhere.
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
            self.env.run(save_every_n_files=1)

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
        """Abort after file 0 completes: its sidecars and worker files persist.
        save_every_n_files=1 forces file 0's completion to be saved to the
        on-disk manifest before the crash, so resume skip-trusts it rather
        than re-scattering over the tampering these tests inject — the
        deferred-save behaviour itself is covered by dedicated tests below."""
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
            self.env.run(save_every_n_files=1)
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


class TestForestMapWorkerDistribution(unittest.TestCase):
    """Slice 2 (item 4): the forest map reaches each worker once per worker
    process, via a Pool initializer, instead of being pickled per task."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        scatter._worker_forest_map = None
        self.tmp.cleanup()

    def test_init_scatter_worker_matches_parent_lookup(self):
        forests = fixtures.standard_forests()
        path = fixtures.write_forests_list(self.dir / "forests.list", forests)
        parent_map = load_forests_list(path)

        scatter._init_scatter_worker(path)
        worker_map = scatter._worker_forest_map
        self.assertIsNotNone(worker_map)
        np.testing.assert_array_equal(
            worker_map.lookup_forest_ids(parent_map.tree_root_ids),
            parent_map.lookup_forest_ids(parent_map.tree_root_ids),
        )
        # the recorded provenance md5 is computed from the exact bytes the
        # loader parsed, so an independent worker-side load of the same file
        # must reproduce it exactly
        self.assertEqual(worker_map.md5, parent_map.md5)

    def test_initializer_runs_once_per_worker_across_many_tasks(self):
        # real spawn-backed multiprocessing (this host's platform default):
        # more tasks than workers, so a per-task load would drive the
        # counter to n_tasks, while a per-worker load bounds it at n_workers
        forests = fixtures.standard_forests()
        path = fixtures.write_forests_list(self.dir / "forests.list", forests)
        expected_map = load_forests_list(path)
        roots = expected_map.tree_root_ids.tolist()

        n_workers = 2
        n_tasks = 8
        self.assertGreater(n_tasks, n_workers)
        tasks = [roots[i % len(roots)] for i in range(n_tasks)]

        ctx = mp.get_context("spawn")
        counter = ctx.Value("i", 0)
        barrier = ctx.Barrier(n_workers + 1)  # +1 for this parent process
        with ctx.Pool(
            processes=n_workers,
            initializer=_mp_worker_init_and_count,
            initargs=(path, counter, barrier),
        ) as pool:
            # blocks until every requested worker has completed its
            # initializer, so the count below cannot be short-circuited by
            # tasks finishing before a slower worker finished starting
            barrier.wait(timeout=30)
            results = pool.map(_mp_lookup_task, tasks)

        self.assertEqual(counter.value, n_workers)
        expected = expected_map.lookup_forest_ids(np.asarray(tasks, dtype=np.int64)).tolist()
        self.assertEqual(results, expected)

    def test_pool_dispatch_wires_initializer_instead_of_per_task_map(self):
        # serialization-event evidence: the task argument tuples handed to
        # imap_unordered must not carry a ForestMap at all, and Pool must be
        # constructed with the per-worker loader wired as its initializer.
        env = ScatterEnv(self.dir, n_files=3)
        captured = {}
        real_pool = scatter.Pool

        class RecordingPool:
            def __init__(self, *args, **kwargs):
                captured["kwargs"] = kwargs
                self._pool = real_pool(*args, **kwargs)

            def __enter__(self):
                self._entered = self._pool.__enter__()
                return self

            def __exit__(self, *exc):
                return self._pool.__exit__(*exc)

            def imap_unordered(self, func, iterable, *a, **kw):
                # materialize the iterable so it can be inspected: this is
                # exactly what run_scatter hands to the real Pool, captured
                # before it is consumed
                task_args = list(iterable)
                captured["task_args"] = task_args
                return self._entered.imap_unordered(func, task_args, *a, **kw)

        with mock.patch("scatter.Pool", RecordingPool):
            env.run(pool_size=2)

        self.assertIs(captured["kwargs"]["initializer"], scatter._init_scatter_worker)
        self.assertEqual(captured["kwargs"]["initargs"], (env.forests_list,))

        task_args = captured["task_args"]
        self.assertEqual(len(task_args), 3)  # one task per pending source file
        for task in task_args:
            self.assertEqual(len(task), 4)  # (path, src_index, scratch_dir, chunksize)
            for element in task:
                self.assertNotIsInstance(element, scatter.ForestMap)

    def test_serial_path_still_loads_forest_map_directly_in_parent(self):
        # pool_size<=1 (and the single-pending-file case) must keep using the
        # parent-loaded ForestMap object directly, the same code path as
        # before this slice -- not the worker initializer/global route.
        env = ScatterEnv(self.dir, n_files=2)
        scatter._worker_forest_map = None
        env.run(pool_size=1)
        self.assertIsNone(scatter._worker_forest_map)


class TestBatchedManifestSave(unittest.TestCase):
    """Slice 1 (item 7): bounded-interval manifest persistence in run_scatter."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.env = ScatterEnv(Path(self.tmp.name))

    def tearDown(self):
        CrashOnNamedFileParser.crash_on_filename = None
        self.tmp.cleanup()

    def _clean_baseline_manifest_bytes(self, env=None, **run_kwargs) -> bytes:
        """Run once, uninterrupted, with the given save-policy kwargs, to
        capture reference manifest bytes, then reset the workdir so a later
        scenario starts from scratch at the exact same path — the source
        files, forests.list and a_list (all outside the workdir) are
        untouched, so every path embedded in the manifest matches between
        the two scenarios and a raw byte compare is meaningful."""
        env = env if env is not None else self.env
        env.run(**run_kwargs)
        baseline = (env.workdir / "manifest.json").read_bytes()
        shutil.rmtree(env.workdir)
        return baseline

    def test_manifest_byte_identical_regardless_of_save_policy(self):
        # save_every_n_files=1 reproduces the pre-batching per-file-save
        # cadence; the default policy batches saves. Same input -> the final
        # manifest content must not depend on when it was written.
        per_file_bytes = self._clean_baseline_manifest_bytes(save_every_n_files=1)
        batched_bytes = self._clean_baseline_manifest_bytes()
        self.assertEqual(batched_bytes, per_file_bytes)

    def test_default_policy_leaves_recent_completion_unsaved_on_crash(self):
        # crash via a named-file parser rather than corrupting file 1's text:
        # neither source file's bytes/mtime change, so file 0's completion is
        # the only thing that differs between a clean run and this one.
        CrashOnNamedFileParser.crash_on_filename = self.env.tree_files[1].name
        with mock.patch("scatter.CtreesFileParser", CrashOnNamedFileParser):
            with self.assertRaisesRegex(ConverterError, "simulated crash"):
                self.env.run()
        # binding invariant: no manifest was written that could name an
        # artifact not durably written -- here, none was written at all
        self.assertFalse((self.env.workdir / "manifest.json").exists())
        # but file 0's worker/sidecar artifacts were durably written already
        scratch = self.env.workdir / "scratch"
        self.assertTrue(next(scratch.glob("snap_*.src_0.bin")).exists())
        self.assertTrue((scratch / "roots_src_0.npy").exists())
        self.assertTrue((scratch / "forest_max_src_0.npy").exists())

    def test_time_based_save_policy_persists_before_count_threshold(self):
        # save_every_n_files is set far out of reach; save_every_seconds=0
        # forces the time-based branch to fire after file 0's completion.
        CrashOnNamedFileParser.crash_on_filename = self.env.tree_files[1].name
        with mock.patch("scatter.CtreesFileParser", CrashOnNamedFileParser):
            with self.assertRaisesRegex(ConverterError, "simulated crash"):
                self.env.run(save_every_n_files=999, save_every_seconds=0.0)
        manifest_path = self.env.workdir / "manifest.json"
        self.assertTrue(manifest_path.exists())
        data = json.loads(manifest_path.read_text())
        file0 = str(self.env.tree_files[0].resolve())
        self.assertEqual(data["source_files"][file0]["status"], "completed")

    def test_default_time_policy_does_not_degrade_to_per_file_saves(self):
        # regression: a finite default for save_every_seconds shorter than
        # the plan's measured production inter-completion interval
        # (~40-108 s, serial vs pooled) would make the time arm fire on
        # every completion, silently reproducing the per-file-save cost
        # this slice exists to remove. A synthetic fixture's wall clock
        # cannot reach those intervals, so drive time.monotonic() forward
        # by far more than any realistic *finite* threshold between every
        # completion, and confirm the DEFAULT policy still batches: with
        # save_every_n_files=25 out of reach for a handful of files and the
        # time arm disabled by default, only the single unconditional
        # post-loop save should fire -- not one save per file.
        n_files = 5
        root = Path(self.tmp.name) / "many"
        root.mkdir()
        env = ScatterEnv(root, n_files=n_files)

        fake_now = [0.0]

        def fake_monotonic():
            fake_now[0] += 200.0
            return fake_now[0]

        save_calls = []
        real_save = Manifest.save

        def spy_save(self):
            save_calls.append(1)
            return real_save(self)

        # _finalize_scatter also calls manifest.save() internally (per
        # snapshot, unrelated to this slice); mocking it out isolates the
        # save count to exactly the dispatch loop and the post-loop save
        # under test here.
        with mock.patch("scatter.time.monotonic", side_effect=fake_monotonic), mock.patch.object(
            Manifest, "save", spy_save
        ), mock.patch("scatter._finalize_scatter", return_value=None):
            env.run()

        self.assertEqual(save_calls, [1])

    def test_resume_after_unsaved_crash_overwrites_and_matches_clean_manifest(self):
        baseline_bytes = self._clean_baseline_manifest_bytes()
        scratch = self.env.workdir / "scratch"

        CrashOnNamedFileParser.crash_on_filename = self.env.tree_files[1].name
        with mock.patch("scatter.CtreesFileParser", CrashOnNamedFileParser):
            with self.assertRaisesRegex(ConverterError, "simulated crash"):
                self.env.run()
        self.assertFalse((self.env.workdir / "manifest.json").exists())
        # roots_src_0.npy survives the whole run (unlike worker-scratch,
        # which finalize deletes after concat), so it can be checked both
        # before and after resume completes
        roots_marker = scratch / "roots_src_0.npy"
        mtime_before = roots_marker.stat().st_mtime_ns

        # resume: file 0's completion was never saved, so it is re-scattered;
        # the leftover artifacts from the crashed attempt must be
        # overwritten deterministically, not silently reused
        manifest = self.env.run()
        self.assertNotEqual(roots_marker.stat().st_mtime_ns, mtime_before)
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)

        resumed_bytes = (self.env.workdir / "manifest.json").read_bytes()
        self.assertEqual(resumed_bytes, baseline_bytes)

    def test_crash_between_saves_resumes_only_the_unsaved_files(self):
        # a two-file fixture can only construct 'crash before the first
        # save'; the ONLY in 're-scatters only the unsaved files' needs a
        # crash where SOME completions already reached disk and others did
        # not. Four files + save_every_n_files=2 (save_every_seconds set high
        # enough never to fire on its own) gives: file 0 completes (1 <2, no
        # save), file 1 completes (2 >=2, SAVE covers files 0+1, counter
        # resets), file 2 completes (1 <2, no save), file 3 crashes before
        # completing -- so file 2 is the mixed state: durably written but
        # never saved, sitting between the save after file 1 and the save
        # that would have followed file 3.
        four_root = Path(self.tmp.name) / "four"
        four_root.mkdir()
        env = ScatterEnv(four_root, n_files=4)
        baseline_bytes = self._clean_baseline_manifest_bytes(env)
        scratch = env.workdir / "scratch"

        CrashOnNamedFileParser.crash_on_filename = env.tree_files[3].name
        with mock.patch("scatter.CtreesFileParser", CrashOnNamedFileParser):
            with self.assertRaisesRegex(ConverterError, "simulated crash"):
                env.run(save_every_n_files=2, save_every_seconds=999.0)

        manifest_path = env.workdir / "manifest.json"
        self.assertTrue(manifest_path.exists())
        data = json.loads(manifest_path.read_text())
        saved_sources = {
            entry["src_index"]
            for entry in data["source_files"].values()
            if entry["status"] == "completed"
        }
        self.assertEqual(saved_sources, {0, 1})  # exactly the threshold-saved pair

        # file 2 was durably written (record() ran) but the save covering it
        # never happened before the crash
        roots_2 = scratch / "roots_src_2.npy"
        self.assertTrue(roots_2.exists())
        mtime_saved_0 = (scratch / "roots_src_0.npy").stat().st_mtime_ns
        mtime_saved_1 = (scratch / "roots_src_1.npy").stat().st_mtime_ns
        mtime_unsaved_2 = roots_2.stat().st_mtime_ns

        manifest = env.run()  # resume

        # (a) already-saved files 0 and 1 were skip-trusted, not re-scattered
        self.assertEqual((scratch / "roots_src_0.npy").stat().st_mtime_ns, mtime_saved_0)
        self.assertEqual((scratch / "roots_src_1.npy").stat().st_mtime_ns, mtime_saved_1)
        # (b) the unsaved file 2 and the never-completed file 3 were
        # (re-)scattered
        self.assertNotEqual(roots_2.stat().st_mtime_ns, mtime_unsaved_2)
        self.assertTrue((scratch / "roots_src_3.npy").exists())

        # (c) the converged result matches an uninterrupted run over the same
        # four files, byte for byte
        expected = env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)
        resumed_bytes = manifest_path.read_bytes()
        self.assertEqual(resumed_bytes, baseline_bytes)


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
