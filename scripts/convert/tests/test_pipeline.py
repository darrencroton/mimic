"""Slice 7 unit tests: generic stage orchestration and verified restart
(scripts/convert/pipeline.py).

**Oracle.** Until the Slice 8 writer exists, the stage/restart oracle is a
small literal L-Halo source -- three trees in two files, packed by hand with
``test_lhalo_adapter``'s independent ``struct`` packer -- and the transposed
arrays hand-derived from it below (:data:`EXPECTED`), not from the transpose.
It carries a three-snapshot descendant gap, a two-snapshot one, mixed-age
sibling progenitors, an early-ending halo, an empty snapshot and a duplicated
``MostBoundID``. Every restart test compares its final outputs with those
arrays *and* byte for byte with an uninterrupted run.

**Failure injection.** Each stage is interrupted before and after its
artifact write, its verification and its manifest save, by an exception
(the handler path) and -- for ingest and transpose -- by ``os._exit`` in a
subprocess (no handler, no ``finally``: a real crash). The write stage runs a
deterministic stub writer, not a premature duplicate of the v3 writer.

Every destructive case works on files it created in its own temporary
directory.
"""

import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
import conversion_manifest as cm  # noqa: E402
import fixtures  # noqa: E402
import pipeline  # noqa: E402
import test_ctrees_hdf5_adapter as h5fixtures  # noqa: E402
import test_lhalo_adapter as lhalo  # noqa: E402
from column_schema import ConverterError  # noqa: E402
from test_conversion_manifest import LEGACY_FIXTURE, tree_state  # noqa: E402

HERE = Path(__file__).resolve().parent
CONVERT_DIR = HERE.parent
REPO_ROOT = HERE.parents[2]
PROFILE_DIR = CONVERT_DIR / "profiles"
MINI_MILLENNIUM_PROPERTIES = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"

BUDGET = 4 << 20

# ==========================================================================
# The literal source and its independently derived transpose
# ==========================================================================

#: Tree A (file 0, tree 0): r0 at snap 3 has progenitors r1 (snap 2) and r2
#: (snap 0, a three-snapshot gap) as mixed-age siblings; r3 shares r1's FoF
#: group and ends early.
TREE_A = [
    dict(
        SnapNum=3, FirstProgenitor=1, FirstHaloInFOFgroup=0, MostBoundID=77, Len=10, M_Crit200=1.5
    ),
    dict(
        SnapNum=2,
        Descendant=0,
        NextProgenitor=2,
        FirstHaloInFOFgroup=1,
        NextHaloInFOFgroup=3,
        MostBoundID=-5,
        Len=9,
        M_Crit200=0.75,
    ),
    dict(SnapNum=0, Descendant=0, FirstHaloInFOFgroup=2, MostBoundID=11, Len=8, M_Crit200=0.25),
    dict(SnapNum=2, FirstHaloInFOFgroup=1, MostBoundID=12, Len=7, M_Crit200=0.125),
]
#: Tree B (file 0, tree 1): a lone halo.
TREE_B = [dict(SnapNum=3, FirstHaloInFOFgroup=0, MostBoundID=13, Len=6, M_Crit200=2.0)]
#: Tree C (file 1, tree 0): a chain whose root progenitor skips snapshot 1.
#: r0 duplicates tree A's MostBoundID, as real catalogs may.
TREE_C = [
    dict(SnapNum=3, FirstProgenitor=1, FirstHaloInFOFgroup=0, MostBoundID=77, Len=5, M_Crit200=3.0),
    dict(
        SnapNum=2,
        Descendant=0,
        FirstProgenitor=2,
        FirstHaloInFOFgroup=1,
        MostBoundID=-3,
        Len=4,
        M_Crit200=4.0,
    ),
    dict(SnapNum=0, Descendant=1, FirstHaloInFOFgroup=2, MostBoundID=14, Len=3, M_Crit200=5.0),
]
A_LIST_TEXT = "0.25\n0.5\n0.75\n1.0\n"

#: SourceHaloID = 1 + prefix position over (file, tree, row): A -> 1-4,
#: B -> 5, C -> 6-8. Rows per snapshot ascend in SourceHaloID. Links are
#: (row, target snapshot); -1 null.
_COLUMNS = (
    "SourceHaloID",
    "ForestIndex",
    "HaloRankInForest",
    "Descendant",
    "DescendantSnapshot",
    "FirstProgenitor",
    "FirstProgenitorSnapshot",
    "NextProgenitor",
    "NextProgenitorSnapshot",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
    "SnapNum",
    "Len",
    "MostBoundID",
    "M_Crit200",
)
_ROWS = {
    0: [
        (3, 0, 2, 0, 3, -1, -1, -1, -1, 0, -1, 0, 8, 11, 0.25),
        (8, 2, 2, 2, 2, -1, -1, -1, -1, 1, -1, 0, 3, 14, 5.0),
    ],
    1: [],
    2: [
        (2, 0, 1, 0, 3, -1, -1, 0, 0, 0, 1, 2, 9, -5, 0.75),
        (4, 0, 3, -1, -1, -1, -1, -1, -1, 0, -1, 2, 7, 12, 0.125),
        (7, 2, 1, 2, 3, 1, 0, -1, -1, 2, -1, 2, 4, -3, 4.0),
    ],
    3: [
        (1, 0, 0, -1, -1, 0, 2, -1, -1, 0, -1, 3, 10, 77, 1.5),
        (5, 1, 0, -1, -1, -1, -1, -1, -1, 1, -1, 3, 6, 13, 2.0),
        (6, 2, 0, -1, -1, 2, 2, -1, -1, 2, -1, 3, 5, 77, 3.0),
    ],
}
EXPECTED = {
    snap: {name: [row[i] for row in rows] for i, name in enumerate(_COLUMNS)}
    for snap, rows in _ROWS.items()
}
TOTAL_HALOS = 8


def lhalo_schema(profile="lhalo_binary.yaml", directory=PROFILE_DIR, properties=None):
    return cs.build_schema(
        cs.load_column_map(Path(directory) / profile),
        cs.load_source_properties(properties or MINI_MILLENNIUM_PROPERTIES),
    )


def same_width_variant(schema):
    """The extras-example schema with one extra's units changed: identical
    record widths and dtypes, a different conversion."""
    record = json.loads(json.dumps(cm.schema_record(schema)))
    canonical = record["canonical"]
    canonical["extra_fields"][0]["units"] += " (substituted)"
    document = {
        "schema_version": 1,
        "source_format": canonical["source_format"],
        "required_columns": canonical["required_columns"],
        "extra_fields": canonical["extra_fields"],
    }
    if canonical.get("source_layout"):
        layout = canonical["source_layout"]
        document["binary_layout"] = {
            "byte_order": layout["byte_order"],
            "itemsize": layout["itemsize"],
            "offsets": {e["name"]: e["offset"] for e in layout["entries"]},
        }
        return cs.build_schema(
            cs.parse_column_map(document, "<variant>"),
            cs.load_source_properties(MINI_MILLENNIUM_PROPERTIES),
        )
    return cs.build_schema(cs.parse_column_map(document, "<variant>"))


class InjectedFailure(Exception):
    """Raised by an injection point; deliberately not a ConverterError."""


class StubWriter(pipeline.StageWriter):
    """Deterministic write-stage stand-in: one copy of each transposed
    snapshot plus a small JSON summary, re-read and checked by ``verify``."""

    def __init__(self, name="stub-writer", version=1):
        self._identity = {"name": name, "version": version}

    @property
    def identity(self):
        return self._identity

    def write(self, inputs, out_dir):
        produced = []
        for entry in inputs.transposed:
            path = out_dir / "copy_{:03d}.bin".format(entry.snapshot)
            path.write_bytes(entry.path.read_bytes())
            produced.append(path)
        summary = out_dir / "summary.json"
        summary.write_text(
            json.dumps(
                {"snapshots": list(inputs.snapshots), "digest": inputs.schema.digest},
                sort_keys=True,
            )
        )
        produced.append(summary)
        return produced

    def verify(self, inputs, produced):
        by_name = {path.name: path for path in produced}
        for entry in inputs.transposed:
            copy = by_name["copy_{:03d}.bin".format(entry.snapshot)]
            if copy.read_bytes() != entry.path.read_bytes():
                raise ConverterError("{}: copy differs from its input".format(copy))
        if json.loads(by_name["summary.json"].read_text())["digest"] != inputs.schema.digest:
            raise ConverterError("summary digest mismatch")


class PipelineCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pipeline_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        self.file0 = lhalo.write_lhalo_file(str(self.src / "trees.0"), [TREE_A, TREE_B])
        self.file1 = lhalo.write_lhalo_file(str(self.src / "trees.1"), [TREE_C])
        self.a_list = self.src / "a.list"
        self.a_list.write_text(A_LIST_TEXT)
        self.schema = lhalo_schema()
        self.parameters = {"sources": [[0, self.file0], [1, self.file1]]}
        self.work = self.tmp / "work"

    def initialize(self, work=None, schema=None, **kwargs):
        kwargs.setdefault("ingest_max_rows", 3)
        kwargs.setdefault("transpose_budget_bytes", BUDGET)
        return pipeline.initialize(
            work or self.work, schema or self.schema, self.parameters, self.a_list, **kwargs
        )

    def run_all(self, work=None, writer=None, **ingest):
        work = work or self.work
        ingest.setdefault("save_every_chunks", 1)
        pipeline.run_ingest(work, **ingest)
        pipeline.run_transpose(work)
        return pipeline.run_write(work, writer or StubWriter())

    def reference(self):
        """An uninterrupted run, for byte comparison."""
        if not hasattr(self, "_reference"):
            work = self.tmp / "reference"
            self.initialize(work)
            self.run_all(work)
            self._reference = work
        return self._reference

    def assert_expected(self, work):
        manifest = cm.ConversionManifest.load(work)
        for snap, columns in EXPECTED.items():
            rows = pipeline.read_transposed(manifest, snap)
            for name, values in columns.items():
                np.testing.assert_array_equal(
                    rows[name], np.asarray(values, dtype=rows[name].dtype), err_msg=name
                )

    def assert_matches_reference(self, work):
        """Every product and artifact record equals the uninterrupted run's,
        modulo the attempt numbering an interruption consumes."""
        reference = self.reference()
        got = cm.ConversionManifest.load(work)
        want = cm.ConversionManifest.load(reference)
        for stage in cm.STAGES:
            self.assertTrue(got.is_complete(stage), stage)
            got_hashes = [
                (Path(r).name, got.artifact(r)["sha256"]) for r in got.stage(stage)["artifacts"]
            ]
            want_hashes = [
                (Path(r).name, want.artifact(r)["sha256"]) for r in want.stage(stage)["artifacts"]
            ]
            self.assertEqual(got_hashes, want_hashes, stage)
            for relpath in got.stage(stage)["artifacts"]:
                self.assertEqual(
                    cm.sha256_file(got.artifact_path(relpath)), got.artifact(relpath)["sha256"]
                )
        for stage in ("ingest", "transpose"):
            self.assertEqual(got.stage(stage)["result"], want.stage(stage)["result"], stage)
        self.assert_expected(work)
        self.assert_no_strays(work)

    def assert_no_strays(self, work):
        """Only the manifest and registered artifacts (plus the directories
        holding them) remain."""
        manifest = cm.ConversionManifest.load(work)
        files = set()
        for base, _dirs, names in os.walk(work):
            for name in names:
                files.add(os.path.relpath(os.path.join(base, name), work))
        registered = {
            relpath
            for relpath, entry in manifest.data["artifacts"].items()
            if entry["status"] == cm.ARTIFACT_PRESENT
        }
        self.assertEqual(files, registered | {cm.MANIFEST_NAME})


# ==========================================================================
# End-to-end and the version-3 record
# ==========================================================================


class EndToEndTests(PipelineCase):
    def test_transposed_output_is_the_hand_derived_graph(self):
        self.initialize()
        self.run_all()
        self.assert_expected(self.work)
        result = cm.ConversionManifest.load(self.work).stage("transpose")["result"]
        self.assertEqual(result["total_halos"], TOTAL_HALOS)
        self.assertEqual(result["n_gapped_descendants"], 2)
        self.assertEqual(result["max_descendant_span"], 3)
        self.assertFalse(result["links_adjacent"])
        self.assert_no_strays(self.work)

    def test_manifest_records_identity_dtypes_sources_stages_and_checksums(self):
        self.initialize()
        self.run_all()
        data = json.loads((self.work / cm.MANIFEST_NAME).read_text())
        self.assertEqual(data["manifest_version"], 3)
        configuration = data["configuration"]
        self.assertEqual(configuration["adapter"]["source_format"], "lhalo_binary")
        self.assertEqual(
            configuration["adapter"]["parameters"]["sources"],
            [[0, os.path.realpath(self.file0)], [1, os.path.realpath(self.file1)]],
        )
        schema = configuration["schema"]
        self.assertEqual(schema["column_mapping_sha256"], self.schema.digest)
        self.assertEqual(len(schema["source_layout_sha256"]), 64)
        self.assertEqual(schema["canonical"]["source_layout"]["itemsize"], 104)
        # full dtype descriptors, checked against a hand-written field list
        ingest = configuration["record_dtypes"]["ingest"]
        names = [field["name"] for field in ingest["fields"]]
        self.assertEqual(
            names[:14],
            [
                "_file",
                "_unit",
                "_row",
                "SourceHaloID",
                "ForestIndex",
                "HaloRankInForest",
                "Descendant",
                "FirstProgenitor",
                "NextProgenitor",
                "FirstHaloInFOFgroup",
                "NextHaloInFOFgroup",
                "Len",
                "SnapNum",
                "M_Crit200",
            ],
        )
        pos = next(f for f in ingest["fields"] if f["name"] == "Pos")
        self.assertEqual((pos["base"], pos["shape"]), ("<f4", [3]))
        transposed = configuration["record_dtypes"]["transposed"]
        self.assertEqual(transposed["fields"][5]["name"], "DescendantSnapshot")
        self.assertEqual(transposed["fields"][5]["base"], "<i4")
        self.assertEqual(configuration["snapshots"]["numbers"], [0, 1, 2, 3])
        self.assertEqual(configuration["snapshots"]["scale_factors"], [0.25, 0.5, 0.75, 1.0])
        # sources: both files as bulk sources, the a_list by content
        dependencies = {d["path"]: d for d in data["sources"]["dependencies"]}
        self.assertEqual(
            sorted(dependencies),
            sorted(os.path.realpath(p) for p in (self.file0, self.file1, self.a_list)),
        )
        a_list = dependencies[os.path.realpath(self.a_list)]
        self.assertEqual(a_list["roles"], ["a_list"])
        self.assertEqual(a_list["sha256"], hashlib.sha256(A_LIST_TEXT.encode()).hexdigest())
        self.assertEqual(dependencies[os.path.realpath(self.file0)]["roles"], ["source"])
        inventory = data["sources"]["inventory"]
        self.assertEqual((inventory["n_units"], inventory["total_halos"]), (3, TOTAL_HALOS))
        self.assertEqual(
            inventory["files"],
            [
                {"source_file_ordinal": 0, "n_units": 2, "n_halos": 5},
                {"source_file_ordinal": 1, "n_units": 1, "n_halos": 3},
            ],
        )
        # stage records and content checksums, recomputed independently
        for stage in cm.STAGES:
            record = data["stages"][stage]
            self.assertEqual(record["status"], "complete")
            self.assertTrue(record["artifacts"])
            for relpath in record["artifacts"]:
                entry = data["artifacts"][relpath]
                self.assertEqual(entry["stage"], stage)
                content = (self.work / relpath).read_bytes()
                self.assertEqual(entry["sha256"], hashlib.sha256(content).hexdigest())
                self.assertEqual(entry["bytes"], len(content))
        chunks = data["stages"]["ingest"]["artifacts"]
        self.assertEqual(
            chunks,
            ["ingest/chunk_000000.bin", "ingest/chunk_000001.bin", "ingest/chunk_000002.bin"],
        )
        self.assertEqual([data["artifacts"][c]["n_rows"] for c in chunks], [3, 3, 2])
        self.assertEqual(data["stages"]["ingest"]["result"]["snapshot_counts"], [2, 0, 3, 3])

    def test_ingest_chunks_hold_the_literal_source_rows(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        manifest = cm.ConversionManifest.load(self.work)
        dtype = pipeline.ingest_dtype(manifest.schema)
        rows = np.concatenate(
            [
                np.fromfile(manifest.artifact_path(r), dtype=dtype)
                for r in manifest.stage("ingest")["artifacts"]
            ]
        )
        np.testing.assert_array_equal(rows["SourceHaloID"], np.arange(1, 9))
        np.testing.assert_array_equal(rows["_file"], [0, 0, 0, 0, 0, 1, 1, 1])
        np.testing.assert_array_equal(rows["_unit"], [0, 0, 0, 0, 1, 0, 0, 0])
        np.testing.assert_array_equal(rows["_row"], [0, 1, 2, 3, 0, 0, 1, 2])
        # links as target SourceHaloID: tree-local row + tree base
        np.testing.assert_array_equal(rows["Descendant"], [-1, 1, 1, -1, -1, -1, 6, 7])
        np.testing.assert_array_equal(rows["MostBoundID"], [77, -5, 11, 12, 13, 77, -3, 14])

    def test_initialize_is_idempotent_and_refuses_a_different_conversion(self):
        self.initialize()
        before = (self.work / cm.MANIFEST_NAME).read_bytes()
        self.initialize()
        self.assertEqual((self.work / cm.MANIFEST_NAME).read_bytes(), before)
        with self.assertRaisesRegex(ConverterError, "different conversion"):
            self.initialize(ingest_max_rows=4)
        with self.assertRaisesRegex(ConverterError, "different conversion"):
            self.initialize(schema=lhalo_schema("lhalo_binary_extras_example.yaml"))

    def test_stages_refuse_to_run_out_of_order(self):
        self.initialize()
        with self.assertRaisesRegex(ConverterError, "'ingest' is 'pending'"):
            pipeline.run_transpose(self.work)
        with self.assertRaisesRegex(ConverterError, "'transpose' is 'pending'"):
            pipeline.run_write(self.work, StubWriter())

    def test_unknown_parameters_and_bad_types_are_refused_up_front(self):
        cases = [
            {"sources": [[0, self.file0]], "surprise": 1},
            {"sources": [[0.0, self.file0]]},
            {"sources": [[True, self.file0]]},
            {"sources": []},
            {"sources": [[0, self.file0]], "memory_budget_bytes": 1.5},
        ]
        for parameters in cases:
            with self.subTest(parameters=parameters):
                with self.assertRaises(ConverterError):
                    pipeline.initialize(self.work, self.schema, parameters, self.a_list)
                self.assertFalse(self.work.exists())
        with self.assertRaisesRegex(ConverterError, "too small"):
            self.initialize(transpose_budget_bytes=1024)
        self.assertFalse(self.work.exists())


# ==========================================================================
# Embedded configuration, schema and dependency binding
# ==========================================================================


class BindingTests(PipelineCase):
    def test_resume_uses_the_embedded_schema_after_the_profile_disappears(self):
        profile_dir = self.tmp / "profile"
        profile_dir.mkdir()
        shutil.copy(PROFILE_DIR / "lhalo_binary_extras_example.yaml", profile_dir / "p.yaml")
        shutil.copy(MINI_MILLENNIUM_PROPERTIES, profile_dir / "halo_properties.yaml")
        schema = lhalo_schema("p.yaml", profile_dir, profile_dir / "halo_properties.yaml")
        self.initialize(schema=schema)
        with mock.patch.object(pipeline, "_write_chunk", side_effect=InjectedFailure):
            with self.assertRaises(InjectedFailure):
                pipeline.run_ingest(self.work)
        shutil.rmtree(profile_dir)
        manifest = self.run_all()
        self.assertEqual(manifest.schema.digest, schema.digest)
        self.assertEqual(
            [e.name for e in manifest.schema.extra_fields], [e.name for e in schema.extra_fields]
        )
        rows = pipeline.read_transposed(manifest, 3)
        # an extra selected by the vanished profile, read from the source
        self.assertIn("M_Mean200", rows.dtype.names)
        np.testing.assert_array_equal(rows["SourceHaloID"], [1, 5, 6])

    def test_same_width_different_schema_fails_before_mutation(self):
        schema = lhalo_schema("lhalo_binary_extras_example.yaml")
        variant = same_width_variant(schema)
        self.assertEqual(
            pipeline.ingest_dtype(schema).itemsize, pipeline.ingest_dtype(variant).itemsize
        )
        self.assertEqual(pipeline.ingest_dtype(schema), pipeline.ingest_dtype(variant))
        self.assertNotEqual(schema.digest, variant.digest)
        self.initialize(schema=schema)
        steps = [
            lambda s: pipeline.run_ingest(self.work, schema=s),
            lambda s: pipeline.run_transpose(self.work, schema=s),
            lambda s: pipeline.run_write(self.work, StubWriter(), schema=s),
        ]
        for index, step in enumerate(steps):
            before = tree_state(self.work)
            with self.assertRaisesRegex(ConverterError, "even at the same record width"):
                step(variant)
            self.assertEqual(tree_state(self.work), before, index)
            step(schema)
        self.assertEqual(cm.ConversionManifest.load(self.work).schema.digest, schema.digest)

    def test_an_edited_schema_in_the_manifest_fails_before_mutation(self):
        self.initialize()
        path = self.work / cm.MANIFEST_NAME
        data = json.loads(path.read_text())
        data["configuration"]["schema"]["canonical"]["payload_fields"][0]["units"] = "edited"
        data["configuration_sha256"] = cm.canonical_sha256(data["configuration"])
        path.write_text(json.dumps(data))
        before = tree_state(self.work)
        # payload declarations are the code's, so the digest still matches;
        # the rebuilt record does not
        with self.assertRaisesRegex(ConverterError, "disagrees with its own rebuilt form"):
            pipeline.run_ingest(self.work)
        self.assertEqual(tree_state(self.work), before)

    def test_changed_source_dependencies_fail_before_mutation(self):
        def rewrite_same_size():
            data = bytearray(Path(self.file1).read_bytes())
            data[-1] ^= 0xFF
            Path(self.file1).write_bytes(bytes(data))

        def replace_inode():
            status = os.stat(self.file0)
            copy = self.src / "copy"
            shutil.copy2(self.file0, copy)
            os.replace(copy, self.file0)
            os.utime(self.file0, ns=(status.st_atime_ns, status.st_mtime_ns))

        def edit_a_list_restoring_mtime():
            status = os.stat(self.a_list)
            self.a_list.write_text(A_LIST_TEXT.replace("0.5", "0.6"))
            os.utime(self.a_list, ns=(status.st_atime_ns, status.st_mtime_ns))

        changes = {
            "rewritten": rewrite_same_size,
            "replaced": replace_inode,
            "removed": lambda: os.unlink(self.file1),
            "a_list": edit_a_list_restoring_mtime,
        }
        for what, change in changes.items():
            for stage in ("ingest", "transpose", "write"):
                with self.subTest(change=what, stage=stage):
                    shutil.rmtree(self.work, ignore_errors=True)
                    shutil.rmtree(self.src)
                    self.setUp_sources()
                    self.initialize()
                    if stage != "ingest":
                        pipeline.run_ingest(self.work)
                    if stage == "write":
                        pipeline.run_transpose(self.work)
                    before = tree_state(self.work)
                    change()
                    with self.assertRaisesRegex(ConverterError, "source dependency"):
                        if stage == "ingest":
                            pipeline.run_ingest(self.work)
                        elif stage == "transpose":
                            pipeline.run_transpose(self.work)
                        else:
                            pipeline.run_write(self.work, StubWriter())
                    self.assertEqual(tree_state(self.work), before)

    def setUp_sources(self):
        self.src.mkdir()
        lhalo.write_lhalo_file(str(self.file0), [TREE_A, TREE_B])
        lhalo.write_lhalo_file(str(self.file1), [TREE_C])
        self.a_list.write_text(A_LIST_TEXT)

    def test_a_source_edited_behind_its_stat_evidence_cannot_resume_ingest(self):
        """An in-place edit that restores size and mtime passes the stat pin;
        the re-read batch no longer matches its registered chunk, so resume
        refuses instead of appending rows derived from different bytes."""
        self.initialize()
        calls = {"n": 0}
        real = pipeline._write_chunk

        def fail_third(path, records):
            calls["n"] += 1
            if calls["n"] == 3:
                raise InjectedFailure
            return real(path, records)

        with mock.patch.object(pipeline, "_write_chunk", side_effect=fail_third):
            with self.assertRaises(InjectedFailure):
                pipeline.run_ingest(self.work, save_every_chunks=1)
        status = os.stat(self.file0)
        data = bytearray(Path(self.file0).read_bytes())
        # Len of tree A row 1 (header 8 + 2 counts, record 1, offset 20)
        offset = 8 + 2 * 4 + 104 + 20
        data[offset] ^= 0x01
        with open(self.file0, "r+b") as handle:
            handle.write(bytes(data))
        os.utime(self.file0, ns=(status.st_atime_ns, status.st_mtime_ns))
        cm.ConversionManifest.load(self.work).verify_dependencies()  # stat evidence passes
        with self.assertRaisesRegex(ConverterError, "chunk_000000.bin.*different rows"):
            pipeline.run_ingest(self.work)
        manifest = cm.ConversionManifest.load(self.work)
        self.assertEqual(manifest.stage("ingest")["status"], cm.STAGE_FAILED)
        self.assertEqual(len(manifest.stage("ingest")["artifacts"]), 2)

    def test_changed_hdf5_backing_file_fails_before_mutation(self):
        directory = self.tmp / "h5"
        directory.mkdir()
        forests = [h5fixtures.MERGER_FOREST]
        info = h5fixtures.write_source(
            str(directory), [{"forests": forests}, {"forests": [h5fixtures.lone(9, snap=5)]}]
        )
        schema = h5fixtures.load_schema()
        a_list = directory.parent / "hdf5.a_list"
        a_list.write_text("".join("{}\n".format(0.5 + 0.1 * i) for i in range(6)))
        work = self.tmp / "h5work"
        parameters = {
            "info_path": info,
            "first_file": 0,
            "last_file": 1,
            "particle_mass": h5fixtures.PARTICLE_MASS,
        }
        pipeline.initialize(work, schema, parameters, a_list, transpose_budget_bytes=BUDGET)
        data = json.loads((work / cm.MANIFEST_NAME).read_text())
        pinned = {d["path"]: d for d in data["sources"]["dependencies"]}
        backing = os.path.realpath(directory / "forests_1.h5")
        self.assertIn(backing, pinned)
        self.assertIn("/File1/Forests/Mvir", pinned[backing]["objects"])
        pipeline.run_ingest(work)
        pipeline.run_transpose(work)
        before = tree_state(work)
        # rewrite the external file with identical content: a new inode
        status = os.stat(backing)
        shutil.copy2(backing, backing + ".new")
        os.replace(backing + ".new", backing)
        os.utime(backing, ns=(status.st_atime_ns, status.st_mtime_ns))
        with self.assertRaisesRegex(ConverterError, "forests_1.h5.*changed"):
            pipeline.run_write(work, StubWriter())
        self.assertEqual(tree_state(work), before)
        # and a resumed ingest of a new conversion refuses the same way
        work2 = self.tmp / "h5work2"
        pipeline.initialize(work2, schema, parameters, a_list, transpose_budget_bytes=BUDGET)
        before = tree_state(work2)
        os.unlink(backing)
        with self.assertRaisesRegex(ConverterError, "forests_1.h5"):
            pipeline.run_ingest(work2)
        self.assertEqual(tree_state(work2), before)

    def test_legacy_workdir_is_refused_and_untouched(self):
        work = self.tmp / "legacy"
        shutil.copytree(LEGACY_FIXTURE / "workdir", work)
        before = tree_state(work)
        with self.assertRaisesRegex(ConverterError, "legacy"):
            pipeline.run_ingest(work)
        with self.assertRaisesRegex(ConverterError, "legacy"):
            pipeline.initialize(work, self.schema, self.parameters, self.a_list)
        self.assertEqual(tree_state(work), before)

    def test_a_workdir_containing_a_source_is_refused(self):
        with self.assertRaisesRegex(ConverterError, "contains source dependency"):
            pipeline.initialize(self.src, self.schema, self.parameters, self.a_list)


# ==========================================================================
# Interruption and restart
# ==========================================================================


def _raise_on_call(target_name, when, count=1):
    """Patch ``pipeline.<target_name>`` to raise on its ``count``-th call,
    either before delegating (``when="before"``) or after (``"after"``)."""
    real = getattr(pipeline, target_name)
    calls = {"n": 0}

    def wrapper(*args, **kwargs):
        calls["n"] += 1
        if calls["n"] == count and when == "before":
            raise InjectedFailure(target_name)
        result = real(*args, **kwargs)
        if calls["n"] == count and when == "after":
            raise InjectedFailure(target_name)
        return result

    return mock.patch.object(pipeline, target_name, side_effect=wrapper)


def _raise_on_method(cls, name, when, count=1, predicate=None):
    real = getattr(cls, name)
    calls = {"n": 0}

    def wrapper(self, *args, **kwargs):
        if predicate is None or predicate(self, *args, **kwargs):
            calls["n"] += 1
            hit = calls["n"] == count
        else:
            hit = False
        if hit and when == "before":
            raise InjectedFailure(name)
        result = real(self, *args, **kwargs)
        if hit and when == "after":
            raise InjectedFailure(name)
        return result

    return mock.patch.object(cls, name, autospec=True, side_effect=wrapper)


def _stage_running(stage):
    return lambda manifest, *a, **k: manifest.stage(stage)["status"] == cm.STAGE_RUNNING


def _stage_completing(stage):
    """True for the save that records ``stage`` complete."""
    return lambda manifest, *a, **k: manifest.stage(stage)["status"] == cm.STAGE_COMPLETE


def _completion_saves(stage):
    return {
        "before completion save": _raise_on_method(
            cm.ConversionManifest, "save", "before", predicate=_stage_completing(stage)
        ),
        "after completion save": _raise_on_method(
            cm.ConversionManifest, "save", "after", predicate=_stage_completing(stage)
        ),
    }


class InterruptionTests(PipelineCase):
    """Every stage, interrupted at every step, retried to identical output."""

    def run_until_failure(self, stage, injection, completed=False):
        self.initialize()
        if stage in ("transpose", "write"):
            pipeline.run_ingest(self.work, save_every_chunks=1)
        if stage == "write":
            pipeline.run_transpose(self.work)
        with injection:
            with self.assertRaises(InjectedFailure):
                if stage == "ingest":
                    pipeline.run_ingest(self.work, save_every_chunks=1)
                elif stage == "transpose":
                    pipeline.run_transpose(self.work)
                else:
                    pipeline.run_write(self.work, StubWriter())
        manifest = cm.ConversionManifest.load(self.work)
        # only a failure after the completion save may leave the stage complete
        self.assertEqual(manifest.is_complete(stage), completed)
        for later in cm.STAGES[cm.STAGES.index(stage) + 1 :]:
            self.assertEqual(manifest.stage(later)["status"], cm.STAGE_PENDING)

    def ingest_injections(self):
        save = _stage_running("ingest")
        return {
            "before chunk write": _raise_on_call("_write_chunk", "before", 2),
            "after chunk write": _raise_on_call("_write_chunk", "after", 2),
            "before chunk verification": _raise_on_method(
                cm.ConversionManifest, "register_artifact", "before", 2
            ),
            "after chunk verification": _raise_on_method(
                cm.ConversionManifest, "register_artifact", "after", 2
            ),
            "before chunk save": _raise_on_method(
                cm.ConversionManifest, "save", "before", 2, predicate=save
            ),
            "after chunk save": _raise_on_method(
                cm.ConversionManifest, "save", "after", 2, predicate=save
            ),
            "before the last chunk save": _raise_on_method(
                cm.ConversionManifest, "save", "before", 4, predicate=save
            ),
            "before attempt save": _raise_on_method(
                cm.ConversionManifest, "save", "before", 1, predicate=save
            ),
            **_completion_saves("ingest"),
        }

    def test_ingest_resumes_after_every_injected_failure(self):
        for point, injection in self.ingest_injections().items():
            with self.subTest(point=point):
                shutil.rmtree(self.work, ignore_errors=True)
                self.run_until_failure(
                    "ingest", injection, completed=point == "after completion save"
                )
                self.run_all()
                self.assert_matches_reference(self.work)

    def test_transpose_resumes_after_every_injected_failure(self):
        injections = {
            "before transpose": mock.patch.object(
                pipeline.transpose_module, "transpose", side_effect=InjectedFailure
            ),
            "inside transpose (chunk read)": _raise_on_call("_batch_from_records", "before", 2),
            "after transpose, before verification": _raise_on_call(
                "_register_transposed", "before"
            ),
            "during verification": _raise_on_method(
                cm.ConversionManifest, "register_artifact", "after", 2
            ),
            "after verification": _raise_on_call("_register_transposed", "after"),
            "before attempt save": _raise_on_method(
                cm.ConversionManifest, "save", "before", 1, predicate=_stage_running("transpose")
            ),
            **_completion_saves("transpose"),
        }
        for point, injection in injections.items():
            with self.subTest(point=point):
                shutil.rmtree(self.work, ignore_errors=True)
                self.run_until_failure(
                    "transpose", injection, completed=point == "after completion save"
                )
                self.run_all()
                self.assert_matches_reference(self.work)
                attempts = sorted(os.listdir(self.work / pipeline.TRANSPOSE_DIR))
                self.assertEqual(len(attempts), 1, attempts)

    def test_write_resumes_after_every_injected_failure(self):
        injections = {
            "before writer": _raise_on_method(StubWriter, "write", "before"),
            "after writer": _raise_on_method(StubWriter, "write", "after"),
            "before writer verification": _raise_on_method(StubWriter, "verify", "before"),
            "after writer verification": _raise_on_method(StubWriter, "verify", "after"),
            "during registration": _raise_on_method(
                cm.ConversionManifest, "register_artifact", "after", 2
            ),
            "before attempt save": _raise_on_method(
                cm.ConversionManifest, "save", "before", 1, predicate=_stage_running("write")
            ),
            **_completion_saves("write"),
        }
        for point, injection in injections.items():
            with self.subTest(point=point):
                shutil.rmtree(self.work, ignore_errors=True)
                self.run_until_failure(
                    "write", injection, completed=point == "after completion save"
                )
                self.run_all()
                self.assert_matches_reference(self.work)
                self.assertEqual(len(os.listdir(self.work / pipeline.WRITE_DIR)), 1)

    def test_failed_stage_names_the_offending_artifact(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        manifest = cm.ConversionManifest.load(self.work)
        chunk = manifest.artifact_path(manifest.stage("ingest")["artifacts"][1])
        data = bytearray(chunk.read_bytes())
        data[40] ^= 0x01
        chunk.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, str(chunk)):
            pipeline.run_transpose(self.work)
        record = cm.ConversionManifest.load(self.work).stage("transpose")
        self.assertEqual(record["status"], cm.STAGE_FAILED)
        self.assertIn("chunk_000001.bin", record["error"])
        self.assertEqual(os.listdir(self.work / record["directory"]), [])


class HardKillTests(PipelineCase):
    """``os._exit`` in a child process: no handler, no ``finally`` runs."""

    SCRIPT = textwrap.dedent("""
        import os, sys
        sys.path.insert(0, {convert!r})
        import conversion_manifest as cm
        import pipeline
        target, stage, count = {target!r}, {stage!r}, {count}
        if target.startswith("cm."):
            owner, name = cm.ConversionManifest, target[3:]
        else:
            owner, name = pipeline, target
        real = getattr(owner, name)
        calls = [0]
        def killer(*args, **kwargs):
            result = real(*args, **kwargs)
            calls[0] += 1
            if calls[0] == count:
                os._exit(137)
            return result
        if owner is pipeline:
            setattr(pipeline, name, killer)
        else:
            setattr(owner, name, lambda self, *a, **k: killer(self, *a, **k))
        if stage == "ingest":
            pipeline.run_ingest({work!r}, save_every_chunks=2)
        else:
            pipeline.run_transpose({work!r})
        sys.exit(0)
        """)

    def kill(self, stage, target, count):
        script = self.SCRIPT.format(
            convert=str(CONVERT_DIR), target=target, stage=stage, count=count, work=str(self.work)
        )
        completed = subprocess.run(
            [sys.executable, "-c", script], capture_output=True, text=True, timeout=300
        )
        self.assertEqual(completed.returncode, 137, completed.stderr)

    def test_ingest_killed_after_an_unsaved_chunk_write(self):
        self.initialize()
        # chunk 0 is registered but saved only every second chunk; the kill
        # lands after chunk 2 is on disk and chunk 0-1 are saved
        self.kill("ingest", "_write_chunk", 3)
        manifest = cm.ConversionManifest.load(self.work)
        self.assertEqual(manifest.stage("ingest")["status"], cm.STAGE_RUNNING)
        self.assertEqual(len(manifest.stage("ingest")["artifacts"]), 2)
        self.assertTrue((self.work / "ingest/chunk_000002.bin").exists())
        self.run_all()
        self.assert_matches_reference(self.work)

    def test_ingest_killed_mid_rename(self):
        self.initialize()
        self.kill("ingest", "cm.save", 2)
        self.run_all()
        self.assert_matches_reference(self.work)

    def test_transpose_killed_after_outputs_exist(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        self.kill("transpose", "_register_transposed", 1)
        manifest = cm.ConversionManifest.load(self.work)
        record = manifest.stage("transpose")
        self.assertEqual(record["status"], cm.STAGE_RUNNING)
        leftovers = sorted(os.listdir(self.work / record["directory"]))
        self.assertEqual(leftovers, ["snapshot_{:03d}.transposed".format(s) for s in range(4)])
        self.run_all()
        self.assert_matches_reference(self.work)
        self.assertEqual(os.listdir(self.work / pipeline.TRANSPOSE_DIR), ["attempt_002"])


# ==========================================================================
# Skip-trust, containment and optional cleanup
# ==========================================================================


class SkipTrustAndCleanupTests(PipelineCase):
    def test_complete_stages_verify_before_skipping(self):
        self.initialize()
        self.run_all()
        manifest = cm.ConversionManifest.load(self.work)
        for stage, rerun in (
            ("ingest", lambda: pipeline.run_ingest(self.work)),
            ("transpose", lambda: pipeline.run_transpose(self.work)),
            ("write", lambda: pipeline.run_write(self.work, StubWriter())),
        ):
            with self.subTest(stage=stage):
                rerun()  # verified, skipped
                artifact = manifest.artifact_path(manifest.stage(stage)["artifacts"][-1])
                original = artifact.read_bytes()
                artifact.write_bytes(original[:-1] + bytes([original[-1] ^ 0x01]))
                with self.assertRaisesRegex(ConverterError, str(artifact)):
                    rerun()
                artifact.write_bytes(original)
                rerun()

    def test_write_refuses_to_consume_a_corrupt_transposed_snapshot(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        pipeline.run_transpose(self.work)
        manifest = cm.ConversionManifest.load(self.work)
        target = manifest.artifact_path(manifest.stage("transpose")["artifacts"][3])
        data = bytearray(target.read_bytes())
        data[0] ^= 0x01
        target.write_bytes(bytes(data))
        before = tree_state(self.work)
        with self.assertRaisesRegex(ConverterError, str(target)):
            pipeline.run_write(self.work, StubWriter())
        self.assertEqual(tree_state(self.work), before)

    def test_a_different_writer_cannot_claim_a_completed_write(self):
        self.initialize()
        self.run_all()
        with self.assertRaisesRegex(ConverterError, "completed by writer"):
            pipeline.run_write(self.work, StubWriter(version=2))

    def test_opt_in_cleanup_consumes_only_verified_predecessors(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        pipeline.run_transpose(self.work)
        sources_before = tree_state(self.src)
        manifest = pipeline.run_transpose(self.work, consume_ingest=True)
        for relpath in manifest.stage("ingest")["artifacts"]:
            self.assertEqual(manifest.artifact(relpath)["status"], cm.ARTIFACT_REMOVED)
            self.assertFalse((self.work / relpath).exists())
        pipeline.run_ingest(self.work)  # consumed chunks are accepted, not re-stat-ed
        manifest = pipeline.run_write(self.work, StubWriter(), consume_transposed=True)
        for relpath in manifest.stage("transpose")["artifacts"]:
            self.assertFalse((self.work / relpath).exists())
        pipeline.run_transpose(self.work)
        pipeline.run_write(self.work, StubWriter())
        self.assertEqual(tree_state(self.src), sources_before)
        self.assert_no_strays(self.work)

    def test_repeated_cleanup_flags_are_idempotent_end_to_end(self):
        """Regression (Slice 7 steer 1): with both consume flags used end to
        end, re-running each stage with its flag is a no-op, not a refusal to
        re-verify a successor whose artifacts were consumed in turn."""
        self.initialize()
        pipeline.run_ingest(self.work)
        pipeline.run_transpose(self.work, consume_ingest=True)
        pipeline.run_write(self.work, StubWriter(), consume_transposed=True)
        before = tree_state(self.work)
        pipeline.run_transpose(self.work, consume_ingest=True)
        pipeline.run_write(self.work, StubWriter(), consume_transposed=True)
        self.assertEqual(tree_state(self.work), before)
        manifest = cm.ConversionManifest.load(self.work)
        self.assertEqual(manifest.consume_stage("ingest", "transpose"), [])
        self.assertEqual(manifest.consume_stage("transpose", "write"), [])
        for stage in ("ingest", "transpose"):
            for relpath in manifest.stage(stage)["artifacts"]:
                self.assertEqual(manifest.artifact(relpath)["status"], cm.ARTIFACT_REMOVED)
        manifest.verify_stage_artifacts("write")

    def test_ingest_cleanup_deferred_until_after_write_cleanup(self):
        """Regression (Slice 7 steer 2): consuming the transposed snapshots
        first and the ingest chunks afterwards is accepted -- the successor's
        own artifacts were consumed by the complete write stage."""
        self.initialize()
        pipeline.run_ingest(self.work)
        pipeline.run_transpose(self.work)
        pipeline.run_write(self.work, StubWriter(), consume_transposed=True)
        manifest = pipeline.run_transpose(self.work, consume_ingest=True)
        for stage in ("ingest", "transpose"):
            for relpath in manifest.stage(stage)["artifacts"]:
                self.assertEqual(manifest.artifact(relpath)["status"], cm.ARTIFACT_REMOVED)
                self.assertFalse((self.work / relpath).exists())
        pipeline.run_write(self.work, StubWriter())
        self.assert_no_strays(self.work)

    def test_deferred_ingest_cleanup_still_verifies_what_remains(self):
        """The transitive allowance covers only consumed successor artifacts,
        and the later stage now holding their rows must verify: a corrupt
        write output blocks the deferred ingest cleanup, deleting nothing."""
        self.initialize()
        pipeline.run_ingest(self.work)
        pipeline.run_transpose(self.work)
        manifest = pipeline.run_write(self.work, StubWriter(), consume_transposed=True)
        output = manifest.artifact_path(manifest.stage("write")["artifacts"][0])
        data = bytearray(output.read_bytes())
        data[0] ^= 0x01
        output.write_bytes(bytes(data))
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            manifest.consume_stage("ingest", "transpose")
        with self.assertRaisesRegex(ConverterError, "content checksum"):
            pipeline.run_transpose(self.work, consume_ingest=True)
        for relpath in manifest.stage("ingest")["artifacts"]:
            self.assertTrue((self.work / relpath).exists())

    def test_leftover_scan_accepts_chunk_indices_past_six_digits(self):
        """Regression (Slice 7 steer 2): chunk_name grows past six digits at
        index 1,000,000; the resume scan must still own those names."""
        self.assertEqual(pipeline.chunk_name(999999), "chunk_999999.bin")
        self.assertEqual(pipeline.chunk_name(1000000), "chunk_1000000.bin")
        for index in (999999, 1000000, 1000001, 12345678):
            match = pipeline._CHUNK_RE.match(pipeline.chunk_name(index))
            self.assertIsNotNone(match, index)
            self.assertEqual(int(match.group(1)), index)
            self.assertIsNotNone(pipeline._CHUNK_RE.match(pipeline.chunk_name(index) + ".partial"))
        self.assertIsNone(pipeline._CHUNK_RE.match("chunk_12345.bin"))
        self.initialize()
        manifest = cm.ConversionManifest.load(self.work)
        ingest_dir = self.work / pipeline.INGEST_DIR
        ingest_dir.mkdir()
        names = ["chunk_1000000.bin", "chunk_1000001.bin.partial", "chunk_1000002.bin"]
        for name in names:
            (ingest_dir / name).write_bytes(b"x")
        leftovers = pipeline._ingest_leftovers(manifest, 1000000)
        self.assertEqual(sorted(p.name for p in leftovers), sorted(names))
        # below the registered count and unregistered is still foreign
        (ingest_dir / "chunk_999999.bin").write_bytes(b"x")
        with self.assertRaisesRegex(ConverterError, "chunk_999999.bin"):
            pipeline._ingest_leftovers(manifest, 1000000)

    def test_cleanup_is_refused_while_its_successor_is_incomplete(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        manifest = cm.ConversionManifest.load(self.work)
        with self.assertRaisesRegex(ConverterError, "before transpose is complete"):
            manifest.consume_stage("ingest", "transpose")

    def test_foreign_files_in_owned_directories_are_refused_not_deleted(self):
        self.initialize()
        with mock.patch.object(pipeline, "_write_chunk", side_effect=InjectedFailure):
            with self.assertRaises(InjectedFailure):
                pipeline.run_ingest(self.work)
        stray = self.work / pipeline.INGEST_DIR / "notes.txt"
        stray.write_text("mine")
        with self.assertRaisesRegex(ConverterError, "unexpected entry"):
            pipeline.run_ingest(self.work)
        self.assertEqual(stray.read_text(), "mine")
        stray.unlink()
        pipeline.run_ingest(self.work)
        self.initialize(self.tmp / "other")
        pipeline.run_ingest(self.tmp / "other")
        foreign = self.tmp / "other" / pipeline.TRANSPOSE_DIR / "attempt_001"
        foreign.mkdir(parents=True)
        (foreign / "keep").write_text("keep")
        with self.assertRaisesRegex(ConverterError, "never recorded"):
            pipeline.run_transpose(self.tmp / "other")
        self.assertEqual((foreign / "keep").read_text(), "keep")

    def test_sources_stay_read_only(self):
        for path in (self.file0, self.file1, self.a_list):
            os.chmod(path, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        self.addCleanup(
            lambda: [
                os.chmod(p, stat.S_IRUSR | stat.S_IWUSR)
                for p in (self.file0, self.file1, self.a_list)
            ]
        )
        before = tree_state(self.src)
        self.initialize()
        with mock.patch.object(pipeline, "_write_chunk", side_effect=InjectedFailure):
            with self.assertRaises(InjectedFailure):
                pipeline.run_ingest(self.work)
        self.run_all()
        pipeline.run_transpose(self.work, consume_ingest=True)
        self.assertEqual(tree_state(self.src), before)
        self.assert_expected(self.work)


# ==========================================================================
# The other two adapters through the same stages
# ==========================================================================


class WriteInputForestTests(PipelineCase):
    """The write stage's forest enumeration (converter generalisation
    Slice 8): lazy, read-only, bound to the recorded inventory."""

    def transposed(self):
        self.initialize()
        pipeline.run_ingest(self.work)
        pipeline.run_transpose(self.work)
        return cm.ConversionManifest.load(self.work)

    def test_lhalo_forests_are_the_inventory_trees_in_order(self):
        inputs = pipeline._write_inputs(self.transposed())
        records = [
            (r.forest_index, r.forest_id, r.source_file_ordinal, r.unit_ordinal, r.n_halos)
            for r in inputs.forests()
        ]
        # TREE_A (4 halos) and TREE_B (1) in file 0, TREE_C (3) in file 1
        self.assertEqual(records, [(0, 0, 0, 0, 4), (1, 1, 0, 1, 1), (2, 2, 1, 0, 3)])
        self.assertEqual(len(list(inputs.forests())), 3)

    def test_the_enumeration_reads_no_source_until_it_is_called(self):
        manifest = self.transposed()
        with mock.patch.object(pipeline, "build_adapter", side_effect=InjectedFailure):
            inputs = pipeline._write_inputs(manifest)
            with self.assertRaises(InjectedFailure):
                inputs.forests()

    def test_the_enumeration_is_bound_to_the_recorded_inventory(self):
        manifest = self.transposed()
        manifest.data["sources"]["inventory"]["units_sha256"] = "0" * 64
        inputs = pipeline._write_inputs(manifest)
        with self.assertRaisesRegex(ConverterError, "inventory changed"):
            inputs.forests()

    def test_the_stub_writer_path_is_unchanged(self):
        self.transposed()
        manifest = pipeline.run_write(self.work, StubWriter())
        self.assertTrue(manifest.is_complete("write"))


class OtherAdapterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pipeline_adapters_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def ascii_source(self):
        src = self.tmp / "ascii"
        src.mkdir()
        forests = fixtures.standard_forests()
        tree = fixtures.write_ctrees_file(src / "tree_0_0_0.dat", fixtures.all_trees(forests))
        return {
            "tree_files": [str(tree)],
            "forests_list": str(fixtures.write_forests_list(src / "forests.list", forests)),
            "simulation_info": str(fixtures.write_simulation_info(src / "simulation_info.yaml")),
        }, fixtures.write_a_list(src / "fixture.a_list")

    def test_ascii_ingest_resumes_to_identical_chunks(self):
        parameters, a_list = self.ascii_source()
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml"))
        clean = self.tmp / "clean"
        pipeline.initialize(
            clean, schema, parameters, a_list, ingest_max_rows=4, transpose_budget_bytes=BUDGET
        )
        pipeline.run_ingest(clean, save_every_chunks=1)
        pipeline.run_transpose(clean)

        work = self.tmp / "work"
        pipeline.initialize(
            work, schema, parameters, a_list, ingest_max_rows=4, transpose_budget_bytes=BUDGET
        )
        with _raise_on_call("_write_chunk", "after", 2):
            with self.assertRaises(InjectedFailure):
                pipeline.run_ingest(work, save_every_chunks=1)
        self.assertTrue((work / pipeline.ASCII_PREPARATION_DIR / cm.MANIFEST_NAME).exists())
        self.assertEqual(
            cm.classify_manifest(work / pipeline.ASCII_PREPARATION_DIR), cm.MANIFEST_LEGACY
        )
        pipeline.run_ingest(work, save_every_chunks=1)
        pipeline.run_transpose(work)
        got, want = cm.ConversionManifest.load(work), cm.ConversionManifest.load(clean)
        self.assertEqual(got.inventory, want.inventory)
        for stage in ("ingest", "transpose"):
            self.assertEqual(
                [got.artifact(r)["sha256"] for r in got.stage(stage)["artifacts"]],
                [want.artifact(r)["sha256"] for r in want.stage(stage)["artifacts"]],
            )
            self.assertEqual(got.stage(stage)["result"], want.stage(stage)["result"])
        n_halos = sum(len(tree.halos) for tree in fixtures.all_trees(fixtures.standard_forests()))
        self.assertEqual(got.stage("ingest")["result"]["n_rows"], n_halos)
        ids = np.concatenate(
            [
                pipeline.read_transposed(got, s)["SourceHaloID"]
                for s in got.configuration["snapshots"]["numbers"]
            ]
        )
        self.assertEqual(sorted(ids.tolist()), list(range(1, n_halos + 1)))

    def test_hdf5_route_runs_and_restarts(self):
        directory = self.tmp / "h5"
        directory.mkdir()
        info = h5fixtures.write_source(
            str(directory),
            [{"forests": [h5fixtures.MERGER_FOREST]}, {"forests": [h5fixtures.lone(9, snap=5)]}],
        )
        a_list = self.tmp / "h5.a_list"
        a_list.write_text("".join("{}\n".format(0.5 + 0.1 * i) for i in range(6)))
        schema = h5fixtures.load_schema()
        parameters = {
            "info_path": info,
            "first_file": 0,
            "last_file": 1,
            "particle_mass": h5fixtures.PARTICLE_MASS,
        }
        work = self.tmp / "work"
        pipeline.initialize(
            work, schema, parameters, a_list, ingest_max_rows=2, transpose_budget_bytes=BUDGET
        )
        with _raise_on_call("_register_transposed", "before"):
            pipeline.run_ingest(work)
            with self.assertRaises(InjectedFailure):
                pipeline.run_transpose(work)
        pipeline.run_transpose(work)
        manifest = pipeline.run_write(work, StubWriter())
        rows5 = pipeline.read_transposed(manifest, 5)
        rows4 = pipeline.read_transposed(manifest, 4)
        # MERGER_FOREST (file 0) then the lone forest (file 1): ids 1-4, 5
        np.testing.assert_array_equal(rows5["SourceHaloID"], [1, 2, 5])
        np.testing.assert_array_equal(rows4["SourceHaloID"], [3, 4])
        np.testing.assert_array_equal(rows5["FirstProgenitor"], [0, 1, -1])
        np.testing.assert_array_equal(rows5["FirstProgenitorSnapshot"], [4, 4, -1])
        np.testing.assert_array_equal(rows4["Descendant"], [0, 1])
        np.testing.assert_array_equal(rows5["NextHaloInFOFgroup"], [1, -1, -1])
        np.testing.assert_array_equal(rows5["ForestIndex"], [0, 0, 1])


if __name__ == "__main__":
    unittest.main()
