"""Slice 3 unit tests: Phase 0 map, root coverage, ForestIndex order, scatter
conservation, manifest resume, aggregates, cleanup containment guard."""

import contextlib
import io
import json
import multiprocessing as mp
import os
import shutil
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_ctrees  # noqa: E402
import fixtures  # noqa: E402
import scatter  # noqa: E402
from ctrees_parser import DTYPE_TAG, RECORD_DTYPE, ConverterError, CtreesFileParser  # noqa: E402
from fixups import run_fixups  # noqa: E402
from hdf5_writer import run_write  # noqa: E402
from links import run_links  # noqa: E402
from scatter import (  # noqa: E402
    SOURCE_COMPLETED,
    SOURCE_CONSUMED,
    Manifest,
    load_a_list,
    load_forests_list,
    run_finalize,
    run_release,
    run_scatter,
    snapshot_scratch_name,
    validate_observed_pairs,
    validate_root_coverage,
    worker_scratch_name,
)
from sort_index import run_sort  # noqa: E402


def _mp_worker_init_and_count(forests_list_path, expected_md5, counter, barrier) -> None:
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
    scatter._init_scatter_worker(forests_list_path, expected_md5)
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


def _stash(path: Path, holding: Path) -> Path:
    """Move a source file out of sight, the way a not-yet-transferred (or
    already-released-and-deleted) file is absent from the operator's disk.

    A rename inside one filesystem keeps the inode, so a file moved out and
    back has byte-identical content and the identical size and ``mtime_ns``
    the frozen manifest recorded — which is what makes it a faithful stand-in
    for "these bytes are not here right now" rather than for "these bytes
    changed"."""
    holding.mkdir(parents=True, exist_ok=True)
    dest = holding / path.name
    path.rename(dest)
    return dest


def _restore(stashed: Path, path: Path) -> None:
    stashed.rename(path)


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
        scatter._worker_init_error = None
        self.tmp.cleanup()

    def test_init_scatter_worker_matches_parent_lookup(self):
        forests = fixtures.standard_forests()
        path = fixtures.write_forests_list(self.dir / "forests.list", forests)
        parent_map = load_forests_list(path)

        scatter._init_scatter_worker(path, parent_map.md5)
        worker_map = scatter._worker_forest_map
        self.assertIsNotNone(worker_map)
        self.assertIsNone(scatter._worker_init_error)
        np.testing.assert_array_equal(
            worker_map.lookup_forest_ids(parent_map.tree_root_ids),
            parent_map.lookup_forest_ids(parent_map.tree_root_ids),
        )
        # the recorded provenance md5 is computed from the exact bytes the
        # loader parsed, so an independent worker-side load of the same file
        # must reproduce it exactly
        self.assertEqual(worker_map.md5, parent_map.md5)

    def test_init_scatter_worker_records_mismatch_without_raising(self):
        # PM correction (steer-attempt-2.md, Finding 1): an uncaught
        # exception inside a Pool initializer does not fail the pool
        # promptly -- CPython just keeps replacing the dead worker, turning
        # a rare data-integrity risk into a silent hang on a multi-day run.
        # The initializer must record a mismatch, not raise it.
        forests = fixtures.standard_forests()
        path = fixtures.write_forests_list(self.dir / "forests.list", forests)
        real_md5 = load_forests_list(path).md5
        wrong_md5 = "0" * 32 if real_md5 != "0" * 32 else "1" * 32

        scatter._init_scatter_worker(path, wrong_md5)  # must not raise

        self.assertIsNone(scatter._worker_forest_map)
        self.assertIsNotNone(scatter._worker_init_error)
        self.assertIn(str(path), scatter._worker_init_error)
        self.assertIn(real_md5, scatter._worker_init_error)
        self.assertIn(wrong_md5, scatter._worker_init_error)

    def test_scatter_worker_raises_clear_error_on_recorded_mismatch(self):
        # the deterministic surfacing half of Finding 1: _scatter_worker
        # turns a recorded mismatch into a ConverterError naming the path
        # and both md5s, raised from the normal per-task path so it
        # propagates to the parent through Pool's ordinary result mechanism
        # (unlike an initializer exception).
        scatter._worker_forest_map = None
        scatter._worker_init_error = (
            "/fake/forests.list: worker-loaded forests.list content (md5 aaa) "
            "does not match the parent's (md5 bbb)"
        )
        with self.assertRaisesRegex(ConverterError, r"aaa.*bbb|/fake/forests\.list"):
            scatter._scatter_worker(("/fake/path.dat", 0, self.dir, 1_000_000))

    def test_scatter_worker_raises_clear_error_when_never_initialized(self):
        # Finding 2 (opencode, P3): the "never initialized" case must say so
        # plainly rather than crash with an AttributeError on None.
        scatter._worker_forest_map = None
        scatter._worker_init_error = None
        with self.assertRaisesRegex(ConverterError, "never initialized"):
            scatter._scatter_worker(("/fake/path.dat", 0, self.dir, 1_000_000))

    def test_worker_forest_map_content_mismatch_aborts_run_with_no_completed_output(self):
        # end-to-end: the two loads must differ in CONTENT, not merely
        # mtime. Mutating forests.list inside Pool's own __init__ (called by
        # run_scatter after the parent's load/md5 are already captured, but
        # before any worker process spawns and performs its own independent
        # load) makes every worker's load see the new bytes deterministically
        # -- not a timing race.
        env = ScatterEnv(self.dir, n_files=3)
        real_pool = scatter.Pool

        class MutatingPool:
            def __init__(self, *args, **kwargs):
                with open(env.forests_list, "a") as handle:
                    handle.write("# content changed after the parent's load\n")
                self._pool = real_pool(*args, **kwargs)

            def __enter__(self):
                return self._pool.__enter__()

            def __exit__(self, *exc):
                return self._pool.__exit__(*exc)

        with mock.patch("scatter.Pool", MutatingPool):
            with self.assertRaisesRegex(ConverterError, "does not match the parent's"):
                env.run(pool_size=2)

        # no completed output: every worker's independent load disagreed
        # with the parent's (the mutation lands before any worker spawns),
        # so no FileScatterResult was ever produced and nothing downstream
        # of scatter was reached
        self.assertFalse((env.workdir / "manifest.json").exists())
        self.assertFalse((env.workdir / "forest_index_table.npy").exists())
        scratch = env.workdir / "scratch"
        self.assertEqual(list(scratch.glob("snap_*.src_*.bin")), [])

    def test_worker_forest_map_unparsable_reload_aborts_without_hanging(self):
        # PM correction 3 (steer-attempt-3.md, Finding 1): the mismatch test
        # above appends a *valid* comment line, which keeps the worker's own
        # load_forests_list call succeeding -- it cannot reach the uncaught-
        # loader-exception subclass of this defect. Here the worker-side
        # reload is made genuinely unparsable (truncated to empty, which
        # load_forests_list turns into "no forest rows found"), so
        # _init_scatter_worker must catch that too and not just an md5
        # mismatch on an otherwise-parseable file. If the initializer were
        # ever to let that exception escape uncaught, Pool would keep
        # replacing the crashed worker forever and imap_unordered would
        # never receive a result or an error -- a hang, not a failure. Run
        # the blocking call on a daemon thread with an explicit join
        # timeout so a regression here fails this test promptly instead of
        # hanging the whole suite.
        env = ScatterEnv(self.dir, n_files=3)
        real_pool = scatter.Pool

        class CorruptingPool:
            def __init__(self, *args, **kwargs):
                # truncate to empty AFTER the parent's own successful
                # load/md5 capture but BEFORE any worker spawns and performs
                # its own independent load -- deterministic, not a race (see
                # the mismatch test above for why Pool.__init__ is the right
                # hook point)
                Path(env.forests_list).write_bytes(b"")
                self._pool = real_pool(*args, **kwargs)

            def __enter__(self):
                return self._pool.__enter__()

            def __exit__(self, *exc):
                return self._pool.__exit__(*exc)

        outcome = {}

        def target():
            with mock.patch("scatter.Pool", CorruptingPool):
                try:
                    env.run(pool_size=2)
                except BaseException as exc:  # noqa: BLE001 -- captured across the thread boundary
                    outcome["exc"] = exc

        thread = threading.Thread(target=target, daemon=True)
        thread.start()
        thread.join(timeout=60)
        self.assertFalse(
            thread.is_alive(),
            "run_scatter did not return within 60s -- suspected initializer "
            "respawn hang (a regression in _init_scatter_worker's total-catch "
            "guarantee)",
        )
        self.assertIn("exc", outcome)
        self.assertIsInstance(outcome["exc"], ConverterError)
        self.assertRegex(str(outcome["exc"]), "no forest rows found")

        self.assertFalse((env.workdir / "manifest.json").exists())
        scratch = env.workdir / "scratch"
        self.assertEqual(list(scratch.glob("snap_*.src_*.bin")), [])

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
            initargs=(path, expected_map.md5, counter, barrier),
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

        expected_md5 = load_forests_list(env.forests_list).md5

        with mock.patch("scatter.Pool", RecordingPool):
            env.run(pool_size=2)

        self.assertIs(captured["kwargs"]["initializer"], scatter._init_scatter_worker)
        self.assertEqual(captured["kwargs"]["initargs"], (env.forests_list, expected_md5))

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


class TestBatchModeSourceInventory(unittest.TestCase):
    """Slice 3 (item 3): the batch-aware source inventory for the interleaved
    consumptive transfer — telling a not-yet-transferred file (deferred) apart
    from a scattered-and-released one (consumed), the explicit finalize, and
    the explicit release.

    Every test here drives the same four-file fixture inventory: batch mode's
    whole contract is that the *complete* inventory is passed every time and
    only the subset on disk varies, so the fixture has to have more than one
    absent file to be able to express that.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.env = ScatterEnv(self.root, n_files=4)
        self.holding = self.root / "not_yet_transferred"

    def tearDown(self):
        self.tmp.cleanup()

    def _key(self, index: int) -> str:
        return str(self.env.tree_files[index].resolve())

    def _status(self, index: int) -> str:
        manifest = Manifest.load_or_create(self.env.workdir)
        return manifest.data["source_files"][self._key(index)]["status"]

    # -- the two absences ---------------------------------------------------

    def test_missing_source_outside_batch_mode_is_still_a_hard_error(self):
        _stash(self.env.tree_files[2], self.holding)
        with self.assertRaises(ConverterError) as ctx:
            self.env.run()
        self.assertIn("input tree file does not exist", str(ctx.exception))
        self.assertIn(str(self.env.tree_files[2]), str(ctx.exception))
        # nothing was scattered: the run aborted before any work
        self.assertFalse((self.env.workdir / scatter.MANIFEST_NAME).exists())

        # and it aborts there *before* the metadata loads, exactly as it always
        # has: with the a_list unreadable as well, the missing source file is
        # still what the operator is told about
        self.env.a_list = self.root / "no_such.a_list"
        with self.assertRaises(ConverterError) as ctx:
            self.env.run()
        self.assertIn("input tree file does not exist", str(ctx.exception))

    def test_batch_mode_defers_absent_files_and_scatters_what_arrived(self):
        _stash(self.env.tree_files[2], self.holding)
        _stash(self.env.tree_files[3], self.holding)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            manifest = self.env.run(batch_mode=True)
        self.assertEqual(
            sorted(manifest.data["source_files"]), sorted([self._key(0), self._key(1)])
        )
        for entry in manifest.data["source_files"].values():
            self.assertEqual(entry["status"], SOURCE_COMPLETED)
        # a deferred entry is never recorded: it becomes pending the moment its
        # bytes arrive, so there is no state anybody has to remember to clear
        self.assertNotIn(self._key(2), manifest.data["source_files"])
        self.assertIn("2 of 4 inventory entries still deferred", err.getvalue())

    def test_completed_source_deleted_without_release_is_refused(self):
        """Consumption is an operator action, never inferred from a missing
        file: bytes deleted without ``release`` were never verified as safe to
        delete, so this must not be silently absorbed as consumed."""
        self.env.run(batch_mode=True)
        _stash(self.env.tree_files[1], self.holding)
        with self.assertRaises(ConverterError) as ctx:
            self.env.run(batch_mode=True)
        self.assertIn("never released", str(ctx.exception))
        self.assertIn(str(self.env.tree_files[1]), str(ctx.exception))
        self.assertEqual(self._status(1), SOURCE_COMPLETED)

    def test_consumed_source_satisfies_resume_without_stat_or_rescatter(self):
        self.env.run(batch_mode=True)
        run_release(self.env.workdir, [self.env.tree_files[0]])
        _stash(self.env.tree_files[0], self.holding)
        marker = self.env.workdir / "scratch" / "roots_src_0.npy"
        mtime_before = marker.stat().st_mtime_ns

        # the bytes are gone, so any stat of them would raise: the run
        # completing at all is the evidence that a consumed entry is not
        # re-stat-ed, and the untouched sidecar that it is not re-scattered
        manifest = self.env.run(batch_mode=True)
        self.assertEqual(marker.stat().st_mtime_ns, mtime_before)
        self.assertEqual(manifest.data["source_files"][self._key(0)]["status"], SOURCE_CONSUMED)

    def test_present_and_not_completed_is_still_scattered_in_both_modes(self):
        _stash(self.env.tree_files[3], self.holding)
        self.env.run(batch_mode=True)
        self.assertEqual(sorted(self._status(i) for i in range(3)), [SOURCE_COMPLETED] * 3)
        _restore(self.holding / self.env.tree_files[3].name, self.env.tree_files[3])
        self.env.run(batch_mode=True)
        self.assertEqual(self._status(3), SOURCE_COMPLETED)
        # and the non-batch path reaches the same state from scratch
        shutil.rmtree(self.env.workdir)
        self.env.run()
        self.assertEqual(sorted(self._status(i) for i in range(4)), [SOURCE_COMPLETED] * 4)

    def test_completed_and_unchanged_is_skipped_in_both_modes(self):
        for kwargs in ({"batch_mode": True}, {}):
            shutil.rmtree(self.env.workdir, ignore_errors=True)
            self.env.run(**kwargs)
            marker = self.env.workdir / "scratch" / "roots_src_0.npy"
            mtime_before = marker.stat().st_mtime_ns
            self.env.run(**kwargs)
            self.assertEqual(marker.stat().st_mtime_ns, mtime_before, kwargs)

    def test_changed_completed_source_refuses_resume_in_batch_mode(self):
        """A batch-mode run never finalizes, so ``snapshots`` stays empty for
        the whole cycle and the "changed after snapshots were finalized" guard
        cannot fire. Batch mode therefore has to refuse a changed completed
        source itself, or silent substitution would become possible in exactly
        the mode this slice adds."""
        self.env.run(batch_mode=True)
        self.assertEqual(Manifest.load_or_create(self.env.workdir).data["snapshots"], {})
        with open(self.env.tree_files[0], "a") as handle:
            handle.write("#tree 10199\n")
        with self.assertRaises(ConverterError) as ctx:
            self.env.run(batch_mode=True)
        self.assertIn("completed source file changed on disk", str(ctx.exception))
        self.assertIn(str(self.env.tree_files[0]), str(ctx.exception))

    def test_source_completed_answers_for_a_consumed_entry_without_stat(self):
        """``Manifest.source_completed`` is the resume predicate, and a
        consumed entry has to satisfy it without touching the filesystem —
        stat-ing released bytes is exactly what used to raise."""
        self.env.run(batch_mode=True)
        run_release(self.env.workdir, [self.env.tree_files[0]])
        _stash(self.env.tree_files[0], self.holding)
        manifest = Manifest.load_or_create(self.env.workdir)
        self.assertTrue(manifest.source_completed(self.env.tree_files[0]))
        self.assertEqual(
            manifest.classify_source(self.env.tree_files[0], batch_mode=True), SOURCE_CONSUMED
        )
        # a completed entry still has to prove its bytes are the scattered ones
        self.assertTrue(manifest.source_completed(self.env.tree_files[1]))
        with open(self.env.tree_files[1], "a") as handle:
            handle.write("#tree 10199\n")
        self.assertFalse(manifest.source_completed(self.env.tree_files[1]))

    # -- provenance guards, unchanged in batch mode -------------------------

    def test_all_deferred_first_batch_run_still_freezes_the_inventory(self):
        """A batch-mode scatter issued before any bytes have arrived is legal
        and ordinary — it is what a scripted transfer/scatter loop does on its
        first iteration, and what an operator smoke-testing the pipeline does.
        It classifies every entry as deferred and scatters nothing, so nothing
        forces a manifest save; the frozen inventory must reach disk anyway.
        Otherwise the anti-mixing guard is absent at exactly the moment it is
        supposed to be established, and the next invocation would accept a
        different conversion."""
        for path in self.env.tree_files:
            _stash(path, self.holding)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            manifest = self.env.run(batch_mode=True)
        self.assertIn(
            "scattered 0 file(s), 4 of 4 inventory entries still deferred", err.getvalue()
        )
        self.assertEqual(manifest.data["source_files"], {})

        persisted = json.loads((self.env.workdir / scatter.MANIFEST_NAME).read_text())
        self.assertEqual(
            persisted["provenance"]["source_files"],
            [str(p.resolve()) for p in self.env.tree_files],
        )
        for key in ("a_list", "forests_list", "simulation_info"):
            self.assertIn("md5", persisted["provenance"][key])

        original = list(self.env.tree_files)
        # a different membership is refused ...
        self.env.tree_files = original[:3]
        with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
            self.env.run(batch_mode=True)
        # ... and so is the same membership in a different order
        self.env.tree_files = list(reversed(original))
        with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
            self.env.run(batch_mode=True)
        # ... while the frozen inventory itself still resumes and proceeds
        self.env.tree_files = original
        for path in original:
            _restore(self.holding / path.name, path)
        manifest = self.env.run(batch_mode=True)
        self.assertEqual(len(manifest.data["source_files"]), 4)

    def test_batch_mode_freeze_costs_one_manifest_save_per_invocation(self):
        """Guards the item-7 interaction the fix above could have broken:
        persisting provenance on a batch-mode return must cost one
        whole-manifest save per INVOCATION, never one per source file. A
        per-file rewrite is quadratic in file count and is the exact cost
        Slice 1 exists to remove."""
        root = self.root / "five"
        root.mkdir()
        # five is the ceiling: standard_forests() has five entries and _split_forests
        # would hand a sixth file an empty forest group, which is not a valid
        # ctrees file at all
        env = ScatterEnv(root, n_files=5)
        real_save = Manifest.save
        calls = []

        def counting_save(manifest_self):
            calls.append(1)
            return real_save(manifest_self)

        # save_every_n_files far above the file count, so the policy's own count
        # arm never fires and every save observed here is the batch-mode freeze
        with mock.patch.object(Manifest, "save", counting_save):
            with contextlib.redirect_stderr(io.StringIO()):
                env.run(batch_mode=True, save_every_n_files=1000)
        self.assertEqual(len(calls), 1, "expected exactly one save for five scattered files")

        # and the same holds for an invocation that scatters nothing at all:
        # a fresh workdir whose sources have not arrived yet, which is the
        # all-deferred first run the fix above exists for
        fresh_root = self.root / "fresh"
        fresh_root.mkdir()
        fresh = ScatterEnv(fresh_root, n_files=5)
        for path in fresh.tree_files:
            _stash(path, fresh_root / "hold")
        del calls[:]
        with mock.patch.object(Manifest, "save", counting_save):
            with contextlib.redirect_stderr(io.StringIO()):
                fresh.run(batch_mode=True, save_every_n_files=1000)
        self.assertEqual(len(calls), 1, "the all-deferred freeze must be exactly one save")

    def test_partial_inventory_refuses_resume_in_batch_mode(self):
        """Passing only the subset currently on disk instead of the complete
        frozen inventory is the operator mistake batch mode must never absorb:
        the frozen-set guard has to keep comparing like with like."""
        _stash(self.env.tree_files[2], self.holding)
        _stash(self.env.tree_files[3], self.holding)
        self.env.run(batch_mode=True)
        self.env.tree_files = self.env.tree_files[:2]
        with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
            self.env.run(batch_mode=True)

    def test_reordered_inventory_refuses_resume_in_both_modes(self):
        self.env.run(batch_mode=True)
        self.env.tree_files = list(reversed(self.env.tree_files))
        with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
            self.env.run(batch_mode=True)
        with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
            self.env.run()

    def test_metadata_md5_guards_still_refuse_resume_in_batch_mode(self):
        self.env.run(batch_mode=True)

        original_a_list = Path(self.env.a_list).read_bytes()
        fixtures.write_a_list(self.env.a_list, [0.5, 0.6, 0.7, 0.8, 0.9, 0.99995])
        with self.assertRaisesRegex(ConverterError, "a_list content changed"):
            self.env.run(batch_mode=True)
        Path(self.env.a_list).write_bytes(original_a_list)

        original_forests = Path(self.env.forests_list).read_bytes()
        with open(self.env.forests_list, "a") as handle:
            handle.write("# annotated later\n")
        with self.assertRaisesRegex(ConverterError, "forests_list content changed"):
            self.env.run(batch_mode=True)
        Path(self.env.forests_list).write_bytes(original_forests)

        with open(self.env.sim_info, "a") as handle:
            handle.write("# annotated later\n")
        with self.assertRaisesRegex(ConverterError, "simulation_info content changed"):
            self.env.run(batch_mode=True)

    def test_pending_after_explicit_finalize_still_hits_the_snapshots_guard(self):
        """The pending-files-after-snapshots-finalized guard still fires on a
        workdir that was driven in batch mode and finalized explicitly."""
        self.env.run(batch_mode=True)
        run_finalize(self.env.workdir, self.env.forests_list)
        with open(self.env.tree_files[0], "a") as handle:
            handle.write("#tree 10199\n")
        with self.assertRaisesRegex(ConverterError, "after snapshots were finalized"):
            self.env.run()

    # -- finalize gating ----------------------------------------------------

    def test_batch_mode_never_finalizes_even_with_nothing_deferred(self):
        manifest = self.env.run(batch_mode=True)
        self.assertEqual(manifest.data["snapshots"], {})
        self.assertFalse((self.env.workdir / "forest_max_snap.npy").exists())
        self.assertFalse((self.env.workdir / "forest_index_table.npy").exists())
        # every worker intermediate survives, which is the whole reason batch
        # mode must not finalize: a later release has to verify these
        workers = [
            v for v in manifest.data["intermediates"].values() if v["kind"] == "worker-scratch"
        ]
        self.assertTrue(workers)
        self.assertTrue(all(v["status"] == "present" for v in workers))

    def test_outside_batch_mode_finalization_still_happens_automatically(self):
        manifest = self.env.run()
        self.assertTrue(manifest.data["snapshots"])
        self.assertTrue((self.env.workdir / "forest_max_snap.npy").exists())
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)

    def test_finalize_refuses_while_any_entry_is_deferred(self):
        stashed = _stash(self.env.tree_files[3], self.holding)
        self.env.run(batch_mode=True)
        with self.assertRaises(ConverterError) as ctx:
            run_finalize(self.env.workdir, self.env.forests_list)
        self.assertIn("still deferred", str(ctx.exception))
        self.assertIn(str(self.env.tree_files[3].resolve()), str(ctx.exception))
        self.assertEqual(Manifest.load_or_create(self.env.workdir).data["snapshots"], {})

        _restore(stashed, self.env.tree_files[3])
        self.env.run(batch_mode=True)
        manifest = run_finalize(self.env.workdir, self.env.forests_list)
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)

    def test_finalize_proceeds_and_covers_roots_with_every_source_consumed(self):
        """Root-coverage validation reads observed roots from the registered
        sidecars, so it still sees every source file's roots when not one
        source byte is left on disk."""
        self.env.run(batch_mode=True)
        run_release(self.env.workdir, self.env.tree_files)
        for path in self.env.tree_files:
            _stash(path, self.holding)
        manifest = run_finalize(self.env.workdir, self.env.forests_list)
        self.assertEqual(
            [e["status"] for e in manifest.data["source_files"].values()],
            [SOURCE_CONSUMED] * 4,
        )
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)
        table = np.load(self.env.workdir / "forest_index_table.npy")
        self.assertEqual(len(table), len(self.env.forests))

    def test_finalize_needs_a_consumed_source_s_roots_sidecar(self):
        """The counterpart to the test above: finalization of a fully consumed
        inventory succeeds only because the roots are read from the registered
        sidecars, so removing one has to stop it."""
        self.env.run(batch_mode=True)
        run_release(self.env.workdir, self.env.tree_files)
        for path in self.env.tree_files:
            _stash(path, self.holding)
        (self.env.workdir / "scratch" / "roots_src_2.npy").unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_finalize(self.env.workdir, self.env.forests_list)

    def test_finalize_refuses_a_changed_forests_list(self):
        self.env.run(batch_mode=True)
        with open(self.env.forests_list, "a") as handle:
            handle.write("# annotated later\n")
        with self.assertRaisesRegex(ConverterError, "forests_list content changed"):
            run_finalize(self.env.workdir, self.env.forests_list)

    def test_finalize_without_scatter_provenance_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "no scatter provenance"):
            run_finalize(self.env.workdir, self.env.forests_list)

    def test_finalize_refuses_a_source_that_is_present_but_never_scattered(self):
        """A file whose bytes arrived but which no batch has scattered yet is
        not deferred and must not pass finalization either."""
        _stash(self.env.tree_files[3], self.holding)
        self.env.run(batch_mode=True)
        _restore(self.holding / self.env.tree_files[3].name, self.env.tree_files[3])
        with self.assertRaisesRegex(ConverterError, "source file incomplete after scatter"):
            run_finalize(self.env.workdir, self.env.forests_list)

    # -- release refusals ---------------------------------------------------

    def test_release_refuses_an_entry_that_is_not_completed(self):
        _stash(self.env.tree_files[3], self.holding)
        self.env.run(batch_mode=True)

        # deferred: never scattered, so never recorded
        with self.assertRaisesRegex(ConverterError, "not a recorded source file"):
            run_release(self.env.workdir, [self.env.tree_files[3]])

        # some other recorded status is refused by name
        manifest = Manifest.load_or_create(self.env.workdir)
        manifest.data["source_files"][self._key(1)]["status"] = "partial"
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "status is 'partial'"):
            run_release(self.env.workdir, [self.env.tree_files[1]])

        # and a second release of the same file is refused, not silently redone
        run_release(self.env.workdir, [self.env.tree_files[0]])
        with self.assertRaisesRegex(ConverterError, "already recorded as 'consumed'"):
            run_release(self.env.workdir, [self.env.tree_files[0]])

    def test_release_refuses_a_tampered_intermediate(self):
        self.env.run(batch_mode=True)
        _flip_last_byte(self.env.workdir / "scratch" / "roots_src_0.npy")
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_release(self.env.workdir, [self.env.tree_files[0]])
        self.assertEqual(self._status(0), SOURCE_COMPLETED)

    def test_release_refuses_a_missing_worker_intermediate(self):
        manifest = self.env.run(batch_mode=True)
        entry = manifest.data["source_files"][self._key(0)]
        snap = sorted(entry["per_snapshot_counts"], key=int)[0]
        (self.env.workdir / "scratch" / worker_scratch_name(int(snap), 0)).unlink()
        with self.assertRaisesRegex(ConverterError, "missing on disk"):
            run_release(self.env.workdir, [self.env.tree_files[0]])
        self.assertEqual(self._status(0), SOURCE_COMPLETED)

    def test_release_refuses_an_unregistered_intermediate(self):
        """The manifest is the ownership record, so an intermediate it does not
        name cannot be vouched for — release must refuse rather than treat an
        unknown artifact as verified."""
        manifest = self.env.run(batch_mode=True)
        entry = manifest.data["source_files"][self._key(0)]
        snap = sorted(entry["per_snapshot_counts"], key=int)[0]
        worker = self.env.workdir / "scratch" / worker_scratch_name(int(snap), 0)
        manifest = Manifest.load_or_create(self.env.workdir)
        del manifest.data["intermediates"][str(worker.resolve())]
        manifest.save()
        with self.assertRaisesRegex(ConverterError, "never registered as an intermediate"):
            run_release(self.env.workdir, [self.env.tree_files[0]])
        self.assertEqual(self._status(0), SOURCE_COMPLETED)

    def test_release_refuses_a_source_whose_bytes_no_longer_match(self):
        self.env.run(batch_mode=True)
        with open(self.env.tree_files[0], "a") as handle:
            handle.write("#tree 10199\n")
        with self.assertRaisesRegex(ConverterError, "no longer match the scattered source"):
            run_release(self.env.workdir, [self.env.tree_files[0]])
        self.assertEqual(self._status(0), SOURCE_COMPLETED)

    def test_release_of_already_deleted_bytes_is_the_documented_recovery(self):
        """Deleting before releasing leaves a workdir that refuses to resume;
        release is the way out, so it must still work with the bytes gone —
        the intermediates are what it verifies, not the source."""
        self.env.run(batch_mode=True)
        _stash(self.env.tree_files[0], self.holding)
        run_release(self.env.workdir, [self.env.tree_files[0]])
        self.assertEqual(self._status(0), SOURCE_CONSUMED)
        self.env.run(batch_mode=True)

    def test_release_is_atomic_across_the_files_it_is_given(self):
        self.env.run(batch_mode=True)
        _flip_last_byte(self.env.workdir / "scratch" / "roots_src_1.npy")
        with self.assertRaisesRegex(ConverterError, "checksum"):
            run_release(self.env.workdir, self.env.tree_files[:2])
        # the file that verified cleanly was transitioned only in memory
        self.assertEqual(self._status(0), SOURCE_COMPLETED)
        self.assertEqual(self._status(1), SOURCE_COMPLETED)


class TestBatchedCycleEquivalence(unittest.TestCase):
    """Slice 3 (item 3): a conversion driven as ``transfer batch -> scatter ->
    release -> transfer next batch -> scatter -> release -> finalize`` must
    emit exactly what one all-at-once run emits.

    Both conversions run over the *same* source files at the same paths, so
    every per-source manifest field — size, ``mtime_ns``, md5, counts,
    checksums, observed pairs — is directly comparable; only the workdir
    differs. The batched run holds at most half of the inventory on disk at
    any moment and finalizes with none of it, which is the property the
    production transfer depends on.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.env = ScatterEnv(self.root, n_files=4)
        self.holding = self.root / "not_yet_transferred"
        self.batched_workdir = self.root / "workdir_batched"

    def tearDown(self):
        self.tmp.cleanup()

    def _run_batched_scatter(self):
        return run_scatter(
            tree_files=self.env.tree_files,
            forests_list_path=self.env.forests_list,
            a_list_path=self.env.a_list,
            workdir=self.batched_workdir,
            simulation_info_path=self.env.sim_info,
            batch_mode=True,
        )

    def _emit_dataset(self, workdir: Path):
        """Run everything downstream of scatter and return the emitted files."""
        run_sort(workdir)
        run_fixups(workdir, a_list_path=self.env.a_list, simulation_info_path=self.env.sim_info)
        run_links(workdir)
        run_write(workdir, a_list_path=self.env.a_list, simulation_info_path=self.env.sim_info)
        return sorted((workdir / "hdf5").glob("*.h5"))

    @staticmethod
    def _normalise(obj, frm: str):
        """Replace one workdir prefix with a placeholder everywhere in a
        manifest so two manifests written to different workdirs compare on
        content instead of on where they live."""
        if isinstance(obj, dict):
            return {
                (
                    key.replace(frm, "<WORKDIR>") if isinstance(key, str) else key
                ): TestBatchedCycleEquivalence._normalise(value, frm)
                for key, value in obj.items()
            }
        if isinstance(obj, list):
            return [TestBatchedCycleEquivalence._normalise(v, frm) for v in obj]
        if isinstance(obj, str):
            return obj.replace(frm, "<WORKDIR>")
        return obj

    def test_batched_cycle_emits_the_same_dataset_as_one_run(self):
        # (1) the oracle: one all-at-once conversion over the whole inventory
        self.env.run()
        single_files = self._emit_dataset(self.env.workdir)
        self.assertTrue(single_files)

        # (2) batch 1 has arrived; batch 2 has not
        batch2_stashed = [_stash(p, self.holding) for p in self.env.tree_files[2:]]
        manifest = self._run_batched_scatter()
        self.assertEqual(manifest.data["snapshots"], {})
        run_release(self.batched_workdir, self.env.tree_files[:2])
        # batch 1's bytes stay stashed for the rest of the cycle: this is the
        # operator deleting them, which is the whole point of releasing first
        for path in self.env.tree_files[:2]:
            _stash(path, self.holding)

        # (3) batch 1's bytes are gone; batch 2 arrives
        for stashed, original in zip(batch2_stashed, self.env.tree_files[2:]):
            _restore(stashed, original)
        manifest = self._run_batched_scatter()
        self.assertEqual(manifest.data["snapshots"], {})
        run_release(self.batched_workdir, self.env.tree_files[2:])
        for path in self.env.tree_files[2:]:
            _stash(path, self.holding)

        # (4) finalize with not one source byte on disk
        batched = run_finalize(self.batched_workdir, self.env.forests_list)
        self.assertEqual(
            [e["status"] for e in batched.data["source_files"].values()],
            [SOURCE_CONSUMED] * 4,
        )
        batched_files = self._emit_dataset(self.batched_workdir)

        # the emitted dataset is byte-identical, file for file
        self.assertEqual([p.name for p in batched_files], [p.name for p in single_files])
        for single, batch in zip(single_files, batched_files):
            self.assertEqual(
                single.read_bytes(), batch.read_bytes(), "{} differs".format(single.name)
            )

        # the manifest is identical in provenance and every per-source content
        # field; only the lifecycle state legitimately differs
        single_manifest = json.loads((self.env.workdir / scatter.MANIFEST_NAME).read_text())
        batched_manifest = json.loads((self.batched_workdir / scatter.MANIFEST_NAME).read_text())
        self.assertEqual(single_manifest["provenance"], batched_manifest["provenance"])
        self.assertEqual(
            set(single_manifest["source_files"]), set(batched_manifest["source_files"])
        )
        for key, expected in single_manifest["source_files"].items():
            got = batched_manifest["source_files"][key]
            self.assertEqual(expected["status"], SOURCE_COMPLETED)
            self.assertEqual(got["status"], SOURCE_CONSUMED)
            self.assertEqual(
                {k: v for k, v in got.items() if k != "status"},
                {k: v for k, v in expected.items() if k != "status"},
                key,
            )

        # and everything else the manifest records matches once the workdir
        # prefix is normalised away
        single_rest = self._normalise(single_manifest, str(self.env.workdir.resolve()))
        batched_rest = self._normalise(batched_manifest, str(self.batched_workdir.resolve()))
        del single_rest["source_files"], batched_rest["source_files"]
        self.assertEqual(single_rest, batched_rest)

    def test_batched_cycle_never_holds_more_than_one_batch_on_disk(self):
        """Guards the property the test above depends on: if the fixture ever
        let both batches be present at once, the equivalence test would stop
        exercising the interleaved transfer and nothing would say so."""
        batch2_stashed = [_stash(p, self.holding) for p in self.env.tree_files[2:]]
        self.assertEqual([p.exists() for p in self.env.tree_files], [True, True, False, False])
        self._run_batched_scatter()
        run_release(self.batched_workdir, self.env.tree_files[:2])
        for path in self.env.tree_files[:2]:
            _stash(path, self.holding)
        for stashed, original in zip(batch2_stashed, self.env.tree_files[2:]):
            _restore(stashed, original)
        self.assertEqual([p.exists() for p in self.env.tree_files], [False, False, True, True])


class TestBatchModeCli(unittest.TestCase):
    """Slice 3 (item 3): the two new operator subcommands and the batch-mode
    selector, exercised through the CLI so the wiring is covered too — a
    subcommand that is not reachable from ``convert_ctrees`` is not an operator
    action, whatever the library does."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.env = ScatterEnv(self.root, n_files=3)
        self.holding = self.root / "not_yet_transferred"

    def tearDown(self):
        self.tmp.cleanup()

    def _scatter_argv(self, batch: bool):
        argv = [
            "scatter",
            "--workdir",
            str(self.env.workdir),
            "--forests-list",
            str(self.env.forests_list),
            "--a-list",
            str(self.env.a_list),
            "--simulation-info",
            str(self.env.sim_info),
        ]
        if batch:
            argv.append("--batch")
        # the complete frozen inventory, every time
        return argv + [str(p) for p in self.env.tree_files]

    def test_batch_flag_defaults_off(self):
        args = convert_ctrees.build_arg_parser().parse_args(self._scatter_argv(batch=False))
        self.assertFalse(args.batch)
        args = convert_ctrees.build_arg_parser().parse_args(self._scatter_argv(batch=True))
        self.assertTrue(args.batch)

    def test_cli_batch_cycle_release_then_finalize(self):
        _stash(self.env.tree_files[2], self.holding)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(convert_ctrees.main(self._scatter_argv(batch=True)), 0)
            self.assertEqual(Manifest.load_or_create(self.env.workdir).data["snapshots"], {})
            self.assertEqual(
                convert_ctrees.main(
                    [
                        "release",
                        "--workdir",
                        str(self.env.workdir),
                        str(self.env.tree_files[0]),
                        str(self.env.tree_files[1]),
                    ]
                ),
                0,
            )
            for path in self.env.tree_files[:2]:
                _stash(path, self.holding)

            # finalize is refused while the last entry is still deferred
            finalize_argv = [
                "finalize",
                "--workdir",
                str(self.env.workdir),
                "--forests-list",
                str(self.env.forests_list),
            ]
            self.assertEqual(convert_ctrees.main(finalize_argv), 1)

            _restore(self.holding / self.env.tree_files[2].name, self.env.tree_files[2])
            self.assertEqual(convert_ctrees.main(self._scatter_argv(batch=True)), 0)
            self.assertEqual(convert_ctrees.main(finalize_argv), 0)

        manifest = Manifest.load_or_create(self.env.workdir)
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            self.assertEqual(manifest.data["snapshots"][str(snap)]["rows"], count)
        statuses = [e["status"] for e in manifest.data["source_files"].values()]
        self.assertEqual(sorted(statuses), [SOURCE_COMPLETED, SOURCE_CONSUMED, SOURCE_CONSUMED])

    def test_cli_release_refusal_exits_nonzero(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(convert_ctrees.main(self._scatter_argv(batch=True)), 0)
            _flip_last_byte(self.env.workdir / "scratch" / "roots_src_0.npy")
            self.assertEqual(
                convert_ctrees.main(
                    ["release", "--workdir", str(self.env.workdir), str(self.env.tree_files[0])]
                ),
                1,
            )
        manifest = Manifest.load_or_create(self.env.workdir)
        self.assertEqual(
            manifest.data["source_files"][str(self.env.tree_files[0].resolve())]["status"],
            SOURCE_COMPLETED,
        )

    def test_cli_scatter_without_batch_still_finalizes(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(convert_ctrees.main(self._scatter_argv(batch=False)), 0)
        manifest = Manifest.load_or_create(self.env.workdir)
        self.assertTrue(manifest.data["snapshots"])
