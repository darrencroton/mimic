"""Unit tests: Phase 0 map, root coverage, ForestIndex order, scatter
conservation, manifest resume, aggregates, cleanup containment guard.

Every scatter runs under a profile schema: the shipped default profile unless a
test names another. ``TestExtendedScatter`` covers the layout of a profile that
declares extras (binding, source-unit sidecars, serial/pooled equality)."""

import functools
import json
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

import fixtures  # noqa: E402
import scatter  # noqa: E402
from column_schema import build_schema, load_column_map  # noqa: E402
from ctrees_parser import ConverterError, CtreesFileParser, ScratchLayout  # noqa: E402
from fixtures import (  # noqa: E402
    ALL_TYPES_PROFILE,
    DEFAULT_PROFILE,
    PROFILE_DIR,
)
from scatter import (  # noqa: E402
    SOURCE_COMPLETED,
    SOURCE_PENDING,
    Manifest,
    load_a_list,
    load_forests_list,
    run_scatter,
    snapshot_scratch_name,
    validate_observed_pairs,
    validate_root_coverage,
)


@functools.lru_cache(maxsize=None)
def _schema(path=ALL_TYPES_PROFILE):
    """The canonical schema of a profile, built once per profile."""
    return build_schema(load_column_map(path))


def default_schema():
    return _schema(DEFAULT_PROFILE)


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
    """Move a source file out of sight, so its bytes are absent from disk.

    A rename inside one filesystem keeps the inode, so the file keeps the
    identical size and ``mtime_ns`` the frozen manifest recorded — which is
    what makes it a faithful stand-in for "these bytes are not here" rather
    than for "these bytes changed"."""
    holding.mkdir(parents=True, exist_ok=True)
    dest = holding / path.name
    path.rename(dest)
    return dest


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

    @property
    def schema(self):
        return default_schema()

    @property
    def dtype(self):
        return ScratchLayout.from_schema(self.schema).dtype

    def run(self, **kwargs):
        kwargs.setdefault("schema", self.schema)
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
            data = np.fromfile(scratch / snapshot_scratch_name(snap), dtype=self.env.dtype)
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

    def test_a_linked_snapshot_is_refused_before_anything_is_created(self):
        self.env.run()
        workdir = self.env.workdir
        path = workdir / "manifest.json"
        data = json.loads(path.read_text())
        linked = min(int(snap) for snap in data["snapshots"])
        data["snapshots"][str(linked)]["status"] = "linked"
        path.write_text(json.dumps(data))
        before = {p: p.read_bytes() for p in workdir.rglob("*") if p.is_file()}
        with self.assertRaisesRegex(
            ConverterError, "snapshot {}: unexpected status 'linked'".format(linked)
        ):
            self.env.run()
        self.assertEqual({p: p.read_bytes() for p in workdir.rglob("*") if p.is_file()}, before)

    def test_missing_input_file_aborts_before_any_metadata_load(self):
        holding = Path(self.tmp.name) / "holding"
        _stash(self.env.tree_files[1], holding)
        with self.assertRaises(ConverterError) as ctx:
            self.env.run()
        self.assertIn("input tree file does not exist", str(ctx.exception))
        self.assertIn(str(self.env.tree_files[1]), str(ctx.exception))
        # nothing was scattered: the run aborted before any work
        self.assertFalse((self.env.workdir / scatter.MANIFEST_NAME).exists())
        # and it aborts there before the metadata loads: with the a_list
        # unreadable as well, the missing source file is still what is reported
        self.env.a_list = Path(self.tmp.name) / "no_such.a_list"
        with self.assertRaises(ConverterError) as ctx:
            self.env.run()
        self.assertIn("input tree file does not exist", str(ctx.exception))

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
        # is independent of the save policy tested elsewhere.
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

        manifest = Manifest.load(self.env.workdir)
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
        self.assertFalse((self.env.workdir / "manifest.json").exists())

        # re-run: 'wb' truncation must absorb the partial files; totals exact
        manifest = self.env.run(chunksize=2)
        expected = self.env.expected_snapshot_counts()
        for snap, count in expected.items():
            entry = manifest.data["snapshots"][str(snap)]
            self.assertEqual(entry["rows"], count)
            data = np.fromfile(scratch / snapshot_scratch_name(snap), dtype=self.env.dtype)
            self.assertEqual(len(data), count)

    def test_changed_input_content_refuses_resume(self):
        """A resume whose a_list, forests.list or simulation_info content differs
        from the recorded one is refused."""

        def edit_a_list(env):
            fixtures.write_a_list(env.a_list, [0.5, 0.6, 0.7, 0.8, 0.9, 0.99995])

        def annotate(attr):
            def edit(env):
                with open(getattr(env, attr), "a") as handle:
                    handle.write("# annotated later\n")

            return edit

        for label, edit, message in (
            ("a_list", edit_a_list, "a_list content changed"),
            ("forests_list", annotate("forests_list"), "forests_list content changed"),
            ("simulation_info", annotate("sim_info"), "simulation_info content changed"),
        ):
            with self.subTest(label):
                root = Path(self.tmp.name) / label
                root.mkdir()
                env = ScatterEnv(root)
                env.run()
                edit(env)
                with self.assertRaisesRegex(ConverterError, message):
                    env.run()

    def test_changed_or_reordered_source_set_refuses_resume(self):
        """A resume over an added source file or over the same files in another
        order is refused."""
        extra = fixtures.ForestSpec(
            forest_id=999,
            trees=[
                fixtures.TreeSpec(
                    root_id=9990, halos=[fixtures.HaloSpec(halo_id=9990, snap=5, mvir=1e11)]
                )
            ],
        )
        extra_file = fixtures.write_ctrees_file(Path(self.tmp.name) / "extra.dat", extra.trees)
        for label, change in (
            ("added file", lambda files: files + [extra_file]),
            ("reordered files", lambda files: list(reversed(files))),
        ):
            with self.subTest(label):
                root = Path(self.tmp.name) / label.replace(" ", "_")
                root.mkdir()
                env = ScatterEnv(root)
                env.run()
                env.tree_files = change(env.tree_files)
                with self.assertRaisesRegex(ConverterError, "source file set/order changed"):
                    env.run()

    def test_classify_source_refuses_a_completed_source_missing_from_disk(self):
        """A completed source whose bytes are gone is refused, never absorbed:
        nothing could then verify that the intermediates it produced are still
        what was scattered. A resume that does not re-enter scatter (the ASCII
        adapter's ``_check_recorded_inputs``) relies on this classification."""
        self.env.run()
        manifest = Manifest.load(self.env.workdir)
        self.assertEqual(manifest.classify_source(self.env.tree_files[0]), SOURCE_COMPLETED)
        _stash(self.env.tree_files[0], Path(self.tmp.name) / "holding")
        with self.assertRaises(ConverterError) as ctx:
            manifest.classify_source(self.env.tree_files[0])
        self.assertIn("completed source file is missing from disk", str(ctx.exception))
        self.assertIn(str(self.env.tree_files[0]), str(ctx.exception))

    def test_source_completed_requires_the_scattered_bytes(self):
        """``Manifest.source_completed`` is the resume predicate: a completed
        entry satisfies it only while the bytes on disk match the recorded size
        and mtime, and a changed one is classified pending."""
        self.env.run()
        manifest = Manifest.load(self.env.workdir)
        self.assertTrue(manifest.source_completed(self.env.tree_files[1]))
        with open(self.env.tree_files[1], "a") as handle:
            handle.write("#tree 10199\n")
        self.assertFalse(manifest.source_completed(self.env.tree_files[1]))
        self.assertEqual(manifest.classify_source(self.env.tree_files[1]), SOURCE_PENDING)

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

    def test_tampered_sidecar_detected_on_resume(self):
        """A flipped byte in a completed file's observed-roots or forest-max-snap
        sidecar is caught by its checksum when the resume skip-trusts that file."""
        for sidecar, message in (
            ("roots_src_0.npy", "observed-roots sidecar.*checksum"),
            ("forest_max_src_0.npy", "forest-max-snap sidecar.*checksum"),
        ):
            with self.subTest(sidecar):
                root = Path(self.tmp.name) / sidecar
                root.mkdir()
                self.env = ScatterEnv(root)
                good_text = self._crash_between_files()
                _flip_last_byte(self.env.workdir / "scratch" / sidecar)
                self.env.tree_files[1].write_text(good_text)
                with self.assertRaisesRegex(ConverterError, message):
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
        manifest = Manifest.load(self.env.workdir)
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


class TestForestMapWorkerDistribution(unittest.TestCase):
    """The forest map reaches each worker once per worker
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
        # An uncaught
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
        # The mismatch test
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


class TestBatchedManifestSave(unittest.TestCase):
    """Bounded-interval manifest persistence in run_scatter."""

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

    def test_resume_after_unsaved_crash_overwrites_and_matches_clean_manifest(self):
        baseline_bytes = self._clean_baseline_manifest_bytes()
        scratch = self.env.workdir / "scratch"

        CrashOnNamedFileParser.crash_on_filename = self.env.tree_files[1].name
        with mock.patch("scatter.CtreesFileParser", CrashOnNamedFileParser):
            with self.assertRaisesRegex(ConverterError, "simulated crash"):
                self.env.run()
        # binding invariant: no manifest was written that could name an
        # artifact not durably written -- here, none was written at all
        self.assertFalse((self.env.workdir / "manifest.json").exists())
        # but file 0's worker/sidecar artifacts were durably written already
        self.assertTrue(next(scratch.glob("snap_*.src_0.bin")).exists())
        self.assertTrue((scratch / "forest_max_src_0.npy").exists())
        # roots_src_0.npy survives the whole run (unlike worker-scratch,
        # which finalize deletes after concat), so it can be checked both
        # before and after resume completes
        roots_marker = scratch / "roots_src_0.npy"
        self.assertTrue(roots_marker.exists())
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
        # not. Four files + save_every_n_files=2 gives: file 0 completes
        # (1 <2, no save), file 1 completes (2 >=2, SAVE covers files 0+1,
        # counter resets), file 2 completes (1 <2, no save), file 3 crashes
        # before completing -- so file 2 is the mixed state: durably written
        # but never saved, sitting between the save after file 1 and the save
        # that would have followed file 3.
        four_root = Path(self.tmp.name) / "four"
        four_root.mkdir()
        env = ScatterEnv(four_root, n_files=4)
        baseline_bytes = self._clean_baseline_manifest_bytes(env)
        scratch = env.workdir / "scratch"

        CrashOnNamedFileParser.crash_on_filename = env.tree_files[3].name
        with mock.patch("scatter.CtreesFileParser", CrashOnNamedFileParser):
            with self.assertRaisesRegex(ConverterError, "simulated crash"):
                env.run(save_every_n_files=2)

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
        self.manifest = Manifest(self.workdir, ScratchLayout.from_schema(default_schema()))

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


class SpanningEnv(ScatterEnv):
    """Two files where forest 100's two trees are split across the files and
    file 0 interleaves two forests' trees -- so units are neither whole forests
    nor contiguous row ranges."""

    def __init__(self, root: Path):
        self.root = root
        self.workdir = root / "workdir"
        self.forests = fixtures.standard_forests()
        multi, satellite, early, zero_mass, sub = self.forests
        file0 = [multi.trees[0], satellite.trees[0], early.trees[0]]
        file1 = [zero_mass.trees[0], multi.trees[1], sub.trees[0]]
        self.tree_files = [
            fixtures.write_ctrees_file(root / "tree_0.dat", file0),
            fixtures.write_ctrees_file(root / "tree_1.dat", file1),
        ]
        self.forests_list = fixtures.write_forests_list(root / "forests.list", self.forests)
        self.a_list = fixtures.write_a_list(root / "test.a_list")
        self.sim_info = fixtures.write_simulation_info(root / "simulation_info.yaml")


class TestExtendedScatter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_manifest_records_the_scratch_layout(self):
        env = SpanningEnv(self.root)
        manifest = env.run(schema=_schema(), chunksize=2)
        layout = ScratchLayout.from_schema(_schema())
        data = json.loads((env.workdir / "manifest.json").read_text())
        self.assertEqual(data["dtype_tag"], layout.dtype_tag)
        self.assertEqual(data["scratch_layout"], layout.to_record())
        self.assertEqual(manifest.layout, layout)
        self.assertEqual(Manifest.load(env.workdir).layout, layout)
        worker_entries = [
            meta for meta in data["intermediates"].values() if meta["kind"] == "worker-scratch"
        ]
        self.assertTrue(worker_entries)
        for meta in worker_entries:
            self.assertEqual(meta["dtype_tag"], layout.dtype_tag)
        for entry in data["snapshots"].values():
            meta = data["intermediates"][entry["scratch_file"]]
            self.assertEqual(meta["dtype_tag"], layout.dtype_tag)
            records = np.fromfile(entry["scratch_file"], dtype=layout.dtype)
            self.assertEqual(records.size, entry["rows"])

    def test_unit_sidecars_are_literal_and_registered(self):
        env = SpanningEnv(self.root)
        manifest = env.run(schema=_schema(), chunksize=3)
        scratch_dir = env.workdir / "scratch"
        # file 0: trees 101 (forest 100, 4 rows), 201 (200, 4), 401 (400, 2)
        # file 1: trees 501 (500, 2), 102 (100, 2), 601 (600, 3)
        expected = {
            0: [[100, 4], [200, 4], [400, 2]],
            1: [[500, 2], [100, 2], [600, 3]],
        }
        for src_index, table in expected.items():
            path = scratch_dir / scatter.source_units_name(src_index)
            self.assertEqual(np.load(path).tolist(), table)
            meta = manifest.data["intermediates"][str(path.resolve())]
            self.assertEqual((meta["kind"], meta["rows"]), ("source-units", len(table)))

    def test_forest_join_and_extras_survive_chunk_and_tree_boundaries(self):
        env = SpanningEnv(self.root)
        manifest = env.run(schema=_schema(), chunksize=1)
        got = {}
        for entry in manifest.data["snapshots"].values():
            for r in np.fromfile(entry["scratch_file"], dtype=manifest.layout.dtype):
                got[int(r["id"])] = (
                    int(r["tree_root_id"]),
                    int(r["forest_id"]),
                    int(r["extra_NumProg"]),
                )
        expected = {
            halo.halo_id: (tree.root_id, forest.forest_id, halo.num_prog)
            for forest in env.forests
            for tree in forest.trees
            for halo in tree.halos
        }
        self.assertEqual(got, expected)

    def test_serial_and_pooled_extended_scratch_are_identical(self):
        outputs = []
        for pool_size in (1, 2):
            root = self.root / "pool{}".format(pool_size)
            root.mkdir()
            env = SpanningEnv(root)
            manifest = env.run(schema=_schema(), pool_size=pool_size, chunksize=2)
            outputs.append(
                {
                    snap: Path(entry["scratch_file"]).read_bytes()
                    for snap, entry in manifest.data["snapshots"].items()
                }
            )
            outputs[-1]["units"] = [
                (env.workdir / "scratch" / scatter.source_units_name(i)).read_bytes()
                for i in range(2)
            ]
        self.assertEqual(outputs[0], outputs[1])

    def test_resume_with_the_same_schema_is_a_skip(self):
        env = SpanningEnv(self.root)
        env.run(schema=_schema())
        before = (env.workdir / "manifest.json").read_text()
        env.run(schema=_schema())
        self.assertEqual((env.workdir / "manifest.json").read_text(), before)

    def test_layout_changes_are_refused_before_mutation(self):
        env = SpanningEnv(self.root)
        env.run(schema=_schema())
        before = (env.workdir / "manifest.json").read_text()
        default = _schema(PROFILE_DIR / "consistent_trees_ascii.yaml")
        with open(ALL_TYPES_PROFILE) as handle:
            relabelled = handle.read().replace("units: kpc/h", "units: Mpc/h")
        same_width = self.root / "same_width.yaml"
        same_width.write_text(relabelled)
        for label, schema in (
            ("different selection", default),
            ("same width, different units", _schema(same_width)),
        ):
            with self.subTest(label):
                with self.assertRaisesRegex(ConverterError, "refusing to resume"):
                    env.run(schema=schema)
                self.assertEqual((env.workdir / "manifest.json").read_text(), before)

    def test_a_manifest_without_a_layout_record_is_refused(self):
        env = SpanningEnv(self.root)
        env.run(schema=_schema())
        path = env.workdir / "manifest.json"
        data = json.loads(path.read_text())
        del data["scratch_layout"]
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ConverterError, "records no scratch layout"):
            Manifest.load(env.workdir)

    def test_a_missing_manifest_is_refused_on_load(self):
        with self.assertRaisesRegex(ConverterError, "no manifest found; run scatter first"):
            Manifest.load(self.root / "nowhere")

    def test_a_manifest_whose_layout_record_and_tag_disagree_is_refused(self):
        """A recorded layout that no longer describes the recorded dtype tag, and a
        recorded tag that no longer matches its layout, are both refused."""

        def retype_first_extra(data):
            data["scratch_layout"]["extras"][0][1] = "double"

        def replace_tag(data):
            data["dtype_tag"] = "something-else"

        env = SpanningEnv(self.root)
        env.run(schema=_schema())
        path = env.workdir / "manifest.json"
        pristine = path.read_text()
        for label, tamper, message in (
            ("layout record", retype_first_extra, "does not match the tag"),
            ("dtype tag", replace_tag, "disagrees with its own scratch layout"),
        ):
            with self.subTest(label):
                data = json.loads(pristine)
                tamper(data)
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(ConverterError, message):
                    Manifest.load(env.workdir)


if __name__ == "__main__":
    unittest.main()
