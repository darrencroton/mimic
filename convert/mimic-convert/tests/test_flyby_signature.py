"""Unit tests: the flyby-demotion signature and the ingest guard
(convert/mimic-convert/flyby_signature.py, ``pipeline.run_ingest``), and the
horizontal check the vertical writer runs (``count_horizontal_signature``).

**Oracle.** Every source is a synthetic L-Halo file packed by hand with
``fixtures.write_lhalo_file`` (the independent ``struct`` packer), and every
expected count is a literal read off the trees below, never the code's own
arithmetic. The signature is a halo with ``MostBoundID < 0`` that is not its
own FoF central and lies at its tree's last snapshot; the known-good trees
carry a negative id that misses exactly one of those conditions.

The horizontal check's datasets are :mod:`fixtures`' correspondence forests
converted to version 3, with ``MostBoundID`` negated on literal rows; its
expected counts are read off the FoF columns quoted in
:class:`HorizontalSignatureTests`.
"""

import contextlib
import dataclasses
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import h5py
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
import conversion_manifest as cm  # noqa: E402
import fixtures  # noqa: E402
import flyby_signature  # noqa: E402
import pipeline  # noqa: E402
from adapters.lhalo_binary import TOPOLOGY_READ_CHUNK_ROWS  # noqa: E402
from column_schema import ConverterError  # noqa: E402
from fixtures import (  # noqa: E402
    LITERAL_A_LIST_TEXT,
    LITERAL_BUDGET,
    LITERAL_TREE_A,
    LITERAL_TREE_B,
    LITERAL_TREE_C,
    PROFILE_DIR,
    convert_one_file_dataset,
    lhalo_schema,
    negate_most_bound_id,
)
from horizontal_dataset import DEFAULT_BLOCK_ROWS, HorizontalDataset  # noqa: E402

# ==========================================================================
# Hand-laid trees (tree-local rows; LITERAL_A_LIST_TEXT gives snapshots 0-3)
# ==========================================================================

#: Known bad, count 2: rows 1 and 2 are satellites of row 0 at the tree's last
#: snapshot (3) with negative ids. Row 3 is a central with a negative id and
#: row 4 a satellite with a negative id below the last snapshot: neither counts.
BAD_TREE = [
    dict(SnapNum=3, FirstHaloInFOFgroup=0, NextHaloInFOFgroup=1, MostBoundID=21),
    dict(SnapNum=3, FirstHaloInFOFgroup=0, NextHaloInFOFgroup=2, MostBoundID=-22),
    dict(SnapNum=3, FirstHaloInFOFgroup=0, MostBoundID=-23),
    dict(SnapNum=2, FirstHaloInFOFgroup=3, NextHaloInFOFgroup=4, MostBoundID=-24),
    dict(SnapNum=2, FirstHaloInFOFgroup=3, MostBoundID=-25),
]
BAD_TREE_COUNT = 2

#: Known bad, count 1: the tree ends at snapshot 2, below the catalogue's last
#: snapshot, and its last snapshot is what the signature is defined on.
EARLY_BAD_TREE = [
    dict(SnapNum=2, FirstHaloInFOFgroup=0, NextHaloInFOFgroup=1, MostBoundID=31),
    dict(SnapNum=2, FirstHaloInFOFgroup=0, MostBoundID=-32),
]
EARLY_BAD_TREE_COUNT = 1

#: Known good: BAD_TREE's structure with no negative id.
NO_NEGATIVE_TREE = [dict(row, MostBoundID=abs(row["MostBoundID"])) for row in BAD_TREE]

#: Known good: the only negative id is on a central at the last snapshot.
NEGATIVE_CENTRAL_TREE = [
    dict(SnapNum=3, FirstHaloInFOFgroup=0, NextHaloInFOFgroup=1, MostBoundID=-41),
    dict(SnapNum=3, FirstHaloInFOFgroup=0, MostBoundID=42),
]

#: Known good: the only negative id is on a satellite below the last snapshot.
NEGATIVE_EARLY_SATELLITE_TREE = [
    dict(SnapNum=3, FirstHaloInFOFgroup=0, MostBoundID=51),
    dict(SnapNum=2, FirstHaloInFOFgroup=1, NextHaloInFOFgroup=2, MostBoundID=52),
    dict(SnapNum=2, FirstHaloInFOFgroup=1, MostBoundID=-53),
]


def columns(tree):
    """The three signature columns of a hand-laid tree, with the packer's
    defaults for anything the tree leaves out."""
    return (
        [row.get("SnapNum", 0) for row in tree],
        [row.get("FirstHaloInFOFgroup", 0) for row in tree],
        [row.get("MostBoundID", 0) for row in tree],
    )


def ingest_state(work):
    """The ingest stage record and the raw manifest bytes."""
    path = Path(work) / cm.MANIFEST_NAME
    manifest = json.loads(path.read_text())
    return manifest["stages"]["ingest"], path.read_bytes()


class InjectedFailure(Exception):
    """Raised by an injection point; deliberately not a ConverterError."""


# ==========================================================================
# The tree function
# ==========================================================================


class TreeSignatureTests(unittest.TestCase):
    def count(self, tree):
        return flyby_signature.count_tree_signature(*columns(tree))

    def test_known_good_trees_count_zero(self):
        for name, tree in (
            ("no negative id", NO_NEGATIVE_TREE),
            ("negative id on a central", NEGATIVE_CENTRAL_TREE),
            ("negative id on a satellite below the last snapshot", NEGATIVE_EARLY_SATELLITE_TREE),
            # tree A's -5 and tree C's -3 are centrals below the last snapshot
            ("LITERAL_TREE_A", LITERAL_TREE_A),
            ("LITERAL_TREE_B", LITERAL_TREE_B),
            ("LITERAL_TREE_C", LITERAL_TREE_C),
        ):
            with self.subTest(name):
                self.assertEqual(self.count(tree), 0)

    def test_known_bad_trees_count_their_literal(self):
        self.assertEqual(self.count(BAD_TREE), 2)
        self.assertEqual(self.count(EARLY_BAD_TREE), 1)

    def test_an_empty_tree_counts_zero(self):
        self.assertEqual(flyby_signature.count_tree_signature([], [], []), 0)

    def test_columns_of_different_lengths_are_refused(self):
        with self.assertRaisesRegex(ConverterError, "one length"):
            flyby_signature.count_tree_signature([3, 3], [0], [-1, -1])


# ==========================================================================
# The pre-scan
# ==========================================================================


class PrescanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="flyby_prescan_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.schema = lhalo_schema()
        self.layout = self.schema.source_layout

    def write(self, name, trees):
        return fixtures.write_lhalo_file(str(self.tmp / name), trees)

    def prescan(self, sources, max_snapshot=3, **kwargs):
        return flyby_signature.prescan_lhalo(
            sources, self.layout, self.schema, max_snapshot, **kwargs
        )

    def test_a_clean_source_counts_zero(self):
        file0 = self.write("t.0", [LITERAL_TREE_A, LITERAL_TREE_B, NEGATIVE_CENTRAL_TREE])
        file1 = self.write("t.1", [LITERAL_TREE_C, NEGATIVE_EARLY_SATELLITE_TREE])
        scan = self.prescan([[0, file0], [1, file1]], chunk_rows=1)
        self.assertEqual(scan, flyby_signature.FlybyScan(0, 0, None, None))
        self.assertEqual(
            scan.record(allowed=True), {"checked": True, "halos": 0, "trees": 0, "allowed": False}
        )

    def test_two_files_with_trees_spanning_several_chunks(self):
        # BAD_TREE (5 rows) spans three chunks of two rows; an empty tree
        # sits between bad ones; the ordinals are the recorded, not positional.
        file0 = self.write("t.0", [LITERAL_TREE_A, BAD_TREE, []])
        file1 = self.write("t.1", [LITERAL_TREE_C, BAD_TREE, EARLY_BAD_TREE])
        states = []
        real_state = flyby_signature._new_snapshot_counts

        def record_state(n_snapshots):
            states.append(real_state(n_snapshots))
            return states[-1]

        with mock.patch.object(
            flyby_signature, "_read_records", wraps=flyby_signature._read_records
        ) as reads, mock.patch.object(
            flyby_signature, "_new_snapshot_counts", side_effect=record_state
        ) as state:
            scan = self.prescan([[4, file0], [5, file1]], chunk_rows=2)
        self.assertEqual(scan, flyby_signature.FlybyScan(5, 3, 4, file0))
        self.assertEqual(
            scan.record(allowed=True), {"checked": True, "halos": 5, "trees": 3, "allowed": True}
        )
        self.assertEqual(
            scan.record(allowed=False), {"checked": True, "halos": 5, "trees": 3, "allowed": False}
        )
        counts = [call.args[1] for call in reads.call_args_list]
        self.assertLessEqual(max(counts), 2)
        # tree A (4 rows) 2+2, BAD_TREE 2+2+1, tree C (3 rows) 2+1, BAD_TREE,
        # EARLY_BAD_TREE (2 rows) 2: no read covers more than its chunk
        self.assertEqual(counts, [2, 2, 2, 2, 1, 2, 1, 2, 2, 1, 2])
        # the per-snapshot state is max_snapshot + 1 entries, one per file
        self.assertEqual([call.args for call in state.call_args_list], [(4,), (4,)])
        self.assertEqual([array.shape for array in states], [(4,), (4,)])

    def test_the_state_length_follows_max_snapshot_not_the_tree(self):
        file0 = self.write("t.0", [BAD_TREE])
        with mock.patch.object(
            flyby_signature, "_new_snapshot_counts", wraps=flyby_signature._new_snapshot_counts
        ) as state:
            scan = self.prescan([[0, file0]], max_snapshot=63, chunk_rows=1)
        self.assertEqual(scan.halos, BAD_TREE_COUNT)
        self.assertEqual([call.args for call in state.call_args_list], [(64,)])

    def test_the_first_file_is_the_first_holding_a_signature_halo(self):
        file0 = self.write("t.0", [LITERAL_TREE_A, LITERAL_TREE_B])
        file1 = self.write("t.1", [LITERAL_TREE_C, BAD_TREE])
        scan = self.prescan([[0, file0], [1, file1]], chunk_rows=3)
        self.assertEqual(scan, flyby_signature.FlybyScan(2, 1, 1, file1))

    def test_the_default_chunk_is_the_adapter_topology_chunk(self):
        file0 = self.write("t.0", [BAD_TREE])
        with mock.patch.object(
            flyby_signature, "_read_records", wraps=flyby_signature._read_records
        ) as reads:
            self.prescan([[0, file0]])
        self.assertEqual([call.args[1] for call in reads.call_args_list], [5])
        self.assertEqual(
            flyby_signature.prescan_lhalo.__kwdefaults__["chunk_rows"], TOPOLOGY_READ_CHUNK_ROWS
        )

    def test_an_out_of_range_snapnum_is_refused_naming_the_file_and_tree(self):
        above = [dict(SnapNum=3), dict(SnapNum=4, FirstHaloInFOFgroup=1)]
        below = [dict(SnapNum=-1)]
        for name, tree, value in (("above", above, 4), ("below", below, -1)):
            with self.subTest(name):
                path = self.write("{}.0".format(name), [LITERAL_TREE_B, tree])
                with self.assertRaises(ConverterError) as caught:
                    self.prescan([[7, path]], chunk_rows=1)
                message = str(caught.exception)
                self.assertIn(path, message)
                self.assertIn("ordinal 7", message)
                self.assertIn("tree 1", message)
                self.assertIn("SnapNum is {}, outside [0, 3]".format(value), message)

    def test_the_shipped_layout_fields_are_viewed_at_their_declared_offsets(self):
        """The signature dtype views each of the three fields of the shipped
        104-byte layout at its declared offset and width, in its stride."""
        entries = {entry.name: entry for entry in self.layout.entries}
        self.assertEqual(entries["MostBoundID"].numpy_dtype, "int64")
        dtype, fields = flyby_signature._signature_dtype(self.layout, self.schema)
        self.assertEqual(dtype.itemsize, self.layout.itemsize)
        for role in flyby_signature.SIGNATURE_ROLES:
            entry = entries[fields[role]]
            self.assertEqual(dtype.fields[entry.name][1], entry.offset)
            self.assertEqual(dtype.fields[entry.name][0].itemsize, entry.itemsize)

    def test_a_relaid_big_endian_layout_is_read_where_it_says(self):
        """A test-only layout -- big-endian, a 120-byte stride, MostBoundID
        moved from offset 80 to offset 4 and every other field laid after it
        in reverse order -- over a source packed by hand for that layout gives
        the literal count. No offset, width, stride or byte order of the
        shipped record is assumed."""
        shipped = self.layout.entries
        order = [entry for entry in shipped if entry.name == "MostBoundID"]
        order += [entry for entry in reversed(shipped) if entry.name != "MostBoundID"]
        entries, offset = [], 4
        for entry in order:
            entries.append(dataclasses.replace(entry, offset=offset))
            offset += entry.itemsize
        itemsize = offset + 12
        layout = cs.SourceLayout(byte_order="big", itemsize=itemsize, entries=tuple(entries))
        at = {entry.name: entry.offset for entry in entries}
        self.assertEqual((itemsize, at["MostBoundID"]), (120, 4))
        self.assertEqual(
            {entry.name: entry.offset for entry in shipped}["MostBoundID"], 80, "shipped offset"
        )

        trees = [BAD_TREE, EARLY_BAD_TREE, NEGATIVE_CENTRAL_TREE]
        path = self.tmp / "relaid.0"
        with open(path, "wb") as handle:
            handle.write(struct.pack(">ii", len(trees), sum(len(tree) for tree in trees)))
            for tree in trees:
                handle.write(struct.pack(">i", len(tree)))
            for tree in trees:
                for row in tree:
                    record = bytearray(itemsize)
                    struct.pack_into(">q", record, at["MostBoundID"], row["MostBoundID"])
                    struct.pack_into(">i", record, at["SnapNum"], row["SnapNum"])
                    struct.pack_into(
                        ">i", record, at["FirstHaloInFOFgroup"], row["FirstHaloInFOFgroup"]
                    )
                    handle.write(bytes(record))
        scan = flyby_signature.prescan_lhalo([[0, str(path)]], layout, self.schema, 3, chunk_rows=2)
        # BAD_TREE 2 + EARLY_BAD_TREE 1 + NEGATIVE_CENTRAL_TREE 0
        self.assertEqual(scan, flyby_signature.FlybyScan(3, 2, 0, str(path)))

    def test_a_non_lhalo_schema_is_refused(self):
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml"))
        with self.assertRaisesRegex(ConverterError, "lhalo_binary sources"):
            flyby_signature.prescan_lhalo([], self.layout, schema, 3)

    def test_bad_parameters_are_refused(self):
        for kwargs in ({"chunk_rows": 0}, {"chunk_rows": True}, {"max_snapshot": -1}):
            with self.subTest(kwargs):
                arguments = dict(max_snapshot=3)
                arguments.update(kwargs)
                with self.assertRaises(ConverterError):
                    flyby_signature.prescan_lhalo([], self.layout, self.schema, **arguments)


# ==========================================================================
# The guard in run_ingest
# ==========================================================================


class GuardCase(unittest.TestCase):
    #: The directory each case's temporary directory is made in (``dir=`` of
    #: ``tempfile.mkdtemp``); ``None`` is the system's temporary directory.
    tmp_parent = None

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="flyby_guard_", dir=self.tmp_parent))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        self.a_list = self.src / "a.list"
        self.a_list.write_text(LITERAL_A_LIST_TEXT)
        self.schema = lhalo_schema()
        self.work = self.tmp / "work"

    def source(self, bad):
        """Two files; the bad one carries BAD_TREE (count 2, one tree) in file 1."""
        file0 = fixtures.write_lhalo_file(
            str(self.src / "trees.0"), [LITERAL_TREE_A, LITERAL_TREE_B]
        )
        file1 = fixtures.write_lhalo_file(
            str(self.src / "trees.1"),
            [LITERAL_TREE_C, BAD_TREE if bad else NO_NEGATIVE_TREE],
        )
        return {"sources": [[0, file0], [1, file1]]}

    def initialize(self, parameters):
        return pipeline.initialize(
            self.work,
            self.schema,
            parameters,
            self.a_list,
            ingest_max_rows=3,
            transpose_budget_bytes=LITERAL_BUDGET,
        )

    def ingest(self, **kwargs):
        """run_ingest with stderr captured; returns (manifest, stderr)."""
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            manifest = pipeline.run_ingest(self.work, save_every_chunks=1, **kwargs)
        return manifest, stderr.getvalue()


class GuardTests(GuardCase):
    def test_a_known_bad_source_is_refused_before_any_attempt(self):
        parameters = self.source(bad=True)
        self.initialize(parameters)
        before, raw_before = ingest_state(self.work)
        with mock.patch.object(
            pipeline, "_stage_attempt", wraps=pipeline._stage_attempt
        ) as attempts, mock.patch.object(pipeline, "_write_chunk") as write_chunk:
            with self.assertRaises(ConverterError) as caught:
                self.ingest()
        message = str(caught.exception)
        # the converter records each source path resolved (os.path.realpath),
        # so the message names the resolved form of the path given
        for text in (
            "2 halo(s) in 1 tree(s)",
            "first in source file ordinal 1, {}".format(
                os.path.realpath(parameters["sources"][1][1])
            ),
            "--allow-flyby-signature",
        ):
            self.assertIn(text, message)
        attempts.assert_not_called()
        write_chunk.assert_not_called()
        after, raw_after = ingest_state(self.work)
        self.assertEqual(raw_after, raw_before, "a refusal changed the manifest")
        self.assertEqual((after["attempt"], after["artifacts"]), (0, []))
        self.assertEqual(after, before)
        self.assertFalse((self.work / pipeline.INGEST_DIR).exists())

    def test_the_prescan_reads_the_recorded_sources_through_the_bound_adapter(self):
        parameters = self.source(bad=False)
        manifest = self.initialize(parameters)
        recorded = manifest.configuration["adapter"]["parameters"]["sources"]
        with mock.patch.object(
            flyby_signature, "prescan_lhalo", wraps=flyby_signature.prescan_lhalo
        ) as prescan, mock.patch.object(
            pipeline, "_stage_attempt", wraps=pipeline._stage_attempt
        ) as attempts:
            order = mock.Mock()
            order.attach_mock(prescan, "prescan")
            order.attach_mock(attempts, "attempt")
            self.ingest()
        self.assertEqual([name for name, _a, _k in order.mock_calls[:2]], ["prescan", "attempt"])
        args = prescan.call_args.args
        self.assertEqual(args[0], recorded)
        self.assertEqual(args[1], self.schema.source_layout)
        self.assertEqual(args[2].digest, self.schema.digest)
        self.assertEqual(args[3], 3)

    def test_a_known_good_source_is_ingested_and_recorded_as_checked(self):
        self.initialize(self.source(bad=False))
        manifest, stderr = self.ingest()
        self.assertTrue(manifest.is_complete("ingest"))
        self.assertEqual(
            manifest.stage("ingest")["result"]["flyby_signature"],
            {"checked": True, "halos": 0, "trees": 0, "allowed": False},
        )
        self.assertEqual(stderr, "")

    def test_the_override_ingests_a_known_bad_source_warns_and_records_it(self):
        self.initialize(self.source(bad=True))
        manifest, stderr = self.ingest(allow_flyby_signature=True)
        self.assertTrue(manifest.is_complete("ingest"))
        self.assertEqual(
            manifest.stage("ingest")["result"]["flyby_signature"],
            {"checked": True, "halos": 2, "trees": 1, "allowed": True},
        )
        self.assertIn("WARNING", stderr)
        self.assertIn("2 halo(s) in 1 tree(s)", stderr)
        self.assertIn("--allow-flyby-signature", stderr)
        self.assertEqual(manifest.stage("ingest")["result"]["n_rows"], 4 + 1 + 3 + 5)

    def test_the_override_on_a_clean_source_neither_warns_nor_records_an_acceptance(self):
        self.initialize(self.source(bad=False))
        manifest, stderr = self.ingest(allow_flyby_signature=True)
        self.assertEqual(stderr, "")
        self.assertEqual(
            manifest.stage("ingest")["result"]["flyby_signature"],
            {"checked": True, "halos": 0, "trees": 0, "allowed": False},
        )

    def test_a_resume_without_the_override_after_an_interrupted_allowed_attempt_is_refused(self):
        self.initialize(self.source(bad=True))
        real_write = pipeline._write_chunk
        calls = []

        def fail_on_second(path, records):
            calls.append(path)
            if len(calls) == 2:
                raise InjectedFailure(path)
            real_write(path, records)

        with mock.patch.object(pipeline, "_write_chunk", side_effect=fail_on_second):
            with self.assertRaises(InjectedFailure):
                self.ingest(allow_flyby_signature=True)
        interrupted, raw_interrupted = ingest_state(self.work)
        self.assertEqual(interrupted["status"], cm.STAGE_FAILED)
        self.assertEqual(len(interrupted["artifacts"]), 1)

        with self.assertRaisesRegex(ConverterError, "--allow-flyby-signature"):
            self.ingest()
        # the refusal began no attempt and kept what the interrupted one registered
        refused, raw_refused = ingest_state(self.work)
        self.assertEqual(raw_refused, raw_interrupted)
        self.assertEqual(refused["attempt"], interrupted["attempt"])

        manifest, stderr = self.ingest(allow_flyby_signature=True)
        self.assertIn("WARNING", stderr)
        self.assertEqual(
            manifest.stage("ingest")["result"]["flyby_signature"],
            {"checked": True, "halos": 2, "trees": 1, "allowed": True},
        )

    def test_a_complete_stage_is_not_rescanned_and_its_entry_stands(self):
        self.initialize(self.source(bad=True))
        self.ingest(allow_flyby_signature=True)
        _state, raw_complete = ingest_state(self.work)
        for kwargs in ({}, {"allow_flyby_signature": True}):
            with self.subTest(kwargs):
                with mock.patch.object(flyby_signature, "prescan_lhalo") as prescan:
                    manifest, stderr = self.ingest(**kwargs)
                prescan.assert_not_called()
                self.assertEqual(stderr, "")
                self.assertEqual(
                    manifest.stage("ingest")["result"]["flyby_signature"]["allowed"], True
                )
                self.assertEqual(ingest_state(self.work)[1], raw_complete)

    def test_the_override_is_not_part_of_the_recorded_configuration(self):
        manifest = self.initialize(self.source(bad=True))
        digest = manifest.data["configuration_sha256"]
        manifest, _stderr = self.ingest(allow_flyby_signature=True)
        self.assertEqual(manifest.data["configuration_sha256"], digest)
        self.assertNotIn("allow_flyby_signature", json.dumps(manifest.configuration))
        self.assertNotIn("allow_flyby_signature", json.dumps(manifest.tuning))

    def test_a_non_bool_override_is_refused(self):
        self.initialize(self.source(bad=False))
        with self.assertRaisesRegex(ConverterError, "must be a bool"):
            pipeline.run_ingest(self.work, allow_flyby_signature=1)

    def test_an_out_of_range_snapnum_is_refused_by_the_guard_naming_file_and_tree(self):
        file0 = fixtures.write_lhalo_file(
            str(self.src / "trees.0"), [LITERAL_TREE_B, [dict(SnapNum=4)]]
        )
        self.initialize({"sources": [[0, file0]]})
        _before, raw_before = ingest_state(self.work)
        with self.assertRaises(ConverterError) as caught:
            self.ingest()
        # the recorded (resolved) source path is the one the guard names
        self.assertIn(
            "{} (ordinal 0): tree 1".format(os.path.realpath(file0)), str(caught.exception)
        )
        self.assertEqual(ingest_state(self.work)[1], raw_before)


class GuardTestsThroughASymbolicLink(GuardTests):
    """Every :class:`GuardTests` test, with its temporary directory made under
    a symbolic link this class creates, so the converter's recorded (resolved)
    source paths differ from the given ones on every platform -- not only where
    the system's temporary directory is itself reached through a link, as
    macOS's ``/var`` is (``/private/var``)."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        base = Path(tempfile.mkdtemp(prefix="flyby_guard_link_"))
        # class cleanups run after every test's own cleanup has removed its
        # temporary directory from under the link
        cls.addClassCleanup(shutil.rmtree, base, ignore_errors=True)
        real = base / "real"
        real.mkdir()
        cls.link = base / "link"
        cls.link.symlink_to(real, target_is_directory=True)
        cls.tmp_parent = str(cls.link)

    def setUp(self):
        super().setUp()
        # the first proves the link is in effect (on macOS the system's
        # temporary directory alone already makes the second hold)
        self.assertEqual(self.tmp.parent, self.link)
        self.assertNotEqual(os.path.realpath(self.tmp), str(self.tmp))


class ConsistentTreesGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="flyby_ctrees_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        src = self.tmp / "ascii"
        src.mkdir()
        forests = fixtures.standard_forests()
        tree = fixtures.write_ctrees_file(src / "tree_0_0_0.dat", fixtures.all_trees(forests))
        self.parameters = {
            "tree_files": [str(tree)],
            "forests_list": str(fixtures.write_forests_list(src / "forests.list", forests)),
            "simulation_info": str(fixtures.write_simulation_info(src / "simulation_info.yaml")),
        }
        self.a_list = fixtures.write_a_list(src / "fixture.a_list")
        self.schema = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml")
        )
        self.work = self.tmp / "work"
        pipeline.initialize(
            self.work,
            self.schema,
            self.parameters,
            self.a_list,
            ingest_max_rows=4,
            transpose_budget_bytes=LITERAL_BUDGET,
        )

    def test_the_override_is_refused_and_the_ingest_records_not_checked(self):
        _before, raw_before = ingest_state(self.work)
        with self.assertRaises(ConverterError) as caught:
            pipeline.run_ingest(self.work, allow_flyby_signature=True)
        self.assertIn(
            "--allow-flyby-signature applies to --source-format lhalo_binary", str(caught.exception)
        )
        self.assertIn("consistent_trees_ascii", str(caught.exception))
        self.assertEqual(ingest_state(self.work)[1], raw_before)
        self.assertFalse((self.work / pipeline.ASCII_PREPARATION_DIR).exists())

        manifest = pipeline.run_ingest(self.work)
        self.assertEqual(
            manifest.stage("ingest")["result"]["flyby_signature"],
            {"checked": False, "reason": "source format consistent_trees_ascii"},
        )
        _complete, raw_complete = ingest_state(self.work)
        # on a complete stage the override is still refused, before the skip
        with self.assertRaisesRegex(ConverterError, "--allow-flyby-signature"):
            pipeline.run_ingest(self.work, allow_flyby_signature=True)
        self.assertEqual(ingest_state(self.work)[1], raw_complete)


class StateIsBoundedTests(unittest.TestCase):
    def test_counts_reset_between_trees(self):
        """A tree's leftover candidates never leak into the next tree's count.
        The first tree leaves one candidate at snapshot 2 (a negative
        satellite below its last snapshot, 3: count 0); the second,
        EARLY_BAD_TREE, ends at snapshot 2 with one signature halo there.
        With the per-tree reset the total is 1; a leaked count would make it
        2."""
        tmp = Path(tempfile.mkdtemp(prefix="flyby_reset_"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        schema = lhalo_schema()
        path = fixtures.write_lhalo_file(
            str(tmp / "t.0"), [NEGATIVE_EARLY_SATELLITE_TREE, EARLY_BAD_TREE]
        )
        scan = flyby_signature.prescan_lhalo(
            [[0, path]], schema.source_layout, schema, 3, chunk_rows=1
        )
        self.assertEqual(scan, flyby_signature.FlybyScan(1, 1, 0, path))
        np.testing.assert_array_equal(flyby_signature._new_snapshot_counts(4), np.zeros(4))


# ==========================================================================
# The horizontal check
# ==========================================================================


def empty_slab(dataset_dir, snap):
    """Make one slab hold no halo: every ``/halos`` dataset recreated with
    zero rows and the header's ``n_halos`` set to 0."""
    path = Path(dataset_dir) / "snapshot_{:03d}.h5".format(snap)
    with h5py.File(path, "r+") as handle:
        halos = handle["halos"]
        for name in list(halos):
            dtype, row_shape = halos[name].dtype, halos[name].shape[1:]
            del halos[name]
            halos.create_dataset(name, shape=(0,) + row_shape, dtype=dtype)
        handle["header"].attrs["n_halos"] = np.int64(0)


class HorizontalSignatureTests(unittest.TestCase):
    """The dataset has six slabs (snapshots 0-5). Its last slab, snapshot 5,
    holds six rows whose ``FirstHaloInFOFgroup`` is ``[0, 0, 2, 3, 3, 5]``:
    rows 1 and 4 are satellites, rows 0, 2, 3 and 5 centrals. Snapshot 4
    holds seven rows, ``[0, 0, 2, 3, 4, 4, 6]``: rows 1 and 5 are
    satellites. No ``MostBoundID`` is negative before an edit."""

    LAST_FOF = [0, 0, 2, 3, 3, 5]
    EARLIER_FOF = [0, 0, 2, 3, 4, 4, 6]

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="flyby_horizontal_"))
        cls.source = convert_one_file_dataset(
            cls.tmp / "source", fixtures.early_ending_correspondence_forests()
        )

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(prefix="flyby_horizontal_case_"))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)

    def dataset(self, edit=None):
        """A fresh copy of the source dataset in its own directory under the case's work
        directory (removed with it), edited by ``edit(directory)``."""
        directory = Path(tempfile.mkdtemp(prefix="dataset_", dir=self.work)) / "dataset"
        shutil.copytree(self.source, directory)
        if edit is not None:
            edit(directory)
        return directory

    def scan(self, directory, **kwargs):
        return flyby_signature.count_horizontal_signature(HorizontalDataset(directory), **kwargs)

    def test_the_fixture_is_what_the_oracle_says(self):
        dataset = HorizontalDataset(self.dataset())
        self.assertEqual(dataset.n_halos, (0, 1, 1, 4, 7, 6))
        for snap, expected in ((5, self.LAST_FOF), (4, self.EARLIER_FOF)):
            fof = dataset.read_column(snap, "FirstHaloInFOFgroup")
            self.assertEqual(fof.tolist(), expected)
            self.assertTrue((dataset.read_column(snap, "MostBoundID") > 0).all())

    def test_known_good_datasets_count_zero(self):
        cases = {
            "no negative id": None,
            "negative ids only on the last slab's centrals": lambda d: negate_most_bound_id(
                d, 5, [0, 2, 3, 5]
            ),
            "negative ids on an earlier slab's satellites": lambda d: negate_most_bound_id(
                d, 4, [1, 5]
            ),
        }
        for name, edit in cases.items():
            for block_rows in (1, 4, DEFAULT_BLOCK_ROWS):
                with self.subTest(name, block_rows=block_rows):
                    directory = self.dataset(edit)
                    scan = self.scan(directory, block_rows=block_rows)
                    self.assertEqual(scan, flyby_signature.HorizontalFlybyScan(0, 5))

    def test_a_known_bad_dataset_counts_its_literal_across_blocks(self):
        """Rows 0 and 3 (centrals) and 1 and 4 (satellites) of snapshot 5
        negated: count 2. At three rows a block, row 3 is the first row of the
        second block, so a block-local row index would count it too (3)."""
        directory = self.dataset(lambda d: negate_most_bound_id(d, 5, [0, 1, 3, 4]))
        for block_rows in (1, 2, 3, 4, 5, 6, DEFAULT_BLOCK_ROWS):
            with self.subTest(block_rows=block_rows):
                self.assertEqual(
                    self.scan(directory, block_rows=block_rows),
                    flyby_signature.HorizontalFlybyScan(2, 5),
                )

    def test_the_read_is_bounded_and_touches_only_the_last_slab_s_two_columns(self):
        directory = self.dataset(lambda d: negate_most_bound_id(d, 5, [0, 1, 3, 4]))
        real = HorizontalDataset.iter_columns
        calls, blocks = [], []

        def spy(dataset, snap, names, block_rows=DEFAULT_BLOCK_ROWS):
            calls.append((snap, tuple(names), block_rows))
            for start, columns in real(dataset, snap, names, block_rows):
                blocks.append((start, {name: len(block) for name, block in columns.items()}))
                yield start, columns

        with mock.patch.object(HorizontalDataset, "iter_columns", spy), mock.patch.object(
            HorizontalDataset, "read_rows", side_effect=AssertionError("whole read")
        ):
            scan = self.scan(directory, block_rows=3)
        self.assertEqual(scan, flyby_signature.HorizontalFlybyScan(2, 5))
        self.assertEqual(calls, [(5, ("MostBoundID", "FirstHaloInFOFgroup"), 3)])
        both = {"MostBoundID": 3, "FirstHaloInFOFgroup": 3}
        self.assertEqual(blocks, [(0, both), (3, both)])

    def test_the_default_block_is_the_dataset_reader_s(self):
        self.assertEqual(DEFAULT_BLOCK_ROWS, 1 << 22)
        directory = self.dataset()
        real = HorizontalDataset.iter_columns
        sizes = []

        def spy(dataset, snap, names, *rest):
            sizes.append(rest)
            return real(dataset, snap, names, *rest)

        with mock.patch.object(HorizontalDataset, "iter_columns", spy):
            self.scan(directory)
        # No block size passed: the iterator's own default, 2^22 rows, applies.
        self.assertEqual(sizes, [()])
        self.assertEqual(real.__defaults__, (DEFAULT_BLOCK_ROWS,))

    def test_the_last_slab_holding_halos_is_checked_not_the_last_file(self):
        """Snapshot 5 emptied: snapshot 4 is the last holding halos, and its
        satellites 1 and 5 (negated, with its central 0) count 2."""

        def edit(directory):
            empty_slab(directory, 5)
            negate_most_bound_id(directory, 4, [0, 1, 5])

        self.assertEqual(
            self.scan(self.dataset(edit), block_rows=2), flyby_signature.HorizontalFlybyScan(2, 4)
        )

    def test_a_dataset_without_halos_counts_zero(self):
        def edit(directory):
            for snap in range(6):
                empty_slab(directory, snap)

        self.assertEqual(
            self.scan(self.dataset(edit)), flyby_signature.HorizontalFlybyScan(0, None)
        )

    def test_a_slab_lacking_either_column_is_refused(self):
        for name in ("MostBoundID", "FirstHaloInFOFgroup"):
            with self.subTest(name):

                def edit(directory, name=name):
                    with h5py.File(directory / "snapshot_005.h5", "r+") as handle:
                        del handle["halos"][name]

                directory = self.dataset(edit)
                with self.assertRaisesRegex(ConverterError, r"no /halos/{} dataset".format(name)):
                    self.scan(directory)


if __name__ == "__main__":
    unittest.main()
